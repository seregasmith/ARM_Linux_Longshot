/* (longshot) How much VRAM the game's memory pools may be mirrored in (vk_memory.cpp mirror_budget), by the card. No Vulkan here.
   tests/test_mirror_budget.py */
#pragma once

#include <algorithm>
#include <cstdint>

namespace mkwvk::mirror_rule {

/// What a card counts as small: 4.5 GB or less, where the mirrors get at most 256 MB and the textures at most 2.2 GB
/// (texture_budget.h)
inline bool small_card(uint64_t vram) { return vram && vram <= (4ull << 30) + (512ull << 20); }

/// What a card of 6.5 GB or less is, past the small ones: a 6 GB card, where the mirrors get a third, at most 1700 MB
inline bool six_gb_card(uint64_t vram) { return !small_card(vram) && vram && vram <= (6ull << 30) + (512ull << 20); }

/// The mirrors: a quarter of the card, at most 2 GB under 15 GB and 3 GB from there; on a card of 6.5 GB or less a third, at most
/// 1700 MB; and on a card of 4.5 GB or less an eighth, at most 256 MB (the Minimum (4GB) preset's). The one rule
/// for every card: no menu row or Quality preset sets them, and vk_memory.cpp works nothing out beside it. (The 6 GB cards'
/// 1700 MB holds open-area peaks without falling back to PCIe reads. A third on a 4 GB card would be 1365 MB beside its 2.2 GB
/// of textures: 3.5 of its 4 GB before a render target, a buffer or a pipeline; the small cards keep 256 MB.)
/// A pool left out is read by the GPU over PCIe: on 8 GB cards that leaves the GPU at a few percent and the frame stuttering,
/// and on a 6 GB laptop card at an eighth or 512 MB, 30-32 GB of bound ranges go over PCIe every 600 submits in Hyrule Field,
/// the GPU's frame 27 ms against 19 at a quarter. 1024 MB fills within minutes on an 8 GB card and new pools go over PCIe; at
/// a quarter (2048) none do, 37 -> 45 fps, 1% lows 16 -> 21. (The image blocks made at the start are in the textures' pool,
/// where the textures take them: vram_compact.h blocks_at_start.) The mirrors are not squeezed when VRAM
/// runs short (RX_VK_MIRROR_TIGHT is opt-in): the texture cache is (on the 6 GB laptop: 95% of what the driver allows, about
/// the same textures held, 39 fps against 18). 2 GB is enough under 15 GB of VRAM: at 2048 no pool goes over PCIe on an 8 GB
/// card, and a quarter, 3 GB, beside 7.5 GB of textures leaves 10 and 11 GB cards too little (auto_settings.h). 3 GB from
/// 15 GB. A small card keeps 256 MB (an eighth of a smaller one, the Minimum preset's)
inline uint64_t auto_bytes(uint64_t vram) {
    if (!vram) return 3072ull << 20;
    if (small_card(vram)) return std::min<uint64_t>(vram / 8, 256ull << 20);
    if (six_gb_card(vram)) return std::min<uint64_t>(vram / 3, 1700ull << 20);
    return std::min<uint64_t>(vram / 4, vram < (15ull << 30) ? 2048ull << 20 : 3072ull << 20);
}

}  // namespace mkwvk::mirror_rule
