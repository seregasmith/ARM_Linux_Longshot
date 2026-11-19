"""The start-up's progress lines say what memory this process and the system have (gpu/vk/linux_pressure.h parse_self_status,
memory_words; vk_draw.cpp start_memory_words, prepare_progress_log; vk_main.cpp's "still working on" line).

The shaders' preparation and the first start went into swap and took a 15 GB handheld down, and their logs said nothing of
memory, so where it went could only be guessed. This compiles the parsing and the words and runs them on /proc text (a
process with and without swap, a file that is not /proc/self/status, nothing read), and checks the progress lines end with
the words. No game or GPU.
python -I -B tests/test_start_memory_words.py [--cxx g++]
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
#include "linux_pressure.h"
#include <cstdio>
#include <cstring>
using namespace linux_pressure;
static int fails = 0;
#define NEED(c, what) do { if (!(c)) { fails++; std::printf("FAIL: %s\n", what); } } while (0)
int main() {
    const char* status = "Name:\tlongshot\nVmPeak:\t 99999 kB\nVmRSS:\t 6359040 kB\nRssAnon:\t 5000000 kB\nVmSwap:\t 1208320 kB\n";
    uint64_t rss = 1, swapped = 1;
    NEED(parse_self_status(status, &rss, &swapped) && rss == 6359040ull << 10 && swapped == 1208320ull << 10,
         "VmRSS and VmSwap read, in bytes");
    NEED(parse_self_status("VmRSS:\t 1024 kB", &rss, &swapped) && rss == 1ull << 20 && swapped == 0, "no VmSwap line: none in swap");
    NEED(!parse_self_status("MemTotal: 1 kB\n", &rss, &swapped), "not /proc/self/status: nothing");
    NEED(!parse_self_status(nullptr, &rss, &swapped), "no text: nothing");
    const std::string w = memory_words(true, 1490ull << 20, 13270ull << 20, true, 6210ull << 20, 1180ull << 20);
    NEED(w == "; memory: this process 6210 MB (1180 MB more in swap), 1490 MB available, 13270 MB of swap free", w.c_str());
    const std::string s = memory_words(true, 1490ull << 20, 13270ull << 20, false, 0, 0);
    NEED(s == "; memory: 1490 MB available, 13270 MB of swap free", s.c_str());
    NEED(memory_words(false, 1, 1, true, 1, 1).empty(), "no meminfo (Windows): no words");
    if (fails) return 1;
    std::printf("ok\n");
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
    pressure = (ROOT / "gpu" / "vk" / "linux_pressure.h").read_text(encoding="utf-8")
    if "parse_self_status(" not in pressure or "memory_words(" not in pressure:
        raise SystemExit("FAIL: linux_pressure.h reads no process memory: the start-up's log says nothing of where memory went")
    problems = []
    draw = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
    at = draw.index("void prepare_progress_log(")
    fn = draw[at:draw.index("\n}\n", at)]
    if "start_memory_words()" not in fn or fn.count("memory.c_str()") != 2:
        problems.append("prepare_progress_log's lines do not end with the memory words")
    main_cpp = (ROOT / "gpu" / "vk" / "vk_main.cpp").read_text(encoding="utf-8")
    if 'still working on %s: %zu of %zu%s", what, at.done, at.total, start_memory_words().c_str());' not in main_cpp:
        problems.append("the preparation's \"still working on\" line does not say the memory")
    with tempfile.TemporaryDirectory(prefix="longshot-memory-words-") as folder:
        tmp = Path(folder)
        (tmp / "t.cpp").write_text(HARNESS, encoding="utf-8")
        flags = ["-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu" / "vk")]
        if os.name == "nt":
            flags.append("-D_CRT_SECURE_NO_WARNINGS")
        r = subprocess.run([cxx, *flags, str(tmp / "t.cpp"), "-o", str(tmp / "t")], capture_output=True, text=True, timeout=120)
        if r.returncode:
            raise SystemExit("FAIL: does not compile\n" + r.stdout + r.stderr)
        out = subprocess.run([str(tmp / "t")], capture_output=True, text=True, timeout=30)
        if out.returncode:
            problems.append(out.stdout.strip())
    if problems:
        raise SystemExit("FAIL:\n  " + "\n  ".join(problems))
    print("PASS: the start-up's progress lines say this process's memory and the system's")


if __name__ == "__main__":
    main()
