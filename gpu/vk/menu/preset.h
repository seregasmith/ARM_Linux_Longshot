// The Quality preset module's (gpu/vk/menu/preset.cpp, #1633) that the rest of the menu uses (vk_menu.cpp): a preset's rows
// set (set_row, Set up for a weak laptop), Auto (this PC) chosen at the start (menu_init, menu_auto_for_gpu) and tuned in
// play (menu_auto_tune), its state saved (save: auto_for=, auto_tuned=, auto_steps=) and reported (the bug report).
#pragma once

#include "auto_settings.h"

#include <cstdint>

namespace mkwvk {
void apply_preset(int p);   // (the rows preset p sets: kAutoPreset, Auto's; 0, Custom, none)
void apply_auto();          // (Auto (this PC)'s rows: the profile auto_choice holds)
extern ::auto_settings::Choice auto_choice;   // (the profile menu_auto_for_gpu chose for this PC; tier 0: none yet)
extern uint64_t auto_for;     // (the fingerprint of the hardware the options' Auto settings were chosen for: auto_for=)
extern uint64_t auto_fp;      // (this PC's)
extern uint64_t auto_tuned;   // (the fingerprint the measured tuning in play finished for: auto_tuned=)
extern ::auto_settings::Tuning auto_tuning;   // (the measured tuning in play; its steps kept as auto_steps=)
} // namespace mkwvk
