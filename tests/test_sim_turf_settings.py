"""Build and run the game-free per-area simulated-turf settings check.

python -I -B tests/test_sim_turf_settings.py [--cxx clang++]
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
    with tempfile.TemporaryDirectory(prefix="sim-turf-settings-") as folder:
        exe = Path(folder) / "sim_turf_settings.exe"
        subprocess.run([
            args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
            "-I" + str(ROOT / "gpu" / "vk"), str(ROOT / "tests" / "sim_turf_settings_test.cpp"), "-o", str(exe),
        ], check=True)
        env = os.environ.copy()
        env["PATH"] = str(Path(args.cxx).resolve().parent) + os.pathsep + env.get("PATH", "")
        subprocess.run([str(exe), folder], check=True, env=env)


if __name__ == "__main__":
    main()
