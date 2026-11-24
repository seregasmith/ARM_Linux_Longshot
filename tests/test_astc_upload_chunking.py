"""Large GPU ASTC uploads split across bounded queue submissions (gpu/vk/vk_texture.cpp; no game or GPU needed).

Kokiri Forest's two 1365 x 1365 x 90 11-mip ASTC terrain arrays each decode 990 slices and re-encode 990 BC3 slices
(1,980 compute dispatches, ~223M texels). Recorded into a single upload command buffer, one queue submission ran past
Windows' 2 s WDDM timeout on integrated AMD GPUs (#1040, #1730). upload_astc_gpu() now counts decoded texels across the
recording (up_astc_texels, reset by texture_begin_recording) and flushes the upload command buffer through
replay_flush_upload() whenever RX_VK_ASTC_CHUNK_MPX million texels (16 by default; 0 disables splitting) have been
recorded and more slices remain.

Cuts the production upload state, bc_encode_into(), bc_encode_resized(), level_geo(), upload_astc_gpu() and the
recording hooks out of gpu/vk/vk_texture.cpp into a Vulkan spy harness and checks:
  - a 1365 x 1365 x 90 11-mip BC3 array flushes across bounded submissions (~16M texels per submit), encodes every
    (level, layer) once in order, clears last_barrier_cmd and re-emits the initial full barrier after in-place CB
    reset, rebinds astc_pipe before dispatching, and keeps up_used set so submit_recording() submits the trailing chunk;
  - a resized BC3 array (bc_resized) and an uncompressed RGBA8 array (!tex.bc) split across bounded submissions with
    matching view layer counts and every slice written once in order;
  - up_astc_texels accumulates across multiple upload_astc_gpu() calls in one recording and resets in
    texture_begin_recording();
  - RX_VK_ASTC_CHUNK_MPX=0 disables splitting, negative values fall back to the default bound, RX_VK_ASTC_CHUNK_MPX=4
    splits at 4M texels, and a no-op replay_flush_upload returning false encodes every slice cleanly in a single
    submission.

python -I -B tests/test_astc_upload_chunking.py [--cxx g++]
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from srccut import between, block   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

struct VkCommandBuffer_T { int id; };
struct VkImage_T { int id; };
struct VkImageView_T { uint32_t level, base_layer, layer_count; bool scratch; };
struct VkBuffer_T { int id; };
struct VkPipeline_T { int id; };
struct VkPipelineLayout_T { int id; };
struct VmaAllocation_T { int id; };

using VkCommandBuffer = VkCommandBuffer_T*;
using VkDeviceSize = uint64_t;
using VkImage = VkImage_T*;
using VkImageView = VkImageView_T*;
using VkBuffer = VkBuffer_T*;
using VkPipeline = VkPipeline_T*;
using VkPipelineLayout = VkPipelineLayout_T*;
using VmaAllocation = VmaAllocation_T*;

#define VK_NULL_HANDLE nullptr
constexpr uint64_t VK_WHOLE_SIZE = ~0ull;
enum {
    VK_IMAGE_TYPE_1D = 0,
    VK_IMAGE_TYPE_2D = 1,
    VK_IMAGE_TYPE_3D = 2,
    VK_SAMPLE_COUNT_1_BIT = 1,
    VK_PIPELINE_BIND_POINT_COMPUTE = 1,
    VK_IMAGE_LAYOUT_GENERAL = 1,
    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET = 35,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE = 3,
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER = 7,
    VK_SHADER_STAGE_COMPUTE_BIT = 0x20,
    VK_IMAGE_ASPECT_COLOR_BIT = 1,
    VK_FILTER_LINEAR = 1,
};

struct VkDescriptorImageInfo {
    void* sampler;
    VkImageView imageView;
    int imageLayout;
};
struct VkDescriptorBufferInfo {
    VkBuffer buffer;
    uint64_t offset, range;
};
struct VkWriteDescriptorSet {
    int sType;
    const void* pNext;
    void* dstSet;
    uint32_t dstBinding, dstArrayElement, descriptorCount;
    int descriptorType;
    const VkDescriptorImageInfo* pImageInfo;
    const VkDescriptorBufferInfo* pBufferInfo;
    const void* pTexelBufferView;
};
struct VkImageSubresourceLayers {
    uint32_t aspectMask, mipLevel, baseArrayLayer, layerCount;
};
struct VkOffset3D { int32_t x, y, z; };
struct VkExtent3D { uint32_t width, height, depth; };
struct VkBufferImageCopy {
    uint64_t bufferOffset;
    uint32_t bufferRowLength, bufferImageHeight;
    VkImageSubresourceLayers imageSubresource;
    VkOffset3D imageOffset;
    VkExtent3D imageExtent;
};
struct VkImageBlit {
    VkImageSubresourceLayers srcSubresource;
    VkOffset3D srcOffsets[2];
    VkImageSubresourceLayers dstSubresource;
    VkOffset3D dstOffsets[2];
};

struct TexDesc {
    uint32_t width = 1, height = 1, depth = 1, levels = 1;
    int block_w = 8, block_h = 8, bpb = 16;
};
struct Texture {
    int type = VK_IMAGE_TYPE_2D;
    int samples = VK_SAMPLE_COUNT_1_BIT;
    uint32_t width = 1, height = 1, depth = 1, levels = 1, layers = 1, skip = 0;
    int block_w = 8, block_h = 8, bpb = 16, nvn_format = 0;
    bool bc = true, bc_resized = false;
    uint64_t gpu = 0x100000;
    VkImage image = nullptr;
    struct { uint64_t layer_size = 0; } L;
    TexDesc d;
};

struct HostStub {
    uint64_t (*tex_level_offset)(const TexDesc* d, uint32_t level);
    void (*tex_level_tiles)(const TexDesc* d, uint32_t level, unsigned* th, unsigned* td);
};
static uint64_t stub_level_offset(const TexDesc* d, uint32_t level) {
    uint64_t off = 0;
    for (uint32_t l = 0; l < level; ++l) {
        const uint32_t w = std::max(1u, d->width >> l), h = std::max(1u, d->height >> l);
        const uint32_t tw = (w + d->block_w - 1) / d->block_w, th = (h + d->block_h - 1) / d->block_h;
        const uint64_t gobs_x = ((uint64_t)tw * d->bpb + 63) / 64;
        const uint64_t rows_y = (th + 7ull) / 8ull;
        off += gobs_x * rows_y * 512ull;
    }
    return off;
}
static void stub_level_tiles(const TexDesc*, uint32_t, unsigned* th, unsigned* td) {
    *th = 0;
    *td = 0;
}
static const HostStub host_impl{stub_level_offset, stub_level_tiles};
static const HostStub* host = &host_impl;

struct {
    struct {
        struct {
            uint64_t minStorageBufferOffsetAlignment = 256;
            uint64_t maxStorageBufferRange = 128ull << 20;
            uint32_t maxComputeWorkGroupCount[3] = {65535, 65535, 65535};
        } limits;
    } props;
    struct {
        bool bc_copy_per_layer = false;
    } policy;
} dev;

static VkPipeline_T pipe_astc_obj{1}, pipe_bc_obj{2}, pipe_bc7_obj{3};
static VkPipelineLayout_T layout_astc_obj{1}, layout_bc_obj{2};
static VkImage_T scratch_img1{10}, scratch_img2{11};
static VkImageView_T scratch_view1{0, 0, 8, true}, scratch_view2{0, 0, 8, true};
static VkImageView_T small_views[8] = {{0,0,1,true}, {0,1,1,true}, {0,2,1,true}, {0,3,1,true},
                                      {0,4,1,true}, {0,5,1,true}, {0,6,1,true}, {0,7,1,true}};
static VkBuffer_T bc_out_obj{20}, staging_buf_obj{21};
static VmaAllocation_T staging_alloc_obj{30};

static VkPipeline astc_pipe = &pipe_astc_obj, bc_pipe = &pipe_bc_obj, bc7_pipe = &pipe_bc7_obj;
static VkPipelineLayout astc_layout = &layout_astc_obj, bc_layout = &layout_bc_obj;
static VkImage bc_scratch = &scratch_img1, bc_scratch2 = &scratch_img2;
static VkImageView bc_scratch_view = &scratch_view1, bc_scratch2_view = &scratch_view2;
static VkImageView bc_scratch_uview = &scratch_view1;   // (the decoder writes the scratch through its UINT views)
static VkBuffer bc_out = &bc_out_obj;
static bool bc7_enabled() { return false; }
constexpr uint32_t kBcDim = 2048, kBcLayers = 8;
static std::atomic<int64_t> step_ns[8];
static int64_t step_now() { return 0; }

[[maybe_unused]] static thread_local VkCommandBuffer last_barrier_cmd = VK_NULL_HANDLE;
[[maybe_unused]] static thread_local VkCommandBuffer last_pass_end_cmd = VK_NULL_HANDLE;
static thread_local const char* astc_fail = nullptr;

// ---- spy state ----
struct SubmitRec {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    uint32_t dispatches = 0;
    uint32_t astc_dispatches = 0;
    uint32_t bc_dispatches = 0;
    uint32_t barriers = 0;
    uint64_t decoded_texels = 0;
};

static std::vector<VkCommandBuffer_T> cb_pool;
static std::vector<VkImageView_T> view_pool;
static VkCommandBuffer active_cb = VK_NULL_HANDLE;
static VkPipeline bound_pipe = VK_NULL_HANDLE;
static VkImageView bound_astc_view = VK_NULL_HANDLE;
static VkImageView bound_bc_view = VK_NULL_HANDLE;
static uint32_t last_astc_blocks_x = 0, last_astc_blocks_y = 0;
static uint32_t pending_scratch_layers = 0;
static bool pending_resized_blit = false;
static uint32_t flushes = 0;
static bool noop_flush = false;
static SubmitRec cur_submit;
static std::vector<SubmitRec> submits;
static std::vector<std::tuple<VkImage, uint32_t, uint32_t>> written_slices;
static std::vector<std::string> errors;
static std::vector<uint32_t> encoded_offsets;
static std::vector<VkImageView> decoded_views, encoded_views;
static std::vector<char> events;

static void fail(const std::string& msg) {
    if (errors.size() < 16) errors.push_back(msg);
}

static VkCommandBuffer alloc_cb() {
    cb_pool.push_back({(int)cb_pool.size() + 1});
    return &cb_pool.back();
}

static void full_barrier(VkCommandBuffer cmd) {
    if (cmd != active_cb) fail("full_barrier on inactive command buffer");
    if (cmd == last_barrier_cmd) return;
    last_barrier_cmd = cmd;
    cur_submit.barriers++;
    events.push_back('B');
}
static void descriptors_disturbed() {}
static void memory_table_begin() {}
static void memory_rb_recording_begun() {}
static bool memory_rb_refresh(VkCommandBuffer) { return false; }
static bool memory_table_flush() { return false; }

static std::vector<uint8_t> staging_bytes;
static int staging_calls = 0, fail_staging_at = 0;
/* STAGING_STRUCT */
static Staging staging(uint64_t n, uint64_t) {
    if (++staging_calls == fail_staging_at) return {};
    if (staging_bytes.size() < n) staging_bytes.resize((size_t)n);
    return {&staging_buf_obj, 0, staging_bytes.data(), &staging_alloc_obj};
}
static const uint8_t* guest_bytes(uint64_t, uint64_t n, std::vector<uint8_t>& scratch) {
    scratch.assign((size_t)n, 0x5Au);
    return scratch.data();
}
static void flush_mapped_range(VmaAllocation, uint64_t, uint64_t) {}
static bool astc_ready() { return true; }
static bool bc_scratch_layout(VkCommandBuffer cmd, bool) {
    if (cmd != active_cb) fail("bc_scratch_layout on inactive command buffer");
    return true;
}
static VkImageView astc_level_view(Texture* tex, uint32_t level, uint32_t layer) {
    const uint32_t base = layer == ~0u ? 0u : layer;
    const uint32_t count = layer == ~0u ? tex->layers : 1u;
    view_pool.push_back({level, base, count, false});
    return &view_pool.back();
}

static void vkCmdBindPipeline(VkCommandBuffer cmd, int, VkPipeline pipe) {
    if (cmd != active_cb) fail("vkCmdBindPipeline on inactive command buffer");
    bound_pipe = pipe;
}
static void vkCmdPushDescriptorSetKHR(VkCommandBuffer cmd, int, VkPipelineLayout, uint32_t, uint32_t count,
                                      const VkWriteDescriptorSet* writes) {
    if (cmd != active_cb) fail("vkCmdPushDescriptorSetKHR on inactive command buffer");
    if (bound_pipe == astc_pipe && count >= 2 && writes[1].pImageInfo) {
        bound_astc_view = writes[1].pImageInfo->imageView;
    }
    if ((bound_pipe == bc_pipe || bound_pipe == bc7_pipe) && count >= 2 && writes[0].pImageInfo)
        bound_bc_view = writes[0].pImageInfo->imageView;
}
/* PUSH_STRUCTS */
static void vkCmdPushConstants(VkCommandBuffer cmd, VkPipelineLayout layout, int, uint32_t, uint32_t size, const void* p) {
    if (cmd != active_cb) fail("vkCmdPushConstants on inactive command buffer");
    if (layout == astc_layout && size == sizeof(AstcPush)) {
        const auto* pc = static_cast<const AstcPush*>(p);
        last_astc_blocks_x = pc->blocks_x;
        last_astc_blocks_y = pc->blocks_y;
    }
    if (layout == bc_layout && size == sizeof(BcPush))
        encoded_offsets.push_back(static_cast<const BcPush*>(p)->out_offset);
}

static Texture* active_tex = nullptr;
static std::vector<uint32_t> decoded_layers;
static std::vector<std::vector<uint64_t>> copy_offsets;
static void vkCmdDispatch(VkCommandBuffer cmd, uint32_t, uint32_t, uint32_t gz) {
    if (cmd != active_cb) fail("vkCmdDispatch on inactive command buffer");
    if (cur_submit.barriers == 0) fail("command recorded without prior barrier (stale last_barrier_cmd?)");
    if (bound_pipe != astc_pipe && bound_pipe != bc_pipe && bound_pipe != bc7_pipe) {
        fail("vkCmdDispatch without compute pipeline bound on current command buffer");
        return;
    }
    cur_submit.dispatches++;
    if (bound_pipe == astc_pipe) {
        events.push_back('D');
        cur_submit.astc_dispatches++;
        if (!bound_astc_view) {
            fail("ASTC dispatch without target image view");
            return;
        }
        if (bound_astc_view->scratch) {
            decoded_layers.push_back(bound_astc_view->base_layer);
            decoded_views.push_back(bound_astc_view);
            pending_scratch_layers = gz;
            uint32_t mip_w = 0, mip_h = 0;
            if (active_tex) {
                for (uint32_t l = 0; l < active_tex->levels; ++l) {
                    const uint32_t w = std::max(1u, active_tex->d.width >> (l + active_tex->skip));
                    const uint32_t h = std::max(1u, active_tex->d.height >> (l + active_tex->skip));
                    if ((w + active_tex->block_w - 1) / active_tex->block_w == last_astc_blocks_x &&
                        (h + active_tex->block_h - 1) / active_tex->block_h == last_astc_blocks_y) {
                        mip_w = w, mip_h = h;
                        break;
                    }
                }
            }
            cur_submit.decoded_texels += (uint64_t)mip_w * mip_h * gz;
        } else {
            if (bound_astc_view->layer_count != gz) {
                fail("RGBA8 ASTC dispatch z != target view layer_count");
            }
            const uint32_t l = bound_astc_view->level;
            const uint32_t w = active_tex ? std::max(1u, active_tex->d.width >> (l + active_tex->skip)) : 0;
            const uint32_t h = active_tex ? std::max(1u, active_tex->d.height >> (l + active_tex->skip)) : 0;
            cur_submit.decoded_texels += (uint64_t)w * h * gz;
            for (uint32_t z = 0; z < gz; ++z) {
                written_slices.emplace_back(active_tex ? active_tex->image : VK_NULL_HANDLE, l, bound_astc_view->base_layer + z);
            }
        }
    } else {
        cur_submit.bc_dispatches++;
        encoded_views.push_back(bound_bc_view);
        if (pending_scratch_layers != gz) fail("BC encode layer count != decoded scratch layers");
    }
    last_barrier_cmd = VK_NULL_HANDLE;
}
static void vkCmdBlitImage(VkCommandBuffer cmd, VkImage, int, VkImage, int, uint32_t, const VkImageBlit* bl, int) {
    if (cmd != active_cb) fail("vkCmdBlitImage on inactive command buffer");
    if (cur_submit.barriers == 0) fail("command recorded without prior barrier (stale last_barrier_cmd?)");
    if (bl->srcSubresource.layerCount != pending_scratch_layers) fail("resized blit layer count mismatch");
    pending_resized_blit = true;
    last_barrier_cmd = VK_NULL_HANDLE;
}
static void vkCmdCopyBufferToImage(VkCommandBuffer cmd, VkBuffer, VkImage dst, int, uint32_t count, const VkBufferImageCopy* c) {
    if (cmd != active_cb) fail("vkCmdCopyBufferToImage on inactive command buffer");
    if (cur_submit.barriers == 0) fail("command recorded without prior barrier (stale last_barrier_cmd?)");
    std::vector<uint64_t> offsets;
    for (uint32_t i = 0; i < count; ++i) {
        if (c[i].imageSubresource.layerCount != pending_scratch_layers) fail("BC copy layer count != encoded layers");
        offsets.push_back(c[i].bufferOffset);
        // (a group's copy: the buffer's rows and height given, in whole blocks covering the level, as bc_encode_into's, #2015)
        if (count > 1 && (c[i].bufferRowLength % 4 || c[i].bufferImageHeight % 4 || c[i].bufferRowLength < c[i].imageExtent.width ||
                          c[i].bufferImageHeight < c[i].imageExtent.height))
            fail("a grouped BC copy leaves the buffer's pitch to the driver");
        for (uint32_t z = 0; z < c[i].imageSubresource.layerCount; ++z)
            written_slices.emplace_back(dst, c[i].imageSubresource.mipLevel, c[i].imageSubresource.baseArrayLayer + z);
    }
    copy_offsets.push_back(offsets);
    pending_scratch_layers = 0;
    last_barrier_cmd = VK_NULL_HANDLE;
}

bool replay_flush_upload(VkCommandBuffer cur);

/* UPLOAD_STATE */
/* HOOKS */
/* PRODUCTION */

bool replay_flush_upload(VkCommandBuffer cur) {
    if (cur != active_cb) fail("replay_flush_upload handed stale command buffer");
    if (!up_used) fail("replay_flush_upload called with up_used == false");
    flushes++;
    if (noop_flush) return false;
    full_barrier(cur);
    submits.push_back(cur_submit);
    bound_pipe = VK_NULL_HANDLE;
    bound_astc_view = VK_NULL_HANDLE;
    cur_submit = {active_cb, 0, 0, 0, 0, 0};
    return true;
}

static Texture make_tex(int img_id, uint32_t w, uint32_t h, uint32_t levels, uint32_t layers, bool bc, bool resized = false) {
    static std::vector<VkImage_T> imgs;
    imgs.push_back({img_id});
    Texture t{};
    t.type = VK_IMAGE_TYPE_2D;
    t.samples = VK_SAMPLE_COUNT_1_BIT;
    t.width = resized ? ((w + 3u) & ~3u) : w;
    t.height = resized ? ((h + 3u) & ~3u) : h;
    t.depth = 1;
    t.levels = levels;
    t.layers = layers;
    t.block_w = 8;
    t.block_h = 8;
    t.bpb = 16;
    t.bc = bc;
    t.bc_resized = resized;
    t.image = &imgs.back();
    t.d = {w, h, 1, levels, 8, 8, 16};
    t.L.layer_size = stub_level_offset(&t.d, levels);
    return t;
}

static void reset_spy() {
    cb_pool.clear();
    cb_pool.reserve(256);
    view_pool.clear();
    view_pool.reserve(4096);
    submits.clear();
    written_slices.clear();
    errors.clear();
    flushes = 0;
    noop_flush = false;
    pending_scratch_layers = 0;
    pending_resized_blit = false;
    decoded_layers.clear(); decoded_views.clear(); encoded_views.clear(); encoded_offsets.clear(); copy_offsets.clear(); events.clear();
    staging_calls = fail_staging_at = 0;
}

static void begin_rec(uint64_t value) {
    active_cb = alloc_cb();
    bound_pipe = VK_NULL_HANDLE;
    bound_astc_view = VK_NULL_HANDLE;
    bound_bc_view = VK_NULL_HANDLE;
    cur_submit = {active_cb, 0, 0, 0, 0, 0};
    texture_begin_recording(active_cb, value);
}

static void end_rec() {
    const bool uploads = texture_uploads_recorded();
    if (cur_submit.dispatches > 0 && !uploads) {
        fail("trailing upload command buffer recorded dispatches with texture_uploads_recorded() == false");
    }
    if (uploads) submits.push_back(cur_submit);
    active_cb = VK_NULL_HANDLE;
}

static bool verify_all_slices(const std::vector<Texture*>& texs) {
    std::vector<std::tuple<VkImage, uint32_t, uint32_t>> want;
    for (Texture* t : texs) {
        for (uint32_t l = 0; l < t->levels; ++l)
            for (uint32_t layer = 0; layer < t->layers; ++layer)
                want.emplace_back(t->image, l, layer);
    }
    if (written_slices != want) {
        fail("slices written (" + std::to_string(written_slices.size()) + ") != expected (" + std::to_string(want.size()) + ") in order");
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string mode = argv[1];

    if (mode == "levels" || mode == "levels_split" || mode == "failure") {
        reset_spy();
        bc_small = &scratch_img2;
        bc_small_view = &scratch_view2;
        bc_small_uview = &scratch_view2;
        for (unsigned i = 0; i < 8; ++i) bc_small_layer[i] = bc_small_layer_u[i] = &small_views[i];
        Texture tex = mode == "levels_split" ? make_tex(31, 1024, 1024, 11, 1, true) : make_tex(31, 64, 64, 7, 1, true);
        begin_rec(1);
        active_tex = &tex;
        if (mode == "failure") {
            dev.props.limits.minStorageBufferOffsetAlignment = 1024;
            fail_staging_at = 2;
            if (upload_astc_gpu(tex, 0, nullptr)) fail("late staging failure was accepted");
            if (events != std::vector<char>{'B', 'D', 'D', 'B'} || decoded_layers != std::vector<uint32_t>{0, 1})
                fail("wrong dispatch or barrier sequence before staging failure");
            fail_staging_at = 0;
            Texture next = make_tex(32, 64, 64, 7, 1, true);
            active_tex = &next;
            if (!upload_astc_gpu(next, 0, nullptr)) fail("next texture failed");
            if (events.size() < 5 || events[4] != 'D' || decoded_layers.size() < 3 || decoded_layers[2] != 0)
                fail("scratch was reused without ordering after failure");
        } else if (mode == "levels_split") {
            if (!upload_astc_gpu(tex, 0, nullptr)) fail("split level upload failed");
            verify_all_slices({&tex});
            if (!flushes || copy_offsets.size() < 2) fail("level group did not split");
        } else {
            if (!upload_astc_gpu(tex, 0, nullptr)) fail("level upload failed");
            verify_all_slices({&tex});
            const bool grouped = std::getenv("RX_VK_BC_LEVELS") != nullptr;
            if (copy_offsets.size() != (grouped ? 1u : 7u)) fail("wrong copy grouping");
            if (decoded_layers.size() != 7 || (grouped && copy_offsets.size() == 1 && copy_offsets[0].size() != 7))
                fail("wrong number of mip levels");
            if (cur_submit.barriers != (grouped ? 4u : 22u)) fail("wrong barrier count");
            uint64_t offset = 0;
            for (unsigned l = 0; l < 7 && l < decoded_layers.size() && copy_offsets.size() == (grouped ? 1u : 7u) &&
                                 (!grouped || copy_offsets[0].size() == 7); ++l) {
                if (decoded_layers[l] != (grouped ? l : 0)) fail("wrong scratch layer");
                if (l >= encoded_views.size() || encoded_views[l] != decoded_views[l]) fail("encoder bound wrong scratch view");
                const uint64_t actual = grouped ? copy_offsets[0][l] : copy_offsets[l][0];
                if (actual != (grouped ? offset : 0)) fail("wrong BC output range");
                if (l >= encoded_offsets.size() || encoded_offsets[l] * 8ull != actual) fail("encoder offset differs from copy");
                const uint32_t dim = std::max(1u, 64u >> l);
                offset += ((dim + 3) / 4) * ((dim + 3) / 4) * 16;
            }
        }
        end_rec();
        for (const auto& e : errors) std::fprintf(stderr, "ERROR: %s\n", e.c_str());
        return errors.empty() ? 0 : 1;
    }

    if (mode == "terrain_bc3_default" || mode == "terrain_bc3_resized" || mode == "terrain_rgba8_default" ||
        mode == "terrain_bc3_off" || mode == "terrain_bc3_4mpx" || mode == "terrain_bc3_neg" ||
        mode == "terrain_bc3_noop_flush") {
        reset_spy();
        if (mode == "terrain_bc3_noop_flush") noop_flush = true;
        const bool bc = mode != "terrain_rgba8_default";
        const bool resized = mode == "terrain_bc3_resized";
        Texture tex = make_tex(1, 1365, 1365, 11, 90, bc, resized);
        begin_rec(1);
        active_tex = &tex;
        const bool ok = upload_astc_gpu(tex, 0, nullptr);
        end_rec();
        if (!ok) fail(std::string("upload_astc_gpu returned false: ") + (astc_fail ? astc_fail : "?"));
        verify_all_slices({&tex});
        if (resized && !pending_resized_blit) fail("bc_resized upload never blitted into scratch2");

        uint32_t max_disp = 0, total_disp = 0;
        uint64_t max_texels = 0;
        for (const SubmitRec& s : submits) {
            max_disp = std::max(max_disp, s.dispatches);
            total_disp += s.dispatches;
            max_texels = std::max(max_texels, s.decoded_texels);
        }
        std::printf("mode=%s flushes=%u submits=%zu total_dispatches=%u max_dispatches=%u max_texels=%llu errors=%zu\n",
                    mode.c_str(), flushes, submits.size(), total_disp, max_disp,
                    (unsigned long long)max_texels, errors.size());
        for (const std::string& e : errors) std::fprintf(stderr, "ERROR: %s\n", e.c_str());
        return errors.empty() ? 0 : 1;
    }

    if (mode == "cumulative_and_reset") {
        // Five 2048 x 2048 x 1 BC3 textures in one recording (~4.19M texels each = ~20.97M total) must cross the
        // 16M bound and flush before the 5th texture; after texture_begin_recording(), three more (~12.58M) must not flush.
        reset_spy();
        std::vector<Texture> a, b;
        a.reserve(5);
        b.reserve(3);
        for (int i = 0; i < 5; ++i) a.push_back(make_tex(10 + i, 2048, 2048, 1, 1, true));
        for (int i = 0; i < 3; ++i) b.push_back(make_tex(20 + i, 2048, 2048, 1, 1, true));

        begin_rec(1);
        std::vector<Texture*> ptrs;
        for (Texture& t : a) {
            active_tex = &t;
            if (!upload_astc_gpu(t, 0, nullptr)) fail("upload in rec 1 failed");
            ptrs.push_back(&t);
        }
        end_rec();
        const uint32_t rec1_flushes = flushes;

        begin_rec(2);
        for (Texture& t : b) {
            active_tex = &t;
            if (!upload_astc_gpu(t, 0, nullptr)) fail("upload in rec 2 failed");
            ptrs.push_back(&t);
        }
        end_rec();
        const uint32_t rec2_flushes = flushes - rec1_flushes;

        verify_all_slices(ptrs);
        std::printf("mode=%s rec1_flushes=%u rec2_flushes=%u submits=%zu errors=%zu\n",
                    mode.c_str(), rec1_flushes, rec2_flushes, submits.size(), errors.size());
        for (const std::string& e : errors) std::fprintf(stderr, "ERROR: %s\n", e.c_str());
        return errors.empty() ? 0 : 1;
    }

    return 2;
}
"""


def parse_kv(line):
    out = {}
    for part in line.strip().split():
        if "=" in part:
            k, v = part.split("=", 1)
            out[k] = v
    return out


def build_fixture(cxx, temp):
    tex = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    upload_state = between(tex, "// ---- the recording's upload command buffer ----", "// ---- staging: host-visible arenas")
    hooks = [
        block(tex, "void texture_begin_recording(VkCommandBuffer upload, uint64_t value) {"),
        block(tex, "bool texture_uploads_recorded() {"),
    ]

    prod = []
    if "uint32_t bc_small_layers() {" in tex:   # (#1843's small scratch: not made here, every level through the first)
        prod.append("static VkImage bc_small = VK_NULL_HANDLE;\nstatic VkImageView bc_small_view = VK_NULL_HANDLE;\n"
                    "static VkImageView bc_small_layer[8] = {};\nstatic VkImageView bc_small_uview = VK_NULL_HANDLE, bc_small_layer_u[8] = {};\nstatic bool bc_levels_on() { return std::getenv(\"RX_VK_BC_LEVELS\") != nullptr; }\n"
                    "constexpr uint32_t kBcSmallDim = 1024;\nstatic uint32_t bc_small_px = kBcSmallDim;\nstatic uint32_t bc_small_layers() { return 8; }")
    if "bool upload_never_put_off(const Texture& tex) {" in tex:
        prod.append(block(tex, "bool upload_never_put_off(const Texture& tex) {"))
    if "GprofSpan" in tex:   # (RX_VK_PASS_TIMING's spans: off here)
        prod.append("struct GprofSpan {\n    uint64_t bytes = 0;\n    GprofSpan(VkCommandBuffer, const char*) {}\n};")
    if "uint64_t astc_chunk_texels(" in tex:
        prod.append(block(tex, "uint64_t astc_chunk_texels("))
    prod.extend([
        block(tex, "uint32_t bc_layers() {"),
        between(tex, "/// (longshot-amd) scratch [0, w) x [0, h), layers [0, n), blitted",
                "\n/// The scratch for the upload recording now:"),
        block(tex, "struct LevelGeo {") + ";",
        block(tex, "LevelGeo level_geo("),
        "using Ranges = std::vector<std::pair<uint64_t, uint64_t>>;",
        block(tex, "bool overlaps("),
        block(tex, "bool astc_nvn_srgb(int nvn) {"),
        block(tex, "bool upload_astc_gpu(Texture& tex, uint64_t total, const Ranges* dirty) {"),
    ])
    push_structs = block(tex, "struct AstcPush {") + ";\n" + block(tex, "struct BcPush {") + ";"
    staging_struct = block(tex, "struct Staging {") + ";"

    src_text = (FIXTURE
                .replace("/* STAGING_STRUCT */", staging_struct)
                .replace("/* PUSH_STRUCTS */", push_structs)
                .replace("/* UPLOAD_STATE */", upload_state)
                .replace("/* HOOKS */", "\n".join(hooks))
                .replace("/* PRODUCTION */", "\n".join(prod)))

    src = Path(temp) / "astc_upload_chunking.cpp"
    exe = Path(temp) / ("astc_upload_chunking.exe" if os.name == "nt" else "astc_upload_chunking")
    src.write_text(src_text, encoding="utf-8")
    subprocess.run([
        cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
        str(src), "-o", str(exe),
    ], check=True)
    return exe


def run_case(exe, mode, chunk_env=None, levels=False):
    env = {k: v for k, v in os.environ.items() if not k.startswith("RX_VK_")}
    if chunk_env is not None:
        env["RX_VK_ASTC_CHUNK_MPX"] = chunk_env
    if levels:
        env["RX_VK_BC_LEVELS"] = "1"
    r = subprocess.run([str(exe), mode], text=True, capture_output=True, timeout=30, env=env)
    if r.returncode != 0:
        raise SystemExit(f"FAIL {mode} (RX_VK_ASTC_CHUNK_MPX={chunk_env!r}):\n{r.stdout}{r.stderr}".strip())
    kv = parse_kv(r.stdout)
    return kv, r.stdout.strip()


def need(cond, why, detail=""):
    if not cond:
        msg = f"FAIL: {why}"
        if detail:
            msg += f" ({detail})"
        raise SystemExit(msg)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    args = p.parse_args()

    with tempfile.TemporaryDirectory(prefix="astc-chunk-") as temp:
        exe = build_fixture(args.cxx, temp)

        for levels in (False, True):
            run_case(exe, "levels", "0", levels)
        run_case(exe, "levels_split", "1", True)
        run_case(exe, "failure", "0", True)

        # 1. Default chunking (16 Mpx) on a 1365x1365x90 11-mip BC3 array (990 slices, 1,980 dispatches, ~223M texels).
        kv, line = run_case(exe, "terrain_bc3_default", None)
        print(line)
        flushes = int(kv["flushes"])
        submits = int(kv["submits"])
        total_disp = int(kv["total_dispatches"])
        max_disp = int(kv["max_dispatches"])
        max_texels = int(kv["max_texels"])
        need(total_disp == 1980, "1365x1365x90 11-mip BC3 array should record 1980 dispatches in total", line)
        need(flushes >= 10 and submits == flushes + 1,
             f"1365x1365x90 BC3 upload was not split across bounded submissions: got {flushes} flushes and {submits} submit(s)",
             line)
        # 9 layers of mip 0 (1365*1365 = 1,863,225 texels) reach 16,769,025 >= 16,000,000 before flushing.
        need(max_texels <= 17_000_000 and max_disp <= 600,
             f"a chunk exceeded the ~16M texel bound: max_texels={max_texels}, max_dispatches={max_disp}", line)

        # 2. Resized BC3 path (bc_resized) splits identically and encodes every slice in order.
        kv_res, line_res = run_case(exe, "terrain_bc3_resized", None)
        print(line_res)
        need(int(kv_res["flushes"]) == flushes and int(kv_res["max_texels"]) <= 17_000_000,
             "bc_resized path did not split with the same bound", line_res)

        # 3. Uncompressed RGBA8 path (!tex.bc) also splits huge mip levels instead of decoding 167M texels in 1 dispatch.
        kv_rgba, line_rgba = run_case(exe, "terrain_rgba8_default", None)
        print(line_rgba)
        need(int(kv_rgba["flushes"]) >= 10 and int(kv_rgba["max_texels"]) <= 17_000_000,
             "RGBA8 (!tex.bc) path did not split huge array mips across bounded submissions", line_rgba)

        # 4. Cumulative up_astc_texels across multiple upload_astc_gpu() calls in one recording, reset per recording.
        kv_cum, line_cum = run_case(exe, "cumulative_and_reset", None)
        print(line_cum)
        need(int(kv_cum["rec1_flushes"]) == 1 and int(kv_cum["rec2_flushes"]) == 0,
             "up_astc_texels must accumulate across uploads in one recording and reset at texture_begin_recording()",
             line_cum)

        # 5. RX_VK_ASTC_CHUNK_MPX=0 disables splitting; negative values guard and keep default; =4 splits finer (~4M).
        kv_off, line_off = run_case(exe, "terrain_bc3_off", "0")
        print(line_off)
        need(int(kv_off["flushes"]) == 0 and int(kv_off["submits"]) == 1 and int(kv_off["total_dispatches"]) == 1980,
             "RX_VK_ASTC_CHUNK_MPX=0 must disable chunk flushing", line_off)

        kv_neg, line_neg = run_case(exe, "terrain_bc3_neg", "-5")
        print(line_neg)
        need(int(kv_neg["flushes"]) == flushes and int(kv_neg["max_texels"]) <= 17_000_000,
             "negative RX_VK_ASTC_CHUNK_MPX must be guarded and fall back to the default bound", line_neg)

        kv_4m, line_4m = run_case(exe, "terrain_bc3_4mpx", "4")
        print(line_4m)
        need(int(kv_4m["flushes"]) > flushes * 2 and int(kv_4m["max_texels"]) <= 6_000_000,
             "RX_VK_ASTC_CHUNK_MPX=4 must split at ~4M texels", line_4m)

        # 6. A no-op replay_flush_upload returning false (no recording's upload buffer to flush).
        kv_noop, line_noop = run_case(exe, "terrain_bc3_noop_flush", None)
        print(line_noop)
        need(int(kv_noop["flushes"]) >= 10 and int(kv_noop["submits"]) == 1 and int(kv_noop["total_dispatches"]) == 1980,
             "no-op replay_flush_upload returning false must encode all slices cleanly in one CB", line_noop)

    print("PASS: GPU ASTC uploads split across bounded submissions and preserve slice order")


if __name__ == "__main__":
    main()
