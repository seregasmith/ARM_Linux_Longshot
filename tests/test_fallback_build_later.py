# SPDX-License-Identifier: GPL-3.0-or-later
"""Which mesh fallback programs' compute pipelines are built before the title, and which wait for their first draw
(gpu/vk/vk_draw.cpp: precompile_at_create's and precompile_run's fallback builds, fallback_pipelines_ready).

On a GPU without mesh shaders every task and mesh program runs as compute shaders, and the start built every one's compute
pipelines before the title. On Turnip (AYN Odin 3, Adreno 830) a shader's private memory, its register spills, is one buffer
for the whole device, sized for the hungriest shader made so far times every invocation the GPU can hold, and never shrunk.
The 80,000-phi mesh program 93e3e999f2fb9fd8 grew it to 1.5 GiB, 5c9454152ebde335 (16,837 phis) took 373 MB more: about 1.9 GB
the kernel cannot swap, before the title, for programs a Kokiri Forest session never drew. So on Turnip, outside a run that
only prepares the shaders, a fallback program with a compute stage of more than 10,000 phis is left to its first draw
(RX_VK_FALLBACK_LATER_PHIS sets the limit on any driver; 0: every one before the title, as before).

The actual loops are cut from vk_draw.cpp and run over made-up programs (the two above, a 253 KB one, one at the limit, a
small one), with the driver, the run and the switch given in each case: the programs built before the title, how many each
loop counts as left to their first draw, and that both still count every program done (the bar's total). Then
fallback_pipelines_ready, as a draw calls it, must build a program left out, saying so with the time it took; with the build
queued, as on a worker, the first draw is left out while it compiles and the next one draws. And program_cache_drop_modules
(vk_progcache.cpp) must keep a fallback program's compute modules until its pipelines are built, which is what lets the first
draw build them. No game, GPU or driver.

python -I -B tests/test_fallback_build_later.py --headers <Vulkan-Headers/include> [--cxx g++]
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]
RUN_ALL_SKIP = {"windows": "setenv and the cut loops' POSIX stand-ins: the rule itself is the same on every OS"}

ROOT = Path(__file__).resolve().parents[1]

HEAD = r"""
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "fallback_size_policy.h"
struct { VkDriverId driver_id = VK_DRIVER_ID_MESA_TURNIP; } dev;
static bool env_on(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}
static std::vector<std::string> lines;
static void logf(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    lines.push_back(buf);
}
constexpr int log_debug = 1;
static void logf_at(int, const char*, ...) {}
struct Stage {
    VkShaderStageFlagBits vk_stage = VK_SHADER_STAGE_COMPUTE_BIT;
    uint32_t phis = 0;
};
struct Program {
    uint64_t key = 0;
    std::vector<Stage> stages;
    std::atomic<int> compute_state{0};
    std::vector<int> fallback_compute;
};
static std::vector<uint64_t> built;   // (the programs fallback_compute_pipelines was called for, in order)
static bool fallback_compute_pipelines(Program& p) {
    built.push_back(p.key);
    p.fallback_compute = {1};
    return true;
}
// (fallback_pipelines_ready's: its job run at once, as a worker would)
struct ParLock { ParLock() {} ~ParLock() {} };
static void pcache_init() {}
static bool serial_builds_on() { return false; }
static bool queued = false;   // (the builds queued, as on a worker, until the test runs them)
static std::vector<std::function<void()>> queue;
static void jobs_push(std::function<void()> f, bool, void*, std::function<void()>) {
    if (queued) queue.push_back(f);
    else f();
}
static void jobs_push_serial(std::function<void()> f, std::function<void()> g) { jobs_push(f, true, nullptr, g); }
static std::atomic<int> st_pending_skips{0};
static bool prepare_stopped() { return false; }
"""

MAIN = r"""
struct Pass {
    std::vector<Program*> fallbacks;
    std::atomic<size_t> next{0}, done{0}, later{0};
    std::atomic<bool> gone{false};
};
static void start_builds(Pass* pass) {   // (precompile_at_create's worker, its fallback part)
    const auto taking = [pass] { return !prepare_stopped() && !pass->gone.load(std::memory_order_relaxed); };
    CUT_AT_CREATE
}
struct Pre {
    std::atomic<size_t> done{0}, later{0};
};
static void run_builds(std::vector<Program*>& fallbacks, Pre* pre) {   // (precompile_run's worker, its fallback part)
    std::vector<int> computes;
    for (size_t i = 0; i < fallbacks.size(); i++) {
        if (i < computes.size()) {
        } CUT_RUN
        pre->done++;
    }
}
int main(int argc, char** argv) {
    // argv: the driver (turnip or radv), the path (create or run), "queued" for the first draws' builds on a worker
    if (argc > 1 && !std::strcmp(argv[1], "radv")) dev.driver_id = VK_DRIVER_ID_MESA_RADV;
    const bool at_create = argc < 3 || !std::strcmp(argv[2], "create");
    queued = argc > 3 && !std::strcmp(argv[3], "queued");
    Program big, second, mid, edge, small;
    big.key = 0x93e3e999f2fb9fd8ull, big.stages = {{VK_SHADER_STAGE_COMPUTE_BIT, 79947}, {VK_SHADER_STAGE_FRAGMENT_BIT, 12}};
    second.key = 0x5c9454152ebde335ull, second.stages = {{VK_SHADER_STAGE_COMPUTE_BIT, 120}, {VK_SHADER_STAGE_COMPUTE_BIT, 16837}};
    mid.key = 0x59ccdc4c3ca0f737ull, mid.stages = {{VK_SHADER_STAGE_COMPUTE_BIT, 9000}};
    edge.key = 0xed6eull, edge.stages = {{VK_SHADER_STAGE_COMPUTE_BIT, 10000}};   // (at the limit: not over it)
    small.key = 0x1a84ull, small.stages = {{VK_SHADER_STAGE_COMPUTE_BIT, 300}, {VK_SHADER_STAGE_FRAGMENT_BIT, 50000}};
    std::vector<Program*> all{&big, &second, &mid, &edge, &small};
    size_t done = 0, later = 0;
    if (at_create) {
        Pass pass;
        pass.fallbacks = all;
        start_builds(&pass);
        done = pass.done, later = pass.later;
    } else {
        Pre pre;
        run_builds(all, &pre);
        done = pre.done, later = pre.later;
    }
    if (done != all.size()) { std::printf("fail: %zu of %zu programs counted done\n", done, all.size()); return 3; }
    std::printf("left to their first draw: %zu\n", later);
    std::printf("before the title:");
    for (const uint64_t k : built) std::printf(" %016llx", (unsigned long long)k);
    std::printf("\n");
    for (Program* p : all)
        if (p->compute_state.load() == 2 && p->fallback_compute.empty()) { std::printf("fail: %016llx marked built with nothing\n", (unsigned long long)p->key); return 4; }
    // (a draw of each: one left out of the start's builds is built at its first draw, and drawn; queued, the first draw is
    // left out while it compiles, and the next draws)
    const size_t before = built.size();
    if (queued) {
        for (Program* p : all)
            if (p->compute_state.load() == 0 && fallback_pipelines_ready(*p)) {
                std::printf("fail: %016llx drawn while its build is queued\n", (unsigned long long)p->key);
                return 6;
            }
        for (auto& job : queue) job();
        queue.clear();
    }
    for (Program* p : all)
        if (!fallback_pipelines_ready(*p)) { std::printf("fail: %016llx not ready at its first draw\n", (unsigned long long)p->key); return 5; }
    std::printf("at their first draw:");
    for (size_t i = before; i < built.size(); i++) std::printf(" %016llx", (unsigned long long)built[i]);
    std::printf("\n");
    for (const std::string& l : lines) std::printf("log: %s\n", l.c_str());
    return 0;
}
"""

BIG, SECOND, MID, EDGE, SMALL = "93e3e999f2fb9fd8", "5c9454152ebde335", "59ccdc4c3ca0f737", "000000000000ed6e", "0000000000001a84"
ALL = [BIG, SECOND, MID, EDGE, SMALL]


def braced(src, start):
    """From start to the brace closing the first one opened after it"""
    at = src.index("{", start) + 1
    depth = 1
    while depth:
        depth += (src[at] == "{") - (src[at] == "}")
        at += 1
    return src[start:at]


def function(src, head):
    start = src.index(head)
    return src[start:src.index("\n}\n", start) + 3]


def build(cxx, headers, src, tmp):
    at_create = src.index("for (size_t i; taking() && (i = pass->next++) < pass->fallbacks.size();) {")
    loop = braced(src, at_create)
    branch_at = src.index("else if (i < computes.size() + fallbacks.size()) {")
    branch = src[branch_at:src.index("} else {\n                PipeNote& n", branch_at)] + "}"
    rule = function(src, "bool fallback_build_later(const Program& p) {") if "bool fallback_build_later(" in src else ""
    ready = function(src, "bool fallback_pipelines_ready(Program& p) {")
    main = MAIN.replace("CUT_AT_CREATE", loop).replace("CUT_RUN", branch)
    cpp, exe = Path(tmp) / "later.cpp", Path(tmp) / "later"
    cpp.write_text(HEAD + rule + ready + main, encoding="utf-8")
    r = subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Wno-unused-function", "-I" + str(headers),
                        "-I" + str(ROOT / "gpu/vk"), str(cpp), "-o", str(exe)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True)
    if r.returncode or r.stdout.strip():
        raise SystemExit("the fallback builds did not compile cleanly:\n" + r.stdout)
    return exe


def run(exe, driver, path, env, mode):
    r = subprocess.run([str(exe), driver, path, mode], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=30,
                       env=env)
    out = r.stdout.splitlines()
    if r.returncode:
        raise SystemExit("%s %s %s: %s" % (driver, path, mode, r.stdout.strip()))
    counted = int(next(l for l in out if l.startswith("left to their first draw:")).split()[-1])
    ahead = next(l for l in out if l.startswith("before the title:")).split()[3:]
    later = next(l for l in out if l.startswith("at their first draw:")).split()[4:]
    return ahead, later, counted, [l[5:] for l in out if l.startswith("log: ")]


def check_drop(progcache):
    """program_cache_drop_modules lets a fallback program's compute modules go only once its pipelines are built (compute_state
    2): a program left to its first draw (0) keeps them, and fallback_compute_pipelines builds from them"""
    fn = function(progcache, "size_t program_cache_drop_modules() {")
    guard = "if (p->fallback && p->compute_state.load(std::memory_order_acquire) == 2) {"
    at = fn.find(guard)
    if at < 0 or fn.find("st.vk_stage == VK_SHADER_STAGE_COMPUTE_BIT", at) > fn.find("\n            }\n", at):
        raise SystemExit("program_cache_drop_modules: a fallback program's compute modules let go before its pipelines are built "
                         "(no longer under: " + guard + "): a program left to its first draw could not build them")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--headers", type=Path, required=True)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--source", type=Path, default=ROOT / "gpu/vk/vk_draw.cpp")
    args = parser.parse_args()
    src = args.source.read_text(encoding="utf-8")
    check_drop((ROOT / "gpu/vk/vk_progcache.cpp").read_text(encoding="utf-8"))
    base = {"PATH": "/usr/bin:/bin"}
    # (driver, path, environment) -> the programs built before the title; the rest at their first draw
    default = [MID, EDGE, SMALL]
    cases = [
        ("turnip", "create", {}, default, "Turnip, a game's start: the two biggest left to their first draw", ""),
        ("turnip", "create", {}, default, "Turnip, the first draws' builds on a worker", "queued"),
        ("turnip", "run", {}, default, "Turnip, RX_VK_LOAD=start's precompile: the same", ""),
        ("turnip", "run", {}, default, "Turnip, RX_VK_LOAD=start's precompile, the builds on a worker", "queued"),
        ("turnip", "create", {"LONGSHOT_PREPARE_ONLY": "1"}, ALL, "Turnip, a run that only prepares the shaders: every one", ""),
        ("turnip", "create", {"LONGSHOT_PREPARE_ONLY": "1", "RX_VK_FALLBACK_LATER_PHIS": "5000"}, ALL,
         "Turnip, preparing the shaders with the switch set: every one still", ""),
        ("turnip", "create", {"RX_VK_FALLBACK_LATER_PHIS": "0"}, ALL, "Turnip, RX_VK_FALLBACK_LATER_PHIS=0: every one, as before", ""),
        ("turnip", "create", {"RX_VK_FALLBACK_LATER_PHIS": "5000"}, [SMALL], "Turnip, RX_VK_FALLBACK_LATER_PHIS=5000", ""),
        ("turnip", "create", {"RX_VK_FALLBACK_LATER_PHIS": "lots"}, default, "Turnip, a value that is not a number: the default", ""),
        ("radv", "create", {}, ALL, "RADV: every one, as before", ""),
        ("radv", "run", {}, ALL, "RADV, RX_VK_LOAD=start's precompile: every one", ""),
        ("radv", "create", {"RX_VK_FALLBACK_LATER_PHIS": "10000"}, default, "RADV, RX_VK_FALLBACK_LATER_PHIS=10000", ""),
        ("radv", "create", {"RX_VK_FALLBACK_LATER_PHIS": "-1"}, ALL, "RADV, a value that is not a number: none left", ""),
    ]
    failures = []
    with tempfile.TemporaryDirectory(prefix="fallback-later-") as tmp:
        exe = build(args.cxx, args.headers, src, tmp)
        for driver, path, env, want, what, mode in cases:
            ahead, later, counted, log = run(exe, driver, path, {**base, **env}, mode)
            rest = [k for k in ALL if k not in want]
            ok = ahead == want and sorted(later) == sorted(rest) and counted == len(rest)
            print("%s %s: %s; before the title %s, at the first draw %s (%d counted)" % ("ok  " if ok else "FAIL", what,
                  "as wanted" if ok else "wanted %s before, %d counted" % (want, len(rest)), " ".join(ahead) or "none",
                  " ".join(later) or "none", counted))
            if not ok:
                failures.append(what)
            value = env.get("RX_VK_FALLBACK_LATER_PHIS")
            if value in ("lots", "-1") and not any("RX_VK_FALLBACK_LATER_PHIS=%s is not a number" % value in l for l in log):
                failures.append(what + ": no line about the value ignored")
            if rest and not any("more than" in l and "phis are built at their first draw" in l for l in log):
                failures.append(what + ": no line saying which programs wait for their first draw")
            for key in rest:
                if not any(l.startswith("vk: fallback: %s's compute pipelines built at its first draw in " % key) for l in log):
                    failures.append(what + ": no line for %s built at its first draw" % key)
    if failures:
        raise SystemExit("FAIL:\n  " + "\n  ".join(failures))
    print("ok: the biggest mesh fallback programs wait for their first draw on Turnip, and are built there")


if __name__ == "__main__":
    main()
