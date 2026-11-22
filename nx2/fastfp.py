"""(longshot) Fast float regions in lifted code (A64_FASTFP at lift time), outside the vendored kit.

A straight run of single-precision float math is lifted twice: a fast copy of plain SSE ops checked once by MXCSR, and the
exact copy (the lifted code as before) as the fallback. The rules and the header the fast copies compile against are below
(ff_region, FASTFP_HEADER); see the comment above FF_CALL.

third_party/a64-kit stays as it was imported (CONTRIBUTING.md: a change goes outside it), so this module:
  - replaces a64_lift.Lifter.emit_function with emit_function below: a copy of the kit's, with the lines that gather a run of
    joinable instructions and emit it through ff_region added, each tagged "# (fastfp)". The copy was taken from
    third_party/a64-kit/lift/a64_lift.py at a64-kit's tree-sha256 6164061e (KIT_TREE) (its PROVENANCE.md).
    tests/test_fastfp_override.py checks that the copy without its tagged lines is the kit's method, and that the kit's tree
    is still that one: an a64-kit refresh fails it until this copy follows;
  - wraps a64_lift.Lifter.write: FASTFP_HEADER appended to a64_fpx.h after the kit writes it, and the regions' count printed.
install() runs before nx2/lift_fixes.py's and nx2/poll.py's wrappers of emit_function (nx2/lift.py), so they wrap this one.
The lift's new processes (nx2/chunks.py spawn_emit) run nx2/lift.py's module level again, so they emit with it too.
"""
import os
import re

import a64_lift
import a64emit
# (the kit's names the copied method reads; nothing in nx2 rebinds them: nx2/insn.py replaces a64_lift.classify and
# a64emit.translate, which the method does not name, or names through the module. fp_is_frame is the kit's, its answers
# kept below)
from a64_lift import (COND, FLAG_REF, FP_STORE, FP_STORE_PRE, MG_NOT_STRAIGHT, REG_REF, SYNC_ALL, SYNC_BACK, SYNC_CALL,
                      SYNC_RELOAD, SYNC_RET, drop_flag_stores, frame_stores, local_regs, localize_regs, pc_relative)

_frame = {}


def fp_is_frame(text):
    """a64_lift.fp_is_frame, kept by the text: the copied method below asks it of every instruction (14 million in a lift
    of OoT, two regex searches each), of far fewer distinct texts"""
    f = _frame.get(text)
    if f is None:
        f = _frame[text] = a64_lift.fp_is_frame(text)
    return f

KIT_TREE = "6164061ea8dc7896da53490df9ebc5e43b50faa50c1da61cdf9fc5e207773f7d"   # (a64-kit's tree-sha256 the copy was taken at)


# (longshot) Fast float regions (A64_FASTFP at lift time: on unless "0"; "hot" the A64_HOT_FUNCS list only; "check"
# both copies every time, compared: see ff_region).
# Each float op on the exact path carries its own tests (~25 instructions around one SSE op). A straight run of
# instructions with no label, call, branch or system access inside (and no guest load after a guest store: see
# FF_STORE below; with A64_FASTFP_STORES=0 no store at all), holding at least A64_FASTFP_MIN (8) single-
# precision adds, subtracts, multiplies, divides or square roots, is emitted twice: a fast copy whose ops are plain SSE,
# with MXCSR's flags cleared before it and read once after, and the exact copy. A clean fast run is the Arm result: with
# no invalid operation (no NaN made), no denormal operand and no underflow (FTZ on under FPCR.FZ: a tiny result is
# flushed and flagged), IEEE single precision round-to-nearest is the same on both, except for a NaN result or a
# product, quotient or multiply-add of exactly 2^-126, which raise the invalid flag on purpose (a64_ff_guard_; the ops
# are asm with Arm's operand order all the same). Divide-by-zero and overflow give the same infinities; they and inexact
# are or-ed into FPSR. On an invalid, denormal or underflow flag the registers the run names are put back and the exact
# copy runs. Measured
# 2026-11-11 on OoT's f_820e1d60 in game (both run per call, compared): 0 mismatches in 150,000 calls, 1.79x faster.
# HOS_NO_FASTFP (run time, any value): the exact copies only, for comparisons.
FF_CALL = re.compile(r"\b((?:recomp|a64|A64|RECOMP)_\w+)\s*\(")
FF_PURE = {"recomp_load8", "recomp_load16", "recomp_load32", "recomp_load64", "recomp_ldp32", "recomp_ldp64",
           "a64_vset64", "a64_vset128", "a64_vset_lo", "a64_vst16", "a64_fcmp32", "a64_fcmp64", "a64_vfcmp",
           "recomp_fixed_to_fp", "recomp_fp_to_int", "recomp_fp_to_fixed", "recomp_umulh", "recomp_smulh",
           "recomp_fp_estimate", "recomp_aes_sbox", "RECOMP_LIKELY", "RECOMP_UNLIKELY", "RECOMP_FPX_OPEN",
           "RECOMP_FPX_FZ", "a64_fcmp32k", "a64_fcmp64k"}
# ((longshot) nx2/fastvec.py's whole-register moves and byte shuffles (#1576): LDR Q as a64_vld16 (a guest load: FF_LOAD),
# DUP by element as a64_vdup, EXT as A64_EXT16 / A64_EXT8, TBL as a64_tbl1. Integer SSE on the registers, no float flag;
# before #1576 the same instructions were recomp_ldp64 and memcpy / a64_vst16 blocks, which joined. Named after FF_PURE was
# written, they ended runs: in the lifted C of 89c3d21e's base, ~23,700 run ends within 6 statements of float ops on both
# sides in the functions play runs (8,100 a64_vld16, 7,500 a64_vdup, 6,700 A64_EXT16). A64_FASTFP_VMOVES=0 at lift time:
# they end a run again)
FF_VMOVES = {"a64_vld16", "a64_vdup", "A64_EXT16", "A64_EXT8", "a64_tbl1"}
FF_BAD = ("c->fpcr", "c->fpsr", "c->excl", "c->pc", "c->halted", "c->poll", "c->tpidr", "\x01", "goto ", "return",
          "switch", "A64_PTR", "A64_MARK")
# (the exact blocks' own uses, safe in a region: FPCR read in a test (it doesn't change inside one: no MSR there), and
# FPSR's cumulative bits or-ed in (put back with the registers when the region runs again exactly). An MSR, or an MRS of
# FPSR, which would see a value the fast copy has not finished, still ends the run.)
FF_FP_OK = re.compile(r"c->fpcr\s*&|c->fpsr\s*\|=")
# ((longshot) fma32_fma32: FMADD/FMSUB/FNMADD/FNMSUB and FMLA/FMLS S lanes, an FMA3 op where the build has one; see
# a64_ff_fma32_fma32 in FASTFP_HEADER)
FF_CONV = re.compile(r"\ba64_vfpx_(add32|sub32|mul32)\(|"
                     r"\ba64_fpx_(add32_add32|sub32_sub32|mul32_fma32|fma32_fma32|div32_div32|sqrt32_sqrt32)\(")
VREG_REF = re.compile(r"c->vreg\[(\d+)\]")
# ((longshot) nx2/vecfp.py: an instruction it puts behind an SSE fast path, `if (!a64_vfpx_<form>(c, ...)) { its lanes }`,
# is in a region's fast copy `if (!a64_ff_<form>(c, ...)) { ... }`: one SSE op in the MXCSR window with the guard on
# its lanes, which always answers 1, so the lanes are dead code there. The run's count of float ops is still its lanes'
# FF_CONV matches, as before vecfp)
FF_VX = re.compile(r"\ba64_vfpx_(mul32e|fma32e|fms32e|mul32v|fma32v|fms32v|div32v)\(")


def fastfp_mode():
    """the mode: empty when A64_FASTFP is 0 (off), else its value, "1" by default (every function), hot or check"""
    v = os.environ.get("A64_FASTFP", "1")
    return "" if v == "0" else v


def ff_min():
    v = os.environ.get("A64_FASTFP_MIN", "")
    return int(v) if v.isdigit() else 8


# (longshot) Guest stores inside a region (A64_FASTFP_STORES at lift time: on unless "0"). A plain store (STR, STP, with
# or without writeback, through any base: recomp_storeN / recomp_stpN and their _sp forms) may join a run, and the run is
# cut before a guest load that follows a store in it (ff_split), so every load of a region comes before every store of
# it. The exact copy is the lifted text as before. The fast copy does not store: each store's address and values go
# into locals (_ffaK, _ffdK, and _ffeK for a pair's second value) where the store stood, and once a64_ff_end has passed
# the stores run from them, in their order, with the same helpers (so the same page marks), before the jump past the
# exact copy. On a flag that sends the run to its exact copy nothing has been stored, the registers are put back and the
# exact copy stores as before. The guest's memory sees the same accesses in the same order either way (the region's
# loads, then its stores), only later by the region's register work; another thread could see a store a few
# instructions later, never a different one or out of order. A store through x29 that the function's frame stores would
# make an unmarked _sp store (a64_lift.frame_stores) names its locals _ffp..., and ff_frame_stores does the same to the
# deferred one. Not joined: STLR (a plain recomp_storeN(c,c->x[N],...), as mark_groups reads it), a store under a
# condition (MG_NOT_STRAIGHT), and a text whose own load follows its store; exclusive, atomic and memcpy (A64_PTR)
# stores stay out by FF_BAD.
FF_STORE = re.compile(r"\brecomp_(store8|store16|store32|store64|stp32|stp64)(_sp)?\(")
FF_STORE_SIZE = {"store8": 8, "store16": 16, "store32": 32, "store64": 64, "stp32": 32, "stp64": 64}
FF_LOAD = re.compile(r"\b(?:recomp_(?:load(?:8|16|32|64)|ldp(?:32|64))|a64_vld16)\(")
FF_STLR = re.compile(r"\brecomp_(?:store|stp)\d+(?:_sp)?\(c,c->x\[\d+\],")
FF_FRAME = re.compile(r"\brecomp_(store8|store16|store32|store64|stp32|stp64)\(c,_ffp")


def ff_stores():
    """A64_FASTFP_STORES: guest stores may sit inside a region (on unless "0")"""
    return os.environ.get("A64_FASTFP_STORES", "1") != "0"


def ff_args(text, at):
    """the top-level arguments of the call whose "(" is at text[at], and the index after its ")"; None if unbalanced"""
    depth, args, start = 0, [], at + 1
    for j in range(at, len(text)):
        ch = text[j]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                args.append(text[start:j].strip())
                return args, j + 1
        elif ch == "," and depth == 1:
            args.append(text[start:j].strip())
            start = j + 1
    return None


def ff_store_ok(text):
    """a text with guest stores may join a region (ff_stores on): plain stores, each a whole statement of its own that
    always runs, no load after the first one"""
    if FF_STLR.search(text) or MG_NOT_STRAIGHT.search(text):
        return False
    first = None
    for m in FF_STORE.finditer(text):
        first = m.start() if first is None else first
        p = ff_args(text, m.end() - 1)
        if p is None or p[0][0] != "c" or len(p[0]) != (4 if m.group(1).startswith("stp") else 3):
            return False
        if not text[p[1]:].lstrip().startswith(";") or text[:m.start()].rstrip()[-1:] not in ("", ";", "{", "}"):
            return False
    return first is None or not FF_LOAD.search(text, first)


def ff_vmoves():
    """A64_FASTFP_VMOVES: nx2/fastvec.py's vector moves and shuffles (FF_VMOVES) may sit inside a region (on unless "0")"""
    return os.environ.get("A64_FASTFP_VMOVES", "1") != "0"


def ff_eligible(text, stores=None):
    """an instruction's C text may sit inside a fast float region: no control flow or system state; only pure calls, and
    guest stores when stores (ff_stores() unless given) and ff_store_ok"""
    rest = FF_FP_OK.sub("", text)
    if any(b in rest for b in FF_BAD):
        return False
    names = FF_CALL.findall(text)
    pure = FF_PURE | FF_VMOVES if ff_vmoves() else FF_PURE
    if all(n in pure or n.startswith(("a64_fpx_", "a64_vfpx_")) for n in names):
        return True
    if not (ff_stores() if stores is None else stores):
        return False
    return all(n in pure or n.startswith(("a64_fpx_", "a64_vfpx_")) or FF_STORE.match(n + "(") for n in names) and \
        ff_store_ok(text)


def ff_split(items):
    """a run cut into the parts a region may hold: a new part before each instruction with a guest load that follows a
    guest store of the part so far (no load after a store inside a region, so the stores can wait for its end)"""
    parts, cur, stored = [], [], False
    for item in items:
        t = item[1]
        if stored and ("recomp_l" in t or "a64_vld16" in t) and FF_LOAD.search(t):
            parts.append(cur)
            cur, stored = [], False
        cur.append(item)
        if "recomp_st" in t and FF_STORE.search(t):
            stored = True
    if cur:
        parts.append(cur)
    return parts


def ff_defer(text, k, record=False):
    """one instruction's text with its guest stores written as assignments of their address and values to the region's
    locals, numbered from k: (the text, [(helper, locals' prefix, number, pair)]). record: the stores kept, each after its
    assignment (the check copy's exact text, A64_FASTFP=check)"""
    if "recomp_st" not in text:
        return text, []
    out, stores, at = [], [], 0
    for m in FF_STORE.finditer(text):
        if m.start() < at:
            continue
        args, end = ff_args(text, m.end() - 1)
        name = "recomp_%s%s" % (m.group(1), m.group(2) or "")
        frame = not m.group(2) and (FP_STORE.match(text, m.start()) is not None or
                                    any(p.end(1) == m.start() for p in FP_STORE_PRE.finditer(text)))
        pre = ("_ffp" if frame else "_ff") + ("o" if record else "")
        vals = ["%sa%d=(%s)" % (pre, k, args[1]), "%sd%d=(%s)" % (pre, k, args[2])]
        if len(args) == 4:
            vals.append("%se%d=(%s)" % (pre, k, args[3]))
        if record:
            vals.append("%s(c,%sa%d,%sd%d%s)" % (name, pre, k, pre, k, ",%se%d" % (pre, k) if len(args) == 4 else ""))
        out.append(text[at:m.start()] + "(" + ",".join(vals) + ")")
        stores.append((name, pre, k, len(args) == 4))
        at, k = end, k + 1
    out.append(text[at:])
    return "".join(out), stores


def ff_frame_stores(text):
    """the deferred stores through x29 (their locals named _ffp...) as frame_stores makes their exact copies: _sp, unmarked.
    Run where emit_function runs frame_stores"""
    return FF_FRAME.sub(r"recomp_\1_sp(c,_ffp", text) if "(c,_ffp" in text else text


_patched_addrs = [None]   # (set_patched: nx2/oot_patches.py's mentioned_addresses, asked once a process)
_patched = []             # (those addresses: [sorted list, set]; made on first use)


def ff_patched(e, body, pcs):
    """the function at e (its body, pcs: its addresses in order) holds an address the game's patches name, its entry or
    an instruction's, as nx2/guest_locals.py's keep_mentioned finds them (nx2/oot_patches.py: an edit of the lifted text
    pins its place by one). No stores join its runs, so that an edit's text, a store among it, stays one text the edit
    finds in a copy that runs (a store's fast copy is rewritten: an edit there would miss it)"""
    if not _patched:
        addrs = sorted(_patched_addrs[0]()) if _patched_addrs[0] else []
        _patched[:] = [addrs, set(addrs)]
    addrs, aset = _patched
    if not addrs or not pcs:
        return False
    if e in aset:
        return True
    import bisect
    for i in range(bisect.bisect_left(addrs, pcs[0]), bisect.bisect_right(addrs, pcs[-1])):
        if addrs[i] in body:
            return True
    return False


def ff_regions(items, least=None, stores=None):
    """a run's C text, cut by ff_split and each part through ff_region: (text, regions made, float ops made fast). stores:
    the run may hold guest stores (ff_stores() unless given; emit_function: not in a function the game's patches edit)"""
    least = ff_min() if least is None else least
    if not (ff_stores() if stores is None else stores) or len(items) < 2 or \
            not any("recomp_st" in t for _, t in items) or sum(ff_convs(t) for _, t in items) < least:
        text, n = ff_region(items, least)   # (no store, or too few ops for any part of it: as before)
        return text, int(n > 0), n
    texts, regions, ops = [], 0, 0
    for part in ff_split(items):
        text, n = ff_region(part, least)
        texts.append(text)
        regions, ops = regions + (n > 0), ops + n
    return "\n".join(texts), regions, ops


# (the emit asks these of every instruction of every function, ~13 million times in a lift of OoT, but there are far fewer
# distinct instruction texts: each answer is kept by its text. Before, ff_eligible ran its FF_FP_OK.sub once for each of
# FF_BAD's 13 words, on every instruction, and emit and write took 106 s where it took ~46 s without the regions)
_joins = {}
_convs = {}


def ff_joins(text, dead, stores=False):
    """ff_eligible of an instruction's text with the flag stores dead there (dead_flags' mask) dropped, by (text, dead,
    stores)"""
    j = _joins.get((text, dead, stores))
    if j is None:
        j = _joins[(text, dead, stores)] = ff_eligible(drop_flag_stores(text, dead) if dead else text, stores)
    return j


def ff_convs(text):
    """the float ops ff_region can make fast in one instruction's text (FF_CONV's matches)"""
    n = _convs.get(text)
    if n is None:
        n = _convs[text] = len(FF_CONV.findall(text))
    return n


def ff_region(items, least=None):
    """a run's C text ([(pc, text)], every text ff_eligible): the fast and exact copies, or the exact text alone when the
    run has too few float ops to pay for its check. (text, number of ops made fast). least: ff_min(), read once by the
    caller (emit_function: once a function, not for each of the 3.8 million runs of OoT)"""
    exact = "\n".join(t for _, t in items)
    # (FF_CONV's matches are in one instruction's text each, so the run's count is their sum: most runs are a few
    # instructions with no float op, and only a run with enough of them is searched again below)
    n = sum(ff_convs(t) for _, t in items)
    if n < (ff_min() if least is None else least):
        return exact, 0
    # (guest stores, FF_STORE: the fast copy's assign their address and values to locals, stored once a64_ff_end passed)
    stores = []
    if "recomp_st" in exact:
        fast_items = []
        for _, t in items:
            t, s = ff_defer(t, len(stores))
            fast_items.append(t)
            stores += s
        fast = "\n".join(fast_items)
    else:
        fast = exact
    fast = FF_CONV.sub(lambda m: "a64_ff_v%s(" % m.group(1) if m.group(1) else "a64_ff_%s(" % m.group(2), fast)
    if "a64_vfpx_" in fast:   # (nx2/vecfp.py's lane forms: their region twins, one SSE op each; their lanes go dead)
        fast = FF_VX.sub(r"a64_ff_\1(", fast)
    xs = sorted({int(r) for r in REG_REF.findall(exact)})
    vs = sorted({int(r) for r in VREG_REF.findall(exact)})
    fl = sorted(set(FLAG_REF.findall(exact)))
    save = ("".join("const uint64_t _ffx%d = c->x[%d]; " % (r, r) for r in xs) +
            "".join("const uint64_t _ffv%d0 = c->vreg[%d][0], _ffv%d1 = c->vreg[%d][1]; " % (r, r, r, r) for r in vs) +
            "".join("const uint8_t _fff%s = c->%s; " % (f, f) for f in fl) + "const uint64_t _ffsr = c->fpsr; ")
    restore = ("".join("c->x[%d] = _ffx%d; " % (r, r) for r in xs) +
               "".join("c->vreg[%d][0] = _ffv%d0; c->vreg[%d][1] = _ffv%d1; " % (r, r, r, r) for r in vs) +
               "".join("c->%s = _fff%s; " % (f, f) for f in fl) + "c->fpsr = _ffsr; ")
    label = "L_ff_%08x" % items[0][0]
    # (the stores' locals, declared with the saved registers; and the stores, run once the fast copy is kept)
    temps = "".join("uint64_t %sa%d, %sd%d%s; " % (p, k, p, k, ", %se%d" % (p, k) if pair else "")
                    for _, p, k, pair in stores)
    deferred = "".join("%s(c,%sa%d,%sd%d%s); " % (h, p, k, p, k, ",%se%d" % (p, k) if pair else "")
                       for h, p, k, pair in stores)
    if fastfp_mode() == "check":
        # (A64_FASTFP=check: both copies every time, the exact one's result kept, the registers the run names compared,
        # and each store's address and values (the exact copy's noted as it stores, _ffo...); a64_ff_check counts and
        # prints the first mismatches with the run's guest address. The fast copy stores nothing here)
        keep = ("".join("const uint64_t _ffox%d = c->x[%d]; " % (r, r) for r in xs) +
                "".join("const uint64_t _ffov%d0 = c->vreg[%d][0], _ffov%d1 = c->vreg[%d][1]; " % (r, r, r, r) for r in vs) +
                "".join("const uint8_t _ffof%s = c->%s; " % (f, f) for f in fl))
        same = " && ".join(["1"] + ["_ffox%d == c->x[%d]" % (r, r) for r in xs] +
                           ["_ffov%d0 == c->vreg[%d][0] && _ffov%d1 == c->vreg[%d][1]" % (r, r, r, r) for r in vs] +
                           ["_ffof%s == c->%s" % (f, f) for f in fl])
        checked = exact
        if stores:
            checked, k = [], 0
            for _, t in items:
                t, s = ff_defer(t, k, record=True)
                checked.append(t)
                k += len(s)
            checked = "\n".join(checked)
            temps += "".join("uint64_t %soa%d, %sod%d%s; " % (p, k, p, k, ", %soe%d" % (p, k) if pair else "")
                             for _, p, k, pair in stores)
            for h, p, k, pair in stores:
                ty = "uint%d_t" % FF_STORE_SIZE[h[len("recomp_"):].replace("_sp", "")]
                same += " && %sa%d == %soa%d && (%s)%sd%d == (%s)%sod%d" % (p, k, p, k, ty, p, k, ty, p, k)
                if pair:
                    same += " && (%s)%se%d == (%s)%soe%d" % (ty, p, k, ty, p, k)
        # (the "}" of its else not at the start of a line: one there closes the function, nx2/split_giants.py)
        return ("{ if (A64_FF_ON(c)) { %s%sconst unsigned _ffm = a64_ff_begin(c);\n%s\n"
                "const int _ffok = a64_ff_end(c, _ffm);\n%s%s\n%s\na64_ff_check(0x%xu, _ffok, %s); } else {\n%s\n    } }"
                % (save, temps, fast, keep, restore, checked, items[0][0], same, exact)), n
    if stores:
        return ("{ if (A64_FF_ON(c)) { %s%sconst unsigned _ffm = a64_ff_begin(c);\n%s\n"
                "if (RECOMP_LIKELY(a64_ff_end(c, _ffm))) { %sgoto %s; }\n%s}\n%s\n%s:; }"
                % (save, temps, fast, deferred, label, restore, exact, label)), n
    return ("{ if (A64_FF_ON(c)) { %sconst unsigned _ffm = a64_ff_begin(c);\n%s\n"
            "if (RECOMP_LIKELY(a64_ff_end(c, _ffm))) goto %s;\n%s}\n%s\n%s:; }"
            % (save, fast, label, restore, exact, label)), n


FASTFP_HEADER = r"""
/* ---- (longshot) fast float regions (A64_FASTFP, ff_region in a64_lift.py) ---- */
#if RECOMP_FPX_HOST == 2 && defined(__x86_64__) && !defined(__EMSCRIPTEN__)
#include <stdlib.h>
#include <stdio.h>
static inline __attribute__((always_inline)) int a64_ff_enabled_(void) { static int v = -1; if (v < 0) v = !getenv("HOS_NO_FASTFP"); return v; }
#define A64_FF_ON(c) (RECOMP_FPX_OPEN(c) && a64_ff_enabled_())
/* MXCSR: exceptions masked, round to nearest, DAZ off, FTZ as FPCR.FZ, flags cleared; the old value returned */
static inline __attribute__((always_inline)) unsigned a64_ff_begin(const Ctx *c) {
    unsigned m, n;
    __asm__ volatile("stmxcsr %0" : "=m"(m));
    n = (m & ~0xE07Fu) | 0x1F80u | (RECOMP_FPX_FZ(c) ? 0x8000u : 0u);
    __asm__ volatile("ldmxcsr %0" : : "m"(n));
    return m;
}
/* 0: an invalid, denormal or underflow flag (the exact copy runs); else inexact, overflow, divide-by-zero to FPSR */
static inline __attribute__((always_inline)) int a64_ff_end(Ctx *c, unsigned m) {
    unsigned f;
    __asm__ volatile("stmxcsr %0" : "=m"(f));
    __asm__ volatile("ldmxcsr %0" : : "m"(m));
    if (f & 0x13u) return 0;
    c->fpsr |= (f & 0x20u ? 0x10u : 0u) | (f & 0x08u ? 0x04u : 0u) | (f & 0x04u ? 0x02u : 0u);
    return 1;
}
/* (asm volatile: kept inside the MXCSR window and in Arm's operand order, see ff_region) */
#ifdef __AVX__
#define A64_FF_OP(ins, d, a, b) __asm__ volatile("v" ins " %2, %1, %0" : "=x"(d) : "x"(a), "x"(b))
#define A64_FF_SQRT(d, a) __asm__ volatile("vsqrtss %1, %1, %0" : "=x"(d) : "x"(a))
#define A64_FF_SPOIL "vcomiss %0, %0"
#else
#define A64_FF_OP(ins, d, a, b) do { (d) = (a); __asm__ volatile(ins " %1, %0" : "+x"(d) : "x"(b)); } while (0)
#define A64_FF_SQRT(d, a) __asm__ volatile("sqrtss %1, %0" : "=x"(d) : "x"(a))
#define A64_FF_SPOIL "comiss %0, %0"
#endif
/* The invalid flag raised on purpose (a signalling compare of a NaN), sending the run to its exact copy, for two results
   whose value x86 gets right but not Arm's flags, or under FZ not Arm's value (found by tests/test_fastfp_fma.py):
   - a NaN: x86 raises no denormal flag for a subnormal next to a NaN operand, where Arm under FZ flushes it and sets
     IDC; and a multiply-add's 0 * inf + a quiet NaN gives that NaN with no flag (Arm: the default NaN, IOC);
   - exactly 2^-126, the smallest normal, from a multiply, divide or multiply-add: x86 detects tininess after rounding,
     Arm before, so a result rounded up to 2^-126 sets UFC on Arm, and is a zero under FZ. (A sum this small is exact.)
   The tests are integer compares on the bits: a float compare would raise the denormal flag on a subnormal result. */
static inline __attribute__((always_inline)) void a64_ff_spoil_(void) {
    __asm__ volatile(A64_FF_SPOIL : : "x"(_mm_castsi128_ps(_mm_cvtsi32_si128(0x7fc00000))) : "cc");
}
static inline __attribute__((always_inline)) void a64_ff_guard_(uint64_t v, int tiny) {
    const uint32_t m = (uint32_t)v & 0x7fffffffu;
    if (__builtin_expect(m > 0x7f800000u || (tiny && m == 0x00800000u), 0)) a64_ff_spoil_();
}
static inline __attribute__((always_inline)) void a64_ff_vguard_(__m128 r, int k, int tiny) {   /* (the k lanes kept) */
    const __m128i m = _mm_and_si128(_mm_castps_si128(r), _mm_set1_epi32(0x7fffffff));
    __m128i bad = _mm_cmpgt_epi32(m, _mm_set1_epi32(0x7f800000));
    if (tiny) bad = _mm_or_si128(bad, _mm_cmpeq_epi32(m, _mm_set1_epi32(0x00800000)));
    if (__builtin_expect(_mm_movemask_ps(_mm_castsi128_ps(bad)) & (k == 4 ? 15 : 3), 0)) a64_ff_spoil_();
}
/* (2 lanes: the upper two are loaded as zeros, so they raise no flag of their own) */
static inline __attribute__((always_inline)) __m128 a64_ff_vld(const uint64_t *s, int k) {
    return k == 4 ? _mm_loadu_ps((const float *)s) : _mm_castsi128_ps(_mm_loadl_epi64((const __m128i *)s));
}
static inline __attribute__((always_inline)) void a64_ff_vst(uint64_t *d, __m128 r, int k) {
    if (k == 4) _mm_storeu_ps((float *)d, r); else _mm_storeu_si128((__m128i *)d, _mm_move_epi64(_mm_castps_si128(r)));
}
#define A64_FF_V(name, ins, tiny) \
static inline __attribute__((always_inline)) void a64_ff_v##name(Ctx *c, uint64_t *d, const uint64_t *n, const uint64_t *m, int k) { \
    __m128 r; (void)c; A64_FF_OP(ins, r, a64_ff_vld(n, k), a64_ff_vld(m, k)); \
    a64_ff_vguard_(r, k, tiny); a64_ff_vst(d, r, k); }
A64_FF_V(add32, "addps", 0)
A64_FF_V(sub32, "subps", 0)
A64_FF_V(mul32, "mulps", 1)
static inline __attribute__((always_inline)) __m128 a64_ff_ss(uint64_t a) { return _mm_castsi128_ps(_mm_cvtsi32_si128((int)(uint32_t)a)); }
static inline __attribute__((always_inline)) uint64_t a64_ff_bits(__m128 r) { return (uint32_t)_mm_cvtsi128_si32(_mm_castps_si128(r)); }
#define A64_FF_S(name, ins, tiny) \
static inline __attribute__((always_inline)) uint64_t a64_ff_##name(Ctx *c, uint64_t a, uint64_t b) { \
    __m128 r; (void)c; A64_FF_OP(ins, r, a64_ff_ss(a), a64_ff_ss(b)); a64_ff_guard_(a64_ff_bits(r), tiny); return a64_ff_bits(r); }
A64_FF_S(add32_add32, "addss", 0)
A64_FF_S(sub32_sub32, "subss", 0)
/* (A subnormal over a zero also takes the exact copy: x86 raises no denormal flag beside divide-by-zero and gives an
   infinity, where Arm under FZ flushes the subnormal first, 0 / 0: the default NaN and IOC.) */
static inline __attribute__((always_inline)) uint64_t a64_ff_div32_div32(Ctx *c, uint64_t a, uint64_t b) {
    __m128 r;
    (void)c;
    if (__builtin_expect(((uint32_t)b << 1) == 0 && ((uint32_t)a << 1) - 1u < 0x00ffffffu, 0)) a64_ff_spoil_();
    A64_FF_OP("divss", r, a64_ff_ss(a), a64_ff_ss(b));
    a64_ff_guard_(a64_ff_bits(r), 1);
    return a64_ff_bits(r);
}
A64_FF_S(mul32_, "mulss", 1)
static inline __attribute__((always_inline)) uint64_t a64_ff_mul32_fma32(Ctx *c, uint64_t a, uint64_t b, uint64_t z) { (void)z; return a64_ff_mul32_(c, a, b); }
static inline __attribute__((always_inline)) uint64_t a64_ff_sqrt32_sqrt32(Ctx *c, uint64_t a) { __m128 r; (void)c; A64_FF_SQRT(r, a64_ff_ss(a)); return a64_ff_bits(r); }
/* (longshot) FMADD/FMSUB/FNMADD/FNMSUB and FMLA/FMLS S lanes: a64_fpx_fma32_fma32(c, a, b, z) is a*b + z, the lifted
   code having already flipped the signs the op negates. FMA3's vfmadd231ss rounds once, as Arm does, and takes its
   NaN in Arm's order (the addend, then a, then b). Two cases differ, and both raise the invalid flag here (a64_ff_guard_),
   which sends the run to its exact copy:
   - 0 * inf + a quiet NaN: x86 returns that NaN with no flag, Arm the default NaN with IOC (so: any NaN result);
   - a result rounded up to the smallest normal, 2^-126: x86 detects tininess after rounding and raises no underflow,
     Arm detects it before, sets UFC, and under FZ returns a zero.
   Without FMA3 (x86-64-v2), the exact helper. */
#ifdef __FMA__
static inline __attribute__((always_inline)) uint64_t a64_ff_fma32_fma32(Ctx *c, uint64_t a, uint64_t b, uint64_t z) {
    __m128 r = a64_ff_ss(z);
    (void)c;
    __asm__ volatile("vfmadd231ss %2, %1, %0" : "+x"(r) : "x"(a64_ff_ss(a)), "x"(a64_ff_ss(b)));
    a64_ff_guard_(a64_ff_bits(r), 1);
    return a64_ff_bits(r);
}
#else
#define a64_ff_fma32_fma32 a64_fpx_fma32_fma32
#endif
/* (A64_FASTFP=check) a run's result: counts per file, and the first mismatches of a clean fast run (which must not
   happen) with the run's guest address, on stderr (the game's log) */
static volatile long long a64_ff_n_[3];   /* runs, clean, mismatched when clean */
static __attribute__((noinline, cold)) void a64_ff_check_print_(unsigned pc, long long bad) {
    if (bad < 16) fprintf(stderr, "fastfp check: MISMATCH in the run at %#x (%s)\n", pc, __BASE_FILE__);
}
static inline __attribute__((always_inline)) void a64_ff_check(unsigned pc, int clean, int same) {
    const long long n = __atomic_add_fetch(&a64_ff_n_[0], 1, __ATOMIC_RELAXED);
    if (clean) __atomic_add_fetch(&a64_ff_n_[1], 1, __ATOMIC_RELAXED);
    if (clean && !same) a64_ff_check_print_(pc, __atomic_fetch_add(&a64_ff_n_[2], 1, __ATOMIC_RELAXED));
    if (!(n & ((1LL << 22) - 1)))
        fprintf(stderr, "fastfp check: %s: %lld runs, %lld clean, %lld mismatched when clean\n", __BASE_FILE__, n, a64_ff_n_[1], a64_ff_n_[2]);
}
#else
/* (no x86 MXCSR here: the fast copies are never run, and name the exact helpers) */
#define A64_FF_ON(c) 0
#define a64_ff_begin(c) 0u
#define a64_ff_end(c, m) 1
#define a64_ff_vadd32 a64_vfpx_add32
#define a64_ff_vsub32 a64_vfpx_sub32
#define a64_ff_vmul32 a64_vfpx_mul32
#define a64_ff_add32_add32 a64_fpx_add32_add32
#define a64_ff_sub32_sub32 a64_fpx_sub32_sub32
#define a64_ff_mul32_fma32 a64_fpx_mul32_fma32
#define a64_ff_fma32_fma32 a64_fpx_fma32_fma32
#define a64_ff_div32_div32 a64_fpx_div32_div32
#define a64_ff_sqrt32_sqrt32 a64_fpx_sqrt32_sqrt32
#define a64_ff_check(pc, clean, same) ((void)0)
#endif
"""

# (longshot) nx2/vecfp.py's lane forms' region twins (FF_VX), appended to a64_fpx.h after FASTFP_HEADER only when vecfp is on
# (vecfp_on): with A64_VECFP=0 the header is main's to the byte. They use FASTFP_HEADER's helpers (a64_ff_vld, the guard)
FF_VX_HEADER = r"""
#if RECOMP_FPX_HOST == 2 && defined(__x86_64__) && !defined(__EMSCRIPTEN__)
/* (longshot) nx2/vecfp.py's lane forms in a region: one SSE op each, guarded as the others (a NaN lane, or one exactly
   2^-126 from a product, quotient or multiply-add, raises the invalid flag), returning 1 so that the lanes the lifted C
   keeps behind them are dead code here. By element: the element broadcast; FMLS: the first operand's sign flipped, as the
   lanes do. 2 lanes: the inputs' top halves are loaded as zeros (a64_ff_vld) and a divisor's as ones, so that they raise
   no flag of their own; the result's top half is zero. Without FMA3 the fused forms answer 0: their lanes run (the region's
   own scalar helpers, a64_ff_fma32_fma32 = the exact helper there) */
static inline __attribute__((always_inline)) __m128 a64_ff_vlane(const void *p) { float x; memcpy(&x, p, 4); return _mm_set1_ps(x); }
static inline __attribute__((always_inline)) int a64_ff_vmul_(uint64_t *d, __m128 a, __m128 b, int k) {
    __m128 r; A64_FF_OP("mulps", r, a, b); a64_ff_vguard_(r, k, 1); a64_ff_vst(d, r, k); return 1; }
static inline __attribute__((always_inline)) int a64_ff_mul32e(Ctx *c, uint64_t *d, const uint64_t *n, const void *m, int k) {
    (void)c; return a64_ff_vmul_(d, a64_ff_vld(n, k), k == 4 ? a64_ff_vlane(m) : _mm_castsi128_ps(_mm_move_epi64(_mm_castps_si128(a64_ff_vlane(m)))), k); }
static inline __attribute__((always_inline)) int a64_ff_mul32v(Ctx *c, uint64_t *d, const uint64_t *n, const uint64_t *m, int k) {
    (void)c; return a64_ff_vmul_(d, a64_ff_vld(n, k), a64_ff_vld(m, k), k); }
static inline __attribute__((always_inline)) __m128 a64_ff_vneg(__m128 a) { return _mm_castsi128_ps(_mm_xor_si128(_mm_castps_si128(a), _mm_set1_epi32((int)0x80000000u))); }
#ifdef __FMA__
static inline __attribute__((always_inline)) int a64_ff_vfma_(uint64_t *d, __m128 a, __m128 b, int k) {
    __m128 r = a64_ff_vld(d, k);
    __asm__ volatile("vfmadd231ps %2, %1, %0" : "+x"(r) : "x"(a), "x"(b));
    a64_ff_vguard_(r, k, 1); a64_ff_vst(d, r, k); return 1; }
static inline __attribute__((always_inline)) __m128 a64_ff_vlane_k(const void *m, int k) {
    return k == 4 ? a64_ff_vlane(m) : _mm_castsi128_ps(_mm_move_epi64(_mm_castps_si128(a64_ff_vlane(m)))); }
static inline __attribute__((always_inline)) int a64_ff_fma32e(Ctx *c, uint64_t *d, const uint64_t *n, const void *m, int k) {
    (void)c; return a64_ff_vfma_(d, a64_ff_vld(n, k), a64_ff_vlane_k(m, k), k); }
static inline __attribute__((always_inline)) int a64_ff_fms32e(Ctx *c, uint64_t *d, const uint64_t *n, const void *m, int k) {
    (void)c; return a64_ff_vfma_(d, a64_ff_vneg(a64_ff_vld(n, k)), a64_ff_vlane_k(m, k), k); }
static inline __attribute__((always_inline)) int a64_ff_fma32v(Ctx *c, uint64_t *d, const uint64_t *n, const uint64_t *m, int k) {
    (void)c; return a64_ff_vfma_(d, a64_ff_vld(n, k), a64_ff_vld(m, k), k); }
static inline __attribute__((always_inline)) int a64_ff_fms32v(Ctx *c, uint64_t *d, const uint64_t *n, const uint64_t *m, int k) {
    (void)c; return a64_ff_vfma_(d, a64_ff_vneg(a64_ff_vld(n, k)), a64_ff_vld(m, k), k); }
#else
#define a64_ff_fma32e(c, ...) 0
#define a64_ff_fms32e(c, ...) 0
#define a64_ff_fma32v(c, ...) 0
#define a64_ff_fms32v(c, ...) 0
#endif
/* (a subnormal over a zero, as a64_ff_div32_div32: x86 raises no denormal flag beside divide-by-zero, Arm under FZ
   flushes it, 0 / 0: such a lane raises the invalid flag here) */
static inline __attribute__((always_inline)) int a64_ff_div32v(Ctx *c, uint64_t *d, const uint64_t *n, const uint64_t *m, int k) {
    const __m128 a = a64_ff_vld(n, k);
    __m128 b = a64_ff_vld(m, k), r;
    (void)c;
    if (k != 4) b = _mm_or_ps(b, _mm_castsi128_ps(_mm_set_epi32(0x3f800000, 0x3f800000, 0, 0)));
    const __m128i ai = _mm_castps_si128(a), bi = _mm_castps_si128(b);
    const __m128i bz = _mm_cmpeq_epi32(_mm_slli_epi32(bi, 1), _mm_setzero_si128());
    const __m128i as = _mm_cmplt_epi32(_mm_sub_epi32(_mm_slli_epi32(ai, 1), _mm_set1_epi32(1)), _mm_set1_epi32(0x00ffffff));
    const __m128i ap = _mm_cmpgt_epi32(_mm_slli_epi32(ai, 1), _mm_setzero_si128());   /* ((a << 1) - 1 in 0 .. 0xfffffe: a subnormal) */
    if (__builtin_expect(_mm_movemask_ps(_mm_castsi128_ps(_mm_and_si128(bz, _mm_and_si128(as, ap)))) & (k == 4 ? 15 : 3), 0)) a64_ff_spoil_();
    A64_FF_OP("divps", r, a, b);
    a64_ff_vguard_(r, k, 1);
    a64_ff_vst(d, r, k);
    return 1;
}
#else
#define a64_ff_mul32e a64_vfpx_mul32e
#define a64_ff_fma32e a64_vfpx_fma32e
#define a64_ff_fms32e a64_vfpx_fms32e
#define a64_ff_mul32v a64_vfpx_mul32v
#define a64_ff_fma32v a64_vfpx_fma32v
#define a64_ff_fms32v a64_vfpx_fms32v
#define a64_ff_div32v a64_vfpx_div32v
#endif
"""


def vecfp_on():
    """nx2/vecfp.py is installed and A64_VECFP turns a group on: the lifted C may name the twins"""
    import vecfp
    return vecfp.installed() and bool(vecfp.groups())



_ff_only = []


def ff_only():
    """A64_FASTFP=hot: the names of the hot functions (A64_HOT_FUNCS), whose runs alone are made regions; else None. Read here
    rather than kept on the lifter, so the lift's new processes, which get only its tables, decide as the lift does"""
    if not _ff_only:
        _ff_only.append(a64_lift.hot_functions() if fastfp_mode() == "hot" else None)
    return _ff_only[0]


# ---- the kit's Lifter.emit_function, with the run gathering (copied at KIT_TREE: see the module's note) ----
def emit_function(self, e, texts):
    """C text of one function, and the set of C names it calls."""
    body, labels = self.funcs[e]
    self.cur = e
    self.refs = set()
    out = ["void f_%08x(Ctx *__restrict A64_NOESC c) {" % e]
    if e in self.names and self.comments:
        out[0] = "/* %s */\n" % self.names[e] + out[0]
    if e in self.plt:   # a PLT stub reached indirectly: x16 = GOT slot, x17 = target, as the stub leaves them
        t, slot = self.plt[e]
        name = self.fname(self.callee(t))
        self.refs.add(name)
        out.append("c->x[16] = 0x%xULL; c->x[17] = 0x%xULL; TAILCALL(%s);\n}" % (slot, t, name))
        return "\n".join(out) + "\n", self.refs
    pcs = sorted(body)
    dead = {} if os.environ.get("A64_KEEP_FLAGS") else self.dead_flags(body, pcs, texts)
    frame = not os.environ.get("A64_MARK_FRAME")
    local = local_regs() and e not in self.no_local
    dirty = self.dirty_regs(body, pcs, texts) if local else None
    # (the run of joinable instructions so far, [(pc, text)]: ff_region)  # (fastfp)
    ff = bool(fastfp_mode()) and (ff_only() is None or self.fname(e) in ff_only())  # (fastfp)
    least = ff_min()  # (fastfp)
    # (guest stores may join a run (A64_FASTFP_STORES), but not in a function the game's patches edit: ff_patched)  # (fastfp)
    stores = ff and ff_stores() and not ff_patched(e, body, pcs)  # (fastfp)
    run = []  # (fastfp)
    def flush():  # (fastfp)
        if run:  # (fastfp)
            text, k, n = ff_regions(run, least, stores)  # (fastfp)
            out.append(text)  # (fastfp)
            if n:  # (fastfp)
                self.stats["ff_regions"] = self.stats.get("ff_regions", 0) + k  # (fastfp)
                self.stats["ff_ops"] = self.stats.get("ff_ops", 0) + n  # (fastfp)
            del run[:]  # (fastfp)
    for i, pc in enumerate(pcs):
        self.cur_i = i
        (k, w) = body[pc]
        kind = k[0]
        # (an instruction joins the run when nothing can enter or leave between it and the next: no label, no patch after  # (fastfp)
        # it, an op a64emit handled, whose text ff_eligible takes; what it appends below goes to the run instead)  # (fastfp)
        joins = False  # (fastfp)
        if ff and kind == "op" and pc not in self.after and not (pc in labels or (pc == e and e in labels)):  # (fastfp)
            t, f = texts[(w, pc) if pc_relative(w) else w]  # (fastfp)
            if not f & (a64emit.UNHANDLED | a64emit.TERMINATOR):  # (fastfp)
                joins = ff_joins(t, dead.get(pc, 0), stores)  # (fastfp)
        if not joins:  # (fastfp)
            flush()  # (fastfp)
        mark = len(out)  # (fastfp)
        if pc in labels or (pc == e and e in labels):
            out.append("L_%08x:;" % pc)
        if self.comments:
            out.append("/* %08x: %08x */" % (pc, w))
        nxt = pc + 4
        falls = True
        if kind == "op":
            text, flags = texts[(w, pc) if pc_relative(w) else w]
            if flags & a64emit.UNHANDLED:
                self.unhandled[w] += 1
                out.append(SYNC_ALL % i + "a64_trap(c, 0x%xULL, 0x%08xu); return;" % (pc, w))
                falls = False
            elif flags & a64emit.TERMINATOR:
                raise SystemExit("a64emit closed a block at %#x (%08x): classify() must handle it" % (pc, w))
            else:
                if frame and not fp_is_frame(text):
                    frame = False
                if pc in dead:
                    text = drop_flag_stores(text, dead[pc])
                    self.stats["dead_flag_stores"] += bin(dead[pc]).count("1")
                out.append(text.rstrip("\n"))
                if pc in self.after:
                    out.append(self.after[pc])
                    self.stats["after_patches"] = self.stats.get("after_patches", 0) + 1
        elif kind == "b":
            out.append(self.transfer(pc, k[1], labels, body))
            falls = False
        elif kind == "bcond":
            out.append("if %s { %s }" % (COND[k[2]], self.transfer(pc, k[1], labels, body)))
        elif kind == "cbz":
            _, t, rt, sf, nz = k
            v = "0" if rt == 31 else ("c->x[%d]" % rt if sf else "(uint32_t)c->x[%d]" % rt)
            out.append("if (%s %s 0) { %s }" % (v, "!=" if nz else "==", self.transfer(pc, t, labels, body)))
        elif kind == "tbz":
            _, t, rt, bit, nz = k
            v = "0" if rt == 31 else "((c->x[%d] >> %d) & 1)" % (rt, bit)
            out.append("if (%s%s) { %s }" % ("" if nz else "!", v, self.transfer(pc, t, labels, body)))
        elif kind == "bl":
            t = self.callee(k[1])
            if t in self.entries and t in getattr(self, "setjmp_owner", {}):
                # (hookshot) a call of a function that calls __setjmp: this frame takes a host setjmp. A guest
                # longjmp's BR to one of the owner's setjmp return addresses comes back here (host/hos_setjmp.c),
                # with the registers it restored, and runs the owner's code from there, as the owner would.
                # (recomp_sj_setjmp, not __builtin_setjmp: on Win64 clang's frame pointer sits above the frame
                # address the builtin saves, and its longjmp restored rbp to that, so the frame's spill slots
                # were read 128 bytes off: c and _sj came back as garbage)
                rets = (self.setjmp_owner[t] + [0, 0, 0, 0])[:4]
                out.append("c->x[30] = 0x%xULL; %s{ void **_sj = recomp_sj_push(c, 0x%xULL, 0x%xULL, 0x%xULL, 0x%xULL); "
                           "if (recomp_sj_setjmp(_sj) == 0) %s(c); else a64_icall(c, recomp_sj_target(_sj)); "
                           "recomp_sj_pop(_sj); }%s"
                           % (nxt, SYNC_CALL % i, rets[0], rets[1], rets[2], rets[3], self.fname(t), SYNC_BACK % i))
                self.refs.add(self.fname(t))
                self.stats["setjmp_call_sites"] = self.stats.get("setjmp_call_sites", 0) + 1
            elif t in self.entries:
                out.append("c->x[30] = 0x%xULL; %s%s(c);%s" % (nxt, SYNC_CALL % i, self.fname(t), SYNC_BACK % i))
                self.refs.add(self.fname(t))
            else:
                self.stats["bad_target"] += 1
                out.append(SYNC_ALL % i + "a64_trap(c, 0x%xULL, 0x%08xu); return;" % (pc, w))
        elif kind == "blr":
            usual = self.devirt.get(pc)
            if usual in self.entries:
                name = self.fname(self.callee(usual))
                self.refs.add(name)
                self.stats["devirt_sites"] += 1
                out.append("{ uint64_t _t = c->x[%d]; c->x[30] = 0x%xULL; %sif (RECOMP_LIKELY(_t == 0x%xULL)) %s(c); "
                           "else A64_ICALL(c, _t); %s}" % (k[1], nxt, SYNC_CALL % i, usual, name, SYNC_BACK % i))
            else:
                out.append("{ uint64_t _t = c->x[%d]; c->x[30] = 0x%xULL; %sA64_ICALL(c, _t); %s}"
                           % (k[1], nxt, SYNC_CALL % i, SYNC_BACK % i))
        elif kind == "ret":
            out.append(SYNC_RET % i + "return;")
            falls = False
        elif kind == "br":
            targets = self.jt.get(pc)
            if targets:
                cases = " ".join("case 0x%xULL: %s" % (t, self.transfer(pc, t, labels, body))
                                 for t in sorted(set(targets)))
                out.append("switch (c->x[%d]) { %s default: %sA64_IJUMP(c, c->x[%d]); return; }"
                           % (k[1], cases, SYNC_ALL % i, k[1]))
            else:
                self.ijump[pc] += 1
                out.append(SYNC_ALL % i + "A64_IJUMP(c, c->x[%d]); return;" % k[1])
            falls = False
        elif kind == "svc":
            out.append(SYNC_ALL % i + "a64_svc(c, %du);" % k[1] + SYNC_RELOAD % i)
        elif kind == "trap":
            out.append(SYNC_ALL % i + "a64_trap(c, 0x%xULL, 0x%08xu); return;" % (pc, w))
            falls = False
        elif kind == "isb":
            out.append("recomp_barrier();")
        elif kind == "ic":
            out.append("/* ic ivau: no self-modifying code */")
        if joins:  # (fastfp)
            run.extend((pc, t) for t in out[mark:])  # (fastfp)
            del out[mark:]  # (fastfp)
        if falls and (i + 1 == len(pcs) or pcs[i + 1] != nxt):  # (fastfp)
            flush()  # (fastfp)
        if falls and (i + 1 == len(pcs) or pcs[i + 1] != nxt):
            out.append(self.transfer(pc, nxt, labels, body))
    flush()  # (fastfp)
    out.append("}")
    text = "\n".join(out)
    if frame:
        text = frame_stores(text)
        text = ff_frame_stores(text)  # (fastfp)
    return localize_regs(text, local, dirty) + "\n", self.refs


_installed = False


def set_patched(addresses):
    """addresses(): the guest addresses the game's patches name (nx2/oot_patches.py's mentioned_addresses; ff_patched)"""
    _patched_addrs[0] = addresses
    del _patched[:]


def install():
    """emit_function above in place of the kit's, and Lifter.write wrapped (the header, the count). Once; before
    nx2/lift_fixes.py's and nx2/poll.py's install()"""
    global _installed
    if _installed:
        return
    _installed = True
    a64_lift.Lifter.emit_function = emit_function
    base_write = a64_lift.Lifter.write

    def write(self, out_dir, chunk_bytes=a64_lift.CHUNK_BYTES):
        files = base_write(self, out_dir, chunk_bytes=chunk_bytes)
        mode = fastfp_mode()
        if mode:
            with open(os.path.join(out_dir, "a64_fpx.h"), "a", newline="\n") as f:   # (after the kit's fpx and FCMP parts)
                f.write(FASTFP_HEADER + (FF_VX_HEADER if vecfp_on() else ""))
            print("fast float regions (A64_FASTFP=%s): %d, %d float ops in them" % (
                mode, self.stats.get("ff_regions", 0), self.stats.get("ff_ops", 0)))
        return files

    a64_lift.Lifter.write = write
