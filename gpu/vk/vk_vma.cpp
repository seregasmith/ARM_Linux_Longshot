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
#include <cstdio>
#include <cstdlib>
#include <vector>

#ifndef LONGSHOT_ENV_H   // (env_on: the switches' one rule)
#include "longshot_env.h"
#endif
#include "vram_compact.h"
#include "memory_priority.h"
#include "vk_work.h"   // (last: RX_VK_RECORDER=defer drains before the destructions and the invalidate below)

namespace mkwvk {

VmaAllocator vma;

/// Largest DEVICE_LOCAL heap size in bytes reported by Vulkan physical device memory properties.
static uint64_t device_local_heap_bytes() {
    uint64_t heap = 0;
    for (uint32_t h = 0; h < dev.memory.memoryHeapCount; h++)
        if (dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) heap = std::max<uint64_t>(heap, test_vram_bytes(dev.memory.memoryHeaps[h].size));   // (RX_VK_TEST_VRAM_MB, #1107)
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
    const VkResult r = vmaBeginDefragmentationPass(vma, compact_ctx, &compact_pass);
    if (r != VK_INCOMPLETE) return compact_over(freed_bytes, freed_blocks);   // (VK_SUCCESS: nothing left to move; or an error)
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
    compact_in_pass = false;
    if (vmaEndDefragmentationPass(vma, compact_ctx, &compact_pass) == VK_INCOMPLETE) return true;
    return compact_over(freed_bytes, freed_blocks);
}

// (mem_account.h) what every image made here takes, by its allocation: the textures', the render targets', the upscalers', the
// scratch ones. vk_texture.cpp takes the ones it knows off it, and the rest is the budget line's "other images". Counted apart
// by where the memory is, [0] a device-local heap and [1] system memory: the last resort below puts an image there when VRAM is full
static std::atomic<uint64_t> image_bytes[2]{};
bool create_image(const VkImageCreateInfo& ci, VkImage& image, VmaAllocation& allocation, bool dedicated, bool target, bool textures) {
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
        if (vmaCreateImage(vma, &ci, &pai, &image, &allocation, nullptr) == VK_SUCCESS) return made();
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
uint64_t allocation_size(VmaAllocation allocation) {
    VmaAllocationInfo info{};
    if (allocation) vmaGetAllocationInfo(vma, allocation, &info);
    return info.size;
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
    if (vmaCreateBuffer(vma, &bi, &ai, &buffer, &allocation, &info) != VK_SUCCESS) return false;
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
    // keep-rendered paths an 8 GB card takes when its VRAM fills (the rainbow textures of issue 993 were never seen here otherwise))
    static const uint64_t test_allowed = getenv("RX_VK_TEST_ALLOWED_MB") ? (uint64_t)atoi(getenv("RX_VK_TEST_ALLOWED_MB")) << 20 : 0;
    if (test_allowed && t > test_allowed) t = test_allowed;
    // (and RX_VK_TEST_ALLOWED_FILE=<file>: the same bound read from that file's number of MB, looked at again once a second, so
    // a run can shrink the allowance in play, as Windows does when another program takes VRAM: an RX 6600 went from 7378 to
    // 5940 MB allowed in Hyrule Field (#1025). A missing file or 0 leaves the driver's)
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
    // (and RX_VK_TEST_ALLOWED_FOLLOW=1: an allowance the count is past is reported as the count, as AMD's Windows driver does
    // (#933, #1025), so the rule below can be seen at work on any card)
    static const bool test_follow = env_on("RX_VK_TEST_ALLOWED_FOLLOW");
    if (test_follow && (test_allowed || test_file) && t && u > t) t = u;
    // (longshot) an allowance the driver gives as the count itself is past: AMD's Windows driver, once a process is over what it
    // allows, reports the allowance as what the process uses. An RX 6600 at 3.4 of 7.2 GB entered Hyrule Field and sat at 5940 of
    // 5940 MB; another went from 5.3 of 6.5 GB to 6.6 of 6.6, the allowance rising with the count. Taken as it is, the count is
    // within it: room in the blocks kept the 97% squeeze waiting and the hold's line rose with the count, while the driver paged:
    // frames of 1.2-15.7 s (#933, #1025). vram_compact::past_allowance makes it a tenth under the count, so every rule sees the
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
