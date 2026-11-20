// big_pools (gpu/vk/big_pools.h): the big placed pools measured for RX_VK_LOG_BIG_POOLS (gpu/vk/vk_memory.cpp big_*). The
// stretches a pool is cut into, the marks of what the GPU reached, the count of blocks holding data, the line's map and the
// pages mapped in. No game, no GPU.
#include "../gpu/vk/big_pools.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <vector>

namespace {

int failures = 0;
void expect(bool ok, const char* what) {
    if (!ok) std::printf("FAIL: %s\n", what), failures++;
}

constexpr uint64_t MB = 1ull << 20;

void shifts() {
    expect(big_pools::stretch_shift(2432 * MB) == 20, "2432 MB: 1 MB stretches");
    expect(big_pools::stretches(2432 * MB, 20) == 2432, "2432 MB: 2432 stretches");
    expect(big_pools::stretch_shift(4096 * MB) == 20, "4 GB: 1 MB stretches, 4096 of them");
    expect(big_pools::stretch_shift(4096 * MB + 1) == 21, "past 4 GB: 2 MB stretches");
    expect(big_pools::stretches(4096 * MB + 1, 21) == 2049, "past 4 GB: 2049 stretches");
    expect(big_pools::stretch_shift(64 * MB) == 20 && big_pools::stretches(64 * MB + 1, 20) == 65, "a part stretch counts");
    expect(big_pools::stretches(UINT64_MAX, 20) == big_pools::kStretches, "never more than kStretches");
}

void marks() {
    std::atomic<uint64_t> bits[big_pools::kWords];
    for (auto& w : bits) w.store(0);
    const uint64_t lo = 0x109c1b0000, n = 2432 * MB;
    expect(big_pools::mark(bits, 20, lo, n, lo + 5 * MB + 10, 100) == 1, "a small binding: one stretch");
    expect(big_pools::mark(bits, 20, lo, n, lo + 5 * MB, MB) == 0, "the same stretch again: none new");
    expect(big_pools::mark(bits, 20, lo, n, lo + 6 * MB - 1, 2) == 1, "across a stretch's end: the next one new");
    expect(big_pools::mark(bits, 20, lo, n, lo - 4096, 8192) == 1, "from before the pool: clipped to its first stretch");
    expect(big_pools::mark(bits, 20, lo, n, lo + n - 1, 1 << 30) == 1, "past its end: clipped to its last stretch");
    expect(big_pools::mark(bits, 20, lo, n, lo + n, 4096) == 0, "right after it: nothing");
    expect(big_pools::mark(bits, 20, lo, n, lo - 4096, 4096) == 0, "right before it: nothing");
    expect(big_pools::mark(bits, 20, lo, n, lo + 7 * MB, 0) == 0, "no bytes: nothing");
    expect(big_pools::mark(bits, 20, lo, n, lo + MB, UINT64_MAX) == 2432 - 4, "no wrap-around: the rest of it");
    uint64_t plain[big_pools::kWords];
    big_pools::snapshot(bits, plain);
    expect(big_pools::count(plain) == 2432, "every stretch marked once");
    expect((plain[0] & 1) && ((plain[2431 >> 6] >> (2431 & 63)) & 1) && !((plain[2432 >> 6] >> (2432 & 63)) & 1),
           "first and last, not past");

    uint64_t a[big_pools::kWords] = {}, b[big_pools::kWords] = {};
    a[0] = 0b1110, b[0] = 0b0111, a[63] = 1ull << 63, b[63] = 1ull << 63;
    expect(big_pools::count(a) == 4 && big_pools::count_both(a, b) == 3, "counted, and both");
}

void data() {
    const uint64_t n = 4 * MB + 1000;   // (a tail past the last whole block)
    std::vector<uint8_t> p(n, 0);
    uint64_t bits[big_pools::kWords] = {}, top = 0;
    expect(big_pools::data_blocks(p.data(), 0, n, 20, bits, &top) == 0 && top == 0 && big_pools::count(bits) == 0, "all zero");
    p[big_pools::kBlock - 1] = 1;              // (block 0's last byte)
    p[MB + 3 * big_pools::kBlock + 70] = 2;    // (inside a block of stretch 1, not on a word's start)
    p[MB + 3 * big_pools::kBlock + 71] = 3;    // (the same block)
    p[n - 1] = 4;                              // (the tail)
    expect(big_pools::data_blocks(p.data(), 0, n, 20, bits, &top) == 3, "three blocks hold data");
    expect(big_pools::count(bits) == 3 && (bits[0] & 0b10011) == 0b10011, "their stretches: 0, 1 and 4");
    expect(top == n, "up to its end");
    // (read in parts, as the scanner does under its lock: an offset in the pool for each)
    uint64_t parts[big_pools::kWords] = {}, top2 = 0, blocks = 0;
    for (uint64_t off = 0; off < n; off += 2 * MB)
        blocks += big_pools::data_blocks(p.data() + off, off, std::min<uint64_t>(2 * MB, n - off), 20, parts, &top2);
    expect(blocks == 3 && top2 == n && parts[0] == bits[0], "read in parts: the same");
    uint64_t at[big_pools::kWords] = {}, top3 = 0;
    big_pools::data_blocks(p.data(), 64 * MB, MB, 20, at, &top3);
    expect(at[1] == 1 && top3 == 64 * MB + big_pools::kBlock, "at an offset: its stretch, its top");
    for (uint64_t i = 0; i < 64; i++) {   // (each byte of a block's first 64 alone)
        std::vector<uint8_t> q(big_pools::kBlock, 0);
        q[i] = 0x80;
        if (big_pools::all_zero(q.data(), q.size())) expect(false, "a single byte other than zero");
    }
}

void maps() {
    uint64_t w[big_pools::kWords] = {}, r[big_pools::kWords] = {}, t[big_pools::kWords] = {}, d[big_pools::kWords] = {};
    // (8 stretches: 0 written, 1 read, 2 texture, 3 data, 4 nothing, 5 written and data, 6 data, 7 nothing)
    w[0] = 1 | (1 << 5), r[0] = 1 << 1, t[0] = 1 << 2, d[0] = (1 << 3) | (1 << 5) | (1 << 6);
    expect(big_pools::map_line(w, r, t, d, 8, 64) == "WRTd.Wd.", "one character a stretch");
    expect(big_pools::map_line(w, r, t, d, 8, 4) == "WTWd", "two stretches a character: the strongest");
    expect(big_pools::map_line(nullptr, nullptr, nullptr, d, 8, 8) == "...d.dd.", "data alone");
    expect(big_pools::map_line(w, r, t, nullptr, 8, 2) == "WW", "no data read");
    expect(big_pools::map_line(w, r, t, d, 0, 64).empty(), "no stretches");
    const std::string wide = big_pools::map_line(w, r, t, d, 2432, 64);
    expect(wide.size() == 64 && wide[0] == 'W' && wide.substr(1) == std::string(63, '.'), "2432 stretches in 64");
}

void resident() {
    const uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
    const uint64_t n = 64 * page;
    auto* p = static_cast<uint8_t*>(mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    expect(p != MAP_FAILED, "mapped");
    if (p == MAP_FAILED) return;
    expect(big_pools::resident_bytes(p, n, page) == 0, "nothing touched: nothing mapped in");
    p[0] = 1, p[5 * page + 7] = 1, p[63 * page] = 1;
    expect(big_pools::resident_bytes(p, n, page) == (int64_t)(3 * page), "three pages written: three mapped in");
    expect(big_pools::resident_bytes(p, 6 * page - 1, page) == (int64_t)(2 * page - 1), "a range ending inside a page");
    expect(big_pools::resident_bytes(p + 1, page, page) == -1, "not page aligned: refused");
    munmap(p, n);
}

} // namespace

int main() {
    shifts();
    marks();
    data();
    maps();
    resident();
    if (failures) return std::printf("%d failed\n", failures), 1;
    std::printf("big_pools: ok\n");
    return 0;
}
