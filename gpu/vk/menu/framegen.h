// The frame generation module's (gpu/vk/menu/framegen.cpp, #1633) that the rest of the menu uses (vk_menu.cpp).
#pragma once

namespace mkwvk {
/// Frame generation's choice as the row shows it (Auto's maker; a value past DLSS FG's top as the top): also the pipeline
/// share's line
const char* tx_framegen(int v);
/// Whether the Frame generation by row offers DLSS FG (where it cannot, 0 and 2 are FSR 3.1 FG: set_row)
bool framegen_by_dlss_offered();
/// Frame generation by with nothing to choose: RX_VK_DLSS_FG=1 chooses DLSS FG for the run (row_fixed)
bool framegen_by_fixed();
} // namespace mkwvk
