// SPDX-License-Identifier: GPL-3.0-or-later
// Game-free Vulkan compute fixture for gpu/vk/host_shaders/astc_decoder.comp: the real compiled ASTC_DECODER_COMP_SPV, through
// the interface the renderer gives it (the swizzled blocks as a storage buffer, a push-constant block, a storage image view of
// an R8G8B8A8_UNORM image), against the CPU decoder gpu/vk/texdecode/astc.cpp, which equals ARM's astcenc byte for byte
// (tests/texdecode_astc_vectors.vec holds astcenc's answers for 4,694 blocks). Equality is exact: no tolerance.
//   1. every block of the vector file, in its footprint, in the linear and the sRGB decode mode, with a table of how many blocks
//      of each class (footprint, partitions, dual plane, colour endpoint modes, void extent, weight grid against the footprint,
//      weight quantization level) are equal, one step off and further off;
//   2. whole levels of those blocks: layers, edge blocks clipped, GOB blocks of 1, 2 and 4 GOBs;
//   3. the hand-built blocks of astc_hand_blocks.h (every footprint, odd sizes, void extents, illegal blocks, 1 to 4 partitions,
//      two weight planes, a weight grid as big as the footprint, LDR colour modes 0, 4, 6, 8, 10, 12, 13).
// An illegal block, and an HDR void extent, decode to transparent black, the renderer's colour (upload_inner's texdecode call);
// a partition with an HDR endpoint mode to magenta, as texdecode and astcenc do.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#else
#include <dlfcn.h>
#endif

#include "astc_hand_blocks.h"
#include "host_shaders/astc_decoder_comp_spv.h"
#include "texdecode/texdecode.h"

#define CHECK(x)                                                                                                      \
    do {                                                                                                              \
        if (!(x)) {                                                                                                   \
            std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x);                                                       \
            std::exit(1);                                                                                             \
        }                                                                                                             \
    } while (0)

namespace {

using mkwvk::texdecode::Rgba8;

constexpr std::array<std::array<uint32_t, 2>, 14> kFootprints = {{
    {4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12},
}};
/// The renderer's colour for an illegal block (vk_texture.cpp's texdecode call)
constexpr Rgba8 kErrorTransparent{0, 0, 0, 0};

/// The shader's push constants, in the order of its block: what vk_texture.cpp's AstcPush is
struct AstcPush {
    uint32_t block_dims[2];
    uint32_t layer_stride;
    uint32_t block_size;
    uint32_t x_shift;
    uint32_t block_height;
    uint32_t block_height_mask;
    uint32_t native_tiling;
    uint32_t blocks_x;
    uint32_t blocks_y;
    uint32_t srgb;   // the sRGB decode mode (the shader ignores it until it has one)
};

uint64_t fnv(const uint8_t* p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}
uint64_t le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

// ---- the vector file (scripts/texdecode_astc_gen.cpp): "ASTCVEC1", a count, then 36 bytes a block: footprint w, h, a flag byte
// (bit 0 / 1: astcenc called the block illegal in the linear / sRGB mode), a kind, 16 block bytes, two FNV-1a sums (linear, sRGB)
struct Vector {
    uint32_t bw, bh;
    uint8_t flags, kind;
    uint8_t block[16];
    uint64_t sum[2];
};

#ifdef _WIN32
std::vector<Vector> load_vectors(const wchar_t* path) {
    FILE* f = _wfopen(path, L"rb");
#else
std::vector<Vector> load_vectors(const char* path) {
    FILE* f = std::fopen(path, "rb");
#endif
    if (!f) {
#ifdef _WIN32
        std::perror("opening vector file");
#else
        std::perror(path);
#endif
        std::exit(2);
    }
    std::vector<uint8_t> v;
    uint8_t buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) v.insert(v.end(), buf, buf + n);
    std::fclose(f);
    CHECK(v.size() >= 12 && !std::memcmp(v.data(), "ASTCVEC1", 8));
    const uint32_t count = v[8] | v[9] << 8 | v[10] << 16 | (uint32_t)v[11] << 24;
    CHECK(v.size() == 12 + (size_t)count * 36);
    std::vector<Vector> out(count);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* r = &v[12 + (size_t)i * 36];
        out[i].bw = r[0], out[i].bh = r[1], out[i].flags = r[2], out[i].kind = r[3];
        std::memcpy(out[i].block, r + 4, 16);
        out[i].sum[0] = le64(r + 20), out[i].sum[1] = le64(r + 28);
    }
    return out;
}

// ---- what a block is: its class, parsed here from the specification's block layout (not from either decoder), for the table
struct BlockClass {
    uint32_t bw = 0, bh = 0;
    int kind = 0;               // 0 legal, 1 constant-colour void extent, 2 illegal (also an HDR void extent), 3 HDR endpoint mode
    uint32_t parts = 0;
    bool dual = false;
    int cem[4] = {-1, -1, -1, -1};
    uint32_t grid_w = 0, grid_h = 0;
    uint32_t wlevels = 0;       // weight quantization: the number of levels (2 .. 32)
};

uint32_t bits_of(const uint8_t* b, uint32_t pos, uint32_t n) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; i++) v |= (uint32_t)((b[(pos + i) >> 3] >> ((pos + i) & 7)) & 1u) << i;
    return v;
}

/// the bits of n values of a weight range of `levels` levels as an integer sequence
uint32_t ise_bits(uint32_t n, uint32_t levels) {
    static const uint32_t kLv[12] = {2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24, 32};
    static const uint32_t kBits[12] = {1, 0, 2, 0, 1, 3, 1, 2, 4, 2, 3, 5};
    static const uint32_t kKind[12] = {0, 1, 0, 2, 1, 0, 2, 1, 0, 2, 1, 0};   // 1 trit, 2 quint
    for (int i = 0; i < 12; i++) {
        if (kLv[i] != levels) continue;
        return n * kBits[i] + (kKind[i] == 1 ? (8 * n + 4) / 5 : kKind[i] == 2 ? (7 * n + 2) / 3 : 0);
    }
    return 0;
}

BlockClass classify(const uint8_t* b, uint32_t bw, uint32_t bh, bool decoded_ok) {
    BlockClass c;
    c.bw = bw, c.bh = bh;
    const uint32_t mode = bits_of(b, 0, 11);
    if ((mode & 0x1FF) == 0x1FC) {
        c.kind = ((mode & 0x200) || !decoded_ok) ? 2 : 1;
        return c;
    }
    uint32_t base_quant = (mode >> 4) & 1, H = (mode >> 9) & 1, D = (mode >> 10) & 1;
    const uint32_t A = (mode >> 5) & 3;
    uint32_t x = 0, y = 0;
    bool legal = true;
    if (mode & 3) {
        base_quant |= (mode & 3) << 1;
        uint32_t B = (mode >> 7) & 3;
        switch ((mode >> 2) & 3) {
            case 0: x = B + 4, y = A + 2; break;
            case 1: x = B + 8, y = A + 2; break;
            case 2: x = A + 2, y = B + 8; break;
            default:
                B &= 1;
                if (mode & 0x100) x = B + 2, y = A + 2;
                else x = A + 2, y = B + 6;
        }
    } else {
        base_quant |= ((mode >> 2) & 3) << 1;
        if (((mode >> 2) & 3) == 0) legal = false;
        const uint32_t B = (mode >> 9) & 3;
        switch ((mode >> 7) & 3) {
            case 0: x = 12, y = A + 2; break;
            case 1: x = A + 2, y = 12; break;
            case 2: x = A + 6, y = B + 6, D = 0, H = 0; break;
            default:
                if (mode & 0x20) x = 10, y = 6;
                else x = 6, y = 10;
                D = 0, H = 0;
        }
    }
    if (!legal || base_quant < 2) {
        c.kind = 2;
        return c;
    }
    static const uint32_t kLevels[12] = {2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24, 32};
    c.grid_w = x, c.grid_h = y, c.dual = D != 0;
    c.wlevels = kLevels[(base_quant - 2) + 6 * H];
    c.parts = bits_of(b, 11, 2) + 1;
    if (c.parts == 1) {
        c.cem[0] = (int)bits_of(b, 13, 4);
    } else {
        const uint32_t wbits = ise_bits(x * y * (D + 1), c.wlevels);
        const uint32_t cls = bits_of(b, 23, 2);
        if (cls == 0) {
            for (uint32_t i = 0; i < c.parts; i++) c.cem[i] = (int)bits_of(b, 25, 4);
        } else {
            const uint32_t extra_n = c.parts == 2 ? 2 : c.parts == 3 ? 5 : 8;
            const int at = 128 - (int)wbits - (int)extra_n;   // (right under the weights; the plane selector is under these)
            if (at < 29) {
                c.kind = 2;
                return c;
            }
            uint32_t cem = (bits_of(b, (uint32_t)at, extra_n) << 6 | bits_of(b, 23, 6)) >> 2;
            uint32_t flags[4], m[4];
            for (uint32_t i = 0; i < c.parts; i++, cem >>= 1) flags[i] = cem & 1;
            for (uint32_t i = 0; i < c.parts; i++, cem >>= 2) m[i] = cem & 3;
            for (uint32_t i = 0; i < c.parts; i++) c.cem[i] = (int)(((cls - (flags[i] ? 0 : 1)) << 2) | m[i]);
        }
    }
    c.kind = decoded_ok ? 0 : 2;
    for (uint32_t i = 0; i < c.parts; i++) {
        const int e = c.cem[i];
        if (e == 2 || e == 3 || e == 7 || e == 11 || e == 14 || e == 15) c.kind = 3;
    }
    return c;
}

std::string describe(const BlockClass& c) {
    char s[200];
    const char* kinds[4] = {"legal", "void extent", "illegal", "HDR endpoint"};
    if (c.kind == 1 || (c.kind == 2 && !c.parts)) {
        std::snprintf(s, sizeof s, "%ux%u %s", c.bw, c.bh, kinds[c.kind]);
    } else {
        std::snprintf(s, sizeof s, "%ux%u %s, %u part%s, %s plane, cem %d/%d/%d/%d, grid %ux%u (%s), %u weight levels", c.bw, c.bh,
                      kinds[c.kind], c.parts, c.parts == 1 ? "" : "s", c.dual ? "two" : "one", c.cem[0], c.cem[1], c.cem[2], c.cem[3],
                      c.grid_w, c.grid_h, c.grid_w == c.bw && c.grid_h == c.bh ? "= footprint" : "smaller", c.wlevels);
    }
    return s;
}

/// equal / one step off / further off, in blocks and in texels, and the largest step
struct Tally {
    uint64_t blocks = 0, equal = 0, one = 0, more = 0, texels = 0, t_equal = 0, t_one = 0, t_more = 0;
    uint32_t largest = 0;
    void add(int worst, uint32_t texel_count, uint32_t eq, uint32_t one_step, uint32_t far_off, uint32_t largest_step) {
        blocks++;
        (worst == 0 ? equal : worst == 1 ? one : more)++;
        texels += texel_count, t_equal += eq, t_one += one_step, t_more += far_off;
        largest = std::max(largest, largest_step);
    }
};

struct Table {
    std::map<std::string, Tally> rows;
    std::map<std::string, Tally> combos;   // footprint, partitions, plane, colour modes, grid, levels: for finding what fails
    uint64_t failed = 0;
    std::vector<std::string> first_failures;
    uint32_t kind_failures[4] = {};   // (the first few failing blocks of each kind are described)
    Tally total;

    void add(const BlockClass& c, const uint8_t* gpu, const uint8_t* want, uint32_t texels, const char* mode, uint32_t block_index) {
        int worst = 0;
        uint32_t eq = 0, one = 0, far_off = 0, largest = 0;
        uint32_t first_texel = 0, first_diff = 0;
        for (uint32_t t = 0; t < texels; t++) {
            uint32_t d = 0;
            for (int ch = 0; ch < 4; ch++)
                d = std::max(d, (uint32_t)std::abs((int)gpu[4 * t + ch] - (int)want[4 * t + ch]));
            if (d == 0) eq++;
            else if (d == 1) one++;
            else far_off++;
            if (d && !first_diff) first_texel = t, first_diff = d;
            largest = std::max(largest, d);
        }
        worst = far_off ? 2 : one ? 1 : 0;
        char key[160];
        auto put = [&](const char* fmt, auto... args) {
            std::snprintf(key, sizeof key, fmt, args...);
            rows[key].add(worst, texels, eq, one, far_off, largest);
        };
        total.add(worst, texels, eq, one, far_off, largest);
        put("%s footprint %ux%u", mode, c.bw, c.bh);
        put("%s kind %s", mode, c.kind == 0 ? "legal" : c.kind == 1 ? "void extent" : c.kind == 2 ? "illegal" : "HDR endpoint mode");
        if (c.parts && c.kind == 0) {   // (the classes of legal blocks: an illegal one or one with an HDR mode has its own kind)
            put("%s legal, partitions %u", mode, c.parts);
            put("%s legal, weight planes %u", mode, c.dual ? 2u : 1u);
            put("%s legal, weight grid %s", mode, c.grid_w == c.bw && c.grid_h == c.bh ? "= footprint" : "smaller than footprint");
            put("%s legal, weight levels %2u", mode, c.wlevels);
            std::set<int> seen;
            for (uint32_t i = 0; i < c.parts; i++)
                if (seen.insert(c.cem[i]).second) put("%s legal, colour endpoint mode %2d (any partition)", mode, c.cem[i]);
            char combo[200];
            std::snprintf(combo, sizeof combo, "%s %ux%u P%u %s cem %d/%d/%d/%d grid %s levels %u", mode, c.bw, c.bh, c.parts,
                          c.dual ? "dual" : "single", c.cem[0], c.cem[1], c.cem[2], c.cem[3],
                          c.grid_w == c.bw && c.grid_h == c.bh ? "direct" : "scaled", c.wlevels);
            combos[combo].add(worst, texels, eq, one, far_off, largest);
        }
        if (worst == 2) {
            failed++;
            if (kind_failures[c.kind]++ < 3) {
                char s[400];
                std::snprintf(s, sizeof s, "  %s block %u: %s; first texel off is %u: gpu %u,%u,%u,%u, texdecode %u,%u,%u,%u (step %u)", mode,
                              block_index, describe(c).c_str(), first_texel, gpu[4 * first_texel], gpu[4 * first_texel + 1],
                              gpu[4 * first_texel + 2], gpu[4 * first_texel + 3], want[4 * first_texel], want[4 * first_texel + 1],
                              want[4 * first_texel + 2], want[4 * first_texel + 3], first_diff);
                first_failures.push_back(s);
            }
        }
    }

    void print(bool with_combos) const {
        auto line = [](const std::string& name, const Tally& t) {
            std::printf("  %-62s %6llu blocks: %6llu equal %6llu one step %6llu further (largest step %3u)\n", name.c_str(),
                        (unsigned long long)t.blocks, (unsigned long long)t.equal, (unsigned long long)t.one,
                        (unsigned long long)t.more, t.largest);
        };
        std::printf("per-class table (a block is as far off as its worst texel; one step = 1 in an 8-bit channel):\n");
        for (const auto& [name, t] : rows) line(name, t);
        line("ALL", total);
        std::printf("  texels: %llu, equal %llu (%.2f%%), one step %llu (%.2f%%), further %llu (%.3f%%)\n", (unsigned long long)total.texels,
                    (unsigned long long)total.t_equal, 100.0 * (double)total.t_equal / (double)std::max<uint64_t>(1, total.texels),
                    (unsigned long long)total.t_one, 100.0 * (double)total.t_one / (double)std::max<uint64_t>(1, total.texels),
                    (unsigned long long)total.t_more, 100.0 * (double)total.t_more / (double)std::max<uint64_t>(1, total.texels));
        if (with_combos) {
            std::printf("classes of legal blocks with blocks further than one step off (footprint, partitions, planes, modes, grid, levels):\n");
            std::vector<std::pair<std::string, Tally>> bad;
            for (const auto& [name, t] : combos)
                if (t.more) bad.emplace_back(name, t);
            std::sort(bad.begin(), bad.end(), [](const auto& a, const auto& b) { return a.second.more > b.second.more; });
            for (size_t i = 0; i < bad.size() && i < 25; i++) line(bad[i].first, bad[i].second);
        }
        for (const std::string& s : first_failures) std::printf("%s\n", s.c_str());
    }
};

#define VK_APIS(X)                                                                                                    \
    X(vkCreateInstance)                                                                                               \
    X(vkDestroyInstance)                                                                                              \
    X(vkEnumerateInstanceLayerProperties)                                                                             \
    X(vkEnumerateInstanceExtensionProperties)                                                                         \
    X(vkEnumeratePhysicalDevices)                                                                                     \
    X(vkGetPhysicalDeviceProperties)                                                                                  \
    X(vkGetPhysicalDeviceQueueFamilyProperties)                                                                       \
    X(vkGetPhysicalDeviceMemoryProperties)                                                                            \
    X(vkGetPhysicalDeviceFeatures)                                                                                    \
    X(vkCreateDevice)                                                                                                 \
    X(vkDestroyDevice)                                                                                                \
    X(vkGetDeviceQueue)                                                                                               \
    X(vkCreateShaderModule)                                                                                           \
    X(vkDestroyShaderModule)                                                                                          \
    X(vkCreateDescriptorSetLayout)                                                                                    \
    X(vkDestroyDescriptorSetLayout)                                                                                   \
    X(vkCreatePipelineLayout)                                                                                         \
    X(vkDestroyPipelineLayout)                                                                                        \
    X(vkCreateComputePipelines)                                                                                       \
    X(vkDestroyPipeline)                                                                                              \
    X(vkCreateDescriptorPool)                                                                                         \
    X(vkDestroyDescriptorPool)                                                                                        \
    X(vkAllocateDescriptorSets)                                                                                       \
    X(vkUpdateDescriptorSets)                                                                                         \
    X(vkCreateBuffer)                                                                                                 \
    X(vkDestroyBuffer)                                                                                                \
    X(vkGetBufferMemoryRequirements)                                                                                  \
    X(vkBindBufferMemory)                                                                                             \
    X(vkCreateImage)                                                                                                  \
    X(vkDestroyImage)                                                                                                 \
    X(vkGetImageMemoryRequirements)                                                                                   \
    X(vkBindImageMemory)                                                                                              \
    X(vkCreateImageView)                                                                                              \
    X(vkDestroyImageView)                                                                                             \
    X(vkAllocateMemory)                                                                                               \
    X(vkFreeMemory)                                                                                                   \
    X(vkMapMemory)                                                                                                    \
    X(vkUnmapMemory)                                                                                                  \
    X(vkCreateCommandPool)                                                                                            \
    X(vkDestroyCommandPool)                                                                                           \
    X(vkAllocateCommandBuffers)                                                                                       \
    X(vkResetCommandBuffer)                                                                                           \
    X(vkBeginCommandBuffer)                                                                                           \
    X(vkEndCommandBuffer)                                                                                             \
    X(vkCmdBindPipeline)                                                                                              \
    X(vkCmdBindDescriptorSets)                                                                                        \
    X(vkCmdPushConstants)                                                                                             \
    X(vkCmdDispatch)                                                                                                  \
    X(vkCmdPipelineBarrier)                                                                                           \
    X(vkCmdCopyImageToBuffer)                                                                                         \
    X(vkCreateFence)                                                                                                  \
    X(vkDestroyFence)                                                                                                 \
    X(vkResetFences)                                                                                                  \
    X(vkQueueSubmit)                                                                                                  \
    X(vkWaitForFences)

#define DECLARE_VK(n) PFN_##n n = nullptr;
VK_APIS(DECLARE_VK)
#undef DECLARE_VK

// ---- a level as the renderer binds it: the blocks in Switch 2 block-linear storage, GOB blocks 2^tile_h GOBs high, the layers
// layer_stride bytes apart
struct Level {
    uint32_t bw = 0, bh = 0, w = 0, h = 0, layers = 1, bx = 0, by = 0, tile_h = 0;
    std::vector<uint8_t> linear;   // bx * by * layers blocks, row-major
    std::vector<uint8_t> swz;
    AstcPush push{};
};

uint32_t swizzle_offset(uint32_t bx_bytes, uint32_t by, bool native_tiling) {
    uint32_t swizzle = ((bx_bytes & 32u) << 3u) | ((by & 6u) << 5u) | ((bx_bytes & 16u) << 1u) | ((by & 1u) << 4u) | (bx_bytes & 15u);
    if (native_tiling) {
        const uint32_t diff = ((swizzle >> 5u) ^ (swizzle >> 6u)) & 1u;
        swizzle ^= (diff << 5u) | (diff << 6u);
    }
    return swizzle;
}

Level make_level(uint32_t bw, uint32_t bh, uint32_t w, uint32_t h, uint32_t layers, uint32_t tile_h, bool native, bool srgb,
                 std::vector<uint8_t> linear) {
    Level l;
    l.bw = bw, l.bh = bh, l.w = w, l.h = h, l.layers = layers, l.tile_h = tile_h;
    l.bx = (w + bw - 1) / bw, l.by = (h + bh - 1) / bh;
    CHECK(linear.size() == (size_t)l.bx * l.by * layers * 16);
    l.linear = std::move(linear);
    const uint32_t mask = (1u << tile_h) - 1u, x_shift = 9u + tile_h;
    const uint32_t gobs_x = (l.bx * 16u + 63u) / 64u, gob_rows = (l.by + 7u) / 8u;
    const uint32_t block_size = gobs_x * (512u << tile_h);
    const uint32_t layer_stride = ((gob_rows + mask) >> tile_h) * block_size;
    l.swz.assign((size_t)layer_stride * layers, 0);
    for (uint32_t z = 0; z < layers; z++)
        for (uint32_t y = 0; y < l.by; y++)
            for (uint32_t x = 0; x < l.bx; x++) {
                const uint32_t gob_y = y >> 3u;
                const uint32_t off = z * layer_stride + (gob_y >> tile_h) * block_size + ((gob_y & mask) << 9u) +
                                     (((x << 4u) >> 6u) << x_shift) + swizzle_offset(x << 4u, y, native);
                CHECK(off + 16u <= l.swz.size());
                std::memcpy(&l.swz[off], &l.linear[((size_t)z * l.by * l.bx + (size_t)y * l.bx + x) * 16], 16);
            }
    if (native) {   // the layout the renderer binds is texdecode's block-linear layout
        CHECK(mkwvk::texdecode::block_linear_size(16, l.bx, l.by, 1, tile_h, 0) == layer_stride);
        std::vector<uint8_t> one(layer_stride);
        for (uint32_t z = 0; z < layers; z++) {
            CHECK(mkwvk::texdecode::swizzle(one, std::span<const uint8_t>(&l.linear[(size_t)z * l.bx * l.by * 16], (size_t)l.bx * l.by * 16),
                                            16, l.bx, l.by, 1, tile_h, 0));
            CHECK(!std::memcmp(one.data(), &l.swz[(size_t)z * layer_stride], layer_stride));
        }
    }
    l.push.block_dims[0] = bw, l.push.block_dims[1] = bh;
    l.push.layer_stride = layer_stride;
    l.push.block_size = block_size;
    l.push.x_shift = x_shift;
    l.push.block_height = tile_h;
    l.push.block_height_mask = mask;
    l.push.native_tiling = native ? 1u : 0u;
    l.push.blocks_x = l.bx, l.push.blocks_y = l.by;
    l.push.srgb = srgb ? 1u : 0u;
    return l;
}

/// texdecode's answer for the level: w * h * layers RGBA texels, an illegal block transparent black
std::vector<uint8_t> reference(const Level& l) {
    std::vector<uint8_t> rgba((size_t)l.w * l.h * l.layers * 4);
    CHECK(mkwvk::texdecode::astc_decode(l.linear, l.w, l.h, l.layers, l.bw, l.bh, l.push.srgb != 0, rgba, kErrorTransparent) >= 0);
    return rgba;
}

/// Does the shader store through an unsigned-integer image (a SPIR-V OpTypeImage whose sampled type is an integer: then the
/// view is R8G8B8A8_UINT) or through a float one (the view is R8G8B8A8_UNORM)?
bool shader_stores_uint() {
    const size_t n = sizeof(ASTC_DECODER_COMP_SPV) / 4;
    std::set<uint32_t> ints;
    std::vector<uint32_t> sampled;
    for (size_t i = 5; i < n;) {
        const uint32_t op = ASTC_DECODER_COMP_SPV[i] & 0xFFFF, len = ASTC_DECODER_COMP_SPV[i] >> 16;
        CHECK(len && i + len <= n);
        if (op == 21) ints.insert(ASTC_DECODER_COMP_SPV[i + 1]);
        if (op == 25) sampled.push_back(ASTC_DECODER_COMP_SPV[i + 2]);
        i += len;
    }
    CHECK(!sampled.empty());
    for (uint32_t s : sampled)
        if (ints.count(s)) return true;
    return false;
}


std::atomic<int> validation_errors{0};

std::string lower_str(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

uint32_t find_memory_type(const VkPhysicalDeviceMemoryProperties& props, uint32_t bits, VkMemoryPropertyFlags required) {
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & required) == required) return i;
    return UINT32_MAX;
}

struct Buf {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* ptr = nullptr;
};

struct Gpu {
    void* loader = nullptr;
    VkInstance instance{};
    VkDevice device{};
    VkQueue queue{};
    VkPhysicalDeviceMemoryProperties mem{};
    VkDescriptorSetLayout set_layout{};
    VkPipelineLayout pipe_layout{};
    VkPipeline pipeline{};
    VkDescriptorPool pool{};
    VkDescriptorSet set{};
    VkCommandPool cmd_pool{};
    VkCommandBuffer cmd{};
    VkFence fence{};
    VkDebugUtilsMessengerEXT messenger{};
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger = nullptr;
    bool uint_view = false;

    // 0: ready, 1: failed, 2: unavailable
    int open(const char* requested) {
#ifdef _WIN32
        HMODULE lib = LoadLibraryW(L"vulkan-1.dll");
        auto symbol = [&](const char* name) { return lib ? reinterpret_cast<void*>(GetProcAddress(lib, name)) : nullptr; };
        loader = lib;
#else
        loader = dlopen("libvulkan.so.1", RTLD_NOW);
        auto symbol = [&](const char* name) { return loader ? dlsym(loader, name) : nullptr; };
#endif
        if (!loader) return unavailable("Vulkan loader unavailable");
        bool api_ready = true;
#define LOAD_VK(n) n = reinterpret_cast<PFN_##n>(symbol(#n)); api_ready = api_ready && (n != nullptr);
        VK_APIS(LOAD_VK)
#undef LOAD_VK
        auto gip = reinterpret_cast<PFN_vkGetInstanceProcAddr>(symbol("vkGetInstanceProcAddr"));
        if (!api_ready || !gip) return unavailable("required Vulkan entry points unavailable");

        uint32_t nl = 0, ne = 0;
        CHECK(vkEnumerateInstanceLayerProperties(&nl, nullptr) == VK_SUCCESS);
        std::vector<VkLayerProperties> layers_list(nl);
        CHECK(vkEnumerateInstanceLayerProperties(&nl, layers_list.data()) == VK_SUCCESS);
        CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &ne, nullptr) == VK_SUCCESS);
        std::vector<VkExtensionProperties> extensions(ne);
        CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &ne, extensions.data()) == VK_SUCCESS);
        bool validation = false, debug_utils = false;
        for (const auto& l : layers_list)
            if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) validation = true;
        for (const auto& e : extensions)
            if (std::strcmp(e.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) debug_utils = true;
        const char* layer_name = "VK_LAYER_KHRONOS_validation";
        const char* ext_name = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        VkDebugUtilsMessengerCreateInfoEXT db{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        db.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        db.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        db.pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                const VkDebugUtilsMessengerCallbackDataEXT* data, void*) -> VkBool32 {
            if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
                validation_errors.fetch_add(1, std::memory_order_relaxed);
                std::fprintf(stderr, "validation: %s\n", data->pMessage);
            }
            return VK_FALSE;
        };
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        if (validation) ici.enabledLayerCount = 1, ici.ppEnabledLayerNames = &layer_name;
        if (validation && debug_utils) ici.enabledExtensionCount = 1, ici.ppEnabledExtensionNames = &ext_name, ici.pNext = &db;
        const VkResult ir = vkCreateInstance(&ici, nullptr, &instance);
        if (ir == VK_ERROR_INCOMPATIBLE_DRIVER || ir == VK_ERROR_INITIALIZATION_FAILED) return unavailable("no compatible Vulkan driver");
        CHECK(ir == VK_SUCCESS);
        if (validation && debug_utils) {
            auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(gip(instance, "vkCreateDebugUtilsMessengerEXT"));
            destroy_messenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(gip(instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (create_messenger && destroy_messenger) CHECK(create_messenger(instance, &db, nullptr, &messenger) == VK_SUCCESS);
        }

        uint32_t nd = 0;
        CHECK(vkEnumeratePhysicalDevices(instance, &nd, nullptr) == VK_SUCCESS);
        std::vector<VkPhysicalDevice> gpus(nd);
        if (nd) CHECK(vkEnumeratePhysicalDevices(instance, &nd, gpus.data()) == VK_SUCCESS);
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties chosen{};
        uint32_t queue_family = UINT32_MAX;
        const std::string want = requested ? lower_str(requested) : std::string{};
        for (VkPhysicalDevice cand : gpus) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(cand, &props);
            if (!want.empty() && lower_str(props.deviceName).find(want) == std::string::npos) continue;
            uint32_t nq = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(cand, &nq, nullptr);
            std::vector<VkQueueFamilyProperties> qf(nq);
            vkGetPhysicalDeviceQueueFamilyProperties(cand, &nq, qf.data());
            uint32_t q_pick = UINT32_MAX;
            for (uint32_t q = 0; q < nq; ++q)
                if ((qf[q].queueFlags & VK_QUEUE_COMPUTE_BIT) && (qf[q].queueFlags & VK_QUEUE_TRANSFER_BIT)) {
                    q_pick = q;
                    break;
                }
            if (q_pick == UINT32_MAX) continue;
            if (physical == VK_NULL_HANDLE || (want.empty() && props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
                                               chosen.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)) {
                physical = cand, chosen = props, queue_family = q_pick;
            }
        }
        if (physical == VK_NULL_HANDLE) {
            if (requested) {
                std::fprintf(stderr, "FAIL: requested Vulkan device '%s' not found\n", requested);
                return 1;
            }
            return unavailable("no Vulkan compute physical device found");
        }
        std::printf("Device: %s\n", chosen.deviceName);
        std::printf("validation: %s\n", validation ? (messenger ? "enabled" : "enabled; debug utils unavailable") : "unavailable");

        const float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = queue_family, qci.queueCount = 1, qci.pQueuePriorities = &prio;
        VkPhysicalDeviceFeatures supported{}, enabled{};
        vkGetPhysicalDeviceFeatures(physical, &supported);
        enabled.shaderStorageImageWriteWithoutFormat = supported.shaderStorageImageWriteWithoutFormat;   // (a float store to an image
        // with no format qualifier, what the shader did before it stored through an unsigned view, needs it)
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1, dci.pQueueCreateInfos = &qci, dci.pEnabledFeatures = &enabled;
        CHECK(vkCreateDevice(physical, &dci, nullptr, &device) == VK_SUCCESS);
        vkGetDeviceQueue(device, queue_family, 0, &queue);
        vkGetPhysicalDeviceMemoryProperties(physical, &mem);

        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = sizeof(ASTC_DECODER_COMP_SPV), smci.pCode = ASTC_DECODER_COMP_SPV;
        VkShaderModule shader{};
        CHECK(vkCreateShaderModule(device, &smci, nullptr, &shader) == VK_SUCCESS);
        const VkDescriptorSetLayoutBinding bindings[2] = {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dslci.bindingCount = 2, dslci.pBindings = bindings;
        CHECK(vkCreateDescriptorSetLayout(device, &dslci, nullptr, &set_layout) == VK_SUCCESS);
        const VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AstcPush)};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 1, plci.pSetLayouts = &set_layout, plci.pushConstantRangeCount = 1, plci.pPushConstantRanges = &pcr;
        CHECK(vkCreatePipelineLayout(device, &plci, nullptr, &pipe_layout) == VK_SUCCESS);
        VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT, cpci.stage.module = shader, cpci.stage.pName = "main";
        cpci.layout = pipe_layout;
        CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline) == VK_SUCCESS);
        vkDestroyShaderModule(device, shader, nullptr);
        const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpci.maxSets = 1, dpci.poolSizeCount = 2, dpci.pPoolSizes = sizes;
        CHECK(vkCreateDescriptorPool(device, &dpci, nullptr, &pool) == VK_SUCCESS);
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = pool, dsai.descriptorSetCount = 1, dsai.pSetLayouts = &set_layout;
        CHECK(vkAllocateDescriptorSets(device, &dsai, &set) == VK_SUCCESS);
        VkCommandPoolCreateInfo cmdpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cmdpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, cmdpci.queueFamilyIndex = queue_family;
        CHECK(vkCreateCommandPool(device, &cmdpci, nullptr, &cmd_pool) == VK_SUCCESS);
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = cmd_pool, cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, cbai.commandBufferCount = 1;
        CHECK(vkAllocateCommandBuffers(device, &cbai, &cmd) == VK_SUCCESS);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        CHECK(vkCreateFence(device, &fci, nullptr, &fence) == VK_SUCCESS);
        uint_view = shader_stores_uint();
        std::printf("the shader stores through an %s view of the image\n", uint_view ? "R8G8B8A8_UINT" : "R8G8B8A8_UNORM");
        return 0;
    }

    int unavailable(const char* why) {
        std::printf("SKIP: %s\n", why);
        return 2;
    }

    Buf make_buf(VkDeviceSize size, VkBufferUsageFlags usage) {
        Buf b;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size, bci.usage = usage, bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        CHECK(vkCreateBuffer(device, &bci, nullptr, &b.buffer) == VK_SUCCESS);
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, b.buffer, &req);
        const uint32_t type = find_memory_type(mem, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        CHECK(type != UINT32_MAX);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size, mai.memoryTypeIndex = type;
        CHECK(vkAllocateMemory(device, &mai, nullptr, &b.memory) == VK_SUCCESS);
        CHECK(vkBindBufferMemory(device, b.buffer, b.memory, 0) == VK_SUCCESS);
        CHECK(vkMapMemory(device, b.memory, 0, size, 0, &b.ptr) == VK_SUCCESS);
        return b;
    }
    void free_buf(Buf& b) {
        vkUnmapMemory(device, b.memory);
        vkDestroyBuffer(device, b.buffer, nullptr);
        vkFreeMemory(device, b.memory, nullptr);
    }

    /// Decode the level's swizzled blocks with the shader into an image of w x h x layers and read it back
    std::vector<uint8_t> decode(const Level& l) {
        Buf in = make_buf(l.swz.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        std::memcpy(in.ptr, l.swz.data(), l.swz.size());
        const size_t out_bytes = (size_t)l.w * l.h * l.layers * 4;
        Buf rb = make_buf(out_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        std::memset(rb.ptr, 0x5A, out_bytes);

        // (R8G8B8A8_UNORM and MUTABLE_FORMAT, as the renderer's decoded textures: the storage view is UNORM or UINT)
        VkImageCreateInfo imci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        imci.imageType = VK_IMAGE_TYPE_2D, imci.format = VK_FORMAT_R8G8B8A8_UNORM;
        imci.extent = {l.w, l.h, 1u}, imci.mipLevels = 1u, imci.arrayLayers = l.layers;
        imci.samples = VK_SAMPLE_COUNT_1_BIT, imci.tiling = VK_IMAGE_TILING_OPTIMAL;
        imci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imci.sharingMode = VK_SHARING_MODE_EXCLUSIVE, imci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage image{};
        CHECK(vkCreateImage(device, &imci, nullptr, &image) == VK_SUCCESS);
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, image, &req);
        uint32_t type = find_memory_type(mem, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) type = find_memory_type(mem, req.memoryTypeBits, 0);
        CHECK(type != UINT32_MAX);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size, mai.memoryTypeIndex = type;
        VkDeviceMemory image_mem{};
        CHECK(vkAllocateMemory(device, &mai, nullptr, &image_mem) == VK_SUCCESS);
        CHECK(vkBindImageMemory(device, image, image_mem, 0) == VK_SUCCESS);
        VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivci.image = image, ivci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        ivci.format = uint_view ? VK_FORMAT_R8G8B8A8_UINT : VK_FORMAT_R8G8B8A8_UNORM;
        ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, l.layers};
        VkImageView view{};
        CHECK(vkCreateImageView(device, &ivci, nullptr, &view) == VK_SUCCESS);

        VkDescriptorBufferInfo dbi{in.buffer, 0, l.swz.size()};
        VkDescriptorImageInfo dii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, writes[0].dstSet = set, writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1, writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, writes[0].pBufferInfo = &dbi;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, writes[1].dstSet = set, writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1, writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, writes[1].pImageInfo = &dii;
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

        CHECK(vkResetCommandBuffer(cmd, 0) == VK_SUCCESS);
        VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        CHECK(vkBeginCommandBuffer(cmd, &cbbi) == VK_SUCCESS);
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image, barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, l.layers};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AstcPush), &l.push);
        vkCmdDispatch(cmd, (l.bx + 7u) / 8u, (l.by + 7u) / 8u, l.layers);
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL, barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, l.layers};
        region.imageExtent = {l.w, l.h, 1u};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer, 1, &region);
        CHECK(vkEndCommandBuffer(cmd) == VK_SUCCESS);
        CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1, si.pCommandBuffers = &cmd;
        CHECK(vkQueueSubmit(queue, 1, &si, fence) == VK_SUCCESS);
        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 60000000000ull) == VK_SUCCESS);
        std::vector<uint8_t> out(out_bytes);
        std::memcpy(out.data(), rb.ptr, out_bytes);

        vkDestroyImageView(device, view, nullptr);
        vkDestroyImage(device, image, nullptr);
        vkFreeMemory(device, image_mem, nullptr);
        free_buf(rb);
        free_buf(in);
        return out;
    }

    void close() {
        if (!loader) return;
        if (device) {
            vkDestroyFence(device, fence, nullptr);
            vkDestroyCommandPool(device, cmd_pool, nullptr);
            vkDestroyDescriptorPool(device, pool, nullptr);
            vkDestroyPipeline(device, pipeline, nullptr);
            vkDestroyPipelineLayout(device, pipe_layout, nullptr);
            vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (messenger && destroy_messenger) destroy_messenger(instance, messenger, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(loader));
#else
        dlclose(loader);
#endif
        loader = nullptr;
    }
};

// ---- the cases

/// Compare two RGBA buffers exactly; prints the first few differing texels; returns how many texels differ
uint64_t compare(const char* label, const std::vector<uint8_t>& got, const std::vector<uint8_t>& want, const Level& l) {
    CHECK(got.size() == want.size());
    uint64_t bad = 0;
    for (size_t i = 0; i < got.size(); i += 4) {
        if (!std::memcmp(&got[i], &want[i], 4)) continue;
        if (bad++ < 1) {
            const size_t t = i / 4;
            const uint32_t x = (uint32_t)(t % l.w), y = (uint32_t)(t / l.w % l.h), z = (uint32_t)(t / ((size_t)l.w * l.h));
            std::fprintf(stderr, "MISMATCH %s %ux%u at (%u,%u) layer %u, block (%u,%u): gpu=(%u,%u,%u,%u) texdecode=(%u,%u,%u,%u)\n", label,
                         l.bw, l.bh, x, y, z, x / l.bw, y / l.bh, got[i], got[i + 1], got[i + 2], got[i + 3], want[i], want[i + 1], want[i + 2],
                         want[i + 3]);
        }
    }
    return bad;
}

/// The vector blocks of one footprint, as a level 32 blocks wide with no clipping (the last row repeats the first block)
Level vector_level(const std::vector<Vector>& vectors, uint32_t bw, uint32_t bh, bool srgb, std::vector<uint32_t>& index) {
    index.clear();
    for (uint32_t i = 0; i < vectors.size(); i++)
        if (vectors[i].bw == bw && vectors[i].bh == bh) index.push_back(i);
    CHECK(!index.empty());
    const uint32_t bx = 32, by = ((uint32_t)index.size() + bx - 1) / bx;
    std::vector<uint8_t> blocks((size_t)bx * by * 16);
    for (size_t k = 0; k < (size_t)bx * by; k++) std::memcpy(&blocks[k * 16], vectors[index[k < index.size() ? k : 0]].block, 16);
    return make_level(bw, bh, bx * bw, by * bh, 1, 1, true, srgb, std::move(blocks));
}

/// The vector file against astcenc's own answers, on the CPU: texdecode equals astcenc on every legal vector block (so it is a
/// reference), and the file has every footprint
void check_vectors_cpu(const std::vector<Vector>& vectors) {
    std::set<uint32_t> footprints;
    uint64_t legal = 0;
    for (const Vector& v : vectors) {
        footprints.insert(v.bw << 8 | v.bh);
        for (int mode = 0; mode < 2; mode++) {
            uint8_t out[12 * 12 * 4];
            const bool ok = mkwvk::texdecode::astc_decode_block(v.block, v.bw, v.bh, mode == 1, out, mkwvk::texdecode::kErrorMagenta);
            const bool astcenc_bad = v.flags >> mode & 1;
            CHECK(ok != astcenc_bad);
            CHECK(fnv(out, v.bw * v.bh * 4) == v.sum[mode]);
            legal += !astcenc_bad;
        }
    }
    CHECK(footprints.size() == 14);
    CHECK(legal > vectors.size());
}

/// the hand-built blocks mean what they were built to: the two illegal ones are illegal, the rest decode to something not black
void check_hand_blocks_cpu() {
    uint64_t texels = 0;
    for (const auto& fp : kFootprints) {
        const uint32_t bw = fp[0], bh = fp[1], w = bw * 4 + 3, h = bh * 3 + 1, bx = (w + bw - 1) / bw, by = (h + bh - 1) / bh;
        const std::vector<uint8_t> blocks = hand::image_blocks(bw, bh, bx, by, 0);
        std::vector<uint8_t> rgba((size_t)w * h * 4);
        const int64_t bad = mkwvk::texdecode::astc_decode(blocks, w, h, 1, bw, bh, false, rgba, mkwvk::texdecode::kErrorMagenta);
        CHECK(bad == 2);   // (blocks 2 and 3)
        uint8_t one[12 * 12 * 4];
        for (uint32_t i = 4; i < bx * by; i++)
            CHECK(mkwvk::texdecode::astc_decode_block(&blocks[(size_t)i * 16], bw, bh, false, one));
        texels += (uint64_t)w * h;
    }
    CHECK(texels > 0);
}

int run_cpu_only(const std::vector<Vector>& vectors) {
    CHECK(sizeof(ASTC_DECODER_COMP_SPV) >= 20u && sizeof(ASTC_DECODER_COMP_SPV) % 4u == 0u && ASTC_DECODER_COMP_SPV[0] == 0x07230203u);
    (void)shader_stores_uint();
    check_vectors_cpu(vectors);
    check_hand_blocks_cpu();
    // the level builder's layout is texdecode's block-linear one
    std::vector<uint8_t> blocks(8 * 9 * 3 * 16, 7);
    (void)make_level(5, 5, 40, 45, 3, 2, true, false, blocks);
    std::printf("PASS: the vectors (%zu blocks) equal astcenc on the CPU, the hand-built blocks mean what they were built to, SPIR-V valid "
                "(%zu bytes); GPU execution not requested\n",
                vectors.size(), sizeof(ASTC_DECODER_COMP_SPV));
    return 0;
}

int run_gpu(const std::vector<Vector>& vectors, const char* device, bool print_table) {
    check_vectors_cpu(vectors);
    check_hand_blocks_cpu();
    Gpu gpu;
    const int opened = gpu.open(device);
    if (opened) {
        gpu.close();
        return opened == 2 ? 0 : 1;
    }
    bool failed = false;

    // 1. the vectors, per footprint and decode mode
    Table table;
    uint64_t decodes = 0, exact = 0;
    for (const auto& fp : kFootprints) {
        const uint32_t bw = fp[0], bh = fp[1];
        for (int mode = 0; mode < 2; mode++) {
            std::vector<uint32_t> index;
            const Level l = vector_level(vectors, bw, bh, mode == 1, index);
            const std::vector<uint8_t> got = gpu.decode(l), want = reference(l);
            for (size_t k = 0; k < index.size(); k++) {
                const uint32_t bxk = (uint32_t)(k % l.bx), byk = (uint32_t)(k / l.bx);
                uint8_t g[12 * 12 * 4], r[12 * 12 * 4];
                for (uint32_t t = 0; t < bw * bh; t++) {
                    const size_t at = ((size_t)(byk * bh + t / bw) * l.w + bxk * bw + t % bw) * 4;
                    std::memcpy(g + 4 * t, &got[at], 4);
                    std::memcpy(r + 4 * t, &want[at], 4);
                }
                uint8_t scratch[12 * 12 * 4];
                const bool ok = mkwvk::texdecode::astc_decode_block(vectors[index[k]].block, bw, bh, mode == 1, scratch, kErrorTransparent);
                table.add(classify(vectors[index[k]].block, bw, bh, ok), g, r, bw * bh, mode ? "sRGB  " : "linear", index[k]);
                decodes++;
                exact += !std::memcmp(g, r, (size_t)bw * bh * 4);
            }
        }
    }
    if (exact != decodes) {
        std::printf("FAIL: %llu of %llu astcenc vector block decodes differ on the GPU\n", (unsigned long long)(decodes - exact),
                    (unsigned long long)decodes);
        table.print(true);
        failed = true;
    } else {
        if (print_table) table.print(false);
        std::printf("PASS: %llu astcenc vector block decodes bit-exact on the GPU, 14 footprints, linear and sRGB\n", (unsigned long long)decodes);
    }

    // 2. whole levels made of vector blocks
    {
        uint64_t levels = 0, bad = 0;
        for (const auto& fp : kFootprints) {
            const uint32_t bw = fp[0], bh = fp[1];
            for (int variant = 0; variant < 4; variant++) {
                const uint32_t tile_h = variant == 0 ? 0 : variant == 1 ? 1 : 2, w = 29, h = 13, layers = 3;
                const bool srgb = variant == 1 || variant == 3;
                const uint32_t bx = (w + bw - 1) / bw, by = (h + bh - 1) / bh;
                std::vector<uint8_t> blocks;
                uint32_t at = variant * 5u;
                for (uint32_t n = 0; n < bx * by * layers; n++) {
                    while (vectors[at].bw != bw || vectors[at].bh != bh) at = (at + 1) % (uint32_t)vectors.size();
                    blocks.insert(blocks.end(), vectors[at].block, vectors[at].block + 16);
                    at = (at + 7) % (uint32_t)vectors.size();
                }
                const Level l = make_level(bw, bh, w, h, layers, tile_h, true, srgb, std::move(blocks));
                bad += compare(srgb ? "level sRGB" : "level linear", gpu.decode(l), reference(l), l);
                levels++;
            }
        }
        // a binding that ends early: the blocks it does not hold decode as zero blocks (illegal: the error colour)
        for (int srgb = 0; srgb < 2; srgb++) {
            const uint32_t bw = 5, bh = 5, w = 40, h = 40;
            std::vector<uint8_t> blocks;
            std::vector<uint32_t> index;
            const Level full = vector_level(vectors, bw, bh, srgb != 0, index);
            blocks.assign(full.linear.begin(), full.linear.begin() + 8 * 8 * 16);
            Level l = make_level(bw, bh, w, h, 1, 0, true, srgb != 0, blocks);
            l.swz.resize(512);   // (the second GOB of the row, blocks 4..7, is not there)
            for (uint32_t y = 0; y < 8; y++) std::memset(&l.linear[((size_t)y * 8 + 4) * 16], 0, 4 * 16);
            bad += compare("short binding", gpu.decode(l), reference(l), l);
            levels++;
        }
        if (bad) {
            std::printf("FAIL: %llu texels of %llu whole levels differ from texdecode\n", (unsigned long long)bad, (unsigned long long)levels);
            failed = true;
        } else {
            std::printf("PASS: %llu whole levels bit-exact against texdecode: layers, edge blocks, GOB tiles, a short binding\n",
                        (unsigned long long)levels);
        }
    }

    // 3. the hand-built blocks (Longshot #2194's cases)
    {
        uint64_t levels = 0, bad = 0;
        for (size_t d = 0; d < kFootprints.size(); d++) {
            const uint32_t bw = kFootprints[d][0], bh = kFootprints[d][1], w = bw * 4 + 3, h = bh * 3 + 1;
            const uint32_t bx = (w + bw - 1) / bw, by = (h + bh - 1) / bh;
            struct Variant { bool native, srgb; uint32_t tile_h; };
            for (const Variant v : {Variant{false, false, 0}, Variant{true, false, 0}, Variant{true, true, 0}, Variant{true, true, 2}}) {
                const Level l = make_level(bw, bh, w, h, 1, v.tile_h, v.native, v.srgb,
                                           hand::image_blocks(bw, bh, bx, by, (uint32_t)d * 2u + (v.native ? 1u : 0u)));
                bad += compare(v.srgb ? "hand sRGB" : "hand linear", gpu.decode(l), reference(l), l);
                levels++;
            }
        }
        if (bad) {
            std::printf("FAIL: %llu texels of %llu hand-built levels differ from texdecode\n", (unsigned long long)bad, (unsigned long long)levels);
            failed = true;
        } else {
            std::printf("PASS: %llu hand-built levels bit-exact against texdecode: void extents, illegal blocks, 1-4 partitions, two planes, grids\n",
                        (unsigned long long)levels);
        }
    }

    gpu.close();
    if (validation_errors.load() != 0) {
        std::fprintf(stderr, "FAIL: %d Vulkan validation error(s)\n", validation_errors.load());
        return 1;
    }
    if (failed) return 1;
    std::printf("PASS: the GPU ASTC decoder equals astcenc and texdecode on the GPU\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    int vectors_arg = 0;
    const char* device = nullptr;
    bool gpu = false, table = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--gpu")) gpu = true;
        else if (!std::strcmp(argv[i], "--device") && i + 1 < argc) gpu = true, device = argv[++i];
        else if (!std::strcmp(argv[i], "--cpu-only")) gpu = false;
        else if (!std::strcmp(argv[i], "--table")) table = true;
        else if (!vectors_arg) vectors_arg = i;
        else {
            std::fprintf(stderr, "usage: %s vectors.vec [--cpu-only | --gpu [--device NAME_SUBSTRING]] [--table]\n", argv[0]);
            return 2;
        }
    }
    if (!vectors_arg) {
        std::fprintf(stderr, "usage: %s vectors.vec [--cpu-only | --gpu [--device NAME_SUBSTRING]] [--table]\n", argv[0]);
        return 2;
    }
#ifdef _WIN32
    int wide_argc = 0;
    wchar_t** wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
    if (!wide_argv || wide_argc != argc) {
        std::fprintf(stderr, "FAIL: could not read Windows command line arguments\n");
        if (wide_argv) LocalFree(wide_argv);
        return 2;
    }
    const std::vector<Vector> vectors = load_vectors(wide_argv[vectors_arg]);
    LocalFree(wide_argv);
#else
    const std::vector<Vector> vectors = load_vectors(argv[vectors_arg]);
#endif
    return gpu ? run_gpu(vectors, device, table) : run_cpu_only(vectors);
}
