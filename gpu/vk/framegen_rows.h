/* Pure helpers for the Insert menu's split Frame generation rows (Mode: Off / Auto / Fixed, and Fixed Multiplier stepped
   slider 2x..8x capped by dlss_fg_most()). No Vulkan, no ImGui: tests/test_menu_stepped_sliders.py checks them. */
#pragma once

#include <algorithm>

namespace mkwvk::framegen_rows {

enum Mode { MODE_OFF = 0, MODE_AUTO = 1, MODE_FIXED = 2, kModes = 3 };

/// Effective top fixed multiplier: 8x normally, or dlss_most + 1 (clamped to [2, 8]) while DLSS FG caps it.
inline int fixed_top(int dlss_most) {
    if (dlss_most <= 0) return 8;
    return std::clamp(dlss_most + 1, 2, 8);
}

/// Number of fixed multiplier stops (2x .. fixed_top): 7 normally (2x..8x), 5 on RTX 50 (2x..6x), 1 on RTX 40 (2x only).
inline int fixed_stop_count(int dlss_most) { return fixed_top(dlss_most) - 2 + 1; }

/// Mode (0 Off, 1 Auto, 2 Fixed) from opts.framegen (0 Off, 1 Auto, 2..8 Fixed).
inline int mode_of(int framegen) {
    if (framegen <= 0) return MODE_OFF;
    if (framegen == 1) return MODE_AUTO;
    return MODE_FIXED;
}

/// Step the Frame generation Mode row (Off / Auto / Fixed, wrapping) from opts.framegen.
/// Remembers last_fixed (updated whenever leaving Fixed, default 2) so switching back to Fixed restores it (capped to fixed_top).
inline int step_mode(int framegen, int dir, int* last_fixed, int dlss_most) {
    if (framegen >= 2 && last_fixed) *last_fixed = std::clamp(framegen, 2, 8);
    const int cur = mode_of(framegen);
    const int next = (cur + (dir > 0 ? 1 : kModes - 1)) % kModes;
    if (next == MODE_OFF) return 0;
    if (next == MODE_AUTO) return 1;
    const int pick = (last_fixed && *last_fixed >= 2 && *last_fixed <= 8) ? *last_fixed : 2;
    return std::min(pick, fixed_top(dlss_most));
}

/// Effective displayed/slider fixed multiplier (2 .. fixed_top) for opts.framegen >= 2.
inline int active_multiplier(int framegen, int dlss_most) {
    const int top = fixed_top(dlss_most);
    return std::clamp(framegen < 2 ? 2 : framegen, 2, top);
}

/// Step the Fixed Multiplier slider (2x .. fixed_top, stops at ends — no wrapping).
inline int step_multiplier(int framegen, int dir, int dlss_most) {
    const int top = fixed_top(dlss_most);
    const int cur = active_multiplier(framegen, dlss_most);
    return std::clamp(cur + (dir > 0 ? 1 : dir < 0 ? -1 : 0), 2, top);
}

/// Map fixed multiplier value (2..top) to permille (0..1000). Handles 1-stop (top == 2) safely without division by zero.
inline int multiplier_permille(int framegen, int dlss_most) {
    const int top = fixed_top(dlss_most);
    if (top <= 2) return 0;
    const int cur = active_multiplier(framegen, dlss_most);
    return (int)((long long)(cur - 2) * 1000 / (top - 2));
}

/// Map permille (0..1000) to fixed multiplier value (2..top). Handles 1-stop (top == 2) cleanly (returns 2).
inline int multiplier_from_permille(int permille, int dlss_most) {
    const int top = fixed_top(dlss_most);
    if (top <= 2) return 2;
    const int p = permille < 0 ? 0 : permille > 1000 ? 1000 : permille;
    const int idx = (p * (top - 2) + 500) / 1000;
    return std::clamp(2 + idx, 2, top);
}

} // namespace mkwvk::framegen_rows
