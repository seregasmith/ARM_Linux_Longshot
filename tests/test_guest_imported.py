"""Check which guest pages a live or retiring GPU import still holds."""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    source = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    start = source.index("int memory_guest_imported(")
    function = source[start:source.index("\n}", start) + 2]
    fixture = r"""
#include <cassert>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <vector>
#include "import_reach.h"

struct Pool { uint64_t lo, hi, memory; };
std::map<uint64_t, Pool> imported;
std::deque<decltype(imported)::node_type> retired_pools, evicted_pools;
std::mutex pools_mu, bg_mu;
std::map<uint64_t, uint64_t> bg_inflight;
std::vector<std::pair<uint64_t, Pool>> bg_ready;
struct DeadImport { uint64_t lo, bytes; };
std::deque<DeadImport> dead_imports;
import_reach::Reach live_reach;   // (vk_memory.cpp: the live imports' spans, kept by imported_add)
""" + function + r"""

int main() {
    imported.emplace(0x2000, Pool{0x2000, 0x5000, 1});
    live_reach.add(0x3000);
    auto check = [] {
        assert(!memory_guest_imported(0x1000, 0x2000));
        assert(!memory_guest_imported(0x5000, 0x6000));
        assert(!memory_guest_imported(0x3000, 0x3000));
        assert(memory_guest_imported(0x2000, 0x3000));
        assert(memory_guest_imported(0x4000, 0x6000));
        assert(memory_guest_imported(0x1000, 0x6000));
    };
    check();
    retired_pools.push_back(imported.extract(0x2000));
    check();
    evicted_pools.swap(retired_pools);
    check();
    evicted_pools.front().mapped().memory = 0;
    assert(!memory_guest_imported(0x2000, 0x5000));
    // (a large import over a small one inside it: the scan down from the small one still finds the large one)
    imported.emplace(0x100000, Pool{0x100000, 0x900000, 1});
    imported.emplace(0x200000, Pool{0x200000, 0x201000, 1});
    live_reach.add(0x800000), live_reach.add(0x1000);
    assert(memory_guest_imported(0x300000, 0x301000));
    assert(!memory_guest_imported(0x900000, 0x901000));
}
"""
    with tempfile.TemporaryDirectory() as tmp:
        cpp = Path(tmp) / "check.cpp"
        exe = Path(tmp) / "check.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-I" + str(ROOT / "gpu/vk"), str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], cwd=tmp, check=True)


if __name__ == "__main__":
    main()
