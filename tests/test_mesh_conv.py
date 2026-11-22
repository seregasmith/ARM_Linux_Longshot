"""RX_VK_MESH_CONV (gpu/vk/mesh_conv.h): the indirect mesh draws' ring slots and the batch at the game's barriers, on a model of
the replay; no Vulkan, GPU or game data.

tests/mesh_conv_test.cpp replays made-up streams in order with the replay's own list of the ops a batch's scan steps over
(cut here from vk_replay.cpp mesh_conv_at_barrier) and checks that every indirect mesh draw reads {its arguments' count words
as they are at the draw, 1, 1}, converted at the draw (0, 1) or at the barrier before it (2), with the value changing between
recordings (HOS_AB) and the ring coming round. Then variants that must fail: slots at 16 bytes as before (the ring filled
once is then wrong), a draw that stores to memory not dropping the batch, the scan stepping over buffer clears, a slot the ring
came round onto used. The op kinds: the scan's kind_of, from the Ops the replay makes (its initializer cut from
vk_replay.cpp too), answers as a walk of the list does for every opcode up to 4096 and a few huge ones, and keeps answering
so once the list it was made from is overwritten: a table, not a walk of the list at each op (main, with no table, fails
sooner: the model does not build without make_ops and kOpTable). Their broken variants: the table shifted by one opcode,
an op past the table answered Pass. The source checks:
vk_draw.cpp drops the batch at a dispatch and at a draw whose program writes memory, reads the switch at each recording's
start (through the host's A/B, version 36), and fills the ring once with 1s; the replay scans right after the game's barrier
and names each mesh draw's op; and a GPU without mesh shaders in use (the compute fallback, which dispatches each indirect
mesh draw's workgroups from the converted records and leaves out a draw with none: every RADV GPU by default, the Steam Deck
among them) still has its draws converted. The conversion and fallback functions are also compiled from the selected source
with stand-ins, checking their copy, published slot and indirect dispatch.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block, between  # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
MUST_FAIL = ("align16", "no_drop", "clearbuf_passes", "no_hold", "rescan_before", "table_shift", "past_pass")


def fail(msg):
    print("FAIL: " + msg)
    raise SystemExit(1)


def live_code(text):
    """Blank comments, and reject preprocessor directives rather than guessing which declarations are active."""
    text = re.sub(r"\\\r?\n", "", text)
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
        elif text[i] in "\"'":
            j = i + 1
            while j < n and text[j] not in (text[i], "\n"):
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
            continue
        else:
            out.append(text[i])
            i += 1
            continue
        out.append(re.sub(r"[^\n]", " ", text[i:j]))
        i = j
    result = "".join(out)
    if re.search(r"^\s*#", result, re.MULTILINE):
        raise ValueError("preprocessor directive in mesh_conv_at_barrier; cannot select the live declarations")
    return result


def check_conditional_rejected():
    # The compiler selects the wrong count, while the inactive copy has the right one.
    source = """#if 0 || 1
static constexpr mesh_conv::Ops ops = make_ops(pass_ops, count - 1);
#else
static constexpr mesh_conv::Ops ops = make_ops(pass_ops, count);
#endif
"""
    try:
        live_code(source)
    except ValueError:
        return
    fail("conditional Ops declarations were accepted")


def source_checks(src):
    draw = (src / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    replay = (src / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    if "mesh_batch_drop();" not in block(draw, "void dispatch(VkCommandBuffer cmd, NvnState& s, uint32_t x"):
        fail("vk_draw.cpp dispatch() does not drop the batch")
    body = block(draw, "void draw(VkCommandBuffer cmd, NvnState& s, const DrawArgs& d) {")
    if "if (mesh_batch.live() && writes_memory(p)) mesh_batch_drop();" not in body:
        fail("vk_draw.cpp draw() does not drop the batch for a program that writes memory")
    if body.index("mesh_indirect_convert(cmd, d);") > body.index("mesh_batch_drop();"):
        fail("vk_draw.cpp draw(): the batch is dropped before the draw takes its own slot")
    if "mesh_conv_begin_recording();" not in block(draw, "void draw_begin_recording(int slot, VkCommandBuffer cmd"):
        fail("vk_draw.cpp: the switch is not read (and the batch let go) at each recording's start")
    begin = block(draw, "void mesh_conv_begin_recording() {")
    if 'host->version >= 36 && host->ab_value ? host->ab_value("RX_VK_MESH_CONV", env) : env' not in begin or \
            "mesh_batch.clear();" not in begin:
        fail("vk_draw.cpp mesh_conv_begin_recording: not the A/B's value, or the batch kept")
    make = block(draw, "bool mesh_ring_make() {")
    if "vkCmdFillBuffer(c, mesh_ring, 0, VK_WHOLE_SIZE, 1u);" not in make or "vkWaitForFences" not in make:
        fail("vk_draw.cpp mesh_ring_make: the ring is not filled with 1s and waited for")
    conv = block(draw, "bool mesh_indirect_convert(VkCommandBuffer cmd, const DrawArgs& d) {")
    if "mesh_batch.take(cmd, d.op)" not in conv or "mesh_conv::still_holds(" not in conv or \
            "mesh_conv::take(" not in block(draw, "bool mesh_ring_slot("):
        fail("vk_draw.cpp: a draw's slot is not taken from the batch with its checks, or not from mesh_conv::take")
    if "if (mode >= 1 && mesh_ring_filled.load(std::memory_order_acquire)) {" not in conv:
        fail("vk_draw.cpp mesh_indirect_convert: the fill is left out without the ring filled once")
    barrier = between(replay, "        case NOP_BARRIER:", "        // ---- work ----")
    if not 0 <= barrier.index('full_barrier(cmd, "barrier (the game\'s nvnCommandBufferBarrier)");') < \
            barrier.index("mesh_conv_at_barrier(cmd, p + h.size, end);"):
        fail("vk_replay.cpp: the batch is not scanned right after the game's full barrier")
    loop = block(replay, "void replay(VkCommandBuffer& cmd, NvnState& s, const uint8_t* p, const uint8_t* end")
    rescan = "if (p == mesh_rescan_at) mesh_conv_at_barrier(cmd, p + h.size, end, true);"
    if rescan not in loop or loop.index(rescan) < loop.index("switch (h.op) {") or             loop.index(rescan) < loop.index("early_split(cmd);"):
        fail("vk_replay.cpp: the scan after the op that ended the last one is not made once the op (and a part's end) is recorded")
    if "da.op = p;" not in between(replay, "        case NOP_DRAW_MESH: {", "        case NOP_DISPATCH: {"):
        fail("vk_replay.cpp: a mesh draw does not carry its op")
    # (the list and the Ops made from it, as the replay makes them: `static const` before the table, `static constexpr` with
    # it; reject directives in the function and require exactly one of each, so the model cannot silently select a different
    # conditional declaration from the replay)
    try:
        at = live_code(block(replay, "void mesh_conv_at_barrier(VkCommandBuffer cmd, const uint8_t* next, const uint8_t* end"))
    except ValueError as exc:
        fail(str(exc))
    heads = list(re.finditer(r"\bstatic\s+(?:const|constexpr)\s+uint32_t\s+pass_ops\s*\[\s*\]\s*=\s*\{", at))
    mades = list(re.finditer(r"\bstatic\s+(?:const|constexpr)\s+mesh_conv::Ops\s+ops\s*(?:=\s*([^;]*?)|(\{[^;]*?\}));", at))
    if len(heads) != 1 or len(mades) != 1:
        fail("vk_replay.cpp mesh_conv_at_barrier: `static const(expr) uint32_t pass_ops[] = {` found %d times and "
             "`static const(expr) mesh_conv::Ops ops` made from it %d times in its live code, not once each"
             % (len(heads), len(mades)))
    head, made = heads[0], mades[0]
    ops = at[head.end():at.index("};", head.end())]
    return ops, " ".join((made.group(1) or made.group(2)).split())


def main():
    check_conditional_rejected()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--source", type=Path, default=ROOT, help="a checkout to check (the negative control: main)")
    args = parser.parse_args()
    cxx = shutil.which(args.cxx)
    if not cxx:
        raise SystemExit("C++ compiler not found: " + args.cxx)
    src = args.source
    if not (src / "gpu/vk/mesh_conv.h").exists():
        fail("gpu/vk/mesh_conv.h is missing: no RX_VK_MESH_CONV")
    pass_ops, make_ops = source_checks(src)
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    with tempfile.TemporaryDirectory(prefix="longshot-mesh-conv-") as folder:
        tmp = Path(folder)
        draw = (src / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
        (tmp / "mesh_conv_real.inc").write_text(
            block(draw, "bool mesh_indirect_convert(VkCommandBuffer cmd, const DrawArgs& d) {") + "\n" +
            block(draw, "bool fallback_record(VkCommandBuffer cmd, Program& p, const DrawArgs& d) {") + "\n",
            encoding="utf-8")
        real_exe = tmp / ("mesh_conv_real.exe" if os.name == "nt" else "mesh_conv_real")
        real_cmd = [cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                    "-I" + str(src), "-I" + str(tmp), str(ROOT / "tests/mesh_conv_real_test.cpp"), "-o", str(real_exe)]
        r = subprocess.run(real_cmd, capture_output=True, text=True, timeout=180, creationflags=flags)
        if r.returncode:
            print(r.stdout + r.stderr)
            fail("the real conversion and fallback fixture did not build")
        r = subprocess.run([str(real_exe)], capture_output=True, text=True, timeout=120, creationflags=flags)
        if r.returncode:
            print(r.stdout + r.stderr)
            fail("the fallback did not copy, publish and dispatch the converted arguments")
        (tmp / "mesh_conv_pass_ops.inc").write_text("#define PASS_OPS " + " ".join(pass_ops.split()) + "\n"
                                                     "#define REPLAY_OPS(pass_ops) " + make_ops + "\n", encoding="utf-8")
        exe = tmp / ("mesh_conv.exe" if os.name == "nt" else "mesh_conv")
        cmd = [cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers", "-I" + str(src),
               "-I" + str(src / "gpu/render"), "-I" + str(tmp), str(ROOT / "tests/mesh_conv_test.cpp"), "-o", str(exe)]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=180, creationflags=flags)
        if r.returncode:
            print(r.stdout + r.stderr)
            fail("the model did not build")
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120, creationflags=flags)
        print(r.stdout.strip())
        if r.returncode:
            fail("RX_VK_MESH_CONV's model: the scan's op kinds, or a draw read other records than its arguments' "
                 "(the FAIL line above)")
        for v in MUST_FAIL:
            r = subprocess.run([str(exe), v], capture_output=True, text=True, timeout=120, creationflags=flags)
            print(r.stdout.strip())
            if r.returncode == 0:
                fail("the broken variant %s was not caught" % v)
    print("PASS: RX_VK_MESH_CONV's slots and batch on the model (0, 1, 2 and flips between recordings); the scan's op kinds "
          "from a table, as the list's walk answers; %d broken variants caught; the source's drops, switch read, ring fill, "
          "scan placement and the compute fallback's conversion" % len(MUST_FAIL))


if __name__ == "__main__":
    main()
