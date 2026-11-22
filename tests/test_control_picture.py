"""Check that the control bridge observes the native texture and crop at host dispatch.

Run with python -I -B tests/test_control_picture.py --cc clang.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from srccut import block

RUN_ALL_ARGS = ["--cc", "{cc}"]
ROOT = Path(__file__).resolve().parents[1]

HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL: %s\n", #x); exit(1); } } while (0)
typedef struct { struct { uint32_t width, height; } d; } nvn_tex;
typedef long LONG;
volatile int hos_control_on;
static int dispatched;
static LONG InterlockedIncrement(volatile LONG *p) { return ++*p; }
static void hos_slow_frame_present(void) {}
static void hos_ab_present(void) {}
static void wgr_nvn_present(const nvn_tex *t, const int crop[4]);
static struct {
    void (*nvn_present)(const nvn_tex *, const int[4]);
    void (*nvn_present_trial)(const nvn_tex *, const int[4], uint64_t);
} *vk, renderer;
"""

CASES = r"""
static unsigned width_at_dispatch;
static void wgr_nvn_present(const nvn_tex *t, const int crop[4]) {
    (void)t;
    (void)crop;
    HosControlPicture size;
    width_at_dispatch = rx_control_picture(&size) ? size.texture_width : 0;
    ++dispatched;
}

static uint64_t tagged;
static void present_trial(const nvn_tex *t, const int crop[4], uint64_t generation) {
    tagged = generation;
    wgr_nvn_present(t, crop);
}

static void expect(unsigned texture_w, unsigned texture_h, unsigned picture_w, unsigned picture_h) {
    HosControlPicture size = {0};
    CHECK(rx_control_picture(&size));
    CHECK(size.texture_width == texture_w && size.texture_height == texture_h);
    CHECK(size.width == picture_w && size.height == picture_h);
}

int main(void) {
    HosControlPicture size;
    const nvn_tex old = {{2560, 1440}}, next = {{1920, 1080}}, invalid = {{65536, 1080}};
    const int crop[] = {8, 16, 1920, 1080}, empty[] = {0, 0, 0, 0}, partial[] = {0, 0, 800, 0};
    CHECK(!rx_control_picture(&size));
    rx_nvn_present(&old, crop);
    CHECK(!rx_control_picture(&size));

    hos_control_on = 1;
    rx_nvn_present(&old, crop);
    CHECK(width_at_dispatch == 2560);
    expect(2560, 1440, 1920, 1080);
    rx_nvn_present(&next, empty);
    CHECK(width_at_dispatch == 1920);
    expect(1920, 1080, 1920, 1080);
    rx_nvn_present(&old, partial);
    expect(2560, 1440, 2560, 1440);
    rx_nvn_present(&invalid, empty);
    CHECK(!rx_control_picture(&size));
    rx_nvn_present(&next, 0);
    expect(1920, 1080, 1920, 1080);
    rx_nvn_present(0, empty);
    CHECK(!rx_control_picture(&size));
    CHECK(dispatched == 7);
    renderer.nvn_present_trial = present_trial;
    vk = &renderer;
    rx_nvn_present_trial(&old, crop, 17);
    CHECK(dispatched == 8 && tagged == 17 && width_at_dispatch == 2560);
    expect(2560, 1440, 1920, 1080);
    puts("PASS: native source texture and crop, including disabled, missing, invalid and repeated observations");
}
"""


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--cc", default=os.environ.get("CC", "clang"))
    args = cli.parse_args()
    source = (ROOT / "host/rx_backend.c").read_text(encoding="utf-8")
    if "int rx_control_picture(" not in source:
        raise SystemExit("FAIL: host dispatch supplies no control-bridge native picture observation")
    observe = block(source, "static void control_picture_note(")
    read = block(source, "int rx_control_picture(")
    dispatch = block(source, "static void nvn_present_with_generation(")
    stock = block(source, "void rx_nvn_present(")
    trial = block(source, "void rx_nvn_present_trial(")
    with tempfile.TemporaryDirectory() as folder:
        src = Path(folder) / "picture.c"
        exe = Path(folder) / "picture.exe"
        src.write_text(HARNESS + '\n#include "control.h"\n'
                       + '\nstatic volatile LONG presents;\nstatic uint64_t control_picture;\n'
                       + '\n'.join((observe, read, dispatch, stock, trial)) + CASES, encoding="utf-8")
        subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "host"),
                        str(src), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
