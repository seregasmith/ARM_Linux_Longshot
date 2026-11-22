"""Game-free checks for the real ReShade FX compiler, built-in presets and import bounds."""

import argparse
from pathlib import Path
import subprocess
import sys


RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{spirv_headers}"]


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--headers", required=True, type=Path)
    args = parser.parse_args()
    headers = args.headers
    if not (headers / "spirv.hpp").is_file():
        headers = headers / "spirv" / "unified1"
    if not (headers / "spirv.hpp").is_file():
        raise SystemExit(f"Missing SPIR-V headers: {headers}")
    script = Path(__file__).resolve().parents[1] / "scripts" / "reshade" / "check_fx.py"
    subprocess.run([sys.executable, "-I", "-B", script, "--spirv-headers", headers, "--cxx", args.cxx], check=True)
