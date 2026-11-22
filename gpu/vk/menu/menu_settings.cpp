// The Insert menu's own settings (#1633): how large it is (on the Gameplay tab) and the keys and button that open it
// (on the Controls tab, after the bindings).
#include "menu_module.h"

#include <string>

namespace mkwvk {

namespace {
const char* tx_menu_size(int v) {
    static const char* t[] = {"Small", "Medium", "Large"};
    return v >= 1 && v <= 3 ? t[v - 1] : t[1];
}
MODULE_HINTS(hn_menu_size, 1, "Smaller, for a desk.", "The usual size.", "Larger, for a TV across the room.")
const char* tx_menu_theme(int v) {
    static const char* const t[] = {"Modern dark", "Classic"};
    return t[std::clamp(v, 0, 1)];
}
MODULE_HINTS(hn_menu_theme, 0, "Slate greys with a soft blue for the selection.", "The game's own pause menu: tan and gold.")
const char* tx_menu_shortcut(int v) {
    // (F1 is never turned off: the menu is always one key away. Esc as well, as a PC game's pause menu)
    static const char* const names[] = {"Insert or F1", "F1 only", "Insert, F1 or Esc"};
    return names[std::clamp(v, 0, 2)];
}
const char* tx_compact_menu_key(int v) {
    static const char* const names[] = {"Right Shift", "Right Ctrl"};
    return names[std::clamp(v, 0, 1)];
}
const char* tx_pad_menu(int v) {
    static const char* t[] = {"None", "Home", "Back left (GL)", "Back right (GR)"};
    return t[std::min(std::max(v, 0), 3)];
}
} // namespace

MENU_MODULE(menu_settings) {
    menu::tab("gameplay")
        .row({.key = "menu_size",
              .label = "Menu size",
              .help = "How large this menu, the FPS counter and the start-up screen are drawn.",
              .value = &opts.menu_size,
              .lo = 1, .hi = 3,
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_menu_size(v)); },
              .hint = hn_menu_size}, menu::before("bug_report").order(200))
        .row({.key = "menu_theme",
              .label = "Menu theme",
              .help = "How this menu looks: Modern dark, or Classic, the game's own tan and gold.",
              .value = &opts.menu_theme,
              .lo = 0, .hi = 1,
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_menu_theme(v)); },
              .hint = hn_menu_theme}, menu::before("bug_report").order(201));
    menu::tab("controls")
        .row({.key = "menu_shortcut",
              .label = "Menu shortcut",
              .help = "Which keys open and close this menu. F1 always does; F1 only leaves Insert free for an overlay. With Esc, Esc opens it "
                      "too, as a PC game's pause menu (open, Esc still goes back a page or closes it).",
              .value = &opts.menu_shortcut,
              .lo = 0, .hi = 2,
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_menu_shortcut(v)); }}, menu::after("@Controls").order(100))
        .row({.key = "controller_menu_button",
              .label = "Controller menu button",
              .help = "Which of the controller's buttons opens and closes this menu, so that no keyboard is needed: Home, or one of the back "
                      "buttons of a pad that has them (GL and GR on a Switch 2 Pro Controller). None leaves all of them to the game.",
              .value = &opts.pad_menu,
              .lo = 0, .hi = 3,
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_pad_menu(v)); }}, menu::after("@Controls").order(110))
        .row({.key = "compact_menu_key",
              .label = "Compact menu key",
              .help = "Which key switches between the full menu and the compact bar (or opens directly to the compact bar when held "
                      "with the menu shortcut).",
              .value = &opts.compact_menu_key,
              .lo = 0, .hi = 1,
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_compact_menu_key(v)); }}, menu::after("@Controls").order(120));
}

} // namespace mkwvk
