// warm_bounded (gpu/vk/warm_bounded.h): every item worked on once, never more alive at once than there are threads, a stop
// honoured before the next item, progress reported; and held (memory short): one worker at a time while hold() says so, the
// others back once it lets go, and the work always ends. No game, no GPU.
#include <algorithm>
#include "../gpu/vk/warm_bounded.h"

#include <cstdio>
#include <mutex>

namespace {
int failures = 0;
void expect(bool ok, const char* what) {
    if (!ok) std::printf("FAIL: %s\n", what), failures++;
}

void bounded(size_t n, unsigned threads) {
    std::atomic<int> alive{0}, most{0};
    std::vector<std::atomic<int>> times(n);
    size_t last_tick = 0, ticks = 0;
    const size_t done = warm_bounded::run(n, threads, [&](size_t i) {
        const int now = ++alive;   // (made: a pipeline, in the game)
        int seen = most.load();
        while (now > seen && !most.compare_exchange_weak(seen, now)) {}
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        times[i]++;
        --alive;                   // (let go before the thread takes the next)
    }, [] { return false; }, [&](size_t d) { expect(d >= last_tick, "progress never goes back"), last_tick = d, ticks++; });
    expect(done == n, "every item is done");
    for (size_t i = 0; i < n; i++) expect(times[i] == 1, "each item is worked on exactly once");
    expect(most.load() <= (int)std::max(threads, 1u), "never more alive at once than there are threads");
    expect(n == 0 || most.load() >= 1, "something was made");
    expect(alive.load() == 0, "nothing is left alive at the end");
    expect(ticks >= 1 && last_tick == n, "the last progress report is the whole count");
}

void stopped() {
    std::atomic<size_t> worked{0};
    std::atomic<bool> stop{false};
    const size_t done = warm_bounded::run(100000, 4, [&](size_t) {
        if (++worked == 50) stop = true;
    }, [&] { return stop.load(); }, [](size_t) {});
    expect(done == worked.load() && done >= 50 && done < 100000, "a stop ends the work before the next item, and what was done is counted");
}

// (memory short the whole time: one worker at a time goes on, the work still ends, each item once)
void held_all_along(size_t n, unsigned threads) {
    std::atomic<int> alive{0}, most{0};
    std::vector<std::atomic<int>> times(n);
    std::atomic<size_t> held{0};
    const size_t done = warm_bounded::run(n, threads, [&](size_t i) {
        const int now = ++alive;
        int seen = most.load();
        while (now > seen && !most.compare_exchange_weak(seen, now)) {}
        std::this_thread::sleep_for(std::chrono::microseconds(300));
        times[i]++;
        --alive;
    }, [] { return false; }, [] { return true; }, [](size_t) {}, &held);
    expect(done == n, "held all along: every item is done");
    for (size_t i = 0; i < n; i++) expect(times[i] == 1, "held all along: each item is worked on exactly once");
    expect(most.load() <= 1, "held all along: never more than one worker at work");
    expect(threads <= 1 || n <= 1 || held.load() >= 1, "held all along: the others waited, and that was counted");
}

// (memory short for a while: one at a time then, all of them again after, and nothing lost)
void held_for_a_while() {
    constexpr size_t n = 400;
    std::atomic<int> alive{0}, most_held{0}, most_after{0};
    std::atomic<bool> short_now{true};
    std::atomic<size_t> worked{0};
    std::vector<std::atomic<int>> times(n);
    const size_t done = warm_bounded::run(n, 6, [&](size_t i) {
        const bool was_short = short_now.load();
        const int now = ++alive;
        std::atomic<int>& most = was_short ? most_held : most_after;
        int seen = most.load();
        while (now > seen && !most.compare_exchange_weak(seen, now)) {}
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        times[i]++;
        --alive;
        if (++worked == 100) short_now = false;   // (memory back)
    }, [] { return false; }, [&] { return short_now.load(); }, [](size_t) {});
    expect(done == n, "held for a while: every item is done");
    for (size_t i = 0; i < n; i++) expect(times[i] == 1, "held for a while: each item is worked on exactly once");
    expect(most_held.load() <= 1, "held for a while: one at work while held");
    expect(most_after.load() >= 2, "held for a while: the others came back once hold() let go");
}

// (stopped while the others wait: they come back and nothing more is taken)
void held_then_stopped() {
    std::atomic<size_t> worked{0};
    std::atomic<bool> stop{false};
    const auto t0 = std::chrono::steady_clock::now();
    const size_t done = warm_bounded::run(100000, 4, [&](size_t) {
        if (++worked == 20) stop = true;
    }, [&] { return stop.load(); }, [] { return true; }, [](size_t) {});
    expect(done == worked.load() && done >= 20 && done < 100000, "held, then stopped: the work ends at the stop");
    expect(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10), "held, then stopped: no worker is left waiting");
}
}   // namespace

int main() {
    bounded(0, 8);
    bounded(1, 8);
    bounded(500, 1);
    bounded(2000, 6);
    bounded(300, 0);   // (no thread count: one)
    stopped();
    held_all_along(0, 4);
    held_all_along(1, 4);
    held_all_along(300, 1);
    held_all_along(600, 6);
    held_for_a_while();
    held_then_stopped();
    // the marker: only a warm that got to its end in a process that plays; never the preparation's own process
    expect(warm_bounded::leaves_marker(false, true, false), "warm_bounded::leaves_marker(false, true, false)");
    expect(!warm_bounded::leaves_marker(true, true, false), "!warm_bounded::leaves_marker(true, true, false)");    // (--prepare-shaders: its cache does not answer the game's asks)
    expect(!warm_bounded::leaves_marker(false, false, false), "!warm_bounded::leaves_marker(false, false, false)");  // (not through)
    expect(!warm_bounded::leaves_marker(false, true, true), "!warm_bounded::leaves_marker(false, true, true)");    // (the window was closed)
    expect(!warm_bounded::leaves_marker(true, false, true), "!warm_bounded::leaves_marker(true, false, true)");
    if (failures) return std::printf("%d checks failed\n", failures), 1;
    std::printf("warm_bounded: ok\n");
    return 0;
}
