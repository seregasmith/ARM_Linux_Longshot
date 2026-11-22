"""The menu's model (gpu/vk/menu_model.h, #1633 step 2): tests/test_menu_model.cpp compiled with it and run, for the order
build() works out from modules' wishes (after, before, first, last, order, at, place: the same whatever order the modules come
in), what it diagnoses (a missing anchor, a cycle, a doubled key or host option, a page or section no module made), and what a
presenter sees and does (shown, locked, translation, stepping, actions, pages, custom items). And the model's own files stay
free of ImGui and Vulkan, so another presenter (the game's own menu) can use it.

    python -I -B tests/test_menu_model.py [--cxx g++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    for name in ("menu_model.h", "menu_model.cpp"):
        src = (ROOT / "gpu" / "vk" / name).read_text(encoding="utf-8")
        for inc in re.findall(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', src, re.M):
            if inc != "menu_model.h" and ("/" in inc or "." in inc):
                raise SystemExit("gpu/vk/%s includes %s: the model takes nothing from the renderer, ImGui or Vulkan" % (name, inc))
    with tempfile.TemporaryDirectory(prefix="menu-model-") as tmp:
        exe = Path(tmp) / "menu-model"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests" / "test_menu_model.cpp"),
                        str(ROOT / "gpu" / "vk" / "menu_model.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: the menu's model")


if __name__ == "__main__":
    main()
