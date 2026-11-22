// The Insert menu's camera rows (#1633), on the Controls tab: the mouse camera and its sensitivity and inversion, the
// camera's centering, inversion and speed.
#include "menu_module.h"

#include <string>

namespace mkwvk {

namespace {
const char* tx_mousecam(int v) { return v == 2 ? "Off" : v ? "Native" : "As the stick"; }
MODULE_HINTS(hn_mousecam, 0, "The mouse stands in for the right stick, with its curve and top speed.",
             "The mouse turns the camera itself, one to one, as in a PC game.",
             "The mouse plays no part: a click in the window doesn't capture it. For a controller or the keyboard alone.")
const char* tx_sens(int v) {
    static char b[12];
    snprintf(b, sizeof b, "%.2f", v / 100.0);
    return b;
}
MODULE_HINTS(hn_mouse_invert, 0, nullptr, "Moving the mouse forward tilts the camera down.")
} // namespace

MENU_MODULE(camera) {
    menu::tab("controls")
        .row({.key = "mouse_camera",
              .label = "Mouse camera",
              .help = "How the captured mouse turns the camera.",
              .value = &opts.mouse_native,
              .lo = 0, .hi = 2,
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_mousecam(v)); },
              .hint = hn_mousecam}, menu::before("@Controls").order(110))
        .row({.key = "disable_camera_centering",
              .label = "Disable camera centering",
              .help = "Stops the camera gradually turning toward the direction you move, useful with native mouse look. Off keeps the "
                      "game's standard behavior. Manual camera controls and recentering still work.",
              .kind = menu::Kind::Toggle,
              .value = &opts.disable_camera_centering,
              .lo = 0, .hi = 1,
              .preset_exempt = true}, menu::before("@Controls").order(120))
        .row({.key = "mouse_sensitivity",
              .label = "Mouse sensitivity",
              .help = "How far the camera turns for a mouse movement, with either Mouse camera: at 1.00 native look turns 0.1 degree a "
                      "count. The - and = keys change it in game too.",
              .kind = menu::Kind::Slider,
              .value = &opts.mouse_sens,
              .lo = 10, .hi = 500,
              .step = 10,
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_sens(v)); }}, menu::before("@Controls").order(130))
        .row({.key = "invert_mouse_vertical",
              .label = "Invert mouse vertical",
              .help = "Swaps up and down for the mouse.",
              .kind = menu::Kind::Toggle,
              .value = &opts.mouse_invert,
              .lo = 0, .hi = 1,
              .preset_exempt = true,
              .hint = hn_mouse_invert}, menu::before("@Controls").order(140))
        .row({.key = "invert_mouse_horizontal",
              .label = "Invert mouse horizontal",
              .help = "Swaps left and right for the mouse.",
              .kind = menu::Kind::Toggle,
              .value = &opts.mouse_invert_x,
              .lo = 0, .hi = 1,
              .preset_exempt = true}, menu::before("@Controls").order(150))
        .row({.key = "invert_camera_x",
              .label = "Invert camera X",
              .help = "Inverts horizontal look (left/right) on the right analog stick.",
              .kind = menu::Kind::Toggle,
              .value = &opts.invert_cam_x,
              .lo = 0, .hi = 1,
              .preset_exempt = true}, menu::before("@Controls").order(160))
        .row({.key = "invert_camera_y",
              .label = "Invert camera Y",
              .help = "Inverts vertical look (up/down) on the right analog stick.",
              .kind = menu::Kind::Toggle,
              .value = &opts.invert_cam_y,
              .lo = 0, .hi = 1,
              .preset_exempt = true}, menu::before("@Controls").order(170))
        .row({.key = "camera_speed",
              .label = "Camera speed",
              .help = "How fast the right stick turns the camera, in percent of the game's own speed; the camera keys too, and the mouse "
                      "with Mouse camera As the stick. The game's own camera speed setting still applies.",
              .kind = menu::Kind::Slider,
              .value = &opts.camera_speed,
              .lo = 50, .hi = 400,
              .step = 25,
              .preset_exempt = true,
              .text = menu_text::percent}, menu::before("@Controls").order(180));
}

} // namespace mkwvk
