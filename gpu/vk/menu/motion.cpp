// The Insert menu's motion controls (#1633): on the Controls tab, the row that opens the Motion page; on that page, the
// gyroscope's aiming, sensitivity, inversion and dead zone.
#include "menu_module.h"

#include <string>

namespace mkwvk {

namespace {
MODULE_HINTS(hn_gyro, 0, "The sticks alone aim.",
             "Switch Pro controllers and pads in Switch mode, DualSense and DualShock 4. The value says whether the "
             "connected pad has a gyroscope.")
const char* tx_deadzone(int v) {
    static const char* t[] = {"Low", "Standard", "High"};
    return t[v < 0 ? 0 : v > 2 ? 2 : v];
}
} // namespace

MENU_MODULE(motion) {
    menu::tab("controls")
        .row({.key = "@Motion",
              .label = "Motion",
              .help = "Motion controls: aiming with the controller's gyroscope, its sensitivity and its inverts.",
              .kind = menu::Kind::Page,
              .opens = "motion"}, menu::before("@Controls").order(210));
    menu::page("motion")
        .row({.key = "motion_controls",
              .label = "Motion controls",
              .help = "Aims and looks with the controller's gyroscope where the game uses motion, as on the Switch. Works with Switch Pro "
                      "controllers and pads in Switch mode, DualSense and DualShock 4. Shown beside it: whether the connected pad has one.",
              .value = &opts.gyro,
              .lo = 0, .hi = 1,
              .preset_exempt = true,
              .text = [](int v) { return std::string(motion_gyro_text(v)); },
              .hint = hn_gyro}, menu::order(10))
        .row({.key = "motion_sensitivity_x",
              .label = "Motion sensitivity X",
              .help = "How far the aim moves left and right for a given turn of the controller. 100% is the console's own.",
              .kind = menu::Kind::Slider,
              .value = &opts.gyro_sens_x,
              .lo = 10, .hi = 600,
              .step = 10,
              .preset_exempt = true,
              .text = menu_text::percent}, menu::order(20))
        .row({.key = "motion_sensitivity_y",
              .label = "Motion sensitivity Y",
              .help = "How far the aim moves up and down for a given tilt of the controller. 100% is the console's own.",
              .kind = menu::Kind::Slider,
              .value = &opts.gyro_sens_y,
              .lo = 10, .hi = 300,
              .step = 10,
              .preset_exempt = true,
              .text = menu_text::percent}, menu::order(30))
        .row({.key = "invert_motion_x",
              .label = "Invert motion X",
              .help = "Turns the motion aim's left and right around.",
              .kind = menu::Kind::Toggle,
              .value = &opts.gyro_inv_x,
              .lo = 0, .hi = 1,
              .preset_exempt = true}, menu::order(40))
        .row({.key = "invert_motion_y",
              .label = "Invert motion Y",
              .help = "Turns the motion aim's up and down around.",
              .kind = menu::Kind::Toggle,
              .value = &opts.gyro_inv_y,
              .lo = 0, .hi = 1,
              .preset_exempt = true}, menu::order(50))
        .row({.key = "motion_deadzone",
              .label = "Motion deadzone",
              .help = "How slow a turn counts as none, the console's three settings: Standard is its own. High steadies a pad that jitters "
                      "when held still; Low keeps the smallest movements.",
              .value = &opts.gyro_deadzone,
              .lo = 0, .hi = 2,
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_deadzone(v)); }}, menu::order(60));
}

} // namespace mkwvk
