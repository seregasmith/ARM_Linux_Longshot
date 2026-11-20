// SPDX-License-Identifier: GPL-3.0-or-later
// (longshot) Linux memory pressure (MemAvailable and SwapFree in /proc/meminfo): the textures' budget, compaction, idle imports
// and spare capture buffers give memory back before earlyoom or kernel direct reclaim acts. Nothing of Vulkan here:
// tests/test_linux_pressure.py.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace linux_pressure {

/// Default reserves on Linux:
/// - kDefaultAvailReserve (1536 MB, RX_VK_AVAIL_RESERVE_MB): physical RAM available without swapping (MemAvailable). On a 16 GB
///   Steam Deck OLED (~14.8 GB MemTotal), earlyoom sends SIGTERM at ~5% available (~750 MB) when swap is low, and direct reclaim
///   stalls texture uploads once MemAvailable dips below ~1-1.5 GB. Every VMA block, staging arena, spare capture buffer and
///   imported guest pool is kernel-pinned in physical RAM and cannot be swapped to zram or disk, so freeing them raises
///   MemAvailable directly.
/// - kDefaultCommitReserve (2560 MB, RX_VK_COMMIT_RESERVE_MB): MemAvailable + SwapFree, the Linux counterpart to Windows'
///   ullAvailPageFile commit reserve.
/// - kClearMargin (256 MB) and kBudgetFloor (512 MB): hysteresis and floor for texture_frame()'s budget hold.
/// - kModuleDropMaxRam (20 GB): post-precompile VkShaderModule release runs by default only on <= 20 GB RAM systems (16 GB
///   handhelds and PCs; RX_VK_DROP_MODULES=1 forces it on larger desktops, RX_VK_KEEP_MODULES=1 or RX_VK_DROP_MODULES=0 keeps them).
constexpr uint64_t kDefaultAvailReserve = 1536ull << 20;
constexpr uint64_t kDefaultCommitReserve = 2560ull << 20;
constexpr uint64_t kClearMargin = 256ull << 20;
constexpr uint64_t kBudgetFloor = 512ull << 20;
constexpr uint64_t kModuleDropMaxRam = 20ull << 30;

inline uint64_t reserve_from_env(const char* env, uint64_t def_bytes) {
    if (!env || !*env) return def_bytes;
    return (uint64_t)std::max(0, std::atoi(env)) << 20;
}

inline uint64_t avail_reserve(const char* env) { return reserve_from_env(env, kDefaultAvailReserve); }
inline uint64_t commit_reserve(const char* env) { return reserve_from_env(env, kDefaultCommitReserve); }

/// Parses MemAvailable and SwapFree (in bytes) from /proc/meminfo text, and optionally MemTotal and whether swap is configured
/// (SwapTotal > 0 or SwapFree > 0). Returns false when MemAvailable is not present; SwapFree defaults to 0 when absent.
inline bool parse_meminfo(const char* text, uint64_t* avail_bytes, uint64_t* swap_free_bytes, uint64_t* total_bytes = nullptr,
                          bool* has_swap = nullptr) {
    if (!text || !avail_bytes || !swap_free_bytes) return false;
    *avail_bytes = 0;
    *swap_free_bytes = 0;
    if (total_bytes) *total_bytes = 0;
    bool have_avail = false, seen_swap_total = false;
    uint64_t swap_total = 0;
    for (const char* p = text; *p;) {
        const char* end = std::strchr(p, '\n');
        const size_t len = end ? (size_t)(end - p) : std::strlen(p);
        unsigned long long kb = 0;
        if (len >= 13 && !std::memcmp(p, "MemAvailable:", 13) && std::sscanf(p + 13, "%llu", &kb) == 1) {
            *avail_bytes = (uint64_t)kb << 10;
            have_avail = true;
        } else if (len >= 9 && !std::memcmp(p, "SwapFree:", 9) && std::sscanf(p + 9, "%llu", &kb) == 1) {
            *swap_free_bytes = (uint64_t)kb << 10;
        } else if (len >= 10 && !std::memcmp(p, "SwapTotal:", 10) && std::sscanf(p + 10, "%llu", &kb) == 1) {
            swap_total = (uint64_t)kb << 10;
            seen_swap_total = true;
        } else if (total_bytes && len >= 9 && !std::memcmp(p, "MemTotal:", 9) && std::sscanf(p + 9, "%llu", &kb) == 1) {
            *total_bytes = (uint64_t)kb << 10;
        }
        if (!end) break;
        p = end + 1;
    }
    if (has_swap) *has_swap = !seen_swap_total || swap_total > 0 || *swap_free_bytes > 0;
    return have_avail;
}

/// Reads MemAvailable and SwapFree (and optionally MemTotal and has_swap) from path (or /proc/meminfo on Linux). Off Linux, only
/// an explicit path is read (for tests).
inline bool read_meminfo(uint64_t* avail_bytes, uint64_t* swap_free_bytes, const char* path = nullptr,
                         uint64_t* total_bytes = nullptr, bool* has_swap = nullptr) {
#if !defined(__linux__)
    if (!path || !*path) return false;
#endif
    const char* target = (path && *path) ? path : "/proc/meminfo";
    FILE* f = std::fopen(target, "rb");
    if (!f) return false;
    char buf[4096];
    const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    buf[n] = '\0';
    return parse_meminfo(buf, avail_bytes, swap_free_bytes, total_bytes, has_swap);
}

/// (longshot) This process's resident and swapped memory (VmRSS and VmSwap, in bytes) from /proc/self/status text. False when
/// VmRSS is not there; VmSwap is 0 when absent (no swap)
inline bool parse_self_status(const char* text, uint64_t* rss_bytes, uint64_t* swap_bytes) {
    if (!text || !rss_bytes || !swap_bytes) return false;
    *rss_bytes = *swap_bytes = 0;
    bool have_rss = false;
    for (const char* p = text; *p;) {
        const char* end = std::strchr(p, '\n');
        const size_t len = end ? (size_t)(end - p) : std::strlen(p);
        unsigned long long kb = 0;
        if (len >= 6 && !std::memcmp(p, "VmRSS:", 6) && std::sscanf(p + 6, "%llu", &kb) == 1) {
            *rss_bytes = (uint64_t)kb << 10;
            have_rss = true;
        } else if (len >= 7 && !std::memcmp(p, "VmSwap:", 7) && std::sscanf(p + 7, "%llu", &kb) == 1) {
            *swap_bytes = (uint64_t)kb << 10;
        }
        if (!end) break;
        p = end + 1;
    }
    return have_rss;
}

/// The same read from path (or /proc/self/status on Linux; off Linux only an explicit path, for tests)
inline bool read_self_status(uint64_t* rss_bytes, uint64_t* swap_bytes, const char* path = nullptr) {
#if !defined(__linux__)
    if (!path || !*path) return false;
#endif
    FILE* f = std::fopen(path && *path ? path : "/proc/self/status", "rb");
    if (!f) return false;
    char buf[8192];
    const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    buf[n] = '\0';
    return parse_self_status(buf, rss_bytes, swap_bytes);
}

/// The words a start-up log line ends with, from what the two files said: "; memory: this process 6210 MB (1180 MB more in swap),
/// 1490 MB available, 13270 MB of swap free". Empty when meminfo was not read (Windows)
inline std::string memory_words(bool have_meminfo, uint64_t avail, uint64_t swap_free, bool have_self, uint64_t rss, uint64_t swapped) {
    if (!have_meminfo) return std::string();
    char s[192];
    if (have_self)
        std::snprintf(s, sizeof s, "; memory: this process %llu MB (%llu MB more in swap), %llu MB available, %llu MB of swap free",
                      (unsigned long long)(rss >> 20), (unsigned long long)(swapped >> 20), (unsigned long long)(avail >> 20),
                      (unsigned long long)(swap_free >> 20));
    else
        std::snprintf(s, sizeof s, "; memory: %llu MB available, %llu MB of swap free", (unsigned long long)(avail >> 20),
                      (unsigned long long)(swap_free >> 20));
    return s;
}

/// How many bytes short of the reserves the system is: the larger of the physical-RAM shortfall (below avail_reserve_bytes) and,
/// when swap is enabled (has_swap), the total-memory-plus-swap shortfall (below commit_reserve_bytes). 0 when both reserves are
/// met or turned off (0).
constexpr uint64_t shortfall(uint64_t avail_bytes, uint64_t swap_free_bytes,
                             uint64_t avail_reserve_bytes = kDefaultAvailReserve,
                             uint64_t commit_reserve_bytes = kDefaultCommitReserve,
                             bool has_swap = true) {
    const uint64_t avail_over = avail_reserve_bytes > avail_bytes ? avail_reserve_bytes - avail_bytes : 0;
    const uint64_t total_free = avail_bytes <= UINT64_MAX - swap_free_bytes ? avail_bytes + swap_free_bytes : UINT64_MAX;
    const uint64_t commit_over = has_swap && commit_reserve_bytes > total_free ? commit_reserve_bytes - total_free : 0;
    return avail_over > commit_over ? avail_over : commit_over;
}

/// (longshot) RX_VK_PRESSURE_KEEP_S: while the hold below is on, the textures the frames drew in the last this many seconds are
/// kept (keep_floor), where swap can take what they hold (swap_short is false). On the AYN Odin 3 (15 GB of RAM, 15 GB of zram)
/// memory was short from the title screen on, the budget sat at the 512 MB floor all session while Kokiri Forest drew 1.1-1.3 GB
/// every few frames, and every pass over it let go of what had not been drawn for 60 frames (3 s at 20 fps): 16,177 textures
/// let go and made again within seconds, 2-15 a frame, freezes of 1.2-3.7 s, the cache at 1.4 GB all the same. Default 20 s;
/// 0: off (the floor alone, as before); at most 120 (and at most what PresentClock's notes span: 64 s at 60 fps)
constexpr uint32_t kDefaultKeepSeconds = 20;
inline uint32_t keep_seconds(const char* env) {
    if (!env || !*env) return kDefaultKeepSeconds;
    const int v = std::atoi(env);
    return v <= 0 ? 0u : (uint32_t)std::min(v, 120);
}

/// Whether memory and swap together are short (below the commit reserve, MemAvailable + SwapFree), or there is no swap at all:
/// then nothing takes what kept textures hold, earlyoom is near (the Steam Deck OLED the hold was made for), and the hold keeps
/// only its floor, as before. Once short it stays short until the reserve is cleared by margin_bytes (as the hold itself leaves),
/// so a total hovering at the reserve does not flip the budget between the keep and the floor at every sample. The reserve
/// turned off (RX_VK_COMMIT_RESERVE_MB=0) is the default here: the keep's own emergency does not go with that rule
constexpr bool swap_short(bool was_short, uint64_t avail_bytes, uint64_t swap_free_bytes, uint64_t commit_reserve_bytes,
                          bool has_swap, uint64_t margin_bytes = kClearMargin) {
    if (!has_swap) return true;
    const uint64_t reserve = commit_reserve_bytes ? commit_reserve_bytes : kDefaultCommitReserve;
    const uint64_t total = avail_bytes <= UINT64_MAX - swap_free_bytes ? avail_bytes + swap_free_bytes : UINT64_MAX;
    return total < (was_short ? reserve + margin_bytes : reserve);
}

/// What the hold keeps under the textures' budget: the bytes the frames drew in the keep window (with their allocations' padding,
/// as total_bytes counts them) and a quarter more, at least floor_bytes. A pass over the budget lets go down to 7/8 of it (1/7
/// more), and the window grows between samples (a tenth): under that, the eviction ate into the window again. The eviction
/// itself also leaves what was drawn in the window (linux_keep_presents in vk_texture.cpp)
constexpr uint64_t keep_floor(uint64_t drawn_bytes, uint64_t floor_bytes = kBudgetFloor) {
    const uint64_t keep = drawn_bytes + drawn_bytes / 4;
    return keep > floor_bytes ? keep : floor_bytes;
}

/// The presents' times, noted at each of the hold's samples (every 30 presents): how many presents the last `seconds` took, so
/// a window in seconds reads the textures' last frame drawn. 128 notes: 64 s of history at 60 fps, 192 s at 20 fps
struct PresentClock {
    static constexpr int kNotes = 128;
    uint64_t frame[kNotes] = {};
    int64_t ms[kNotes] = {};
    int count = 0, next = 0;
    constexpr void note(uint64_t frame_no, int64_t now_ms) {
        frame[next] = frame_no, ms[next] = now_ms;
        next = (next + 1) % kNotes;
        if (count < kNotes) count++;
    }
    /// The presents since `seconds` ago: back to the newest note at least that old; with none that old, to the oldest note
    /// (all that is known); with no note yet, every present
    constexpr uint64_t presents_since(uint64_t frame_no, int64_t now_ms, uint32_t seconds) const {
        if (!count) return frame_no;
        uint64_t since = frame_no;
        for (int i = 0; i < count; i++) {   // (newest first)
            const int k = (next - 1 - i + kNotes) % kNotes;
            since = frame[k];
            if (now_ms - ms[k] >= (int64_t)seconds * 1000) break;
        }
        return frame_no > since ? frame_no - since : 0;
    }
};

/// Whether post-precompile VkShaderModules should be let go at pipeline_cache_play(): off when keep_env (RX_VK_KEEP_MODULES) is
/// on or drop_env (RX_VK_DROP_MODULES) is explicitly off ("0", "off", "false", "no"); on when drop_env is on or when the machine
/// has <= kModuleDropMaxRam (20 GB) of physical RAM (16 GB handhelds and PCs) or already has a memory shortfall at start-up.
/// 32-64 GB Linux desktops keep their modules by default so new pipeline variants in play never read the disk.
inline bool should_drop_modules(uint64_t total_ram_bytes, uint64_t current_shortfall, const char* keep_env,
                                const char* drop_env = nullptr) {
    if (keep_env && *keep_env && std::strcmp(keep_env, "0") != 0 && std::strcmp(keep_env, "off") != 0 &&
        std::strcmp(keep_env, "false") != 0 && std::strcmp(keep_env, "no") != 0)
        return false;
    if (drop_env && *drop_env) {
        if (!std::strcmp(drop_env, "0") || !std::strcmp(drop_env, "off") || !std::strcmp(drop_env, "false") ||
            !std::strcmp(drop_env, "no"))
            return false;
        return true;
    }
    return (total_ram_bytes > 0 && total_ram_bytes <= kModuleDropMaxRam) || current_shortfall > 0;
}

/// Hysteresis state for texture_frame()'s Linux memory-pressure budget hold:
/// - Enters pressure when shortfall(avail, swap_free, avail_res, commit_res, has_swap) > 0, capturing the texture total at entry
///   (baseline_total = total_bytes) and holding held_budget = max(floor_bytes, baseline_total - over).
/// - While active, if shortfall grows (MemAvailable drops further), held_budget steps down to max(floor_bytes, baseline_total - over),
///   never ratcheting off the already-shrunk total_bytes on each frame while VMA compaction is still freeing blocks.
/// - Once both reserves are cleared by kClearMargin (256 MB), steps held_budget back up by margin_bytes per sample and leaves
///   active once it reaches baseline_total, preventing flapping or an immediate full-cut regrowth spike.
/// - (longshot) keep_bytes (keep_floor, RX_VK_PRESSURE_KEEP_S): while active, the budget given is never under it. It is not
///   kept in held_budget: as the frames draw less (another area), what they no longer draw may go again
struct PressureHold {
    bool active = false;
    uint64_t baseline_total = 0;
    uint64_t held_budget = UINT64_MAX;
    uint64_t last_over = 0;

    constexpr uint64_t step(uint64_t total_bytes, uint64_t avail_bytes, uint64_t swap_free_bytes,
                            uint64_t avail_res = kDefaultAvailReserve, uint64_t commit_res = kDefaultCommitReserve,
                            uint64_t margin_bytes = kClearMargin, uint64_t floor_bytes = kBudgetFloor,
                            bool has_swap = true, uint64_t keep_bytes = 0) {
        const uint64_t over = shortfall(avail_bytes, swap_free_bytes, avail_res, commit_res, has_swap);
        if (!active) {
            if (!over) {
                last_over = 0;
                return UINT64_MAX;
            }
            active = true;
            baseline_total = total_bytes;
            last_over = over;
            const uint64_t cut = baseline_total > over ? baseline_total - over : 0;
            held_budget = std::max(cut, floor_bytes);
            return std::max(held_budget, keep_bytes);
        }
        const uint64_t clear_avail = avail_res ? avail_res + margin_bytes : 0;
        const uint64_t clear_commit = commit_res ? commit_res + margin_bytes : 0;
        if (shortfall(avail_bytes, swap_free_bytes, clear_avail, clear_commit, has_swap) == 0) {
            if (margin_bytes && held_budget <= UINT64_MAX - margin_bytes && held_budget + margin_bytes < baseline_total) {
                held_budget += margin_bytes;
                last_over = baseline_total > held_budget ? baseline_total - held_budget : 0;
                return std::max(held_budget, keep_bytes);
            }
            active = false;
            baseline_total = 0;
            held_budget = UINT64_MAX;
            last_over = 0;
            return UINT64_MAX;
        }
        if (over > last_over) {
            last_over = over;
            const uint64_t cut = baseline_total > over ? baseline_total - over : 0;
            held_budget = std::min(held_budget, std::max(cut, floor_bytes));
        }
        return std::max(held_budget, keep_bytes);
    }
};

/// How many bytes of idle unmirrored guest-pool imports imports_give_back() should unpin under a Linux memory shortfall,
/// counting imports already queued for destruction (pending_dead_bytes) as given back.
constexpr uint64_t import_want(uint64_t shortfall_bytes, uint64_t pending_dead_bytes) {
    return shortfall_bytes > pending_dead_bytes ? shortfall_bytes - pending_dead_bytes : 0;
}

/// What imports_give_back() may still unpin in this pressure episode: want, at most the deepest shortfall of the episode less what
/// it has unpinned for it already. The texture cut, the staging arenas and malloc_trim cover the same bytes, and asking for the
/// whole shortfall again at every sample unpinned the idle imports over and over while the episode lasted
constexpr uint64_t episode_want(uint64_t want, uint64_t deepest_shortfall, uint64_t given_in_episode) {
    const uint64_t room = deepest_shortfall > given_in_episode ? deepest_shortfall - given_in_episode : 0;
    return want < room ? want : room;
}

} // namespace linux_pressure
