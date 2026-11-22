// The render target pool's bound by default (vk_texture.cpp pool_cap): a twelfth of what the driver allows the process, from 256 MB
// to 768 MB. OoT makes its transient targets (a 384 x 384 depth target, 1280 x 720, 640 x 360 and 2560 x 1440 ones) at shifted
// offsets of a scratch heap every frame; a dead one is reusable a frame or two after it is let go, once the GPU is done with it, so
// the pool has to hold two or three frames of them. At 256 MB it held less than that at 2560 x 1440: in Castle Town's market at
// night it destroyed ~22,000 images in 100 s for being over its bound and the replay made as many again, two a frame (vkDestroyImage
// and vmaCreateImage ~20 % of the render thread). On a small card a twelfth stays near the old 256 MB.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace mkwvk::target_pool {

inline constexpr uint64_t kMinMB = 256, kMaxMB = 768;
// How long an image waits in the pool for the next target of its shape, in presents (RX_VK_POOL_IDLE). The dead targets come
// back to the pool every present (dead_targets.h), and a shape the game makes every frame is taken again within a present or two.
// One that waits longer is a shape that is not coming back: OoT gives each character near the camera a depth map whose side it
// computes anew every frame (397, 439, 479 ...), each a pool key of its own. At 120 presents those filled the pool to its bound.
inline constexpr uint64_t kIdlePresents = 8;

/// The default bound in MB for a driver allowance of `allowed` bytes (0: not known, the minimum)
inline uint64_t default_mb(uint64_t allowed) {
    if (!allowed) return kMinMB;
    return std::clamp<uint64_t>((allowed >> 20) / 12, kMinMB, kMaxMB);
}

/// Whether an image put into the pool at present `since` has waited too long at present `now`
inline bool idle_out(uint64_t now, uint64_t since, uint64_t idle) { return now - since > idle; }

// ---- (longshot, RX_VK_TARGET_REUSE_MEMORY) a render target whose create info missed the pool takes a pooled image's memory ----

/// RX_VK_TARGET_REUSE_MEMORY: 0 off (every target that misses the pool a new allocation of its size, as before), 1 a pooled
/// image's memory taken by size, 2 also the size classes below. Unset: 2 on an integrated GPU, the configuration measured there
/// (a Steam Deck OLED, Minimum preset, Render 50%: every run of the camera turn above every control of its block); 1 on a discrete
/// GPU, where the take costs nothing it does not save and the classes' memory is not measured. Level 1 with the integrated GPU's
/// hold is less measured, not shown worse
inline int reuse_level(const char* env, bool integrated) {
    if (!env || !*env) return integrated ? 2 : 1;
    const int v = atoi(env);
    return v <= 0 ? 0 : v >= 2 ? 2 : 1;
}

enum class Fit : uint8_t { fits, own_memory, type, alignment, too_small, too_big };
/// Whether a pooled render target's allocation can hold a new image whose create info missed the pool, or why not: a memory type
/// the new image can use, an offset its alignment divides, at least its size and at most 1.5 times it (what is left over is held as
/// long as the image is; or its own size class, which under 1 MB may be more), and not memory of its own, which Vulkan binds to the
/// one image it was made for (VkMemoryDedicatedAllocateInfo). OoT's shadow maps are the case: a side new every frame (397, 439,
/// 479 ...), so the pool's keys never match while one of nearly the same size waits in it.
inline Fit verdict(uint64_t pooled_bytes, uint64_t pooled_offset, uint32_t pooled_memory_type, bool pooled_dedicated,
                   uint64_t need_bytes, uint64_t need_alignment, uint32_t need_memory_type_bits);
inline bool fits(uint64_t pooled_bytes, uint64_t pooled_offset, uint32_t pooled_memory_type, bool pooled_dedicated,
                 uint64_t need_bytes, uint64_t need_alignment, uint32_t need_memory_type_bits) {
    return need_bytes && verdict(pooled_bytes, pooled_offset, pooled_memory_type, pooled_dedicated, need_bytes, need_alignment,
                                 need_memory_type_bits) == Fit::fits;
}

/// How many presents an image waits in the pool before a target of another shape may take its memory (pool_take_memory). A shape
/// the game makes every frame takes its image back within the present it came in, mostly: taking that one by size would only make
/// the next target of its shape miss and allocate in its place. Those that wait longer are the shapes not coming back
inline constexpr uint64_t kSizeTakeIdle = 1;

/// Whether an image put into the pool at present `since` may give its memory to another shape at present `now`
inline bool takeable_by_size(uint64_t now, uint64_t since, uint64_t min_idle) { return now - since >= min_idle; }

/// Why a render target whose create info missed the pool was not given a pooled image's memory either (the status line's "not
/// reused by size"): the pool empty (and whether targets of the pooled kind still waited in the dead queue for the GPU), none in
/// it a present yet, none of a kind it can use (memory of its own, memory type, alignment), all smaller, all over 1.5 times, some
/// of each, the image over a block of the targets' pool (memory of its own whatever waits), the driver's requirements refused,
/// or the image not bound
enum class Miss : uint8_t {
    empty, empty_dead_waiting, too_new, unusable, too_small, too_big, between, over_block, requirements, not_bound, count
};
/// What the scan of the pool saw, for miss_reason
struct Tally {
    uint32_t too_new = 0, unusable = 0, too_small = 0, too_big = 0;
    uint64_t largest_small = 0;   // (bytes: the largest of the smaller ones, how far short the nearest was)
};
inline void add(Tally& t, bool takeable, Fit f, uint64_t pooled_bytes) {
    if (!takeable) t.too_new++;
    else if (f == Fit::too_small) t.too_small++, t.largest_small = std::max(t.largest_small, pooled_bytes);
    else if (f == Fit::too_big) t.too_big++;
    else if (f != Fit::fits) t.unusable++;
}
/// A scan of a pool that was not empty, none of it taken
inline Miss miss_reason(const Tally& t) {
    if (t.too_small || t.too_big) return !t.too_big ? Miss::too_small : !t.too_small ? Miss::too_big : Miss::between;
    return t.unusable ? Miss::unusable : Miss::too_new;
}

// The pool's hold while a pooled image's memory can be taken by size, on an integrated GPU: a shape not coming back is then no
// longer memory held for nothing, it is the next new shape's allocation, and there each allocation is a kernel call. On a Steam
// Deck in a camera turn, 8 presents let 2,255-2,946 images go idle and 1,006-1,104 new targets found the pool empty; 64 left
// 22-29 empty, took 1,058-1,108 by size instead of 425-516, and the turn's frame rate rose past the control's spread. What bounds it
// then is the pool's byte cap (default_mb), halved or given back under pressure (pool_pressure). Holding without the take measured
// worse; on a discrete card the pool would fill to its cap, up to 768 MB of VRAM, for a gain measured only where an allocation is a
// kernel call
inline constexpr uint64_t kIdlePresentsReuse = 64;

/// How long an image waits in the pool for a target of its shape, in presents: RX_VK_POOL_IDLE as set (at least 1), else
/// kIdlePresentsReuse while the take by size is on (`reuse_level` 1 or 2) on an integrated GPU, kIdlePresents otherwise
inline uint64_t idle_presents(const char* env, int reuse_level, bool integrated) {
    if (env && *env) return (uint64_t)std::max(1, atoi(env));
    return reuse_level >= 1 && integrated ? kIdlePresentsReuse : kIdlePresents;
}

/// Size classes for the render targets' allocations (RX_VK_TARGET_REUSE_MEMORY=2): a target of the pooled kind is made on an
/// allocation of its requirement rounded up to a class, from 64 KiB in steps of a quarter, each a multiple of the 64 KiB the class
/// allocations are aligned to (so the class is all it takes in its block), the largest the block; its image takes what it needs of
/// it. OoT's shadow maps change side by a few texels a frame, mostly growing as a character nears: the image let go of last frame
/// was a few percent too small for this frame's. In a class, any image of it fits any pooled allocation of it. Where fewer than 8
/// fit a block, a class holds no fewer to a block than the requirement rounded to 64 KiB would (a 1280 x 800 RGBA16F target: two
/// to a 16 MB block, with or without). What it costs, over the requirement rounded to 64 KiB: at most a quarter, 6% on average over
/// sizes spread evenly from 64 KiB to 16 MB, 8.7% over square D32 shadow maps of 384 to 1920; over the exact requirement 16.5% on
/// average (the small ones' 64 KiB steps). Counted where the memory is: the budget (x.pad) and the pool's bytes take the class
inline constexpr uint64_t kClassFloor = 64 << 10, kClassAlign = 64 << 10;
/// The class of a requirement of `need` bytes in blocks of `block`: 0 for none (nothing, or over a block: memory of its own)
inline uint64_t class_bytes(uint64_t need, uint64_t block) {
    if (!need || need > block) return 0;
    const auto up = [](uint64_t x) { return (x + kClassAlign - 1) / kClassAlign * kClassAlign; };
    uint64_t c = kClassFloor;
    while (c < need) c = up(c * 5 / 4);
    const uint64_t per_block = block / up(need);
    if (per_block < 8) c = std::min(c, block / per_block / kClassAlign * kClassAlign);
    return std::min(c, block);
}
/// The pool's index of class allocations: by memory type and class
inline uint64_t class_key(uint32_t memory_type, uint64_t class_bytes) { return class_bytes << 6 | (memory_type & 63); }

inline Fit verdict(uint64_t pooled_bytes, uint64_t pooled_offset, uint32_t pooled_memory_type, bool pooled_dedicated,
                   uint64_t need_bytes, uint64_t need_alignment, uint32_t need_memory_type_bits) {
    if (pooled_dedicated) return Fit::own_memory;
    if (pooled_memory_type >= 32 || !(need_memory_type_bits >> pooled_memory_type & 1)) return Fit::type;
    if (need_alignment > 1 && pooled_offset % need_alignment) return Fit::alignment;
    if (!need_bytes || pooled_bytes < need_bytes) return Fit::too_small;
    return pooled_bytes * 2 <= need_bytes * 3 || pooled_bytes <= class_bytes(need_bytes, UINT64_MAX) ? Fit::fits : Fit::too_big;
}

}  // namespace mkwvk::target_pool
