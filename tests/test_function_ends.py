"""No "}" at the start of a line inside a lifted function's body, game-free (no compiler).

In the lifted C a "}" at the start of a line closes a function: the lifter writes every instruction's own blocks so, and the
tools that read the lifted C end a function's body at the first one: nx2/split_giants.py (split_text) and nx2/oot_patches.py
(each edit looks for its place before it). nx2/vecfp.py's first wrapping (A64_VECFP) put the "}" that closes its `if` there,
and a fast float region's check copy (A64_FASTFP=check) put its else's there. With them:
  - split_giants took a function with one of vecfp's forms to end at that block and left it whole (unbalanced), so 221 of the
    game's 368 giant functions were compiled whole: the build with the vector float paths of 2026-11-17 compiled in 13 min, the
    same lift without them in 5;
  - oot_patches looked for its places in the part of a function before that block: the free camera's (f_804e48c0, an FCVT at
    +39 KB of its 210 KB, its place at +96 KB) and the camera centering's (f_80644c80, an FMAX at +277 KB of 642 KB, its place
    at +321 KB) were "found 0 times", and the game was built without them.
Checked here, with the blocks the emitters write (vecfp.wrap, fastfp.ff_region in both modes):
  1. none puts a "}" at the start of a line;
  2. split_text cuts a giant function with wrapped instructions in it (piece size 1 KB, every function a giant);
  3. each oot_patches edit that looks for its place inside one function finds it after a wrapped instruction, a region of
     wrapped instructions, and a region's check copy, before its place; and leaves a place in the next function alone.

    python -I -B tests/test_function_ends.py
"""
import os
import random
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
for _n in ("A64_FASTFP", "A64_FASTFP_MIN", "A64_VECFP"):
    os.environ.pop(_n, None)
import kit  # noqa: E402,F401  (the vendored kit's folders on the path: fastfp and vecfp import a64_lift and a64emit)
import fastfp  # noqa: E402
import oot_patches as P  # noqa: E402
import split_giants as SG  # noqa: E402
import test_split_giants as TSG  # noqa: E402  (its lifted-shaped function generator)
import vecfp  # noqa: E402

FAILS = []

# (an instruction's C as the kit writes a lane-by-lane multiply by element, and its fast path's call)
LANES = ("    { uint32_t _n[4],_r[4]; memcpy(_n,c->vreg[1],16);\n"
         "    for(unsigned _i=0;_i<4;++_i) _r[_i]=a64_fpx_mul32_fma32(c,_n[_i],(uint32_t)c->vreg[2][0],0);\n"
         "    memcpy(c->vreg[20],_r,16); }")
CALL = vecfp.fast_call(0x4FA29034, set(vecfp.GROUPS))   # (fmul v20.4s, v1.4s, v2.s[1])
TAIL = "void f_8fff0000(Ctx *__restrict A64_NOESC c) {\n    c->x[0]=0;\nreturn;\n}\n"   # (the function after, in the same file)


def check(ok, what):
    if not ok:
        FAILS.append(what)
        print("FAIL: " + what)


def region(mode):
    """a fast float region of 8 wrapped multiplies, as ff_region writes it with A64_FASTFP=mode"""
    os.environ["A64_FASTFP"] = mode
    try:
        text, n = fastfp.ff_region([(0x1000 + 4 * i, vecfp.wrap(LANES, CALL)) for i in range(8)], 1)
    finally:
        os.environ.pop("A64_FASTFP")
    check(n == 8, "a region of 8 wrapped multiplies has %d float ops (A64_FASTFP=%s)" % (n, mode))
    return text


def blocks():
    """(what, the C of one or more instructions) for each block an emitter writes over several lines"""
    return (("a wrapped instruction", vecfp.wrap(LANES, CALL).rstrip("\n")),
            ("a region of wrapped instructions", region("1")),
            ("a region's check copy", region("check")))


def check_emitters():
    text = vecfp.wrap(LANES, CALL)
    check(text.startswith("if (!%s) {\n" % CALL) and LANES in text and text.rstrip().endswith("}"),
          "vecfp.wrap is not the fast path's test, the instruction's own C, the closing brace:\n" + text)
    check("a64_ff_check(" in region("check"), "A64_FASTFP=check did not make a check copy")
    for what, block in blocks():
        starts = [ln for ln in block.split("\n") if ln.startswith("}")]
        check(not starts, "%s puts a '}' at the start of a line:\n%s" % (what, block))


def check_split():
    rng = random.Random(20261118)
    for k in range(6):
        name = "f_%08x" % (0x80100000 + 0x100 * k)
        lines = TSG.gen_function(name, rng, blocks=40).split("\n")
        at = [i for i, ln in enumerate(lines) if ln.startswith("    ") and rng.random() < 0.3]
        for i in at:   # (some of the instructions are vecfp's forms: behind their fast path)
            lines[i] = vecfp.wrap(lines[i], CALL).rstrip("\n")
        src = '#include "a64.h"\n' + "\n".join(lines) + "\n" + TAIL
        out, st = SG.split_text(src, piece_kb=1, min_kb=0, only={name})
        check(st["functions_split"] == 1, "%s with %d wrapped instructions is not split (%s)" % (name, len(at), dict(st)))
        check(out.count(CALL) == len(at), "%s split lost or doubled a wrapped instruction" % name)


def edits():
    """(what, the function's head, its place, the rest of the function, the edit, the text it must make)"""
    whole_head, whole_old, whole_key = P.WHOLE_SITES[0]
    speed_head, speed_anchor, per_second = P.SPEED_PLACES[0]
    return (
        ("free camera", P.VIEW_HEAD, P.VIEW_ANCHORS[0], "    c->x[8]=c->x[20];\nreturn;\n", P.patch_view,
         P.VIEW_ANCHORS[0] + "    hos_oot_free_view(c, c->x[20]);\n"),
        ("camera centering", P.CAM_HEAD, P.CAM_FOLLOW_ANCHOR, "    c->x[25] = c->x[26];\n" + P.CAM_FOLLOW_JOIN + "\nreturn;\n",
         P.patch_camera_centering, P.CAM_FOLLOW_ANCHOR + P.CAM_FOLLOW_HOOK),
        ("heart protection", P.LIFE_HEAD, P.LIFE_CAS + "\n", "    c->x[9]=1;\n" + P.LIFE_CAS + "\nreturn;\n", P.patch_life,
         "hos_oot_damage((uint32_t)c->x[11], (uint32_t)c->x[12], (uint32_t)c->x[9])"),
        ("flight", P.MOVE_HEAD, P.MOVE_ANCHOR, P.MOVE_MATRIX + "return;\n", P.patch_movement,
         "    hos_oot_lift_player(c, c->x[19]);\n" + P.MOVE_ANCHOR),
        ("flight rotation", P.WARP_HEAD, P.WARP_ANCHOR, "return;\n", P.patch_flight_rotation,
         "    hos_oot_flight_rotation(c, c->x[0], c->x[1]);\n" + P.WARP_ANCHOR),
        ("movement speed", speed_head, speed_anchor, "return;\n", lambda s: P.patch_cct_speed(s)[0],
         speed_anchor + P.SPEED_HOOK % (per_second, "second" if per_second else "frame")),
        ("whole frames", whole_head, whole_old, "return;\n", lambda s: P.patch_whole_frames(s)[0], P.whole_new(whole_old, whole_key)),
    )


def check_edits():
    for what, head, place, rest, edit, made in edits():
        for shape, block in (("no block", ""),) + blocks():
            src = '#include "a64.h"\n' + head + "    c->x[1]=c->x[2];\n" + (block + "\n" if block else "") + place + rest + "}\n" + TAIL
            out = edit(src)
            ok = out is not None and made in out
            check(ok, "%s: the edit is not made after %s" % (what, shape))
            if ok:
                check(out.index(made) < out.index(TAIL) and out.endswith(TAIL), "%s: the edit is not in its function (%s)" % (what, shape))
        # (a place in the function after this one is not this function's)
        block = blocks()[0][1]
        src = '#include "a64.h"\n' + head + block + "\nreturn;\n}\n" + TAIL.replace("return;\n", place + rest)
        out = edit(src)
        check(out is None or made not in out, "%s: a place in the next function was taken for this one's" % what)


def main():
    check_emitters()
    check_split()
    check_edits()
    if FAILS:
        raise SystemExit("function_ends: %d failed" % len(FAILS))
    print("PASS: no emitter puts a '}' at the start of a line inside a function: giant functions with vecfp's forms are cut, "
          "every edit finds its place after a wrapped instruction or a region")


if __name__ == "__main__":
    main()
