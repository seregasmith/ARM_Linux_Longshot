// Save my pipeline list's module (gpu/vk/menu/pipeline_list.cpp, #1633) that the rest of the menu uses (vk_menu.cpp): its
// worker's answer taken in menu_tick, and its line, which the help box is measured again for when it changes.
#pragma once

#include <string>

namespace mkwvk {
extern std::string share_said;   // (the help box's line after a save: where the zip went and what to do, or why there is none)
void share_finish();             // (the worker's answer, once it is in: the line finished)
} // namespace mkwvk
