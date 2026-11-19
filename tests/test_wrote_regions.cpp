// tests/test_wrote_regions.py compiles and runs this: gpu/vk/wrote_regions.h on its own.
#include "../gpu/vk/wrote_regions.h"
#include "../gpu/vk/gpu_write_history.h"

#include <cstdio>
#include <cstdlib>
#include <new>

// Count live map allocations, including temporary overlap while publishing a batch.
static size_t live_nodes = 0, peak_nodes = 0;
void* operator new(size_t n) {
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc{};
    peak_nodes = std::max(peak_nodes, ++live_nodes);
    return p;
}
void operator delete(void* p) noexcept { if (p) { --live_nodes; std::free(p); } }
void operator delete(void* p, size_t) noexcept { ::operator delete(p); }

#define CHECK(c)                                                           \
    do {                                                                   \
        if (!(c)) {                                                        \
            std::fprintf(stderr, "%s:%d: not true: %s\n", __FILE__, __LINE__, #c); \
            return 1;                                                      \
        }                                                                  \
    } while (0)

int main() {
    const uint64_t MB = 1ull << 20, R = 2 * MB;
    // nothing written yet: no range is in a written region
    CHECK(!wrote_regions::any(0, 1));
    CHECK(!wrote_regions::any(5 * R, 9 * R));
    CHECK(!wrote_regions::any(0, 1ull << 39));

    // one write inside a region: that region, and only it
    wrote_regions::mark(10 * R + 4096, 10 * R + 8192);
    CHECK(wrote_regions::any(10 * R, 10 * R + 1));              // (the region's first byte)
    CHECK(wrote_regions::any(11 * R - 1, 11 * R));              // (its last byte)
    CHECK(wrote_regions::any(9 * R, 12 * R));                   // (a range over it)
    CHECK(!wrote_regions::any(9 * R, 10 * R));                  // (ends where it starts)
    CHECK(!wrote_regions::any(11 * R, 12 * R));                 // (starts where it ends)

    // a write over a boundary: both regions; one ending on a boundary: not the next
    wrote_regions::mark(20 * R + R - 16, 20 * R + R + 16);
    CHECK(wrote_regions::any(20 * R, 20 * R + 1) && wrote_regions::any(21 * R, 21 * R + 1));
    wrote_regions::mark(30 * R, 31 * R);
    CHECK(wrote_regions::any(30 * R + 5, 30 * R + 6) && !wrote_regions::any(31 * R, 32 * R) && !wrote_regions::any(29 * R, 30 * R));

    // an empty range is in none; an empty write marks none
    CHECK(!wrote_regions::any(10 * R + 4096, 10 * R + 4096));
    wrote_regions::mark(40 * R, 40 * R);
    CHECK(!wrote_regions::any(40 * R, 41 * R));

    // an address past the 40 bits the table covers shares a region's bit: never a wrong "no"
    wrote_regions::mark((1ull << 40) + 50 * R, (1ull << 40) + 50 * R + 8);
    CHECK(wrote_regions::any((1ull << 40) + 50 * R, (1ull << 40) + 50 * R + 8));
    CHECK(wrote_regions::any(50 * R, 50 * R + 8));              // (the shared bit: a "maybe", which the map then answers)

    // a range wider than the whole table: asked of every bit, and a write that wide marks every bit
    CHECK(wrote_regions::any(0, 1ull << 41));
    wrote_regions::mark(0, 1ull << 41);
    CHECK(wrote_regions::any(5 * R, 5 * R + 1) && wrote_regions::any(99999 * R, 99999 * R + 1));
    mkwvk::GpuWriteHistory history;
    history.record(16, 96, 2);
    history.record(32, 56, 3);
    history.record(48, 64, 3);
    history.record(96, 104, 2);
    CHECK(history.newest(48, 96) == 0);
    history.complete(2);
    CHECK(history.newest(48, 96) == 2);
    history.complete(3);
    CHECK(history.newest(16, 32) == 2);
    CHECK(history.newest(32, 33) == 3);
    CHECK(history.newest(63, 64) == 3);
    CHECK(history.newest(64, 104) == 2);
    CHECK(history.newest(104, 105) == 0);
    history.record(24, 80, 4);
    history.complete(4);
    CHECK(history.newest(23, 24) == 2);
    CHECK(history.newest(24, 80) == 4);
    CHECK(history.newest(80, 81) == 2);
    // Exceed the pending limit in one recording. Publication must preserve both
    // ends of that recording, and a later value must stay hidden until completion.
    constexpr size_t limit = 65536;
    const size_t base = live_nodes;
    {
        mkwvk::GpuWriteHistory many;
        many.watch(1, 2*limit+2, 2*limit+3);
        many.watch(2, 2*limit+3, 2*limit+4);
        many.drew(1, 2*limit+2, 2*limit+3, 1, 0); // an overflow write lands here
        many.drew(2, 2*limit+3, 2*limit+4, 1, 0); // beside it: GPU owns the pixels
        many.record(0, 1, 5);
        many.complete(5);
        for (size_t i = 0; i < 3 * limit; ++i) many.record(2*i, 2*i+1, 10);
        many.record(0, 1, 20);
        CHECK(peak_nodes <= base + 2*limit + 8);
        CHECK(many.newest(0, 1) == 5 && many.newest(2, 3) == 0);
        CHECK(many.newest(2*limit+2, 2*limit+3, 1) == 0);
        many.complete(10);
        CHECK(many.newest(2*limit+2, 2*limit+3, 1) == 10);
        CHECK(many.newest(2*limit+3, 2*limit+4, 2) == 0);
        many.forget(1, 2*limit+2, 2*limit+3);
        many.watch(1, 2*limit+3, 2*limit+4);
        many.drew(1, 2*limit+3, 2*limit+4, 11, 0);
        CHECK(many.newest(2*limit+3, 2*limit+4, 1) == 0);
        for (size_t i = 0; i < 3 * limit; ++i) CHECK(many.newest(2*i, 2*i+1) == 10);
        many.complete(20);
        CHECK(many.newest(0, 1) == 20 && many.newest(2, 3) >= 10);
        many.record(0, 1, 21);
        many.complete(21);
        CHECK(many.newest(0, 1) == 21 && many.newest(2, 3) <= 20);
        CHECK(peak_nodes <= base + 2*limit + 8);
    }

    // Overflow across many unfinished timeline values, with intermittent polls.
    {
        mkwvk::GpuWriteHistory backlog;
        for (size_t i = 1; i <= 3 * limit; ++i) backlog.record(8*i, 8*i+1, i);
        CHECK(peak_nodes <= base + 2*limit + 16 && backlog.newest(8, 9) == 0);
        for (uint64_t value : {1u, 50000u, 100000u, 196608u}) {
            backlog.complete(value);
            CHECK(backlog.newest(8*value, 8*value+1) == value);
            CHECK(backlog.newest(8*(value+1), 8*(value+1)+1) <= value);
        }
    }

    // Ordinary sparse writes across many completed batches overflow the finished
    // index independently. Old dirtiness must survive eviction from that index.
    mkwvk::GpuWriteHistory finished;
    finished.watch(1, 8, 9);
    finished.watch(2, 9, 10);
    finished.watch(3, 8, 9);
    finished.drew(1, 8, 9, 1, 0);
    finished.drew(2, 9, 10, 1, 0);
    finished.record(8, 9, 1);
    finished.complete(1);
    finished.drew(3, 8, 9, 7, 0); // the GPU rendered over the older buffer data
    CHECK(finished.newest(8, 9, 3) == 0);
    for (size_t i = 2; i <= limit + 2; ++i) {
        finished.record(8*i, 8*i+1, i);
        finished.complete(i);
    }
    CHECK(finished.newest(8, 9) >= 1);
    CHECK(finished.newest(8, 9, 1) == 1 && finished.newest(9, 10, 2) == 0);
    CHECK(finished.newest(8, 9, 3) == 0); // still protected after eviction
    // A full non-overflowing pending batch and a full finished map coexist here.
    // Publishing it may split a span, but must not allocate a third full map.
    for (size_t i = 2; i < limit + 2; ++i) finished.record(8*i+4, 8*i+5, 3*limit);
    peak_nodes = live_nodes;
    finished.complete(3*limit);
    CHECK(peak_nodes <= base + 2*limit + 10);
    // In one submission an earlier write to the backing buffer cannot replace a
    // later render. A real later write can, even if parallel workers record its
    // segment before the draw. Exercise ordinary and overflow representations.
    for (const bool overflow : {false, true}) {
        mkwvk::GpuWriteHistory ordered;
        ordered.watch(7, 8, 9);
        if (overflow)
            for (size_t i = 0; i < limit; ++i) ordered.record(100+2*i, 101+2*i, 7);
        ordered.record(8, 9, 7, 2);
        ordered.drew(7, 8, 9, 7, 2);
        ordered.complete(7);
        CHECK(ordered.newest(8, 9, 7) == 0);
        ordered.drew(7, 8, 9, 8, 4);
        ordered.record(8, 9, 8, 4);
        CHECK(ordered.newest(8, 9, 7) == 0);
        ordered.complete(8);
        CHECK(ordered.newest(8, 9, 7) == 8);
        ordered.record(8, 9, 9, 6);
        ordered.drew(7, 8, 9, 9, 5);
        ordered.complete(9);
        CHECK(ordered.newest(8, 9, 7) == 9);
        ordered.drew(7, 8, 9, 10, 8);
        ordered.record(8, 9, 10, 7);
        ordered.complete(10);
        CHECK(ordered.newest(8, 9, 7) == 0);
    }
    std::puts("wrote regions: ok");
    return 0;
}
