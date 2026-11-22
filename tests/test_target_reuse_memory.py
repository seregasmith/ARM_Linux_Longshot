"""A render target whose create info misses the pool takes the memory of a pooled one it fits in, instead of a new allocation.

OoT gives each character near the camera a shadow depth map whose side it computes anew every frame, so its pool key never comes
back and every one was a new image while one of nearly the same size had just gone into the pool. On a Steam Deck OLED the
replay spent about 3.2 ms a frame in VMA's block allocations (a kernel call that failed compaction and swapped), 92% of it these
targets. Now, when the exact pool_take misses, create binds the new image to a pooled image's allocation that holds it
(target_pool.h's fit rule: the memory type, the alignment, the size and at most 1.5 times it, only a share of the targets'
blocks), with vmaCreateAliasingImage, and the pooled image and its views are destroyed. Only an image that has waited in the
pool a present or more is taken so (target_pool.h's takeable_by_size): one of a shape that comes back every frame stays for it.
RX_VK_TARGET_REUSE_MEMORY=0 turns it off.

On an integrated GPU, while the take by size is on, the pool holds its images 64 presents instead of 8 (idle_presents).

RX_VK_TARGET_REUSE_MEMORY=2, the default on an integrated GPU, also makes each new target of the pooled kind on an allocation of its size class (class_bytes), so a
shape a few percent larger fits it; the pool indexes such allocations by memory type and class.

Compiles target_pool.h's rules with a small fixture, and vk_texture.cpp's pool_take_memory and pool_take, cut out by their text,
with stubs for Vulkan and VMA: what is taken and what is left, what is destroyed, the counts, and that the memory comes from the
pool alone, never from the dead queue (whose images the GPU may still use: they reach the pool only once its work completed).
The rest is read from create and the VMA wrapper.

python -I -B tests/test_target_reuse_memory.py --headers <Vulkan-Headers> [--cxx clang++]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]
CXX = "clang++"
HEADERS = None

RULES = r'''#include <cstdio>
#include "target_pool.h"
using mkwvk::target_pool::fits;
using mkwvk::target_pool::takeable_by_size;
static int fails;
#define CHECK(what, cond) do { if (!(cond)) { std::printf("FAIL: %s\n", what); fails++; } } while (0)
int main() {
    const unsigned long long MB = 1ull << 20;
    // fits(pooled bytes, pooled offset, pooled memory type, pooled dedicated, need bytes, need alignment, need memory type bits)
    CHECK("the same size fits", fits(4 * MB, 0, 2, false, 4 * MB, 65536, 1u << 2));
    CHECK("a larger allocation up to 1.5 times fits", fits(6 * MB, 0, 2, false, 4 * MB, 65536, 1u << 2));
    CHECK("over 1.5 times does not", !fits(6 * MB + 1, 0, 2, false, 4 * MB, 65536, 1u << 2));
    CHECK("a smaller allocation does not", !fits(4 * MB - 1, 0, 2, false, 4 * MB, 65536, 1u << 2));
    CHECK("a memory type the image cannot use does not", !fits(4 * MB, 0, 2, false, 4 * MB, 65536, (1u << 1) | (1u << 3)));
    CHECK("a memory type among several it can use fits", fits(4 * MB, 0, 3, false, 4 * MB, 65536, (1u << 1) | (1u << 3)));
    CHECK("an offset the alignment does not divide does not", !fits(4 * MB, 4096, 2, false, 4 * MB, 65536, 1u << 2));
    CHECK("an offset the alignment divides fits", fits(4 * MB, 3 * 65536, 2, false, 4 * MB, 65536, 1u << 2));
    CHECK("dedicated memory does not (bound to its own image)", !fits(4 * MB, 0, 2, true, 4 * MB, 65536, 1u << 2));
    CHECK("nothing needed does not", !fits(4 * MB, 0, 2, false, 0, 65536, 1u << 2));
    CHECK("a memory type past 31 does not", !fits(4 * MB, 0, 40, false, 4 * MB, 65536, ~0u));
    CHECK("alignment 0 counts as 1", fits(4 * MB, 12, 2, false, 4 * MB, 0, 1u << 2));
    // (OoT's shadow maps: a 397 x 397 D32 one, ~616 KB, into the memory a 439 x 439 one left, ~753 KB)
    CHECK("the next frame's shadow map fits the last one's", fits(439ull * 439 * 4, 0, 0, false, 397ull * 397 * 4, 1024, 1));
    CHECK("but not one half its size", !fits(439ull * 439 * 4, 0, 0, false, 300ull * 300 * 4, 1024, 1));
    // takeable_by_size(now, since, min idle presents): put in at `since`, taken by size from `since + min` on
    CHECK("not in the present it came in", !takeable_by_size(100, 100, 1));
    CHECK("from the next present on", takeable_by_size(101, 100, 1) && takeable_by_size(108, 100, 1));
    CHECK("the default waits a present", mkwvk::target_pool::kSizeTakeIdle >= 1);
    // why a pooled allocation does not take a need (verdict), and why a miss found none (miss_reason over a Tally)
    using mkwvk::target_pool::Fit;
    using mkwvk::target_pool::Miss;
    using mkwvk::target_pool::verdict;
    CHECK("verdict: fits", verdict(4 * MB, 0, 2, false, 4 * MB, 65536, 1u << 2) == Fit::fits);
    CHECK("verdict: memory of its own", verdict(4 * MB, 0, 2, true, 4 * MB, 65536, 1u << 2) == Fit::own_memory);
    CHECK("verdict: memory type", verdict(4 * MB, 0, 2, false, 4 * MB, 65536, 1u << 3) == Fit::type);
    CHECK("verdict: alignment", verdict(4 * MB, 4096, 2, false, 4 * MB, 65536, 1u << 2) == Fit::alignment);
    CHECK("verdict: too small", verdict(4 * MB - 1, 0, 2, false, 4 * MB, 65536, 1u << 2) == Fit::too_small);
    CHECK("verdict: too big", verdict(6 * MB + 1, 0, 2, false, 4 * MB, 65536, 1u << 2) == Fit::too_big);
    {
        mkwvk::target_pool::Tally t;
        add(t, false, Fit::fits, 4 * MB);
        CHECK("only images that came in this present: too new", miss_reason(t) == Miss::too_new);
        add(t, true, Fit::type, 4 * MB);
        CHECK("and ones of the wrong kind: unusable", miss_reason(t) == Miss::unusable);
        add(t, true, Fit::too_small, 3 * MB);
        add(t, true, Fit::too_small, 3 * MB + 512 * 1024);
        CHECK("and smaller ones: too small", miss_reason(t) == Miss::too_small && t.largest_small == 3 * MB + 512 * 1024);
        add(t, true, Fit::too_big, 9 * MB);
        CHECK("smaller and bigger: between", miss_reason(t) == Miss::between);
        mkwvk::target_pool::Tally big;
        add(big, true, Fit::too_big, 9 * MB);
        CHECK("only bigger: too big", miss_reason(big) == Miss::too_big);
    }
    // size classes (class_bytes): a pooled-kind target's allocation is its requirement rounded up to a class, so a shape a few
    // percent larger takes it
    {
        using mkwvk::target_pool::class_bytes;
        const unsigned long long block = 16 * MB;
        CHECK("a tiny image: the floor class", class_bytes(4096, block) == 64 * 1024 && class_bytes(64 * 1024, block) == 64 * 1024);
        CHECK("over a block: no class", class_bytes(block + 1, block) == 0 && class_bytes(0, block) == 0);
        CHECK("the top class is the block", class_bytes(15 * MB, block) == block);
        const unsigned long long K64 = 64 * 1024;
        auto r64 = [&](unsigned long long x) { return (x + K64 - 1) / K64 * K64; };
        bool ok = true, packs = true, fixed = true;
        for (unsigned long long need = 1; need <= block; need += need / 97 + 4093) {
            const unsigned long long c = class_bytes(need, block);
            // (a multiple of the 64 KiB alignment, so the class is all it takes in its block: at most a quarter over the need
            // rounded to 64 KiB)
            ok = ok && c >= need && c <= block && c % K64 == 0 && c <= r64(need * 5 / 4) &&
                 mkwvk::target_pool::fits(c, 0, 0, false, need, 1, 1);
            // (as many to a block as without classes, where that is fewer than 8)
            if (block / r64(need) < 8) packs = packs && block / c == block / r64(need);
            fixed = fixed && class_bytes(c, block) == c;
        }
        CHECK("every class holds its need, 64 KiB multiples, within a quarter, and fits", ok);
        CHECK("as many to a block as without classes, up to 7", packs);
        CHECK("a class is its own class (pool_put tells class allocations so)", fixed);
        CHECK("a 1280 x 800 RGBA16F target: still two to a 16 MB block", block / class_bytes(1280ull * 800 * 8, block) == 2);
        CHECK("a shadow map a little larger is in the same class", class_bytes(401ull * 401 * 4, block) == class_bytes(410ull * 410 * 4, block));
        CHECK("a tiny need takes the floor class", mkwvk::target_pool::fits(64 * 1024, 0, 0, false, 4096, 1, 1));
        CHECK("the class key tells memory types apart",
              mkwvk::target_pool::class_key(2, 64 * 1024) != mkwvk::target_pool::class_key(3, 64 * 1024));
        CHECK("RX_VK_TARGET_REUSE_MEMORY=2 asks for them, more is 2", mkwvk::target_pool::reuse_level("2", false) == 2 &&
              mkwvk::target_pool::reuse_level("7", false) == 2);
        CHECK("unset: the classes on an integrated GPU, the take alone on a discrete one",
              mkwvk::target_pool::reuse_level(nullptr, true) == 2 && mkwvk::target_pool::reuse_level(nullptr, false) == 1);
    }
    // RX_VK_TARGET_REUSE_MEMORY: 0 off, 1 the take by size (unset)
    {
        using mkwvk::target_pool::reuse_level;
        CHECK("unset: the take by size on a discrete GPU", reuse_level(nullptr, false) == 1 && reuse_level("", false) == 1);
        CHECK("0 off, 1 on", reuse_level("0", true) == 0 && reuse_level("-3", false) == 0 && reuse_level("1", true) == 1);
    }
    // the pool's hold: 64 presents while its images' memory can be taken by size on an integrated GPU (a Steam Deck's turn: 8 let
    // 2,255-2,946 images go idle and 1,006-1,104 misses find the pool empty; 64 left 22-29), 8 with the take off (holding alone
    // measured worse) and on a discrete GPU (where the pool would fill to its cap for a gain not measured there); RX_VK_POOL_IDLE
    // as set either way
    {
        using mkwvk::target_pool::idle_presents;
        CHECK("taking by size on an integrated GPU: 64 presents", idle_presents(nullptr, 1, true) == 64 &&
              idle_presents(nullptr, 2, true) == 64 && mkwvk::target_pool::kIdlePresentsReuse == 64);
        CHECK("on a discrete GPU: 8, as before", idle_presents(nullptr, 1, false) == 8 && idle_presents(nullptr, 2, false) == 8);
        CHECK("the take off: 8, as before", idle_presents(nullptr, 0, true) == 8 && mkwvk::target_pool::kIdlePresents == 8);
        CHECK("RX_VK_POOL_IDLE as set", idle_presents("120", 0, false) == 120 && idle_presents("16", 2, true) == 16);
        CHECK("at least 1", idle_presents("0", 2, true) == 1 && idle_presents("-5", 0, false) == 1);
    }
    if (!fails) std::puts("PASS: a pooled render target's memory takes a new image of the same memory type, aligned, up to 1.5 times "
                          "its size, never dedicated memory, once it has waited a present");
    return fails ? 1 : 0;
}
'''

# (pool_take_memory and pool_take as vk_texture.cpp has them, with what they use stubbed: {pooled}, {take}, {take_memory})
TAKE = r'''#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <list>
#include <atomic>
#include <cstdlib>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "target_pool.h"
struct VmaAllocation_T { int id; };
typedef VmaAllocation_T* VmaAllocation;
namespace mkwvk {
namespace {
struct {
    VkDevice device = (VkDevice)0x1;
    VkPhysicalDeviceProperties props{.deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU};   // (pool_trim's hold: 64 presents)
} dev;
uint64_t frame_no = 100;
// (the dead queue: what the GPU may still use. pool_take_memory must not read it; any use of it does not compile)
struct OffLimits {} dead;
bool reuse_on = true, requirements_ok = true, alias_ok = true;
int requirement_asks, aliases;
VkMemoryRequirements need{};
std::vector<uint64_t> destroyed_images, destroyed_views;
VmaAllocation aliased_on = nullptr;
bool reuse_memory_on() { return reuse_on; }
int reuse_memory_level() { return 2; }
bool image_requirements(const VkImageCreateInfo&, VkMemoryRequirements* mr) { requirement_asks++; *mr = need; return requirements_ok; }
@IMAGE_NEED@;
VkImageCreateInfo the_ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
uint32_t device_local_types() { return 1u << 2; }
bool create_aliasing_image(const VkImageCreateInfo&, VmaAllocation allocation, VkImage& image) {
    if (!alias_ok) return false;
    aliases++, aliased_on = allocation, image = (VkImage)(uintptr_t)0x900;
    return true;
}
void vkDestroyImageView(VkDevice, VkImageView v, const void*) { destroyed_views.push_back((uint64_t)(uintptr_t)v); }
void vkDestroyImage(VkDevice, VkImage i, const void*) { destroyed_images.push_back((uint64_t)(uintptr_t)i); }
@POOLED@;
@DEAD@;
uint8_t pool_pressure;
uint64_t cap_bytes = 1ull << 30, st_pool_destroyed, st_pool_trim_cap, st_pool_trim_idle;
uint64_t pool_cap() { return cap_bytes; }
// (the reaper's thread destroys them later, vk_vma.cpp: counted here as destroyed when handed to it)
void destroy_image_later(VkImage i, VmaAllocation, std::vector<VkImageView> views) {
    destroyed_images.push_back((uint64_t)(uintptr_t)i);
    for (const VkImageView v : views) destroyed_views.push_back((uint64_t)(uintptr_t)v);
}
struct FitInfo { uint64_t bytes, offset; uint32_t type; bool dedicated; };
std::map<VmaAllocation, FitInfo> fit_info;
bool allocation_fit_info(VmaAllocation a, uint64_t* bytes, uint64_t* offset, uint32_t* type, bool* dedicated) {
    const FitInfo f = fit_info.at(a);
    *bytes = f.bytes, *offset = f.offset, *type = f.type, *dedicated = f.dedicated;
    return true;
}
std::mutex pool_mu;
std::list<Pooled> pool_by_age;
std::unordered_map<uint64_t, std::deque<std::list<Pooled>::iterator>> target_pool, by_class;
std::unordered_map<uint64_t, uint64_t> taken_by_size;
uint64_t pool_bytes, pool_n, st_pool_reused, st_pool_resized, st_pool_stolen, st_pool_put, st_pool_idle_out;
uint64_t st_miss[(int)target_pool::Miss::count], st_small_gap_pct;
std::atomic<uint64_t> dead_pooled_n{0};
uint64_t target_block_bytes() { return 16ull << 20; }
@UNINDEX@
@DESTROY@
@PUT@
@TRIM@
@TAKE@
@TAKE_MEMORY@
bool take_memory(VkImage& image, VmaAllocation& allocation) {   // (a new ImageNeed each, as create makes one a texture)
    ImageNeed n{&the_ci};
    return pool_take_memory(n, image, allocation);
}
int fails;
#define CHECK(what, cond) do { if (!(cond)) { std::printf("FAIL: %s\n", what); fails++; } } while (0)
VmaAllocation_T allocs[8];
void put(int i, uint64_t key, uint64_t bytes, uint64_t since, uint32_t type, bool own_memory) {
    Pooled p{(VkImage)(uintptr_t)(0x100 + i), &allocs[i], {{1, (VkImageView)(uintptr_t)(0x200 + i)}}, bytes, since, key};
    p.alloc_bytes = bytes, p.alloc_offset = 0, p.memory_type = type, p.dedicated = own_memory;
    pool_by_age.push_back(std::move(p));
    target_pool[key].push_back(std::prev(pool_by_age.end()));
    pool_bytes += bytes, pool_n++;
}
int run() {
    const uint64_t MB = 1ull << 20;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    // (an empty pool: nothing asked of the driver)
    CHECK("an empty pool takes nothing", !take_memory(image, allocation));
    CHECK("an empty pool asks the driver nothing", requirement_asks == 0);
    CHECK("counted as empty", st_miss[(int)target_pool::Miss::empty] == 1);
    dead_pooled_n = 2;
    CHECK("an empty pool with targets still in the dead queue", !take_memory(image, allocation) &&
          st_miss[(int)target_pool::Miss::empty_dead_waiting] == 1 && st_miss[(int)target_pool::Miss::empty] == 1);
    dead_pooled_n = 0;
    need.size = 4 * MB, need.alignment = 65536, need.memoryTypeBits = (1u << 2) | (1u << 3);
    put(0, 11, 5 * MB, 90, 2, false);         // (fits: old enough, in the targets' blocks)
    put(1, 12, 4 * MB + 4096, 95, 2, false);  // (fits, smaller: the one taken)
    put(2, 13, 4 * MB, 99, 2, true);          // (memory of its own: never)
    put(3, 14, 4 * MB, 99, 3, false);         // (a memory type not in VRAM: never)
    put(4, 15, 4 * MB, 100, 2, false);        // (came in this present: kept for its own shape)
    put(5, 16, 7 * MB, 90, 2, false);         // (over 1.5 times)
    const uint64_t before = pool_bytes;
    reuse_on = false;
    CHECK("RX_VK_TARGET_REUSE_MEMORY=0 takes nothing", !take_memory(image, allocation) && pool_n == 6);
    reuse_on = true, alias_ok = false;
    CHECK("an image not bound leaves the pool as it was", !take_memory(image, allocation) && pool_n == 6 &&
          destroyed_images.empty() && destroyed_views.empty());
    alias_ok = true;
    CHECK("a fit is taken", take_memory(image, allocation));
    CHECK("the smallest old enough fit, in VRAM and the targets' blocks", allocation == &allocs[1] && aliased_on == &allocs[1]);
    CHECK("the new image is the one bound", image == (VkImage)(uintptr_t)0x900);
    CHECK("its image and views destroyed, nothing else", destroyed_images == std::vector<uint64_t>{0x101} &&
          destroyed_views == std::vector<uint64_t>{0x201});
    CHECK("out of the pool and its counts", pool_n == 5 && pool_bytes == before - (4 * MB + 4096) && !target_pool.count(12) &&
          st_pool_resized == 1);
    CHECK("the rest stays", target_pool.count(11) && target_pool.count(13) && target_pool.count(14) && target_pool.count(15) &&
          target_pool.count(16));
    // (its shape asked for again: a miss, counted once as taken by size)
    std::unordered_map<uint64_t, VkImageView> views;
    CHECK("its shape misses after", !pool_take(12, image, allocation, views));
    CHECK("the miss is counted as one taken by size", st_pool_stolen == 1);
    CHECK("and only once", !pool_take(12, image, allocation, views) && st_pool_stolen == 1);
    CHECK("an exact take is not such a miss", pool_take(15, image, allocation, views) && st_pool_stolen == 1 &&
          allocation == &allocs[4] && st_pool_reused == 1);
    // (the driver says the image must have memory of its own: nothing taken)
    requirements_ok = false;
    CHECK("an image that must have memory of its own takes nothing", !take_memory(image, allocation) && pool_n == 4);
    CHECK("counted as refused", st_miss[(int)target_pool::Miss::requirements] == 1);
    requirements_ok = true;
    CHECK("the one not bound was counted", st_miss[(int)target_pool::Miss::not_bound] == 1);
    // (left: 5 MB and 7 MB in the targets' blocks, waited; one of its own; one of another memory type)
    need.size = 8 * MB;
    CHECK("all smaller", !take_memory(image, allocation) && st_miss[(int)target_pool::Miss::too_small] == 1 &&
          st_small_gap_pct == 12);   // (7 MB for 8: 12.5% short)
    need.size = 2 * MB;
    CHECK("all bigger", !take_memory(image, allocation) && st_miss[(int)target_pool::Miss::too_big] == 1);
    need.size = 20 * MB;
    CHECK("over a block: memory of its own anyway", !take_memory(image, allocation) &&
          st_miss[(int)target_pool::Miss::over_block] == 1);
    reuse_on = false;
    uint64_t all = 0;
    for (uint64_t n : st_miss) all += n;
    CHECK("off: nothing counted", !take_memory(image, allocation) && all == 7);
    reuse_on = true;
    // (one ImageNeed asks the driver once, however often it is read)
    {
        const int asks = requirement_asks;
        ImageNeed n{&the_ci};
        n.get(), n.get();
        CHECK("an ImageNeed asks once", requirement_asks == asks + 1);
    }
    // (from here through pool_put, as the dead targets come in)
    pool_by_age.clear(), target_pool.clear(), pool_bytes = pool_n = 0;
    auto dead_one = [&](int i, uint64_t key, uint64_t bytes) {
        fit_info[&allocs[i]] = {bytes, 0, 2, false};
        Dead d{};
        d.image = (VkImage)(uintptr_t)(0x300 + i), d.allocation = &allocs[i], d.pool_key = key, d.bytes = bytes;
        pool_put(d);
    };
    std::unordered_map<uint64_t, VkImageView> v2;
    need.size = 4 * MB, need.alignment = 4096;
    // (the record of a key taken by size goes when an image of its shape comes back, or its shape is taken exactly: a later miss
    // of it is then the idle rule's, not one a take by size caused)
    {
        frame_no = 400;
        dead_one(6, 81, 4 * MB);
        frame_no = 401;
        CHECK("a take by size records its key", take_memory(image, allocation) && taken_by_size.count(81));
        dead_one(6, 81, 4 * MB);
        CHECK("an image of that shape back: forgotten", !taken_by_size.count(81));
        frame_no = 1400;
        pool_trim();
        const uint64_t stolen = st_pool_stolen;
        CHECK("its idle miss is not counted as taken by size", !pool_take(81, image, allocation, v2) && st_pool_stolen == stolen);
        frame_no = 1500;
        dead_one(6, 82, 4 * MB), dead_one(7, 82, 4 * MB);
        frame_no = 1501;
        CHECK("two of a shape, one taken by size", take_memory(image, allocation) && taken_by_size.count(82));
        CHECK("the other taken exactly: forgotten", pool_take(82, image, allocation, v2) && !taken_by_size.count(82));
        CHECK("a later miss not counted", !pool_take(82, image, allocation, v2) && st_pool_stolen == stolen && pool_n == 0);
    }
    // (the hold: 64 presents on an integrated GPU with the take by size)
    {
        frame_no = 3000;
        dead_one(6, 91, 4 * MB);
        frame_no = 3000 + target_pool::kIdlePresentsReuse;
        pool_trim();
        CHECK("64 presents in the pool: kept", pool_n == 1);
        frame_no++;
        pool_trim();
        CHECK("65: let go", pool_n == 0);
    }
    // (size classes: through pool_put, the index by memory type and class, pool_trim and pool_take)
    {
        by_class.clear();
        const uint64_t cls = target_pool::class_bytes(401ull * 401 * 4, target_block_bytes());
        frame_no = 5200;
        dead_one(6, 61, cls);       // (a shadow map's, in its class)
        dead_one(7, 62, 5 * MB);    // (not a class: found by the scan only)
        CHECK("pool_put indexes a class allocation, and only that", by_class.size() == 1 && by_class.begin()->second.size() == 1);
        frame_no = 5201;
        need.size = 410ull * 410 * 4, need.alignment = 4096;
        CHECK("a map a little larger takes its class's", take_memory(image, allocation) && allocation == &allocs[6]);
        CHECK("and it leaves the index", by_class.empty());
        dead_one(6, 63, cls);
        frame_no = 5201 + target_pool::kIdlePresentsReuse + 1;
        pool_trim();
        CHECK("the trim lets go of it and its index entry", by_class.empty() && pool_n == 0);
        dead_one(6, 64, cls);
        CHECK("an exact take", pool_take(64, image, allocation, v2) && allocation == &allocs[6]);
        CHECK("leaves the index too", by_class.empty());
        // (the index: its front not a present old yet is not taken, nor by the scan)
        frame_no = 5400;
        dead_one(6, 71, cls);
        pool_by_age.back().alloc_offset = 64 * 1024;   // (64 KiB into its block)
        const uint64_t too_new = st_miss[(int)target_pool::Miss::too_new];
        CHECK("a class front that came in this present stays", !take_memory(image, allocation) && by_class.size() == 1 &&
              st_miss[(int)target_pool::Miss::too_new] == too_new + 1);
        // (a front its alignment does not take: the scan says why)
        frame_no = 5401;
        need.alignment = 1u << 20;
        const uint64_t unusable = st_miss[(int)target_pool::Miss::unusable];
        CHECK("a class front of the wrong alignment stays", !take_memory(image, allocation) && by_class.size() == 1 &&
              st_miss[(int)target_pool::Miss::unusable] == unusable + 1);
        need.alignment = 4096;
        CHECK("then a fitting need takes it", take_memory(image, allocation) && allocation == &allocs[6]);
        CHECK("out of its key's bucket and the index", !target_pool.count(71) && by_class.empty() && pool_n == 0);
    }
    if (!fails) std::puts("PASS: pool_take_memory takes the smallest fit that waited a present, from the pool alone, and counts "
                          "the misses it causes");
    return fails ? 1 : 0;
}
}  // namespace
}  // namespace mkwvk
int main() { return mkwvk::run(); }
'''


def vulkan_headers():
    """Vulkan-Headers where the build would look: LONGSHOT_VULKAN_HEADERS, or the CPM cache in RX_CPM"""
    if os.environ.get("LONGSHOT_VULKAN_HEADERS"):
        return os.environ["LONGSHOT_VULKAN_HEADERS"]
    cpm = os.environ.get("RX_CPM")
    found = sorted(Path(cpm).glob("vulkanheaders/*/include/vulkan/vulkan.h")) if cpm else []
    return str(found[-1].parents[1]) if found else None


def compile_and_run(test, source, extra=()):
    cxx = shutil.which(CXX)
    if not cxx:
        test.skipTest("no C++ compiler %r (pass --cxx)" % CXX)
    with tempfile.TemporaryDirectory(prefix="target-reuse-memory-") as temp:
        d = Path(temp)
        src, exe = d / "fixture.cpp", d / "fixture.exe"
        src.write_text(source, encoding="utf-8")
        built = subprocess.run([cxx, "-std=c++20", "-O1", "-DVK_NO_PROTOTYPES", "-I", str(ROOT / "gpu" / "vk"), *extra,
                                str(src), "-o", str(exe)], capture_output=True, text=True)
        test.assertEqual(built.returncode, 0, built.stderr)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        print(result.stdout, end="")
        test.assertEqual(result.returncode, 0, result.stdout)


class RuleTests(unittest.TestCase):
    def test_the_fit_and_wait_rules(self):
        compile_and_run(self, RULES)


class TakeTests(unittest.TestCase):
    def test_pool_take_memory_takes_from_the_pool_alone(self):
        headers = HEADERS or vulkan_headers()
        self.assertTrue(headers, "no Vulkan headers: pass --headers, or set LONGSHOT_VULKAN_HEADERS or RX_CPM (run_all passes them)")
        inc = Path(headers) / "include" if (Path(headers) / "include").is_dir() else Path(headers)
        tex = (ROOT / "gpu" / "vk" / "vk_texture.cpp").read_text(encoding="utf-8")
        internal = (ROOT / "gpu" / "vk" / "vk_internal.h").read_text(encoding="utf-8")
        source = (TAKE.replace("@POOLED@", block(tex, "struct Pooled {"))
                  .replace("@DEAD@", block(tex, "struct Dead {"))
                  .replace("@UNINDEX@", block(tex, "void pool_unindex(std::list<Pooled>::iterator it)"))
                  .replace("@DESTROY@", block(tex, "void pool_destroy(Pooled& p)"))
                  .replace("@PUT@", block(tex, "void pool_put(Dead& d)"))
                  .replace("@TRIM@", block(tex, "size_t pool_trim()"))
                  .replace("@TAKE@", block(tex, "bool pool_take(uint64_t key, VkImage& image"))
                  .replace("@TAKE_MEMORY@", block(tex, "bool pool_take_memory(ImageNeed& need"))
                  .replace("@IMAGE_NEED@", block(internal, "struct ImageNeed {")))
        compile_and_run(self, source, ["-I", str(inc)])


def switches(test):
    return (ROOT / "gpu" / "vk" / "switches.def").read_text(encoding="utf-8")


def docs_row(test):
    """docs/dev/reference/switches.md's row of RX_VK_TARGET_REUSE_MEMORY"""
    dev = (ROOT / "docs" / "dev" / "reference" / "switches.md").read_text(encoding="utf-8")
    row = dev[dev.index("`RX_VK_TARGET_REUSE_MEMORY=0/1/2`"):]
    return row[:row.index("\n")]


class ReuseMemoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tex = (ROOT / "gpu" / "vk" / "vk_texture.cpp").read_text(encoding="utf-8")
        cls.vma = (ROOT / "gpu" / "vk" / "vk_vma.cpp").read_text(encoding="utf-8")

    def test_create_tries_the_memory_only_after_the_exact_pool_take_missed(self):
        make = block(self.tex, "Texture* create(const Key& key")
        self.assertEqual(make.count("pool_take_memory("), 1)
        self.assertIn("pool_key && (pool_take(pool_key, x.image, x.allocation, x.views) || "
                      "pool_take_memory(need, x.image, x.allocation))", make)
        # (the driver asked for the image's requirements once a texture: the take by size reads what create_image's check of the
        # targets' pool's memory type asks for, and the other way round: test_target_image_pool)
        self.assertLess(make.index("ImageNeed need{&ci};"), make.index("pool_take_memory(need, x.image, x.allocation)"))
        # (and only for a pooled kind of image, never a sparse one: pool_key is 0 for it)
        rule = make[make.index("const uint64_t pool_key ="):make.index("x.pool_key = pool_key;")]
        self.assertIn("!x.sparse", rule)
        # (a reused memory is not counted as made new; the image is still put into GENERAL from UNDEFINED and cleared)
        taken = make.index("pool_take_memory(need, x.image, x.allocation)")
        self.assertLess(taken, make.index("st_pool_made++"))
        self.assertIn("st_pool_resized == resized_before", make)
        after = make[taken:]
        self.assertIn("b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;", after)
        self.assertIn("vkCmdClearColorImage", after)
        self.assertIn("vkCmdClearDepthStencilImage", after)
        # (the budget counts the allocation the image sits on, not only its texels)
        self.assertIn("x.pad = std::max(x.bytes, allocation_size(x.allocation)) - x.bytes;", after)

    def test_the_pool_reads_its_allocations_when_they_come_in_and_forgets_old_takes(self):
        self.assertIn('target_pool::reuse_level(getenv("RX_VK_TARGET_REUSE_MEMORY"), '
                      'dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)', self.tex)
        put = block(self.tex, "void pool_put(Dead& d)")
        self.assertIn("allocation_fit_info(d.allocation, &fit.bytes, &fit.offset, &fit.memory_type, &fit.dedicated)", put)
        trim = block(self.tex, "size_t pool_trim()")
        self.assertIn("taken_by_size", trim)   # (the record of what was taken by size goes with the pool's idle rule)

    def test_the_wrapper_needs_no_new_vma_and_refuses_memory_of_its_own(self):
        alias = block(self.vma, "bool create_aliasing_image(const VkImageCreateInfo& ci, VmaAllocation allocation, VkImage& image)")
        self.assertIn("vmaCreateAliasingImage(vma, allocation, &ci, &image)", alias)
        info = block(self.vma, "bool allocation_fit_info(VmaAllocation allocation")
        # (only an allocation create_image made in the targets' blocks, which VMA never makes dedicated (an explicit block
        # size), and told by the tag it gave it, not by vmaGetAllocationInfo2: that is VMA 3.1, and a distribution's VMA may
        # be 3.0. The reaper's allocation_dedicated calls it since #1914, so the build takes no VMA without it)
        self.assertNotIn("vmaGetAllocationInfo2", info)
        if "vmaGetAllocationInfo2" in self.vma:
            build = (ROOT / "nx2" / "build.py").read_text(encoding="utf-8")
            self.assertIn('HEADER_NEEDS = {"vulkanmemoryallocator": "vmaGetAllocationInfo2"}', build)
        self.assertIn("vmaGetAllocationInfo(vma, allocation, &info)", info)
        self.assertIn("info.pUserData != &target_blocks_tag", info)
        image = block(self.vma, "bool create_image(const VkImageCreateInfo& ci")
        self.assertIn("if (pool == target_image_pool) pai.pUserData = &target_blocks_tag;", image)
        self.assertIn("static const uint64_t", block(self.vma, "uint64_t target_block_bytes()"))   # (read once)

    def test_the_counts_are_logged_and_the_switch_is_documented(self):
        self.assertIn("render target images: %llu reused from the pool, %llu made new, %llu destroyed from it, "
                      "%llu reused by size", self.tex)
        self.assertIn("%llu missed after their image was taken by size", self.tex)
        # (why the others missed, and why the pool let its images go)
        for part in ("vk: render targets not reused by size: %llu with the pool empty (%llu of them with targets still in the ",
                     "dead queue), %llu with none in it a present yet, %llu with none of a kind they can use, %llu with all smaller ",
                     "(the largest %llu%% short on average), %llu with all bigger (over 1.5 times), %llu with some of each, %llu over ",
                     "a block, %llu refused by the driver, %llu not bound; the pool let go of %llu over its bound and %llu idle"):
            self.assertIn(part, self.tex)
        self.assertIn("SW(RX_VK_TARGET_REUSE_MEMORY, hw, int,", switches(self))
        for part in ("`RX_VK_TARGET_REUSE_MEMORY=1`, the default",
                     "what stays on is the check that the targets' pool's memory type suits an image"):
            self.assertIn(part, docs_row(self))

    def test_the_pool_holds_its_images_64_presents_on_an_integrated_gpu_while_the_take_is_on(self):
        trim = block(self.tex, "size_t pool_trim()")
        self.assertIn('target_pool::idle_presents(getenv("RX_VK_POOL_IDLE"), reuse_memory_level(),', trim)
        self.assertIn("dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU", trim)
        self.assertIn("SW(RX_VK_POOL_IDLE, hw, int, \"64 presents on an integrated GPU while render targets' memory is taken by size",
                      switches(self))
        for part in ("64 presents", "integrated GPU"):
            self.assertIn(part, docs_row(self))

    def test_size_classes_are_made_by_create_when_asked_for(self):
        make = block(self.tex, "Texture* create(const Key& key")
        # (a new one of the pooled kind is made on an allocation of its size class at RX_VK_TARGET_REUSE_MEMORY=2 while the pool is
        # not pressed at all (halved under the Linux hold or over 90% of what the driver allows); otherwise, or refused, as before)
        classed = make.index("pool_key && reuse_memory_level() >= 2 && pool_pressure == 0 &&")
        self.assertEqual(make.count("create_image_in_class("), 1)
        self.assertLess(classed, make.index("create_image_in_class(need, x.image, x.allocation)"))
        self.assertLess(classed, make.index("st_pool_made++"))
        self.assertLess(classed, make.index("!create_image(ci, x.image, x.allocation, false, as_target"))
        cls = block(self.vma, "bool create_image_in_class(ImageNeed& need, VkImage& image, VmaAllocation& allocation)")
        for part in ("target_images_pool()", "const VkMemoryRequirements* mr = need.get();",
                     "!(mr->memoryTypeBits >> target_image_pool_type & 1)", "target_pool::class_bytes(mr->size, target_block_bytes())",
                     "ai.pool = pool;", "ai.pUserData = &target_blocks_tag;", "vmaAllocateMemory(vma, &cr, &ai, &allocation, nullptr)",
                     "vmaCreateAliasingImage(vma, allocation, need.ci, &image)", "vmaFreeMemory(vma, allocation)",
                     "image_bytes[allocation_in_vram(allocation) ? 0 : 1].fetch_add(allocation_size(allocation)"):
            self.assertIn(part, cls)
        self.assertIn("SW(RX_VK_TARGET_REUSE_MEMORY, hw, int,", switches(self))
        for part in ("`=2`, the default on an integrated GPU (measured on a Steam Deck OLED, Minimum, Render 50%",
                     "less measured, not shown worse", "or its size class", "`class_bytes`"):
            self.assertIn(part, docs_row(self))


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="clang++", help="C++ compiler")
    p.add_argument("--headers", help="Vulkan-Headers (the package or its include folder)")
    a, rest = p.parse_known_args()
    CXX, HEADERS = a.cxx, a.headers
    unittest.main(argv=[sys.argv[0]] + rest)
