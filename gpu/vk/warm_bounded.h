// (longshot) Work done one item at a time on a few threads, each item made, used and let go before the thread takes the
// next: no more than `threads` items are alive at once, however many there are (vk_draw.cpp precompile_warm: every noted
// pipeline built into the pipeline cache and destroyed, where keeping them all ready needs more memory than some PCs have).
// tests/test_warm_bounded.py.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>
#include <utility>
#include <vector>

namespace warm_bounded {

/// work(i) for every i < n, on up to `threads` threads (at least one), each call finished before its thread takes another
/// item. stop(): asked before each item; once true no new item is taken. hold(): asked before each item too; while it is
/// true a worker waits (asking again every 20 ms) instead of taking the next one, unless every other worker is waiting
/// already: one always goes on, so the work ends however long hold() says so (memory short, the warm goes on a worker at a
/// time instead of all of them: vk_draw.cpp precompile_warm). held, where given: how many times a worker waited.
/// tick(done): called on the calling thread about every 50 ms while the others work, and once at the end. Returns how
/// many items were done
template <class Work, class Stop, class Hold, class Tick>
size_t run(size_t n, unsigned threads, Work&& work, Stop&& stop, Hold&& hold, Tick&& tick, std::atomic<size_t>* held = nullptr) {
    std::atomic<size_t> next{0}, done{0};
    std::atomic<unsigned> busy{0}, going{0};   // (going: the workers not waiting on hold())
    const auto worker = [&] {
        for (;;) {
            if (stop()) break;
            // (held: this worker steps aside while another goes on; the last one going never waits, and a waiting one comes
            // back once hold() lets go, the work is stopped or taken, or no other is going any more)
            for (unsigned g; hold() && (g = going.load()) > 1;) {
                if (!going.compare_exchange_weak(g, g - 1)) continue;
                if (held) held->fetch_add(1);
                while (hold() && !stop() && next.load() < n && going.load() > 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                going.fetch_add(1);
            }
            const size_t i = next.fetch_add(1);
            if (i >= n) break;
            work(i);
            done.fetch_add(1);
        }
        going.fetch_sub(1);
        busy.fetch_sub(1);
    };
    const unsigned count = n ? (unsigned)std::min<size_t>(threads ? threads : 1, n) : 0;
    busy = count;
    going = count;
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < count; t++) pool.emplace_back(worker);
    while (busy.load()) {
        tick(done.load());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    for (std::thread& t : pool) t.join();
    tick(done.load());
    return done.load();
}
/// The same with nothing held: every worker takes items as long as there are some
template <class Work, class Stop, class Tick>
size_t run(size_t n, unsigned threads, Work&& work, Stop&& stop, Tick&& tick) {
    return run(n, threads, std::forward<Work>(work), std::forward<Stop>(stop), [] { return false; }, std::forward<Tick>(tick));
}


/// Whether a warm may leave the marker that says every noted pipeline is in the pipeline cache (longshot_pipelines.bin.built),
/// after which a start loads at create calls and builds nothing itself. Only a warm that got to its end, and only in a process
/// that plays: on NVIDIA the cache a --prepare-shaders process writes does not answer the asks of the play process that
/// follows (0 of 32 of a sample; 3,720 of 3,880 pipelines "in no cache" in the first session, compiled in play with draws left
/// out), while what a play process builds is answered from then on. So the preparation's process never leaves the marker,
/// and the first start of the game after it warms again, itself
inline bool leaves_marker(bool prepare_only, bool got_to_the_end, bool stopped) { return !prepare_only && got_to_the_end && !stopped; }
} // namespace warm_bounded
