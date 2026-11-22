"""The copy out of the ASTC-to-BC encoder's buffer gives the buffer's row length and image height (gpu/vk/vk_texture.cpp).

An ASTC array kept as BC3 is decoded into a scratch a run of layers at a time, encoded into a buffer
(host_shaders/bc_encoder.comp: block (z * blocks_y + y) * blocks_x + x, 16 bytes each) and copied into the image by
bc_encode_into. The copy left bufferRowLength and bufferImageHeight at 0, so the layers' pitch in the buffer was the
driver's to work out from the extent. The terrain's material arrays are 1365 x 1365 x 90: no level is a multiple of 4,
and on AMD's Windows driver (an RX 6700 XT, #2006; an RX 6600 before, ec89f92e) every layer after a run's first came out
wrong, Hyrule Field's ground in a grid of dark lines, gone at one layer a run (RX_VK_BC_SMALL_LAYERS=1). The check runs
the real bc_encode_into against stubbed Vulkan calls and needs the copy's rows and layers to be the encoder's, in whole
blocks, for levels that are and aren't a multiple of 4.

python -I -B tests/test_bc_copy_layer_pitch.py --headers <Vulkan-Headers/include> [--cxx g++]
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from srccut import block  # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]
TEX = ROOT / "gpu" / "vk" / "vk_texture.cpp"

FIXTURE = r"""
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
struct Texture { VkImage image = (VkImage)1; };
VkPipeline bc_pipe = (VkPipeline)1, bc7_pipe = (VkPipeline)2;
VkPipelineLayout bc_layout = (VkPipelineLayout)1;
VkImageView bc_scratch_view = (VkImageView)1;
VkBuffer bc_out = (VkBuffer)1;
bool bc7_enabled() { return false; }
void full_barrier(VkCommandBuffer) {}
void descriptors_disturbed() {}
void vkCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {}
void vkCmdPushDescriptorSetKHR(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t, const VkWriteDescriptorSet*) {}
void vkCmdDispatch(VkCommandBuffer, uint32_t, uint32_t, uint32_t) {}
/*BCPUSH*/
BcPush pushed{};
VkBufferImageCopy copied{};
int copies = 0;
void vkCmdPushConstants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t, uint32_t size, const void* p) {
    if (size == sizeof pushed) std::memcpy(&pushed, p, size);
}
void vkCmdCopyBufferToImage(VkCommandBuffer, VkBuffer, VkImage, VkImageLayout, uint32_t count, const VkBufferImageCopy* c) {
    copies += (int)count;
    if (count) copied = c[0];
}
/*ENCODE*/
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    const uint32_t w = (uint32_t)atoi(argv[1]), h = (uint32_t)atoi(argv[2]), n = (uint32_t)atoi(argv[3]);
    Texture t;
    bc_encode_into((VkCommandBuffer)1, w, h, n, t, 1, 3, 0, 0);
    // (the encoder's layout: blocks_x * blocks_y blocks a layer, 16 bytes a block)
    const uint64_t enc_row = (uint64_t)pushed.blocks_x * 16, enc_layer = enc_row * pushed.blocks_y;
    std::printf("copies %d rowlen %u imgheight %u extent %ux%ux%u layers %u at %u, encoder %ux%u blocks\n", copies,
                copied.bufferRowLength, copied.bufferImageHeight, copied.imageExtent.width, copied.imageExtent.height,
                copied.imageExtent.depth, copied.imageSubresource.layerCount, copied.imageSubresource.baseArrayLayer,
                pushed.blocks_x, pushed.blocks_y);
    if (copies != 1 || copied.imageSubresource.layerCount != n || copied.imageExtent.width != w || copied.imageExtent.height != h)
        return std::puts("FAIL: the copy isn't one region of the run's layers at the level's size"), 1;
    if (!copied.bufferRowLength || !copied.bufferImageHeight)
        return std::puts("FAIL: the buffer's layer pitch is left to the driver to work out from the extent"), 1;
    if (copied.bufferRowLength % 4 || copied.bufferImageHeight % 4 || copied.bufferRowLength < w || copied.bufferImageHeight < h)
        return std::puts("FAIL: the buffer's rows or layers aren't whole blocks covering the level"), 1;
    const uint64_t row = (uint64_t)(copied.bufferRowLength / 4) * 16, layer = row * (copied.bufferImageHeight / 4);
    if (row != enc_row || layer != enc_layer)
        return std::puts("FAIL: the copy reads the buffer's rows or layers at another pitch than the encoder wrote them"), 1;
    return 0;
}
"""


def need(ok, why):
    if not ok:
        raise SystemExit("FAIL: " + why)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--headers", type=Path, required=True)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    tex = TEX.read_text(encoding="utf-8")
    fixture = FIXTURE.replace("/*BCPUSH*/", block(tex, "struct BcPush {", where=TEX) + ";")
    fixture = fixture.replace("/*ENCODE*/", block(tex, "void bc_encode_into(VkCommandBuffer cmd,", where=TEX))
    with tempfile.TemporaryDirectory(prefix="bc-copy-pitch-") as temp:
        src, exe = Path(temp) / "pitch.cpp", Path(temp) / "pitch.exe"
        src.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                        "-Wno-unused-parameter", "-I" + str(a.headers), str(src), "-o", str(exe)], check=True)
        # (the terrain arrays' levels from 1365: 682 and 341 go eight layers a run through the small scratch; 1024 and the
        # tail's 2 and 1 for the levels that are, and aren't, a multiple of 4; one layer as a single-layer texture)
        for w, h, n in ((682, 682, 8), (341, 341, 8), (85, 85, 8), (2, 2, 8), (1, 1, 8), (1024, 1024, 8), (1365, 1365, 1), (683, 170, 3)):
            r = subprocess.run([str(exe), str(w), str(h), str(n)], text=True, capture_output=True, timeout=20)
            need(r.returncode == 0, f"{w} x {h}, {n} layers: {r.stdout.strip()}")
    print("PASS: the copy out of the BC encoder's buffer reads its rows and layers at the encoder's pitch, in whole blocks")


if __name__ == "__main__":
    main()
