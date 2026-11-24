// Hand-built LDR ASTC blocks for tests/astc_decoder_gpu.cpp: a void extent with a constant colour, two kinds of illegal block, and
// legal blocks with 1 to 4 partitions, one or two weight planes (all four planes the second can be), a weight grid as big as the
// footprint (when the grid layouts reach it) or a smaller one, and the LDR colour modes 0, 4, 6, 8, 10, 12 and 13. The builders are
// those of this fixture's earlier form (Jer9, #2194), with the comment on the block-mode layouts rewritten; the expected values
// are texdecode's (astcenc's), not suyu's.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace hand {

struct Block {
    uint8_t b[16]{};
};

inline void set_bits(Block& blk, uint32_t pos, uint32_t count, uint32_t value) {
    for (uint32_t i = 0; i < count; ++i)
        if (((value >> i) & 1u) && pos + i < 128u) blk.b[(pos + i) >> 3] |= (uint8_t)(1u << ((pos + i) & 7u));
}

/// `weight_bits` pseudo-random bits from the top of the block downward, where the weights are stored
inline void set_weight_bits_reversed(Block& blk, uint32_t weight_bits, uint64_t seed) {
    uint64_t s = seed ^ 0x9E3779B97F4A7C15ull;
    for (uint32_t i = 0; i < weight_bits; ++i) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        const uint32_t p = 127u - i;
        if ((s >> 63u) & 1u) blk.b[p >> 3] |= (uint8_t)(1u << (p & 7u));
    }
}

/// A void extent (one colour) with no extent bounds: block mode bits 0x1FC, bit 9 clear (LDR), bits 10 and 11 set, then the
/// coordinates all ones, then the colour as four UNORM16
inline Block make_void_extent(uint16_t r, uint16_t g, uint16_t b, uint16_t a) {
    Block blk;
    set_bits(blk, 0, 12, 0xDFCu);
    for (uint32_t p = 12; p < 64; ++p) set_bits(blk, p, 1, 1u);
    const uint16_t c[4] = {r, g, b, a};
    for (int i = 0; i < 4; i++) blk.b[8 + 2 * i] = (uint8_t)(c[i] & 0xFF), blk.b[9 + 2 * i] = (uint8_t)(c[i] >> 8);
    return blk;
}

/// An illegal block: 0 a reserved block mode (the low four bits clear); 1 four partitions with two weight planes
inline Block make_error_block(uint32_t variant) {
    Block blk;
    if (variant == 1u) set_bits(blk, 0, 11, 0x424u), set_bits(blk, 11, 2, 3u);
    return blk;
}

/// A block mode (11 bits) for a weight grid of wg_w x wg_h with weight range code `range_code` (R2:R1:R0, R2:R1 not 00).
/// Two of the layouts of the specification's table of block modes: bits 1:0 = R2:R1, bit 4 = R0, bit 9 = precision, bit 10 = two
/// planes, and either bits 3:2 = 00, bits 6:5 = height - 2, bits 8:7 = width - 4 (width 4 to 7, height 2 to 5), or bits 3:2 = 11,
/// bit 8 = 1, bits 6:5 = height - 2 (clamped to 5), bit 7 = width - 2 (clamped to 3)
inline uint32_t encode_block_mode(uint32_t wg_w, uint32_t wg_h, uint32_t range_code, bool high_prec, bool dual_plane) {
    const uint32_t r0 = range_code & 1u, r21 = (range_code >> 1u) & 3u;
    const uint32_t tail = (high_prec ? 1u << 9u : 0u) | (dual_plane ? 1u << 10u : 0u);
    if (wg_w >= 4u && wg_w <= 7u && wg_h >= 2u && wg_h <= 5u)
        return r21 | (r0 << 4u) | ((wg_h - 2u) << 5u) | ((wg_w - 4u) << 7u) | tail;
    return r21 | (3u << 2u) | (r0 << 4u) | (std::min(wg_h - 2u, 3u) << 5u) | (std::min(wg_w - 2u, 1u) << 7u) | (1u << 8u) | tail;
}

/// A legal block of `num_parts` partitions (seed `part_seed`), one weight plane or two (the second plane's colour channel
/// `sec_chan`), colour mode `cem` for every partition; with `direct_grid` (one partition, one plane, footprint up to 7x5) a weight
/// grid as big as the footprint; the colour and weight bits pseudo-random from `seed`
inline Block make_ldr_block(uint32_t bw, uint32_t bh, uint32_t num_parts, uint32_t part_seed, bool dual_plane, uint32_t sec_chan,
                            uint32_t cem, bool direct_grid, uint64_t seed) {
    Block blk;
    uint32_t wg_w = 3u, wg_h = 3u, range_code = 4u;
    if (direct_grid && bw <= 7u && bh <= 5u && !dual_plane && num_parts == 1u) {
        wg_w = bw, wg_h = bh, range_code = 3u;   // (3 levels: 4x4 is 26 weight bits, 5x5 40, 6x5 48)
    } else if (dual_plane) {
        range_code = 4u;                        // (4 levels: 2 * 9 * 2 = 36 weight bits)
    } else if (num_parts >= 3u) {
        range_code = 6u;                        // (6 levels, trit and a bit: 24 weight bits, the least allowed)
    } else {
        wg_w = bw >= 4u ? 4u : 3u, wg_h = bh >= 4u ? 4u : 3u;
        range_code = (seed & 1u) ? 5u : 6u;
    }
    set_bits(blk, 0, 11, encode_block_mode(wg_w, wg_h, range_code, false, dual_plane));
    set_bits(blk, 11, 2, num_parts - 1u);
    uint32_t pos = 13u;
    if (num_parts == 1u) {
        set_bits(blk, pos, 4, cem & 0xFu);
        pos += 4u;
    } else {   // (partition seed, then one colour mode for every partition: class bits 00, mode in bits 5:2)
        set_bits(blk, pos, 10, part_seed & 0x3FFu);
        set_bits(blk, pos + 12u, 4, cem & 0xFu);
        pos += 16u;
    }
    uint64_t s = seed * 0xBF58476D1CE4E5B9ull + 1u;   // (colour bits up to bit 79; the weights overwrite from the top)
    for (uint32_t p = pos; p < 80u; ++p) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        set_bits(blk, p, 1, (uint32_t)((s >> 63u) & 1u));
    }
    set_weight_bits_reversed(blk, 44u, seed ^ 0xA5A5A5A5u);
    if (dual_plane) {   // the colour channel of the second plane: the two bits under the 36 weight bits
        blk.b[90 >> 3] &= (uint8_t) ~(3u << (90 & 7));
        set_bits(blk, 90, 2, sec_chan & 3u);
    }
    return blk;
}

/// The linear blocks (blocks_x x blocks_y, 16 bytes each) of test image `case_idx` for a footprint: the first sixteen are one of
/// each kind, the rest a mix of partitions and colour modes
inline std::vector<uint8_t> image_blocks(uint32_t bw, uint32_t bh, uint32_t blocks_x, uint32_t blocks_y, uint32_t case_idx) {
    static const uint32_t kCems[7] = {0u, 4u, 6u, 8u, 10u, 12u, 13u};
    std::vector<uint8_t> out((size_t)blocks_x * blocks_y * 16);
    for (uint32_t idx = 0; idx < blocks_x * blocks_y; ++idx) {
        const uint64_t seed = ((uint64_t)case_idx << 16u) | (idx * 131u + 17u);
        Block blk;
        switch (idx) {
            case 0: blk = make_void_extent(0x1212u, 0x5656u, 0x9A9Au, 0xDEDEu); break;   // (replicated bytes: shift and round agree)
            case 1: blk = make_void_extent(0xFFFFu, 0x0000u, 0x8080u, 0xFFFFu); break;
            case 2: blk = make_error_block(0u); break;
            case 3: blk = make_error_block(1u); break;
            case 4: blk = make_ldr_block(bw, bh, 1u, 0u, false, 0u, 12u, true, seed); break;     // direct grid, RGBA
            case 5: blk = make_ldr_block(bw, bh, 1u, 0u, false, 0u, 8u, false, seed); break;     // scaled grid, RGB
            case 6: blk = make_ldr_block(bw, bh, 1u, 0u, true, 0u, 12u, false, seed); break;     // two planes, the second R
            case 7: blk = make_ldr_block(bw, bh, 1u, 0u, true, 1u, 12u, false, seed); break;     // G
            case 8: blk = make_ldr_block(bw, bh, 1u, 0u, true, 2u, 8u, false, seed); break;      // B
            case 9: blk = make_ldr_block(bw, bh, 1u, 0u, true, 3u, 12u, false, seed); break;     // A
            case 10: blk = make_ldr_block(bw, bh, 2u, 137u + case_idx, false, 0u, 8u, false, seed); break;
            case 11: blk = make_ldr_block(bw, bh, 2u, 421u + case_idx, true, 3u, 12u, false, seed); break;
            case 12: blk = make_ldr_block(bw, bh, 3u, 733u + case_idx, false, 0u, 6u, false, seed); break;
            case 13: blk = make_ldr_block(bw, bh, 3u, 511u + case_idx, true, 1u, 8u, false, seed); break;
            case 14: blk = make_ldr_block(bw, bh, 4u, 299u + case_idx, false, 0u, 0u, false, seed); break;   // luminance
            case 15: blk = make_ldr_block(bw, bh, 4u, 887u + case_idx, false, 0u, 4u, false, seed); break;   // luminance, alpha
            default:
                blk = make_ldr_block(bw, bh, 1u + (idx % 3u), (idx * 97u + case_idx * 13u) & 0x3FFu, false, 0u, kCems[idx % 7u],
                                     false, seed);
        }
        std::memcpy(&out[(size_t)idx * 16], blk.b, 16);
    }
    return out;
}

}  // namespace hand
