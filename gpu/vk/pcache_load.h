// SPDX-License-Identifier: GPL-3.0-or-later
// (longshot, issue 388) Whether the pipeline cache's file (cache/longshot_vk_pipelines.bin, 1.2 GB after a preparation) is read
// at the start (vk_draw.cpp pcache_init), where RX_VK_PCACHE_LOAD does not say, and how NVIDIA's own shader cache is found for
// that. Nothing of Vulkan here: tests/test_pcache_load.py.
//
// Read, the file is handed whole to vkCreatePipelineCache and the driver keeps its copy in the process's private memory for
// the session. On Linux NVIDIA's driver keeps its own shader cache too, with room for the preparation's pipelines
// (hos_main.c driver_cache_room: 10 GiB), and a program loaded at its create call finds its pipelines there as well. On a
// 16 GB PC that copy is what pushes the game into swap: measured on an RTX 3070 Ti, 16 GB, Linux, an eight-area tour with
// programs loaded at create, three runs each way, the game's peak (resident and swapped) 12.7 GB with the file and 11.9 GB
// without, its private memory and swap at the end 5.2 GB against 4.4 GB; fps the same. What it costs: a load at create
// takes its pipelines from the driver's cache instead (median 1.8 ms against 5.0 ms, on the load workers, not the frame).
//
// So the file is not read, by default, only where all of these hold, and read as before everywhere else:
// - programs are loaded at their create calls (in a start that loads them all before the title, the precompile builds its
//   pipelines into the cache object anyway, which then holds about what the file did: no saving, and the precompile took
//   twice as long, 29 s against 15 s);
// - Linux and NVIDIA's own driver (VK_DRIVER_ID_NVIDIA_PROPRIETARY: the one measured; Mesa's drivers, NVK among them, keep
//   their own disk caches, untested here);
// - 20 GB of RAM or less (16 GB PCs report about 15.7 GB);
// - NVIDIA's own cache is on and its folder holds at least kWarmMb (nvidia_cache below: a cache turned off, emptied or never
//   filled would leave every pipeline to be compiled in play, and there the file is what answers). Only NVIDIA's folder
//   counts: Mesa's, or another driver's, cannot answer NVIDIA's pipelines;
// - not a --prepare-shaders run, which builds into the file and saves it.
// RX_VK_PCACHE_LOAD=0 or 1 (or off/on): not read or read, whatever these say, a --prepare-shaders run included.
//
// (longshot) Turnip (Mesa's driver for Adreno) on such a PC too, its own cache standing for NVIDIA's (mesa_cache below: the game
// folder's cache/driver, hos_main.c driver_cache_shared), and there the preparation builds into the driver's cache alone as
// well, the file neither read nor written (kPreparingIntoDriver): on a 15 GB AYN Odin 3 the file read at the start (2.2 GB,
// handed whole to the driver, which copies it) came on top of 1.5 GB left available, and the preparation, building into the
// cache object every pipeline it then wrote out, was down to 86 MB available (2026-11-18). Turnip answered the notes from its
// own cache alone (48 of 48, the cold check's sample). Mesa's other drivers keep reading the file until they are measured.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace pcache_load {

constexpr uint32_t kSmallRamMb = 20480;
constexpr uint64_t kWarmMb = 512;

/// NVIDIA's own shader cache as the rule sees it: measured (its folder walked, or not there: 0), turned off, or not known (its
/// folder could not be walked through)
enum class Cache { kMeasured, kOff, kUnknown };

enum class Why { kSwitch, kPreparing, kStart, kNotLinuxNvidia, kRam, kDriverCacheOff, kDriverCacheUnknown, kColdDriverCache, kSmallPc,
                 kPreparingIntoDriver, kSmallPcTurnip };
struct Choice {
    bool load;
    Why why;
};

/// Whether everything but the driver's own cache already says not to read the file: its folder is walked only then.
/// sw: RX_VK_PCACHE_LOAD (-1 unset, 0 off, 1 on). linux_nvidia: Linux with NVIDIA's own driver. ram_mb: the PC's RAM (0: not
/// known). linux_turnip: Linux with Turnip, whose preparation looks at Mesa's cache too
constexpr bool needs_cache(int sw, bool preparing, bool load_at_create, bool linux_nvidia, uint32_t ram_mb, bool linux_turnip = false) {
    return sw < 0 && (!preparing || linux_turnip) && load_at_create && (linux_nvidia || linux_turnip) && ram_mb &&
           ram_mb <= kSmallRamMb;
}
/// cache, cache_mb: the driver's own cache (nvidia_cache, or mesa_cache with Turnip), looked at only where needs_cache()
constexpr Choice choose(int sw, bool preparing, bool load_at_create, bool linux_nvidia, uint32_t ram_mb, Cache cache,
                        uint64_t cache_mb, bool linux_turnip = false) {
    if (sw >= 0) return {sw == 1, Why::kSwitch};
    // (Turnip on a small PC: the preparation fills the driver's own cache, however empty it is now, as long as it is on)
    if (preparing)
        return linux_turnip && load_at_create && ram_mb && ram_mb <= kSmallRamMb && cache == Cache::kMeasured
                   ? Choice{false, Why::kPreparingIntoDriver} : Choice{true, Why::kPreparing};
    if (!load_at_create) return {true, Why::kStart};
    if (!linux_nvidia && !linux_turnip) return {true, Why::kNotLinuxNvidia};
    if (!ram_mb || ram_mb > kSmallRamMb) return {true, Why::kRam};
    if (cache == Cache::kOff) return {true, Why::kDriverCacheOff};
    if (cache != Cache::kMeasured) return {true, Why::kDriverCacheUnknown};
    if (cache_mb < kWarmMb) return {true, Why::kColdDriverCache};
    return {false, linux_turnip ? Why::kSmallPcTurnip : Why::kSmallPc};
}

/// Whether what is built only for the driver's cache goes through a pipeline cache of its own, let go after it, instead of the
/// one the file is read into (vk_draw.cpp DriverOnlyBuilds): where Turnip's own cache is the store, the file neither read nor
/// written. Held in that one, every pipeline's shaders stayed for the whole process and were never written
constexpr bool builds_into_driver(Why w) { return w == Why::kPreparingIntoDriver || w == Why::kSmallPcTurnip; }

/// For the log line: why the file was or was not read
constexpr const char* why_text(Why w) {
    switch (w) {
        case Why::kSwitch: return "RX_VK_PCACHE_LOAD says so";
        case Why::kPreparing: return "a --prepare-shaders run builds into it";
        case Why::kStart: return "programs are loaded before the title";
        case Why::kNotLinuxNvidia: return "not Linux with NVIDIA's own driver or Turnip";
        case Why::kRam: return "more than 20 GB of RAM, or the RAM not known";
        case Why::kDriverCacheOff: return "the driver's own shader cache is off";
        case Why::kDriverCacheUnknown: return "the driver's own shader cache could not be measured";
        case Why::kColdDriverCache: return "the driver's own shader cache is nearly empty";
        case Why::kSmallPc: return "20 GB of RAM or less, programs loaded at create: NVIDIA's own shader cache answers them";
        case Why::kPreparingIntoDriver:
            return "Turnip, 20 GB of RAM or less, programs loaded at create: the preparation builds into the driver's own shader "
                   "cache alone";
        case Why::kSmallPcTurnip:
            return "Turnip, 20 GB of RAM or less, programs loaded at create: the driver's own shader cache answers them";
    }
    return "";
}

/// The folder NVIDIA's Linux driver keeps its shader cache in, as its README gives it (OpenGL environment variables,
/// __GL_SHADER_DISK_CACHE_PATH): <path>/GLCache where __GL_SHADER_DISK_CACHE_PATH names a folder (the driver files it in GLCache
/// there: seen with driver 595; Steam names one per game, hos_main.c driver_cache_folder makes it); else
/// $XDG_CACHE_HOME/nvidia/GLCache, or $HOME/.cache/nvidia/GLCache without XDG_CACHE_HOME, or the earlier drivers'
/// $XDG_CACHE_HOME/.nv/GLCache and $HOME/.nv/GLCache in their place where those are there (the driver reuses them). Empty when
/// none of the three is set: the driver then keeps no cache
inline std::filesystem::path nvidia_cache_dir(const char* path, const char* xdg, const char* home) {
    namespace fs = std::filesystem;
    if (path && *path) return fs::path(path) / "GLCache";
    const char* base = xdg && *xdg ? xdg : home && *home ? home : nullptr;
    if (!base) return {};
    std::error_code ec;
    const fs::path earlier = fs::path(base) / ".nv" / "GLCache";
    if (fs::is_directory(earlier, ec)) return earlier;
    return base == xdg ? fs::path(base) / "nvidia" / "GLCache" : fs::path(base) / ".cache" / "nvidia" / "GLCache";
}

struct Measured {
    Cache cache;
    uint64_t bytes;
};
/// What a cache folder holds, every file under it: a folder that is not there holds 0 (a cache never filled); one that is not a
/// folder, or that cannot be walked through (a folder in it that cannot be read), is not known
inline Measured folder_bytes(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    if (dir.empty()) return {Cache::kUnknown, 0};
    std::error_code ec;
    const fs::file_status st = fs::status(dir, ec);
    if (st.type() == fs::file_type::not_found) return {Cache::kMeasured, 0};
    if (ec || st.type() != fs::file_type::directory) return {Cache::kUnknown, 0};
    uint64_t n = 0;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::none, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fe;
        if (!it->is_regular_file(fe)) continue;
        const uintmax_t size = it->file_size(fe);
        if (fe == std::errc::no_such_file_or_directory) continue;   // (gone since it was listed: the driver tidies its cache)
        if (fe) return {Cache::kUnknown, 0};
        n += size;
    }
    if (ec) return {Cache::kUnknown, 0};
    return {Cache::kMeasured, n};
}

/// NVIDIA's own shader cache for the rule. switched_off: __GL_SHADER_DISK_CACHE is an off value (the caller reads it as the
/// switches are read: longshot_env.h). path, xdg, home: __GL_SHADER_DISK_CACHE_PATH, XDG_CACHE_HOME, HOME. Off with no folder
/// to keep it in either. dir: the folder looked at, for the log
inline Measured nvidia_cache(bool switched_off, const char* path, const char* xdg, const char* home, std::string* dir = nullptr) {
    if (switched_off) return {Cache::kOff, 0};
    const std::filesystem::path d = nvidia_cache_dir(path, xdg, home);
    if (d.empty()) return {Cache::kOff, 0};
    if (dir) *dir = d.string();
    return folder_bytes(d);
}

/// Mesa's own shader cache for the rule (with Turnip): off where MESA_SHADER_CACHE_DISABLE says so (switched_off); else the
/// folder MESA_SHADER_CACHE_DIR names (dir: hos_main.c sets it to the game folder's cache/driver), or where Mesa keeps it
/// without one, XDG_CACHE_HOME's or ~/.cache's mesa_shader_cache, _db and _sf folders together. where: the folder, for the log
inline Measured mesa_cache(bool switched_off, const char* dir, const char* xdg, const char* home, std::string* where = nullptr) {
    namespace fs = std::filesystem;
    if (switched_off) return {Cache::kOff, 0};
    if (dir && *dir) {
        if (where) *where = dir;
        return folder_bytes(fs::path(dir));
    }
    const fs::path base = xdg && *xdg ? fs::path(xdg) : home && *home ? fs::path(home) / ".cache" : fs::path();
    if (base.empty()) return {Cache::kOff, 0};
    if (where) *where = (base / "mesa_shader_cache").string();
    uint64_t n = 0;
    for (const char* sub : {"mesa_shader_cache", "mesa_shader_cache_db", "mesa_shader_cache_sf"}) {
        const Measured m = folder_bytes(base / sub);
        if (m.cache != Cache::kMeasured) return m;
        n += m.bytes;
    }
    return {Cache::kMeasured, n};
}

} // namespace pcache_load
