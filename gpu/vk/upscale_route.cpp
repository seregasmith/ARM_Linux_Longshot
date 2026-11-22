// (longshot) The gameplay upscale as drawn, for the FPS counter's footer: upscale_route.h
#include "upscale_route.h"

#include <atomic>
#include <chrono>
#include <mutex>

namespace mkwvk::upscale_route {
namespace {
int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
constexpr int64_t kWantNs = 1000000000;    // (the footer refreshes 4 times a second: a second without one, nobody reads)
constexpr int64_t kSceneNs = 100000000;    // (a scene size seen this long ago is still this frame's or the one before's)

std::atomic<int64_t> wanted_at{0};
std::atomic<uint64_t> scene_wh{0};   // (replay thread only: the largest scene-like target lately, and when)
std::atomic<int64_t> scene_at{0};

std::mutex mu;   // (one record: the replay thread writes it at most a few times a frame, the footer reads it 4 times a second)
Sample last;
int64_t last_at = 0;
}   // namespace

void want() { wanted_at.store(now_ns(), std::memory_order_relaxed); }

bool wanted() {
    const int64_t at = wanted_at.load(std::memory_order_relaxed);
    return at && now_ns() - at < kWantNs;
}

void note_scene(uint32_t w, uint32_t h) {
    const int64_t now = now_ns();
    const uint64_t wh = scene_wh.load(std::memory_order_relaxed);
    const uint64_t area = (uint64_t)w * h, had = (wh >> 32) * (wh & 0xFFFFFFFFu);
    if (area >= had || now - scene_at.load(std::memory_order_relaxed) > kSceneNs / 2) {
        scene_wh.store((uint64_t)w << 32 | h, std::memory_order_relaxed);
        scene_at.store(now, std::memory_order_relaxed);
    }
}

bool is_scene(uint32_t w, uint32_t h) {
    return scene_wh.load(std::memory_order_relaxed) == ((uint64_t)w << 32 | h) &&
           now_ns() - scene_at.load(std::memory_order_relaxed) < kSceneNs;
}

void publish(const Sample& s) {
    if (!s.in_w || !s.in_h || !s.out_w || !s.out_h) return;
    std::lock_guard lk(mu);
    last = s, last_at = now_ns();
}

bool latest(Sample* out, int64_t max_age_ns) {
    std::lock_guard lk(mu);
    if (!last_at || now_ns() - last_at > max_age_ns) return false;
    *out = last;
    return true;
}

}   // namespace mkwvk::upscale_route
