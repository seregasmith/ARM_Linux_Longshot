// The Insert menu's Quality preset (#1633), the Graphics tab's first row: the rows a GPU with that much memory runs well with
// (the scaling rows, the profile, frame pacing and game speed; Shadow resolution back to Auto), or Auto (this PC), chosen for
// this PC's graphics card, memory and screen (auto_settings.h) and tuned in play; Custom once a row it sets is changed by hand
// (vk_menu.cpp set_row: a row's preset_exempt says it is not one). The Upscaler and Frame generation stay the player's. The
// engine (apply_preset, apply_auto, Auto's state) is what vk_menu.cpp starts with and tunes (gpu/vk/menu/preset.h); fenced from
// a layout-only build (MENU_LAYOUT_ONLY).
#include "menu_module.h"

#include "menu/preset.h"

#ifndef MENU_LAYOUT_ONLY
#include "upscaler_rows.h"   // (snap_render: a preset's Render resolution on the upscaler's steps)
#endif

#include <algorithm>
#include <string>

namespace mkwvk {

#ifndef MENU_LAYOUT_ONLY
namespace ur = upscaler_rows;
// (Sharpness: tuned for the window pass's neighbours one picture pixel apart, each about the old setting's sharpening on a
// 3840 x 2160 window at that preset's picture, 2048 x 1152 or the handheld profile's 1536 x 864: same frame, mean |Laplacian|
// of the window and of the picture, on the slider's steps)
// (longshot) the Quality preset "Auto (this PC)", kAutoPreset: the profile menu_auto_for_gpu chose for this PC (auto_settings.h)
::auto_settings::Choice auto_choice;   // (tier 0: no profile yet, before the card is known)
uint64_t auto_for = 0;               // (the fingerprint of the hardware the options' Auto settings were chosen for: auto_for=)
uint64_t auto_fp = 0;                // (this PC's, menu_auto_for_gpu)
uint64_t auto_tuned = 0;             // (the fingerprint the measured tuning in play finished for: auto_tuned=)
::auto_settings::Tuning auto_tuning;   // (the measured tuning in play, menu_auto_tune; its steps kept as auto_steps=)
void apply_preset(int p) {
    if (p == kAutoPreset) {
        apply_auto();   // (chosen on the row: all of its rows, as any preset)
        return;
    }
    // (the rows it sets are the preset's from now on, even at the value the environment forced: save writes them)
    if (p > 0)
        for (const int* v : {&opts.resolution, &opts.upscale, &opts.sharpness, &opts.pace, &opts.realtime, &opts.handheld})
            env_release(v);
    if (p > 0) opts.shadow_res = 0;   // (Shadow resolution back to Auto: the size for this preset's memory, menu_shadow_res)
    switch (p) {
    case 1: // Minimum (4GB VRAM / iGPU)
        opts.resolution = 4; opts.upscale = 2; opts.sharpness = 30;
        opts.pace = 1;    opts.realtime = 1; opts.handheld = 1;
        break;
    case 2: // Low (6GB VRAM)
        opts.resolution = 2; opts.upscale = 2; opts.sharpness = 30;
        opts.pace = 1;    opts.realtime = 1; opts.handheld = 1;
        break;
    case 3: // Balanced (8GB VRAM)
        opts.resolution = 1; opts.upscale = 1; opts.sharpness = 30;
        opts.pace = 1;    opts.realtime = 1; opts.handheld = 0;
        break;
    case 4: // High (12GB VRAM)
        opts.resolution = 1; opts.upscale = 2; opts.sharpness = 40;
        opts.pace = 1;    opts.realtime = 1; opts.handheld = 0;
        break;
    case 5: // Ultra (16GB+ VRAM)
        opts.resolution = 1; opts.upscale = 2; opts.sharpness = 50;
        opts.pace = 1;    opts.realtime = 1; opts.handheld = 0;
        break;
    default:
        break;
    }
    // (the Upscaler and Frame generation are the player's: a preset never turns DLSS or made frames on or off. With DLSS
    // its resolution is DLSS's nearest mode)
    if (p > 0) opts.resolution = ur::snap_render(upscaler(), opts.resolution);   // (a slider stop kept)
    opts.budget = p;   // (Custom chosen on this row: none, as before)
}
/// The Output resolution Auto never sets (auto_settings.h Choice: the game's own or the player's, read at boot), nor
/// Mesh shaders (RX_VK_MESH_SHADERS), and the fingerprint has the card's mesh shaders, not this run's mode
void apply_auto() {
    const ::auto_settings::Choice& c = auto_choice;
    if (c.tier < 1) return;
    env_release(&opts.dlss);
    opts.dlss = c.upscaler;   // (before the tier's rows: apply_preset snaps its Render resolution to this upscaler)
    apply_preset(c.tier);   // (the tier's scaling rows)
    if (c.tier == 1) opts.upscale = 0, opts.sharpness = 0;   // (the plain scale, as RX_VK_LOW_SPEC: the lightest)
    opts.handheld = 0;   // (docked: the handheld profile gave a GTX 1050 a black picture; the Render resolution lightens the card)
    if (c.resolution) opts.resolution = ur::snap_render(c.upscaler, c.resolution);
    auto_tuned = 0;   // (chosen again: measured again in play)
    auto_tuning = {};
    opts.preset = kAutoPreset;   // (apply_preset set the tier's number)
    auto_for = auto_fp;          // (chosen for this PC, whether at a start or on the menu's row)
}
#endif

namespace {
const char* tx_preset(int v) {
    static const char* t[] = {"Custom", "Minimum (4GB)", "Low (6GB)", "Balanced (8GB)", "High (12GB)", "Ultra (16GB+)",
                              "Auto (this PC)"};
    return t[std::min(std::max(v, 0), 6)];
}
MODULE_HINTS(hn_preset, 0, "Each row as you set it. Changing most rows yourself comes back to Custom.",
             "Resolution 50% (with DLSS its nearest mode), Lanczos, Sharpness 30%, the Handheld Profile.",
             "Resolution 75% (with DLSS its nearest mode), Lanczos, Sharpness 30%, the Handheld Profile.",
             "Resolution 100% (with DLSS its nearest mode), Catmull-Rom, Sharpness 30%.",
             "Resolution 100% (with DLSS its nearest mode), Lanczos, Sharpness 40%.",
             "Resolution 100% (with DLSS its nearest mode), Lanczos, Sharpness 50%.",
             "Chosen for this PC from its graphics card, video memory and screen, and chosen again when they change. The log's "
             "\"Auto (this PC)\" line says why.")
} // namespace

MENU_MODULE(preset) {
    menu::tab("graphics")
        .row({.key = "quality_preset",
              .label = "Quality preset",
              .help = "Sets the scaling rows and Shadow resolution for a GPU with that much memory. The Upscaler and Frame "
                      "generation stay yours.",
              .value = &opts.preset,
              .lo = 0, .hi = 6,
              .text = [](int v) { return std::string(tx_preset(v)); },
              .hint = hn_preset});
}

} // namespace mkwvk
