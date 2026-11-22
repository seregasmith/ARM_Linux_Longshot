// The Insert menu's GPU and memory section (#1633): the Graphics tab's last, the rows to change when something is wrong
// or on a card short of memory. GPU device, the graphics card the game runs on: vk_main.cpp pick_device counts the cards
// (gpu_device_names) and vk_menu.cpp menu_set_gpu_count sets the row's top to them and finds the card the options file saved
// (gpu_device_id=, gpu_choice.h). The card names ask the renderer: fenced from a layout-only build (MENU_LAYOUT_ONLY, MENU_HOOK:
// menu_module.h). On the Debug tab's Developer section, two of the same kind: Robust buffers (bounds-checked reads) and
// ASTC as RGBA8 (an AMD texture workaround that takes far more video memory; RX_VK_ASTC_RGBA8 holds it for the run).
#include "menu_module.h"

#ifndef MENU_LAYOUT_ONLY
#include "vk_internal.h"   // (gpu_device_names: vk_main.cpp pick_device)
#endif

#include <cstdio>
#include <string>

namespace mkwvk {

namespace {
const char* tx_robust(int v) {
    static const char* t[] = {"Auto", "Off", "On"};
    return (unsigned)v < 3 ? t[v] : t[0];
}
MODULE_HINTS(hn_robust, 0, "On, on every card.", "Can be faster on some NVIDIA cards.", "Always on.")
/// (the ASTC as RGBA8 row's line while RX_VK_ASTC_RGBA8 holds it: the old readme's way, still in some longshot_env.txt files)
const char* hn_astc_rgba8(int, int variant) {
    return variant <= 0 && getenv("RX_VK_ASTC_RGBA8") ? "Set by RX_VK_ASTC_RGBA8 (longshot_env.txt or the environment): remove that line to choose here."
                                       : nullptr;
}
#ifndef MENU_LAYOUT_ONLY
const char* tx_gpu(int v) {
    static char buf[300];
    const int idx = v - 1;
    if (v <= 0) snprintf(buf, sizeof buf, "Auto");
    else if (idx < (int)gpu_device_names.size()) snprintf(buf, sizeof buf, "%d: %s", idx, gpu_device_names[idx].c_str());
    else snprintf(buf, sizeof buf, "%d: Device %d", idx, idx);
    // (the whole name shows: the selected row slides through it, see marquee_pos)
    return buf;
}
#endif
MODULE_HINTS(hn_gpu, 0, "The best dedicated GPU.")
} // namespace

MENU_MODULE(gpu) {
    menu::tab("graphics")
        .heading("gpu", "GPU and memory", menu::after("detail"))
        .row({.key = "gpu_device",
              .label = "GPU device",
              .help = "Which graphics card the game runs on. Applies after a restart.",
              .value = &opts.gpu_device,
              .lo = 0, .hi = 8,
              .restart = true,
              .preset_exempt = true,
              .text = MENU_HOOK([](int v) { return std::string(tx_gpu(v)); }),
              .hint = hn_gpu});
    menu::tab("debug")
        .row({.key = "robust_buffers",
              .label = "Robust buffers",
              .help = "Bounds-checked buffer reads: a read past a buffer's end is dropped instead of stopping the GPU. Applies after a "
                      "restart.",
              .value = &opts.robust,
              .lo = 0, .hi = 2,
              .restart = true,
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_robust(v)); },
              .hint = hn_robust}, menu::after("save_pipeline_list").order(50))
        .row({.key = "astc_as_rgba8",
              .label = "ASTC as RGBA8",
              .help = "Big ASTC textures kept as decoded RGBA8 instead of re-encoded to BC3: a workaround for texture artifacts seen on some AMD "
                      "cards (issues 57 and 1006). Takes far more video memory. Applies after restart.",
              .kind = menu::Kind::Toggle,
              .value = &opts.astc_rgba8,
              .lo = 0, .hi = 1,
              .restart = true,
              .preset_exempt = true,
              .locked = []() -> const char* {   // (why: its line while the switch holds it, hn_astc_rgba8)
                      return getenv("RX_VK_ASTC_RGBA8") ? hn_astc_rgba8(0, -1) : nullptr;
                  },
              .hint = hn_astc_rgba8}, menu::after("save_pipeline_list").order(70));
}

} // namespace mkwvk
