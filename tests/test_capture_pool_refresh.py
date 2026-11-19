"""Check capture pool enumeration, expiry and range invalidation. Run python -I -B tests/test_capture_pool_refresh.py."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from srccut import block


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default="c++")
    args = parser.parse_args()
    source = (ROOT / "gpu/vk/vk_memory.cpp").read_text()
    harness = r"""
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <utility>
#include <vector>
using Range = std::pair<uint64_t, uint64_t>;
static uint64_t captures_made, pool_cap_built, cap_ranges_ver = 1, expiry;
static uint32_t pool_cap_gen, generation = 1;
static unsigned scans;
static std::vector<Range> pool_cap_ranges;
static std::unordered_map<uint64_t, uint64_t> pool_first_seen;
static std::vector<Range> pools = {{0x2000, 0x1000}};
static uint64_t capture_pool_max() { return 1u << 20; }
static uint64_t eager_captures() { return expiry; }
struct Host {
    uint32_t maps_generation() { return generation; }
    int gpu_pools(uint64_t (*out)[2], int max) {
        ++scans;
        assert(pools.size() <= (unsigned)max);
        for (size_t i = 0; i < pools.size(); ++i) { out[i][0] = pools[i].first; out[i][1] = pools[i].second; }
        return (int)pools.size();
    }
} instance, *host = &instance;
"""
    cases = r"""
int main(int argc, char**) {
    expiry = argc > 1 ? 64 : 0;
    refresh_pool_ranges_locked();
    assert(pool_cap_ranges.size() == 1);
    auto version = cap_ranges_ver;
    captures_made = 64;
    refresh_pool_ranges_locked();
    if (expiry) {
        assert(scans == 2 && pool_cap_ranges.empty() && cap_ranges_ver != version);
        pools.clear(); ++generation; refresh_pool_ranges_locked();
        pools.emplace_back(0x2000,0x1000); ++generation; refresh_pool_ranges_locked();
        assert(pool_cap_ranges.size() == 1);
    } else {
        if (scans != 1) { std::fprintf(stderr, "re-enumerated unchanged pools without expiry\n"); return 1; }
        assert(cap_ranges_ver == version);
        ++generation; refresh_pool_ranges_locked();
        assert(scans == 2 && cap_ranges_ver == version);
        pools.emplace_back(0x3000,0x1000); ++generation; refresh_pool_ranges_locked();
        assert(pool_cap_ranges.size() == 1 && pool_cap_ranges.front() == Range(0x2000,0x4000));
        assert(cap_ranges_ver != version);
        pools.clear(); ++generation; refresh_pool_ranges_locked();
        assert(pool_cap_ranges.empty());
    }
}
"""
    with tempfile.TemporaryDirectory() as temp:
        fixture = Path(temp) / "refresh.cpp"
        fixture.write_text(harness + block(source, "void refresh_pool_ranges_locked()") + cases)
        exe = fixture.with_suffix(".exe")
        subprocess.run([args.cxx, "-std=c++17", "-O2", str(fixture), "-o", str(exe)], check=True)
        for mode in ([], ["expiry"]):
            subprocess.run([str(exe), *mode], check=True)


if __name__ == "__main__":
    main()
