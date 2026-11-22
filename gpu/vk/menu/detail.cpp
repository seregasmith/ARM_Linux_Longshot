// The Insert menu's Detail rows (#1633): the Graphics tab's Detail section, after HDR: how sharp textures stay at a slant
// (anisotropic filtering, its samplers made again on a change by set_row) and how large the sun's shadow maps are.
#include "menu_module.h"

#include <string>

namespace mkwvk {

namespace {
/// Anisotropic filtering: the game's own, or 2x to 16x (vk_draw.cpp aniso_override)
const char* tx_anisotropy(int v) {
    static const char* t[] = {"Game's", "2x", "4x", "8x", "16x"};
    return (unsigned)v < 5 ? t[v] : t[4];
}
/// Grass distance: the game's reach for the short grass blades, or 2x to 4x (vk_draw.cpp grass_reach)
const char* tx_grass_distance(int v) {
    static const char* t[] = {"Game's", "2x", "3x", "4x"};
    return (unsigned)v < 4 ? t[v] : t[0];
}
/// Shadow resolution: the sun's cascades at the game's own sizes (host/oot_shadow.c). Auto, the default, takes the size
/// for the card's memory (menu_shadow_res)
const char* tx_shadow_res(int v) {
    static const char* t[] = {"Auto", "Game's", "High", "Highest"};
    return (unsigned)v < 4 ? t[v] : t[0];
}
MODULE_HINTS(hn_shadow_res, 0,
             "Chosen from the graphics card's memory: the game's size on 4 and 6 GB, High on 8 GB, Highest on 12 GB or "
             "more.",
             "1600 pixels a side, the console's. About 31 MB of video memory.",
             "2200 pixels a side. About 58 MB of video memory.",
             "3072 pixels a side, the game's own highest, and the shadows near you shimmer less while the camera moves. "
             "About 113 MB of video memory.")
} // namespace

MENU_MODULE(detail) {
    menu::tab("graphics")
        .heading("detail", "Detail", menu::after("hdr"))
        .row({.key = "anisotropic_filtering",
              .label = "Anisotropic filtering",
              .help = "Keeps textures sharp on surfaces seen at a slant, such as the ground ahead: higher is sharper in the distance, for a "
                      "little GPU time. Game's is the console's own (1x or 2x).",
              .value = &opts.anisotropy,
              .lo = 0, .hi = 4,
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_anisotropy(v)); }})
        .row({.key = "grass_distance",
              .label = "Grass distance",
              .help = "How far away short grass blades reach, such as Kokiri Forest's tufts and the moss on its stones. The game "
                      "stops them at about 20 m. Higher extends their reach for some GPU time and applies at once.",
              .value = &opts.grass_distance,
              .lo = 0, .hi = 3,
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_grass_distance(v)); }})
        .row({.key = "shadow_resolution",
              .label = "Shadow resolution",
              .help = "The size of the sun's shadow maps: higher keeps the shadows of thin things near you, such as chains, fences and "
                      "leaves, smooth instead of stair-stepped, for more video memory and a little GPU time. Highest also makes them "
                      "shimmer less while the camera moves. Applies at once.",
              .value = &opts.shadow_res,
              .lo = 0, .hi = 3,
              .text = [](int v) { return std::string(tx_shadow_res(v)); },
              .hint = hn_shadow_res});
}

} // namespace mkwvk
