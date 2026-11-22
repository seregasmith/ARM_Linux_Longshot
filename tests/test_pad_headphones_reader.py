"""A DualSense's jack reader (host/hos_sdl.c hos_sdl_pad_headphones) lives for its pad's open life only.

The reader's second HID handle, the last status it saw and a failed open are tied to the SDL pad pointer. SDL may hand
the same address to the next pad after a reconnect, so closing a pad (hos_sdl_pad_close) closes that handle and forgets
both: the new pad opens its own, is not refused by the old one's failure, and does not inherit its headphones. The real
functions are compiled from the source around a made-up SDL (no SDL, no controller).

    python -I -B tests/test_pad_headphones_reader.py [--cc gcc]
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = []
ROOT = Path(__file__).resolve().parents[1]

PRELUDE = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d %s\n", __LINE__, #x); exit(1); } } while (0)
static void hos_log(const char *format, ...) { (void)format; }
static int opens, open_fails, closes, pad_closes, plugged = -1;
static int handle;
static int read_timeout(void *dev, unsigned char *report, size_t size, int ms) {
    (void)ms, (void)size;
    CHECK(dev == &handle);
    if (plugged < 0) return 0;   /* (nothing queued) */
    memset(report, 0, 80);
    report[0] = 0x01, report[54] = (unsigned char)plugged;
    plugged = -1;
    return 64;
}
static void open_path(void) {}
static void hid_close(void) {}
static void pad_close(void *pad) { (void)pad; pad_closes++; }
static struct { void *HidReadTimeout, *HidOpenPath, *HidClose, *PadClose; } f = {
    (void *)read_timeout, (void *)open_path, (void *)hid_close, (void *)pad_close};
#define CALL(type, fn) ((type)f.fn)
static const char *hos_sdl_pad_path(void *pad) { (void)pad; return "pad path"; }
static void *hos_sdl_hid_open(const char *path) {
    CHECK(!strcmp(path, "pad path"));
    opens++;
    return open_fails ? NULL : &handle;
}
static void hos_sdl_hid_close(void *hid) { CHECK(hid == &handle); closes++; }
'''

FIXTURE = r'''
int main(void) {
    int sdl_pad;   /* (one address, handed out again after each reconnect) */
    void *pad = &sdl_pad;
    open_fails = 1;
    CHECK(hos_sdl_pad_headphones(pad) == -1 && opens == 1);
    CHECK(hos_sdl_pad_headphones(pad) == -1 && opens == 1);   /* (a failed open is not tried again for that pad) */
    hos_sdl_pad_close(pad);
    CHECK(pad_closes == 1);
    open_fails = 0, plugged = 1;   /* reconnected at the same address: its own open, not the old failure */
    CHECK(hos_sdl_pad_headphones(pad) == 1 && opens == 2);
    CHECK(hos_sdl_pad_headphones(pad) == 1 && opens == 2);   /* (nothing new queued: the last status, same handle) */
    hos_sdl_pad_close(pad);
    CHECK(closes == 1 && pad_closes == 2);   /* the handle closed with the pad */
    CHECK(hos_sdl_pad_headphones(pad) == -1 && opens == 3);   /* reconnected, nothing read yet: not the old pad's 1 */
    plugged = 0;
    CHECK(hos_sdl_pad_headphones(pad) == 0);
    int other;
    hos_sdl_pad_close(&other);   /* (another pad closed: this one's reader stays) */
    CHECK(closes == 1 && hos_sdl_pad_headphones(pad) == 0 && opens == 3);
    puts("pad headphones: the reader closed and forgotten with its pad, a reused address starts afresh");
    return 0;
}
'''


def block(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    sdl = (ROOT / "host" / "hos_sdl.c").read_text(encoding="utf-8")
    reader = sdl[sdl.index("static void *hp_dev, *hp_pad;"):sdl.index("#undef CALL", sdl.index("static void *hp_dev"))]
    close = block(sdl, "void hos_sdl_pad_close(void *pad) {")
    if "pad_headphones_forget(pad);" not in close:
        raise SystemExit("host/hos_sdl.c: closing a pad no longer forgets its jack reader")
    code = PRELUDE + "static void pad_headphones_forget(void *pad);\n" + close + "\n" + reader + FIXTURE
    with tempfile.TemporaryDirectory(prefix="pad-headphones-") as tmp:
        src, exe = Path(tmp) / "reader.c", Path(tmp) / "reader.exe"
        src.write_text(code, encoding="utf-8")
        subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", str(src), "-o", str(exe)],
                       check=True, timeout=120)
        subprocess.run([str(exe)], check=True, timeout=60)
    print("PASS: the jack reader lives for its pad's open life, a reused SDL pointer inherits nothing")


if __name__ == "__main__":
    main()
