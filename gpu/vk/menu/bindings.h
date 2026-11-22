// The Controls page module's (gpu/vk/menu/bindings.cpp, #1633) that the rest of the menu uses (vk_menu.cpp: the bindings grid,
// the options file's pad_/key_ lines, menu_tick's layout read when the menu opens).
#pragma once

namespace mkwvk {
bool can_bind();             // (the host takes bindings: the grid and the Layout preset work)
void apply_pad_layout(int layout);   // (preset layout's face buttons (3: every button the Switch pad's own) bound)
int detect_pad_layout();     // (the preset the bindings are now; 4, Custom, when none)
} // namespace mkwvk
