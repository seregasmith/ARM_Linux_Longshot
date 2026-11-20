# SPDX-License-Identifier: GPL-3.0-or-later
"""RX_VK_LOG_PIPELINE_RAM (gpu/vk/vk_draw.cpp pipeline_ram_*, gpu/vk/spirv_shape.h, gpu/vk/vk_memory.cpp
driver_outside_heap_now): what each pipeline costs the driver in memory of its own, outside its device heap.

On the AYN Odin 3 the driver held 2.8 GB outside the device heap: 1.5 GiB of it one compute pipeline of the title screen (a
4.2 MB SPIR-V; a near twin made after it took 2.5 MB), 373 MB another: Turnip's private memory, one buffer for the device sized
for the hungriest shader so far. With the switch on, the Linux guards read the driver's count less the heap's before and after
each pipeline, one pipeline at a time. The actual code is compiled here, with a scripted count:
- spirv_shape::read: a module's Private and Function variables (an invocation's private memory), Workgroup ones, its workgroup
  size (LocalSize, or LocalSizeId's constants), and nothing from a cut-short or foreign module;
- driver_outside_heap_now: the driver's count less the heap's, the heap read again when it moved meanwhile, nothing when either
  is not known;
- a pipeline's cost is the count after less the count before, put in its size's bucket and in the total;
- one of 1 MB or more gets its line (the first 300; every one of 8 MB or more), with its thread, the program named for it
  (pipeline_ram_name: only when the module is the one named, a stale name is not taken) and its shaders: stage, SPIR-V size,
  hash, private memory an invocation, shared memory, workgroup;
- a named program whose pipeline took 8 MB or more is kept once, and the game's first draw or dispatch with it said once;
- every 1000 weighed, a summary by size (a sum below 0 said as 0 MB);
- a refused pipeline or a count the kernel did not give is not counted;
- the lock is let go after every weighing, refused or not (a kept lock would leave every later pipeline waiting), and a
  fault's release does nothing once let go;
- pipelines weighed from four threads at once: each one's two reads together, nobody else's between them.
No GPU or driver required.

python -I -B tests/test_pipeline_ram.py --headers <Vulkan-Headers/include> [--cxx g++]
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]
RUN_ALL_SKIP = {"windows": "the weighing reads Linux's fdinfo: Linux only"}

ROOT = Path(__file__).resolve().parents[1]
START = "// (longshot, RX_VK_LOG_PIPELINE_RAM=1, Linux)"
END = "// (longshot) A driver's compiler that faults on a shader"


def spirv(local_id=False, words=2048):
    """A compute module: a Private array of 4096 struct{uint} (16384 bytes), a Function vec4 (16), a Workgroup array (16384);
    LocalSize 64x2x1, or LocalSizeId 128x1x1; OpNop to words"""
    def ins(op, *ops):
        return [((len(ops) + 1) << 16) | op, *ops]
    w = [0x07230203, 0x00010300, 0, 30, 0]
    w += ins(17, 1) + ins(14, 0, 1) + ins(15, 5, 1, 0x6E69616D, 0)
    w += ins(331, 1, 38, 20, 21, 22) if local_id else ins(16, 1, 17, 64, 2, 1)
    w += ins(19, 2) + ins(21, 3, 32, 0) + ins(43, 3, 4, 4096)
    if local_id:
        w += ins(43, 3, 20, 128) + ins(43, 3, 21, 1) + ins(43, 3, 22, 1)
    w += ins(30, 5, 3) + ins(28, 6, 5, 4) + ins(32, 7, 6, 6) + ins(59, 7, 8, 6)
    w += ins(22, 9, 32) + ins(23, 10, 9, 4) + ins(32, 11, 7, 10) + ins(32, 12, 4, 6) + ins(59, 12, 13, 4)
    w += ins(33, 14, 2) + ins(54, 2, 1, 0, 14) + ins(248, 15) + ins(59, 11, 16, 7) + [(1 << 16) | 253, (1 << 16) | 56]
    return w + [1 << 16] * (words - len(w))


def fnv(words):
    h = 2166136261
    for b in b"".join(x.to_bytes(4, "little") for x in words):
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def c_array(name, words):
    return "static const uint32_t %s[] = {%s};\n" % (name, ", ".join("0x%08xu" % x for x in words))


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
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <pthread.h>
#include "spirv_shape.h"
bool env_on(const char* name) { return !std::strcmp(name, "RX_VK_LOG_PIPELINE_RAM"); }
/* (the driver's count outside its heap, scripted: each read takes the next; in the threaded part each read says who read it) */
static std::vector<int64_t> counts;
static size_t count_at;
static std::atomic<bool> threaded{false};
static std::atomic<int> read_n{0};
static std::thread::id readers[4096];
int64_t driver_outside_heap_now() {
    if (threaded) {
        const int i = read_n.fetch_add(1);
        if (i < 4096) readers[i] = std::this_thread::get_id();
        std::this_thread::sleep_for(std::chrono::microseconds(30));
        return 1 << 30;
    }
    return count_at < counts.size() ? counts[count_at++] : -1;
}
static std::vector<std::string> lines;
void logf(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    lines.push_back(buf);
}
"""

MAIN = r"""
static int failed;
static void need(bool ok, const char* what) {
    if (!ok) { std::printf("fail: %s\n", what); ++failed; }
}
static void need_line(size_t at, const std::string& want, const char* what) {
    const bool ok = lines.size() > at && lines[at] == want;
    need(ok, what);
    if (!ok) std::printf("  got:  %s\n  want: %s\n", lines.size() > at ? lines[at].c_str() : "(no line)", want.c_str());
}
/* (from another thread: whether the weighing's lock is free; a kept one ends the test before a later weighing waits on it) */
static bool lock_free() {
    bool free_ = false;
    std::thread t([&] { if (pipeline_ram_mu.try_lock()) free_ = true, pipeline_ram_mu.unlock(); });
    t.join();
    return free_;
}
template <class Info>
static void weigh(const Info* ci, std::vector<int64_t> reads, VkResult r = VK_SUCCESS) {
    counts = reads;
    count_at = 0;
    if (!pipeline_ram_begin()) { std::printf("fail: not weighing with the switch on\n"); std::exit(2); }
    pipeline_ram_end(ci, r);
    if (!lock_free()) { std::printf("fail: the weighing's lock kept after a pipeline\n"); std::exit(3); }
}
static size_t big_lines() {
    size_t n = 0;
    for (const std::string& l : lines) n += l.rfind("vk: RAM, a ", 0) == 0;
    return n;
}

int main() {
    // ---- spirv_shape::read ----
    {
        const spirv_shape::Shape a = spirv_shape::read(kSpirv, sizeof kSpirv / 4);
        need(a.ok && a.private_bytes == 16384 + 16 && a.function_vars == 1 && a.shared_bytes == 16384,
             "a module's private memory (its Private and Function variables) and shared memory");
        need(a.local[0] == 64 && a.local[1] == 2 && a.local[2] == 1, "the workgroup from LocalSize");
        const spirv_shape::Shape b = spirv_shape::read(kSpirvIds, sizeof kSpirvIds / 4);
        need(b.ok && b.local[0] == 128 && b.local[1] == 1 && b.local[2] == 1 && b.private_bytes == 16400,
             "the workgroup from LocalSizeId's constants");
        std::vector<uint32_t> cut(kSpirv, kSpirv + 40);
        cut[39] = (9u << 16) | 59;   // (an instruction running past the end)
        need(!spirv_shape::read(cut.data(), cut.size()).ok, "a cut-short module is not read");
        std::vector<uint32_t> other(kSpirv, kSpirv + sizeof kSpirv / 4);
        other[0] = 0x12345678;
        need(!spirv_shape::read(other.data(), other.size()).ok, "a module that is not SPIR-V is not read");
    }

    pthread_setname_np(pthread_self(), "weigher");
    VkShaderModuleCreateInfo mi{};
    mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mi.codeSize = sizeof kSpirv;
    mi.pCode = kSpirv;
    const VkShaderModule vs = reinterpret_cast<VkShaderModule>(uintptr_t(0x10)), fs = reinterpret_cast<VkShaderModule>(uintptr_t(0x20));
    pipeline_ram_module(&mi, vs);
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT, st[0].module = vs;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, st[1].module = fs;
    VkGraphicsPipelineCreateInfo gi{};
    gi.stageCount = 2, gi.pStages = st;
    VkComputePipelineCreateInfo ci{};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;   // (no module: an identifier or inline code)
    const int64_t MB = 1 << 20, base = 1000 * MB;
    const std::string unnamed = "no program named: the renderer's own, or made elsewhere";

    weigh(&gi, {base, base + 3 * MB});   // (#1: 3 MB, its line)
    char want[600];
    std::snprintf(want, sizeof want, "vk: RAM, a graphics pipeline took 3072 KB of the driver's memory outside its heap (#1 "
                  "weighed, on weigher; %s; its shaders: vs 8 KB %08x, private 16400 bytes an invocation, shared 16384 bytes, "
                  "workgroup 64x2x1, fs (its module made before the weighing))", unnamed.c_str(), (unsigned)EXPECT_HASH);
    need_line(0, want, "a pipeline of 1 MB or more: its line, with its thread and its shaders' private memory");
    weigh(&ci, {base, base + 2 * MB});   // (#2: 2 MB, a compute pipeline with no module, no program named)
    need_line(1, "vk: RAM, a compute pipeline took 2048 KB of the driver's memory outside its heap (#2 weighed, on weigher; " +
              unnamed + "; its shaders: cs (no module: an identifier or inline code))", "a compute pipeline no program was named for");
    pipeline_ram_name(0xabc, VK_NULL_HANDLE, "the game's compute program");
    weigh(&ci, {base, base + 20 * MB});   // (#3: 20 MB, named: kept)
    need_line(2, "vk: RAM, a compute pipeline took 20480 KB of the driver's memory outside its heap (#3 weighed, on weigher; "
              "program 0000000000000abc, the game's compute program; its shaders: cs (no module: an identifier or inline code))",
              "a compute pipeline's program named in its line");
    need(pipeline_ram_heavy_n.load() == 1 && pipeline_ram_heavy[0].bytes == 20 * MB && pipeline_ram_heavy[0].no == 3,
         "a named program whose pipeline took 8 MB or more is kept");
    pipeline_ram_used(0xabc, "dispatch");
    need_line(3, "vk: RAM, program 0000000000000abc: the game's first dispatch with it (its pipeline, #3 weighed, took 20 MB of the "
              "driver's memory outside its heap; the game's compute program)", "the game's first dispatch with a kept program");
    pipeline_ram_used(0xabc, "draw");
    pipeline_ram_used(0x999, "dispatch");
    need(lines.size() == 4, "a kept program's first use said once, another program's not at all");
    pipeline_ram_name(0xdef, vs, "the game's compute program");   // (named for another module: stale for this pipeline)
    weigh(&ci, {base, base + 9 * MB});   // (#4: 9 MB, not named)
    need(lines.size() == 5 && lines[4].find(unnamed) != std::string::npos, "a name given for another module is not taken");
    need(pipeline_ram_heavy_n.load() == 1, "a pipeline with no program named is not kept");
    weigh(&ci, {base, base + 10 * 1024});   // (#5: 10 KB, no line; the stale name was let go at #4)
    weigh(&gi, {base, base - MB / 2});   // (#6: less than before, another thread's frees between the reads: noise)
    need(lines.size() == 5, "a pipeline under 1 MB has no line");
    need(pipeline_ram.n == 6, "six pipelines weighed");
    need(pipeline_ram_bytes.load() == 3 * MB + 2 * MB + 20 * MB + 9 * MB + 10 * 1024 - MB / 2,
         "the total: each pipeline's after less its before");
    need(pipeline_ram.count[0] == 1 && pipeline_ram.count[1] == 1 && pipeline_ram.count[4] == 2 && pipeline_ram.count[5] == 2,
         "the buckets by size");

    weigh(&gi, {base, base + 5 * MB}, VK_ERROR_OUT_OF_HOST_MEMORY);   // (refused: not counted, its after not read)
    need(pipeline_ram.n == 6 && count_at == 1, "a refused pipeline is not counted");
    weigh(&gi, {-1, base});   // (the kernel gave no count before)
    weigh(&gi, {base, -1});   // (nor after)
    need(pipeline_ram.n == 6 && lines.size() == 5, "a pipeline without the driver's count is not counted");
    pipeline_ram_fault();   // (a fault's release once let go: nothing)
    need(lock_free(), "a fault's release after the weighing let go keeps the lock free");

    for (int i = 0; i < 994; i++) weigh(&ci, {base, base});
    need_line(5, "vk: RAM, pipelines weighed: 1000, 33 MB of the driver's memory outside its heap in all: none or less 995 (0 MB), "
              "under 64 KB 1 (0 MB), 64-256 KB 0 (0 MB), 256 KB-1 MB 0 (0 MB), 1-8 MB 2 (5 MB), 8 MB or more 2 (29 MB)",
              "the summary every 1000, by size");
    for (int i = 0; i < 400; i++) weigh(&gi, {base, base + 2 * MB});
    need(big_lines() == 300, "the first 300 pipelines of 1 MB or more have their lines, no more");
    weigh(&gi, {base, base + 9 * MB});
    need(big_lines() == 301, "a pipeline of 8 MB or more has its line past the first 300");
    pipeline_ram_name(0xabc, VK_NULL_HANDLE, "the game's compute program");
    weigh(&ci, {base, base + 30 * MB});
    need(big_lines() == 302 && pipeline_ram_heavy_n.load() == 1 && pipeline_ram_heavy[0].bytes == 20 * MB,
         "a program kept once, at its first pipeline of 8 MB or more");

    threaded = true;
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; t++)
        ts.emplace_back([&] {
            for (int i = 0; i < 200; i++) {
                if (!pipeline_ram_begin()) return;
                pipeline_ram_end(&ci, VK_SUCCESS);
            }
        });
    for (std::thread& t : ts) t.join();
    threaded = false;
    const int reads = read_n.load();
    need(reads == 1600, "four threads' 800 pipelines, two reads each");
    int mixed = 0;
    for (int i = 0; i + 1 < reads && i + 1 < 4096; i += 2) mixed += readers[i] != readers[i + 1];
    need(mixed == 0, "a pipeline's two reads together, another thread's between them");
    if (mixed) std::printf("  %d of 800 pipelines had another thread's read between theirs\n", mixed);
    need(lock_free(), "the lock free after the threads");
    if (!failed) std::printf("ok: %d pipelines weighed\n", (int)pipeline_ram.n);
    return failed ? 1 : 0;
}
"""

# driver_outside_heap_now, cut from vk_memory.cpp, with a scripted heap and driver count
OUTSIDE_HEAD = r"""
#include <cstdint>
#include <cstdio>
#include <vector>
static bool heap_known = true;
static std::vector<uint64_t> heaps;
static std::vector<int64_t> totals;
static size_t heap_at, total_at, fresh;
bool vram_usage(uint64_t* used, uint64_t* budget, uint64_t* = nullptr) {
    *used = heaps.empty() ? 0 : heaps[0], *budget = 1;
    return heap_known;
}
bool vram_usage_now(uint64_t* used, uint64_t* budget) {
    ++fresh;
    *used = heap_at < heaps.size() ? heaps[heap_at++] : heaps.back(), *budget = 1;
    return true;
}
int64_t driver_memory_now() { return total_at < totals.size() ? totals[total_at++] : -1; }
"""

OUTSIDE_MAIN = r"""
static int failed;
static int64_t outside(std::vector<uint64_t> h, std::vector<int64_t> t, bool known = true) {
    heaps = h, totals = t, heap_at = total_at = fresh = 0, heap_known = known;
    return driver_outside_heap_now();
}
static void need(bool ok, const char* what) {
    if (!ok) { std::printf("fail: %s\n", what); ++failed; }
}
int main() {
    need(outside({300, 300}, {1000}) == 700 && fresh == 2, "the driver's count less the heap's, the heap read fresh around it");
    // (a 10 MB texture made between the first heap read and the driver's count: in the count, not in that heap read)
    need(outside({300, 310, 310, 310}, {1010, 1010}) == 700, "a heap that moved meanwhile read again");
    need(outside({1, 2, 3, 4, 5, 6, 7, 8, 9}, {10, 20, 30, 40}) == -1, "a heap that kept moving: nothing");
    need(outside({300, 300}, {-1}) == -1, "no driver count: nothing");
    need(outside({300, 300}, {1000}, false) == -1 && total_at == 0, "no heap yet: nothing, the driver's count not read");
    if (!failed) std::printf("ok: the driver's memory outside its heap\n");
    return failed ? 1 : 0;
}
"""


def body(src, head):
    start = src.index(head)
    return src[start:src.index("\n}\n", start) + 3]


def check_source(src, mem):
    # (the guards: weighed between the fault window's edges, the weighing let go in each Linux fault branch; the guards'
    # own test, test_linux_driver_fault_guard.py, runs them with a lock that a kept weighing would show)
    linux = src[src.index("return (VkResult)-77;   // (a fault in the driver, told apart from what it returns)"):]
    linux = linux[:linux.index("\n#endif\nVkPipeline build_graphics")]
    for call in ("vkCreateGraphicsPipelines", "vkCreateComputePipelines"):
        at = linux.index("const VkResult r = %s(" % call)
        before, after = linux[:at], linux[at:]
        if not before.rstrip().endswith("const bool weigh = pipeline_ram_begin();   // (RX_VK_LOG_PIPELINE_RAM)\n    fault_armed = 1;"):
            raise SystemExit("the Linux guard of %s does not begin the weighing just before the driver" % call)
        if after.split("\n", 2)[1:2] != ["    fault_armed = 0;"] or "    if (weigh) pipeline_ram_end(ci, r);\n    return r;" not in after[:400]:
            raise SystemExit("the Linux guard of %s does not end the weighing just after the driver" % call)
    if linux.count("pipeline_ram_fault();") != 2:
        raise SystemExit("the Linux guards' fault branches do not each let the weighing go")
    for needle, what in (("if (r == VK_SUCCESS) pipeline_ram_module(ci, *m);", "guarded_shader_module no longer notes its modules"),
                         ("int64_t pipeline_ram_total() { return pipeline_ram_bytes.load(std::memory_order_relaxed); }",
                          "pipeline_ram_total no longer gives the weighed total")):
        if needle not in src:
            raise SystemExit(what)
    info = body(src, "VkComputePipelineCreateInfo compute_pipeline_info(const Program& q, bool noopt) {")
    if 'pipeline_ram_name(q.key, ci.stage.module, "the game\'s compute program");' not in info:
        raise SystemExit("compute_pipeline_info no longer names the program for the weighing")
    by_id = body(src, "VkPipeline compute_pipeline_by_module_id(")
    if by_id.index("pipeline_ram_name(p.key, VK_NULL_HANDLE,") < by_id.index("ci.stage.module = VK_NULL_HANDLE;"):
        raise SystemExit("compute_pipeline_by_module_id names the program before its module is let go")
    if src.count('pipeline_ram_name(p.key, si.module, "a task or mesh program\'s compute fallback");') != 1:
        raise SystemExit("the mesh fallback's compute stages are no longer named for the weighing")
    for how in ("draw", "dispatch"):
        if src.count('pipeline_ram_heavy_n.load(std::memory_order_relaxed)) pipeline_ram_used(cp->key, "%s");' % how) != 1:
            raise SystemExit("the game's first %s with a heavy pipeline's program is no longer said (or not behind one load)" % how)
    now = body(mem, "int64_t driver_outside_heap_now() {")
    if "smaps" in now or "placed_mu" in now:
        raise SystemExit("driver_outside_heap_now reads more than the driver's count and the heap's")
    if "const int64_t weighed = pipeline_ram_total();" not in mem:
        raise SystemExit("ram_driver_log no longer says what the pipelines weighed took")
    return now


def build_run(cxx, flags, source, tmp, name):
    cpp, exe = Path(tmp) / (name + ".cpp"), Path(tmp) / name
    cpp.write_text(source, encoding="utf-8")
    build = subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-pthread", *flags, str(cpp), "-o", str(exe)],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if build.returncode:
        raise SystemExit("%s did not compile:\n%s" % (name, build.stdout))
    if build.stdout.strip():
        raise SystemExit("%s compiled with warnings:\n%s" % (name, build.stdout))
    run = subprocess.run([str(exe)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=120)
    print(run.stdout.strip())
    return run.returncode == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--headers", type=Path, required=True)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--source", type=Path, default=ROOT / "gpu/vk/vk_draw.cpp")
    parser.add_argument("--memory", type=Path, default=ROOT / "gpu/vk/vk_memory.cpp")
    args = parser.parse_args()
    src = args.source.read_text(encoding="utf-8")
    mem = args.memory.read_text(encoding="utf-8")
    now = check_source(src, mem)
    block = src[src.index(START):src.index(END)]
    module = spirv()
    arrays = c_array("kSpirv", module) + c_array("kSpirvIds", spirv(local_id=True, words=64))
    ok = True
    with tempfile.TemporaryDirectory(prefix="pipeline-ram-") as tmp:
        ok &= build_run(args.cxx, ["-DEXPECT_HASH=0x%08xu" % fnv(module), "-I" + str(args.headers), "-I" + str(ROOT / "gpu/vk")],
                        HEAD + arrays + block + MAIN, tmp, "pipeline_ram")
        ok &= build_run(args.cxx, [], OUTSIDE_HEAD + now + OUTSIDE_MAIN, tmp, "outside_heap")
    if not ok:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
