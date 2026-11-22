#!/usr/bin/env python3
"""Link and render the game-free Graphics fixture on Linux using the selected compiler."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

HELPER = Path(__file__).with_name("capture_menu.py")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=HELPER.resolve().parents[2])
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--vulkan-headers", type=Path, default=Path("/usr/include"))
    parser.add_argument("--evidence", type=Path, help="optional new directory for the verified PNG and receipt")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="longshot-menu-smoke-") as tmp:
        work = Path(tmp)
        output = work / "output"
        command = [sys.executable, "-I", "-B", str(HELPER.resolve()), "--views", "graphics",
                   "--cxx", args.cxx, "--vulkan-headers", str(args.vulkan_headers), "--output", str(output)]
        old = subprocess.run([*command, "--source", str(work)], capture_output=True, text=True)
        if old.returncode != 2 or "use the capture helper from commit f4fa8dae" not in old.stderr or output.exists():
            parser.error("expected documented rejection of a source without the modular menu")
        subprocess.run([*command, "--source", str(args.source)], check=True)
        screenshot = output / "settings-graphics.png"
        receipt = output / "menu-capture.json"
        png = screenshot.read_bytes()
        info = json.loads(receipt.read_text(encoding="utf-8"))
        if (png[:16] != b"\x89PNG\r\n\x1a\n\0\0\0\rIHDR" or struct.unpack(">II", png[16:24]) != (1280, 720)
                or info["images"].get(screenshot.name) != hashlib.sha256(png).hexdigest()
                or not {"gpu/vk/menu_model.cpp", "assets/longshot.png"} <= info["source_files"].keys()):
            parser.error("invalid Graphics capture or incomplete capture provenance")
        if args.evidence:
            args.evidence.mkdir()
            shutil.copy2(screenshot, args.evidence)
            shutil.copy2(receipt, args.evidence)
    print("Graphics capture smoke check passed")


if __name__ == "__main__":
    main()
