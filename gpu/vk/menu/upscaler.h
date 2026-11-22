// The upscaler module's (gpu/vk/menu/upscaler.cpp, #1633) that the rest of the menu uses (vk_menu.cpp): the Upscaler's and
// the Render resolution's values as their rows show them (the pipeline share's line), and the DLSS model's switch for this
// run (the bug report: its default depends on it).
#pragma once

namespace mkwvk {
const char* tx_upscaler(int v);     // (the Upscaler that runs, as its row names it)
const char* tx_resolution(int v);   // (the Render resolution at value v for the Upscaler that runs, with the 3D's size)
const char* dlss_preset_env();      // (RX_VK_DLSS_PRESET for this run, as the DLSS model row takes it, or nullptr)
} // namespace mkwvk
