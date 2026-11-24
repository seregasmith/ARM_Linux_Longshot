"""A renderer-owned simulated-turf list never holds more records than one draw or dispatch of the device can read.

python -I -B tests/test_sim_turf_group_limit.py [--cxx clang++]

Cuts `sim_turf_max_records` out of gpu/vk/vk_draw.cpp and builds it, with a stand-in for the device, against the real
gpu/vk/sim_turf_capacity.h. The game's programs turn a list's record count into workgroups of one indirect command, 32
records a group for a pool (its mesh draw) and 64 for a blade list (the pool producer's dispatch), both capped only by the
list's length. The length came from the storage buffer range alone, so at wide turf ranges a pool of 2,795,584 records asked
for 70,195 groups of a device whose maxMeshWorkGroupCount[0] is 65,535 (issue 2251). The check: whatever the demand, the list
the policy grows to fits the limit of the path that reads it: the native mesh draw's for a pool, the compute dispatch's for
a blade list and for a pool drawn through the compute fallback; and a device with room is given as many records as before.

The limit itself is read in create_device: its property query is cut out of gpu/vk/vk_main.cpp too and run over stand-in
structures, so a limit nobody asks the driver for, or asks for where the extension is off, fails here as well.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from srccut import between, block  # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r"""
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "sim_turf_capacity.h"

namespace Shader::Optimization {
inline constexpr uint32_t kSimTurfBladeCap = 143360;
inline constexpr uint32_t kSimTurfPoolCap = 138240;
inline constexpr uint32_t kSimTurfListHeader = 64;
}
struct Limits { uint32_t maxStorageBufferRange = UINT32_MAX; uint32_t maxComputeWorkGroupCount[3] = {65535, 65535, 65535}; };
struct Device {
    struct { Limits limits; } props;
    uint32_t max_mesh_groups = UINT32_MAX;
    bool fallback = false, mesh_shader = false;
    void* physical = nullptr;
    bool uses_mesh_fallback(bool) const { return fallback; }
} dev;
namespace mkwvk {
@CUT@
}

// Vulkan's property structures, as far as the query names them: the driver fills the ones in the chain
enum { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES = 1, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
       VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
       VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
struct Link { int sType = 0; void* pNext = nullptr; };
struct VkPhysicalDeviceFloatControlsProperties { int sType = 0; void* pNext = nullptr; };
struct VkPhysicalDeviceDriverProperties { int sType = 0; void* pNext = nullptr; };
struct VkPhysicalDeviceSubgroupSizeControlProperties { int sType = 0; void* pNext = nullptr; };
struct VkPhysicalDeviceSubgroupProperties { int sType = 0; void* pNext = nullptr; };
struct VkPhysicalDeviceProperties2 { int sType = 0; void* pNext = nullptr; };
struct VkPhysicalDeviceMeshShaderPropertiesEXT {
    int sType = 0;
    void* pNext = nullptr;
    uint32_t maxMeshWorkGroupCount[3] = {};
    uint32_t maxMeshWorkGroupTotalCount = 0;
};
static uint32_t driver_x, driver_total;
static int asked;
void vkGetPhysicalDeviceProperties2(void*, VkPhysicalDeviceProperties2* p) {
    for (Link* l = reinterpret_cast<Link*>(p); l; l = static_cast<Link*>(l->pNext))
        if (l->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT) {
            auto* m = reinterpret_cast<VkPhysicalDeviceMeshShaderPropertiesEXT*>(l);
            m->maxMeshWorkGroupCount[0] = driver_x, m->maxMeshWorkGroupTotalCount = driver_total, asked++;
        }
}
static uint32_t queried(bool mesh, uint32_t x, uint32_t total) {   // dev.max_mesh_groups after create_device's query
    dev.max_mesh_groups = UINT32_MAX, dev.mesh_shader = mesh, driver_x = x, driver_total = total, asked = 0;
    {
@QUERY@
    }
    return dev.max_mesh_groups;
}

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)

// the groups the game's program asks for once the list has grown for a demand past every cap
static uint64_t groups_at_most(bool pool) {
    const uint32_t stock = pool ? Shader::Optimization::kSimTurfPoolCap : Shader::Optimization::kSimTurfBladeCap;
    const uint32_t group = pool ? 32 : 64;
    mkwvk::sim_turf_capacity::Policy policy(stock, group, mkwvk::sim_turf_max_records(pool, 64), stock);
    CHECK(policy.observe(UINT32_MAX, 1));
    policy.installed();
    return ((uint64_t)policy.capacity() + group - 1) / group;
}
static uint32_t by_range(bool pool) {   // what the storage buffer range alone leaves
    const uint32_t stride = pool ? 48 : 8, group = pool ? 32 : 64;
    return (UINT32_MAX - 64 - 64) / stride / group * group;
}

int main() {
    // the limit asked of the driver: the smaller of the x and the total, and only with the extension on
    CHECK(queried(true, 65535, 4194304) == 65535);   // (main: never asked)
    CHECK(asked == 1);
    CHECK(queried(true, 4194304, 1000000) == 1000000);
    CHECK(queried(false, 65535, 4194304) == UINT32_MAX && asked == 0);
    dev.mesh_shader = false, dev.max_mesh_groups = UINT32_MAX;
    // AMD: 65535 groups a mesh draw. The reported scene's demand asked for 70,195
    dev.max_mesh_groups = 65535;
    CHECK(groups_at_most(true) == 65535);   // (main: 2.7 million)
    CHECK(mkwvk::sim_turf_max_records(true, 64) == 2097120);
    // the blade list is read by a dispatch: the compute limit, whatever the mesh draw's
    dev.max_mesh_groups = 4194304;
    CHECK(groups_at_most(false) == 65535);
    // the compute fallback dispatches the pool's groups: the compute limit, not the mesh draw's
    dev.fallback = true, dev.max_mesh_groups = UINT32_MAX;
    CHECK(groups_at_most(true) == 65535);
    // a device with room keeps the range's size (RADV's compute dispatch; NVIDIA's mesh draws: 4,194,304 groups)
    dev.props.limits.maxComputeWorkGroupCount[0] = UINT32_MAX;
    CHECK(mkwvk::sim_turf_max_records(true, 64) == by_range(true));
    CHECK(mkwvk::sim_turf_max_records(false, 64) == by_range(false));
    dev.fallback = false, dev.max_mesh_groups = 4194304;
    CHECK(mkwvk::sim_turf_max_records(true, 64) == by_range(true));
    // a small storage range is still the smaller of the two
    dev.max_mesh_groups = 65535, dev.props.limits.maxStorageBufferRange = 1u << 24;
    CHECK(mkwvk::sim_turf_max_records(true, 64) == ((1u << 24) - 128) / 48 / 32 * 32);
    std::puts("PASS: simulated-turf lists fit one draw or dispatch of the device");
}
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    draw = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
    cut = block(draw, "uint32_t sim_turf_max_records(bool pool, uint64_t salign) {")
    device = (ROOT / "gpu" / "vk" / "vk_main.cpp").read_text(encoding="utf-8")
    query = between(device, "VkPhysicalDeviceFloatControlsProperties fc{", "const bool pin = ")
    with tempfile.TemporaryDirectory(prefix="sim-turf-groups-") as folder:
        src = Path(folder) / "sim_turf_groups.cpp"
        src.write_text(HARNESS.replace("@CUT@", cut).replace("@QUERY@", query), encoding="utf-8")
        exe = Path(folder) / "sim_turf_groups.exe"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-I" + str(ROOT / "gpu" / "vk"), str(src), "-o", str(exe)],
                       check=True)
        env = os.environ.copy()
        env["PATH"] = str(Path(args.cxx).resolve().parent) + os.pathsep + env.get("PATH", "")
        subprocess.run([str(exe)], check=True, env=env)


if __name__ == "__main__":
    main()
