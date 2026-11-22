// (longshot) The start's wait for workers that build pipelines in the driver before the title, bounded by their progress:
// a build the driver takes minutes over, or never finishes, must not hold the start for good behind a screen that shows
// nothing moving. vk_draw.cpp precompile_at_create (RX_VK_LOAD=create, the default): on RDNA 1 cards (AMD's Windows driver,
// the mesh programs through the compute fallback) a start without it can sit at 0%, "Loading the shader cache", for good.
// RX_VK_LOAD=start's precompile has the same bound.
// Standard library only: tests/test_bounded_wait.py
#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>

namespace bounded_wait {

enum class End {
    kDone,      // (every item finished)
    kStopped,   // (stopped(): the window was closed)
    kStalled,   // (no item finished for stall_s seconds: the caller goes on, the workers left running)
};

/// Waits until done reaches total (kDone), stopped() says so (kStopped), or no item has finished for stall_s seconds
/// (kStalled; stall_s 0 or less: never). tick(done) is called each round before pause(): the start-up screen's progress and
/// the log. now() is the clock (std::chrono::steady_clock::now in the game; the test's own). While held() says so, no stall
/// is counted: the stall's seconds start again when it stops (a --prepare-shaders run with a compile at work, which its
/// watchdog bounds instead, vk_draw.cpp precompile_at_create)
template <class Now, class Tick, class Stopped, class Pause, class Held>
End wait(const std::atomic<size_t>& done, size_t total, int stall_s, Now now, Tick tick, Stopped stopped, Pause pause,
         Held held) {
    size_t last = done.load();
    auto last_change = now();
    for (;;) {
        const size_t d = done.load();
        tick(d);
        if (d >= total) return End::kDone;
        if (stopped()) return End::kStopped;
        const auto t = now();
        if (d != last) last = d, last_change = t;
        else if (stall_s > 0 && t - last_change >= std::chrono::seconds(stall_s)) {
            if (!held()) return End::kStalled;
            last_change = t;
        }
        pause();
    }
}

template <class Now, class Tick, class Stopped, class Pause>
End wait(const std::atomic<size_t>& done, size_t total, int stall_s, Now now, Tick tick, Stopped stopped, Pause pause) {
    return wait(done, total, stall_s, now, tick, stopped, pause, [] { return false; });
}

}  // namespace bounded_wait
