// Vulkan Memory Allocator (v3.3.0, from Eglantine's CPM cache: nx2/build.py adds its include path): images and
// device buffers are sub-allocated from large blocks (NVIDIA allows ~4096 allocations, and the imported pools
// already take about 2400 of them). Its Vulkan functions come from vkGetInstanceProcAddr / vkGetDeviceProcAddr.
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_internal.h"
#include <vk_mem_alloc.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#ifndef LONGSHOT_ENV_H   // (env_on: the switches' one rule)
#include "longshot_env.h"
#endif
#include "vram_compact.h"
#include "memory_priority.h"
#include "target_pool.h"
#include "vk_work.h"   // (last: RX_VK_RECORDER=defer drains before the destructions and the invalidate below)

namespace mkwvk {

VmaAllocator vma;

/// Largest DEVICE_LOCAL heap size in bytes reported by Vulkan physical device memory properties.
static uint64_t device_local_heap_bytes() {
    uint64_t heap = 0;
    for (uint32_t h = 0; h < dev.memory.memoryHeapCount; h++)
        if (dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) heap = std::max<uint64_t>(heap, test_vram_bytes(dev.memory.memoryHeaps[h].size));   // (RX_VK_TEST_VRAM_MB)
    return heap;
}

// (longshot) RX_VK_MEMORY_PRIORITY's priorities (memory_priority.h), read once. With the extension on, VMA gives each pool of
// ours and each allocation with a memory object of its own the priority in its create info, and a zero left there is the
// lowest there is: every pool and every allocation below sets one. Off, VMA ignores them all
static const memory_priority::Tiers& memory_tiers() {
    static const memory_priority::Tiers t = memory_priority::parse(getenv("RX_VK_MEMORY_PRIORITY"));
    return t;
}

bool vma_init(PFN_vkGetInstanceProcAddr gipa) {
    VmaVulkanFunctions f{};
    f.vkGetInstanceProcAddr = gipa;
    f.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo ci{};
    ci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT |
               (dev.memory_budget ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0);
    if (dev.memory_priority) ci.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT;
    if (memory_tiers().unread)
        logf("vk: memory priority (RX_VK_MEMORY_PRIORITY): the value is not <targets>,<textures> with two numbers from 0 to 1 "
             "(a decimal point, not a comma): the usual %.2f and %.2f instead", memory_tiers().targets, memory_tiers().textures);
    if (memory_tiers().on && dev.memory_priority)
        logf("vk: memory priority (RX_VK_MEMORY_PRIORITY, VK_EXT_memory_priority): render targets %.2f, textures %.2f, the rest %.2f",
             memory_tiers().targets, memory_tiers().textures, memory_tiers().rest);
    else if (memory_tiers().on)
        logf("vk: memory priority (RX_VK_MEMORY_PRIORITY): the device has no VK_EXT_memory_priority with its feature, nothing changes");
    ci.physicalDevice = dev.physical;
    ci.device = dev.device;
    ci.instance = dev.instance;
    ci.vulkanApiVersion = VK_API_VERSION_1_3;
    ci.pVulkanFunctions = &f;
    // (64 MB blocks on > 8 GB cards; 32 MB on <= 8 GB cards to halve fragmentation in default heap 0 blocks)
    ci.preferredLargeHeapBlockSize = device_local_heap_bytes() <= vram_compact::GLOBAL_MIN_GRAPHICS_REQ ? (32ull << 20) : (64ull << 20);
    return vmaCreateAllocator(&ci, &vma) == VK_SUCCESS;
}

// Images come from a pool of 128 MB blocks: on their own, VMA gave most of them dedicated memory (the driver prefers it
// for render targets, and anything over half a default block), a vkAllocateMemory and a kernel call for each, and
// the dynamic resolution makes a new set of render targets every few frames. Images too large for a block, or of a
// memory type the pool's is not, are allocated as before.
VmaPool image_pool = VK_NULL_HANDLE;
bool image_pool_tried = false;
// (RX_VK_IMAGE_BLOCK_MB=<n>, 16 and up: the block's size. VMA keeps one block empty for the next image, 128 MB of VRAM that a 64 MB block
// would halve, at twice the memory objects: the driver allows ~4096, the imported pools take ~2400)
const uint64_t kImageBlock = (getenv("RX_VK_IMAGE_BLOCK_MB") ? (uint64_t)std::max(16, atoi(getenv("RX_VK_IMAGE_BLOCK_MB"))) : 32u) << 20;

// (longshot) and the render targets' images in blocks of their own (texture_get's for a target: OoT's transient ones, made and let go
// all the time). In the same blocks as the textures that stay, each one gone left a hole beside a texture that kept its block: the
// pool's 47-54 blocks (6-6.9 GB) held 1.9-3.6 GB of images in Kokiri Forest, VRAM sat at 93-98% of what the driver allows, and an
// image made there then took 30-70 ms (RTX 5070)
VmaPool target_image_pool = VK_NULL_HANDLE;
bool target_pool_tried = false;
uint32_t target_image_pool_type = UINT32_MAX;   // (its memory type: VMA does not check an image's memoryTypeBits against a pool's)
VmaPool images_pool();
VmaPool target_images_pool() {
    if (target_pool_tried) return target_image_pool;
    target_pool_tried = true;
    if (!images_pool()) return VK_NULL_HANDLE;
    VkImageCreateInfo rep{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};   // (the image pool's representative image: the same memory type)
    rep.imageType = VK_IMAGE_TYPE_2D;
    rep.format = VK_FORMAT_R8G8B8A8_UNORM;
    rep.extent = {256, 256, 1};
    rep.mipLevels = rep.arrayLayers = 1;
    rep.samples = VK_SAMPLE_COUNT_1_BIT;
    rep.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type = 0;
    if (vmaFindMemoryTypeIndexForImageInfo(vma, &rep, &ai, &type) != VK_SUCCESS) return VK_NULL_HANDLE;
    target_image_pool_type = type;
    const uint64_t block_size =
        device_local_heap_bytes() <= vram_compact::GLOBAL_MIN_GRAPHICS_REQ ? std::min<uint64_t>(kImageBlock, 16ull << 20) : kImageBlock;
    VmaPoolCreateInfo pi{};
    pi.memoryTypeIndex = type;
    pi.blockSize = block_size;
    pi.priority = memory_tiers().targets;
    if (vmaCreatePool(vma, &pi, &target_image_pool) != VK_SUCCESS) target_image_pool = VK_NULL_HANDLE;
    logf("vk: render targets' images from blocks of their own (%llu MB, memory type %u)%s", (unsigned long long)(block_size >> 20),
         type, target_image_pool ? "" : ": not made");
    return target_image_pool;
}

// (longshot, RX 7800 XT: part of a pool's blocks made at the start, not when the game first needs them. Each is a vkAllocateMemory of
// 128 MB, ~170 ms on AMD's driver, on the replay thread: ~70 of them in a walk across Hyrule Field, the frame rate dropping to the low
// 40s whenever new ground came in. In the pool the sampled textures are made in and no other, 15% of the card's memory, 2 to 24 blocks,
// none on an integrated GPU: vram_compact::blocks_at_start. RX_VK_IMAGE_PREALLOC=<blocks>: that many, 0: none)
static size_t blocks_at_start(bool holds_textures) {
    if (!holds_textures) return 0;
    if (const char* e = getenv("RX_VK_IMAGE_PREALLOC")) return (size_t)std::max(0, atoi(e));
    return (size_t)vram_compact::blocks_at_start(device_local_heap_bytes(), kImageBlock, holds_textures,
                                                 dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU);
}
/// The pool with n blocks made now, or empty when there is no room for them (its blocks made as needed); the blocks made
static size_t make_pool(VmaPoolCreateInfo pi, size_t n, VmaPool& pool) {
    pi.minBlockCount = n;
    if (n && vmaCreatePool(vma, &pi, &pool) == VK_SUCCESS) return n;
    pi.minBlockCount = 0;
    if (vmaCreatePool(vma, &pi, &pool) != VK_SUCCESS) pool = VK_NULL_HANDLE;
    return 0;
}

VmaPool images_pool() {
    if (image_pool_tried) return image_pool;
    image_pool_tried = true;
    VkImageCreateInfo rep{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};   // (a representative image: the pool's memory type)
    rep.imageType = VK_IMAGE_TYPE_2D;
    rep.format = VK_FORMAT_R8G8B8A8_UNORM;
    rep.extent = {256, 256, 1};
    rep.mipLevels = rep.arrayLayers = 1;
    rep.samples = VK_SAMPLE_COUNT_1_BIT;
    rep.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type = 0;
    if (vmaFindMemoryTypeIndexForImageInfo(vma, &rep, &ai, &type) != VK_SUCCESS) return VK_NULL_HANDLE;
    VmaPoolCreateInfo pi{};
    pi.memoryTypeIndex = type;
    pi.blockSize = kImageBlock;
    pi.priority = memory_tiers().rest;   // (the textures' too when they are made here: RX_VK_COMPACT=0)
    const size_t n = make_pool(pi, blocks_at_start(!vram_compact::enabled(getenv("RX_VK_COMPACT"))), image_pool);
    logf("vk: images from a pool of %llu MB blocks (memory type %u), %zu made at the start%s", (unsigned long long)(kImageBlock >> 20), type,
         n, image_pool ? "" : ": not made");
    return image_pool;
}

// (longshot) The sampled textures' own pool, when they are compacted (unless RX_VK_COMPACT=0): VMA's defragmentation asks to move any
// allocation of the pool it works on, and only vk_texture.cpp's images can be moved (it knows what they hold and who uses
// them) or kept from being freed while a pass is open. The render targets' images stay in their blocks above; DLSS's, the
// frame interpolation's and the scratch ones in the images' pool
VmaPool texture_pool = VK_NULL_HANDLE;
bool texture_pool_tried = false;
uint64_t texture_pool_kept = 0;   // (its blocks made at the start, which VMA keeps: bytes)
VmaPool textures_pool() {
    if (texture_pool_tried) return texture_pool;
    texture_pool_tried = true;
    VkImageCreateInfo rep{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};   // (the image pool's representative image: the same memory type)
    rep.imageType = VK_IMAGE_TYPE_2D;
    rep.format = VK_FORMAT_R8G8B8A8_UNORM;
    rep.extent = {256, 256, 1};
    rep.mipLevels = rep.arrayLayers = 1;
    rep.samples = VK_SAMPLE_COUNT_1_BIT;
    rep.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type = 0;
    if (vmaFindMemoryTypeIndexForImageInfo(vma, &rep, &ai, &type) != VK_SUCCESS) return VK_NULL_HANDLE;
    VmaPoolCreateInfo pi{};
    pi.memoryTypeIndex = type;
    pi.blockSize = kImageBlock;
    pi.priority = memory_tiers().textures;
    texture_pool_kept = make_pool(pi, blocks_at_start(true), texture_pool) * kImageBlock;
    logf("vk: textures from a pool of their own, %llu MB blocks (RX_VK_COMPACT), %llu made at the start%s",
         (unsigned long long)(kImageBlock >> 20), (unsigned long long)(texture_pool_kept / kImageBlock), texture_pool ? "" : ": not made");
    return texture_pool;
}
bool textures_pool_usage(uint64_t* block_bytes, uint64_t* free_bytes, uint64_t* kept_bytes) {
    if (!texture_pool) return false;
    VmaStatistics st{};
    vmaGetPoolStatistics(vma, texture_pool, &st);
    *block_bytes = st.blockBytes, *free_bytes = st.blockBytes - std::min(st.blockBytes, st.allocationBytes);
    if (kept_bytes) *kept_bytes = texture_pool_kept;
    return true;
}

// ---- the textures' pool compacted: VMA's defragmentation context and its open pass ----
namespace {
VmaDefragmentationContext compact_ctx = VK_NULL_HANDLE;
VmaDefragmentationPassMoveInfo compact_pass{};
bool compact_in_pass = false;
// (the reaper's, destroy_image_later: its frees and a pass take turns; a pass waits for the frees in flight)
std::mutex reap_mu;
std::condition_variable reap_cv;        // (the reaper's: something handed over, or a pass ended)
std::condition_variable reap_done_cv;   // (a pass's: the frees in flight are done)
int reaping = 0;                        // (under reap_mu: frees in flight, outside the lock: the reaper's, a cap's, reap_now's)
bool pass_waiting = false;              // (under reap_mu: a pass waits for them: the reaper takes no more until it has begun)
bool compact_over(uint64_t* freed_bytes, uint32_t* freed_blocks) {
    VmaDefragmentationStats st{};
    vmaEndDefragmentation(vma, compact_ctx, &st);
    compact_ctx = VK_NULL_HANDLE;
    if (freed_bytes) *freed_bytes = st.bytesFreed;
    if (freed_blocks) *freed_blocks = st.deviceMemoryBlocksFreed;
    return false;
}
}
bool image_compact_begin(uint64_t max_bytes_a_pass, uint32_t max_moves_a_pass) {
    if (compact_ctx || !texture_pool) return false;
    VmaDefragmentationInfo di{};
    di.pool = texture_pool;
    di.maxBytesPerPass = max_bytes_a_pass;
    di.maxAllocationsPerPass = max_moves_a_pass;
    return vmaBeginDefragmentation(vma, &di, &compact_ctx) == VK_SUCCESS;
}
bool image_compact_active() { return compact_ctx != VK_NULL_HANDLE; }
bool image_compact_pass(std::vector<ImageMove>& moves, uint64_t* freed_bytes, uint32_t* freed_blocks) {
    moves.clear();
    if (!compact_ctx || compact_in_pass) return false;
    std::unique_lock rk(reap_mu);   // (nothing freed by the reaper from here until the pass ends: image_compact_pass_end)
    pass_waiting = true;   // (or a reaper with a queue begins its next free before this wakes, and the pass starves)
    reap_done_cv.wait(rk, [] { return !reaping; });   // (the frees in flight, done first)
    pass_waiting = false;
    const VkResult r = vmaBeginDefragmentationPass(vma, compact_ctx, &compact_pass);
    if (r != VK_INCOMPLETE) {   // (VK_SUCCESS: nothing left to move; or an error. The reaper, held for it, goes on)
        compact_over(freed_bytes, freed_blocks);
        rk.unlock();
        reap_cv.notify_one();
        return false;
    }
    compact_in_pass = true;
    for (uint32_t i = 0; i < compact_pass.moveCount; i++) moves.push_back({compact_pass.pMoves[i].srcAllocation});
    return true;
}
bool image_compact_bind(size_t i, VkImage image) {
    return compact_in_pass && i < compact_pass.moveCount &&
           vmaBindImageMemory(vma, compact_pass.pMoves[i].dstTmpAllocation, image) == VK_SUCCESS;
}
void image_compact_ignore(size_t i) {
    if (compact_in_pass && i < compact_pass.moveCount)
        compact_pass.pMoves[i].operation = VMA_DEFRAGMENTATION_MOVE_OPERATION_IGNORE;
}
bool image_compact_pass_end(uint64_t* freed_bytes, uint32_t* freed_blocks) {
    if (!compact_ctx || !compact_in_pass) return false;
    bool more;
    {
        std::lock_guard rk(reap_mu);   // (the pass ended before the reaper frees again)
        compact_in_pass = false;
        more = vmaEndDefragmentationPass(vma, compact_ctx, &compact_pass) == VK_INCOMPLETE;
    }
    reap_cv.notify_one();
    return more || compact_over(freed_bytes, freed_blocks);
}

// (mem_account.h) what every image made here takes, by its allocation: the textures', the render targets', the upscalers', the
// scratch ones. vk_texture.cpp takes the ones it knows off it, and the rest is the budget line's "other images". Counted apart
// by where the memory is, [0] a device-local heap and [1] system memory: the last resort below puts an image there when VRAM is full
static std::atomic<uint64_t> image_bytes[2]{};
// (longshot) the user data of the allocations made in the render targets' blocks: allocation_fit_info tells them by it. VMA never
// makes memory of their own there (their pool's block size is set), so another target may be bound to one (pool_take_memory)
char target_blocks_tag;
/// Whether an image may be in the targets' blocks: their memory type among those it can use, which VMA does not check for a pool
/// (not known: as before, yes). From the requirements create asked for already, where it did (need)
bool target_pool_takes(const VkImageCreateInfo& ci, ImageNeed* need) {
    VkMemoryRequirements asked{};
    const VkMemoryRequirements* mr = need ? need->get() : image_requirements(ci, &asked) ? &asked : nullptr;
    return !mr || (mr->memoryTypeBits >> target_image_pool_type & 1);
}
bool create_image(const VkImageCreateInfo& ci, VkImage& image, VmaAllocation& allocation, bool dedicated, bool target, bool textures,
                  ImageNeed* need) {
    const auto made = [&] {
        return image_bytes[allocation_in_vram(allocation) ? 0 : 1].fetch_add(allocation_size(allocation), std::memory_order_relaxed), true;
    };
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    // (in VRAM or not at all: an image VMA puts in system memory when VRAM is full is read over PCIe every frame,
    // and takes the RAM the game and everything else on the machine need)
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    ai.priority = memory_priority::image(memory_tiers(), target, textures);   // (a memory object of its own; a pool's block has its pool's)
    if (dev.memory_budget) ai.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
    if (dedicated) {   // (a memory object of its own, which freeing gives back whole: a pool's block is kept by VMA when it empties)
        VmaAllocationCreateInfo dai = ai;
        dai.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (vmaCreateImage(vma, &ci, &dai, &image, &allocation, nullptr) == VK_SUCCESS) return made();
    }
    if (VmaPool pool = textures && textures_pool() ? textures_pool()
                       : target && target_images_pool() ? target_images_pool() : images_pool()) {
        VmaAllocationCreateInfo pai = ai;
        pai.pool = pool;
        pai.usage = VMA_MEMORY_USAGE_UNKNOWN;
        if (pool == target_image_pool) pai.pUserData = &target_blocks_tag;
        if ((pool != target_image_pool || target_pool_takes(ci, need)) && vmaCreateImage(vma, &ci, &pai, &image, &allocation,
                                                                                     nullptr) == VK_SUCCESS)
            return made();
    }
    if (vmaCreateImage(vma, &ci, &ai, &image, &allocation, nullptr) == VK_SUCCESS) return made();
    if (ai.flags & VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT) {
        static std::atomic<int> told_budget{0};
        if (told_budget++ < 10) logf("vk: image over budget, retrying without WITHIN_BUDGET");
        VmaAllocationCreateInfo vai = ai;
        vai.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        if (vmaCreateImage(vma, &ci, &vai, &image, &allocation, nullptr) == VK_SUCCESS) return made();
    }
    // (longshot) VRAM is full (a 4 GB card at a high resolution, #96): an image left unmade is a black screen, so it goes
    // to system memory instead, slow but drawn
    VmaAllocationCreateInfo sai{};
    sai.usage = VMA_MEMORY_USAGE_AUTO;
    sai.priority = ai.priority;
    if (vmaCreateImage(vma, &ci, &sai, &image, &allocation, nullptr) != VK_SUCCESS) return false;
    static std::atomic<int> told{0};
    if (told++ < 4) logf("vk: VRAM full: a %ux%u image (format %d) in system memory", ci.extent.width, ci.extent.height, (int)ci.format);
    return made();
}
void destroy_image(VkImage image, VmaAllocation allocation) {
    image_bytes[allocation_in_vram(allocation) ? 0 : 1].fetch_sub(allocation_size(allocation), std::memory_order_relaxed);
    vmaDestroyImage(vma, image, allocation);
}

// ---- (longshot) Images destroyed on a thread of their own (destroy_image_later): on NVIDIA's Windows driver a vmaDestroyImage
// is a kernel call of a millisecond and more, and texture_frame made them on the render thread, many in one present when the
// camera turned (Project ReLink measured 2.4 ms of each frame over 25 ms there, against 0.1 ms of the others: 31689010,
// 5224e821). The views, the buffer and the memory objects that go with an image go with it. VMA is thread-safe here (no
// externally synchronized flag, vma_init); an image here is one nothing can use again (texture_frame: its last recorded
// use has finished). The pool's compaction frees nothing while a pass is open (image_compact_pass): the reaper waits for
// it, and a pass waits for the frees in flight. RX_VK_REAPER=0: destroyed where they were, as before.
// What the queue holds is bounded: past reap_cap() (an 8th of the driver's VRAM allowance, 128 MB to 1 GB: a safety net above an
// arrival's burst, 444 images and 255 MB and more on a 10 GB card) an image handed over is destroyed where it was, as before;
// near the allowance the arbiter (mem_account.h, the pool "reaper") has the queue freed at once on its thread (reap_now). A pass open holds the queue as main's dead list held its images then. A line every 10 s
// says what went and the queue's most (tests/test_image_reaper.py runs this against stubs) ----
namespace {
struct Reaped {
    VkImage image;
    VmaAllocation allocation;
    std::vector<VkImageView> views;
    VkBuffer buffer;
    VmaAllocation buffer_alloc;
    std::vector<VkDeviceMemory> memory;
    uint64_t image_bytes = 0, buffer_bytes = 0;   // (allocation_size, at the hand-over)
    int ledger = 0, buffer_ledger = 0;            // (0: VRAM, 1: system memory: mem_account.h's Ledger)
    // (by ledger, what its free gives back to the driver: its dedicated allocations' bytes. A share of a block leaves the block
    // with VMA, and the sparse chunks' sizes are not known here: 0, so the arbiter goes on asking the other pools)
    uint64_t returns[2]{};
    /// Something of it in that ledger: its image, its buffer, or (VRAM) a sparse texture's chunks
    bool in(int l) const { return (image_bytes && ledger == l) || (buffer_bytes && buffer_ledger == l) || (l == 0 && !memory.empty()); }
};
std::deque<Reaped> reap_queue;   // (under reap_mu)
// (under reap_mu: what the queue and the frees in flight hold, by ledger; the queue's most since the last line; what went)
uint64_t reap_image_bytes[2]{}, reap_buffer_bytes[2]{};
uint64_t reap_peak_bytes = 0, reap_peak_count = 0, reaped_n = 0, reaped_bytes = 0, reaped_in_place = 0;
uint64_t reap_held() { return reap_image_bytes[0] + reap_image_bytes[1] + reap_buffer_bytes[0] + reap_buffer_bytes[1]; }
void reap(Reaped& r) {   // (destroy_image and destroy_buffer: their counts, mem_account.h, go down with them)
    for (VkImageView v : r.views) vkDestroyImageView(dev.device, v, nullptr);
    if (r.image) destroy_image(r.image, r.allocation);
    if (r.buffer) destroy_buffer(r.buffer, r.buffer_alloc);
    for (VkDeviceMemory m : r.memory) vkFreeMemory(dev.device, m, nullptr);
}
void reaped_one(const Reaped& r, bool in_place, bool queued = true) {   // (under reap_mu, once its free is done)
    if (queued) reap_image_bytes[r.ledger] -= r.image_bytes, reap_buffer_bytes[r.buffer_ledger] -= r.buffer_bytes;
    reaped_n++, reaped_bytes += r.image_bytes + r.buffer_bytes, reaped_in_place += in_place;
}
/// (under reap_mu) the reaper's line, at most every 10 s and only when something went
void reap_tell(std::chrono::steady_clock::time_point now) {
    static std::chrono::steady_clock::time_point told_at = now;
    if (now - told_at < std::chrono::seconds(10) || !reaped_n) return;
    told_at = now;
    logf("vk: reaper: %llu images (%llu MB) destroyed in 10 s, %llu of them where they were handed over (its cap or VRAM "
         "pressure); the queue at most %llu (%llu MB)", (unsigned long long)reaped_n, (unsigned long long)(reaped_bytes >> 20),
         (unsigned long long)reaped_in_place, (unsigned long long)reap_peak_count, (unsigned long long)(reap_peak_bytes >> 20));
    reaped_n = reaped_bytes = reaped_in_place = 0;
    reap_peak_count = reap_queue.size(), reap_peak_bytes = reap_held();
}
void reaper() {
    SetThreadDescription(GetCurrentThread(), L"mkw_vk reaper");
    std::unique_lock lk(reap_mu);
    for (;;) {
        reap_cv.wait(lk, [] { return !reap_queue.empty() && !compact_in_pass && !pass_waiting; });
        Reaped r = std::move(reap_queue.front());
        reap_queue.pop_front();
        // (freed outside the lock, so that the render thread hands more over meanwhile: under it, the lock was held through
        // the whole queue, and destroy_image_later waited for every free before its own, as on the render thread before. A
        // pass that begins waits for this one: reaping, image_compact_pass)
        reaping++;
        lk.unlock();
        reap(r);
        lk.lock();
        reaped_one(r, false);
        reap_tell(std::chrono::steady_clock::now());
        if (!--reaping) reap_done_cv.notify_all();
    }
}
bool reaper_on() {
    static const bool on = [] {
        const bool v = env_on_or("RX_VK_REAPER", 1);   // (longshot_env.h: unset is on)
        if (!v) logf("vk: images destroyed on the render thread, as before (RX_VK_REAPER=0)");
        return v;
    }();
    return on;
}
/// The most the queue holds: an 8th of the driver's VRAM allowance, 128 MB to 1 GB (256 MB when the allowance is unknown)
uint64_t reap_cap() {
    static const uint64_t cap = []() -> uint64_t {
        uint64_t used = 0, allowed = 0;
        if (!vram_usage(&used, &allowed) || !allowed) return 256ull << 20;
        return std::clamp<uint64_t>(allowed / 8, 128ull << 20, 1024ull << 20);
    }();
    return cap;
}
bool reaper_started = false;   // (under reap_mu; false again when no thread could be made)
}  // namespace
void destroy_image_later(VkImage image, VmaAllocation allocation, std::vector<VkImageView> views, VkBuffer buffer,
                         VmaAllocation buffer_alloc, std::vector<VkDeviceMemory> memory) {
    Reaped r{image, allocation, std::move(views), buffer, buffer_alloc, std::move(memory)};
    if (!reaper_on()) {
        reap(r);
        return;
    }
    r.image_bytes = allocation_size(allocation), r.ledger = allocation_in_vram(allocation) ? 0 : 1;
    r.buffer_bytes = buffer_alloc ? allocation_size(buffer_alloc) : 0, r.buffer_ledger = allocation_in_vram(buffer_alloc) ? 0 : 1;
    if (allocation_dedicated(allocation)) r.returns[r.ledger] += r.image_bytes;
    if (allocation_dedicated(buffer_alloc)) r.returns[r.buffer_ledger] += r.buffer_bytes;
    const uint64_t cap = reap_cap();   // (before the lock: the first one asks the driver)
    {
        std::unique_lock lk(reap_mu);
        if (!reaper_started) {
            try {
                std::thread(reaper).detach();   // (the device lives as long as the process: nothing to join)
            } catch (...) {   // (no thread: destroyed here, as before; the next one tries again)
                lk.unlock();
                reap(r);
                return;
            }
            reaper_started = true;
            logf("vk: images are destroyed on a thread of their own (RX_VK_REAPER=0: on the render thread)");
        }
        // (past the cap: destroyed here, as before, unless a pass is open, when nothing of the pools may be freed: then it waits
        // for the pass's end, as it waited in texture_frame's dead list)
        if (!reap_queue.empty() && reap_held() + r.image_bytes + r.buffer_bytes > cap && !compact_in_pass) {
            reaping++;
            lk.unlock();
            reap(r);
            lk.lock();
            reaped_one(r, true, false);   // (never queued: its bytes were not added)
            if (!--reaping) reap_done_cv.notify_all();
            return;
        }
        reap_image_bytes[r.ledger] += r.image_bytes, reap_buffer_bytes[r.buffer_ledger] += r.buffer_bytes;
        reap_queue.push_back(std::move(r));
        reap_peak_count = std::max<uint64_t>(reap_peak_count, reap_queue.size()), reap_peak_bytes = std::max(reap_peak_bytes, reap_held());
    }
    reap_cv.notify_one();
}
size_t images_to_reap() {
    std::lock_guard lk(reap_mu);   // (the reaper holds it only to take one off the queue)
    return reap_queue.size() + (size_t)reaping;
}
void reaper_bytes(uint64_t images[2], uint64_t buffers[2]) {
    std::lock_guard lk(reap_mu);
    for (int l = 0; l < 2; l++) images[l] = reap_image_bytes[l], buffers[l] = reap_buffer_bytes[l];
}
uint64_t reap_now(uint64_t want, bool vram) {
    std::vector<Reaped> taken;
    uint64_t got = 0;   // (what goes back to the driver: the dedicated allocations' bytes in the ledger asked)
    const int l = vram ? 0 : 1;
    {
        std::lock_guard lk(reap_mu);
        if (compact_in_pass) return 0;   // (nothing of the pools freed inside a pass)
        // (every entry with something in that ledger, until what goes back covers want: the shares of blocks give nothing back
        // the driver counts, so with only those queued all of them go, their space for the next images of the blocks)
        for (auto it = reap_queue.begin(); it != reap_queue.end() && got < want;) {
            if (!it->in(l)) {
                ++it;
                continue;
            }
            got += it->returns[l];
            taken.push_back(std::move(*it));
            it = reap_queue.erase(it);
        }
        reaping += (int)!taken.empty();
    }
    if (taken.empty()) return 0;
    for (Reaped& r : taken) reap(r);
    std::lock_guard lk(reap_mu);
    for (const Reaped& r : taken) reaped_one(r, true);
    if (!--reaping) reap_done_cv.notify_all();
    return got;
}
uint64_t allocation_size(VmaAllocation allocation) {
    VmaAllocationInfo info{};
    if (allocation) vmaGetAllocationInfo(vma, allocation, &info);
    return info.size;
}
bool allocation_dedicated(VmaAllocation allocation) {
    if (!allocation) return false;
    VmaAllocationInfo2 info{};
    vmaGetAllocationInfo2(vma, allocation, &info);
    return info.dedicatedMemory == VK_TRUE;
}
uint64_t images_made_bytes(bool in_vram) { return image_bytes[in_vram ? 0 : 1].load(std::memory_order_relaxed); }
bool allocation_in_vram(VmaAllocation allocation) {
    if (!allocation) return false;
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(vma, allocation, &info);
    return info.memoryType < dev.memory.memoryTypeCount &&
           (dev.memory.memoryHeaps[dev.memory.memoryTypes[info.memoryType].heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
}
bool system_heap_exists() {
    for (uint32_t h = 0; h < dev.memory.memoryHeapCount; h++)
        if (!(dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) return true;
    return false;
}
// (mem_account.h) and what every buffer made here takes, and every sparse mirror's block: [0] in a device-local heap, [1] in
// system memory. With the images' count these are all the allocator holds for the renderer, so what the budget line leaves
// "unaccounted" is what the allocator never made. The pools take the buffers they know off it; the rest is "other buffers"
static std::atomic<uint64_t> buffer_bytes[2]{};
static void buffer_count(VmaAllocation allocation, bool made) {
    std::atomic<uint64_t>& n = buffer_bytes[allocation_in_vram(allocation) ? 0 : 1];
    if (made) n.fetch_add(allocation_size(allocation), std::memory_order_relaxed);
    else n.fetch_sub(allocation_size(allocation), std::memory_order_relaxed);
}
uint64_t buffers_made_bytes(bool in_vram) { return buffer_bytes[in_vram ? 0 : 1].load(std::memory_order_relaxed); }
bool allocation_fit_info(VmaAllocation allocation, uint64_t* bytes, uint64_t* offset, uint32_t* memory_type, bool* dedicated) {
    if (!allocation) return false;
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(vma, allocation, &info);
    *bytes = info.size, *offset = info.offset, *memory_type = info.memoryType;
    *dedicated = info.pUserData != &target_blocks_tag;   // (not known to be a share of the targets' blocks: taken as its own)
    return true;
}
bool image_requirements(const VkImageCreateInfo& ci, VkMemoryRequirements* mr) {
    if (!vkGetDeviceImageMemoryRequirements) return false;
    VkDeviceImageMemoryRequirements in{VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS};
    in.pCreateInfo = &ci;
    VkMemoryDedicatedRequirements own{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 out{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &own};
    vkGetDeviceImageMemoryRequirements(dev.device, &in, &out);
    *mr = out.memoryRequirements;
    return mr->size != 0 && !own.requiresDedicatedAllocation;   // (an image that must have memory of its own: never on another's)
}
uint64_t target_block_bytes() {
    static const uint64_t v =
        device_local_heap_bytes() <= vram_compact::GLOBAL_MIN_GRAPHICS_REQ ? std::min<uint64_t>(kImageBlock, 16ull << 20) : kImageBlock;
    return v;
}
uint32_t device_local_types() {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < dev.memory.memoryTypeCount && i < 32; i++)
        if (dev.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) mask |= 1u << i;
    return mask;
}
bool create_image_in_class(ImageNeed& need, VkImage& image, VmaAllocation& allocation) {
    image = VK_NULL_HANDLE, allocation = nullptr;
    const VmaPool pool = target_images_pool();
    const VkMemoryRequirements* mr = need.get();
    // (VMA allocates in the pool's memory type whatever the requirements say: one the image cannot use, made as before)
    if (!pool || !mr || !(mr->memoryTypeBits >> target_image_pool_type & 1)) return false;
    const uint64_t cls = target_pool::class_bytes(mr->size, target_block_bytes());
    if (!cls) return false;
    // (the class's size, and an alignment any image of it may ask: what the next image on this allocation will need)
    VkMemoryRequirements cr = *mr;
    cr.size = cls, cr.alignment = std::max<VkDeviceSize>(mr->alignment, target_pool::kClassAlign);
    VmaAllocationCreateInfo ai{};
    ai.pool = pool;
    ai.pUserData = &target_blocks_tag;
    // (a render target's, as create_image gives one: VMA takes the pool's own for a pool's block, this one stands where it does not)
    ai.priority = memory_priority::image(memory_tiers(), true, false);
    if (dev.memory_budget) ai.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
    if (vmaAllocateMemory(vma, &cr, &ai, &allocation, nullptr) != VK_SUCCESS) return allocation = nullptr, false;
    if (vmaCreateAliasingImage(vma, allocation, need.ci, &image) != VK_SUCCESS) {
        vmaFreeMemory(vma, allocation);
        image = VK_NULL_HANDLE, allocation = nullptr;
        return false;
    }
    image_bytes[allocation_in_vram(allocation) ? 0 : 1].fetch_add(allocation_size(allocation), std::memory_order_relaxed);
    return true;
}
bool create_aliasing_image(const VkImageCreateInfo& ci, VmaAllocation allocation, VkImage& image) {
    image = VK_NULL_HANDLE;
    return vmaCreateAliasingImage(vma, allocation, &ci, &image) == VK_SUCCESS;
}
// (longshot, #1879) RX_VK_BUFFER_BUDGET=0: a host-visible buffer made where VMA prefers it whatever its heap's budget, as before
static bool buffer_budget_on() {
    static const bool on = [] {
        const bool v = env_on_or("RX_VK_BUFFER_BUDGET", 1);   // (longshot_env.h: unset is on)
        if (!v) logf("vk: host-visible buffers made where VMA prefers them whatever their heap's budget, as before (RX_VK_BUFFER_BUDGET=0)");
        return v;
    }();
    return on;
}
bool create_buffer(uint64_t size, VkBufferUsageFlags usage, bool host_visible, VkBuffer& buffer,
                   VmaAllocation& allocation, void** mapped, bool dedicated) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.priority = memory_tiers().rest;
    if (host_visible) ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    // (a buffer with VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT gets VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT from VMA:
    // vma_init sets VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT)
    if (dedicated) ai.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    VmaAllocationInfo info{};
    // (longshot, #1879) a host-visible buffer first where its heap's budget has room: asked to stay within the budgets, VMA takes
    // the next memory type the buffer can use when the heap of the one it prefers is full. Not asked, it put a host-visible buffer
    // the GPU reads (the staging arenas: STORAGE, for the ASTC decoder) in DEVICE_LOCAL | HOST_VISIBLE whatever that heap held. On
    // a Legion Go (Radeon 780M, 8 GB UMA, no resizable BAR) that heap is the carve-out's 256 MB window, of which AMD's Windows
    // driver allows 243 MB; a save load's or an area's staging took it to 252-277 MB while VRAM as a whole stood at 1.9 of 7.8 GB,
    // and the driver refused every submit after (VK_ERROR_OUT_OF_DEVICE_MEMORY): a black screen, the sound going on. Now such an
    // arena goes to system memory, which the GPU reads as well (the same RAM on an APU). Where no heap has room, made as before
    VkResult made = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (host_visible && dev.memory_budget && buffer_budget_on()) {
        VmaAllocationCreateInfo within = ai;
        within.flags |= VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        made = vmaCreateBuffer(vma, &bi, &within, &buffer, &allocation, &info);
    }
    if (made != VK_SUCCESS && vmaCreateBuffer(vma, &bi, &ai, &buffer, &allocation, &info) != VK_SUCCESS) return false;
    buffer_count(allocation, true);
    if (mapped) *mapped = info.pMappedData;
    return true;
}
/// A buffer in cached host memory, mapped: written on one thread, read back on the CPU too (the captures: the game's
/// threads copy pages into it, the replay reads texture pages out of it and the GPU copies the rest from it)
bool create_buffer_cached(uint64_t size, VkBufferUsageFlags usage, VkBuffer& buffer, VmaAllocation& allocation,
                          void** mapped, bool dedicated) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    ai.priority = memory_tiers().rest;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    if (dedicated) ai.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;   // (its own memory object: no part of a 64 MB block left unused)
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(vma, &bi, &ai, &buffer, &allocation, &info) != VK_SUCCESS) return false;
    if (mapped && !info.pMappedData) {
        vmaDestroyBuffer(vma, buffer, allocation);
        buffer = VK_NULL_HANDLE;
        allocation = nullptr;
        *mapped = nullptr;
        return false;
    }
    buffer_count(allocation, true);
    if (mapped) *mapped = info.pMappedData;
    return true;
}
bool host_cached(VmaAllocation allocation) {
    VkMemoryPropertyFlags f = 0;
    if (allocation) vmaGetAllocationMemoryProperties(vma, allocation, &f);
    return (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0;
}
bool host_coherent(VmaAllocation allocation) {
    VkMemoryPropertyFlags f = 0;
    if (allocation) vmaGetAllocationMemoryProperties(vma, allocation, &f);
    return (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
}
void flush_mapped(VmaAllocation allocation, uint64_t size) {
    if (size) vmaFlushAllocation(vma, allocation, 0, size);
}
void flush_mapped_range(VmaAllocation allocation, uint64_t offset, uint64_t size) {
    if (size) vmaFlushAllocation(vma, allocation, offset, size);
}
void invalidate_mapped(VmaAllocation allocation, uint64_t size) {
    if (size) vmaInvalidateAllocation(vma, allocation, 0, size);
}
bool flush_mapped_checked(VmaAllocation allocation, uint64_t size) {
    return !size || vmaFlushAllocation(vma, allocation, 0, size) == VK_SUCCESS;
}
bool invalidate_mapped_checked(VmaAllocation allocation, uint64_t size) {
    return !size || vmaInvalidateAllocation(vma, allocation, 0, size) == VK_SUCCESS;
}
void destroy_buffer(VkBuffer buffer, VmaAllocation allocation) {
    if (allocation) buffer_count(allocation, false);
    vmaDestroyBuffer(vma, buffer, allocation);
}
bool create_device_buffer(uint64_t size, VkBufferUsageFlags usage, VkBuffer& buffer, VmaAllocation& allocation, bool dedicated) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    ai.priority = memory_tiers().rest;
    if (dev.memory_budget) ai.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
    if (dedicated) ai.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    if (vmaCreateBuffer(vma, &bi, &ai, &buffer, &allocation, nullptr) != VK_SUCCESS) return false;
    buffer_count(allocation, true);
    return true;
}
/// (longshot, RX_VK_MIRROR_SPARSE) A buffer with sparse residency and no memory: its blocks are bound one by one
/// (vk_memory.cpp, sparse_mirror.h). req: its size, block (alignment) and memory types
bool create_sparse_buffer(uint64_t size, VkBufferUsageFlags usage, VkBuffer& buffer, VkMemoryRequirements& req) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev.device, &bi, nullptr, &buffer) != VK_SUCCESS) {
        buffer = VK_NULL_HANDLE;
        return false;
    }
    vkGetBufferMemoryRequirements(dev.device, buffer, &req);
    return true;
}
/// One block of device memory for a sparse buffer (req's alignment and memory types), from VMA's blocks: counted in its
/// budget as the rest of the device's memory is
bool alloc_sparse_block(const VkMemoryRequirements& req, VmaAllocation& allocation, VkDeviceMemory& memory, uint64_t& offset) {
    VkMemoryRequirements r = req;
    r.size = req.alignment;
    VmaAllocationCreateInfo ai{};
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    ai.priority = memory_tiers().rest;
    if (dev.memory_budget) ai.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
    VmaAllocationInfo info{};
    if (vmaAllocateMemory(vma, &r, &ai, &allocation, &info) != VK_SUCCESS) {
        allocation = nullptr;
        return false;
    }
    buffer_count(allocation, true);
    memory = info.deviceMemory;
    offset = info.offset;
    return true;
}
void free_sparse_block(VmaAllocation allocation) {
    if (allocation) buffer_count(allocation, false), vmaFreeMemory(vma, allocation);
}
void destroy_sparse_buffer(VkBuffer buffer) {   // (its blocks' memory is the caller's to free, after)
    if (buffer) vkDestroyBuffer(dev.device, buffer, nullptr);
}
/// (every heap: what VMA has in blocks and allocations there, what the process uses of it, its budget; one line)
/// The image pools: their blocks, the images in them, and the free ranges between them (every 600 presents, with the heaps)
void vma_log_image_blocks() {
    for (VmaPool pool : {image_pool, target_image_pool, texture_pool}) {
        if (!pool) continue;
        VmaDetailedStatistics d{};
        vmaCalculatePoolStatistics(vma, pool, &d);
        logf("vk: image blocks%s: %u blocks %llu MB, %u images %llu MB; %u free ranges, the largest %llu MB",
             pool == target_image_pool ? " (render targets)" : pool == texture_pool ? " (textures, compacted)" : "", d.statistics.blockCount,
             (unsigned long long)(d.statistics.blockBytes >> 20), d.statistics.allocationCount,
             (unsigned long long)(d.statistics.allocationBytes >> 20), d.unusedRangeCount,
             (unsigned long long)(d.unusedRangeSizeMax >> 20));
    }
}
void vma_log_heaps() {
    VmaBudget b[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(vma, b);
    char line[1024];
    int n = snprintf(line, sizeof line, "vk: memory heaps:");
    for (uint32_t h = 0; h < dev.memory.memoryHeapCount && n < 900; h++)
        n += snprintf(line + n, sizeof line - n, " heap %u%s: %u blocks %llu MB, %u allocations %llu MB; used %llu of %llu MB;", h,
                      (dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " (device)" : " (system)",
                      b[h].statistics.blockCount, (unsigned long long)(b[h].statistics.blockBytes >> 20),
                      b[h].statistics.allocationCount, (unsigned long long)(b[h].statistics.allocationBytes >> 20),
                      (unsigned long long)(b[h].usage >> 20), (unsigned long long)(b[h].budget >> 20));
    logf("%s", line);
}
bool vram_usage(uint64_t* used, uint64_t* budget, uint64_t* block_free) {
    if (!vma) {   // (not made yet: memory_init, which can start RX_VK_LOG_RAM's thread, runs before vma_init)
        *used = *budget = 0;
        if (block_free) *block_free = 0;
        return false;
    }
    VmaBudget b[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(vma, b);
    uint64_t u = 0, t = 0, f = 0;
    for (uint32_t h = 0; h < dev.memory.memoryHeapCount; h++)
        if (dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            u += b[h].usage, t += b[h].budget;
            // (the free space in VMA's blocks: the driver counts it as the process's, new allocations take it first)
            f += b[h].statistics.blockBytes - std::min(b[h].statistics.blockBytes, b[h].statistics.allocationBytes);
        }
    // (a test: RX_VK_TEST_ALLOWED_MB=<n> reports what the driver allows as at most n MB, so a 16 GB card plays the eviction and
    // keep-rendered paths an 8 GB card takes when its VRAM fills)
    static const uint64_t test_allowed = getenv("RX_VK_TEST_ALLOWED_MB") ? (uint64_t)atoi(getenv("RX_VK_TEST_ALLOWED_MB")) << 20 : 0;
    if (test_allowed && t > test_allowed) t = test_allowed;
    // (and RX_VK_TEST_ALLOWED_FILE=<file>: the same bound read from that file's number of MB, looked at again once a second, so
    // a run can shrink the allowance in play, as Windows does when another program takes VRAM. A missing file or 0 leaves the
    // driver's)
    static const char* test_file = getenv("RX_VK_TEST_ALLOWED_FILE");
    if (test_file) {
        static std::atomic<uint64_t> file_allowed{0};
        static std::atomic<int64_t> read_at{0};
        const int64_t now = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now - read_at.load(std::memory_order_relaxed) >= 1000) {
            read_at.store(now, std::memory_order_relaxed);
            uint64_t mb = 0;
            if (FILE* fp = fopen(test_file, "r")) {
                unsigned long long v = 0;
                if (fscanf(fp, "%llu", &v) == 1) mb = v;
                fclose(fp);
            }
            if ((mb << 20) != file_allowed.load(std::memory_order_relaxed))
                logf("vk: test allowance (RX_VK_TEST_ALLOWED_FILE): %llu MB%s", (unsigned long long)mb, mb ? "" : " (the driver's)");
            file_allowed.store(mb << 20, std::memory_order_relaxed);
        }
        const uint64_t fa = file_allowed.load(std::memory_order_relaxed);
        if (fa && t > fa) t = fa;
    }
    // (and RX_VK_TEST_ALLOWED_FOLLOW=1: an allowance the count is past is reported as the count, as AMD's Windows driver does,
    // so the rule below can be seen at work on any card)
    static const bool test_follow = env_on("RX_VK_TEST_ALLOWED_FOLLOW");
    if (test_follow && (test_allowed || test_file) && t && u > t) t = u;
    // (longshot) an allowance the driver gives as the count itself is past: AMD's Windows driver, once a process is over what it
    // allows, reports the allowance as what the process uses (5940 of 5940 MB, the allowance rising with the count). Taken as it
    // is, the count is within it: room in the blocks keeps the 97% squeeze waiting and the hold's line rises with the count, while
    // the driver pages: frames of seconds. vram_compact::past_allowance makes it a tenth under the count, so every rule sees the
    // count past it and gives back until the driver reports an allowance over the count again
    {
        const uint64_t was = t;
        t = vram_compact::past_allowance(u, t);
        static std::atomic<int> told{0};
        if (t != was && told.fetch_add(1, std::memory_order_relaxed) < 8)
            logf("vk: the driver's allowance is the count itself (%llu MB used of %llu): past it, taken as %llu MB",
                 (unsigned long long)(u >> 20), (unsigned long long)(was >> 20), (unsigned long long)(t >> 20));
    }
    *used = u, *budget = t;
    if (block_free) *block_free = f;
    return t != 0;
}

/// The system-memory heaps the GPU reaches: what the process uses there (the imported guest memory too: pinned by the
/// driver) and what the driver allows it, asked now (a new frame index makes VMA ask the driver again); false when unknown
namespace {
std::atomic<uint32_t> budget_frame{0};   // (VMA's frame index: a new one makes it ask the driver for the budgets again)
}
bool vram_usage_now(uint64_t* used, uint64_t* budget) {
    vmaSetCurrentFrameIndex(vma, ++budget_frame);
    return vram_usage(used, budget) && dev.memory_budget;
}
bool system_heap_usage(uint64_t* used, uint64_t* budget) {
    vmaSetCurrentFrameIndex(vma, ++budget_frame);
    VmaBudget b[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(vma, b);
    uint64_t u = 0, t = 0;
    for (uint32_t h = 0; h < dev.memory.memoryHeapCount; h++)
        if (!(dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) u += b[h].usage, t += b[h].budget;
    *used = u, *budget = t;
    return dev.memory_budget && t != 0;
}

} // namespace mkwvk
