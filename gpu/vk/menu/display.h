// The display module's (gpu/vk/menu/display.cpp, #1633) that the rest of the menu uses (vk_menu.cpp): the options file's
// lines for the chosen fullscreen monitor (save, the load), and the monitors followed as the window moves between screens.
#pragma once

#include "monitor_choice.h"

#include <string>
#include <vector>

namespace mkwvk {
// (the Fullscreen monitor row: its value, the host's monitors as last listed, and the chosen one by its ID and the name it had)
extern int monitor_pick;
extern std::vector<monitor_choice::Choice> fullscreen_monitors;
extern std::string monitor_want, monitor_saved_name;
bool can_monitors();
void refresh_monitors();   // (the host's monitors listed again, the row on the chosen one)
} // namespace mkwvk
