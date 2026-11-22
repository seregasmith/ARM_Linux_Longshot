"""Fast float regions (A64_FASTFP, nx2/fastfp.py's ff_region): what may join a region, and the region's fast copy against
plain IEEE single precision, game-free.

Python, no compiler:
  - ff_eligible: loads, vector moves, the FPX helpers and the exact compare blocks' FPCR tests and FPSR |= may join; a
    page mark, a raw guest pointer (a memcpy store), an MRS or MSR of FPSR/FPCR, a call, a branch may not;
  - (longshot) nx2/fastvec.py's vector moves and shuffles (a64_vld16, a64_vdup, A64_EXT16, A64_EXT8, a64_tbl1) may join,
    unless A64_FASTFP_VMOVES=0;
  - (longshot) guest stores (A64_FASTFP_STORES): STR / STP, through a register, SP or x29, with writeback, may join; with
    the switch off none may; STLR, a store under a condition, a text whose load follows its store may not. ff_split cuts
    a run before a load that follows a store in it (not before one that comes before every store);
  - ff_region: a run below A64_FASTFP_MIN float ops is the exact text unchanged; above it the fast copy names the a64_ff_
    ops, saves and puts back every register the run names, and keeps the exact copy. With stores: the fast copy calls no
    store helper, its stores' address and values go to locals, stored after a64_ff_end passed, in their order, with the
    same helpers; a store through x29 is the unmarked _sp one in both copies once frame_stores and ff_frame_stores ran;
    the exact copy is the run's text unchanged; A64_FASTFP=check compares each store's address and values too.

C (skipped without a compiler or on a host that is not x86-64): a region ff_region makes from lifted-style text (vector
FMUL/FADD/FSUB in place, the 4- and 2-lane forms, scalar FADD/FSUB/FMUL/FDIV/FSQRT, an integer register written), with
FASTFP_HEADER and stand-in exact helpers that count their calls and note the registers they first see. Built at
-march=x86-64-v3 (the AVX forms) and -march=x86-64-v2 (SSE). Each build must:
  - random normal operands (200,000 runs): the fast copy only, and every output register equal to the same ops done in
    plain C in the host's default mode (a swapped operand order in a subtract or a divide fails);
  - two quiet NaN operands: the exact copy (any NaN result does: a64_ff_guard_);
  - 0 * inf (invalid), a denormal operand, a tiny product under FPCR.FZ (underflow, FTZ): the exact copy runs, and sees
    the region's input registers as they were (the fast copy wrote over them);
  - a tiny exact product without FZ, read again by the next op: the exact copy (a denormal operand flags it);
  - overflow and divide-by-zero: fast, the infinities, FPSR.OFC / FPSR.DZC set;
  - the gate closed (FPSR.IXC clear, or HOS_NO_FASTFP set): the exact copy;
  - (longshot) the run's three guest stores (a 32-bit store through x19, one through x19 with writeback, an STP of a
    vector register's halves through x29): on the fast copy, guest memory and the store order as the reference's, the two
    x19 stores marked and the x29 pair not (_sp); on every fall-back, each store made once (by the exact copy, nothing
    by the fast one), in order;
  - (longshot) the same region written with A64_FASTFP=check: the random runs clean, and no mismatch counted (the stores'
    addresses and values compared as well as the registers).
  - (longshot) a scalar FMSUB in the region (FMA3 at x86-64-v3, the exact helper at v2), and at v3 the cases where x86's
    FMA is not Arm's: 0 * inf + a quiet NaN, a product rounded up to 2^-126 (FZ off and on), a subnormal addend under
    FZ: the exact copy. (Dropping fma32_fma32 from FF_CONV fails the 2^-126 cases.) Each op alone against its exact
    body: tests/test_fastfp_fma.py.

It has teeth (each by hand, then restored, 2026-11-11):
  - the AVX ops' operands swapped (A64_FF_OP's "%2, %1, %0" made "%1, %2, %0"): the random runs differ at v3;
  - the invalid flag no longer sending the region to its exact copy (a64_ff_end's 0x13 made 0x12): 0 * inf fails.
  (longshot, 2026-11-20) the deferred stores made before a64_ff_end (the fast copy storing in place): the fall-backs
  store twice; the deferred stores in reverse order: the store order differs; ff_split's cut dropped: the load after a
  store joins the region (the Python check).

    python -I -B tests/test_fastfp_regions.py [--cc clang]
"""
import argparse
import os
import platform
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KIT = os.path.join(ROOT, "third_party", "a64-kit")
sys.path.insert(0, os.path.join(KIT, "lift"))
sys.path.insert(0, os.path.join(KIT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
os.environ["A64_FASTFP"] = "1"
os.environ.pop("A64_FASTFP_MIN", None)
os.environ.pop("A64_FASTFP_STORES", None)
os.environ.pop("A64_FASTFP_VMOVES", None)
import fastfp as L  # noqa: E402  (the rules and the header live in nx2, outside the kit)
import toolchain  # noqa: E402


def fail(msg):
    raise SystemExit("fastfp regions: " + msg)


def check_python():
    ok = ["c->x[8]=recomp_load64(c,c->x[3] + 0);",
          "{ recomp_ldp64(c,(c->x[2]+0ULL),&c->vreg[1][0],&c->vreg[1][1]); }",
          "a64_vfpx_mul32(c,c->vreg[3],c->vreg[1],c->vreg[2],4);",
          "{ if (!a64_fcmp32(c,c->vreg[0],a64_vzero)) { if(c->fpcr & (1ULL<<24)) { c->fpsr|=128; } } }",
          "{ uint8_t _s[16]; memcpy(_s,c->vreg[3],16); a64_vst16(c->vreg[4],_s); }"]
    bad = ["memcpy(A64_PTR(c->x[0]), c->vreg[1], 16);",
           "c->x[3] = c->fpsr;",          # MRS FPSR
           "c->fpcr = c->x[3];",          # MSR FPCR
           "A64_ICALL(c, c->x[8]);",
           "goto L_80001000;"]
    for t in ok:
        if not L.ff_eligible(t):
            fail("should join a region: %s" % t)
    for t in bad:
        if L.ff_eligible(t):
            fail("should end a region: %s" % t)
    stores = ["recomp_store64(c,c->x[1] + 8,c->x[2]);",
              "{ recomp_stp64(c,(c->x[12]+c->x[13]),c->vreg[20][0],c->vreg[20][1]); }",
              "{ uint64_t _v=0; memcpy(&_v,&c->vreg[0][0],4); recomp_store32(c,(c->x[19]+4ULL),_v); }",
              "{ uint64_t _b=c->x[29]; int64_t _o=-16; recomp_stp64(c,(_b+_o),c->vreg[8][0],c->vreg[8][1]); }",
              "{ uint64_t _b=c->x[0]; int64_t _o=8; recomp_store32(c,(_b+_o),c->x[3]); c->x[0]=_b+_o; }",
              "recomp_store32_sp(c,c->x[31] + 104,c->x[25]);"]
    no_stores = ["recomp_store32(c,c->x[1],c->x[2]);",                                # STLR (suyu's spelling)
                 "if (c->x[1]) { recomp_store32(c,c->x[1] + 0,c->x[2]); }",           # under a condition
                 "recomp_store32(c,c->x[1] + 0,c->x[2]); c->x[3]=recomp_load32(c,c->x[4] + 0);",   # its own load after it
                 "{ uint64_t _a=c->x[0]; uint32_t _o = __atomic_exchange_n((uint32_t *)A64_PTR(_a), (uint32_t)c->x[1], "
                 "__ATOMIC_SEQ_CST); A64_MARK(_a, 4); c->x[2] = (uint64_t)_o; }"]   # atomic
    for t in stores:
        if not L.ff_eligible(t, True):
            fail("a store should join a region (A64_FASTFP_STORES): %s" % t)
        if L.ff_eligible(t, False):
            fail("a store should end a region with A64_FASTFP_STORES=0: %s" % t)
    for t in no_stores:
        if L.ff_eligible(t, True):
            fail("should end a region even with stores on: %s" % t)
    # (nx2/fastvec.py's vector moves and shuffles join, unless A64_FASTFP_VMOVES=0)
    vmoves = ["{ a64_vld16(c->vreg[0],(c->x[9]+1184ULL)); }", "a64_vdup(c->vreg[7],c->vreg[7],8,4,1);",
              "A64_EXT16(c->vreg[1],c->vreg[2],c->vreg[3],4);", "A64_EXT8(c->vreg[1],c->vreg[2],c->vreg[3],4);",
              "a64_tbl1(c->vreg[1],c->vreg[2],c->vreg[3],1);"]
    for t in vmoves:
        if not L.ff_eligible(t, False):
            fail("a vector move should join a region: %s" % t)
    os.environ["A64_FASTFP_VMOVES"] = "0"
    try:
        for t in vmoves:
            if L.ff_eligible(t, False):
                fail("a vector move should end a region with A64_FASTFP_VMOVES=0: %s" % t)
    finally:
        os.environ.pop("A64_FASTFP_VMOVES")
    # (a load after a store cuts the run there, a64_vld16's too; one before every store does not)
    ld = "c->x[8]=recomp_load64(c,c->x[3] + 0);"
    mul = "a64_vfpx_mul32(c,c->vreg[1],c->vreg[1],c->vreg[2],4);"
    st = stores[2]
    run_ = [(4 * i, t) for i, t in enumerate([ld, mul, st, mul, ld, mul, st, st, mul, vmoves[0], mul])]
    parts = [[t for _, t in p] for p in L.ff_split(run_)]
    if parts != [[ld, mul, st, mul], [ld, mul, st, st, mul], [vmoves[0], mul]]:
        fail("ff_split cut the run as %s" % parts)
    few = [(0x1000 + 4 * i, "a64_vfpx_add32(c,c->vreg[1],c->vreg[1],c->vreg[2],4);") for i in range(L.ff_min() - 1)]
    text, n = L.ff_region(few)
    if n or text != "\n".join(t for _, t in few):
        fail("a run below the minimum must stay the exact text")
    text, n = L.ff_region(few + [(0x2000, "c->x[5] = c->x[5] + 1; a64_vfpx_mul32(c,c->vreg[7],c->vreg[1],c->vreg[2],4);")])
    if n != L.ff_min():
        fail("ops made fast: %d, not %d" % (n, L.ff_min()))
    for need in ("a64_ff_vadd32(", "a64_ff_vmul32(", "_ffx5 = c->x[5]", "c->x[5] = _ffx5", "_ffv70 = c->vreg[7][0]",
                 "c->vreg[1][1] = _ffv11", "c->fpsr = _ffsr", "a64_vfpx_add32(", "L_ff_00001000:;"):
        if need not in text:
            fail("the region text has no %r" % need)
    check_python_stores()
    print("fastfp regions: python checks passed")


def check_python_stores():
    """(longshot) a region with guest stores: the deferred stores' shape, their order, the exact copy unchanged"""
    mul = "a64_vfpx_mul32(c,c->vreg[%d],c->vreg[1],c->vreg[2],4);"
    s1 = "{ uint64_t _v=0; memcpy(&_v,&c->vreg[3][0],4); recomp_store32(c,(c->x[19]+4ULL),_v); }"
    s2 = "{ uint64_t _b=c->x[29]; int64_t _o=-16; recomp_stp64(c,(_b+_o),c->vreg[4][0],c->vreg[4][1]); }"
    s3 = "recomp_store64(c,c->x[19] + 8,c->x[2]);"
    texts = [mul % 3] * 4 + [s1] + [mul % 4] * 4 + [s2, s3]
    items = [(0x2000 + 4 * i, t) for i, t in enumerate(texts)]
    exact = "\n".join(texts)
    text, k, n = L.ff_regions(items, 8, True)
    if (k, n) != (1, 8):
        fail("a run of 8 multiplies and 3 stores: %d regions of %d ops, not one of 8" % (k, n))
    head, _, rest = text.partition("\nif (RECOMP_LIKELY(a64_ff_end(c, _ffm))) { ")
    fast = head.split("\n", 1)[1]
    if not rest or "recomp_st" in fast:
        fail("the fast copy stores, or its stores do not wait for a64_ff_end:\n" + text)
    for need in ("(_ffa0=((c->x[19]+4ULL)),_ffd0=(_v))", "(_ffpa1=((_b+_o)),_ffpd1=(c->vreg[4][0]),_ffpe1=(c->vreg[4][1]))",
                 "(_ffa2=(c->x[19] + 8),_ffd2=(c->x[2]))", "uint64_t _ffa0, _ffd0; uint64_t _ffpa1, _ffpd1, _ffpe1; "):
        if need not in text:
            fail("the region text has no %r:\n%s" % (need, text))
    deferred = rest.split("\n", 1)[0]
    want = "recomp_store32(c,_ffa0,_ffd0); recomp_stp64(c,_ffpa1,_ffpd1,_ffpe1); recomp_store64(c,_ffa2,_ffd2); goto L_ff_00002000; }"
    if deferred != want:
        fail("the deferred stores are\n  %s\nnot\n  %s" % (deferred, want))
    if ("\n%s\nL_ff_00002000:; }" % exact) not in text:
        fail("the exact copy is not the run's text unchanged")
    framed = L.ff_frame_stores(L.frame_stores(text))
    if "recomp_stp64_sp(c,_ffpa1,_ffpd1,_ffpe1)" not in framed or "recomp_stp64_sp(c,(_b+_o)" not in framed or \
            "recomp_store32_sp" in framed or "recomp_store64_sp" in framed:
        fail("the x29 pair is not the unmarked _sp store in both copies (and only it):\n" + framed)
    os.environ["A64_FASTFP_STORES"] = "0"
    try:
        if L.ff_regions(items, 8) != (L.ff_region(items, 8)[0], 1, 8):
            fail("A64_FASTFP_STORES=0: the run is not written as ff_region writes it")
    finally:
        os.environ.pop("A64_FASTFP_STORES")
    os.environ["A64_FASTFP"] = "check"
    try:
        text, _ = L.ff_region(items, 8)
    finally:
        os.environ["A64_FASTFP"] = "1"
    for need in ("_ffa0 == _ffoa0 && (uint32_t)_ffd0 == (uint32_t)_ffod0",
                 "_ffpa1 == _ffpoa1 && (uint64_t)_ffpd1 == (uint64_t)_ffpod1 && (uint64_t)_ffpe1 == (uint64_t)_ffpoe1",
                 "(_ffoa0=((c->x[19]+4ULL)),_ffod0=(_v),recomp_store32(c,_ffoa0,_ffod0))"):
        if need not in text:
            fail("A64_FASTFP=check: the region text has no %r:\n%s" % (need, text))
    print("fastfp regions: python store checks passed")


# lifted-style instruction texts for the C region (ops in place, so the exact copy must see the inputs put back)
OPS = ["a64_vfpx_mul32(c,c->vreg[0],c->vreg[0],c->vreg[1],4);",                       # v0 = v0 * v1
       "a64_vfpx_add32(c,c->vreg[2],c->vreg[0],c->vreg[3],4);",                       # v2 = v0 + v3
       # (longshot) STR s2, [x19, #4]
       "{ uint64_t _v=0; memcpy(&_v,&c->vreg[2][0],4); recomp_store32(c,(c->x[19]+4ULL),_v); }",
       "a64_vfpx_sub32(c,c->vreg[4],c->vreg[3],c->vreg[1],2);",                       # v4 = v3 - v1 (2 lanes)
       # (longshot) STR w-of-v4, [x19, #8]! (writeback: x19 moves, the next stores use the old one)
       "{ uint64_t _b=c->x[19]; int64_t _o=8; recomp_store32(c,(_b+_o),c->vreg[4][0]); c->x[19]=_b+_o; }",
       "a64_vfpx_add32(c,c->vreg[5],c->vreg[1],c->vreg[3],2);",                       # v5 = v1 + v3 (2 lanes)
       "{ uint32_t _n[4],_m[4],_r[4]={0};memcpy(_n,c->vreg[1],16);memcpy(_m,c->vreg[3],16);"
       "{uint64_t _a=_n[0],_b=_m[0],_v=0;_v=a64_fpx_add32_add32(c,_a,_b);_r[0]=(uint32_t)_v;}"
       "{uint64_t _a=_n[1],_b=_m[1],_v=0;_v=a64_fpx_sub32_sub32(c,_a,_b);_r[1]=(uint32_t)_v;}"
       "{uint64_t _a=_n[2],_b=_m[2],_z=(_a^_b)&0x80000000ULL,_v=0;_v=a64_fpx_mul32_fma32(c,_a,_b,_z);_r[2]=(uint32_t)_v;}"
       "{uint64_t _a=_n[3],_b=_m[3],_v=0;_v=a64_fpx_div32_div32(c,_a,_b);_r[3]=(uint32_t)_v;}"
       "memcpy(c->vreg[6],_r,16);}",                                                    # v6 = scalar lanes
       # (longshot) STP d-halves of v6, [x29, #16]: a frame store (unmarked once frame_stores ran)
       "{ uint64_t _b=c->x[29]; int64_t _o=16; recomp_stp64(c,(_b+_o),c->vreg[6][0],c->vreg[6][1]); }",
       "{ uint32_t _n[4],_r[4]={0};memcpy(_n,c->vreg[3],16);"
       "{uint64_t _a=_n[0],_v=0;_v=a64_fpx_sqrt32_sqrt32(c,_a);_r[0]=(uint32_t)_v;} memcpy(c->vreg[7],_r,16);}",
       # (longshot) FMSUB s8, s9, s10, s11 as the lifter emits it: v8 = v11 - v9 * v10 (a64_ff_fma32_fma32)
       "{ uint64_t _a=0,_b=0,_z=0,_v; memcpy(&_a,c->vreg[9],4);memcpy(&_b,c->vreg[10],4);memcpy(&_z,c->vreg[11],4);"
       "_a^=0x80000000ULL;_v=a64_fpx_fma32_fma32(c,_a,_b,_z);c->vreg[8][0]=_v;c->vreg[8][1]=0;}",
       "c->x[5] = c->x[5] + 1;"]

DRIVER = r"""
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <immintrin.h>
typedef struct { uint64_t x[32]; uint8_t n, z, c, v; uint64_t vreg[32][2]; uint64_t fpcr, fpsr; } Ctx;
#define A64_NOESC
#define RECOMP_FPX_HOST 2
#define RECOMP_LIKELY(x) __builtin_expect(!!(x), 1)
#define RECOMP_FPX_FZ(c) ((uint32_t)(c)->fpcr & 0x01000000u)
#define RECOMP_FPX_OPEN(c) (((c)->fpsr & 16u) && !((c)->fpcr & 0x06C8FF07u))
/* stand-in exact helpers: plain C in the host's default mode, counted, the first registers they see noted */
static int exact_calls; static uint64_t seen_v0[2], seen_x5; static Ctx *cur;
static void note(void) { if (!exact_calls++) { memcpy(seen_v0, cur->vreg[0], 16); seen_x5 = cur->x[5]; } }
static float F(uint64_t b) { uint32_t u = (uint32_t)b; float f; memcpy(&f, &u, 4); return f; }
static uint64_t B(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
#define VOP(name, op) static void a64_vfpx_##name(Ctx *c, uint64_t *d, const uint64_t *n, const uint64_t *m, int k) { \
    float a[4], b[4], r[4] = {0}; (void)c; note(); memcpy(a, n, 16); memcpy(b, m, 16); \
    for (int j = 0; j < k; j++) r[j] = a[j] op b[j]; memcpy(d, r, 16); }
VOP(add32, +) VOP(sub32, -) VOP(mul32, *)
static uint64_t a64_fpx_add32_add32(Ctx *c, uint64_t a, uint64_t b) { (void)c; note(); return B(F(a) + F(b)); }
static uint64_t a64_fpx_sub32_sub32(Ctx *c, uint64_t a, uint64_t b) { (void)c; note(); return B(F(a) - F(b)); }
static uint64_t a64_fpx_mul32_fma32(Ctx *c, uint64_t a, uint64_t b, uint64_t z) { (void)c; (void)z; note(); return B(F(a) * F(b)); }
static uint64_t a64_fpx_div32_div32(Ctx *c, uint64_t a, uint64_t b) { (void)c; note(); return B(F(a) / F(b)); }
static uint64_t a64_fpx_sqrt32_sqrt32(Ctx *c, uint64_t a) { (void)c; note(); return B(sqrtf(F(a))); }
/* (not counted: at x86-64-v2 the fast copy calls it too, as a64_ff_fma32_fma32; the region's other ops count the
   exact copy) */
static uint64_t a64_fpx_fma32_fma32(Ctx *c, uint64_t a, uint64_t b, uint64_t z) { (void)c; return B(fmaf(F(a), F(b), F(z))); }
/* (longshot) stand-in guest stores: guest memory a small array, each store logged in order, the marked ones counted */
static uint8_t mem[256]; static int nst, nmark; static uint64_t slog[16];
static void st_(uint64_t a, const void *p, int n, int mark) { memcpy(mem + (a & 0x7f), p, n); if (nst < 16) slog[nst] = a; nst++; nmark += mark; }
static void recomp_store32(Ctx *c, uint64_t a, uint64_t v) { uint32_t t = (uint32_t)v; (void)c; st_(a, &t, 4, 1); }
static void recomp_stp64(Ctx *c, uint64_t a, uint64_t v0, uint64_t v1) { uint64_t t[2] = {v0, v1}; (void)c; st_(a, t, 16, 1); }
static void recomp_stp64_sp(Ctx *c, uint64_t a, uint64_t v0, uint64_t v1) { uint64_t t[2] = {v0, v1}; (void)c; st_(a, t, 16, 0); }
static void mem_reset(void) { memset(mem, 0, sizeof mem); nst = nmark = 0; memset(slog, 0, sizeof slog); }
%(header)s
static void region(Ctx *__restrict c) {
%(region)s
}
/* (longshot) the same run written with A64_FASTFP=check */
static void region_check(Ctx *__restrict c) {
%(check)s
}
/* the region's ops in plain C (the reference for normal operands) */
static void reference(Ctx *c) {
    float v0[4], v1[4], v3[4], r[4];
    memcpy(v0, c->vreg[0], 16); memcpy(v1, c->vreg[1], 16); memcpy(v3, c->vreg[3], 16);
    for (int j = 0; j < 4; j++) v0[j] = v0[j] * v1[j];
    memcpy(c->vreg[0], v0, 16);
    for (int j = 0; j < 4; j++) r[j] = v0[j] + v3[j];
    memcpy(c->vreg[2], r, 16);
    recomp_store32(c, c->x[19] + 4, c->vreg[2][0]);
    memset(r, 0, 16); for (int j = 0; j < 2; j++) r[j] = v3[j] - v1[j]; memcpy(c->vreg[4], r, 16);
    recomp_store32(c, c->x[19] + 8, c->vreg[4][0]); c->x[19] += 8;
    memset(r, 0, 16); for (int j = 0; j < 2; j++) r[j] = v1[j] + v3[j]; memcpy(c->vreg[5], r, 16);
    r[0] = v1[0] + v3[0]; r[1] = v1[1] - v3[1]; r[2] = v1[2] * v3[2]; r[3] = v1[3] / v3[3]; memcpy(c->vreg[6], r, 16);
    recomp_stp64_sp(c, c->x[29] + 16, c->vreg[6][0], c->vreg[6][1]);
    memset(r, 0, 16); r[0] = sqrtf(v3[0]); memcpy(c->vreg[7], r, 16);
    { float a, b, z; memcpy(&a, c->vreg[9], 4); memcpy(&b, c->vreg[10], 4); memcpy(&z, c->vreg[11], 4);
      c->vreg[8][0] = B(fmaf(-a, b, z)); c->vreg[8][1] = 0; }
    c->x[5] += 1;
}
static uint64_t rng = 20261111;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }
static uint32_t normal(void) {   /* |x| in [2^-20, 2^20), either sign */
    return (rnd() & 0x80000000u) | ((uint32_t)(107 + rnd() %% 40) << 23) | (rnd() & 0x7fffffu); }
static void set(Ctx *c, int v, uint32_t a, uint32_t b, uint32_t d, uint32_t e) {
    uint32_t l[4] = {a, b, d, e}; memcpy(c->vreg[v], l, 16); }
static void fresh(Ctx *c, uint64_t fpcr) {
    memset(c, 0, sizeof *c); c->fpcr = fpcr; c->fpsr = 0x10; c->x[5] = 41; c->x[19] = 0x40; c->x[29] = 0x50;
    for (int v = 0; v < 12; v++) set(c, v, normal(), normal(), normal(), 0x3f800000u + (rnd() & 0xfffu));
    c->vreg[3][0] &= ~0x80000000ULL; }   /* (v3's lane 0 is square-rooted: not negative) */
static int run(Ctx *c) { exact_calls = 0; cur = c; mem_reset(); region(c); return exact_calls; }
static int bad;
#define EXPECT(cond, ...) do { if (!(cond)) { printf(__VA_ARGS__); printf("\n"); bad++; } } while (0)
/* (longshot) the run's stores, each once and in order: x19 + 4, x19 + 8 (marked), x29 + 16 (the frame pair, unmarked) */
static int stores_once(void) { return nst == 3 && slog[0] == 0x44 && slog[1] == 0x48 && slog[2] == 0x60 && nmark == 2; }
static void must_fall_back(const char *what, Ctx *c) {
    uint64_t v0[2]; memcpy(v0, c->vreg[0], 16); const uint64_t x5 = c->x[5];
    EXPECT(run(c) > 0, "%%s: the fast copy was kept", what);
    EXPECT(!memcmp(seen_v0, v0, 16) && seen_x5 == x5, "%%s: the exact copy did not see the inputs put back", what);
    EXPECT(stores_once(), "%%s: %%d stores (%%d marked), not the run's 3 once each in order", what, nst, nmark);
}
int main(void) {
    Ctx c, r;
    uint8_t want[sizeof mem];
    for (int i = 0; i < 200000; i++) {
        fresh(&c, i & 1 ? 0x01000000u : 0); r = c; mem_reset(); reference(&r); memcpy(want, mem, sizeof mem);
        EXPECT(run(&c) == 0, "random %%d: the exact copy ran", i);
        EXPECT(!memcmp(c.vreg, r.vreg, sizeof c.vreg) && c.x[5] == r.x[5] && c.x[19] == r.x[19], "random %%d: the fast copy differs", i);
        EXPECT(!memcmp(mem, want, sizeof mem) && stores_once(), "random %%d: the fast copy's stores differ (%%d stores, %%d marked)", i, nst, nmark);
        if (bad > 10) return 1;
    }
    /* (longshot) A64_FASTFP=check: the same runs clean, no mismatch counted, the exact copy's stores kept */
    for (int i = 0; i < 20000; i++) {
        fresh(&c, i & 1 ? 0x01000000u : 0); r = c; mem_reset(); reference(&r); memcpy(want, mem, sizeof mem);
        cur = &c; mem_reset(); region_check(&c);
        EXPECT(!memcmp(c.vreg, r.vreg, sizeof c.vreg) && c.x[19] == r.x[19] && !memcmp(mem, want, sizeof mem) && stores_once(),
               "check %%d: the exact copy's result or stores differ", i);
        if (bad > 10) return 1;
    }
    EXPECT(a64_ff_n_[0] == 20000 && a64_ff_n_[1] == 20000 && a64_ff_n_[2] == 0,
           "check: %%lld runs, %%lld clean, %%lld mismatched", a64_ff_n_[0], a64_ff_n_[1], a64_ff_n_[2]);
    fresh(&c, 0); set(&c, 0, 0x7fc00001u, 0x7fc00002u, 0, 0); set(&c, 1, 0x7fc00003u, 0x7fc00004u, 0, 0);
    must_fall_back("two NaNs (a NaN result: the exact copy flags it as Arm does)", &c);
    fresh(&c, 0); set(&c, 0, 0, 0x3f800000u, 0x3f800000u, 0x3f800000u); set(&c, 1, 0x7f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u);
    must_fall_back("0 * inf", &c);
    fresh(&c, 0); set(&c, 1, 0x00000123u, 0x3f800000u, 0x3f800000u, 0x3f800000u);
    must_fall_back("a denormal operand", &c);
    fresh(&c, 0x01000000u); set(&c, 0, 0x1c800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u); set(&c, 1, 0x1c800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u);
    must_fall_back("a tiny product under FZ", &c);   /* (2^-70 * 2^-70) */
    fresh(&c, 0); set(&c, 0, 0x1c800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u); set(&c, 1, 0x1c800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u);
    must_fall_back("a tiny exact product read again (a denormal operand)", &c);   /* (2^-140, then v0 + v3) */
#ifdef __FMA__
    /* (longshot) FMSUB v8 = v11 - v9 * v10: the cases where x86's FMA is not Arm's */
    fresh(&c, 0); set(&c, 9, 0, 0, 0, 0); set(&c, 10, 0x7f800000u, 0, 0, 0); set(&c, 11, 0x7fc00001u, 0, 0, 0);
    must_fall_back("0 * inf + a quiet NaN (x86: the NaN, no flag; Arm: the default NaN, IOC)", &c);
    for (int fz = 0; fz < 2; fz++) {
        fresh(&c, fz ? 0x01000000u : 0); set(&c, 9, 0xbf000001u, 0, 0, 0); set(&c, 10, 0x00fffffeu, 0, 0, 0); set(&c, 11, 0, 0, 0, 0);
        must_fall_back(fz ? "a product rounded up to 2^-126, FZ" : "a product rounded up to 2^-126", &c);
    }
    fresh(&c, 0x01000000u); set(&c, 9, 0xbf800000u, 0, 0, 0); set(&c, 10, 0x3f800000u, 0, 0, 0); set(&c, 11, 0x00000123u, 0, 0, 0);
    must_fall_back("a subnormal addend under FZ", &c);
#endif
    fresh(&c, 0); set(&c, 0, 0x71800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u); set(&c, 1, 0x71800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u);
    EXPECT(run(&c) == 0, "overflow: the exact copy ran");
    { uint32_t l[4]; memcpy(l, c.vreg[0], 16); EXPECT(l[0] == 0x7f800000u && (c.fpsr & 4), "overflow: %%08x, FPSR %%llx", l[0], (unsigned long long)c.fpsr); }
    fresh(&c, 0); set(&c, 3, 0x3f800000u, 0x3f800000u, 0x3f800000u, 0); set(&c, 1, 0x3f800000u, 0x3f800000u, 0x3f800000u, 0x40000000u);
    EXPECT(run(&c) == 0, "divide by zero: the exact copy ran");
    { uint32_t l[4]; memcpy(l, c.vreg[6], 16); EXPECT(l[3] == 0x7f800000u && (c.fpsr & 2), "divide by zero: %%08x, FPSR %%llx", l[3], (unsigned long long)c.fpsr); }
    fresh(&c, 0); c.fpsr = 0;
    EXPECT(run(&c) > 0, "the gate closed (IXC clear): the fast copy ran");
#ifdef _WIN32
    _putenv("HOS_NO_FASTFP=1");
#else
    setenv("HOS_NO_FASTFP", "1", 1);
#endif
    fresh(&c, 0); a64_ff_enabled_cache_reset();
    EXPECT(run(&c) > 0, "HOS_NO_FASTFP: the fast copy ran");
    printf("%%s\n", bad ? "FAILED" : "ok");
    return bad != 0;
}
"""


def compiler(given):
    for c in (given, os.environ.get("CC")):
        if c:
            return shutil.which(c) or (c if os.path.isfile(c) else None)
    if toolchain.mingw(host=True):
        c = toolchain.find("clang", host=True)
        if os.path.isfile(c):
            return c
    for n in ("clang", "gcc", "cc"):
        if shutil.which(n):
            return shutil.which(n)
    return None


def check_c(cc):
    items = [(0x1000 + 4 * i, t) for i, t in enumerate(OPS)]
    os.environ["A64_FASTFP_MIN"] = "8"
    text, n = L.ff_region(items)
    if not n:
        fail("the C region was not made fast")
    os.environ["A64_FASTFP"] = "check"
    try:
        check, _ = L.ff_region(items)
    finally:
        os.environ["A64_FASTFP"] = "1"
    # (as emit_function leaves them in a function whose x29 is its frame pointer)
    text, check = (L.ff_frame_stores(L.frame_stores(t)) for t in (text, check))
    # (the test resets the enable cache between cases: a hook the real header doesn't need)
    header = L.FASTFP_HEADER.replace(
        "static inline __attribute__((always_inline)) int a64_ff_enabled_(void) { static int v = -1; if (v < 0) v = !getenv(\"HOS_NO_FASTFP\"); return v; }",
        "static int a64_ff_v_ = -1;\nstatic void a64_ff_enabled_cache_reset(void) { a64_ff_v_ = -1; }\n"
        "static int a64_ff_enabled_(void) { if (a64_ff_v_ < 0) a64_ff_v_ = !getenv(\"HOS_NO_FASTFP\"); return a64_ff_v_; }")
    if "a64_ff_enabled_cache_reset" not in header:
        fail("FASTFP_HEADER's enable test changed: update the test's hook")
    src = DRIVER % {"header": header, "region": text, "check": check}
    tmp = tempfile.mkdtemp(prefix="fastfp_")
    try:
        path = os.path.join(tmp, "fastfp_test.c")
        with open(path, "w", newline="\n") as f:
            f.write(src)
        flags = toolchain.native_target(host=True) + toolchain.native_link(host=True) if (
            os.name == "nt" and "clang" in os.path.basename(cc).lower() and toolchain.mingw(host=True)) else []
        # (Windows keeps the math functions in the C runtime: an MSVC-target clang has no m.lib)
        libm = [] if os.name == "nt" else ["-lm"]
        for march in ("x86-64-v3", "x86-64-v2"):
            exe = os.path.join(tmp, "fastfp_%s.exe" % march)
            r = subprocess.run([cc] + flags + ["-march=" + march, "-ffp-contract=off", "-fno-strict-aliasing", "-O2",
                                               path, "-o", exe] + libm,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
            if r.returncode:
                print(r.stdout[-3000:])
                fail("building the C test at %s failed" % march)
            r = subprocess.run([exe], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                               timeout=300)
            print("fastfp regions: %s: %s" % (march, r.stdout.strip().splitlines()[-1] if r.stdout.strip() else "?"))
            if r.returncode:
                print(r.stdout[-3000:])
                fail("the C checks failed at %s" % march)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc")
    a = ap.parse_args()
    check_python()
    if platform.machine().lower() not in ("amd64", "x86_64"):
        print("fastfp regions: C checks skipped (not x86-64)")
        return
    cc = compiler(a.cc)
    if not cc:
        print("fastfp regions: C checks skipped (no C compiler)")
        return
    check_c(cc)
    print("fastfp regions: ok")


if __name__ == "__main__":
    main()
