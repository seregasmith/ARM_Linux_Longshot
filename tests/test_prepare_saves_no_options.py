"""A --prepare-shaders run writes no options (gpu/vk/vk_menu.cpp save()).

The Builder's shader preparation has no window, so the screen is 0 x 0 to it. Its first start chose Auto (this PC) for that
screen and saved the Output resolution as 2560x1440 into longshot_options.ini, which every later start then took for the
player's choice: a 1920 x 1080 AYN Odin 3 drew at 1440p and scaled down. This cuts the start of save() out of vk_menu.cpp, up
to where it begins to write, and runs it on stand-ins:
 - LONGSHOT_PREPARE_ONLY=1 (a preparation): it goes no further, and says so in the log once, however often it is called;
 - unset or 0 (the game): it goes on to write, as before.
No game or GPU.
python -I -B tests/test_prepare_saves_no_options.py [--cxx g++]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r"""
#include <cstdarg>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
static bool env_on(const char* name) { const char* v = getenv(name); return v && *v && strcmp(v, "0") != 0; }
static int logged = 0;
static void logf(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    std::vprintf(fmt, a);
    va_end(a);
    std::printf("\n");
    logged++;
}
static std::mutex options_save_mu;
static int wrote = 0;
%s
    wrote++;   // (save() goes on to write the file from here)
}
int main() {
    save();
    save();
    std::printf("WROTE=%%d LOGGED=%%d\n", wrote, logged);
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    args = parser.parse_args()
    cxx = shutil.which(args.cxx)
    if not cxx:
        raise SystemExit("C++ compiler not found: " + args.cxx)
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    head = "void save() {"
    if menu.count(head) != 1:
        raise SystemExit("FAIL: vk_menu.cpp has no single save()")
    at = menu.index(head)
    end = menu.index("    // (longshot) the lines that are not the menu's are kept", at)
    if "fopen(" in menu[at:end] or "fprintf(" in menu[at:end]:
        raise SystemExit("FAIL: save() reads or writes the file before it asks whether this is a preparation")
    problems = []
    with tempfile.TemporaryDirectory(prefix="longshot-prepare-options-") as folder:
        tmp = Path(folder)
        (tmp / "t.cpp").write_text(HARNESS % menu[at:end], encoding="utf-8")
        r = subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", str(tmp / "t.cpp"),
                            "-o", str(tmp / "t")], capture_output=True, text=True, timeout=120)
        if r.returncode:
            raise SystemExit("FAIL: the start of save() does not compile with the stand-ins\n" + r.stdout + r.stderr)
        base = {k: v for k, v in os.environ.items() if k != "LONGSHOT_PREPARE_ONLY"}
        for name, env, wrote, logged in (("a preparation", dict(base, LONGSHOT_PREPARE_ONLY="1"), 0, 1),
                                         ("the game", base, 2, 0),
                                         ("LONGSHOT_PREPARE_ONLY=0", dict(base, LONGSHOT_PREPARE_ONLY="0"), 2, 0)):
            out = subprocess.run([str(tmp / "t")], env=env, capture_output=True, text=True, timeout=30).stdout
            want = "WROTE=%d LOGGED=%d" % (wrote, logged)
            if want not in out:
                problems.append("%s: %s, not %s\n    %s" % (name, out.strip().splitlines()[-1] if out.strip() else "nothing", want,
                                                           out.strip().replace("\n", "\n    ")))
            if logged and "not saved by the shader preparation" not in out:
                problems.append("%s: the log does not say the options were not saved" % name)
    if problems:
        raise SystemExit("FAIL:\n  " + "\n  ".join(problems))
    print("PASS: a --prepare-shaders run writes no options (its screen is 0 x 0); the game saves as before")


if __name__ == "__main__":
    main()
