// (longshot) Threaded rendering, the Insert menu's row (vk_menu.cpp, threaded_rendering= in longshot_options.ini): Auto, On or
// Off, as one rule every reader shares: the menu (vk_menu.cpp threaded_choice resolves it once, at the start, and hands each
// reader its half: menu_render_thread, menu_recorder_thread), the replay (vk_replay.cpp render_thread_mode), the recorder
// (vk_recorder.h mode_from_env), the host's present poll (HOS_OPT_RENDER_THREAD, host/nvn_native.c, through async_on) and the
// checks (tests/test_threaded_rendering.py compiles this file alone).
//
//   Off    the replay on the game's submitting thread, the recorder as RX_VK_RECORDER chooses it: as before the row
//   On     the render thread (what RX_VK_ASYNC=1 gives: the replay on a thread of its own, the game a frame ahead of it) and
//          the recorder thread (what RX_VK_RECORDER=thread gives) together, whatever the CPU
//   Auto   (the default) decided from the physical cores among the logical processors the process may run on (its
//          affinity mask, not the PC's count; a core with two usable SMT siblings counts once: vk_progcache.cpp usable_cores,
//          cores_of below), in two tiers:
//            kAutoCores (4) or more: the render thread and the recorder thread, as On;
//            fewer: off, as Off.
//          Where only buffer-pool mirrors are absent (integrated GPUs where !f.discrete(), or RX_VK_MIRROR=0), the render
//          thread stays off while without_mirrors() runs the recorder thread alone with serial replay at
//          kAutoRecorderOnlyCores (6) or more physical cores, and leaves 4- and 5-core systems off.
//          And off where the mode is not safe, whatever the CPU (the caller says so: the capture off, RX_VK_LOW_SPEC, a
//          frame dump, a benchmark replay). All of that behind kAutoEnabled, below, which is on: turned off, Auto is Off on
//          every PC.
//
// The tiers as first measured (18 Nov 2026: a Ryzen 9 9950X3D with SMT off, held to N cores by its affinity, Castle Town's
// market entrance, two runs each; fps, then the share of frames of 16.7 ms or more):
//
//   cores   serial replay (Off)        the render thread alone     the render thread and the recorder thread
//   4       65.5 / 65.0, 12.7 / 15.0%  67.6 / 67.9, 7.7 / 8.7%     60.9 / 62.1, 34.7 / 31.5%
//   6       67.4 / 68.4, 6.9 / 4.5%    69.5 / 69.0, 3.3 / 4.2%     72.9 / 72.4, 1.9 / 2.4%
//   8       67.0 / 66.9, 5.5 / 6.6%    69.9 / 69.9, 3.1 / 2.9%     72.8 / 73.5, 1.4 / 1.4%
//
// So on 4 cores the render thread alone was the best of the three and the recorder thread beside it the worst (a third of
// the frames long); from 6 cores both threads were the best, and Auto had a third tier, the render thread alone on 4 or 5
// cores. 16 cores ran both at about 75 fps. Before, Auto was both threads from 12 logical processors and off below, which
// left CPUs of 6 to 11 cores without SMT serial and counted a 4-core, 8-thread CPU (a Steam Deck) as 8: it is 4 here.
//
// That table is from before the threads were rewritten (#1763, 20 Nov) and before the recorder took whole parts, ended and
// submitted them, and waited on its ring after a 10 us spin where it had spun 20-200 us (#1866, 21 Nov). On that code
// (22 Nov 2026, main dd5ffe6a; a Ryzen 7 5700X3D, 8 cores / 16 threads, held to 4 of its cores by the process's affinity
// mask: masks of an 8-core CPU, not 4-core CPUs; an RX 9070 XT on AMD's 26.11.2; Hyrule Field turning, the frame rate held
// only by HOS_FPS_MAX=240, with a fixed light GPU load beside the game; three alternating pairs, medians, a diagnostic
// result) the recorder thread beside the render thread was the faster on 4 cores, with and without their SMT siblings, and
// no tier is left for the render thread alone:
//
//   mask                   the render thread alone      the render thread and the recorder thread
//   4 cores, 8 threads     64.6 fps, p99 23.1 ms        78.7 fps, p99 18.6 ms
//   4 cores, 4 threads     78.7 fps, p99 18.9 ms        89.2 fps, p99 16.1 ms
//
// Real 4-core CPUs (their own cache, no idle cores beside them for the driver and the system) and the Steam Deck are to be
// measured.
//
// The recorder thread was taken out once (19 Nov 2026) and is back, from 4 cores since 22 Nov: without it the render thread
// alone makes every one of the driver's calls, and on AMD's drivers (about twice NVIDIA's CPU time a call) the market's
// densest view, ~2,500 draws a frame, fell 16-25% behind main on an RX 7900 XTX, RX 9070 XT and RX 9060 XT. Its known costs:
// woken 20-25 times a frame, its short sleeps let an i7-9700K's clock control slow every core (Kokiri Forest 15 fps against
// 48), and kept spinning it cost a Ryzen 9 7900X 3-11%. The segment workers, which recorded a big command buffer's segments
// in parallel, stay removed (19 Nov 2026: 3-35% slower on every PC measured).
//
// The environment's switches stay the overrides they were, for testing: RX_VK_ASYNC set chooses the render thread and
// RX_VK_RECORDER set the recorder, each over its half of the row; unset, the row decides. Read once, at the start: a change of
// the row takes effect at the next start (the replay's async_on and the recorder's mode are read once).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace mkwvk::threaded {

enum { AUTO = 0, ON = 1, OFF = 2 };   // (the row's values, in V-Sync's order: Auto, On, Off)

/// THE GATE: Auto's one switch. True: Auto follows the rule below (its tiers by cores, and off where unsafe), so a PC with 4
/// cores for the game runs the render thread and the recorder thread by default. False: Auto is Off
/// everywhere and the row changes nobody's default; nothing else needs to change to turn Auto off again. Still open with it
/// on: the game's large GPU-written tables (Hyrule Field's culling tables) can be read a frame old behind the game.
constexpr bool kAutoEnabled = true;
/// (longshot) Auto's tier, in physical cores the process may run on (the game's own threads, the render thread and the
/// recorder beside one another): from kAutoCores the render thread and the recorder thread. On 4 cores on 18 Nov the render
/// thread alone was 67.6 / 67.9 fps against 60.9 / 62.1 for both, and 65.5 / 65.0 serial; on 22 Nov's code both threads
/// were +22 % on a mask of 4 cores and 8 threads and +13 % on 4 of 4 (the tables above). The count follows the process's
/// affinity mask, not the PC's cores: a 4-core, 8-thread CPU held to 2 cores' 4 siblings counts 2. Where the core masks
/// cannot be read, usable_cores counts logical processors instead (its fallback, older than this tier)
constexpr unsigned kAutoCores = 4;
/// (longshot) Physical-core threshold for running the recorder thread alone under Auto when buffer-pool mirrors are absent
/// (integrated GPUs where !f.discrete(), or RX_VK_MIRROR=0): only the render thread runs a frame ahead of replay and needs
/// buffer-pool mirrors, while the recorder thread replays synchronously under replay_mu and offloads only Vulkan command
/// recording and submits (RX_VK_RECORDER_PARTS). Holding this tier at 6 physical cores leaves 4- and 5-core non-mirrored
/// handhelds (such as the Steam Deck) unchanged until tested on that hardware
inline constexpr unsigned kAutoRecorderOnlyCores = 6;

enum class Why {
    On,                 // the row says On
    Off,                // the row says Off
    AutoGate,           // Auto, kAutoEnabled off: off everywhere
    AutoUnsafe,         // Auto, and the mode is not safe on this run
    AutoFewCores,       // Auto, fewer than kAutoCores cores (or their count unknown: 0): off
    AutoRecorderOnly,   // Auto, kAutoRecorderOnlyCores or more, pools have no mirrors: recorder thread alone
    AutoBothThreads,    // Auto, kAutoCores or more: the render thread and the recorder thread
};
/// The row's answer in two halves, each read by its own reader. With buffer-pool mirrors both threads come together; without
/// mirrors, without_mirrors() can pick the recorder thread alone at kAutoRecorderOnlyCores or more physical cores
struct Choice {
    bool render_thread;     // the render thread, RX_VK_ASYNC=1's (vk_replay.cpp render_thread_mode; the host's poll by it)
    bool recorder_thread;   // the recorder thread, RX_VK_RECORDER=thread's (vk_recorder.h mode_from_env)
    Why why;
};

/// What Auto picks for `cores` physical cores where the mode is safe, the gate aside
constexpr Choice auto_picks(unsigned cores) {
    if (cores >= kAutoCores) return {true, true, Why::AutoBothThreads};
    return {false, false, Why::AutoFewCores};
}

/// The rule, from the row's value, the physical cores the process may run on and whether the mode is unsafe on this run: no
/// environment, no clock, for the checks. `auto_enabled`: the gate (the checks pass it both ways). A value that is none of
/// the row's is Auto
constexpr Choice choose(int setting, unsigned cores, bool unsafe, bool auto_enabled = kAutoEnabled) {
    if (setting == ON) return {true, true, Why::On};
    if (setting == OFF) return {false, false, Why::Off};
    if (!auto_enabled) return {false, false, Why::AutoGate};
    if (unsafe) return {false, false, Why::AutoUnsafe};
    return auto_picks(cores);
}

/// (longshot) Auto picked the threads: the caller's last safety check, one that reads the device (the buffer pools'
/// mirrors), is asked only then
constexpr bool auto_picked(const Choice& c) { return c.why == Why::AutoBothThreads; }

/// (longshot) Auto's fallback when buffer-pool mirrors are absent (integrated GPUs where !f.discrete(), or RX_VK_MIRROR=0):
/// only the render thread runs a frame ahead of replay and needs mirrors, whereas the recorder thread replays synchronously
/// under replay_mu and offloads only Vulkan command recording and submits (RX_VK_RECORDER_PARTS). Keep the render thread off;
/// at kAutoRecorderOnlyCores (6) or more physical cores run the recorder thread alone, and below that turn both off (leaving
/// 4- and 5-core non-mirrored handhelds such as the Steam Deck unchanged)
constexpr Choice without_mirrors(const Choice& c, unsigned cores) {
    if (c.why != Why::AutoBothThreads) return c;
    if (cores >= kAutoRecorderOnlyCores) return {false, true, Why::AutoRecorderOnly};
    return {false, false, Why::AutoUnsafe};
}

/// (longshot) Auto's count: the physical cores among the logical processors the process may run on. `core_masks`: one mask
/// a physical core, of its logical processors (GetLogicalProcessorInformation's RelationProcessorCore entries, an SMT core's
/// two siblings in one mask; on Linux host/posix's, from sysfs core_id and physical_package_id), `n` of them; `usable`: the
/// affinity mask. A core counts once when any of its logical processors is usable, so a 4-core, 8-thread CPU is 4. 0: none
/// counted (nothing read, or no core in the mask): the caller then counts logical processors, as before
constexpr unsigned cores_of(const uint64_t* core_masks, unsigned n, uint64_t usable) {
    unsigned cores = 0;
    for (unsigned i = 0; i < n; i++) cores += (core_masks[i] & usable) != 0;
    return cores;
}

/// (longshot) The start's line for the choice (vk_menu.cpp logs it once, after "vk: Threaded rendering: "): `cores` Auto's
/// count, `cpus` the logical processors the process may run on, `unsafe` what made Auto not safe (AutoUnsafe only)
inline void describe(char* out, size_t size, const Choice& c, unsigned cores, unsigned cpus, const char* unsafe) {
    switch (c.why) {
    case Why::On: std::snprintf(out, size, "On (the render thread and the recorder thread)"); return;
    case Why::Off: std::snprintf(out, size, "Off"); return;
    case Why::AutoGate:
        std::snprintf(out, size, "Auto: off (Auto is turned off in this build; %u cores of %u logical processors, its rule is "
                      "the render thread and the recorder thread from %u cores)", cores, cpus, kAutoCores);
        return;
    case Why::AutoUnsafe: std::snprintf(out, size, "Auto: off (not safe on this run: %s)", unsafe ? unsafe : "?"); return;
    case Why::AutoFewCores:
        std::snprintf(out, size, "Auto: off (%u cores of %u logical processors; the render thread and the recorder thread "
                      "from %u cores)", cores, cpus, kAutoCores);
        return;
    case Why::AutoRecorderOnly:
        std::snprintf(out, size,
                      "Auto: the recorder thread only (%u cores of %u logical processors; the render thread off: %s)",
                      cores, cpus, unsafe ? unsafe : "?");
        return;
    case Why::AutoBothThreads:
        std::snprintf(out, size, "Auto: on, the render thread and the recorder thread (%u cores of %u logical processors)",
                      cores, cpus);
        return;
    }
    std::snprintf(out, size, "?");
}

} // namespace mkwvk::threaded
