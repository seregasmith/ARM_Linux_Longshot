// placed (gpu/vk/placed_memory.h): the guest's pools placed in memory of the device's, as gpu/vk/vk_memory.cpp does where the
// driver takes neither host pointers nor dma-bufs of the guest window (Turnip on a kernel without /dev/udmabuf). A memfd
// stands in for the device's dma-buf (both are mapped shared, by fd, and the device's view is a second mapping of the same
// memory); the region stands in for the guest window, made as host/hos_main.c makes it there. Linux; no game, no GPU.
#include "../gpu/vk/placed_memory.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdio>
#include <map>
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

} // namespace

int main() {
    plans();
    mappings();
    if (failures) return 1;
    std::printf("placed memory: ok\n");
    return 0;
}
