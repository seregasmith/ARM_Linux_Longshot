// The Insert menu's Set up for a weak laptop (#1633), last in the Debug tab's Developer section, on integrated graphics only:
// Minimum's rows and mip trimming, then lighter still (the game's own upscaler at 33%, the plain window scale, Intel's shaders
// one at a time and quick compute builds). The action is the menu's own (vk_menu.cpp weak_laptop_apply: a preset, the
// samplers, the restart question, the save; menu_module.h). Fenced from a layout-only build (MENU_LAYOUT_ONLY, MENU_HOOK).
#include "menu_module.h"

#ifndef MENU_LAYOUT_ONLY
#include "vk_internal.h"   // (dev: the GPU's type)
#endif

#include <string>

namespace mkwvk {

namespace {
#ifndef MENU_LAYOUT_ONLY
bool laptop_row() { return dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU; }   // (integrated graphics)
#endif
} // namespace

MENU_MODULE(laptop) {
    menu::tab("debug")
        .row({.key = "weak_laptop",
              .label = "Set up for a weak laptop",
              .help = "For integrated graphics with 4 CPU threads or fewer (an i3 laptop's UHD Graphics): Render resolution 33% with "
                      "the game's own upscaler, the plain window scale without sharpening, the handheld profile, smaller textures, "
                      "Intel's shaders one at a time and quick compute builds. The handheld profile applies after a restart.",
              .kind = menu::Kind::Action,
              .choices = {"Apply", "Applied"},   // (its cell's texts)
              .shown = MENU_HOOK(laptop_row),
              .text = MENU_HOOK([](int) { return std::string(laptop_applied ? "Applied" : "Apply"); }),
              .on_activate = MENU_HOOK(weak_laptop_apply)}, menu::after("save_pipeline_list").order(100));
}

} // namespace mkwvk
