"""fps_presented must hand the renderer's cumulative draw-wait count to perf_frame, not a separate zero variable.

This compiles the relevant piece of gpu/vk/vk_fps.cpp with stand-ins: draw_stats3 reports seven waited draws, and the
stand-in perf_frame records what it was given. The check fails when the count is dropped on the way, which is how
causes.pipe_waits always read zero in v2 summaries.

python -I -B tests/test_perf_pipe_waits.py [--cxx g++]
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import between   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]


def fixture():
    src = (ROOT / "gpu" / "vk" / "vk_fps.cpp").read_text(encoding="utf-8")
    if "int64_t slept[2] = {0, 0}" not in src or "if (last_at) {" not in src:
        raise SystemExit("vk_fps.cpp: the fps_presented declarations were renamed: cut them by hand here")
    start = between(src, "int64_t slept[2] = {0, 0}", "if (last_at) {")
    call = re.search(r"^\s*if \(perf && f\.ms > 0\) perf_frame\([^;]+;", src, re.M)
    if not call:
        raise SystemExit("vk_fps.cpp: the perf_frame call in fps_presented was renamed: cut it by hand here")
    return r'''#include "perf_report.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace fs = mkwvk::fps_stats;
static int64_t given = -1;
void perf_frame(const fs::Frame&, double, int64_t pipe_waits) { given = pipe_waits; }

struct Host { void (*slept_ns)(int64_t*); };
static void no_sleep(int64_t* s) { s[0] = s[1] = 0; }
static Host host_stub{no_sleep};
static Host* host = &host_stub;
static int64_t fps_clock() { return 1; }
static int64_t gpu_time_ns() { return 0; }
static int64_t slot_wait_ns() { return 0; }
static void draw_stats(int64_t* d, int64_t* sk, int64_t* di, int64_t* pipes) { *d = *sk = *di = 0; *pipes = 0; }
static void draw_stats3(int64_t* left, int64_t* waits, double* wait_ms) { *left = 0; *waits = 7; *wait_ms = 2.5; }

static void check(bool breakdown, bool perf) {
''' + start + r'''
    fs::Frame f;
    f.ms = 16.7f;
''' + call.group(0) + r'''
}

int main() {
    check(true, true);
    if (given != 7) {
        std::fprintf(stderr, "FAIL: perf_frame was given %lld draw waits, draw_stats3 reported 7\n", (long long)given);
        return 1;
    }
    check(false, true);
    if (given != 0) {
        std::fprintf(stderr, "FAIL: with the breakdown off perf_frame was given %lld waits, want 0\n", (long long)given);
        return 1;
    }
    std::printf("PASS: fps_presented passes the renderer's cumulative draw waits to perf_frame\n");
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    a = p.parse_args()
    cxx = shutil.which(a.cxx)
    if not cxx:
        raise SystemExit("C++ compiler not found: " + a.cxx)
    with tempfile.TemporaryDirectory(prefix="longshot-pipe-waits-") as tmp:
        cpp, exe = Path(tmp) / "fixture.cpp", Path(tmp) / "fixture"
        cpp.write_text(fixture(), encoding="utf-8")
        r = subprocess.run([cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-function", "-Wno-unused-but-set-variable", "-Wno-unused-variable",
                            "-I" + str(ROOT / "gpu" / "vk"), str(cpp), "-o", str(exe)],
                           capture_output=True, text=True)
        if r.returncode:
            raise SystemExit("FAIL: the fps_presented fixture does not compile\n" + r.stdout + r.stderr)
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        print(r.stdout, end="")
        print(r.stderr, end="", file=sys.stderr)
        if r.returncode:
            raise SystemExit("FAIL: the renderer's draw waits do not reach the perf summary")
    print("PASS: draw-wait plumbing (no game required)")


if __name__ == "__main__":
    main()
