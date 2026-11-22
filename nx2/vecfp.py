"""(longshot) SSE fast paths for the float forms the translator does lane by lane or with no fast path at all (A64_VECFP).

What the per-instruction translator (third_party/a64-kit/lift/suyu/arm64_to_c.h) writes for these forms, and what it costs on
the exact path (outside a fast float region; cycles a call on a Ryzen 9 5900X, measured offline on game routines lifted and
compiled as the build does):
  - FMUL, FMLA, FMLS by element (4S, 2S) and vector FMLA, FMLS (4S, 2S), FMUL 2S, FDIV (4S, 2S): a loop over the lanes, each
    lane a call of the scalar FPX helper (its gate, its native op, its keep test): 40 to 60 for a 4-lane one, where the
    kit's own vectorize_fpx makes 2- and 4-lane FADD/FSUB and 4-lane FMUL one SSE op (13);
  - FMAX, FMIN, FMAXNM, FMINNM (S, D, 2S, 4S): no fast path, an integer body per lane (NaN classes, FZ, the zero signs):
    about 50 for 4 lanes, 370 host instructions;
  - FCVT between S and D (scalar): no fast path, a software conversion with its rounding modes: 25 to 40.
Here each such instruction's C becomes `if (!a64_vfpx_<form>(c, ...)) { <its C as before> }`: the helper does the op on SSE
and keeps the result only where FPX1's rules prove it Arm's with no FPSR bit to add (a64_suyu.h's comment: the gate, FPSR.IXC
already set and FPCR at its default but FZ, and per lane the RECOMP_FPX_KEEP32 family of tests), writing the destination
and returning 1; else it writes nothing and returns 0, and the instruction's own C runs as it always did. So every result is
the one the exact C gives, and that C is still there for every case the fast path declines. The rules, per form:
  - multiplies, multiply-adds, divides: FPX1's (KEEP32 / KEEPDIV32; under FZ no subnormal operand, the addend included), on
    every lane, with FMA3 for the fused forms (none without it: the helper declines and the lanes run as before);
  - FMAX/FMIN(NM): exact whenever no operand is a NaN (and, under FZ, none is subnormal): the larger (smaller) by IEEE
    order, of two equal values the AND (OR) of their bits, which is +0 for max(+0, -0) as Arm has it. They raise no flag,
    and the NM forms differ only on NaNs. Gate: FPCR at its default but FZ (FPSR.IXC is not needed: nothing is inexact);
  - FCVT S to D: exact for every number (no flag); a NaN, or under FZ a subnormal, declines. Gate as FMAX's;
  - FCVT D to S: FPX1's gate, kept when the result is big (RECOMP_FPX_BIG32: no overflow, no underflow) or the input is a
    zero (an exact zero of its sign).
The helpers' names begin a64_vfpx_ (and take the context first), so nx2/fastfp.py's ff_eligible lets these instructions
join a fast float region as the lane loops did; inside a region's fast copy they run as they are (SSE with their own tests,
correct with the region's MXCSR window or without), and their lane loops' scalar helpers are the region's own (FF_CONV).

A64_VECFP (lift time, so in the lift key: an A64_ variable): unset or "1" (also "on", "all", "true", "yes") all of the
above; "0" ("off", "none", "false", "no") none (the lift as before); or a comma list of the groups: elem (by element), vec
(vector FMLA/FMLS, FMUL 2S, FDIV), minmax, fcvt. Anything else stops the lift with a message, so a typo cannot turn every
path off unnoticed.
With A64_VECFP=0 nothing is wrapped and neither this HEADER nor nx2/fastfp.py's FF_VX_HEADER is written: the lift's C and
a64_fpx.h are main's to the byte. A form the lift's own passes already make a vector op (the kit's 4-lane FMUL, nx2/fastvec.py's
FMUL 4S by element) is left to them (main_handles). Guest registers as C locals (nx2/guest_locals.py) cover the general
registers and NZCV only; these helpers read and write c->vreg, c->fpcr and c->fpsr, which stay in the context, so a wrapped
instruction is right in a function with locals as without.
Game-free tests: tests/test_vecfp.py (every form against the same word translated with FPX off, on random and edge cases).
install() is run by nx2/lift.py at its module level, after nx2/insn.py's (it wraps that translate); the lift's new emit
processes on Windows (nx2/chunks.py spawn_emit) run that module level again, and _spawn_init compares their a64emit.translate
with the lift's (_identities), so a process without vecfp's wrapper stops rather than emit different C.
"""
import os

import a64emit
import a64_lift

GROUPS = ("elem", "vec", "minmax", "fcvt")


def groups():
    """the groups A64_VECFP turns on"""
    v = os.environ.get("A64_VECFP", "1").strip().lower()
    if v in ("", "1", "on", "all", "true", "yes"):
        return set(GROUPS)
    if v in ("0", "off", "none", "false", "no"):
        return set()
    names = [g.strip() for g in v.split(",") if g.strip()]
    bad = [g for g in names if g not in GROUPS]
    if bad:
        raise SystemExit("A64_VECFP=%s: unknown %s (1 or 0, or a comma list of %s)" % (
            os.environ.get("A64_VECFP"), ", ".join(bad), ", ".join(GROUPS)))
    return set(names)


def fast_call(w, on):
    """the helper call (C expression) for the instruction word w, or None when w is not one of these forms (or its group
    is off)"""
    d, n, m = w & 31, (w >> 5) & 31, (w >> 16) & 31
    q = (w >> 30) & 1
    k = 4 if q else 2
    v = "c->vreg[%d]"
    if "elem" in on and (w & 0xBFC0F400) in (0x0F809000, 0x0F801000, 0x0F805000):
        # FMUL / FMLA / FMLS (by element), single: Rm = M:Rm (bits 20:16), index H:L
        op = {0x0F809000: "mul32e", 0x0F801000: "fma32e", 0x0F805000: "fms32e"}[w & 0xBFC0F400]
        idx = ((w >> 11) & 1) << 1 | ((w >> 21) & 1)
        return "a64_vfpx_%s(c,%s,%s,(const uint8_t*)%s+%d,%d)" % (op, v % d, v % n, v % (m & 31), 4 * idx, k)
    if "vec" in on:
        f = w & 0xBFE0FC00
        if f in (0x0E20CC00, 0x0EA0CC00):   # FMLA / FMLS (vector), single
            return "a64_vfpx_%s(c,%s,%s,%s,%d)" % ("fma32v" if f == 0x0E20CC00 else "fms32v", v % d, v % n, v % m, k)
        if f == 0x2E20DC00 and not q:   # FMUL (vector) 2S (4S: the kit's vectorize_fpx)
            return "a64_vfpx_mul32v(c,%s,%s,%s,2)" % (v % d, v % n, v % m)
        if f == 0x2E20FC00:   # FDIV (vector), single
            return "a64_vfpx_div32v(c,%s,%s,%s,%d)" % (v % d, v % n, v % m, k)
    if "minmax" in on:
        f = w & 0xBFE0FC00
        mm = {0x0E20F400: (0, 0), 0x0EA0F400: (1, 0), 0x0E20C400: (0, 1), 0x0EA0C400: (1, 1)}   # (min, nm)
        if f in mm:   # FMAX / FMIN / FMAXNM / FMINNM (vector), single
            return "a64_vfpx_minmax32v(c,%s,%s,%s,%d,%d)" % (v % d, v % n, v % m, k, mm[f][0])
        f = w & 0xFFE0FC00   # (type, bits 23:22: 00 S, 01 D)
        sm = {0x1E204800: (0, 32), 0x1E205800: (1, 32), 0x1E206800: (0, 32), 0x1E207800: (1, 32),
              0x1E604800: (0, 64), 0x1E605800: (1, 64), 0x1E606800: (0, 64), 0x1E607800: (1, 64)}
        if f in sm:   # FMAX / FMIN / FMAXNM / FMINNM (scalar), S and D
            mn, bits = sm[f]
            return "a64_vfpx_minmax%ds(c,%s,%s,%s,%d)" % (bits, v % d, v % n, v % m, mn)
    if "fcvt" in on:
        if (w & 0xFFFFFC00) == 0x1E22C000:   # FCVT Dd, Sn
            return "a64_vfpx_cvtds(c,%s,%s)" % (v % d, v % n)
        if (w & 0xFFFFFC00) == 0x1E624000:   # FCVT Sd, Dn
            return "a64_vfpx_cvtsd(c,%s,%s)" % (v % d, v % n)
    return None


HEADER = r"""
/* ---- (longshot) SSE fast paths for lane-by-lane float forms (A64_VECFP, nx2/vecfp.py) ----
   Each a64_vfpx_<form> writes its destination and returns 1 only when every lane's native result is provably Arm's (FPX1's
   rules, a64_suyu.h); else it writes nothing and returns 0, and the instruction's own C runs. */
#if RECOMP_FPX_HOST == 2 && (defined(__x86_64__) || defined(_M_X64))
#include <immintrin.h>
#define A64_VX static inline __attribute__((always_inline))
A64_VX __m128i a64_vx_ult(__m128i x, uint32_t k) {   /* unsigned x < k, per lane */
    return _mm_cmplt_epi32(_mm_xor_si128(x, _mm_set1_epi32((int)0x80000000u)), _mm_set1_epi32((int)(k ^ 0x80000000u)));
}
A64_VX __m128i a64_vx_abs(__m128i x) { return _mm_and_si128(x, _mm_set1_epi32(0x7fffffff)); }
A64_VX __m128i a64_vx_big(__m128i t) { return a64_vx_ult(_mm_sub_epi32(a64_vx_abs(t), _mm_set1_epi32(0x00800001)), 0x7effffffu); }
A64_VX __m128i a64_vx_zero(__m128i x) { return _mm_cmpeq_epi32(a64_vx_abs(x), _mm_setzero_si128()); }
A64_VX __m128i a64_vx_nonfin(__m128i t) { const __m128i e = _mm_set1_epi32(0x7f800000); return _mm_cmpeq_epi32(_mm_and_si128(t, e), e); }
A64_VX __m128i a64_vx_nan(__m128i x) { return _mm_cmpgt_epi32(a64_vx_abs(x), _mm_set1_epi32(0x7f800000)); }
A64_VX __m128i a64_vx_subn(__m128i x) { return a64_vx_ult(_mm_sub_epi32(_mm_slli_epi32(x, 1), _mm_set1_epi32(1)), 0x00ffffffu); }
A64_VX int a64_vx_none(__m128i bad, int k) { return !(_mm_movemask_epi8(bad) & (k == 4 ? 0xFFFF : 0x00FF)); }
A64_VX void a64_vx_store(uint64_t *d, __m128i t, int k) {   /* (2 lanes: the top half zero, as Arm's 64-bit ops leave it) */
    _mm_storeu_si128((__m128i *)d, k == 4 ? t : _mm_move_epi64(t));
}
A64_VX __m128i a64_vx_fz(const Ctx *c) { return _mm_set1_epi32(-(int)(RECOMP_FPX_FZ(c) != 0)); }
/* KEEP32 (a product: big, or a finite product of a zero) and FZOK32 under FZ; the "not kept" lanes */
A64_VX __m128i a64_vx_mul_bad(const Ctx *c, __m128i a, __m128i b, __m128i t) {
    const __m128i keep = _mm_or_si128(a64_vx_big(t), _mm_andnot_si128(a64_vx_nonfin(t), _mm_or_si128(a64_vx_zero(a), a64_vx_zero(b))));
    return _mm_or_si128(_mm_xor_si128(keep, _mm_set1_epi32(-1)), _mm_and_si128(a64_vx_fz(c), _mm_or_si128(a64_vx_subn(a), a64_vx_subn(b))));
}
A64_VX int a64_vfpx_mul32_(Ctx *c, uint64_t *d, __m128i a, __m128i b, int k) {
    if (!RECOMP_FPX_OPEN(c)) return 0;
    const __m128i t = _mm_castps_si128(_mm_mul_ps(_mm_castsi128_ps(a), _mm_castsi128_ps(b)));
    if (!a64_vx_none(a64_vx_mul_bad(c, a, b, t), k)) return 0;
    a64_vx_store(d, t, k);
    return 1;
}
#ifdef __FMA__
/* FMLA / FMLS: d + a*b rounded once (FMLS: a's sign flipped first, as Arm's FMLS and the lifted lanes do); KEEP32 of the
   product's operands, and under FZ no subnormal operand or addend (recomp_fpx_fma32) */
A64_VX int a64_vfpx_fma32_(Ctx *c, uint64_t *d, __m128i a, __m128i b, int k) {
    if (!RECOMP_FPX_OPEN(c)) return 0;
    const __m128i z = _mm_loadu_si128((const __m128i *)d);
    const __m128i t = _mm_castps_si128(_mm_fmadd_ps(_mm_castsi128_ps(a), _mm_castsi128_ps(b), _mm_castsi128_ps(z)));
    const __m128i bad = _mm_or_si128(a64_vx_mul_bad(c, a, b, t), _mm_and_si128(a64_vx_fz(c), a64_vx_subn(z)));
    if (!a64_vx_none(bad, k)) return 0;
    a64_vx_store(d, t, k);
    return 1;
}
#else
A64_VX int a64_vfpx_fma32_(Ctx *c, uint64_t *d, __m128i a, __m128i b, int k) { (void)c; (void)d; (void)a; (void)b; (void)k; return 0; }
#endif
A64_VX __m128i a64_vx_ld(const uint64_t *s) { return _mm_loadu_si128((const __m128i *)s); }
A64_VX __m128i a64_vx_lane(const void *p) { uint32_t x; memcpy(&x, p, 4); return _mm_set1_epi32((int)x); }
A64_VX __m128i a64_vx_neg(__m128i a) { return _mm_xor_si128(a, _mm_set1_epi32((int)0x80000000u)); }
A64_VX int a64_vfpx_mul32e(Ctx *c, uint64_t *d, const uint64_t *n, const void *m, int k) { return a64_vfpx_mul32_(c, d, a64_vx_ld(n), a64_vx_lane(m), k); }
A64_VX int a64_vfpx_fma32e(Ctx *c, uint64_t *d, const uint64_t *n, const void *m, int k) { return a64_vfpx_fma32_(c, d, a64_vx_ld(n), a64_vx_lane(m), k); }
A64_VX int a64_vfpx_fms32e(Ctx *c, uint64_t *d, const uint64_t *n, const void *m, int k) { return a64_vfpx_fma32_(c, d, a64_vx_neg(a64_vx_ld(n)), a64_vx_lane(m), k); }
A64_VX int a64_vfpx_mul32v(Ctx *c, uint64_t *d, const uint64_t *n, const uint64_t *m, int k) { return a64_vfpx_mul32_(c, d, a64_vx_ld(n), a64_vx_ld(m), k); }
A64_VX int a64_vfpx_fma32v(Ctx *c, uint64_t *d, const uint64_t *n, const uint64_t *m, int k) { return a64_vfpx_fma32_(c, d, a64_vx_ld(n), a64_vx_ld(m), k); }
A64_VX int a64_vfpx_fms32v(Ctx *c, uint64_t *d, const uint64_t *n, const uint64_t *m, int k) { return a64_vfpx_fma32_(c, d, a64_vx_neg(a64_vx_ld(n)), a64_vx_ld(m), k); }
/* FDIV: KEEPDIV32 (big, or a finite quotient of a zero), under FZ no subnormal operand. (2 lanes: the top two of the
   divisor may be zeros; their lanes are not looked at) */
A64_VX int a64_vfpx_div32v(Ctx *c, uint64_t *d, const uint64_t *n, const uint64_t *m, int k) {
    if (!RECOMP_FPX_OPEN(c)) return 0;
    const __m128i a = a64_vx_ld(n), b = a64_vx_ld(m);
    const __m128i t = _mm_castps_si128(_mm_div_ps(_mm_castsi128_ps(a), _mm_castsi128_ps(b)));
    const __m128i keep = _mm_or_si128(a64_vx_big(t), _mm_andnot_si128(a64_vx_nonfin(t), a64_vx_zero(a)));
    const __m128i bad = _mm_or_si128(_mm_xor_si128(keep, _mm_set1_epi32(-1)), _mm_and_si128(a64_vx_fz(c), _mm_or_si128(a64_vx_subn(a), a64_vx_subn(b))));
    if (!a64_vx_none(bad, k)) return 0;
    a64_vx_store(d, t, k);
    return 1;
}
/* FMAX / FMIN (and the NM forms, the same without a NaN): no NaN operand, under FZ no subnormal one; then the larger (smaller)
   by IEEE order, and of two equal values (+0 and -0 among them) the AND (OR) of their bits. FPCR at its default but FZ (the
   rounding mode does not matter: nothing is rounded) */
#define A64_VX_FPCR_OK(c) (!((c)->fpcr & RECOMP_FPX_FPCR_MASK))
A64_VX int a64_vfpx_minmax32v(Ctx *c, uint64_t *d, const uint64_t *n, const uint64_t *m, int k, int min) {
    if (!A64_VX_FPCR_OK(c)) return 0;
    const __m128i a = a64_vx_ld(n), b = a64_vx_ld(m);
    const __m128i bad = _mm_or_si128(_mm_or_si128(a64_vx_nan(a), a64_vx_nan(b)),
                                     _mm_and_si128(a64_vx_fz(c), _mm_or_si128(a64_vx_subn(a), a64_vx_subn(b))));
    if (!a64_vx_none(bad, k)) return 0;
    const __m128 fa = _mm_castsi128_ps(a), fb = _mm_castsi128_ps(b);
    const __m128i pick = _mm_castps_si128(min ? _mm_cmplt_ps(fa, fb) : _mm_cmpgt_ps(fa, fb));   /* (a wins) */
    const __m128i eq = _mm_castps_si128(_mm_cmpeq_ps(fa, fb));
    const __m128i same = min ? _mm_or_si128(a, b) : _mm_and_si128(a, b);
    __m128i t = _mm_or_si128(_mm_and_si128(pick, a), _mm_andnot_si128(pick, b));
    t = _mm_or_si128(_mm_and_si128(eq, same), _mm_andnot_si128(eq, t));
    a64_vx_store(d, t, k);
    return 1;
}
A64_VX int a64_vfpx_minmax32s(Ctx *c, uint64_t *d, const uint64_t *n, const uint64_t *m, int min) {
    if (!A64_VX_FPCR_OK(c)) return 0;
    uint32_t a, b;
    memcpy(&a, n, 4);
    memcpy(&b, m, 4);
    if ((a & 0x7fffffffu) > 0x7f800000u || (b & 0x7fffffffu) > 0x7f800000u) return 0;
    if (RECOMP_FPX_FZ(c) && (RECOMP_FPX_SUBN32(a) || RECOMP_FPX_SUBN32(b))) return 0;
    float fa, fb;
    memcpy(&fa, &a, 4);
    memcpy(&fb, &b, 4);
    const uint32_t t = fa == fb ? (min ? a | b : a & b) : ((min ? fa < fb : fa > fb) ? a : b);
    a64_vset64(d, t);
    return 1;
}
A64_VX int a64_vfpx_minmax64s(Ctx *c, uint64_t *d, const uint64_t *n, const uint64_t *m, int min) {
    if (!A64_VX_FPCR_OK(c)) return 0;
    uint64_t a, b;
    memcpy(&a, n, 8);
    memcpy(&b, m, 8);
    if ((a & UINT64_C(0x7fffffffffffffff)) > UINT64_C(0x7ff0000000000000) ||
        (b & UINT64_C(0x7fffffffffffffff)) > UINT64_C(0x7ff0000000000000)) return 0;
    if (RECOMP_FPX_FZ(c) && (RECOMP_FPX_SUBN64(a) || RECOMP_FPX_SUBN64(b))) return 0;
    double fa, fb;
    memcpy(&fa, &a, 8);
    memcpy(&fb, &b, 8);
    const uint64_t t = fa == fb ? (min ? a | b : a & b) : ((min ? fa < fb : fa > fb) ? a : b);
    a64_vset64(d, t);
    return 1;
}
/* FCVT Dd, Sn: exact for every number; a NaN (its quieting and payload, FPCR.DN) or under FZ a subnormal (flushed, IDC)
   declines */
A64_VX int a64_vfpx_cvtds(Ctx *c, uint64_t *d, const uint64_t *n) {
    if (!A64_VX_FPCR_OK(c)) return 0;
    uint32_t a;
    memcpy(&a, n, 4);
    if ((a & 0x7fffffffu) > 0x7f800000u || (RECOMP_FPX_FZ(c) && RECOMP_FPX_SUBN32(a))) return 0;
    float f;
    memcpy(&f, &a, 4);
    a64_vset64(d, recomp_fpx_db((double)f));
    return 1;
}
/* FCVT Sd, Dn: FPX1's gate (round to nearest, FPSR.IXC set); kept when the result is big (no overflow, no underflow: what is
   left is IXC) or the input is a zero (exact). Under FZ a subnormal input gives a tiny result, not kept */
A64_VX int a64_vfpx_cvtsd(Ctx *c, uint64_t *d, const uint64_t *n) {
    if (!RECOMP_FPX_OPEN(c)) return 0;
    uint64_t a;
    memcpy(&a, n, 8);
    double f;
    memcpy(&f, &a, 8);
    const uint32_t t = (uint32_t)recomp_fpx_fb((float)f);
    if (!RECOMP_FPX_BIG32(t) && !RECOMP_FPX_ZERO64(a)) return 0;
    a64_vset64(d, t);
    return 1;
}
#else
#define a64_vfpx_mul32e(c, ...) 0
#define a64_vfpx_fma32e(c, ...) 0
#define a64_vfpx_fms32e(c, ...) 0
#define a64_vfpx_mul32v(c, ...) 0
#define a64_vfpx_fma32v(c, ...) 0
#define a64_vfpx_fms32v(c, ...) 0
#define a64_vfpx_div32v(c, ...) 0
#define a64_vfpx_minmax32v(c, ...) 0
#define a64_vfpx_minmax32s(c, ...) 0
#define a64_vfpx_minmax64s(c, ...) 0
#define a64_vfpx_cvtds(c, ...) 0
#define a64_vfpx_cvtsd(c, ...) 0
#endif
"""


def main_handles(text):
    """whether the lift's own passes already give this instruction's C a vector fast path: the kit's vectorize_fpx (4-lane
    FMUL, 2- and 4-lane FADD/FSUB) or nx2/fastvec.py's wrapper of it (FMUL 4S by element as a64_vfpx_mul32_elem, #1576).
    Asked of the translator's text as op_text makes it (outline_fpx, then vectorize_fpx as installed): a form main handles is
    left to main, so vecfp adds a path only where main has none; A64_NO_VFPX_ELEM=1 (main's by-element pass off) hands FMUL
    4S by element back to vecfp"""
    return "a64_vfpx_" in a64_lift.vectorize_fpx(a64_lift.outline_fpx(text, set()))


def wrap(text, call):
    """an instruction's C behind its fast path. The brace that closes the `if` is indented: in the lifted C a "}" at the start
    of a line closes a function (the lifter writes every instruction's own blocks so), and nx2/split_giants.py and
    nx2/oot_patches.py end a function's body at the first one. With it at the start of a line 221 of the game's 368 giant
    functions were compiled whole (a file of them 1.4 to 3.4 times as long), and two of oot_patches' edits were looked for in
    part of their function and not made (the free camera, the camera centering). tests/test_function_ends.py"""
    return "if (!%s) {\n%s\n    }\n" % (call, text.rstrip("\n"))


_installed = False
_base = []   # (the translate this one wraps: tests take their reference from it)


def installed():
    """install() has run (nx2/fastfp.py asks before it appends the region twins, FF_VX_HEADER)"""
    return _installed


def install():
    """a64emit.translate wrapped (the forms above behind their fast paths) and a64_lift.Lifter.write wrapped (HEADER at the
    end of a64_fpx.h). Once; after nx2/insn.py's install (it wraps that translate)"""
    global _installed
    if _installed:
        return
    _installed = True
    base_translate = a64emit.translate
    _base.append(base_translate)

    def translate(items, batch=65536):
        texts, flags = base_translate(items, batch)
        on = groups()
        if on:
            for i, (pc, w) in enumerate(items):
                if not flags[i]:
                    call = fast_call(w, on)
                    if call and not main_handles(texts[i]):
                        texts[i] = wrap(texts[i], call)
        return texts, flags

    a64emit.translate = translate
    base_write = a64_lift.Lifter.write

    def write(self, out_dir, chunk_bytes=a64_lift.CHUNK_BYTES):
        files = base_write(self, out_dir, chunk_bytes=chunk_bytes)
        if groups():   # (A64_VECFP=0: a64_fpx.h as main writes it, to the byte)
            with open(os.path.join(out_dir, "a64_fpx.h"), "a", newline="\n") as f:
                f.write(HEADER)
        return files

    a64_lift.Lifter.write = write
