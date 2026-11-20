// texture_frame's Linux memory-pressure hold (gpu/vk/vk_texture.cpp, gpu/vk/linux_pressure.h), cut out of vk_texture.cpp by
// tests/test_pressure_keep.py and pasted where ACTUAL is, then run over a session as the AYN Odin 3 played one on 2026-11-20:
// memory short from the title screen on, then Kokiri Forest drawing 1.1-1.3 GB of textures every few frames. The budget the
// hold leaves is read after each present, and the window it hands the eviction (linux_keep_presents, which the eviction's
// minimum age takes: NEED_UNUSED below is that line of vk_texture.cpp). Stand-ins: /proc/meminfo (fake_meminfo), the presents'
// clock (frame_clock_ms), the texture table (last frame drawn, bytes and the allocation's padding). No game, no GPU.
#include "linux_pressure.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>

namespace fixture {

int failures = 0;
void expect(bool ok, const char* what, ...) {
    if (ok) return;
    va_list ap;
    va_start(ap, what);
    std::printf("FAIL: ");
    std::vprintf(what, ap);
    std::printf("\n");
    va_end(ap);
    failures++;
}

constexpr uint64_t MB = 1ull << 20;
constexpr int kFps = 20;                         // (the forest's frame rate on the Odin 3)
constexpr uint64_t kNormal = 2560 * MB;          // (the integrated GPU's texture budget, device_policy.h)

// ---- what texture_frame's code around the hold is ----
enum { VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU = 1, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU = 2 };
struct Props {
    int deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
};
struct Dev {
    Props props;
} dev;
uint64_t frame_no = 1, total_bytes = 0, budget = 0, rule_budget = 0;
int pool_pressure = 0;
struct Compact {
    bool pressed = false;
    uint64_t next_check = 0;
} compact;
namespace vram_compact {
inline uint64_t rest(bool) { return 10; }
}
bool linux_pressed = false, linux_entered_pressure = false;
std::atomic<uint64_t> linux_shortfall_bytes{0}, linux_shortfall_seq{0};
uint64_t linux_keep_presents = 0;
uint64_t linux_avail_reserve() { return linux_pressure::avail_reserve(nullptr); }
uint64_t commit_reserve() { return linux_pressure::commit_reserve(std::getenv("RX_VK_COMMIT_RESERVE_MB")); }
void logf(const char*, ...) {}
struct Texture {
    uint64_t last_frame = 0, bytes = 0, pad = 0;   // (pad: what the allocator gave past the texels, ~8%: total_bytes counts it)
};
std::unordered_map<int, std::unique_ptr<Texture>> textures;
int64_t now_ms = 0;
int64_t frame_clock_ms() { return now_ms; }
uint64_t mem_avail = 0, mem_swap = 0;
bool mem_has_swap = true;
bool fake_meminfo(uint64_t* avail, uint64_t* swap_free, const char*, uint64_t*, bool* has_swap) {
    *avail = mem_avail, *swap_free = mem_swap;
    if (has_swap) *has_swap = mem_has_swap;
    return true;
}

void hold_block() {
    {
        /* ACTUAL */
}
// (the eviction's minimum age, as texture_frame takes it: a sweep's or a pass's, and the keep window's)
uint64_t need_unused_of(bool sweep, uint64_t min_unused) {
    /* NEED_UNUSED */
    return need_unused;
}

// ---- a session ----
// A scene: groups of textures, each drawn `ago` seconds before every present (0: in every frame)
struct Group {
    uint64_t mb;
    int ago;
};
void scene(std::initializer_list<Group> groups) {
    textures.clear();
    int id = 0;
    for (const Group& g : groups) textures[id++] = std::make_unique<Texture>(Texture{0, g.mb * MB, g.mb * MB / 12});
}
uint64_t lowest = UINT64_MAX, highest = 0;   // (the lowest and highest budget of the last play)
// `seconds` of presents with that scene and that memory: the hold's block at each, as texture_frame runs it
uint64_t play(std::initializer_list<Group> groups, int seconds, uint64_t avail_mb, uint64_t swap_mb) {
    scene(groups);
    mem_avail = avail_mb * MB, mem_swap = swap_mb * MB;
    lowest = UINT64_MAX, highest = 0;
    for (int f = 0; f < seconds * kFps; f++) {
        total_bytes = 0;
        int id = 0;
        for (const Group& g : groups) {
            Texture& t = *textures[id++];
            const uint64_t back = (uint64_t)g.ago * kFps;
            t.last_frame = frame_no > back ? frame_no - back : 0;
            total_bytes += t.bytes + t.pad;
        }
        budget = rule_budget = kNormal;
        hold_block();
        lowest = std::min(lowest, budget), highest = std::max(highest, budget);
        frame_no++;
        now_ms += 1000 / kFps;
    }
    return budget;
}
// what a scene's groups drawn within `seconds` hold, padding and all, as the eviction counts them
uint64_t drawn_within(std::initializer_list<Group> groups, int seconds) {
    uint64_t b = 0;
    for (const Group& g : groups)
        if (g.ago <= seconds) b += g.mb * MB + g.mb * MB / 12;
    return b;
}
unsigned long long mb(uint64_t b) { return (unsigned long long)(b / MB); }

// The title screen, memory already short (1279 MB available, the reserve 1536), 50 MB of textures: the hold is on
uint64_t title() { return play({{50, 0}}, 30, 1279, 14791); }
// Kokiri Forest: 1000 MB drawn in every frame, 200 MB 10 s ago, 600 MB last drawn 45 s ago (an area left behind)
constexpr std::initializer_list<Group> kForest = {{1000, 0}, {200, 10}, {600, 45}};
// the window's presents at 20 fps, as the clock's notes (one every 30 presents) give them: 20 s and at most a note more
bool window_presents(uint64_t p) { return p >= 20 * kFps && p <= 20 * kFps + 30; }

void run(const std::string& mode) {
    const uint64_t kept = drawn_within(kForest, 20);   // (1300 MB with their padding)
    if (mode == "forest") {
        expect(title() == 512 * MB, "the title screen's budget under the hold: %llu MB, not the 512 MB floor", mb(budget));
        const uint64_t b = play(kForest, 60, 250, 9800);
        expect(b >= kept,
               "Kokiri Forest under the hold, 9.8 GB of swap free: the budget %llu MB, under the %llu MB the last 20 s drew "
               "(what it lets go is made again within seconds)", mb(b), mb(kept));
        expect(b < total_bytes, "Kokiri Forest under the hold: the budget %llu MB keeps the 600 MB not drawn for 45 s too", mb(b));
        expect(window_presents(linux_keep_presents),
               "Kokiri Forest under the hold: the eviction told to leave %llu presents, not the 20 s window's %d",
               (unsigned long long)linux_keep_presents, 20 * kFps);
        const uint64_t age = need_unused_of(false, 60);
        expect(age >= 20 * kFps, "the eviction's minimum age under the keep: %llu presents, under the 20 s window's %d (the "
               "budget's 7/8 target and the padding let go of what the window drew)", (unsigned long long)age, 20 * kFps);
    } else if (mode == "band") {   // (between the reserve and its clearing line: the hold stays on, and so does the keep)
        title();
        play(kForest, 30, 250, 9800);
        play(kForest, 30, 1650, 9800);
        expect(linux_pressed && lowest >= kept, "1650 MB available, the hold still on: the budget fell to %llu MB, under the "
               "%llu MB the last 20 s drew", mb(lowest), mb(kept));
    } else if (mode == "entry") {   // (the hold entered in the forest itself: the keep from its first sample)
        play(kForest, 30, 1900, 9800);
        play(kForest, 30, 250, 9800);
        expect(lowest >= kept, "the hold entered in the forest: the budget fell to %llu MB, under the %llu MB the last 20 s drew",
               mb(lowest), mb(kept));
    } else if (mode == "step-up") {   // (memory back: the hold steps its budget up from the cut, never under the keep)
        play({{1000, 0}, {200, 10}, {350, 45}}, 30, 1900, 9800);
        play({{1000, 0}, {200, 10}, {350, 45}}, 30, 250, 9800);
        const uint64_t b = play({{1000, 0}, {200, 10}, {350, 45}}, 4, 1900, 9800);
        expect(linux_pressed && lowest >= kept, "the hold stepping back up (%s): the budget fell to %llu MB, under the %llu MB "
               "the last 20 s drew", linux_pressed ? "on" : "already off", mb(lowest), mb(kept));
        (void)b;
    } else if (mode == "swap-gone") {   // (the Steam Deck the rule was made for: swap nearly full, earlyoom near)
        title();
        const uint64_t b = play(kForest, 60, 180, 60);
        expect(b == 512 * MB, "swap nearly gone too: the budget %llu MB, not today's 512 MB floor", mb(b));
        expect(!linux_keep_presents && need_unused_of(false, 60) == 60, "swap nearly gone too: the eviction still told to keep "
               "%llu presents", (unsigned long long)linux_keep_presents);
    } else if (mode == "swap-hover") {   // (swap hovering at the reserve: short once, short until it clears the reserve by 256 MB)
        title();
        play(kForest, 30, 250, 2000);   // (2250 MB: short)
        play(kForest, 30, 250, 2400);   // (2650: over the reserve, not past its clearing line)
        expect(highest == 512 * MB, "swap hovering at the reserve: the budget back up to %llu MB before the reserve cleared by "
               "256 MB", mb(highest));
    } else if (mode == "swap-no-room") {   // (swap with less room than what would be kept)
        title();
        const uint64_t b = play(kForest, 60, 1400, 1300);
        expect(b == 512 * MB, "1.3 GB of swap free, %llu MB to keep: the budget %llu MB, not the 512 MB floor", mb(kept), mb(b));
    } else if (mode == "no-swap") {   // (no swap at all: nothing takes what the textures keep)
        mem_has_swap = false;
        title();
        const uint64_t b = play(kForest, 60, 250, 0);
        expect(b == 512 * MB, "no swap: the budget %llu MB, not today's 512 MB floor", mb(b));
    } else if (mode == "commit-off") {   // (RX_VK_COMMIT_RESERVE_MB=0: the keep's own emergency stays)
        title();
        const uint64_t b = play(kForest, 60, 250, 2000);   // (2250 MB with swap: short of the default reserve, room for the keep)
        expect(b == 512 * MB, "RX_VK_COMMIT_RESERVE_MB=0, 2250 MB with swap: the budget %llu MB, not the 512 MB floor", mb(b));
    } else if (mode == "off") {   // (RX_VK_PRESSURE_KEEP_S=0: as before)
        title();
        const uint64_t b = play(kForest, 60, 250, 9800);
        expect(b == 512 * MB, "RX_VK_PRESSURE_KEEP_S=0: the budget %llu MB, not today's 512 MB floor", mb(b));
        expect(need_unused_of(false, 60) == 60 && need_unused_of(true, 60) == 120,
               "RX_VK_PRESSURE_KEEP_S=0: the eviction's minimum age %llu (a pass) and %llu (a sweep), not 60 and 120",
               (unsigned long long)need_unused_of(false, 60), (unsigned long long)need_unused_of(true, 60));
    } else if (mode == "cap") {   // (never more than the budget without the hold)
        title();
        const uint64_t b = play({{3000, 0}}, 60, 250, 9800);
        expect(b <= kNormal, "3000 MB drawn in every frame: the budget %llu MB, over the normal %llu MB", mb(b), mb(kNormal));
    } else if (mode == "recover") {   // (memory back: the hold lets go as before)
        title();
        play(kForest, 30, 250, 9800);
        const uint64_t b = play(kForest, 120, 2200, 9800);
        expect(b == kNormal && !linux_pressed && !linux_keep_presents, "memory back (2200 MB available): the budget %llu MB, "
               "the hold %s", mb(b), linux_pressed ? "still on" : "off");
    } else if (mode == "discrete") {   // (a discrete GPU: the textures' budget is not cut, as before)
        dev.props.deviceType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        title();
        const uint64_t b = play(kForest, 60, 250, 9800);
        expect(b == kNormal && !linux_keep_presents, "a discrete GPU under the hold: the budget %llu MB, cut", mb(b));
    } else {
        std::printf("FAIL: no case %s\n", mode.c_str());
        failures++;
    }
}

} // namespace fixture

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    fixture::run(argv[1]);
    if (fixture::failures) return 1;
    std::printf("%s: ok\n", argv[1]);
    return 0;
}
