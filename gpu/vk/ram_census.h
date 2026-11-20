// (arm64 port, longshot) Where the RAM goes, for the log (RX_VK_LOG_RAM: gpu/vk/vk_memory.cpp ram_census_*). On the AYN Odin 3
// (15 GB, zram swap) Kokiri Forest left 340-540 MB available with 6 GB in swap, and the textures' budget held below what the
// frames draw; the renderer's own lines (the driver's heap, the textures, the process's private memory) leave out the game's
// memory as against the rest of the process, what is in swap, what zram's compressed pages cost, and the other programs
// (Steam, its web helper, gamescope). Parsers of the kernel's text, no file reading: /proc/meminfo, zram's mm_stat, a
// process's status, /proc/self/smaps split at the guest window, and the DRM driver's per-client memory in fdinfo
// (Documentation/gpu/drm-usage-stats.rst). tests/test_ram_census.py checks them on made-up text.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ram_census {

/// The number after `key` at a line's start in /proc text ("Key:   123 kB"), in bytes (kB: times 1024; no unit: as is).
/// False when no line has it
inline bool field(const char* text, const char* key, uint64_t* bytes) {
    const size_t k = std::strlen(key);
    for (const char* p = text; p && *p;) {
        if (!std::strncmp(p, key, k)) {
            unsigned long long v = 0;
            char unit[8] = "";
            if (std::sscanf(p + k, "%llu %7s", &v, unit) >= 1) {
                *bytes = (!std::strcmp(unit, "kB") || !std::strcmp(unit, "KiB")) ? (uint64_t)v << 10
                         : !std::strcmp(unit, "MiB")                             ? (uint64_t)v << 20
                                                                                 : (uint64_t)v;
                return true;
            }
        }
        p = std::strchr(p, '\n');
        if (p) p++;
    }
    return false;
}

/// The whole machine (/proc/meminfo); a field the kernel has not: 0
struct System {
    uint64_t total = 0, avail = 0, free = 0, cached = 0, shmem = 0, anon = 0, kreclaim = 0, sunreclaim = 0,
             unevictable = 0, swap_total = 0, swap_free = 0, swap_cached = 0;
};
inline System system(const char* meminfo) {
    System s;
    field(meminfo, "MemTotal:", &s.total), field(meminfo, "MemAvailable:", &s.avail), field(meminfo, "MemFree:", &s.free);
    field(meminfo, "Cached:", &s.cached), field(meminfo, "Shmem:", &s.shmem), field(meminfo, "AnonPages:", &s.anon);
    field(meminfo, "KReclaimable:", &s.kreclaim), field(meminfo, "SUnreclaim:", &s.sunreclaim);
    field(meminfo, "Unevictable:", &s.unevictable), field(meminfo, "SwapTotal:", &s.swap_total);
    field(meminfo, "SwapFree:", &s.swap_free), field(meminfo, "SwapCached:", &s.swap_cached);
    return s;
}

/// zram's devices (/sys/block/zram*/mm_stat, added one by one): the data swapped there, its compressed size, and the RAM the
/// device takes for it (mem_used_total)
struct Zram {
    int devices = 0;
    uint64_t data = 0, compressed = 0, ram = 0;
};
inline void add_mm_stat(Zram& z, const char* mm_stat) {
    unsigned long long orig = 0, compr = 0, used = 0;
    if (std::sscanf(mm_stat, "%llu %llu %llu", &orig, &compr, &used) != 3) return;
    z.devices++, z.data += orig, z.compressed += compr, z.ram += used;
}

/// A process (/proc/<pid>/status): its name, resident memory by kind, and what it has in swap. ok: it has a VmRSS (a kernel
/// thread has none)
struct Process {
    std::string name;
    uint64_t rss = 0, anon = 0, file = 0, shmem = 0, swap = 0;
    bool ok = false;
};
inline Process process(const char* status) {
    Process p;
    if (const char* n = std::strstr(status, "Name:")) {
        n += 5;
        while (*n == ' ' || *n == '\t') n++;
        const char* e = n;
        while (*e && *e != '\n') e++;
        p.name.assign(n, (size_t)(e - n));
    }
    p.ok = field(status, "VmRSS:", &p.rss);
    field(status, "RssAnon:", &p.anon), field(status, "RssFile:", &p.file), field(status, "RssShmem:", &p.shmem);
    field(status, "VmSwap:", &p.swap);
    return p;
}

/// This process's mappings (/proc/self/smaps) split at the guest window, [lo, hi) in host addresses: the game's memory (its
/// anonymous pages resident and in swap; the device's memory placed over its pools, by mapping, apart), and the rest of the
/// process (the renderer, the host, the driver's and the libraries' memory: anonymous, files and in swap, and the driver's
/// device files mapped)
struct Maps {
    uint64_t guest_rss = 0, guest_anon = 0, guest_swap = 0, guest_dmabuf = 0, guest_maps = 0, dmabuf_maps = 0;
    uint64_t other_anon = 0, other_file = 0, other_swap = 0, dri = 0, other_maps = 0;
};
inline Maps maps(const char* smaps, uint64_t lo, uint64_t hi) {
    Maps m;
    enum { kNone, kGuest, kDmabuf, kDri, kOther } kind = kNone;
    for (const char* p = smaps; p && *p;) {
        const char* e = std::strchr(p, '\n');
        const size_t len = e ? (size_t)(e - p) : std::strlen(p);
        unsigned long long a = 0, b = 0;
        int used = 0;
        // (a mapping's own line: "start-end perms offset dev inode path"; the lines after it are "Key: n kB")
        if (std::sscanf(p, "%llx-%llx %*s %*s %*s %*s%n", &a, &b, &used) == 2 && used > 0 && (size_t)used <= len) {
            std::string path(p + used, len - (size_t)used);
            path.erase(0, path.find_first_not_of(" \t"));
            const bool inside = a >= lo && a < hi;
            const bool dmabuf = path.find("dmabuf") != std::string::npos;
            kind = inside ? (dmabuf ? kDmabuf : kGuest) : path.rfind("/dev/dri/", 0) == 0 ? kDri : kOther;
            (kind == kGuest ? m.guest_maps : kind == kDmabuf ? m.dmabuf_maps : m.other_maps)++;
        } else if (kind != kNone) {
            uint64_t v = 0;
            const std::string line(p, len);
            if (!line.compare(0, 4, "Rss:") && field(line.c_str(), "Rss:", &v)) {
                if (kind == kGuest) m.guest_rss += v;
                else if (kind == kDmabuf) m.guest_dmabuf += v;
                else if (kind == kDri) m.dri += v;
                else m.other_file += v;   // (the anonymous part taken off below, from Anonymous:)
            } else if (!line.compare(0, 10, "Anonymous:") && field(line.c_str(), "Anonymous:", &v)) {
                if (kind == kGuest) m.guest_anon += v;
                else if (kind == kOther) m.other_anon += v, m.other_file -= std::min(m.other_file, v);
            } else if (!line.compare(0, 5, "Swap:") && field(line.c_str(), "Swap:", &v)) {
                if (kind == kGuest || kind == kDmabuf) m.guest_swap += v;
                else m.other_swap += v;
            }
        }
        p = e ? e + 1 : nullptr;
    }
    return m;
}

/// The DRM driver's memory for this process's clients (fdinfo of its device files, added one by one; a client opened twice,
/// the same drm-client-id, counted once): the objects it has (total), those in memory (resident), shared with other
/// processes, and those it may drop (purgeable). The keys are drm-<kind>-<region>, summed over the regions; drm-memory-<region>
/// (older kernels) counts as resident
struct Drm {
    struct Client {
        unsigned long long id = 0;
        uint64_t total = 0, resident = 0;
    };
    std::string driver;
    std::vector<unsigned long long> clients;
    std::vector<Client> each;   // (by client, as added: which of them holds the memory)
    uint64_t total = 0, resident = 0, shared = 0, purgeable = 0;
    bool any = false;
};
inline void add_fdinfo(Drm& d, const char* fdinfo) {
    const char* drv = std::strstr(fdinfo, "drm-driver:");
    if (!drv) return;
    unsigned long long id = 0;
    if (const char* c = std::strstr(fdinfo, "drm-client-id:")) {
        id = std::strtoull(c + 14, nullptr, 10);
        if (std::find(d.clients.begin(), d.clients.end(), id) != d.clients.end()) return;
        d.clients.push_back(id);
    }
    if (d.driver.empty()) {
        drv += 11;
        while (*drv == ' ' || *drv == '\t') drv++;
        const char* e = drv;
        while (*e && *e != '\n') e++;
        d.driver.assign(drv, (size_t)(e - drv));
    }
    Drm::Client mine{id, 0, 0};
    for (const char* p = fdinfo; p && *p;) {
        const char* e = std::strchr(p, '\n');
        const std::string line(p, e ? (size_t)(e - p) : std::strlen(p));
        const size_t colon = line.find(':');
        uint64_t v = 0;
        if (!line.compare(0, 4, "drm-") && colon != std::string::npos && field(line.c_str(), line.substr(0, colon + 1).c_str(), &v)) {
            const std::string key = line.substr(4, colon - 4);
            if (!key.compare(0, 6, "total-")) d.total += v, mine.total += v, d.any = true;
            else if (!key.compare(0, 9, "resident-") || !key.compare(0, 7, "memory-")) d.resident += v, mine.resident += v, d.any = true;
            else if (!key.compare(0, 7, "shared-")) d.shared += v;
            else if (!key.compare(0, 10, "purgeable-")) d.purgeable += v;
        }
        p = e ? e + 1 : nullptr;
    }
    d.each.push_back(mine);
}

/// The other processes by name (Steam's web helper is a dozen of them): resident and in swap, the most first, n of them
struct Group {
    std::string name;
    uint64_t rss = 0, swap = 0;
    int count = 0;
};
inline std::vector<Group> top(const std::vector<Process>& procs, size_t n) {
    std::vector<Group> g;
    for (const Process& p : procs) {
        if (!p.ok) continue;
        auto it = std::find_if(g.begin(), g.end(), [&](const Group& x) { return x.name == p.name; });
        if (it == g.end()) g.push_back({p.name, 0, 0, 0}), it = g.end() - 1;
        it->rss += p.rss, it->swap += p.swap, it->count++;
    }
    std::sort(g.begin(), g.end(), [](const Group& a, const Group& b) { return a.rss + a.swap > b.rss + b.swap; });
    if (g.size() > n) g.resize(n);
    return g;
}

} // namespace ram_census
