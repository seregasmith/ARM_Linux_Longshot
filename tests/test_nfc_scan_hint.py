"""The NFC Scan row's help lines end: the menu counts them by asking for the next one until nullptr.

`measure_help` (gpu/vk/vk_menu.cpp) sizes the help box for the tallest line any row can have: for a button with a hint it calls
`hint(0, k)` for k = 0, 1, 2, ... until nullptr (#1633). The Scan row's hint, `nfc_scan_says` (gpu/vk/menu/nfc.cpp), answered every k
with a text while the host's scan state was not "none" (0), and on a host with no reader the state is always 5 (host/nn_nfp.c, a
non-Windows build): the count never ended, the render thread spun at the menu's first open and the game stopped presenting.
This check compiles the function from the source with a stub host, for every scan state the host can report, and runs the menu's
loop with a bound: it must end within the two lines the function documents (0 the longest, 1 the one now).
Also cover a host with no scan callback and check that measuring hints preserves live status for states 0 through 7.

    python -I -B tests/test_nfc_scan_hint.py [--cxx g++]
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from srccut import block  # noqa: E402

PROGRAM = r'''
#include <cstdio>

struct Host {
    int (*nfc_scan)(int);
    const char* (*nfc_scan_name)(void);
};
static int state;
static int scan(int) { return state; }
static const char* scan_name(void) { return "tag"; }
static Host real{scan, scan_name};
static Host* host = &real;

@FUNCTION@

// (the menu's loop, with a bound: no abort, so a failing run leaves no core dump)
static bool ends() {
    int lines = 0;
    for (int k = 0; nfc_scan_says(k); k++)
        if (++lines > 2) return false;
    return true;
}

int main() {
    host = nullptr;   // (no host at all)
    if (!ends()) return std::puts("FAIL: the Scan row's help lines do not end with no host"), 1;
    Host missing{};
    host = &missing;
    if (!ends()) return std::puts("FAIL: help lines do not end without a scan callback"), 1;
    host = &real;
    for (state = 0; state <= 7; state++) {
        if (!ends()) return std::printf("FAIL: the Scan row's help lines do not end in scan state %d\n", state), 1;
        if ((nfc_scan_says(-1) != nullptr) != (state >= 1 && state <= 6))
            return std::printf("FAIL: live scan status changed in state %d\n", state), 1;
    }
    std::puts("ok");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    source = (ROOT / "gpu/vk/menu/nfc.cpp").read_text(encoding="utf-8")
    function = block(source, "const char* nfc_scan_says(int variant) {")
    with tempfile.TemporaryDirectory(prefix="nfc-scan-hint-") as tmp:
        cpp, exe = Path(tmp) / "scan.cpp", Path(tmp) / "scan.exe"
        cpp.write_text(PROGRAM.replace("@FUNCTION@", function), encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: the Scan row's help lines end for every scan state and with no host")


if __name__ == "__main__":
    main()
