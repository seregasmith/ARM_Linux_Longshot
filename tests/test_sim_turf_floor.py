"""A renderer-owned simulated-turf list never shrinks below the game's own size: an area without turf leaves it ready.

python -I -B tests/test_sim_turf_floor.py [--cxx clang++]

Cuts `SimTurfPrivate` out of gpu/vk/vk_draw.cpp, builds it against the real gpu/vk/sim_turf_capacity.h with stand-ins for
Vulkan's handles and the IR pass's caps, and plays Market then Hyrule Field at it: 700 frames with no demand, then
Hyrule Field's 17013 blades and 10069 pool records. A list released to 0 records in Market came back to Hyrule Field
empty and grew list by list, and each growth's first write into the game's records stalled the replay 250 ms
(a capture's wait for write-back that timed out) while the GPU ran one submission for seconds. The check: the blade list
and the pool stay at their stock caps through the quiet window, and Hyrule Field's demand fits them without a resize.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from srccut import block  # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r"""
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "sim_turf_capacity.h"

using VkBuffer = void*;
using VmaAllocation = void*;
#define VK_NULL_HANDLE nullptr
namespace Shader::Optimization {
inline constexpr uint32_t kSimTurfBladeCap = 143360;
inline constexpr uint32_t kSimTurfPoolCap = 138240;
}
namespace mkwvk {
@CUT@;
}

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)

int main() {
    using mkwvk::SimTurfPrivate;
    for (const bool pool : {false, true}) {
        const uint32_t stock = pool ? 138240u : 143360u;
        SimTurfPrivate g(pool, 1u << 20);
        CHECK(g.policy.capacity() == stock);
        // Market: no simulated turf for 700 frames. A resize the policy asks for is installed, as vk_draw.cpp does
        for (uint64_t frame = 1; frame <= 700; frame++)
            if (g.policy.observe(0, frame)) g.policy.installed();
        CHECK(g.policy.capacity() == stock);   // (main: 0 records, the list empty when the turf comes back)
        // Hyrule Field: its demand fits the list as it is, so nothing is made again and nothing is written for the first time
        CHECK(!g.policy.observe(pool ? 10069u : 17013u, 701));
        CHECK(g.policy.capacity() == stock);
    }
    std::puts("PASS: simulated-turf lists keep the game's size through an area without turf");
}
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    draw = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
    cut = block(draw, "struct SimTurfPrivate {")
    with tempfile.TemporaryDirectory(prefix="sim-turf-floor-") as folder:
        src = Path(folder) / "sim_turf_floor.cpp"
        src.write_text(HARNESS.replace("@CUT@", cut), encoding="utf-8")
        exe = Path(folder) / "sim_turf_floor.exe"
        subprocess.run([args.cxx, "-std=c++20", "-I" + str(ROOT / "gpu" / "vk"), str(src), "-o", str(exe)], check=True)
        env = os.environ.copy()
        env["PATH"] = str(Path(args.cxx).resolve().parent) + os.pathsep + env.get("PATH", "")
        subprocess.run([str(exe)], check=True, env=env)


if __name__ == "__main__":
    main()
