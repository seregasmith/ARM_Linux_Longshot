// SPDX-License-Identifier: GPL-3.0-or-later
// A game-free policy for the renderer-owned simulated-turf record buffers. The Vulkan side supplies the raw requested count,
// installs target() when allocation succeeds, and keeps capacity() when it does not.
#pragma once

#include <algorithm>
#include <cstdint>

namespace mkwvk::sim_turf_capacity {

struct MirrorSpan {
    uint64_t source = 0;
    uint64_t destination = 0;
    uint64_t size = 0;
    bool operator==(const MirrorSpan&) const = default;
};

// The compatible stock window copied from a private list after its producer runs. Forward lists keep their first records;
// reverse lists align their ends, because record zero is addressed as capacity - 1 - reservation. This also handles a
// private buffer smaller than stock without moving its records to the wrong end of the game's list.
[[nodiscard]] constexpr MirrorSpan mirror_span(uint32_t capacity, uint32_t stock, uint32_t stride,
                                               bool reverse) noexcept {
    const uint32_t records = std::min(capacity, stock);
    if (!records || !stride) return {};
    const uint32_t source = reverse && capacity > stock ? capacity - stock : 0;
    const uint32_t destination = reverse && stock > capacity ? stock - capacity : 0;
    return {static_cast<uint64_t>(source) * stride, static_cast<uint64_t>(destination) * stride,
            static_cast<uint64_t>(records) * stride};
}

class Policy {
public:
    constexpr Policy(uint32_t stock, uint32_t group, uint32_t maximum, uint32_t minimum) noexcept
        : stock_(stock), minimum_(std::min(stock, minimum)), group_(group),
          maximum_(std::max(stock, maximum)), capacity_(stock), target_(stock) {}

    [[nodiscard]] constexpr uint32_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] constexpr uint32_t target() const noexcept { return target_; }

    // true means the caller should try to replace the installed buffer with target(). Demand at seven eighths of
    // capacity counts as pressure, so the replacement has arrived before reservations become visibly clipped.
    constexpr bool observe(uint32_t requested, uint64_t frame) noexcept {
        const bool pressure = static_cast<uint64_t>(requested) * 8 >= static_cast<uint64_t>(capacity_) * 7;
        if (pressure) {
            low_since_ = 0, low_peak_ = 0;
            last_pressure_ = frame;
            const uint32_t wanted = demand_target(requested);
            if (wanted > capacity_) return target_ = wanted, true;
            return false;
        }

        // A setting/area transition may make the old peak irrelevant. Wait 600 texture frames before releasing its
        // headroom, and only shrink when the new demand fits in less than half the current buffer. Size from the
        // quiet window's high-water demand rather than its final sample, so ordinary variation does not immediately
        // grow the buffer again.
        if (static_cast<uint64_t>(requested) * 2 < capacity_) {
            if (!low_since_) low_since_ = frame, low_peak_ = requested;
            else low_peak_ = std::max(low_peak_, requested);
            if (frame >= low_since_ + kShrinkQuietFrames && frame >= last_pressure_ + kShrinkQuietFrames) {
                const uint32_t wanted = std::max(minimum_, demand_target(low_peak_));
                if (wanted < capacity_) return target_ = wanted, true;
            }
        } else {
            low_since_ = 0, low_peak_ = 0;
        }
        return false;
    }

    constexpr void installed() noexcept { capacity_ = target_; low_since_ = 0; low_peak_ = 0; }
    constexpr void install_failed() noexcept { target_ = capacity_; }

private:
    static constexpr uint64_t kShrinkQuietFrames = 600;

    constexpr uint32_t demand_target(uint32_t requested) const noexcept {
        const uint64_t with_headroom = static_cast<uint64_t>(requested) + (static_cast<uint64_t>(requested) + 3) / 4;
        const uint64_t aligned = group_ ? ((with_headroom + group_ - 1) / group_) * group_ : with_headroom;
        return static_cast<uint32_t>(std::min<uint64_t>(maximum_, aligned));
    }

    uint32_t stock_;
    uint32_t minimum_;
    uint32_t group_;
    uint32_t maximum_;
    uint32_t capacity_;
    uint32_t target_;
    uint32_t low_peak_ = 0;
    uint64_t last_pressure_ = 0;
    uint64_t low_since_ = 0;
};

} // namespace mkwvk::sim_turf_capacity
