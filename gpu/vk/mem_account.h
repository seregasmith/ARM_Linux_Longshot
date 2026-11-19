// The renderer's memory, counted in one place: every pool that holds VRAM or system memory says how much it holds, and one line
// sums them beside what the driver counts ("vk: budget:", every 600 presents, vk_texture.cpp texture_frame). Each pool had a figure
// and a log line of its own, at its own time, and nothing added them up: what the driver counted past the sum of the pools (its own
// objects, pipelines, an upscaler's library) was a number nobody had.
//
// The figures are counted only: no eviction, limit or allocation reads a Report. A pool is a name and a function that fills one.
//
// And one place asks the pools for memory back (the arbiter, below): near what the driver allows, each pool that keeps memory
// only to use it again is asked, in a stated order, before the texture cache evicts textures the game is using. Only the texture
// cache answered every such signal; the target pool had a rule of its own for the same line, the staging arenas and the capture
// spares answered Linux's memory pressure and nothing of the card's. The signal and its lines are the texture cache's own
// (vk_texture.cpp texture_frame); what a pool does when asked is the trim it already had. Below the line nobody is asked and
// nothing here runs but two comparisons.
//
// No Vulkan here, and no lock of its own: a report function reads an atomic its pool published, a sample its own thread took, or
// a short list under that list's leaf lock (vk_memory.cpp). tests/test_mem_account.py
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mkwvk::mem_account {

/// Where bytes are charged. VRAM: the device-local heaps, what the driver's count and allowance are about. RAM: system memory and
/// commit. A pool can hold bytes in both (the staging arenas are VRAM behind a resizable BAR, else system memory)
enum Ledger : int { VRAM = 0, RAM = 1 };
inline constexpr int kLedgers = 2;

/// What a pool holds now, in bytes by ledger. `held` is all of it; `reuse` and `pending` are the parts of `held` that hold nothing
/// in use: kept for the next use (spares, a pool of dead images, free space in blocks), and already given back but not freed until
/// the GPU is past them. A pool that does not know a part leaves it 0
struct Report {
    uint64_t held[kLedgers]{};
    uint64_t reuse[kLedgers]{};
    uint64_t pending[kLedgers]{};
};

/// What the arbiter asks of a pool: up to `want` bytes of `ledger`, at `level` 1 (near the allowance) or 2 (at the driver's own
/// line). `entered`: the level has just risen past the highest since memory was last calm; a pool whose memory costs a stall to
/// make again (a staging arena, a capture's spare buffer) gives it then and not at every look after, or it would be made and
/// freed in turn for as long as the card stays full
struct Ask {
    Ledger ledger = VRAM;
    uint64_t want = 0;
    int level = 0;
    bool entered = false;
};

struct Pool {
    const char* name;           // (a string literal, short: it is printed and parsed, scripts/memtour.py)
    void (*report)(Report&);    // (callable from the thread that logs, and never waiting long there)
    /// Where the pool stands when memory is asked back: 1 first; 0: never asked. The cheapest to lose first
    int order = 0;
    /// Gives back what it keeps for reuse, up to a.want, by the trim it already has, and returns the bytes it let go of now
    /// (0 for what only goes later). Called on the thread that owns texture_frame, at its eviction pass, only at level 1 or 2
    uint64_t (*give_back)(const Ask& a) = nullptr;
};

inline constexpr size_t kMaxPools = 48;

struct Registry {
    Pool pools[kMaxPools]{};
    size_t n = 0;
    /// False, and nothing added, for a pool with no name or function, a name already there, or a full registry
    bool add(const Pool& p) {
        if (!p.name || !p.report || n == kMaxPools) return false;
        for (size_t i = 0; i < n; i++)
            if (!std::strcmp(pools[i].name, p.name)) return false;
        pools[n++] = p;
        return true;
    }
};

/// The renderer's own registry: filled while the library's statics are made (one thread), read after
inline Registry& registry() {
    static Registry r;
    return r;
}
inline bool add(const Pool& p) { return registry().add(p); }

/// Every pool asked once, and the sums by ledger
struct Totals {
    const char* name[kMaxPools]{};
    Report pool[kMaxPools]{};
    size_t n = 0;
    Report sum{};
};
inline Totals collect(const Registry& r) {
    Totals t;
    for (size_t i = 0; i < r.n; i++) {
        t.name[t.n] = r.pools[i].name;
        r.pools[i].report(t.pool[t.n]);
        for (int l = 0; l < kLedgers; l++) {
            t.sum.held[l] += t.pool[t.n].held[l];
            t.sum.reuse[l] += t.pool[t.n].reuse[l];
            t.sum.pending[l] += t.pool[t.n].pending[l];
        }
        t.n++;
    }
    return t;
}

// ---- the arbiter ----

/// The lines, as the texture cache's squeeze and the target pool's rule read them before this (vk_texture.cpp texture_frame):
/// near the allowance is `pressure` past 90% of it, where pressure is the driver's count less the free space the textures can
/// still take (the budget's "90% rule", and the pool's "halve"); at the driver's line is the count itself past 97% (the pool's
/// "flush": past there the driver pages VRAM out). 0 with no allowance known
inline constexpr uint64_t kNearTenths = 9, kLinePct = 97;
inline uint64_t near_line(uint64_t allowed) { return allowed / 10 * kNearTenths; }
inline uint64_t count_line(uint64_t allowed) { return allowed / 100 * kLinePct; }
inline int level(uint64_t pressure, uint64_t used, uint64_t allowed) {
    if (!allowed) return 0;
    return used > count_line(allowed) ? 2 : pressure > near_line(allowed) ? 1 : 0;
}
/// What is asked back: the excess over the line that was crossed (the larger, at level 2)
inline uint64_t excess(uint64_t pressure, uint64_t used, uint64_t allowed) {
    if (!allowed) return 0;
    const uint64_t over = pressure > near_line(allowed) ? pressure - near_line(allowed) : 0;   // (not "near": a macro on Windows)
    return std::max(over, used > count_line(allowed) ? used - count_line(allowed) : 0);
}

/// One look at the lines, every eviction pass (10 presents). Memory is calm again after kCalmLooks looks in a row at level 0
/// (600 presents: the time the compaction rests between runs when nothing presses, vram_compact::rest), and until then a
/// level reached before is not entered anew: a count that sways across a line asks the stall-costing pools once
struct Arbiter {
    static constexpr int kCalmLooks = 60;
    int peak = 0, calm = 0;
    Ask look(uint64_t pressure, uint64_t used, uint64_t allowed) {
        Ask a;
        a.level = level(pressure, used, allowed);
        a.want = excess(pressure, used, allowed);
        if (!a.level) {
            if (peak && ++calm >= kCalmLooks) peak = calm = 0;
            return a;
        }
        calm = 0;
        a.entered = a.level > peak;
        peak = std::max(peak, a.level);
        return a;
    }
};

/// What one pool gave
struct Given {
    const char* name;
    uint64_t bytes;
};
/// The pools asked, in their order (equal ones as registered), each for what the ones before left of a.want, until it is
/// covered; nobody at level 0. The pools that gave something go to out (at most cap of them); returns the bytes given in all
inline uint64_t ask(const Registry& r, const Ask& a, Given* out, size_t cap, size_t* count) {
    if (count) *count = 0;
    if (a.level <= 0 || !a.want) return 0;
    size_t by_order[kMaxPools];
    size_t n = 0;
    for (size_t i = 0; i < r.n; i++)
        if (r.pools[i].order > 0 && r.pools[i].give_back) by_order[n++] = i;
    std::stable_sort(by_order, by_order + n, [&](size_t x, size_t y) { return r.pools[x].order < r.pools[y].order; });
    uint64_t given = 0;
    for (size_t i = 0; i < n && given < a.want; i++) {
        Ask left = a;
        left.want = a.want - given;
        const uint64_t g = r.pools[by_order[i]].give_back(left);
        if (!g) continue;
        given += g;
        if (out && count && *count < cap) out[(*count)++] = {r.pools[by_order[i]].name, g};
    }
    return given;
}
/// "vk: budget: gave back <MB> MB from <pool> at <P>% of the allowance": P is the driver's count against it
inline size_t format_given(char* out, size_t cap, const Given& g, uint64_t used, uint64_t allowed) {
    if (!cap) return 0;
    const int w = std::snprintf(out, cap, "vk: budget: gave back %llu MB from %s at %llu%% of the allowance",
                                (unsigned long long)(g.bytes >> 20), g.name, (unsigned long long)(allowed ? used * 100 / allowed : 0));
    return w < 0 ? 0 : std::min((size_t)w, cap - 1);
}

/// What the driver counts less what the pools hold: negative when the pools count more than the driver does
inline int64_t unaccounted(uint64_t driver_used, uint64_t held) { return (int64_t)driver_used - (int64_t)held; }

/// Whole MB, towards zero
inline long long mb(int64_t bytes) { return (long long)(bytes / (int64_t)(1ull << 20)); }

/// One ledger's pools into `out`: "<pool> <MB>, <pool> <MB>, ..., others <MB>". The largest first (equal ones in the order they
/// were registered); the pools under 1 MB are summed as "others", last, and left out when they hold nothing; "none" when no pool
/// holds anything. Returns the characters written (cut short at cap - 1)
inline size_t format_pools(char* out, size_t cap, const Totals& t, Ledger l) {
    size_t order[kMaxPools];
    size_t shown = 0;
    uint64_t others = 0;
    for (size_t i = 0; i < t.n; i++) {
        if (t.pool[i].held[l] < (1ull << 20)) others += t.pool[i].held[l];
        else order[shown++] = i;
    }
    std::stable_sort(order, order + shown, [&](size_t a, size_t b) { return t.pool[a].held[l] > t.pool[b].held[l]; });
    size_t at = 0;
    const auto put = [&](const char* name, uint64_t bytes) {
        if (at >= cap) return;
        const int w = std::snprintf(out + at, cap - at, "%s%s %llu", at ? ", " : "", name, (unsigned long long)(bytes >> 20));
        at = w < 0 ? at : std::min(at + (size_t)w, cap - 1);
    };
    for (size_t i = 0; i < shown; i++) put(t.name[order[i]], t.pool[order[i]].held[l]);
    if (others) put("others", others);
    if (!at && cap) at = std::min((size_t)std::max(0, std::snprintf(out, cap, "none")), cap - 1);
    return at;
}

/// The line: "vk: budget: VRAM <sum> of <allowed> MB (<used> MB by the driver, <unaccounted> MB unaccounted): <pools>; RAM <sum> MB
/// (the process's private memory <private> MB): <pools>". vram_allowed and vram_used are the driver's allowance and count.
/// driver_counts: that count is the driver's (VK_EXT_memory_budget); without it vram_used is only the allocator's own
/// allocations, which leave out what some pools hold, so the line says "unaccounted n/a" for a difference that means nothing.
/// ram_private is the process's private memory, beside the sum and not a whole it is part of: guest memory the driver has pinned
/// (the imports) is shared memory on Linux, outside that figure. Returns the characters written (cut short at cap - 1)
inline size_t format(char* out, size_t cap, const Totals& t, uint64_t vram_allowed, uint64_t vram_used, bool driver_counts,
                     uint64_t ram_private) {
    if (!cap) return 0;
    char vram[400], ram[400];   // (the log's lines are 1024 characters: the two lists and the figures before them fit one)
    format_pools(vram, sizeof vram, t, VRAM);
    format_pools(ram, sizeof ram, t, RAM);
    char left[48] = "unaccounted n/a";
    if (driver_counts) std::snprintf(left, sizeof left, "%lld MB unaccounted", mb(unaccounted(vram_used, t.sum.held[VRAM])));
    const int w = std::snprintf(out, cap, "vk: budget: VRAM %llu of %llu MB (%llu MB by the driver, %s): %s; RAM %llu MB (the process's "
                                "private memory %llu MB): %s", (unsigned long long)(t.sum.held[VRAM] >> 20),
                                (unsigned long long)(vram_allowed >> 20), (unsigned long long)(vram_used >> 20), left, vram,
                                (unsigned long long)(t.sum.held[RAM] >> 20), (unsigned long long)(ram_private >> 20), ram);
    return w < 0 ? 0 : std::min((size_t)w, cap - 1);
}

}  // namespace mkwvk::mem_account
