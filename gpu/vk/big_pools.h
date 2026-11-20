// (arm64 port) The big placed pools measured for the log (RX_VK_LOG_BIG_POOLS: gpu/vk/vk_memory.cpp's big_*). On the AYN
// Odin 3 the guest's pools are placed in the device's own memory, and each placement seems to cost its whole size in RAM:
// 5.5 GB of them in Kokiri Forest, one of 2.4 GB placed at boot with 8 KB copied in. Whether a pool can be placed in part, or
// later, depends on which parts of it the GPU reaches and which the game fills: the stretches of a pool (1 MB each, larger
// for a pool over 4 GB) each reached, the 64 KB blocks holding data, and a one-line map of both. No Vulkan;
// tests/test_big_pools.py checks it.
#pragma once
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#ifdef __linux__
#include <sys/mman.h>
#endif

namespace big_pools {

constexpr int kStretches = 4096;   // (a pool's stretches, each marked once reached)
constexpr int kWords = kStretches / 64;
constexpr uint64_t kBlock = 64ull << 10;   // (the data count's block)

/// The shift of a stretch of a pool of n bytes: 1 MB (20), or larger so that the whole pool is kStretches stretches at most
inline int stretch_shift(uint64_t n) {
    int s = 20;
    while (s < 62 && (n >> s) + ((n & ((1ull << s) - 1)) != 0) > (uint64_t)kStretches) s++;
    return s;
}

/// How many stretches a pool of n bytes has
inline int stretches(uint64_t n, int shift) {
    const uint64_t k = (n >> shift) + ((n & ((1ull << shift) - 1)) != 0);
    return k > (uint64_t)kStretches ? kStretches : (int)k;
}

/// Marks the stretches of a pool over [lo, lo + n) that [at, at + size) reaches, clipped to the pool (bits: kWords words).
/// Returns how many were not marked before. Threads may mark the same bits at once
inline int mark(std::atomic<uint64_t>* bits, int shift, uint64_t lo, uint64_t n, uint64_t at, uint64_t size) {
    if (!size || !n || lo > UINT64_MAX - n) return 0;
    const uint64_t end = size > UINT64_MAX - at ? UINT64_MAX : at + size, pool_end = lo + n;
    if (at >= pool_end || end <= lo) return 0;
    const uint64_t a = (at < lo ? lo : at) - lo, b = (end > pool_end ? pool_end : end) - lo;
    int fresh = 0;
    for (uint64_t s = a >> shift, last = (b - 1) >> shift; s <= last && s < (uint64_t)kStretches; s++) {
        const uint64_t bit = 1ull << (s & 63);
        std::atomic<uint64_t>& w = bits[s >> 6];
        if (w.load(std::memory_order_relaxed) & bit) continue;   // (the usual case: no write to a shared line)
        fresh += !(w.fetch_or(bit, std::memory_order_relaxed) & bit);
    }
    return fresh;
}

/// The marked bits as they are now (to: kWords words)
inline void snapshot(const std::atomic<uint64_t>* bits, uint64_t* to) {
    for (int i = 0; i < kWords; i++) to[i] = bits[i].load(std::memory_order_relaxed);
}

/// How many stretches bits has marked
inline int count(const uint64_t* bits) {
    int n = 0;
    for (int i = 0; i < kWords; i++) n += std::popcount(bits[i]);
    return n;
}

/// How many stretches both a and b have marked
inline int count_both(const uint64_t* a, const uint64_t* b) {
    int n = 0;
    for (int i = 0; i < kWords; i++) n += std::popcount(a[i] & b[i]);
    return n;
}

/// Whether [p, p + n) holds only zero bytes
inline bool all_zero(const uint8_t* p, uint64_t n) {
    uint64_t i = 0;
    for (; i + 64 <= n; i += 64) {   // (eight words at a time: most of a pool's blocks are read whole)
        uint64_t w[8];
        std::memcpy(w, p + i, sizeof w);
        if (w[0] | w[1] | w[2] | w[3] | w[4] | w[5] | w[6] | w[7]) return false;
    }
    for (; i < n; i++)
        if (p[i]) return false;
    return true;
}

/// Counts the kBlock blocks of [p, p + len) holding a byte other than zero, p being the pool's offset off (a multiple of
/// kBlock), and marks each such block's stretch in data (kWords words). Returns the count; the highest such block's end, as
/// an offset in the pool, goes in *top when it is above what *top holds
inline uint64_t data_blocks(const uint8_t* p, uint64_t off, uint64_t len, int shift, uint64_t* data, uint64_t* top) {
    uint64_t blocks = 0;
    for (uint64_t at = 0; at < len; at += kBlock) {
        const uint64_t n = len - at < kBlock ? len - at : kBlock;
        if (all_zero(p + at, n)) continue;
        blocks++;
        const uint64_t s = (off + at) >> shift;
        if (s < (uint64_t)kStretches) data[s >> 6] |= 1ull << (s & 63);
        if (top && off + at + n > *top) *top = off + at + n;
    }
    return blocks;
}

/// One line of width characters over a pool of n stretches (width at most n: one each), each character its share of them
/// and the strongest thing found there: W bound for the GPU to write, R bound for it to read, T read by the textures' GPU
/// path, d data the GPU did not reach, . none of these. Any of the bit sets may be null (nothing marked)
inline std::string map_line(const uint64_t* written, const uint64_t* read, const uint64_t* tex, const uint64_t* data, int n,
                            int width) {
    if (n <= 0 || width <= 0) return std::string();
    if (n > kStretches) n = kStretches;
    if (width > n) width = n;
    auto has = [](const uint64_t* bits, int s) { return bits && ((bits[s >> 6] >> (s & 63)) & 1); };
    std::string out((size_t)width, '.');
    for (int c = 0; c < width; c++) {
        const int from = (int)((int64_t)c * n / width), to = (int)((int64_t)(c + 1) * n / width);
        int rank = 0;   // (0 '.', 1 'd', 2 'T', 3 'R', 4 'W')
        for (int s = from; s < to && rank < 4; s++) {
            const int r = has(written, s) ? 4 : has(read, s) ? 3 : has(tex, s) ? 2 : has(data, s) ? 1 : 0;
            if (r > rank) rank = r;
        }
        out[(size_t)c] = ".dTRW"[rank];
    }
    return out;
}

#ifdef __linux__
/// Bytes of [at, at + n) (at page aligned) whose pages this process has mapped in now (mincore: for the device's memory
/// mapped over the guest's pages, those the CPU has touched since). -1: mincore refused
inline int64_t resident_bytes(const void* at, uint64_t n, uint64_t page) {
    if (!page || reinterpret_cast<uintptr_t>(at) % page) return -1;
    constexpr uint64_t kPagesAtOnce = 1u << 16;
    std::vector<unsigned char> v(kPagesAtOnce);
    int64_t bytes = 0;
    for (uint64_t off = 0; off < n; off += kPagesAtOnce * page) {
        const uint64_t len = n - off < kPagesAtOnce * page ? n - off : kPagesAtOnce * page;
        if (mincore(const_cast<uint8_t*>(static_cast<const uint8_t*>(at)) + off, len, v.data())) return -1;
        const uint64_t pages = (len + page - 1) / page;
        for (uint64_t i = 0; i < pages; i++)
            if (v[i] & 1) bytes += (int64_t)(i + 1 == pages && len % page ? len % page : page);
    }
    return bytes;
}
#endif

} // namespace big_pools
