// (arm64 port) The guest's memory pools placed in the GPU's own memory: the bookkeeping and the mappings, no Vulkan.
// gpu/vk/vk_memory.cpp's placed_* use it where the driver takes neither host pointers (VK_EXT_external_memory_host) nor
// dma-bufs of the guest window (no /dev/udmabuf): Turnip, Mesa's Adreno driver, on a kernel without udmabuf. A pool the game
// makes gets memory of the device's, exported as a dma-buf; its contents are copied in (copy_in) and the dma-buf is mapped
// over the pool's guest pages (place). When the game lets the pool go, its contents go back into ordinary memory at the same
// addresses (unplace). tests/test_placed_memory.py checks all of it on a memfd, which maps as a dma-buf does.
#pragma once
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace placed {

struct Range {
    uint64_t lo, hi;
};

/// What a new pool over [lo, hi) does to the placements already made (`placements`: by start, each with its end in .hi;
/// they never overlap): none new when one of them holds the whole range (`inside`, that one's start in `inside_lo`); else
/// one new placement over `range`, the pool's range and every placement it runs into, grown until it runs into no other,
/// which replaces those (`absorbed`: their starts)
struct Plan {
    bool inside = false;
    uint64_t inside_lo = 0;
    Range range{0, 0};
    std::vector<uint64_t> absorbed;
};
template <class Map>
Plan plan(const Map& placements, uint64_t lo, uint64_t hi) {
    Plan p;
    p.range = {lo, hi};
    for (bool grew = true; grew;) {
        grew = false;
        p.absorbed.clear();
        for (const auto& [at, pl] : placements) {   // (a few hundred at most, one a pool; made as rarely as pools)
            if (pl.hi <= p.range.lo || at >= p.range.hi) continue;
            p.absorbed.push_back(at);
            if (at < p.range.lo) p.range.lo = at, grew = true;
            if (pl.hi > p.range.hi) p.range.hi = pl.hi, grew = true;
        }
    }
    if (p.absorbed.size() == 1 && p.absorbed[0] <= lo && placements.at(p.absorbed[0]).hi >= hi)
        p.inside = true, p.inside_lo = p.absorbed[0], p.absorbed.clear();
    return p;
}

/// The page holds only zeros
inline bool zero_page(const uint8_t* p, uint64_t page) {
    uint64_t any = 0, w;
    for (uint64_t i = 0; i < page; i += 8) std::memcpy(&w, p + i, 8), any |= w;
    return any == 0;
}

/// Copies into dst the pages of src [0, n) (n and src page aligned) that may hold anything: those `olds` covers (offsets
/// from src: earlier placements, whose pages are all real) always; of the rest, those in memory or swapped out, as
/// /proc/self/pagemap says (pagemap: its fd; -1, or a read that fails: every page). A page in neither was never written or
/// was given back: zero, as the new memory is. The bytes copied
inline uint64_t copy_in(uint8_t* dst, const uint8_t* src, uint64_t n, uint64_t page, int pagemap,
                        const std::vector<Range>& olds) {
    constexpr uint64_t kPresent = 1ull << 63, kSwapped = 1ull << 62;
    constexpr uint64_t kBatch = 512;
    uint64_t entries[kBatch] = {};
    uint64_t copied = 0, have_lo = 0, have_hi = 0;   // (the pages whose entries are read: [have_lo, have_hi))
    bool all = pagemap < 0;
    uint64_t run = 0, run_n = 0;   // (the pages to copy, joined: [run, run + run_n))
    const auto flush = [&] {
        if (run_n) std::memcpy(dst + run, src + run, run_n), copied += run_n;
        run_n = 0;
    };
    for (uint64_t off = 0; off < n; off += page) {
        bool want = all;
        for (const Range& o : olds) want = want || (off >= o.lo && off < o.hi);
        if (!want) {
            const uint64_t i = off / page;
            if (i >= have_hi) {
                const uint64_t count = std::min<uint64_t>(kBatch, (n - off) / page);
                const uint64_t first = (uint64_t)(uintptr_t)(src + off) / page;
                const ssize_t got = pread(pagemap, entries, count * 8, (off_t)(first * 8));
                if (got != (ssize_t)(count * 8)) all = true, want = true;
                have_lo = i, have_hi = i + count;
            }
            if (!want) want = (entries[i - have_lo] & (kPresent | kSwapped)) != 0;
        }
        if (want && run_n && run + run_n == off) run_n += page;
        else if (want) flush(), run = off, run_n = page;
    }
    flush();
    return copied;
}

/// fd's memory mapped somewhere, n bytes, readable and writable (nullptr: refused)
inline uint8_t* map(int fd, uint64_t n) {
    void* p = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    return p == MAP_FAILED ? nullptr : (uint8_t*)p;
}

/// fd's memory mapped at `at` over whatever was there, n bytes (page aligned), readable and writable. False: refused (the
/// pages there may be gone then: the kernel unmaps before it maps)
inline bool place(uint8_t* at, uint64_t n, int fd) {
    return mmap(at, n, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == (void*)at;
}

/// The placement at [at, at + n) back into ordinary memory (private, anonymous, no huge pages, no swap reserved: as the guest
/// window is made, host/hos_main.c): its contents copied into new pages, the zero pages left out (they read as zero there),
/// which then take its place at the same addresses in one step (mremap). False: refused, the placement left where it was.
/// `copied`: the bytes copied
inline bool unplace(uint8_t* at, uint64_t n, uint64_t page, uint64_t* copied = nullptr) {
    void* tmp = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (tmp == MAP_FAILED) return false;
    madvise(tmp, n, MADV_NOHUGEPAGE);
    uint64_t bytes = 0;
    for (uint64_t off = 0; off < n; off += page)
        if (!zero_page(at + off, page)) std::memcpy((uint8_t*)tmp + off, at + off, page), bytes += page;
    if (mremap(tmp, n, n, MREMAP_MAYMOVE | MREMAP_FIXED, at) != (void*)at) {
        munmap(tmp, n);
        return false;
    }
    if (copied) *copied = bytes;
    return true;
}

} // namespace placed
