// The Insert menu's CRT rows (#1633), in the Graphics tab's Screen section: the CRT filter the finished picture is shown
// through, and the lines of its raster (only with a filter on). docs/development.md, "CRT output stage".
#include "menu_module.h"

#include <iterator>
#include <string>

namespace mkwvk {

namespace {
const char* tx_crt_filter(int v) {
    static const char* const t[] = {"Off", "Lottes aperture grille", "Lottes consumer TV", "Hyllian Fast", "zfast CRT"};
    return (unsigned)v < std::size(t) ? t[v] : t[0];
}
const char* hn_crt_filter(int v, int variant) {
    static const char* const t[] = {
        "Off: no extra CRT passes or image allocations.",
        "Timothy Lottes' crt-lottes: flat aperture grille, a PVM-style look, not a calibrated Sony model.",
        "Timothy Lottes' crt-lottes: curved consumer TV with a shadow mask and beam bloom.",
        "Hyllian's crt-hyllian-fast: cubic reconstruction, scanlines and a magenta/green mask.",
        "Greg Hogan's zfast CRT: softer scanlines and a monochrome aperture mask."};
    if (variant < 0)
        variant = v > 0 && (unsigned)v < std::size(t) && crt_native_pq() ? 1 : 0;
    if (variant == 1)
        return "Bypassed: the game is presenting Native HDR (PQ). The saved CRT choice is kept; SDR and Expanded HDR can use it.";
    return menu_text::hint_in(t, (int)std::size(t), 0, v, variant);
}
const char* tx_crt_lines(int v) {
    static const char* const t[] = {"240", "480", "720"};
    return (unsigned)v < std::size(t) ? t[v] : t[0];
}
MODULE_HINTS(hn_crt_lines, 0, "240 virtual lines: a coarser raster and wider scanline spacing.",
             "480 virtual lines: a finer raster, reduced when the destination is too small.",
             "720 virtual lines: the finest raster, reduced when the destination is too small.")
} // namespace

MENU_MODULE(crt) {
    menu::tab("graphics")
        .row({.key = "crt_filter",
              .label = "CRT filter",
              .help = "Filters the finished picture, including the game's HUD, after window effects. Longshot's menu and overlays stay "
                      "clean. Changes at once; quality presets leave it alone.",
              .value = &opts.crt_filter,
              .lo = 0, .hi = 4,
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_crt_filter(v)); },
              .hint = hn_crt_filter}, menu::after("fullscreen_monitor").order(30))
        .row({.key = "crt_lines",
              .label = "CRT lines",
              .help = "Virtual CRT raster, not Render resolution. Capped at half the destination height (at least one line); width follows "
                      "its aspect. Non-integer scaling can cause moire.",
              .value = &opts.crt_lines,
              .lo = 0, .hi = 2,
              .preset_exempt = true,
              .shown = [] { return opts.crt_filter != 0; },
              .text = [](int v) { return std::string(tx_crt_lines(v)); },
              .hint = hn_crt_lines}, menu::after("fullscreen_monitor").order(40));
}

} // namespace mkwvk
