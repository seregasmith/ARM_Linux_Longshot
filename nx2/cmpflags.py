"""(longshot) A subtraction's V flag as N xor (a < b, signed), so that clang folds a signed compare to one host compare (A64_SUB_V).

suyu's translator (third_party/a64-kit/lift/suyu/arm64_to_c.h SetFlags) writes every flag-setting subtraction's NZCV from its
operands and result: SUBS and CMP (immediate, shifted and extended register) and CCMP when its condition holds. V is the
overflow of a - b as bits, ((a ^ b) & (a ^ r)) >> 63 (31 at 32 bits), and the signed conditions read it against N: LT is
N != V, GE N == V, GT and LE the same with Z. clang does not see through that: CMP x1, x2; B.LT, the game's most common
signed loop and bounds test, became sub, xor, andn, and, or and a test of the sign (seven instructions and two registers
where the CPU has cmp and jl), at -O1 and at -O2 alike, in every function, hot or not.

For a subtraction N xor V is exactly a < b as signed integers of the operation's width: N is the sign of the wrapped
difference and V says whether the wrap flipped it (that is what Arm's LT condition means after a CMP). So V = N xor (a <s b),
for every a and b. Written that way, from the result's sign bit and the signed compare (never from c->n, so that dead_flags
sees no new read of N), a reader's N != V is (N xor N) xor (a <s b): clang folds it to the compare, and B.LT, B.GE, B.GT,
B.LE, CSEL and CSET on them become cmp and a jump, cmov or setcc. A reader of V alone (B.VS, B.VC: rare) costs about what it
did. Every flag keeps its value, bit for bit; only the C text of V changes.

The text replaced is SetFlags' subtraction V exactly, and SetFlags is its only writer: ADDS/CMN write V with a ~ in it, and
ADCS/SBCS write theirs in another form (c->v = (... & sign) ? 1 : 0), so neither is touched. Rekarina, another recompiler of
the game, compares a SUBS's operands directly where only that kind reaches the reader (its recomp/flags.py, runtime/cpu.h
fc_sub: https://git.rekarina.dev/rekarina/rekarina); the identity here needs no such analysis and is exact for every reader.

A64_SUB_V (lift time, so in the lift key: an A64_ variable): unset or "1" on; "0" off, and the lift's text is main's to the
byte. Game-free check: tests/test_cmpflags.py (the identity against the old V on edge and random operands at both widths,
the substitution on translated words, and that it leaves ADDS/ADCS/SBCS alone). install() runs at nx2/lift.py's module level,
so the lift's emit processes (nx2/chunks.py) run it too.
"""
import os

import a64emit

# SetFlags' subtraction V, at 64 and 32 bits (arm64_to_c.h: "c->v=(int)((((_fa^_fb)&(_fa^_fr))>>" top ")&1); }")
OLD = {64: "c->v=(int)((((_fa^_fb)&(_fa^_fr))>>63)&1);", 32: "c->v=(int)((((_fa^_fb)&(_fa^_fr))>>31)&1);"}
# (_fa, _fb, _fr: the width's unsigned operands and result, as SetFlags declares them)
NEW = {64: "c->v=(int)(((_fr>>63)&1)^((int64_t)_fa<(int64_t)_fb));",
       32: "c->v=(int)(((_fr>>31)&1)^((int32_t)_fa<(int32_t)_fb));"}


def on():
    v = os.environ.get("A64_SUB_V", "1").strip().lower()
    if v in ("", "1", "on", "true", "yes"):
        return True
    if v in ("0", "off", "false", "no"):
        return False
    raise SystemExit("A64_SUB_V=%s: 1 or 0" % os.environ.get("A64_SUB_V"))


def has_sub_v(text):
    """whether text has SetFlags' subtraction V: its exact text, since "_fa^_fb" alone is in ADDS/CMN's ~(_fa^_fb) too"""
    return OLD[64] in text or OLD[32] in text


def rewrite(text):
    """an instruction's translated C with its subtraction V (if any) written as N xor (a <s b)"""
    if not has_sub_v(text):
        return text
    for width in (64, 32):
        text = text.replace(OLD[width], NEW[width])
    return text


_installed = False
_base = []   # (the translate this one wraps: the tests take their reference from it)


def install():
    """a64emit.translate wrapped. Once; nothing with A64_SUB_V=0"""
    global _installed
    if _installed or not on():
        return
    _installed = True
    base_translate = a64emit.translate
    _base.append(base_translate)

    def translate(items, batch=65536):
        texts, flags = base_translate(items, batch)
        if not any(has_sub_v(t) for t in texts):
            return texts, flags
        return [rewrite(t) for t in texts], flags

    a64emit.translate = translate
