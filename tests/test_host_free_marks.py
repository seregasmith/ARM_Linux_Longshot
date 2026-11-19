"""Guest pages the host zeroes at a free are marked written when the renderer has them imported (host/hos_mem.c,
host/hos_svc.c, host/hos.h).

Pages the Vulkan back end imported are not handed back to the system at a free: the GPU still maps them, so they stay
where they are and are zeroed in place. Those zeros are a write of guest memory like any the game makes, and the
renderer learns of a write by its page's mark alone (HOS_WRITTEN, the lifted code's A64_MARK). Zeroed with no mark, the
renderer's mirror, its capture entries and its write-back shadow of the pages went on holding the bytes from before
the free, and a later write-back could put them back over the zeros.

The fixture is host/hos_mem.c as it is, built over a 1 MB window at the top of the guest space with stand-ins for what
it calls, and svc UnmapPhysicalMemory's case cut out of host/hos_svc.c. For each of the three paths
(nn::os::FreeMemoryPages, a freed address region's discard_pages, svc UnmapPhysicalMemory):

- imported: every page of the range reads zero, every one of them is marked, none outside it is, and each mark was made
  after its page's bytes were written (a mark the capture takes before the bytes land is a write nobody hears of);
- not imported: the pages go back to the system, nothing is written and nothing is marked.

On Windows the freed region's pages took DiscardVirtualMemory with no look at the imports; the same fixture, built
there, shows them zeroed and marked like Linux's.

python -I -B tests/test_host_free_marks.py [--cc gcc]
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block, between   # noqa: E402

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
# (env_on, the switches' rule, before host/hos_mem.c: it reads gpu/vk/compat/longshot_env.h by a path from host/)
ENV_ON_H = str(ROOT / "gpu" / "vk" / "compat" / "longshot_env.h")

# A hos.h of the check's own: the guest window is the top megabyte of the real address map (where hos_region_alloc
# puts its first region), and HOS_WRITTEN notes the mark instead of setting a64_dirty.
HOS_H = r'''
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef struct { uint64_t x[32]; } Ctx;
#define HLE(sym) void imp_##sym(Ctx *c)
#define ARG(n) (c->x[n])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define HOS_SYSTEM_BASE 0x01000000ULL
#define HOS_SYSTEM_END 0x07000000ULL
#define HOS_HEAP_BASE 0x1000000000ULL
#define HOS_GUEST_SPACE (1ULL << 39)
#define HOS_SPACE_END 0x8000000000ULL
#define WIN_BYTES 0x100000ULL
#define WIN_PAGES (WIN_BYTES >> 12)
#define WIN_BASE (HOS_SPACE_END - WIN_BYTES)
extern uint8_t win[];
#define GP(a) ((void *)(win + ((uint64_t)(a) - WIN_BASE)))
#define G64(a) (*(uint64_t *)GP(a))
#define G32(a) (*(uint32_t *)GP(a))
static inline void *hos_guest_range(uint64_t a, uint64_t n) {
    return a >= WIN_BASE && a < HOS_SPACE_END && n <= HOS_SPACE_END - a ? GP(a) : (void *)0;
}
void test_written(const void *p, size_t n);
#define HOS_WRITTEN(p, n) test_written((p), (n))
@HELPERS@
extern const uint64_t hos_image_base, hos_image_end;
int hos_commit(uint64_t addr, uint64_t size);
void hos_log(const char *fmt, ...);
void hos_fatal(Ctx *c, const char *fmt, ...);
int64_t hos_now_ns(void);
#define HOS_TRACE_CALL(name) ((void)0)
'''

# What both fixtures share: the window, the marks as HOS_WRITTEN made them, and what the checks ask of a range.
COMMON_H = r'''
#include <stdio.h>
#include <stdlib.h>
#include "hos.h"
_Alignas(4096) uint8_t win[WIN_BYTES];
static uint8_t marks[WIN_PAGES];
static int marked_before_write;   /* (a page marked while it still held the bytes from before the free) */
void test_written(const void *p, size_t n) {
    const uint64_t a = (uint64_t)((const uint8_t *)p - win);
    if (!n || (const uint8_t *)p < win || a >= WIN_BYTES) return;
    for (uint64_t q = a >> 12; q <= (a + n - 1) >> 12 && q < WIN_PAGES; q++) {
        marks[q] = 1;
        if (win[q << 12] || win[(q << 12) + 0xFFF]) marked_before_write = 1;
    }
}
static int imported_answer, imported_calls;
int rx_guest_imported(uint64_t lo, uint64_t hi) {
    (void)lo, (void)hi;
    imported_calls++;
    return imported_answer;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
/* (the range filled with what the game left there, and no page marked) */
static void stale(uint64_t a, uint64_t n) {
    memset(GP(a), 0xAB, (size_t)n);
    memset(marks, 0, sizeof marks);
    marked_before_write = 0;
}
static int bytes_are(uint64_t a, uint64_t n, uint8_t v) {
    const uint8_t *p = (const uint8_t *)GP(a);
    for (uint64_t i = 0; i < n; i++)
        if (p[i] != v) return 0;
    return 1;
}
/* (the pages of [a, a + n): how many of them are marked) */
static int marked_in(uint64_t a, uint64_t n) {
    int k = 0;
    for (uint64_t q = (a - WIN_BASE) >> 12; q < (a + n - WIN_BASE) >> 12; q++) k += marks[q];
    return k;
}
static int marked_in_all(void) {
    int k = 0;
    for (uint64_t q = 0; q < WIN_PAGES; q++) k += marks[q];
    return k;
}
'''

MEM_FIXTURE = r'''
#include "common.h"
#include <stdarg.h>
const uint64_t hos_image_base = 0x80000000ULL, hos_image_end = 0x80001000ULL;
int hos_large_pages;   /* (Windows: hos_main.c's) */
static int discards;   /* (Linux: pages handed back to the system) */
int hos_posix_guest_discard(void *p, size_t n) {
    (void)p, (void)n;
    discards++;
    return 0;
}
int hos_commit(uint64_t addr, uint64_t size) {
    (void)addr, (void)size;
    return 1;
}
void hos_log(const char *fmt, ...) { (void)fmt; }
void hos_fatal(Ctx *c, const char *fmt, ...) {
    (void)c;
    fprintf(stderr, "FAIL: hos_fatal: %s\n", fmt);
    exit(1);
}
int64_t hos_now_ns(void) { return 0; }
#include "hos_mem.c"
int main(void) {
    Ctx c = {{0}};
    const uint64_t a = WIN_BASE + 0x10000, n = 0x8000;   /* (eight pages, above 4 GB: FreeMemoryPages' own branch) */

    /* nn::os::FreeMemoryPages over pages the renderer imported: zeroed in place, and the renderer told */
    stale(a, n);
    imported_answer = 1;
    c.x[0] = a, c.x[1] = n;
    imp__ZN2nn2os15FreeMemoryPagesEmm(&c);
    CHECK(c.x[0] == 0);
    CHECK(bytes_are(a, n, 0));
    CHECK(marked_in(a, n) == 8);          /* (every zeroed page is marked written) */
    CHECK(marked_in_all() == 8);          /* (and no page outside the range) */
    CHECK(!marked_before_write);          /* (the bytes first, then the mark) */

    /* not imported: handed back to the system, nothing written here, nothing marked */
    stale(a, n);
    imported_answer = 0, discards = 0;
    c.x[0] = a, c.x[1] = n;
    imp__ZN2nn2os15FreeMemoryPagesEmm(&c);
    CHECK(c.x[0] == 0);
    CHECK(marked_in_all() == 0);
#if !defined(_WIN32)
    CHECK(discards == 1);
    CHECK(bytes_are(a, n, 0xAB));         /* (the stand-in discards nothing: the host wrote no byte itself) */
#endif

    /* a freed address region (hos_region_free, discard_pages), imported: the same on every system. Windows discarded
       these pages without asking whether the GPU had them */
    const uint64_t size = 0x20000;
    const uint64_t r = hos_region_alloc(size, 0x10000);
    CHECK(r >= WIN_BASE && r + size <= HOS_SPACE_END);
    stale(r, size);
    imported_answer = 1, imported_calls = 0, discards = 0;
    hos_region_free(r);
    CHECK(imported_calls == 1);
    CHECK(bytes_are(r, size, 0));
    CHECK(marked_in(r, size) == 32);
    CHECK(marked_in_all() == 32);
    CHECK(!marked_before_write);
    CHECK(discards == 0);                 /* (no hole punched under the import) */

    /* and not imported: discarded, not marked */
    const uint64_t r2 = hos_region_alloc(size, 0x10000);
    CHECK(r2 >= WIN_BASE && r2 + size <= HOS_SPACE_END);
    stale(r2, size);
    imported_answer = 0, discards = 0;
    hos_region_free(r2);
    CHECK(marked_in_all() == 0);
#if !defined(_WIN32)
    CHECK(discards == 1);
    CHECK(bytes_are(r2, size, 0xAB));
#endif
    printf("PASS: FreeMemoryPages and a freed region zero imported pages in place and mark them written\n");
    return 0;
}
'''

SVC_FIXTURE = r'''
#include "common.h"
/* (what the case uses of hos_svc.c and of Windows) */
typedef size_t SIZE_T;
#define MEM_DECOMMIT 0x4000
#define R_OK 0
static int decommits;
static int VirtualFree(void *p, SIZE_T n, int how) {
    (void)p, (void)n, (void)how;
    decommits++;
    return 1;
}
static void svc(Ctx *c, int number) {
    switch (number) {
    @CASE@
    default:
        break;
    }
}
int main(void) {
    Ctx c = {{0}};
    const uint64_t a = WIN_BASE + 0x40000, n = 0x4000;
    stale(a, n);
    imported_answer = 1;
    c.x[0] = a, c.x[1] = n;
    svc(&c, 0x2D);
    CHECK(c.x[0] == R_OK);
    CHECK(decommits == 0);
    CHECK(bytes_are(a, n, 0));
    CHECK(marked_in(a, n) == 4);
    CHECK(marked_in_all() == 4);
    CHECK(!marked_before_write);

    stale(a, n);
    imported_answer = 0;
    c.x[0] = a, c.x[1] = n;
    svc(&c, 0x2D);
    CHECK(c.x[0] == R_OK);
    CHECK(decommits == 1);
    CHECK(bytes_are(a, n, 0xAB));
    CHECK(marked_in_all() == 0);

    c.x[0] = 0x1000, c.x[1] = n;          /* (outside the guest window: refused, nothing touched) */
    svc(&c, 0x2D);
    CHECK(c.x[0] == 0xCA01);
    CHECK(decommits == 1);
    printf("PASS: svc UnmapPhysicalMemory zeroes imported pages in place and marks them written\n");
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()

    hos = (ROOT / "host" / "hos.h").read_text(encoding="utf-8")
    # (the real helpers, over the stand-in GP and HOS_WRITTEN. A source from before them has none and calls none: the
    # fixture then builds as that source is, and fails on the marks it does not make)
    helpers = "".join(block(hos, marker) + "\n" for marker in ("static inline void hos_guest_fill(",
                                                                "static inline void hos_guest_copy(") if marker in hos)
    svc = (ROOT / "host" / "hos_svc.c").read_text(encoding="utf-8")
    case = between(svc, "case 0x2D:", "case 0x1F:")

    env = {k: v for k, v in os.environ.items() if k != "HOS_KEEP_FREED_PAGES"}   # (the switch that keeps freed pages)
    with tempfile.TemporaryDirectory(prefix="host-free-marks-") as tmp:
        tmp = Path(tmp)
        (tmp / "hos.h").write_text(HOS_H.replace("@HELPERS@", helpers), encoding="utf-8")
        (tmp / "common.h").write_text(COMMON_H, encoding="utf-8")
        # (beside the stand-in hos.h: an include in quotes looks in the including file's folder first)
        (tmp / "hos_mem.c").write_text((ROOT / "host" / "hos_mem.c").read_text(encoding="utf-8"), encoding="utf-8")
        (tmp / "mem.c").write_text(MEM_FIXTURE, encoding="utf-8")
        (tmp / "svc.c").write_text(SVC_FIXTURE.replace("@CASE@", case), encoding="utf-8")
        for name in ("mem", "svc"):
            exe = tmp / (name + (".exe" if os.name == "nt" else ""))
            cmd = [a.cc, "-std=gnu11", "-include", ENV_ON_H, "-Wall", "-Wextra", "-I", str(tmp), str(tmp / (name + ".c")),
                   "-o", str(exe)]
            if os.name != "nt":
                cmd.append("-pthread")
            subprocess.run(cmd, check=True)
            subprocess.run([str(exe)], check=True, env=env)


if __name__ == "__main__":
    main()
