# SPDX-License-Identifier: GPL-3.0-or-later
"""Game-free check that the GPU ASTC decoder (gpu/vk/host_shaders/astc_decoder.comp) decodes exactly as ARM's astcenc does.

Reference: gpu/vk/texdecode/astc.cpp (texdecode), equal to astcenc 6.0.0 byte for byte; tests/texdecode_astc_vectors.vec holds
4,694 blocks with astcenc's answers. Nothing here depends on suyu's decoder, which is wrong on some blocks.

Default mode (run_all.py; no GPU or Vulkan loader needed): compiles tests/astc_decoder_gpu.cpp against the checked-in SPIR-V header
and texdecode; checks the vectors against astcenc's answers on the CPU, that the hand-built blocks mean what they were built to,
that the level builder lays blocks out as texdecode's block-linear layout does, and that the SPIR-V header is valid.

GPU mode (--device NAME, or --gpu; tests/run_vulkan_fixtures.py runs it on lavapipe with validation): dispatches the real compiled
shader, as the renderer does (the swizzled blocks as a storage buffer, its push constants, a storage view of an R8G8B8A8_UNORM
image), over every vector block in linear and sRGB mode, whole levels (layers, edge blocks, GOB tiles, a short binding) and the
hand-built blocks, and demands every texel equal to texdecode's. An illegal block is transparent black (the renderer's colour), a
partition with an HDR endpoint mode magenta (as texdecode and astcenc). On a failure it prints a table of how many blocks of each
class are equal, one step off and further off; --table prints it always.

python -I -B tests/test_astc_decoder_gpu.py --headers <Vulkan headers> [--device llvmpipe] [--table]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++") or "g++")
    parser.add_argument("--headers", type=Path, required=True, help="path to Vulkan-Headers include directory")
    parser.add_argument("--gpu", action="store_true", help="dispatch astc_decoder_comp_spv.h on a Vulkan device")
    parser.add_argument("--gpu-device", "--device", dest="device", help="case-insensitive device-name substring (implies --gpu)")
    parser.add_argument("--table", action="store_true", help="print the per-class table even when everything is equal")
    args = parser.parse_args()
    args.gpu = args.gpu or bool(args.device)

    texdecode = ROOT / "gpu/vk/texdecode"
    sources = [ROOT / "tests/astc_decoder_gpu.cpp", texdecode / "astc.cpp", texdecode / "block_linear.cpp"]
    with tempfile.TemporaryDirectory(prefix="longshot-astc-gpu-") as directory:
        exe = Path(directory) / ("astc_decoder_gpu.exe" if os.name == "nt" else "astc_decoder_gpu")
        link_flags = ["-pthread"]
        if os.name == "nt" and "mingw" in args.cxx.lower():
            link_flags += ["-static"]
        libraries = ["-ldl"] if os.name != "nt" else ["-lshell32"]
        subprocess.run(
            [args.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
             "-I", str(args.headers), "-I", str(ROOT / "gpu/vk"), "-I", str(ROOT / "tests"),
             *map(str, sources), "-o", str(exe), *link_flags, *libraries],
            check=True, timeout=300)
        env = os.environ.copy()
        if os.name == "nt":
            cxx_dir = str(Path(args.cxx).resolve().parent)
            if Path(cxx_dir).is_dir():
                env["PATH"] = cxx_dir + os.pathsep + env.get("PATH", "")
        cmd = [str(exe), str(ROOT / "tests/texdecode_astc_vectors.vec")]
        if args.gpu:
            cmd.append("--gpu")
            if args.device:
                cmd += ["--device", args.device]
        else:
            cmd.append("--cpu-only")
        if args.table:
            cmd.append("--table")
        result = subprocess.run(cmd, timeout=600 if args.gpu else 60, env=env)
        if result.returncode:
            raise SystemExit(result.returncode)
    return 0


if __name__ == "__main__":
    sys.exit(main())
