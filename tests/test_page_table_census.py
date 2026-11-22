"""The guest pools the page table cannot reach are logged: one line for each pool that comes outside its windows (the first 30),
and at close how many came, their bytes outside and where they lie. And RX_VK_GP_TABLE's cover of the storage buffers bound to
stages that read global memory through the table alone.

Guest memory's page table, for the pointers shaders follow, covers three windows: 8 GB from 64 GB, 16 GB from 496 GB and NVN's
virtual pools' 4 GB from 1 TB (vk_memory.cpp guest_pages_set, read by spirv_emit_context.cpp). A pointer into a pool outside them
that no bound buffer holds reads 0, and whether the game has such pools was never counted. Checked here:
- gpu/vk/page_table_census.h's windows are the shaders' own (kGuestPagesBase .. kGuestPagesBytes3), and guest_pages_set (through
  page_table_runs) and memory_guest_pages take theirs from it: the census counts what the table leaves out;
- its census, run against pool tables like memory_sync's: pools at 128 GB and 488 GB and two across a window's end are named
  (the two by their bytes outside), the ones inside are not; a table refreshed with the same pools names nothing again; past 30
  the pools are counted and not named; one that leaves and comes back counts again; the summary line gives what came, the most at
  once, what is outside now and where they lie; with none outside it says so, with the pools in the table;
- page_table_cover, the entry every page of a binding needs (its memory, 4 KB a page), every page looked at: held, wrong, a
  conflict with a run not written yet, or impossible (outside the windows, past the address space's end, no device address);
- PR #2103's review, L1 (the table-only path reads 0 for a bound buffer outside the windows or in a virtual pool), with the game's
  own cases: a pool at 506 GB the census saw outside the old windows is mapped by its import now, and program 26fe0c70's
  virtual-pool buffer reads 0 through the table until it is covered at its binding, then its own memory; and the table writes
  that answer rests on (a pool's import, a mirror made or given back, a pool retired) are in vk_memory.cpp, the cover in
  vk_draw.cpp by the shared rule and after the binding is final;
- the invariant that makes the table route read what the storage buffer search read, in a model of the renderer's memory: pools
  read in place or through mirrors made and given back, virtual pages mapped and moved, storage buffers bound a submission at a
  time and covered as memory_table_cover covers them: every byte of every binding is found through the table at the binding's
  own memory;
- memory_sync refreshes the census with the pool table it is about to take, and mkwvk_close_asked logs the summary.

No Vulkan, GPU or game. python -I -B tests/test_page_table_census.py [--cxx clang++] [--source <tree>]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    at = text.index(marker)
    end = text.index("{", at) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[at:end]


FIXTURE = r"""
#include "page_table_census.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace mkwvk;
struct GuestPool { uint64_t lo, hi; };   // (vk_memory.cpp's: aligned outwards, by lo)
constexpr uint64_t KB = 1ull << 10, MB = 1ull << 20, GB = 1ull << 30;
void out(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) std::printf("LOG %s\n", line.c_str());
}
int main() {
    for (const PageWindow& w : kPageWindows)
        std::printf("WINDOW %llu %llu %llu\n", (unsigned long long)w.base, (unsigned long long)w.bytes, (unsigned long long)w.first);
    std::printf("ENTRIES %llu\n", (unsigned long long)kPageTableEntries);
    // (the bytes outside)
    CHECK(page_table_outside(0x1000000000ull, 0x1004000000ull) == 0);
    CHECK(page_table_outside(0x2000000000ull, 0x2004000000ull) == 64 * MB);
    CHECK(page_table_outside(0x11ffff0000ull, 0x1200010000ull) == 64 * KB);
    CHECK(page_table_outside(0x0fffff0000ull, 0x1000010000ull) == 64 * KB);
    CHECK(page_table_outside(0x7fffff0000ull, 0x8000010000ull) == 64 * KB);
    CHECK(page_table_outside(0, 512 * GB) == 488 * GB);
    CHECK(page_table_outside(0x2000000000ull, 0x2000000000ull) == 0 && page_table_outside(5, 4) == 0);
    const GuestPool in_a{0x1000000000ull, 0x1004000000ull}, across_a{0x11ffff0000ull, 0x1200010000ull},
                    at_128{0x2000000000ull, 0x2004000000ull}, at_504{0x7a00000000ull, 0x7a00100000ull},
                    in_b{0x7f00000000ull, 0x7f40000000ull}, across_b{0x7fffff0000ull, 0x8000010000ull};
    {
        PageTableCensus none;
        const std::vector<GuestPool> inside{in_a, in_b};
        CHECK(none.refresh(inside).empty());
        std::printf("SUMMARY %s\n", none.summary("close").c_str());
    }
    PageTableCensus c;
    std::vector<GuestPool> first{in_a, across_a, at_128, at_504, in_b, across_b};
    std::printf("REFRESH 1\n");
    out(c.refresh(first));
    std::printf("REFRESH 2\n");
    out(c.refresh(first));   // (the same pools: nothing new)
    CHECK(c.came == 4 && c.came_bytes == 64 * MB + 1 * MB + 128 * KB);
    std::vector<GuestPool> second{in_a, across_a, at_504, in_b, across_b};
    for (uint64_t i = 0; i < 40; i++) second.push_back({0x3000000000ull + i * MB, 0x3000000000ull + (i + 1) * MB});
    std::printf("REFRESH 3\n");
    out(c.refresh(second));   // (128 GB's pool gone, 40 new ones: 26 named, the 30 reached)
    CHECK(c.came == 44 && c.named == 30 && c.outside.size() == 43);
    std::vector<GuestPool> third = second;
    third.push_back(at_128);   // (back: it counts again, and is not named again past the 30)
    std::printf("REFRESH 4\n");
    out(c.refresh(third));
    CHECK(c.came == 45 && c.outside.size() == 44 && c.most == 44 && c.pools == 46);
    std::printf("SUMMARY %s\n", c.summary("close").c_str());
    {   // (RX_VK_GP_TABLE's cover: the entry each page of a binding needs, exactly, the runs not written yet over the entries)
        std::vector<uint64_t> e(kPageTableEntries, 0);
        const uint64_t a = 0x1000000000ull;
        auto set = [&](uint64_t lo, uint64_t hi, uint64_t address) {
            page_table_runs(lo, hi, address, [&](uint64_t first, uint64_t count, uint64_t at) {
                for (uint64_t i = 0; i < count; i++) e[first + i] = at ? at + (i << 12) : 0;
            });
        };
        set(a, a + 64 * KB, 0xabc000ull);   // (a pool's 64 KB, its memory at 0xabc000)
        PageCover c = page_table_cover(e.data(), nullptr, 0, a + 4 * KB + 8, 16, 0xabc000ull + 4 * KB + 8);
        CHECK(!c.outside && c.pages == 1 && !c.wrong);
        c = page_table_cover(e.data(), nullptr, 0, a, 64 * KB, 0xabc000ull);
        CHECK(c.pages == 16 && !c.wrong);
        c = page_table_cover(e.data(), nullptr, 0, a + 60 * KB, 8 * KB, 0xabc000ull + 60 * KB);   // (its last page has none)
        CHECK(c.pages == 2 && c.wrong == 1 && !c.conflicts);
        c = page_table_cover(e.data(), nullptr, 0, a, 4 * KB, 0x123000ull);   // (the entry is other memory: a mirror)
        CHECK(c.wrong == 1);
        set(a + 1 * MB, a + 1 * MB + 128 * 4 * KB, 0x800000ull);   // (128 pages right, then one odd page in the middle wrong:
        e[(1 * MB >> 12) + 67] = 0x123000ull;                     // every page is looked at, none sampled past)
        c = page_table_cover(e.data(), nullptr, 0, a + 1 * MB, 128 * 4 * KB, 0x800000ull);
        CHECK(c.pages == 128 && c.wrong == 1);
        CHECK(page_table_cover(e.data(), nullptr, 0, 0x2000000000ull, 4, 0x1000ull).outside);   // (outside the windows)
        CHECK(page_table_cover(e.data(), nullptr, 0, 0x11fffff000ull, 8 * KB, 0x1000ull).outside);   // (across a window's end)
        CHECK(page_table_cover(e.data(), nullptr, 0, ~0ull - 100, 200, 0x1000ull).outside);   // (past the address space's end)
        CHECK(page_table_cover(e.data(), nullptr, 0, a, 4, 0).outside);   // (no device address: a copy)
        CHECK(!page_table_cover(e.data(), nullptr, 0, 0x10000000000ull, 4, 0x5000ull).outside);   // (a virtual pool's window)
        const PageTableRun add[] = {{16, 4, 0x123000ull}}, other[] = {{0, 1, 0x999000ull}};
        c = page_table_cover(e.data(), add, 1, a + 64 * KB, 16 * KB, 0x123000ull);   // (pending, not written yet: held)
        CHECK(!c.wrong);
        c = page_table_cover(e.data(), other, 1, a, 4 * KB, 0xabc000ull);   // (another binding's run this submission)
        CHECK(c.wrong == 1 && c.conflicts == 1);
    }
    {   // (PR #2103's review, L1: "the table-only path returns zero for a valid bound buffer outside the two windows or in a
        // virtual pool, whereas the old search reads the binding". Both cases as the game had them, through the renderer's rules)
        std::vector<uint64_t> e(kPageTableEntries, 0);
        auto set = [&](uint64_t lo, uint64_t hi, uint64_t address) {
            page_table_runs(lo, hi, address, [&](uint64_t first, uint64_t count, uint64_t at) {
                for (uint64_t i = 0; i < count; i++) e[first + i] = at ? at + (i << 12) : 0;
            });
        };
        auto reads = [&](uint64_t at) -> uint64_t {   // (what page_load reads at a guest address: the entry, 0 where none)
            const int64_t i = page_table_index(at);
            return i < 0 || !e[(size_t)i] ? 0 : e[(size_t)i] + (at & 0xFFF);
        };
        // (a pool the census saw outside the old windows, at 506 GB (0x7e95460000, 12 of 627 runs): inside the second window now,
        // so its import's table write maps it, and a binding there needs nothing more)
        const uint64_t pool = 0x7e95460000ull, pool_mem = 0x40000000ull;
        CHECK(page_table_outside(pool, pool + 1 * MB) == 0);
        set(pool, pool + 1 * MB, pool_mem);   // (its import: vk_memory.cpp guest_pages_set(p.lo, p.hi, p.address))
        CHECK(!page_table_cover_wanted(pool + 64 * KB, false));   // (a guest pool's binding: left alone by default)
        for (uint64_t at = pool; at < pool + 1 * MB; at += 256) CHECK(reads(at) == pool_mem + (at - pool));
        // (a virtual pool's storage buffer, as program 26fe0c70ab9fc547 bound it: 0x10001ef0600, 1,843,200 bytes, its pages backed
        // by guest memory. Before its cover the table reads 0 there: L1 exactly. Covered at its binding, it reads the binding)
        const uint64_t v = 0x10001ef0600ull, vn = 1843200, backing = 0x40800600ull;   // (the memory the binding reads)
        CHECK(page_table_cover_wanted(v, false));
        for (uint64_t at = v; at < v + vn; at += 4096) CHECK(reads(at) == 0);
        const PageCover before = page_table_cover(e.data(), nullptr, 0, v, vn, backing);
        CHECK(!before.outside && before.wrong == before.pages && before.pages == 451);
        set(v, v + vn, backing);   // (gp_table_cover: memory_table_cover's write)
        CHECK(!page_table_cover(e.data(), nullptr, 0, v, vn, backing).wrong);
        for (uint64_t at = v; at < v + vn; at += 64) CHECK(reads(at) == backing + (at - v));
        // (and outside every window, which the census never saw: the cover says so, and the log names it)
        CHECK(page_table_cover(e.data(), nullptr, 0, 0x2000000000ull, 4096, 0x1000ull).outside);
        CHECK(page_table_cover_wanted(0x2000000000ull, true) && !page_table_cover_wanted(0x2000000000ull, false));
        std::printf("L1 checked\n");
    }
    {   // (the invariant, in a model of the renderer's memory: guest pools in the windows, each read in place or through a mirror made
        // and given back (the table following each, as vk_memory.cpp's writes do), virtual pages mapped onto them and moved, and
        // storage buffers bound over all of it, a submission at a time. A pool's binding finds its memory in the table already; a
        // virtual pool's is covered as gp_table_cover covers it (runs not written yet, then written at the next submission). For
        // every binding of the submission when it is done, every byte the binding holds is found through the table at the very
        // memory the binding reads: what the storage buffer search read)
        uint64_t rng = 0x9e3779b97f4a7c15ull;
        auto next = [&]() { rng += 0x9e3779b97f4a7c15ull; uint64_t z = rng; z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
                            z = (z ^ (z >> 27)) * 0x94d049bb133111ebull; return z ^ (z >> 31); };
        struct Pool { uint64_t lo, hi, address, mirror; };   // (mirror: its address, 0 none)
        std::vector<Pool> pools;
        uint64_t device = 0x100000000ull;   // (device addresses handed out, 4 KB aligned)
        auto take = [&](uint64_t bytes) { const uint64_t at = device; device += (bytes + 0xFFFFull) & ~0xFFFull; return at; };
        const uint64_t lows[] = {0x1000000000ull, 0x1080000000ull, 0x11f0000000ull, 0x7c00000000ull, 0x7e95460000ull, 0x7fff000000ull};
        std::vector<uint64_t> e(kPageTableEntries, 0);
        std::vector<PageTableRun> pending;
        auto set = [&](uint64_t lo, uint64_t hi, uint64_t address) {
            page_table_runs(lo, hi, address, [&](uint64_t first, uint64_t count, uint64_t at) { pending.push_back({first, count, at}); });
        };
        auto submit = [&]() {   // (the submission's table writes land before its draws, in order)
            for (const PageTableRun& r : pending)
                for (uint64_t i = 0; i < r.count; i++) e[r.first + i] = r.address ? r.address + (i << 12) : 0;
            pending.clear();
        };
        for (uint64_t lo : lows) {   // (imported: the table gives guest memory)
            const uint64_t bytes = (1 + next() % 64) * 64 * KB;
            pools.push_back({lo, lo + bytes, take(bytes), 0});
            set(lo, lo + bytes, pools.back().address);
        }
        submit();
        std::vector<uint64_t> vmap(256, 0);   // (virtual page i, from 1 TB: the guest page behind it, 0 unmapped)
        auto reads_at = [&](uint64_t guest) -> uint64_t {   // (the memory a binding of guest reads: the mirror, else the pool)
            for (const Pool& p : pools)
                if (guest >= p.lo && guest < p.hi) return (p.mirror ? p.mirror : p.address) + (guest - p.lo);
            return 0;
        };
        struct Bound { uint64_t gpu, n, address; };
        int bindings = 0, written = 0;
        for (int round = 0; round < 400; round++) {
            for (int k = 0; k < 3; k++) {   // (the memory changes between submissions)
                Pool& p = pools[next() % pools.size()];
                switch (next() % 3) {
                case 0:   // (a mirror made, or given back: the table follows, as mirror_make and mirrors_give_back set it)
                    p.mirror = p.mirror ? 0 : take(p.hi - p.lo);
                    set(p.lo, p.hi, p.mirror ? p.mirror : p.address);
                    break;
                case 1: {   // (a virtual page mapped onto a guest page, or unmapped)
                    const Pool& q = pools[next() % pools.size()];
                    vmap[next() % vmap.size()] = next() % 4 ? (q.lo + ((next() % ((q.hi - q.lo) >> 12)) << 12)) : 0;
                    break;
                }
                default: break;
                }
            }
            std::vector<Bound> bound;
            for (int k = 0; k < 6; k++) {   // (storage buffers bound this submission, each covered at its binding)
                Bound b{};
                if (next() % 3) {   // (in a pool, as gpu_buffer_bind binds a guest range)
                    const Pool& p = pools[next() % pools.size()];
                    const uint64_t off = (next() % (p.hi - p.lo)) & ~15ull, room = p.hi - p.lo - off;
                    b.gpu = p.lo + off, b.n = std::max<uint64_t>(16, std::min<uint64_t>(room, 16 + next() % (96 * KB)) & ~15ull);
                    b.address = reads_at(b.gpu);
                } else {   // (in a virtual pool: one virtual page, as host->gpu_ptr maps it, onto its guest page's memory)
                    const uint64_t i = next() % vmap.size();
                    if (!vmap[i]) continue;
                    const uint64_t off = (next() % 4096) & ~15ull;
                    b.gpu = 0x10000000000ull + (i << 12) + off, b.n = std::max<uint64_t>(16, (4096 - off) & ~15ull);
                    b.address = reads_at(vmap[i] + off);
                }
                const PageCover c = page_table_cover(e.data(), pending.data(), pending.size(), b.gpu, b.n, b.address);
                CHECK(!c.outside && !c.conflicts);
                // (a pool's binding needs nothing: the table followed the pool's import and its mirror as they happened. Only a
                // virtual pool's is covered, as gp_table_cover covers it)
                if (b.gpu < 0x10000000000ull) CHECK(!c.wrong);
                else if (c.wrong) set(b.gpu, b.gpu + b.n, b.address), written++;
                CHECK(!page_table_cover(e.data(), pending.data(), pending.size(), b.gpu, b.n, b.address).wrong);
                bound.push_back(b);
                bindings++;
            }
            submit();
            for (const Bound& b : bound)   // (each byte through the table, as page_load reads it, against the binding's own memory)
                for (uint64_t at = b.gpu; at < b.gpu + b.n; at += 512) {
                    const int64_t i = page_table_index(at);
                    CHECK(i >= 0 && e[(size_t)i] && e[(size_t)i] + (at & 0xFFF) == b.address + (at - b.gpu));
                }
        }
        CHECK(bindings > 1000 && written > 50);   // (the model reached both: bindings already held, and virtual ones written)
        std::printf("MODEL %d bindings, %d written\n", bindings, written);
    }
    return 0;
}
"""

WANT_FIRST = [
    "vk: page table: pool 0x11ffff0000..0x1200010000 (128 KB) is partly outside its windows (64 KB of it): a pointer into those "
    "bytes that no bound buffer holds reads 0",
    "vk: page table: pool 0x2000000000..0x2004000000 (64.0 MB) is outside its windows: a pointer into it that no bound buffer "
    "holds reads 0",
    "vk: page table: pool 0x7a00000000..0x7a00100000 (1.0 MB) is outside its windows: a pointer into it that no bound buffer "
    "holds reads 0",
    "vk: page table: pool 0x7fffff0000..0x8000010000 (128 KB) is partly outside its windows (64 KB of it): a pointer into those "
    "bytes that no bound buffer holds reads 0",
]
WANT_THIRD = ["vk: page table: pool %#x..%#x (1.0 MB) is outside its windows: a pointer into it that no bound buffer holds reads 0"
              % (0x3000000000 + i * 2**20, 0x3000000000 + (i + 1) * 2**20) for i in range(26)]
WANT_NONE = "vk: page table census (close): no pool outside its windows, 64-72 GB and 496-512 GB and 1024-1028 GB (2 pools in the table now)"
WANT_SUMMARY = ("vk: page table census (close): 45 pools outside its windows, 64-72 GB and 496-512 GB and 1024-1028 GB, came into the pool table "
                "(the first 30 named above), 169.1 MB of them outside; at most 44 at once (105.1 MB), 44 now of 46 pools; between "
                "0x11ffff0000 and 0x8000010000")


def number(text):
    """0x1000000000ull, 8ull << 30"""
    m = re.fullmatch(r"\s*(0x[0-9a-fA-F]+|\d+)ull(?:\s*<<\s*(\d+))?\s*", text)
    if not m:
        raise ValueError(text)
    return int(m.group(1), 0) << int(m.group(2) or 0)


def source_checks(vk, fails):
    emit = (vk / "eden/shader_recompiler/backend/spirv/spirv_emit_context.cpp").read_text(encoding="utf-8")
    shaders = {}
    for name in ("kGuestPagesBase", "kGuestPagesBytes", "kGuestPagesBase2", "kGuestPagesBytes2", "kGuestPagesBase3", "kGuestPagesBytes3"):
        m = re.search(r"constexpr unsigned long long %s = ([^;]+);" % name, emit)
        if not m:
            fails.append("spirv_emit_context.cpp: no %s" % name)
            return None
        shaders[name] = number(m.group(1))
    memory = (vk / "vk_memory.cpp").read_text(encoding="utf-8")
    setter = block(memory, "void guest_pages_set(")
    if "page_table_runs(lo, hi, address," not in setter or re.search(r"0x[0-9a-fA-F]{6,}", setter):
        fails.append("vk_memory.cpp guest_pages_set: its windows are not page_table_census.h's kPageWindows")
    if "const uint64_t bytes = kPageTableEntries * 8;" not in block(memory, "uint64_t memory_guest_pages("):
        fails.append("vk_memory.cpp memory_guest_pages: the table is not sized by page_table_census.h's kPageTableEntries")
    # (L1's answer rests on the table following the pools by itself: their import, a mirror made (whole or sparse), a mirror given
    # back, a pool retired; and on the cover of a virtual pool's bindings after the binding is made, by the shared rule)
    for line, why in (("guest_pages_set(p.lo, p.hi, p.address);", "a pool's import maps its memory"),
                      ("guest_pages_set(p->lo, p->hi, ma);", "a mirror made moves the pool's pages to it"),
                      ("sparse_table(*p, ma, 0, p->has.size());", "a sparse mirror made moves its blocks with memory to it"),
                      ("if (live != imported.end() && &live->second == &p) guest_pages_set(p.lo, p.hi, p.address);",
                       "a mirror given back moves the pool's pages back"),
                      ("guest_pages_set(old->lo, old->hi, 0);", "a pool retired leaves the table")):
        if line not in memory:
            fails.append("vk_memory.cpp: %s no longer (%s): the table may not follow the pools, check RX_VK_GP_TABLE's cover" % (why, line))
    draw = (vk / "vk_draw.cpp").read_text(encoding="utf-8")
    cover = block(draw, "void gp_table_cover(")
    if "page_table_cover_wanted(base, every)" not in cover or "memory_table_cover(base, n, address)" not in cover:
        fails.append("vk_draw.cpp gp_table_cover: not by page_table_cover_wanted, or not through memory_table_cover")
    if "if (b) gp_table_cover(p.key, st, base, n, b);" not in draw or draw.index("if (b) gp_table_cover(") < draw.index("if (b && wide_n) {"):
        fails.append("vk_draw.cpp: the cover is not on the binding as finally made (after the smaller retry and the wide one)")
    sync = block(memory, "void memory_sync(")
    refresh, swap = sync.find("page_census.refresh(next)"), sync.find("guest_pools.swap(next);")
    if refresh < 0 or swap < 0 or refresh > swap or "page_census_mu" not in sync:
        fails.append("vk_memory.cpp memory_sync: the census is not refreshed (under its lock) with the pool table before it is taken")
    log = block(memory, "void page_table_census_log(const char* when)")
    if "page_census_mu" not in log or "page_census.summary(when)" not in log:
        fails.append("vk_memory.cpp page_table_census_log: not the census's summary, under its lock")
    close = block((vk / "vk_main.cpp").read_text(encoding="utf-8"), "void mkwvk_close_asked(void)")
    if 'page_table_census_log("close");' not in close:
        fails.append("vk_main.cpp mkwvk_close_asked: the census's summary is not logged at close")
    return shaders


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cxx", default="clang++")
    ap.add_argument("--source", type=Path, default=ROOT, help="the tree to check (default: this one)")
    a = ap.parse_args()
    vk = a.source / "gpu" / "vk"
    fails = []
    if not (vk / "page_table_census.h").is_file():
        fails.append("gpu/vk/page_table_census.h: no census of the guest pools outside the page table's windows")
    else:
        shaders = source_checks(vk, fails)
        with tempfile.TemporaryDirectory(prefix="page-table-census-") as tmp:
            src, exe = Path(tmp) / "fixture.cpp", Path(tmp) / ("fixture" + (".exe" if os.name == "nt" else ""))
            src.write_text(FIXTURE, encoding="utf-8")
            r = subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(vk), str(src), "-o", str(exe)],
                               capture_output=True, text=True, timeout=180)
            if r.returncode:
                print(r.stdout + r.stderr)
                fails.append("the fixture did not compile")
            else:
                got = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
                if got.returncode:
                    fails.append("the fixture failed: " + got.stderr.strip())
                else:
                    out = got.stdout.splitlines()
                    windows = [tuple(int(x) for x in line.split()[1:]) for line in out if line.startswith("WINDOW ")]
                    one, two = shaders["kGuestPagesBytes"] >> 12 if shaders else 0, shaders["kGuestPagesBytes2"] >> 12 if shaders else 0
                    if shaders and windows != [(shaders["kGuestPagesBase"], shaders["kGuestPagesBytes"], 0),
                                               (shaders["kGuestPagesBase2"], shaders["kGuestPagesBytes2"], one),
                                               (shaders["kGuestPagesBase3"], shaders["kGuestPagesBytes3"], one + two)]:
                        fails.append("page_table_census.h's windows %r are not the shaders' %r" % (windows, shaders))
                    if shaders and out.count("ENTRIES %d" % ((shaders["kGuestPagesBytes"] + shaders["kGuestPagesBytes2"] +
                                                               shaders["kGuestPagesBytes3"]) >> 12)) != 1:
                        fails.append("kPageTableEntries is not the three windows' 4 KB pages")
                    if not any(line.startswith("MODEL ") for line in out):
                        fails.append("the cover's model did not run")
                    if "L1 checked" not in out:
                        fails.append("the review's L1 cases did not run")
                    parts, at = {}, None
                    for line in out:
                        if line.startswith("REFRESH "):
                            at = int(line.split()[1])
                            parts[at] = []
                        elif line.startswith("LOG "):
                            parts.setdefault(at, []).append(line[4:])
                    summaries = [line[8:] for line in out if line.startswith("SUMMARY ")]
                    for k, want in ((1, WANT_FIRST), (2, []), (3, WANT_THIRD), (4, [])):
                        if parts.get(k) != want:
                            fails.append("refresh %d named:\n  got  %s\n  want %s" % (k, "\n       ".join(parts.get(k) or ["(none)"]),
                                                                                    "\n       ".join(want or ["(none)"])))
                    if summaries != [WANT_NONE, WANT_SUMMARY]:
                        fails.append("the summaries:\n  got  %s\n  want %s" % ("\n       ".join(summaries),
                                                                              "\n       ".join([WANT_NONE, WANT_SUMMARY])))
    for f in fails:
        print("FAIL: " + f)
    if fails:
        return 1
    print("PASS: each guest pool outside the page table's windows is named as it comes (the first 30), and the close's line counts "
          "them all, their bytes outside and where they lie")
    return 0


if __name__ == "__main__":
    sys.exit(main())
