// The Sprint module's (gpu/vk/menu/sprint.cpp) that the rest of the menu uses (vk_menu.cpp: the Controls page's Reset to
// defaults, which gives Sprint nothing, lays its controls out again while the row is on).
#pragma once

namespace mkwvk {
void apply_sprint_layout(bool on);   // (Sprint's controls laid out for the Sprint row on or off: Shift and the left stick's press)
} // namespace mkwvk
