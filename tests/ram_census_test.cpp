// ram_census (gpu/vk/ram_census.h): where the RAM goes, for RX_VK_LOG_RAM (gpu/vk/vk_memory.cpp ram_census_*). Made-up
// kernel text in the shapes the kernel writes; no game, no GPU.
#include "../gpu/vk/ram_census.h"

#include <cstdio>
#ifdef __linux__
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {

int failures = 0;
void expect(bool ok, const char* what) {
    if (!ok) std::printf("FAIL: %s\n", what), failures++;
}
constexpr uint64_t KB = 1024, MB = 1024 * 1024;

void fields() {
    uint64_t v = 0;
    expect(ram_census::field("MemTotal:       15561604 kB\nMemFree:  2 kB\n", "MemFree:", &v) && v == 2 * KB, "a kB field, on its line");
    expect(ram_census::field("SwapCached:  7 kB\nCached:  9 kB\n", "Cached:", &v) && v == 9 * KB,
           "Cached: its own line, not SwapCached's");
    expect(!ram_census::field("SwapCached:  7 kB\n", "Cached:", &v), "a key only inside another: not there");
    expect(ram_census::field("drm-total-memory:\t3 MiB\n", "drm-total-memory:", &v) && v == 3 * MB, "MiB");
    expect(ram_census::field("drm-total-memory: 4096\n", "drm-total-memory:", &v) && v == 4096, "no unit: bytes");
    expect(!ram_census::field("", "MemTotal:", &v), "empty text: nothing");
}

void system() {
    const char* meminfo =
        "MemTotal:       15561604 kB\nMemFree:          204800 kB\nMemAvailable:     409600 kB\nBuffers:            1024 kB\n"
        "Cached:          2097152 kB\nSwapCached:        102400 kB\nAnonPages:       8388608 kB\nShmem:            524288 kB\n"
        "KReclaimable:     307200 kB\nSUnreclaim:       204800 kB\nUnevictable:       65536 kB\nSwapTotal:      15561600 kB\n"
        "SwapFree:        9437184 kB\n";
    const ram_census::System s = ram_census::system(meminfo);
    expect(s.total == 15561604 * KB && s.avail == 400 * MB && s.free == 200 * MB, "total, available, free");
    expect(s.cached == 2048 * MB && s.swap_cached == 100 * MB && s.shmem == 512 * MB && s.anon == 8192 * MB, "cache, anonymous");
    expect(s.kreclaim == 300 * MB && s.sunreclaim == 200 * MB && s.unevictable == 64 * MB, "the kernel's");
    expect(s.swap_total == 15561600 * KB && s.swap_free == 9216 * MB, "swap");
    const ram_census::System none = ram_census::system("MemTotal: 1 kB\n");
    expect(none.total == KB && none.avail == 0 && none.swap_total == 0, "a field the kernel has not: 0");
}

void zram() {
    ram_census::Zram z;
    ram_census::add_mm_stat(z, "4294967296 1073741824 1157627904        0 1157627904    12345     6789        0        0\n");
    ram_census::add_mm_stat(z, "1048576 524288 655360 0 655360 0 0 0 0\n");
    ram_census::add_mm_stat(z, "garbage\n");
    expect(z.devices == 2 && z.data == 4096 * MB + MB && z.compressed == 1024 * MB + MB / 2 &&
               z.ram == 1104 * MB + 640 * 1024,
           "two devices summed: data, compressed, the RAM they take; a line not theirs left out");
}

void processes() {
    const char* status = "Name:\tsteamwebhelper\nUmask:\t0022\nState:\tS (sleeping)\nVmRSS:\t  307200 kB\nRssAnon:\t  204800 kB\n"
                         "RssFile:\t   81920 kB\nRssShmem:\t   20480 kB\nVmSwap:\t  102400 kB\n";
    const ram_census::Process p = ram_census::process(status);
    expect(p.ok && p.name == "steamwebhelper", "its name");
    expect(p.rss == 300 * MB && p.anon == 200 * MB && p.file == 80 * MB && p.shmem == 20 * MB && p.swap == 100 * MB,
           "resident by kind, and in swap");
    const ram_census::Process k = ram_census::process("Name:\tkworker/0:1\nState:\tI (idle)\n");
    expect(!k.ok && k.name == "kworker/0:1", "a kernel thread: no VmRSS, not counted");
    std::vector<ram_census::Process> all{p, p, k, ram_census::process("Name:\tgamescope\nVmRSS:\t 512000 kB\nVmSwap:\t 0 kB\n"),
                                         ram_census::process("Name:\tsmall\nVmRSS:\t 1024 kB\n")};
    const std::vector<ram_census::Group> g = ram_census::top(all, 2);
    expect(g.size() == 2 && g[0].name == "steamwebhelper" && g[0].count == 2 && g[0].rss == 600 * MB && g[0].swap == 200 * MB,
           "by name, two of the web helper as one, resident and swap together the most first");
    expect(g[1].name == "gamescope" && g[1].count == 1, "then the next; the rest left out");
}

void smaps() {
    // (the guest window at 0x100000000000: its anonymous memory, a pool placed in the device's memory inside it, and outside
    // it the renderer's heap, a library and the driver's device file)
    const uint64_t lo = 0x100000000000ull, hi = lo + (1ull << 39);
    const char* text =
        "aaaab0000000-aaaab0100000 r-xp 00000000 103:02 1234                       /usr/lib/longshot/longshot\n"
        "Size:               1024 kB\nRss:                 800 kB\nAnonymous:             0 kB\nSwap:                  0 kB\n"
        "VmFlags: rd ex mr mw me\n"
        "100000000000-101000000000 rw-p 00000000 00:00 0 \n"
        "Size:           67108864 kB\nRss:             3145728 kB\nAnonymous:       3145728 kB\nSwap:            2097152 kB\n"
        "101000000000-101008000000 rw-s 00000000 00:0c 77                         /dmabuf:\n"
        "Size:             131072 kB\nRss:                   0 kB\nAnonymous:             0 kB\nSwap:                  0 kB\n"
        "101008000000-108000000000 rw-p 00000000 00:00 0 \n"
        "Size:           12345678 kB\nRss:              524288 kB\nAnonymous:        524288 kB\nSwap:             524288 kB\n"
        "ffff80000000-ffff90000000 rw-p 00000000 00:00 0                          [anon:libc_malloc]\n"
        "Size:             262144 kB\nRss:              204800 kB\nAnonymous:        204800 kB\nSwap:              51200 kB\n"
        "ffff90000000-ffff90400000 rw-s 12345000 00:05 900                        /dev/dri/renderD128\n"
        "Size:               4096 kB\nRss:                2048 kB\nAnonymous:             0 kB\nSwap:                  0 kB\n"
        "ffffa0000000-ffffa0100000 rw-p 00000000 00:00 0 \n"
        "Size:               1024 kB\nRss:                1024 kB\nAnonymous:          1000 kB\nSwap:                  8 kB\n";
    const ram_census::Maps m = ram_census::maps(text, lo, hi);
    expect(m.guest_maps == 2 && m.dmabuf_maps == 1 && m.other_maps == 4, "the mappings by where they are");
    expect(m.guest_rss == 3584 * MB && m.guest_anon == 3584 * MB && m.guest_swap == 2560 * MB,
           "the game's memory: resident and in swap, both of its mappings");
    expect(m.guest_dmabuf == 0, "the device's memory placed there: mapped, but not resident pages of the process");
    expect(m.other_anon == 200 * MB + 1000 * KB && m.other_swap == 50 * MB + 8 * KB, "the rest: anonymous and in swap");
    expect(m.other_file == 800 * KB + 24 * KB, "the rest's files: the program, and what of a mapping is not anonymous");
    expect(m.dri == 2 * MB, "the driver's device file mapped, apart");
    const ram_census::Maps none = ram_census::maps("", lo, hi);
    expect(none.guest_maps == 0 && none.other_maps == 0 && none.guest_rss == 0, "no text: nothing");
}

void drm() {
    ram_census::Drm d;
    const char* a = "pos:\t0\nflags:\t02100002\nmnt_id:\t25\ndrm-driver:\tmsm\ndrm-client-id:\t7\ndrm-engine-gpu:\t123456 ns\n"
                    "drm-cycles-gpu:\t99\ndrm-maxfreq-gpu:\t900000000 Hz\ndrm-total-memory:\t6291456 KiB\n"
                    "drm-shared-memory:\t1048576 KiB\ndrm-active-memory:\t4096 KiB\ndrm-resident-memory:\t5242880 KiB\n"
                    "drm-purgeable-memory:\t0\n";
    ram_census::add_fdinfo(d, a);
    ram_census::add_fdinfo(d, a);   // (the same client through another fd: once)
    ram_census::add_fdinfo(d, "pos:\t0\nflags:\t02\nmnt_id:\t3\n");   // (not a DRM file)
    expect(d.any && d.driver == "msm" && d.clients.size() == 1, "the driver, one client");
    expect(d.total == 6 * 1024 * MB && d.resident == 5 * 1024 * MB && d.shared == 1024 * MB && d.purgeable == 0,
           "its memory: the engine's time, cycles and clock left out; a client opened twice counted once");
    ram_census::add_fdinfo(d, "drm-driver:\tmsm\ndrm-client-id:\t8\ndrm-memory-vram:\t2 MiB\ndrm-total-system:\t1 MiB\n");
    expect(d.clients.size() == 2 && d.resident == 5 * 1024 * MB + 2 * MB && d.total == 6 * 1024 * MB + MB,
           "another client added: regions summed, the older drm-memory- as resident");
    expect(d.each.size() == 2 && d.each[0].id == 7 && d.each[0].total == 6 * 1024 * MB && d.each[0].resident == 5 * 1024 * MB &&
               d.each[1].id == 8 && d.each[1].total == MB && d.each[1].resident == 2 * MB,
           "each client's own, as added: which of them holds the memory");
    ram_census::Drm none;
    ram_census::add_fdinfo(none, "drm-driver:\tmsm\ndrm-client-id:\t1\ndrm-engine-gpu:\t5 ns\n");
    expect(!none.any && none.driver == "msm", "a driver that gives no memory statistics: none (said so in the log)");
}


#ifdef __linux__
std::string slurp(const char* path) {   // (as vk_memory.cpp's ram_slurp: the kernel's files read to their end)
    std::string s;
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return s;
    char buf[1 << 16];
    for (ssize_t r; (r = read(fd, buf, sizeof buf)) > 0;) s.append(buf, (size_t)r);
    close(fd);
    return s;
}

// ---- this machine's own /proc, as the kernel writes it: a window of 64 MB standing in for the guest's, 10 MB of it touched ----
void real_kernel() {
    const uint64_t n = 64 * MB, touched = 10 * MB;
    uint8_t* const w = (uint8_t*)mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (w == MAP_FAILED) return expect(false, "no window to try it in");
    madvise(w, n, MADV_NOHUGEPAGE);
    for (uint64_t i = 0; i < touched; i += 4096) w[i] = 1;
    const ram_census::Maps m = ram_census::maps(slurp("/proc/self/smaps").c_str(), (uint64_t)(uintptr_t)w, (uint64_t)(uintptr_t)w + n);
    expect(m.guest_maps >= 1 && m.other_maps > 5, "the real smaps: the window's mapping found, and the rest");
    if (m.guest_anon < touched || m.guest_anon > touched + MB)
        std::printf("  window: %llu KB resident, %llu KB anonymous\n", (unsigned long long)(m.guest_rss >> 10),
                    (unsigned long long)(m.guest_anon >> 10));
    expect(m.guest_anon >= touched && m.guest_anon <= touched + MB && m.guest_rss == m.guest_anon,
           "the real smaps: the window's 10 MB touched, resident and anonymous");
    expect(m.other_file > 0, "the real smaps: the program's own file mapped");
    const ram_census::Process self = ram_census::process(slurp("/proc/self/status").c_str());
    expect(self.ok && !self.name.empty() && self.rss >= touched && self.anon >= touched, "the real status: this process");
    const ram_census::System sys = ram_census::system(slurp("/proc/meminfo").c_str());
    expect(sys.total > 0 && sys.avail > 0 && sys.avail <= sys.total, "the real meminfo");
    munmap(w, n);
}
#endif

} // namespace

int main() {
    fields();
    system();
    zram();
    processes();
    smaps();
    drm();
#ifdef __linux__
    real_kernel();
#endif
    if (failures) return std::printf("%d failed\n", failures), 1;
    std::printf("ram census: ok\n");
    return 0;
}
