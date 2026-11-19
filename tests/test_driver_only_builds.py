"""Builds made only for the driver's own shader cache go through a pipeline cache of their own where Turnip's cache is the store
(gpu/vk/vk_draw.cpp DriverOnlyBuilds, pcache_driver_builds from pcache_load.h's rule), and into the one pipeline cache as before
everywhere else.

Built into that one cache, every pipeline's shaders stayed there, with what the driver keeps of each, for the whole process,
and were never written where the file is neither read nor written: the preparation's 36,449 pipelines left a 15 GB AYN Odin 3
86 MB available. This cuts DriverOnlyBuilds out of vk_draw.cpp and runs it on stand-ins for Vulkan:
 - off (the rule's other answers): no cache made, builds go to pcache, compute builds through cached_compute, the thread's
   driver-only asking untouched;
 - on: a cache made and let go at the end of the scope, graphics builds handed it (driver_cache_only, driver_ask_cache, as
   build_graphics reads them) and the thread's asking put back as it was, even nested; compute builds handed it too;
 - a cache that cannot be made: as off.
And checks the warm, the preparation's compute pipelines and the load at create use it, and that pcache_load_off sets the flag
from the rule. No game or GPU.
python -I -B tests/test_driver_only_builds.py [--cxx g++]
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
#include <cstdio>
typedef struct VkPipelineCache_T* VkPipelineCache;
typedef struct VkPipeline_T* VkPipeline;
typedef int VkResult;
#define VK_NULL_HANDLE nullptr
#define VK_SUCCESS 0
#define VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO 17
struct VkPipelineCacheCreateInfo { int sType; };
struct VkComputePipelineCreateInfo { int sType; };
struct { void* device = nullptr; } dev;
static VkPipelineCache pcache = (VkPipelineCache)0x1000;
static bool pcache_driver_builds = false;
thread_local bool driver_cache_only = false;
thread_local VkPipelineCache driver_ask_cache = VK_NULL_HANDLE;
static int made = 0, destroyed = 0, fail_create = 0, guarded = 0, cached = 0;
static VkPipelineCache last_guarded = nullptr;
static VkResult vkCreatePipelineCache(void*, const VkPipelineCacheCreateInfo*, const void*, VkPipelineCache* out) {
    if (fail_create) return -1;
    *out = (VkPipelineCache)(unsigned long long)(0x2000 + 16 * ++made);
    return VK_SUCCESS;
}
static void vkDestroyPipelineCache(void*, VkPipelineCache c, const void*) { if (c) destroyed++; }
static VkResult guarded_compute_held(VkPipelineCache c, const VkComputePipelineCreateInfo*, VkPipeline*) {
    guarded++, last_guarded = c;
    return VK_SUCCESS;
}
static VkResult cached_compute(const VkComputePipelineCreateInfo*, VkPipeline*) { cached++; return VK_SUCCESS; }
%s
static int fails = 0;
#define NEED(c, what) do { if (!(c)) { fails++; std::printf("FAIL: %%s\n", what); } } while (0)
int main() {
    VkComputePipelineCreateInfo ci{};
    VkPipeline pl = VK_NULL_HANDLE;
    {   // off: pcache, as before
        DriverOnlyBuilds own;
        NEED(!own.on() && own.cache() == pcache && made == 0, "off: no cache of its own, pcache");
        NEED(!driver_cache_only && driver_ask_cache == VK_NULL_HANDLE, "off: the thread's asking untouched");
        own.compute(&ci, &pl);
        NEED(cached == 1 && guarded == 0, "off: compute builds through cached_compute");
    }
    NEED(destroyed == 0, "off: nothing let go");
    pcache_driver_builds = true;
    {   // on: a cache of its own, handed to the builds, let go after
        DriverOnlyBuilds own;
        NEED(own.on() && own.cache() != pcache && made == 1, "on: a cache of its own");
        NEED(driver_cache_only && driver_ask_cache == own.cache(), "on: graphics builds handed it (build_graphics)");
        own.compute(&ci, &pl);
        NEED(guarded == 1 && last_guarded == own.cache() && cached == 1, "on: compute builds handed it");
        {
            DriverOnlyBuilds inner;
            NEED(driver_ask_cache == inner.cache() && inner.cache() != own.cache(), "nested: the inner one's");
        }
        NEED(driver_cache_only && driver_ask_cache == own.cache(), "nested: the outer one's asking put back");
    }
    NEED(destroyed == 2 && made == 2, "on: each cache let go at the end of its scope");
    NEED(!driver_cache_only && driver_ask_cache == VK_NULL_HANDLE, "on: the thread's asking put back as it was");
    driver_cache_only = true;   // (predict_run's builds ask the driver alone with no cache)
    {
        DriverOnlyBuilds own;
        NEED(driver_ask_cache == own.cache(), "inside a driver-only builder: its own cache");
    }
    NEED(driver_cache_only && driver_ask_cache == VK_NULL_HANDLE, "a driver-only builder's asking put back as it was");
    driver_cache_only = false;
    fail_create = 1;
    {
        DriverOnlyBuilds own;
        NEED(!own.on() && own.cache() == pcache && !driver_cache_only, "no cache could be made: pcache, as before");
    }
    NEED(destroyed == 3, "nothing let go that was not made");
    if (fails) return 1;
    std::printf("ok\n");
    return 0;
}
"""


def block(src, head, end):
    if src.count(head) != 1:
        raise SystemExit("FAIL: vk_draw.cpp has no single " + head)
    at = src.index(head)
    return src[at:src.index(end, at) + len(end)]


def check_source(draw):
    problems = []
    warm = block(draw, "void precompile_warm(void (*progress)(size_t done, size_t total)) {", "\n}\n")
    borrow = warm[warm.index("program_cache_borrow(progs[i]"):]
    if "DriverOnlyBuilds own;" not in borrow or borrow.index("DriverOnlyBuilds own;") > borrow.index("build_graphics("):
        problems.append("precompile_warm's graphics builds do not go through DriverOnlyBuilds")
    if "own.compute(&ci, &made)" not in warm:
        problems.append("precompile_warm's compute builds do not go through DriverOnlyBuilds")
    prep = block(draw, "const auto warm_compute = [&](Program& p) {", "\n    };\n")
    if "guarded_compute_held(own.cache(), &ci, &pl)" not in prep or "DriverOnlyBuilds own;" not in prep:
        problems.append("the preparation's compute pipelines are still built into pcache whatever the rule says")
    off = block(draw, "bool pcache_load_off() {", "\n}\n")
    if "pcache_driver_builds = !choice.load && pcache_load::builds_into_driver(choice.why);" not in off:
        problems.append("pcache_load_off does not set pcache_driver_builds from the rule")
    at_create = block(draw, "void precompile_at_create() {", "\n}\n")
    runs = at_create.split("warm_bounded::run(")[1:]
    if len(runs) != 2 or any(", held_now, [&](size_t) {\n        memory.sample();" not in r for r in runs) or \
            at_create.count("THREAD_PRIORITY_BELOW_NORMAL") != 2:
        problems.append("precompile_at_create's workers are not held while memory is short, or not below normal")
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    args = parser.parse_args()
    cxx = shutil.which(args.cxx)
    if not cxx:
        raise SystemExit("C++ compiler not found: " + args.cxx)
    draw = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
    if "struct DriverOnlyBuilds {" not in draw:
        raise SystemExit("FAIL: vk_draw.cpp has no DriverOnlyBuilds: what is built for the driver's cache alone is held in the "
                         "pipeline cache for the whole process, where Turnip's cache is the store")
    problems = check_source(draw)
    with tempfile.TemporaryDirectory(prefix="longshot-driver-only-") as folder:
        tmp = Path(folder)
        (tmp / "t.cpp").write_text(HARNESS % block(draw, "struct DriverOnlyBuilds {", "\n};\n"), encoding="utf-8")
        r = subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", str(tmp / "t.cpp"),
                            "-o", str(tmp / "t")], capture_output=True, text=True, timeout=120)
        if r.returncode:
            raise SystemExit("FAIL: DriverOnlyBuilds does not compile\n" + r.stdout + r.stderr)
        out = subprocess.run([str(tmp / "t")], capture_output=True, text=True, timeout=30)
        if out.returncode:
            problems.append(out.stdout.strip())
    if problems:
        raise SystemExit("FAIL:\n  " + "\n  ".join(problems))
    print("PASS: builds for the driver's cache alone go through caches of their own where Turnip's cache is the store, into "
          "the pipeline cache as before elsewhere")


if __name__ == "__main__":
    main()
