"""The write marks' walk (host/rx_backend.c collect_in) timed game-free, as the word walk and as the vector walk
(HOS_COLLECT_VECTOR), on mark arrays shaped like the game's. An indication only: warm caches, one thread, no game thread
writing marks on another core (in the game the walk also waits for the cache lines those writes move). The gain in the
game is measured in the game, with HOS_COLLECT_VECTOR=0 against the default.

    python -I scripts/collect_bench.py [--cc clang] [--arch arm64|x86_64] [--reps N] [--source FILE] [--keep DIR]

collect_in, marks_clear_256 and the vector walk are cut out of the source as tests/test_collect_wide.py cuts them, and
compiled twice in one program at -O2 (the runtime's level, nx2/build.py): HOS_MARKS_VECTOR 0 (the word walk) and as
built (the vector walk; the program is not built where it is not). On x86-64 two programs: SSE2's loads (-mno-avx2)
and AVX2's (-mavx2, as in a build for x86-64-v3; run where the CPU has AVX2). On arm64 NEON's. The mark array is the
real one's size (1 << 27 marks). Each repetition sets the shape's marks and times one walk, then sets them again and times the
other; the walks take turns at going first (the word walk in the even repetitions, the vector walk in the odd ones), so neither
always runs behind the other's reads of the same marks. The two walks' pages are compared at every repetition, and the program
stops if they differ. Prints one CSV table per program: the median and the fastest repetition of each walk, in microseconds.

Shapes (the ranges' sizes and marks a submit, as measured in the game on the macOS port):
- capture: 2,728 ranges, about 1,760 MB, 165 marks (a submit-time capture of the mirrored pools in a busy scene);
- collection: 60 ranges of 100 MB, 6,000 MB, 400 marks (the collection of every GPU-visible range);
- short: 2,728 ranges of 1-8 pages, unaligned (what a range costs);
- dense: one 256 MB range, every mark set (each page's take dominates);
- half: one 256 MB range, half the marks set at random.
On macOS --arch picks the slice (x86_64 runs under Rosetta on Apple silicon: an indication of the walk's instruction
count, not of an x86-64 PC).
"""
import argparse
import os
import platform
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

PROGRAM = r"""
#define _CRT_SECURE_NO_WARNINGS   /* (Windows' CRT marks getenv and memcpy deprecated: before the first include, as the header's own is) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "longshot_env.h"   /* (env_value_off: the switches' one rule, as host/rx_backend.c includes it) */
#ifdef _WIN32
#include <windows.h>
static uint64_t now_ns(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart * 1e9 / (double)f.QuadPart);
}
#else
#include <time.h>
static uint64_t now_ns(void) {
    struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW   /* (nanoseconds; macOS's CLOCK_MONOTONIC counts whole microseconds) */
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}
#endif

#define A64_DIRTY_PAGES (1ull << 27)
static uint8_t a64_dirty[A64_DIRTY_PAGES];
static uint64_t hos_guest_size(void) { return A64_DIRTY_PAGES << 12; }
void hos_log(const char *fmt, ...) { (void)fmt; }

/* the word walk: the source with the vector walk left out */
#define HOS_MARKS_VECTOR 0
#define marks_clear_256 word_marks_clear_256
@MARKS_CLEAR_256@
@VECTOR@
#define collect_in word_collect_in
@COLLECT_IN@
#undef collect_in
#undef marks_clear_256
#undef HOS_MARKS_VECTOR

/* the walk as built here */
@MARKS_CLEAR_256@
@VECTOR@
#if !(defined(HOS_MARKS_VECTOR) && HOS_MARKS_VECTOR)
#error "the vector walk is not built for this CPU or compiler"
#endif
@COLLECT_IN@

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17; return rng; }
static uint64_t ranges[4096][2];
static uint32_t *marked;   /* (the shape's marked pages) */
static size_t nmarked;
static int cmp_u64(const void *a, const void *b) {
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* (a page of the ranges, at random) */
static uint64_t page_in(int n, uint64_t pages) {
    uint64_t k = rnd() % pages;
    for (int r = 0; r < n; r++) {
        const uint64_t p0 = ranges[r][0] >> 12, p1 = (ranges[r][1] + 4095) >> 12;
        if (k < p1 - p0) return p0 + k;
        k -= p1 - p0;
    }
    return ranges[0][0] >> 12;
}

static int shape(const char *name, uint64_t *pages) {
    int n = 0;
    uint64_t at = 1ull << 18;   /* (1 GB in) */
    nmarked = 0;
    if (!strcmp(name, "capture") || !strcmp(name, "short")) {
        const int capture = !strcmp(name, "capture");
        for (n = 0; n < 2728; n++) {
            const uint64_t len = capture ? 1 + rnd() % 330 : 1 + rnd() % 8;
            ranges[n][0] = at << 12 | (rnd() & 4095);
            ranges[n][1] = (at + len) << 12 | (rnd() & 4095);
            at += len + 1 + rnd() % 2000;
        }
    } else if (!strcmp(name, "collection")) {
        for (n = 0; n < 60; n++) {
            ranges[n][0] = at << 12;
            ranges[n][1] = (at + 25600) << 12;
            at += 25600 + 4096 + rnd() % 8192;
        }
    } else {
        ranges[0][0] = at << 12, ranges[0][1] = (at + 65536) << 12, n = 1;
    }
    *pages = 0;
    for (int r = 0; r < n; r++) *pages += ((ranges[r][1] + 4095) >> 12) - (ranges[r][0] >> 12);
    const size_t want = !strcmp(name, "capture") ? 165 : !strcmp(name, "collection") ? 400 : !strcmp(name, "short") ? 165
                      : !strcmp(name, "dense") ? (size_t)*pages : (size_t)*pages / 2;
    marked = malloc(want * sizeof *marked);
    for (size_t i = 0; i < want; i++)
        marked[nmarked++] = (uint32_t)(!strcmp(name, "dense") ? (ranges[0][0] >> 12) + i : page_in(n, *pages));
    return n;
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    const int reps = atoi(argv[1]);
    printf("shape,ranges,MB,marks_set,pages_found,word_us_median,word_us_min,vector_us_median,vector_us_min,"
           "word_ns_per_range,vector_ns_per_range,speedup\n");
    for (int s = 2; s < argc; s++) {
        uint64_t pages;
        const int n = shape(argv[s], &pages);
        uint64_t *t[2] = {malloc(reps * sizeof(uint64_t)), malloc(reps * sizeof(uint64_t))};
        int64_t found = -1;
        for (int i = -5; i < reps; i++) {   /* (five warm-up repetitions) */
            uint32_t *got[2] = {0, 0};
            int64_t k[2];
            for (int s2 = 0; s2 < 2; s2++) {
                const int w = (i & 1) ? 1 - s2 : s2;   /* (the walks take turns at going first: repetition by repetition) */
                for (size_t j = 0; j < nmarked; j++) a64_dirty[marked[j]] = 1;
                const uint64_t t0 = now_ns();
                k[w] = (w ? collect_in : word_collect_in)((const uint64_t (*)[2])ranges, n, &got[w]);
                const uint64_t t1 = now_ns();
                if (i >= 0) t[w][i] = t1 - t0;
            }
            if (k[0] != k[1] || (k[0] > 0 && memcmp(got[0], got[1], (size_t)k[0] * sizeof *got[0]))) {
                fprintf(stderr, "%s: the walks differ (%lld and %lld pages)\n", argv[s], (long long)k[0], (long long)k[1]);
                return 1;
            }
            found = k[0];
            free(got[0]), free(got[1]);
        }
        double med[2], min[2];
        for (int w = 0; w < 2; w++) {
            qsort(t[w], reps, sizeof *t[w], cmp_u64);
            med[w] = t[w][reps / 2] / 1e3, min[w] = t[w][0] / 1e3;
        }
        printf("%s,%d,%.0f,%zu,%lld,%.2f,%.2f,%.2f,%.2f,%.1f,%.1f,%.1f\n", argv[s], n, pages * 4096.0 / (1 << 20), nmarked,
               (long long)found, med[0], min[0], med[1], min[1], med[0] * 1e3 / n, med[1] * 1e3 / n, med[0] / med[1]);
        free(t[0]), free(t[1]), free(marked);
    }
    return 0;
}
"""

# (x86-64: does this CPU have AVX2, for the program built with -mavx2? Built for the CPU's baseline)
AVX2_PROBE = r"""
int main(void) {
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") ? 0 : 1;
}
"""

SHAPES = ("capture", "collection", "short", "dense", "half")


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def program(src):
    at = src.find("/* (the vector walk, on x86-64 and arm64:")
    if at < 0:
        sys.exit("collect_bench: no vector walk in this source")
    return (PROGRAM.replace("@MARKS_CLEAR_256@", block(src, "static inline int marks_clear_256("))
            .replace("@VECTOR@", src[at:src.index("static int64_t collect_in(", at)])
            .replace("@COLLECT_IN@", block(src, "static int64_t collect_in(")))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cc", default="clang")
    ap.add_argument("--arch", choices=("arm64", "x86_64"), help="macOS only: the slice to build and run")
    ap.add_argument("--reps", type=int, default=301)
    ap.add_argument("--source", type=Path, default=ROOT / "host" / "rx_backend.c")
    ap.add_argument("--keep", type=Path, help="keep the programs and their source in this folder")
    a = ap.parse_args()
    darwin = sys.platform == "darwin"
    if a.arch and not darwin:
        ap.error("--arch is for macOS")
    cpu = (a.arch or platform.machine()).lower()
    flags, run = (["-arch", a.arch], ["arch", "-" + a.arch]) if a.arch else ([], [])
    if cpu in ("x86_64", "amd64"):
        builds = [("SSE2", ["-mno-avx2"]), ("AVX2", ["-mavx2"])]
    elif cpu in ("arm64", "aarch64"):
        builds = [("NEON", [])]
    else:
        sys.exit("collect_bench: the vector walk is built for x86-64 and arm64 only, not %s" % cpu)
    src = a.source.read_text(encoding="utf-8")
    failed = 0
    env = {k: v for k, v in os.environ.items() if k not in ("HOS_COLLECT_VECTOR", "HOS_COLLECT_WIDE")}   # (both walks at their defaults)
    with tempfile.TemporaryDirectory(prefix="collect-bench-") as tmp:
        out = a.keep or Path(tmp)
        out.mkdir(parents=True, exist_ok=True)
        c = out / "collect_bench.c"
        c.write_text(program(src), encoding="utf-8")
        for loads, extra in builds:
            exe = out / ("collect_bench-" + cpu + "-" + loads)
            subprocess.run([a.cc, "-std=gnu17", "-O2", "-g0", "-Wall", "-Wextra", "-I", str(ROOT / "gpu" / "vk" / "compat"), *flags,
                            *extra, str(c), "-o", str(exe)], check=True)
            if loads == "AVX2":
                probe = out / "avx2_probe.c"
                probe.write_text(AVX2_PROBE, encoding="utf-8")
                subprocess.run([a.cc, *flags, str(probe), "-o", str(out / "avx2_probe")], check=True)
                if subprocess.run(run + [str(out / "avx2_probe")]).returncode:
                    print("%s, %s: not run (this CPU has no AVX2)" % (cpu, loads), flush=True)
                    continue
            print("%s, the vector walk in %s's loads (an indication only: warm caches, one thread, no game), %d repetitions:"
                  % (cpu + (" under Rosetta" if darwin and cpu == "x86_64" and platform.machine() == "arm64" else ""),
                     loads, a.reps), flush=True)
            done = subprocess.run(run + [str(exe), str(a.reps), *SHAPES], env=env)
            failed |= done.returncode != 0
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
