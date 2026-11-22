"""ASTC levels of 1024 or less re-encode to BC eight layers a run, through a scratch of their own (gpu/vk/vk_texture.cpp).

An ASTC array kept as BC3 is decoded into a scratch image and encoded from it one run at a time; a run is a decode, an
encode, a copy and three full barriers. The 2048 x 2048 scratch has one layer (RX_VK_BC_LAYERS), so the terrain's
material arrays (1365 x 1365 x 90, 11 levels) made ~990 runs, and at Hyrule Field's entry on an RX 9070 XT the GPU took
51 ms an array, most of it between runs (#1720). Levels no larger than 1024 x 1024 now go through a small scratch of
bc_small_layers() layers (8; RX_VK_BC_SMALL_LAYERS=1: none): 15 ms an array, for 36 MB more, not made out of VRAM. On a
small-VRAM card, where 36 MB were too much and every level was a run of its own, it is 256 x 256 (2 MB), for one thing only: a
single-layer texture's levels go one into each of its layers, a group a run (on every card). A small-VRAM card's layer arrays go
one layer a run as before. RX_VK_BC_LEVELS=0: no groups and no 256 x 256 scratch, as before. One layer a run (ec89f92e)
was for Hyrule Field's grid lines on an RX 6600, which came back on AMD's Windows driver with this scratch (#2006), and
stayed with the copy's layer pitch given (#2015; #2107, #2110 on an RX 6750 XT). So bc_small_layers() is 8 only on the
drivers whose runs of 8 were seen clean, RADV and NVIDIA's (device_policy.h's bc_small_runs), and 1 on every other: no
small scratch there, so no groups either. The check runs it on both, with and without RX_VK_BC_SMALL_LAYERS. Runs still
never hold a repacked level (`!repack`), and a run is no longer than the upload chunk holds.

python -I -B tests/test_bc_small_scratch.py [--cxx g++]
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
TEX = ROOT / "gpu" / "vk" / "vk_texture.cpp"
SWITCHES = ROOT / "gpu" / "vk" / "switches.def"

FIXTURE = r"""
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
constexpr uint32_t kBcLayers = 8;
struct Policy { bool bc_small_runs = false; };
struct Dev { Policy policy; } dev;   // (device_policy.h's field, as create_device fills it: argv[1] 1 or 0)
#include "small.inc"
int main(int argc, char** argv) {
    dev.policy.bc_small_runs = argc > 1 && argv[1][0] == '1';
    std::printf("%u %d\n", bc_small_layers(), (int)bc_levels_on());
    return 0;
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


def need(ok, why):
    if not ok:
        raise SystemExit("FAIL: " + why)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    cxx = p.parse_args().cxx
    tex = TEX.read_text(encoding="utf-8")
    need("uint32_t bc_small_layers() {" in tex, "no small scratch: every ASTC-to-BC run is one layer, whatever its level's size")
    need("constexpr uint32_t kBcSmallDim = 1024;" in tex, "the small scratch's size changed")

    upload = block(tex, "bool upload_astc_gpu(Texture& tex, uint64_t total, const Ranges* dirty) {")
    small = ("const bool small = tex.bc && bc_small && !tex.bc_resized && g.w <= bc_small_px && g.h <= bc_small_px &&\n"
             "                           (bc_small_px == kBcSmallDim || grouped);")
    need(small in upload, "a level the small scratch can't hold (or a resized texture's) may go through it")
    need("std::min<uint64_t>(small ? bc_small_layers() : bc_layers()," in upload and "tex.bc ? bc_run" in upload,
         "a level's runs aren't the layers of the scratch they go through")
    need("(small ? bc_small_view : bc_scratch_view)" in upload, "a small level isn't decoded into the small scratch")
    need("small ? bc_small_view : VK_NULL_HANDLE);" in upload, "a small level isn't encoded from the small scratch")
    need("while (!repack && k1 < layers.size()" in upload, "a run may hold a repacked level again (Hyrule Field's grid lines)")
    need("chunk_texels / level_texels" in upload, "a run may be longer than the upload chunk holds")

    make = block(tex, "bool bc_scratch_make() {")
    need("si.extent = {bc_small_px, bc_small_px, 1};" in make and "si.arrayLayers = bc_small_layers();" in make,
         "the small scratch is not made at its size and layers")
    need("const bool tiny = low_vram_texture_mips() && bc_levels_on();" in make and
         "if (bc_small_layers() > 1 && (!low_vram_texture_mips() || tiny)) {" in make,
         "the small scratch is made at RX_VK_BC_SMALL_LAYERS=1, or on a small-VRAM card at another size than its own")
    need("constexpr uint32_t kBcTinyDim = 256;" in tex and "bc_small_px = tiny ? kBcTinyDim : kBcSmallDim;" in make, "the small scratch's side isn't the card's: 1024, or a small-VRAM card's")
    need("!allocation_in_vram(bc_small_alloc)" in make, "the small scratch is kept in system memory")
    need("bc_small ? (uint64_t)(bc_small_px / 4)" in make, "the output buffer grows for a small scratch that wasn't made")
    need("(bc_small_px / 4) * (bc_small_px / 4) * bc_small_layers() * 16" in make and "std::max(big_out, small_out)" in make,
         "the output buffer can't hold a small scratch's run")
    layout = block(tex, "bool bc_scratch_layout(")
    need("bs.image = bc_small;" in layout and "bc_small ? 2 : 1" in layout, "the small scratch's first layout is never set")
    idle = block(tex, "void bc_scratch_idle(")
    need("bc_small = VK_NULL_HANDLE" in idle and "bc_small_view}" in idle, "the small scratch is kept when the first is given back")

    need("(uint64_t)(bc_small_px / 4) * (bc_small_px / 4) * bc_small_layers() * 16) / 8;" in upload,
         "a group of levels isn't bounded by the output buffer as it was made")
    need("copies.push_back(VkBufferImageCopy{(VkDeviceSize)q.off * 8, qp.blocks_x * 4, qp.blocks_y * 4," in upload,
         "a group's copy leaves the buffer's row length and image height to the driver (#2015)")
    rows = SWITCHES.read_text(encoding="utf-8")
    need("SW(RX_VK_BC_SMALL_LAYERS, tune, int," in rows, "RX_VK_BC_SMALL_LAYERS has no row in switches.def")
    need("SW(RX_VK_BC_LEVELS, kill, bool," in rows, "RX_VK_BC_LEVELS is not a switch that turns the groups off")

    with tempfile.TemporaryDirectory(prefix="bc-small-") as temp:
        (Path(temp) / "small.inc").write_text("\n".join(block(tex, m) for m in (
            "uint32_t bc_small_layers() {", "bool bc_levels_on() {")), encoding="utf-8")
        src, exe = Path(temp) / "small.cpp", Path(temp) / "small.exe"
        src.write_text(FIXTURE, encoding="utf-8")
        subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I", temp, str(src), "-o", str(exe)], check=True)
        # (runs of 8: RADV or NVIDIA's driver, "1"; any other, AMD's Windows driver among them, "0")
        for runs, value, want in (("1", None, "8"), ("0", None, "1"), ("1", "1", "1"), ("1", "4", "4"), ("1", "99", "8"),
                                  ("1", "0", "1"), ("0", "8", "8"), ("0", "4", "4"), ("0", "1", "1")):
            env = {k: v for k, v in os.environ.items() if k != "RX_VK_BC_SMALL_LAYERS"}
            if value is not None:
                env["RX_VK_BC_SMALL_LAYERS"] = value
            got = subprocess.run([str(exe), runs], text=True, capture_output=True, timeout=20, env=env, check=True).stdout.split()[0]
            pc = "RADV or NVIDIA's driver" if runs == "1" else "another driver (AMD's Windows driver: Hyrule Field's grid lines)"
            need(got == want, f"{pc}, RX_VK_BC_SMALL_LAYERS={value}: {got} layers a run, wanted {want}")
        # (the groups, and a small-VRAM card's 256 x 256 scratch with them: on unless 0)
        for name, at, cases in (("RX_VK_BC_LEVELS", 1, ((None, "1"), ("1", "1"), ("0", "0"))),):
            for value, want in cases:
                env = {k: v for k, v in os.environ.items() if k != name}
                if value is not None:
                    env[name] = value
                got = subprocess.run([str(exe), "1"], text=True, capture_output=True, timeout=20, env=env, check=True).stdout.split()[at]
                need(got == want, f"{name}={value}: {got}, wanted {want}")
    print("PASS: levels of 1024 or less re-encode to BC 8 layers a run, through a small scratch of their own (256 on a small-VRAM "
          "card), on RADV and NVIDIA's driver; one layer a run, with no small scratch and no groups, on every other")


if __name__ == "__main__":
    main()
