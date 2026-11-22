"""The write marks' collection finds the same pages whichever way it walks the marks (host/rx_backend.c collect_in).

collect_in, its word mark test (marks_clear_256) and the vector walk's code are cut out of the source and compiled twice in
one program against a small mark array: as the word walk (HOS_MARKS_VECTOR 0: the walk before the vector one, and the walk
with HOS_COLLECT_VECTOR=0) and as built here (on x86-64 and arm64 the vector walk: 256 marks a test while none is set, else 64,
in SSE2's 16-byte loads, AVX2's 32-byte ones or NEON's 16-byte ones). For random marks and ranges (aligned or not,
short, empty, overlapping, near page 0, at and past the array's end, past the guest's end, marks just outside each range,
dense runs) each walk's pages
found, their order and the marks it leaves must be a plain walk's, and the two walks' must be the same, with
HOS_COLLECT_WIDE and HOS_COLLECT_VECTOR on and off (the switch's values by the on/off rule: off, false, no, 0 and empty turn
the vector walk off). The fixture also checks that the steps are really taken (4096 clear marks: 16 tests of 256 in the
word walk and in the vector walk, none of 64; 130 clear marks from page 5: 3 vector tests of 64; 600 from page 5: 2 of 256
and 2 of 64; one mark at page 300 of 1024; 4096 set marks: 64 vector tests of 64, each window's marks taken from one read),
that the walk is logged once with the walk and loads this run takes, and which vector walk is built.
A mark a producer publishes while a page is taken (the fixture's acquire exchange, the take, sets the mark of another page right
after its n-th call, as a lifted store or HOS_WRITTEN does from another thread) must be taken in this pass if its page lies
after the page just taken and left set for the next pass if it lies before it, in the word walk and in the walk built here, for
every page taken in turn: no mark is lost and none is taken twice.
Each vector walk this CPU can have is built and run: on x86-64 SSE2's (built with -mno-avx2) and AVX2's (built with
-mavx2; run where the CPU has AVX2), on arm64 NEON's. On another CPU only the word walk is built, and it is checked
against the plain walk.

python -I -B tests/test_collect_wide.py [--cc gcc] [--arch arm64|x86_64 ...] [--source host/rx_backend.c]
On macOS --arch picks the slices (default: this Mac's own; x86_64 on Apple silicon runs under Rosetta).
"""
import argparse
import os
import platform
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# (HOS_COLLECT_VECTOR follows the switches' one rule, env_value_off in gpu/vk/compat/longshot_env.h: unset is on, the words
#  below, any case and with spaces around them, are off, anything else is on. HOS_COLLECT_WIDE, main's own switch, reads only
#  a leading 0)
OFF = ("", "0", "false", "off", "no")
VECTOR_VALUES = (None, "1", "0", "off", "false", "no", "OFF", " No ", "", "on", "yes", "2")

FIXTURE = r"""
#define _POSIX_C_SOURCE 200112L
#define _CRT_SECURE_NO_WARNINGS   /* (Windows' CRT marks getenv and memcpy deprecated: before the first include, as the header's own is) */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "longshot_env.h"   /* (env_value_off: the switches' one rule, as host/rx_backend.c includes it) */

#define A64_DIRTY_PAGES (1ull << 16)
static uint8_t a64_dirty[A64_DIRTY_PAGES];
static uint64_t guest_size = A64_DIRTY_PAGES << 12;
static uint64_t hos_guest_size(void) { return guest_size; }
static int word_wide_calls, wide_calls, vector_calls, vector256_calls, logged;
static char log_line[256];
void hos_log(const char *fmt, ...) {   /* (not static: unused where the vector walk is not built) */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(log_line, sizeof log_line, fmt, ap);
    va_end(ap);
    logged++;
}
static int hos_ab_value(const char *name, int normal) { (void)name; return normal; }   /* (host/hos_ab.c: no A/B here) */

/* the seam: a producer's mark published while a page is taken. Both walks take a page with
   __atomic_exchange_n(&a64_dirty[p], 0, __ATOMIC_ACQUIRE); in the code cut out below that call is this function, which takes the
   page and then, after the seam_after-th page taken, sets the mark of seam_page (what a lifted store or HOS_WRITTEN does from
   another thread, at that point of the walk) */
static int seam_taken, seam_after = -1;
static uint64_t seam_page;
static inline uint8_t seam_exchange(uint8_t *m, uint8_t v) {
    const uint8_t old = __atomic_exchange_n(m, v, __ATOMIC_ACQUIRE);
    if (old && ++seam_taken == seam_after) a64_dirty[seam_page] = 1;
    return old;
}

/* the word walk: the source with the vector walk left out */
#define HOS_MARKS_VECTOR 0
#define marks_clear_256 word_marks_clear_256
@MARKS_CLEAR_256@
#undef marks_clear_256
#define marks_clear_256(m) (word_wide_calls++, word_marks_clear_256(m))
@VECTOR@
#define collect_in word_collect_in
#define __atomic_exchange_n(m, v, o) seam_exchange((m), (v))
@COLLECT_IN@
#undef collect_in
#undef marks_clear_256
#undef HOS_MARKS_VECTOR

/* the walk as built here */
@MARKS_CLEAR_256@
#define marks_clear_256(m) (wide_calls++, marks_clear_256(m))
@VECTOR@
#if defined(HOS_MARKS_VECTOR) && HOS_MARKS_VECTOR
#define BUILT 1
#define marks_from(p, p1) (vector_calls++, marks_from(p, p1))
#define marks_vclear_256(m) (vector256_calls++, marks_vclear_256(m))
#else
#define BUILT 0
#endif
@COLLECT_IN@
#undef marks_clear_256
#undef marks_from
#undef marks_vclear_256
#undef __atomic_exchange_n

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) {
    rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
    return rng;
}

/* (the plain walk: every page of every range, in order) */
static size_t plain(const uint64_t (*ranges)[2], int n, const uint8_t *marks, uint8_t *left, uint32_t *out) {
    size_t k = 0;
    memcpy(left, marks, A64_DIRTY_PAGES);
    for (int r = 0; r < n; r++) {
        const uint64_t hi = ranges[r][1] < guest_size ? ranges[r][1] : guest_size;
        uint64_t p1 = (hi + 4095) >> 12;
        if (p1 > A64_DIRTY_PAGES) p1 = A64_DIRTY_PAGES;
        for (uint64_t p = ranges[r][0] >> 12; p < p1; p++)
            if (left[p]) left[p] = 0, out[k++] = (uint32_t)p;
    }
    return k;
}

typedef int64_t (*Walk)(const uint64_t (*)[2], int, uint32_t **);
static uint8_t marks[A64_DIRTY_PAGES], left[A64_DIRTY_PAGES], left_word[A64_DIRTY_PAGES];
static uint32_t expect[A64_DIRTY_PAGES], got_word[A64_DIRTY_PAGES], got[A64_DIRTY_PAGES];
/* (one walk over a copy of the marks: its pages kept in out, its marks left in a64_dirty) */
static int64_t walk(Walk w, const uint64_t (*ranges)[2], int n, uint32_t *out) {
    memcpy(a64_dirty, marks, sizeof marks);
    uint32_t *pages = 0;
    const int64_t k = w(ranges, n, &pages);
    if (k > 0) memcpy(out, pages, (size_t)k * sizeof *out);
    free(pages);
    return k;
}

int main(int argc, char **argv) {
    const int want_wide = argc > 2 && argv[1][0] == '1', want_vector = argc > 2 && argv[2][0] == '1';
    static uint64_t ranges[64][2];
    const uint64_t (*cranges)[2] = (const uint64_t (*)[2])ranges;
#if BUILT
    printf("vector walk: built, %s\n", MARKS_LOADS);
#else
    printf("vector walk: not built\n");
#endif
    for (int round = 0; round < 1600; round++) {
        memset(marks, 0, sizeof marks);
        const int density = round % 8;   /* (0: no mark at all; then sparse to dense, some in clusters) */
        const uint64_t count = density == 0 ? 0 : (1ull << (density * 2));
        for (uint64_t i = 0; i < count; i++) {
            const uint64_t at = rnd() % A64_DIRTY_PAGES;
            marks[at] = (uint8_t)(1 + rnd() % 255);   /* (any value but 0 is a mark) */
            if (density == 5) marks[(at + 1) % A64_DIRTY_PAGES] = marks[(at + 255) % A64_DIRTY_PAGES] = 1;
        }
        if (round % 5 == 1) marks[255] = marks[256] = marks[511] = marks[A64_DIRTY_PAGES - 1] = 1;   /* (the steps' edges) */
        if (round % 19 == 3) {   /* (a dense run) */
            const uint64_t at = rnd() % A64_DIRTY_PAGES, len = 1 + rnd() % 3000;
            for (uint64_t i = 0; i < len && at + i < A64_DIRTY_PAGES; i++) marks[at + i] = 1;
        }
        guest_size = round % 7 == 3 ? (rnd() % A64_DIRTY_PAGES) << 12 | (rnd() & 4095) : A64_DIRTY_PAGES << 12;
        int n = (int)(rnd() % 12);
        const int shape = round % 13;   /* (4: near page 0; 9: at the array's end; 11: anywhere, overlapping) */
        uint64_t at = shape == 4 ? rnd() % 70 : shape == 9 ? A64_DIRTY_PAGES - 1 - rnd() % 400 : rnd() % 4096;
        for (int r = 0; r < n; r++) {   /* (sorted, disjoint, as hos_gpu_guest_ranges gives them, but for shape 11) */
            const uint64_t len = shape == 4 ? rnd() % 100 : round % 3 == 0 ? rnd() % 300 : rnd() % 40000;
            if (shape == 11) at = rnd() % A64_DIRTY_PAGES;
            ranges[r][0] = at << 12 | (round % 2 ? rnd() & 4095 : 0);
            ranges[r][1] = (at + len) << 12 | (round % 2 ? rnd() & 4095 : 0);
            if (ranges[r][1] < ranges[r][0]) ranges[r][1] = ranges[r][0];
            if (shape == 9 && r == n - 1) ranges[r][1] = rnd() % 2 ? A64_DIRTY_PAGES << 12 : (A64_DIRTY_PAGES + rnd() % 70) << 12;
            at += len + 1 + rnd() % 600;
            if (round % 11 == 7 && r == n - 1) ranges[r][1] = (A64_DIRTY_PAGES + 5000) << 12;   /* (past the array) */
        }
        if (round % 4 == 2) {   /* (marks just outside each range, and at its ends) */
            for (int r = 0; r < n; r++) {
                const uint64_t p0 = ranges[r][0] >> 12, p1 = (ranges[r][1] + 4095) >> 12;
                if (p0 > 0 && p0 - 1 < A64_DIRTY_PAGES) marks[p0 - 1] = 1;
                if (p0 < A64_DIRTY_PAGES) marks[p0] = 1;
                if (p1 > 0 && p1 - 1 < A64_DIRTY_PAGES) marks[p1 - 1] = 1;
                if (p1 < A64_DIRTY_PAGES) marks[p1] = 1;
            }
        }
        const size_t k = plain(cranges, n, marks, left, expect);
        const int64_t word = walk(word_collect_in, cranges, n, got_word);
        memcpy(left_word, a64_dirty, sizeof left_word);
        const int64_t built = walk(collect_in, cranges, n, got);
        if ((size_t)word != k || (k && memcmp(got_word, expect, k * sizeof *got_word)) || memcmp(left_word, left, sizeof left)) {
            printf("round %d: the word walk found %lld pages, %zu expected, or other pages, or left other marks\n", round, (long long)word, k);
            return 1;
        }
        if (built != word || (k && memcmp(got, got_word, k * sizeof *got))) {
            printf("round %d: the walk built here found %lld pages, the word walk %lld, or other pages or in another order\n",
                   round, (long long)built, (long long)word);
            return 1;
        }
        if (memcmp(a64_dirty, left_word, sizeof left_word)) {
            printf("round %d: the walk built here left other marks than the word walk\n", round);
            return 1;
        }
    }
    /* (a mark published while a page is taken, PR 936's review: every read that decides a page after the one just taken is made
       after it, so a page that lies after it is taken in this pass, and one that lies before it stays marked for the next; the
       word walk and the walk built here both, for random marks and ranges, the mark published after each page taken in turn.
       No mark is lost and none is taken twice) */
    {
        long schedules = 0;
        static uint32_t found[A64_DIRTY_PAGES], want[A64_DIRTY_PAGES];
        guest_size = A64_DIRTY_PAGES << 12;
        memset(a64_dirty, 0, sizeof a64_dirty);
        for (int which = 0; which < 2; which++) {   /* (the review's case: pages 1 and 63 marked in [1,65), page 2 published when page 1 is taken) */
            memset(marks, 0, sizeof marks);
            marks[1] = marks[63] = 1;
            ranges[0][0] = 1ull << 12, ranges[0][1] = 65ull << 12;
            memcpy(a64_dirty, marks, 2048);
            seam_taken = 0, seam_after = 1, seam_page = 2;
            uint32_t *pages = 0;
            const int64_t n = (which ? collect_in : word_collect_in)(cranges, 1, &pages);
            seam_after = -1;
            const int ok = n == 3 && pages[0] == 1 && pages[1] == 2 && pages[2] == 63 && !a64_dirty[1] && !a64_dirty[2] && !a64_dirty[63];
            free(pages);
            if (!ok) {
                printf("%s: pages 1 and 63 marked in [1,65), page 2 published when page 1 is taken: %lld pages found, 1, 2 and 63 expected, "
                       "page 2 %s\n", which ? "the walk built here" : "the word walk", (long long)n, a64_dirty[2] ? "left marked" : "not left marked");
                return 1;
            }
        }
        for (int round = 0; round < 500; round++) {   /* (ranges lie below page 1100: only the first 2048 marks are copied and checked) */
            memset(marks, 0, sizeof marks);
            const uint64_t lo = rnd() % 300, hi = lo + 1 + rnd() % 700;
            for (int i = 1 + (int)(rnd() % 6); i > 0; i--) marks[lo + rnd() % (hi - lo)] = 1;
            if (round % 3 == 0)
                for (uint64_t s = lo + rnd() % (hi - lo), i = 0; i < 24 && s + i < hi; i++) marks[s + i] = 1;
            ranges[0][0] = lo << 12, ranges[0][1] = hi << 12;
            uint64_t at[64];
            int nm = 0;
            for (uint64_t p = lo; p < hi && nm < 64; p++)
                if (marks[p]) at[nm++] = p;
            for (int j = 1; j <= nm; j++) {
                for (int c = 0; c < 9; c++) {
                    uint64_t q = c == 0 ? at[j - 1] + 1 : lo + rnd() % (hi - lo);
                    if (q >= hi || marks[q]) continue;
                    size_t w = 0;
                    for (int i = 0; i < nm; i++)
                        want[w++] = (uint32_t)at[i];
                    if (q > at[j - 1]) {   /* (after the page just taken: this pass takes it, in address order) */
                        size_t i = w++;
                        while (i && want[i - 1] > q) want[i] = want[i - 1], i--;
                        want[i] = (uint32_t)q;
                    }
                    for (int which = 0; which < 2; which++) {
                        memcpy(a64_dirty, marks, 2048);
                        seam_taken = 0, seam_after = j, seam_page = q;
                        uint32_t *pages = 0;
                        const int64_t n = (which ? collect_in : word_collect_in)(cranges, 1, &pages);
                        seam_after = -1;
                        if (n > 0) memcpy(found, pages, (size_t)n * sizeof *found);
                        free(pages);
                        int left_q = a64_dirty[q] != 0, left_other = 0;
                        for (uint64_t p = 0; p < 2048; p++)
                            if (a64_dirty[p] && p != q) left_other = 1;
                        if ((size_t)n != w || (w && memcmp(found, want, w * sizeof *found)) || left_other || left_q != (q < at[j - 1])) {
                            printf("%s: range [%llu,%llu), %d marks, page %llu published after page %llu was taken: %lld pages found, %zu "
                                   "expected, page %llu %s\n", which ? "the walk built here" : "the word walk", (unsigned long long)lo,
                                   (unsigned long long)hi, nm, (unsigned long long)q, (unsigned long long)at[j - 1], (long long)n, w,
                                   (unsigned long long)q, left_q ? "left marked" : "not left marked");
                            return 1;
                        }
                        schedules++;
                    }
                }
            }
        }
        printf("a mark published after each page taken: %ld schedules, both walks take it in the pass if it lies after the page, else leave it\n",
               schedules);
    }
    /* (the steps are taken: 4096 clear marks in 16 tests of 256 in the word walk and in the vector walk, none with a switch
       off; the vector walk's tests of 64 only where fewer than 256 are left) */
    memset(marks, 0, sizeof marks);
    guest_size = A64_DIRTY_PAGES << 12;
    ranges[0][0] = 0, ranges[0][1] = 4096ull << 12;
    word_wide_calls = wide_calls = vector_calls = vector256_calls = 0;
    if (walk(word_collect_in, cranges, 1, got_word) != 0) return 1;
    if (walk(collect_in, cranges, 1, got) != 0) return 1;
    const int vector_on = BUILT && want_vector;
    if (word_wide_calls != (want_wide ? 16 : 0) || wide_calls != (want_wide && !vector_on ? 16 : 0) ||
        vector256_calls != (vector_on ? 16 : 0) || vector_calls != 0) {
        printf("4096 clear marks: %d and %d word tests of 256, %d vector tests of 256, %d of 64\n", word_wide_calls, wide_calls,
               vector256_calls, vector_calls);
        return 1;
    }
    /* (from any page: 130 clear marks from page 5 in 3 vector tests of 64, the last one the 64 marks that end at page 135) */
    ranges[0][0] = 5ull << 12, ranges[0][1] = 135ull << 12;
    vector_calls = vector256_calls = 0;
    if (walk(collect_in, cranges, 1, got) != 0) return 1;
    if (vector_calls != (vector_on ? 3 : 0) || vector256_calls != 0) {
        printf("130 clear marks from page 5: %d vector tests of 64, %d of 256\n", vector_calls, vector256_calls);
        return 1;
    }
    /* (600 clear marks from page 5: two tests of 256 from pages 5 and 261, then 88 marks in two tests of 64) */
    ranges[0][0] = 5ull << 12, ranges[0][1] = 605ull << 12;
    vector_calls = vector256_calls = 0;
    if (walk(collect_in, cranges, 1, got) != 0) return 1;
    if (vector_calls != (vector_on ? 2 : 0) || vector256_calls != (vector_on ? 2 : 0)) {
        printf("600 clear marks from page 5: %d vector tests of 64, %d of 256\n", vector_calls, vector256_calls);
        return 1;
    }
    /* (one mark, page 300 of 1024: tests of 256 from pages 0 (clear) and 256 (the mark), a test of 64 from 256 that takes it,
       then tests of 256 from pages 301 and 557 (clear) and four of 64 for the last 211 marks) */
    ranges[0][0] = 0, ranges[0][1] = 1024ull << 12;
    memset(marks, 0, sizeof marks);
    marks[300] = 1;
    vector_calls = vector256_calls = 0;
    if (walk(collect_in, cranges, 1, got) != 1 || got[0] != 300) return 1;
    if (vector_calls != (vector_on ? 5 : 0) || vector256_calls != (vector_on ? 4 : 0)) {
        printf("one mark at page 300 of 1024: %d vector tests of 64, %d of 256\n", vector_calls, vector256_calls);
        return 1;
    }
    /* (and a dense run: 4096 set marks in 64 vector tests of 64, each window's marks taken from one read, after a test of
       256 that finds a mark for each of the 61 windows with 256 left) */
    ranges[0][0] = 0, ranges[0][1] = 4096ull << 12;
    memset(marks, 1, 4096);
    vector_calls = vector256_calls = 0;
    if (walk(collect_in, cranges, 1, got) != 4096) return 1;
    if (vector_calls != (vector_on ? 64 : 0) || vector256_calls != (vector_on ? 61 : 0)) {
        printf("4096 set marks: %d vector tests of 64, %d of 256\n", vector_calls, vector256_calls);
        return 1;
    }
    if (logged != BUILT) {
        printf("the walk was logged %d times\n", logged);
        return 1;
    }
#if BUILT
    /* (the line says which walk this run takes, and in which loads) */
    const char *said = want_vector ? "mem: the write marks' walk: 256 marks a test, else 64, in " MARKS_LOADS " (HOS_COLLECT_VECTOR=0"
                                   : "mem: the write marks' walk: word loads (HOS_COLLECT_VECTOR=0)";
    if (strncmp(log_line, said, strlen(said))) {
        printf("the walk's line: \"%s\", expected \"%s\"\n", log_line, said);
        return 1;
    }
#endif
    return 0;
}
"""

# (x86-64: does this CPU have AVX2, for the walk built with -mavx2? Built for the CPU's baseline)
AVX2_PROBE = r"""
int main(void) {
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") ? 0 : 1;
}
"""


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def fixture(src):
    # (the vector walk: its comment up to collect_in, after marks_clear_256; none in a source without it)
    at = src.find("/* (the vector walk, on x86-64 and arm64:")
    return (FIXTURE.replace("@MARKS_CLEAR_256@", block(src, "static inline int marks_clear_256("))
            .replace("@VECTOR@", src[at:src.index("static int64_t collect_in(", at)] if at >= 0 else "")
            .replace("@COLLECT_IN@", block(src, "static int64_t collect_in(")))


def builds(arches, darwin):
    """Each build of the fixture: (label, compiler flags, run prefix, the vector walk it must have: NEON, AVX2, SSE2 or
    None for none)"""
    out = []
    for arch in arches:
        flags, run = (["-arch", arch], ["arch", "-" + arch]) if darwin else ([], [])
        cpu = arch if darwin else platform.machine().lower()
        name = arch or cpu
        if cpu in ("arm64", "aarch64"):
            out.append((name, flags, run, "NEON"))
        elif cpu in ("x86_64", "amd64"):
            out.append((name + " SSE2", flags + ["-mno-avx2"], run, "SSE2"))
            out.append((name + " AVX2", flags + ["-mavx2"], run, "AVX2"))
        else:
            out.append((name, flags, run, None))
    return out


def has_avx2(cc, tmp, flags, run):
    c, exe = Path(tmp) / "avx2_probe.c", Path(tmp) / "avx2_probe"
    c.write_text(AVX2_PROBE, encoding="utf-8")
    # (a compiler without the CPU builtins fails the test: the AVX2 walk would go unchecked)
    subprocess.run([cc, *[f for f in flags if f != "-mavx2"], str(c), "-o", str(exe)], check=True)
    return subprocess.run(run + [str(exe)]).returncode == 0


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    p.add_argument("--arch", choices=("arm64", "x86_64"), action="append", help="macOS only: a slice to build and run")
    p.add_argument("--source", type=Path, default=ROOT / "host" / "rx_backend.c")
    a = p.parse_args()
    darwin = sys.platform == "darwin"
    if a.arch and not darwin:
        p.error("--arch is for macOS")
    arches = a.arch or ([platform.machine()] if darwin else [None])
    src = a.source.read_text(encoding="utf-8")
    done = []
    with tempfile.TemporaryDirectory(prefix="collect-wide-") as tmp:
        c = Path(tmp) / "fixture.c"
        c.write_text(fixture(src), encoding="utf-8")
        for label, flags, run, want in builds(arches, darwin):
            exe = Path(tmp) / ("fixture-" + label.replace(" ", "-"))
            subprocess.run([a.cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu" / "vk" / "compat"),
                            *flags, str(c), "-o", str(exe)], check=True)
            if want == "AVX2" and not has_avx2(a.cc, tmp, flags, run):
                print("%s: built; not run (this CPU has no AVX2)" % label)
                continue
            for wide, vector in ((w, v) for v in VECTOR_VALUES for w in (None, "0")):
                env = dict(os.environ)
                env.pop("HOS_COLLECT_WIDE", None)
                env.pop("HOS_COLLECT_VECTOR", None)
                if wide is not None:
                    env["HOS_COLLECT_WIDE"] = wide
                if vector is not None:
                    env["HOS_COLLECT_VECTOR"] = vector
                # (HOS_COLLECT_WIDE: main's own read, a leading 0; HOS_COLLECT_VECTOR: the switches' one rule)
                want_wide = wide is None
                want_vector = vector is None or vector.strip().lower() not in OFF
                out = subprocess.run(run + [str(exe), "1" if want_wide else "0", "1" if want_vector else "0"], env=env,
                                     stdout=subprocess.PIPE, text=True)
                sys.stdout.write(out.stdout)
                if out.returncode:
                    raise SystemExit("FAIL: %s, HOS_COLLECT_WIDE=%r HOS_COLLECT_VECTOR=%r" % (label, wide, vector))
                if want and ("vector walk: built" not in out.stdout or "(%s)" % want not in out.stdout):
                    raise SystemExit("FAIL: %s: the vector walk in %s loads is not the one built" % (label, want))
                if not want and "vector walk: not built" not in out.stdout:
                    raise SystemExit("FAIL: %s: a vector walk is built on a CPU without one" % label)
            done.append(label)
    print("ok: the same pages, order and marks left in the word walk and the walk built here (%s)" % ", ".join(done))


if __name__ == "__main__":
    main()
