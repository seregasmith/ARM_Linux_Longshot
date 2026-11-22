// The Insert menu's upscaler rows (#1633), in the Graphics tab's Resolution and upscaling section after the output's: what
// scales the 3D up to the Output resolution (the game's own, NVIDIA DLSS and DLAA, AMD FSR 3.1 and FSR 4, Intel XeSS, or
// none: upscaler_rows.h; each shown only where it runs, and the row shows what runs), NVIDIA's DLSS model, AMD's FSR 4
// model, DLAA before the bloom, the Render resolution (the 3D's share of the output, on the upscaler's own steps) and
// Sharpness. What runs is the renderer's (vk_menu.cpp upscaler, upscalers_available: menu_module.h); a row with nothing
// to choose is locked, its line saying why. The parts that ask the renderer are fenced from a layout-only build
// (MENU_LAYOUT_ONLY, MENU_HOOK).
#include "menu_module.h"

#include "upscaler_rows.h"

#ifndef MENU_LAYOUT_ONLY
#include "menu/output.h"     // (game_size: the 3D's size at a share of the output)
#include "menu/upscaler.h"
#include "vk_dlss.h"       // (dlss_why, dlss_built)
#include "vk_fsr4.h"       // (fsr4_why, fsr4_built)
#include "vk_internal.h"   // (device_vram_bytes)
#include "vk_xess.h"       // (xess_why, xess_built)
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

namespace mkwvk {

namespace ur = upscaler_rows;

#ifndef MENU_LAYOUT_ONLY
/// The DLSS model row (opts.dlss_model): NVIDIA's network by its preset (the K row's E on a card with less than 7 GB of video
/// memory: upscaler_rows.h cnn_for_vram, as DLSS is made), or RX_VK_DLSS_PRESET's for this run
const char* dlss_preset_env() {
    static const char* const e = getenv("RX_VK_DLSS_PRESET");
    return ur::preset_env(e) ? e : nullptr;
}
const char* tx_upscaler(int) {
    static const char* t[ur::kUpscalers] = {"Game's own", "NVIDIA DLSS", "NVIDIA DLAA", "AMD FSR 3.1", "AMD FSR 4",
                                            "AMD FSR 4 Native AA", "Off (no upscaling)", "Intel XeSS", "Intel XeSS Native AA"};
    // (by engine: DLAA and FSR 4's and XeSS's native AA are the Render resolution row's Native, upscaler_rows.h engine)
    return t[std::clamp(ur::engine(upscaler()), 0, ur::kUpscalers - 1)];
}
const char* tx_resolution(int v) {
    static char b[64];
    const int up = upscaler();
    // (each stop with the 3D's real size, the render input: a twelfth of the picture (game_size), Native
    // the picture itself)
    if (up == ur::DLAA || up == ur::FSR4_AA || up == ur::XESS_AA) {
        uint32_t w, h;
        game_size(1.0f, w, h);   // (the whole picture, the output shown: twelve twelfths of it)
        snprintf(b, sizeof b, "Native (%s, %ux%u)", up == ur::DLAA ? "DLAA" : up == ur::FSR4_AA ? "FSR 4" : "XeSS", w, h);
        return b;
    }
    v = ur::snap_render(up, v);   // (show the active engine's size without overwriting the player's saved scale)
    if (!ur::percent(v)) return "Auto (dynamic)";
    uint32_t w, h;
    game_size(ur::percent(v) / 100.0f, w, h);
    // (XeSS: the preset whose range holds the share, upscaler_rows.h xess_mode_name)
    if (const char* mode = up == ur::XESS ? ur::xess_mode_name(v) : ur::scales_by_mode(up) ? ur::dlss_mode_name(v) : nullptr) {
        snprintf(b, sizeof b, "%s %d%% (%ux%u)", mode, ur::percent(v), w, h);
        return b;
    }
    snprintf(b, sizeof b, "%d%% (%ux%u)", ur::percent(v), w, h);
    return b;
}
#endif

namespace {
#ifndef MENU_LAYOUT_ONLY
/// The Upscaler: the one shown (what runs, upscaler()), and with the game's own, why DLSS, FSR and XeSS are not offered.
/// Variants (every one): each value, then the game's own with each of DLSS's reasons, then with each of FSR's, then XeSS's
const char* hn_upscaler(int v, int variant) {
    static const char* const t[ur::kUpscalers] = {
        "The game's own network.",
        "NVIDIA's, from a smaller 3D. Softer than the game's own: set Sharpness to around 80%. Things that move by "
        "themselves can trail for now.",
        "NVIDIA's, the 3D at full size with its edges smoothed; the slowest. Softer than the game's own: set Sharpness to "
        "around 80%. Things that move by themselves can trail for now.",
        "AMD's, from a smaller 3D, on any GPU: built from AMD's own shaders. Things that move by themselves can trail for "
        "now.",
        "AMD's machine-learned upscaler, from a smaller 3D, with AMD's files beside the game. Things that move by themselves "
        "can trail for now.",
        "AMD's machine-learned upscaler, the 3D at full size with its edges smoothed; the slowest.",
        "No upscaler, not even the game's network: the 3D is stretched to the output with a plain linear filter. The "
        "fastest choice and the roughest picture: no edge smoothing, and softer below 100%.",
        "Intel's machine-learned upscaler, from a smaller 3D, with Intel's libxess.dll beside the game. Things that move by "
        "themselves can trail for now.",
        "Intel's machine-learned upscaler, the 3D at full size with its edges smoothed; the slowest.",
    };
    static const char* const fsr_no[FSR_WHY_COUNT] = {
        nullptr,
        "AMD FSR 4: not in this build (see the readme).",
        "AMD FSR 4: off for this run (RX_VK_FSR4=0).",
        "AMD FSR 4: AMD's files were not found beside the game (see the readme).",
        "AMD FSR 4: AMD's files have no FSR 4 for this GPU.",
    };
    static const char* const xess_no[XESS_WHY_COUNT] = {
        nullptr,
        "Intel XeSS: Windows only (Intel makes no XeSS library for Linux).",
        "Intel XeSS: off for this run (RX_VK_XESS=0).",
        "Intel XeSS: Intel's libxess.dll was not found beside the game (see the readme).",
        "Intel XeSS: the GPU or its Vulkan driver lacks what XeSS needs.",
        "Intel XeSS: Intel's library does not run on this GPU or driver.",
    };
    // (now: the value shown and what runs here, with every engine's reasons; every one: as said above)
    int d = 0, f = 0, x = 0;
    if (variant < 0) {
        v = std::clamp(upscaler(), 0, ur::kUpscalers - 1), d = (int)dlss_why(), f = (int)fsr4_why(), x = (int)xess_why();
    } else if (v == ur::GAME) {
        if (variant < DLSS_WHY_COUNT) d = variant;
        else if (variant < DLSS_WHY_COUNT + FSR_WHY_COUNT - 1) f = variant - DLSS_WHY_COUNT + 1;
        else if (variant < DLSS_WHY_COUNT + FSR_WHY_COUNT + XESS_WHY_COUNT - 2) x = variant - DLSS_WHY_COUNT - FSR_WHY_COUNT + 2;
        else return nullptr;
    } else if (variant > 0) {
        return nullptr;
    }
    v = std::clamp(v, 0, ur::kUpscalers - 1);
    // (the reasons are said only where the engine is in the build: one without FSR does not tell an NVIDIA user why, and a
    // Linux build does not tell anyone that XeSS is Windows only)
    const char* dn = dlss_built() || variant >= 0 ? menu_dlss_why_text(d) : nullptr;
    const char* fn = fsr4_built() || variant >= 0 ? fsr_no[f] : nullptr;
    const char* xn = xess_built() || variant >= 0 ? xess_no[x] : nullptr;
    if (v != ur::GAME || (!dn && !fn && !xn)) return t[v];
    static char b[DLSS_WHY_COUNT][FSR_WHY_COUNT][XESS_WHY_COUNT][400];
    snprintf(b[d][f][x], sizeof b[d][f][x], "%s%s%s%s%s%s%s", t[0], dn ? " " : "", dn ? dn : "", fn ? " " : "", fn ? fn : "",
             xn ? " " : "", xn ? xn : "");
    return b[d][f][x];
}
const char* tx_dlss_model(int v) {
    static const char* const t[] = {"Transformer (K)", "Transformer 2 (M)", "Transformer 2 (L)", "NVIDIA default", "CNN (E)", "CNN (F)"};
    if (const char* e = dlss_preset_env()) {
        static char b[48];
        const unsigned p = ur::dlss_preset(v, e);
        if (p) snprintf(b, sizeof b, "Preset %c (RX_VK_DLSS_PRESET)", ur::preset_letter(p));
        else snprintf(b, sizeof b, "NVIDIA default (RX_VK_DLSS_PRESET)");
        return b;
    }
    if (ur::cnn_for_vram(v, device_vram_bytes())) return "CNN (E)";
    return t[std::clamp(v, 0, ur::kModels - 1)];
}
/// The DLSS model: the value chosen (the K row's on a card under 7 GB: why it is E); variant 1: the game's own upscales (the
/// row waits for DLSS or DLAA); variant 2: set for this run by RX_VK_DLSS_PRESET
const char* hn_dlss_model(int v, int variant) {
    static const char* const t[] = {
        "NVIDIA's transformer model: the one tested most here.",
        "NVIDIA's second-generation transformer, its pick for Performance: sharper, at a little more GPU time.",
        "The second-generation transformer as NVIDIA tuned it for Ultra Performance, the smallest Render resolutions.",
        "Lets DLSS's runtime pick for each mode: what it picks can change with a runtime update.",
        "NVIDIA's lightweight CNN model: uses ~59 MB VRAM and runs faster on constrained RTX GPUs.",
        "NVIDIA's lightweight CNN model tuned for DLAA and Ultra Performance (~59 MB VRAM).",
    };
    static const char* const unused = "Used by NVIDIA DLSS and DLAA: choose one in the Upscaler row.";
    static const char* const env = "Set for this run by RX_VK_DLSS_PRESET.";
    static const char* const cnn = "NVIDIA's CNN model, chosen for graphics cards with less than 7 GB of memory: less video "
                                   "memory and GPU time than the transformer.";
    if (variant < 0) variant = dlss_preset_env() ? 2 : upscaler() == ur::GAME || upscaler() == ur::OFF ? 1 : 0;
    if (variant == 1) return unused;
    if (variant == 2) return env;
    if (variant == 0 && ur::cnn_for_vram(v, device_vram_bytes())) return cnn;
    return variant == 0 ? t[std::clamp(v, 0, ur::kModels - 1)] : nullptr;
}
/// The FSR 4 model row (opts.fsr4_model): AMD's own pick for this GPU, or the smaller INT8 Lite network
const char* tx_fsr4_model(int v) {
    return v ? "Lite (INT8)" : "Auto (AMD's)";
}
/// The FSR 4 model: the value chosen; variant 1: the Upscaler is not FSR 4 (the row waits); variant 2: set for this run
/// by RX_VK_FSR4_MODEL
const char* hn_fsr4_model(int v, int variant) {
    static const char* const t[] = {
        "AMD's own choice for this GPU: FP8 on RDNA 4, full INT8 on RDNA 3 and Strix Halo, the Navi 2 INT8 model on RX 6000, "
        "Lite on Strix Point and Van Gogh.",
        "AMD's smaller FSR 4 network. On a GPU AMD already runs Lite "
        "on, this setting does nothing.",
    };
    static const char* const unused = "Used by AMD FSR 4: choose it in the Upscaler row.";
    static const char* const env = "Set for this run by RX_VK_FSR4_MODEL.";
    if (variant < 0) variant = fsr4_model_env() >= 0 ? 2 : !ur::is_fsr4(upscaler()) ? 1 : 0;
    if (variant == 1) return unused;
    if (variant == 2) return env;
    return variant == 0 ? t[std::clamp(v, 0, ur::kFsr4Models - 1)] : nullptr;
}
/// (longshot) The DLAA before the bloom row (opts.dlss_pre_bloom; vk_temporal.cpp pre_bloom_wanted): RX_VK_DLSS_PRE_BLOOM set
/// for this run wins over it
bool pre_bloom_env() {
    static const char* const e = getenv("RX_VK_DLSS_PRE_BLOOM");
    return e && *e;
}
/// Its line: the value chosen; variant 1: DLSS at a Render resolution below Native (the row waits for DLAA); variant 2: set
/// for this run by RX_VK_DLSS_PRE_BLOOM
const char* hn_dlss_pre_bloom(int v, int variant) {
    static const char* const t[] = {
        "Off: DLAA smooths the finished picture, after the game's bloom and tone map.",
        "On: DLAA smooths the 3D before the game's bloom and tone map, so the glow is made from a smooth picture.",
    };
    static const char* const not_dlaa = "DLAA only: set the Render resolution to Native. DLSS's other modes draw the 3D smaller "
                                        "than the picture, which this cannot go back into.";
    static const char* const env = "Set for this run by RX_VK_DLSS_PRE_BLOOM.";
    if (variant < 0) variant = pre_bloom_env() ? 2 : upscaler() != ur::DLAA ? 1 : 0;
    if (variant == 1) return not_dlaa;
    if (variant == 2) return env;
    return variant == 0 ? t[std::clamp(v, 0, 1)] : nullptr;
}
/// The Render resolution: the value chosen, for the Upscaler shown (every one: each value with each Upscaler)
const char* hn_resolution(int v, int variant) {
    const int up = variant < 0 ? std::clamp(upscaler(), 0, ur::kUpscalers - 1) : variant;
    if (up >= ur::kUpscalers) return nullptr;
    // (Native is the slider's top: the 3D at the picture's full size)
    if (up == ur::DLAA) return "DLAA: the 3D at the picture's full size, anti-aliased by DLSS. The sharpest, and the heaviest.";
    if (up == ur::FSR4_AA) return "FSR 4's native AA: the 3D at the picture's full size. The sharpest, and the heaviest.";
    if (up == ur::XESS_AA) return "XeSS's Native AA: the 3D at the picture's full size. The sharpest, and the heaviest.";
    const int p = ur::percent(ur::snap_render(up, v));
    if (up == ur::DLSS) {
        switch (p) {
        case 0: return "The game lowers it when the GPU is busy, and DLSS follows. Set Sharpness to around 80%.";
        case 92: case 83: case 75: return "Between Native and Quality: sharper than Quality, a little slower.";
        case 67: return "DLSS's Quality mode: its sharpest from a smaller 3D. Set Sharpness to around 80%.";
        case 58: return "Between Quality and Performance. Set Sharpness to around 80%.";
        case 50: return "Faster, and softer. Set Sharpness to around 80%.";
        case 42: return "Between Performance and Ultra Performance.";
        case 33: return "DLSS's fastest mode, and its softest. Set Sharpness to around 80%.";
        default: return nullptr;   // (not one of DLSS's steps: the row moves to one)
        }
    }
    if (up == ur::FSR3 || up == ur::FSR4) {   // (the same modes, AMD's ratios)
        switch (p) {
        case 0: return "The game lowers it when the GPU is busy, and FSR follows.";
        case 100: return "FSR 3.1's native AA: the 3D at the picture's full size, anti-aliased by FSR. The sharpest, and the heaviest.";
        case 92: case 83: case 75: return "Between native AA and Quality: sharper than Quality, a little slower.";
        case 67: return "FSR's Quality mode, 1.5x: the sharpest of AMD's upscaling modes.";
        case 58: return "FSR's Balanced mode, 1.7x.";
        case 50: return "FSR's Performance mode, 2x: faster, and softer.";
        case 33: return "FSR's Ultra Performance mode, 3x: its fastest, and its softest.";
        default: return nullptr;   // (not one of FSR's steps: the row moves to one)
        }
    }
    if (up == ur::XESS) {   // (each stop under the XeSS preset whose range holds it: xess_rules.h)
        switch (p) {
        case 0: return "The game lowers it when the GPU is busy, and XeSS follows.";
        case 92: case 83: return "XeSS's Ultra Quality Plus mode, 1.3x: sharper than Ultra Quality, a little slower.";
        case 75: return "Between Ultra Quality Plus and Ultra Quality, under XeSS's Ultra Quality mode.";
        case 67: return "XeSS's Ultra Quality mode, 1.5x.";
        case 58: return "Between XeSS's Quality and Balanced, under its Balanced mode.";
        case 50: return "XeSS's Balanced mode, 2x: faster, and softer.";
        case 33: return "XeSS's Ultra Performance mode, 3x: its fastest, and its softest.";
        default: return nullptr;   // (not one of the slider's stops: the row moves to one)
        }
    }
    if (up == ur::OFF) {   // (no upscaler: the 3D stretched to the output as it is drawn)
        if (!p) return "The game lowers it when the GPU is busy; the 3D is stretched from whatever size it draws.";
        if (p == 100) return "The 3D at the output's full size, not stretched: the sharpest, but its edges stay jagged.";
        return "A steady size, stretched to the output: lower is faster, and blurrier.";
    }
    if (!p) return "The game lowers it when the GPU is busy.";
    if (p == 100) return "The 3D at the output's full size: the sharpest, and the slowest.";
    return "A steady size: lower is faster, and softer.";
}
/// Sharpness: with DLSS or DLAA, the amount that suits them
const char* hn_sharpness(int v, int variant) {
    static const char* const dlss = "DLSS and DLAA look softer than the game's own upscaler: around 80% suits them.";
    const bool with_dlss = variant < 0 ? upscaler() == ur::DLSS || upscaler() == ur::DLAA : variant == 1;
    if (variant > 1) return nullptr;
    return with_dlss ? dlss : v == 0 ? "Off: the picture as it is." : nullptr;
}
/// (a row with nothing to choose here: its line now says why)
const char* fixed_by_line(bool fixed, const char* line) { return fixed ? (line ? line : "") : nullptr; }
#endif
} // namespace

MENU_MODULE(upscaler) {
    menu::tab("graphics")
        .row({.key = "dlss",
              .label = "Upscaler",
              .help = "What scales the 3D up to the Output resolution.",
              .value = &opts.dlss,
              .lo = 0, .hi = ur::kUpscalers - 1,
              .shown = MENU_HOOK([] { return ur::upscaler_shown(upscalers_available()); }),
              .locked = MENU_HOOK([] { return fixed_by_line(ur::upscaler_fixed(upscalers_available()), hn_upscaler(opts.dlss, -1)); }),
              .text = MENU_HOOK([](int v) { return std::string(tx_upscaler(v)); }),
              .hint = MENU_HOOK(hn_upscaler)}, menu::after("handheld_profile").order(10))
        .row({.key = "dlss_model",
              .label = "DLSS model",
              .help = "Which of NVIDIA's DLSS networks draws the picture.",
              .value = &opts.dlss_model,
              .lo = 0, .hi = ur::kModels - 1,
              .preset_exempt = true,
              .shown = MENU_HOOK([] { return ur::model_shown(dlss_why() == DLSS_OK, upscaler()); }),
              .locked = MENU_HOOK([] {
                  return fixed_by_line(ur::model_fixed(upscaler(), dlss_preset_env() != nullptr), hn_dlss_model(opts.dlss_model, -1));
              }),
              .text = MENU_HOOK([](int v) { return std::string(tx_dlss_model(v)); }),
              .hint = MENU_HOOK(hn_dlss_model)}, menu::after("handheld_profile").order(20))
        .row({.key = "fsr4_model",
              .label = "FSR 4 model",
              .help = "The FSR 4 network: Auto lets AMD pick for this GPU, Lite is the faster INT8 network.",
              .value = &opts.fsr4_model,
              .lo = 0, .hi = ur::kFsr4Models - 1,
              .preset_exempt = true,
              .shown = MENU_HOOK([] { return ur::fsr4_model_shown(fsr4_why() == FSR_OK, fsr4_lite_available()); }),
              .locked = MENU_HOOK([] {
                  return fixed_by_line(ur::fsr4_model_fixed(upscaler(), fsr4_model_env() >= 0), hn_fsr4_model(opts.fsr4_model, -1));
              }),
              .text = MENU_HOOK([](int v) { return std::string(tx_fsr4_model(v)); }),
              .hint = MENU_HOOK(hn_fsr4_model),
              .on_change = MENU_HOOK([](int v) { fsr4_set_model(v ? FSR4_MODEL_LITE : FSR4_MODEL_AUTO); })},
             menu::after("dlss_model").order(25))
        .row({.key = "dlss_pre_bloom",
              .label = "DLAA before the bloom",
              .help = "Where NVIDIA DLAA smooths the picture: after the game's bloom and tone map, or before them. Changes at "
                      "once.",
              .kind = menu::Kind::Toggle,
              .value = &opts.dlss_pre_bloom,
              .lo = 0, .hi = 1,
              .preset_exempt = true,
              .shown = MENU_HOOK([] { return ur::pre_bloom_shown(dlss_why() == DLSS_OK, upscaler()); }),
              .locked = MENU_HOOK([] {
                  return fixed_by_line(ur::pre_bloom_fixed(upscaler(), pre_bloom_env()), hn_dlss_pre_bloom(opts.dlss_pre_bloom, -1));
              }),
              .hint = MENU_HOOK(hn_dlss_pre_bloom)}, menu::after("handheld_profile").order(30))
        .row({.key = "resolution",
              .label = "Render resolution",
              .help = "The share of the output the 3D is drawn at; the Upscaler fills in the rest.",
              .value = &opts.resolution,
              .lo = 0, .hi = ur::kValues - 1,
              .locked = MENU_HOOK([] { return fixed_by_line(ur::render_fixed(upscaler()), hn_resolution(opts.resolution, -1)); }),
              .text = MENU_HOOK([](int v) { return std::string(tx_resolution(v)); }),
              .hint = MENU_HOOK(hn_resolution)}, menu::after("handheld_profile").order(40))
        .row({.key = "sharpness",
              .label = "Sharpness",
              .help = "Sharpens the picture as it goes to the window, with any filter. Too much shows halos around edges.",
              .kind = menu::Kind::Slider,
              .value = &opts.sharpness,
              .lo = 0, .hi = 100, .step = 10,
              .wraps = true,   // (100% goes round to 0%, as it always has: tests/test_menu_slider_ends.py)
              .text = menu_text::percent,
              .hint = MENU_HOOK(hn_sharpness)}, menu::after("handheld_profile").order(50));
}

} // namespace mkwvk
