// (arm64 port) The guest's memory pools placed in the GPU's own memory: the bookkeeping and the mappings, no Vulkan.
// gpu/vk/vk_memory.cpp's placed_* use it where the driver takes neither host pointers (VK_EXT_external_memory_host) nor
// dma-bufs of the guest window (no /dev/udmabuf): Turnip, Mesa's Adreno driver, on a kernel without udmabuf. A pool the game
// makes gets memory of the device's, exported as a dma-buf; its contents are copied in (copy_in) and the dma-buf is mapped
// over the pool's guest pages (place). When the game lets the pool go, its contents go back into ordinary memory at the same
// addresses (unplace). A pool of a size the renderer leaves in ordinary memory until the GPU binds it is placed then, while
// the game may be writing it: its writes are held back while its contents are copied (Barrier, place_live). Which of those
// pools a binding places: later_range. tests/test_placed_memory.py checks all of it on a memfd, which maps as a dma-buf does.
#pragma once
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <map>
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


/// The pools left in ordinary memory (`later`: by start, each with its end in .hi; they may overlap) that a binding of
/// [lo, hi) places: those it runs into, and every other one those run into, grown until they run into no other (`keys`:
/// their starts). Their whole range; {0, 0} when the binding runs into none of them
template <class Map>
Range later_range(const Map& later, uint64_t lo, uint64_t hi, std::vector<uint64_t>& keys) {
    uint64_t a = lo, b = hi;   // (what is looked for: the binding, then what was found)
    for (bool first = true;; first = false) {
        keys.clear();
        Range u{UINT64_MAX, 0};
        for (const auto& [at, l] : later)
            if (at < b && l.hi > a) keys.push_back(at), u.lo = std::min(u.lo, at), u.hi = std::max(u.hi, l.hi);
        if (keys.empty()) return {0, 0};
        if (!first && u.lo == a && u.hi == b) return u;
        a = u.lo, b = u.hi;
    }
}

/// The game's pools left in ordinary memory until the GPU binds them, by start (they may run into one another, never into a
/// placement: vk_memory.cpp places those a new pool runs into first). Each pool's own end is kept, so that letting go of one
/// takes off that one alone (another pool, never left here, at an address of theirs takes off none); hi is the largest, for
/// later_range. retry_ns: their placement was refused, not tried again before then
struct Later {
    struct Entry {
        uint64_t hi = 0;
        std::vector<uint64_t> ends;
        int64_t retry_ns = 0;
    };
    std::map<uint64_t, Entry> pools;

    void add(uint64_t lo, uint64_t hi) {
        Entry& e = pools[lo];
        e.ends.push_back(hi);
        e.hi = std::max(e.hi, hi);
    }
    /// The pool over exactly [lo, hi) let go: true when it was one of them (taken off)
    bool let_go(uint64_t lo, uint64_t hi) {
        const auto it = pools.find(lo);
        if (it == pools.end()) return false;
        std::vector<uint64_t>& ends = it->second.ends;
        const auto e = std::find(ends.begin(), ends.end(), hi);
        if (e == ends.end()) return false;
        ends.erase(e);
        if (ends.empty()) pools.erase(it);
        else it->second.hi = *std::max_element(ends.begin(), ends.end());
        return true;
    }
    /// Whether one of them runs into [lo, hi)
    bool overlaps(uint64_t lo, uint64_t hi) const {
        for (const auto& [at, e] : pools) {
            if (at >= hi) break;
            if (e.hi > lo) return true;
        }
        return false;
    }
    /// Their bytes (those over the same memory counted again: at most)
    uint64_t bytes() const {
        uint64_t n = 0;
        for (const auto& [at, e] : pools) n += e.hi - at;
        return n;
    }
    /// Those a binding of [lo, hi) places (later_range), taken out into `taken`: their range, and the game's pools over it in
    /// *n. {0, 0} with nothing taken: none there, or one of them waits for its retry after now_ns
    Range take(uint64_t lo, uint64_t hi, int64_t now_ns, int* n, std::vector<std::pair<uint64_t, Entry>>& taken) {
        taken.clear();
        *n = 0;
        std::vector<uint64_t> keys;
        const Range r = later_range(pools, lo, hi, keys);
        if (keys.empty()) return {0, 0};
        for (const uint64_t k : keys)
            if (pools.at(k).retry_ns > now_ns) return {0, 0};
        for (const uint64_t k : keys) {
            const auto it = pools.find(k);
            *n += (int)it->second.ends.size();
            taken.emplace_back(it->first, std::move(it->second));
            pools.erase(it);
        }
        return r;
    }
    /// Those taken, their placement refused: back, not tried again before retry_ns
    void put_back(std::vector<std::pair<uint64_t, Entry>>& taken, int64_t retry_ns) {
        for (auto& [lo, e] : taken) {
            e.retry_ns = retry_ns;
            Entry& here = pools[lo];   // (one made at the same start meanwhile: both kept)
            for (const uint64_t end : e.ends) here.ends.push_back(end);
            here.hi = std::max(here.hi, e.hi), here.retry_ns = std::max(here.retry_ns, retry_ns);
        }
        taken.clear();
    }
};

/// What a pool the game makes over [lo, hi) becomes, from the placements' plan for it and the pools left in ordinary memory
/// (later_min: the size from which a pool is left there; 0, none). None of those ever runs into a placement, so that placing
/// them later takes in no placement the GPU may be writing:
/// - inside: in a placement already, counted there;
/// - later: left in ordinary memory, it runs into no placement and is later_min or more, or runs into some left there (with
///   them: placed together at a binding of any of them);
/// - wake_first: it runs into a placement and into some left in ordinary memory: those placed first, then this one with it;
/// - place: placed now
enum class Made { inside, later, wake_first, place };
inline Made made(const Plan& plan, const Later& later, uint64_t lo, uint64_t hi, uint64_t later_min) {
    if (plan.inside) return Made::inside;
    if (!later_min) return Made::place;
    const bool into_later = later.overlaps(lo, hi);
    if (plan.absorbed.empty() && (hi - lo >= later_min || into_later)) return Made::later;
    return into_later ? Made::wake_first : Made::place;
}

/// Writes held back while a range the game may be writing is placed (place_live): the range is published here, then made
/// read-only; a write into it faults, and the fault handler (vk_memory.cpp's, first among the host's vectored handlers) asks
/// held(), which waits until the device's memory is mapped there and says to run the write again, into it. A range stays
/// known for kRecentNs after its end, for a thread that faulted before the end and is handled after it. A fault of the
/// placing thread itself is not held (it writes no guest memory there: a fault of its own, a read gone wrong, would wait for
/// itself). One range is placed at a time (placed_mu); a few are kept. held() is async-signal-safe: atomics, the gettid and
/// clock_gettime system calls, nanosleep
struct Barrier {
    static constexpr int kSlots = 8;
    static constexpr int64_t kRecentNs = 2000000000;
    struct Slot {
        std::atomic<uintptr_t> lo{0}, hi{0};   // (hi 0: none; set last)
        std::atomic<bool> busy{false};         // (being placed: its writers wait)
        std::atomic<int64_t> done_ns{0};
        std::atomic<long> placer{0};           // (the placing thread's id)
    };
    Slot slots[kSlots];
    std::atomic<unsigned> next{0};
    std::atomic<uint64_t> waits{0};   // (the faults held: writes that waited, or ran again after a placement's end)

    static int64_t now_ns() {
        timespec t{};
        clock_gettime(CLOCK_MONOTONIC, &t);
        return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
    }
    /// [lo, hi) (host addresses) about to be made read-only: its slot, for end()
    int begin(uintptr_t lo, uintptr_t hi) {
        const int i = (int)(next.fetch_add(1, std::memory_order_relaxed) % kSlots);
        Slot& s = slots[i];
        s.hi.store(0, std::memory_order_release);
        s.placer.store((long)syscall(SYS_gettid), std::memory_order_relaxed);
        s.busy.store(true, std::memory_order_release);
        s.lo.store(lo, std::memory_order_relaxed);
        s.hi.store(hi, std::memory_order_release);
        return i;
    }
    /// The range writable again (the device's memory mapped there, or the old pages given their access back): its writers go on
    void end(int i) {
        Slot& s = slots[i];
        s.done_ns.store(now_ns(), std::memory_order_relaxed);
        s.busy.store(false, std::memory_order_release);
    }
    static bool in(const Slot& s, uintptr_t addr) {
        const uintptr_t hi = s.hi.load(std::memory_order_acquire);
        return hi && addr < hi && addr >= s.lo.load(std::memory_order_relaxed);
    }
    /// (a fault handler's) A fault at addr: true when it is in a range being placed, after waiting for its end, or in one placed
    /// in the last kRecentNs (the access is to run again); false: not one of them, or the placing thread's own
    bool held(uintptr_t addr) {
        for (Slot& s : slots) {   // (a range being placed first: one placed before over the same pages would only spin)
            if (!in(s, addr) || !s.busy.load(std::memory_order_acquire)) continue;
            if (s.placer.load(std::memory_order_relaxed) == (long)syscall(SYS_gettid)) return false;
            while (s.busy.load(std::memory_order_acquire)) {
                const timespec t{0, 20000};   // (20 us)
                nanosleep(&t, nullptr);
            }
            waits.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        for (Slot& s : slots)
            if (in(s, addr) && now_ns() - s.done_ns.load(std::memory_order_relaxed) < kRecentNs) {
                waits.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        return false;
    }
};

/// [at, at + n) held: published in b, then made read-only, so a write into it waits in b.held until b.end(slot). The slot;
/// -1 when it cannot be made read-only (errno says why; what of it was made read-only before the refusal, a hole in the
/// range or the kernel's mapping count, made writable again; nothing held then)
inline int hold(Barrier& b, uint8_t* at, uint64_t n) {
    const int slot = b.begin((uintptr_t)at, (uintptr_t)at + n);
    if (mprotect(at, n, PROT_READ)) {
        const int e = errno;
        mprotect(at, n, PROT_READ | PROT_WRITE);
        b.end(slot);
        errno = e;
        return -1;
    }
    return slot;
}

/// [at, at + n) placed while other threads may write it: held (their writes wait in b.held), its contents copied into fresh
/// (copy_in: `olds` as there), fd's memory mapped over it, and b's range ended. False: refused, errno in *err: the read-only
/// step (nothing changed) or the mapping (the pages there may be gone, as with place). `copied`: the bytes copied
inline bool place_live(Barrier& b, uint8_t* at, uint64_t n, int fd, uint8_t* fresh, uint64_t page, int pagemap,
                       const std::vector<Range>& olds, uint64_t* copied, int* err) {
    const int slot = hold(b, at, n);
    if (slot < 0) {
        if (err) *err = errno;
        return false;
    }
    const uint64_t bytes = copy_in(fresh, at, n, page, pagemap, olds);
    const bool ok = place(at, n, fd);
    if (!ok) {
        if (err) *err = errno;
        mprotect(at, n, PROT_READ | PROT_WRITE);   // (what of the old pages is left: writable again)
    }
    b.end(slot);
    if (copied) *copied = bytes;
    return ok;
}

} // namespace placed
