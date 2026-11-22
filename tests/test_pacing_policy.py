"""Characterize the current #1519 host cap and swapchain policy with source cuts.

Run with python -I -B tests/test_pacing_policy.py [--cxx clang++].
The Windows and non-Windows selection branches run against the same fake
surface. No game, Vulkan device, display, or real waits are needed.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from srccut import between, block  # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]

STUBS = r'''
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "gpu/vk/compat/longshot_env.h"
#include "gpu/vk/fps_stats.h"
#include "gpu/vk/vk_api.h"

namespace fps_stats = mkwvk::fps_stats;
struct { int fps_cap = 0, vsync = 0, framegen = 0; } opts;
int refresh_mhz = 0;
int display_refresh_mhz() { return refresh_mhz; }

using Ctx = void;
using LONG = int32_t;
using LONG64 = int64_t;
using DWORD = uint32_t;
#define WINAPI
int64_t clock_ns = 1000000000LL, slept_ns;
int64_t hos_now_ns() { return clock_ns; }
void hos_log(const char*, ...) {}
void hos_sleep_ns(int64_t ns) { slept_ns = ns; }
int hos_ini_option(const char*, int fallback) { return fallback; }
bool per_frame = false;
int rx_option(int id, int fallback) {
    if (id == HOS_OPT_FPS_CAP) return opts.fps_cap;
    if (id == 0) return !per_frame;
    return fallback;
}

// Only the fields touched by the host's game-time/limiter code have meaning here.
constexpr uint64_t MODULE = 0x1000;
alignas(8) unsigned char guest[0x4000];
#define GP(address) (guest + (address))
volatile uint64_t hos_gfx_module = MODULE, hos_res_module, hos_step_probe = 0x2000;
volatile int hos_fixed_resolution, hos_dynres_runs, hos_scene_rgba16f;
volatile float hos_text_scale, hos_camera_scale;
volatile uint32_t hos_frame_seconds_bits, hos_fixed_scale_bits;
volatile int64_t hos_acquire_slept_ns;
volatile int hos_fps_timing;
void InterlockedAdd64(volatile int64_t*, int64_t) {}

enum TickerStop { FirstSleep };
void vsync_sleep(int64_t ns) { slept_ns = ns; throw FirstSleep; }
LONG nvsync = 0;
int vsync_events[1];
void hos_system_event_signal(int) {}

uint8_t guest_vrr;
int guest_result;
#define HLE(name) void name()
#define ARG(i) (i)
#define RET(result) (guest_result = (result))

enum VkPresentModeKHR { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR };
enum { DLSS_OK };
bool ngx_accepts = false;
bool dlss_fg_runs(int) { return ngx_accepts; }
bool dlss_fg_wanted() { return true; }  // allow the requested-but-rejected path
int dlss_why() { return DLSS_OK; }
int ngx_most = 0;
int dlss_fg_most() { return ngx_most; }
void logf(const char*, ...) {}
struct VkSwapchainCreateInfoKHR {
    int type;
    int surface = 0;
    uint32_t minImageCount = 0;
};
enum { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
struct { int surface = 1; } dev;
'''

CHECKS = r'''
void expect(bool ok, const char* row, const char* field) {
    if (!ok) {
        std::fprintf(stderr, "FAIL (%s): %s\n", row, field);
        std::exit(1);
    }
}

float game_step() {
    float step;
    const uint32_t bits = hos_frame_seconds_bits;
    std::memcpy(&step, &bits, sizeof step);
    return step;
}

// Run each host row in a new process so production's read-once switches start fresh.
void host_row(char** args) {
    const char* name = args[1];
    opts.fps_cap = atoi(args[2]);
    per_frame = atoi(args[3]) != 0;
    const int movie_age_ns = atoi(args[4]);  // -1 means a player has never updated
    if (movie_age_ns >= 0) movie_seen_at = (uint64_t)(clock_ns - movie_age_ns);
    const int host_cap = atoi(args[5]), overlay_cap = atoi(args[6]), interval = atoi(args[7]);
    const double seconds = atof(args[8]);   // zero means the fractional hook is unset
    const int nominal_rate = atoi(args[9]);
    const bool no_pacing = atoi(args[10]) != 0;

    auto& limit = *reinterpret_cast<int*>(GP(MODULE + 0xfc));
    auto& rate = *reinterpret_cast<int*>(GP(MODULE + 0x2c8));
    limit = rate = 60;
    realtime_step(clock_ns);
    expect(fps_max() == host_cap && limit == host_cap, name, "host cap and guest limiter");
    const int64_t tick = 1000000000LL / host_cap;
    try { vsync_main(nullptr); } catch (TickerStop) {}
    expect(slept_ns == tick, name, "first ticker period");
    expect(hos_vi_native_interval(nullptr, 2) == 2 && present_interval == interval, name, "requested and effective VI interval");
    slept_ns = 0;
    schedule_show(clock_ns);
    hos_vi_native_acquire(1);
    expect(slept_ns == (no_pacing ? 0 : tick * interval), name, "host texture acquire sleep");

    // 1 ms between presents exercises the capped fractional step's lower bound.
    clock_ns += 1000000;
    realtime_step(clock_ns);
    expect(std::abs(game_step() - seconds) < 1e-7, name, "game-time hook");
    expect(rate == nominal_rate, name, "nominal game step slot");
    if (seconds > 0) {
        clock_ns += 200000000;
        realtime_step(clock_ns);
        expect(std::abs(game_step() - 1.0 / 15) < 1e-7, name, "upper bound of the fractional step");
    }

    expect(fps_cap_now() == overlay_cap, name, "FPS overlay cap");
    const double target = overlay_cap ? 1000.0 / overlay_cap : 0;
    expect(std::abs(fps_target_ms() - target) < 1e-7, name, "FPS overlay target");

    guest[0] = 1;
    guest_result = -1;
    _ZN2nn2vi24QueryDisplayVrrSupportedEPbPKNS0_7DisplayE();
    expect(guest[0] == 0 && guest_result == 0, name, "guest QueryDisplayVrrSupported");

    if (host_cap == 240 && overlay_cap == 60) {
        clock_ns = (int64_t)movie_seen_at + 249999999LL;
        realtime_step(clock_ns);
        expect(fps_max() == 240 && limit == 240, name, "last ns of movie override");
        ++clock_ns;
        realtime_step(clock_ns);
        expect(fps_max() == 60 && limit == 60, name, "movie expiry restores the limiter");
    }
}

void mode_rows() {
    using Mode = VkPresentModeKHR;
    constexpr Mode F = VK_PRESENT_MODE_FIFO_KHR, M = VK_PRESENT_MODE_MAILBOX_KHR, I = VK_PRESENT_MODE_IMMEDIATE_KHR;
    // Columns are no optional modes, mailbox only, immediate only, both.
    const std::vector<Mode> surfaces[] = {{F}, {F, M}, {F, I}, {F, I, M}};
    struct Row { const char* name; int vsync, fg; bool dlss; Mode expected[4]; };
    const Row rows[] = {
        {"Auto, FG off",      0, 0, false, {F, M, TEST_WINDOWS ? I : F, M}},
        {"Auto, Auto FG",     0, 1, true,  {F, M, TEST_WINDOWS ? I : F, M}},
        {"Auto, built-in 3x", 0, 3, false, {F, M, TEST_WINDOWS ? I : F, M}},
        {"Auto, DLSS 2x",     0, 2, true,  {F, F, F, F}},
        {"On, FG off",        1, 0, false, {F, F, F, F}},
        {"On, built-in 3x",   1, 3, false, {F, F, F, F}},
        {"Off, DLSS 2x",      2, 2, true,  {F, M, I, I}},
        {"Off, FG off",       2, 0, false, {F, M, I, I}},
    };
    for (const Row& row : rows) {
        opts.vsync = row.vsync;
        opts.framegen = row.fg;
        ngx_accepts = row.dlss;
        for (int refresh : {0, 60000, 165000}) {
            refresh_mhz = refresh;
            for (int surface = 0; surface < 4; ++surface)
                expect(select_swapchain(surfaces[surface], {2, 8}).mode == row.expected[surface],
                       row.name, "present mode for advertised surface modes");
        }
    }
    expect(select_swapchain({}, {2, 8}).mode == F, "no reported modes", "mandatory FIFO fallback");
    // The overlay only consults display refresh for explicit On and saved FG Off.
    opts.fps_cap = 2;
    refresh_mhz = 59940;
    opts.vsync = 0;
    opts.framegen = 2;
    ngx_accepts = true;
    expect(select_swapchain({F, M, I}, {2, 8}).mode == F &&
           std::abs(fps_target_ms() - 1000.0 / 120) < 1e-7, "Auto + DLSS FIFO", "overlay ignores actual FIFO");
    opts.vsync = 1;
    expect(std::abs(fps_target_ms() - 1000.0 / 120) < 1e-7, "On + fixed FG", "overlay ignores refresh with FG selected");
    opts.framegen = 0;
    expect(std::abs(fps_target_ms() - 1000.0 / 60) < 1e-7, "On, FG off", "overlay can use display refresh");
    refresh_mhz = 0;
    expect(std::abs(fps_target_ms() - 1000.0 / 120) < 1e-7, "On, refresh unknown", "overlay uses the cap");
}

void fg_rows(bool timeline_off) {
    using Mode = VkPresentModeKHR;
    constexpr Mode F = VK_PRESENT_MODE_FIFO_KHR, I = VK_PRESENT_MODE_IMMEDIATE_KHR;
    struct Row { const char* name; int fg, ngx_limit, mhz; uint32_t images, slots, legacy_images; Mode mode; };
    const Row rows[] = {
        {"FIFO, 2x known refresh",    2, 0, 165000, 6, 4, 4, F},
        {"FIFO, 3x known refresh",    3, 0, 165000, 8, 6, 5, F},
        {"FIFO, 3x unknown refresh",  3, 0,      0, 5, 3, 5, F},
        {"FIFO, 8x too large",        8, 0, 165000, 6, 3, 6, F},
        {"FIFO, 8x capped to 2x",     8, 1, 165000, 6, 4, 4, F},
        {"Immediate, 3x no refresh",  3, 0,      0, 8, 6, 5, I},
        {"Immediate, 2x no refresh",  2, 0,      0, 6, 4, 4, I},
        {"FIFO, Auto FG",             1, 0, 165000, 4, 3, 4, F},
    };
    for (const Row& row : rows) {
        opts.vsync = row.mode == F ? 1 : 2;
        opts.framegen = row.fg;
        ngx_most = row.ngx_limit;
        refresh_mhz = row.mhz;
        const auto actual = select_swapchain({F, I}, {2, 8});
        const uint32_t expected_images = timeline_off ? row.legacy_images : row.images;
        expect(actual.mode == row.mode && actual.images == expected_images &&
               actual.slots == (timeline_off ? 3 : row.slots), row.name, "swapchain images and in-flight slots");
    }
    expect(fixed_on_timeline(true, timeline_off) == !timeline_off && !fixed_on_timeline(false, timeline_off),
           "fixed FG", "game timeline gate");
    expect(fifo_slot(102, 100, 10, 102) == 110 && fifo_slot(102, 100, 0, 102) == 102,
           "fixed FG FIFO", "one refresh between slots when the refresh is known");
}

int main(int argc, char** argv) {
    if (argc == 11) host_row(argv);
    else if (!std::strcmp(argv[1], "modes")) mode_rows();
    else fg_rows(!std::strcmp(argv[1], "legacy"));
}
'''


def source_cuts():
    def read(path):
        return (ROOT / path).read_text(encoding="utf-8")

    host = read("host/nn_vi.c")
    menu = read("gpu/vk/vk_menu.cpp")
    main = read("gpu/vk/vk_main.cpp")
    fg = read("gpu/vk/vk_framegen.cpp")
    replay = read("gpu/vk/vk_replay.cpp")
    selection = between(main, "    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;",
                        "    ci.imageFormat = fmt.format;").replace("#ifdef _WIN32", "#if TEST_WINDOWS")
    return "\n".join([
        between(host, "enum { FPS_UNCAPPED", "static int64_t next_frame_at"),
        block(host, "static DWORD WINAPI vsync_main("),
        between(host, "static volatile int present_interval", "/* (longshot) The frame rate cap"),
        "[[maybe_unused]] " + between(host, "static int64_t next_frame_at", "static volatile LONG64 presents;"),
        block(host, "int hos_vi_native_interval("),
        between(host, "static int64_t shown_at", "/* (mkw-recomp) MKW's frame pacer"),
        "void schedule_show(int64_t now) {",
        between(host, "    const int64_t period = 1000000000LL / fps_max() * present_interval;",
                "    const int64_t n = InterlockedIncrement64(&presents);"),
        "}",
        block(host, "static float text_scale("),
        block(host, "static float camera_scale("),
        block(host, "static int realtime_rate("),
        block(host, "static void realtime_step("),
        block(host, "HLE(_ZN2nn2vi24QueryDisplayVrrSupported"),
        block(menu, "int menu_vsync() {"),
        block(menu, "int fps_cap_now() {"),
        block(menu, "double fps_target_ms() {"),
        block(fg, "int framegen_capped("),
        block(fg, "int framegen_now() {"),
        block(fg, "bool fixed_on_timeline("),
        block(fg, "int64_t fifo_slot("),
        block(main, "int swapchain_framegen() {"),
        block(main, "bool swapchain_dlssg() {"),
        between(read("gpu/vk/vk_internal.h"), "constexpr int kPresentSlotsMax", ";") + ";",
        block(replay, "int present_slots_for("),
        """struct Caps { uint32_t minImageCount, maxImageCount; };
struct Selection { VkPresentModeKHR mode; uint32_t images, slots; };
Selection select_swapchain(const std::vector<VkPresentModeKHR>& modes, Caps caps) {""",
        selection,
        "    return {mode, ci.minImageCount, slots_made};\n}",
    ])


# name, cap row, Per frame, movie age in ns, host cap, overlay cap, VI interval,
# short game step, nominal rate at that step, and optional read-once switches.
HOST_ROWS = [
    ("30 real time",         -1, 0,        -1,   30,  30, 1,  1/240, 60, {}),
    ("30 Per frame, movie",  -1, 1,         0,   30,  30, 1,  1/240, 60, {}),
    ("60 real time",          0, 0,        -1,   60,  60, 1,      0, 60, {}),
    ("60 Per frame",          0, 1,        -1,   60,  60, 2,      0, 60, {}),
    ("60 Per frame, movie",   0, 1,         0,  240,  60, 2,  1/240, 60, {}),
    ("60 stale boundary",     0, 0, 250000000,   60,  60, 1,      0, 60, {}),
    ("60 movie disabled",     0, 0,         0,   60,  60, 1,      0, 60, {"HOS_MOVIE_PACE": "0"}),
    ("120 Per frame, movie",  2, 1,         0,  120, 120, 2,  1/240, 60, {}),
    ("uncapped",              6, 0,        -1, 1000,   0, 1, 1/1000, 60, {}),
    ("120, no acquire pace",  2, 0,        -1,  120, 120, 1,  1/240, 60, {"HOS_NO_PACING": "1"}),
    ("120, step hook off",    2, 0,        -1,  120, 120, 1,      0, 120, {"HOS_FPS_STEP": "0"}),
    ("env overrides 120",     2, 0,         0,  240,  60, 1,  1/240, 60, {"HOS_FPS_MAX": "60"}),
    ("unknown cap row",       7, 0,        -1,   60,  60, 1,      0, 60, {}),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    fixture = "\n".join((STUBS, source_cuts(), CHECKS))
    env = {k: v for k, v in os.environ.items() if not k.startswith(("HOS_", "RX_VK_"))}
    with tempfile.TemporaryDirectory(prefix="pacing-policy-") as tmp:
        source = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture.exe"
        source.write_text(fixture, encoding="utf-8")
        for windows in (0, 1):
            subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
                            f"-DTEST_WINDOWS={windows}", "-I", str(ROOT), str(source), "-o", str(exe)], check=True)
            subprocess.run([exe, "modes"], env=env, check=True)
        subprocess.run([exe, "timeline"], env=env, check=True)
        subprocess.run([exe, "legacy"], env=dict(env, RX_VK_FG_TIMELINE="0"), check=True)
        for *row, switches in HOST_ROWS:
            cmd = [exe, *map(str, row), str(int("HOS_NO_PACING" in switches))]
            subprocess.run(cmd, env=dict(env, **switches), check=True)
    print(f"PASS: {len(HOST_ROWS)} host/overlay rows, 192 mode cases, and 16 FG allocation cases")


if __name__ == "__main__":
    main()
