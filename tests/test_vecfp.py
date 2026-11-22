"""(longshot) nx2/vecfp.py's SSE fast paths (A64_VECFP) against the instructions' own exact C, game-free.

Python, no compiler:
  - fast_call knows each form below (FMUL/FMLA/FMLS by element and vector, FMUL 2S, FDIV, FMAX/FMIN(NM) vector and scalar
    S and D, FCVT S<->D) and nothing else (FMUL 4S is the kit's vectorize_fpx, FADD is not touched); A64_VECFP=0 and a
    group list turn them off;
  - the wrapped C keeps the instruction's own C behind the helper, joins a fast float region as before (ff_eligible), and a
    region's fast copy names the region twins (a64_ff_<form>, nx2/fastfp.py's FF_VX) with an unchanged count of float ops.

C (skipped without a compiler or on a host that is not x86-64), with the lift's own headers (a64emit built here): each of
the 28 hand-written words below translated as the lift does (vecfp on; op_text), its hot copy (inline fast paths), and a
region made of it alone (A64_FASTFP_MIN 1, the MXCSR window), all against the same word translated with FPX off (the
exact integer bodies: the reference). Every vector register and FPSR must be equal on 60,000 cases of each kind: normal
lanes, and lanes mixed with zeros, subnormals, the extreme normals, infinities, quiet and signalling NaNs, products near
the underflow edge (binary64 values in the registers the D forms read); FPCR.FZ off and on; FPSR with IXC only, with every
cumulative flag, and with none (the FPX1 gate closed). On the normal cases the fast path itself must be taken (99.9 %
with IXC set; the min/max and S->D forms whatever FPSR holds), so a fast path that declines everything fails too.
Built at -march=x86-64-v3 and x86-64-v2 (no FMA3: the fused forms decline there, and are checked to be exact anyway).

It has teeth (each by hand, then restored, 2026-11-17): min/max of two equal values given b's bits (max(+0, -0) is -0:
mismatches); FMLS without the sign flip (mismatches); the FZ subnormal test dropped from the product's lanes (mismatches
under FZ); the 2-lane store keeping the top half (mismatches); FCVT D->S keeping a tiny result (UFC missing). It also
caught a real fault while vecfp was written: the scalar FMAX/FMIN decode without the type bits took the D forms for S.
(The same words were also run against Unicorn's AArch64, outside this repository: every register and FPSR equal.)

    python -I -B tests/test_vecfp.py [--cc clang] [--cxx clang++]
"""
import argparse
import os
import platform
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KIT = os.path.join(ROOT, "third_party", "a64-kit")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
for _n in ("A64_FASTFP", "A64_FASTFP_MIN", "A64_VECFP"):
    os.environ.pop(_n, None)
import test_fpx_sticky as S  # noqa: E402  (its compiler, build, run and a64emit helpers; it puts the kit on the path)
import vecfp  # noqa: E402
import fastfp  # noqa: E402

# (assembly, word, fast path needs FPSR.IXC, D registers in: the inputs' lanes are binary64 values)
WORDS = (
    ("fmul v20.4s, v1.4s, v2.s[1]", 0x4FA29034, 1, 0), ("fmul v19.2s, v1.2s, v2.s[3]", 0x0FA29833, 1, 0),
    ("fmla v20.4s, v3.4s, v2.s[2]", 0x4F821874, 1, 0), ("fmla v18.2s, v1.2s, v2.s[0]", 0x0F821032, 1, 0),
    ("fmls v21.4s, v4.4s, v5.s[3]", 0x4FA55895, 1, 0), ("fmls v17.2s, v7.2s, v8.s[1]", 0x0FA850F1, 1, 0),
    ("fmla v1.4s, v1.4s, v1.s[2]", 0x4F811821, 1, 0), ("fmla v23.4s, v8.4s, v9.4s", 0x4E29CD17, 1, 0),
    ("fmla v16.2s, v1.2s, v2.2s", 0x0E22CC30, 1, 0), ("fmls v15.4s, v1.4s, v2.4s", 0x4EA2CC2F, 1, 0),
    ("fmls v14.2s, v1.2s, v2.2s", 0x0EA2CC2E, 1, 0), ("fmul v22.2s, v6.2s, v7.2s", 0x2E27DCD6, 1, 0),
    ("fdiv v24.4s, v10.4s, v11.4s", 0x6E2BFD58, 1, 0), ("fdiv v13.2s, v1.2s, v2.2s", 0x2E22FC2D, 1, 0),
    ("fmax v12.2s, v1.2s, v2.2s", 0x0E22F42C, 0, 0), ("fmaxnm v11.4s, v1.4s, v2.4s", 0x4E22C42B, 0, 0),
    ("fmin v10.4s, v1.4s, v2.4s", 0x4EA2F42A, 0, 0), ("fminnm v9.2s, v1.2s, v2.2s", 0x0EA2C429, 0, 0),
    ("fminnm s0, s1, s2", 0x1E227820, 0, 0), ("fmax s3, s1, s2", 0x1E224823, 0, 0),
    ("fmin s4, s1, s2", 0x1E225824, 0, 0), ("fmaxnm s5, s1, s2", 0x1E226825, 0, 0),
    ("fmax d6, d1, d2", 0x1E624826, 0, 1), ("fmin d7, d1, d2", 0x1E625827, 0, 1),
    ("fmaxnm d8, d1, d2", 0x1E626828, 0, 1), ("fminnm d25, d1, d2", 0x1E627839, 0, 1),
    ("fcvt s26, d1", 0x1E62403A, 1, 1), ("fcvt d27, s1", 0x1E22C03B, 0, 0),
)
FUSED = {0x4F821874, 0x0F821032, 0x4FA55895, 0x0FA850F1, 0x4F811821, 0x4E29CD17, 0x0E22CC30, 0x4EA2CC2F, 0x0EA2CC2E}

DRIVER = r"""
#include "a64.h"
#include <stdio.h>
#include <stdlib.h>
%(funcs)s
typedef void (*F)(Ctx *__restrict);
typedef int (*T)(Ctx *__restrict);
static const struct { const char *name; F lifted, hot, region, exact; T fast; int ixc, dbl, fused; } W[] = {%(table)s};
static uint64_t rs = 20261117;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static uint32_t r32(void) { return (uint32_t)(rnd() >> 16); }
static uint32_t rbelow(uint32_t n) { return (uint32_t)(((rnd() >> 32) * n) >> 32); }
static uint32_t nf(void) { return (r32() & 0x80000000u) | ((107u + rbelow(40)) << 23) | (r32() & 0x7fffffu); }
static uint64_t nd(void) { return ((uint64_t)(r32() & 1) << 63) | ((uint64_t)(1003 + rbelow(40)) << 52) | (rnd() >> 12); }
static uint32_t ef(void) {
    const uint32_t s = r32() & 0x80000000u;
    switch (rbelow(14)) {
    case 0: return s; case 1: return s | (r32() & 0x7fffff) | 1; case 2: return s | 0x00800000u | (rbelow(2) ? 0 : (r32() & 0x7fffff));
    case 3: return s | 0x7f7fffffu; case 4: return s | 0x7f800000u; case 5: return s | 0x7fc00000u | (r32() & 0x3fffff);
    case 6: return s | 0x7f800000u | ((r32() & 0x3fffff) | 1); case 7: return s | ((60u + rbelow(8)) << 23) | (r32() & 0x7fffff);
    case 8: return s | 0x3f800000u;
    default: return nf();
    }
}
static uint64_t ed(void) {
    const uint64_t s = (uint64_t)(r32() & 1) << 63;
    switch (rbelow(12)) {
    case 0: return s; case 1: return s | (rnd() >> 12) | 1; case 2: return s | 0x7ff0000000000000ULL;
    case 3: return s | 0x7ff8000000000000ULL | (rnd() >> 14); case 4: return s | 0x7ff0000000000000ULL | ((rnd() >> 14) | 1);
    case 5: return s | ((uint64_t)(874 + rbelow(30)) << 52) | (rnd() >> 12);   /* (near FLT_MIN: FCVT to S) */
    case 6: return s | ((uint64_t)(1150 + rbelow(30)) << 52) | (rnd() >> 12);  /* (near FLT_MAX) */
    case 7: return s | 0x0010000000000000ULL;
    default: return nd();
    }
}
static void fill(Ctx *c, int edge, int dbl) {
    memset(c, 0, sizeof *c);
    for (int v = 0; v < 32; v++) {
        if (dbl) { c->vreg[v][0] = edge ? ed() : nd(); c->vreg[v][1] = edge ? ed() : nd(); continue; }
        uint32_t l[4];
        for (int j = 0; j < 4; j++) l[j] = edge ? ef() : nf();
        memcpy(c->vreg[v], l, 16);
    }
    if (edge && rbelow(4) == 0) {   /* (equal operands: +0 against -0, and a value against itself) */
        c->vreg[2][0] = c->vreg[1][0] ^ (rbelow(2) ? 0 : (dbl ? 0x8000000000000000ULL : 0x8000000080000000ULL));
        c->vreg[2][1] = c->vreg[1][1];
        if (rbelow(2)) { c->vreg[1][0] &= dbl ? 0x8000000000000000ULL : 0x8000000080000000ULL; c->vreg[2][0] = c->vreg[1][0] ^ (dbl ? 0x8000000000000000ULL : 0x8000000080000000ULL); }
    }
}
static int same(const Ctx *a, const Ctx *b) { return !memcmp(a->vreg, b->vreg, sizeof a->vreg) && a->fpsr == b->fpsr; }
int main(int argc, char **argv) {
    const int n = argc > 1 ? atoi(argv[1]) : 60000;
    if ((_mm_getcsr() & 0xffc0) != 0x1f80) { printf("MXCSR %%x: not the FPX1 host contract\n", _mm_getcsr()); return 2; }
    static const uint64_t fpsrs[3] = {0x10, 0x9f, 0x00};
    int bad = 0;
    for (unsigned w = 0; w < sizeof W / sizeof *W; w++) {
        long long took = 0, normal = 0, mism = 0;
        for (int i = 0; i < n; i++) {
            const int edge = i & 1, fz = (i >> 1) & 1, sr = (i >> 2) %% 3;
            Ctx ref, x;
            fill(&ref, edge, W[w].dbl);
            ref.fpcr = fz ? 0x01000000u : 0;
            ref.fpsr = fpsrs[sr];
            x = ref;
            W[w].exact(&ref);
            F f[3] = {W[w].lifted, W[w].hot, W[w].region};
            for (int k = 0; k < 3; k++) {
                Ctx y = x;
                f[k](&y);
                if (!same(&y, &ref)) {
                    if (mism++ < 4) {
                        printf("%%s (%%s copy) %%s FZ=%%d FPSR=%%02llx: ", W[w].name, k == 0 ? "lifted" : k == 1 ? "hot" : "region",
                               edge ? "edge" : "normal", fz, (unsigned long long)fpsrs[sr]);
                        for (int v = 0; v < 32; v++) if (memcmp(y.vreg[v], ref.vreg[v], 16))
                            printf("v%%d %%016llx%%016llx != %%016llx%%016llx (in v1 %%016llx%%016llx v2 %%016llx%%016llx) ", v,
                                   (unsigned long long)y.vreg[v][1], (unsigned long long)y.vreg[v][0],
                                   (unsigned long long)ref.vreg[v][1], (unsigned long long)ref.vreg[v][0],
                                   (unsigned long long)x.vreg[1][1], (unsigned long long)x.vreg[1][0],
                                   (unsigned long long)x.vreg[2][1], (unsigned long long)x.vreg[2][0]);
                        printf("FPSR %%llx %%llx\n", (unsigned long long)y.fpsr, (unsigned long long)ref.fpsr);
                    }
                }
            }
            if (!edge && (sr != 2 || !W[w].ixc)) { Ctx y = x; normal++; took += W[w].fast(&y); }
        }
        const int need_fast = !(W[w].fused && !A64_TEST_FMA);
        printf("vecfp %%-30s %%lld mismatches; fast path on %%lld of %%lld normal cases\n", W[w].name, mism, took, normal);
        if (mism) bad = 1;
        if (need_fast && took * 1000 < normal * 999) { printf("  (the fast path must be taken on normal cases)\n"); bad = 1; }
        if (!need_fast && took) { printf("  (without FMA3 the fused forms must decline)\n"); bad = 1; }
    }
    return bad;
}
"""


def fail(msg):
    raise SystemExit("vecfp: " + msg)


def check_python():
    on = vecfp.groups()
    for asm, w, _, _ in WORDS:
        if not vecfp.fast_call(w, on):
            fail("no fast path for %s" % asm)
    for asm, w in (("fmul v0.4s, v1.4s, v2.4s", 0x6E22DC20), ("fadd v0.4s, v1.4s, v2.4s", 0x4E22D420),
                   ("fmul s0, s1, v2.s[1]", 0x5FA29020), ("add x5, x5, #1", 0x910004A5)):
        if vecfp.fast_call(w, on):
            fail("a fast path for %s" % asm)
    os.environ["A64_VECFP"] = "0"
    if vecfp.groups() or vecfp.fast_call(0x4FA29034, vecfp.groups()):
        fail("A64_VECFP=0 leaves a group on")
    os.environ["A64_VECFP"] = "minmax,fcvt"
    if vecfp.groups() != {"minmax", "fcvt"} or vecfp.fast_call(0x4FA29034, vecfp.groups()) or \
            not vecfp.fast_call(0x1E62403A, vecfp.groups()):
        fail("A64_VECFP=minmax,fcvt is not those two groups")
    for v, want in (("true", set(vecfp.GROUPS)), ("Yes", set(vecfp.GROUPS)), ("no", set()), ("false", set())):
        os.environ["A64_VECFP"] = v
        if vecfp.groups() != want:
            fail("A64_VECFP=%s gives %s" % (v, sorted(vecfp.groups())))
    for v in ("ture", "minmax,fcvtt", "2"):
        os.environ["A64_VECFP"] = v
        try:
            vecfp.groups()
        except SystemExit as e:
            if "A64_VECFP=" not in str(e):
                fail("A64_VECFP=%s stops without naming the switch: %s" % (v, e))
        else:
            fail("A64_VECFP=%s is taken silently" % v)
    os.environ.pop("A64_VECFP")
    text = vecfp.wrap("{ a64_fpx_mul32_fma32(c,1,2,0); }", vecfp.fast_call(0x4FA29034, on))
    if not text.startswith("if (!a64_vfpx_mul32e(c,c->vreg[20],c->vreg[1],(const uint8_t*)c->vreg[2]+4,4)) {") or \
            "a64_fpx_mul32_fma32" not in text or not fastfp.ff_eligible(text):
        fail("the wrapped C is not the helper, then the lanes, in a region's terms:\n" + text)
    region, nops = fastfp.ff_region([(0x1000 + 4 * i, text) for i in range(8)])
    if nops != 8 or region.count("a64_ff_mul32e(c,") != 8 or region.count("a64_vfpx_mul32e(c,") != 8:
        fail("a region of 8 wrapped multiplies has %d ops and not the twins in its fast copy:\n%s" % (nops, region))
    print("vecfp: python checks passed")


def check_main(a64emit, a64_lift):
    """beside main's own vector paths (nx2/fastvec.py installed, as the lift has it): FMUL 4S by element is left to main's
    a64_vfpx_mul32_elem and every other form is wrapped; A64_NO_VFPX_ELEM=1 hands FMUL 4S by element back to vecfp; A64_VECFP=0
    gives the translator's text unchanged for every word"""
    words = [(0x1000, w) for _, w, _, _ in WORDS]
    base, _ = vecfp._base[0](words)
    for env, mine in ((None, {0x4FA29034}), ("1", set())):
        if env:
            os.environ["A64_NO_VFPX_ELEM"] = env
        try:
            got, _ = a64emit.translate(words)
        finally:
            os.environ.pop("A64_NO_VFPX_ELEM", None)
        for (asm, w, _, _), t, b in zip(WORDS, got, base):
            if w in mine:
                if t != b or "a64_vfpx_mul32_elem(" not in a64_lift.op_text(t, set()):
                    fail("%s is not left to main's by-element path:\n%s" % (asm, t))
            elif not t.startswith("if (!a64_vfpx_"):
                fail("%s is not wrapped (A64_NO_VFPX_ELEM=%s):\n%s" % (asm, env, t))
    os.environ["A64_VECFP"] = "0"
    try:
        got, _ = a64emit.translate(words)
    finally:
        os.environ.pop("A64_VECFP")
    if got != base:
        fail("A64_VECFP=0 changes the translator's text")
    print("vecfp: beside main's vector paths: FMUL 4S by element left to main, %d forms wrapped, A64_VECFP=0 unchanged"
          % (len(WORDS) - 1))


def texts(a64emit, a64_lift, used):
    words = [(0x1000, w) for _, w, _, _ in WORDS]
    lifted, fl = a64emit.translate(words)
    if any(fl):
        fail("a64emit did not translate the words: %s" % fl)
    lifted = [a64_lift.op_text(t, used) for t in lifted]
    for (asm, _, _, _), lt in zip(WORDS, lifted):   # (a "}" at the start of a line ends the function: tests/test_function_ends.py)
        if any(ln.startswith("}") for ln in lt.split("\n")):
            fail("the lifted C of %s puts a '}' at the start of a line:\n%s" % (asm, lt))
    a64emit.lib().a64_config(1, 0)   # (FPX off: the exact integer bodies inline, the reference; vecfp's wrapper not applied)
    try:
        exact, _ = vecfp._base[0](words)
    finally:
        a64emit.lib().a64_config(1, 1)
    out, table = [], []
    for i, ((asm, w, ixc, dbl), lt, ex) in enumerate(zip(WORDS, lifted, exact)):
        region, _ = fastfp.ff_region([(0x1000, lt)], 1)   # (min/max and FCVT have no region op: their own C again)
        out.append("static void lifted%d(Ctx *__restrict c) {\n%s\n}" % (i, lt))
        out.append("static void hot%d(Ctx *__restrict c) {\n%s\n}" % (i, a64_lift.hot_text(lt)))
        out.append("static void region%d(Ctx *__restrict c) {\n%s\n}" % (i, region))
        out.append("static void exact%d(Ctx *__restrict c) {\n%s\n}" % (i, ex))
        out.append("static int fast%d(Ctx *__restrict c) { return %s; }" % (i, vecfp.fast_call(w, vecfp.groups())))
        table.append('{"%s", lifted%d, hot%d, region%d, exact%d, fast%d, %d, %d, %d}' % (asm, i, i, i, i, i, ixc, dbl, w in FUSED))
    return "\n".join(out), ", ".join(table)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cc")
    p.add_argument("--cxx")
    a = p.parse_args()
    check_python()
    if platform.machine().lower() not in ("x86_64", "amd64"):
        print("vecfp: C checks skipped (not an x86-64 host)")
        return
    cc = S.compiler(a.cc, "CC", ("clang", "gcc", "cc"), "clang")
    cxx = S.compiler(a.cxx, "CXX", ("clang++", "g++", "c++"), "clang++")
    if not cc or not cxx:
        print("vecfp: C checks skipped (no C/C++ compiler)")
        return
    t0 = time.time()
    failed = []
    with tempfile.TemporaryDirectory(prefix="longshot-vecfp-") as tmp:
        S.generate(cxx, tmp, S.target_flags(cxx, tmp))
        import a64_lift
        import a64emit
        import fastvec
        fastvec.install()   # (as nx2/lift.py: main's passes first, then vecfp around insn's translate)
        vecfp.install()
        check_main(a64emit, a64_lift)
        os.environ["A64_FASTFP"] = "1"
        used = set()
        funcs, table = texts(a64emit, a64_lift, used)
        with open(os.path.join(tmp, "a64_fpx.h"), "w", newline="\n") as f:   # (as the lift writes it: kit, vecfp, fastfp)
            f.write(a64_lift.fpx_header(used | {("mul32", "fma32", 2, 3), ("fma32", "fma32", 3, 3)}) + vecfp.HEADER +
                    fastfp.FASTFP_HEADER + fastfp.FF_VX_HEADER)
        with open(os.path.join(tmp, "vecfp_test.c"), "w", newline="\n") as f:
            f.write(DRIVER % {"funcs": funcs, "table": table})
        if a64emit._lib is not None:
            import _ctypes
            (_ctypes.FreeLibrary if os.name == "nt" else _ctypes.dlclose)(a64emit._lib._handle)
            a64emit._lib = None
        for march, fma in (("x86-64-v3", 1), ("x86-64-v2", 0)):
            exe = os.path.join(tmp, "vecfp_%s.exe" % march)
            S.build([cc] + S.target_flags(cc, tmp) + ["-march=" + march] + S.FLAGS +
                    ["-D_CRT_SECURE_NO_WARNINGS", "-DA64_TEST_FMA=%d" % fma, "-I" + os.path.join(KIT, "rt"), "-I" + tmp,
                     os.path.join(tmp, "vecfp_test.c"), os.path.join(tmp, "a64_suyu.c"), "-o", exe], "the driver at " + march)
            r = S.run([exe])
            print("\n".join("%s: %s" % (march, l) for l in r.stdout.rstrip().splitlines()))
            if r.returncode:
                failed.append(march)
    os.environ.pop("A64_FASTFP", None)
    if failed:
        fail("FAIL (%s) in %.1f s" % (", ".join(failed), time.time() - t0))
    print("vecfp: pass in %.1f s" % (time.time() - t0))


if __name__ == "__main__":
    main()
