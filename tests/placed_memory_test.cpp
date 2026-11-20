// placed (gpu/vk/placed_memory.h): the guest's pools placed in memory of the device's, as gpu/vk/vk_memory.cpp does where the
// driver takes neither host pointers nor dma-bufs of the guest window (Turnip on a kernel without /dev/udmabuf). A memfd
// stands in for the device's dma-buf (both are mapped shared, by fd, and the device's view is a second mapping of the same
// memory); the region stands in for the guest window, made as host/hos_main.c makes it there. Linux; no game, no GPU.
#include "../gpu/vk/placed_memory.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <thread>
#include <vector>

namespace {

int failures = 0;
void expect(bool ok, const char* what) {
    if (!ok) std::printf("FAIL: %s\n", what), failures++;
}

struct Pl {
    uint64_t hi;
};

void plans() {
    const std::map<uint64_t, Pl> none;
    placed::Plan p = placed::plan(none, 0x1000, 0x3000);
    expect(!p.inside && p.range.lo == 0x1000 && p.range.hi == 0x3000 && p.absorbed.empty(),
           "a pool where nothing is placed: a placement of its own, its range");
    const std::map<uint64_t, Pl> m{{0x10000, {0x20000}}, {0x30000, {0x40000}}};
    p = placed::plan(m, 0x10000, 0x20000);
    expect(p.inside && p.inside_lo == 0x10000 && p.absorbed.empty(), "a second pool over the same memory: that placement, shared");
    p = placed::plan(m, 0x14000, 0x18000);
    expect(p.inside && p.inside_lo == 0x10000 && p.absorbed.empty(), "a pool inside another's memory: that placement, shared");
    p = placed::plan(m, 0x20000, 0x30000);
    expect(!p.inside && p.range.lo == 0x20000 && p.range.hi == 0x30000 && p.absorbed.empty(),
           "a pool between two placements, touching both: a placement of its own (touching is not overlapping)");
    p = placed::plan(m, 0x18000, 0x34000);
    expect(!p.inside && p.range.lo == 0x10000 && p.range.hi == 0x40000 && p.absorbed.size() == 2,
           "a pool across two placements: one placement over all three, replacing both");
    p = placed::plan(m, 0x8000, 0x12000);
    expect(!p.inside && p.range.lo == 0x8000 && p.range.hi == 0x20000 && p.absorbed.size() == 1 && p.absorbed[0] == 0x10000,
           "a pool across a placement's start: one over both");
    p = placed::plan(m, 0x0, 0x50000);
    expect(!p.inside && p.range.lo == 0 && p.range.hi == 0x50000 && p.absorbed.size() == 2,
           "a pool over two placements: one over the pool, replacing both");
}

int memfd(uint64_t n) {
    const int fd = (int)syscall(SYS_memfd_create, "placed-test", 1u /* MFD_CLOEXEC */);
    if (fd >= 0 && ftruncate(fd, (off_t)n) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void mappings() {
    const uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE), n = 16 * page, n2 = 20 * page;
    // (the guest window where /dev/udmabuf is not: private, anonymous, no swap reserved, no huge pages, host/hos_main.c)
    uint8_t* const window = (uint8_t*)mmap(nullptr, 3 * n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (window == MAP_FAILED) return expect(false, "no window to try it in");
    madvise(window, 3 * n, MADV_NOHUGEPAGE);
    uint8_t* const pool = window + n;   // (a pool in the middle; nothing before it touched)
    std::vector<uint8_t> want(n2, 0);   // (what the pool holds: written here, never read from it before it is placed, as that
                                        // would map the zero page under every page and make them all "in memory")
    const auto write = [&](uint8_t* base, uint64_t at, uint8_t v, uint64_t len) {
        std::memset(base + at, v, len);
        std::memset(want.data() + at, v, len);
    };
    for (const int k : {1, 2, 5}) write(pool, k * page, (uint8_t)(0x10 + k), page);
    write(pool, 9 * page, 0, page);   // (written with zeros: in memory all the same)
    std::memset(pool + 3 * page, 0x33, page);
    madvise(pool + 3 * page, page, MADV_DONTNEED);   // (given back by the game: reads as zero)
    write(pool, 15 * page - 1, 0x7F, 1);              // (the last byte of page 14)
    const int pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);

    // ---- placed: the contents copied in, then the device's memory over the pool ----
    const int fd = memfd(n);
    uint8_t* const device = fd >= 0 ? placed::map(fd, n) : nullptr;   // (the device's view of its memory)
    if (!device) return expect(false, "no memfd to stand in for the device's memory");
    const uint64_t copied = placed::copy_in(device, pool, n, page, pagemap, {});
    expect(std::memcmp(device, want.data(), n) == 0, "copied in: the pool's contents, every page of them");
    expect(copied == (pagemap >= 0 ? 5 * page : n),
           "only the pages in memory copied in (1, 2, 5, 9 and 14): not those never touched or given back");
    expect(placed::place(pool, n, fd), "the device's memory placed over the pool");
    close(fd);   // (the mapping keeps it)
    // (all but page 13, which the game does not touch here: the GPU writes it below)
    expect(std::memcmp(pool, want.data(), 13 * page) == 0 && std::memcmp(pool + 14 * page, want.data() + 14 * page, 2 * page) == 0,
           "the pool reads as it did, through its placement");
    write(pool, 7 * page + 3, 0x77, 1);
    expect(device[7 * page + 3] == 0x77, "the game's writes are in the device's memory, nothing copied");
    device[13 * page + 5] = 0x55, want[13 * page + 5] = 0x55;   // (a GPU write: page 13 never touched through the pool)
    expect(pool[11 * page] == 0 && device[11 * page] == 0, "an untouched page reads as zero on both sides");
    expect(window[n - 1] == 0 && window[2 * n] == 0, "the pages around the pool left as they were");

    // ---- a pool across its end: one placement over both, the first one's pages all copied ----
    write(pool, n + page, 0x44, page);   // (ordinary memory past the first pool's end, in the second: page 17 from the first's start)
    const int fd2 = memfd(n2);
    uint8_t* const device2 = fd2 >= 0 ? placed::map(fd2, n2) : nullptr;
    if (!device2) return expect(false, "no second memfd");
    // (the game's mapping of page 13 dropped, as the CPU never reached it: the kernel maps pages in the page cache around a
    // fault on its own (fault-around), which a dma-buf's pages written by the GPU need not be. Its contents stay)
    madvise(pool + 13 * page, page, MADV_DONTNEED);
    placed::copy_in(device2, pool, n2, page, pagemap, {{0, n}});
    expect(device2[13 * page + 5] == 0x55,
           "a page the GPU wrote in the old placement and the game never touched: copied all the same");
    expect(std::memcmp(device2, want.data(), n2) == 0, "both pools' contents in the one placement");
    expect(placed::place(pool, n2, fd2), "the one placement over both");
    close(fd2);
    pool[0] = 0x99, want[0] = 0x99;
    expect(device[0] == 0 && device2[0] == 0x99, "the old placement no longer under the pool, the new one is");

    // ---- given back: ordinary memory again, the contents kept ----
    uint64_t kept = 0;
    expect(placed::unplace(pool, n2, page, &kept), "the placement given back");
    expect(std::memcmp(pool, want.data(), n2) == 0, "the pool reads as it did, in ordinary memory");
    uint64_t nonzero = 0;
    for (uint64_t off = 0; off < n2; off += page) nonzero += !placed::zero_page(want.data() + off, page) ? page : 0;
    expect(kept == nonzero, "only the pages holding anything copied out: the zero pages left untouched");
    pool[2 * page] = 0x22;
    expect(device2[2 * page] == 0x12, "the device's memory no longer under the pool");
    expect(madvise(pool + page, page, MADV_DONTNEED) == 0 && pool[page] == 0,
           "a page given back reads as zero again, as ordinary memory's do (a placed page would not)");
    munmap(device, n);
    munmap(device2, n2);
    munmap(window, 3 * n);
    if (pagemap >= 0) close(pagemap);
}


// ---- the pools left in ordinary memory a binding places (later_range) ----
void later() {
    std::vector<uint64_t> keys;
    const std::map<uint64_t, Pl> none;
    placed::Range r = placed::later_range(none, 0x1000, 0x2000, keys);
    expect(r.lo == 0 && r.hi == 0 && keys.empty(), "no pool left in ordinary memory: none placed");
    // (0x10000..0x20000 alone; 0x30000..0x50000 overlapped by 0x48000..0x60000, which 0x5f000..0x70000 overlaps in turn)
    const std::map<uint64_t, Pl> m{{0x10000, {0x20000}}, {0x30000, {0x50000}}, {0x48000, {0x60000}}, {0x5f000, {0x70000}}};
    r = placed::later_range(m, 0x14000, 0x14100, keys);
    expect(r.lo == 0x10000 && r.hi == 0x20000 && keys == std::vector<uint64_t>{0x10000},
           "a binding inside one pool: that pool, whole");
    r = placed::later_range(m, 0x20000, 0x30000, keys);
    expect(r.lo == 0 && r.hi == 0 && keys.empty(), "a binding between two, touching both: none (touching is not overlapping)");
    r = placed::later_range(m, 0x31000, 0x31010, keys);
    expect(r.lo == 0x30000 && r.hi == 0x70000 && keys == (std::vector<uint64_t>{0x30000, 0x48000, 0x5f000}),
           "a binding in a pool another runs into: all three, the chain followed to its end");
    r = placed::later_range(m, 0x6f000, 0x6f100, keys);
    expect(r.lo == 0x30000 && r.hi == 0x70000 && keys.size() == 3, "from the chain's other end: the same three");
    r = placed::later_range(m, 0x1f000, 0x31000, keys);
    expect(r.lo == 0x10000 && r.hi == 0x70000 && keys.size() == 4, "a binding across two of them: both, and what they run into");
}


// ---- the pools left in ordinary memory, one by one (Later) ----
void later_pools() {
    placed::Later l;
    l.add(0x100000, 0x200000);
    l.add(0x100000, 0x180000);   // (a second pool at the same start, smaller)
    l.add(0x400000, 0x500000);
    expect(l.overlaps(0x1ff000, 0x201000) && !l.overlaps(0x200000, 0x400000) && l.overlaps(0x0, 0x100001),
           "runs into them: overlapping, not touching");
    expect(!l.let_go(0x100000, 0x190000) && !l.let_go(0x140000, 0x180000) && !l.let_go(0x300000, 0x310000),
           "a pool that was never left here lets go of none of them (a refused placement's pool, say)");
    expect(l.pools.at(0x100000).ends.size() == 2, "both pools at that start kept");
    expect(l.let_go(0x100000, 0x200000) && l.pools.at(0x100000).hi == 0x180000,
           "the larger one let go: the smaller one left, with its own end");
    expect(l.let_go(0x100000, 0x180000) && !l.pools.count(0x100000), "the smaller one too: gone");
    l.add(0x100000, 0x180000);
    l.add(0x170000, 0x300000);   // (runs into the first)
    int n = 0;
    std::vector<std::pair<uint64_t, placed::Later::Entry>> taken;
    placed::Range r = l.take(0x101000, 0x102000, 1000, &n, taken);
    expect(r.lo == 0x100000 && r.hi == 0x300000 && n == 2 && taken.size() == 2 && l.pools.size() == 1,
           "a binding takes out the pool it is in and the one that runs into it, with their two pools");
    l.put_back(taken, 5000);
    expect(l.pools.size() == 3 && l.pools.at(0x100000).retry_ns == 5000 && taken.empty(), "refused: back, with a retry time");
    r = l.take(0x101000, 0x102000, 4000, &n, taken);
    expect(r.hi == 0 && taken.empty() && l.pools.size() == 3, "not taken again before the retry time");
    r = l.take(0x101000, 0x102000, 6000, &n, taken);
    expect(r.lo == 0x100000 && r.hi == 0x300000 && taken.size() == 2, "taken after it");
    r = l.take(0x600000, 0x601000, 6000, &n, taken);
    expect(r.hi == 0 && taken.empty(), "a binding in none of them: none taken");
}


// ---- what a pool made becomes (made), and the rule it keeps: none left in ordinary memory runs into a placement ----
struct Pc {
    uint64_t hi;
    int pools;
};
void made_table() {
    const std::map<uint64_t, Pc> pl{{100, {120, 1}}};
    placed::Later l;
    l.add(200, 300);
    auto m = [&](uint64_t lo, uint64_t hi, uint64_t min) { return placed::made(placed::plan(pl, lo, hi), l, lo, hi, min); };
    expect(m(105, 110, 32) == placed::Made::inside, "inside a placement: counted there");
    expect(m(10, 60, 32) == placed::Made::later, "big, running into nothing: left in ordinary memory");
    expect(m(10, 20, 32) == placed::Made::place, "small, running into nothing: placed");
    expect(m(210, 220, 32) == placed::Made::later, "small, inside one left in ordinary memory: left with it (no hitch, no placement)");
    expect(m(290, 310, 32) == placed::Made::later, "across the end of one left there: left with it");
    expect(m(110, 140, 32) == placed::Made::place, "big, across a placement's end: placed with it, as before");
    expect(m(110, 210, 32) == placed::Made::wake_first, "across a placement and one left there: that one placed first");
    expect(m(10, 60, 0) == placed::Made::place && m(210, 220, 0) == placed::Made::place, "none left there (0): placed, as before");
}

void made_model() {
    const uint64_t later_min = 32, space = 256;
    int64_t worst = 0;
    for (unsigned seed = 1; seed <= 40; seed++) {
        uint64_t rng = seed * 0x9E3779B97F4A7C15ull;
        auto next = [&](uint64_t n) { rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17; return rng % n; };
        std::map<uint64_t, Pc> pl;
        placed::Later later;
        std::vector<std::pair<uint64_t, uint64_t>> refused, live;
        int64_t now = 0;
        auto make = [&](uint64_t lo, uint64_t hi, int pools) {   // (vk_memory.cpp placed_make: never refused here)
            const placed::Plan plan = placed::plan(pl, lo, hi);
            if (plan.inside) return void(pl[plan.inside_lo].pools += pools);
            for (const uint64_t a : plan.absorbed) pools += pl.at(a).pools, pl.erase(a);
            pl[plan.range.lo] = Pc{plan.range.hi, pools};
        };
        auto wake = [&](uint64_t lo, uint64_t hi) {   // (placed_wake; one in four refused)
            int n = 0;
            std::vector<std::pair<uint64_t, placed::Later::Entry>> taken;
            const placed::Range r = later.take(lo, hi, now, &n, taken);
            if (taken.empty()) return false;
            if (next(4) == 0) return later.put_back(taken, now + 30), false;
            const placed::Plan plan = placed::plan(pl, r.lo, r.hi);
            if (!plan.absorbed.empty() || plan.inside) worst = std::max<int64_t>(worst, 1);   // (took a placement in)
            make(r.lo, r.hi, n);
            return true;
        };
        auto back = [&](uint64_t lo, uint64_t hi) {   // (placed_back)
            switch (placed::made(placed::plan(pl, lo, hi), later, lo, hi, later_min)) {
            case placed::Made::inside: return make(lo, hi, 1);
            case placed::Made::later: return later.add(lo, hi);
            case placed::Made::wake_first:
                if (!wake(lo, hi)) return void(refused.emplace_back(lo, hi));
                break;
            case placed::Made::place: break;
            }
            make(lo, hi, 1);
        };
        auto unback = [&](uint64_t lo, uint64_t hi) {   // (placed_unback)
            for (auto r = refused.begin(); r != refused.end(); ++r)
                if (r->first == lo && r->second == hi) return void(refused.erase(r));
            auto it = pl.upper_bound(lo);
            if (it == pl.begin() || std::prev(it)->second.hi < hi) return void(later.let_go(lo, hi));
            if (--std::prev(it)->second.pools == 0) pl.erase(std::prev(it));
        };
        for (int op = 0; op < 3000; op++, now++) {
            const uint64_t k = next(10);
            if (k < 4 || live.empty()) {   // (a pool made: small mostly, a big one in three)
                const uint64_t lo = next(space - 16), n = next(3) ? 1 + next(15) : later_min + next(64);
                const uint64_t hi = std::min(space, lo + n);
                live.emplace_back(lo, hi);
                back(lo, hi);
            } else if (k < 7) {            // (one let go)
                const size_t i = next(live.size());
                unback(live[i].first, live[i].second);
                live.erase(live.begin() + (ptrdiff_t)i);
            } else {                       // (a GPU binding in one: placed_find)
                const auto& [lo, hi] = live[next(live.size())];
                const uint64_t a = lo + next(hi - lo);
                auto it = pl.upper_bound(a);
                if (it == pl.begin() || std::prev(it)->second.hi <= a) wake(a, a + 1);
            }
            // (the rule: none left in ordinary memory runs into a placement)
            for (const auto& [at, e] : later.pools) {
                const placed::Plan plan = placed::plan(pl, at, e.hi);
                if (plan.inside || !plan.absorbed.empty()) {
                    std::printf("  seed %u op %d: one left in ordinary memory at %llu..%llu runs into a placement\n", seed, op,
                                (unsigned long long)at, (unsigned long long)e.hi);
                    return expect(false, "none left in ordinary memory runs into a placement");
                }
            }
            // (every live pool counted once: in a placement, left in ordinary memory, or refused)
            int counted = (int)refused.size();
            for (const auto& [at, c] : pl) counted += c.pools;
            for (const auto& [at, e] : later.pools) counted += (int)e.ends.size();
            if (counted != (int)live.size()) {
                std::printf("  seed %u op %d: %d pools counted, %zu live\n", seed, op, counted, live.size());
                return expect(false, "every live pool counted once (none dropped, none counted twice)");
            }
            for (const auto& [lo, hi] : live) {
                auto it = pl.upper_bound(lo);
                const bool placed = it != pl.begin() && std::prev(it)->second.hi >= hi;
                const auto e = later.pools.find(lo);
                const bool left = e != later.pools.end() &&
                                  std::find(e->second.ends.begin(), e->second.ends.end(), hi) != e->second.ends.end();
                if (!placed && !left && std::find(refused.begin(), refused.end(), std::make_pair(lo, hi)) == refused.end()) {
                    std::printf("  seed %u op %d: pool %llu..%llu nowhere\n", seed, op, (unsigned long long)lo, (unsigned long long)hi);
                    return expect(false, "every live pool placed, left in ordinary memory, or refused");
                }
            }
        }
    }
    expect(worst == 0, "a placement at a binding never takes a placement in");
}

// ---- what the barrier does not hold: other addresses, long ago, the placing thread itself; a range being placed first ----
void barrier_refusals() {
    placed::Barrier b;
    const uintptr_t lo = 0x10000000, hi = 0x10100000;
    expect(!b.held(lo), "nothing placed: no fault held (another fault goes on to the other handlers)");
    const int s = b.begin(lo, hi);
    expect(!b.held(lo + 5), "the placing thread's own fault is not held (it would wait for itself)");
    std::atomic<bool> back{false};
    std::thread other([&] { back.store(b.held(lo + 5)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    b.end(s);
    other.join();
    expect(back.load(), "another thread's fault in it: held until its end");
    expect(b.held(lo) && !b.held(hi) && !b.held(lo - 1), "just after its end: run again; next to it: not held");
    b.slots[s].done_ns.store(placed::Barrier::now_ns() - placed::Barrier::kRecentNs - 1);
    expect(!b.held(lo), "long after its end: not held (a fault there now is not this placement's)");
    // (placed again over the same pages within kRecentNs: a fault waits for the new placement, not runs again at once)
    const int s1 = b.begin(lo, hi);
    b.end(s1);
    const int s2 = b.begin(lo, hi);
    std::atomic<int64_t> waited_ns{0};
    std::thread third([&] {
        const int64_t t0 = placed::Barrier::now_ns();
        b.held(lo + 7);
        waited_ns.store(placed::Barrier::now_ns() - t0);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    b.end(s2);
    third.join();
    expect(waited_ns.load() >= 20000000, "a range being placed comes before one placed before over the same pages");
}

// ---- placed while threads write it (Barrier, place_live) ----
placed::Barrier barrier;
struct sigaction old_segv;
std::atomic<int> faults{0};   // (the writes that faulted: held_write waits for one)
void on_segv(int sig, siginfo_t* si, void* uc) {
    faults.fetch_add(1, std::memory_order_release);
    if (barrier.held((uintptr_t)si->si_addr)) return;   // (the write runs again, into the placement)
    sigaction(SIGSEGV, &old_segv, nullptr);              // (not ours: as before, and the fault comes again)
    (void)sig, (void)uc;
}

void live() {
    const uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE), n = 16ull << 20, slots = n / 8;
    uint8_t* const pool = (uint8_t*)mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (pool == MAP_FAILED) return expect(false, "no pool to try it in");
    madvise(pool, n, MADV_NOHUGEPAGE);
    struct sigaction sa{};
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old_segv);
    // (each writer fills its own slots in order, every one once, with its index + 1, paced to take kSpan: a write lost under
    // the placement stays lost, as nothing writes that slot again)
    constexpr int kWriters = 4;
    constexpr auto kSpan = std::chrono::milliseconds(120);
    std::atomic<uint64_t> written{0};
    std::vector<std::thread> writers;
    const auto t0 = std::chrono::steady_clock::now();
    for (int w = 0; w < kWriters; w++)
        writers.emplace_back([&, w] {
            volatile uint64_t* const v = (volatile uint64_t*)pool;
            for (uint64_t i = (uint64_t)w; i < slots; i += kWriters) {
                v[i] = i + 1;
                if (i % 4096 == (uint64_t)w) {
                    written.fetch_add(4096 / kWriters, std::memory_order_relaxed);
                    while (std::chrono::steady_clock::now() - t0 < kSpan * (double)i / (double)slots) {}
                }
            }
        });
    while (written.load(std::memory_order_relaxed) < slots / 4) std::this_thread::yield();   // (a quarter written: placed now)
    const int fd = memfd(n);
    uint8_t* const fresh = fd >= 0 ? placed::map(fd, n) : nullptr;
    if (!fresh) {
        for (auto& t : writers) t.join();
        return expect(false, "no memfd to stand in for the device's memory");
    }
    const int pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    uint64_t copied = 0;
    int err = 0;
    const uint64_t during = written.load(std::memory_order_relaxed);
    const bool ok = placed::place_live(barrier, pool, n, fd, fresh, page, pagemap, {}, &copied, &err);
    const uint64_t after = written.load(std::memory_order_relaxed);
    for (auto& t : writers) t.join();
    expect(ok, "placed while written");
    expect(during < slots && after < slots, "the writers were still writing when it was placed (the test's own pacing)");
    expect(copied > 0 && (pagemap < 0 || copied < n), "the pages written so far copied in, the rest not");
    uint64_t lost = 0;
    for (uint64_t i = 0; i < slots; i++) lost += ((const uint64_t*)fresh)[i] != i + 1;
    if (lost) std::printf("  %llu of %llu writes lost\n", (unsigned long long)lost, (unsigned long long)slots);
    expect(lost == 0, "no write lost: every slot the writers wrote is in the device's memory");
    std::printf("  placed while written: %llu writes held back (none when the writers were not running then: held_write\n"
                "  checks that case for sure)\n", (unsigned long long)barrier.waits.load());
    pool[0] = 0xEE;
    expect(fresh[0] == 0xEE, "the pool is the device's memory now, and writable");
    bool idle = true;
    for (const auto& s : barrier.slots) idle = idle && !s.busy.load();
    expect(idle, "no range left being placed");
    sigaction(SIGSEGV, &old_segv, nullptr);
    munmap(fresh, n);
    close(fd);
    munmap(pool, n);
    if (pagemap >= 0) close(pagemap);
}


// ---- a write that meets the placement for sure: held until it is done, then in the placement (hold, then the rest) ----
void held_write() {
    const uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE), n = 64 * page;
    uint8_t* const pool = (uint8_t*)mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (pool == MAP_FAILED) return expect(false, "no pool to try it in");
    std::memset(pool, 0x5A, n);
    const int fd = memfd(n);
    uint8_t* const fresh = fd >= 0 ? placed::map(fd, n) : nullptr;
    if (!fresh) return expect(false, "no memfd");
    struct sigaction sa{};
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old_segv);
    const uint64_t waits0 = barrier.waits.load(), faults0 = (uint64_t)faults.load();
    const int slot = placed::hold(barrier, pool, n);
    expect(slot >= 0, "held: made read-only");
    std::atomic<bool> wrote{false};
    std::thread writer([&] {
        ((volatile uint8_t*)pool)[10 * page + 7] = 0xA7;   // (faults: read-only; waits in on_segv until the end below)
        wrote.store(true, std::memory_order_release);
    });
    const auto t0 = std::chrono::steady_clock::now();
    while ((uint64_t)faults.load(std::memory_order_acquire) == faults0 &&
           std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10))
        std::this_thread::yield();
    const bool faulted = (uint64_t)faults.load() > faults0;
    expect(faulted, "the write faulted on the held range");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    expect(!wrote.load(), "the write waits while the range is held");
    expect(pool[10 * page + 7] == 0x5A, "nothing written yet: the old pages still read as they were");
    placed::copy_in(fresh, pool, n, page, -1, {});
    expect(placed::place(pool, n, fd), "the placement mapped there");
    barrier.end(slot);
    writer.join();
    expect(wrote.load() && fresh[10 * page + 7] == 0xA7, "the held write ran again once it was done, into the placement");
    expect(fresh[10 * page + 6] == 0x5A && fresh[n - 1] == 0x5A, "the rest copied in");
    expect(barrier.waits.load() > waits0, "counted as held back");
    sigaction(SIGSEGV, &old_segv, nullptr);
    munmap(fresh, n);
    close(fd);
    munmap(pool, n);
}

void live_refused() {
    const uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE), n = 4 * page;
    const int fd = memfd(n);
    uint8_t* const fresh = fd >= 0 ? placed::map(fd, n) : nullptr;
    if (!fresh) return expect(false, "no memfd");
    // (made last: the kernel hands an address just freed to the next mapping)
    uint8_t* const hole = (uint8_t*)mmap(nullptr, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (hole == MAP_FAILED) return expect(false, "no range to try it in");
    munmap(hole, n);   // (nothing mapped there now: mprotect refuses it)
    placed::Barrier b;
    int err = 0;
    expect(!placed::place_live(b, hole, n, fd, fresh, page, -1, {}, nullptr, &err) && err == ENOMEM,
           "a range that cannot be made read-only: refused, with its errno, and nothing mapped there");
    expect(!b.slots[0].busy.load() && b.held((uintptr_t)hole), "its range ended at once (a fault there just after: run again)");
    expect(mmap(hole, n, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == hole,
           "nothing was mapped there");
    munmap(hole, n);
    // (a hole after its first pages: the kernel makes those read-only before it refuses at the hole, and they are made
    // writable again: a write there goes on, with no fault and no handler)
    uint8_t* const part = (uint8_t*)mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (part == MAP_FAILED) return expect(false, "no range to try it in");
    munmap(part + 2 * page, page);
    expect(placed::hold(b, part, n) < 0 && errno == ENOMEM, "a range with a hole: not held");
    part[page + 1] = 0x11;
    expect(part[page + 1] == 0x11, "its pages before the hole writable again");
    munmap(part, 2 * page);
    munmap(part + 3 * page, page);
    munmap(fresh, n);
    close(fd);
}

} // namespace

int main() {
    plans();
    later();
    later_pools();
    made_table();
    made_model();
    barrier_refusals();
    mappings();
    live();
    held_write();
    live_refused();
    if (failures) return 1;
    std::printf("placed memory: ok\n");
    return 0;
}
