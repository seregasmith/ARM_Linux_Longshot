// The per-area simulated-turf controls page. The merged short-decorative-blade Grass distance row stays in detail.cpp;
// the renderer's private turf lists measure this page's resulting demand and resize independently
// (vk_draw.cpp sim_turf_private_binding).
#include "menu_module.h"

#include "menu/sim_turf.h"

#ifndef MENU_LAYOUT_ONLY
#include "vk_internal.h"
#include "vk_paths.h"
#include "menu/sim_turf_settings.h"
#endif

#include <array>
#include <string>

namespace mkwvk {
namespace {
constexpr char sim_turf_about[] =
    "Simulated turf is the broad field grass animated by the game's fluid simulation: the dense carpet that bends and "
    "ripples around Link. Grass distance controls separate short decorative blades.";

std::string sim_turf_decimal_text(int value, int scale, int decimals, bool multiplier = false) {
#ifndef MENU_LAYOUT_ONLY
    return sim_turf_settings::decimal(value, scale, decimals, multiplier);
#else
    char text[24];
    snprintf(text, sizeof text, "%.*f%s", decimals, double(value) / scale, multiplier ? "x" : "");
    return text;
#endif
}
} // namespace

#ifndef MENU_LAYOUT_ONLY
void sim_turf_settings_init() { sim_turf_settings::load_config(); }
void sim_turf_settings_tick(int64_t now) {
    if (sim_turf_settings::tick(now)) menu_redraw();
}
void sim_turf_settings_flush() { sim_turf_settings::flush_config(); }
#else
void sim_turf_settings_init() {}
void sim_turf_settings_tick(int64_t) {}
void sim_turf_settings_flush() {}
#endif

MENU_MODULE(sim_turf) {
    menu::tab("graphics")
        .row({.key = "@Simulated turf",
              .label = "Simulated turf",
              .help = sim_turf_about,
              .kind = menu::Kind::Page,
              .opens = "sim_turf_settings"}, menu::after("grass_distance"));

#ifndef MENU_LAYOUT_ONLY
    using namespace sim_turf_settings;
#else
    static int global = 0;
    static std::array<int, 4> multipliers{10, 10, 10, 10};
    static std::array<int, 4> values{};
    constexpr int max_factor = 100;
#endif
    auto page = menu::page("sim_turf_settings");
    page.row({.key = "sim_turf_global_multipliers",
              .label = "Use global multipliers",
              .help = "Apply multipliers to each area's installed simulated-turf profile. 1.0x uses the profile value.",
              .kind = menu::Kind::Toggle,
              .value = &global,
              .lo = 0, .hi = 1,
              .preset_exempt = true,
              .hint = [](int, int variant) -> const char* { return variant <= 0 ? sim_turf_about : nullptr; },
              .on_change = MENU_HOOK([](int) { mode_changed(); })}, menu::order(10));

    page.row({.key = "sim_turf_fade_start_multiplier",
              .label = "Fade start multiplier",
              .help = "Scales each area's fade start within the game's fade range.",
              .kind = menu::Kind::Slider,
              .value = &multipliers[0],
              .lo = 0, .hi = max_factor,
              .preset_exempt = true,
              .shown = MENU_HOOK([] { return global != 0; }),
              .text = [](int v) { return sim_turf_decimal_text(v, 10, 1, true); },
              .on_change = MENU_HOOK([](int) { multiplier_changed(); })}, menu::order(20))
        .row({.key = "sim_turf_density_multiplier",
              .label = "Turf density multiplier",
              .help = "Scales simulated-turf density relative to each area. 0.0x reduces density to near zero.",
              .kind = menu::Kind::Slider,
              .value = &multipliers[1],
              .lo = 0, .hi = max_factor,
              .preset_exempt = true,
              .shown = MENU_HOOK([] { return global != 0; }),
              .text = [](int v) { return sim_turf_decimal_text(v, 10, 1, true); },
              .on_change = MENU_HOOK([](int) { multiplier_changed(); })}, menu::order(30))
        .row({.key = "sim_turf_minimum_blades_multiplier",
              .label = "Minimum blades multiplier",
              .help = "Scales each area's minimum simulated-turf blade count. The result is rounded to a whole blade.",
              .kind = menu::Kind::Slider,
              .value = &multipliers[2],
              .lo = 0, .hi = max_factor,
              .preset_exempt = true,
              .shown = MENU_HOOK([] { return global != 0; }),
              .text = [](int v) { return sim_turf_decimal_text(v, 10, 1, true); },
              .on_change = MENU_HOOK([](int) { multiplier_changed(); })}, menu::order(40))
        .row({.key = "sim_turf_far_power_multiplier",
              .label = "Far power multiplier",
              .help = "Scales each area's simulated-turf density falloff exponent.",
              .kind = menu::Kind::Slider,
              .value = &multipliers[3],
              .lo = 0, .hi = max_factor,
              .preset_exempt = true,
              .shown = MENU_HOOK([] { return global != 0; }),
              .text = [](int v) { return sim_turf_decimal_text(v, 10, 1, true); },
              .on_change = MENU_HOOK([](int) { multiplier_changed(); })}, menu::order(50));

    page.row({.key = "sim_turf_fade_start",
              .label = "Fade start",
              .help = "Distance at which simulated turf begins fading.",
              .kind = menu::Kind::Slider,
              .value = &values[0],
              .lo = 0, .hi = 649,
              .preset_exempt = true,
              .shown = MENU_HOOK([] { return global == 0; }),
              .locked = MENU_HOOK([]() -> const char* { return ready ? nullptr : "Load an area."; }),
              .text = [](int v) { return sim_turf_decimal_text(v, 10, 1); },
              .hint = MENU_HOOK([](int, int variant) -> const char* { return variant <= 0 ? area_note : nullptr; }),
              .on_change = MENU_HOOK([](int) { area_changed(0); })}, menu::order(20))
        .row({.key = "sim_turf_density",
              .label = "Turf density",
              .help = "Controls how densely simulated turf is placed.",
              .kind = menu::Kind::Slider,
              .value = &values[1],
              .lo = 1, .hi = 500,
              .preset_exempt = true,
              .shown = MENU_HOOK([] { return global == 0; }),
              .locked = MENU_HOOK([]() -> const char* { return ready ? nullptr : "Load an area."; }),
              .text = [](int v) { return sim_turf_decimal_text(v, 1, 0); },
              .hint = MENU_HOOK([](int, int variant) -> const char* { return variant <= 0 ? area_note : nullptr; }),
              .on_change = MENU_HOOK([](int) { area_changed(1); })}, menu::order(30))
        .row({.key = "sim_turf_minimum_blades",
              .label = "Minimum blades",
              .help = "Lower blade count as simulated turf thins with distance.",
              .kind = menu::Kind::Slider,
              .value = &values[2],
              .lo = 0, .hi = 128,
              .preset_exempt = true,
              .shown = MENU_HOOK([] { return global == 0; }),
              .locked = MENU_HOOK([]() -> const char* { return ready ? nullptr : "Load an area."; }),
              .text = [](int v) { return sim_turf_decimal_text(v, 1, 0); },
              .hint = MENU_HOOK([](int, int variant) -> const char* { return variant <= 0 ? area_note : nullptr; }),
              .on_change = MENU_HOOK([](int) { area_changed(2); })}, menu::order(40))
        .row({.key = "sim_turf_far_power",
              .label = "Far power",
              .help = "Shapes the simulated-turf density falloff with distance.",
              .kind = menu::Kind::Slider,
              .value = &values[3],
              .lo = 0, .hi = 400, .step = 5,
              .preset_exempt = true,
              .shown = MENU_HOOK([] { return global == 0; }),
              .locked = MENU_HOOK([]() -> const char* { return ready ? nullptr : "Load an area."; }),
              .text = [](int v) { return sim_turf_decimal_text(v, 100, 2); },
              .hint = MENU_HOOK([](int, int variant) -> const char* { return variant <= 0 ? area_note : nullptr; }),
              .on_change = MENU_HOOK([](int) { area_changed(3); })}, menu::order(50));

    page.row({.key = "@Reset current simulated-turf area",
              .label = "Reset current area",
              .help = "Clear this area's saved edits and restore its installed simulated-turf preset.",
              .kind = menu::Kind::Action,
              .locked = MENU_HOOK([]() -> const char* { return can_reset_area() ? nullptr : ready ? "This area already uses its installed preset." : "Load an area."; }),
              .text = MENU_HOOK([](int) { return std::string(can_reset_area() ? "Reset" : ready ? "Using preset" : "Load an area"); }),
              .hint = MENU_HOOK([](int, int variant) -> const char* { return variant <= 0 ? area_note : nullptr; }),
              .on_activate = MENU_HOOK(reset_area),
              .verb = "Reset"}, menu::order(70));
}

} // namespace mkwvk
