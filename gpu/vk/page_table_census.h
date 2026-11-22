// (longshot) Guest memory's page table, for the pointers shaders follow (vk_memory.cpp guest_pages_set), covers three windows,
// as the shaders read it (spirv_emit_context.cpp kGuestPagesBase, kGuestPagesBase2, kGuestPagesBase3): 8 GB from 64 GB, where
// the heap is, 16 GB from 496 GB, where the game's GPU allocator puts its storage buffers, and 4 GB from 1 TB, NVN's virtual
// pools, whose entries are written as their storage buffers are bound (page_table_cover). A load through a pointer into a pool
// outside them that no bound buffer holds reads 0. Whether the game has such pools was never counted: the census logs each one
// as it comes into the pool table (the first 30), and at close how many came, their bytes outside and where they lie, before
// the table is made to reach the whole window.
// Rules only, no Vulkan: vk_memory.cpp keeps the census (memory_sync, page_table_census_log). tests/test_page_table_census.py
// runs them.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace mkwvk {

struct PageWindow {
    uint64_t base, bytes, first;   // (first: the window's first entry in the table, after the windows before it)
};
inline constexpr PageWindow kPageWindows[] = {{0x1000000000ull, 8ull << 30, 0}, {0x7c00000000ull, 16ull << 30, (8ull << 30) >> 12},
                                              {0x10000000000ull, 4ull << 30, ((8ull << 30) + (16ull << 30)) >> 12}};
inline constexpr uint64_t kPageTableEntries = ((8ull << 30) + (16ull << 30) + (4ull << 30)) >> 12;   // (4 KB pages: a device address each)

/// The bytes of [lo, hi) that no window covers
inline uint64_t page_table_outside(uint64_t lo, uint64_t hi) {
    if (hi <= lo) return 0;
    uint64_t in = 0;
    for (const PageWindow& w : kPageWindows) {
        const uint64_t a = std::max(lo, w.base), b = std::min(hi, w.base + w.bytes);
        if (a < b) in += b - a;
    }
    return hi - lo - in;
}

/// "16 KB", "64.0 MB", "1.25 GB"
inline std::string page_census_size(uint64_t bytes) {
    char b[32];
    if (bytes < (1ull << 20)) std::snprintf(b, sizeof b, "%llu KB", (unsigned long long)(bytes >> 10));
    else if (bytes < (1ull << 30)) std::snprintf(b, sizeof b, "%.1f MB", bytes / 1048576.0);
    else std::snprintf(b, sizeof b, "%.2f GB", bytes / 1073741824.0);
    return b;
}

/// "64-72 GB and 508-512 GB"
inline std::string page_census_windows() {
    std::string s;
    for (const PageWindow& w : kPageWindows) {
        char b[48];
        std::snprintf(b, sizeof b, "%s%llu-%llu GB", s.empty() ? "" : " and ", (unsigned long long)(w.base >> 30),
                      (unsigned long long)((w.base + w.bytes) >> 30));
        s += b;
    }
    return s;
}

/// A run of the table's entries not written yet (vk_memory.cpp table_pending): count entries from first, address the first's
struct PageTableRun {
    uint64_t first, count, address;
};
/// The entries guest_pages_set(lo, hi, address) writes (vk_memory.cpp): a run in each window [lo, hi) reaches, the first page's
/// entry address + (its page - lo), 0 for none, the next pages' 4 KB further each; emit(first, count, address) for each
template <class Emit>
inline void page_table_runs(uint64_t lo, uint64_t hi, uint64_t address, Emit&& emit) {
    for (const PageWindow& w : kPageWindows) {
        const uint64_t a0 = std::max(lo, w.base) & ~0xFFFull, a1 = std::min(hi, w.base + w.bytes);
        if (a0 >= a1) continue;
        emit(w.first + ((a0 - w.base) >> 12), (a1 - a0 + 0xFFF) >> 12, address ? address + (a0 - lo) : 0);
    }
}
/// (RX_VK_GP_TABLE, vk_draw.cpp gp_table_cover) Whether a storage buffer bound at gpu is covered: one in a virtual pool (from 1 TB)
/// always, as no table write follows those; a guest pool's only when every binding is checked (every: RX_VK_GP_TABLE set), the
/// table following the pools by itself (vk_memory.cpp guest_pages_set at their import, mirror_make, sparse_table)
inline bool page_table_cover_wanted(uint64_t gpu, bool every) { return every || gpu >= (1ull << 40); }
/// The table's entry for the page at gpu, -1 outside the windows
inline int64_t page_table_index(uint64_t gpu) {
    for (const PageWindow& w : kPageWindows)
        if (gpu >= w.base && gpu - w.base < w.bytes) return (int64_t)(w.first + ((gpu - w.base) >> 12));
    return -1;
}
/// An entry as the GPU will read it: the latest of the runs not written yet that names it, else the table's
inline uint64_t page_table_entry(const uint64_t* entries, const PageTableRun* pending, size_t npending, uint64_t index,
                                 bool* from_pending = nullptr) {
    for (size_t k = npending; k-- > 0;)
        if (index >= pending[k].first && index - pending[k].first < pending[k].count) {
            if (from_pending) *from_pending = true;
            return pending[k].address ? pending[k].address + ((index - pending[k].first) << 12) : 0;
        }
    if (from_pending) *from_pending = false;
    return entries[index];
}
/// (RX_VK_GP_TABLE, vk_memory.cpp memory_table_cover) A storage buffer bound at gpu, n bytes, read at device address address: the
/// table must give each of its pages the memory the binding reads, page p (from gpu's page) the entry address - (gpu & 0xFFF) +
/// (p << 12). Every page looked at. Then a load through the table reads what the storage buffer search read, wherever the search
/// finds the address (it finds it in a bound buffer or not at all, and then it went on to the table already)
struct PageCover {
    uint64_t pages = 0, wrong = 0;   // (its pages, and those whose entry is not the binding's memory)
    uint64_t conflicts = 0;          // (wrong ones that a run not yet written gives other memory: another binding of the same pages)
    bool outside = false;            // (a page outside the windows, or a range past the address space's end: it cannot be covered)
};
inline PageCover page_table_cover(const uint64_t* entries, const PageTableRun* pending, size_t npending, uint64_t gpu, uint64_t n,
                                  uint64_t address) {
    PageCover c;
    n = std::max<uint64_t>(n, 1);
    if (gpu + (n - 1) < gpu || !address) return c.outside = true, c;
    const uint64_t first = gpu & ~0xFFFull, last = (gpu + n - 1) & ~0xFFFull, page0 = address - (gpu & 0xFFFull);
    c.pages = ((last - first) >> 12) + 1;
    const int64_t i0 = page_table_index(first), i1 = page_table_index(last);
    if (i0 < 0 || i1 < 0 || (uint64_t)(i1 - i0) != c.pages - 1) return c.outside = true, c;   // (a window's end in between)
    // (the entries first, a plain loop; then the pages a run not written yet gives otherwise, the latest run first: each page's
    // last run decides it, as the submission writes them in order)
    const uint64_t* at = entries + i0;
    for (uint64_t p = 0; p < c.pages; p++) c.wrong += at[p] != page0 + (p << 12);
    std::vector<uint64_t> done;   // (a bit a page of the range, once a later run has decided it)
    for (size_t k = npending; k-- > 0;) {
        const PageTableRun& r = pending[k];
        const uint64_t lo = std::max<uint64_t>(r.first, (uint64_t)i0), hi = std::min<uint64_t>(r.first + r.count, (uint64_t)i1 + 1);
        if (lo >= hi) continue;
        if (done.empty()) done.assign((c.pages + 63) / 64, 0);
        for (uint64_t i = lo; i < hi; i++) {
            const uint64_t p = i - (uint64_t)i0;
            if (done[p / 64] >> (p % 64) & 1) continue;
            done[p / 64] |= 1ull << (p % 64);
            const uint64_t want = page0 + (p << 12), e = r.address ? r.address + ((i - r.first) << 12) : 0;
            c.wrong -= at[p] != want;
            if (e != want) c.wrong++, c.conflicts += e != 0;
        }
    }
    return c;
}

struct PageTableCensus {
    static constexpr int kNamed = 30;   // (pools named one by one; the rest are counted)
    std::vector<std::pair<uint64_t, uint64_t>> outside;   // (lo, hi) of the pools outside now, sorted
    uint64_t pools = 0;                    // (in the table now, all of them)
    uint64_t came = 0, came_bytes = 0;     // (pools that came into the table with bytes outside, and those bytes; again if one leaves and comes back)
    uint64_t most = 0, most_bytes = 0;     // (the most outside at once, and their bytes outside then)
    uint64_t lowest = UINT64_MAX, highest = 0;   // (the lowest start and the highest end of those pools)
    int named = 0;

    /// The pool table refreshed (its pools, each with lo and hi): the lines naming the pools that came outside the windows, the
    /// first kNamed of all that came
    template <class Pools>
    std::vector<std::string> refresh(const Pools& table) {
        std::vector<std::string> lines;
        std::vector<std::pair<uint64_t, uint64_t>> now;
        uint64_t now_bytes = 0;
        pools = 0;
        for (const auto& p : table) {
            pools++;
            const uint64_t out = page_table_outside(p.lo, p.hi);
            if (!out) continue;
            const std::pair<uint64_t, uint64_t> r{p.lo, p.hi};
            now.push_back(r);
            now_bytes += out;
            if (std::binary_search(outside.begin(), outside.end(), r)) continue;
            came++, came_bytes += out;
            lowest = std::min(lowest, p.lo), highest = std::max(highest, p.hi);
            if (named >= kNamed) continue;
            named++;
            const bool partly = out < p.hi - p.lo;
            char b[288];
            std::snprintf(b, sizeof b, "vk: page table: pool %#llx..%#llx (%s) is %soutside its windows%s%s%s: a pointer into %s that no "
                          "bound buffer holds reads 0", (unsigned long long)p.lo, (unsigned long long)p.hi,
                          page_census_size(p.hi - p.lo).c_str(), partly ? "partly " : "", partly ? " (" : "",
                          partly ? page_census_size(out).c_str() : "", partly ? " of it)" : "", partly ? "those bytes" : "it");
            lines.push_back(b);
        }
        std::sort(now.begin(), now.end());
        if (now.size() > most || (now.size() == most && now_bytes > most_bytes)) most = now.size(), most_bytes = now_bytes;
        outside.swap(now);
        return lines;
    }

    /// The census in a line (when: "close")
    std::string summary(const char* when) const {
        char b[512];
        if (!came) {
            std::snprintf(b, sizeof b, "vk: page table census (%s): no pool outside its windows, %s (%llu pools in the table now)", when,
                          page_census_windows().c_str(), (unsigned long long)pools);
            return b;
        }
        char named_text[48];
        if (came > (uint64_t)named) std::snprintf(named_text, sizeof named_text, "the first %d named above", named);
        else std::snprintf(named_text, sizeof named_text, "named above");
        std::snprintf(b, sizeof b, "vk: page table census (%s): %llu pool%s outside its windows, %s, came into the pool table (%s), %s "
                      "of %s outside; at most %llu at once (%s), %zu now of %llu pools; between %#llx and %#llx", when,
                      (unsigned long long)came, came == 1 ? "" : "s", page_census_windows().c_str(), named_text,
                      page_census_size(came_bytes).c_str(), came == 1 ? "it" : "them", (unsigned long long)most,
                      page_census_size(most_bytes).c_str(), outside.size(), (unsigned long long)pools, (unsigned long long)lowest,
                      (unsigned long long)highest);
        return b;
    }
};

} // namespace mkwvk
