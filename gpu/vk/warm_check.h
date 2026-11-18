// SPDX-License-Identifier: GPL-3.0-or-later
// (longshot) Whether the first start after a preparation still has to warm the pipeline cache (vk_draw.cpp precompile_warm),
// or the caches already answer the notes: a sample of the noted pipelines and of the compute programs asked as a load at a
// create call asks them, without a compile (vk_draw.cpp notes_answered_by_caches).
//
// The preparation's own process warms too but leaves no marker (warm_bounded.h leaves_marker): on NVIDIA what it writes does
// not answer the play process that follows ("kept 0 of 32"). On Mesa's drivers (Turnip, RADV) its pipeline cache file does,
// and the first start's warm then built every noted pipeline again before the game, into a cache object that holds every
// shader built through it (Mesa's keeps each one's uploaded copy too) for the whole process: 36,449 on a 15 GB AYN Odin 3
// (Turnip), where the save took 30 s (2.5 s in the preparation's process) and the system went down right after it
// (2026-11-17). Answered, the marker is written and the warm left out;
// else (NVIDIA, a driver changed since the preparation, a cache that lost them) the warm runs as before. Nothing of Vulkan
// here: tests/test_warm_check.py.
#pragma once

#include <cstddef>

namespace warm_check {

constexpr size_t kNotes = 256;     // (noted pipelines asked, spread over the notes: a warm cut short leaves a run of them out)
constexpr size_t kComputes = 64;   // (compute programs asked, spread over the kept ones)

/// The index of the i-th of k asked, spread evenly over n (k <= n): each one once, the first and nearly the last among them
constexpr size_t pick(size_t i, size_t k, size_t n) { return k ? i * n / k : 0; }

/// What one part of the sample came to. tried: picked; asked: put to the caches (a note whose program is not kept in its
/// variant, or a compute program that cannot be built, is not asked); cold: asked and not answered (a compile would be needed)
struct Asked {
    size_t tried = 0, asked = 0, cold = 0;
};

/// One part answered: none of what was asked needed a compile, and at least half of what was tried could be asked (a sample
/// of which most could not be asked says nothing). Nothing tried (no compute programs kept, say) answers by itself
constexpr bool part_answered(const Asked& a) {
    if (!a.tried) return true;
    return a.cold == 0 && a.asked > 0 && a.asked * 2 >= a.tried;
}

/// The warm can be left out: there are notes, and both parts of the sample were answered
constexpr bool answered(const Asked& notes, const Asked& computes) {
    return notes.tried > 0 && part_answered(notes) && part_answered(computes);
}

}  // namespace warm_check
