// The Insert menu's rows for Intel graphics (#1633), in the Debug tab's Developer section, shown only on Intel graphics on
// Windows, the one place IntelDriverGate holds the compiler (vk_internal.h): how many shaders Intel's compiler is given at
// once (RX_VK_NO_INTEL_SERIAL holds it for the run), and quick compute builds first (vk_draw.cpp compute_optimize). What the
// renderer asks of them (intel_compiles_side_by_side, intel_quick_compute) is here too, fenced from a layout-only build
// (MENU_LAYOUT_ONLY, MENU_HOOK).
#include "menu_module.h"

#ifndef MENU_LAYOUT_ONLY
#include "intel_compiles.h"   // (intel_side_by_side_chosen)
#include "vk_internal.h"   // (dev: the device's vendor, model and driver)
#endif

#include <string>

namespace mkwvk {

namespace {
#ifndef MENU_LAYOUT_ONLY
/// (longshot) The Intel shader compiles row: Intel graphics on Windows, the only place IntelDriverGate holds the compiler
bool intel_compiler_row() {
#ifdef _WIN32
    return dev.props.vendorID == 0x8086;
#else
    return false;
#endif
}
#endif
const char* tx_intel_compiles(int v) { return v == 1 ? "Side by side" : v == 2 ? "One at a time" : "Auto"; }
/// (the Intel shader compiles row's line: RX_VK_NO_INTEL_SERIAL holds it, as RX_VK_ASTC_RGBA8 the ASTC row)
const char* hn_intel_compiles(int v, int variant) {
    if (variant <= 0 && getenv("RX_VK_NO_INTEL_SERIAL"))
        return "Set by RX_VK_NO_INTEL_SERIAL: the launcher's build settings (Intel shader compiles), longshot_env.txt or the "
               "environment. Change it there to choose here.";
    static const char* const t[] = {"Side by side on the Arc B-series (driver 101.9154 or newer), where it was measured; one at "
                                    "a time on other Intel graphics, but for the shader preparation's graphics pipelines from "
                                    "driver 101.9175.",
                                    "Shaders are built faster on a CPU with many cores, but Intel's driver can crash the game "
                                    "while it builds them.",
                                    "The safe choice, and the best on a CPU with 4 cores or fewer: a shader Intel's compiler "
                                    "faults on is left out instead of closing the game."};
    return menu_text::hint_in(t, 3, 0, v, variant);
}
} // namespace

#ifndef MENU_LAYOUT_ONLY
// (what the renderer asks: vk_internal.h)
bool intel_compiles_side_by_side() {   // (RX_VK_NO_INTEL_SERIAL in the row: menu_init. Asked once the device is made)
    return intel_side_by_side_chosen(opts.intel_compiles, dev.props.deviceID, dev.props.driverVersion);
}
// (the shader preparation's graphics builds: Auto is side by side there from driver 101.9175 too, intel_compiles.h)
bool intel_prepare_side_by_side() {
    return intel_prepare_side_by_side_chosen(opts.intel_compiles, dev.props.deviceID, dev.props.driverVersion);
}
// (Intel's Windows driver only, where the row shows: Mesa's ANV ignores DISABLE_OPTIMIZATION, same instructions, spills and
// compile time on an Iris Xe, its slow part register allocation at SIMD32)
bool intel_quick_compute() { return opts.intel_quick != 0 && intel_compiler_row(); }   // (RX_VK_COMPUTE_QUICK_FIRST in it: menu_init)
#endif

MENU_MODULE(intel) {
    menu::tab("debug")
        .row({.key = "intel_shader_compiles",
              .label = "Intel shader compiles",
              .help = "Intel graphics: how many shaders Intel's compiler is given at once. Applies after a restart.",
              .value = &opts.intel_compiles,
              .lo = 0, .hi = 2,
              .restart = true,
              .preset_exempt = true,
              .shown = MENU_HOOK(intel_compiler_row),
              .locked = []() -> const char* {   // (why: its line while the switch holds it, hn_intel_compiles)
                      return getenv("RX_VK_NO_INTEL_SERIAL") ? hn_intel_compiles(0, -1) : nullptr;
                  },
              .text = [](int v) { return std::string(tx_intel_compiles(v)); },
              .hint = hn_intel_compiles}, menu::after("save_pipeline_list").order(80))
        .row({.key = "intel_quick_compute_builds",
              .label = "Intel quick compute builds",
              .help = "Intel graphics, shaders one at a time: a pass over the scene (its light, its exposure) is built first without the "
                      "compiler's optimizer, in seconds instead of minutes, and drawn with until the optimized one is ready. Changes at once.",
              .kind = menu::Kind::Toggle,
              .value = &opts.intel_quick,
              .lo = 0, .hi = 1,
              .preset_exempt = true,
              .shown = MENU_HOOK(intel_compiler_row)}, menu::after("save_pipeline_list").order(90));
}

} // namespace mkwvk
