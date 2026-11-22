/* tests/test_cmpflags.py's driver: each guest word as the lift translates it now (nx2/cmpflags.py: a subtraction's V as N xor
   (a < b, signed)) against the same word as before, from one Ctx, the whole Ctx compared after (every register, N, Z, C, V).

   cmpflags_test sweep               every pair of the edge values below in Xn and Xm (the words read x1 and x2), under each
                                     of the 16 NZCV values (CCMP and CCMN read them), the other registers random
   cmpflags_test random <n> <seed>   n random states per word (registers: random, edges, near each other)
   Exit 0: no mismatch, and V came out both 0 and 1 for every subtraction. 1: a mismatch. */
#include "a64.h"
#include <stdio.h>
#include <stdlib.h>
#include "cmpflags_lifted.h"

static uint64_t rs;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

static const uint64_t EDGE[] = {
    0, 1, 2, 3, 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFEull,
    0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0x8000000000000001ull, 0x7FFFFFFFFFFFFFFEull,
    0x000000007FFFFFFFull, 0x0000000080000000ull, 0x0000000080000001ull, 0x000000007FFFFFFEull, 0x00000000FFFFFFFFull,
    0x00000000FFFFFFFEull, 0x0000000100000000ull, 0xFFFFFFFF80000000ull, 0xFFFFFFFF7FFFFFFFull, 0xFFFFFFFF00000000ull,
    5, 4, 6, 7, 4095, 4096, 4094, 0x1000, 0xFFF, 0x1001,
    0x00000000000000FFull, 0x0000000000000100ull, 0xFFFFFFFFFFFFFF80ull, 0x0000000000000080ull, 0x0000000000007FFFull,
    0x0000000000008000ull, 0x3FFFFFFFFFFFFFFFull, 0xC000000000000000ull, 0x40000000ull, 0xC0000000ull,
};
enum { NEDGE = sizeof EDGE / sizeof EDGE[0] };

static uint64_t checks, mismatches, v0[64], v1[64];

static void check(int k, const Ctx *start) {
    static Ctx a, b;
    memcpy(&a, start, sizeof a);
    memcpy(&b, start, sizeof b);
    CASES[k].old(&a);
    CASES[k].now(&b);
    checks++;
    if (a.v) v1[k]++; else v0[k]++;
    if (memcmp(&a, &b, sizeof a) != 0 && mismatches++ < 12)
        printf("MISMATCH %s: x1 %#llx x2 %#llx nzcv %d%d%d%d -> before nzcv %d%d%d%d x3 %#llx, now nzcv %d%d%d%d x3 %#llx\n",
               CASES[k].name, (unsigned long long)start->x[1], (unsigned long long)start->x[2], start->n, start->z, start->c,
               start->v, a.n, a.z, a.c, a.v, (unsigned long long)a.x[3], b.n, b.z, b.c, b.v, (unsigned long long)b.x[3]);
}

static void random_regs(Ctx *c) {
    for (int r = 0; r < 32; r++) c->x[r] = rnd();
}
static uint64_t operand(void) {
    switch (rnd() % 4) {
    case 0: return EDGE[rnd() % NEDGE];
    case 1: return EDGE[rnd() % NEDGE] + (rnd() % 5) - 2;
    case 2: return (uint64_t)(int64_t)(int32_t)rnd();
    default: return rnd();
    }
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    static Ctx start;
    const int ncases = (int)(sizeof CASES / sizeof CASES[0]);
    if (!strcmp(argv[1], "sweep")) {
        rs = 11;
        for (int k = 0; k < ncases; k++)
            for (int i = 0; i < NEDGE; i++)
                for (int j = 0; j < NEDGE; j++)
                    for (int f = 0; f < 16; f++) {
                        memset(&start, 0, sizeof start);
                        random_regs(&start);
                        start.x[1] = EDGE[i], start.x[2] = EDGE[j];
                        start.n = f >> 3 & 1, start.z = f >> 2 & 1, start.c = f >> 1 & 1, start.v = f & 1;
                        check(k, &start);
                    }
    } else if (!strcmp(argv[1], "random")) {
        const uint64_t n = argc > 2 ? strtoull(argv[2], 0, 10) : 1000000;
        rs = argc > 3 ? strtoull(argv[3], 0, 10) : 1;
        for (int k = 0; k < ncases; k++)
            for (uint64_t i = 0; i < n; i++) {
                memset(&start, 0, sizeof start);
                random_regs(&start);
                start.x[1] = operand(), start.x[2] = operand();
                if (rnd() % 4 == 0) start.x[2] = start.x[1] + (rnd() % 3) - 1;   /* (equal or one apart) */
                const unsigned f = (unsigned)rnd();
                start.n = f & 1, start.z = f >> 1 & 1, start.c = f >> 2 & 1, start.v = f >> 3 & 1;
                check(k, &start);
            }
    } else {
        return 2;
    }
    printf("cmpflags %s: %llu checks over %d guest words, %llu mismatches\n", argv[1], (unsigned long long)checks, ncases,
           (unsigned long long)mismatches);
    if (mismatches) return 1;
    for (int k = 0; k < ncases; k++)
        if (CASES[k].sub && (!v0[k] || !v1[k])) {
            printf("%s: V came out only %d: the cases do not reach both\n", CASES[k].name, v1[k] ? 1 : 0);
            return 4;
        }
    return 0;
}
