"""Exercise the detached output controller with synthetic image and display receipts."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

STUBS = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>

using VkImage = uint64_t;
using VkImageView = uint64_t;
using VmaAllocation = uint64_t;
enum { OUTPUT_TRIAL_EXPIRED, OUTPUT_TRIAL_PREPARING, OUTPUT_TRIAL_READY, OUTPUT_TRIAL_ACTIVE, OUTPUT_TRIAL_SHOWN };
struct VkImageCreateInfo {
    int flags{}, imageType{}, format{};
    struct { uint32_t width{}, height{}, depth{}; } extent;
    int mipLevels{}, arrayLayers{}, samples{}, tiling{}, usage{}, initialLayout{}, sharingMode{};
    const void *pNext{};
};
static struct { int device{}; } dev;
static int views_destroyed;
static void vkDestroyImageView(int, VkImageView view, const void*) { assert(view); ++views_destroyed; }
struct nvn_tex {
    uint64_t serial; void *pool; uint64_t gpu; uint8_t output_preview{};
    struct { uint32_t width{}, height{}; } d{};
};
static std::atomic<int> releases;
static std::mutex staging_mu;
static std::condition_variable staging_cv;
static bool staging_paused, staging_entered;
static void logf(const char*, ...) {}
struct OutputTrialBacking {
    struct Entry {
        VkImage image{};
        VmaAllocation allocation{};
        VkImageCreateInfo shape{};
        VkImageView view{};
        uint64_t serial{}, gpu{};
    } images[3];
    ~OutputTrialBacking() { releases++; }
    bool matches(uint64_t serial, VkImage image) const {
        for (const auto& candidate : images)
            if (!candidate.allocation && candidate.serial == serial && image == candidate.image) return true;
        return false;
    }
};
static std::unique_ptr<OutputTrialBacking> output_probe_backing(
        const nvn_tex *targets, bool fail, unsigned& made, uint64_t& bytes) {
    {
        std::unique_lock lk(staging_mu);
        staging_entered = true;
        staging_cv.notify_all();
        staging_cv.wait(lk, [] { return !staging_paused; });
    }
    assert(targets[0].serial == 101 && targets[1].serial == 102 && targets[2].serial == 103);
    assert(!targets[0].pool && !targets[1].pool && !targets[2].pool);
    auto backing = std::make_unique<OutputTrialBacking>();
    for (unsigned i = 0; i < 3; ++i)
        backing->images[i] = {targets[i].serial + 1000, i + 20, {}, i + 40, targets[i].serial, targets[i].gpu};
    made = fail ? 1 : 3;
    bytes = made * 4096;
    if (fail) return {};
    return backing;
}
'''


CASES = r'''
using namespace std::chrono_literals;
constexpr auto fifo = OutputPath::FifoShown;

static uint64_t start(char mode = 'v') {
    nvn_tex request[] = {{101, &releases, 800}, {102, &releases, 801}, {103, &releases, 802}};
    assert(request_output_probe_backing(request, mode));
    assert(!request_output_probe_backing(request, mode));
    return output_trial_generation.load();
}

static void deadline(TrialClock::time_point t) {
    std::lock_guard lk(output_trial_mu);
    output_trial.request.deadline = t;
}

int main() {
    assert(!output_preview_path.load(std::memory_order_relaxed));
    uint64_t published = 0;
    const auto apply = +[](void* data, uint64_t generation) { *static_cast<uint64_t*>(data) = generation; };
    const nvn_tex candidate{101, nullptr, 800, 1};
    uint64_t image = 0, allocation = 0;
    const auto first = start();
    const auto original = make_output_trial_frame({first, 21, 1021, true}, 0, fifo);
    assert(original.generation == first && original.swapchain_generation == 0 && original.path == fifo);
    assert(!releases && output_trial.backing);
    assert(!acknowledge_output_trial_frame(original));
    auto other_image = make_output_trial_frame({first, 101, 1150, true}, 0, fifo);
    assert(!acknowledge_output_trial_frame(other_image));
    auto not_rendered = make_output_trial_frame({first, 101, 1101, false}, 0, fifo);
    assert(!acknowledge_output_trial_frame(not_rendered));
    assert(!make_output_trial_frame({first, 101, 1101, true, first}, 0, fifo).matches_backing);
    assert(output_trial_publish(101, apply, &published) && published == first);
    assert(!make_output_trial_frame({first, 101, 1101, true, first}, 0, fifo).matches_backing);
    take_output_trial_image(&candidate, {}, image, allocation);
    assert(image == 1101 && allocation == 20);
    const auto good = make_output_trial_frame({first, 101, 1101, true, first}, 0, fifo);
    assert(good.matches_backing);
    auto old_chain = good;
    old_chain.swapchain_generation = 1;
    assert(!acknowledge_output_trial_frame(old_chain));
    auto other_path = good;
    other_path.path = OutputPath::Unavailable;
    assert(!acknowledge_output_trial_frame(other_path));
    assert(acknowledge_output_trial_frame(good));
    assert(!acknowledge_output_trial_frame(good));
    assert(!make_output_trial_frame({first, 101, 1101, true}, 1, fifo).generation);
    assert(!output_trial_generation.load() && releases == 1);
    assert(!acknowledge_output_trial_frame(good));

    const auto second = start();
    assert(second > first);
    auto late = make_output_trial_frame({second, 101, 1101, true}, 4, fifo);
    assert(!acknowledge_output_trial_frame(good));
    deadline(TrialClock::now() - 1ms);
    assert(!acknowledge_output_trial_frame(late));  // Check the deadline even before the watchdog wakes.
    output_trial_watchdog(output_trial_generation.load());
    assert(!output_trial_generation.load() && releases == 2);
    assert(!acknowledge_output_trial_frame(late));

    const auto guarded = start();
    assert(!make_output_trial_frame({guarded, 101, 1101, true}, 4, OutputPath::Unavailable).generation);
    assert(!output_trial_generation.load() && releases == 2);
    const auto refused = start('g');
    assert(!make_output_trial_frame({refused, 101, 1101, true}, 4, fifo).generation);
    assert(!output_trial_generation.load() && releases == 3);
    const auto immediate = start('x');
    assert(!make_output_trial_frame({immediate, 101, 1101, true}, 4, fifo).generation);
    output_trial_watchdog(output_trial_generation.load());
    assert(!output_trial_generation.load() && releases == 3);

    const auto no_frame = start();
    assert(make_output_trial_frame({no_frame, 21, 1021, true}, 4, fifo).generation == no_frame);
    deadline(TrialClock::now() + 30ms);
    // No further replay, presentation or receipt. The real request watchdog releases the backing.
    const auto watch_until = TrialClock::now() + 2s;
    while (releases < 4 && TrialClock::now() < watch_until) std::this_thread::sleep_for(5ms);
    assert(!output_trial_generation.load() && releases == 4);

    const auto slow = start();
    {
        std::lock_guard lk(staging_mu);
        staging_paused = true;
        staging_entered = false;
    }
    std::thread prep([slow] {
        assert(!make_output_trial_frame({slow, 101, 1101, true}, 4, fifo).generation);
    });
    {
        std::unique_lock lk(staging_mu);
        assert(staging_cv.wait_for(lk, 2s, [] { return staging_entered; }));
    }
    deadline(TrialClock::now() - 1ms);
    output_trial_watchdog(output_trial_generation.load());
    assert(!output_trial_generation.load() && releases == 4);
    const auto replacement = start();
    {
        std::lock_guard lk(staging_mu);
        staging_paused = false;
        staging_cv.notify_all();
    }
    prep.join();
    assert(output_trial_generation.load() == replacement && releases == 5 && !output_trial.backing);
    deadline(TrialClock::now() - 1ms);
    output_trial_watchdog(output_trial_generation.load());
    assert(!output_trial_generation.load());
    const auto active = start('n');
    image = allocation = 0;
    assert(output_trial_phase(101, 0) == OUTPUT_TRIAL_PREPARING);
    assert(!output_trial_publish(101, apply, &published));
    assert(make_output_trial_frame({active, 21, 1021, true}, 0, fifo).generation == active);
    assert(output_trial_phase(101, 0) == OUTPUT_TRIAL_READY);
    assert(output_trial_phase(102, 0) == OUTPUT_TRIAL_EXPIRED);
    assert(!output_trial_publish(102, apply, &published));
    take_output_trial_image(&candidate, {}, image, allocation);
    assert(!image && !allocation);
    assert(output_trial_publish(101, apply, &published) && published == active);
    assert(output_trial_phase(101, 0) == OUTPUT_TRIAL_ACTIVE);
    assert(!output_trial_publish(101, apply, &published));
    assert(output_trial_skip_present(&candidate, active));
    const nvn_tex other{101, nullptr, 803, 1};
    const nvn_tex ordinary{101, nullptr, 800, 0};
    const nvn_tex recorded{401, nullptr, 800, 1};
    VkImageCreateInfo wrong;
    wrong.extent.width = 1024;
    take_output_trial_image(&candidate, wrong, image, allocation);
    take_output_trial_image(&other, {}, image, allocation);
    take_output_trial_image(&ordinary, {}, image, allocation);
    assert(!image && !allocation);
    take_output_trial_image(&recorded, {}, image, allocation);
    assert(image == 1101 && allocation == 20 && views_destroyed == 2);
    uint64_t second_image = 0, second_allocation = 0;
    take_output_trial_image(&candidate, {}, second_image, second_allocation);
    assert(!second_image && !second_allocation);
    deadline(TrialClock::now() - 1ms);
    output_trial_watchdog(output_trial_generation.load());
    assert(output_trial_phase(101, 0) == OUTPUT_TRIAL_EXPIRED);
    assert(output_trial_skip_present(&candidate, active));
    assert(!output_trial_skip_present(&candidate, 0));
    output_preview_queued_until = TrialClock::now() - 1ms;
    assert(output_preview_seconds() == 0);  // The guest refused before staging: restart fallback.
    output_preview_queued_until = TrialClock::now() + 2s;
    const auto unshown = start('k');
    assert(make_output_trial_frame({unshown, 21, 1021, true}, 0, fifo).generation == unshown);
    assert(output_trial_publish(101, apply, &published));
    deadline(TrialClock::now() - 1ms);
    assert(output_preview_seconds() == -2);
    output_trial_watchdog(unshown);
    assert(output_preview_seconds() == -2);  // Cleanup must retain the activated outcome.
    output_preview_activated = false;
    output_preview_queued_until = TrialClock::now() + 2s;
    const auto asking = start('k');
    assert(make_output_trial_frame({asking, 21, 1021, true, 0}, 0, fifo).generation == asking);
    assert(output_trial_publish(101, apply, &published) && published == asking);
    take_output_trial_image(&candidate, {}, image, allocation);
    const auto stale_native = make_output_trial_frame({asking, 101, 1101, true, active}, 0, fifo);
    assert(!acknowledge_output_trial_frame(stale_native));
    const auto new_native = make_output_trial_frame({asking, 101, 1101, true, asking}, 0, fifo);
    assert(acknowledge_output_trial_frame(new_native));
    assert(output_trial_phase(101, 0) == OUTPUT_TRIAL_SHOWN);
    assert(output_trial.request.deadline > TrialClock::now() + 9s);
    assert(!output_preview_rebuilt.load());
    assert(output_trial_phase(101, 1) == OUTPUT_TRIAL_EXPIRED);
    assert(!output_preview_rebuilt.load());  // Rebuild before Yes leaves no Keep to restart for.
    output_preview_queued_until = TrialClock::now() + 2s;
    const auto keeping = start('k');
    assert(make_output_trial_frame({keeping, 21, 1021, true, 0}, 0, fifo).generation == keeping);
    assert(output_trial_publish(101, apply, &published));
    take_output_trial_image(&candidate, {}, image, allocation);
    assert(acknowledge_output_trial_frame(make_output_trial_frame({keeping, 101, 1101, true, keeping}, 0, fifo)));
    output_preview_swapchain = 1;
    assert(!output_preview_answer(true));  // A receipt from the replaced swapchain cannot keep the trial.
    output_preview_swapchain = 0;
    assert(output_preview_answer(true));
    assert(output_trial_phase(102, 1) == OUTPUT_TRIAL_EXPIRED && !output_preview_rebuilt.load());
    assert(output_trial_phase(101, 1) == OUTPUT_TRIAL_EXPIRED);
    assert(output_preview_rebuilt.load());
    const nvn_tex rebuilt{201, nullptr, 900, 0, {1280, 720}};
    observe_output_preview_native(nullptr, 0);
    observe_output_preview_native(&rebuilt, keeping);
    assert(!output_preview_take_rebuilt_size());
    observe_output_preview_native(&rebuilt, 0);
    assert(output_preview_take_rebuilt_size() == (uint64_t{1280} << 32 | 720));
    assert(!output_preview_rebuilt.load() && !output_preview_take_rebuilt_size());
    assert(output_trial_skip_present(&candidate, asking));
    std::puts("PASS: FIFO identity, trial adoption, expired-source suppression and no-present cleanup");
}
'''


def check_native_queue(repo, cxx, directory):
    native = (repo / 'host/nvn_native.c').read_text(encoding='utf-8')
    start = native.index('void nvn_detached_present_received(')
    end = native.index('/* Called while the native presentation thread', start)
    callback = native[start:end]
    start = native.index('            detached_last_present_window = h2g(o);')
    end = native.index('\n        }', start)
    record = native[start:end]
    fixture = r'''
    #include <stdint.h>
    #include <stddef.h>
    #include <assert.h>
    #define _Thread_local thread_local
    typedef struct { int held; } SRWLOCK;
    static void AcquireSRWLockExclusive(SRWLOCK *s) { assert(!s->held); s->held = 1; }
    static void ReleaseSRWLockExclusive(SRWLOCK *s) { assert(s->held); s->held = 0; }
    static uint64_t h2g(uint64_t o) { return o; }
    static SRWLOCK detached_idle_lock;
    static const int *detached_idle_queue_length;
    static SRWLOCK *detached_idle_queue_lock;
    static const unsigned *detached_idle_queue_life;
    static unsigned detached_idle_queue_instance;
    static uint64_t detached_idle_window, detached_idle_generation;
    static _Thread_local SRWLOCK *detached_received_queue, *detached_last_present_queue;
    static _Thread_local unsigned detached_received_instance, detached_last_present_instance;
    static _Thread_local uint64_t detached_last_present_window, detached_last_present_generation;
    ''' + callback + '\nstatic void observe(uint64_t o, uint64_t preview) {\n' + record + r'''
    }
    int main(void) {
        SRWLOCK a = {0}, b = {0};
        unsigned life_a = 4, life_b = 4;
        int empty = 0;
        // Initial stock authority cannot be borrowed by a second queue.
        nvn_detached_present_received(&a, life_a);
        observe(1280, 0);
        nvn_detached_present_wait(&empty, &a, &life_a);
        assert(detached_idle_window == 1280 && !detached_idle_generation);
        assert(detached_idle_queue_length == &empty && detached_idle_queue_lock == &a && detached_idle_queue_life == &life_a);
        nvn_detached_present_wait(&empty, &b, &life_b);
        assert(!detached_idle_window && !detached_idle_generation);
        // A candidate observed from A cannot authorize recovery under B or a reused A.
        observe(1024, 7);
        nvn_detached_present_wait(&empty, &a, &life_a);
        assert(detached_idle_window == 1024 && detached_idle_generation == 7);
        nvn_detached_present_wait(&empty, &b, &life_b);
        assert(!detached_idle_window && !detached_idle_generation);
        ++life_a;
        nvn_detached_present_wait(&empty, &a, &life_a);
        assert(!detached_idle_window && !detached_idle_generation);
        nvn_detached_present_received(&b, life_b);
        nvn_detached_present_wait(&empty, &b, &life_b);
        assert(!detached_idle_window && !detached_idle_generation);
        observe(1024, 7);
        nvn_detached_present_wait(&empty, &b, &life_b);
        assert(detached_idle_window == 1024 && detached_idle_generation == 7);
        nvn_detached_present_wait(NULL, NULL, NULL);
        assert(!detached_idle_window && !detached_idle_generation);
        assert(!detached_idle_queue_length && !detached_idle_queue_lock && !detached_idle_queue_life);
    }
    '''
    source = Path(directory) / 'queue.cpp'
    program = Path(directory) / 'queue.exe'
    source.write_text(fixture, encoding='utf-8')
    subprocess.run([cxx, '-std=c++20', '-Wall', '-Wextra', '-Werror', source, '-o', program], check=True)
    subprocess.run([program], timeout=8, check=True)
    print('PASS: the native wait marker rejects another queue and a new incarnation of the same queue')


def check_probe_metadata(repo, cxx, directory):
    texture = (repo / 'gpu/vk/vk_texture.cpp').read_text(encoding='utf-8')
    start = texture.index('static std::unique_ptr<Texture> probe_texture_metadata() {')
    end = texture.index('\n}', start) + 2
    fixture = r'''
#include <cassert>
#include <memory>
#include <new>
#include "gpu/vk/vk_new.h"
using namespace mkwvk;
static bool refuse;
struct Texture {
    static void* operator new(size_t n, const std::nothrow_t&) noexcept {
        assert(t_new_nothrow);
        return refuse ? nullptr : ::operator new(n, std::nothrow);
    }
    static void operator delete(void* p) noexcept { ::operator delete(p); }
};
''' + texture[start:end] + r'''
int main() {
    refuse = true;
    assert(!probe_texture_metadata());
    assert(!t_new_nothrow);
    refuse = false;
    assert(probe_texture_metadata());
    assert(!t_new_nothrow);
}
'''
    source = Path(directory) / 'metadata.cpp'
    program = Path(directory) / 'metadata.exe'
    source.write_text(fixture, encoding='utf-8')
    subprocess.run([cxx, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I', str(repo), source, '-o', program], check=True)
    subprocess.run([program], check=True, timeout=8)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    replay = (args.repo / 'gpu/vk/vk_replay.cpp').read_text(encoding='utf-8')
    start = replay.index('using TrialClock =')
    end = replay.index('void present_image_with_trial(', start)
    adopt = replay.index('void take_output_trial_image(')
    adopt_end = replay.index('int shot_ask_next(', adopt)
    seconds = replay[replay.index('int output_preview_seconds() {'):adopt]
    actual = replay[start:end] + seconds + replay[adopt:adopt_end]
    assert 'while (output_trial_watchdog(generation));' in actual
    assert 'if (res == VK_SUCCESS) acknowledge_output_trial_frame(trial);' in replay
    assert 'dev.swapchain && !dev.hdr' in replay and 'dev.swapchain_generation && !dev.hdr' not in replay
    with tempfile.TemporaryDirectory(prefix='output-trial-') as directory:
        check_native_queue(args.repo, args.cxx, directory)
        check_probe_metadata(args.repo, args.cxx, directory)
        source = Path(directory) / 'trial.cpp'
        program = Path(directory) / 'trial.exe'
        source.write_text(STUBS + actual + CASES, encoding='utf-8')
        static = ['-static'] if os.name == 'nt' else []
        subprocess.run([args.cxx, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread', *static, source, '-o', program],
                       check=True)
        subprocess.run([program], check=True, timeout=10)


if __name__ == '__main__':
    main()
