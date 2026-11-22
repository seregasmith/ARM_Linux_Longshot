// The Insert menu's walk rows (#1633), on the Controls tab: how the Walk key works and how fast Link walks.
#include "menu_module.h"

#include <string>

namespace mkwvk {

namespace {
const char* tx_walk_mode(int v) { return v == 1 ? "Toggle" : "Hold"; }
MODULE_HINTS(hn_walk_mode, 0, "Link walks while the Walk key is held.", "One press of the Walk key walks, the next runs again.")
} // namespace

MENU_MODULE(walk) {
    menu::tab("controls")
        .row({.key = "walk_mode",
              .label = "Walk mode",
              .help = "How the Walk key makes Link walk with the movement keys: while it is held, or from one press to the next. It is Caps "
                      "Lock unless the Controls page changes it.",
              .value = &opts.walk_mode,
              .lo = 0, .hi = 1,
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_walk_mode(v)); },
              .hint = hn_walk_mode}, menu::before("@Controls").order(220))
        .row({.key = "walk_speed",
              .label = "Walk speed",
              .help = "How far the movement keys tilt the stick while Link walks, in percent of the whole way. Controllers, the mouse and "
                      "the camera keys are not changed.",
              .value = &opts.walk_speed,
              .lo = 15, .hi = 90,
              .step = 5,
              .preset_exempt = true,
              .text = menu_text::percent}, menu::before("@Controls").order(230));
}

} // namespace mkwvk
