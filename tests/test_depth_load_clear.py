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
#include <string>
#include <vector>
struct Texture {};
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
    for marker in ["void begin_pass(", "VkRect2D clear_rect(", "void clear_after_draws(", "void clear_depth_stencil("]:
        fixture += block(source, marker) + "\n"
    fixture += """
int main() {
    Texture depth;
    at.depth = &depth;
    at.depth_format = VK_FORMAT_D32_SFLOAT;
    at.width = 768;
    at.height = 432;
    NvnState s;
    auto clear = [&] { clear_depth_stencil({}, s, 2.0f, true, 0, 0); };
    clear();
    assert(events == "C" && value.depth == 1.0f && ps.depth_load_clear);
    assert(ps.written == 256 && ps.clears == 1 && st_clears == 1);
    end_pass({}, "texture copy");
    events += 'X';
    begin_pass({}, at);
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
}
"""
    with tempfile.TemporaryDirectory(prefix="depth-load-clear-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-I", args.headers, str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
