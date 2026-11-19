// Buffer writes invalidate sampled texture data only after their GPU timeline value completes.
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <tuple>

namespace mkwvk {
class GpuWriteHistory {
    struct Span {
        uint64_t end, value;
    };
    // Writes in a submission share a timeline value. Merge touching ranges before
    // publishing them so a later, unfinished write cannot hide a finished write.
    using Ranges = std::map<uint64_t, uint64_t>;
    std::map<uint64_t, Ranges> pending;
    std::map<uint64_t, Span> finished;
    static constexpr std::size_t max_ranges = 65536;
    std::size_t pending_ranges = 0;
    // Under pressure an untracked write can dirty any previously written region.
    // Keep both timeline ends: a newer unfinished write cannot hide an older
    // completed write, and an unfinished stamp must never be returned to a reader.
    uint64_t overflow_first = UINT64_MAX, overflow_last = 0;
    uint64_t finished_floor = 0, completed = 0;

    // A drawn image may own the only copy of its pixels. Only an actual buffer
    // write to its bytes, ordered after the draw, can replace them. Register live
    // images at creation so parallel recording can observe a later segment first.
    // A first successful replacement transfers ownership back to guest memory.
    using Position = std::tuple<uint64_t, uint64_t, uint64_t>;  // timeline, segment, order within segment
    struct Image {
        uint64_t end = 0, first_write = 0;
        Position draw{}, write{};
    };
    using ImageKey = std::pair<uint64_t, uint32_t>;  // storage start, live texture id
    struct ImageIndex {
        std::map<ImageKey, Image> ranges;
        uint64_t longest = 0;
    };
    // A few large arrays must not make every short write visit all smaller
    // textures. Only search starts close enough to overlap within each size class.
    std::array<ImageIndex, 4> images;
    uint64_t sequence = 0;

    static unsigned size_class(uint64_t size) {
        if (size <= 65536) return 0;
        if (size <= (4ull << 20)) return 1;
        if (size <= (256ull << 20)) return 2;
        return 3;
    }
    void note_images(uint64_t lo, uint64_t hi, const Position& write) {
        for (auto& index : images) {
            if (index.ranges.empty() || hi <= index.ranges.begin()->first.first) continue;
            const uint64_t start = lo < index.longest ? 0 : lo - index.longest + 1;
            if (index.ranges.rbegin()->first.first < start) continue;
            auto it = index.ranges.lower_bound({start, 0});
            for (; it != index.ranges.end() && it->first.first < hi; ++it) {
                auto& image = it->second;
                if (lo >= image.end) continue;
                image.write = std::max(image.write, write);
                const auto value = std::get<0>(write);
                if (image.draw < write && (!image.first_write || value < image.first_write))
                    image.first_write = value;
            }
        }
    }

    void publish(uint64_t lo, uint64_t hi, uint64_t value) {
        auto it = finished.lower_bound(lo);
        if (it != finished.begin()) {
            auto prev = std::prev(it);
            if (prev->second.end > lo) {
                const Span old = prev->second;
                prev->second.end = lo;
                if (old.end > hi) finished.emplace_hint(it, hi, old);
            }
            if (prev->second.end == lo && prev->second.value == value) {
                lo = prev->first;
                finished.erase(prev);
            }
        }
        while (it != finished.end() && it->first < hi) {
            const Span old = it->second;
            it = finished.erase(it);
            if (old.end > hi) {
                it = finished.emplace_hint(it, hi, old);
                break;
            }
        }
        if (it != finished.end() && it->first == hi && it->second.value == value) {
            hi = it->second.end;
            it = finished.erase(it);
        }
        finished.emplace_hint(it, lo, Span{hi, value});
        while (finished.size() > max_ranges) {
            auto oldest_address = finished.begin();
            finished_floor = std::max(finished_floor, oldest_address->second.value);
            finished.erase(oldest_address);
        }
    }

public:
    void watch(uint32_t id, uint64_t lo, uint64_t hi) {
        if (!id || lo >= hi) return;
        auto& index = images[size_class(hi - lo)];
        index.ranges.try_emplace({lo, id}, Image{hi});
        index.longest = std::max(index.longest, hi - lo);
    }
    void drew(uint32_t id, uint64_t lo, uint64_t hi, uint64_t value, uint64_t segment = 0) {
        if (!id || lo >= hi) return;
        auto& ranges = images[size_class(hi - lo)].ranges;
        auto it = ranges.find({lo, id});
        if (it == ranges.end()) return;
        auto& image = it->second;
        const Position draw{value, segment, ++sequence};
        if (draw < image.draw) return;  // an earlier segment recorded on a later worker
        image.draw = draw;
        image.first_write = image.write > draw ? std::get<0>(image.write) : 0;
    }
    void forget(uint32_t id, uint64_t lo, uint64_t hi) {
        if (id && lo < hi) images[size_class(hi - lo)].ranges.erase({lo, id});
    }

    void record(uint64_t lo, uint64_t hi, uint64_t value, uint64_t segment = 0) {
        if (lo >= hi) return;
        note_images(lo, hi, Position{value, segment, ++sequence});
        if (pending_ranges == max_ranges) {
            overflow_first = std::min(overflow_first, value);
            overflow_last = std::max(overflow_last, value);
            return;
        }
        auto& ranges = pending[value];
        auto next = ranges.upper_bound(lo);
        if (next != ranges.begin()) {
            auto prev = std::prev(next);
            if (prev->second >= hi) return;
            if (prev->second >= lo) {
                lo = prev->first;
                ranges.erase(prev);
                --pending_ranges;
            }
        }
        while (next != ranges.end() && next->first <= hi) {
            hi = std::max(hi, next->second);
            next = ranges.erase(next);
            --pending_ranges;
        }
        ranges.emplace_hint(next, lo, hi);
        ++pending_ranges;
    }

    void complete(uint64_t value) {
        completed = std::max(value, completed);
        if (overflow_first <= value && overflow_first <= overflow_last) {
            finished_floor = std::max(finished_floor, std::min(value, overflow_last));
            if (value >= overflow_last) {
                overflow_first = UINT64_MAX;
                overflow_last = 0;
            } else {
                overflow_first = value + 1;
            }
        }
        auto batch = pending.begin();
        while (batch != pending.end() && batch->first <= value) {
            for (auto [lo, hi] : batch->second) publish(lo, hi, batch->first);
            pending_ranges -= batch->second.size();
            batch = pending.erase(batch);
        }
    }

    uint64_t newest(uint64_t lo, uint64_t hi, uint32_t drawn_id = 0) const {
        if (lo >= hi) return 0;
        if (drawn_id) {
            const auto& ranges = images[size_class(hi - lo)].ranges;
            auto it = ranges.find({lo, drawn_id});
            if (it == ranges.end() || it->second.first_write > completed) return 0;
            return it->second.first_write;
        }
        auto it = finished.upper_bound(lo);
        if (it != finished.begin()) --it;
        uint64_t newest = finished_floor;
        for (; it != finished.end() && it->first < hi; ++it)
            if (lo < it->second.end) newest = std::max(newest, it->second.value);
        return newest;
    }
};
}  // namespace mkwvk
