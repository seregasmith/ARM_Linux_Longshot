"""The Insert menu's Threaded rendering (Auto / On / Off): its rule (gpu/vk/threaded_rendering.h), Auto's count of cores
(gpu/vk/vk_progcache.cpp usable_cores) and how the row reaches the replay, the recorder and the host. Compiled checks of
the rule, the count and the readers, plus source checks; no game, no GPU.

The rule is one function every reader shares: On is the render thread and the recorder thread together, Off neither, and
Auto picks by the physical cores among the logical processors the process may run on (a core's SMT siblings counted
once): 4 or more cores both threads, fewer off. Where only buffer-pool mirrors are absent (integrated GPUs or
RX_VK_MIRROR=0), the render thread stays off and without_mirrors() runs the recorder thread alone with serial replay
at 6 or more physical cores (kAutoRecorderOnlyCores), leaving 4- and 5-core non-mirrored systems off. The tiers were
three on 18 Nov 2026 (a Ryzen 9 9950X3D with SMT off held
to N cores, Castle Town's market: the render thread alone on 4 or 5, both threads 60.9 / 62.1 fps there against 67.6 /
67.9); on the code after #1763 and #1866 (22 Nov 2026, a Ryzen 7 5700X3D held to 4 of its 8 cores by an affinity mask,
Hyrule Field) both threads were the faster on 4 cores with and without SMT, and the render thread's tier was taken out.
Auto is off where the mode is not safe, and all of Auto is behind one constant, threaded::kAutoEnabled, which is on:
turned off, Auto is Off on every PC and the row changes nobody's default.

Checked:
 - the rule, compiled: the tiers at every count to 256 (both threads at 4 and 5 cores), without_mirrors() across 0..16
   cores (off below 6, recorder thread alone at 6 or more, never the render thread), On and Off whatever the CPU, the
   unsafe gate and the gate over Auto, the start's log line for each answer;
 - the count, compiled: threaded::cores_of on a Steam Deck's 4 cores / 8 threads (4), one sibling of each core usable, the
   Linux and the Windows numbering of siblings, a hybrid CPU, SMT off; and usable_cores itself, cut from vk_progcache.cpp,
   on a stand-in GetLogicalProcessorInformation and affinity mask: the SMT siblings once, the fall-backs to the logical
   count where the cores cannot be read, never more than the logical count;
 - the readers, compiled from their source (vk_replay.cpp render_thread_mode, vk_recorder.h mode_from_env and the hook
   vk_replay.cpp sets) and run in made-up environments: each takes its own half of the row, and RX_VK_ASYNC and
   RX_VK_RECORDER, when set, are over their halves;
 - the source: the gate is the only one and on; the row is Auto by default and takes a restart; the menu resolves the row
   once, from usable_cores, asks the pools' mirrors where Auto picked the threads and logs describe's line; RX_VK_ASYNC is
   read in one place; the host's present poll asks the renderer (HOS_OPT_RENDER_THREAD); the device policy and the facts
   count cores; the row's help, the settings page, the developer docs and the switch table say the tiers, and nothing names
   the render thread's tier that was taken out;
 - the menu's threaded_choice, compiled from its source on stand-ins and run in made-up environments: Auto is off with the
   capture off, RX_VK_LOW_SPEC, RX_VK_DUMP, RX_VK_BENCH, or pools with no mirrors (asked only where Auto picked the threads),
   and logs which; On is not checked;
 - and the checks have teeth: broken copies (the recorder thread left off at 4 and 5 cores, or on below 4, the tier from
   6 or 12, SMT siblings counted as cores, the logical count back, the recorder hooked to the render thread's half, the
   mirrors not asked, the old words in the help and the docs, and others) must each fail.

python -I -B tests/test_threaded_rendering.py [--cxx clang++]
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
import sys  # noqa: E402
sys.path.insert(0, str(ROOT / "tests"))
from menu_model_fixture import menu_text  # noqa: E402
VK = ROOT / "gpu" / "vk"

# The rule and the count: threaded_rendering.h alone, and usable_cores cut from vk_progcache.cpp on stand-ins
RULE_FIXTURE = r'''
#include "threaded_rendering.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace mkwvk::threaded;

// (the stand-ins usable_cores is built on: the affinity mask, GetLogicalProcessorInformation, usable_cpus)
typedef uintptr_t DWORD_PTR;
typedef uintptr_t ULONG_PTR;
typedef uint32_t DWORD;
typedef void* HANDLE;
typedef int BOOL;
enum LOGICAL_PROCESSOR_RELATIONSHIP { RelationProcessorCore = 0, RelationNumaNode = 1, RelationCache = 2, RelationProcessorPackage = 3 };
struct SYSTEM_LOGICAL_PROCESSOR_INFORMATION {
    ULONG_PTR ProcessorMask;
    LOGICAL_PROCESSOR_RELATIONSHIP Relationship;
    uint64_t Reserved[2];
};
static uint64_t fake_mask = 0;
static bool fake_mask_ok = true;
static std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> fake_info;
static int fake_info_fails = 0;   // (0 none; 1 the size asked for fails; 2 the size given, the call with it fails)
static unsigned fake_cpus = 8;
static HANDLE GetCurrentProcess() { return (HANDLE)(intptr_t)-1; }
static BOOL GetProcessAffinityMask(HANDLE, DWORD_PTR* proc, DWORD_PTR* sys) {
    *proc = (DWORD_PTR)fake_mask, *sys = ~(DWORD_PTR)0;
    return fake_mask_ok;
}
static BOOL GetLogicalProcessorInformation(SYSTEM_LOGICAL_PROCESSOR_INFORMATION* info, DWORD* len) {
    if (fake_info_fails == 1) return 0;
    const DWORD need = (DWORD)(fake_info.size() * sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    if (!info || *len < need) { *len = need; return 0; }
    if (fake_info_fails == 2) return 0;
    for (size_t i = 0; i < fake_info.size(); i++) info[i] = fake_info[i];
    *len = need;
    return 1;
}
static unsigned usable_cpus() { return fake_cpus; }
namespace threaded = mkwvk::threaded;
@@USABLE_CORES@@
// (a CPU for the stand-ins: one entry a core, its logical processors in its mask, and the caches and the package beside)
static void cpu(std::initializer_list<uint64_t> cores) {
    fake_info.clear();
    uint64_t all = 0;
    for (uint64_t m : cores) {
        fake_info.push_back({(ULONG_PTR)m, RelationProcessorCore, {0, 0}});
        fake_info.push_back({(ULONG_PTR)m, RelationCache, {0, 0}});   // (its L1 and L2: not cores)
        all |= m;
    }
    fake_info.push_back({(ULONG_PTR)all, RelationCache, {0, 0}});
    fake_info.push_back({(ULONG_PTR)all, RelationProcessorPackage, {0, 0}});
    fake_info.push_back({(ULONG_PTR)all, RelationNumaNode, {0, 0}});
}

static bool same(const Choice& c, bool render, bool recorder, Why why) {
    return c.render_thread == render && c.recorder_thread == recorder && c.why == why;
}
static std::string line_of(const Choice& c, unsigned cores, unsigned cpus, const char* unsafe) {
    char b[256];
    describe(b, sizeof b, c, cores, cpus, unsafe);
    return b;
}

int main() {
    // (the row's values, in V-Sync's order)
    CHECK(AUTO == 0 && ON == 1 && OFF == 2);
    // (Auto's tier: the render thread and the recorder thread from 4 cores, 22 Nov; fewer off)
    CHECK(kAutoCores == 4);
    for (unsigned cores = 0; cores <= 256; cores++) {
        const Choice c = auto_picks(cores);
        if (cores < 4) CHECK(same(c, false, false, Why::AutoFewCores));
        else CHECK(same(c, true, true, Why::AutoBothThreads));
    }
    // (the recorder thread never without the render thread; Auto picked the threads exactly in its tier)
    for (const int setting : {-1, (int)AUTO, (int)ON, (int)OFF, 7})
        for (const bool gate : {false, true})
            for (const bool unsafe : {false, true})
                for (unsigned cores = 0; cores <= 16; cores++) {
                    const Choice c = choose(setting, cores, unsafe, gate);
                    CHECK(!c.recorder_thread || c.render_thread);
                    CHECK(auto_picked(c) == (c.why == Why::AutoBothThreads));
                    CHECK(c.render_thread == c.recorder_thread);   // (no answer has one thread without the other now)
                    CHECK(auto_picked(c) == (setting != ON && setting != OFF && c.render_thread));
                }
    // (On and Off are the player's word: whatever the CPU, whatever is unsafe, whatever the gate)
    for (const bool gate : {false, true})
        for (const bool unsafe : {false, true})
            for (const unsigned cores : {0u, 2u, 4u, 5u, 6u, 12u, 64u}) {
                CHECK(same(choose(ON, cores, unsafe, gate), true, true, Why::On));
                CHECK(same(choose(OFF, cores, unsafe, gate), false, false, Why::Off));
            }
    // (Auto with the gate on: both threads from 4 cores, 4 and 5 among them; off below)
    for (const unsigned cores : {0u, 1u, 2u, 3u}) CHECK(same(choose(AUTO, cores, false, true), false, false, Why::AutoFewCores));
    for (const unsigned cores : {4u, 5u, 6u, 7u, 8u, 11u, 12u, 16u, 64u})
        CHECK(same(choose(AUTO, cores, false, true), true, true, Why::AutoBothThreads));
    // (not safe: off in Auto's tier, and below it)
    for (const unsigned cores : {0u, 3u, 4u, 5u, 6u, 8u, 16u, 64u})
        CHECK(same(choose(AUTO, cores, true, true), false, false, Why::AutoUnsafe));
    // (the gate off: off on every PC, and said so)
    for (const bool unsafe : {false, true})
        for (const unsigned cores : {0u, 3u, 4u, 5u, 6u, 12u, 64u})
            CHECK(same(choose(AUTO, cores, unsafe, false), false, false, Why::AutoGate));
    // (a value that is none of the row's: Auto)
    CHECK(choose(7, 4, false, true).why == Why::AutoBothThreads && choose(-1, 64, false, true).why == Why::AutoBothThreads);
    CHECK(choose(7, 3, false, true).why == Why::AutoFewCores);
    CHECK(choose(-1, 64, false, false).why == Why::AutoGate);
    // (as built: the gate is on, so Auto is its rule)
    CHECK(kAutoEnabled);
    CHECK(same(choose(AUTO, 3, false), false, false, Why::AutoFewCores));
    CHECK(same(choose(AUTO, 4, false), true, true, Why::AutoBothThreads));
    CHECK(same(choose(AUTO, 5, false), true, true, Why::AutoBothThreads));
    CHECK(same(choose(AUTO, 6, false), true, true, Why::AutoBothThreads));
    CHECK(same(choose(AUTO, 6, true), false, false, Why::AutoUnsafe) && same(choose(AUTO, 4, true), false, false, Why::AutoUnsafe));

    // (no buffer-pool mirrors: the render thread requires mirrors and stays off; at kAutoRecorderOnlyCores (6) or more
    // physical cores the recorder thread runs alone with serial replay; 4- and 5-core non-mirrored handhelds stay off)
    CHECK(kAutoRecorderOnlyCores == 6);
    for (unsigned cores = 0; cores <= 16; cores++) {
        const Choice base = choose(AUTO, cores, false, true);
        const Choice nm = without_mirrors(base, cores);
        CHECK(!nm.render_thread);
        if (cores < kAutoCores) CHECK(same(nm, false, false, Why::AutoFewCores));
        else if (cores < kAutoRecorderOnlyCores) CHECK(same(nm, false, false, Why::AutoUnsafe));
        else CHECK(same(nm, false, true, Why::AutoRecorderOnly));
    }
    for (const unsigned cores : {0u, 2u, 4u, 5u, 6u, 8u, 16u}) {
        CHECK(same(without_mirrors(choose(ON, cores, true, true), cores), true, true, Why::On));
        CHECK(same(without_mirrors(choose(OFF, cores, false, true), cores), false, false, Why::Off));
        CHECK(same(without_mirrors(choose(AUTO, cores, true, true), cores), false, false, Why::AutoUnsafe));
        CHECK(same(without_mirrors(choose(AUTO, cores, false, false), cores), false, false, Why::AutoGate));
        CHECK(same(without_mirrors(Choice{false, true, Why::AutoRecorderOnly}, cores), false, true, Why::AutoRecorderOnly));
    }

    // (the count: the physical cores among the usable logical processors, a core's SMT siblings once)
    {
        const uint64_t deck[] = {0x3, 0xC, 0x30, 0xC0};   // (4 cores, 8 threads, Windows' numbering: a core's siblings side by side)
        CHECK(cores_of(deck, 4, 0xFF) == 4);
        CHECK(cores_of(deck, 4, 0x55) == 4);   // (one sibling of each core)
        CHECK(cores_of(deck, 4, 0x0F) == 2);   // (4 logical processors: 2 cores)
        CHECK(cores_of(deck, 4, 0x01) == 1 && cores_of(deck, 4, 0) == 0 && cores_of(deck, 0, 0xFF) == 0);
        CHECK(cores_of(deck, 4, ~0ull) == 4);
        CHECK(same(choose(AUTO, cores_of(deck, 4, 0xFF), false, true), true, true, Why::AutoBothThreads));
        CHECK(same(choose(AUTO, cores_of(deck, 4, 0x55), false, true), true, true, Why::AutoBothThreads));
        CHECK(same(choose(AUTO, cores_of(deck, 4, 0x0F), false, true), false, false, Why::AutoFewCores));
        uint64_t linux_smt[8];   // (8 cores, 16 threads, Linux's numbering: a core's siblings i and i + 8)
        for (int i = 0; i < 8; i++) linux_smt[i] = 1ull << i | 1ull << (i + 8);
        CHECK(cores_of(linux_smt, 8, 0xFFFF) == 8 && cores_of(linux_smt, 8, 0x000F) == 4 && cores_of(linux_smt, 8, 0x0F0F) == 4);
        CHECK(cores_of(linux_smt, 8, 0x0707) == 3);
        uint64_t smt_off[16];   // (a 9950X3D with SMT off: one logical processor a core, held to N of them as measured)
        for (int i = 0; i < 16; i++) smt_off[i] = 1ull << i;
        CHECK(cores_of(smt_off, 16, 0xF) == 4 && cores_of(smt_off, 16, 0x3F) == 6 && cores_of(smt_off, 16, 0xFF) == 8);
        CHECK(cores_of(smt_off, 16, 0xFFFF) == 16);
        uint64_t hybrid[14];   // (6 performance cores with SMT and 8 efficiency cores without: 14 cores, 20 threads)
        for (int i = 0; i < 6; i++) hybrid[i] = 3ull << (2 * i);
        for (int i = 0; i < 8; i++) hybrid[6 + i] = 1ull << (12 + i);
        CHECK(cores_of(hybrid, 14, (1ull << 20) - 1) == 14 && cores_of(hybrid, 14, 0xFFF) == 6);
        uint64_t wide[32];   // (64 logical processors: the mask's top bit too)
        for (int i = 0; i < 32; i++) wide[i] = 3ull << (2 * i);
        CHECK(cores_of(wide, 32, ~0ull) == 32 && cores_of(wide, 32, 1ull << 63) == 1);
    }
    // (usable_cores as vk_progcache.cpp has it, on the stand-ins)
    {
        cpu({0x3, 0xC, 0x30, 0xC0}), fake_mask = 0xFF, fake_cpus = 8;   // (a Steam Deck: 4 cores, 8 threads)
        CHECK(usable_cores() == 4);
        CHECK(same(choose(AUTO, usable_cores(), false, true), true, true, Why::AutoBothThreads));
        fake_mask = 0x0F, fake_cpus = 4;   // (held to 4 logical processors: 2 cores)
        CHECK(usable_cores() == 2);
        fake_mask = 0x55, fake_cpus = 4;   // (one sibling of each core: 4)
        CHECK(usable_cores() == 4);
        fake_mask_ok = false, fake_cpus = 8;   // (the mask not read: every core)
        CHECK(usable_cores() == 4);
        fake_mask_ok = true, fake_mask = 0;   // (an empty mask: every core)
        CHECK(usable_cores() == 4);
        fake_mask = 0xFF;
        fake_info_fails = 1;   // (the cores not read: the logical count, as before)
        CHECK(usable_cores() == 8);
        fake_info_fails = 2;
        CHECK(usable_cores() == 8);
        fake_info_fails = 0;
        fake_info.clear();   // (no core listed)
        CHECK(usable_cores() == 8);
        fake_info.push_back({(ULONG_PTR)0xFF, RelationCache, {0, 0}});   // (caches only: no core)
        CHECK(usable_cores() == 8);
        cpu({0x3, 0xC}), fake_mask = 0x300, fake_cpus = 2;   // (no listed core in the mask: the logical count)
        CHECK(usable_cores() == 2);
        cpu({0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80}), fake_mask = 0xFF, fake_cpus = 4;   // (never more than the logical count)
        CHECK(usable_cores() == 4);
        const uint64_t one = 1;   // (the 9950X3D with SMT off, as measured: 4, 6 and 8 of its 16)
        cpu({one, one << 1, one << 2, one << 3, one << 4, one << 5, one << 6, one << 7, one << 8, one << 9, one << 10, one << 11,
             one << 12, one << 13, one << 14, one << 15});
        fake_mask = 0xF, fake_cpus = 4;
        CHECK(usable_cores() == 4 && same(choose(AUTO, usable_cores(), false, true), true, true, Why::AutoBothThreads));
        fake_mask = 0x1F, fake_cpus = 5;
        CHECK(usable_cores() == 5 && same(choose(AUTO, usable_cores(), false, true), true, true, Why::AutoBothThreads));
        fake_mask = 0x7, fake_cpus = 3;
        CHECK(usable_cores() == 3 && same(choose(AUTO, usable_cores(), false, true), false, false, Why::AutoFewCores));
        fake_mask = 0x3F, fake_cpus = 6;
        CHECK(usable_cores() == 6 && same(choose(AUTO, usable_cores(), false, true), true, true, Why::AutoBothThreads));
        fake_mask = 0xFF, fake_cpus = 8;
        CHECK(usable_cores() == 8 && same(choose(AUTO, usable_cores(), false, true), true, true, Why::AutoBothThreads));
        cpu({3ull << 0, 3ull << 2, 3ull << 4, 3ull << 6, 3ull << 8, 3ull << 10}), fake_mask = 0xFFF, fake_cpus = 12;
        CHECK(usable_cores() == 6);   // (6 cores, 12 threads: both threads, which Auto gave before only from 12 logical)
    }

    // (the start's line, after "vk: Threaded rendering: ", for each answer)
    CHECK(line_of(choose(ON, 2, true), 2, 4, nullptr) == "On (the render thread and the recorder thread)");
    CHECK(line_of(choose(OFF, 16, false), 16, 16, nullptr) == "Off");
    CHECK(line_of(choose(AUTO, 4, false, false), 4, 8, nullptr) ==
          "Auto: off (Auto is turned off in this build; 4 cores of 8 logical processors, its rule is the render thread and "
          "the recorder thread from 4 cores)");
    CHECK(line_of(choose(AUTO, 8, true), 8, 8, "RX_VK_DUMP") == "Auto: off (not safe on this run: RX_VK_DUMP)");
    CHECK(line_of(Choice{false, false, Why::AutoUnsafe}, 6, 12, "the buffer pools have no mirrors") ==
          "Auto: off (not safe on this run: the buffer pools have no mirrors)");
    CHECK(line_of(without_mirrors(choose(AUTO, 4, false), 4), 4, 8, "the buffer pools have no mirrors") ==
          "Auto: off (not safe on this run: the buffer pools have no mirrors)");
    CHECK(line_of(without_mirrors(choose(AUTO, 5, false), 5), 5, 10, "the buffer pools have no mirrors") ==
          "Auto: off (not safe on this run: the buffer pools have no mirrors)");
    CHECK(line_of(without_mirrors(choose(AUTO, 6, false), 6), 6, 12, "the buffer pools have no mirrors") ==
          "Auto: the recorder thread only (6 cores of 12 logical processors; the render thread off: the buffer pools have no mirrors)");
    CHECK(line_of(without_mirrors(choose(AUTO, 8, false), 8), 8, 16, "the buffer pools have no mirrors") ==
          "Auto: the recorder thread only (8 cores of 16 logical processors; the render thread off: the buffer pools have no mirrors)");
    CHECK(line_of(without_mirrors(choose(AUTO, 8, false), 8), 8, 16, nullptr) ==
          "Auto: the recorder thread only (8 cores of 16 logical processors; the render thread off: ?)");
    CHECK(line_of(choose(AUTO, 2, false), 2, 4, nullptr) ==
          "Auto: off (2 cores of 4 logical processors; the render thread and the recorder thread from 4 cores)");
    CHECK(line_of(choose(AUTO, 4, false), 4, 8, nullptr) ==
          "Auto: on, the render thread and the recorder thread (4 cores of 8 logical processors)");
    CHECK(line_of(choose(AUTO, 5, false), 5, 5, nullptr) ==
          "Auto: on, the render thread and the recorder thread (5 cores of 5 logical processors)");
    CHECK(line_of(choose(AUTO, 8, false), 8, 16, nullptr) ==
          "Auto: on, the render thread and the recorder thread (8 cores of 16 logical processors)");
    CHECK(line_of(choose(AUTO, 6, false), 6, 12, nullptr) ==
          "Auto: on, the render thread and the recorder thread (6 cores of 12 logical processors)");
    {   // (a short buffer: cut, never overrun)
        char b[16];
        std::memset(b, 'x', sizeof b);
        describe(b, 8, choose(AUTO, 4, false), 4, 8, nullptr);
        CHECK(std::strlen(b) == 7 && b[8] == 'x');
    }
    std::printf("PASS: the Threaded rendering rule, Auto's count of cores and the start's line\n");
}
'''

# The readers, cut from their source: the replay's half (render_thread_mode), the recorder's (mode_from_env, through the
# hook vk_replay.cpp sets), on the row as the menu resolves it for the arguments' setting, cores and safety
READERS_FIXTURE = r'''
#define _CRT_SECURE_NO_WARNINGS   // (getenv, as the renderer reads it: MSVC's library calls it unsafe)
#include "threaded_rendering.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
static mkwvk::threaded::Choice row;   // (the menu's threaded_choice, for the arguments)
[[maybe_unused]] static bool menu_render_thread() { return row.render_thread; }
[[maybe_unused]] static bool menu_recorder_thread() { return row.recorder_thread; }
namespace rec {
@@MODE_ENUM@@
inline bool (*g_setting_thread)() = nullptr;
@@MODE_FROM_ENV@@
}
@@RENDER_THREAD_MODE@@
int main(int argc, char** argv) {
    if (argc != 5) return 2;
    const unsigned cores = (unsigned)std::atoi(argv[2]);
    row = mkwvk::threaded::choose(std::atoi(argv[1]), cores, std::atoi(argv[3]) != 0);
    if (mkwvk::threaded::auto_picked(row) && std::atoi(argv[4]) == 0)
        row = mkwvk::threaded::without_mirrors(row, cores);
    if (std::atoi(argv[1]) == @@SPLIT@@)   // (a row with its halves apart: the render thread alone, which no answer of the
        row = mkwvk::threaded::Choice{true, false, mkwvk::threaded::Why::On};   // rule gives now, so each reader's own half shows)
    rec::g_setting_thread = @@HOOK@@;
    bool refused = false;
    const int recorder = rec::mode_from_env(&refused);
    std::printf("render=%d recorder=%d refused=%d\n", render_thread_mode() != 0 ? 1 : 0, recorder, refused ? 1 : 0);
    return 0;
}
'''
REC_OFF, SYNC, DEFER, THREAD = 0, 1, 2, 3   # (vk_recorder.h's Mode)
AUTO, ON, OFF = 0, 1, 2                     # (the row's values)
SPLIT = -2                                  # (not a row's value: the fixture's row with the render thread alone)
# (the row's setting, cores, unsafe, mirrored, the environment's switches; then the render thread, the recorder's mode, refused)
READER_CASES = [
    (AUTO, 2, 0, 1, {}, 0, REC_OFF, 0),
    (AUTO, 3, 0, 1, {}, 0, REC_OFF, 0),
    (AUTO, 4, 0, 1, {}, 1, THREAD, 0),      # (both threads from 4 cores)
    (AUTO, 5, 0, 1, {}, 1, THREAD, 0),
    (AUTO, 6, 0, 1, {}, 1, THREAD, 0),
    (AUTO, 16, 0, 1, {}, 1, THREAD, 0),
    (AUTO, 3, 0, 0, {}, 0, REC_OFF, 0),     # (no mirrors, <4 cores: off)
    (AUTO, 4, 0, 0, {}, 0, REC_OFF, 0),     # (no mirrors, 4 cores, e.g. Steam Deck: off)
    (AUTO, 5, 0, 0, {}, 0, REC_OFF, 0),     # (no mirrors, 5 cores: off)
    (AUTO, 6, 0, 0, {}, 0, THREAD, 0),      # (no mirrors, 6+ cores: recorder thread alone)
    (AUTO, 8, 0, 0, {}, 0, THREAD, 0),
    (AUTO, 16, 0, 0, {}, 0, THREAD, 0),
    (AUTO, 8, 1, 1, {}, 0, REC_OFF, 0),     # (not safe)
    (AUTO, 8, 1, 0, {}, 0, REC_OFF, 0),     # (not safe overrides no-mirror recorder-only)
    (AUTO, 4, 1, 1, {}, 0, REC_OFF, 0),
    (ON, 2, 1, 1, {}, 1, THREAD, 0),
    (ON, 4, 0, 0, {}, 1, THREAD, 0),        # (explicit ON ignores mirror gate)
    (OFF, 16, 0, 1, {}, 0, REC_OFF, 0),
    (OFF, 8, 0, 0, {}, 0, REC_OFF, 0),      # (explicit OFF)
    # (the switches over their halves)
    (AUTO, 4, 0, 1, {"RX_VK_RECORDER": "thread"}, 1, THREAD, 0),
    (AUTO, 4, 0, 1, {"RX_VK_RECORDER": "off"}, 1, REC_OFF, 0),   # (the way back for a 4-core PC the new Auto does not suit)
    (AUTO, 5, 0, 1, {"RX_VK_RECORDER": "0"}, 1, REC_OFF, 0),
    (AUTO, 3, 0, 1, {"RX_VK_RECORDER": "thread"}, 0, THREAD, 0),
    (AUTO, 6, 0, 1, {"RX_VK_RECORDER": "sync"}, 1, SYNC, 0),
    (AUTO, 6, 0, 1, {"RX_VK_RECORDER": "off"}, 1, REC_OFF, 0),
    (AUTO, 6, 0, 1, {"RX_VK_RECORDER": "defer"}, 1, DEFER, 0),
    (AUTO, 8, 0, 0, {"RX_VK_RECORDER": "off"}, 0, REC_OFF, 0),   # (switch overrides Auto recorder-only)
    (AUTO, 8, 0, 0, {"RX_VK_RECORDER": "0"}, 0, REC_OFF, 0),
    (AUTO, 8, 0, 0, {"RX_VK_RECORDER": "sync"}, 0, SYNC, 0),
    (AUTO, 8, 0, 0, {"RX_VK_ASYNC": "1"}, 1, THREAD, 0),
    (AUTO, 6, 0, 1, {"RX_VK_ASYNC": "0"}, 0, THREAD, 0),
    (AUTO, 4, 0, 1, {"RX_VK_ASYNC": "0"}, 0, THREAD, 0),
    (AUTO, 2, 0, 1, {"RX_VK_ASYNC": "1"}, 1, REC_OFF, 0),
    (OFF, 16, 0, 1, {"RX_VK_ASYNC": "1", "RX_VK_RECORDER": "thread"}, 1, THREAD, 0),
    (ON, 16, 0, 1, {"RX_VK_ASYNC": "0", "RX_VK_RECORDER": "sync"}, 0, SYNC, 0),
    # (each reader its own half: with the halves apart, the render thread on and the recorder off)
    (SPLIT, 8, 0, 1, {}, 1, REC_OFF, 0),
    (SPLIT, 8, 0, 1, {"RX_VK_RECORDER": "thread"}, 1, THREAD, 0),
    (SPLIT, 8, 0, 1, {"RX_VK_ASYNC": "0"}, 0, REC_OFF, 0),
]
SWITCHES = ("RX_VK_ASYNC", "RX_VK_RECORDER")

# The menu's threaded_choice, cut from vk_menu.cpp whole and run on stand-ins (the row, the cores, the pools' mirrors) in
# made-up environments: each of Auto's safety gates turns Auto off by its behaviour, not only by its text (the checker,
# 22 Nov 2026: dropping the RX_VK_LOW_SPEC, RX_VK_DUMP or RX_VK_BENCH condition, keeping its string, passed every other
# check)
CHOICE_FIXTURE = r'''
#define _CRT_SECURE_NO_WARNINGS
#include "threaded_rendering.h"
#include "@@ENV_H@@"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
namespace mkwvk {
static struct { int threaded_rendering = 0; } opts;
static unsigned fake_cores = 0;
static bool fake_mirrors = false;
static int mirrors_asked = 0;
static void menu_init() {}
static unsigned usable_cpus() { return fake_cores * 2; }
static unsigned usable_cores() { return fake_cores; }
static bool pools_mirrored() { mirrors_asked++; return fake_mirrors; }
static void logf(const char* f, ...) {
    va_list ap;
    va_start(ap, f);
    std::vprintf(f, ap);
    va_end(ap);
    std::printf("\n");
}
@@THREADED_CHOICE@@
}
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    mkwvk::opts.threaded_rendering = std::atoi(argv[1]);
    mkwvk::fake_cores = (unsigned)std::atoi(argv[2]);
    mkwvk::fake_mirrors = std::atoi(argv[3]) != 0;
    const mkwvk::threaded::Choice& c = mkwvk::threaded_choice();
    std::printf("render=%d recorder=%d asked=%d\n", c.render_thread ? 1 : 0, c.recorder_thread ? 1 : 0, mkwvk::mirrors_asked);
    return 0;
}
'''
GATES = ("RX_VK_CAPTURE", "RX_VK_LOW_SPEC", "RX_VK_DUMP", "RX_VK_BENCH")
# (the row, cores, the pools mirrored, the environment; then the render thread, the recorder thread, the mirrors asked, and
# the start's line)
CHOICE_CASES = [
    (AUTO, 4, 1, {}, 1, 1, 1, "Auto: on, the render thread and the recorder thread (4 cores of 8 logical processors)"),
    (AUTO, 5, 1, {}, 1, 1, 1, "Auto: on, the render thread and the recorder thread (5 cores of 10 logical processors)"),
    (AUTO, 3, 1, {}, 0, 0, 0, "Auto: off (3 cores of 6 logical processors; "),
    (AUTO, 3, 0, {}, 0, 0, 0, "Auto: off (3 cores of 6 logical processors; "),   # (the mirrors asked only where Auto picked)
    (AUTO, 4, 0, {}, 0, 0, 1, "Auto: off (not safe on this run: the buffer pools have no mirrors)"),   # (the Deck, 4-core iGPU)
    (AUTO, 5, 0, {}, 0, 0, 1, "Auto: off (not safe on this run: the buffer pools have no mirrors)"),
    (AUTO, 6, 0, {}, 0, 1, 1, "Auto: the recorder thread only (6 cores of 12 logical processors; the render thread off: the buffer pools have no mirrors)"),
    (AUTO, 8, 0, {}, 0, 1, 1, "Auto: the recorder thread only (8 cores of 16 logical processors; the render thread off: the buffer pools have no mirrors)"),
    (AUTO, 8, 0, {"RX_VK_CAPTURE": "0"}, 0, 0, 0, "Auto: off (not safe on this run: RX_VK_CAPTURE)"),
    (AUTO, 8, 0, {"RX_VK_LOW_SPEC": "1"}, 0, 0, 0, "Auto: off (not safe on this run: RX_VK_LOW_SPEC)"),
    (AUTO, 8, 0, {"RX_VK_DUMP": "1"}, 0, 0, 0, "Auto: off (not safe on this run: RX_VK_DUMP)"),
    (AUTO, 8, 0, {"RX_VK_BENCH": "1"}, 0, 0, 0, "Auto: off (not safe on this run: RX_VK_BENCH)"),
    (AUTO, 4, 1, {"RX_VK_CAPTURE": "0"}, 0, 0, 0, "Auto: off (not safe on this run: RX_VK_CAPTURE)"),
    (AUTO, 4, 1, {"RX_VK_CAPTURE": "1"}, 1, 1, 1, "Auto: on, "),
    (AUTO, 4, 1, {"RX_VK_LOW_SPEC": "1"}, 0, 0, 0, "Auto: off (not safe on this run: RX_VK_LOW_SPEC)"),
    (AUTO, 4, 1, {"RX_VK_LOW_SPEC": "0"}, 1, 1, 1, "Auto: on, "),
    (AUTO, 4, 1, {"RX_VK_DUMP": "1"}, 0, 0, 0, "Auto: off (not safe on this run: RX_VK_DUMP)"),
    (AUTO, 4, 1, {"RX_VK_BENCH": "1"}, 0, 0, 0, "Auto: off (not safe on this run: RX_VK_BENCH)"),
    (AUTO, 16, 1, {"RX_VK_BENCH": "1"}, 0, 0, 0, "Auto: off (not safe on this run: RX_VK_BENCH)"),
    (ON, 2, 0, {"RX_VK_CAPTURE": "0"}, 1, 1, 0, "On"),   # (On is the player's word: nothing is checked for it)
    (OFF, 16, 1, {}, 0, 0, 0, "Off"),
]


def block(text, marker):
    """From marker to the brace closing its first one"""
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def read_sources():
    paths = dict(rule=VK / "threaded_rendering.h", menu=VK / "vk_menu.cpp", menu_h=VK / "vk_menu.h", replay=VK / "vk_replay.cpp",
                 recorder=VK / "vk_recorder.h", api=VK / "vk_api.h", progcache=VK / "vk_progcache.cpp",
                 internal=VK / "vk_internal.h", policy=VK / "device_policy.h", main=VK / "vk_main.cpp",
                 switches=VK / "switches.def", native=ROOT / "host" / "nvn_native.c",
                 settings=ROOT / "docs" / "user" / "graphics-and-controls.md",
                 docs=ROOT / "docs" / "dev" / "reference" / "switches.md")
    s = {k: p.read_text(encoding="utf-8") for k, p in paths.items()}
    s["menu"] = menu_text(ROOT)   # (vk_menu.cpp and its modules, their rows as table lines, #1633: threads.cpp's row)
    return s


def constant(rule, name):
    m = re.search(r"constexpr unsigned %s = (\d+);" % name, rule)
    return int(m.group(1)) if m else None


def check_sources(s):
    """The source checks: the failures, an empty list when none"""
    bad = []
    rule, menu = s["rule"], s["menu"]
    auto_cores = constant(rule, "kAutoCores")
    if auto_cores is None:
        return ["threaded_rendering.h: no kAutoCores, Auto's tier"]
    rec_only_cores = constant(rule, "kAutoRecorderOnlyCores")
    if rec_only_cores is None:
        return ["threaded_rendering.h: no kAutoRecorderOnlyCores, Auto's no-mirror recorder-only threshold"]
    # (the render thread's tier, 4 or 5 cores to 22 Nov 2026, taken out: nothing names it or says it)
    for key in ("rule", "menu", "menu_h", "replay", "recorder", "policy", "switches", "settings", "docs"):
        if re.search(r"\b(AutoRenderThread|kAutoRenderCores|kAutoRecorderCores)\b", s[key]):
            bad.append("%s still names the render thread's tier (Why::AutoRenderThread, kAutoRenderCores, kAutoRecorderCores)" % key)
    old_words = re.compile(r"render thread (alone|only) (with|from|on) \d|\d or \d, the render thread alone|"
                           r"recorder thread too from \d|from 6 (physical )?cores")
    for key in ("rule", "menu", "menu_h", "replay", "recorder", "policy", "switches", "settings", "docs"):
        text = s[key]
        if key == "rule":   # (the header keeps 18 Nov's table and what it said: the record, not the rule)
            text = text[text.index("#pragma once"):]
        m = old_words.search(text.replace("\n", " ").replace("// ", "").replace("/// ", ""))
        if m:
            bad.append("%s still says the render thread's tier: %r" % (key, m.group(0)))

    # (the gate: one constant, on, and Auto behind it)
    if len(re.findall(r"constexpr bool \w+ = ", rule)) != 1 or "constexpr bool kAutoEnabled = true;" not in rule:
        bad.append("threaded_rendering.h: Auto's gate is not the one constant kAutoEnabled, on (turning it off changes the "
                   "default for every PC with 4 cores: that is its own change)")
    choose = block(rule, "constexpr Choice choose(")
    if "bool auto_enabled = kAutoEnabled" not in choose or "if (!auto_enabled) return {false, false, Why::AutoGate};" not in choose:
        bad.append("threaded_rendering.h: choose() no longer keeps Auto behind the gate")
    if "kAutoCpus" in rule:
        bad.append("threaded_rendering.h: kAutoCpus, the logical processors' threshold, is back")

    # (the row: Auto by default, read once so it takes a restart)
    if "    int threaded_rendering = 0;" not in s["menu_h"]:
        bad.append("vk_menu.h: the Threaded rendering option is not Auto (0) out of the box")
    row = re.search(r'\{TAB_DEBUG, "Threaded rendering", &opts\.threaded_rendering, 0, 2, 1, tx_vsync,[^{}]*\}', menu)
    if not row or not row.group(0).endswith("hn_threaded, true, true}"):
        bad.append("vk_menu.cpp: no Threaded rendering row (Auto, On, Off), or it no longer takes a restart")
    elif "after a restart" not in row.group(0).replace('"\n     "', ""):
        bad.append("vk_menu.cpp: the Threaded rendering row's help does not say it applies after a restart")
    # (the row's line for Auto says the tier as the header has it)
    hint = block(menu, "const char* hn_threaded(int v, int variant) {")
    end = hint.find(': "The same as Off, on every PC."')
    auto_line = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', hint[:end])) if end > 0 else ""
    for need in ("On with %d or more processor cores" % auto_cores, "Off with fewer",
                 "%d or more" % rec_only_cores, "mirrors"):
        if need not in auto_line:
            bad.append("vk_menu.cpp: hn_threaded's Auto line does not say " + need)
    if "render thread alone" in auto_line:
        bad.append("vk_menu.cpp: hn_threaded's Auto line still says the render thread's tier")

    # (resolved once, from the cores, the mirrors asked where Auto picked the threads, describe's line logged; each reader
    # its half)
    marker = "static const threaded::Choice& threaded_choice() {"
    resolved = block(menu, marker) if marker in menu else ""
    if not resolved:
        bad.append("vk_menu.cpp: no threaded_choice(), the row resolved once")
    doc_start = menu.find("The Threaded rendering row, resolved once")
    doc_end = menu.find(marker)
    if doc_start >= 0 and doc_end > doc_start and "GCN" in menu[doc_start:doc_end]:
        bad.append("vk_menu.cpp: threaded_choice() comment still lists GCN among unmirrored GPUs (discrete GCN has mirrors)")
    for need in ("static const threaded::Choice choice = [] {", "menu_init();", "const unsigned cpus = usable_cpus(), cores = usable_cores();",
                 "threaded::choose(opts.threaded_rendering, cores, unsafe != nullptr)",
                 'const char* capture = getenv("RX_VK_CAPTURE");',
                 'capture && atoi(capture) == 0 ? "RX_VK_CAPTURE" :', '"RX_VK_LOW_SPEC"',
                 '"RX_VK_DUMP"', '"RX_VK_BENCH"', "if (threaded::auto_picked(c) && !pools_mirrored()) {",
                 "c = threaded::without_mirrors(c, cores);",
                 "threaded::describe(line, sizeof line, c, cores, cpus, unsafe);", 'logf("vk: Threaded rendering: %s", line);'):
        if resolved and need not in resolved:
            bad.append("vk_menu.cpp: threaded_choice() lost " + need + " (resolved once, from the cores the process may run on, "
                       "off for Auto where the mode is not safe, and logged)")
    if "RX_VK_ASYNC" in resolved or "RX_VK_RECORDER" in resolved:
        bad.append("vk_menu.cpp: threaded_choice() is the row's answer alone: the switches are read over it by their own readers")
    for need in ("bool menu_render_thread() { return threaded_choice().render_thread; }",
                 "bool menu_recorder_thread() { return threaded_choice().recorder_thread; }"):
        if need not in menu:
            bad.append("vk_menu.cpp: no " + need.split(" {")[0] + " giving its own half of the row")
    if "bool menu_render_thread();" not in s["menu_h"] or "bool menu_recorder_thread();" not in s["menu_h"]:
        bad.append("vk_menu.h: menu_render_thread and menu_recorder_thread are not declared")

    # (the replay: RX_VK_ASYNC in one place, the switch over the row's render thread)
    mode = block(s["replay"], "int render_thread_mode() {")
    if 'const char* e = getenv("RX_VK_ASYNC");' not in mode or "return e ? atoi(e) : menu_render_thread() ? 1 : 0;" not in mode:
        bad.append("vk_replay.cpp: render_thread_mode() is no longer RX_VK_ASYNC when set, else the row's render thread as 1 or 0")
    if "render_thread_mode() != 0" not in block(s["replay"], "bool async_on() {"):
        bad.append("vk_replay.cpp: async_on() no longer reads render_thread_mode()")
    if 'getenv("RX_VK_ASYNC")' not in block(menu, "const char* threaded_env() {"):
        bad.append("vk_menu.cpp: RX_VK_ASYNC is read outside threaded_env(), the row's note that the switch is set")
    # (the recorder: the switch when set; unset, the row's recorder thread, through the pointer vk_replay.cpp sets)
    if "inline bool (*g_setting_thread)() = nullptr;" not in s["recorder"] or \
            "(rec::g_setting_thread = menu_recorder_thread, true)" not in s["replay"]:
        bad.append("the recorder's question to the row is not its own half (vk_recorder.h g_setting_thread, vk_replay.cpp: "
                   "menu_recorder_thread)")

    # (the host: the present poll asks the renderer, which answers async_on(), the render thread's half)
    if "enum { HOS_OPT_RENDER_THREAD = 58 };" not in s["api"]:
        bad.append("vk_api.h: HOS_OPT_RENDER_THREAD is not option 58")
    if "if (id == HOS_OPT_RENDER_THREAD) return async_on();" not in block(s["replay"], "t.option = [](int id) -> int {"):
        bad.append("vk_replay.cpp: the option callback no longer answers HOS_OPT_RENDER_THREAD with async_on()")
    poll = block(s["native"], "NV(nvnEventGetValue) {")
    if "now = e ? !env_value_off(e) : rx_option(HOS_OPT_RENDER_THREAD, -1);" not in poll or 'getenv("HOS_PRESENT_NOWAIT")' not in poll:
        bad.append("nvn_native.c: the present poll is not HOS_PRESENT_NOWAIT when set, else the renderer's HOS_OPT_RENDER_THREAD")

    # (the count: usable_cores beside usable_cpus, by the cores' masks, declared; the facts and the policy count cores too)
    marker = "unsigned usable_cores() {"
    cores = block(s["progcache"], marker) if marker in s["progcache"] else ""
    if not cores:
        bad.append("vk_progcache.cpp: no usable_cores()")
    for need in ("GetLogicalProcessorInformation(", "RelationProcessorCore", "threaded::cores_of(", "usable_cpus()",
                 "GetProcessAffinityMask("):
        if cores and need not in cores:
            bad.append("vk_progcache.cpp: usable_cores() lost " + need)
    if "unsigned usable_cores();" not in s["internal"]:
        bad.append("vk_internal.h: usable_cores is not declared")
    if "threaded::auto_picks(f.cores_usable)" not in s["policy"] or "pf.cores_usable = usable_cores();" not in s["main"]:
        bad.append("the device policy's Threaded rendering flags are not by the cores (device_policy.h f.cores_usable, "
                   "vk_main.cpp pf.cores_usable = usable_cores())")

    # (no reader left on the old one-half answer)
    for key in ("menu", "menu_h", "replay", "recorder", "policy", "native", "rule"):
        if "menu_threaded_rendering" in s[key] or re.search(r"\bWhy::Auto(Cpus|FewCpus)\b", s[key]):
            bad.append("%s still names menu_threaded_rendering or Why::AutoCpus: the row has two halves now" % key)

    # (the segment workers stay gone, 19 Nov 2026: 3-35% slower on every PC measured; the recorder thread is back)
    for key in ("menu", "menu_h", "replay", "recorder", "policy", "native", "rule", "internal"):
        if re.search(r"\b(menu_segment_workers|par_ceiling|par_threads|workers_for|kAutoWorkerCores|kMaxWorkers|ParLock|"
                     r"par_active|parallel_configured)\b", s[key]):
            bad.append("%s still names the segment workers" % key)
    if not re.search(r'^GONE\(RX_VK_PARALLEL, ', s["switches"], re.M):
        bad.append("switches.def: RX_VK_PARALLEL is not a GONE row")

    # (said where the settings and the switches are)
    settings, docs = s["settings"], s["docs"]
    table = [line for line in settings.splitlines() if line.startswith("| | Developer: Threaded rendering | Auto, On, Off")]
    start = settings.find("- **Threaded rendering:**")
    para = settings[start:start + 900].replace("\n  ", " ") if start >= 0 else ""
    if len(table) != 1 or not para:
        bad.append("docs/user/graphics-and-controls.md: no Threaded rendering line in the settings table, or no paragraph on it")
    else:
        for where, text in (("the option table", table[0]), ("the paragraph", para)):
            if "%d or more" % auto_cores not in text or "Off with fewer" not in text or "12 or more" in text or \
                    "render thread alone" in text or "%d or more" % rec_only_cores not in text or "mirror" not in text:
                bad.append("docs/user/graphics-and-controls.md: %s does not say Auto's tiers (%d or more cores, recorder alone at %d or more without mirrors, Off with fewer)"
                           % (where, auto_cores, rec_only_cores))
    rows = [line for line in docs.splitlines() if line.startswith("| `threaded_rendering` |")]
    if len(rows) != 1 or "threaded::kAutoEnabled" not in rows[0]:
        bad.append("docs/dev/reference/switches.md: no threaded_rendering row in the options file's keys, or it does not name the gate")
    elif "%d or more cores, the render thread and the recorder thread" % auto_cores not in rows[0] or \
            "fewer, Off" not in rows[0] or "render thread alone" in rows[0] or "usable_cores" not in rows[0] or \
            "%d or more" % rec_only_cores not in rows[0] or "kAutoRecorderOnlyCores" not in rows[0]:
        bad.append("docs/dev/reference/switches.md: the threaded_rendering row does not say Auto's tiers by cores (usable_cores, kAutoRecorderOnlyCores)")
    for name, need in (("RX_VK_ASYNC", "on from %d physical cores" % auto_cores),
                       ("RX_VK_RECORDER", "Auto from %d physical cores" % auto_cores),
                       ("RX_VK_RECORDER", "%d or more physical cores" % rec_only_cores)):
        m = re.search(r'^SW\(%s, \w+, \w+, "([^"]*)"' % name, s["switches"], re.M)
        if not m or need not in m.group(1):
            bad.append("switches.def: %s's default does not say %s" % (name, need))
    return bad


def compile_and_run(cxx, source, include, tmp, name, runs):
    """Build the fixture against `include` (threaded_rendering.h's folder) and run it once for each (arguments, environment)
    of runs: (built, the compiler's output or the completed runs)"""
    cpp, exe = Path(tmp) / (name + ".cpp"), Path(tmp) / (name + (".exe" if os.name == "nt" else ""))
    cpp.write_text(source, encoding="utf-8")
    r = subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + str(include), str(cpp), "-o", str(exe)],
                       capture_output=True, text=True, timeout=300)
    if r.returncode:
        return False, r.stdout + r.stderr
    return True, [subprocess.run([str(exe)] + [str(a) for a in args], capture_output=True, text=True, timeout=60, env=env)
                  for args, env in runs]


def with_rule(s, tmp, name):
    """A folder of its own with this copy's threaded_rendering.h"""
    inc = Path(tmp) / (name + "-include")
    inc.mkdir(exist_ok=True)
    with open(inc / "threaded_rendering.h", "w", encoding="utf-8", newline="\n") as f:
        f.write(s["rule"])
    return inc


def check_rule(cxx, s, tmp, name="rule"):
    """The rule's and the count's fixture: None when it passes, else why not ('build: ...' when it did not build)"""
    source = RULE_FIXTURE.replace("@@USABLE_CORES@@", block(s["progcache"], "unsigned usable_cores() {"))
    built, out = compile_and_run(cxx, source, with_rule(s, tmp, name), tmp, name, [([], None)])
    if not built:
        return "build: " + out
    return None if out[0].returncode == 0 else (out[0].stdout + out[0].stderr).strip()


def check_readers(cxx, s, tmp, name="readers"):
    """The readers' fixture in each of READER_CASES' environments: None when every one answers as expected"""
    hook = re.search(r"\(rec::g_setting_thread = (\w+), true\)", s["replay"])
    enum = re.search(r"enum Mode : int \{[^}]*\};", s["recorder"])
    source = READERS_FIXTURE.replace("@@MODE_ENUM@@", enum.group(0) if enum else "") \
                            .replace("@@MODE_FROM_ENV@@", block(s["recorder"], "inline int mode_from_env(bool* refused) {")) \
                            .replace("@@RENDER_THREAD_MODE@@", block(s["replay"], "int render_thread_mode() {")) \
                            .replace("@@HOOK@@", hook.group(1) if hook else "nullptr") \
                            .replace("@@SPLIT@@", str(SPLIT))
    base = {k: v for k, v in os.environ.items() if k not in SWITCHES}
    runs = [([setting, cores, unsafe, mirrored], dict(base, **environment))
            for setting, cores, unsafe, mirrored, environment, *_ in READER_CASES]
    built, out = compile_and_run(cxx, source, with_rule(s, tmp, name), tmp, name, runs)
    if not built:
        return "build: " + out
    for (setting, cores, unsafe, mirrored, environment, render, recorder, refused), run in zip(READER_CASES, out):
        want = "render=%d recorder=%d refused=%d" % (render, recorder, refused)
        if run.returncode or run.stdout.strip() != want:
            return "the row %d, %d cores, unsafe %d, mirrored %d, %s: %s, expected %s" % (
                setting, cores, unsafe, mirrored, environment or "no switch", (run.stdout + run.stderr).strip(), want)
    return None


def check_choice(cxx, s, tmp, name="choice"):
    """The menu's threaded_choice in each of CHOICE_CASES: None when every one answers and logs as expected"""
    marker = "static const threaded::Choice& threaded_choice() {"
    if marker not in s["menu"]:
        return "no threaded_choice() in vk_menu.cpp"
    source = CHOICE_FIXTURE.replace("@@ENV_H@@", (VK / "compat" / "longshot_env.h").as_posix()) \
                           .replace("@@THREADED_CHOICE@@", block(s["menu"], marker))
    base = {k: v for k, v in os.environ.items() if k not in GATES}
    runs = [([setting, cores, mirrors], dict(base, **environment)) for setting, cores, mirrors, environment, *_ in CHOICE_CASES]
    built, out = compile_and_run(cxx, source, with_rule(s, tmp, name), tmp, name, runs)
    if not built:
        return "build: " + out
    for (setting, cores, mirrors, environment, render, recorder, asked, line), run in zip(CHOICE_CASES, out):
        want = "render=%d recorder=%d asked=%d" % (render, recorder, asked)
        lines = run.stdout.strip().splitlines()
        logged = [x for x in lines if x.startswith("vk: Threaded rendering: ")]
        if run.returncode or not lines or lines[-1] != want or len(logged) != 1 or \
                not logged[0][len("vk: Threaded rendering: "):].startswith(line):
            return "the row %d, %d cores, mirrors %d, %s: %s, expected %s and the line %r" % (
                setting, cores, mirrors, environment or "no switch", (run.stdout + run.stderr).strip(), want, line)
    return None


# Broken copies, each of which a check must catch: (what, the source, its text, the text in its place, the check)
BROKEN = [
    ("the recorder thread left off at 4 and 5 cores (18 Nov's tier back)", "rule",
     "    if (cores >= kAutoCores) return {true, true, Why::AutoBothThreads};",
     "    if (cores >= 6) return {true, true, Why::AutoBothThreads};\n"
     "    if (cores >= kAutoCores) return {true, false, Why::AutoBothThreads};", "rule"),
    ("the recorder thread left off in Auto's tier", "rule", "return {true, true, Why::AutoBothThreads};",
     "return {true, false, Why::AutoBothThreads};", "rule"),
    ("the recorder thread on below 4 cores", "rule", "return {false, false, Why::AutoFewCores};",
     "return {false, true, Why::AutoFewCores};", "rule"),
    ("both threads below 4 cores", "rule", "return {false, false, Why::AutoFewCores};",
     "return {true, true, Why::AutoFewCores};", "rule"),
    ("the render thread alone below 4 cores", "rule", "return {false, false, Why::AutoFewCores};",
     "return {true, false, Why::AutoFewCores};", "rule"),
    ("the recorder thread without the render thread", "rule", "return {true, true, Why::AutoBothThreads};",
     "return {false, true, Why::AutoBothThreads};", "rule"),
    ("both threads from 6 cores, 18 Nov's threshold", "rule", "constexpr unsigned kAutoCores = 4;",
     "constexpr unsigned kAutoCores = 6;", "rule"),
    ("both threads from 3 cores", "rule", "constexpr unsigned kAutoCores = 4;", "constexpr unsigned kAutoCores = 3;", "rule"),
    ("the old threshold, both threads from 12", "rule", "constexpr unsigned kAutoCores = 4;",
     "constexpr unsigned kAutoCores = 12;", "rule"),
    ("auto_picked false in Auto's tier", "rule", "{ return c.why == Why::AutoBothThreads; }", "{ return c.why == Why::On; }",
     "rule"),
    ("SMT siblings counted as cores", "rule", "cores += (core_masks[i] & usable) != 0;",
     "cores += (unsigned)__builtin_popcountll(core_masks[i] & usable);", "rule"),
    ("the unsafe gate from 6 cores only", "rule", "if (unsafe) return {false, false, Why::AutoUnsafe};",
     "if (unsafe && cores >= 6) return {false, false, Why::AutoUnsafe};", "rule"),
    ("Auto's gate from 6 cores only", "rule", "if (!auto_enabled) return {false, false, Why::AutoGate};",
     "if (!auto_enabled && cores >= 6) return {false, false, Why::AutoGate};", "rule"),
    ("Auto's tier logged as the render thread alone", "rule", '"Auto: on, the render thread and the recorder thread (',
     '"Auto: the render thread only (', "rule"),
    ("the render thread's tier named again", "rule", "    AutoFewCores, ", "    AutoRenderThread, AutoFewCores, ", "source"),
    ("usable_cores counting logical processors", "progcache",
     "threaded::cores_of(masks.data(), (unsigned)masks.size(), (uint64_t)proc)", "usable_cpus()", "rule"),
    ("usable_cores with no fall-back to the logical count", "progcache", "return cores ? std::min(cores, cpus) : cpus;",
     "return cores ? std::min(cores, cpus) : 0;", "rule"),
    ("usable_cores counting every core of the PC, not the mask's", "progcache",
     "threaded::cores_of(masks.data(), (unsigned)masks.size(), (uint64_t)proc)",
     "threaded::cores_of(masks.data(), (unsigned)masks.size(), ~0ull)", "rule"),
    ("the recorder hooked to the render thread's half", "replay", "(rec::g_setting_thread = menu_recorder_thread, true)",
     "(rec::g_setting_thread = menu_render_thread, true)", "readers"),
    ("the replay on the recorder thread's half", "replay", "return e ? atoi(e) : menu_render_thread() ? 1 : 0;",
     "return e ? atoi(e) : menu_recorder_thread() ? 1 : 0;", "readers"),
    ("the row over RX_VK_RECORDER", "recorder", 'if (!e && g_setting_thread && g_setting_thread()) e = "thread";',
     'if (g_setting_thread && g_setting_thread()) e = "thread";', "readers"),
    ("the menu counting logical processors", "menu", "threaded::choose(opts.threaded_rendering, cores, unsafe != nullptr)",
     "threaded::choose(opts.threaded_rendering, cpus, unsafe != nullptr)", "source"),
    ("the capture's test inverted (Auto on with the capture off)", "menu", "capture && atoi(capture) == 0 ?",
     "capture && atoi(capture) != 0 ?", "source"),
    ("the capture's switch not read", "menu", 'const char* capture = getenv("RX_VK_CAPTURE");',
     "const char* capture = nullptr;", "source"),
    ("the mirrors not asked", "menu", "if (threaded::auto_picked(c) && !pools_mirrored()) {",
     "if (false && !pools_mirrored()) {", "source"),
    ("the menu's recorder half from the render thread", "menu",
     "bool menu_recorder_thread() { return threaded_choice().recorder_thread; }",
     "bool menu_recorder_thread() { return threaded_choice().render_thread; }", "source"),
    ("the device policy by logical processors", "policy", "threaded::auto_picks(f.cores_usable)",
     "threaded::auto_picks(f.cpus_usable)", "source"),
    ("the row's help with 18 Nov's threshold", "menu", '"On with 4 or more processor cores', '"On with 6 or more processor cores',
     "source"),
    ("the row's help with the render thread's tier", "menu", "for the game, Off with fewer",
     "for the game, the render thread alone with 4 or 5, Off with fewer", "source"),
    ("the settings page with the render thread's tier", "settings", "both threads with 4 or more, Off with fewer",
     "both threads with 6 or more, the render thread alone with 4 or 5, Off with fewer", "source"),
    ("the switch table with the render thread's tier", "docs", "4 or more cores, the render thread and the recorder thread, as On; fewer",
     "6 or more cores, the render thread and the recorder thread, as On; 4 or 5, the render thread alone; fewer", "source"),
    ("the recorder's switch with 18 Nov's threshold", "switches", "Auto from 4 physical cores", "Auto from 6 physical cores",
     "source"),
    # (each of Auto's safety gates dropped with its text kept: the menu's own code, run)
    ("the capture's gate dropped", "menu", "capture && atoi(capture) == 0 ?", "false && capture && atoi(capture) == 0 ?",
     "choice"),
    ("RX_VK_LOW_SPEC's gate dropped", "menu", 'env_on("RX_VK_LOW_SPEC") ? "RX_VK_LOW_SPEC" :', 'false ? "RX_VK_LOW_SPEC" :',
     "choice"),
    ("RX_VK_DUMP's gate dropped", "menu", 'getenv("RX_VK_DUMP") ? "RX_VK_DUMP" :', 'false ? "RX_VK_DUMP" :', "choice"),
    ("RX_VK_BENCH's gate dropped", "menu", 'getenv("RX_VK_BENCH") ? "RX_VK_BENCH" :', 'false ? "RX_VK_BENCH" :', "choice"),
    ("the mirrors' gate dropped", "menu", "if (threaded::auto_picked(c) && !pools_mirrored()) {",
     "if (threaded::auto_picked(c) && !pools_mirrored() && false) {", "choice"),
    ("the mirrors asked before the cores", "menu", "if (threaded::auto_picked(c) && !pools_mirrored()) {",
     "if (!pools_mirrored() && threaded::auto_picked(c)) {", "choice"),
    ("the unsafe run given the threads", "menu", "c = threaded::without_mirrors(c, cores);",
     "c = threaded::Choice{true, true, threaded::Why::AutoUnsafe};", "choice"),
    # (no-mirror recorder-only rule mutations)
    ("without_mirrors enables the render thread", "rule", "return {false, true, Why::AutoRecorderOnly};",
     "return {true, true, Why::AutoRecorderOnly};", "rule"),
    ("without_mirrors enables recorder on 4-core non-mirrored handhelds", "rule",
     "inline constexpr unsigned kAutoRecorderOnlyCores = 6;",
     "inline constexpr unsigned kAutoRecorderOnlyCores = 4;", "rule"),
    ("without_mirrors leaves recorder off at 6+ cores", "rule",
     "if (cores >= kAutoRecorderOnlyCores) return {false, true, Why::AutoRecorderOnly};",
     "if (false && cores >= kAutoRecorderOnlyCores) return {false, true, Why::AutoRecorderOnly};", "rule"),
    ("threaded_choice passes logical processors to without_mirrors", "menu",
     "c = threaded::without_mirrors(c, cores);", "c = threaded::without_mirrors(c, cpus);", "choice"),
    ("the row's help omits no-mirror recorder-only tier", "menu",
     "recorder thread alone with 6 or more when buffer pools have no mirrors",
     "recorder thread off when buffer pools have no mirrors", "source"),
    ("switches.def omits no-mirror recorder-only tier", "switches",
     "and Auto alone with 6 or more physical cores when only buffer-pool mirrors are absent",
     "and off without mirrors", "source"),
]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    s = read_sources()

    bad = check_sources(s)
    if bad:
        raise SystemExit("FAIL: " + "\nFAIL: ".join(bad))
    for path in sorted(list(VK.glob("*.cpp")) + list(VK.glob("*.h")) + list((VK / "menu").glob("*.cpp")) +
                       list((ROOT / "host").glob("*.c"))):
        text = path.read_text(encoding="utf-8", errors="replace")
        reads = len(re.findall(r'getenv\("RX_VK_ASYNC"\)', text))
        # (the menu's one is threaded_env, the row's line that says the switch is set: it decides nothing. Its module's,
        # gpu/vk/menu/threads.cpp, #1633; vk_menu.cpp's before the modules)
        menu_reader = "threads.cpp" if (VK / "menu" / "threads.cpp").exists() else "vk_menu.cpp"
        if reads != (1 if path.name in ("vk_replay.cpp", menu_reader) else 0):
            raise SystemExit("FAIL: %s reads RX_VK_ASYNC itself (%d times): only render_thread_mode() decides by it, once, so "
                             "the row and the switch cannot disagree" % (path.relative_to(ROOT).as_posix(), reads))

    with tempfile.TemporaryDirectory(prefix="threaded-rendering-") as tmp:
        why = check_rule(a.cxx, s, tmp)
        if why:
            raise SystemExit("FAIL: the rule's fixture: " + why)
        print("PASS: the Threaded rendering rule, Auto's count of cores and the start's line")
        why = check_readers(a.cxx, s, tmp)
        if why:
            raise SystemExit("FAIL: the readers: " + why)
        print("PASS: the readers take their halves, the switches over them (%d environments)" % len(READER_CASES))
        why = check_choice(a.cxx, s, tmp)
        if why:
            raise SystemExit("FAIL: the menu's threaded_choice: " + why)
        print("PASS: the menu's threaded_choice: Auto's safety gates and the mirrors, run (%d cases)" % len(CHOICE_CASES))

        for i, (what, key, old, new, check) in enumerate(BROKEN):
            if s[key].count(old) != 1:
                raise SystemExit("FAIL: broken copy %r: its text is not once in %s" % (what, key))
            broken = dict(s, **{key: s[key].replace(old, new)})
            if check == "source":
                caught = bool(check_sources(broken))
            else:
                why = dict(rule=check_rule, readers=check_readers, choice=check_choice)[check](a.cxx, broken, tmp, "broken%d" % i)
                if why and why.startswith("build: "):
                    raise SystemExit("FAIL: broken copy %r did not build, so it proves nothing:\n%s" % (what, why))
                caught = why is not None
            if not caught:
                raise SystemExit("FAIL: broken copy %r passed the %s check" % (what, check))
        print("PASS: %d broken copies caught" % len(BROKEN))
    print("PASS: Threaded rendering: the rule's tiers by cores, the gate (on), and the row's way to the replay, the "
          "recorder and the host")


if __name__ == "__main__":
    main()
