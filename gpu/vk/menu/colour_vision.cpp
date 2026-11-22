#include "menu_module.h"

#ifndef MENU_LAYOUT_ONLY
#include <atomic>
#endif

namespace mkwvk {

#ifndef MENU_LAYOUT_ONLY
namespace {
std::atomic<int> live_mode{0};
}

void menu_colour_vision_changed(int mode) { live_mode.store(mode, std::memory_order_relaxed); }
int menu_colour_vision_mode() { return live_mode.load(std::memory_order_relaxed); }
#endif

MENU_MODULE(colour_vision) {
    menu::tab("graphics")
        .heading("accessibility", "Accessibility", menu::after("hdr"))
        .row({.key = "colour_vision", .label = "Colour vision correction",
              .help = "Recolours the picture for red-green or blue-yellow colour vision deficiency. Changes apply immediately.",
              .value = &opts.colour_vision, .lo = 0, .hi = 3,
              .choices = {"Off", "Protan (red-green)", "Deutan (red-green)", "Tritan (blue-yellow)"},
              .preset_exempt = true, .on_change = MENU_HOOK(menu_colour_vision_changed)});
}

} // namespace mkwvk
