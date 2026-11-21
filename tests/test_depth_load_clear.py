"""Exercise depth clear recording at pass start and across a copy boundary with game-free Vulkan stubs."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from srccut import block

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]
DRAW = Path(__file__).resolve().parents[1] / "gpu/vk/vk_draw.cpp"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--headers", required=True)
    args = parser.parse_args()
    source = DRAW.read_text(encoding="utf-8")
    fixture = """
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#define _Static_assert static_assert
#include "rx_nvn.h"
#include <string>
#include <vector>
struct Texture {};
struct { VkDriverId driver_id = VK_DRIVER_ID_MESA_RADV; } dev;
struct ParLock { ParLock(bool = true) {} };
struct NvnState { int scissor[4]{}; };
"""
    fixture += block(source, "struct Targets {") + ";\n"
    fixture += block(source, "struct Pass {", unique=False) + ";\n"
    fixture += """
Pass ps;
Pass& pass() { return ps; }
Targets at;
Targets targets(NvnState&) { return at; }
std::string events;
VkClearDepthStencilValue value;
VkImageAspectFlags aspects;
VkRect2D area;
int st_passes, st_clears;
bool work_since_barrier;
bool depth_aspect(VkFormat f) { return f != VK_FORMAT_S8_UINT; }
bool has_stencil(VkFormat f) { return f != VK_FORMAT_D32_SFLOAT; }
bool texture_marked(Texture*) { return true; }
void texture_written(Texture*, const char*) {}
void texture_rendered(Texture*) {}
void full_barrier(VkCommandBuffer, const char*) {}
void pass_start_barrier(VkCommandBuffer) {}
void copy_block_attachment(VkCommandBuffer, Texture&, VkImage, uint32_t, uint32_t, bool) {}
void gprof_begin(VkCommandBuffer) {}
void logf(const char*, ...) {}
void end_pass(VkCommandBuffer, const char*) { if (ps.open) events += 'E'; ps.open = false; }
void vkCmdBeginRendering(VkCommandBuffer, const VkRenderingInfo* r) {
    const auto& a = *r->pDepthAttachment;
    assert(a.storeOp == VK_ATTACHMENT_STORE_OP_STORE);
    events += a.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? 'C' : 'L';
    value = a.clearValue.depthStencil;
    if (r->pStencilAttachment) assert(r->pStencilAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
}
void vkCmdClearAttachments(VkCommandBuffer, uint32_t, const VkClearAttachment* a, uint32_t, const VkClearRect* r) {
    events += 'A';
    value = a->clearValue.depthStencil;
    aspects = a->aspectMask;
    area = r->rect;
}
"""
    for marker in ["void trace_depth_clear_packet(", "void mark_depth_target(", "void begin_pass(", "VkRect2D clear_rect(", "void clear_after_draws(", "void clear_depth_stencil("]:
        fixture += block(source, marker) + "\n"
    fixture += """
int main() {
    Texture depth;
    at.depth = &depth;
    at.depth_view = (VkImageView)1;
    at.depth_format = VK_FORMAT_D32_SFLOAT;
    at.width = 768;
    at.height = 432;
    NvnState s;
    auto clear = [&] { clear_depth_stencil({}, s, 2.0f, true, 0, 0); };
    clear();
    assert(events == "C" && value.depth == 1.0f && ps.depth_load_clear);
    assert(ps.written == 256 && ps.clears == 1 && st_clears == 1);
    nvn_p_counter counter{NVN_COUNTER_TYPE_TIMESTAMP_TOP, 0x12340};
    nvn_p_flag enable{0, 0};
    trace_depth_clear_packet(NOP_REPORT_COUNTER, reinterpret_cast<uint8_t*>(&counter), sizeof counter);
    trace_depth_clear_packet(NOP_RENDER_ENABLE, reinterpret_cast<uint8_t*>(&enable), sizeof enable);
    std::string expected = "REPORT_COUNTER(type=0x10 addr=0x12340) RENDER_ENABLE(0)";
    for (int i = 0; i < 75; ++i) {
        trace_depth_clear_packet(NOP_SET_RT, nullptr, 0);
        expected += " SET_RT";
    }
    trace_depth_clear_packet(NOP_COPY_T2T, nullptr, 0);
    expected += " COPY_T2T";
    assert(ps.clear_interval.size() > 1);
    std::string actual;
    for (const auto& line : ps.clear_interval) {
        assert(line.size() <= 270);
        if (!actual.empty()) actual += ' ';
        actual += line;
    }
    assert(actual == expected);
    end_pass({}, "texture copy");
    events += 'X';
    begin_pass({}, at);
    assert(ps.clear_interval.empty());
    assert(events == "CEXL" && !ps.depth_load_clear);
    clear();
    assert(events == "CEXLA");
    ps.draws = 1;
    clear();
    assert(events == "CEXLAEC");
    end_pass({}, "test");
    events.clear();
    s.scissor[0] = 12; s.scissor[2] = 768; s.scissor[3] = 432;
    clear();
    assert(events == "LA" && area.offset.x == 12 && area.extent.width == 756);
    end_pass({}, "test");
    s.scissor[0] = 768;
    events.clear();
    clear();
    assert(events == "L" && ps.clears == 0 && !ps.written);
    s = {};
    end_pass({}, "test");
    events.clear();
    at.depth_format = VK_FORMAT_D24_UNORM_S8_UINT;
    clear();
    assert(events == "LA" && aspects == VK_IMAGE_ASPECT_DEPTH_BIT);
    clear_depth_stencil({}, s, 0.5f, false, 7, 255);
    assert(events == "LAA" && aspects == VK_IMAGE_ASPECT_STENCIL_BIT && value.stencil == 7);
    end_pass({}, "test");
    events.clear();
    at.depth_format = VK_FORMAT_D32_SFLOAT;
    at.n = 1;
    clear();
    assert(events == "LA");
    end_pass({}, "test");
    events.clear();
    at.n = 0;
    dev.driver_id = VK_DRIVER_ID_MESA_TURNIP;
    clear();
    assert(events == "LA" && value.depth == 1.0f && !ps.depth_load_clear);
    end_pass({}, "texture copy");
    events += 'X';
    begin_pass({}, at);
    assert(events == "LAEXL");
}
"""
    with tempfile.TemporaryDirectory(prefix="depth-load-clear-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-I", args.headers, "-I", str(DRAW.parents[1] / "render"),
                        str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
