// The Insert menu's Debug tab's own rows (#1633): the Debug tools' switch (turning it on asks first: vk_menu.cpp set_row's
// debug warning), Autosave (works with the tools off) and Damage taken (with the tools on: row_shown). The tools themselves,
// the panel's rows drawn and pressed by the ImGui menu's own code (cheat_press, draw_debug_rows), stay vk_menu.cpp's custom
// section until the panels' rework.
#include "menu_module.h"

#include <string>

namespace mkwvk {

namespace {
const char* tx_damage_taken(int v) {
    static const char* const names[] = {"Half", "Normal", "Double", "Quadruple"};
    return names[v >= 0 && v < 4 ? v : 1];
}
} // namespace

MENU_MODULE(debug_tools) {
    menu::tab("debug")
        .row({.key = "debug_tools",
              .label = "Debug tools",
              .help = "Testing tools: no-clip flight, a free camera, teleport, Link's age, the inventory, the clock, heart protection and "
                      "movement speed. Off puts flight, the free camera and heart protection away and Link back to his own speed and damage.",
              .kind = menu::Kind::Toggle,
              .value = &opts.debug_tools,
              .lo = 0, .hi = 1,
              .preset_exempt = true}, menu::first())
        .row({.key = "autosave:",
              .label = "Autosave:",
              .help = "Works with Debug tools off. Turning it off skips supported autosaves. You can still save manually.",
              .kind = menu::Kind::Toggle,
              .value = &opts.transition_autosave,
              .lo = 0, .hi = 1,
              .preset_exempt = true}, menu::after("@Heart protection").order(10))   // (where the table had it: the tools' panel lays them out)
        .row({.key = "damage_taken",
              .label = "Damage taken",
              .help = "How much health Link loses from a hit. Normal is unchanged. Half rounds up to the next quarter heart. "
                      "Changes apply at once; healing is unchanged. Heart protection takes priority. Debug tools Off makes it Normal "
                      "until they are on again.",
              .value = &opts.damage_taken,
              .lo = 0, .hi = 3,
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_damage_taken(v)); }}, menu::after("@Heart protection").order(20));
}

} // namespace mkwvk
