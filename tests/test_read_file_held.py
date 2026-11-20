"""A file read into memory that is read-only for a moment waits it out (host/posix/win32_posix.c ReadFile).

Where the renderer leaves the game's big pools in ordinary memory until the GPU binds them (RX_VK_PLACED_LATER_MB, the arm64
port), it places such a pool at that binding with the pool read-only while its contents are copied
(gpu/vk/placed_memory.h place_live). A write by the CPU then waits in the renderer's fault handler; a write by the kernel
does not fault, it fails with EFAULT, and the game's file reads go straight into its memory (host/nn_fs.c, through the
posix ReadFile's pread). Builds ReadFile from the source as it is and checks, without the game:

- a read into a buffer that is read-only for 50 ms reads the whole file once it is writable again (it failed with EFAULT);
- a read into memory nothing maps fails at once, as before.

python -I -B tests/test_read_file_held.py [--cc gcc]
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

RUN_ALL_SKIP = {"windows": "the posix layer's ReadFile: Linux only"}

ROOT = Path(__file__).resolve().parents[1]

MAIN = r"""
static void *give_back(void *p) {   /* (the placement's end: writable again 50 ms on) */
    usleep(50000);
    mprotect(p, 1 << 18, PROT_READ | PROT_WRITE);
    return NULL;
}
static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
int main(int argc, char **argv) {
    (void)argc;
    const size_t n = 1 << 18;
    char path[4096];
    snprintf(path, sizeof path, "%s/read-held.bin", argv[1]);
    const int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return printf("FAIL: no file\n"), 1;
    unsigned char *want = malloc(n);
    for (size_t i = 0; i < n; i++) want[i] = (unsigned char)(i * 7 + 3);
    if (write(fd, want, n) != (ssize_t)n) return printf("FAIL: file not written\n"), 1;
    HFile f = {{H_FILE}, fd};
    int failures = 0;

    unsigned char *buf = mmap(NULL, n, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    pthread_t t;
    pthread_create(&t, NULL, give_back, buf);
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    DWORD got = 0;
    const double t0 = now_ms();
    const BOOL ok = ReadFile(&f, buf, (DWORD)n, &got, &ov);
    const double waited = now_ms() - t0;
    pthread_join(t, NULL);
    if (!ok || got != n || memcmp(buf, want, n)) {
        printf("FAIL: a read into memory read-only for 50 ms: %s, %u of %zu bytes (error %u)\n", ok ? "done" : "refused",
               (unsigned)got, n, (unsigned)last_error);
        failures++;
    } else if (waited < 30) {
        printf("FAIL: read in %.1f ms: the buffer was never read-only (the test's own setup)\n", waited);
        failures++;
    } else {
        printf("PASS: read once writable again, after %.0f ms\n", waited);
    }

    unsigned char *hole = mmap(NULL, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    munmap(hole, n);
    const double t1 = now_ms();
    got = 0;
    const BOOL bad = ReadFile(&f, hole, (DWORD)n, &got, &ov);
    if (bad || now_ms() - t1 > 500) {
        printf("FAIL: a read into memory nothing maps: %s after %.0f ms\n", bad ? "done" : "refused", now_ms() - t1);
        failures++;
    } else {
        printf("PASS: a read into memory nothing maps refused at once\n");
    }
    close(fd);
    unlink(path);
    return failures ? 1 : 0;
}
"""


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    posix = (ROOT / "host/posix/win32_posix.c").read_text(encoding="utf-8")
    pieces = [
        block(posix, "static DWORD error_of(int e) {"),
        "static BOOL fail_errno(void) { last_error = error_of(errno); return FALSE; }",
        "enum { H_THREAD = 0x48540001, H_EVENT, H_SEMA, H_TIMER, H_FILE, H_FIND };",
        "typedef struct { int type; } HObj;",
        "static int is(HANDLE h, int type) { return h && h != INVALID_HANDLE_VALUE && (uintptr_t)h > 0x10000 && ((HObj *)h)->type == type; }",
        "typedef struct { HObj h; int fd; } HFile;",
        *([block(posix, "static int page_mapped(const void *p) {")] if "static int page_mapped(" in posix else []),
        block(posix, "BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, OVERLAPPED *ov) {"),
    ]
    fixture = "\n".join([
        "#define _GNU_SOURCE",
        "#include <windows.h>",
        "#include <errno.h>",
        "#include <fcntl.h>",
        "#include <pthread.h>",
        "#include <stdint.h>",
        "#include <stdio.h>",
        "#include <stdlib.h>",
        "#include <string.h>",
        "#include <sys/mman.h>",
        "#include <time.h>",
        "#include <unistd.h>",
        "static __thread DWORD last_error;",
        *pieces,
        MAIN,
    ])
    with tempfile.TemporaryDirectory(prefix="read-held-") as tmp:
        src, exe = Path(tmp) / "read_held.c", Path(tmp) / "read_held"
        src.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cc, "-std=gnu11", "-I", str(ROOT / "host/posix"), "-Wall", "-Wno-unused-function", str(src), "-o",
                        str(exe), "-pthread"], check=True)
        r = subprocess.run([str(exe), tmp], capture_output=True, text=True, timeout=60)
        sys.stdout.write(r.stdout)
        if r.returncode:
            sys.stderr.write(r.stderr)
            raise SystemExit("ReadFile: a read into memory read-only for a moment is not waited out (above)")


if __name__ == "__main__":
    main()
