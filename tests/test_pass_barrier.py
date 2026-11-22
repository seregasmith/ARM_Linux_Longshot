"""The barrier a draw pass starts with (gpu/vk/vk_texture.cpp pass_start_barrier) and the work counts it is decided by
(gpu/vk/vk_work.h). No game, no GPU.

A draw pass starts with a full barrier (every stage to every stage) where anything but draw passes was recorded on its
command buffer since this thread's last full barrier, or where that barrier went into another command buffer; otherwise,
only draw passes having come since, with a barrier whose source is every stage's writes and whose destination is the pass's
fragment-side stages, so its vertex, task and mesh stages start while the passes before it finish. RX_VK_PASS_BARRIER=full
gives the full barrier at every pass start. What "anything but draw passes" is comes from vk_work.h: each vkCmd* it wraps
counts as work (note_work: barriers, the passes' begins and ends, draws, their attachments' clears and queries) or as other
work (note_other_work: dispatches, copies, blits, resolves, whole images' clears, fills and updates, query results copied),
and the upscalers' libraries (NVIDIA NGX, AMD FidelityFX, Intel XeSS), which record into the command buffer themselves,
are counted as other work by hand. The counts are each recording thread's own (the render thread's, any other recording thread's).

Checked:
 - the sources: each vkCmd* macro in vk_work.h counts and then calls the driver (through the recorder's packer,
   vk_recorder.h), as work or other work by the list above, and the state-only and uncounted ones it wraps count nothing;
   every vkCmd* a file that includes vk_work.h calls is wrapped, or sets state only, or is one of the few known to write
   nothing a pass reads (timestamps, query resets, AMD's breadcrumbs); vk_work.h is that file's last include; no header
   calls vkCmd*; the files that call vkCmd* without it are the known ones; every library recording (NGX's create and
   evaluate, DLSS and frame generation, ffxDispatch, xessVKExecute) is followed by note_other_work(), and every
   note_other_work() written by hand follows one; no note_work() is written by hand (it would hide other work);
   texture_begin_recording forgets the last full barrier's command buffer (a new recording's may have its handle);
 - compiled, full_barrier and pass_start_barrier as vk_texture.cpp has them with vk_work.h's counts and macros, over a
   driver that records each barrier's stages: the first pass full; passes after passes fragment-side, again and again; a
   dispatch, a copy, a fill or a library's work between: full; queries and attachment clears inside a pass: still
   fragment-side; another command buffer: full, and full again back on the first; a full barrier right before a pass: the
   pass's barrier left out (nothing recorded since), the next fragment-side; a new recording: full; another thread's
   dispatch on its own command buffer changes nothing on this one; every fragment-side barrier waits for every stage's writes and
   holds back the fragment stages alone; with RX_VK_PASS_BARRIER=full every pass start is full;
 - and the checks have teeth: broken copies (other work ignored, another command buffer trusted, a dispatch counted as a draw
   pass's work, the full barrier not clearing the other work, the counts shared between threads, the vertex stages held
   back, the source narrowed, the switch ignored) must each fail.

python -I -B tests/test_pass_barrier.py --headers <Vulkan-Headers/include> [--cxx clang++]
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from srccut import between, block   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"

# (the commands of a draw pass, and the barriers: note_work. Every other wrapped command is other work)
DRAW_PASS = {"vkCmdPipelineBarrier2", "vkCmdBeginRendering", "vkCmdEndRendering", "vkCmdDraw", "vkCmdDrawIndexed",
             "vkCmdDrawIndirect", "vkCmdDrawIndexedIndirect", "vkCmdDrawIndirectCount", "vkCmdDrawIndexedIndirectCount",
             "vkCmdDrawMeshTasksEXT", "vkCmdDrawMeshTasksIndirectEXT", "vkCmdDrawMeshTasksIndirectCountEXT",
             "vkCmdClearAttachments", "vkCmdBeginQuery", "vkCmdEndQuery"}
OTHER_WORK = {"vkCmdDispatch", "vkCmdDispatchIndirect", "vkCmdCopyBuffer", "vkCmdCopyBufferToImage", "vkCmdCopyImageToBuffer",
              "vkCmdCopyImage", "vkCmdBlitImage", "vkCmdResolveImage", "vkCmdClearColorImage", "vkCmdClearDepthStencilImage",
              "vkCmdFillBuffer", "vkCmdUpdateBuffer", "vkCmdCopyQueryPoolResults"}
# (unwrapped calls that set state only: what the next draw or dispatch uses, no memory written)
STATE_ONLY = re.compile(r"^vkCmd(Bind|Push|Set)\w*$")
# (unwrapped calls that write, but nothing a pass reads, and why)
NOT_COUNTED = {
    "vkCmdWriteTimestamp2": "the GPU timings' timestamps (RX_VK_PASS_TIMING, the frame's GPU time): a query pool no pass reads",
    "vkCmdResetQueryPool": "the timestamps' query pools reset before they are written",
    "vkCmdWriteBufferMarkerAMD": "AMD's breadcrumbs (a device lost's report): read by the CPU after the fact",
}
# (files that call vkCmd* without vk_work.h, and why that is right)
UNWRAPPED_OK = {
    "vk_coopmat.cpp": "the cooperative-matrix probe at start: its own command pool, buffer, submit and fence wait",
    "vk_imgui.cpp": "Dear ImGui's overlays and its font upload, recorded last into the frame's command buffer (vk_menu.cpp "
                    "menu_record): no pass of the game's after them",
}

# (files whose ::vkx::vkCmd* calls are the driver's, made from behind vk_work.h's macros, so counted already)
DRIVER_CALLS_OK = {
    "vk_recorder.h": "the recorder's packers (rx_*), which each vkCmd* macro of vk_work.h calls after its count: they call the "
                     "driver now, or defer the call in order (RX_VK_RECORDER)",
}

C_COMMENT_OR_STRING = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)
NGX_RECORD_CALL = re.compile(r"\bNGX_(?:VULKAN_\w+_EXT|VK_\w+_DLSSG)\s*\(")   # (NVIDIA NGX's create/evaluate: DLSS's, frame generation's)
FFX_RECORD_CALL = re.compile(r"\bl\s*\.\s*dispatch\s*\(")   # (AMD FidelityFX's ffxDispatch, through its loaded pointer)
XESS_RECORD_CALL = re.compile(r"\bl\s*\.\s*execute\s*\(")   # (Intel XeSS's xessVKExecute, through its loaded pointer)
LIBRARY_CALL = re.compile(NGX_RECORD_CALL.pattern + "|" + FFX_RECORD_CALL.pattern + "|" + XESS_RECORD_CALL.pattern)
LIBRARY_FILES = {"vk_dlss.cpp": NGX_RECORD_CALL, "vk_fsr4.cpp": FFX_RECORD_CALL, "vk_xess.cpp": XESS_RECORD_CALL}
NOTE_OTHER = re.compile(r"(?<![\w:])(?:::\s*mkwvk\s*::\s*)?note_other_work\s*\(\s*\)\s*;")
NOTE_WORK = re.compile(r"(?<![\w:])(?:::\s*mkwvk\s*::\s*)?note_work\s*\(\s*\)\s*;")
NEAR = 600   # (characters: the few statements after a library's call, or before a hand-written count)


def code(text):
    """the text with its comments blanked and its string literals kept, newlines kept"""
    return C_COMMENT_OR_STRING.sub(lambda m: m.group(0) if m.group(0)[0] in "\"'" else re.sub(r"[^\n]", " ", m.group(0)), text)


def library_problems(text, name, call):
    """A library's recording not followed by note_other_work(), a hand-written note_other_work() after no library call this
    check knows, a hand-written note_work()"""
    c = code(text)
    bad = []
    line = lambda at: c.count("\n", 0, at) + 1
    sites = list(call.finditer(c)) if call else []
    if call and not sites:
        bad.append("%s: no library recording found: update this check's patterns" % name)
    for m in sites:
        if not NOTE_OTHER.search(c[m.end():m.end() + NEAR]):
            bad.append("%s:%d: a library's recording into the command buffer without note_other_work() after it: the next "
                       "pass would start with the fragment-side barrier after the library's dispatches" % (name, line(m.start())))
    for m in NOTE_OTHER.finditer(c):
        if not LIBRARY_CALL.search(c[max(0, m.start() - NEAR):m.start()]):
            bad.append("%s:%d: note_other_work() after a recording this check does not know: add it to LIBRARY_CALL" %
                       (name, line(m.start())))
    for m in NOTE_WORK.finditer(c):
        bad.append("%s:%d: note_work() written by hand: what it counts is not known to be a draw pass's; note_other_work()" %
                   (name, line(m.start())))
    return bad


def check_sources():
    bad = []
    work = (VK / "vk_work.h").read_text(encoding="utf-8")
    macros = dict(re.findall(r"^#define (vkCmd\w+)\(\.\.\.\) (.*)$", work, re.M))
    # (vk_work.h also wraps the state-only calls and the few NOT_COUNTED, for the recorder and the call counts: no count)
    unsorted = sorted(f for f in set(macros) - DRAW_PASS - OTHER_WORK - set(NOT_COUNTED) if not STATE_ONLY.match(f))
    if unsorted or not DRAW_PASS | OTHER_WORK <= set(macros):
        bad.append("vk_work.h's vkCmd* macros changed: only there %s, only here %s (say here which count each new one is)"
                   % (unsorted, sorted((DRAW_PASS | OTHER_WORK) - set(macros))))
    for f, body in macros.items():
        count = "note_work" if f in DRAW_PASS else "note_other_work" if f in OTHER_WORK else None
        # (the count, then the diagnostics' call count and GPU profile glue span, which record nothing, then the recorder's
        # packer, vk_recorder.h rx_*: the driver's call, now or deferred in order)
        shape = re.fullmatch(r"\((?:::mkwvk::(note_work|note_other_work)\(\), )?(?:::mkwvk::call_count\(::mkwvk::CK_\w+\), )?"
                             r"(?:MKWVK_GLUE\(\"%s\", __VA_ARGS__\), )?::mkwvk::rec::rx_%s\(__VA_ARGS__\)\)" % (f, f), body.strip())
        if not shape or shape.group(1) != count:
            bad.append("vk_work.h: %s is not counted as %s, then the driver's call: %s" % (f, count or "nothing", body))
    for want in ("extern thread_local uint64_t work_epoch;", "extern thread_local bool other_work_since_barrier;",
                 "inline void note_work() { work_epoch++; }",
                 "inline void note_other_work() { work_epoch++, other_work_since_barrier = true; }"):
        if want not in work:
            bad.append("vk_work.h: not found: " + want)
    texture = (VK / "vk_texture.cpp").read_text(encoding="utf-8")
    for want in ("thread_local uint64_t work_epoch = 0;", "thread_local bool other_work_since_barrier = true;",
                 "thread_local VkCommandBuffer last_barrier_cmd = VK_NULL_HANDLE;", "thread_local uint64_t last_barrier_epoch = 0;"):
        if want not in texture:
            bad.append("vk_texture.cpp: the counts or the last full barrier are not each thread's own: " + want)
    if "last_barrier_cmd = VK_NULL_HANDLE;" not in block(texture, "void texture_begin_recording(VkCommandBuffer upload, uint64_t value) {"):
        bad.append("vk_texture.cpp: texture_begin_recording does not forget the last full barrier's command buffer (a new "
                   "recording's may have its handle, and its first pass would trust it)")
    unwrapped = {}
    routed = 0
    for path in sorted(VK.glob("*.cpp")) + sorted(VK.glob("*.h")):
        if path.name in ("vk_work.h", "vk_loader.h"):
            continue
        text = code(path.read_text(encoding="utf-8", errors="replace"))
        includes_work = '#include "vk_work.h"' in text
        calls = C_COMMENT_OR_STRING.sub(lambda m: "" if m.group(0)[0] in "\"'" else m.group(0), text)   # (and no strings)
        if path.name in DRIVER_CALLS_OK:   # (its ::vkx:: calls are the driver's, behind vk_work.h's counts; any other is not)
            calls = re.sub(r"::\s*vkx\s*::\s*vkCmd\w+\s*\(", "(", calls)
        used = sorted(set(re.findall(r"\b(vkCmd\w+)\s*\(", calls)))
        if path.suffix == ".h":
            if used:
                bad.append("%s (a header) calls %s: move them into a .cpp that includes vk_work.h" % (path.name, used))
            continue
        if includes_work:
            routed += 1
            for c in used:
                if c not in macros and not STATE_ONLY.match(c) and c not in NOT_COUNTED:
                    bad.append("%s calls %s, which vk_work.h does not count: wrap it (work or other work), or say here why "
                               "it writes nothing a pass reads" % (path.name, c))
            if text.rfind('#include "vk_work.h"') != text.rfind("#include"):
                bad.append("%s: vk_work.h is not its last include" % path.name)
        elif used:
            unwrapped[path.name] = used
    for name, used in unwrapped.items():
        if name not in UNWRAPPED_OK:
            bad.append("%s calls %s without vk_work.h: not counted" % (name, used))
    for name in sorted(set(UNWRAPPED_OK) - set(unwrapped)):
        bad.append("%s no longer calls vkCmd* without vk_work.h: its entry in UNWRAPPED_OK can go" % name)
    for path in sorted(VK.glob("*.cpp")):
        bad += library_problems(path.read_text(encoding="utf-8"), path.name, LIBRARY_FILES.get(path.name))
    # (controls: each way of losing a library's count is caught)
    dlss = (VK / "vk_dlss.cpp").read_text(encoding="utf-8")
    first = dlss.index("note_other_work();")
    for what, broken in (("removed", dlss[:first] + dlss[first + len("note_other_work();"):]),
                         ("in a comment", dlss[:first] + "// " + dlss[first:]),
                         ("as note_work()", dlss[:first] + "note_work();" + dlss[first + len("note_other_work();"):])):
        if not library_problems(broken, "vk_dlss.cpp", NGX_RECORD_CALL):
            bad.append("the library check missed NGX's count " + what)
    if not library_problems("void f() {\n    some_sdk_record(cmd);\n    note_other_work();\n}\n", "control.cpp", None):
        bad.append("the library check missed a count after an unknown recording")
    return bad, routed, len(macros)


FIXTURE = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
namespace mkwvk {
bool env_on(const char*) { return false; }   // (RX_VK_BARRIER_DEDUPE_OFF unset: the dedupe on)
void logf(const char*, ...) {}
struct GprofSpan { GprofSpan(VkCommandBuffer, const char*) {} };
struct Recorded { VkCommandBuffer cmd; bool full; };
std::vector<Recorded> recorded;   // (the main thread's barriers, in order)
std::thread::id main_thread;
int fails, shape_errors;
}
namespace vkx {   // (the driver: the barriers recorded with their kind; the rest does nothing)
using namespace mkwvk;
constexpr VkPipelineStageFlags2 kGeometry = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
    VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT |
    VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
constexpr VkPipelineStageFlags2 kFragments = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
    VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
void vkCmdPipelineBarrier2(VkCommandBuffer cmd, const VkDependencyInfo* d) {
    if (d->memoryBarrierCount != 1 || d->bufferMemoryBarrierCount || d->imageMemoryBarrierCount) { shape_errors++; return; }
    const VkMemoryBarrier2& b = d->pMemoryBarriers[0];
    const bool full = b.dstStageMask == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    // (every one: every stage's writes before it; read and written after it)
    if (b.srcStageMask != VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT || !(b.srcAccessMask & VK_ACCESS_2_MEMORY_WRITE_BIT) ||
        (b.dstAccessMask & (VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT)) !=
            (VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT)) {
        std::printf("FAIL: a barrier does not wait for every stage's writes, or for reads and writes after it\n");
        shape_errors++;
    }
    // (the fragment-side one: the fragment stages held back, all of them, and nothing of the geometry)
    if (!full && ((b.dstStageMask & kFragments) != kFragments || (b.dstStageMask & kGeometry))) {
        std::printf("FAIL: a pass start's fragment-side barrier holds back %#llx, not the fragment stages alone\n",
                    (unsigned long long)b.dstStageMask);
        shape_errors++;
    }
    if (std::this_thread::get_id() == main_thread) recorded.push_back({cmd, full});
}
void vkCmdBeginRendering(VkCommandBuffer, const VkRenderingInfo*) {}
void vkCmdEndRendering(VkCommandBuffer) {}
void vkCmdDraw(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t) {}
void vkCmdClearAttachments(VkCommandBuffer, uint32_t, const VkClearAttachment*, uint32_t, const VkClearRect*) {}
void vkCmdBeginQuery(VkCommandBuffer, VkQueryPool, uint32_t, VkQueryControlFlags) {}
void vkCmdEndQuery(VkCommandBuffer, VkQueryPool, uint32_t) {}
void vkCmdDispatch(VkCommandBuffer, uint32_t, uint32_t, uint32_t) {}
void vkCmdCopyBuffer(VkCommandBuffer, VkBuffer, VkBuffer, uint32_t, const VkBufferCopy*) {}
void vkCmdFillBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, uint32_t) {}
}
namespace mkwvk::rec {   // (vk_recorder.h's packers, which vk_work.h's macros call after their count: the recorder off)
void rx_vkCmdPipelineBarrier2(VkCommandBuffer c, const VkDependencyInfo* d) { vkx::vkCmdPipelineBarrier2(c, d); }
void rx_vkCmdBeginRendering(VkCommandBuffer c, const VkRenderingInfo* r) { vkx::vkCmdBeginRendering(c, r); }
void rx_vkCmdEndRendering(VkCommandBuffer c) { vkx::vkCmdEndRendering(c); }
void rx_vkCmdDraw(VkCommandBuffer c, uint32_t a, uint32_t b, uint32_t d, uint32_t e) { vkx::vkCmdDraw(c, a, b, d, e); }
void rx_vkCmdClearAttachments(VkCommandBuffer c, uint32_t n, const VkClearAttachment* a, uint32_t m, const VkClearRect* r) {
    vkx::vkCmdClearAttachments(c, n, a, m, r);
}
void rx_vkCmdBeginQuery(VkCommandBuffer c, VkQueryPool p, uint32_t q, VkQueryControlFlags f) { vkx::vkCmdBeginQuery(c, p, q, f); }
void rx_vkCmdEndQuery(VkCommandBuffer c, VkQueryPool p, uint32_t q) { vkx::vkCmdEndQuery(c, p, q); }
void rx_vkCmdDispatch(VkCommandBuffer c, uint32_t x, uint32_t y, uint32_t z) { vkx::vkCmdDispatch(c, x, y, z); }
void rx_vkCmdCopyBuffer(VkCommandBuffer c, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy* r) { vkx::vkCmdCopyBuffer(c, s, d, n, r); }
void rx_vkCmdFillBuffer(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkDeviceSize n, uint32_t v) { vkx::vkCmdFillBuffer(c, b, o, n, v); }
}
namespace mkwvk {
@@WORK@@
// (the macros' glue span of the GPU profile, vk_gprof.cpp: off, as when RX_VK_PASS_TIMING is under 3)
bool gprof_glue_on() { return false; }
GprofGlue::GprofGlue(VkCommandBuffer c, const char* l) : cmd(c), label(l) {}
GprofGlue::~GprofGlue() {}
}
@@MACROS@@
namespace mkwvk {
@@BARRIERS@@

VkCommandBuffer handle(uintptr_t n) { return reinterpret_cast<VkCommandBuffer>(n); }
const VkCommandBuffer A = handle(0x10), B = handle(0x20), C = handle(0x30);
/// a draw pass on cmd: its start barrier, then a draw (inside: a query and an attachment clear, the pass's own work)
void pass(VkCommandBuffer cmd, bool inside = false) {
    pass_start_barrier(cmd);
    vkCmdBeginRendering(cmd, nullptr);
    if (inside) vkCmdBeginQuery(cmd, VK_NULL_HANDLE, 0, 0), vkCmdClearAttachments(cmd, 0, nullptr, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    if (inside) vkCmdEndQuery(cmd, VK_NULL_HANDLE, 0);
    vkCmdEndRendering(cmd);
}
/// what the last pass's start recorded: 'F' full, 'f' fragment-side, '-' nothing (left out)
char last(size_t before) {
    if (recorded.size() == before) return '-';
    if (recorded.size() != before + 1) return '?';
    return recorded.back().full ? 'F' : 'f';
}
void expect(VkCommandBuffer cmd, bool inside, char want, const char* what) {
    const size_t before = recorded.size();
    pass(cmd, inside);
    const char got = last(before);
    if (got != want) std::printf("FAIL: %s: the pass started with '%c', expected '%c' (F full, f fragment-side, - none)\n", what, got, want), fails++;
}
}

int main(int argc, char** argv) {
    using namespace mkwvk;
    main_thread = std::this_thread::get_id();
    const bool always_full = argc > 1 && !std::strcmp(argv[1], "full");
    const char F = 'F', f = always_full ? 'F' : 'f';
    expect(A, false, F, "the thread's first pass (nothing known)");
    expect(A, false, f, "a pass after a pass");
    expect(A, false, f, "and after that one");
    expect(A, true, f, "a pass whose query and attachment clear are its own");
    expect(A, false, f, "after it");
    vkCmdDispatch(A, 1, 1, 1);
    expect(A, false, F, "a dispatch between");
    expect(A, false, f, "passes after it again");
    vkCmdCopyBuffer(A, VK_NULL_HANDLE, VK_NULL_HANDLE, 0, nullptr);
    expect(A, false, F, "a copy between");
    vkCmdFillBuffer(A, VK_NULL_HANDLE, 0, 4, 0);
    expect(A, false, F, "a fill between");
    note_other_work();   // (an upscaler library's dispatches, counted by hand)
    expect(A, false, F, "a library's work between");
    expect(A, false, f, "a pass after that pass");
    expect(B, false, F, "another command buffer");
    expect(A, false, F, "back on the first: its last full barrier went into the other");
    expect(A, false, f, "and passes after that");
    full_barrier(A, "test");
    expect(A, false, '-', "a full barrier right before the pass: nothing recorded since, so left out");
    expect(A, false, f, "the pass after that");
    last_barrier_cmd = VK_NULL_HANDLE;   // (texture_begin_recording: a new recording)
    expect(A, false, F, "a new recording's first pass");
    expect(A, false, f, "its second");
    std::thread worker([] {   // (another recording thread: its own command buffer, its own counts)
        vkCmdDispatch(C, 1, 1, 1);
        pass(C);
        vkCmdDispatch(C, 1, 1, 1);
    });
    worker.join();
    expect(A, false, f, "a worker's dispatch on its own thread changes nothing here");
    if (shape_errors || fails) return 1;
    std::printf("PASS: pass start barriers%s\n", always_full ? " (RX_VK_PASS_BARRIER=full: every one full)" : "");
    return 0;
}
'''


def fixture_source():
    work = (VK / "vk_work.h").read_text(encoding="utf-8")
    texture = (VK / "vk_texture.cpp").read_text(encoding="utf-8")
    work_ns = between(work, "namespace mkwvk {\nextern thread_local uint64_t work_epoch;", "} // namespace mkwvk")[len("namespace mkwvk {"):]
    macros = "\n".join(line for line in work.splitlines() if line.startswith(("#define MKWVK_", "#define vkCmd")))
    # (the last full barrier's command buffer is declared further up, before upload_astc_gpu, which forgets it)
    barriers = (between(texture, "thread_local VkCommandBuffer last_barrier_cmd = VK_NULL_HANDLE;", "\n") + "\n" +
                between(texture, "thread_local uint64_t work_epoch = 0;", "void texture_begin_recording("))
    for want in ("void full_barrier(VkCommandBuffer cmd, const char* why) {", "void pass_start_barrier(VkCommandBuffer cmd) {"):
        if want not in barriers:
            raise SystemExit("FAIL: vk_texture.cpp: %s is no longer between the counts and texture_begin_recording" % want)
    return FIXTURE.replace("@@WORK@@", work_ns).replace("@@MACROS@@", macros).replace("@@BARRIERS@@", barriers)


# Broken copies, each of which the compiled check must catch: (what, the text, the text in its place)
BROKEN = [
    ("other work ignored", "if (always_full || other_work_since_barrier || cmd != last_barrier_cmd ||",
     "if (always_full || cmd != last_barrier_cmd ||"),
    ("another command buffer trusted", "other_work_since_barrier || cmd != last_barrier_cmd || work_epoch",
     "other_work_since_barrier || work_epoch"),
    ("a dispatch counted as a draw pass's work", "#define vkCmdDispatch(...) (::mkwvk::note_other_work(),",
     "#define vkCmdDispatch(...) (::mkwvk::note_work(),"),
    ("the full barrier not clearing the other work", "    other_work_since_barrier = false;\n", "\n"),
    ("the counts shared between threads", "thread_local bool other_work_since_barrier", "bool other_work_since_barrier"),
    ("the vertex stages held back", "b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |",
     "b.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |"),
    ("the source narrowed", "    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;\n",
     "    b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;\n"),
    ("RX_VK_PASS_BARRIER=full ignored", 'const bool on = e && !strcmp(e, "full");', 'const bool on = e && !strcmp(e, "never");'),
]


def build(cxx, headers, text, path):
    path.with_suffix(".cpp").write_text(text, encoding="utf-8")
    exe = path.with_suffix(".exe")
    cmd = [cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers", "-Wno-unused-parameter",
           "-Wno-unused-function", "-pthread", "-I" + str(headers), str(path.with_suffix(".cpp")), "-o", str(exe)]
    if os.name == "nt":
        cmd.append("-static")
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    if r.returncode:
        raise SystemExit("FAIL: %s did not build\n%s%s" % (path.name, r.stdout, r.stderr))
    return exe


def run(exe, full):
    env = {k: v for k, v in os.environ.items() if k != "RX_VK_PASS_BARRIER"}
    if full:
        env["RX_VK_PASS_BARRIER"] = "full"
    return subprocess.run([str(exe)] + (["full"] if full else []), capture_output=True, text=True, timeout=30, env=env)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="clang++")
    p.add_argument("--headers", required=True, type=Path)
    a = p.parse_args()
    bad, routed, wrapped = check_sources()
    if bad:
        raise SystemExit("FAIL: " + "\nFAIL: ".join(bad))
    print("PASS sources: %d vkCmd* counted by vk_work.h, every call in its %d files counted or known to need no count, the "
          "libraries' recordings counted as other work" % (wrapped, routed))
    source = fixture_source()
    with tempfile.TemporaryDirectory(prefix="pass-barrier-") as tmp:
        exe = build(a.cxx, a.headers, source, Path(tmp) / "current")
        for full in (False, True):
            r = run(exe, full)
            print(r.stdout, end="")
            if r.returncode:
                raise SystemExit("FAIL: pass start barriers%s\n%s" % (" with RX_VK_PASS_BARRIER=full" if full else "", r.stderr))
        for i, (what, old, new) in enumerate(BROKEN):
            if source.count(old) < 1 or (old != "thread_local bool other_work_since_barrier" and source.count(old) != 1):
                raise SystemExit("FAIL: broken copy %r: its text is not in the fixture once" % what)
            broken = build(a.cxx, a.headers, source.replace(old, new), Path(tmp) / ("broken%d" % i))
            full = what.startswith("RX_VK_PASS_BARRIER")
            if run(broken, full).returncode == 0:
                raise SystemExit("FAIL: broken copy %r passed" % what)
        print("PASS: %d broken copies caught" % len(BROKEN))
    print("PASS: a draw pass waits in its fragment stages after only draw passes, else a full barrier")


if __name__ == "__main__":
    main()
