"""The FSR 4 model scanner (gpu/vk/fsr4_models.h/.cpp) on synthetic PE images: the
four provider classes, their Complete Object Locators, vtables and static registry, laid out as AMD's 4.1.2 Vulkan DLL
has them. Checks the four providers and their order, the registry instances, the 4.1.2 shape (entries 0, 1, 3..7
shared, entry 2 distinct), a missing Lite, a single provider, a shifted shared entry, an unknown provider, a non-PE
image, the fail-closed install and the selection helpers. No AMD DLL, GPU or game.

python -I -B tests/test_fsr4_models.py [--cxx g++]
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"
HARNESS = ROOT / "tests" / "fsr4_models_test.cpp"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as td:
        exe = Path(td) / ("fsr4_models_test.exe" if os.name == "nt" else "fsr4_models_test")
        cmd = [args.cxx, "-std=c++20", "-O2", "-DFSR4_MODELS_TEST", "-I", str(VK), str(VK / "fsr4_models.cpp"), str(HARNESS), "-o", str(exe)]
        if os.name == "nt":
            cmd.append("-static")
        built = subprocess.run(cmd, capture_output=True, text=True)
        if built.returncode != 0:
            print(built.stdout, end="")
            print(built.stderr, end="", file=sys.stderr)
            print("FAIL: the scanner test did not build")
            return 1
        ran = subprocess.run([str(exe)], capture_output=True, text=True)
        print(ran.stdout, end="")
        if ran.returncode != 0:
            print(ran.stderr, end="", file=sys.stderr)
            print("FAIL: the scanner test failed")
            return 1

    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
