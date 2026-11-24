// Textures: one VkImage per texture storage (vk_texture.h). After totk-pc-recomp's runtime/vk_texture.c (MIT,
// Copyright (c) 2026 the totk-pc-recomp authors): the cache keyed by storage address plus descriptor, uploads on first
// use and when page write generations show the storage written, the GPU-owned rule (an image the GPU wrote is the
// content), every image in GENERAL layout, deferred destruction. The GPU ASTC decode is Eden's
// (video_core/host_shaders/astc_decoder.comp and renderer_vulkan/vk_compute_pass.cpp ASTCDecoderPass, GPL). The CPU
// block-linear (Switch 2 GOB) swizzle and unswizzle and the CPU ASTC decoder, which runs when the GPU one declines or with
// RX_VK_ASTC_CPU=1, are texdecode's (gpu/vk/texdecode/: ported code, PROVENANCE.md there).
//
// Formats share images by class: an image is made in the format first asked for (sRGB as its UNORM twin), with
// MUTABLE_FORMAT, so that a texture rendered as RGBA8 and sampled as RGBA8_SRGB, or written as R32UI and read as
// RGBA8, is one image. A class is the bytes per block for uncompressed colour, the format family for BC and ASTC,
// the format itself for depth / stencil.
#include <algorithm>
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#if defined(__linux__)
#include <dlfcn.h>
#include <malloc.h>
#endif
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <list>
#include <map>
#include <optional>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <vector>

#include "vk_texture.h"
#include "vk_new.h"
#ifdef TRACY_ENABLE
#include "../../host/hos_tracy.h"   // (LONGSHOT_TRACY: the profiler's zones; nothing in any other build)
#endif
#include "vk_menu.h"
#include "vk_draw.h"
#include "vk_programs.h"
#include "vk_gprof.h"
#include "vk_dump.h"
#include "vk_jobs.h"
#include "texture_budget.h"
#include "mem_account.h"
#include "texture_ahead.h"
#include "texture_tiles.h"
#include "vk_paths.h"

#include <span>

#include "texdecode/texdecode.h"

#include "vk_builtin_spv.h"   // (RX_VK_SHADER_DIR)
#include "host_shaders/astc_decoder_comp_spv.h"
#include "host_shaders/bc_encoder_comp_spv.h"
#include "host_shaders/bc7_encoder_comp_spv.h"
#include "bc_levels.h"
#include "astc_bc_policy.h"
#include "astc_reuse.h"
#include "drawn_writeback.h"
#include "vram_compact.h"
#include "commit_room.h"
#include "linux_pressure.h"
#include "evict_reason.h"
#include "gpu_write_history.h"
#include "wrote_regions.h"
#include "keep_rendered.h"
#include "guest_holes.h"
#include "target_overlap.h"
#include "target_pool.h"
#include "page_marks.h"   // (vw_ever: invalidate_quick)
#include "dead_targets.h"
#include "host_shaders/block_linear_unswizzle_2d_comp_spv.h"
#ifndef LONGSHOT_ENV_H   // (env_on: the switches' one rule)
#include "longshot_env.h"
#endif
#include "cxx_escape.h"   // (fork_join: a helper that cannot be started at the commit limit leaves its part to the caller)
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)

namespace mkwvk {
namespace {

uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

// ---- formats ----
std::mutex fmt_mu;
std::unordered_map<int, VkFormatProperties> fmt_cache;
VkFormatProperties format_props(VkFormat f) {
    std::lock_guard lk(fmt_mu);
    auto it = fmt_cache.find((int)f);
    if (it != fmt_cache.end()) return it->second;
    VkFormatProperties p{};
    vkGetPhysicalDeviceFormatProperties(dev.physical, f, &p);
    fmt_cache[(int)f] = p;
    return p;
}
bool is_bc(VkFormat f) {
    return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK;
}
bool is_astc(VkFormat f) {
    return f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
}
/// The UNORM twin of an sRGB format (the image's format: storage writes and every view format of the class)
VkFormat linear_twin(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK: return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case VK_FORMAT_BC2_SRGB_BLOCK: return VK_FORMAT_BC2_UNORM_BLOCK;
    case VK_FORMAT_BC3_SRGB_BLOCK: return VK_FORMAT_BC3_UNORM_BLOCK;
    case VK_FORMAT_BC7_SRGB_BLOCK: return VK_FORMAT_BC7_UNORM_BLOCK;
    default:
        if (is_astc(f) && ((f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) & 1)) return (VkFormat)(f - 1);
        return f;
    }
}
/// 3-component formats are not image formats on NVIDIA: the image holds 4 (alpha one), the upload widens them
VkFormat widened(VkFormat f, int* from_bpb) {
    switch (f) {
    case VK_FORMAT_R8G8B8_UNORM: *from_bpb = 3; return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8_SNORM: *from_bpb = 3; return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT: *from_bpb = 3; return VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8_SINT: *from_bpb = 3; return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R16G16B16_SFLOAT: *from_bpb = 6; return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VK_FORMAT_R16G16B16_UNORM: *from_bpb = 6; return VK_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16_SNORM: *from_bpb = 6; return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16_UINT: *from_bpb = 6; return VK_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R16G16B16_SINT: *from_bpb = 6; return VK_FORMAT_R16G16B16A16_SINT;
    case VK_FORMAT_R32G32B32_SFLOAT: *from_bpb = 12; return VK_FORMAT_R32G32B32A32_SFLOAT;
    case VK_FORMAT_R32G32B32_UINT: *from_bpb = 12; return VK_FORMAT_R32G32B32A32_UINT;
    case VK_FORMAT_R32G32B32_SINT: *from_bpb = 12; return VK_FORMAT_R32G32B32A32_SINT;
    default: *from_bpb = 0; return f;
    }
}
/// A storage-capable view format of the image's class (sRGB views cannot be storage)
VkFormat storage_view_format(VkFormat f) {
    f = linear_twin(f);
    if (format_props(f).optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) return f;
    switch (f) {   // (same class, a format storage can be)
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: case VK_FORMAT_R8G8B8A8_SNORM: return VK_FORMAT_R32_UINT;
    case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM: return VK_FORMAT_R32G32_UINT;
    case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R16_SNORM: return VK_FORMAT_R16_UINT;
    case VK_FORMAT_R8_SNORM: return VK_FORMAT_R8_UINT;
    // (compressed: one block a texel, written raw by the game's encoders)
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK: return VK_FORMAT_R32G32_UINT;
    case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC5_SNORM_BLOCK: case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK: return VK_FORMAT_R32G32B32A32_UINT;
    default: return f;
    }
}
}  // namespace
/// (longshot) The format of the image made for a texture of this Vulkan format (vk_format's, widened): depth as it is, decoded ASTC as
/// RGBA8, the rest as their UNORM twin. create's choice, and the pipeline state list's (attachment_format): one rule for both
VkFormat image_format_of(VkFormat vf, bool astc_decoded) {
    return is_depth_format(vf) ? vf : astc_decoded ? VK_FORMAT_R8G8B8A8_UNORM : linear_twin(vf);
}
/// (longshot) The format a view of an image takes for a use: the view's NVN format as this device has it (widened), the image's own
/// for depth and for a format Vulkan lacks, decoded ASTC as RGBA8, ASTC kept as BC3 as BC3 (sampled only); UNDEFINED: no such view
/// (a compressed format is no target). texture_view's choice, and the pipeline state list's: one rule for both
VkFormat view_format_of(VkFormat image, bool astc_decoded, bool bc, int view_nvn, ViewUse use, VkFormat force_format) {
    int from = 0;
    VkFormat f = widened(vk_format(view_nvn).format, &from);
    if (is_depth_format(image) || f == VK_FORMAT_UNDEFINED) f = image;
    if (astc_decoded) f = (view_nvn >= 0x87 && view_nvn <= 0x94) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    if (bc) {
        if (use != ViewUse::Sample) return VK_FORMAT_UNDEFINED;
        const bool srgb = f == VK_FORMAT_R8G8B8A8_SRGB;
        if (linear_twin(image) == VK_FORMAT_BC7_UNORM_BLOCK)
            f = srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
        else
            f = srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
    }
    if (use == ViewUse::Storage) f = force_format != VK_FORMAT_UNDEFINED ? force_format : storage_view_format(f);
    if (use == ViewUse::Attachment && (is_astc(f) || is_bc(f))) return VK_FORMAT_UNDEFINED;
    return f;
}
/// (longshot) The format a draw renders in, on this device, into a texture of NVN format tex_nvn through a view of NVN format view_nvn
/// (a depth target: its own). What texture_get and texture_view make of them, without the texture: the pipeline state list's
/// targets are the game's NVN formats, and each GPU builds the pipelines its draws will ask for (vk_draw.cpp state_targets)
VkFormat attachment_format(int tex_nvn, int view_nvn) {
    const VkFormatInfo fi = vk_format(tex_nvn);
    if (fi.format == VK_FORMAT_UNDEFINED) return VK_FORMAT_UNDEFINED;
    int from = 0;
    return view_format_of(image_format_of(widened(fi.format, &from), fi.astc), fi.astc, false, view_nvn, ViewUse::Attachment,
                          VK_FORMAT_UNDEFINED);
}
namespace {
/// The class an image format shares images by (see the top of the file)
uint32_t format_class(VkFormat vf, int nvn_format, int bpb, bool astc_decoded) {
    if (astc_decoded) return 0x30000u | (uint32_t)(nvn_format - 0x79) % 14;
    if (is_depth_format(vf)) return 0x10000u | (uint32_t)vf;
    if (is_bc(vf) || is_astc(vf)) return 0x20000u | (uint32_t)linear_twin(vf);
    int from = 0;
    widened(vf, &from);
    // (RGB10A2 a class of its own: the game's window textures are RGBA8 until it is told its display is HDR10, then RGB10A2 at
    // the same addresses. Shared by class, a window texture first made as RGBA8 would stay RGBA8, the native-HDR present
    // (vk_replay.cpp: the image's format) would take it for an SDR picture, and frames would be grey)
    return 0x40000u | (uint32_t)bpb | (from ? 0x100u : 0u) | (vf == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ? 0x200u : 0u);
}

VkComponentSwizzle component(unsigned s) {
    switch (s) {
    case NVN_TEXTURE_SWIZZLE_ZERO: return VK_COMPONENT_SWIZZLE_ZERO;
    case NVN_TEXTURE_SWIZZLE_ONE: return VK_COMPONENT_SWIZZLE_ONE;
    case NVN_TEXTURE_SWIZZLE_R: return VK_COMPONENT_SWIZZLE_R;
    case NVN_TEXTURE_SWIZZLE_G: return VK_COMPONENT_SWIZZLE_G;
    case NVN_TEXTURE_SWIZZLE_B: return VK_COMPONENT_SWIZZLE_B;
    case NVN_TEXTURE_SWIZZLE_A: return VK_COMPONENT_SWIZZLE_A;
    default: return VK_COMPONENT_SWIZZLE_IDENTITY;
    }
}

// ---- the cache ----
struct Key {
    uint64_t gpu;
    uint32_t target, width, height, depth, levels, samples, layout, cls;
    bool operator==(const Key& o) const {
        return gpu == o.gpu && target == o.target && width == o.width && height == o.height && depth == o.depth &&
               levels == o.levels && samples == o.samples && layout == o.layout && cls == o.cls;
    }
};
struct KeyHash {
    size_t operator()(const Key& k) const {
        uint64_t h = mix(k.gpu, k.target);
        h = mix(h, (uint64_t)k.width << 32 | k.height);
        h = mix(h, (uint64_t)k.depth << 32 | k.levels);
        h = mix(h, (uint64_t)k.samples << 40 | (uint64_t)k.layout << 20 | k.cls);
        return (size_t)h;
    }
};
std::unordered_map<Key, std::unique_ptr<Texture>, KeyHash> textures;
std::unordered_multimap<uint64_t, Texture*> by_gpu;
// (longshot) The textures a pass has rendered into (first_render_frame set: texture_rendered adds one there, once), for the
// dead targets' sweep at every present: a few hundred of the table's thousands (5,000 textures in Castle Town's market, which
// the sweep walked twice a present, 0.33 ms of the render thread, to find the render targets among them). Every erase from
// `textures` takes its textures out first (rendered_forget): a pointer here is always one the table owns
std::vector<Texture*> rendered_targets;
template <class Set> void rendered_forget(const Set& gone) {   // (before the textures in `gone` leave the table)
    if (!gone.empty()) std::erase_if(rendered_targets, [&](Texture* t) { return gone.count(t) != 0; });
}
uint64_t frame_no = 1;
uint64_t total_bytes = 0;

// (RX_VK_LOG_REMADE=1) Textures evicted and made again within 600 frames, and those of them that were then sampled
// empty (storage the GPU rendered, not uploaded): what the eviction's minimum age below is measured by
struct Evicted {
    uint64_t frame;
    bool rendered;
};
std::mutex evicted_mu;
std::unordered_map<uint64_t, Evicted> evicted_at;   // (storage, size and format -> when it went)
std::unordered_map<const Texture*, uint64_t> remade;   // (made again: how many frames after it went)
uint64_t evicted_key(uint64_t gpu, uint32_t w, uint32_t h, uint32_t fmt) {
    return gpu ^ ((uint64_t)w << 48) ^ ((uint64_t)h << 32) ^ ((uint64_t)fmt << 20);
}
bool remade_log_on() {
    static const bool on = getenv("RX_VK_LOG_REMADE") && atoi(getenv("RX_VK_LOG_REMADE")) != 0;
    return on;
}
// (longshot) Textures uploaded from the game's memory (never drawn by the GPU) that the budget evicted: storage, size and format.
// Made again, such a texture is uploaded from that memory whatever ranges render targets once took over it (upload's "storage
// the GPU rendered" skip below). Skipped, its new image is never filled: whatever the VRAM held before, random-colour blocks on
// characters, shop shelves, floors, spreading over play time on 4-8 GB cards (the ones that evict)
std::mutex from_memory_mu;
std::unordered_map<uint64_t, uint64_t> evicted_from_memory;   // (evicted_key -> guest size)
std::atomic<uint64_t> st_remade_uploaded{0};

struct Dead {
    uint64_t value;
    VkImage image;
    VmaAllocation allocation;
    std::vector<VkImageView> views;
    std::vector<VkDeviceMemory> memory;   // (a sparse image's)
    VkBuffer buffer = VK_NULL_HANDLE;     // (the BC3 scratch's output buffer: given back with its image)
    VmaAllocation buffer_alloc = nullptr;
    uint64_t pool_key = 0;                // (a render target's image: to the pool below, with its views, not destroyed)
    std::unordered_map<uint64_t, VkImageView> view_map;
    uint64_t bytes = 0;
};
std::deque<Dead> dead;

// ---- (longshot) Render target images, reused. OoT makes a frame's transient targets in a scratch heap whose offsets shift with
// what came before them in the frame (the dead targets, texture_frame): each shifted one was a texture of its own with a new image,
// and every 60 presents the sweep let go of the dead ones, 300-700 at once, destroyed in the next present: 20-50 ms on the thread
// replaying, once a second while the camera moves (RTX 5070, Kokiri Forest: 10,500 images made and destroyed in 4.5 minutes,
// 17 GB). Now a render target's image whose texture is gone waits in a pool by its create info, views and all, and a new texture
// of the same create info takes it; it is put into GENERAL from UNDEFINED and cleared as a new one is (create), so nothing it held
// shows. Only images made for a target (texture_get's !for_sampling) are pooled. The pool is bounded (RX_VK_TARGET_POOL_MB, default
// 256; 0: none, every image destroyed as before; and none while VRAM is near what the driver allows), and an image unused in it for
// RX_VK_POOL_IDLE presents (target_pool::idle_presents: 8, or 64 on an integrated GPU while the take by size is on) is destroyed, at
// most 16 a present. A target of a create info not in the pool takes the memory of a pooled image it fits in instead
// (pool_take_memory, RX_VK_TARGET_REUSE_MEMORY) ----
struct Pooled {
    VkImage image;
    VmaAllocation allocation;
    std::unordered_map<uint64_t, VkImageView> views;
    uint64_t bytes, since, key;
    // (its allocation, for pool_take_memory: read once when it comes in)
    uint64_t alloc_bytes = 0, alloc_offset = 0;
    uint32_t memory_type = UINT32_MAX;
    bool dedicated = true;
    bool classed = false;   // (an allocation of a size class: in by_class)
};
std::mutex pool_mu;
// Global insertion order makes the oldest eligible victim available in constant time.
std::list<Pooled> pool_by_age;
std::unordered_map<uint64_t, std::deque<std::list<Pooled>::iterator>> target_pool;
// (the pooled allocations of a size class, by memory type and class (target_pool::class_key), oldest first: pool_take_memory's
// look-up, before its scan)
std::unordered_map<uint64_t, std::deque<std::list<Pooled>::iterator>> by_class;
uint64_t pool_bytes, pool_n, st_pool_reused, st_pool_made, st_pool_destroyed, st_pool_resized, st_pool_stolen;
// (the keys whose image pool_take_memory gave to another shape, and when: a miss of one of them is counted (st_pool_stolen),
// what the wait in target_pool::kSizeTakeIdle is meant to keep at 0. Forgotten when an image of the shape comes back or is taken
// exactly, or after RX_VK_POOL_IDLE presents (pool_trim))
std::unordered_map<uint64_t, uint64_t> taken_by_size;
// (why the misses pool_take_memory did not answer were not: target_pool::Miss; and the smaller ones' shortfall in percent, summed)
uint64_t st_miss[(int)target_pool::Miss::count], st_small_gap_pct, st_pool_trim_cap, st_pool_trim_idle;
std::atomic<uint64_t> dead_pooled_n{0};   // (render targets' images in the dead queue on their way to the pool)
uint64_t st_pool_put, st_pool_idle_out;   // (TEST, local, the trace's pool columns: put back; of the destroyed, those gone for idleness)
uint8_t pool_pressure = 0;   // 0: normal, 1: pressed (>90%, halve pool), 2: critical (>97%, flush pool)
bool compact_on() {   // (longshot: the textures in a pool of their own, compacted: compact_step. On unless RX_VK_COMPACT=0)
    static const bool v = vram_compact::enabled(getenv("RX_VK_COMPACT"));
    return v;
}
// (longshot) the commit the textures leave free for the rest of the system: texture_frame's commit rule. RX_VK_COMMIT_RESERVE_MB
// (default 2560, 0: off). Windows only: Linux overcommits, and its limit is not one commit can reach
static uint64_t commit_reserve() {
    static const uint64_t v = getenv("RX_VK_COMMIT_RESERVE_MB") ? (uint64_t)std::max(0, atoi(getenv("RX_VK_COMMIT_RESERVE_MB"))) << 20
                                                                : 2560ull << 20;
    return v;
}
#if defined(_WIN32)
// (what Windows' page files can still grow by: commit_room.h. NtQuerySystemInformation's SystemPageFileInformationEx gives each
// file's size and its maximum, which for a file Windows manages is the one Windows chose; read again after a second. 0 where it
// cannot be read: then the room is what is left under the limit now, as before)
static uint64_t page_file_growth() {
    static uint64_t value;
    static ULONGLONG at;
    const ULONGLONG now = GetTickCount64();
    if (at && now - at < 1000) return value;
    at = now ? now : 1, value = 0;
    using Query = LONG(WINAPI*)(int, void*, ULONG, ULONG*);
    static const Query query = (Query)(void*)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformation");
    alignas(16) static unsigned char buf[16384];   // (a record is 40 bytes and its name; Windows has at most 16 page files)
    ULONG len = 0;
    if (!query || query(0x90, buf, sizeof buf, &len) < 0 || len > sizeof buf) return 0;
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    commit_room::PageFile files[16];
    const size_t n = commit_room::page_files(buf, len, si.dwPageSize, files, 16);
    value = commit_room::growth(files, n, [](char drive) -> uint64_t {
        const wchar_t root[] = {(wchar_t)drive, L':', L'\\', 0};
        ULARGE_INTEGER free_bytes{};
        return GetDiskFreeSpaceExW(root, nullptr, nullptr, &free_bytes) ? free_bytes.QuadPart : 0;
    });
    return value;
}
#endif
// (what Windows lets the process commit before its limit, RAM plus the page file: GlobalMemoryStatusEx's ullAvailPageFile, the
// limit as it is now less the whole system's commit, and below the reserve what the page files can still grow by: a page file
// Windows manages grows as the commit nears the limit, and the limit with it. false where it cannot be read)
static bool commit_headroom(uint64_t* free_bytes) {
#if defined(_WIN32)
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    if (!GlobalMemoryStatusEx(&m)) return false;
    uint64_t grow = 0;
    if (commit_room::wants_growth(m.ullAvailPageFile, commit_reserve())) {
        grow = page_file_growth();
        static uint64_t said_at;
        if (grow && (!said_at || frame_no - said_at >= 3600)) {
            said_at = frame_no;
            logf("vk: commit: %llu MB left under Windows' limit as it is now, and the page files can grow by %llu MB more: counted "
                 "as room", (unsigned long long)(m.ullAvailPageFile >> 20),
                 (unsigned long long)(grow >> 20));
        }
    }
    *free_bytes = commit_room::headroom(m.ullAvailPageFile, grow);
    return true;
#else
    (void)free_bytes;
    return false;
#endif
}
static uint64_t linux_avail_reserve() {
    static const uint64_t v = linux_pressure::avail_reserve(getenv("RX_VK_AVAIL_RESERVE_MB"));
    return v;
}
static bool linux_pressed = false;
static bool linux_entered_pressure = false;
// (longshot, #1879) set by a submit the driver refused for want of memory (texture_memory_refused, vk_replay.cpp submit_give_back);
// texture_frame takes it: the idle staging arenas go as when Linux's memory pressure begins, and the reaper's queue with them
static std::atomic<bool> memory_refused{false};
static std::atomic<uint64_t> linux_shortfall_bytes{0}, linux_shortfall_seq{0};
uint64_t pool_cap() {
    // (longshot) RX_VK_TARGET_POOL_MB=<MB>, else a twelfth of what the driver allows, 256 to 768 MB (target_pool.h)
    static const uint64_t mb = [] {
        if (const char* e = getenv("RX_VK_TARGET_POOL_MB")) return (uint64_t)std::max(0, atoi(e));
        uint64_t used = 0, allowed = 0;
        const uint64_t v = target_pool::default_mb(vram_usage(&used, &allowed) ? allowed : 0);
        logf("vk: render target pool: up to %llu MB (RX_VK_TARGET_POOL_MB)", (unsigned long long)v);
        return v;
    }();
    return mb << 20;
}
uint64_t pool_key_of(const VkImageCreateInfo& ci) {
    uint64_t k = mix((uint64_t)ci.flags, (uint64_t)ci.imageType << 32 | (uint64_t)ci.format);
    k = mix(k, (uint64_t)ci.extent.width << 32 | ci.extent.height);
    k = mix(k, (uint64_t)ci.extent.depth << 32 | ci.mipLevels);
    k = mix(k, (uint64_t)ci.arrayLayers << 32 | (uint64_t)ci.samples);
    k = mix(k, (uint64_t)ci.usage << 8 | (uint64_t)ci.tiling);
    return k | 1;   // (0: not pooled)
}
/// An image leaving the pool leaves the class index
void pool_unindex(std::list<Pooled>::iterator it) {
    if (!it->classed) return;
    auto c = by_class.find(target_pool::class_key(it->memory_type, it->alloc_bytes));
    if (c == by_class.end()) return;
    if (auto at = std::find(c->second.begin(), c->second.end(), it); at != c->second.end()) c->second.erase(at);
    if (c->second.empty()) by_class.erase(c);
}
bool pool_take(uint64_t key, VkImage& image, VmaAllocation& allocation, std::unordered_map<uint64_t, VkImageView>& views) {
    std::lock_guard lk(pool_mu);
    auto it = target_pool.find(key);
    if (it == target_pool.end() || it->second.empty()) {
        if (auto t = taken_by_size.find(key); t != taken_by_size.end()) st_pool_stolen++, taken_by_size.erase(t);
        return false;
    }
    const auto node = it->second.back();
    pool_unindex(node);
    Pooled& p = *node;
    taken_by_size.erase(key);   // (its shape found its own: a later miss of it is not one a take by size caused)
    image = p.image, allocation = p.allocation, views = std::move(p.views);
    pool_bytes -= std::min(pool_bytes, p.bytes), pool_n--;
    it->second.pop_back();
    pool_by_age.erase(node);
    if (it->second.empty()) target_pool.erase(it);
    st_pool_reused++;
    return true;
}
void pool_destroy(Pooled& p) {
    std::vector<VkImageView> views;
    for (auto& kv : p.views)
        if (kv.second) views.push_back(kv.second);
    destroy_image_later(p.image, p.allocation, std::move(views));   // (on the reaper's thread: vk_vma.cpp)
    st_pool_destroyed++;
}
/// (texture_frame) a dead render target's image the GPU is done with, into the pool
void pool_put(Dead& d) {
    struct { uint64_t bytes = 0, offset = 0; uint32_t memory_type = UINT32_MAX; bool dedicated = true; } fit;
    allocation_fit_info(d.allocation, &fit.bytes, &fit.offset, &fit.memory_type, &fit.dedicated);
    std::lock_guard lk(pool_mu);
    st_pool_put++;   // (TEST, local)
    pool_by_age.push_back({d.image, d.allocation, std::move(d.view_map), d.bytes, frame_no, d.pool_key, fit.bytes, fit.offset,
                           fit.memory_type, fit.dedicated});
    const auto node = std::prev(pool_by_age.end());
    target_pool[d.pool_key].push_back(node);
    taken_by_size.erase(d.pool_key);   // (an image of its shape is back: a later miss of it is the pool's idle rule's, not a take's)
    if (!fit.dedicated && fit.bytes == target_pool::class_bytes(fit.bytes, target_block_bytes())) {
        node->classed = true;
        by_class[target_pool::class_key(fit.memory_type, fit.bytes)].push_back(node);
    }
    pool_bytes += d.bytes, pool_n++;
}
// (longshot) RX_VK_TARGET_REUSE_MEMORY: 0 off, 1 a pooled image's memory taken by size, 2 also the size classes; unset, 2 on an
// integrated GPU and 1 elsewhere (target_pool::reuse_level)
int reuse_memory_level() {
    static const int v =
        target_pool::reuse_level(getenv("RX_VK_TARGET_REUSE_MEMORY"), dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU);
    return v;
}
bool reuse_memory_on() { return reuse_memory_level() >= 1; }
/// (create, after pool_take missed) a render target's new image on the memory of a pooled one it fits in (target_pool::fits), the
/// smallest that does and has waited a present (target_pool::takeable_by_size): OoT's shadow maps get a side of their own every frame,
/// so their create info never comes back, and each was a new allocation (on a Steam Deck a block's kernel call) while one of nearly
/// the same size waited here. Only the pool's images: the dead queue's may still be in the GPU's work. The new image is bound
/// first; then the pooled image and its views are destroyed, at once as pool_destroy does: pool_put takes only what the GPU is done
/// with. Its allocation becomes the new image's, which create counts at the allocation's size (x.pad) and which goes back to the
/// pool, or is freed, with it
bool pool_take_memory(ImageNeed& need, VkImage& image, VmaAllocation& allocation) {
    using target_pool::Miss;
    if (!reuse_memory_on()) return false;
    std::lock_guard lk(pool_mu);
    if (pool_by_age.empty()) {
        st_miss[(int)(dead_pooled_n.load(std::memory_order_relaxed) ? Miss::empty_dead_waiting : Miss::empty)]++;
        return false;
    }
    const VkMemoryRequirements* asked = need.get();
    if (!asked) {   // (also when the image must have memory of its own)
        st_miss[(int)Miss::requirements]++;
        return false;
    }
    const VkMemoryRequirements& mr = *asked;
    if (mr.size > target_block_bytes()) {   // (memory of its own: no share of the targets' blocks holds it)
        st_miss[(int)Miss::over_block]++;
        return false;
    }
    const uint32_t types = mr.memoryTypeBits & device_local_types();   // (in VRAM: not one a full VRAM put in system memory)
    auto best = pool_by_age.end();
    // (first the oldest of its size class, of a memory type it can use: one look-up)
    if (const uint64_t cls = target_pool::class_bytes(mr.size, target_block_bytes()))
        for (uint32_t t = 0; t < 32 && best == pool_by_age.end(); t++) {
            if (!(types >> t & 1)) continue;
            auto c = by_class.find(target_pool::class_key(t, cls));
            if (c == by_class.end()) continue;
            const auto it = c->second.front();
            if (target_pool::takeable_by_size(frame_no, it->since, target_pool::kSizeTakeIdle) &&
                target_pool::fits(it->alloc_bytes, it->alloc_offset, it->memory_type, it->dedicated, mr.size, mr.alignment, types))
                best = it;
        }
    const bool from_class = best != pool_by_age.end();
    target_pool::Tally seen;
    for (auto it = pool_by_age.begin(); !from_class && it != pool_by_age.end(); ++it) {
        const bool takeable = target_pool::takeable_by_size(frame_no, it->since, target_pool::kSizeTakeIdle);
        const target_pool::Fit f = takeable ? target_pool::verdict(it->alloc_bytes, it->alloc_offset, it->memory_type, it->dedicated,
                                                                   mr.size, mr.alignment, types)
                                            : target_pool::Fit::fits;
        target_pool::add(seen, takeable, f, it->alloc_bytes);
        if (takeable && f == target_pool::Fit::fits && (best == pool_by_age.end() || it->alloc_bytes < best->alloc_bytes)) best = it;
    }
    if (best == pool_by_age.end()) {   // (none: the scan saw them all)
        const Miss why = target_pool::miss_reason(seen);
        st_miss[(int)why]++;
        if (why == Miss::too_small) st_small_gap_pct += (mr.size - seen.largest_small) * 100 / mr.size;
        return false;
    }
    VkImage made = VK_NULL_HANDLE;
    if (!create_aliasing_image(*need.ci, best->allocation, made)) {
        st_miss[(int)Miss::not_bound]++;
        return false;
    }
    for (auto& kv : best->views)
        if (kv.second) vkDestroyImageView(dev.device, kv.second, nullptr);
    vkDestroyImage(dev.device, best->image, nullptr);
    auto bucket = target_pool.find(best->key);
    if (bucket != target_pool.end()) {
        auto& q = bucket->second;
        if (auto at = std::find(q.begin(), q.end(), best); at != q.end()) q.erase(at);
        if (q.empty()) target_pool.erase(bucket);
    }
    image = made, allocation = best->allocation;
    taken_by_size[best->key] = frame_no;
    pool_unindex(best);
    pool_bytes -= std::min(pool_bytes, best->bytes), pool_n--;
    pool_by_age.erase(best);
    st_pool_resized++;
    return true;
}
/// (texture_frame, each present) the pool over its bound, or all of it while VRAM is short, and what waits in it unused for
/// RX_VK_POOL_IDLE presents: destroyed, at most 16 a present (64 while VRAM is short). Returns how many
size_t pool_trim() {
    // (longshot) RX_VK_POOL_IDLE=<presents>; 64 on an integrated GPU while a pooled image's memory can be taken by size, else 8
    // (120 before): target_pool::idle_presents
    static const uint64_t idle = target_pool::idle_presents(getenv("RX_VK_POOL_IDLE"), reuse_memory_level(),
                                                            dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU);
    std::lock_guard lk(pool_mu);
    const uint64_t full_cap = pool_cap();
    const uint64_t cap = (pool_pressure >= 2) ? 0 : (pool_pressure == 1) ? (full_cap / 2) : full_cap;
    const size_t most = (pool_pressure > 0) ? 64 : 16;
    size_t n = 0;
    while (!pool_by_age.empty() && n < most) {
        Pooled& oldest = pool_by_age.front();
        if (pool_bytes <= cap && !target_pool::idle_out(frame_no, oldest.since, idle)) break;
        if (pool_bytes <= cap) st_pool_idle_out++;   // (TEST, local)
        ++(pool_bytes > cap ? st_pool_trim_cap : st_pool_trim_idle);
        pool_unindex(pool_by_age.begin());
        auto bucket = target_pool.find(oldest.key);
        bucket->second.pop_front();
        if (bucket->second.empty()) target_pool.erase(bucket);
        pool_destroy(oldest);
        pool_bytes -= oldest.bytes;
        --pool_n;
        pool_by_age.pop_front();
        ++n;
    }
    for (auto t = taken_by_size.begin(); t != taken_by_size.end();)
        t = target_pool::idle_out(frame_no, t->second, idle) ? taken_by_size.erase(t) : std::next(t);
    return n;
}

// (each thread's own: the display thread's present records barriers too; declared before upload_astc_gpu resets it)
thread_local VkCommandBuffer last_barrier_cmd = VK_NULL_HANDLE;   // (the command buffer this thread's last full barrier went into, and the work count then)

// ---- the recording's upload command buffer ----
thread_local VkCommandBuffer up_cmd = VK_NULL_HANDLE;
uint64_t rec_value = 0;
thread_local bool up_used = false;
thread_local uint64_t up_astc_texels = 0;
uint32_t epoch = 0;
int64_t st_uploads, st_astc, st_bytes;

VkCommandBuffer upload_cmd() {
    if (!up_used) {
        up_used = true;
        full_barrier(up_cmd);   // (after the work submitted before: it may still read what is replaced)
    }
    return up_cmd;
}

// ---- staging: host-visible arenas, reused once the submission that filled them completed ----
struct Arena {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    uint8_t* mapped = nullptr;
    uint64_t size = 0, used = 0, value = 0;   // value: the last submission that reads it
    std::chrono::steady_clock::time_point used_at;   // (when it last staged something)
};
std::vector<Arena> arenas;
constexpr uint64_t kArena = 16ull << 20;   // (16 MB arenas)
// (every arena staging() made, and the time making them took: the memory line's "made so far")
std::atomic<uint64_t> st_arena_makes{0};
std::atomic<int64_t> st_arena_make_ns{0};

struct Staging {
    VkBuffer buffer = VK_NULL_HANDLE;
    uint64_t offset = 0;
    uint8_t* data = nullptr;
    VmaAllocation allocation = nullptr;
};
uint64_t completed_value = 0;
// (GPU writes into guest memory by buffers: GPU address -> (end, the submission that wrote it last))
GpuWriteHistory gpu_wrote;
std::mutex gpu_wrote_mu;
}  // namespace
uint64_t gpu_wrote_into(const Texture& tex);
void texture_memory_refused() { memory_refused.store(true, std::memory_order_relaxed); }
void helper_thread_start() {
#ifndef _WIN32
    GetCurrentThreadId();
#endif
}

VkResult shared_compute_pipeline(const VkComputePipelineCreateInfo* ci, VkPipeline* out);   // (vk_draw.cpp: through the pipeline cache kept across runs)
/// (longshot) A texture pipeline (the ASTC decoder, the BC encoders, the GPU unswizzle) through the pipeline cache kept across
/// runs, so that a later start finds it there; with none where that one is refused. Compiled without a cache the first time a
/// texture needs it, inside a frame on the render thread, one costs a frame of hundreds of ms (the ASTC decoder's ~270 ms):
/// texture_pipelines_warm makes them before the game. vk_draw.cpp's upscale and mask helpers are made here too.
/// extern_guarded_compute asks once more without the optimizer where the driver refused the optimized build: Intel's Windows
/// driver on the Arc A-series (an A580 on 101.9175, #1874) did, and with the ASTC decoder refused, every ASTC texture was
/// decoded on the CPU (a frame's uploads 30.9 s, the screen black after the opening cutscene).
/// The caller holds no IntelDriverGate: the shared path takes that lock itself
VkResult texture_compute_pipeline(const VkComputePipelineCreateInfo& ci, VkPipeline& out) {
    out = VK_NULL_HANDLE;
    if (shared_compute_pipeline(&ci, &out) == VK_SUCCESS && out) return VK_SUCCESS;
    out = VK_NULL_HANDLE;
    IntelDriverGate gate;
    return extern_guarded_compute(VK_NULL_HANDLE, &ci, &out);
}
namespace {
Staging staging(uint64_t n, uint64_t align = 256) {
    // Separate slices may already be read by the submission's parts sent before. A flush must not share their cache atom.
    align = std::max<uint64_t>(align, dev.props.limits.nonCoherentAtomSize);
    for (Arena& a : arenas) {   // the current one first, then any whose readers completed
        if (a.value == rec_value) {
            const uint64_t at = (a.used + align - 1) & ~(align - 1);
            if (at + n <= a.size) {
                a.used = at + n;
                a.used_at = std::chrono::steady_clock::now();
                return {a.buffer, at, a.mapped + at, a.allocation};
            }
        }
    }
    for (Arena& a : arenas) {
        if (a.value != rec_value && a.value <= completed_value && a.size >= n) {
            a.value = rec_value, a.used = n;
            a.used_at = std::chrono::steady_clock::now();
            return {a.buffer, 0, a.mapped, a.allocation};
        }
    }
    Arena a;
    void* m = nullptr;
    const auto make = [&](uint64_t size) {
        a.size = size, m = nullptr;
        if (!create_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, a.buffer,
                           a.allocation, &m))
            return false;
        if (m) return true;
        destroy_buffer(a.buffer, a.allocation);   // (made but not mapped: of no use here)
        a.buffer = VK_NULL_HANDLE, a.allocation = nullptr;
        return false;
    };
    const auto t_make = std::chrono::steady_clock::now();
    // (longshot) and one of the size asked for when a 64 MB one cannot be made: without resizable BAR an arena is new system
    // memory the driver must pin. On a 16 GB laptop with the game's pools imported and RAM nearly gone, no 64 MB arena could
    // be made even for a 36 KB upload, the idle ones having been given back (texture_frame), and every upload failed until one
    // was free: BC3 textures drawn as noise (RX 5500M, 2.9 GB of RAM available)
    const uint64_t need = (n + 0xFFFFF) & ~0xFFFFFull;
    if (!make(std::max(kArena, need))) {
        const bool small = need < kArena && make(need);
        static std::atomic<int> told{0};
        if (told++ < 8)
            logf("vk: staging: no %llu MB arena could be made (%zu kept); %s", (unsigned long long)(kArena >> 20), arenas.size(),
                 small ? "one of the size asked for instead" : "the upload waits");
        if (!small) return {};
    }
    {   // (longshot: what making a staging arena costs the replay thread: on a card with resizable BAR it is VRAM the driver hands
        // out, and a scene change made several right after the idle ones were freed)
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_make).count();
        st_arena_makes.fetch_add(1, std::memory_order_relaxed);
        st_arena_make_ns.fetch_add((int64_t)(ms * 1e6), std::memory_order_relaxed);
        static int slow_logged;
        if (ms > 5 && slow_logged++ < 24) logf_at(log_debug, "vk: a staging arena of %llu MB took %.1f ms to make", (unsigned long long)(a.size >> 20), ms);
    }
    a.mapped = (uint8_t*)m;
    a.value = rec_value, a.used = n;
    a.used_at = std::chrono::steady_clock::now();
    arenas.push_back(a);
    static int logged;
    if (logged++ < 4 || arenas.size() % 16 == 0)
        logf("vk: staging arenas: %zu (%llu MB in this one)", arenas.size(), (unsigned long long)(a.size >> 20));
    return {a.buffer, 0, a.mapped, a.allocation};
}

// ---- the GPU ASTC decoder (Eden's) ----
struct AstcPush {
    uint32_t block_dims[2], layer_stride, block_size, x_shift, block_height, block_height_mask, native_tiling;
    uint32_t blocks_x, blocks_y;
    uint32_t srgb;   // 1: the sRGB decode mode (an sRGB NVN format), endpoints widened as (v << 8) | 0x80 rather than v * 257
};
/// The NVN ASTC formats' sRGB half (0x87 to 0x94, as view_format_of reads them): decoded in the specification's sRGB mode, which
/// is what their R8G8B8A8_SRGB view samples
bool astc_nvn_srgb(int nvn) {
    return nvn >= 0x87 && nvn <= 0x94;
}
VkDescriptorSetLayout astc_set = VK_NULL_HANDLE;
VkPipelineLayout astc_layout = VK_NULL_HANDLE;
VkPipeline astc_pipe = VK_NULL_HANDLE;
/// (longshot) ASTC decoded on the CPU, not by the GPU decoder: RX_VK_ASTC_CPU by the switches' rule (off by default, as
/// upload_astc_gpu splits large GPU ASTC uploads across bounded submissions via RX_VK_ASTC_CHUNK_MPX so Kokiri Forest's
/// two 1365 x 1365 x 90 ASTC arrays no longer hold the GPU past Windows' 2 s WDDM timeout). When RX_VK_ASTC_CPU=1 forces CPU
/// decoding, the textures are kept as RGBA8 (no BC3), and texture_frame raises the iGPU texture cap above 16 GB RAM (6 GB at
/// 24 GB, 8 GB above 24 GB) so the RGBA8 arrays do not thrash the 4,096 MB cap
bool astc_on_cpu() {
    static const bool on = [] {
        const bool v = dev.policy.astc_on_cpu;   // (device_policy.h: off by default, then RX_VK_ASTC_CPU)
        if (v) logf("vk: ASTC decoded on the CPU (RX_VK_ASTC_CPU=1; unset: by the GPU)");
        return v;
    }();
    return on;
}
bool astc_ready() {
    static int state = 0;   // 0 untried, 1 ready, -1 unavailable
    if (state) return state > 0;
    state = -1;
    if (!dev.push_descriptor || !vkCmdPushDescriptorSetKHR || astc_on_cpu()) return false;
    const VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 2;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &astc_set) != VK_SUCCESS) return false;
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AstcPush)};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &astc_set;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &astc_layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    const auto spv = BUILTIN_SPV(ASTC_DECODER_COMP_SPV);
    mi.codeSize = spv.size;
    mi.pCode = spv.code;
    VkShaderModule m = VK_NULL_HANDLE;
    {
        IntelDriverGate gate;   // (the module's only: texture_compute_pipeline takes the same lock)
        if (extern_guarded_shader_module(&mi, &m) != VK_SUCCESS) return false;
    }
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = astc_layout;
    const VkResult r = texture_compute_pipeline(ci, astc_pipe);
    vkDestroyShaderModule(dev.device, m, nullptr);
    if (r != VK_SUCCESS) {   // (said: on an Arc A580 its refusal was in no line, only in a frame's uploads of 30 s on the CPU)
        logf("vk: the GPU ASTC decoder's pipeline was not made (%d): ASTC is decoded on the CPU", (int)r);
        return false;
    }
    state = 1;
    logf("vk: ASTC is decoded on the GPU (Eden's decoder, Switch 2 tiling)");
    return true;
}

VkImageView make_view(Texture* tex, VkImageViewType type, VkFormat f, VkImageAspectFlags aspect, uint32_t level0,
                      uint32_t nlevels, uint32_t layer0, uint32_t nlayers, const VkComponentMapping& cm,
                      VkImageUsageFlags usage, VkResult* result = nullptr) {
    if (result) *result = VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (!usage || (usage & tex->usage) != usage) return VK_NULL_HANDLE;
    // (block-texel views of a compressed image: one level; several layers where the driver takes them)
    const bool block_view = is_bc(tex->format) && !is_bc(f);
    if (block_view && nlevels != 1) return VK_NULL_HANDLE;
    VkImageViewUsageCreateInfo uci{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    uci.usage = usage & tex->usage;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.pNext = &uci;
    vi.image = tex->image;
    vi.viewType = type;
    vi.format = f;
    vi.components = cm;
    vi.subresourceRange = {aspect, level0, nlevels, layer0, nlayers};
    VkImageView v = VK_NULL_HANDLE;
    const VkResult first = vkCreateImageView(dev.device, &vi, nullptr, &v);
    if (result) *result = first;
    if (first == VK_SUCCESS) return v;
    if (!block_view || nlayers == 1) return VK_NULL_HANDLE;
    // (Vulkan 1.1 says a block-texel view is of a single layer; some drivers hold to it)
    vi.subresourceRange.layerCount = 1;
    if (vi.viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY) vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    const VkResult last = vkCreateImageView(dev.device, &vi, nullptr, &v);
    if (result) *result = last == VK_SUCCESS || (first != VK_ERROR_OUT_OF_HOST_MEMORY && first != VK_ERROR_OUT_OF_DEVICE_MEMORY)
                              ? last : first;
    if (last != VK_SUCCESS) return VK_NULL_HANDLE;
    return v;
}

/// A failed allocation is not a permanent negative cache entry. At most one failing attempt per texture/frame.
template<class Make>
VkImageView cached_image_view(Texture* tex, uint64_t key, Make&& make) {
    if (auto it = tex->views.find(key); it != tex->views.end()) return it->second;
    if (tex->view_failed_frame == frame_no) return VK_NULL_HANDLE;
    VkResult result = VK_SUCCESS;
    const VkImageView view = make(&result);
    if (!view && (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY)) {
        tex->view_failed_frame = frame_no;
        return VK_NULL_HANDLE;
    }
    try {
        auto [it, inserted] = tex->views.emplace(key, view);
        if (!inserted && view) vkDestroyImageView(dev.device, view, nullptr);
        return it->second;
    } catch (const std::exception&) {
        // The new view has no owner when its cache entry cannot be committed.
        if (view) vkDestroyImageView(dev.device, view, nullptr);
        tex->view_failed_frame = frame_no;
        return VK_NULL_HANDLE;
    }
}

// ---- big ASTC textures kept as BC3 (8 bits a texel, a quarter of RGBA8): decoded into a scratch image a few
// layers at a time, re-encoded by Eden's encoder (host_shaders/bc_encoder.comp: bounding box along the colours'
// covariance, inset; GPL, as the decoder) into a buffer, copied into the image. Decoded RGBA8 took 6 GB of VRAM at
// the title (the terrain's material arrays, 2048 x 2048 x 68, 1.45 GB each): more than the driver allowed, which
// paged VRAM out and evicted textures that were made and uploaded again. ASTC as RGBA8 (the menu's Developer row) or
// RX_VK_ASTC_RGBA8=1: all as RGBA8.
struct BcPush {
    uint32_t blocks_x, blocks_y, width, height, bc3, out_offset;
};
VkDescriptorSetLayout bc_set = VK_NULL_HANDLE;
VkPipelineLayout bc_layout = VK_NULL_HANDLE;
VkPipeline bc_pipe = VK_NULL_HANDLE;
VkPipeline bc7_pipe = VK_NULL_HANDLE;
bool bc7_ok = true;   // (false once BC7 is unsupported or its pipeline could not be made: BC3 from then on)
constexpr uint32_t kBcDim = 2048, kBcLayers = 8;   // the scratch: the largest level a BC texture may have
/// The scratch's layers, which are the layers one run decodes and encodes: 1, or RX_VK_BC_LAYERS=<n> up to kBcLayers
/// (upload_astc_gpu says why one). It had kBcLayers whatever the runs used: 140 of its 160 MB held and never written.
uint32_t bc_layers() {
    static const uint32_t n = getenv("RX_VK_BC_LAYERS") ? std::clamp<uint32_t>((uint32_t)atoi(getenv("RX_VK_BC_LAYERS")), 1u, kBcLayers) : 1u;
    return n;
}
/// (longshot) RX_VK_ASTC_BC7=1: big ASTC textures as BC7 instead of BC3. Both are 8 bits a texel; BC7's mode 6 keeps
/// the colours' gradients and alpha much better (the encoder is host_shaders/bc7_encoder.comp).
/// (longshot) The small scratch: levels no larger than kBcSmallDim (the terrain's material arrays from their second level
/// down) decode and encode bc_small_layers() layers a run through it, not one. A run is a decode, an encode, a copy and
/// three full barriers: an array of 90 layers and 11 levels made ~990 of them, and the GPU spent most of the time between
/// them (Hyrule Field's entry on an RX 9070 XT: 51 ms an array at one layer a run, 15 ms at 8; #1720). 36 MB (32 for the
/// image, 4 more of the output buffer), beside the 2048 x 2048 scratch's 20; on a card of small VRAM
/// (low_vram_texture_mips) it is kBcTinyDim a side; not made where it would land in system memory. Its layers: 8 on
/// RADV, NVIDIA's driver and AMD's Windows driver, 1 (no small scratch, every run one layer, and no groups of levels) on
/// every other (device_policy.h bc_small_runs); RX_VK_BC_SMALL_LAYERS=<n> over that. On AMD's Windows driver (diagnosed
/// on RDNA 2: RX 6700 XT, RX 6600), a multi-layer VkBufferImageCopy into a non-4-aligned compressed mip tail (e.g. 5x5
/// mip 8) truncates to floor(w/4) x floor(h/4) blocks, leaving edge blocks zeroed (Hyrule Field's grid lines: ec89f92e,
/// #2006, #2107, #2110); dev.policy.bc_copy_per_layer splits those copies into one region per layer in bc_encode_into so
/// 8-layer runs stay clean across AMD's Windows driver. Runs still never hold a repacked level (upload_astc_gpu)
constexpr uint32_t kBcSmallDim = 1024;
/// (longshot) The small scratch on a card of small VRAM (low_vram_texture_mips), where the 1024 x 1024 one (36 MB) was not
/// made at all: 256 x 256 and bc_small_layers() layers, 2 MB with its share of the output buffer. Such a card keeps all its large ASTC
/// textures as BC3 and had every level as a run of its own: a single-layer texture's levels this one holds go one a layer, a
/// group a run (bc_levels_on): 9 full barriers for a 1024 x 1024 texture where there were 33. For the groups alone: a layer
/// array's levels go through the first scratch there, one layer a run, as before; RX_VK_BC_LEVELS=0 makes none, as before
constexpr uint32_t kBcTinyDim = 256;
uint32_t bc_small_px = kBcSmallDim;   // (the small scratch's side as it was made: bc_scratch_make, bc_mu held)
uint32_t bc_small_layers() {
    static const uint32_t n = getenv("RX_VK_BC_SMALL_LAYERS") ? std::clamp<uint32_t>((uint32_t)atoi(getenv("RX_VK_BC_SMALL_LAYERS")), 1u, kBcLayers)
                                                              : dev.policy.bc_small_runs ? kBcLayers : 1u;
    return n;
}
bool bc7_enabled() {
    static const bool on = [] {
        const char* e = getenv("RX_VK_ASTC_BC7");
        return e && atoi(e) != 0;
    }();
    return on && bc7_ok;
}
VkImage bc_scratch = VK_NULL_HANDLE;
VmaAllocation bc_scratch_alloc = nullptr;
VkImageView bc_scratch_view = VK_NULL_HANDLE;
// (the three scratch views' R8G8B8A8_UINT twins, which the ASTC decoder writes: it stores the bytes as integers, an integer format
// keeping the bits as they are. The scratch images are MUTABLE_FORMAT; the BC encoders read the UNORM views)
VkImageView bc_scratch_uview = VK_NULL_HANDLE, bc_small_uview = VK_NULL_HANDLE;
VkBuffer bc_out = VK_NULL_HANDLE;
VmaAllocation bc_out_alloc = nullptr;
VkImage bc_small = VK_NULL_HANDLE;   // (the small scratch, made and given back with the first; none at RX_VK_BC_SMALL_LAYERS=1)
VmaAllocation bc_small_alloc = nullptr;
VkImageView bc_small_view = VK_NULL_HANDLE;
// (a view of each of the small scratch's layers: a single-layer texture's levels are decoded one into each. RX_VK_BC_LEVELS=0:
// none, each level a run of its own, and no small scratch on a card of small VRAM, as before)
VkImageView bc_small_layer[8] = {};
VkImageView bc_small_layer_u[8] = {};   // (and their UINT twins)
bool bc_levels_on() {
    static const bool on = !getenv("RX_VK_BC_LEVELS") || atoi(getenv("RX_VK_BC_LEVELS")) != 0;
    return on;
}
bool bc_scratch_fresh = false;   // (its layout still to be set, in the next upload command buffer)
// (longshot-amd) a second scratch for textures re-encoded at another size (uneven block levels on AMD): the decoded level
// is blitted into it, filtered, at the image's size, and encoded from there. Made when an upload of such a texture first
// needs it, given back with the first scratch.
VkImage bc_scratch2 = VK_NULL_HANDLE;
VmaAllocation bc_scratch2_alloc = nullptr;
VkImageView bc_scratch2_view = VK_NULL_HANDLE;
bool bc_scratch2_fresh = false;
// (the scratch and its buffer, 20 MB a layer, are made when an upload first needs them and given back after RX_VK_BC_IDLE_S seconds
// (20) without one: a title or a load decodes hundreds of textures through them, steady gameplay few (the "given back" line says
// whether it did).
// 0: kept for good once made. They have memory objects of their own, so that freeing gives the VRAM back: VMA keeps the empty
// block of its image pool for the next images. RX_VK_BC_POOL=1: from the pool as before)
std::mutex bc_mu;
std::chrono::steady_clock::time_point bc_used_at;
uint64_t bc_last_value = 0;   // (the recording that last used them: they go once it is done)
std::atomic<bool> bc_have{false};
uint32_t bc_idle_s() {
    // (longshot) 0 by default, kept once made: given back after an idle time, garbled/rainbow textures follow on 4-8 GB cards
    // (they go with RX_VK_BC_IDLE_S=0, not with RX_VK_NO_EVICT=1). Its cost: 20-36 MB of VRAM held after the first decode.
    // RX_VK_BC_IDLE_S=<seconds>: given back after that long
    static const uint32_t v = getenv("RX_VK_BC_IDLE_S") ? (uint32_t)atoi(getenv("RX_VK_BC_IDLE_S")) : 0;
    return v;
}
bool low_vram_texture_mips();   // (below: a card of small VRAM makes no small scratch)
bool bc_scratch_make() {   // (bc_mu held)
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {kBcDim, kBcDim, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = bc_layers();
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;   // (written through a UINT view, read through a UNORM one)
    static const bool pool = env_on("RX_VK_BC_POOL");
    if (!create_image(ii, bc_scratch, bc_scratch_alloc, !pool)) return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = bc_scratch;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, bc_layers()};
    // (the same view as R8G8B8A8_UINT)
    const auto uview = [](VkImageViewCreateInfo v, VkImageView& out) {
        v.format = VK_FORMAT_R8G8B8A8_UINT;
        if (vkCreateImageView(dev.device, &v, nullptr, &out) == VK_SUCCESS) return true;
        out = VK_NULL_HANDLE;
        return false;
    };
    const bool tiny = low_vram_texture_mips() && bc_levels_on();   // (a card of small VRAM: the 256 x 256 one)
    if (bc_small_layers() > 1 && (!low_vram_texture_mips() || tiny)) {   // (not made: its levels go through the first scratch, one layer a run)
        bc_small_px = tiny ? kBcTinyDim : kBcSmallDim;
        VkImageCreateInfo si = ii;
        si.extent = {bc_small_px, bc_small_px, 1};
        si.arrayLayers = bc_small_layers();
        const char* why = nullptr;
        if (!create_image(si, bc_small, bc_small_alloc, !pool)) why = "could not be made";
        else if (!allocation_in_vram(bc_small_alloc)) why = "would be in system memory";
        else {
            VkImageViewCreateInfo sv = vi;
            sv.image = bc_small;
            sv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, bc_small_layers()};
            if (vkCreateImageView(dev.device, &sv, nullptr, &bc_small_view) != VK_SUCCESS || !uview(sv, bc_small_uview)) why = "has no view";
            for (uint32_t l = 0; !why && bc_levels_on() && l < bc_small_layers(); l++) {   // (a view a layer: a group's levels)
                sv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, l, 1};
                if (vkCreateImageView(dev.device, &sv, nullptr, &bc_small_layer[l]) != VK_SUCCESS || !uview(sv, bc_small_layer_u[l])) {
                    for (uint32_t k = 0; k < 8; k++) {
                        if (bc_small_layer[k]) vkDestroyImageView(dev.device, bc_small_layer[k], nullptr);
                        if (bc_small_layer_u[k]) vkDestroyImageView(dev.device, bc_small_layer_u[k], nullptr);
                        bc_small_layer[k] = bc_small_layer_u[k] = VK_NULL_HANDLE;
                    }
                    break;
                }
            }
        }
        if (why) {
            for (uint32_t k = 0; k < 8; k++) {
                if (bc_small_layer[k]) vkDestroyImageView(dev.device, bc_small_layer[k], nullptr);
                if (bc_small_layer_u[k]) vkDestroyImageView(dev.device, bc_small_layer_u[k], nullptr);
                bc_small_layer[k] = bc_small_layer_u[k] = VK_NULL_HANDLE;
            }
            if (bc_small_view) vkDestroyImageView(dev.device, bc_small_view, nullptr);
            if (bc_small_uview) vkDestroyImageView(dev.device, bc_small_uview, nullptr);
            if (bc_small) destroy_image(bc_small, bc_small_alloc);
            bc_small = VK_NULL_HANDLE, bc_small_alloc = nullptr, bc_small_view = VK_NULL_HANDLE, bc_small_uview = VK_NULL_HANDLE;
            static std::atomic<int> told{0};
            if (told++ < 2) logf("vk: the small BC3 scratch %s: its levels go one layer a run", why);
        } else {
            static std::atomic<int> told{0};
            if (told++ < 1)
                logf("vk: the small BC3 scratch: %u x %u, %u layers%s", bc_small_px, bc_small_px, bc_small_layers(),
                     tiny ? " (a card of small VRAM)" : "");
        }
    }
    const uint64_t big_out = (uint64_t)(kBcDim / 4) * (kBcDim / 4) * bc_layers() * 16;
    const uint64_t small_out = bc_small ? (uint64_t)(bc_small_px / 4) * (bc_small_px / 4) * bc_small_layers() * 16 : 0;
    const uint64_t out = std::max(big_out, small_out);   // (one output buffer: a run of either scratch)
    const VkBufferUsageFlags out_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    const auto make_out = [&] {
        if (create_device_buffer(out, out_usage, bc_out, bc_out_alloc, !pool)) return true;
        // (longshot) VRAM full, the process at its budget (a 4 GB card, the textures evicted after an alt-tab): in system
        // memory, as create_image puts the scratch image there. Only in VRAM, it could not be made, every BC3 texture made
        // again then failed, and they were drawn as coloured noise
        if (!create_buffer(out, out_usage, false, bc_out, bc_out_alloc, nullptr, !pool)) return false;
        static std::atomic<int> told{0};
        if (told++ < 4) logf("vk: VRAM full: the BC3 scratch's output buffer (%llu MB) in system memory", (unsigned long long)(out >> 20));
        return true;
    };
    if (vkCreateImageView(dev.device, &vi, nullptr, &bc_scratch_view) != VK_SUCCESS || !uview(vi, bc_scratch_uview) || !make_out()) {
        if (bc_scratch_view) vkDestroyImageView(dev.device, bc_scratch_view, nullptr);
        if (bc_scratch_uview) vkDestroyImageView(dev.device, bc_scratch_uview, nullptr);
        destroy_image(bc_scratch, bc_scratch_alloc);
        bc_scratch = VK_NULL_HANDLE, bc_scratch_view = VK_NULL_HANDLE, bc_scratch_uview = VK_NULL_HANDLE;
        for (uint32_t k = 0; k < 8; k++) {
            if (bc_small_layer[k]) vkDestroyImageView(dev.device, bc_small_layer[k], nullptr);
            if (bc_small_layer_u[k]) vkDestroyImageView(dev.device, bc_small_layer_u[k], nullptr);
            bc_small_layer[k] = bc_small_layer_u[k] = VK_NULL_HANDLE;
        }
        if (bc_small_view) vkDestroyImageView(dev.device, bc_small_view, nullptr);
        if (bc_small_uview) vkDestroyImageView(dev.device, bc_small_uview, nullptr);
        if (bc_small) destroy_image(bc_small, bc_small_alloc);
        bc_small = VK_NULL_HANDLE, bc_small_alloc = nullptr, bc_small_view = VK_NULL_HANDLE, bc_small_uview = VK_NULL_HANDLE;
        return false;
    }
    bc_scratch_fresh = true;
    bc_have = true;
    return true;
}

bool bc_ready() {
    static int state = 0;
    if (state) return state > 0;
    state = -1;
    // (the Developer section's ASTC as RGBA8 row, or RX_VK_ASTC_RGBA8 for a run: no BC3, the AMD texture workaround)
    const char* rgba8 = getenv("RX_VK_ASTC_RGBA8");
    if (!astc_ready() || (rgba8 ? !env_value_off(rgba8) : opts.astc_rgba8 != 0) || !dev.features.textureCompressionBC) return false;
    bool bc7 = bc7_enabled();
    VkFormat bc_format = bc7 ? VK_FORMAT_BC7_UNORM_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(dev.physical, bc_format, &fp);
    if (bc7 && !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        // (this GPU has BC but not BC7: the texture is BC3, not RGBA8)
        bc7 = false, bc7_ok = false;
        bc_format = VK_FORMAT_BC3_UNORM_BLOCK;
        vkGetPhysicalDeviceFormatProperties(dev.physical, bc_format, &fp);
    }
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return false;
    const VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 2;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &bc_set) != VK_SUCCESS) return false;
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BcPush)};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &bc_set;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &bc_layout) != VK_SUCCESS) return false;
    auto make_pipe = [&](const uint32_t* code, size_t bytes, VkPipeline& pipe) {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = bytes;
        mi.pCode = code;
        VkShaderModule m = VK_NULL_HANDLE;
        {
            IntelDriverGate gate;   // (the module's only: texture_compute_pipeline takes the same lock)
            if (extern_guarded_shader_module(&mi, &m) != VK_SUCCESS) return false;
        }
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = m;
        ci.stage.pName = "main";
        ci.layout = bc_layout;
        const VkResult r = texture_compute_pipeline(ci, pipe);
        vkDestroyShaderModule(dev.device, m, nullptr);
        return r == VK_SUCCESS;
    };
    if (!make_pipe(BUILTIN_SPV_ARGS(BC_ENCODER_COMP_SPV), bc_pipe)) return false;
    if (bc7 && !make_pipe(BUILTIN_SPV_ARGS(BC7_ENCODER_COMP_SPV), bc7_pipe)) {
        // (the BC7 pipeline could not be made: BC3, not RGBA8. The next bc7_enabled() answers false too, so the encoders
        // and the textures made from now on take bc_pipe)
        bc7_ok = false, bc7 = false;
    }
    state = 1;
    logf("vk: big ASTC textures are kept as %s (re-encoded on the GPU; ASTC as RGBA8 or RX_VK_ASTC_RGBA8=1: as RGBA8)", bc7 ? "BC7" : "BC3");
    return true;
}

/// (longshot-amd) the second scratch (see bc_scratch2; bc_mu held)
bool bc_scratch2_make() {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {kBcDim, kBcDim, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = bc_layers();
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    static const bool pool = env_on("RX_VK_BC_POOL");
    if (!create_image(ii, bc_scratch2, bc_scratch2_alloc, !pool)) return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = bc_scratch2;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, bc_layers()};
    if (vkCreateImageView(dev.device, &vi, nullptr, &bc_scratch2_view) != VK_SUCCESS) {
        destroy_image(bc_scratch2, bc_scratch2_alloc);
        bc_scratch2 = VK_NULL_HANDLE, bc_scratch2_view = VK_NULL_HANDLE;
        return false;
    }
    bc_scratch2_fresh = true;
    return true;
}

/// (longshot-amd) scratch [0, w) x [0, h), layers [0, n), blitted (linear) into scratch2 at ow x oh, then encoded from there
void bc_encode_resized(VkCommandBuffer cmd, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, uint32_t n, Texture& dst,
                       uint32_t level, uint32_t layer);

/// Encodes scratch layers [0, n) (a w x h region at its corner) and copies the blocks into dst's level and layers
void bc_encode_into(VkCommandBuffer cmd, uint32_t w, uint32_t h, uint32_t n, Texture& dst, uint32_t level,
                    uint32_t layer, int dx, int dy, VkImageView from = VK_NULL_HANDLE) {
    full_barrier(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bc7_enabled() ? bc7_pipe : bc_pipe);
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, from ? from : bc_scratch_view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bi{bc_out, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wr[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    wr[0].dstBinding = 0, wr[0].descriptorCount = 1, wr[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    wr[0].pImageInfo = &ii;
    wr[1].dstBinding = 1, wr[1].descriptorCount = 1, wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wr[1].pBufferInfo = &bi;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bc_layout, 0, 2, wr);
    descriptors_disturbed();
    const BcPush pc{(w + 3) / 4, (h + 3) / 4, w, h, 1, 0};
    vkCmdPushConstants(cmd, bc_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cmd, (pc.blocks_x + 7) / 8, (pc.blocks_y + 7) / 8, n);
    full_barrier(cmd);
    // (the buffer's rows and layers given in whole blocks, as the encoder wrote them: blocks_x * blocks_y blocks a layer.
    // On AMD's Windows driver (diagnosed on RDNA 2: RX 6700 XT, RX 6600; #2006, #2015), a single multi-layer
    // VkBufferImageCopy (layerCount > 1) into a compressed mip-tail level whose extent is not a multiple of 4 (e.g. 5x5
    // mip 8 on the 1365 x 1365 x 90 terrain arrays) truncates the copy to floor(w/4) x floor(h/4) blocks per layer, leaving
    // the right and bottom edge blocks zeroed (pure black). When dev.policy.bc_copy_per_layer is on (AMD's Windows driver),
    // emit one single-layer region per layer in the same vkCmdCopyBufferToImage call, keeping 8-layer decode/encode runs
    // intact and leaving every other driver on the single-region path.)
    if (dev.policy.bc_copy_per_layer && n > 1 && ((w & 3) || (h & 3))) {
        const uint64_t layer_bytes = (uint64_t)pc.blocks_x * pc.blocks_y * 16;
        for (uint32_t i0 = 0; i0 < n;) {
            VkBufferImageCopy regions[8];
            const uint32_t m = n - i0 < 8 ? n - i0 : 8;
            for (uint32_t j = 0; j < m; ++j)
                regions[j] = {(uint64_t)(i0 + j) * layer_bytes, pc.blocks_x * 4, pc.blocks_y * 4,
                              {VK_IMAGE_ASPECT_COLOR_BIT, level, layer + i0 + j, 1}, {dx, dy, 0}, {w, h, 1}};
            vkCmdCopyBufferToImage(cmd, bc_out, dst.image, VK_IMAGE_LAYOUT_GENERAL, m, regions);
            i0 += m;
        }
    } else {
        const VkBufferImageCopy c{0, pc.blocks_x * 4, pc.blocks_y * 4, {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, n}, {dx, dy, 0}, {w, h, 1}};
        vkCmdCopyBufferToImage(cmd, bc_out, dst.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    full_barrier(cmd);   // (the scratch and the buffer are the next part's)
}

void bc_encode_resized(VkCommandBuffer cmd, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, uint32_t n, Texture& dst,
                       uint32_t level, uint32_t layer) {
    full_barrier(cmd);
    VkImageBlit bl{};
    bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, n};
    bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, n};
    bl.srcOffsets[1] = {(int32_t)w, (int32_t)h, 1};
    bl.dstOffsets[1] = {(int32_t)ow, (int32_t)oh, 1};
    vkCmdBlitImage(cmd, bc_scratch, VK_IMAGE_LAYOUT_GENERAL, bc_scratch2, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
    bc_encode_into(cmd, ow, oh, n, dst, level, layer, 0, 0, bc_scratch2_view);
}

/// The scratch for the upload recording now: made if it is not there, and its layout set (false: it could not be made).
/// resized: the second scratch too (longshot-amd)
bool bc_scratch_layout(VkCommandBuffer cmd, bool resized = false) {
    {
        std::lock_guard lk(bc_mu);
        if (!bc_have && !bc_scratch_make()) return false;
        if (resized && !bc_scratch2 && !bc_scratch2_make()) return false;
        bc_used_at = std::chrono::steady_clock::now();
        bc_last_value = rec_value;
    }
    if (bc_scratch2_fresh && bc_scratch2) {   // (longshot-amd: the second scratch, once made)
        bc_scratch2_fresh = false;
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = bc_scratch2;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, bc_layers()};
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
    }
    if (!bc_scratch_fresh) return true;
    bc_scratch_fresh = false;
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = bc_scratch;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, bc_layers()};
    VkImageMemoryBarrier2 bs = b;   // (the small scratch, made with it)
    bs.image = bc_small;
    bs.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, bc_small_layers()};
    const VkImageMemoryBarrier2 both[2] = {b, bs};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = bc_small ? 2 : 1;
    di.pImageMemoryBarriers = both;
    vkCmdPipelineBarrier2(cmd, &di);
    return true;
}
/// (every present: the scratch unused for RX_VK_BC_IDLE_S seconds goes with the images the GPU is done with)
void bc_scratch_idle() {
    if (!bc_have.load(std::memory_order_relaxed) || !bc_idle_s()) return;
    std::lock_guard lk(bc_mu);
    if (!bc_have || std::chrono::steady_clock::now() - bc_used_at < std::chrono::seconds(bc_idle_s())) return;
    uint64_t freed = allocation_size(bc_scratch_alloc) + allocation_size(bc_out_alloc);
    Dead d{bc_last_value, bc_scratch, bc_scratch_alloc, {bc_scratch_view, bc_scratch_uview}, {}};
    d.buffer = bc_out, d.buffer_alloc = bc_out_alloc;
    dead.push_back(std::move(d));
    bc_scratch = VK_NULL_HANDLE, bc_scratch_alloc = nullptr, bc_scratch_view = VK_NULL_HANDLE, bc_scratch_uview = VK_NULL_HANDLE,
    bc_out = VK_NULL_HANDLE, bc_out_alloc = nullptr;
    bc_have = false, bc_scratch_fresh = false;
    if (bc_small) {
        freed += allocation_size(bc_small_alloc);
        Dead ds{bc_last_value, bc_small, bc_small_alloc, {bc_small_view, bc_small_uview}, {}};
        for (uint32_t k = 0; k < 8; k++) {
            if (bc_small_layer[k]) ds.views.push_back(bc_small_layer[k]);
            if (bc_small_layer_u[k]) ds.views.push_back(bc_small_layer_u[k]);
            bc_small_layer[k] = bc_small_layer_u[k] = VK_NULL_HANDLE;
        }
        dead.push_back(std::move(ds));
        bc_small = VK_NULL_HANDLE, bc_small_alloc = nullptr, bc_small_view = VK_NULL_HANDLE, bc_small_uview = VK_NULL_HANDLE;
    }
    const bool second = bc_scratch2 != VK_NULL_HANDLE;
    if (second) {   // (longshot-amd: the second scratch, 16 MB a layer, with it)
        freed += allocation_size(bc_scratch2_alloc);
        dead.push_back(Dead{bc_last_value, bc_scratch2, bc_scratch2_alloc, {bc_scratch2_view}, {}});
        bc_scratch2 = VK_NULL_HANDLE, bc_scratch2_alloc = nullptr, bc_scratch2_view = VK_NULL_HANDLE, bc_scratch2_fresh = false;
    }
    logf("vk: the BC3 scratch (%u MB%s) unused for %u s: given back", (unsigned)(freed >> 20), second ? ", both" : "", bc_idle_s());
}

/// The storage view of one level (every layer) the ASTC decoder writes: R8G8B8A8_UINT, through which it stores its bytes as they are
/// (a UNORM store of n / 255 is exact only where the GPU rounds to nearest, which Vulkan recommends but does not require). The
/// decoded textures are MUTABLE_FORMAT, and storage use of this format is mandatory
VkImageView astc_target_view(Texture* tex, uint32_t level) {
    const uint64_t key = mix(0xA57C, level);
    return cached_image_view(tex, key, [&](VkResult* result) {
        return make_view(tex, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_FORMAT_R8G8B8A8_UINT, VK_IMAGE_ASPECT_COLOR_BIT,
                         level, 1, 0, tex->layers, {}, VK_IMAGE_USAGE_STORAGE_BIT, result);
    });
}

// ---- uploads ----
struct LevelGeo {
    uint64_t offset;            // from the storage's start
    uint32_t w, h, d;           // texels
    uint32_t tw, th, td;        // blocks
    unsigned tile_h, tile_d;    // block height / depth (log2 GOBs)
    uint64_t swizzled;          // bytes of one layer of it in the storage
};
LevelGeo level_geo(const Texture& tex, uint32_t level) {   // (level: the image's; the storage's is level + skip)
    LevelGeo g{};
    const uint32_t gl = level + tex.skip;
    g.offset = host->tex_level_offset(&tex.d, gl);
    const uint32_t w0 = std::max(1u, tex.d.width);
    const uint32_t h0 = tex.type == VK_IMAGE_TYPE_1D ? 1 : std::max(1u, tex.d.height);
    const uint32_t d0 = tex.type == VK_IMAGE_TYPE_3D ? std::max(1u, tex.d.depth) : 1;
    g.w = std::max(1u, w0 >> gl);
    g.h = std::max(1u, h0 >> gl);
    g.d = std::max(1u, d0 >> gl);
    g.tw = (g.w + tex.block_w - 1) / tex.block_w;
    g.th = (g.h + tex.block_h - 1) / tex.block_h;
    g.td = g.d;
    host->tex_level_tiles(&tex.d, gl, &g.tile_h, &g.tile_d);
    const uint64_t gobs_x = ((uint64_t)g.tw * tex.bpb + 63) / 64;
    const uint64_t rows_y = (g.th + (8ull << g.tile_h) - 1) >> (3 + g.tile_h);
    const uint64_t slices = (g.td + (1ull << g.tile_d) - 1) >> g.tile_d;
    g.swizzled = gobs_x * rows_y * slices * (512ull << (g.tile_h + g.tile_d));
    return g;
}

/// The guest bytes [gpu, gpu + n) in host memory: in place when contiguous, else copied into scratch
const uint8_t* guest_bytes(uint64_t gpu, uint64_t n, std::vector<uint8_t>& scratch) {
    uint64_t avail = 0;
    const uint8_t* p = host->gpu_ptr(gpu, &avail);
    // (below 1 TB the GPU address is the guest's: the storage as it was at the submit being replayed, where the game has
    // rewritten it since (vk_memory.cpp, "texture storage in the capture"))
    if (p && avail >= n) return gpu < (1ull << 40) ? memory_as_submitted(gpu, p, n, scratch) : p;
    if (n >= (32u << 20)) {   // (a big sparse texture's pages, gathered on several threads: the course maps' 349 MB)
        scratch.resize(n);
        const uint32_t nt = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 6u);
        const uint64_t per = ((n + nt - 1) / nt + 0xFFFF) & ~0xFFFFull;
        auto part = [&](uint32_t t) {
            const uint64_t a = (uint64_t)t * per, b = std::min(n, a + per);
            for (uint64_t at = a; at < b; at += 0x10000)
                if (!host->gpu_read(gpu + at, scratch.data() + at, std::min<uint64_t>(0x10000, b - at)))
                    std::memset(scratch.data() + at, 0, std::min<uint64_t>(0x10000, b - at));
        };
        ::mkwvk::cxx_escape::fork_join("texture gather", nt, part, helper_thread_start);
        return scratch.data();
    }
    scratch.assign(n, 0);
    if (!host->gpu_read(gpu, scratch.data(), n)) {
        for (uint64_t at = 0; at < n; at += 0x10000) host->gpu_read(gpu + at, scratch.data() + at, std::min<uint64_t>(0x10000, n - at));
    }
    return scratch.data();
}

/// The alpha an RGB format widened to RGBA gets: one in the format's own encoding (by component size alone, SNORM8 read
/// -0.008, UNORM16 0.23 and the integer formats 255, 15360 or 0x3F800000)
uint32_t widen_alpha(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8_SNORM: return 0x7F;
    case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_R8G8B8_SINT: case VK_FORMAT_R16G16B16_UINT:
    case VK_FORMAT_R16G16B16_SINT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT: return 1;
    case VK_FORMAT_R16G16B16_SFLOAT: return 0x3C00;
    case VK_FORMAT_R16G16B16_UNORM: return 0xFFFF;
    case VK_FORMAT_R16G16B16_SNORM: return 0x7FFF;
    case VK_FORMAT_R32G32B32_SFLOAT: return 0x3F800000;
    default: return 0xFF;
    }
}
void widen_rows(uint8_t* dst, const uint8_t* src, size_t texels, int from, int to, uint32_t alpha) {
    const int n = to - from;
    for (size_t i = 0; i < texels; i++) {
        std::memcpy(dst + i * to, src + i * from, from);
        std::memcpy(dst + i * to + from, &alpha, n);   // (little endian: the low n bytes)
    }
}

/// Byte ranges of a texture's storage, from its start (what the CPU wrote since its last upload)
using Ranges = std::vector<std::pair<uint64_t, uint64_t>>;
bool overlaps(const Ranges* dirty, uint64_t a, uint64_t b) {
    if (!dirty) return true;
    for (const auto& r : *dirty)
        if (r.first < b && a < r.second) return true;
    return false;
}

/// The view of one level the ASTC decoder writes: every layer, or one (layer); R8G8B8A8_UINT as astc_target_view's
VkImageView astc_level_view(Texture* tex, uint32_t level, uint32_t layer) {
    if (layer == ~0u) return astc_target_view(tex, level);
    const uint64_t key = mix(mix(0xA57D, level), layer);
    return cached_image_view(tex, key, [&](VkResult* result) {
        return make_view(tex, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_FORMAT_R8G8B8A8_UINT, VK_IMAGE_ASPECT_COLOR_BIT,
                         level, 1, layer, 1, {}, VK_IMAGE_USAGE_STORAGE_BIT, result);
    });
}

// (TEST, local, exp/rewrite-track) the render thread's time in the steps of making a texture, for the frame trace's last columns:
// 0 create(), 1 the reuse hash and copy, 2 the ASTC blocks staged, 3 the decode's recording (without 4 and 5), 4 the level views
// made, 5 the upload batch handed over mid-frame (replay_flush_upload), 6 how many such hand-overs, 7 how many create()
std::atomic<int64_t> step_ns[8];
bool step_enabled() { static const bool on = getenv("RX_VK_FRAME_TRACE") && atoi(getenv("RX_VK_FRAME_TRACE")); return on; }
int64_t step_now() { return step_enabled() ? std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() : 0; }
struct StepTimer {
    int i;
    int64_t t0 = step_now();
    explicit StepTimer(int k) : i(k) {}
    StepTimer(int k, int64_t start) : i(k), t0(start) {}
    ~StepTimer() { if (t0) step_ns[i].fetch_add(step_now() - t0, std::memory_order_relaxed); }
};
/// ASTC decoded on the GPU (Eden's decoder): the layers dirty overlaps (all of them without dirty), each layer's
/// levels from the first one the image keeps (a texture that leaves out its top levels stages only the tail of
/// every layer's chain: Eden's AssembleInto does the same)
/// (longshot) why the last upload_astc_gpu on this thread failed, for the log of a BC3 image left to be tried again
thread_local const char* astc_fail = nullptr;
/// (longshot) A texture the upload budget never puts off (upload_defer): a terrain material, a layer array or over 1024 x
/// 1024 both ways, which must be there in the frame that first draws it (the ground flickered when one waited a frame)
bool upload_never_put_off(const Texture& tex) {
    return tex.layers > 1 || (tex.width > 1024 && tex.height > 1024);
}

/// Decoded texels per GPU ASTC upload submission (RX_VK_ASTC_CHUNK_MPX, in millions of texels; 16 by default, 0: no split)
uint64_t astc_chunk_texels() {
    static const uint64_t v = []() -> uint64_t {
        constexpr uint64_t kDefault = 16ull * 1000000ull;
        const char* e = getenv("RX_VK_ASTC_CHUNK_MPX");
        if (!e || !*e) return kDefault;
        char* end = nullptr;
        const long long mpx = std::strtoll(e, &end, 10);
        if (end == e || *end != '\0' || mpx < 0) return kDefault;
        if (mpx == 0) return 0;
        const uint64_t u = (uint64_t)mpx;
        return u > UINT64_MAX / 1000000ull ? UINT64_MAX : u * 1000000ull;
    }();
    return v;
}

bool upload_astc_gpu(Texture& tex, uint64_t total, const Ranges* dirty) {
#ifdef TRACY_ENABLE
    HT_SCOPE("texture upload_astc_gpu");
#endif
    astc_fail = nullptr;
    if (tex.type != VK_IMAGE_TYPE_2D || tex.samples != VK_SAMPLE_COUNT_1_BIT || !astc_ready()) {
        astc_fail = "not a 2D image the GPU decoder takes";
        return false;
    }
    const uint64_t layer_size = tex.layers > 1 ? tex.L.layer_size : std::max<uint64_t>(tex.L.layer_size, total);
    const uint64_t tail0 = level_geo(tex, 0).offset;
    if (!layer_size || tail0 >= layer_size || (tail0 & 15) || (layer_size & 15)) {
        astc_fail = "its layout";
        return false;
    }
    const uint64_t tail = layer_size - tail0;
    std::vector<uint32_t> layers;
    for (uint32_t layer = 0; layer < tex.layers; layer++)
        if (overlaps(dirty, (uint64_t)layer * layer_size + tail0, (uint64_t)(layer + 1) * layer_size)) layers.push_back(layer);
    if (layers.empty()) return true;
    const uint64_t alignment = std::max<uint64_t>(256, dev.props.limits.minStorageBufferOffsetAlignment);
    if (tail > UINT32_MAX - alignment || layers.size() > UINT64_MAX / (tail + alignment)) {
        astc_fail = "layer stride too large";
        return false;
    }
    const uint64_t stride = (tail + alignment - 1) & ~(alignment - 1);
    const uint64_t staged = (layers.size() - 1) * stride + tail;
    const Staging s = staging(staged, alignment);
    if (!s.buffer) {
        astc_fail = "no staging memory for its ASTC blocks";
        return false;
    }
    std::vector<uint8_t> scratch;
    const int64_t stage_t0 = step_now();
    for (size_t k = 0; k < layers.size(); k++) {
        const uint64_t at = (uint64_t)layers[k] * layer_size + tail0;
        const uint64_t n = at < total ? std::min(tail, total - at) : 0;
        uint8_t* dest = s.data + k * stride;
        if (n) std::memcpy(dest, guest_bytes(tex.gpu + at, n, scratch), n);
        if (n < tail) std::memset(dest + n, 0, tail - n);
    }
    flush_mapped_range(s.allocation, s.offset, staged);   // (encoded blocks read by the GPU decoder, not necessarily coherent)
    const int64_t record_t0 = step_now();
    if (stage_t0) step_ns[2].fetch_add(record_t0 - stage_t0, std::memory_order_relaxed);
    int64_t record_out = 0;   // (the views' and the hand-overs' time, counted under their own steps)
    struct RecordDone {
        int64_t t0;
        int64_t& out;
        ~RecordDone() { if (t0) step_ns[3].fetch_add(step_now() - t0 - out, std::memory_order_relaxed); }
    } record_done{record_t0, record_out};
    st_bytes += (int64_t)staged;
    VkCommandBuffer cmd = upload_cmd();
    const uint64_t limit = dev.props.limits.maxStorageBufferRange;
    const uint64_t chunk_texels = astc_chunk_texels();
    // (RX_VK_PASS_TIMING=1: the GPU's decode, and its re-encode, one span a texture, named by whether the upload budget could
    // put the texture off. An area's entry decodes dozens in one frame, which the GPU time line doesn't show, #1720)
    static const char* const kSpans[2][2] = {
        {"astc decode (the upload budget can put it off)", "astc decode (never put off: a layer array, or over 1024x1024)"},
        {"astc decode + BC re-encode (the upload budget can put it off)",
         "astc decode + BC re-encode (never put off: a layer array, or over 1024x1024)"}};
    // (closed and opened again around an upload chunk's flush: a span across it would count the CPU's wait as GPU time)
    const char* const span_label = kSpans[tex.bc ? 1 : 0][upload_never_put_off(tex) ? 1 : 0];
    std::optional<GprofSpan> span;
    span.emplace(cmd, span_label);
    span->bytes = staged;
    if (tex.bc) {
        if (!bc_scratch_layout(cmd, tex.bc_resized)) {
            astc_fail = "no memory for the BC3 scratch";
            return false;
        }
    }
    // (RX_VK_BC_LEVELS=0: off) A single-layer BC3 texture's levels the small scratch holds: decoded one into each of
    // the scratch's layers with nothing between them, then encoded each to its own place in the output buffer, then copied
    // with one command: three full barriers per group of up to bc_small_layers() small levels and one big level,
    // also limited by the output buffer's capacity, where each level had three barriers
    struct GroupLevel { uint32_t level; VkImageView view; uint32_t w, h, off; };   // (off: its blocks' place in bc_out, in 8-byte units)
    std::vector<GroupLevel> group;
    uint32_t group_off = 0, group_small = 0;
    bool group_big = false;
    // (bc_out's size as bc_scratch_make made it, in the encoder's 8-byte units)
    const uint64_t group_cap = std::max<uint64_t>((uint64_t)(kBcDim / 4) * (kBcDim / 4) * bc_layers() * 16,
                                                  (uint64_t)(bc_small_px / 4) * (bc_small_px / 4) * bc_small_layers() * 16) / 8;
    const bool grouped = bc_levels_on() && tex.bc && !tex.bc_resized && !bc7_enabled() && bc_small && bc_small_layer[0] &&
                         tex.layers == 1 && layers.size() == 1;
    const auto group_flush = [&] {
        if (group.empty()) return;
        full_barrier(cmd);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bc_pipe);
        std::vector<VkBufferImageCopy> copies;
        for (const GroupLevel& q : group) {
            VkDescriptorImageInfo qi{VK_NULL_HANDLE, q.view, VK_IMAGE_LAYOUT_GENERAL};
            VkDescriptorBufferInfo qb{bc_out, 0, VK_WHOLE_SIZE};
            VkWriteDescriptorSet wr[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
            wr[0].dstBinding = 0, wr[0].descriptorCount = 1, wr[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            wr[0].pImageInfo = &qi;
            wr[1].dstBinding = 1, wr[1].descriptorCount = 1, wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            wr[1].pBufferInfo = &qb;
            vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bc_layout, 0, 2, wr);
            const BcPush qp{(q.w + 3) / 4, (q.h + 3) / 4, q.w, q.h, 1, q.off};
            vkCmdPushConstants(cmd, bc_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof qp, &qp);
            vkCmdDispatch(cmd, (qp.blocks_x + 7) / 8, (qp.blocks_y + 7) / 8, 1);
            // (the buffer's rows given in whole blocks, as the encoder wrote them and as bc_encode_into's copy gives them, #2015:
            // left at 0 a level not a multiple of 4 leaves the pitch to the driver's rounding)
            copies.push_back(VkBufferImageCopy{(VkDeviceSize)q.off * 8, qp.blocks_x * 4, qp.blocks_y * 4,
                                               {VK_IMAGE_ASPECT_COLOR_BIT, q.level, 0, 1}, {0, 0, 0}, {q.w, q.h, 1}});
        }
        descriptors_disturbed();
        full_barrier(cmd);
        vkCmdCopyBufferToImage(cmd, bc_out, tex.image, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)copies.size(), copies.data());
        full_barrier(cmd);   // (the scratch and the buffer are the next group's)
        group.clear(), group_off = 0, group_small = 0, group_big = false;
    };
    // A failed upload may already have recorded decoder writes to the shared scratch.
    // Order those writes before any subsequent upload can reuse it.
    struct GroupFailureBarrier {
        VkCommandBuffer& cmd;
        decltype(group)& pending;
        ~GroupFailureBarrier() { if (!pending.empty()) { last_barrier_cmd = VK_NULL_HANDLE; full_barrier(cmd); } }
    } group_failure_barrier{cmd, group};
    for (uint32_t level = 0; level < tex.levels; ++level) {
        const LevelGeo g = level_geo(tex, level);
        const uint64_t off = g.offset - tail0;
        if (off >= tail || !g.swizzled) {
            astc_fail = "invalid mip range";
            return false;
        }
        const uint64_t bytes = std::min(g.swizzled, tail - off);
        if (bytes > limit) {
            astc_fail = "mip exceeds storage buffer limit";
            return false;
        }
        const uint64_t max_layers = std::min<uint64_t>(dev.props.limits.maxComputeWorkGroupCount[2],
                                                      1 + (limit - bytes) / stride);
        // (a level over the chunk bound takes one layer a dispatch, so a flush can fall between its layers: astc_level_view
        // views one layer, and the cost is a few more dispatches on every GPU, never a longer submission)
        const bool all = !tex.bc && layers.size() == tex.layers && layers.size() <= max_layers &&
                         (!chunk_texels || (uint64_t)g.w * g.h * tex.layers <= chunk_texels);
        const bool repack = off % dev.props.limits.minStorageBufferOffsetAlignment != 0 || bytes < g.swizzled;
        // (a level the small scratch holds: bc_small_layers() layers a run through it; a resized one has its own scratch. A
        // small-VRAM card's 256 x 256 one holds a group's levels only: its layer arrays go as they did, through the first)
        const bool small = tex.bc && bc_small && !tex.bc_resized && g.w <= bc_small_px && g.h <= bc_small_px &&
                           (bc_small_px == kBcSmallDim || grouped);
        // (and no longer than the upload chunk holds: RX_VK_ASTC_CHUNK_MPX set low keeps a run inside one submission)
        const uint64_t level_texels = std::max<uint64_t>(1, (uint64_t)g.w * g.h);
        const uint32_t bc_run = (uint32_t)std::min<uint64_t>(small ? bc_small_layers() : bc_layers(),
                                                             chunk_texels ? std::max<uint64_t>(1, chunk_texels / level_texels) : ~0ull);
        AstcPush pc{};
        pc.block_dims[0] = tex.block_w;
        pc.block_dims[1] = tex.block_h;
        pc.layer_stride = (uint32_t)stride;
        pc.blocks_x = g.tw;
        pc.blocks_y = g.th;
        pc.x_shift = 9 + g.tile_h + g.tile_d;
        pc.block_size = ((g.tw * 16 + 63) >> 6) << pc.x_shift;
        pc.block_height = g.tile_h;
        pc.block_height_mask = (1u << g.tile_h) - 1;
        pc.native_tiling = 1;
        pc.srgb = astc_nvn_srgb(tex.nvn_format) ? 1u : 0u;
        for (size_t k0 = 0; k0 < layers.size();) {
            const uint32_t run = tex.bc ? bc_run : all ? tex.layers : 1;
            size_t k1 = k0 + 1;
            while (!repack && k1 < layers.size() && k1 - k0 < run && k1 - k0 < max_layers &&
                   layers[k1] == layers[k1 - 1] + 1) ++k1;
            const uint32_t n = (uint32_t)(k1 - k0);
            const uint64_t from = k0 * stride + off;
            VkDescriptorBufferInfo bi{s.buffer, s.offset + from, (n - 1) * stride + bytes};
            if (repack) {
                const Staging mip = staging(bytes, alignment);
                if (!mip.buffer) {
                    astc_fail = "no aligned staging for mip";
                    return false;
                }
                std::memcpy(mip.data, s.data + from, bytes);
                flush_mapped_range(mip.allocation, mip.offset, bytes);
                bi = {mip.buffer, mip.offset, bytes};
            }
            // (a group's level: into a scratch layer of its own; a level over the small scratch's size into the
            // big scratch, one a group. Not past what bc_out holds, nor a second big level: the group so far goes first)
            const bool in_group = grouped && (small || (g.w <= kBcDim && g.h <= kBcDim));
            if (in_group) {
                const uint64_t need = (uint64_t)((std::max(1u, tex.width >> level) + 3) / 4) * ((std::max(1u, tex.height >> level) + 3) / 4) * 2;
                if ((!small && group_big) || (!group.empty() && group_off + need > group_cap)) group_flush();
            }
            const int64_t view_t0 = step_now();
            // (a BC texture's scratch: the decoder writes it through its UINT view, `target`; the encoder reads its UNORM one, `view`)
            VkImageView view = in_group ? (small ? bc_small_layer[group_small] : bc_scratch_view)
                               : tex.bc ? (small ? bc_small_view : bc_scratch_view)
                               : astc_level_view(&tex, level, n == tex.layers ? ~0u : layers[k0]);
            VkImageView target = in_group ? (small ? bc_small_layer_u[group_small] : bc_scratch_uview)
                                 : tex.bc ? (small ? bc_small_uview : bc_scratch_uview) : view;
            if (view_t0) {
                const int64_t d = step_now() - view_t0;
                step_ns[4].fetch_add(d, std::memory_order_relaxed), record_out += d;
            }
            if (!view || !target) {
                astc_fail = "no view of a level";
                return false;
            }
            // Each BC3 slice run also emits 3 full barriers; a 64K-texel floor prevents hundreds of tiny tail mips
            // from packing 1,500+ dispatches into a single trailing chunk.
            const uint64_t step_texels = std::max<uint64_t>((uint64_t)g.w * g.h * n, 65536ull);
            const bool chunk_full = step_texels > chunk_texels || up_astc_texels > chunk_texels - step_texels;
            if (chunk_texels && up_astc_texels && chunk_full) {
                group_flush();   // (the scratch's layers are read before the batch goes)
                span.reset();
                const int64_t flush_t0 = step_now();
                if (replay_flush_upload(cmd)) {
                    last_barrier_cmd = VK_NULL_HANDLE;   // (cmd was reset and re-begun in place with the same handle)
                    full_barrier(cmd);
                }
                if (flush_t0) {
                    const int64_t d = step_now() - flush_t0;
                    step_ns[5].fetch_add(d, std::memory_order_relaxed), step_ns[6].fetch_add(1, std::memory_order_relaxed), record_out += d;
                }
                span.emplace(cmd, span_label);
                up_astc_texels = 0;
            }
            if (in_group) {   // (the hand-over above emptied the group)
                view = small ? bc_small_layer[group_small] : bc_scratch_view;
                target = small ? bc_small_layer_u[group_small] : bc_scratch_uview;
            }
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, astc_pipe);
            VkDescriptorImageInfo ii{VK_NULL_HANDLE, target, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
            w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[0].pBufferInfo = &bi;
            w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[1].pImageInfo = &ii;
            vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, astc_layout, 0, 2, w);
            vkCmdPushConstants(cmd, astc_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
            vkCmdDispatch(cmd, (g.tw + 7) / 8, (g.th + 7) / 8, n);
            if (in_group) {
                const uint32_t iw = std::max(1u, tex.width >> level), ih = std::max(1u, tex.height >> level);
                group.push_back({level, view, iw, ih, group_off});
                group_off += ((iw + 3) / 4) * ((ih + 3) / 4) * 2;
                if (small) group_small++;
                else group_big = true;
                if (group_small >= bc_small_layers()) group_flush();
            } else if (tex.bc) {
                const uint32_t iw = std::max(1u, tex.width >> level), ih = std::max(1u, tex.height >> level);
                if (tex.bc_resized) bc_encode_resized(cmd, g.w, g.h, iw, ih, n, tex, level, layers[k0]);
                else bc_encode_into(cmd, iw, ih, n, tex, level, layers[k0], 0, 0, small ? bc_small_view : VK_NULL_HANDLE);
            }
            up_astc_texels += step_texels;
            k0 = k1;
        }
    }
    group_flush();   // (the last group's levels)
    st_astc++;
    return true;
}

// ---- big block-linear textures unswizzled on the GPU (Eden's block_linear_unswizzle_2d.comp, GPL, with the Switch
// 2 tiling): the shader reads the storage where it is, in its imported pool, and writes the image. The course maps
// the title loads at each scene change (six of 349 MB) were copied into staging and unswizzled on the CPU: ~200 ms
// of the replay thread. ----
struct UnswizzlePush {
    uint32_t origin[3], pad0;
    int32_t destination[3];
    uint32_t bytes_per_block_log2, layer_stride, block_size, x_shift, block_height, block_height_mask, native_tiling;
};
VkDescriptorSetLayout unsw_set = VK_NULL_HANDLE;
VkPipelineLayout unsw_layout = VK_NULL_HANDLE;
VkPipeline unsw_pipe = VK_NULL_HANDLE;
bool unswizzle_ready() {
    static int state = 0;
    if (state) return state > 0;
    state = -1;
    if (!dev.push_descriptor || !vkCmdPushDescriptorSetKHR) return false;
    const VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 2;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &unsw_set) != VK_SUCCESS) return false;
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(UnswizzlePush)};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &unsw_set;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &unsw_layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    const auto spv = BUILTIN_SPV(BLOCK_LINEAR_UNSWIZZLE_2D_COMP_SPV);
    mi.codeSize = spv.size;
    mi.pCode = spv.code;
    VkShaderModule m = VK_NULL_HANDLE;
    {
        IntelDriverGate gate;   // (the module's only: texture_compute_pipeline takes the same lock)
        if (extern_guarded_shader_module(&mi, &m) != VK_SUCCESS) return false;
    }
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = unsw_layout;
    const VkResult r = texture_compute_pipeline(ci, unsw_pipe);
    vkDestroyShaderModule(dev.device, m, nullptr);
    if (r != VK_SUCCESS) return false;
    state = 1;
    logf("vk: big block-linear textures are unswizzled on the GPU from their pools");
    return true;
}
}  // namespace
/// (longshot) The texture pipelines made before the game (precompile_run, vk_draw.cpp), not inside the frame of the first texture
/// that needs one (texture_compute_pipeline above). Each is made once whoever asks first; before the game nothing else asks
void texture_pipelines_warm() {
    const auto t0 = std::chrono::steady_clock::now();
    const bool astc = astc_ready(), bc = bc_ready(), unswizzle = unswizzle_ready();
    logf("vk: texture pipelines made before the game in %.0f ms (ASTC decoder %s, BC encoder %s, GPU unswizzle %s)",
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), astc ? "yes" : "no",
         bc ? "yes" : "no", unswizzle ? "yes" : "no");
}
namespace {

/// The whole texture (every level and layer dirty overlaps) unswizzled on the GPU; false: not a texture for it
bool upload_unswizzle_gpu(Texture& tex, uint64_t total, const Ranges* dirty) {
#ifdef TRACY_ENABLE
    HT_SCOPE("texture upload_unswizzle_gpu");
#endif
    if (tex.type != VK_IMAGE_TYPE_2D || tex.astc || tex.bc || !(tex.usage & VK_IMAGE_USAGE_STORAGE_BIT) ||
        is_bc(tex.format) || is_astc(tex.format) || is_depth_format(tex.format) || nvn_tex_is_linear(&tex.d))
        return false;
    if (total < (8u << 20) || tex.block_w != 1 || tex.block_h != 1) return false;
    int widen_from = 0;
    widened(tex.d.format ? vk_format(tex.nvn_format).format : tex.format, &widen_from);
    if (widen_from) return false;
    VkFormat vf;
    switch (tex.bpb) {
    case 1: vf = VK_FORMAT_R8_UINT; break;
    case 2: vf = VK_FORMAT_R16_UINT; break;
    case 4: vf = VK_FORMAT_R32_UINT; break;
    case 8: vf = VK_FORMAT_R32G32_UINT; break;
    case 16: vf = VK_FORMAT_R32G32B32A32_UINT; break;
    default: return false;
    }
    if (!unswizzle_ready()) return false;
    // (arm64 port) a texture in a pool left in ordinary memory until the GPU binds it (RX_VK_PLACED_LATER_MB): on the CPU, as
    // most are. The GPU would read it through a binding, which places the whole pool: on the Odin 3, the textures' 2.4 GB
    if (memory_placed_later(tex.gpu, total)) return false;
    const BufferRef src = gpu_buffer(tex.gpu, total);
    const uint64_t align = std::max<uint64_t>(dev.props.limits.minStorageBufferOffsetAlignment, 16);
    if (!src || (src.offset % align)) return false;
    for (uint32_t level = 0; level < tex.levels; level++) {   // (every level must be possible before any is done)
        const LevelGeo g = level_geo(tex, level);
        if (g.td != 1 || g.tile_d || (g.offset % align) || g.offset >= total) return false;
        // (the shader reads whole GOB-block rows: a level whose last one runs past the storage, which the CPU path
        // reads from a zero-padded copy, would read past the pool and fault the device)
        if (!tex.layers || !g.swizzled || g.swizzled > total - g.offset ||
            (tex.layers > 1 && tex.L.layer_size > (total - g.offset - g.swizzled) / (tex.layers - 1))) return false;
        const uint64_t range = (tex.layers - 1ull) * tex.L.layer_size + g.swizzled;
        if (range > dev.props.limits.maxStorageBufferRange || range > src.size || g.offset > src.size - range ||
            tex.layers > dev.props.limits.maxComputeWorkGroupCount[2]) return false;
    }
    VkCommandBuffer cmd = upload_cmd();
    full_barrier(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, unsw_pipe);
    for (uint32_t level = 0; level < tex.levels; level++) {
        const LevelGeo g = level_geo(tex, level);
        bool any = false;
        for (uint32_t layer = 0; layer < tex.layers && !any; layer++)
            any = overlaps(dirty, g.offset + (uint64_t)layer * tex.L.layer_size,
                           g.offset + (uint64_t)layer * tex.L.layer_size + g.swizzled);
        if (!any) continue;
        const uint64_t key = mix(0x5B1Cu, mix(level, (uint64_t)vf));
        const VkImageView view = cached_image_view(&tex, key, [&](VkResult* result) {
            return make_view(&tex, VK_IMAGE_VIEW_TYPE_2D_ARRAY, vf, VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0,
                             tex.layers, {}, VK_IMAGE_USAGE_STORAGE_BIT, result);
        });
        if (!view) return false;
        UnswizzlePush pc{};
        pc.bytes_per_block_log2 = (uint32_t)std::countr_zero((uint32_t)tex.bpb);
        pc.layer_stride = (uint32_t)tex.L.layer_size;
        const uint32_t gobs_x = (uint32_t)(((uint64_t)g.tw * tex.bpb + 63) >> 6);
        pc.x_shift = 9 + g.tile_h;
        pc.block_size = gobs_x << pc.x_shift;
        pc.block_height = g.tile_h;
        pc.block_height_mask = (1u << g.tile_h) - 1;
        pc.native_tiling = 1;
        VkDescriptorBufferInfo bi{src.buffer, src.offset + g.offset, (tex.layers - 1ull) * tex.L.layer_size + g.swizzled};
        VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[0].pBufferInfo = &bi;
        w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[1].pImageInfo = &ii;
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, unsw_layout, 0, 2, w);
        vkCmdPushConstants(cmd, unsw_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        checkpoint_mark(cmd, tex.gpu, 'g');
        vkCmdDispatch(cmd, (g.tw + 31) / 32, (g.th + 31) / 32, tex.layers);
        st_bytes += (int64_t)g.swizzled * tex.layers;
    }
    full_barrier(cmd);
    descriptors_disturbed();
    return true;
}

/// The texture from guest memory into its image: all of it, or (dirty) the layers, levels and block rows the CPU
/// wrote. Block-linear levels are rows of blocks a GOB-block high, each self-contained: a written page re-uploads
/// the block rows it falls in.
uint64_t st_made;          // (textures made)
// (longshot) and by kind, with their bytes (texture_made_kinds): what a frame's new textures are, the work each brings
// (a colour or depth target cleared whole, an ASTC texture decoded, any other uploaded)
uint64_t st_made_kind[4], st_made_kind_bytes[4];   // (0 colour target, 1 depth, 2 ASTC or BC, 3 other)
int64_t st_unswizzle_bytes, st_unswizzle_ns;   // (the CPU's unswizzling)
std::map<uint64_t, std::pair<uint64_t, uint64_t>> gpu_owned;   // GPU address -> (end, memory generation then)
thread_local uint64_t tl_watch_prog;
/// (RX_VK_VERIFY_DIM=WxH: the guest memory of textures that size, hashed when uploaded and every 300 frames: a change
/// the write tracking did not report is logged as a missed write)
std::pair<uint32_t, uint32_t> verify_dim() {
    static const std::pair<uint32_t, uint32_t> d = [] {
        unsigned w = 0, h = 0;
        if (const char* e = getenv("RX_VK_VERIFY_DIM")) sscanf(e, "%ux%u", &w, &h);
        return std::pair<uint32_t, uint32_t>{w, h};
    }();
    return d;
}
std::unordered_map<uint32_t, std::vector<uint8_t>> verify_snap;   // (each verified texture's memory when uploaded)
uint64_t guest_hash(const Texture& tex, bool snapshot = false) {
    std::vector<uint8_t> b(tex.guest_size);
    if (!host->gpu_read(tex.gpu, b.data(), b.size())) return 0;
    uint64_t h = 1469598103934665603ull;
    for (uint8_t c : b) h = (h ^ c) * 1099511628211ull;
    if (snapshot) verify_snap[tex.id] = std::move(b);
    return h;
}
/// (a missed write: which 4 KB pages changed since the upload, and the CPU's next writes to them watched)
void verify_report(const Texture& tex) {
    const auto it = verify_snap.find(tex.id);
    if (it == verify_snap.end()) return;
    std::vector<uint8_t> now(tex.guest_size);
    if (!host->gpu_read(tex.gpu, now.data(), now.size())) return;
    std::string runs;
    int nruns = 0, watched = 0;
    uint64_t changed = 0;
    for (uint64_t off = 0; off < tex.guest_size; off += 4096) {
        const uint64_t n = std::min<uint64_t>(4096, tex.guest_size - off);
        if (!std::memcmp(now.data() + off, it->second.data() + off, n)) continue;
        changed++;
        if (nruns < 12) {
            char b[48];
            snprintf(b, sizeof b, " +%#llx", (unsigned long long)off);
            runs += b, nruns++;
        }
        static int watches;
        if (host->watch_guest_writes && watches < 4 && watched < 2) {
            const uint64_t g = host->gpu_guest(tex.gpu + off);
            if (g) host->watch_guest_writes(g, 4096), watches++, watched++;
        }
    }
    logf("vk: verify: texture %#llx: %llu pages changed since the upload:%s%s", (unsigned long long)tex.gpu,
         (unsigned long long)changed, runs.c_str(), watched ? " (the next CPU writes to two of them watched)" : "");
    {   // (the first changed page as the write tracking has it: ever written, written since the upload, and when)
        for (uint64_t off = 0; off < tex.guest_size; off += 4096) {
            const uint64_t n = std::min<uint64_t>(4096, tex.guest_size - off);
            if (!std::memcmp(now.data() + off, it->second.data() + off, n)) continue;
            const uint64_t g = host->gpu_guest(tex.gpu + off);
            std::vector<std::pair<uint64_t, uint64_t>> ever, since;
            memory_written_runs(g, 4096, 0, ever);
            memory_written_runs(g, 4096, tex.upload_gen, since);
            logf("vk: verify: its page +%#llx (guest %#llx): stamped ever %d, since the upload (gen %llu) %d; generation now %llu",
                 (unsigned long long)off, (unsigned long long)g, (int)!ever.empty(), (unsigned long long)tex.upload_gen,
                 (int)!since.empty(), (unsigned long long)memory_generation());
            break;
        }
    }
    it->second = std::move(now);
}   // (the draw or dispatch the replay is at, for watch_tex)
/// (RX_VK_WATCH_TEX=addr[,addr...]: what happens to the textures over those GPU addresses, 600 lines)
void watch_tex(const Texture* tex, const char* what) {
    static std::vector<uint64_t> addrs = [] {
        std::vector<uint64_t> v;
        if (const char* e = getenv("RX_VK_WATCH_TEX"))
            for (const char* p = e; *p;) {
                char* end;
                const uint64_t a = strtoull(p, &end, 16);
                if (end == p) break;
                v.push_back(a);
                p = *end ? end + 1 : end;
            }
        return v;
    }();
    static const std::pair<uint32_t, uint32_t> dim = [] {
        unsigned w = 0, h = 0;
        if (const char* e = getenv("RX_VK_WATCH_DIM")) sscanf(e, "%ux%u", &w, &h);
        return std::pair<uint32_t, uint32_t>{w, h};
    }();
    static const std::pair<uint64_t, uint64_t> range = [] {   // (RX_VK_WATCH_RANGE=<lo>:<hi>: textures overlapping it)
        unsigned long long a = 0, b = 0;
        if (const char* e = getenv("RX_VK_WATCH_RANGE")) sscanf(e, "%llx:%llx", &a, &b);
        return std::pair<uint64_t, uint64_t>{a, b};
    }();
    if (tex && range.second && tex->gpu < range.second && tex->gpu + std::max<uint64_t>(tex->guest_size, 1) > range.first) {
        static int rlogged;
        if (rlogged++ < 600)
            logf("vk: watch range frame %llu prog %016llx %s #%u %#llx %ux%ux%u x%u levels %u NVN %#x target %d size %llu%s%s",
                 (unsigned long long)frame_no, (unsigned long long)tl_watch_prog, what, tex->id, (unsigned long long)tex->gpu,
                 tex->width, tex->height, tex->depth, tex->layers, tex->levels, tex->nvn_format, tex->d.target,
                 (unsigned long long)tex->guest_size, tex->gpu_written ? " rendered" : "", tex->uploaded ? " uploaded" : "");
    }
    if ((addrs.empty() && !dim.first) || !tex) return;
    {   // (RX_VK_DUMP_ON_WATCH=<frames>: a watched texture made after boot dumps the frames after it, once)
        static const int on_watch = getenv("RX_VK_DUMP_ON_WATCH") ? atoi(getenv("RX_VK_DUMP_ON_WATCH")) : 0;
        static bool requested = false;
        if (on_watch && !requested && frame_no > 1000 && !strcmp(what, "made")) {
            for (const uint64_t a : addrs)
                if (a == tex->gpu) {
                    requested = true;
                    dump_request((int64_t)frame_no + 1, on_watch);
                    logf("vk: watch: texture %#llx made at frame %llu: the next %d frames dumped", (unsigned long long)a,
                         (unsigned long long)frame_no, on_watch);
                }
        }
    }
    static int logged;
    static std::unordered_map<uint32_t, uint64_t> told;   // (a texture's events once a frame)
    const bool by_dim = dim.first && tex->width == dim.first && tex->height == dim.second;
    if (by_dim && !strcmp(what, "written")) {   // ("written" once a frame; the rest each time)
        uint64_t& f = told[tex->id];
        if (f == frame_no || logged >= 20000) return;
        f = frame_no;
    }
    const uint64_t hi = tex->gpu + std::max<uint64_t>(tex->guest_size, 1);
    for (const uint64_t a : by_dim ? std::vector<uint64_t>{tex->gpu} : addrs) {
        if (a < tex->gpu || a >= hi || logged >= (by_dim ? 20000 : 600)) continue;
        logged++;
        logf("vk: watch tex frame %llu prog %016llx %s #%u %#llx %ux%ux%u x%u levels %u NVN %#x layout %#x class %#x target %d "
             "size %llu%s%s", (unsigned long long)frame_no, (unsigned long long)tl_watch_prog, what, tex->id, (unsigned long long)tex->gpu, tex->width,
             tex->height, tex->depth, tex->layers, tex->levels, tex->nvn_format, tex->key_layout, tex->key_cls,
             tex->d.target, (unsigned long long)tex->guest_size, tex->gpu_written ? " rendered" : "",
             tex->uploaded ? " uploaded" : "");
        return;
    }
}
/// Whether [gpu, gpu + n) is storage the GPU's images rendered last (covered by ranges render targets took), and the
/// generation from which the CPU's writes would count (0: not the GPU's)
/// (longshot) The ranges can overlap (texture_rendered drops only the ones inside a new range), so the bytes they cover are
/// counted once each: summed, two overlapping ranges counted their overlap twice and a range with a gap the GPU never drew
/// read as covered, and a texture over it was never uploaded
uint64_t gpu_owned_since(uint64_t gpu, uint64_t n) {
    uint64_t covered = 0, reach = gpu;   // (reach: the end of what is counted so far; the ranges come in address order)
    uint64_t since = UINT64_MAX;
    auto it = gpu_owned.upper_bound(gpu);
    if (it != gpu_owned.begin()) --it;
    for (; it != gpu_owned.end() && it->first < gpu + n; ++it) {
        const uint64_t a = std::max(it->first, gpu), b = std::min(it->second.first, gpu + n);
        if (b <= a) continue;
        since = std::min<uint64_t>(since, it->second.second);
        if (b > std::max(a, reach)) covered += b - std::max(a, reach), reach = b;
    }
    return covered >= n && since != UINT64_MAX ? std::max<uint64_t>(since, 1) : 0;
}

// (longshot, smoothness) What a frame's uploads cost by the way they went: when a frame's uploads take over 50 ms (the stutters at
// area loads and in walking: 180-360 ms frames of 250-450 MB), one log line of the routes
// (ASTC on the GPU, unswizzled on the GPU, on the CPU), their count, MB and ms, and the slowest textures
enum UpRoute { UP_ASTC, UP_GPU, UP_CPU, UP_COUNT };
thread_local int up_route = UP_CPU;
struct UpStat { int64_t n = 0, bytes = 0, ns = 0; };
UpStat up_frame[UP_COUNT];
struct UpSlow { int64_t ns; uint32_t w, h, layers, nvn; int route; uint64_t bytes; };
std::vector<UpSlow> up_slowest;
std::mutex up_mu;   // (uploads may come from a background thread)
std::atomic<int64_t> frame_upload_ns{0};   // (this frame's time in upload(), every thread's)
std::atomic<int64_t> st_deferred{0}, st_deferred_frame{0};
void upload_frame_report() {
    std::lock_guard lk(up_mu);
    int64_t ns = 0;
    for (const UpStat& u : up_frame) ns += u.ns;
    static int logged;
    if (ns >= 50000000 && logged < 300) {
        logged++;
        static const char* names[UP_COUNT] = {"ASTC on the GPU", "unswizzled on the GPU", "on the CPU"};
        char line[900];
        int k = snprintf(line, sizeof line, "vk: a frame's uploads took %.0f ms:", ns / 1e6);
        for (int r = 0; r < UP_COUNT; r++)
            if (up_frame[r].n)
                k += snprintf(line + k, sizeof line - k, " %s %lld (%.1f MB, %.0f ms);", names[r], (long long)up_frame[r].n,
                              up_frame[r].bytes / 1048576.0, up_frame[r].ns / 1e6);
        std::sort(up_slowest.begin(), up_slowest.end(), [](const UpSlow& a, const UpSlow& b) { return a.ns > b.ns; });
        for (size_t i = 0; i < up_slowest.size() && i < 3 && k < (int)sizeof line - 80; i++)
            k += snprintf(line + k, sizeof line - k, " [%ux%u x%u NVN %#x %s %.1f MB %.0f ms]", up_slowest[i].w, up_slowest[i].h,
                          up_slowest[i].layers, up_slowest[i].nvn, names[up_slowest[i].route], up_slowest[i].bytes / 1048576.0,
                          up_slowest[i].ns / 1e6);
        logf("%s", line);
    }
    for (UpStat& u : up_frame) u = {};
    up_slowest.clear();
}
bool upload_inner(Texture& tex, const Ranges* dirty);
// (longshot, smoothness: textures prepared ahead, further down) a first upload a draw asked for is remembered
thread_local bool ahead_running = false;
void ahead_learn(const Texture& tex);
bool upload(Texture& tex, const Ranges* dirty) {
#ifdef TRACY_ENABLE
    HT_SCOPE("texture upload");
#endif
    if (!tex.uploaded && !ahead_running) ahead_learn(tex);
    const auto t0 = std::chrono::steady_clock::now();
    up_route = UP_CPU;
    const bool ok = upload_inner(tex, dirty);
    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    frame_upload_ns.fetch_add(ns, std::memory_order_relaxed);
    std::lock_guard lk(up_mu);
    UpStat& u = up_frame[up_route];
    u.n++, u.bytes += (int64_t)tex.guest_size, u.ns += ns;
    if (ns > 2000000) {
        up_slowest.push_back({ns, tex.width, tex.height, tex.layers, tex.nvn_format, up_route, tex.guest_size});
        if (up_slowest.size() > 64) {
            std::sort(up_slowest.begin(), up_slowest.end(), [](const UpSlow& a, const UpSlow& b) { return a.ns > b.ns; });
            up_slowest.resize(8);
        }
    }
    return ok;
}
// ---- (longshot) decoded ASTC used again (astc_reuse.h): a new texture whose blocks and image are those of one the game
// finalized takes that one's image by a GPU copy, and the old one goes at the next texture_frame ----
std::mutex reuse_mu;
astc_reuse::Freed<Texture> freed;   // (reuse_mu)
std::vector<Texture*> reuse_gone;   // (reuse_mu: the images that served one, for texture_frame)
std::atomic<int64_t> st_reused{0}, st_reused_bytes{0}, st_reuse_retired{0}, st_reuse_hash_ns{0}, st_reuse_hash_bytes{0};
astc_reuse::Shape reuse_shape(const Texture& t) {
    return {(uint32_t)t.format, t.width, t.height, t.depth, t.levels, t.layers, t.skip, (uint32_t)t.nvn_format, t.key_layout,
            t.bc, t.bc_resized};
}
/// (used again by its own address, or going: no longer one to copy from)
void reuse_drop(Texture& t) {
    if (!t.freed_listed && !t.reuse_retire) return;
    std::lock_guard lk(reuse_mu);
    if (t.freed_listed) freed.unlist(t.blocks_hash, &t), t.freed_listed = false;
    t.reuse_retire = false;
}
/// The game finalized `t`: its decoded ASTC from memory, of the same shape, listed to serve the same blocks made again elsewhere
void reuse_released(const nvn_tex* t) {
    auto [a, b] = by_gpu.equal_range(t->gpu);
    for (auto it = a; it != b; ++it) {
        Texture* x = it->second;
        if (!x->astc || !x->blocks_hash || !x->uploaded || x->gpu_written || x->freed_listed || x->reuse_retire) continue;
        if (x->d.format != t->d.format || x->d.width != t->d.width || x->d.height != t->d.height ||
            x->d.depth != t->d.depth || x->d.levels != t->d.levels || x->d.target != t->d.target)
            continue;
        std::lock_guard lk(reuse_mu);
        freed.list(x->blocks_hash, x);
        x->freed_listed = true;
    }
}
// (RX_VK_HASH_MEMO) The hash remembered by the storage and the shape, with the memory generation it was taken at: the same
// texture made again over storage no page of which was written since takes it without reading the storage again (a turn in
// Kokiri hashed ~1.8 ms of it a frame, under the helpers' lock). The generation is taken before the hash, so a write during
// it makes the memo older than the bytes, never newer. RX_VK_HASH_MEMO=0: off
struct HashMemo { uint64_t hash, gen; };
std::mutex hash_memo_mu;
std::unordered_map<uint64_t, HashMemo> hash_memo;   // (hash_memo_mu)
std::atomic<int64_t> hash_memo_hits{0}, hash_memo_hit_bytes{0}, hash_memo_misses{0};
uint64_t blocks_hash_memoized(const Texture& tex, uint64_t total) {   // (the storage the CPU route reads, as submitted)
    static const bool memo_on = [] { const char* e = getenv("RX_VK_HASH_MEMO"); return !(e && env_value_off(e)); }();
    const uint64_t guest = memo_on && tex.gpu < (1ull << 40) ? tex.gpu : 0;
    uint64_t key = 0, gen0 = 0;
    if (guest) {
        key = guest * 0x9E3779B97F4A7C15ull ^ total * 0xC2B2AE3D27D4EB4Full ^ ((uint64_t)tex.nvn_format << 48) ^
              ((uint64_t)tex.width << 32) ^ ((uint64_t)tex.height << 16) ^ ((uint64_t)tex.layers << 8) ^ tex.levels;
        gen0 = memory_generation();
        std::lock_guard lk(hash_memo_mu);
        auto it = hash_memo.find(key);
        if (it != hash_memo.end() && !memory_written_since(guest, total, it->second.gen)) {
            hash_memo_hits.fetch_add(1, std::memory_order_relaxed);
            hash_memo_hit_bytes.fetch_add((int64_t)total, std::memory_order_relaxed);
            return it->second.hash;
        }
    }
    static thread_local std::vector<uint8_t> scratch;
    astc_reuse::Hasher h;
    for (uint64_t at = 0; at < total; at += 64ull << 20) {
        const uint64_t n = std::min<uint64_t>(64ull << 20, total - at);
        h.add(guest_bytes(tex.gpu + at, n, scratch), n);
    }
    const uint64_t v = h.h | 1;
    if (guest) {
        hash_memo_misses.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lk(hash_memo_mu);
        if (hash_memo.size() > 200000) hash_memo.clear();   // (a bound)
        hash_memo[key] = {v, gen0};
    }
    return v;
}
/// A new decoded ASTC texture's blocks hashed (its first upload, whole), and the image of a finalized one with the same blocks
/// and shape copied into it: true when it was. One uploaded again keeps no hash, and is not listed when finalized
bool reuse_upload(Texture& tex, uint64_t total) {
    const auto t0 = std::chrono::steady_clock::now();
    const StepTimer step_timed(1, step_enabled() ? std::chrono::duration_cast<std::chrono::nanoseconds>(t0.time_since_epoch()).count() : 0);
    tex.blocks_hash = blocks_hash_memoized(tex, total);   // (the same bytes, the same Hasher, | 1: from the memo when it holds)
    st_reuse_hash_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    st_reuse_hash_bytes += (int64_t)total;
    if (tex.sparse || tex.type != VK_IMAGE_TYPE_2D || !tex.image) return false;
    Texture* from;
    {
        std::lock_guard lk(reuse_mu);
        if (!freed.size()) return false;
        from = freed.find(tex.blocks_hash, reuse_shape(tex), [](const Texture* t) { return reuse_shape(*t); });
        if (!from || from == &tex || !from->image || !from->uploaded || from->gpu_written) return false;
        freed.unlist(from->blocks_hash, from);
        from->freed_listed = false, from->reuse_retire = true;
        reuse_gone.push_back(from);
    }
    VkCommandBuffer cmd = upload_cmd();
    full_barrier(cmd);
    std::vector<VkImageCopy> regions(tex.levels);
    for (uint32_t l = 0; l < tex.levels; l++) {
        VkImageCopy& r = regions[l];
        r.srcSubresource = r.dstSubresource = {tex.aspect, l, 0, tex.layers};
        r.extent = {std::max(1u, tex.width >> l), std::max(1u, tex.height >> l), 1};
    }
    vkCmdCopyImage(cmd, from->image, VK_IMAGE_LAYOUT_GENERAL, tex.image, VK_IMAGE_LAYOUT_GENERAL, tex.levels, regions.data());
    full_barrier(cmd);
    from->last_value = std::max(from->last_value, rec_value);   // (its image goes once this submission is done with it)
    const int64_t n = ++st_reused;
    st_reused_bytes += (int64_t)tex.bytes;
    if (n <= 8 || n % 500 == 0)
        logf("vk: ASTC texture #%u %#llx %ux%u x%u levels %u: the image of #%u %#llx (the same blocks, finalized by the game) "
             "copied, not decoded again (%lld so far, %lld MB)", tex.id, (unsigned long long)tex.gpu, tex.width, tex.height,
             tex.layers, tex.levels, from->id, (unsigned long long)from->gpu, (long long)n,
             (long long)(st_reused_bytes.load() >> 20));
    return true;
}

/// (longshot) A big texture whose whole storage is zero: the image cleared to zero on the GPU, not unswizzled on the CPU. All-zero
/// bits are 0 in every format this path takes (not ASTC or BC, not one widened on the way), so the clear is what the unswizzle
/// would have made. The game's shadow map (12288 x 12288 D16, 288 MB, its storage fresh and zero) took 73 ms of the replay
/// thread at a scene change; the check reads the storage once, on several threads, and stops at the first byte that is not 0
bool upload_all_zero(Texture& tex, uint64_t total, const Ranges* dirty, bool widened_format, bool& checked) {
    checked = false;
    if (total < (32ull << 20) || tex.astc || tex.bc || tex.sparse || widened_format || !tex.image ||
        tex.type == VK_IMAGE_TYPE_3D || is_bc(tex.format) || is_astc(tex.format))
        return false;
    if (dirty) {   // (only an upload of the whole storage: a part of it says nothing of the rest)
        uint64_t covered = 0;
        for (const auto& r : *dirty) covered += std::min(r.second, total) - std::min(r.first, total);
        if (covered < total) return false;
    }
    // (live memory, as the GPU unswizzle reads it: not a storage the capture kept as it was at its submit, which the CPU path reads)
    if (tex.gpu >= (1ull << 40) || memory_has_shadow(tex.gpu, total)) return false;
    uint64_t avail = 0;
    const uint8_t* p = host->gpu_ptr(tex.gpu, &avail);
    if (!p || avail < total) return false;
    checked = true;
    const auto t0 = std::chrono::steady_clock::now();
    // (longshot) only the parts of the storage that hold pages: a page of the guest window's memfd nothing wrote is a hole, zero,
    // and reading it made the kernel give it a page (guest_holes.h). The 12288 x 12288 D16 shadow map, 284 of its 288 MB outside
    // any import and never written, became guest memory that way at each Hyrule Field load on a Steam Deck (checked in 203-353 ms,
    // against 5.5 ms for a 32 MB storage already in memory), and went to swap with the rest of the load
    guest_holes::Extents scan;
    bool holes_used = false;
#if defined(__linux__)
    {
        static const int memfd = [] {
            typedef int (*MemfdFn)(void);
            const MemfdFn fn = (MemfdFn)dlsym(RTLD_DEFAULT, "hos_posix_guest_memfd");
            return fn ? fn() : -1;
        }();
        const uint8_t* base = host->guest_base();
        const uint64_t window = host->guest_size();
        const bool in_window = base && p >= base && (uint64_t)(p - base) <= window && total <= window - (uint64_t)(p - base);
        holes_used = memfd >= 0 && in_window && guest_holes::fd_data_extents(memfd, (uint64_t)(p - base), total, scan);
        if (!holes_used) scan.assign(1, {0, total});
    }
#else
    scan.assign(1, {0, total});
#endif
    uint64_t scanned = 0;
    for (const auto& [a, b] : scan) scanned += b - a;
    std::atomic<bool> found{false};
    std::atomic<uint64_t> nread{0};
    const uint32_t nthreads = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u);
    auto part = [&](uint32_t t) {   // (the threads share the data, not the storage: guest_holes::share)
        uint64_t mine = 0;
        for (const auto& [lo, hi] : guest_holes::share(scan, t, nthreads)) {
            for (uint64_t at = lo; at < hi && !found.load(std::memory_order_relaxed); at += 65536) {
                const uint64_t end = std::min<uint64_t>(hi, at + 65536);
                uint64_t acc = 0;
                uint64_t k = at;
                for (; k + 8 <= end; k += 8) {
                    uint64_t w;
                    std::memcpy(&w, p + k, 8);
                    acc |= w;
                }
                for (; k < end; k++) acc |= p[k];
                mine += end - at;
                if (acc) found.store(true, std::memory_order_relaxed);
            }
        }
        nread.fetch_add(mine, std::memory_order_relaxed);
    };
    if (nthreads > 1 && scanned) {   // (no helper threads when it is all holes: nothing to read)
        ::mkwvk::cxx_escape::fork_join("texture check", nthreads, part, helper_thread_start);
    } else {
        part(0);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    static std::atomic<int> told{0};
    char what[64];   // (what the check read: "4 MB read, the rest holes", "512 KB read", "nothing read, all holes")
    if (const uint64_t n = nread.load(); !n && holes_used) {
        snprintf(what, sizeof what, "nothing read, all holes");
    } else {
        const bool mb = n >= (1ull << 20);
        snprintf(what, sizeof what, "%llu %s read%s", (unsigned long long)(mb ? n >> 20 : n >> 10), mb ? "MB" : "KB",
                 holes_used && scanned < total ? ", the rest holes" : "");
    }
    if (found.load()) {
        if (told.fetch_add(1) < 4)
            logf("vk: upload: %ux%u NVN %#x (%llu MB) is not all zero (checked in %.1f ms, %s): unswizzled as before", tex.width,
                 tex.height, tex.nvn_format, (unsigned long long)(total >> 20), ms, what);
        return false;
    }
    texture_resident_flush(&tex);
    checkpoint_mark(upload_cmd(), tex.gpu, 'u');
    const VkImageSubresourceRange range{tex.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    if (tex.aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        const VkClearDepthStencilValue z{0.0f, 0};
        vkCmdClearDepthStencilImage(upload_cmd(), tex.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &range);
    } else {
        const VkClearColorValue z{};
        vkCmdClearColorImage(upload_cmd(), tex.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &range);
    }
    if (told.fetch_add(1) < 4)
        logf("vk: upload: %ux%u NVN %#x (%llu MB) is all zero: cleared on the GPU (checked in %.1f ms, %s)", tex.width, tex.height,
             tex.nvn_format, (unsigned long long)(total >> 20), ms, what);
    return true;
}
/// texdecode refused a call (an argument out of range, or a span shorter than the level needs: no caller here makes either, and
/// nothing falls back to another decoder): said at most 5 times. What the caller does is in `then`
void texdecode_refused(const char* what, const char* then) {
    static std::atomic<int> told{0};
    if (told.fetch_add(1) < 5) logf("vk: texdecode refused its arguments for the %s (%s)", what, then);
}
void unswizzle_upload(std::span<uint8_t> dest, std::span<const uint8_t> src, uint32_t bpb,
                      uint32_t width, uint32_t height, uint32_t depth, uint32_t tile_h, uint32_t tile_d) {
    if (depth == 1 && !tile_d && texture_tiles::unswizzle(dest, src, uint64_t(width) * bpb, height, tile_h)) return;
    // level_geo counts GOB columns from width * bpb, including for odd-width RGB textures.
    if (!texdecode::deswizzle(dest, src, bpb, width, height, depth, tile_h, tile_d)) {
        std::memset(dest.data(), 0, dest.size());   // (never what the staging memory held before)
        texdecode_refused("unswizzle", "the texels are zero");
    }
}
/// An ASTC texture's blocks the specification calls illegal (the decoder wrote transparent black for each, as main's decoder and the GPU shader do): its count, said once per
/// texture and for the first 16 textures only. Counts only, never texel values
void astc_error_blocks_told(const Texture& tex, uint64_t blocks) {
    static std::mutex mu;
    static std::vector<uint32_t> told;
    std::lock_guard lk(mu);
    if (told.size() >= 16 || std::find(told.begin(), told.end(), tex.id) != told.end()) return;
    told.push_back(tex.id);
    logf("vk: ASTC texture #%u %ux%u NVN %#x: %llu illegal blocks decoded as transparent black%s", tex.id, tex.width, tex.height,
         tex.nvn_format, (unsigned long long)blocks, told.size() == 16 ? " (no more such lines)" : "");
}

/// An image no upload takes, whatever memory there is: its upload fails every time, and it is not tried again
bool upload_never(const Texture& tex) {
    return !tex.gpu || tex.samples != VK_SAMPLE_COUNT_1_BIT || !tex.guest_size ||
           (tex.guest_size > (1ull << 31) && !tex.sparse) ||   // (a sparse image: only what holds texels)
           // (depth-stencil: the depth aspect only; packed D24S8 storage is not copied into it as is: such targets are
           // rendered, not uploaded)
           tex.aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
}
bool upload_inner(Texture& tex, const Ranges* dirty) {
    PROF(PROF_UPLOAD);
    if (upload_never(tex)) return false;
    const uint64_t total = tex.guest_size;
    st_uploads++;
    {   // RX_VK_LOG_UPLOADS=<present>: the uploads from that present on (200 of them; what churns)
        static const int64_t from = getenv("RX_VK_LOG_UPLOADS") ? atoll(getenv("RX_VK_LOG_UPLOADS")) : -1;
        static int logged;
        if (from >= 0 && (int64_t)frame_no >= from && logged < 200) {
            logged++;
            uint64_t part = 0;
            if (dirty)
                for (const auto& r : *dirty) part += r.second - r.first;
            logf("vk: upload #%u %#llx %ux%ux%u x%u levels %u NVN %#x%s: %llu KB of %llu KB (%s), made at present %llu",
                 tex.id, (unsigned long long)tex.gpu, tex.width, tex.height, tex.depth, tex.layers, tex.levels,
                 tex.nvn_format, tex.astc ? " ASTC" : "", (unsigned long long)((dirty ? part : total) >> 10),
                 (unsigned long long)(total >> 10), !tex.uploaded ? "new" : dirty ? "written" : "whole",
                 (unsigned long long)tex.first_seen);
        }
    }
    if (tex.astc) {
        tex.blocks_hash = 0;
        if (!dirty && !tex.uploaded && reuse_upload(tex, total)) return up_route = UP_ASTC, true;
    }
    if (tex.astc && (up_route = UP_ASTC, upload_astc_gpu(tex, total, dirty))) return true;
    up_route = UP_CPU;
    if (tex.bc) return false;   // (no CPU path into BC3)
    // (the GPU's unswizzle reads guest memory as it is: not where the capture kept the storage as it was at its submit)
    if (!(tex.gpu < (1ull << 40) && memory_has_shadow(tex.gpu, total)) && upload_unswizzle_gpu(tex, total, dirty))
        return up_route = UP_GPU, true;
    const bool linear = nvn_tex_is_linear(&tex.d);
    int widen_from = 0;
    const VkFormat src_vf = tex.d.format ? vk_format(tex.nvn_format).format : tex.format;
    widened(src_vf, &widen_from);
    bool large_zero_checked = false;
    if (upload_all_zero(tex, total, dirty, widen_from != 0, large_zero_checked)) return true;
    {   // (RX_VK_DEPTH_ZERO) a depth buffer read before the GPU drew it, its storage all zero: cleared on the GPU, not
        // unswizzled on the CPU (upload_all_zero takes only storage 32 MB and more; these are 1-9 MB in a virtual pool).
        // In Kokiri 15 of 25 such reads were of zero storage: the shadow maps the game draws at one size (816 x 816) and
        // samples at another (812, 813) over the same memory. The bytes checked are guest_bytes', as the CPU route reads
        // them; a depth image cleared to 0 is what unswizzling zero storage makes. RX_VK_DEPTH_ZERO=0: off
        static const bool dz_on = [] { const char* e = getenv("RX_VK_DEPTH_ZERO"); return !(e && env_value_off(e)); }();
        static std::atomic<int64_t> dz_n{0}, dz_bytes{0}, dz_not{0};
        if (dz_on && !large_zero_checked && !dirty && (tex.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && tex.image && !tex.sparse && tex.type != VK_IMAGE_TYPE_3D) {
            static thread_local std::vector<uint8_t> zs;
            bool zero = true;
            for (uint64_t at = 0; at < total && zero; at += 16ull << 20) {
                const uint64_t n = std::min<uint64_t>(16ull << 20, total - at);
                const uint8_t* q = guest_bytes(tex.gpu + at, n, zs);
                uint64_t acc = 0, i = 0;
                for (; i + 8 <= n && !acc; i += 8) {
                    uint64_t w;
                    std::memcpy(&w, q + i, 8);
                    acc |= w;
                }
                for (; i < n && !acc; i++) acc |= q[i];
                zero = !acc;
            }
            if (zero) {
                checkpoint_mark(upload_cmd(), tex.gpu, 'u');
                const VkImageSubresourceRange range{tex.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
                const VkClearDepthStencilValue z{0.0f, 0};
                vkCmdClearDepthStencilImage(upload_cmd(), tex.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &range);
                const int64_t k = ++dz_n;
                dz_bytes += (int64_t)total;
                if (k <= 4 || k % 200 == 0)
                    logf("vk: depth buffer %ux%u NVN %#x (%.1f MB) all zero: cleared on the GPU (%lld so far, %.0f MB; %lld were not zero)",
                         tex.width, tex.height, tex.nvn_format, total / 1048576.0, (long long)k, dz_bytes.load() / 1048576.0, (long long)dz_not.load());
                return true;
            }
            dz_not++;
        }
    }
    const int src_bpb = tex.bpb;
    const int out_bpb = tex.astc ? 4 : widen_from ? (widen_from == 3 ? 4 : widen_from == 6 ? 8 : 16) : src_bpb;
    const uint32_t copy_layers = tex.type == VK_IMAGE_TYPE_3D ? 1 : tex.layers;
    const VkImageAspectFlags aspect = tex.aspect;
    std::vector<VkBufferImageCopy> copies;
    std::vector<uint8_t> scratch, lin;
    VkBuffer buffer = VK_NULL_HANDLE;
    uint64_t astc_bad_blocks = 0;   // (illegal ASTC blocks decoded here, over every level and layer)
    for (uint32_t level = 0; level < tex.levels; level++) {
        const LevelGeo g = level_geo(tex, level);
        for (uint32_t layer = 0; layer < copy_layers; layer++) {
            const uint64_t src_off = g.offset + (uint64_t)layer * tex.L.layer_size;
            if (src_off >= total) continue;
            const uint32_t stride = linear ? (tex.d.stride ? tex.d.stride : host->tex_linear_stride(&tex.d)) : 0;
            const uint64_t span = linear ? (uint64_t)stride * g.th : std::max<uint64_t>(g.swizzled, 1);
            if (!overlaps(dirty, src_off, src_off + span)) continue;
            // the block rows to upload: all of them, or those a written page falls in (2D block-linear levels)
            const uint32_t rows_per = 8u << g.tile_h;   // (block rows of a GOB block)
            const uint32_t nrows = (g.th + rows_per - 1) / rows_per;
            const uint64_t gobs_x = ((uint64_t)g.tw * src_bpb + 63) / 64;
            const uint64_t row_bytes = gobs_x * (512ull << (g.tile_h + g.tile_d));
            std::vector<std::pair<uint32_t, uint32_t>> runs;   // (first row, rows)
            if (!dirty || linear || tex.astc || widen_from || g.td > 1 || g.tile_d) {
                runs.push_back({0, nrows});
            } else {
                std::vector<uint8_t> mark(nrows, 0);
                for (const auto& r : *dirty) {
                    const uint64_t lo = std::max(r.first, src_off), hi = std::min(r.second, src_off + span);
                    if (lo >= hi) continue;
                    const uint64_t r0 = (lo - src_off) / row_bytes, r1 = std::min<uint64_t>((hi - 1 - src_off) / row_bytes, nrows - 1);
                    for (uint64_t k = r0; k <= r1; k++) mark[k] = 1;
                }
                for (uint32_t k = 0; k < nrows;) {
                    if (!mark[k]) { k++; continue; }
                    uint32_t e = k;
                    while (e < nrows && mark[e]) e++;
                    runs.push_back({k, e - k});
                    k = e;
                }
            }
            for (const auto& [row0, nrow] : runs) {
                const uint32_t y0 = row0 * rows_per;                       // (blocks)
                const uint32_t th = std::min(g.th - y0, nrow * rows_per);   // (blocks)
                const uint64_t block_bytes = (uint64_t)g.tw * th * g.td * src_bpb;
                const uint32_t px_h = std::min(g.h - y0 * (uint32_t)tex.block_h, th * (uint32_t)tex.block_h);
                const uint64_t out_bytes = tex.astc ? (uint64_t)g.w * px_h * g.d * 4 : (uint64_t)g.tw * th * g.td * out_bpb;
                const Staging st = staging((out_bytes + 15) & ~15ull, 16);
                if (!st.buffer) {
                    // (the sparse binds queued by the rows before are this image's: bound now, not left in sparse_binds
                    // for the next flush, which binds them into another texture's image)
                    texture_resident_flush(&tex);
                    return false;
                }
                if (buffer && st.buffer != buffer && !copies.empty()) {   // (a new arena: copy what the last one holds)
                    texture_resident_flush(&tex);
                    checkpoint_mark(upload_cmd(), tex.gpu, 'u');
                    vkCmdCopyBufferToImage(upload_cmd(), buffer, tex.image, VK_IMAGE_LAYOUT_GENERAL,
                                           (uint32_t)copies.size(), copies.data());
                    copies.clear();
                }
                buffer = st.buffer;
                uint8_t* out = st.data;
                uint8_t* blocks = (tex.astc || widen_from) ? (lin.resize(block_bytes), lin.data()) : out;
                if (linear) {
                    const uint64_t row = (uint64_t)g.tw * src_bpb;
                    const uint8_t* src = guest_bytes(tex.gpu + src_off, (uint64_t)stride * (g.th - 1) + row, scratch);
                    for (uint32_t y = 0; y < g.th; y++) std::memcpy(blocks + y * row, src + (uint64_t)y * stride, row);
                } else {
                    // (texdecode's deswizzle refuses a span that ends before the last GOB a texel is in, and reads only
                    // the GOBs the texels are in: a span the storage ends in is read from a zero-padded copy)
                    const uint64_t from = src_off + (uint64_t)row0 * row_bytes;
                    const uint64_t need = runs.size() == 1 && row0 == 0 && nrow == nrows ? span : (uint64_t)nrow * row_bytes;
                    // (a big 2D level: its GOB-block rows unswizzled on several threads, each row on its own. The
                    // course maps the title loads at each scene change, 6 of 349 MB, took ~100 ms each on one. Their
                    // storage is sparse: each thread gathers its own rows, 8 MB at a time, rather than all of it
                    // being copied first (a 349 MB scratch beside the 349 MB staged))
                    const uint32_t nthreads = block_bytes >= (16u << 20) && g.td == 1 && !g.tile_d
                                                  ? std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u) : 1u;
                    uint64_t avail = 0;
                    const uint8_t* direct = host->gpu_ptr(tex.gpu + from, &avail);
                    const bool gather_parts = nthreads > 1 && !(direct && avail >= need) && from < total;
                    const uint8_t* src = nullptr;
                    if (gather_parts) {
                    } else if (from < total && total - from >= need) {
                        src = guest_bytes(tex.gpu + from, need, scratch);
                    } else {
                        scratch.assign(need, 0);
                        if (from < total) host->gpu_read(tex.gpu + from, scratch.data(), total - from);
                        src = scratch.data();
                    }
                    const auto u0 = std::chrono::steady_clock::now();
                    if (nthreads > 1) {
                        const uint64_t out_row = (uint64_t)g.tw * rows_per * src_bpb;   // (bytes per GOB-block row)
                        const uint32_t per = (nrow + nthreads - 1) / nthreads;
                        const uint32_t chunk = (uint32_t)std::max<uint64_t>(1, (8u << 20) / row_bytes);
                        auto part = [&](uint32_t t) {
                            const uint32_t a0 = t * per, b0 = std::min(nrow, a0 + per);
                            std::vector<uint8_t> mine;
                            for (uint32_t a = a0; a < b0; a += chunk) {
                                const uint32_t b = std::min(b0, a + chunk);
                                const uint32_t h = std::min(th - a * rows_per, (b - a) * rows_per);
                                const uint8_t* rows;
                                uint64_t rows_n;
                                if (gather_parts) {   // (its rows, zero past the storage's end)
                                    rows_n = (uint64_t)(b - a) * row_bytes;
                                    mine.assign(rows_n, 0);
                                    const uint64_t at = from + (uint64_t)a * row_bytes;
                                    for (uint64_t o = 0; o < rows_n && at + o < total; o += 0x10000) {
                                        const uint64_t k = std::min<uint64_t>({0x10000, rows_n - o, total - at - o});
                                        host->gpu_read(tex.gpu + at + o, mine.data() + o, k);
                                    }
                                    rows = mine.data();
                                } else {
                                    rows = src + (uint64_t)a * row_bytes, rows_n = need - (uint64_t)a * row_bytes;
                                }
                                unswizzle_upload(
                                    std::span<uint8_t>(blocks + a * out_row, (uint64_t)g.tw * h * src_bpb),
                                    std::span<const uint8_t>(rows, rows_n), (uint32_t)src_bpb, g.tw, h, 1, g.tile_h, 0);
                            }
                        };
                        ::mkwvk::cxx_escape::fork_join("texture unswizzle", nthreads, part, helper_thread_start);
                        static const bool check = env_on("RX_VK_UNSWIZZLE_CHECK");   // (against one thread)
                        if (check && src) {
                            std::vector<uint8_t> ref(block_bytes);
                            const bool same =
                                texdecode::deswizzle(std::span<uint8_t>(ref.data(), block_bytes), std::span<const uint8_t>(src, need),
                                                     (uint32_t)src_bpb, g.tw, th, g.td, g.tile_h, g.tile_d) &&
                                !std::memcmp(ref.data(), blocks, block_bytes);
                            logf("vk: unswizzle check %#llx level %u rows %u+%u: %s", (unsigned long long)tex.gpu,
                                 level, row0, nrow, same ? "same" : "DIFFERENT");
                        }
                    } else {
                        unswizzle_upload(std::span<uint8_t>(blocks, block_bytes), std::span<const uint8_t>(src, need),
                                         (uint32_t)src_bpb, g.tw, th, g.td, g.tile_h, g.tile_d);
                    }
                    st_unswizzle_bytes += (int64_t)block_bytes;
                    st_unswizzle_ns +=
                        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - u0).count();
                }
                if (tex.astc) {
                    const int64_t bad = texdecode::astc_decode(std::span<const uint8_t>(blocks, block_bytes), g.w, px_h, g.d,
                                                               (uint32_t)tex.block_w, (uint32_t)tex.block_h, astc_nvn_srgb(tex.nvn_format),
                                                               std::span<uint8_t>(out, out_bytes),
                                                               texdecode::Rgba8{0, 0, 0, 0});   // (as suyu's decoder and the GPU shader write an illegal block: an all-zero block is one, and storage the game has not written yet is decoded)
                    if (bad < 0) {
                        std::memset(out, 0, out_bytes);   // (never what the staging memory held before)
                        texdecode_refused("ASTC decode", "the texels are zero");
                    } else {
                        astc_bad_blocks += (uint64_t)bad;
                    }
                } else if (widen_from) {
                    widen_rows(out, blocks, (size_t)g.tw * th * g.td, src_bpb, out_bpb, widen_alpha(src_vf));
                }
                st_bytes += (int64_t)out_bytes;
                // (a sparse image: only its blocks this holds texels other than zero get memory; the others read
                // zero unbound, and what the copy writes into them is dropped)
                if (tex.sparse) {
                    const VkExtent3D gr = tex.sparse_req.formatProperties.imageGranularity;
                    const uint32_t bw = (uint32_t)tex.block_w, bh = (uint32_t)tex.block_h;
                    const uint64_t row = (uint64_t)g.tw * out_bpb;   // (bytes of a row of blocks)
                    const uint32_t tbw = std::max(1u, gr.width / bw);   // (blocks across a sparse block)
                    const uint32_t ntx = (g.tw + tbw - 1) / tbw;
                    std::vector<uint8_t> hit;   // (sparse block rows x columns this reaches)
                    const uint32_t ty0 = y0 * bh / gr.height, ty1 = ((y0 + th) * bh - 1) / gr.height;
                    hit.assign((size_t)(ty1 - ty0 + 1) * ntx, 0);
                    if (level < tex.sparse_req.imageMipTailFirstLod) {
                        for (uint32_t br = 0; br < th; br++) {
                            const uint8_t* p = out + br * row;
                            const uint32_t ty = (y0 + br) * bh / gr.height - ty0;
                            for (uint32_t tx = 0; tx < ntx; tx++) {
                                uint8_t& h = hit[(size_t)ty * ntx + tx];
                                if (h) continue;
                                const uint64_t a = (uint64_t)tx * tbw * out_bpb, e = std::min<uint64_t>(row, a + (uint64_t)tbw * out_bpb);
                                uint64_t any = 0;
                                for (uint64_t i = a; i + 8 <= e; i += 8) {
                                    uint64_t v;
                                    std::memcpy(&v, p + i, 8);
                                    any |= v;
                                }
                                h = any != 0;
                            }
                        }
                    } else {
                        uint64_t any = 0;
                        for (uint64_t i = 0; i + 8 <= out_bytes && !any; i += 8) {
                            uint64_t v;
                            std::memcpy(&v, out + i, 8);
                            any |= v;
                        }
                        if (any) hit.assign(hit.size(), 1);
                    }
                    bool some = false;
                    for (uint32_t ty = 0; ty < ty1 - ty0 + 1; ty++)
                        for (uint32_t tx = 0; tx < ntx; tx++)
                            if (hit[(size_t)ty * ntx + tx]) {
                                some = true;
                                texture_resident_queue(&tex, level, layer, tx * gr.width, (ty0 + ty) * gr.height,
                                                       gr.width, gr.height);
                            }
                    if (!some) continue;
                }
                flush_mapped_range(st.allocation, st.offset, out_bytes);   // (all CPU conversion workers joined before publication)
                VkBufferImageCopy c{};
                c.bufferOffset = st.offset;
                c.bufferRowLength = tex.astc ? g.w : g.tw * tex.block_w;
                c.bufferImageHeight = tex.astc ? px_h : th * tex.block_h;
                c.imageSubresource = {aspect, level, tex.type == VK_IMAGE_TYPE_3D ? 0 : layer, 1};
                c.imageOffset = {0, (int32_t)(y0 * (uint32_t)tex.block_h), 0};
                c.imageExtent = {g.w, px_h, g.d};
                copies.push_back(c);
            }
        }
    }
    if (astc_bad_blocks) astc_error_blocks_told(tex, astc_bad_blocks);
    if (!copies.empty()) {
        texture_resident_flush(&tex);
        checkpoint_mark(upload_cmd(), tex.gpu, 'u');
        vkCmdCopyBufferToImage(upload_cmd(), buffer, tex.image, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)copies.size(),
                               copies.data());
    }
    return true;
}

/// What the CPU wrote of a texture's storage since gen, as byte ranges from its start. A texture in a virtual
/// (sparse) pool is looked at page by page through its mapping, and is all dirty when its mapping changed.
bool texture_dirty(Texture& tex, uint64_t gen, Ranges& out) {
    out.clear();
    constexpr uint64_t kVirtual = 1ull << 40, kVPage = 1ull << 16;
    if (tex.gpu < kVirtual) {
        memory_written_runs(tex.guest, tex.guest_size, gen, out);
        return !out.empty();
    }
    // (the page walk only when the GPU mappings changed: the terrain's sparse arrays span over 1 GB, 64 KB pages,
    // and were walked every recording; between changes, runs of contiguous guest memory are checked)
    const uint32_t maps_gen = host->maps_generation() + host->vmap_generation();
    if (tex.runs_maps_gen != maps_gen) {
        tex.runs_maps_gen = maps_gen;
        tex.runs.clear();
        uint64_t sig = 0x5157;
        // (the pages whose mapping changed: only those are dirty. The course maps' sparse pools are remapped every
        // ~15 s at the title, a few pages at a time, and each remap re-uploaded all of six 349 MB maps)
        const size_t npages = (size_t)((tex.guest_size + kVPage - 1) / kVPage);
        const bool had_map = tex.page_map.size() == npages;
        if (!had_map) tex.page_map.assign(npages, 0);
        Ranges remapped;
        for (uint64_t off = 0; off < tex.guest_size; off += kVPage) {
            const uint64_t n = std::min(kVPage, tex.guest_size - off);
            const uint64_t g = host->gpu_guest(tex.gpu + off);
            sig = mix(sig, g);
            uint64_t& was = tex.page_map[(size_t)(off / kVPage)];
            if (had_map && was != g) {
                if (!remapped.empty() && remapped.back().second == off) remapped.back().second = off + n;
                else remapped.push_back({off, off + n});
            }
            was = g;
            if (!g) continue;
            Texture::Run& last = tex.runs.empty() ? tex.runs.emplace_back(Texture::Run{off, g, 0}) : tex.runs.back();
            if (last.off + last.n == off && last.guest + last.n == g) last.n += n;
            else tex.runs.push_back({off, g, n});
        }
        std::erase_if(tex.runs, [](const Texture::Run& r) { return r.n == 0; });
        {   // (RX_VK_LOG_REMAP=1: a big texture's pages mapped anew: to which guest memory, and what images are there)
            static const bool log_remap = env_on("RX_VK_LOG_REMAP");
            static int logged;
            if (log_remap && had_map && !remapped.empty() && tex.guest_size >= (32ull << 20) && logged < 120) {
                for (size_t i = 0; i < remapped.size() && i < 4 && logged < 120; i++, logged++) {
                    const uint64_t g = host->gpu_guest(tex.gpu + remapped[i].first);
                    std::string there;
                    for (const auto& kv : textures) {   // (images over that guest memory, by their storage)
                        const Texture* o = kv.second.get();
                        if (o == &tex || !g || o->gpu >= (1ull << 40) || g < o->gpu || g >= o->gpu + o->guest_size) continue;
                        char b[160];
                        snprintf(b, sizeof b, " #%u %#llx %ux%u x%u NVN %#x%s%s;", o->id, (unsigned long long)o->gpu, o->width,
                                 o->height, o->layers, o->nvn_format, o->gpu_written ? " rendered" : "",
                                 o->uploaded ? " uploaded" : "");
                        if (there.size() < 400) there += b;
                    }
                    logf("vk: remap of #%u %#llx %ux%u NVN %#x: +%#llx..+%#llx now at guest %#llx%s%s", tex.id,
                         (unsigned long long)tex.gpu, tex.width, tex.height, tex.nvn_format,
                         (unsigned long long)remapped[i].first, (unsigned long long)remapped[i].second, (unsigned long long)g,
                         g ? "" : " (unmapped)", there.empty() ? " (no image there)" : there.c_str());
                }
            }
        }
        if (sig != tex.map_sig) {   // (pages mapped or unmapped since; gen 0 asks what was ever written)
            tex.map_sig = sig;
            if (gen && !had_map) {
                out.assign(1, {0, tex.guest_size});
                return true;
            }
            if (gen) out = remapped;   // (and below, what the CPU wrote in the pages mapped all along)
        }
    }
    static thread_local Ranges in_run;
    const size_t remaps = out.size();
    for (const Texture::Run& r : tex.runs) {
        in_run.clear();
        memory_written_runs(r.guest, r.n, gen, in_run);
        for (const auto& [lo, hi] : in_run) {
            if (out.size() > remaps && out.back().second == r.off + lo) out.back().second = r.off + hi;
            else out.push_back({r.off + lo, r.off + hi});
        }
    }
    if (remaps && out.size() > remaps) {   // (the two lists in order, overlaps merged)
        std::sort(out.begin(), out.end());
        Ranges merged;
        for (const auto& r : out) {
            if (!merged.empty() && r.first <= merged.back().second) merged.back().second = std::max(merged.back().second, r.second);
            else merged.push_back(r);
        }
        out.swap(merged);
    }
    return !out.empty();
}

uint64_t destroy_gen = 1;   // (bumped when textures go: what was resolved to them goes stale)
// (RX_VK_EXACT_STALE) Which texture went at each generation: its creation number (Texture::id), or kGoneAll where every
// resolved view is stale. A handle resolved to another texture stands (texture_still_resolved): one counter for all made
// every draw of the next frame look its textures up again and build its descriptors again whenever any texture went, 11 ms
// of lookups and 12 ms of descriptors in the frame after the pause menu's own targets went, with the render thread 30-40 ms
// behind for the frames after it. The slot is written before the generation that names it is, so a reader that saw the
// generation sees the slot.
constexpr uint32_t kGoneLog = 16384, kGoneAll = 0xFFFFFFFFu;   // (a power of two; a sweep takes ~1,400 at once at most)
std::atomic<uint32_t> gone_log[kGoneLog];
void gone_note(uint32_t id) {
    gone_log[(destroy_gen + 1) & (kGoneLog - 1)].store(id, std::memory_order_release);
    destroy_gen++;
}

/// (`completed`: the newest submission the GPU has finished. A render target's image it is done with goes to the pool now, not
/// at the next present: the next frame is recorded between the two, and its target of the same shape made a new image while
/// this one waited, to be destroyed unused 120 presents later (first seen in Hookshot-old's 7bba12ad). One the GPU may still
/// read, a timed-out write-back's source, waits in the dead list as before)
void destroy_later(Texture& tex, uint64_t completed) {
    gone_note(tex.id);
    Dead d{tex.last_value, tex.image, tex.allocation, {}, {}};
    if (tex.pool_key && tex.allocation && tex.sparse_chunks.empty() && pool_cap()) {   // (a render target's: to the pool, its views too)
        d.pool_key = tex.pool_key, d.view_map = std::move(tex.views), d.bytes = tex.bytes + tex.pad;
        tex.views.clear();
    } else {
        for (auto& kv : tex.views)
            if (kv.second) d.views.push_back(kv.second);
    }
    d.memory = tex.sparse_chunks;
    if (d.pool_key && d.value <= completed) pool_put(d);
    else dead_pooled_n.fetch_add(d.pool_key ? 1 : 0, std::memory_order_relaxed), dead.push_back(std::move(d));
    for (const auto& attachment : tex.block_attachments)
        dead.push_back(Dead{tex.last_value, attachment.image, attachment.allocation, {}, {}});
    total_bytes -= std::min(total_bytes, tex.bytes + tex.pad);
}

void vw_forget(const Texture& t);   // (below: a destroyed texture leaves the list of GPU-written ones)
void forget_gpu(Texture* tex) {
    {
        std::lock_guard lk(gpu_wrote_mu);
        gpu_wrote.forget(tex->id, tex->gpu, tex->gpu + tex->guest_size);
    }
    vw_forget(*tex);
    reuse_drop(*tex);
    auto [a, b] = by_gpu.equal_range(tex->gpu);
    for (auto it = a; it != b; ++it)
        if (it->second == tex) {
            by_gpu.erase(it);
            break;
        }
}

/// (as_target: texture_get's for a render target or a copy's destination, not for sampling: its image goes to the render targets'
/// blocks, and to the target pool when its texture goes)
/// (longshot) a size and sample count Vulkan can make on this GPU: they come from the game's texture (its width a u32 it
/// reads from a file), and past the device's limits vkCreateImage is undefined, and the uploads' buffers grew with them
bool texture_size_ok(VkImageType type, uint32_t w, uint32_t h, uint32_t z, uint32_t layers, int samples) {
    const VkPhysicalDeviceLimits& lim = dev.props.limits;
    const uint32_t most = type == VK_IMAGE_TYPE_1D ? lim.maxImageDimension1D
                        : type == VK_IMAGE_TYPE_3D ? lim.maxImageDimension3D : lim.maxImageDimension2D;
    return w <= most && h <= most && z <= lim.maxImageDimension3D && layers <= lim.maxImageArrayLayers &&
           (samples <= 1 || samples == 2 || samples == 4 || samples == 8 || samples == 16);
}
/// One mip level in texels (bw = bh = 1), or whole blocks for a compressed image.
/// Round each mip extent up separately: even a 1x1 BC/ASTC level occupies one block.
/// (Counted as texels, an ASTC image the GPU samples itself was 16 bytes a texel, 16 to 144 times its size, and
/// BC1/BC4 twice theirs: tests/test_native_astc_texture_count.py)
uint64_t texture_level_units(uint32_t w, uint32_t h, uint32_t d, uint32_t bw, uint32_t bh) {
    w = std::max(1u, w), h = std::max(1u, h), d = std::max(1u, d), bw = std::max(1u, bw), bh = std::max(1u, bh);
    return (uint64_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * d;
}
/// (longshot) the PC's RAM in bytes, read once; 0 when it cannot be read
uint64_t system_ram_bytes() {
    static const uint64_t v = [] {
        uint64_t ram = 0;
#if defined(_WIN32)
        MEMORYSTATUSEX m{};
        m.dwLength = sizeof m;
        if (GlobalMemoryStatusEx(&m)) ram = m.ullTotalPhys;
#else
        if (FILE* f = fopen("/proc/meminfo", "r")) {
            char line[256];
            unsigned long long kb = 0;
            while (fgets(line, sizeof line, f))
                if (sscanf(line, "MemTotal: %llu kB", &kb) == 1) {
                    ram = (uint64_t)kb << 10;
                    break;
                }
            fclose(f);
        }
#endif
        return ram;
    }();
    return v;
}
bool low_vram_texture_mips() {
    static const bool disabled = [] {
        const char* e = getenv("RX_VK_LOW_VRAM_MIPS");
        return e && env_value_off(e);
    }();
    // A driver budget, sampled once, not the card's physical capacity. The
    // preset is checked for each texture so changing it in the menu still works.
    static const bool small = [] {
        uint64_t used = 0, allowed = 0;
        return vram_usage(&used, &allowed) && allowed && allowed <= (4ull << 30) + (512ull << 20);
    }();
    // (longshot) An integrated GPU's budget is a share of the system's RAM, which the game shares: an i3-1115G4's UHD Graphics
    // with 16 GB was allowed 7.4 GB, past the 4.5 GiB, while the game, 2 GB of textures and the driver held 12-13 GB and
    // Windows paged the game out (3-6 fps). With 16 GB of RAM or less it counts as small whatever the driver allows
    static const bool shared = dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU && system_ram_bytes() &&
                               system_ram_bytes() <= (16ull << 30);
    // (Minimum's settings, memory tier 1: Minimum, Auto (this PC) at Minimum's tier, or Custom after either, which keeps them: the
    // Debug tab's Set up for a weak laptop leaves the preset on Custom. Custom with no preset behind it goes by Auto's tier for
    // this PC: a Steam Deck on an options file from before memory_budget drew the terrain's 1365x1365x90 arrays at full size, and
    // the arrivals that decode them again took twice as long, 1.0 against 2.2-2.4 s at the save's load)
    return !disabled && (small || shared) && (opts.preset == 1 || menu_memory_tier() == 1);
}

static std::unique_ptr<Texture> probe_texture_metadata() {
    const NewNothrow nothrow;
    return std::unique_ptr<Texture>(new (std::nothrow) Texture);
}

Texture* create(const Key& key, const nvn_tex* t, VkFormat vf, bool astc, int bw, int bh, int bpb, bool as_target,
                OutputTrialBacking::Image* output_probe = nullptr) {
#ifdef TRACY_ENABLE
    HT_SCOPE("texture create");
#endif
    PROF_EXACT();   // (texture_get's slow path: PROF_TEXGET samples the rest)
    const StepTimer step_timed(0);
    if (step_enabled()) step_ns[7].fetch_add(1, std::memory_order_relaxed);
    auto tex = output_probe ? probe_texture_metadata() : std::make_unique<Texture>();
    if (!tex) return nullptr;
    Texture& x = *tex;
    x.gpu = t->gpu, x.d = t->d, x.L = t->L, x.nvn_format = t->d.format;
    x.guest = host->gpu_guest(t->gpu);
    x.guest_size = t->L.size;
    x.astc = astc, x.block_w = bw, x.block_h = bh, x.bpb = bpb;
    int from = 0;
    vf = widened(vf, &from);
    const bool depth = is_depth_format(vf);
    x.format = image_format_of(vf, astc);
    x.aspect = !depth ? VK_IMAGE_ASPECT_COLOR_BIT
             : vf == VK_FORMAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT
             : has_stencil(vf) ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                               : VK_IMAGE_ASPECT_DEPTH_BIT;
    const nvn_texdesc& d = t->d;
    x.width = std::max(1u, d.width);
    x.height = std::max(1u, d.height);
    switch (d.target) {
    case NVN_TEXTURE_TARGET_1D: x.type = VK_IMAGE_TYPE_1D, x.height = 1; break;
    case NVN_TEXTURE_TARGET_1D_ARRAY: x.type = VK_IMAGE_TYPE_1D, x.layers = std::max(1u, d.height), x.height = 1; break;
    case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: x.layers = std::max(1u, d.depth); break;
    case NVN_TEXTURE_TARGET_CUBEMAP: x.layers = 6; break;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: x.layers = std::max(6u, d.depth); break;
    case NVN_TEXTURE_TARGET_3D: x.type = VK_IMAGE_TYPE_3D, x.depth = std::max(1u, d.depth); break;
    default: break;
    }
    const uint32_t max_dim = std::max({x.width, x.height, x.depth});
    const uint32_t max_levels = 1 + (uint32_t)std::floor(std::log2((double)max_dim));
    x.levels = nvn_tex_is_linear(&d) ? 1 : std::clamp<uint32_t>(d.levels, 1, max_levels);
    {   // huge textures leave out their top levels: wider than RX_VK_TEX_MAX_DIM (default 2048) once they would take
        // 64 MB, and down to RX_VK_TEX_MAX_MB (default 1536) whatever their size (MKW's terrain material arrays are
        // 8192 x 8192 x 68 ASTC: 24 GB as RGBA8)
        const bool small_mips = low_vram_texture_mips();
        static const char* dim_env = getenv("RX_VK_TEX_MAX_DIM");
        const uint32_t max_dim = dim_env ? (uint32_t)atoi(dim_env) : small_mips ? 1024 : 2048;
        static const uint64_t max_bytes = (uint64_t)(getenv("RX_VK_TEX_MAX_MB") ? atoi(getenv("RX_VK_TEX_MAX_MB")) : 1536) << 20;
        const bool blocks = !astc && (is_bc(x.format) || is_astc(x.format));
        const uint32_t texel = astc ? 4 : blocks ? bpb : std::max(1, bpb / std::max(1, bw * bh));
        auto bytes_at = [&](uint32_t skip) {
            uint64_t px = 0;
            for (uint32_t l = skip; l < x.levels; l++)
                px += texture_level_units(x.width >> l, x.height >> l, x.depth >> l,
                                          blocks ? (uint32_t)bw : 1u, blocks ? (uint32_t)bh : 1u);
            return px * x.layers * texel * std::max(1, d.samples);
        };
        // (decoded ASTC only by size: uncompressed maps may be read at exact texel coordinates, and the GPU may
        // render into them, where a level left out is wrong)
        // (a 2D array too big to keep whole, in a format with sparse residency: kept whole, as a sparse image)
        if (!astc && !depth && dev.sparse_images && x.type == VK_IMAGE_TYPE_2D && x.layers > 1 && d.samples <= 1 &&
            bytes_at(0) > max_bytes) {
            VkImageUsageFlags u = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            if (!is_bc(x.format)) u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
            uint32_t np = 0;
            vkGetPhysicalDeviceSparseImageFormatProperties(dev.physical, x.format, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT,
                                                           u, VK_IMAGE_TILING_OPTIMAL, &np, nullptr);
            x.sparse = np > 0;
        }
        // (RX_VK_TEX_SKIP_MB: from how many MB as RGBA8 a decoded ASTC texture wider than max_dim loses its top levels;
        // 8 on Minimum with a small driver budget, otherwise 64. A 3072-square image is about 48 MB decoded:
        // below 64, and too wide for the BC3 scratch. Explicit overrides keep main's existing parsing.)
        static const char* skip_env = getenv("RX_VK_TEX_SKIP_MB");
        const uint64_t skip_from = (uint64_t)(skip_env ? atoi(skip_env) : small_mips ? 8 : 64) << 20;
        while (!x.sparse && astc && x.skip + 1 < x.levels && bytes_at(x.skip) > skip_from &&
               (std::max(x.width, x.height) >> x.skip) > max_dim)
            x.skip++;
        while (!x.sparse && x.skip + 1 < x.levels && bytes_at(x.skip) > max_bytes) x.skip++;
        if (x.skip) {
            static int logged;
            if (logged++ < 40)
                logf("vk: texture %#llx %ux%ux%u x%u (NVN %#x, %llu MB): its %u top level(s) left out (%llu MB)",
                     (unsigned long long)t->gpu, x.width, x.height, x.depth, x.layers, d.format,
                     (unsigned long long)(bytes_at(0) >> 20), x.skip, (unsigned long long)(bytes_at(x.skip) >> 20));
            x.width = std::max(1u, x.width >> x.skip), x.height = std::max(1u, x.height >> x.skip);
            x.depth = std::max(1u, x.depth >> x.skip);
            x.levels -= x.skip;
        }
    }
    if (!texture_size_ok(x.type, x.width, x.height, x.depth, x.layers, d.samples)) {
        static int logged;
        if (logged++ < 16)
            logf("vk: texture %#llx %ux%ux%u x%u samples %d (NVN %#x): larger than this GPU's images, not made",
                 (unsigned long long)t->gpu, x.width, x.height, x.depth, x.layers, d.samples, d.format);
        return nullptr;
    }
    x.samples = d.samples > 1 ? (VkSampleCountFlagBits)d.samples : VK_SAMPLE_COUNT_1_BIT;
    {   // (decoded ASTC as BC3: 2D, at most the scratch's size; from blocks of 6 x 6 texels up (2.25 bits a texel or
        // less: BC3's 8 keep what they have) from 2 MB as RGBA8 (RX_VK_ASTC_BC_MB), the finer ones (UI, characters)
        // only from 64 MB)
        static const uint64_t low = (uint64_t)(getenv("RX_VK_ASTC_BC_MB") ? atoi(getenv("RX_VK_ASTC_BC_MB")) : 1) << 20;
        // (on a card the driver gives under 6 GB, the finer ones too: decoded ASTC drawn every frame can pass a 4 GB card's
        // allowance alone (1.75 GB in Goron City), and it pages at a few fps. As BC3, a quarter. 8 GB cards and integrated
        // GPUs too: astc_bc_policy.h. RX_VK_ASTC_BC_ALL=1 or 0 says so on any card)
        static const bool all = [] {
            if (const char* e = getenv("RX_VK_ASTC_BC_ALL")) return e[0] == '1';
            uint64_t used = 0, allowed = 0;
            if (!vram_usage(&used, &allowed)) allowed = 0;
            const bool integrated = dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
            const bool small = astc_bc_policy::all(allowed, integrated);
            if (small)
                logf("vk: %llu MB of VRAM allowed%s: all large ASTC textures kept as BC3", (unsigned long long)(allowed >> 20),
                     integrated ? ", an integrated GPU" : "");
            return small;
        }();
        const uint64_t min_bytes = (bw * bh >= 36 || all) ? low : 64ull << 20;
        uint64_t px = 0;
        for (uint32_t l = 0; l < x.levels; l++)
            px += (uint64_t)std::max(1u, x.width >> l) * std::max(1u, x.height >> l);
        // (not one with its top levels left out and a single layer: copies into those levels are blitted into its
        // first, which BC3 cannot take; trimmed, it is small)
        // (longshot-amd) AMD's drivers sample BC levels wrong when a level's block grid is not level 0's halved: 1365 texels
        // are 342 blocks, but level 2 (341 texels) is 86 blocks, not 342 >> 2 = 85. Ocarina's terrain material arrays
        // (1365 x 1365 x 90) then draw a grid of dark lines along the terrain's seams, worse in the distance. On AMD such a texture is encoded at the nearest size whose blocks halve
        // evenly down to its 16-texel levels (1365 -> 1536, 683 -> 768; 768 and 1536 stay) and keeps only the levels that
        // halve evenly: the game samples with normalized coordinates, so a resized level covers the same area. Where that
        // would grow it by more than a quarter, or past the scratch, it stays RGBA8. RX_VK_UNEVEN_RGBA8=1: RGBA8 instead of resizing.
        // Off by default: with it an RX 6700 XT's GPU time goes from 10-13 ms to 70-100 ms a frame in play, and AMD shows
        // rainbow ground. RX_VK_UNEVEN_BC=0: on (every card); unset or 1: BC3 without resizing.
        using bc_levels::first_uneven;
        using bc_levels::even_size;
        const bool uneven = first_uneven(x.width, x.height, x.levels) < x.levels;
        static const bool amd_fix = [] {
            if (const char* e = getenv("RX_VK_UNEVEN_BC")) return e[0] == '0';
            return false;
        }();
        static const bool rgba8_instead = env_on("RX_VK_UNEVEN_RGBA8");
        const bool bc_candidate = astc && !(x.skip && x.layers == 1) && x.type == VK_IMAGE_TYPE_2D &&
                                  x.samples == VK_SAMPLE_COUNT_1_BIT && x.width <= kBcDim && x.height <= kBcDim &&
                                  px * x.layers * 4 >= min_bytes;
        if (bc_candidate && uneven && amd_fix) {
            const uint32_t ew = even_size(x.width, x.levels), eh = even_size(x.height, x.levels);
            const uint32_t el = first_uneven(ew, eh, x.levels);
            const bool resize = ew != x.width || eh != x.height;
            static int n_resized, n_trimmed, n_rgba8;
            static uint64_t mb_saved;
            if (!rgba8_instead && ew <= kBcDim && eh <= kBcDim && ew * 4 <= x.width * 5 && eh * 4 <= x.height * 5 &&
                el >= 1 && bc_ready()) {
                uint64_t epx = 0;
                for (uint32_t l = 0; l < el; l++) epx += (uint64_t)std::max(1u, ew >> l) * std::max(1u, eh >> l);
                mb_saved += (px * x.layers * 4 - epx * x.layers) >> 20;
                int& n = resize ? n_resized : n_trimmed;
                n++;
                if (n <= 4 || (n & (n - 1)) == 0)
                    logf("vk: texture %#llx %ux%u x%u, %u levels: uneven BC block levels on AMD, as BC3 at %ux%u, %u levels "
                         "(%d resized, %d with fewer levels, %d as RGBA8; %llu MB less than RGBA8 so far)",
                         (unsigned long long)t->gpu, x.width, x.height, x.layers, x.levels, ew, eh, el, n_resized, n_trimmed,
                         n_rgba8, (unsigned long long)mb_saved);
                x.width = ew, x.height = eh, x.levels = el;
                x.bc = true, x.bc_resized = resize;
                x.format = bc7_enabled() ? VK_FORMAT_BC7_UNORM_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
            } else {
                n_rgba8++;
                if (n_rgba8 <= 4 || (n_rgba8 & (n_rgba8 - 1)) == 0)
                    logf("vk: texture %#llx %ux%u x%u, %u levels: uneven BC block levels on AMD, kept as RGBA8 (%d so far)",
                         (unsigned long long)t->gpu, x.width, x.height, x.layers, x.levels, n_rgba8);
            }
        } else if (bc_candidate && bc_ready()) {
            x.bc = true;
            x.format = bc7_enabled() ? VK_FORMAT_BC7_UNORM_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
        }
    }
    const VkFormatProperties fp = format_props(x.format);
    x.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) x.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageCreateFlags flags = 0;
    if (depth) {
        x.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    } else if (!is_bc(x.format) && !is_astc(x.format)) {
        // (any format of the class may be a view: attachments and storage where one of them can)
        flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        x.usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (x.samples == VK_SAMPLE_COUNT_1_BIT) x.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    } else {
        // Raw storage views address one compressed block per texel (8 or 16 bytes).
        flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT |
                 VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        const VkFormat raw = bpb == 8 ? VK_FORMAT_R32G32_UINT : VK_FORMAT_R32G32B32A32_UINT;
        VkImageFormatProperties limits{};
        // (asked without STORAGE: AMD's driver checks that against the compressed format and says no, though it makes
        // the image and its raw views (RX 6600 XT, amdvlk); without them the game's BC encoders wrote nowhere and
        // Link's clothes showed stale blocks. If the image is then refused, it is made again without)
        // (RX_VK_BC_STORAGE=0: compressed images without it, for an A/B on AMD, where the game's BC encoders running there is
        // the lead for GPU time of 60-100 ms a frame where 6-12 ms is expected)
        static const bool bc_storage = !getenv("RX_VK_BC_STORAGE") || atoi(getenv("RX_VK_BC_STORAGE")) != 0;
        if (bc_storage && (format_props(raw).optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) &&
            vkGetPhysicalDeviceImageFormatProperties(dev.physical, x.format, x.type, VK_IMAGE_TILING_OPTIMAL,
                x.usage, flags, &limits) == VK_SUCCESS)
            x.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        // Render to raw block views of 2D images. A 3D image's attachments use a separate uncompressed mip
        if (x.type != VK_IMAGE_TYPE_3D && (format_props(raw).optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
            const VkResult r = vkGetPhysicalDeviceImageFormatProperties(dev.physical, x.format, x.type, VK_IMAGE_TILING_OPTIMAL,
                x.usage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, flags, &limits);
            if (r == VK_SUCCESS) x.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            static int told;
            if (r != VK_SUCCESS && told++ < 4)
                logf("vk: BC image %ux%u (vk %d) as a raw colour attachment: the driver says %d", x.width, x.height,
                     (int)x.format, (int)r);
        }
    }
    if (x.type == VK_IMAGE_TYPE_2D && x.layers >= 6 && x.width == x.height && x.samples == VK_SAMPLE_COUNT_1_BIT)
        flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, x.cube = true;
    if (x.type == VK_IMAGE_TYPE_3D) {
        flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
        if (dev.view_2d_of_3d) flags |= VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT;
    }
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = flags;
    ci.imageType = x.type;
    ci.format = x.format;
    ci.extent = {x.width, x.height, x.depth};
    ci.mipLevels = x.levels;
    ci.arrayLayers = x.layers;
    ci.samples = x.samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = x.usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (output_probe) {
        if (!x.sparse && create_image(ci, x.image, x.allocation, false, as_target))
            *output_probe = {x.image, x.allocation, ci};
        return nullptr;   // (no commands, tracking, or texture cache entry for this image)
    }
    // (a render target's image, not compressed: from the pool when one of this create info waits there, above)
    const uint64_t pool_key = as_target && !x.sparse && pool_cap() && !is_bc(x.format) && !is_astc(x.format) &&
                                      (x.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
                                  ? pool_key_of(ci)
                                  : 0;
    x.pool_key = pool_key;
    const uint64_t reused_before = st_pool_reused, resized_before = st_pool_resized;
    x.create_flags = ci.flags;
    ImageNeed need{&ci};   // (a render target's requirements, asked once: the take by size and the size class read them)
    // (longshot, RX_VK_COMPACT) an image that is not a render target's: from the textures' own pool, the one compacted
    const bool in_textures = compact_on() && !x.sparse && !(as_target && !is_bc(x.format) && !is_astc(x.format));
    if (!x.sparse) take_output_trial_image(t, ci, x.image, x.allocation);
    const bool from_output_trial = x.image != VK_NULL_HANDLE;
    if (x.image) {
        // A detached trial image has the same creation parameters and still needs normal layout and cache registration.
    } else if (x.sparse) {
        ci.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
        // (the sparse flags and usage within the device's limits for them: extent, levels, layers, samples)
        const auto fits = [&] {
            VkImageFormatProperties ifp{};
            return vkGetPhysicalDeviceImageFormatProperties(dev.physical, ci.format, ci.imageType, ci.tiling, ci.usage,
                                                            ci.flags, &ifp) == VK_SUCCESS &&
                   ci.extent.width <= ifp.maxExtent.width && ci.extent.height <= ifp.maxExtent.height &&
                   ci.extent.depth <= ifp.maxExtent.depth && ci.mipLevels <= ifp.maxMipLevels &&
                   ci.arrayLayers <= ifp.maxArrayLayers && (ci.samples & ifp.sampleCounts);
        };
        // (a compressed one the driver refuses sparse with its raw attachment usage, or then its raw storage usage, the
        // residency query above did not ask about: asked again without it, as a compressed image refused is made below;
        // its raw views are then not made)
        bool ok = fits() ||
                  ((ci.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && is_bc(x.format) &&
                   (ci.usage &= ~VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, x.usage = ci.usage, fits())) ||
                  ((ci.usage & VK_IMAGE_USAGE_STORAGE_BIT) && is_bc(x.format) &&
                   (ci.usage &= ~VK_IMAGE_USAGE_STORAGE_BIT, x.usage = ci.usage, fits()));
        ok = ok && vkCreateImage(dev.device, &ci, nullptr, &x.image) == VK_SUCCESS;
        if (ok) {
            VkMemoryRequirements mr{};
            vkGetImageMemoryRequirements(dev.device, x.image, &mr);
            uint32_t n = 0;
            vkGetImageSparseMemoryRequirements(dev.device, x.image, &n, nullptr);
            std::vector<VkSparseImageMemoryRequirements> reqs(n);
            vkGetImageSparseMemoryRequirements(dev.device, x.image, &n, reqs.data());
            ok = false;
            for (const auto& r : reqs)
                if (r.formatProperties.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) x.sparse_req = r, ok = true;
            x.sparse_align = mr.alignment;
            x.sparse_type = UINT32_MAX;
            for (uint32_t i = 0; i < dev.memory.memoryTypeCount && x.sparse_type == UINT32_MAX; i++)
                if ((mr.memoryTypeBits >> i & 1) && (dev.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                    x.sparse_type = i;
            ok = ok && x.sparse_type != UINT32_MAX && x.sparse_align;
            if (!ok) vkDestroyImage(dev.device, x.image, nullptr), x.image = VK_NULL_HANDLE;
        }
        if (!ok) {
            logf("vk: sparse image %ux%u x%u levels %u format %d not made", x.width, x.height, x.layers, x.levels,
                 (int)x.format);
            return nullptr;
        }
        x.tiles.resize((size_t)x.layers * x.levels);
        x.tail_bound.assign(x.layers, 0);
        logf("vk: texture %#llx %ux%u x%u levels %u (NVN %#x): sparse, layers made resident as they are written "
             "(blocks %ux%u, mip tail from level %u, %llu KB%s)",
             (unsigned long long)t->gpu, x.width, x.height, x.layers, x.levels, d.format,
             x.sparse_req.formatProperties.imageGranularity.width, x.sparse_req.formatProperties.imageGranularity.height,
             x.sparse_req.imageMipTailFirstLod, (unsigned long long)(x.sparse_req.imageMipTailSize >> 10),
             x.sparse_req.formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT ? ", one for all layers" : " a layer");
    } else if (in_textures && create_image(ci, x.image, x.allocation, false, false, true)) {
        // (made in the textures' pool; one refused there is made as any other, below)
    } else if (!(pool_key && (pool_take(pool_key, x.image, x.allocation, x.views) || pool_take_memory(need, x.image, x.allocation))) &&
               // (longshot, RX_VK_TARGET_REUSE_MEMORY) one of the pooled kind on an allocation of its size class, so the next of
               // a slightly different size takes it: not while the pool is pressed (halved or given back), where its room costs most)
               !(pool_key && reuse_memory_level() >= 2 && pool_pressure == 0 &&
                 create_image_in_class(need, x.image, x.allocation)) &&
               !create_image(ci, x.image, x.allocation, false, as_target && !is_bc(x.format) && !is_astc(x.format), false, &need) &&
               // (a compressed image refused: made again without its raw attachment usage, then without its raw storage
               // usage; its raw views are then not made)
               !((ci.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && is_bc(x.format) &&
                 (ci.usage &= ~VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, x.usage = ci.usage, create_image(ci, x.image, x.allocation))) &&
               !((ci.usage & VK_IMAGE_USAGE_STORAGE_BIT) && (is_bc(x.format) || is_astc(x.format)) &&
                 (ci.usage &= ~VK_IMAGE_USAGE_STORAGE_BIT, x.usage = ci.usage, create_image(ci, x.image, x.allocation)))) {
        static int logged;
        if (logged++ < 16)
            logf("vk: image %ux%ux%u x%u levels %u format %d (NVN %#x) not made", x.width, x.height, x.depth, x.layers,
                 x.levels, (int)x.format, d.format);
        return nullptr;
    }
    if (pool_key && x.image && !x.sparse && !in_textures && !from_output_trial &&
        st_pool_reused == reused_before && st_pool_resized == resized_before) st_pool_made++;
    // what it costs: whole blocks for native BC/ASTC and re-encoded BC3, texels for other images.
    // Re-encoded BC3 has 4x4, 16-byte blocks, not the source ASTC's block dimensions.
    const bool blocks = is_bc(x.format) || is_astc(x.format);
    const uint32_t unit_w = x.bc ? 4u : blocks ? (uint32_t)bw : 1u;
    const uint32_t unit_h = x.bc ? 4u : blocks ? (uint32_t)bh : 1u;
    uint64_t px = 0;
    for (uint32_t l = 0; l < x.levels; l++)
        px += texture_level_units(x.width >> l, x.height >> l, x.depth >> l, unit_w, unit_h);
    const uint32_t texel = x.bc ? 16 : astc ? 4 : std::max(1, from ? (from == 3 ? 4 : from == 6 ? 8 : 16) : bpb);
    x.bytes = x.sparse ? 0 : px * x.layers * texel * x.samples;   // (a sparse image's: as its layers are bound)
    // (the budget counts what the allocator gave it: the texels' 8% short of that over a scene's 2000 textures, 3228 MB counted for 3504
    // MB held, and a budget of 4035 MB took 4300)
    if (!x.sparse) x.pad = std::max(x.bytes, allocation_size(x.allocation)) - x.bytes;
    total_bytes += x.bytes + x.pad;
    // into GENERAL, and cleared (a target the game renders into before writing it all reads zeros, not garbage)
    VkCommandBuffer cmd = upload_cmd();
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = x.image;
    b.subresourceRange = {x.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    if (depth) {
        const VkClearDepthStencilValue z{0.0f, 0};
        vkCmdClearDepthStencilImage(cmd, x.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &b.subresourceRange);
    } else if (!is_bc(x.format) && !is_astc(x.format)) {
        const VkClearColorValue z{};
        vkCmdClearColorImage(cmd, x.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &b.subresourceRange);
    }
    if (!is_bc(x.format) && !is_astc(x.format)) {
        // (the clear finished before anything after it in this command buffer writes the image: the upload of its contents
        // comes right after, a copy into the same image. Two transfer writes with no barrier between them may run in either
        // order; NVIDIA's ran in order, AMD's (a fast clear) landed last, so a texture uploaded in the recording that made it
        // read zeros there. The menus' text is that case: the CPU writes each string's letters into a new R8 texture, which is
        // drawn once into the string's label, so every menu and title text was blank on AMD cards)
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier2(cmd, &di);
    }
    Texture* p = tex.get();
    static uint32_t ids;
    p->id = ++ids;
    p->first_seen = frame_no;
    st_made++;
    {
        const int k = depth ? 1 : (is_bc(x.format) || is_astc(x.format) || astc) ? 2 : (x.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ? 0 : 3;
        st_made_kind[k]++, st_made_kind_bytes[k] += x.bytes;
    }
    p->key_layout = key.layout, p->key_cls = key.cls;
    {   // (another image at this storage: what tells them apart; content one renders the other does not see)
        static int told;
        auto [a, e] = by_gpu.equal_range(p->gpu);
        for (auto it = a; it != e && told < 80; ++it) {
            const Texture* o = it->second;
            told++;
            logf("vk: texture #%u at %#llx beside #%u: %ux%ux%u x%u levels %u layout %#x class %#x NVN %#x target %d, "
                 "the other %ux%ux%u x%u levels %u layout %#x class %#x NVN %#x target %d%s",
                 p->id, (unsigned long long)p->gpu, o->id, p->width, p->height, p->depth, p->layers, p->levels,
                 key.layout, key.cls, d.format, d.target, o->width, o->height, o->depth, o->layers, o->levels,
                 o->key_layout, o->key_cls, o->nvn_format, o->d.target, o->gpu_written ? " (rendered)" : "");
        }
    }
    textures.emplace(key, std::move(tex));
    by_gpu.emplace(p->gpu, p);
    {
        std::lock_guard lk(gpu_wrote_mu);
        gpu_wrote.watch(p->id, p->gpu, p->gpu + p->guest_size);
    }
    watch_tex(p, "made");
    if (remade_log_on()) {   // (one evicted not long ago, made again)
        std::lock_guard lk(evicted_mu);
        auto ev = evicted_at.find(evicted_key(p->gpu, p->width, p->height, p->nvn_format));
        if (ev != evicted_at.end()) {
            const uint64_t ago = frame_no - ev->second.frame;
            if (ago < 600) {
                remade[p] = ago;
                static int told;
                if (told++ < 300)
                    logf("vk: texture #%u %#llx %ux%u NVN %#x made again %llu frames after it was evicted (%s then)", p->id,
                         (unsigned long long)p->gpu, p->width, p->height, p->nvn_format, (unsigned long long)ago,
                         ev->second.rendered ? "rendered" : "not rendered");
            }
            evicted_at.erase(ev);
        }
    }
    static int logged;
    if (logged++ < 40 || textures.size() % 500 == 0 || p->bytes > (64ull << 20))
        logf("vk: texture %#llx %ux%ux%u x%u levels %u NVN %#x -> vk %d%s (%zu textures, %llu MB)",
             (unsigned long long)p->gpu, p->width, p->height, p->depth, p->layers, p->levels, d.format, (int)p->format,
             astc ? " (ASTC decoded)" : "", textures.size(), (unsigned long long)(total_bytes >> 20));
    return p;
}

} // namespace

thread_local uint64_t work_epoch = 0;   // (vk_work.h: the recording thread's own)
thread_local bool other_work_since_barrier = true;   // (vk_work.h; true at the start: nothing known)
namespace {
thread_local uint64_t last_barrier_epoch = 0;
std::atomic<int64_t> st_barriers_made{0}, st_barriers_left_out{0};
} // namespace
void barrier_stats(int64_t* made, int64_t* left_out) {
    *made = st_barriers_made.load(std::memory_order_relaxed), *left_out = st_barriers_left_out.load(std::memory_order_relaxed);
}

// (longshot) A full barrier right after another one on the same command buffer, no command that does or orders GPU work in between
// (vk_work.h counts them), orders nothing the first did not: left out. The game's own barrier (end of a pass), then the next pass's
// start barrier, then a dispatch's, were two or three in a row. RX_VK_BARRIER_DEDUPE_OFF=1: all recorded, as before.
void full_barrier(VkCommandBuffer cmd, const char* why) {
    static const bool off = env_on("RX_VK_BARRIER_DEDUPE_OFF");
    const bool dedupe = !off;   // (the count and the last barrier are the recording thread's own: vk_work.h)
    if (dedupe && cmd == last_barrier_cmd && work_epoch == last_barrier_epoch) {
        st_barriers_left_out.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    st_barriers_made.fetch_add(1, std::memory_order_relaxed);
    GprofSpan gp(cmd, why);   // (RX_VK_PASS_TIMING: what a barrier costs the GPU that has nothing left to wait for)
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
    last_barrier_cmd = cmd, last_barrier_epoch = work_epoch;
    other_work_since_barrier = false;
}

namespace {
std::atomic<int64_t> st_pass_relaxed{0};
} // namespace
int64_t pass_barriers_relaxed() { return st_pass_relaxed.load(std::memory_order_relaxed); }
// (longshot) The barrier a draw pass starts with. Full (ALL_COMMANDS to ALL_COMMANDS) where anything but draw passes was
// recorded on cmd since this thread's last full barrier (vk_work.h note_other_work: a dispatch, a copy, a fill, a blit, a
// whole image's clear, an upscaler library's dispatches), or where the last full barrier was on another command buffer: what
// they wrote may be read by any stage of the pass, its vertex fetch and vertex shaders included. Otherwise only draw passes
// came since: every write before it is made visible to this pass's fragment-side stages (the source stays every stage, so
// nothing written before is missed), and its vertex, task and mesh stages start while the passes before it finish, as on the
// console's GPU, which orders a pass's fragments, not its geometry, after the pass before. A pass whose geometry stages read
// what an earlier pass's fragments wrote in the same stretch has a barrier of the game's between (NOP_BARRIER: full). Left out
// entirely where nothing was recorded since the last full barrier on cmd (full_barrier's dedupe). RX_VK_PASS_BARRIER=full:
// always the full barrier (a test)
void pass_start_barrier(VkCommandBuffer cmd) {
    static const bool always_full = [] {
        const char* e = getenv("RX_VK_PASS_BARRIER");
        const bool on = e && !strcmp(e, "full");
        if (on) logf("vk: RX_VK_PASS_BARRIER=full: every draw pass starts with a full barrier");
        return on;
    }();
    if (always_full || other_work_since_barrier || cmd != last_barrier_cmd || work_epoch == last_barrier_epoch) {
        full_barrier(cmd, "barrier (pass start)");
        return;
    }
    st_pass_relaxed.fetch_add(1, std::memory_order_relaxed);
    GprofSpan gp(cmd, "barrier (pass start, fragments)");
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
    // (cmd stays the last full barrier's: the next pass is relaxed too while only draw passes follow; its epoch moves on, so a
    // full barrier after this one is not taken for a repeat of the last full one)
}

void texture_begin_recording(VkCommandBuffer upload, uint64_t value) {
    last_barrier_cmd = VK_NULL_HANDLE;   // (a new recording: its command buffers start afresh, whatever their handles held)
    up_cmd = upload;
    if (value != rec_value) epoch++;   // (a submission's parts check a texture's memory once: nothing is collected between)
    rec_value = value;
    up_used = false;
    up_astc_texels = 0;
    memory_table_begin();
    memory_rb_recording_begun();   // (which readback pages its uploads may refresh: memory_rb_refresh)
}
VkCommandBuffer upload_command_buffer() {
    return upload_cmd();
}
bool texture_uploads_recorded() {
    if (memory_rb_refresh(up_cmd)) up_used = true;   // (readback pages from guest memory, last of the uploads)
    memory_table_flush();   // (vk_memory.cpp: the page table's changes, after this recording's mirror copies)
    if (up_used) full_barrier(up_cmd);   // (before the recording that reads them)
    return up_used;
}

int view_nvn_format(const nvn_tex* t, const nvn_view* v) {
    return v && v->set_format ? v->format : t->d.format;
}
unsigned view_level(const nvn_view* v) {
    return v && v->set_levels ? v->base_level : 0;
}
unsigned image_level(const Texture* tex, unsigned level) {
    return level > tex->skip ? std::min(level - tex->skip, tex->levels - 1) : 0;
}
unsigned view_layer(const nvn_view* v) {
    return v && v->set_layers ? v->min_layer : 0;
}

OutputTrialBacking::~OutputTrialBacking() {
    for (const Image& owned : images) {
        if (owned.view) vkDestroyImageView(dev.device, owned.view, nullptr);
        if (owned.allocation) destroy_image(owned.image, owned.allocation);
    }
}

bool OutputTrialBacking::matches(uint64_t serial, VkImage image) const {
    for (const Image& owned : images)
        if (image && !owned.allocation && owned.image == image && owned.serial == serial) return true;
    return false;
}

std::unique_ptr<OutputTrialBacking> output_probe_backing(const nvn_tex* targets, bool fail_second,
                                                        unsigned& made, uint64_t& bytes) {
    std::unique_ptr<OutputTrialBacking> backing;
    {
        const NewNothrow nothrow;
        backing.reset(new (std::nothrow) OutputTrialBacking);
    }
    if (!backing) return {};
    for (unsigned i = 0; i < 3; ++i) {
        const nvn_tex& t = targets[i];
        if ((i == 1 && fail_second) || t.d.width != 1024 || t.d.height != 576 || t.d.target != NVN_TEXTURE_TARGET_2D ||
            t.d.format != NVN_FORMAT_RGBA8 || t.d.depth != 1 || t.d.levels != 1 || t.d.samples > 1 || !t.serial)
            return {};
        for (unsigned prior = 0; prior < i; ++prior)
            if (targets[prior].serial == t.serial) return {};
        const VkFormatInfo f = vk_format(t.d.format);
        if (f.format == VK_FORMAT_UNDEFINED) return {};
        auto& owned = backing->images[i];
        create(Key{}, &t, f.format, false, 1, 1, 4, true, &owned);
        if (!owned.image) return {};
        ++made;
        bytes += allocation_size(owned.allocation);
        if (!(owned.shape.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) return {};
        VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ci.image = owned.image;
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ci.format = owned.shape.format;
        ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const VkResult result = vkCreateImageView(dev.device, &ci, nullptr, &owned.view);
        if (result == VK_ERROR_DEVICE_LOST) device_lost();
        if (result != VK_SUCCESS) return {};
        owned.serial = t.serial;
        owned.gpu = t.gpu;
    }
    return backing;
}

namespace {
int burst_minimum() {
    static const int n = getenv("RX_VK_LOG_MADE_BURST") ? atoi(getenv("RX_VK_LOG_MADE_BURST")) : 0;
    return n;
}
uint64_t burst_count;
int bursts_logged;
std::vector<std::string> burst_lines;
void burst_frame_close() {
    static const uint64_t from = getenv("RX_VK_LOG_MADE_BURST_FROM") ? (uint64_t)atoll(getenv("RX_VK_LOG_MADE_BURST_FROM")) : 0;
    if (burst_minimum() > 0 && burst_count >= (uint64_t)burst_minimum() && frame_no >= from && bursts_logged < 40) {
        bursts_logged++;
        logf("vk: made burst: %llu images made in present %llu:", (unsigned long long)burst_count, (unsigned long long)frame_no);
        for (const auto& line : burst_lines) logf("vk: made burst:   %s", line.c_str());
    }
    burst_count = 0;
    burst_lines.clear();
}
}

Texture* texture_get(const nvn_tex* t, bool for_sampling) {
    PROF_HOT(PROF_TEXGET);
    if (!t || !t->gpu || t->d.target == NVN_TEXTURE_TARGET_BUFFER) return nullptr;
    const VkFormatInfo fi = vk_format(t->d.format);
    if (fi.format == VK_FORMAT_UNDEFINED) {
        static int seen[256];
        if (t->d.format >= 0 && t->d.format < 256 && seen[t->d.format]++ < 2)
            logf("vk: NVN format %#x has no Vulkan format: its textures are left out", t->d.format);
        return nullptr;
    }
    int bw = 1, bh = 1;
    const int bpb = nvn_block_size(t->d.format, &bw, &bh);
    if (bpb <= 0) return nullptr;
    Key k{};
    k.gpu = t->gpu;
    k.target = (uint32_t)t->d.target;
    // (a cube map and a 2D array of 6 layers over the same storage are one image; MS targets stay apart)
    if (k.target == NVN_TEXTURE_TARGET_CUBEMAP) k.target = NVN_TEXTURE_TARGET_2D_ARRAY;
    if (k.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) k.target = NVN_TEXTURE_TARGET_2D_ARRAY;
    if (k.target == NVN_TEXTURE_TARGET_RECTANGLE) k.target = NVN_TEXTURE_TARGET_2D;
    k.width = t->d.width, k.height = t->d.height;
    k.depth = t->d.target == NVN_TEXTURE_TARGET_CUBEMAP ? 6 : t->d.depth;
    if (k.target == NVN_TEXTURE_TARGET_2D) k.depth = 1;
    k.levels = std::max(1u, t->d.levels);
    k.samples = std::max(1, t->d.samples);
    k.layout = nvn_tex_is_linear(&t->d) ? 0x80000000u | (t->d.stride ? t->d.stride : host->tex_linear_stride(&t->d))
                                        : t->L.tileH | t->L.tileD << 4;
    k.cls = format_class(fi.format, t->d.format, bpb, fi.astc);
    Texture* tex;
    auto it = textures.find(k);
    if (it == textures.end()) {
        const bool collect = burst_minimum() > 0 && bursts_logged < 40 && burst_lines.size() < 160;
        const uint64_t pool_reused0 = st_pool_reused, pool_resized0 = st_pool_resized, pool_made0 = st_pool_made;
        const auto create_t0 = collect ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        tex = create(k, t, fi.format, fi.astc, bw, bh, bpb, !for_sampling);
        if (!tex) return nullptr;
        if (burst_minimum() > 0 && st_pool_reused == pool_reused0) burst_count++;
        if (collect) {
            char line[240];
            snprintf(line, sizeof line, "#%u %#llx %ux%ux%u x%u levels %u NVN %#x %s, %s, %.3f ms", tex->id, (unsigned long long)tex->gpu,
                     t->d.width, t->d.height, t->d.depth, tex->layers, t->d.levels, t->d.format, for_sampling ? "sampled" : "rendered",
                     st_pool_reused != pool_reused0 ? "exact pool reuse" : st_pool_resized != pool_resized0 ? "new image on pooled memory" : st_pool_made != pool_made0 ? "new pool allocation" : "new (not pooled)",
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - create_t0).count());
            burst_lines.push_back(line);
        }
        static const int64_t log_from = getenv("RX_VK_LOG_MADE") ? atoll(getenv("RX_VK_LOG_MADE")) : -1;
        static int made_logged;
        if (log_from >= 0 && (int64_t)frame_no >= log_from && made_logged++ < 300) {
            auto [a, e] = by_gpu.equal_range(tex->gpu);
            int others = 0;
            for (auto it = a; it != e; ++it) others += it->second != tex;
            logf("vk: made #%u %#llx %ux%ux%u x%u levels %u NVN %#x target %d tileH %u, first %s; %d other images at it",
                 tex->id, (unsigned long long)tex->gpu, t->d.width, t->d.height, t->d.depth, tex->layers, t->d.levels,
                 t->d.format, t->d.target, t->L.tileH, for_sampling ? "sampled" : "rendered", others);
        }
    } else {
        tex = it->second.get();
    }
    return texture_touch(tex, for_sampling);
}

uint64_t texture_destroy_generation() {
    return destroy_gen;
}
bool texture_still_resolved(uint32_t id, uint64_t since, uint64_t now) {   // (RX_VK_EXACT_STALE; the log by gone_note)
    if (!id || now - since >= kGoneLog / 2) return false;   // (nothing resolved; or further back than the log holds)
    for (uint64_t g = since + 1; g <= now; g++) {
        const uint32_t went = gone_log[g & (kGoneLog - 1)].load(std::memory_order_acquire);
        if (went == id || went == kGoneAll) return false;
    }
    return true;
}

bool texture_alive(uint64_t gpu, const Texture* t, VkImage image) {
    auto [a, b] = by_gpu.equal_range(gpu);
    for (auto it = a; it != b; ++it)
        if (it->second == t) return t->image == image;
    return false;
}

uint32_t texture_epoch() {
    return epoch;
}

thread_local bool upload_defer_ok = false;
namespace {
/// (longshot) A new ASTC texture a draw samples, met after this frame's uploads took their budget: not uploaded now, the draw
/// samples the placeholder, and the next frame's first look at it uploads it. An area's load makes 300-800 ASTC uploads in one
/// frame, 200-600 ms of the replay thread (the "a frame's uploads took" lines): spread over several frames, the textures come
/// in a few frames late. ASTC only: those are the materials' colour, normal and mask textures; uncompressed
/// ones can be data (height maps, look-up tables) that a frame without would get wrong. RX_VK_UPLOAD_BUDGET_MS=<n>: the
/// budget (40: only an area load's burst; at 8 the ground's streamed textures wait a frame in ordinary play and the ground
/// flickers), 0: no deferral
bool upload_defer(const Texture& tex) {
    // Terrain materials (layer arrays > 1 or very large surfaces) must always upload synchronously
    // to prevent ground flickering. Only defer smaller single-layer NPC/character/prop textures.
    if (upload_never_put_off(tex)) return false;

    // Limit NPC/prop ASTC upload stalls to 12 ms per frame to protect 60 fps frame pacing (16.6 ms target)
    static const int64_t budget_ns = (int64_t)(getenv("RX_VK_UPLOAD_BUDGET_MS") ? atof(getenv("RX_VK_UPLOAD_BUDGET_MS")) : 12.0) * 1000000;
    if (!budget_ns || !upload_defer_ok || tex.uploaded || !tex.astc || tex.gpu_written) return false;
    if (frame_upload_ns.load(std::memory_order_relaxed) < budget_ns) return false;
    st_deferred.fetch_add(1, std::memory_order_relaxed), st_deferred_frame.fetch_add(1, std::memory_order_relaxed);
    return true;
}
}  // namespace

// ---- (longshot, smoothness) textures prepared ahead of their first draw: texture_ahead.h says why and holds the rules ----
// RX_VK_TEX_AHEAD=1: on (off without it: measured on a Steam Deck only, after a save is loaded). RX_VK_TEX_AHEAD_MS=<n>: what a
// light frame may spend preparing (12 ms). The list of textures drawn before: the game folder's cache/longshot_textures_drawn.bin
namespace {
bool ahead_on() {
    static const bool on = [] {
        const char* e = getenv("RX_VK_TEX_AHEAD");
        return e && atoi(e) != 0;
    }();
    return on;
}
std::mutex ahead_mu;   // (the host's threads announce textures, the replay's prepare them: the queue, the list, the counts)
texture_ahead::Queue<nvn_tex> ahead_queue;
std::unordered_set<uint64_t> ahead_drawn;   // (the signatures of the textures a draw has sampled: this run's and the file's)
bool ahead_loaded = false, ahead_dirty = false;
std::atomic<int64_t> ahead_allow_ns{0}, ahead_spent_ns{0};   // (this frame's allowance and what the replay has spent of it)
uint64_t ahead_budget_bytes = 0;                  // (the cache's budget as texture_frame last had it; 0: not known yet, no room)
struct AheadCounts {
    int64_t prepared, bytes, ns, not_drawn_before, learnt, late, not_soon, stale, no_room;
} ahead_n{};

int64_t ahead_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
/// Block formats (ASTC, BC: a material's textures, never a render target's storage) in the game's own memory (a virtual
/// pool's mapping moves)
bool ahead_wanted(const nvn_texdesc& d, uint64_t gpu, uint64_t size) {
    if (!gpu || gpu >= (1ull << 39) || !size) return false;
    int bw = 1, bh = 1;
    return nvn_block_size(d.format, &bw, &bh) > 0 && (bw > 1 || bh > 1);
}
/// The list's entry for a texture: what it is, and where the game put it (texture_ahead::place says why)
uint64_t ahead_signature(const nvn_texdesc& d, const nvn_texlayout& L, uint64_t gpu) {
    uint64_t avail = 0;
    const uint8_t* p = host->gpu_ptr(gpu, &avail);
    return texture_ahead::place(texture_ahead::signature((uint32_t)d.format, d.width, d.height, d.depth, d.levels, L.size,
                                                         p && avail >= L.size ? p : nullptr),
                                gpu);
}
void ahead_load() {   // (ahead_mu held)
    if (ahead_loaded) return;
    ahead_loaded = true;
    const bool ok = texture_ahead::load_drawn(cache_path("longshot_textures_drawn.bin").c_str(), &ahead_drawn);
    logf("vk: textures ahead: %zu textures drawn before are known%s (RX_VK_TEX_AHEAD)", ahead_drawn.size(),
         ok ? "" : " (no list yet: this run starts it)");
}
/// A first upload a draw asked for: the texture is remembered, for the next time the game makes it
void ahead_learn(const Texture& tex) {
    if (!ahead_on() || !ahead_wanted(tex.d, tex.gpu, tex.L.size)) return;
    std::lock_guard lk(ahead_mu);
    // (only one first drawn soon after the game made it: a load's. One met minutes later in play would be prepared at
    // every load from then on, for nothing most of the time)
    int64_t made = 0;
    if (!ahead_queue.made_at(tex.gpu, &made) || !texture_ahead::learnable(made, ahead_now_ms())) {
        ahead_n.not_soon++;
        return;
    }
    const uint64_t s = ahead_signature(tex.d, tex.L, tex.gpu);
    if (!s) return;
    ahead_load();
    if (texture_ahead::remember(&ahead_drawn, s))
        ahead_dirty = true, ahead_n.learnt++;
    else
        ahead_n.late++;   // (known, and its first draw came before it was prepared)
}
}  // namespace
void texture_ahead_made(const nvn_tex* t) {
    if (!ahead_on() || !t || !ahead_wanted(t->d, t->gpu, t->L.size)) return;
    std::lock_guard lk(ahead_mu);
    ahead_queue.made(t->gpu, ahead_now_ms(), *t);
}
void texture_ahead_gone(const nvn_tex* t) {
    if (!ahead_on() || !t) return;
    std::lock_guard lk(ahead_mu);
    ahead_queue.gone(t->gpu);
}
void texture_ahead_frame(int64_t draws) {
    if (!ahead_on()) return;
    static const int64_t budget_ns = (int64_t)((getenv("RX_VK_TEX_AHEAD_MS") ? atof(getenv("RX_VK_TEX_AHEAD_MS")) : 12.0) * 1e6);
    ahead_allow_ns = texture_ahead::allowance_ns(draws, budget_ns);
    ahead_spent_ns = 0;
    std::lock_guard lk(ahead_mu);
    ahead_n.stale += ahead_queue.trim(ahead_now_ms());
    static uint64_t frames = 0;
    if (++frames % 300) return;
    if (ahead_dirty) {
        ahead_dirty = false;
        if (!texture_ahead::save_drawn(cache_path("longshot_textures_drawn.bin").c_str(), ahead_drawn)) {
            static bool told = false;
            if (!told) told = true, logf("vk: textures ahead: the list of textures drawn could not be written (cache/)");
        }
    }
    static AheadCounts last{};
    if (std::memcmp(&last, &ahead_n, sizeof last)) {
        logf("vk: textures ahead: %lld prepared so far (%.0f MB of images, %.0f ms); a draw's first uploads: %lld new to the list, "
             "%lld known and not prepared in time, %lld drawn too long after they were made to be kept; made and never drawn "
             "before %lld, let go for their age %lld; %zu waiting, %zu in the list%s",
             (long long)ahead_n.prepared, ahead_n.bytes / 1048576.0, ahead_n.ns / 1e6, (long long)ahead_n.learnt,
             (long long)ahead_n.late, (long long)ahead_n.not_soon, (long long)ahead_n.not_drawn_before, (long long)ahead_n.stale,
             ahead_queue.waiting(),
             ahead_drawn.size(), ahead_n.no_room ? "; the cache had no room at times" : "");
        last = ahead_n;
    }
}
void texture_ahead_run() {
    if (!ahead_on() || ahead_spent_ns >= ahead_allow_ns) return;
    const auto t0 = std::chrono::steady_clock::now();
    const auto spent = [&] {
        return ahead_spent_ns +
               std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    };
    const int64_t now = ahead_now_ms();
    ahead_running = true;
    while (spent() < ahead_allow_ns) {
        nvn_tex t;
        bool known = false;
        {
            std::lock_guard lk(ahead_mu);
            ahead_load();
            if (!texture_ahead::room(total_bytes, ahead_budget_bytes)) {
                ahead_n.no_room++;
                break;
            }
            if (!ahead_queue.take(now, &t)) break;
            known = ahead_drawn.count(ahead_signature(t.d, t.L, t.gpu)) != 0;
            if (!known) ahead_n.not_drawn_before++;
        }
        if (!known) continue;
        // (as a draw's first use of it would: made, and uploaded from the game's memory, into this recording's uploads)
        const uint64_t bytes0 = total_bytes;
        if (texture_get(&t, true)) {
            std::lock_guard lk(ahead_mu);
            ahead_n.prepared++, ahead_n.bytes += (int64_t)(total_bytes - std::min(total_bytes, bytes0));
        }
    }
    ahead_running = false;
    const int64_t ns = spent() - ahead_spent_ns;
    ahead_spent_ns += ns;
    std::lock_guard lk(ahead_mu);
    ahead_n.ns += ns;
}
// ---- (TEST, local, exp/rewrite-track) RX_VK_LOG_REWRITES=1: which textures are uploaded again because the game wrote their
// memory, how often and how much, and for how many presents in a row; a block of the ten biggest every 600 presents.
// ----
namespace {
int rewrites_level() {
    static const int v = getenv("RX_VK_LOG_REWRITES") ? atoi(getenv("RX_VK_LOG_REWRITES")) : 0;
    return v;
}
struct RewriteStat {
    uint64_t gpu = 0, guest_size = 0;
    uint32_t id = 0, width = 0, height = 0, layers = 0, levels = 0;
    int nvn_format = 0;
    bool astc = false, drawn = false;
    uint64_t n = 0, bytes = 0, whole = 0, by_gpu_buffer = 0, presents = 0;   // (this window's)
    uint64_t total_n = 0, total_bytes = 0;
    uint64_t last_present = 0, run = 0, longest = 0;
};
std::mutex rewrites_mu;
std::map<uint32_t, RewriteStat> rewrites;
struct RewriteWindow {
    uint64_t first_n = 0, first_bytes = 0, again_n = 0, again_bytes = 0, untracked = 0, presents = 0;
} rewrites_win;

void rewrites_note(const Texture& tex, bool again, uint64_t bytes, bool whole, bool gpu_buffer) {
    if (!rewrites_level()) return;
    std::lock_guard lk(rewrites_mu);
    if (!again) {
        rewrites_win.first_n++, rewrites_win.first_bytes += bytes;
        return;
    }
    rewrites_win.again_n++, rewrites_win.again_bytes += bytes;
    auto it = rewrites.find(tex.id);
    if (it == rewrites.end()) {
        // Keep diagnostics bounded even in scenes that continually create textures.
        if (rewrites.size() >= 4096) { rewrites_win.untracked++; return; }
        it = rewrites.emplace(tex.id, RewriteStat{}).first;
    }
    RewriteStat& s = it->second;
    if (!s.total_n)
        s.gpu = tex.gpu, s.guest_size = tex.guest_size, s.width = tex.width, s.height = tex.height, s.layers = tex.layers,
        s.levels = tex.levels, s.nvn_format = tex.nvn_format, s.astc = tex.astc;
    s.id = tex.id, s.drawn = s.drawn || tex.gpu_written;
    s.n++, s.bytes += bytes, s.total_n++, s.total_bytes += bytes;
    s.whole += whole, s.by_gpu_buffer += gpu_buffer;
    if (s.last_present != frame_no) {
        s.presents++;
        s.run = s.last_present + 1 == frame_no ? s.run + 1 : 1;
        s.longest = std::max(s.longest, s.run);
        s.last_present = frame_no;
    }
}
void rewrites_frame() {   // (once a texture frame)
    if (!rewrites_level()) return;
    std::lock_guard lk(rewrites_mu);
    if (++rewrites_win.presents < 600) return;
    std::vector<const RewriteStat*> top;
    for (const auto& kv : rewrites)
        if (kv.second.n) top.push_back(&kv.second);
    std::sort(top.begin(), top.end(), [](const RewriteStat* a, const RewriteStat* b) { return a->bytes > b->bytes; });
    logf("vk: rewrites: presents %llu-%llu: %llu first uploads (%.1f MB), %llu uploads again of %zu tracked textures (%.1f MB), %llu rewrites without a retained entry",
         (unsigned long long)(frame_no - rewrites_win.presents), (unsigned long long)frame_no,
         (unsigned long long)rewrites_win.first_n, rewrites_win.first_bytes / 1048576.0, (unsigned long long)rewrites_win.again_n,
         top.size(), rewrites_win.again_bytes / 1048576.0, (unsigned long long)rewrites_win.untracked);
    for (size_t i = 0; i < top.size() && i < 10; i++) {
        const RewriteStat& s = *top[i];
        logf("vk: rewrites:   #%u %#llx %ux%u x%u levels %u NVN %#x%s%s (%llu KB): %llu times in %llu of 600 presents, %.1f MB, "
             "%llu whole, %llu from a GPU buffer, longest run %llu presents; "
             "%llu times so far",
             s.id, (unsigned long long)s.gpu, s.width, s.height, s.layers, s.levels, s.nvn_format, s.astc ? " ASTC" : "",
             s.drawn ? " (GPU-drawn)" : "", (unsigned long long)(s.guest_size >> 10), (unsigned long long)s.n,
             (unsigned long long)s.presents, s.bytes / 1048576.0, (unsigned long long)s.whole,
             (unsigned long long)s.by_gpu_buffer, (unsigned long long)s.longest, (unsigned long long)s.total_n);
    }
    for (auto it = rewrites.begin(); it != rewrites.end();) {
        RewriteStat& s = it->second;
        if (!s.n) { it = rewrites.erase(it); continue; }
        s.n = s.bytes = s.whole = s.by_gpu_buffer = s.presents = 0, s.longest = s.run;
        ++it;
    }
    rewrites_win = {};
}
}  // namespace

Texture* texture_touch(Texture* tex, bool for_sampling) {
    tex->last_frame = frame_no;
    tex->last_value = rec_value;
    if (tex->freed_listed || tex->reuse_retire) reuse_drop(*tex);
    if (!for_sampling) return tex;
    if (upload_defer(*tex)) return nullptr;
    // contents: from guest memory unless the GPU wrote the image (then it is the content, unless the texture was
    // finalized since and the CPU writes its memory again). What the CPU writes into it after the GPU's write is taken
    // all the same, those ranges alone, the rest staying the GPU's: Eden's texture cache takes a CPU write over the
    // GPU's contents, and MKW's font cache is cleared on the GPU with its glyphs written by the CPU as text appears
    // (the menus were missing every letter added later). Only whole pages of its storage: a write to another variable
    // sharing an edge page once re-uploaded garbage over a drawn LUT (the rainbow sky). Opt-in (RX_VK_CPU_OVER_GPU=1): transient render targets share memory the CPU writes for other things, and took that over their texels, and the menus' letters stayed missing with it.
    if (tex->gpu_written && !tex->released) {
        // (longshot) and a texture the GPU only cleared: the game's font cache, whose letters the CPU writes as text appears. Its menu text
        // went missing at random (Longshot issues 40 and 44), whenever the clear reached the image first
        static const bool take = env_on("RX_VK_CPU_OVER_GPU");
        if ((!take && !tex->only_cleared) || tex->checked_epoch == epoch) return tex;
        tex->checked_epoch = epoch;
        static thread_local Ranges cpu, inner;
        if (!texture_dirty(*tex, std::max(tex->upload_gen, tex->gpu_gen), cpu)) return tex;
        const uint64_t first = (0x1000 - (tex->gpu & 0xFFF)) & 0xFFF;
        const uint64_t last = ((tex->gpu + tex->guest_size) & ~0xFFFull) - tex->gpu;
        inner.clear();
        uint64_t bytes = 0;
        for (const auto& r : cpu) {
            const uint64_t a = std::max<uint64_t>(r.first, first), b = std::min<uint64_t>(r.second, last);
            if (a < b) inner.push_back({a, b}), bytes += b - a;
        }
        const uint64_t gen = memory_generation();
        if (!inner.empty() && upload(*tex, &inner)) {
            static int logged;
            if (logged++ < 20)
                logf("vk: texture %#llx %ux%u x%u NVN %#x: %llu KB the CPU wrote after the GPU taken", (unsigned long long)tex->gpu,
                     tex->width, tex->height, tex->layers, tex->nvn_format, (unsigned long long)(bytes >> 10));
        }
        tex->upload_gen = gen;
        return tex;
    }
    {   // (RX_VK_REUPLOAD_DIM=<w>x<h> (debugging): textures that size uploaded whole again every frame: is what a
        // sampled texture misses in guest memory, unseen by the write tracking, or not there at all?)
        static uint32_t rw = 0, rh = 0;
        static const bool parsed = [] {
            if (const char* e = getenv("RX_VK_REUPLOAD_DIM")) sscanf(e, "%ux%u", &rw, &rh);
            return true;
        }();
        (void)parsed;
        static thread_local uint64_t last;
        if (rw && tex->width == rw && tex->height == rh && tex->uploaded && tex->checked_epoch != epoch &&
            last != frame_no) {
            last = frame_no;
            tex->checked_epoch = epoch;
            upload(*tex, nullptr);
            return tex;
        }
    }
    if (tex->uploaded && tex->checked_epoch == epoch) return tex;
    tex->checked_epoch = epoch;
    // (longshot) an image whose last upload failed waits before the next try, before anything is checked or taken as seen:
    // a BC3 image's upload stages the whole ASTC source (some 200 MB for a terrain array), not to be repeated every
    // submission while the memory it needs is not there
    if (tex->retry_frame > frame_no) return tex;
    static thread_local Ranges dirty;
    // (a buffer the GPU wrote over its storage, done since: all of it again; the tiles of MKW's terrain are filled so
    // and stayed the flat placeholder they were made with)
    const uint64_t seen_before = tex->gpu_wrote_seen;   // (put back if the upload fails: the write is still to be taken)
    uint64_t gpu_dirty_found = tex->uploaded ? gpu_wrote_into(*tex) : 0;
    const uint64_t gpu_dirty = gpu_dirty_found;
    const bool partial = !gpu_dirty && tex->uploaded && !tex->gpu_written && !tex->released;
    // (a texture the GPU drew and the game finalized: only what the CPU wrote since then. Its memory never got the
    // GPU's texels, and a write before (another variable in the same page) re-uploaded garbage over a drawn LUT:
    // the rainbow sky)
    const uint64_t since = tex->gpu_written && tex->released ? std::max(tex->upload_gen, tex->release_gen) : tex->upload_gen;
    if (tex->gpu_written && tex->released && frame_no > tex->last_written + 60) {
        // (longshot: one the game finalized, sampled again well after it was drawn: what the eviction keeps them for)
        static std::unordered_set<uint32_t> told;
        if (told.size() < 200 && told.insert(tex->id).second)
            logf("vk: texture #%u %#llx %ux%u x%u NVN %#x the GPU drew, finalized by the game, sampled again at frame %llu (drawn at "
                 "%llu)", tex->id, (unsigned long long)tex->gpu, tex->width, tex->height, tex->layers, tex->nvn_format,
                 (unsigned long long)frame_no, (unsigned long long)tex->last_written);
    }
    if (tex->uploaded && !gpu_dirty && !texture_dirty(*tex, since, dirty)) return tex;
    const uint64_t gen = memory_generation();
    // (storage render targets took last, which the CPU has not written since: nothing to upload, the memory holds
    // nothing the GPU's images did not replace. The game's transient heaps move with the dynamic resolution, and a
    // render target sampled before it is rendered at its new address (the culling reads last frame's shadow map)
    // was unswizzled from that memory: 55 MB for the 3072 x 3072 x 3 shadow map, frame after frame. Not "storage
    // the write watch never saw written": the game fills memory before it makes a pool of it.)
    // (from Shakoquo/Mako ff479e988d) never for a block-compressed texture (ASTC, BC, ETC): the GPU cannot render into one, so
    // its texels come from memory whatever render targets used that storage before. Skipped, a boss or a new enemy met for
    // the first time (its textures loaded into the transient heaps' memory) was drawn with an empty texture, all blue, for
    // the whole cutscene
    const bool compressed = tex->astc || tex->bc ||
                            (tex->format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && tex->format <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK);
    uint64_t owned = !tex->uploaded && !compressed ? gpu_owned_since(tex->gpu, tex->guest_size) : 0;
    if (!tex->uploaded && compressed) {
        static std::atomic<int> told{0};
        if (told.load(std::memory_order_relaxed) < 30 && gpu_owned_since(tex->gpu, tex->guest_size) && told++ < 30)
            logf("vk: texture #%u %#llx %ux%u x%u NVN %#x (compressed) over storage render targets took: uploaded from the game's "
                 "memory all the same", tex->id, (unsigned long long)tex->gpu, tex->width, tex->height, tex->layers,
                 tex->nvn_format);
    }
    bool from_memory_taken = false;   // (put back if the upload fails)
    if (owned) {   // (one evicted that came from memory: uploaded from it again. Above)
        std::lock_guard lk(from_memory_mu);
        auto e = evicted_from_memory.find(evicted_key(tex->gpu, tex->width, tex->height, tex->nvn_format));
        if (e != evicted_from_memory.end() && e->second == tex->guest_size) {
            evicted_from_memory.erase(e);
            from_memory_taken = true;
            owned = 0;
            const uint64_t n = ++st_remade_uploaded;
            if (n <= 20 || n % 100 == 0)
                logf("vk: texture #%u %#llx %ux%u NVN %#x evicted, made again over storage render targets took: uploaded from "
                     "the game's memory (%llu so far)", tex->id, (unsigned long long)tex->gpu, tex->width, tex->height,
                     tex->nvn_format, (unsigned long long)n);
        }
    }
    watch_tex(tex, owned ? "sampled, over rendered storage" : "sampled, uploaded");
    if (owned && !texture_dirty(*tex, owned, dirty)) {
        tex->uploaded = true;
        tex->upload_gen = gen;
        watch_tex(tex, "NOT uploaded (over storage the GPU rendered)");
        if (remade_log_on()) {   // (made again after its eviction, and sampled empty)
            std::lock_guard lk(evicted_mu);
            auto r = remade.find(tex);
            if (r != remade.end()) {
                static int told;
                if (told++ < 300)
                    logf("vk: texture #%u %#llx %ux%u NVN %#x, made again %llu frames after its eviction, sampled EMPTY "
                         "(storage the GPU rendered, not uploaded) at frame %llu", tex->id, (unsigned long long)tex->gpu,
                         tex->width, tex->height, tex->nvn_format, (unsigned long long)r->second,
                         (unsigned long long)frame_no);
                remade.erase(r);
            }
        }
        return tex;
    }
    if (partial && env_on("RX_VK_LOG_PARTIAL")) {
        static int logged;
        if (logged++ < 200) {
            char line[512];
            int n = snprintf(line, sizeof line, "vk: partial upload %#llx %ux%ux%u x%u levels %u NVN %#x size %llu flags %#x:",
                             (unsigned long long)tex->gpu, tex->width, tex->height, tex->depth, tex->layers, tex->levels,
                             tex->nvn_format, (unsigned long long)tex->guest_size, tex->d.flags);
            for (size_t i = 0; i < dirty.size() && i < 8 && n < 480; i++)
                n += snprintf(line + n, sizeof line - n, " [%#llx,%#llx)", (unsigned long long)dirty[i].first,
                              (unsigned long long)dirty[i].second);
            logf("%s", line);
        }
    }
    if (!tex->uploaded && tex->gpu >= (1ull << 40) && tex->page_map.empty()) {
        // (a sparse texture's mapping as it is uploaded first: its first check after does not take every page for
        // remapped, which uploaded the course maps twice)
        static thread_local Ranges first;
        texture_dirty(*tex, 0, first);
    }
    // (a sparse image, first or whole: only the storage ever written, not all of its gigabytes)
    const bool sparse_written = tex->sparse && !partial;
    if (sparse_written) texture_dirty(*tex, 0, dirty);
    if (tex->gpu_written) {   // (an image the GPU drew, finalized and sampled again: from guest memory, 400 lines)
        static int logged;
        uint64_t kb = 0;
        for (const auto& r : dirty) kb += (r.second - r.first) >> 10;
        if (logged++ < 400)
            logf("vk: upload over a drawn image: #%u %#llx %ux%ux%u x%u NVN %#x at present %llu (texture frame %llu), %s, "
                 "%llu KB dirty in %zu ranges%s", tex->id, (unsigned long long)tex->gpu, tex->width, tex->height, tex->depth,
                 tex->layers, tex->nvn_format, (unsigned long long)(frame_no - 1), (unsigned long long)frame_no,
                 tex->released ? "finalized" : "live", (unsigned long long)kb, dirty.size(), gpu_dirty ? ", a GPU buffer wrote it" : "");
    }
    const auto up0 = std::chrono::steady_clock::now();
    const bool up_ok = upload(*tex, partial || sparse_written ? &dirty : nullptr);
    {   // (an upload over 15 ms: what it was, 60 of them)
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - up0).count();
        static int slow_logged;
        if (ms > 15.0) programs_slow_upload(ms, tex->width, tex->height, tex->nvn_format);   // (RX_VK_LOG_PROGRAM_LIFE=1)
        if (ms > 15.0 && slow_logged++ < 60) {
            uint64_t part = 0;
            if (partial) for (const auto& r : dirty) part += r.second - r.first;
            logf("vk: slow upload %.1f ms: #%u %#llx %ux%ux%u x%u levels %u NVN %#x%s%s, %llu of %llu KB%s", ms, tex->id,
                 (unsigned long long)tex->gpu, tex->width, tex->height, tex->depth, tex->layers, tex->levels,
                 tex->nvn_format, tex->astc ? " ASTC" : "",
                 tex->bc ? (linear_twin(tex->format) == VK_FORMAT_BC7_UNORM_BLOCK ? " (BC7)" : " (BC3)") : "",
                 (unsigned long long)((partial ? part : tex->guest_size) >> 10), (unsigned long long)(tex->guest_size >> 10),
                 tex->uploaded ? " again" : "");
        }
    }
    if (up_ok) {
        if (gpu_dirty) {
            tex->gpu_wrote_seen = gpu_dirty;
            static int logged;
            if (logged++ < 40)
                logf("vk: texture #%u %#llx %ux%u NVN %#x: the GPU wrote its storage (submission %llu): uploaded again (%s)",
                     tex->id, (unsigned long long)tex->gpu, tex->width, tex->height, tex->nvn_format,
                     (unsigned long long)gpu_dirty, (partial || sparse_written) ? "partial" : "whole");
        }
        // (its storage joins the submit-time capture: what the game writes into it after a submit is not taken for it)
        // (any size: the capture copies only the pages the CPU wrote, and the big CPU-streamed textures were read as the
        // game had rewritten them a frame or two later. RX_VK_TEX_CAPTURE_MAX_MB=<n>: only up to that, 8 before)
        static const uint64_t cap_max = getenv("RX_VK_TEX_CAPTURE_MAX_MB") ? (uint64_t)atoi(getenv("RX_VK_TEX_CAPTURE_MAX_MB")) << 20 : (2ull << 30);
        if (tex->gpu < (1ull << 40) && tex->guest_size < cap_max) memory_note_texture_upload(tex->gpu, tex->guest_size);
        if (verify_dim().first && tex->width == verify_dim().first && tex->height == verify_dim().second)
            tex->verify_hash = guest_hash(*tex, true);
        if (rewrites_level()) {   // (TEST, local: RX_VK_LOG_REWRITES)
            const bool ranged = partial || sparse_written;
            uint64_t part = 0;
            if (ranged) for (const auto& r : dirty) part += r.second - r.first;
            rewrites_note(*tex, tex->uploaded, ranged ? part : tex->guest_size, !ranged, gpu_dirty != 0);
        }
        if (tex->uploaded) {
            static int logged;
            static std::unordered_map<uint64_t, int> big;
            if (logged++ < 20 || (tex->guest_size > (4u << 20) && big[tex->gpu]++ < 3)) {
                uint64_t part = 0, nz = 0, readable = 0;
                for (const auto& r : dirty) part += r.second - r.first;
                if (partial) {   // (what the CPU wrote: how much of it is not zero, and in which levels)
                    std::vector<uint8_t> b;
                    char lv[96] = "";
                    int lvn = 0;
                    for (const auto& r : dirty) {
                        b.resize((size_t)(r.second - r.first));
                        if (!host->gpu_read(tex->gpu + r.first, b.data(), b.size())) continue;
                        readable += b.size();
                        for (uint8_t c : b) nz += c != 0;
                        for (uint32_t l = 0; l < tex->levels && lvn < 90; l++) {
                            const uint64_t lo = host->tex_level_offset(&tex->d, l);
                            const uint64_t hi = l + 1 < tex->levels ? host->tex_level_offset(&tex->d, l + 1) : tex->guest_size;
                            if (r.first < hi && r.second > lo) lvn += snprintf(lv + lvn, sizeof lv - lvn, " L%u", l);
                        }
                    }
                    logf("vk:   (its written part: %llu of %llu KB readable, %llu bytes not zero; levels%s)",
                         (unsigned long long)(readable >> 10), (unsigned long long)(part >> 10), (unsigned long long)nz, lv);
                }
                logf("vk: texture %#llx %ux%ux%u x%u levels %u NVN %#x (%llu KB of storage) re-uploaded (its memory was "
                     "written; flags %#x): %s, %llu KB of it%s%s", (unsigned long long)tex->gpu, tex->width, tex->height,
                     tex->depth, tex->layers, tex->levels, tex->nvn_format, (unsigned long long)(tex->guest_size >> 10),
                     tex->d.flags, gpu_dirty ? "a GPU write into its storage" : partial ? "what the CPU wrote" : "whole",
                     (unsigned long long)((partial || sparse_written ? part : tex->guest_size) >> 10),
                     tex->gpu_written ? ", over what the GPU wrote" : "", tex->released ? " (finalized since)" : "");
            }
        }
        tex->uploaded = true;
        tex->upload_gen = gen;
        tex->gpu_written = false;
        tex->released = false;
        tex->retry_frame = 0;
    } else if (upload_never(*tex)) {
        tex->uploaded = true;   // (nothing to try again)
        tex->upload_gen = gen;
    } else {
        // (longshot) a BC3 image holds nothing until its encode has run. Marked uploaded, its blocks as the memory had them
        // were drawn as coloured noise, and never made again unless the game wrote its memory: the path, the trees and Saria
        // after an alt-tab on a 4 GB RX 5500M, its VRAM full and the textures evicted and made again. The other kinds the same
        // way: a first upload that failed (no staging memory, the VRAM or RAM full) left an empty image, and one that failed
        // again dropped what the CPU or a GPU buffer had written since. Nothing is marked done: uploaded, what it was
        // uploaded after and the GPU write it found stay as they were, and it is tried again 30 frames on
        tex->retry_frame = frame_no + 30;
        tex->gpu_wrote_seen = seen_before;
        if (from_memory_taken) {   // (still one that came from memory: the next try has to find it so again)
            std::lock_guard lk(from_memory_mu);
            evicted_from_memory[evicted_key(tex->gpu, tex->width, tex->height, tex->nvn_format)] = tex->guest_size;
        }
        static std::atomic<int> told{0};
        if (told++ < 40) {
            if (tex->bc)
                logf("vk: BC3 texture #%u %#llx %ux%u x%u: its upload failed (%s): tried again in 30 frames", tex->id,
                     (unsigned long long)tex->gpu, tex->width, tex->height, tex->layers, astc_fail ? astc_fail : "unknown");
            else
                logf("vk: texture #%u %#llx %ux%u x%u NVN %#x: its upload failed: tried again in 30 frames", tex->id,
                     (unsigned long long)tex->gpu, tex->width, tex->height, tex->layers, tex->nvn_format);
        }
    }
    return tex;
}

VkImageView texture_view(Texture* tex, const nvn_tex* t, const nvn_view* v, VkImageViewType type, ViewUse use,
                         VkFormat* format_out, VkFormat force_format) {
    if (!tex) return VK_NULL_HANDLE;
    const int nf = view_nvn_format(t, v);
    const VkFormat f = view_format_of(tex->format, tex->astc, tex->bc, nf, use, force_format);
    if (f == VK_FORMAT_UNDEFINED) return VK_NULL_HANDLE;
    const bool depth = is_depth_format(tex->format);

    const uint32_t vl = view_level(v);
    uint32_t level0 = image_level(tex, vl);
    uint32_t nlevels = v && v->set_levels && v->nlevels ? (vl + v->nlevels > tex->skip ? vl + v->nlevels - tex->skip - level0 : 1)
                                                       : tex->levels - level0;
    nlevels = std::clamp<uint32_t>(nlevels, 1, tex->levels - level0);
    const uint32_t total_layers = tex->type == VK_IMAGE_TYPE_3D ? 1 : tex->layers;
    uint32_t layer0 = std::min(view_layer(v), total_layers - 1);
    uint32_t nlayers = v && v->set_layers && v->nlayers ? v->nlayers : total_layers - layer0;
    nlayers = std::clamp<uint32_t>(nlayers, 1, total_layers - layer0);
    VkImageAspectFlags aspect = tex->aspect;
    if (use != ViewUse::Attachment && aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
        aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (use == ViewUse::Attachment) {
        nlevels = 1, nlayers = 1;
        type = tex->type == VK_IMAGE_TYPE_1D ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D;
        // (a slice of a 3D target: a 2D view of it, 2D_ARRAY_COMPATIBLE, the slice as its layer)
        if (tex->type == VK_IMAGE_TYPE_3D) layer0 = std::min(view_layer(v), std::max(1u, tex->depth >> level0) - 1);
    } else if (use == ViewUse::Storage) {
        nlevels = 1;
        if (tex->type == VK_IMAGE_TYPE_3D) type = VK_IMAGE_VIEW_TYPE_3D, layer0 = 0, nlayers = 1;
        else if (type == VK_IMAGE_VIEW_TYPE_2D && nlayers > 1) nlayers = 1;
        else if (type == VK_IMAGE_VIEW_TYPE_1D && nlayers > 1) nlayers = 1;
    }
    // the shader's type over the image's: what Vulkan allows of them
    switch (type) {
    case VK_IMAGE_VIEW_TYPE_1D: case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
        if (tex->type != VK_IMAGE_TYPE_1D) return VK_NULL_HANDLE;
        if (type == VK_IMAGE_VIEW_TYPE_1D) nlayers = 1;
        break;
    case VK_IMAGE_VIEW_TYPE_2D: case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
        if (tex->type == VK_IMAGE_TYPE_1D) return VK_NULL_HANDLE;
        if (tex->type == VK_IMAGE_TYPE_3D) {
            // (a 2D sample instruction over a 3D texture, as Eden's recompiler keeps the instruction's type: its first
            // slice, one level (VK_EXT_image_2d_view_of_3d). MKW reads a 64 x 64 x 64 texture so, which was sampled
            // as the dummy)
            if (use == ViewUse::Sample && dev.view_2d_of_3d) {
                type = VK_IMAGE_VIEW_TYPE_2D, nlevels = 1, layer0 = 0, nlayers = 1;
                break;
            }
            if (use != ViewUse::Attachment) return VK_NULL_HANDLE;
        }
        if (type == VK_IMAGE_VIEW_TYPE_2D) nlayers = 1;
        break;
    case VK_IMAGE_VIEW_TYPE_3D:
        if (tex->type != VK_IMAGE_TYPE_3D) return VK_NULL_HANDLE;
        layer0 = 0, nlayers = 1;
        break;
    case VK_IMAGE_VIEW_TYPE_CUBE: case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
        if (!tex->cube) return VK_NULL_HANDLE;
        if (type == VK_IMAGE_VIEW_TYPE_CUBE) nlayers = 6;
        else nlayers = std::max(6u, nlayers / 6 * 6);
        if (layer0 + nlayers > tex->layers) return VK_NULL_HANDLE;
        break;
    default: break;
    }
    if (tex->type == VK_IMAGE_TYPE_3D && use == ViewUse::Attachment) type = VK_IMAGE_VIEW_TYPE_2D;
    VkComponentMapping cm{};
    if (use == ViewUse::Sample) {
        const nvn_texdesc& d = t->d;
        unsigned sw[4] = {d.swizzle_r, d.swizzle_g, d.swizzle_b, d.swizzle_a};
        if (v && v->set_swizzle)
            for (int i = 0; i < 4; i++) sw[i] = v->swz[i];
        // (X formats read alpha as one)
        switch (nf) {
        case NVN_FORMAT_RGBX8: case NVN_FORMAT_RGBX8SN: case NVN_FORMAT_RGBX8UI: case NVN_FORMAT_RGBX8I:
        case NVN_FORMAT_RGBX16F: case NVN_FORMAT_RGBX16: case NVN_FORMAT_RGBX16SN: case NVN_FORMAT_RGBX16UI:
        case NVN_FORMAT_RGBX16I: case NVN_FORMAT_RGBX32F: case NVN_FORMAT_RGBX32UI: case NVN_FORMAT_RGBX32I:
        case NVN_FORMAT_RGBX8_SRGB: case NVN_FORMAT_BGRX8: case NVN_FORMAT_BGRX8_SRGB: case NVN_FORMAT_BGR5:
        case NVN_FORMAT_RGB5:
            for (unsigned& s : sw)
                if (s == NVN_TEXTURE_SWIZZLE_A) s = NVN_TEXTURE_SWIZZLE_ONE;
            break;
        default: break;
        }
        // (RGB5A1 and RGB5 have R in the low bits, their A1R5G5B5 image B)
        if (nf == NVN_FORMAT_RGB5A1 || nf == NVN_FORMAT_RGB5)
            for (unsigned& s : sw)
                s = s == NVN_TEXTURE_SWIZZLE_R ? NVN_TEXTURE_SWIZZLE_B : s == NVN_TEXTURE_SWIZZLE_B ? NVN_TEXTURE_SWIZZLE_R : s;
        static const bool swap_9e5 = env_on("RX_VK_9E5_SWAP");   // (longshot, diagnosis: R and B swapped)
        if (swap_9e5 && nf == NVN_FORMAT_RGB9E5F) std::swap(sw[0], sw[2]);
        cm = {component(sw[0]), component(sw[1]), component(sw[2]), component(sw[3])};
        static const bool log_swizzle = env_on("RX_VK_LOG_SWIZZLE");   // (longshot: RGB9E5 and R11G11B10 views)
        static int swizzle_logged;
        if (log_swizzle && nf == NVN_FORMAT_RGB9E5F && swizzle_logged++ < 12)
            logf("vk: sampled view of #%u NVN %#x as vk %d: swizzle %u %u %u %u (descriptor %u %u %u %u, view %d)", tex->id,
                 nf, (int)f, sw[0], sw[1], sw[2], sw[3], d.swizzle_r, d.swizzle_g, d.swizzle_b, d.swizzle_a,
                 v && v->set_swizzle ? 1 : 0);
    }
    uint64_t key = mix((uint64_t)use, (uint64_t)type);
    key = mix(key, (uint64_t)f << 32 | aspect);
    key = mix(key, (uint64_t)level0 << 48 | (uint64_t)nlevels << 32 | (uint64_t)layer0 << 16 | nlayers);
    key = mix(key, (uint64_t)cm.r << 24 | (uint64_t)cm.g << 16 | (uint64_t)cm.b << 8 | cm.a);
    if (format_out) *format_out = f;
    const VkImageUsageFlags usage = use == ViewUse::Sample ? VK_IMAGE_USAGE_SAMPLED_BIT
                                  : use == ViewUse::Storage ? VK_IMAGE_USAGE_STORAGE_BIT
                                  : depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                          : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    return cached_image_view(tex, key, [&](VkResult* result) {
        const VkImageView view = make_view(tex, type, f, aspect, level0, nlevels, layer0, nlayers, cm, usage, result);
        if (!view) {
            static int logged;
            if (logged++ < 16)
                logf("vk: view of texture %#llx (%ux%u, vk %d, usage %#x) as vk %d type %d usage %#x "
                     "levels %u+%u layers %u+%u not made",
                     (unsigned long long)tex->gpu, tex->width, tex->height, (int)tex->format, tex->usage,
                     (int)f, (int)type, usage, level0, nlevels, layer0, nlayers);
        }
        return view;
    });
}

TextureAttachment texture_attachment(Texture* tex, const nvn_tex* t, const nvn_view* v) {
    TextureAttachment out;
    if (tex->type != VK_IMAGE_TYPE_3D || !is_bc(tex->format)) {
        out.view = texture_view(tex, t, v, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Attachment, &out.format);
        return out;
    }

    // NVIDIA's Pascal driver reports support for attaching a 2D raw view of a BC volume, then loses the device
    // even on a clear. Render the encoded blocks into an uncompressed mip and copy them to the sampled volume.
    const int nf = view_nvn_format(t, v);
    const VkFormat format = view_format_of(tex->format, tex->astc, tex->bc, nf, ViewUse::Attachment, VK_FORMAT_UNDEFINED);
    int bw, bh, widened_from;
    int bytes = nvn_block_size(nf, &bw, &bh);
    widened(vk_format(nf).format, &widened_from);
    if (widened_from) bytes = widened_from * 4 / 3;
    if (format == VK_FORMAT_UNDEFINED || bw != 1 || bh != 1 || bytes != tex->bpb ||
        !(format_props(format).optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
        return out;

    const uint32_t level = image_level(tex, view_level(v));
    const uint32_t depth = std::max(1u, tex->depth >> level);
    const uint32_t slice = std::min(view_layer(v), depth - 1);
    const uint64_t key = mix(mix(0xbc3d, format), (uint64_t)level << 32 | slice);
    auto attachment = std::find_if(tex->block_attachments.begin(), tex->block_attachments.end(), [&](const auto& a) {
        return a.level == level && a.format == format;
    });
    if (attachment == tex->block_attachments.end()) {
        if (tex->view_failed_frame == frame_no) return out;
        // Each compressed mip can round up to a different block extent, so it has its own uncompressed image
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.flags = VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
        ci.imageType = VK_IMAGE_TYPE_3D;
        ci.format = format;
        ci.extent = {(std::max(1u, tex->width >> level) + 3) / 4, (std::max(1u, tex->height >> level) + 3) / 4, depth};
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        Texture::BlockAttachment made{VK_NULL_HANDLE, nullptr, format, level};
        if (!create_image(ci, made.image, made.allocation)) {
            tex->view_failed_frame = frame_no;
            return out;
        }
        try {
            tex->block_attachments.push_back(made);
        } catch (const std::exception&) {
            destroy_image(made.image, made.allocation);
            tex->view_failed_frame = frame_no;
            return out;
        }
        attachment = std::prev(tex->block_attachments.end());
        const uint64_t bytes = allocation_size(made.allocation);
        tex->pad += bytes;
        total_bytes += bytes;

        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = made.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(upload_cmd(), &dependency);
    }

    out.view = cached_image_view(tex, key, [&](VkResult* result) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = attachment->image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, slice, 1};
        VkImageView view = VK_NULL_HANDLE;
        *result = vkCreateImageView(dev.device, &vi, nullptr, &view);
        return *result == VK_SUCCESS ? view : VK_NULL_HANDLE;
    });
    out.format = format;
    out.blocks = attachment->image;
    return out;
}

void copy_block_attachment(VkCommandBuffer cmd, const Texture& tex, VkImage blocks, uint32_t level, uint32_t slice,
                           bool to_attachment) {
    const uint32_t width = std::max(1u, tex.width >> level), height = std::max(1u, tex.height >> level);
    const VkImageSubresourceLayers compressed{VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
    const VkImageSubresourceLayers raw{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    const VkOffset3D offset{0, 0, (int32_t)std::min(slice, std::max(1u, tex.depth >> level) - 1)};
    // Vulkan takes the extent in source texels, which are blocks in the uncompressed attachment
    const VkImageCopy region{to_attachment ? compressed : raw, offset, to_attachment ? raw : compressed, offset,
                             to_attachment ? VkExtent3D{width, height, 1} : VkExtent3D{(width + 3) / 4, (height + 3) / 4, 1}};
    vkCmdCopyImage(cmd, to_attachment ? tex.image : blocks, VK_IMAGE_LAYOUT_GENERAL,
                   to_attachment ? blocks : tex.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
}

bool texture_copy_into_bc(VkCommandBuffer cmd, Texture* src, uint32_t src_level, uint32_t src_layer, int sx, int sy,
                          Texture* dst, uint32_t dst_level, uint32_t dst_layer, int dx, int dy, uint32_t w, uint32_t h,
                          uint32_t layers) {
    if (!src || !dst || !dst->bc || !src->astc || src->bc || !bc_ready() || w > kBcDim || h > kBcDim || (dx & 3) || (dy & 3))
        return false;
    // (longshot-amd) into a resized BC3 image only whole levels, resized as their upload is
    const uint32_t ow = std::max(1u, dst->width >> dst_level), oh = std::max(1u, dst->height >> dst_level);
    if (dst->bc_resized) {
        const uint32_t gl = dst_level + dst->skip;
        const uint32_t gw = std::max(1u, dst->d.width >> gl), gh = std::max(1u, dst->d.height >> gl);
        if (dx || dy || w != gw || h != gh) {
            static int logged;
            if (logged++ < 8)
                logf("vk: a copy into part of resized BC3 texture %#llx (level %u, %d,%d %ux%u of %ux%u) left out",
                     (unsigned long long)dst->gpu, dst_level, dx, dy, w, h, gw, gh);
            return false;
        }
    }
    // (the blocks are copied in with the region's extent: whole 4x4 blocks, or reaching the level's edge. A region of
    // another size, from an ASTC 6x6 or 10x10 source, is left to the copy's other paths)
    if (!dst->bc_resized && (((w & 3) && dx + w != ow) || ((h & 3) && dy + h != oh))) return false;
    if (!bc_scratch_layout(cmd, dst->bc_resized)) return false;
    for (uint32_t l0 = 0; l0 < layers; l0 += bc_layers()) {   // (a run is the scratch's layers: one, as an upload's)
        const uint32_t n = std::min(bc_layers(), layers - l0);
        full_barrier(cmd);
        const VkImageCopy cp{{VK_IMAGE_ASPECT_COLOR_BIT, src_level, src_layer + l0, n}, {sx, sy, 0},
                             {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, n}, {0, 0, 0}, {w, h, 1}};
        vkCmdCopyImage(cmd, src->image, VK_IMAGE_LAYOUT_GENERAL, bc_scratch, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
        if (dst->bc_resized)
            bc_encode_resized(cmd, w, h, ow, oh, n, *dst, dst_level, dst_layer + l0);
        else
            bc_encode_into(cmd, w, h, n, *dst, dst_level, dst_layer + l0, dx, dy);
    }
    return true;
}

// ---- what the GPU wrote into textures of virtual pools, back into their memory ----
// MK World draws its course maps' tiles (the terrain's height and material maps, 16384 x 16384 R8, 8192 x 8192 R16
// and RG16, sparse in virtual pools) into small 512 x 512 render targets in virtual pools of their own, finalizes
// those, unmaps their pools and maps the physical pages into the course map: on the console the texels are in that
// memory. Here they were only in the small targets' images, the course maps read zeros, and the terrain they shape
// was missing in places (ground drawn nowhere, the sky through it, snow left floating). Before virtual pages are
// unmapped or remapped (nvnMemoryPoolMapVirtual, nvnMemoryPoolFinalize: host/nvn_native.c), the textures the GPU
// wrote there are read back and written into their memory, block linear: the course maps then upload the tiles as
// they are mapped. The same when the CPU is about to read memory the GPU may have written (nvnMemoryPoolInvalidate-
// MappedRange, nvnBufferInvalidateMappedRange): MK World reads back a 1 x 1 RGBA32F texture its GPU computes (the
// scene's luminance), which was never in memory.
namespace {
constexpr uint64_t kFlushMax = 16ull << 20;   // (the small ones: render targets that size and more are not)
std::mutex vw_mu;
std::map<uint64_t, uint64_t> vw;   // (storage start -> end of the GPU-written textures of virtual pools that may be flushed)
// (longshot) and the 4 KB pages (GPU addresses below 512 GB) of every texture ever listed there, read without vw_mu: for
// invalidate_quick (vk_replay.cpp), on the game's thread with its virtual core held. Marked under vw_mu as a texture is listed,
// never taken off (page_marks.h: the order)
PageMarks<(1ull << 39 >> 12), (1ull << 16)> vw_ever;
bool flushable(const Texture& t) {
    if (!t.guest_size || t.guest_size > kFlushMax || t.astc || t.bc || t.sparse || t.skip ||
        t.samples != VK_SAMPLE_COUNT_1_BIT || t.aspect != VK_IMAGE_ASPECT_COLOR_BIT || t.bpb <= 0 || !t.image ||
        (nvn_tex_is_linear(&t.d) && (t.levels != 1 || t.type == VK_IMAGE_TYPE_3D)))   // (linear: one level, rows)
        return false;
    int from = 0;
    widened(t.d.format ? vk_format(t.nvn_format).format : t.format, &from);
    return !from;   // (the image's texels as the storage's: no 3-byte formats kept as four)
}
void vw_note(Texture& t) {
    if (t.vw_listed || !flushable(t)) return;
    std::lock_guard lk(vw_mu);
    vw_ever.mark(t.gpu >> 12, (t.gpu + t.guest_size - 1) >> 12);
    uint64_t& end = vw[t.gpu];
    end = std::max(end, t.gpu + t.guest_size);
    t.vw_listed = true;
}
void vw_forget(const Texture& t) {   // (the entry goes; other listed textures at that start stay listed)
    if (!t.vw_listed) return;
    std::lock_guard lk(vw_mu);
    vw.erase(t.gpu);
    auto [a, b] = by_gpu.equal_range(t.gpu);
    for (auto it = a; it != b; ++it)
        if (it->second != &t && it->second->vw_listed) {
            uint64_t& end = vw[t.gpu];
            end = std::max(end, t.gpu + it->second->guest_size);
        }
}
// (a command buffer of its own for the read-backs, submitted and waited for at once)
VkCommandPool rb_pool = VK_NULL_HANDLE;
VkCommandBuffer rb_cmd = VK_NULL_HANDLE;
VkFence rb_fence = VK_NULL_HANDLE;
// (a read-back whose wait timed out: still on the GPU, its command buffer and fence in use until rb_fence signals. rb_lost:
// a wait failed otherwise, the device lost: no read-back from then on, and none waited for)
bool rb_pending = false, rb_lost = false;
bool readback_begin() {
    if (rb_lost) return false;
    if (rb_pending) {
        const VkResult r = vkWaitForFences(dev.device, 1, &rb_fence, VK_TRUE, 0);
        if (r == VK_TIMEOUT) return false;
        rb_pending = false, rb_lost = r != VK_SUCCESS;
        if (rb_lost) return false;
    }
    if (!rb_pool) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = dev.queue_family;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (vkCreateCommandPool(dev.device, &pi, nullptr, &rb_pool) != VK_SUCCESS) return rb_pool = VK_NULL_HANDLE, false;
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = rb_pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkAllocateCommandBuffers(dev.device, &ai, &rb_cmd) != VK_SUCCESS ||
            vkCreateFence(dev.device, &fi, nullptr, &rb_fence) != VK_SUCCESS) {
            // (made again next time, not used half made: a null rb_cmd or rb_fence with rb_pool set)
            vkDestroyCommandPool(dev.device, rb_pool, nullptr);
            rb_pool = VK_NULL_HANDLE, rb_cmd = VK_NULL_HANDLE, rb_fence = VK_NULL_HANDLE;
            return false;
        }
    }
    vkResetCommandPool(dev.device, rb_pool, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vkBeginCommandBuffer(rb_cmd, &bi) == VK_SUCCESS;
}
/// (longshot, RX_VK_FRAME_TRACE) The time spent in a read-back's submit and wait for the GPU (the write-backs before an
/// eviction, a flush to guest memory), and in texture_frame as a whole: neither was in any of the trace's columns. An area
/// change had frames of 344 and 528 ms with every column near zero, the replay inside the driver in this wait for 317 ms
std::atomic<int64_t> st_readback_wait_ns{0}, st_texture_frame_ns{0};
struct TraceTimer {
    std::atomic<int64_t>& into;
    const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~TraceTimer() {
        into.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(),
                       std::memory_order_relaxed);
    }
};
bool readback_submit_wait() {
    const TraceTimer timed{st_readback_wait_ns};
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};   // (the copies' writes, for the host)
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(rb_cmd, &d);
    if (vkEndCommandBuffer(rb_cmd) != VK_SUCCESS) return false;
    VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cb.commandBuffer = rb_cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cb;
    vkResetFences(dev.device, 1, &rb_fence);
    {
        std::lock_guard qk(dev.present_mu);   // (one queue: submits and presents take turns)
        queue_flush_locked();
        if (vkQueueSubmit2(dev.queue, 1, &si, rb_fence) != VK_SUCCESS) return false;
    }
    const VkResult r = vkWaitForFences(dev.device, 1, &rb_fence, VK_TRUE, 5000000000ull);
    rb_pending = r == VK_TIMEOUT, rb_lost = r != VK_SUCCESS && r != VK_TIMEOUT;
    return r == VK_SUCCESS;
}
/// A texture's write-back: its image's levels and layers read back, swizzled as the storage lays them out, and written into
/// its memory within [clip_lo, clip_hi). The GPU half (flush_record) goes into rb_cmd, the CPU half (flush_finish) runs once
/// the read-back completed: flush_textures does a batch of them with one submission and one wait. Each wait drains the
/// queue, and the retirement sweep wrote back hundreds one by one at a scene load (seconds of stall)
struct Flush {
    Texture* t;
    uint64_t clip_lo, clip_hi;
    struct Part { uint32_t level, layer; uint64_t at, n; LevelGeo g; };
    std::vector<Part> parts;
    uint64_t total = 0;
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    void* map = nullptr;
    bool ok = false;
    bool busy = false;             // (not written back: a read-back is still on the GPU (rb_pending), to be tried again later)
    bool only_untouched = false;   // (drawn_writeback.h: only into memory still all zero, or holding its own last write-back)
    bool touched = false;          // (and it was neither, or a guest thread wrote it during the write: the CPU's, nothing written)
    uint64_t written_hash = 0;     // (and when written: the hash of its memory as the write left it)
};
// (cached host memory, which the CPU reads: the sequential-write kind is write-combined, or VRAM behind the BAR, and every read
// of it by the unswizzle goes over PCIe uncached: flush_record's staging buffer)
/// The copies of f's image into a staging buffer of its own, recorded into cmd (rb_cmd, begun by the caller, when none is
/// given); false: nothing recorded
bool flush_record(Flush& f, VkCommandBuffer cmd = VK_NULL_HANDLE) {
    if (!cmd) cmd = rb_cmd;
    Texture& t = *f.t;
    const uint32_t layers = t.type == VK_IMAGE_TYPE_3D ? 1 : t.layers;
    for (uint32_t level = 0; level < t.levels; level++) {
        const LevelGeo g = level_geo(t, level);
        for (uint32_t layer = 0; layer < layers; layer++) {
            const uint64_t n = (uint64_t)g.tw * g.th * g.td * (uint64_t)t.bpb;
            f.parts.push_back({level, layer, f.total, n, g});
            f.total += (n + 15) & ~15ull;
        }
    }
    if (!f.total) return f.ok = false;
    if (!create_buffer_cached(f.total, VK_BUFFER_USAGE_TRANSFER_DST_BIT, f.buf, f.alloc, &f.map) || !f.map) {
        if (f.buf) destroy_buffer(f.buf, f.alloc);
        f.buf = VK_NULL_HANDLE, f.alloc = nullptr, f.map = nullptr;
        return f.ok = false;
    }
    std::vector<VkBufferImageCopy> c;
    for (const Flush::Part& p : f.parts) {
        VkBufferImageCopy r{};
        r.bufferOffset = p.at;
        r.bufferRowLength = p.g.tw * (uint32_t)t.block_w;
        r.bufferImageHeight = p.g.th * (uint32_t)t.block_h;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, p.level, t.type == VK_IMAGE_TYPE_3D ? 0 : p.layer, 1};
        r.imageExtent = {p.g.w, p.g.h, p.g.d};
        c.push_back(r);
    }
    vkCmdCopyImageToBuffer(cmd, t.image, VK_IMAGE_LAYOUT_GENERAL, f.buf, (uint32_t)c.size(), c.data());
    return f.ok = true;
}
/// [gpu, gpu + n) of guest memory from `from` to `to`, only where it still holds `from` (drawn_writeback::swap_words; went
/// and only as there, n bytes each): false when a guest thread wrote some of it meanwhile, or it is not mapped
bool guest_swap(uint64_t gpu, const uint8_t* from, const uint8_t* to, uint64_t n, uint8_t* went = nullptr,
                const uint8_t* only = nullptr) {
    bool all = true;
    for (uint64_t at = 0; at < n;) {
        uint64_t avail = 0;
        uint8_t* p = host->gpu_ptr(gpu + at, &avail);
        if (!p || !avail) {
            all = false;
            break;
        }
        const uint64_t k = std::min(avail, n - at);
        all &= drawn_writeback::swap_words(p, from + at, to + at, (size_t)k, went ? went + at : nullptr, only ? only + at : nullptr) == 0;
        host->mark_written(p, k);   // (as host->gpu_write does: the soft write watch)
        at += k;
    }
    guest_cpu_wrote(gpu, n);
    return all;
}
/// The CPU half, once the batch's read-back completed (gpu_ok) or failed: the texels into the texture's memory, the buffer freed
bool flush_finish(Flush& f, bool gpu_ok) {
    using Part = Flush::Part;
    Texture& t = *f.t;
    const uint64_t clip_lo = f.clip_lo, clip_hi = f.clip_hi, total = f.total;
    const std::vector<Part>& parts = f.parts;
    void* map = f.map;
    bool ok = f.ok && gpu_ok;
    if (ok) invalidate_mapped(f.alloc, total);
    // (only_untouched: the memory as it is now, whatever the page generations know of it yet. The texels are staged
    // against this snapshot and go in at the end, compared with it word by word: publish below)
    std::vector<uint8_t> was, staged;
    struct Piece { uint64_t gpu, at, n; };   // (staged[at, at + n) for guest memory at gpu)
    std::vector<Piece> pieces;
    if (ok && f.only_untouched) {
        was.resize((size_t)t.guest_size);
        if (!host->gpu_read(t.gpu, was.data(), was.size()) || !drawn_writeback::may_write(was.data(), was.size(), t.written_back_hash))
            ok = false, f.touched = true;
    }
    if (ok && total <= 64) {   // (RX_VK_LOG_TINY_FLUSH=1: the values of tiny ones, 400 lines: a 1 x 1 luminance)
        static const bool log_tiny = env_on("RX_VK_LOG_TINY_FLUSH");
        static int logged;
        if (log_tiny && logged++ < 400) {
            const float* f = (const float*)map;
            const uint32_t* u = (const uint32_t*)map;
            logf("vk: tiny write-back #%u %#llx NVN %#x: %g %g %g %g (%08x %08x %08x %08x)", t.id, (unsigned long long)t.gpu,
                 t.nvn_format, f[0], f[1], f[2], f[3], u[0], u[1], u[2], u[3]);
        }
    }
    // (longshot) pages the CPU wrote after the GPU last wrote the texture keep the CPU's bytes: on the console the GPU's
    // writes are in memory as it renders, and a later CPU write is what is there. OoT reuses a render target's memory
    // for scene data and then invalidates a range of it: this wrote the whole texture back over that data, and its
    // SceneLowPriority thread read texels as pointers (an access violation at 0xffffffffffffffff)
    std::vector<std::pair<uint64_t, uint64_t>> cpu_runs;   // (offsets from t.gpu)
    if (ok && t.gpu < (1ull << 40)) memory_written_runs(t.gpu, t.guest_size, t.gpu_gen, cpu_runs);
    uint64_t kept_cpu = 0;
    // (and only into the pages being remapped: those are what the texture's content would be lost from. The rest of it stays where
    // the game may have put other things since. The watch above does not catch all of those: OoT's SceneLowPriority crash, walking
    // in Kokiri Forest, was texels landing right beside the remapped pages, on scene data)
    const auto write_back = [&](uint64_t whole_gpu, const uint8_t* whole_src, uint64_t whole_n) {
        const uint64_t from = std::max(whole_gpu, clip_lo), to = std::min(whole_gpu + whole_n, clip_hi);
        if (from >= to) return true;
        const uint64_t gpu = from, n = to - from;
        const uint8_t* src = whole_src + (from - whole_gpu);
        const uint64_t lo = gpu - t.gpu, hi = lo + n;
        bool w = true;
        const auto put = [&](uint64_t off, const uint8_t* bytes, uint64_t len) {
            if (!f.only_untouched) return guest_cpu_write(t.gpu + off, bytes, len) != 0;
            pieces.push_back({t.gpu + off, staged.size(), len});
            staged.insert(staged.end(), bytes, bytes + len);
            return true;
        };
        uint64_t at = lo;
        for (const auto& [a, b] : cpu_runs) {
            if (b <= at) continue;
            if (a >= hi) break;
            if (a > at) w &= put(at, src + (at - lo), a - at);
            kept_cpu += std::min(b, hi) - std::max(a, at);
            at = std::max(at, b);
            if (at >= hi) break;
        }
        if (at < hi) w &= put(at, src + (at - lo), hi - at);
        return w;
    };
    // (only_untouched) the staged texels into memory: the snapshot once more, since the unswizzle took its time, then
    // each word only where it still is as the snapshot has it. A word a guest thread wrote meanwhile stays, and then
    // the words that did go in (those only) come out again: either all of the texture's texels are in its memory, or
    // the memory is as the guest has it
    const auto publish = [&] {
        if (!ok || !f.only_untouched) return;
        std::vector<uint8_t> again(was.size());
        bool same = host->gpu_read(t.gpu, again.data(), again.size()) && again == was;
        if (same) {
            std::vector<uint8_t> went(staged.size());   // (the bytes that went in: the undo takes out those only)
            for (const Piece& p : pieces)
                same &= guest_swap(p.gpu, was.data() + (p.gpu - t.gpu), staged.data() + p.at, p.n, went.data() + p.at);
            if (!same)
                for (const Piece& p : pieces)
                    guest_swap(p.gpu, staged.data() + p.at, was.data() + (p.gpu - t.gpu), p.n, nullptr, went.data() + p.at);
        }
        if (!same) {
            ok = false, f.touched = true;
            return;
        }
        for (const Piece& p : pieces) std::memcpy(again.data() + (p.gpu - t.gpu), staged.data() + p.at, (size_t)p.n);
        f.written_hash = drawn_writeback::hash(again.data(), again.size());
    };
    const auto log_kept = [&] {
        static int logged;
        if (kept_cpu && logged++ < 20)
            logf("vk: write-back of #%u %#llx: %llu KB the CPU wrote since the GPU kept", t.id, (unsigned long long)t.gpu,
                 (unsigned long long)(kept_cpu >> 10));
    };
    if (ok && nvn_tex_is_linear(&t.d)) {   // (a linear texture: its rows at the stride, one level)
        const uint64_t stride = t.d.stride ? t.d.stride : host->tex_linear_stride(&t.d);
        for (const Part& p : parts) {
            const uint64_t row = (uint64_t)p.g.tw * (uint64_t)t.bpb;
            const uint64_t base = (uint64_t)p.layer * t.L.layer_size;
            for (uint32_t y = 0; y < p.g.th; y++) {
                const uint64_t off = base + (uint64_t)y * stride;
                if (off + row > t.guest_size) break;
                if (!write_back(t.gpu + off, (const uint8_t*)map + p.at + (uint64_t)y * row, row)) ok = false;
            }
        }
        publish();
        log_kept();
        if (f.buf) destroy_buffer(f.buf, f.alloc);
        return ok;
    }
    if (ok) {
        std::vector<uint8_t> sw;
        for (const Part& p : parts) {
            const uint64_t at = t.gpu + p.g.offset + (uint64_t)p.layer * t.L.layer_size;
            if (p.g.offset + (uint64_t)p.layer * t.L.layer_size >= t.guest_size) continue;
            const uint64_t span = std::min<uint64_t>(p.g.swizzled, t.guest_size - p.g.offset - (uint64_t)p.layer * t.L.layer_size);
            sw.assign(std::max<uint64_t>(p.g.swizzled, span), 0);
            // (what is there: the padding between texels kept)
            if (f.only_untouched) std::memcpy(sw.data(), was.data() + (at - t.gpu), (size_t)span);
            else host->gpu_read(at, sw.data(), span);
            // (a refusal writes nothing: this part is not written back, and `ok` says so; zeroing `sw` instead would overwrite the
            // guest's bytes)
            if (!texdecode::swizzle(std::span<uint8_t>(sw.data(), sw.size()),
                                    std::span<const uint8_t>((const uint8_t*)map + p.at, p.n), (uint32_t)t.bpb, p.g.tw, p.g.th,
                                    p.g.td, p.g.tile_h, p.g.tile_d)) {
                texdecode_refused("swizzle", "this part is not written back");
                ok = false;
                continue;
            }
            if (!write_back(at, sw.data(), span)) ok = false;   // (marks the pages written: textures over them upload)
        }
        publish();
        log_kept();
    }
    if (f.buf) destroy_buffer(f.buf, f.alloc);
    return ok;
}
/// A batch of write-backs: one submission and one wait for all of them
void flush_textures(std::vector<Flush>& fs) {
    bool any = false;
    const bool begun = readback_begin();
    if (begun) {
        full_barrier(rb_cmd);   // (after what the submissions before wrote)
        for (Flush& f : fs) any |= flush_record(f);
    } else {
        for (Flush& f : fs) f.ok = false;
    }
    const bool gpu_ok = any && readback_submit_wait();
    if (any && !gpu_ok) {
        // (the read-back did not complete: its 5 s wait, which also covers what queue_flush_locked just submitted, or a submit
        // that failed. The copies may still be writing into the staging buffers, so they are not freed here: they go the way a
        // destroyed image does, once a submission made after this one is done. The batch's write-backs are lost, as a single
        // one's was before)
        static int logged;
        if (logged++ < 8)
            logf("vk: a batch of %zu texture write-backs did not complete in time: not written back, its staging buffers kept "
                 "until the GPU is past them", fs.size());
        for (Flush& f : fs) {
            // (and the images they copy from, by the same rule: evicted now, their end waits for the copy too)
            f.t->last_value = std::max(f.t->last_value, rec_value + 1);
            if (f.buf) {
                Dead d{rec_value + 1, VK_NULL_HANDLE, nullptr, {}, {}};
                d.buffer = f.buf, d.buffer_alloc = f.alloc;
                dead.push_back(std::move(d));
                f.buf = VK_NULL_HANDLE, f.alloc = nullptr, f.map = nullptr;
            }
        }
    }
    const bool busy = rb_pending && (!begun || (any && !gpu_ok));   // (this batch's wait timed out, or one before it is still running)
    for (Flush& f : fs) f.ok = flush_finish(f, gpu_ok), f.busy = busy && !f.ok;
}
bool flush_texture(Texture& t, uint64_t clip_lo, uint64_t clip_hi) {
    std::vector<Flush> one;
    one.push_back(Flush{&t, clip_lo, clip_hi});
    flush_textures(one);
    return one[0].ok;
}
// (longshot) Small mip-chained textures the GPU drew in one frame: their texels into their memory, from two frames later
// (drawn_writeback.h has the rule and why: OoT copies a drawn terrain tile's memory on the CPU, and the copy was all
// zero). Nothing waits for the GPU: a frame's batch has its copies recorded with the present's uploads, and a later
// frame writes the texels once the GPU is past that recording (a wait here took the frame 3 to 8 ms, and 232 ms behind
// an area load's uploads)
std::mutex drawn_mu;
std::vector<std::pair<uint64_t, Texture*>> drawn_noted;   // (storage, texture: drawn_mu)
struct DrawnBatch {
    uint64_t value;   // (the recording its copies are in)
    uint64_t frame;
    std::vector<Flush> fs;
    struct Was { uint32_t id; uint64_t last_written; };   // (each texture as it was then: the same one, not drawn since)
    std::vector<Was> was;
};
std::deque<DrawnBatch> drawn_batches;
constexpr size_t kDrawnBatches = 4;   // (open at once: with the GPU that far behind, the textures due wait)
void drawn_writeback_note(Texture* tex) {
    if (tex->writeback_noted || !drawn_writeback::noted(tex->gpu, tex->guest_size, tex->levels)) return;
    tex->writeback_noted = true;
    std::lock_guard lk(drawn_mu);
    drawn_noted.push_back({tex->gpu, tex});
}
bool drawn_alive(uint64_t gpu, const Texture* t) {   // (not destroyed since it was listed)
    auto [a, b] = by_gpu.equal_range(gpu);
    for (auto it = a; it != b; ++it)
        if (it->second == t) return true;
    return false;
}
/// For each texture (nullptr: skipped), the newest drawing into another image whose storage overlaps its own; 0 for none
std::vector<uint64_t> drawn_others(const std::vector<const Texture*>& ts) {
    std::vector<uint64_t> other(ts.size(), 0);
    for (const auto& kv : textures) {   // (from any start: a scratch heap's targets overlap at other offsets)
        const Texture* o = kv.second.get();
        if (!o->gpu_written || !o->guest_size) continue;
        for (size_t i = 0; i < ts.size(); i++) {
            const Texture* t = ts[i];
            if (t && o != t && o->gpu < t->gpu + t->guest_size && o->gpu + o->guest_size > t->gpu)
                other[i] = std::max(other[i], o->last_written);
        }
    }
    return other;
}
/// The batches the GPU is done copying: their texels into memory, where the texture is still as it was copied
void drawn_writeback_finish() {
    static int64_t batches, done, failed, left_cpu, dropped, bytes;
    static double all_ms;
    while (!drawn_batches.empty() && drawn_batches.front().value <= completed_value) {
        DrawnBatch batch = std::move(drawn_batches.front());
        drawn_batches.pop_front();
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<const Texture*> ts;
        for (size_t i = 0; i < batch.fs.size(); i++) {
            const Texture* t = batch.fs[i].t;   // (clip_lo is its storage: the pointer is looked at only once found there)
            ts.push_back(drawn_alive(batch.fs[i].clip_lo, t) && t->id == batch.was[i].id ? t : nullptr);
        }
        const std::vector<uint64_t> other = drawn_others(ts);
        int64_t wrote = 0;
        const Texture* first = nullptr;
        for (size_t i = 0; i < batch.fs.size(); i++) {
            Flush& f = batch.fs[i];
            // (drawn or uploaded over since the copy, or another image drawn over its storage: the copy is not its content)
            if (!ts[i] || !f.t->gpu_written || f.t->only_cleared || f.t->last_written != batch.was[i].last_written ||
                (other[i] && other[i] >= f.t->last_written)) {
                if (f.buf) destroy_buffer(f.buf, f.alloc);
                dropped++;
                continue;
            }
            f.ok = flush_finish(f, true);
            f.t->written_back_for = f.t->last_written;   // (written, or the CPU's memory now: not tried again for this drawing)
            if (!f.ok) {
                (f.touched ? left_cpu : failed)++;
                continue;
            }
            done++, wrote++, bytes += (int64_t)f.t->guest_size;
            f.t->written_back_hash = f.written_hash;   // (its own texels there, for the next drawing's write-back)
            if (!first) first = f.t;
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        all_ms += ms;
        if (batches++ < 12 || batches % 200 == 0)
            logf("vk: %lld of %zu small textures the GPU drew written into their memory in %.1f ms, %llu frames after their copies "
                 "were recorded, the first #%u %#llx %ux%u levels %u NVN %#x (%lld so far, %lld KB, %.0f ms; %lld left, the CPU wrote "
                 "their memory since; %lld dropped, drawn over or gone; %lld failed)", (long long)wrote, batch.fs.size(), ms,
                 (unsigned long long)(frame_no - batch.frame), first ? first->id : 0, (unsigned long long)(first ? first->gpu : 0),
                 first ? first->width : 0, first ? first->height : 0, first ? first->levels : 0, first ? first->nvn_format : 0,
                 (long long)done, (long long)(bytes >> 10), all_ms, (long long)left_cpu, (long long)dropped, (long long)failed);
    }
}
void drawn_writeback_frame() {
    drawn_writeback_finish();
    if (drawn_batches.size() >= kDrawnBatches) return;
    std::vector<std::pair<uint64_t, Texture*>> noted;
    {
        std::lock_guard lk(drawn_mu);
        noted.swap(drawn_noted);
    }
    if (noted.empty()) return;
    std::vector<std::pair<uint64_t, Texture*>> wait, cands;
    std::vector<const Texture*> ts;
    for (const auto& e : noted)
        if (drawn_alive(e.first, e.second)) cands.push_back(e), ts.push_back(e.second);
    const std::vector<uint64_t> other = drawn_others(ts);
    DrawnBatch batch{rec_value, frame_no, {}, {}};
    for (size_t i = 0; i < cands.size(); i++) {
        Texture* t = cands[i].second;
        const drawn_writeback::When when = drawn_writeback::due(frame_no, t->first_written, t->last_written, t->written_back_for,
                                                                t->gpu_written, t->only_cleared, other[i]);
        if (when == drawn_writeback::When::Wait) {
            wait.push_back(cands[i]);
            continue;
        }
        t->writeback_noted = false;
        if (when == drawn_writeback::When::Now && flushable(*t)) {
            Flush f{t, t->gpu, t->gpu + t->guest_size};
            f.only_untouched = true;
            batch.fs.push_back(std::move(f));
        }
    }
    if (!wait.empty()) {
        std::lock_guard lk(drawn_mu);
        drawn_noted.insert(drawn_noted.end(), wait.begin(), wait.end());
    }
    if (batch.fs.empty()) return;
    // (the copies, in this present's recording with its uploads, as the compaction's are: the drawings are two frames
    // back, in recordings before it)
    VkCommandBuffer cmd = upload_cmd();
    full_barrier(cmd, "drawn write-back: before the copies");
    std::vector<Flush> recorded;
    for (Flush& f : batch.fs) {
        if (!flush_record(f, cmd)) continue;   // (no staging memory: its texels stay out of memory, as before this)
        f.t->last_value = std::max(f.t->last_value, rec_value);   // (its end, if it goes now, waits for the copy too)
        batch.was.push_back({f.t->id, f.t->last_written});
        recorded.push_back(std::move(f));
    }
    batch.fs = std::move(recorded);
    if (batch.fs.empty()) return;
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};   // (the copies' writes, for the host)
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
    drawn_batches.push_back(std::move(batch));
}
} // namespace

void texture_describe_written(uint64_t lo, uint64_t size, std::string& out) {
    const uint64_t hi = lo + size;
    int n = 0;
    for (auto& kv : textures) {
        const Texture* t = kv.second.get();
        if (t->gpu >= hi || t->gpu + t->guest_size <= lo || !t->gpu_written) continue;
        if (n++ < 6) {
            char b[160];
            snprintf(b, sizeof b, " #%u %#llx %ux%ux%u x%u NVN %#x %llu KB%s%s;", t->id, (unsigned long long)t->gpu, t->width,
                     t->height, t->depth, t->layers, t->nvn_format, (unsigned long long)(t->guest_size >> 10),
                     t->released ? " finalized" : "", flushable(*t) ? "" : " (not flushable)");
            out += b;
        }
    }
    if (n > 6) out += " (" + std::to_string(n) + " in all)";
}

bool texture_gpu_written_in(uint64_t lo, uint64_t size) {
    const uint64_t hi = lo + size;
    std::lock_guard lk(vw_mu);
    for (auto it = vw.lower_bound(lo > kFlushMax ? lo - kFlushMax : 0); it != vw.end() && it->first < hi; ++it)
        if (it->second > lo) return true;
    return false;
}

/// (longshot) Whether a texture the GPU wrote was ever listed in [lo, lo + size), texture_gpu_written_in's list, without its lock
/// (invalidate_quick): false only where texture_gpu_written_in is false. Ranges reaching past 512 GB are not followed: true
bool texture_gpu_written_ever(uint64_t lo, uint64_t size) {
    if (!size) return false;
    if (lo >= (1ull << 39) || size > (1ull << 39) - lo) return true;
    return vw_ever.any(lo >> 12, (lo + size - 1) >> 12);
}

int texture_flush_to_guest(uint64_t lo, uint64_t size) {
    const uint64_t hi = lo + size;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<Texture*> todo;
    for (auto& kv : textures) {
        Texture* t = kv.second.get();
        if (t->gpu >= hi || t->gpu + t->guest_size <= lo) continue;
        t->vw_listed = false;
        if (t->gpu_written && flushable(*t)) todo.push_back(t);
    }
    {
        std::lock_guard lk(vw_mu);
        for (auto it = vw.lower_bound(lo > kFlushMax ? lo - kFlushMax : 0); it != vw.end() && it->first < hi;)
            it = it->second > lo ? vw.erase(it) : std::next(it);
    }
    int done = 0;
    uint64_t bytes = 0;
    for (Texture* t : todo) {
        if (!flush_texture(*t, lo, hi)) continue;
        done++, bytes += t->guest_size;
        if (t->gpu < lo || t->gpu + t->guest_size > hi) continue;   // (a part of it: the rest is still only in the image)
        // (its memory is its content now: CPU writes to it count again, and an image made over it uploads it)
        t->gpu_written = false;
        t->uploaded = true;
        t->upload_gen = memory_generation();
        watch_tex(t, "written back into its memory");
    }
    static int64_t flushes, all_bytes;
    static double all_ms;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    flushes += done, all_bytes += (int64_t)bytes, all_ms += ms;
    static int logged;
    if (done && (logged++ < 12 || logged % 200 == 0))
        logf("vk: virtual pages %#llx..%#llx remapped: %d texture(s) the GPU wrote there written back into their memory "
             "(%llu KB, %.1f ms; %lld in all, %lld MB, %.0f ms)", (unsigned long long)lo, (unsigned long long)hi, done,
             (unsigned long long)(bytes >> 10), ms, (long long)flushes, (long long)(all_bytes >> 20), all_ms);
    return done;
}

static void note_drawn_storage(Texture* tex) {
    if (!tex->id || !tex->guest_size) return;
    const uint64_t end = tex->gpu + tex->guest_size;
    if (tex->gpu_written && !tex->released && tex->gpu_draw_value == rec_value && !wrote_regions::any(tex->gpu, end)) return;
    std::lock_guard lk(gpu_wrote_mu);
    gpu_wrote.drew(tex->id, tex->gpu, end, rec_value);
    tex->gpu_draw_value = rec_value;
}
void texture_written(Texture* tex, const char* why) {
    if (!tex) return;
    vw_note(*tex);
    tex->only_cleared = false;
    if (tex->sparse && !tex->gpu_written) {
        static int logged;
        if (logged++ < 20) logf_at(log_debug, "vk: sparse texture %#llx written by the GPU (#%u)", (unsigned long long)tex->gpu, tex->id);
    }
    watch_tex(tex, "written");
    note_drawn_storage(tex);
    if (!tex->gpu_written || tex->released) tex->first_written = frame_no, drawn_writeback_note(tex);
    // (longshot) A texture in a virtual pool that the GPU writes first is marked uploaded below without the first upload,
    // which notes its pages' mapping (texture_touch): its first check after the game finalized it took all of its storage
    // for remapped, and the whole image was uploaded from guest memory over the GPU's texels (OoT's HUD button labels blank
    // after a zone change). Noted here instead, by the writers that give a reason (tests/test_drawn_page_map.py lists them).
    // The upscaler's stand-ins give none. A texture clear gives none: resolve() samples its texture first, which notes the map, unless
    // only writers that give none wrote it before (then as before)
    if (why && strcmp(why, "?") != 0 && tex->gpu >= (1ull << 40) && tex->page_map.empty() && tex->guest_size) {
        static thread_local Ranges mapped;
        texture_dirty(*tex, 0, mapped);
    }
    tex->last_written = frame_no;
    tex->gpu_written = true;
    tex->gpu_gen = memory_generation();
    tex->released = false;
    tex->uploaded = true;
}


bool texture_touch_quick(Texture* tex, bool for_sampling) {
    if (tex->freed_listed || tex->reuse_retire) return false;   // (texture_touch takes it out of the reuse list)
    tex->last_frame = frame_no;   // (the same values whichever thread stores them)
    tex->last_value = rec_value;
    if (!for_sampling) return true;
    if (tex->gpu_written && !tex->released && !tex->only_cleared) return true;
    return tex->uploaded && tex->checked_epoch == epoch;
}
void texture_gpu_wrote(uint64_t gpu, uint64_t size) {
    if (!gpu || !size) return;
    const uint64_t lo = gpu, hi = gpu + size;
    {   // (RX_VK_WATCH_GPU_WRITE=<lo hex>[:<hi hex>]: the GPU buffer writes reaching that range, by program, 60 lines)
        static uint64_t wlo = 0, whi = 0;
        static const bool parsed = [] {
            if (const char* e = getenv("RX_VK_WATCH_GPU_WRITE")) {
                unsigned long long a = 0, b = 0;
                if (sscanf(e, "%llx:%llx", &a, &b) < 2) b = a + 1;
                wlo = a, whi = b;
            }
            return true;
        }();
        (void)parsed;
        static int logged;
        if (wlo < hi && lo < whi && logged++ < 60)
            logf("vk: watch: GPU buffer write [%#llx, +%#llx) into [%#llx, %#llx), frame %llu, program %016llx",
                 (unsigned long long)lo, (unsigned long long)size, (unsigned long long)wlo, (unsigned long long)whi,
                 (unsigned long long)frame_no, (unsigned long long)tl_watch_prog);
    }
    wrote_regions::mark(lo, hi);   // (before the map has it: gpu_wrote_into asks the regions first, without the lock)
    std::lock_guard lk(gpu_wrote_mu);
    gpu_wrote.record(lo, hi, rec_value);
}
/// A completed buffer write into the texture's storage since its last successful upload.
uint64_t gpu_wrote_into(const Texture& tex) {
    const uint64_t lo = tex.gpu, hi = tex.gpu + std::max<uint64_t>(tex.guest_size, 1);
    // (longshot: nearly every texture is in memory no GPU buffer write ever reached, and each one a submission samples asked
    // here: the lock and the walk were 0.6 % of the replay thread on a Steam Deck. wrote_regions.h)
    if (!wrote_regions::any(lo, hi)) return 0;
    std::lock_guard lk(gpu_wrote_mu);
    const uint64_t newest = gpu_wrote.newest(lo, hi, tex.gpu_written ? tex.id : 0);
    if (newest <= tex.gpu_wrote_seen) return 0;
    return newest;
}
void texture_watch_prog(uint64_t prog) {
    tl_watch_prog = prog;
}

namespace {
std::mutex sparse_mu;
std::vector<VkSparseImageMemoryBind> sparse_binds;   // (what the next flush binds, under sparse_mu)
std::vector<VkSparseMemoryBind> sparse_opaque;
VkDeviceSize sparse_added = 0;

/// One sparse block's memory (or n bytes) from the texture's chunks; false when none can be had
bool sparse_take(Texture* tex, VkDeviceSize n, VkDeviceMemory& mem, VkDeviceSize& off) {
    constexpr VkDeviceSize kChunk = 64ull << 20;
    n = (n + tex->sparse_align - 1) / tex->sparse_align * tex->sparse_align;
    for (size_t i = 0; i < tex->sparse_free.size(); i++) {   // (a failed bind's memory first: a bind that keeps failing takes no more)
        if (tex->sparse_free[i].size < n) continue;
        mem = tex->sparse_free[i].memory, off = tex->sparse_free[i].offset;
        tex->sparse_free.erase(tex->sparse_free.begin() + (std::ptrdiff_t)i);
        return true;
    }
    if (tex->sparse_chunks.empty() || tex->chunk_used + n > kChunk) {
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = std::max(kChunk, n);
        ai.memoryTypeIndex = tex->sparse_type;
        VkDeviceMemory m = VK_NULL_HANDLE;
        if (vkAllocateMemory(dev.device, &ai, nullptr, &m) != VK_SUCCESS) {
            static int logged;
            if (logged++ < 8) logf("vk: sparse texture %#llx: no device memory for its blocks", (unsigned long long)tex->gpu);
            return false;
        }
        tex->sparse_chunks.push_back(m);
        tex->chunk_used = 0;
        tex->bytes += ai.allocationSize;
        sparse_added += ai.allocationSize;
    }
    mem = tex->sparse_chunks.back();
    off = tex->chunk_used;
    tex->chunk_used += n;
    return true;
}

/// The queued binds that were not made: their blocks and mip tails marked unbound again, so the next write to them
/// queues them anew rather than writing into unbacked memory (their memory goes to the texture's free list, for the retry)
void sparse_unqueue(Texture* tex) {
    const VkSparseImageMemoryRequirements& r = tex->sparse_req;
    const VkExtent3D g = r.formatProperties.imageGranularity;
    for (const VkSparseImageMemoryBind& b : sparse_binds) {
        const uint32_t level = b.subresource.mipLevel, layer = b.subresource.arrayLayer;
        if (level >= tex->levels || layer >= tex->layers || !g.width || !g.height) continue;
        std::vector<uint8_t>& map = tex->tiles[(size_t)layer * tex->levels + level];
        const uint32_t lw = std::max(1u, tex->width >> level);
        const size_t i = (size_t)(b.offset.y / g.height) * ((lw + g.width - 1) / g.width) + b.offset.x / g.width;
        if (i < map.size()) map[i] = 0;
        tex->sparse_free.push_back({b.memory, b.memoryOffset, tex->sparse_align});
    }
    const bool single = r.formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT;
    for (const VkSparseMemoryBind& b : sparse_opaque) {
        const VkDeviceSize layer = single || !r.imageMipTailStride ? 0 : (b.resourceOffset - r.imageMipTailOffset) / r.imageMipTailStride;
        if (layer < tex->tail_bound.size()) tex->tail_bound[layer] = 0;
        const VkDeviceSize a = tex->sparse_align ? tex->sparse_align : 1;
        tex->sparse_free.push_back({b.memory, b.memoryOffset, (b.size + a - 1) / a * a});
    }
    total_bytes += sparse_added;
    sparse_added = 0;
    sparse_binds.clear();
    sparse_opaque.clear();
}

bool sparse_flush(Texture* tex) {
    if (sparse_binds.empty() && sparse_opaque.empty()) return true;
    VkSparseImageMemoryBindInfo ib{tex->image, (uint32_t)sparse_binds.size(), sparse_binds.data()};
    VkSparseImageOpaqueMemoryBindInfo ob{tex->image, (uint32_t)sparse_opaque.size(), sparse_opaque.data()};
    VkBindSparseInfo bi{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
    bi.imageBindCount = sparse_binds.empty() ? 0 : 1;
    bi.pImageBinds = &ib;
    bi.imageOpaqueBindCount = sparse_opaque.empty() ? 0 : 1;
    bi.pImageOpaqueBinds = &ob;
    static VkFence fence = VK_NULL_HANDLE;
    if (!fence) {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(dev.device, &fi, nullptr, &fence) != VK_SUCCESS) {   // (never a bind waited on through a null fence)
            fence = VK_NULL_HANDLE;
            static int logged;   // (retried on the next write: capped like the other sparse messages)
            if (logged++ < 8) logf("vk: sparse texture %#llx: no fence for its binds", (unsigned long long)tex->gpu);
            sparse_unqueue(tex);
            return false;
        }
    }
    VkResult res;
    {   // (bound before any work that writes the blocks is submitted: waited for here, on the recording thread. And
        // after the work submitted before is done: a sparse bind is not ordered with command buffers by their
        // barriers, and one that landed while a pass sampled the image changed what the pass read part-way through
        // its tiles. Opt-in, RX_VK_SPARSE_IDLE=1: MKW binds hundreds of blocks a race, and the replay's stalls at
        // each made its other races worse)
        static const bool no_idle = !env_on("RX_VK_SPARSE_IDLE");
        static int64_t binds;
        std::lock_guard qk(dev.present_mu);
        queue_flush_locked();
        if (!no_idle) vkQueueWaitIdle(dev.queue);
        res = vkQueueBindSparse(dev.queue, 1, &bi, fence);
        if (++binds % 200 == 0) logf("vk: %lld sparse binds", (long long)binds);
    }
    if (res == VK_SUCCESS) {
        vkWaitForFences(dev.device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(dev.device, 1, &fence);
    } else {
        static int failed;   // (retried on the next write: capped like the other sparse messages)
        if (failed++ < 8) logf("vk: sparse texture %#llx: binding failed (%d)", (unsigned long long)tex->gpu, (int)res);
        sparse_unqueue(tex);
        return false;
    }
    static int logged;
    static uint64_t next_mb = 0;
    if (logged < 20 || (tex->bytes >> 20) >= next_mb) {
        logged++;
        next_mb = (tex->bytes >> 20) + 256;
        logf("vk: sparse texture %#llx: %zu blocks bound (%llu MB of device memory)", (unsigned long long)tex->gpu,
             sparse_binds.size() + sparse_opaque.size(), (unsigned long long)(tex->bytes >> 20));
    }
    total_bytes += sparse_added;
    sparse_added = 0;
    sparse_binds.clear();
    sparse_opaque.clear();
    return res == VK_SUCCESS;
}

/// Queues the binds of what a rectangle reaches that is not bound yet (sparse_mu held)
bool sparse_queue(Texture* tex, uint32_t level, uint32_t layer, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    const VkSparseImageMemoryRequirements& r = tex->sparse_req;
    if (level >= tex->levels || layer >= tex->layers) return true;
    if (level >= r.imageMipTailFirstLod) {   // (the mip tail: bound whole)
        const bool single = r.formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT;
        uint8_t& bound = tex->tail_bound[single ? 0 : layer];
        if (bound || !r.imageMipTailSize) return true;
        VkDeviceMemory m;
        VkDeviceSize off;
        if (!sparse_take(tex, r.imageMipTailSize, m, off)) return false;
        sparse_opaque.push_back({r.imageMipTailOffset + (single ? 0 : layer * r.imageMipTailStride), r.imageMipTailSize,
                                 m, off, 0});
        bound = 1;
        return true;
    }
    const VkExtent3D g = r.formatProperties.imageGranularity;
    const uint32_t lw = std::max(1u, tex->width >> level), lh = std::max(1u, tex->height >> level);
    if (x >= lw || y >= lh || !w || !h) return true;
    w = std::min(w, lw - x), h = std::min(h, lh - y);
    const uint32_t ntx = (lw + g.width - 1) / g.width, nty = (lh + g.height - 1) / g.height;
    std::vector<uint8_t>& map = tex->tiles[(size_t)layer * tex->levels + level];
    if (map.empty()) map.assign((size_t)ntx * nty, 0);
    bool ok = true;
    for (uint32_t ty = y / g.height; ty <= (y + h - 1) / g.height; ty++)
        for (uint32_t tx = x / g.width; tx <= (x + w - 1) / g.width; tx++) {
            uint8_t& bound = map[(size_t)ty * ntx + tx];
            if (bound) continue;
            VkDeviceMemory m;
            VkDeviceSize off;
            if (!sparse_take(tex, tex->sparse_align, m, off)) {
                ok = false;
                continue;
            }
            VkSparseImageMemoryBind b{};
            b.subresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer};
            b.offset = {(int32_t)(tx * g.width), (int32_t)(ty * g.height), 0};
            b.extent = {std::min(g.width, lw - tx * g.width), std::min(g.height, lh - ty * g.height), 1};
            b.memory = m;
            b.memoryOffset = off;
            sparse_binds.push_back(b);
            bound = 1;
        }
    return ok;
}
} // namespace

/// (the upload's: queued, and bound together before its copies are recorded)
void texture_resident_queue(Texture* tex, uint32_t level, uint32_t layer, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    std::lock_guard lk(sparse_mu);
    sparse_queue(tex, level, layer, x, y, w, h);
}
void texture_resident_flush(Texture* tex) {
    if (!tex || !tex->sparse) return;
    std::lock_guard lk(sparse_mu);
    sparse_flush(tex);
}

bool texture_resident(Texture* tex, uint32_t level, uint32_t first, uint32_t count, uint32_t x, uint32_t y, uint32_t w,
                      uint32_t h) {
    if (!tex || !tex->sparse) return true;
    std::lock_guard lk(sparse_mu);
    bool ok = true;
    for (uint32_t layer = first; layer < std::min(tex->layers, first + std::max(1u, count)); layer++)
        ok &= sparse_queue(tex, level, layer, x, y, w, h);
    return sparse_flush(tex) && ok;
}

bool texture_resident(Texture* tex, uint32_t first, uint32_t count) {
    if (!tex || !tex->sparse) return true;
    std::lock_guard lk(sparse_mu);
    bool ok = true;
    for (uint32_t layer = first; layer < std::min(tex->layers, first + std::max(1u, count)); layer++)
        for (uint32_t level = 0; level < tex->levels; level++)
            ok &= sparse_queue(tex, level, layer, 0, 0, ~0u, ~0u);
    return sparse_flush(tex) && ok;
}

void texture_rendered(Texture* tex) {
    if (!tex) return;
    if (tex->sparse && !tex->first_render_frame) texture_resident(tex, 0, tex->layers);   // (rendered into: all of it)
    if (!tex->first_render_frame) tex->first_render_frame = frame_no, rendered_targets.push_back(tex);
    // (its storage's range as the GPU's, once a frame: render targets only. Copies and storage writes registered it
    // too, and a texture made later over such storage (MKW's terrain tiles reuse the slots of a pool) was not uploaded
    // and kept the placeholder it was made with)
    if (tex->owned_frame != frame_no) {
        tex->owned_frame = frame_no;
        const uint64_t lo = tex->gpu, hi = tex->gpu + std::max<uint64_t>(tex->guest_size, 1);
        for (auto it = gpu_owned.lower_bound(lo); it != gpu_owned.end() && it->first < hi;)
            it = it->second.first <= hi ? gpu_owned.erase(it) : std::next(it);
        gpu_owned[lo] = {hi, memory_generation()};
        if (gpu_owned.size() > 65536) gpu_owned.erase(gpu_owned.begin());   // (bounded; the oldest addresses go)
    }
}
uint64_t texture_frame_no() {
    return frame_no;
}

namespace {
// A guest GPU address may resolve to another imported buffer later. Keep the views with the actual backing,
// and release them only where memory.cpp already destroys that backing after its recorded users complete.
using TexelViewKey = std::tuple<VkFormat, uint64_t, uint64_t>;   // (format, offset, range)
struct TexelViews {
    std::map<TexelViewKey, VkBufferView> views;
    uint64_t failed_frame = UINT64_MAX;
};
std::mutex texel_views_mu;
std::map<VkBuffer, TexelViews> texel_views;
uint64_t texel_registry_failed_frame = UINT64_MAX;

int texel_kind(VkFormat f) {   // 0 float, 1 unsigned integer, 2 signed integer (the numeric type a shader reads it as)
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_A8B8G8R8_UINT_PACK32: case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16_UINT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32A32_UINT: return 1;
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8_SINT: case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_A8B8G8R8_SINT_PACK32: case VK_FORMAT_A2B10G10R10_SINT_PACK32: case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16_SINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_SINT:
    case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32_SINT: case VK_FORMAT_R32G32B32A32_SINT: return 2;
    default: return 0;
    }
}

/// Without robustness2's nullDescriptor a texel buffer descriptor must not be VK_NULL_HANDLE
/// (VUID-VkWriteDescriptorSet-descriptorType-02995): a view of 16 zero bytes in R32_SFLOAT, R32_UINT or R32_SINT (the
/// numeric type of the format it stands for; uniform and storage texel buffers of all three are mandatory), made once
VkBufferView dummy_texel_view(VkFormat f) {
    static std::mutex mu;
    static VkBuffer buffer = VK_NULL_HANDLE;
    static VmaAllocation alloc = nullptr;
    static VkBufferView views[3]{};
    static bool tried = false;
    std::lock_guard lk(mu);
    if (!tried) {
        tried = true;
        void* mapped = nullptr;
        if (create_buffer(16, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT, true, buffer,
                          alloc, &mapped)) {
            if (mapped) std::memset(mapped, 0, 16);
            const VkFormat formats[3] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT};
            for (int i = 0; i < 3; i++) {
                VkBufferViewCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
                ci.buffer = buffer, ci.format = formats[i], ci.offset = 0, ci.range = 16;
                if (vkCreateBufferView(dev.device, &ci, nullptr, &views[i]) != VK_SUCCESS) views[i] = VK_NULL_HANDLE;
            }
        } else {
            buffer = VK_NULL_HANDLE, alloc = nullptr;
        }
    }
    return views[texel_kind(f)];
}
} // namespace

void release_texel_buffer_views(VkBuffer buffer) {
    // Unpublished imports may also be dropped by the import worker. Never take pools_mu under this lock:
    // the callers that retire a published buffer already hold pools_mu, and gpu_buffer takes it first.
    std::lock_guard lk(texel_views_mu);
    auto it = texel_views.find(buffer);
    if (it == texel_views.end()) return;
    for (const auto& [key, view] : it->second.views)
        if (view) vkDestroyBufferView(dev.device, view, nullptr);
    texel_views.erase(it);   // (a later import may receive the same numerical VkBuffer handle)
}

VkBufferView texel_buffer_view(const nvn_tex* t, const nvn_view* v, bool storage) {
    if (!t || !t->gpu) return VK_NULL_HANDLE;
    const int nf = view_nvn_format(t, v);
    VkFormat f = vk_format(nf).format;
    if (f == VK_FORMAT_UNDEFINED) return VK_NULL_HANDLE;
    {   // (a format the device has no texel buffers of, e.g. some 3-component ones: no view rather than an invalid one; a
        // dummy view where the device has no null descriptor)
        if (!(format_props(f).bufferFeatures & (storage ? VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT : VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT))) {
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
                logf("vk: texel buffer of format %d (%s) not supported by the device: unbound", (int)f, storage ? "storage" : "uniform");
            return dev.null_descriptor ? VK_NULL_HANDLE : dummy_texel_view(f);
        }
    }
    int bw = 1, bh = 1;
    const int bpb = nvn_block_size(nf, &bw, &bh);
    if (bpb <= 0) return VK_NULL_HANDLE;
    const uint64_t size = (uint64_t)std::max(1u, t->d.width) * bpb;
    // (a formatted store into it: textures over that memory are uploaded again once the GPU is done)
    if (storage) texture_gpu_wrote(t->gpu, size);
    // Resolve and stamp even a cache hit: the address can be remapped, and cached-only bindings are not idle.
    BufferRef b = gpu_buffer(t->gpu, size);
    if (!b || b.offset % std::max<uint64_t>(dev.props.limits.minTexelBufferOffsetAlignment, 1)) return VK_NULL_HANDLE;
    const uint64_t range = std::min<uint64_t>(size, (uint64_t)dev.props.limits.maxTexelBufferElements * bpb);
    if (!range) return VK_NULL_HANDLE;
    const TexelViewKey key{f, b.offset, range};
    std::lock_guard lk(texel_views_mu);
    auto owner = texel_views.find(b.buffer);
    if (owner == texel_views.end()) {
        if (texel_registry_failed_frame == frame_no) return VK_NULL_HANDLE;
        try {
            owner = texel_views.try_emplace(b.buffer).first;
        } catch (const std::exception&) {
            texel_registry_failed_frame = frame_no;
            return VK_NULL_HANDLE;
        }
    }
    TexelViews& cache = owner->second;
    auto it = cache.views.find(key);
    if (it != cache.views.end()) return it->second;
    if (cache.failed_frame == frame_no) return VK_NULL_HANDLE;   // (one failed allocation per backing and present)
    VkBufferViewCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
    ci.buffer = b.buffer;
    ci.format = f;
    ci.offset = b.offset;
    ci.range = range;
    VkBufferView view = VK_NULL_HANDLE;
    if (vkCreateBufferView(dev.device, &ci, nullptr, &view) != VK_SUCCESS) {
        cache.failed_frame = frame_no;   // (an error's output handle is undefined; never cache or destroy it)
        return VK_NULL_HANDLE;
    }
    try {
        cache.views.emplace(key, view);
    } catch (const std::exception&) {
        vkDestroyBufferView(dev.device, view, nullptr);   // (only the new, uncommitted view)
        cache.failed_frame = frame_no;
        return VK_NULL_HANDLE;
    }
    return view;
}

// ---- (longshot) VRAM compacted (on by default since the hold lets go of the dead render targets first and of long-unused
// textures only; RX_VK_COMPACT=0: off, as before) ----
// An evicted texture's memory stays inside its 128 MB block, and the driver counts the block whole: after some play 0.6 to
// 1.6 GB of what the driver counts is such free space (3 to 4 GB after the cache was emptied), and evicting more gives
// none of it back. Compacted, the textures that are not render targets have a pool of their own (vk_vma.cpp; the targets have theirs) and, when RX_VK_COMPACT_MB (512)
// or more of it is free space, VMA's defragmentation moves textures out of the sparsely used blocks, a few a pass: a new
// image at the new place, the texels copied on the GPU in the present's upload commands (every draw of the frame is in an
// earlier recording), the texture's views made again when next asked for, and the old image destroyed once the GPU is past
// the copy; then VMA frees the blocks that emptied. While a pass is open nothing of the pool is freed (the images the GPU
// is done with wait a frame or two): VMA holds the allocations it listed.
struct Compact {
    bool in_pass = false;
    uint64_t value = 0;                    // (the recording the open pass's copies are in)
    std::vector<VkImage> old_images;       // (of the open pass: destroyed when the GPU is past them)
    std::vector<VkImageView> old_views;
    uint64_t next_check = 0;               // (the present from which another compaction may start)
    uint64_t moved = 0, moved_bytes = 0, ignored = 0, passes = 0;   // (of the compaction under way)
    uint64_t runs = 0, all_moved = 0, all_freed = 0;
    // (the driver's count over 90% of what it allows, texture_frame's look every 10 presents: vram_compact.h)
    bool pressed = false;
} compact;
const Texture* present_source = nullptr;   // (what the present shows this frame: do_present holds its image and view)
void compact_step(uint64_t completed) {
    if (!compact_on()) return;
    if (compact.in_pass) {
        if (compact.value > completed) return;   // (the GPU has not run the copies yet)
        for (VkImageView v : compact.old_views) vkDestroyImageView(dev.device, v, nullptr);
        for (VkImage i : compact.old_images) vkDestroyImage(dev.device, i, nullptr);   // (their memory is VMA's: the pass's end frees it)
        compact.old_views.clear(), compact.old_images.clear();
        compact.in_pass = false;
        uint64_t freed = 0;
        uint32_t blocks = 0;
        if (!image_compact_pass_end(&freed, &blocks)) {
            compact.all_moved += compact.moved, compact.all_freed += freed;
            logf("vk: VRAM compacted: %llu textures moved (%llu MB) in %llu passes, %llu left where they were; %u blocks (%llu MB) "
                 "given back to the driver; %llu MB in all so far", (unsigned long long)compact.moved,
                 (unsigned long long)(compact.moved_bytes >> 20), (unsigned long long)compact.passes,
                 (unsigned long long)compact.ignored, blocks, (unsigned long long)(freed >> 20),
                 (unsigned long long)(compact.all_freed >> 20));
            compact.next_check = frame_no + vram_compact::rest(compact.pressed);
        }
        return;   // (the images the GPU is done with are freed now, before the next pass opens)
    }
    if (!image_compact_active()) {
        if (frame_no < compact.next_check) return;
        compact.next_check = frame_no + 120;
        static const char* compact_env = getenv("RX_VK_COMPACT_MB");
        uint64_t min_free = 512ull << 20;
        if (compact_env) {
            min_free = (uint64_t)std::max(1, atoi(compact_env)) << 20;
        }
        else {
            uint64_t used_vram = 0, allowed_vram = 0;
            if (vram_usage(&used_vram, &allowed_vram))
                min_free = vram_compact::default_min_free(allowed_vram);
        }
        uint64_t block_bytes = 0, free_bytes = 0, kept = 0;   // (kept: the blocks made at the start, which no run gives back)
        if (!textures_pool_usage(&block_bytes, &free_bytes, &kept) ||
            vram_compact::returnable(block_bytes, free_bytes, kept) < vram_compact::run_from(min_free, compact.pressed))
            return;
        if (!image_compact_begin(64ull << 20, 12)) return;
        compact.runs++, compact.moved = compact.moved_bytes = compact.ignored = compact.passes = 0;
        logf("vk: VRAM compaction %llu begins: the textures' blocks hold %llu MB, %llu MB of it free space",
             (unsigned long long)compact.runs, (unsigned long long)(block_bytes >> 20), (unsigned long long)(free_bytes >> 20));
    }
    // (RX_VK_REAPER: the dead images handed to the reaper are freed before a pass opens, as they were here before: a pass VMA
    // plans with them still in the blocks moves around them and empties fewer. A present or so later; never a wait here)
    if (images_to_reap()) return;
    static thread_local std::vector<ImageMove> moves;
    uint64_t freed_now = 0;
    uint32_t blocks_now = 0;
    if (!image_compact_pass(moves, &freed_now, &blocks_now)) {   // (nothing left to move)
        compact.all_moved += compact.moved, compact.all_freed += freed_now;
        logf("vk: VRAM compaction %llu over: %llu textures moved (%llu MB) in %llu passes, %llu left where they were; %u blocks "
             "(%llu MB) given back to the driver, %llu MB in all so far", (unsigned long long)compact.runs,
             (unsigned long long)compact.moved, (unsigned long long)(compact.moved_bytes >> 20), (unsigned long long)compact.passes,
             (unsigned long long)compact.ignored, blocks_now, (unsigned long long)(freed_now >> 20),
             (unsigned long long)(compact.all_freed >> 20));
        compact.next_check = frame_no + vram_compact::rest(compact.pressed);
        return;
    }
    std::unordered_map<VmaAllocation, Texture*> by_allocation;
    by_allocation.reserve(textures.size());
    for (auto& kv : textures)
        if (kv.second->allocation) by_allocation[kv.second->allocation] = kv.second.get();
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    for (size_t i = 0; i < moves.size(); i++) {
        const auto f = by_allocation.find(moves[i].src);
        Texture* t = f == by_allocation.end() ? nullptr : f->second;   // (none: an image the GPU is not done with)
        const VkImageUsageFlags both = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        bool ok = t && t->image && !t->sparse && t != present_source && t->samples == VK_SAMPLE_COUNT_1_BIT &&
                  (t->usage & both) == both;
        VkImage image = VK_NULL_HANDLE;
        if (ok) {
            VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};   // (as create() made it)
            ci.flags = t->create_flags;
            ci.imageType = t->type;
            ci.format = t->format;
            ci.extent = {t->width, t->height, t->depth};
            ci.mipLevels = t->levels;
            ci.arrayLayers = t->layers;
            ci.samples = t->samples;
            ci.tiling = VK_IMAGE_TILING_OPTIMAL;
            ci.usage = t->usage;
            ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            ok = vkCreateImage(dev.device, &ci, nullptr, &image) == VK_SUCCESS;
            if (ok && !image_compact_bind(i, image)) {
                vkDestroyImage(dev.device, image, nullptr);
                ok = false;
            }
        }
        if (!ok) {
            image_compact_ignore(i);
            compact.ignored++;
            continue;
        }
        if (!cmd) {
            cmd = upload_cmd();
            full_barrier(cmd, "compaction: before the copies");
        }
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {t->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
        static thread_local std::vector<VkImageCopy> regions;
        regions.clear();
        for (uint32_t l = 0; l < t->levels; l++) {
            VkImageCopy r{};
            r.srcSubresource = r.dstSubresource = {t->aspect, l, 0, t->layers};
            r.extent = {std::max(1u, t->width >> l), std::max(1u, t->height >> l), std::max(1u, t->depth >> l)};
            regions.push_back(r);
        }
        vkCmdCopyImage(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, image, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)regions.size(), regions.data());
        compact.old_images.push_back(t->image);
        for (auto& kv : t->views)
            if (kv.second) compact.old_views.push_back(kv.second);
        t->views.clear();   // (made again when next asked for)
        gone_note(t->id);   // (what was resolved to this texture's views is stale)
        t->image = image;
        t->last_value = std::max(t->last_value, rec_value);   // (its end, if it goes now, waits for the copy too)
        compact.moved++, compact.moved_bytes += t->bytes + t->pad;
    }
    compact.passes++;
    if (!cmd) {   // (nothing of this pass could be moved: ended at once)
        uint64_t freed = 0;
        uint32_t blocks = 0;
        if (!image_compact_pass_end(&freed, &blocks)) {
            compact.all_moved += compact.moved, compact.all_freed += freed;
            logf("vk: VRAM compacted: %llu textures moved (%llu MB) in %llu passes, %llu left where they were; %u blocks (%llu MB) "
                 "given back to the driver; %llu MB in all so far", (unsigned long long)compact.moved,
                 (unsigned long long)(compact.moved_bytes >> 20), (unsigned long long)compact.passes,
                 (unsigned long long)compact.ignored, blocks, (unsigned long long)(freed >> 20),
                 (unsigned long long)(compact.all_freed >> 20));
            compact.next_check = frame_no + vram_compact::rest(compact.pressed);
        }
        return;
    }
    full_barrier(cmd, "compaction: after the copies");   // (gone_note named each moved texture as it went: no whole-pass bump)
    compact.in_pass = true, compact.value = rec_value;
}


Texture* texture_for_present(const nvn_tex* t) {
    Texture* tex = texture_get(t, false);
    if ((t && t->output_preview) || (tex && tex->gpu_written)) return present_source = tex, tex;
    Texture* best = nullptr;   // (one rendered at its storage in another description)
    auto [a, b] = by_gpu.equal_range(t ? t->gpu : 0);
    for (auto it = a; it != b; ++it)
        if (it->second->gpu_written && (it->second->aspect & VK_IMAGE_ASPECT_COLOR_BIT) &&
            (!best || it->second->last_frame > best->last_frame))
            best = it->second;
    present_source = best ? best : tex;
    return best ? best : tex;
}

int64_t texture_readback_wait_ns() { return st_readback_wait_ns.load(std::memory_order_relaxed); }
int64_t texture_frame_ns() { return st_texture_frame_ns.load(std::memory_order_relaxed); }
void texture_step_times(int64_t out[8]) {   // (TEST, local: the frame trace's last columns)
    for (int i = 0; i < 8; i++) out[i] = step_ns[i].load(std::memory_order_relaxed);
}
/// (TEST, local: the frame trace's pool columns) taken from the pool, made new, put back, gone idle, destroyed in all (running
/// counts); what the pool holds now, what waits for the GPU, whether a compaction pass is open
void texture_pool_counts(int64_t out[9]) {
    out[0] = (int64_t)st_pool_reused, out[1] = (int64_t)st_pool_made, out[2] = (int64_t)st_pool_put, out[3] = (int64_t)st_pool_idle_out;
    out[4] = (int64_t)st_pool_destroyed, out[5] = (int64_t)pool_n, out[6] = (int64_t)dead.size(), out[7] = compact.in_pass ? 1 : 0;
    out[8] = (int64_t)st_pool_resized;
}
uint64_t linux_pressure_shortfall(uint64_t* seq) {
    if (seq) *seq = linux_shortfall_seq.load(std::memory_order_acquire);
    return linux_shortfall_bytes.load(std::memory_order_acquire);
}

// (longshot) The dead transient targets' switches (texture_frame's eviction reads them too).
// RX_VK_DEAD_EVERY=<presents>: how often they are looked for; RX_VK_DEAD_MIN_UNUSED
uint64_t dead_every_presents() {
    static const uint64_t n = getenv("RX_VK_DEAD_EVERY") ? (uint64_t)std::max(1, atoi(getenv("RX_VK_DEAD_EVERY"))) : 1;
    return n;
}
uint64_t dead_min_unused_presents() {
    static const uint64_t n = getenv("RX_VK_DEAD_MIN_UNUSED") ? (uint64_t)std::max(1, atoi(getenv("RX_VK_DEAD_MIN_UNUSED"))) : 1;
    return n;
}
uint64_t st_dead_swept, st_dead_swept_bytes;   // (let go by dead_targets_sweep so far)
/// (longshot) Between the eviction's passes (dead_targets.h): the render targets another target was drawn over from another start
/// since their last use (target_overlap.h), unused for RX_VK_DEAD_MIN_UNUSED presents and done with by the GPU, let go as that
/// pass lets them go: nothing written back (their storage is another target's now), their images to the target pool
void dead_targets_sweep(uint64_t completed) {
    const uint64_t need = dead_min_unused_presents();
    // (the rendered textures only, rendered_targets, not the whole table; the list kept from one present to the next; and
    // nothing sorted at a present where no target is a candidate)
    static thread_local std::vector<const Texture*> by_start;
    by_start.clear();
    uint64_t longest = 0;
    bool any = false;
    const auto candidate = [&](const Texture* tex) {
        return tex->gpu_written && tex->first_render_frame && tex->guest_size &&
               dead_targets::candidate(frame_no, tex->last_frame, need, tex->last_value, completed,
                                       tex->levels == 1 && tex->layers == 1 && tex->depth <= 1);
    };
    for (const Texture* t : rendered_targets) {
        if (!t->gpu_written || !t->first_render_frame || !t->guest_size) continue;
        by_start.push_back(t), longest = std::max(longest, t->guest_size);
        any = any || candidate(t);
    }
    if (!any) return;
    std::sort(by_start.begin(), by_start.end(), [](const Texture* a, const Texture* b) { return a->gpu < b->gpu; });
    std::unordered_set<Texture*> gone;
    for (Texture* tex : rendered_targets) {
        if (!candidate(tex)) continue;
        const uint64_t lo = tex->gpu, hi = tex->gpu + tex->guest_size;
        auto it = std::lower_bound(by_start.begin(), by_start.end(), lo > longest ? lo - longest : 0,
                                   [](const Texture* t, uint64_t g) { return t->gpu < g; });
        // (their parts added up, as drawn_over_shifted's: several may each cover less than an eighth of it and together more)
        target_overlap::ShiftedOver over{lo, hi, tex->last_frame};
        for (; it != by_start.end() && (*it)->gpu < hi; ++it) {
            if (*it == tex) continue;
            over.add((*it)->gpu, (*it)->gpu + (*it)->guest_size, (*it)->last_written);
            if (over.an_eighth()) {
                gone.insert(tex);
                break;
            }
        }
    }
    if (gone.empty()) return;
    for (Texture* tex : gone) {   // (each one's end as the eviction's)
        st_dead_swept++, st_dead_swept_bytes += tex->bytes;
        watch_tex(tex, "evicted");
        if (remade_log_on()) {
            std::lock_guard lk(evicted_mu);
            if (evicted_at.size() > 20000) evicted_at.clear();   // (a bound: only the recent ones matter)
            evicted_at[evicted_key(tex->gpu, tex->width, tex->height, tex->nvn_format)] = {frame_no, tex->gpu_written};
            remade.erase(tex);
        }
        destroy_later(*tex, completed);
        forget_gpu(tex);
    }
    rendered_forget(gone);
    for (auto it = textures.begin(); it != textures.end() && !gone.empty();) {
        if (gone.erase(it->second.get())) it = textures.erase(it);
        else ++it;
    }
}

/// (longshot) the images that served a new texture's blocks (astc_reuse.h): gone, unless their own texture was used since
void reuse_retire_frame() {
    std::vector<Texture*> went;
    {
        std::lock_guard lk(reuse_mu);
        went.swap(reuse_gone);
    }
    std::unordered_set<const Texture*> gone;
    for (Texture* t : went) {
        if (!t->reuse_retire) continue;
        t->reuse_retire = false;
        watch_tex(t, "its image served a new texture: retired");
        if (!t->gpu_written && t->uploaded) {   // (made again here, it is uploaded from memory: evict's)
            std::lock_guard lk(from_memory_mu);
            if (evicted_from_memory.size() > 50000) evicted_from_memory.clear();
            evicted_from_memory[evicted_key(t->gpu, t->width, t->height, t->nvn_format)] = t->guest_size;
        }
        destroy_later(*t, completed_value);   // (the newest the GPU finished, as texture_frame last read it)
        forget_gpu(t);
        gone.insert(t);
    }
    st_reuse_retired += (int64_t)gone.size();
    rendered_forget(gone);
    for (auto it = textures.begin(); it != textures.end() && !gone.empty();) {
        if (gone.erase(it->second.get())) it = textures.erase(it);
        else ++it;
    }
}
/// (longshot) A finalized texture the GPU drew whose storage the game has since written over (the CPU wrote three quarters of it or more
/// since it was finalized): its image holds nothing that memory holds on the console, and a draw that sampled it would take the
/// memory's contents over its texels (texture_touch). Only a texture of 32 KB or more: a small one shares its pages with what the game
/// keeps beside it, and a write there says nothing of it. The eviction pass lets such a texture go whatever the budget says
bool finalized_obsolete(const Texture& t) {
    if (!t.released || !t.gpu_written || t.gpu >= (1ull << 40) || t.guest_size < (32ull << 10)) return false;
    static thread_local Ranges written;
    written.clear();
    memory_written_runs(t.guest, t.guest_size, t.release_gen, written);
    uint64_t sum = 0;
    for (const auto& r : written) sum += r.second - r.first;
    return sum * 4 >= t.guest_size * 3;
}
void texture_complete_writes(uint64_t completed) {
    std::lock_guard lk(gpu_wrote_mu);
    gpu_wrote.complete(completed);
}
// (mem_account.h) What this file's pools hold, for the budget line: counted when the line is made, every 600 presents, on the
// thread the textures, the dead list and the staging arenas are this file's own on, so nothing that makes or frees one counts
// anything more. A texture is what the budget counts it as (bytes + pad); the images create_image made that are no texture's,
// pooled or dead (the upscalers', the frame generation's, the scratch ones) are what is left of images_made_bytes. Each image
// and buffer is counted in the ledger its allocation's memory is in: with VRAM full, create_image puts an image in system memory
namespace {
struct {
    uint64_t textures, sparse, in_ram, blocks_free;
    // (by ledger. dead_to_pool: the part of `dead` that is render targets' images, which go to the pool, not back to the driver)
    uint64_t pool[mem_account::kLedgers], dead[mem_account::kLedgers], dead_to_pool[mem_account::kLedgers],
        dead_buffers[mem_account::kLedgers], other_images[mem_account::kLedgers], staging[mem_account::kLedgers],
        other_buffers[mem_account::kLedgers], reaper_images[mem_account::kLedgers], reaper_buffers[mem_account::kLedgers];
} account{};
void account_sample(uint64_t block_free) {
    using mem_account::RAM, mem_account::VRAM;
    account = {};
    account.blocks_free = block_free;
    const auto ledger = [](VmaAllocation a) { return allocation_in_vram(a) ? VRAM : RAM; };
    uint64_t known[mem_account::kLedgers]{};   // (the allocations of the images counted here)
    for (const auto& kv : textures) {
        const Texture& t = *kv.second;
        known[ledger(t.allocation)] += allocation_size(t.allocation);
        for (const auto& attachment : t.block_attachments) known[ledger(attachment.allocation)] += allocation_size(attachment.allocation);
        // (a sparse texture's chunks are device memory of its own, outside the blocks)
        (t.sparse ? account.sparse : t.allocation && ledger(t.allocation) == RAM ? account.in_ram : account.textures) += t.bytes + t.pad;
    }
    {
        std::lock_guard lk(pool_mu);
        for (const Pooled& p : pool_by_age)   // (every pooled image: target_pool indexes into this list)
            account.pool[ledger(p.allocation)] += p.bytes, known[ledger(p.allocation)] += allocation_size(p.allocation);
    }
    // (the dead list: off the textures' count since destroy_later, and the driver's until the GPU is past them. An entry is an
    // image, an image with the BC3 scratch's output buffer, or a buffer alone: a timed-out write-back's, in host memory)
    for (const Dead& d : dead) {
        if (d.allocation) {
            const auto l = ledger(d.allocation);
            const uint64_t image = allocation_size(d.allocation);
            known[l] += image;
            account.dead[l] += d.pool_key ? d.bytes : image;
            if (d.pool_key) account.dead_to_pool[l] += d.bytes;
        }
        if (d.buffer_alloc) account.dead_buffers[ledger(d.buffer_alloc)] += allocation_size(d.buffer_alloc);
    }
    // (handed from the dead list to the reaper's thread, vk_vma.cpp destroy_image_later: still the driver's until it frees them)
    reaper_bytes(account.reaper_images, account.reaper_buffers);
    for (const auto l : {VRAM, RAM}) known[l] += account.reaper_images[l];
    for (const auto l : {VRAM, RAM}) {
        const uint64_t made = images_made_bytes(l == VRAM);
        account.other_images[l] = made - std::min(made, known[l]);
        // (more counted here than was made: the figure above is wrong, and 0 would hide it. An image another thread made or
        // destroyed between the walk and this look can do it too, by that image's size)
        static bool told;
        if (known[l] > made && !told) {
            told = true;
            logf("vk: budget: the images counted (%llu MB) are more than the images made (%llu MB) in %s: \"other images\" is not exact",
                 (unsigned long long)(known[l] >> 20), (unsigned long long)(made >> 20), l == VRAM ? "VRAM" : "system memory");
        }
    }
    // (and the buffers the same way: every buffer made, less the staging arenas, the dead ones and what vk_memory.cpp's pools
    // report (mirrors, shadows, captures). What is left are the buffers no pool counts: the recordings' streams ("vk: the
    // recording streams:" has their size), the guest page table, the mesh rings and the mesh fallback's buffers, the stamp
    // blocks, the BC3 scratch's output, a mirror or a shadow given back until the GPU is past it, a capture being taken. The
    // other pools' figures are a submission old and the spares' a look old, so this one sways by what those moved since)
    uint64_t known_buffers[mem_account::kLedgers]{};
    for (const Arena& a : arenas) account.staging[ledger(a.allocation)] += a.size, known_buffers[ledger(a.allocation)] += allocation_size(a.allocation);
    memory_account_sample(known_buffers);
    for (const auto l : {VRAM, RAM}) {
        const uint64_t made = buffers_made_bytes(l == VRAM),
                       known_here = known_buffers[l] + account.dead_buffers[l] + account.reaper_buffers[l];
        account.other_buffers[l] = made - std::min(made, known_here);
    }
}
/// (mem_account.h, the arbiter) The pools asked for what they keep for reuse, and a line for each that gave: the lot at most
/// once a second. Returns the bytes given
uint64_t account_ask(const mem_account::Ask& a, uint64_t used, uint64_t allowed) {
    mem_account::Given gave[mem_account::kMaxPools];
    size_t n = 0;
    const uint64_t given = mem_account::ask(mem_account::registry(), a, gave, std::size(gave), &n);
    static std::chrono::steady_clock::time_point told_at;
    const auto now = std::chrono::steady_clock::now();
    if (n && now - told_at >= std::chrono::seconds(1)) {
        told_at = now;
        for (size_t i = 0; i < n; i++) {
            char line[160];
            mem_account::format_given(line, sizeof line, gave[i], used, allowed);
            logf("%s", line);
        }
    }
    return given;
}
[[maybe_unused]] const bool account_registered = [] {
    using namespace mem_account;
    static const auto both = [](Report& r, const uint64_t* bytes) { r.held[VRAM] = bytes[VRAM], r.held[RAM] = bytes[RAM]; };
    add({"textures", [](Report& r) { r.held[VRAM] = account.textures; }});
    add({"sparse textures", [](Report& r) { r.held[VRAM] = account.sparse; }});
    add({"textures in RAM", [](Report& r) { r.held[RAM] = account.in_ram; }});
    // (the arbiter's order, mem_account.h: the capture spares 1 (vk_memory.cpp), the staging arenas 2, the blocks' free space 3,
    // the target pool 4. The first two hold buffers nothing uses; the blocks are returned by a compaction, which costs copies;
    // the pool's images are taken again every frame, so it goes last and by its own bound)
    // (asked last: pool_trim's own bound for the level, which texture_frame has just set (pool_pressure: half the pool's cap
    // near the allowance, none of it at the driver's line), now and not at the next present. What that destroyed)
    add({"target pool", [](Report& r) { both(r, account.pool), r.reuse[VRAM] = account.pool[VRAM], r.reuse[RAM] = account.pool[RAM]; }, 4,
         [](const Ask& a) -> uint64_t {
             if (a.ledger != VRAM) return 0;
             const auto held = [] {
                 std::lock_guard lk(pool_mu);
                 return pool_bytes;
             };
             const uint64_t before = held();
             pool_trim();
             return before - std::min(before, held());
         }});
    add({"dead images", [](Report& r) {
             both(r, account.dead);
             for (const auto l : {VRAM, RAM}) r.reuse[l] = account.dead_to_pool[l], r.pending[l] = account.dead[l] - account.dead_to_pool[l];
         }});
    add({"dead buffers", [](Report& r) { both(r, account.dead_buffers), r.pending[VRAM] = r.held[VRAM], r.pending[RAM] = r.held[RAM]; }});
    // (RX_VK_REAPER: the dead images and their buffers on the reaper's queue, freed a moment later. Asked first: nothing uses
    // them, and freeing them here is what main did at every present; the reaper's own cap bounds them below the lines)
    add({"reaper",
         [](Report& r) {
             for (const auto l : {VRAM, RAM}) r.held[l] = r.pending[l] = account.reaper_images[l] + account.reaper_buffers[l];
         },
         1, [](const Ask& a) -> uint64_t { return reap_now(a.want, a.ledger == VRAM); }});
    add({"other images", [](Report& r) { both(r, account.other_images); }});
    add({"other buffers", [](Report& r) { both(r, account.other_buffers); }});
    // (the textures' blocks' free space goes back to the driver by a compaction: as the pressure begins, compact_step looks at
    // its next present and not after its rest, and starts a run if there is enough to return (192 MB while pressed). Its own
    // lines say what came back, later: nothing is given at once)
    add({"blocks free", [](Report& r) { r.held[VRAM] = r.reuse[VRAM] = account.blocks_free; }, 3,
         [](const Ask& a) -> uint64_t {
             if (a.ledger == VRAM && a.entered && compact_on()) compact.next_check = std::min(compact.next_check, frame_no);
             return 0;
         }});
    // (the idle arenas, where they are VRAM behind the BAR: all of them go at the next present, as when Linux's memory pressure
    // begins (linux_entered_pressure, texture_frame's look at the arenas). Once as the pressure begins: an arena made again is
    // 9 to 108 ms of vkAllocateMemory, and the loads that follow need two)
    add({"staging", [](Report& r) { both(r, account.staging); }, 2,
         [](const Ask& a) -> uint64_t {
             if (a.ledger != VRAM || !a.entered) return 0;
             uint64_t idle = 0;
             for (const Arena& x : arenas)
                 if (x.value <= completed_value && x.value != rec_value && allocation_in_vram(x.allocation)) idle += x.size;
             if (idle) linux_entered_pressure = true;
             return idle;
         }});
    return true;
}();
}  // namespace
void texture_frame(uint64_t completed) {
    const TraceTimer timed{st_texture_frame_ns};   // (longshot: the frame trace's tex_frame_ms)
    burst_frame_close();
    upload_frame_report();
    frame_upload_ns.store(0, std::memory_order_relaxed);
    if (const int64_t d = st_deferred_frame.exchange(0, std::memory_order_relaxed)) {
        // (the views and descriptor sets resolved to the placeholder this frame go stale, as when textures go: the next frame
        // looks those textures up again, and uploads them)
        gone_note(kGoneAll);   // (every view resolved to a placeholder goes stale)
        static int told;
        if (told++ < 40 || st_deferred.load(std::memory_order_relaxed) % 2000 < d)
            logf("vk: %lld new ASTC textures left for a later frame (this frame's uploads took their budget; %lld so far)", (long long)d,
                 (long long)st_deferred.load(std::memory_order_relaxed));
    }
    completed_value = completed;
    texture_complete_writes(completed);
    frame_no++;
    drawn_writeback_frame();
    rewrites_frame();   // (RX_VK_LOG_REWRITES)
    reuse_retire_frame();
    if (memory_refused.exchange(false, std::memory_order_relaxed)) {   // (#1879: a submit refused for want of memory since the last)
        linux_entered_pressure = true;   // (every idle arena below)
        const uint64_t freed = reap_now(UINT64_MAX, true) + reap_now(UINT64_MAX, false);
        static int told;
        if (told++ < 16)
            logf("vk: a submit was refused for want of memory: the idle staging arenas freed, and %llu MB of the reaper's queue",
                 (unsigned long long)(freed >> 20));
    }
    {   // (staging arenas the GPU is done with, beyond 256 MB of them, and any over 64 MB: freed. A scene change stages
        // hundreds of MB to over a GB (the course maps, the terrain's ASTC arrays), and that host memory was kept)
        // (longshot: 128 where the driver allows 6.5 GB or less, device_policy.h staging_keep. On one with resizable BAR the arenas are VRAM: VMA puts a host-visible buffer the GPU
        // reads (STORAGE: the ASTC decoder) in DEVICE_LOCAL | HOST_VISIBLE. The RTX 4050 Laptop held 4 idle ones, 256 MB of 5.7 GB, through
        // all of Kokiri gameplay, which stages ~nothing. A scene change makes the ones it needs again. RX_VK_STAGING_KEEP_MB=256: as before)
        static const uint64_t keep = []() -> uint64_t {
            if (dev.policy.set(Sw::staging_keep)) return dev.policy.staging_keep;   // (the switch, read by device_policy_env)
            uint64_t used = 0, allowed = 0;   // (the rule, asked with the allowance now, at the first frame)
            return policy_rule::staging_keep(vram_usage(&used, &allowed) ? allowed : 0);
        }();
        uint64_t kept = 0;
        for (auto it = arenas.begin(); it != arenas.end();) {
            const bool idle = it->value <= completed && it->value != rec_value;
            // (and one nothing has staged into for 30 s, or 5 s while Linux memory pressure is on: Kokiri Forest's gameplay stages
            // next to nothing, and the 64 MB it kept are VRAM behind the BAR. A load makes one again. RX_VK_STAGING_IDLE_S=<n>:
            // that long, 0: kept)
            static const int64_t idle_s = getenv("RX_VK_STAGING_IDLE_S") ? atoll(getenv("RX_VK_STAGING_IDLE_S")) : 30;
            // always keep at least 2 warm arenas against stale eviction, against 70-108 ms vkAllocateMemory stalls
            const int64_t wait_s = linux_pressed && idle_s > 5 ? 5 : idle_s;
            const bool stale = wait_s && std::chrono::steady_clock::now() - it->used_at > std::chrono::seconds(wait_s);
            // (and up to 256 MB of the ones staged into in the last 2 s stay whatever `keep` is. An arena is free for reuse once
            // the GPU finished the submission that read it, which is after the next frame staged, so frames that stage in a row
            // need two taking turns: with 64 MB kept, the idle one of the two went at every second present and the frame after
            // made a 64 MB arena again, 9 to 41 ms each on a Steam Deck. RX_VK_STAGING_BURST_S=<n>: that long, 0: as before)
            // (not while VRAM is near what the driver allows, pool_pressed as the frame before left it: on a card of 6.5 GB or
            // less the arenas are VRAM behind the BAR, and the 192 MB more would come off what the load's textures need)
            static const int64_t burst_s = getenv("RX_VK_STAGING_BURST_S") ? atoll(getenv("RX_VK_STAGING_BURST_S")) : 2;
            const bool recent = burst_s && !pool_pressure && std::chrono::steady_clock::now() - it->used_at < std::chrono::seconds(burst_s);
            const uint64_t allowed = recent ? std::max<uint64_t>(keep, 256ull << 20) : keep;
            if (idle && (it->size > kArena || kept + it->size > allowed || (stale && kept + it->size > kArena * 2) || linux_entered_pressure)) {
                destroy_buffer(it->buffer, it->allocation);
                it = arenas.erase(it);
                continue;
            }
            kept += it->size;
            ++it;
        }
        linux_entered_pressure = false;
        if (frame_no % 600 == 0) {   // (the process's memory now and then: what the host side holds)
            PROCESS_MEMORY_COUNTERS_EX pm{};
            pm.cb = sizeof pm;
            if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm))
                logf("vk: memory: working set %llu MB (peak %llu), private %llu MB, staging %llu MB in %zu arenas, %llu made "
                     "so far (%.0f ms)",
                     (unsigned long long)(pm.WorkingSetSize >> 20), (unsigned long long)(pm.PeakWorkingSetSize >> 20),
                     (unsigned long long)(pm.PrivateUsage >> 20), (unsigned long long)(kept >> 20), arenas.size(),
                     (unsigned long long)st_arena_makes.load(), st_arena_make_ns.load() / 1e6);
        }
#if defined(__linux__) && defined(__GLIBC__)
        // (glibc's heap: what its arenas hold free. The shader translation and the loads' decoding leave their garbage spread through
        // the threads' arenas (26 of them, 1.2 GB resident in Kokiri Forest, the heap's growth in a scene change never going back),
        // and a free chunk's pages are the process's till malloc_trim gives them to the system. Asked on a worker, when 192 MB or
        // more are free (64 MB while Linux memory pressure is on): RX_VK_MALLOC_TRIM_MB=<n>: that many, 0: never; the "vk: malloc:"
        // line says what the arenas hold)
        // (every 1800 presents, or every 600 while Linux memory pressure is on so long as the last trim gave at least 16 MB back
        // or free arena bytes grew by 64 MB since it: mallinfo2 walks every free chunk of every arena with its lock held)
        {
            static std::atomic<bool> busy{false};
            static std::atomic<uint64_t> last_unreturned_mb{0};
            const bool log_malloc = frame_no % 1800 == 0;
            const bool check_malloc = log_malloc || (linux_pressed && frame_no % 600 == 0);
            if (check_malloc && !busy.exchange(true)) {
                const bool pressed_now = linux_pressed;
                jobs_push([log_malloc, pressed_now] {
                    static const uint64_t trim_mb = getenv("RX_VK_MALLOC_TRIM_MB") ? (uint64_t)atoll(getenv("RX_VK_MALLOC_TRIM_MB")) : 192;
                    const uint64_t threshold_mb = pressed_now && trim_mb > 64 ? 64 : trim_mb;
                    const struct mallinfo2 a = mallinfo2();
                    const uint64_t free_mb = (uint64_t)(a.fordblks >> 20);
                    const uint64_t prev_unreturned = last_unreturned_mb.load(std::memory_order_relaxed);
                    PROCESS_MEMORY_COUNTERS_EX pm0{};
                    pm0.cb = sizeof pm0;
                    K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm0, sizeof pm0);
                    if (log_malloc)
                        logf("vk: malloc: %llu MB in arenas (%llu MB in use, %llu MB free, %llu MB of it at the tops), %llu MB mapped",
                             (unsigned long long)(a.arena >> 20), (unsigned long long)(a.uordblks >> 20), (unsigned long long)free_mb,
                             (unsigned long long)(a.keepcost >> 20), (unsigned long long)(a.hblkhd >> 20));
                    if (threshold_mb && free_mb >= threshold_mb && (log_malloc || !prev_unreturned || free_mb >= prev_unreturned + 64)) {
                        uint64_t avail0 = 0, swap0 = 0, avail1 = 0, swap1 = 0;
                        linux_pressure::read_meminfo(&avail0, &swap0);
                        const auto t0 = std::chrono::steady_clock::now();
                        malloc_trim(0);
                        PROCESS_MEMORY_COUNTERS_EX pm1{};
                        pm1.cb = sizeof pm1;
                        K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm1, sizeof pm1);
                        linux_pressure::read_meminfo(&avail1, &swap1);
                        const uint64_t now_free_mb = (uint64_t)(mallinfo2().fordblks >> 20);
                        const uint64_t before_mb = (uint64_t)(pm0.PrivateUsage >> 20), after_mb = (uint64_t)(pm1.PrivateUsage >> 20);
                        const uint64_t priv_saved_mb = before_mb > after_mb ? before_mb - after_mb : 0;
                        const uint64_t sys_saved_mb = (avail1 + swap1) > (avail0 + swap0) ? ((avail1 + swap1) - (avail0 + swap0)) >> 20 : 0;
                        const uint64_t saved_mb = std::max(priv_saved_mb, sys_saved_mb);
                        last_unreturned_mb.store(saved_mb < 16 ? now_free_mb : 0, std::memory_order_relaxed);
                        logf("vk: malloc_trim: %llu MB free in the arenas now, the process's private memory %llu -> %llu MB (%.0f ms)",
                             (unsigned long long)now_free_mb, (unsigned long long)before_mb, (unsigned long long)after_mb,
                             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
                    }
                    busy = false;
                });
            }
        }
#endif
    }
    bc_scratch_idle();
    compact_step(completed);   // (longshot, RX_VK_COMPACT: while its pass is open nothing of the textures' pool is freed)
    for (auto it = dead.begin(); it != dead.end();) {
        // Nothing is freed or pooled while a compaction pass is open.
        if (compact.in_pass) break;
        if (it->value > completed) {
            ++it;
            continue;
        }
        if (it->pool_key) {   // (a render target's image: kept for the next of its shape, above)
            dead_pooled_n.fetch_sub(1, std::memory_order_relaxed);
            pool_put(*it);
            it = dead.erase(it);
            continue;
        }
        // (longshot: on the reaper's thread, vk_vma.cpp; a vmaDestroyImage is a millisecond and more on NVIDIA's Windows
        // driver, and a turn of the camera let go of many in one present)
        destroy_image_later(it->image, it->allocation, std::move(it->views), it->buffer, it->buffer_alloc, std::move(it->memory));
        it = dead.erase(it);
    }
    pool_trim();
    {   // (TEST, local, RX_VK_LOG_POOL=<n>: above)
        static const int log_pool = getenv("RX_VK_LOG_POOL") ? atoi(getenv("RX_VK_LOG_POOL")) : 0;
        if (log_pool > 0) {
            struct PoolRow { uint64_t present, reused, resized, made, put, idle, bound, held_n, held_mb, dead_n; bool in_pass, evict; };
            static PoolRow ring[16];
            static uint64_t l_reused, l_resized, l_made, l_put, l_destroyed, l_idle, rows;
            static int logged;
            const uint64_t destroyed = st_pool_destroyed - l_destroyed, idle = st_pool_idle_out - l_idle;
            const PoolRow row{frame_no, st_pool_reused - l_reused, st_pool_resized - l_resized, st_pool_made - l_made, st_pool_put - l_put, idle,
                              destroyed - std::min(destroyed, idle), pool_n, pool_bytes >> 20, dead.size(), compact.in_pass,
                              dead_targets::pass_due(frame_no)};
            l_reused = st_pool_reused, l_resized = st_pool_resized, l_made = st_pool_made, l_put = st_pool_put, l_destroyed = st_pool_destroyed, l_idle = st_pool_idle_out;
            ring[rows++ % 16] = row;
            if (row.made + row.resized >= (uint64_t)log_pool && logged < 80) {
                logged++;
                logf("vk: pool: %llu pool-shaped images made new before present %llu; the presents up to it:", (unsigned long long)(row.made + row.resized),
                     (unsigned long long)row.present);
                for (uint64_t i = rows > 16 ? rows - 16 : 0; i < rows; i++) {
                    const PoolRow& r = ring[i % 16];
                    logf("vk: pool:   present %llu: exact pool reuse %llu, new on pooled memory %llu, new pool allocation %llu, put back %llu, gone idle %llu, gone for the bound %llu; "
                         "the pool holds %llu (%llu MB), %llu wait for the GPU%s%s", (unsigned long long)r.present,
                         (unsigned long long)r.reused, (unsigned long long)r.resized, (unsigned long long)r.made, (unsigned long long)r.put, (unsigned long long)r.idle,
                         (unsigned long long)r.bound, (unsigned long long)r.held_n, (unsigned long long)r.held_mb,
                         (unsigned long long)r.dead_n, r.in_pass ? "; a compaction pass open" : "", r.evict ? "; the eviction's pass" : "");
                }
            }
        }
    }
    // A budget: RX_VK_VRAM_MB of textures (default 4096), and less when the driver says the process is near what it
    // may have (other programs use the GPU too). The least recently used go first, down to 7/8 of it; nothing the
    // last 8 presents used, nothing recorded work may still read.
    static uint64_t own = 0;
    if (!dead_targets::pass_due(frame_no)) {
        // (longshot) the dead transient targets on the presents between the passes (dead_targets.h)
        static const bool evicting = !env_on("RX_VK_NO_EVICT");
        if (dead_targets::sweep_due(frame_no, dead_every_presents(), evicting)) dead_targets_sweep(completed);
        return;
    }
    uint64_t used = 0, allowed = 0, block_free = 0;
    const bool known = vram_usage(&used, &allowed, &block_free);
    // (longshot) RX_VK_VRAM_MB=<MB> for a test, else the one rule by what the driver allows at the first look: texture_budget.h.
    // No menu row or Quality preset sets it
    if (!own) {
        const bool igpu = dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
        const uint64_t a = known ? allowed : 0;
        own = getenv("RX_VK_VRAM_MB") ? (uint64_t)atoi(getenv("RX_VK_VRAM_MB")) << 20
              : igpu                 ? texture_rule::integrated_bytes(a, device_vram_bytes(), astc_on_cpu())
                                     : texture_rule::dedicated_bytes(a, device_vram_bytes(), system_ram_bytes());
        if (!igpu)   // (the integrated GPUs' own line follows, with their caps by the system's RAM)
            logf("vk: texture budget: %llu MB (driver allows %llu MB, the card has %llu MB, RAM %llu MB%s)",
                 (unsigned long long)(own >> 20), (unsigned long long)(a >> 20), (unsigned long long)(device_vram_bytes() >> 20),
                 (unsigned long long)(system_ram_bytes() >> 20), getenv("RX_VK_VRAM_MB") ? ", RX_VK_VRAM_MB" : "");
    }
    {   // (longshot) an integrated GPU's "VRAM" is the system's RAM, dynamically calculated based on system RAM
        static bool told;
        if (!getenv("RX_VK_VRAM_MB") && dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            // Get system RAM
            uint64_t system_ram = 0;
#if defined(_WIN32)
            MEMORYSTATUSEX mem_info{sizeof(MEMORYSTATUSEX)};
            if (GlobalMemoryStatusEx(&mem_info)) {
                system_ram = mem_info.ullTotalPhys;
            }
#else
            // Linux: read from /proc/meminfo
            FILE* f = fopen("/proc/meminfo", "r");
            if (f) {
                char line[256];
                while (fgets(line, sizeof(line), f)) {
                    if (sscanf(line, "MemTotal: %llu kB", (unsigned long long*)&system_ram) == 1) {
                        system_ram *= 1024;
                        break;
                    }
                }
                fclose(f);
            }
#endif
            if (system_ram == 0) system_ram = 8ull << 30;  // 8 GB fallback

            // (the rule is device_policy.h's integrated_texture_budget, written once: a third of the RAM left to the game's own
            // allocations (~10 GB), the targets, buffers and pipelines (~3-4 GB), the system and the driver, 8 GB at least
            // and 16 at most; a cap by the RAM, 1.5 GB at 8 to 6 GB past 32, or 6/8 GB past 16 GB when ASTC is decoded on the
            // CPU; 1 GB at least; never more than the share)
            const uint64_t reserved = policy_rule::integrated_reserved(system_ram);   // (for the line below)
            const uint64_t driver_budget = own;  // (its share of the allowance: texture_budget.h integrated_bytes)
            own = policy_rule::integrated_texture_budget(driver_budget, system_ram, astc_on_cpu());

            if (!told) {
                told = true;
                // (the allowance itself, not the share: "driver allows 3331 MB" was read as the limit on a UHD Graphics allowed 7402)
                logf("vk: integrated GPU texture budget: %llu MB (system RAM %llu GB, driver allows %llu MB, its share for textures "
                     "%llu MB, reserved %llu GB)",
                     (unsigned long long)(own >> 20),
                     (unsigned long long)(system_ram >> 30),
                     (unsigned long long)((known ? allowed : 0) >> 20),
                     (unsigned long long)(driver_budget >> 20),
                     (unsigned long long)(reserved >> 30));
            }
        }
    }
    uint64_t budget = own;
    // (near what the driver allows, the budget shrinks by the excess. The excess of what the process holds less the
    // free space in VMA's blocks, which new images take before VMA makes a block: measured by the blocks alone, the
    // textures evicted freed space inside them that the driver still counted, so 10 frames later the budget shrank
    // again. A race kept ~3 GB free in the blocks and evicted and made again ~100 textures every 10 frames (1300 at
    // once), mostly the same ones: their uploads and decodes, and images made empty in the frame that drew them.
    // The driver's own count near its limit (97%) still squeezes, whatever the blocks hold free: then it pages VRAM
    // out)
    uint64_t evict_to = 0;   // (longshot, RX_VK_COMPACT: what an eviction goes down to when the compaction's hold set the budget)
    uint64_t hold_to = 0;    // (and what the hold's own line asks for, when the reserve's lower line set evict_to)
    // (longshot, RX_VK_COMPACT) compacted, only the textures' pool's free space comes off the count: vram_compact.h
    uint64_t reclaim = block_free;
    bool kept_only = false;   // (the textures' pool is down to its blocks made at the start: the 97% rule, below)
    if (compact_on()) {
        uint64_t pool_blocks = 0, pool_free = 0, pool_kept = 0;
        if (textures_pool_usage(&pool_blocks, &pool_free, &pool_kept)) reclaim = vram_compact::reclaimable(block_free, pool_free);
        kept_only = vram_compact::kept_only(pool_blocks, pool_kept);
    }
    const uint64_t pressure = used - std::min(used, reclaim);
    static uint64_t dynamic_floor = 0;
    static uint64_t floor_frame = 0;
    if (known) {
        if (!dynamic_floor || frame_no - floor_frame >= 30) {
            floor_frame = frame_no;
            dynamic_floor = vram_compact::update_dynamic_floor(dynamic_floor, total_bytes, used, pressure, allowed);
        }
    }
    // (and none past the allowance: vram_compact::floor_within)
    const uint64_t active_floor = known ? vram_compact::floor_within(dynamic_floor, used, allowed) : 0;
    // (mem_account.h, the arbiter: one look at the lines for every pool. They are the ones read here before: pressure past 90%
    // of the allowance, level 1, the 90% rule's below and the target pool's "halve"; the count past 97% of it, level 2, the
    // pool's "flush". At level 0, a card with room, nothing more happens here than before)
    static mem_account::Arbiter arbiter;
    const mem_account::Ask asked = arbiter.look(pressure, used, known ? allowed : 0);
    pool_pressure = (uint8_t)asked.level;   // (target pool: halve at >90%, flush at >97%)
    compact.pressed = known && vram_compact::pressed(used, allowed);   // (longshot, RX_VK_COMPACT: by the driver's own count)
    // (and near the allowance the pools that keep memory only to use it again are asked for it, in their order, before the
    // textures: what they let go of comes off the excess the 90% rule takes from the cache, so a texture the game is using
    // stays while a spare buffer or a pooled image can go in its place. Not off the 97% rule's: there the driver is about to
    // page, and space freed inside a block is not yet the driver's)
    const uint64_t given_back = asked.level ? account_ask(asked, used, allowed) : 0;
    const uint64_t size_budget = budget;   // (the budget as a size, before the rules that take an excess off it: rule_target)
    ahead_budget_bytes = size_budget;      // (and what textures prepared ahead stay under: texture_ahead_run)
    if (known && pressure > allowed / 10 * 9) {
        const uint64_t over = pressure - allowed / 10 * 9;
        if (const uint64_t left = over - std::min(over, given_back)) {
            const uint64_t damped_over = policy_rule::squeeze_damped(allowed) ? (left / 4) : left;   // (device_policy.h: under 8 GB allowed)
            uint64_t cut = total_bytes > damped_over ? total_bytes - damped_over : 0;
            if (active_floor && cut < active_floor) cut = active_floor;
            budget = std::min(budget, cut);
        }
    }
    // (longshot, RX_VK_COMPACT: what the budget's size and the 90% rule ask on their own, before the hold's lines; the 97%
    // rule's joins it below. The hold's pass lets a texture unused for the eviction's own time go for that part, as a pass
    // does with the compaction off: vram_compact::hold_target)
    uint64_t rule_budget = budget;
    // (longshot, RX_VK_COMPACT) and the count is held at RX_VK_COMPACT_PCT% of the allowance (85; 50 to 90), as it will be once a
    // run has given the textures' free space back: within ~0.8 GB of what the driver allows the GPU's frame went from ~10 ms
    // to 13-20 ms with the shadows in place, the driver itself short. At 85 a five-minute walk across Hyrule Field at DLAA
    // 2560 x 1440 had 11-12 frames over 33 ms against 34-49 uncompacted, and a 1% low of 37 fps against 26-29
    if (compact_on() && known) {
        static const uint64_t pct = getenv("RX_VK_COMPACT_PCT") ? (uint64_t)std::clamp(atoi(getenv("RX_VK_COMPACT_PCT")), 50, 90) : 80;
        const uint64_t held = vram_compact::held_budget(budget, total_bytes, used, reclaim, allowed, pct, active_floor);
        // (and what goes is the excess alone. Over its budget the cache is cut to 7/8 of it, below: right for a budget that
        // is a size, but this one is "what the textures hold less the excess", crossed again whenever anything else grows.
        // A recorder started beside the game took 0.8 GB of the allowance as play came back from 14 s in the menu: the line
        // was crossed on five passes in a row and the cache went from 4,589 to 2,312 MB (0.875^5) in two seconds, the
        // textures made again at 65-90 ms a frame)
        if (held < budget) budget = evict_to = held;
        hold_to = evict_to;
        // (and a reserve under that line, for the textures long unused: vram_compact.h. The pass below takes the others only
        // for the hold's own line)
        const uint64_t reserve = vram_compact::held_budget(budget, total_bytes, used, reclaim, allowed, vram_compact::reserve_pct(pct), active_floor);
        if (reserve < budget) budget = evict_to = reserve;
    }
    // (longshot) not while VMA's blocks hold an eighth of the allowance free: new images take that space, the driver's count does
    // not grow, and what this evicts frees space inside the blocks that the count keeps, so ten frames later it squeezes again: a
    // ratchet, which can empty a paused game's textures in seconds (made again on unpausing, the HUD's drawn ones with them).
    // But only while the count is within the allowance: past it the driver pages VRAM out to system memory, whatever room the
    // blocks hold, and the frame rate collapses
    // (longshot) an integrated GPU's line is lower: vram_compact::count_line_pct
    static const bool integrated = dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    static const uint64_t line_pct = vram_compact::count_line_pct(integrated);
    const uint64_t count_line = allowed / 100 * line_pct;
    // (longshot) and on an integrated GPU no room in the blocks spares it: vram_compact::room_in_blocks
    const bool room_in_blocks = vram_compact::room_in_blocks(used, allowed, block_free, integrated);
    // (nor while the textures' pool is down to its blocks made at the start, which VMA keeps: what this evicts would leave the
    // count as it is, the same ratchet, past the allowance too: there no eviction can bring the count down)
    if (known && !kept_only && used > count_line && !room_in_blocks) {
        const uint64_t over = used - count_line;
        const uint64_t floor = vram_compact::count_line_floor(used, allowed);   // (none past the allowance)
        uint64_t cut = total_bytes > over ? total_bytes - over : 0;
        if (floor && cut < floor) cut = floor;
        budget = std::min(budget, cut);
        rule_budget = std::min(rule_budget, cut);
    }
    // (longshot) and pipelines the driver refused for want of memory since the last look: what vram_compact::short_excess asks,
    // whatever the count said
    static int64_t refusals_seen = 0;
    const int64_t refusals = st_driver_out_of_memory.load(std::memory_order_relaxed);
    if (refusals != refusals_seen && known && !kept_only) {
        const uint64_t over = vram_compact::short_excess(used, count_line);
        budget = std::min(budget, total_bytes > over ? total_bytes - over : 0);
        rule_budget = std::min(rule_budget, total_bytes > over ? total_bytes - over : 0);
        static int told;
        if (told++ < 8)
            logf("vk: the driver refused %lld pipeline(s) for want of memory (%llu MB used of %llu allowed): the textures give up %llu MB",
                 (long long)(refusals - refusals_seen), (unsigned long long)(used >> 20), (unsigned long long)(allowed >> 20),
                 (unsigned long long)(over >> 20));
    }
    refusals_seen = refusals;
    // (longshot) and Windows' commit limit (RAM plus the page file): every VRAM block is charged to the process's commit, as the
    // guest's memory is. A run on an RTX 4060 Ti held private commit at the VRAM blocks plus ~21 GB the whole way (19.3 GB at the
    // start, 35.4 GB in Hyrule Field with 13.3 GB of blocks); a 16 GB PC with a fixed 32 GB page file reached its 47.9 GB limit at
    // 33.9 GB of its own, a guest commit failed while an area loaded, and the game's next write there crashed it. Below the reserve
    // the shortfall comes off the textures' budget, as the driver's limit's excess does above; the eviction and the compaction
    // then give blocks back. RX_VK_COMMIT_RESERVE_MB=<MB> (default 2560; 0: off)
    {
        uint64_t commit_free = 0;
        const uint64_t reserve = commit_reserve();
        if (reserve && commit_headroom(&commit_free) && commit_free < reserve) {
            const uint64_t over = reserve - commit_free;
            budget = std::min(budget, total_bytes > over ? total_bytes - over : 0);
            rule_budget = std::min(rule_budget, total_bytes > over ? total_bytes - over : 0);
            static uint64_t said_at;
            if (!said_at || frame_no - said_at >= 600) {
                said_at = frame_no;
                logf("vk: commit limit near: %llu MB of commit left (reserve %llu MB); the textures' budget down to %llu MB of "
                     "their %llu MB (RX_VK_COMMIT_RESERVE_MB)", (unsigned long long)(commit_free >> 20),
                     (unsigned long long)(reserve >> 20), (unsigned long long)(budget >> 20),
                     (unsigned long long)(total_bytes >> 20));
            }
        }
    }
    // (longshot) and Linux's memory pressure (MemAvailable and SwapFree in /proc/meminfo, linux_pressure.h): on a 16 GB Steam
    // Deck OLED, guest shmem (5.6-6.0 GB), renderer anon (~3.9 GB) and VMA blocks share the same RAM while RADV's device-heap
    // budget (~7.5 GB) says VRAM is under 50% used. Every VMA block is kernel-pinned in physical RAM and cannot be swapped out,
    // so MemAvailable dipped to 180-440 MB with 4-7 GB in swap and earlyoom killed the game. Sampled every 30 presents (and shared
    // with imports_give_back through linux_shortfall_bytes so no file is read under pools_mu): below RX_VK_AVAIL_RESERVE_MB
    // (1536 MB) of MemAvailable or RX_VK_COMMIT_RESERVE_MB (2560 MB) of MemAvailable + SwapFree, PressureHold cuts the texture
    // budget on an integrated GPU (where VRAM is system RAM) from the texture total at entry (with a 512 MB floor) and steps it
    // back up once both reserves clear by 256 MB (kClearMargin), while the RAM-side valves (staging arenas, capture spares,
    // idle imports, malloc_trim) run on every GPU
    {
        static linux_pressure::PressureHold hold;
        static uint64_t cached_avail = 0, cached_swap = 0, held_cached = UINT64_MAX;
        static bool cached_has_swap = true, have_meminfo = false;
        const uint64_t avail_res = linux_avail_reserve(), commit_res = commit_reserve();
        if (!avail_res && !commit_res) {
            hold = {};
            held_cached = UINT64_MAX;
            linux_pressed = false;
            linux_shortfall_bytes.store(0, std::memory_order_release);
        } else {
            const bool sampled_now = !have_meminfo || frame_no % 30 == 0;
            if (sampled_now)
                have_meminfo = linux_pressure::read_meminfo(&cached_avail, &cached_swap, nullptr, nullptr, &cached_has_swap);
            if (have_meminfo) {
                const bool was_active = hold.active;
                const uint64_t over = linux_pressure::shortfall(cached_avail, cached_swap, avail_res, commit_res, cached_has_swap);
                if (sampled_now) {
                    linux_shortfall_bytes.store(over, std::memory_order_relaxed);
                    linux_shortfall_seq.fetch_add(1, std::memory_order_release);
                    held_cached = hold.step(total_bytes, cached_avail, cached_swap, avail_res, commit_res,
                                            linux_pressure::kClearMargin, linux_pressure::kBudgetFloor, cached_has_swap);
                }
                const uint64_t held = held_cached;
                linux_pressed = hold.active;
                if (hold.active && !was_active) linux_entered_pressure = true;
                if (hold.active) {
                    const bool igpu = dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
                    if (igpu) {
                        budget = std::min(budget, held);
                        rule_budget = std::min(rule_budget, held);
                        pool_pressure = 2;   // (the hold on an iGPU: the pool given back, as before)
                        compact.pressed = true;
                        if (compact.next_check > frame_no + vram_compact::rest(true))
                            compact.next_check = frame_no + vram_compact::rest(true);
                    }
                    static uint64_t said_at;
                    if (!said_at || frame_no - said_at >= 600) {
                        said_at = frame_no;
                        if (igpu)
                            logf("vk: memory pressure on Linux: %llu MB available, %llu MB swap free (reserves %llu / %llu MB); the "
                                 "textures' budget %s %llu MB of their %llu MB (RX_VK_AVAIL_RESERVE_MB, RX_VK_COMMIT_RESERVE_MB)",
                                 (unsigned long long)(cached_avail >> 20), (unsigned long long)(cached_swap >> 20),
                                 (unsigned long long)(avail_res >> 20), (unsigned long long)(commit_res >> 20),
                                 budget < total_bytes ? "down to" : "held at",
                                 (unsigned long long)(budget >> 20), (unsigned long long)(total_bytes >> 20));
                        else
                            logf("vk: memory pressure on Linux: %llu MB available, %llu MB swap free (reserves %llu / %llu MB); the "
                                 "textures' budget is not cut on a discrete GPU (%llu MB, %llu MB in use) (RX_VK_AVAIL_RESERVE_MB, "
                                 "RX_VK_COMMIT_RESERVE_MB)",
                                 (unsigned long long)(cached_avail >> 20), (unsigned long long)(cached_swap >> 20),
                                 (unsigned long long)(avail_res >> 20), (unsigned long long)(commit_res >> 20),
                                 (unsigned long long)(budget >> 20), (unsigned long long)(total_bytes >> 20));
                    }
                }
            }
        }
    }
    // (longshot) the 90% rule and the driver's limit let go of their excess, not of an eighth of the cache: vram_compact.h
    const uint64_t rule_to = vram_compact::rule_target(total_bytes, size_budget, rule_budget);
    // (longshot, RX_VK_COMPACT: the driver's limit asks for more than the reserve does: a pass over the budget, to 7/8 of it, as
    // before. Rare once the reserve is set: its line is 17 points under the limit's and counts the textures' free space out, so
    // this takes more than that share of the allowance free in their pool. Otherwise the pass is the hold's, and what the
    // limit and the 90% rule ask for goes by rule_to)
    if (budget < evict_to) evict_to = hold_to = 0;
    if (frame_no % 600 == 0) {
        vma_log_heaps();
        vma_log_image_blocks();
        {
            std::lock_guard lk(pool_mu);
            logf("vk: render target images: %llu reused from the pool, %llu made new, %llu destroyed from it, %llu reused by size "
                 "(on a pooled one's memory, RX_VK_TARGET_REUSE_MEMORY=%d), %llu missed after their image was taken by size; it "
                 "holds %llu (%llu MB)%s",
                 (unsigned long long)st_pool_reused, (unsigned long long)st_pool_made, (unsigned long long)st_pool_destroyed,
                 (unsigned long long)st_pool_resized, reuse_memory_level(),
                 (unsigned long long)st_pool_stolen, (unsigned long long)pool_n, (unsigned long long)(pool_bytes >> 20),
                 pool_cap() ? (pool_pressure >= 2 ? ", VRAM critical: given back" : pool_pressure == 1 ? ", VRAM short: halved" : "") : " (RX_VK_TARGET_POOL_MB=0)");
            if (reuse_memory_on()) {
                using target_pool::Miss;
                const auto n = [](Miss m) { return (unsigned long long)st_miss[(int)m]; };
                logf("vk: render targets not reused by size: %llu with the pool empty (%llu of them with targets still in the "
                     "dead queue), %llu with none in it a present yet, %llu with none of a kind they can use, %llu with all smaller "
                     "(the largest %llu%% short on average), %llu with all bigger (over 1.5 times), %llu with some of each, %llu over "
                     "a block, %llu refused by the driver, %llu not bound; the pool let go of %llu over its bound and %llu idle",
                     n(Miss::empty) + n(Miss::empty_dead_waiting), n(Miss::empty_dead_waiting), n(Miss::too_new), n(Miss::unusable),
                     n(Miss::too_small), n(Miss::too_small) ? (unsigned long long)(st_small_gap_pct / st_miss[(int)Miss::too_small]) : 0ull,
                     n(Miss::too_big), n(Miss::between), n(Miss::over_block), n(Miss::requirements), n(Miss::not_bound),
                     (unsigned long long)st_pool_trim_cap, (unsigned long long)st_pool_trim_idle);
            }
        }
        logf("vk: VRAM: %llu MB used of %llu MB allowed (%llu MB of it free in the blocks); textures %llu MB (budget %llu "
             "MB), %zu textures%s", (unsigned long long)(used >> 20), (unsigned long long)(allowed >> 20),
             (unsigned long long)(block_free >> 20), (unsigned long long)(total_bytes >> 20),
             (unsigned long long)(budget >> 20), textures.size(),
             known && used > count_line && room_in_blocks ? " (at the driver's limit, room in the blocks: not squeezed)" : "");
        {   // (mem_account.h: what every pool holds, summed, beside that count and the process's private memory. Nothing reads it back)
            account_sample(block_free);
            PROCESS_MEMORY_COUNTERS_EX pm{};
            pm.cb = sizeof pm;
            K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm);
            char line[1024];
            mem_account::format(line, sizeof line, mem_account::collect(mem_account::registry()), allowed, used, dev.memory_budget,
                                pm.PrivateUsage);
            logf("%s", line);
        }
        // (what takes it: decoded ASTC, the GPU's images, the rest; the largest)
        uint64_t astc_b = 0, rt_b = 0, other_b = 0, recent_b = 0, baked_b = 0;
        size_t baked_n = 0;
        std::vector<const Texture*> big, baked;
        size_t bucket_n[4]{}, bucket_rel[4]{};
        uint64_t bucket_b[4]{};
        for (auto& kv : textures) {
            const Texture* t = kv.second.get();
            (t->astc ? astc_b : t->gpu_written ? rt_b : other_b) += t->bytes;
            if (frame_no - t->last_frame < 8) recent_b += t->bytes;
            if (t->gpu_written && t->first_written && t->first_written == t->last_written && t->bytes <= (4ull << 20)) {
                if (!t->released) baked_n++, baked_b += t->bytes;
                const int k = t->bytes <= (64u << 10) ? 0 : t->bytes <= (256u << 10) ? 1 : t->bytes <= (1u << 20) ? 2 : 3;
                bucket_n[k]++, bucket_b[k] += t->bytes, bucket_rel[k] += t->released;
                baked.push_back(t);
            }
            big.push_back(t);
        }
        if (env_on("RX_VK_LOG_BAKED")) {   // (what the textures drawn in one frame are: by size, finalized or not)
            logf("vk: drawn in one frame: <=64 KB %zu (%llu KB, %zu finalized), <=256 KB %zu (%llu KB, %zu), <=1 MB %zu "
                 "(%llu KB, %zu), <=4 MB %zu (%llu KB, %zu)", bucket_n[0], (unsigned long long)(bucket_b[0] >> 10),
                 bucket_rel[0], bucket_n[1], (unsigned long long)(bucket_b[1] >> 10), bucket_rel[1], bucket_n[2],
                 (unsigned long long)(bucket_b[2] >> 10), bucket_rel[2], bucket_n[3], (unsigned long long)(bucket_b[3] >> 10),
                 bucket_rel[3]);
            std::sort(baked.begin(), baked.end(), [](const Texture* a, const Texture* b) { return a->bytes > b->bytes; });
            for (size_t i = 0; i < baked.size() && i < 10; i++) {
                const Texture* t = baked[i];
                logf("vk:   one frame: #%u %#llx %ux%ux%u x%u levels %u NVN %#x %llu KB, written at %llu, made at %llu, last "
                     "used %llu%s%s", t->id, (unsigned long long)t->gpu, t->width, t->height, t->depth, t->layers, t->levels,
                     t->nvn_format, (unsigned long long)(t->bytes >> 10), (unsigned long long)t->first_written,
                     (unsigned long long)t->first_seen, (unsigned long long)t->last_frame, t->released ? " finalized" : "",
                     t->first_render_frame ? " rendered" : " copied or stored");
            }
        }
        std::sort(big.begin(), big.end(), [](const Texture* a, const Texture* b) { return a->bytes > b->bytes; });
        logf("vk: textures: decoded ASTC %llu MB, rendered %llu MB (%zu drawn in one frame, kept: %llu KB), other %llu MB; "
             "used in the last 8 presents %llu MB", (unsigned long long)(astc_b >> 20), (unsigned long long)(rt_b >> 20),
             baked_n, (unsigned long long)(baked_b >> 10), (unsigned long long)(other_b >> 20),
             (unsigned long long)(recent_b >> 20));
        logf("vk: decoded ASTC used again: %lld textures (%lld MB) copied from ones the game finalized, not decoded; %lld of "
             "those retired, %zu listed; %lld MB of blocks hashed in %lld ms; %lld MB of it not read again (%lld textures), "
             "%lld hashed", (long long)st_reused.load(),
             (long long)(st_reused_bytes.load() >> 20), (long long)st_reuse_retired.load(), freed.size(),
             (long long)(st_reuse_hash_bytes.load() >> 20), (long long)(st_reuse_hash_ns.load() / 1000000),
             (long long)(hash_memo_hit_bytes.load() >> 20), (long long)hash_memo_hits.load(), (long long)hash_memo_misses.load());
        for (size_t i = 0; i < big.size() && i < 8; i++)
            logf("vk:   %llu MB: %#llx %ux%ux%u x%u levels %u NVN %#x%s%s, last used %llu presents ago",
                 (unsigned long long)(big[i]->bytes >> 20), (unsigned long long)big[i]->gpu, big[i]->width, big[i]->height,
                 big[i]->depth, big[i]->layers, big[i]->levels, big[i]->nvn_format, big[i]->astc ? " ASTC" : "",
                 big[i]->gpu_written ? " rendered" : "", (unsigned long long)(frame_no - big[i]->last_frame));
    }
    if (verify_dim().first && frame_no % 300 == 0) {
        int checked = 0, changed = 0, missed = 0;
        for (auto& kv : textures) {
            Texture* t = kv.second.get();
            if (t->width != verify_dim().first || t->height != verify_dim().second || !t->verify_hash || t->gpu_written) continue;
            checked++;
            const uint64_t h = guest_hash(*t);
            if (h == t->verify_hash) continue;
            changed++;
            static thread_local Ranges d;
            if (!texture_dirty(*t, t->upload_gen, d)) {
                missed++;
                static int logged;
                if (logged++ < 40) {
                    logf("vk: MISSED WRITE: texture #%u %#llx %ux%u NVN %#x: its memory changed since its upload, the write "
                         "tracking saw nothing", t->id, (unsigned long long)t->gpu, t->width, t->height, t->nvn_format);
                    verify_report(*t);
                }
            }
        }
        logf("vk: verify %ux%u: %d textures, %d changed since uploaded, %d of them unnoticed", verify_dim().first,
             verify_dim().second, checked, changed, missed);
    }
    static const bool no_evict = env_on("RX_VK_NO_EVICT");   // (an experiment: nothing evicted)
    // (longshot) under the budget, every 600 presents: the render targets drawn over since (below), unused for 120
    const bool sweep = total_bytes <= budget;
    // (longshot-amd) and every 60 presents for the render targets drawn over by one at another start (below): OoT makes a frame's
    // transient targets (768 x 432 RGBA16F, 1536 x 864) in a scratch heap whose offsets shift with what came before them in the
    // frame, so turning the camera made new ones each frame ~1.2 MB from the last, each a new image. Left for the 600-present
    // sweep or the budget, ~200 dead ones (1.2 GB) filled an 8 GB card's VRAM in Hyrule Field, the driver paged it out and the
    // GPU's frame went from 15 to 35 ms
    // (longshot) every present (RX_VK_DEAD_EVERY; 10 before, 60 before that): the target pool gets the dead ones as the game makes
    // new ones. Every 10 presents a burst of ten frames' dead targets came back at once, went over the pool's bound and were
    // destroyed, and the next frames made new ones (target_pool.h). The choice is cheap: 1930 textures ordered in 0.3 ms, 251
    // chosen in 0.2 (RTX 5070, Ryzen 7 9800X3D). This pass itself runs at every tenth present only (dead_targets.h): on the
    // presents between, dead_targets_sweep above lets them go
    static const uint64_t dead_every = dead_every_presents();
    if (no_evict || (sweep && frame_no % dead_every)) return;
    std::vector<std::pair<uint64_t, Texture*>> order;
    order.reserve(textures.size());
    for (auto& kv : textures) order.push_back({kv.second->last_frame, kv.second.get()});
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    // (the rendered textures by where their storage starts, for drawn_over_shifted)
    std::vector<const Texture*> by_start;
    for (auto& [last, tex] : order)
        if (tex->gpu_written && tex->first_render_frame && tex->guest_size) by_start.push_back(tex);
    std::sort(by_start.begin(), by_start.end(), [](const Texture* a, const Texture* b) { return a->gpu < b->gpu; });
    uint64_t longest = 0;
    for (const Texture* t : by_start) longest = std::max(longest, t->guest_size);
    std::unordered_set<Texture*> gone;
    // (over the budget, what goes has gone unused for RX_VK_EVICT_MIN_UNUSED frames, 60 by default; it was 8. On an
    // 8 GB laptop card the VRAM sits at the driver's limit, the budget shrinks and this runs every 10 frames: with 8,
    // ~300 textures in a few minutes of Kokiri Forest were evicted and made again, all of them within 60 frames (200
    // within 10), uploaded and decoded again each time; and a 260 x 260 one the GPU renders and the game samples every
    // 40 frames came back empty each time (22 times; the lit ground black for a frame, a tiled pattern over the ground).
    // With 60: 2. The textures that are really done with still go, 52 frames later)
    static const uint64_t min_unused =
        getenv("RX_VK_EVICT_MIN_UNUSED") ? (uint64_t)std::max(1, atoi(getenv("RX_VK_EVICT_MIN_UNUSED"))) : 60;
    // (longshot) a render target another image has since been drawn over (same storage, another size or format:
    // OoT's dynamic resolution makes a new set at each size in the same pool memory) holds nothing any more: it
    // goes like any other. Kept, they were 3.6 GB of 4.5 GB rendered, VRAM near the driver's limit, and the
    // driver's paging made submissions of seconds (5-9 fps for a minute)
    const auto drawn_over = [&](const Texture* tex) {
        if (!tex->gpu_written) return false;
        auto [a, e] = by_gpu.equal_range(tex->gpu);
        for (auto o = a; o != e; ++o)
            if (o->second != tex && o->second->last_written > tex->last_frame) return true;
        return false;
    };
    // (longshot-amd) a render target of one level and layer whose storage newer render targets drew over from another start
    // after its last use (target_overlap.h): the transient targets above. Its texels are gone from that memory on the console
    // too. Their parts are added up: several may each cover less than an eighth of it and together more
    const auto drawn_over_shifted = [&](const Texture* tex) {
        if (!tex->gpu_written || !tex->first_render_frame || !tex->guest_size || tex->levels != 1 ||
            tex->layers != 1 || tex->depth > 1)
            return false;
        const uint64_t lo = tex->gpu, hi = tex->gpu + tex->guest_size;
        auto it = std::lower_bound(by_start.begin(), by_start.end(), lo > longest ? lo - longest : 0,
                                   [](const Texture* t, uint64_t g) { return t->gpu < g; });
        target_overlap::ShiftedOver over{lo, hi, tex->last_frame};
        for (; it != by_start.end() && (*it)->gpu < hi; ++it) {
            if (*it == tex) continue;
            over.add((*it)->gpu, (*it)->gpu + (*it)->guest_size, (*it)->last_written);
            if (over.an_eighth()) return true;
        }
        return false;
    };
    // (the transient targets: a frame's own, not sampled again, their storage drawn over by another since. 16 frames left
    // ~500 MB of them in Hyrule Castle Town's market at 20 fps (59 768 x 432 RGBA16F, 135 384 x 384, 41 1536 x 864 at a
    // time), VRAM at the driver's limit; the GPU's own use is checked below (last_value). RX_VK_DEAD_MIN_UNUSED=<frames>, 1 (3
    // before: with the GPU's own wait on top, a dead target came back to the pool four or five frames late, and the game had made
    // a new one of its shape every frame meanwhile; 80 % of the images made new had last frame's one of their shape alive and unused)
    static const uint64_t dead_min_unused = dead_min_unused_presents();
    // (longshot) a small texture the GPU drew in one frame that the game has finalized, with nothing drawn over its storage
    // since: the game may make a texture over that storage again and sample it without drawing it. OoT's terrain does that with its
    // tiles' 260 x 260 RG8 normal maps: drawn when a chunk comes in, finalized when it goes out of range, made again and sampled as
    // they are when it comes back. On the console the texels are still in that memory; here they are only in the image, and one
    // evicted comes back empty (made again over storage the GPU rendered, not uploaded): streaks over the ground in square patches
    // after a while of play. Kept, up to RX_VK_KEEP_FINALIZED_MB (512 by default; 0:
    // evicted), the least recently used beyond that going. (Not written back into the memory instead: the game
    // may have put other things there since, and a write-back over scene data can crash it)
    static const uint64_t keep_finalized_max =
        (uint64_t)(getenv("RX_VK_KEEP_FINALIZED_MB") ? std::max(0, atoi(getenv("RX_VK_KEEP_FINALIZED_MB"))) : 128) << 20;
    const auto keep_finalized = [&](const Texture* tex) {
        if (!keep_finalized_max || !tex->gpu_written || !tex->released || !tex->first_written ||
            tex->first_written != tex->last_written || tex->bytes > (4ull << 20) || finalized_obsolete(*tex))
            return false;
        const auto g = gpu_owned.find(tex->gpu);   // (its storage's range still the one the GPU drew it in)
        if (g == gpu_owned.end() || g->second.first != tex->gpu + std::max<uint64_t>(tex->guest_size, 1)) return false;
        return !drawn_over(tex) && !drawn_over_shifted(tex);
    };
    uint64_t finalized_kept = 0;
    size_t finalized_kept_n = 0;
    if (keep_finalized_max)
        for (auto& [last, tex] : order)
            if (keep_finalized(tex)) finalized_kept += tex->bytes, finalized_kept_n++;
    if (finalized_kept && frame_no % 600 == 0) {
        std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, std::pair<size_t, uint64_t>> kinds;   // (w, h, layers, NVN format)
        for (auto& [last, tex] : order)
            if (keep_finalized(tex)) {
                auto& k = kinds[{tex->width, tex->height, tex->layers, tex->nvn_format}];
                k.first++, k.second += tex->bytes;
            }
        char line[512];
        int n = 0;
        for (const auto& [k, v] : kinds)
            if (n < 440)
                n += snprintf(line + n, sizeof line - n, " %ux%ux%u NVN %#x: %zu (%llu KB);", std::get<0>(k), std::get<1>(k),
                              std::get<2>(k), std::get<3>(k), v.first, (unsigned long long)(v.second >> 10));
        logf("vk: finalized textures the GPU drew once, kept for being made again: %zu (%llu KB):%s", finalized_kept_n,
             (unsigned long long)(finalized_kept >> 10), n ? line : "");
    }
    const uint64_t need_unused = sweep ? std::max<uint64_t>(120, min_unused) : min_unused;
    const uint64_t need_dead = std::min(need_unused, dead_min_unused);
    static uint64_t n_dead, b_dead, dead_logged_at;
    // (longshot) the textures to go are chosen first and destroyed after; the write-backs among them go to the GPU in a few
    // batches (flush_textures), not one submission and one wait each: a sweep of 500 at a scene load stalled for seconds
    struct Evict { Texture* tex; bool flush, shifted, overwritten; };
    std::vector<Evict> evict;
    uint64_t pending_bytes = 0;   // (chosen below, their bytes not yet off total_bytes)
    // (longshot, RX_VK_COMPACT) a pass the compaction's hold asked for (evict_to) lets go of the dead render targets as a sweep
    // does, all of them, and of live textures only for the excess left after those, and only of ones long unused: vram_compact.h's
    // hold_met and hold_target. So it does not stop at the first texture past the excess: what the dead targets hold is counted first
    const bool by_hold = evict_to != 0 && !sweep;
    uint64_t hold_dead = 0, hold_live = 0;   // (what the dead targets hold; the live textures chosen so far)
    if (by_hold)
        for (auto& [last, tex] : order) {
            if (frame_no - last < need_dead) break;
            if (tex->last_value > completed) continue;
            const bool shifted = drawn_over_shifted(tex);
            if (frame_no - last < need_unused && !shifted) continue;
            if (shifted || drawn_over(tex)) hold_dead += tex->bytes + tex->pad;
        }
    for (auto& [last, tex] : order) {
        // (total_bytes can have drifted below the textures' sum (destroy_later clamps it): the chosen ones' bytes past it mean the
        // budget is met, not a wrapped difference that never is)
        if ((!sweep && !by_hold && (pending_bytes >= total_bytes || total_bytes - pending_bytes <= budget / 8 * 7)) ||
            frame_no - last < need_dead)
            break;
        const bool shifted = drawn_over_shifted(tex);
        if (frame_no - last < need_unused && !shifted) continue;   // (younger: only a dead transient target goes)
        if (tex->last_value > completed) continue;
        // (nor what the GPU wrote and the game still has: the image is its only copy, and one made again over that
        // storage started empty or from stale memory: a sky or course texture drawn once, garbage from then on)
        // (one unused for 3600 presents (a minute) goes: a course left behind; the game draws it again when it comes
        // back to that course. Without it a course change kept 1.4 GB arrays and VRAM ran over)
        // (600 presents near what the driver allows, at the budget's 97%: past it the driver pages VRAM to system memory.
        // AMD's driver reports the use at most at what it allows, so "over it" never held there: keep_rendered.h)
        const uint64_t keep_rendered = keep_rendered::presents(known, used, allowed);
        const bool overwritten = shifted || drawn_over(tex);   // (above)
        // (longshot, RX_VK_COMPACT: the hold's pass, above. A live texture stays once the excess over its line is covered: the
        // reserve's line for one long unused, the hold's own for one unused ten seconds, and what the other rules ask (the 90%
        // rule, the budget's size, the driver's limit) for one unused as long as an eviction asks with the compaction off)
        // (longshot) a finalized texture whose storage the game has written over since goes, whatever the budget says: what its image
        // held is gone from that memory on the console too, and a draw that sampled it would take the memory's contents over its
        // texels (texture_touch). Within the budget the pass is a sweep that lets go of nothing the GPU did not draw over, and these
        // were never drawn over by a target: the game's own writes to the memory they stood in would keep every one for good
        const bool obsolete = !overwritten && finalized_obsolete(*tex);
        if (by_hold && !overwritten && !obsolete) {
            const uint64_t to = vram_compact::hold_target(frame_no - last, need_unused, rule_to, hold_to, evict_to);
            if (to == UINT64_MAX || vram_compact::hold_met(total_bytes, hold_live, hold_dead, to)) continue;
        }
        if (!obsolete && finalized_kept && keep_finalized(tex)) {   // (above. Oldest first: beyond the cap the least recently used go)
            if (finalized_kept <= keep_finalized_max) continue;
            finalized_kept -= tex->bytes;
            static int logged;
            if (logged++ < 100)
                logf("vk: kept finalized textures over %llu MB: #%u %#llx %ux%u NVN %#x goes (unused for %llu frames)",
                     (unsigned long long)(keep_finalized_max >> 20), tex->id, (unsigned long long)tex->gpu, tex->width,
                     tex->height, tex->nvn_format, (unsigned long long)(frame_no - last));
        }
        if (!overwritten && tex->gpu_written && !tex->released && frame_no - last < keep_rendered) continue;
        if (sweep && !overwritten && !obsolete) continue;
        // (longshot) nor a live texture the GPU drew whose texels cannot be written back into its memory (ASTC kept as BC3, sparse,
        // past the write-back's size, ...): its image is the only copy, and one made again over that storage is not uploaded (the
        // memory counts as the GPU's), so it would show whatever its new memory held: rainbow/garbled textures spreading over play
        // time on 4-8 GB cards. Up to 256 MB of such textures each stay; bigger ones (a whole area left behind) go. On by
        // default: the VRAM it costs is bounded (256 MB a texture) and a wrong texture is worse than a fuller budget.
        // RX_VK_EVICT_UNFLUSHABLE=1: evicted
        static const bool evict_unflushable = getenv("RX_VK_EVICT_UNFLUSHABLE") && atoi(getenv("RX_VK_EVICT_UNFLUSHABLE")) != 0;
        if (!overwritten && tex->gpu_written && !tex->released && !evict_unflushable && tex->bytes < (256ull << 20) && !flushable(*tex)) {
            static std::atomic<int> kept_logged{0};
            if (kept_logged.fetch_add(1) < 40)
                logf("vk: texture #%u %#llx %ux%u x%u NVN %#x the GPU drew kept, not evicted: its texels cannot be written back",
                     tex->id, (unsigned long long)tex->gpu, tex->width, tex->height, tex->layers, tex->nvn_format);
            continue;
        }
        // (and a small one never: its texels exist nowhere else, and it is made once and kept by the game. The sky's
        // 256 x 256 lookup table, unused while a menu was up, went and came back from memory as garbage: the rainbow
        // sky. The big ones that may go are a course left behind)
        // (a render target, drawn in the frames it was used in, goes: the dynamic resolution makes a new set of ~20 at
        // each size, and the old ones are not used again)
        const bool drawn_once = tex->last_written + 8 < tex->last_frame;
        if (!overwritten && tex->gpu_written && !tex->released && tex->bytes < (64ull << 20) && drawn_once) continue;
        // (nor a small one the GPU wrote in one frame alone: a table the game bakes once and samples now and then. The
        // pause screen's colour grading, 8 x 8 x 8, its slices drawn at boot and sampled next when the game is paused,
        // went after a minute unused, and the one made again over storage only the GPU had written stayed empty: the
        // pause blur black. Not a finalized one: the transient targets the game makes and finalizes every frame are
        // each "written in one frame", and kept so they added up, ~20 MB a minute)
        const bool baked = tex->first_written && tex->first_written == tex->last_written && tex->bytes <= (4ull << 20);
        if (!overwritten && tex->gpu_written && !tex->released && baked) continue;
        // (a texture the GPU drew that still holds the game's content goes into the game's memory first: made again later, it is
        // uploaded from there with what the GPU drew. Evicted without it, it comes back empty or stale: a terrain chunk dark, a
        // tiled pattern over the Deku Tree's planks and Hyrule Field's path)
        evict.push_back({tex, !overwritten && tex->gpu_written && !tex->released && flushable(*tex), shifted, overwritten || obsolete});
        pending_bytes += tex->bytes + tex->pad;
        if (by_hold && !overwritten && !obsolete) hold_live += tex->bytes + tex->pad;
    }
    {   // (longshot) nor one whose storage another image was drawn into after its own last drawing (evict_reason.h's
        // write_back_stale): its write-back puts old texels over memory that is the other image's now, and marks those pages
        // written. OoT draws its HUD's labels inside the storage of a 1920 x 1080 target it has stopped using; that target's
        // eviction, ~600 frames later at a 6 GB budget, made the finalized labels upload from memory that never held their
        // texels: the d-pad and item prompts as noise until the pause menu redrew them
        std::vector<const Texture*> ts;
        for (const Evict& e : evict)
            if (e.flush) ts.push_back(e.tex);
        if (!ts.empty()) {
            const std::vector<uint64_t> other = drawn_others(ts);
            size_t i = 0;
            for (Evict& e : evict)
                if (e.flush) e.flush = !evict_reason::write_back_stale(e.tex->last_written, other[i++]);
        }
    }
    evict_reason::Tally tally;                     // (why each one goes: the retired line below)
    std::unordered_set<const Texture*> flushed_ok;   // (the write-backs that worked)
    // (and those that wait: a read-back still on the GPU, its wait timed out. They stay, their texels in their images, for a
    // later pass once rb_fence signals; evicted now, they went with no copy and were made again empty)
    std::unordered_set<const Texture*> flush_later;
    {   // (the write-backs: batches of up to 64 textures or ~512 MB of staging memory, each one submission and one wait)
        std::vector<Flush> batch;
        uint64_t batch_bytes = 0;
        const auto run = [&] {
            if (batch.empty()) return;
            flush_textures(batch);
            for (const Flush& f : batch)
                if (f.busy) flush_later.insert(f.t);
                else if (f.ok) {
                    flushed_ok.insert(f.t);
                    static std::atomic<int64_t> flushed{0};
                    if (++flushed % 50 == 1) logf("vk: textures the GPU drew, written back into their memory before eviction: %lld so far", (long long)flushed.load());
                }
            batch.clear();
            batch_bytes = 0;
        };
        for (const Evict& e : evict) {
            if (!e.flush) continue;
            batch.push_back(Flush{e.tex, e.tex->gpu, e.tex->gpu + e.tex->guest_size});
            batch_bytes += e.tex->guest_size;   // (what its staging buffer holds: the texels as the storage lays them out)
            if (batch.size() >= 64 || batch_bytes >= (512ull << 20)) run();
        }
        run();
    }
    for (const Evict& e : evict) {   // (and each one's bookkeeping and its end, as before)
        Texture* tex = e.tex;
        if (flush_later.count(tex)) continue;
        if (e.shifted) n_dead++, b_dead += tex->bytes;
        {   // (longshot) why it goes, and whether its texels go with it (evict_reason.h). The flush above can fail: then they did
            const bool lost = evict_reason::texels_lost(tex->gpu_written, e.overwritten, e.flush && flushed_ok.count(tex));
            tally.add(evict_reason::classify(e.shifted, e.overwritten), lost, tex->bytes, tex->released);
            static int lost_logged;
            if (lost && lost_logged++ < 100)
                // (always over the budget: a sweep lets only drawn-over textures go, and those lose nothing)
                logf("vk: texture #%u %#llx %ux%ux%u x%u NVN %#x the GPU drew goes with no copy of its texels (%s, unused for %llu "
                     "frames, over the budget): made again over this storage, it starts empty", tex->id, (unsigned long long)tex->gpu,
                     tex->width, tex->height, tex->depth, tex->layers, tex->nvn_format, tex->released ? "finalized" : "live",
                     (unsigned long long)(frame_no - tex->last_frame));
        }
        watch_tex(tex, "evicted");
        if (!tex->gpu_written && tex->uploaded) {   // (above: made again, it is uploaded from memory)
            std::lock_guard lk(from_memory_mu);
            if (evicted_from_memory.size() > 50000) evicted_from_memory.clear();   // (a bound)
            evicted_from_memory[evicted_key(tex->gpu, tex->width, tex->height, tex->nvn_format)] = tex->guest_size;
        }
        if (remade_log_on()) {
            std::lock_guard lk(evicted_mu);
            if (evicted_at.size() > 20000) evicted_at.clear();   // (a bound: only the recent ones matter)
            evicted_at[evicted_key(tex->gpu, tex->width, tex->height, tex->nvn_format)] = {frame_no, tex->gpu_written};
            remade.erase(tex);
        }
        destroy_later(*tex, completed);
        forget_gpu(tex);
        gone.insert(tex);
    }
    rendered_forget(gone);
    for (auto it = textures.begin(); it != textures.end() && !gone.empty();) {
        if (gone.erase(it->second.get())) it = textures.erase(it);
        else ++it;
    }
    if (n_dead && frame_no >= dead_logged_at + 600) {   // (the dead transient targets let go so far, every 600 presents)
        dead_logged_at = frame_no;
        logf("vk: render targets drawn over from another start, let go: %llu so far (%llu MB), %llu of them (%llu MB) between the "
             "passes", (unsigned long long)(n_dead + st_dead_swept), (unsigned long long)((b_dead + st_dead_swept_bytes) >> 20),
             (unsigned long long)st_dead_swept, (unsigned long long)(st_dead_swept_bytes >> 20));
    }
    const size_t n = order.size() - textures.size();
    if (n) {
        char why[160];
        evict_reason::describe(tally, why, sizeof why);
        logf("vk: %zu textures retired (%zu left, %llu MB): %s", n, textures.size(), (unsigned long long)(total_bytes >> 20), why);
    }
}

void texture_log_guest(const Texture* t) {
    if (!t || !t->guest_size) return;
    std::vector<uint8_t> b(65536);
    char lv[512] = "";
    int k = 0;
    uint64_t all_nz = 0, all_unmapped = 0;
    for (uint32_t l = 0; l < std::max(1u, t->d.levels) && l < 16; l++) {
        const uint64_t lo = host->tex_level_offset(&t->d, l);
        const uint64_t hi = l + 1 < std::max(1u, t->d.levels) ? host->tex_level_offset(&t->d, l + 1) : t->guest_size;
        uint64_t nz = 0, unmapped = 0, pages = 0, empty_pages = 0;
        for (uint64_t off = lo; off < hi; off += 65536) {
            const uint64_t n = std::min<uint64_t>(65536, hi - off);
            if (!host->gpu_read(t->gpu + off, b.data(), n)) { unmapped += n; continue; }
            uint64_t page_nz = 0;
            for (uint64_t i = 0; i < n; i++) page_nz += b[i] != 0;
            nz += page_nz, pages++, empty_pages += !page_nz;
        }
        all_nz += nz, all_unmapped += unmapped;
        if (k < 480)
            k += snprintf(lv + k, sizeof lv - k, " L%u: %llu KB, %llu not zero, %llu of %llu mapped pages empty%s;", l,
                          (unsigned long long)((hi - lo) >> 10), (unsigned long long)nz, (unsigned long long)empty_pages,
                          (unsigned long long)pages, unmapped ? " (part unmapped)" : "");
    }
    const uint64_t g0 = host->gpu_guest(t->gpu);
    logf("vk: dump guest memory of #%u %#llx %ux%u NVN %#x (guest %#llx%s): %llu bytes not zero, %llu KB unmapped;%s", t->id,
         (unsigned long long)t->gpu, t->width, t->height, t->nvn_format, (unsigned long long)g0,
         t->gpu >= (1ull << 40) ? ", a virtual pool's" : "", (unsigned long long)all_nz,
         (unsigned long long)(all_unmapped >> 10), lv);
}

void texture_released(const nvn_tex* t) {
    if (!t) return;
    if (t->output_preview) {
        // The sorted replay release follows the last native submissions and present copy for this exclusive storage.
        // Drop every cached view of the allocation before reusing its guest address; recorded descriptors can retain a serial.
        const uint64_t fence = vk_retirement_value();
        const uint64_t end = t->gpu + t->L.size;
        for (auto it = gpu_owned.lower_bound(t->gpu); it != gpu_owned.end() && it->first < end;)
            it = it->second.first <= end ? gpu_owned.erase(it) : std::next(it);
        for (auto it = textures.begin(); it != textures.end();) {
            if (it->first.gpu < t->gpu || it->first.gpu >= end) {
                ++it;
                continue;
            }
            Texture& texture = *it->second;
            texture.pool_key = 0;
            texture.last_value = fence ? std::max(texture.last_value, fence) : UINT64_MAX;
            std::erase(rendered_targets, &texture);
            if (present_source == &texture) present_source = nullptr;
            destroy_later(texture, completed_value);
            forget_gpu(&texture);
            it = textures.erase(it);
        }
        return;
    }
    auto [a, b] = by_gpu.equal_range(t->gpu);
    for (auto it = a; it != b; ++it)
        if (it->second->gpu_written && !it->second->released) {
            it->second->released = true, it->second->release_gen = memory_generation();
            watch_tex(it->second, "finalized");
            static int logged;   // (a big one: the course maps' kind)
            if (it->second->bytes >= (32ull << 20) && logged++ < 40)
                logf("vk: texture #%u %#llx %ux%u x%u NVN %#x (%llu MB, rendered or copied into) finalized by the game",
                     it->second->id, (unsigned long long)it->second->gpu, it->second->width, it->second->height,
                     it->second->layers, it->second->nvn_format, (unsigned long long)(it->second->bytes >> 20));
        }
    reuse_released(t);
}

uint64_t texture_made() {
    return st_made;
}
void texture_made_kinds(uint64_t n[4], uint64_t bytes[4]) {
    for (int i = 0; i < 4; i++) n[i] = st_made_kind[i], bytes[i] = st_made_kind_bytes[i];
}
void texture_unswizzle_stats(int64_t* bytes, int64_t* ns) {
    *bytes = st_unswizzle_bytes, *ns = st_unswizzle_ns;
}
size_t texture_count() {
    return textures.size();
}
uint64_t texture_bytes() {
    return total_bytes;
}
void texture_stats(int64_t* uploads, int64_t* astc_gpu, int64_t* bytes_uploaded) {
    *uploads = st_uploads, *astc_gpu = st_astc, *bytes_uploaded = st_bytes;
}

bool texels_fit(const nvn_tex* t, const NVNcopyRegion* r, uint64_t row, uint64_t avail) {
    if (!t || !r) return false;
    int bw = 1, bh = 1;
    const int bpb = nvn_block_size(t->d.format, &bw, &bh);
    if (bpb <= 0) return true;   // (texture_read_texels copies nothing)
    const uint64_t w = (uint64_t)((std::max(0, r->width) + bw - 1) / bw), h = (uint64_t)((std::max(0, r->height) + bh - 1) / bh);
    const uint64_t dd = (uint64_t)std::max(1, r->depth);
    if (!w || !h) return true;
    const uint64_t pitch = row ? row : w * bpb;
    uint64_t image = 0, a = 0, b = 0, n = 0;
    return !__builtin_mul_overflow(pitch, h, &image) && !__builtin_mul_overflow(dd - 1, image, &a) &&
           !__builtin_mul_overflow(h - 1, pitch, &b) && !__builtin_add_overflow(a, b, &n) &&
           !__builtin_add_overflow(n, w * bpb, &n) && n <= avail;
}

/// ReadTexels / WriteTexels: the CPU's copy of a region in guest memory (block linear, the Switch 2 GOB swizzle).
/// A write also stamps the pages, so the image is uploaded again; a texture the GPU wrote is read from guest memory
/// as it is (its newest texels are in the image: not yet read back).
void texture_read_texels(const nvn_tex* t, unsigned level, unsigned layer0, const NVNcopyRegion* r, uint8_t* data,
                         uint64_t row, uint64_t img, bool write) {
    if (!t || !t->gpu || !r) return;
    {   // (RX_VK_WATCH_RANGE: ReadTexels / WriteTexels of textures overlapping it, 400 lines)
        static const std::pair<uint64_t, uint64_t> range = [] {
            unsigned long long a = 0, b = 0;
            if (const char* e = getenv("RX_VK_WATCH_RANGE")) sscanf(e, "%llx:%llx", &a, &b);
            return std::pair<uint64_t, uint64_t>{a, b};
        }();
        static int logged;
        if (range.second && t->gpu < range.second && t->gpu + std::max<uint64_t>(t->L.size, 1) > range.first && logged++ < 400)
            logf("vk: watch range %s %#llx %ux%u NVN %#x level %u layer %u region (%d,%d %dx%d) mapped %s", write ? "WriteTexels" : "ReadTexels",
                 (unsigned long long)t->gpu, t->d.width, t->d.height, t->d.format, level, layer0, r->xoffset, r->yoffset,
                 r->width, r->height, host->gpu_guest(t->gpu + host->tex_level_offset(&t->d, level)) ? "(level start yes)" : "(level start no)");
    }
    int bw = 1, bh = 1;
    const int bpb = nvn_block_size(t->d.format, &bw, &bh);
    if (bpb <= 0) return;
    const nvn_texdesc& d = t->d;
    const uint32_t lw = (std::max(1u, d.width >> level) + bw - 1) / bw;
    const uint32_t lh = d.target == NVN_TEXTURE_TARGET_1D || d.target == NVN_TEXTURE_TARGET_1D_ARRAY
                            ? 1 : (std::max(1u, d.height >> level) + bh - 1) / bh;
    const bool vol = d.target == NVN_TEXTURE_TARGET_3D;
    const uint64_t base = t->gpu + host->tex_level_offset(&d, level);
    unsigned th = 0, td = 0;
    host->tex_level_tiles(&d, level, &th, &td);
    const uint32_t x0 = (uint32_t)std::max(0, r->xoffset) / bw, y0 = (uint32_t)std::max(0, r->yoffset) / bh;
    const uint32_t w = (uint32_t)(std::max(0, r->width) + bw - 1) / bw, h = (uint32_t)(std::max(0, r->height) + bh - 1) / bh;
    const uint32_t z0 = (uint32_t)std::max(0, r->zoffset), dd = (uint32_t)std::max(1, r->depth);
    const uint64_t pitch = row ? row : (uint64_t)w * bpb, image = img ? img : pitch * h;
    const bool linear = nvn_tex_is_linear(&d);
    const uint32_t stride = linear ? (d.stride ? d.stride : host->tex_linear_stride(&d)) : 0;
    const uint64_t gobs_x = ((uint64_t)lw * bpb + 63) >> 6;
    const uint64_t rows_y = (lh + (8u << th) - 1) >> (3 + th);
    const uint64_t block = 512ull << (th + td);
    for (uint32_t k = 0; k < dd; k++) {
        const uint32_t z = vol ? z0 + k : 0;
        const uint64_t layer_base = base + (vol ? 0 : (uint64_t)(layer0 + z0 + k) * t->L.layer_size);
        for (uint32_t y = 0; y < h && y0 + y < lh; y++) {
            uint8_t* line = data + k * image + y * pitch;
            const uint32_t yy = y0 + y;
            for (uint32_t xb = 0; xb < (uint64_t)w * bpb;) {
                const uint32_t xbyte = x0 * bpb + xb;
                uint64_t off;
                uint32_t run;
                if (linear) {
                    off = (uint64_t)yy * stride + xbyte;
                    run = w * bpb - xb;
                } else {   // (16 bytes of a GOB row are contiguous)
                    const uint64_t gob = ((xbyte & 63) >> 5) << 8 | ((yy & 7) >> 2) << 7 | ((xbyte & 31) >> 4) << 6 |
                                         ((yy >> 1) & 1) << 5 | (yy & 1) << 4 | (xbyte & 15);
                    off = (uint64_t)(z >> td) * gobs_x * rows_y * block + (uint64_t)(yy >> (3 + th)) * gobs_x * block +
                          (uint64_t)(xbyte >> 6) * block + ((uint64_t)(z & ((1u << td) - 1)) << (9 + th)) +
                          ((uint64_t)((yy >> 3) & ((1u << th) - 1)) << 9) + gob;
                    run = std::min<uint32_t>(16 - (xbyte & 15), w * bpb - xb);
                }
                if (write) host->gpu_write(layer_base + off, line + xb, run);   // (the shadows hear of the whole: guest_cpu_wrote below)
                else host->gpu_read(layer_base + off, line + xb, run);
                xb += run;
            }
        }
    }
    if (write) {
        guest_cpu_wrote(t->gpu, t->L.size);   // (the write-back diff's shadows: one report for the 16-byte runs above)
        const uint64_t guest = host->gpu_guest(t->gpu);
        if (guest) memory_stamp(guest, t->L.size);
        auto [a, b] = by_gpu.equal_range(t->gpu);
        for (auto it = a; it != b; ++it) it->second->gpu_written = false, it->second->checked_epoch = 0;
    }
}

} // namespace mkwvk
