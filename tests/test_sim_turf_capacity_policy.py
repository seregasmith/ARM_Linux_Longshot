"""Build and run the game-free adaptive simulated-turf-capacity policy check.

python -I -B tests/test_sim_turf_capacity_policy.py [--cxx clang++]
"""
from __future__ import annotations

import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="sim-turf-capacity-") as folder:
        exe = Path(folder) / "sim_turf_capacity_policy.exe"
        subprocess.run([
            args.cxx, "-std=c++20", "-I" + str(ROOT / "gpu" / "vk"),
            str(ROOT / "tests" / "sim_turf_capacity_policy.cpp"), "-o", str(exe),
        ], check=True)
        env = os.environ.copy()
        env["PATH"] = str(Path(args.cxx).resolve().parent) + os.pathsep + env.get("PATH", "")
        subprocess.run([str(exe)], check=True, env=env)


if __name__ == "__main__":
    main()
