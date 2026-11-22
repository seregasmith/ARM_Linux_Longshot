// The Insert menu's frame generation and latency rows (#1633): the Graphics tab's section after Resolution and upscaling.
// Frame generation and who makes the frames (NVIDIA DLSS FG, the built-in one, AMD FSR 3.1: vk_framegen.h, vk_dlss.h), the
// game's frame rate cap, and NVIDIA Reflex and AMD Anti-Lag 2 where they run (vk_reflex.h, vk_antilag.h). The parts that ask
// the renderer are fenced from a layout-only build (MENU_LAYOUT_ONLY, MENU_HOOK: menu_module.h). On the Debug tab, for
// testing and not a player's choice: Frame pacing, when each made frame is shown (vk_framegen.cpp).
#include "menu_module.h"

#ifndef MENU_LAYOUT_ONLY
#include "menu/framegen.h"
#include "vk_antilag.h"
#include "vk_dlss.h"
#include "vk_framegen.h"
#include "vk_fsr3fi.h"
#include "vk_reflex.h"
#endif

#include <algorithm>
#include <cstdio>
#include <string>

namespace mkwvk {

#ifndef MENU_LAYOUT_ONLY
const char* tx_framegen(int v) {
    static const char* t[] = {"Off", "Auto", "2x", "3x", "4x", "5x", "6x", "7x", "8x"};
    // (Auto, chosen: who makes its frames now (vk_framegen.cpp framegen_maker, as the FPS counter names it). NVIDIA DLSS FG
    // makes fixed multipliers only, so they are AMD FSR 3.1's or the built-in one's; none made lately (the game keeps up with
    // the display): Auto alone)
    if (v == 1 && opts.framegen == 1) {
        const FgMaker m = framegen_maker();
        if (m == FgMaker::Fsr3) return "Auto (FSR 3.1 FG)";
        if (m == FgMaker::BuiltIn) return "Auto (Built-in FG)";
    }
    // (as it runs: a saved value past DLSS FG's top while it makes the frames shows as the top, vk_framegen.h framegen_capped)
    return t[framegen_capped(v, dlss_fg_most())];
}
/// (longshot) Whether the Frame generation by row offers DLSS FG: where DLSS-G can run in this process as far as is known
/// (vk_dlss.cpp dlss_fg_unavailable). Where it cannot, the row has FSR 3.1 FG (0, the default, and 2 the same) and Built-in
/// FG, and its line at FSR 3.1 FG says why not DLSS FG
bool framegen_by_dlss_offered() { return dlss_fg_unavailable() == FG_SKIP_NONE; }
/// Frame generation by with nothing to choose: RX_VK_DLSS_FG=1 chooses DLSS FG for the run (vk_dlss.cpp dlss_fg_forced)
bool framegen_by_fixed() { return dlss_fg_forced(); }
#endif

namespace {
MODULE_HINTS(hn_pace, 0,
             "As soon as it is ready. Frame generation's fixed multipliers show each frame at its own time.",
             "At an even interval: less stutter, at up to one frame of delay. Frame generation's fixed multipliers show each "
             "frame at its own time.")
#ifndef MENU_LAYOUT_ONLY
/// Frame generation: how many frames are made for each of the game's
const char* hn_framegen(int v, int variant) {
    const int dlss_most = variant < 0 ? dlss_fg_most() : 0;
    if (variant < 0) v = framegen_capped(v, dlss_most);   // (the value shown: DLSS FG's top for a saved one past it)
    // (2x: by AMD FSR 3.1 where it makes the frame, from the scene's depth and motion: vk_framegen.cpp. Not while NVIDIA
    // DLSS FG makes it (dlss_fg_most: chosen, on a temporal Upscaler, its runtime's answer a yes, not given up): FSR 3.1 is
    // asked then too (fsr3fi::frames_wanted), but only for the sets DLSS-G does not make. Variant 1 is that text)
    if (v == 2 && (variant < 0 ? dlss_most == 0 && fsr3fi::frames_wanted() != 0 : variant == 1))
        return "One made frame between each two of the game's, by AMD FSR 3.1 from the scene's depth and motion.";
    if (variant > 0) return nullptr;
    if (v <= 0) return "Only the game's own frames: no added delay.";
    if (v == 1) return "A frame for each of the display's refreshes: none made when the game keeps up. Frame pacing does not apply.";
    static char b[9][96];
    snprintf(b[v], sizeof b[v], "%s between each two of the game's.", v == 2 ? "One made frame" : v == 3 ? "Two made frames" : v == 4 ? "Three made frames"
             : v == 5 ? "Four made frames" : v == 6 ? "Five made frames" : v == 7 ? "Six made frames" : "Seven made frames");
    return b[std::clamp(v, 2, 8)];
}
/// Frame generation by: the choice, DLSS FG (0), Built-in FG (1) or FSR 3.1 FG (2); where DLSS FG cannot run, 0 is FSR 3.1
/// FG (AMD FSR 3.1's frame interpolation, vk_fsr3fi.cpp asked); DLSS FG with RX_VK_DLSS_FG=1.
/// Who makes the frames now (the game's own menus give DLSS-G nothing to make them from) is the row's line and the FPS
/// counter's (framegen_maker_name), not the choice's name
const char* tx_framegen_by(int v) {
    if (framegen_by_dlss_offered() && (v == 0 || dlss_fg_forced())) return "DLSS FG";
    return v == 1 ? "Built-in FG" : "FSR 3.1 FG";
}
/// Why DLSS-G does not make the frames now (vk_dlss.h FgSkip, and the Auto multiplier's FG_SKIP_COUNT), the row's line after
/// "Not DLSS FG now: ". most: its runtime's most made frames, for FG_SKIP_TOO_MANY
const char* framegen_skip_text(int why, unsigned most) {
    switch (why) {
    case FG_SKIP_OFF: return "DLSS Frame Generation is turned off for this run (RX_VK_DLSS_FG=0).";
    case FG_SKIP_DEVICE: return "NVIDIA DLSS does not run on this GPU.";
    case FG_SKIP_START:
        return "NVIDIA's runtime did not start DLSS Frame Generation: it needs an RTX 40 or newer, and nvngx_dlssg.dll in the "
               "game's ngx folder (the builder's NVIDIA DLSS tick).";
    case FG_SKIP_TOO_MANY: {
        static char b[160];
        snprintf(b, sizeof b, "Its runtime makes %u made frames at most: a lower Frame generation, or a newer nvngx_dlssg.dll.",
                 most ? most : 1u);
        return b;
    }
    case FG_SKIP_INPUTS:
        return "No picture from a temporal Upscaler to make them from: NVIDIA DLSS or DLAA, or AMD FSR, on the Upscaler row. "
               "The game's own menus have none.";
    case FG_SKIP_REFUSED: return "NGX refused its frames.";
    case FG_SKIP_NOT_BUILT: return "This build has no NVIDIA DLSS (the builder's NVIDIA DLSS tick).";
    case FG_SKIP_COUNT: return "DLSS Frame Generation makes a fixed 2x and up, not Frame generation's Auto.";
    default: return nullptr;
    }
}
/// Its line: the choice's. Where DLSS FG cannot run in this run, why, whatever is saved ("DLSS FG cannot run: " and
/// framegen_skip_text, or for DLSS itself dlss_why_text: the log has NGX's own answer); with RX_VK_DLSS_FG=1, that it chooses.
/// At DLSS FG with the game's frames alone now (its own menus, movies and loading screens draw no 3D to make frames from:
/// vk_dlss.cpp dlss_fg_holds), that it is paused; with another making the frames now, why ("Not DLSS FG now: "). Variants
/// (every one): the choice's; at DLSS FG "Not DLSS FG now: " with each reason, and the paused line; the switch's line; then
/// "DLSS FG cannot run: " with each of its reasons
const char* hn_framegen_by(int v, int variant) {
    static const char* const t[] = {
        "NVIDIA DLSS Frame Generation (RTX 40 and newer), made from the Upscaler's depth and motion.",
        "The renderer's own: made from the finished picture alone.",
        "AMD FSR 3.1 frame interpolation, made from the scene's depth and motion: any GPU.",
    };
    static const char* const paused = "DLSS FG paused: the game's own menus, movies and loading screens draw no 3D to make "
                                      "frames from, so only the game's own frames show.";
    // (dlss_fg_unavailable's reasons, each for the whole run; FG_SKIP_DEVICE with each of DLSS's own, dlss_why)
    struct Cannot { int why, dlss; };
    static const Cannot cannot[] = {{FG_SKIP_OFF, 0},
                                    {FG_SKIP_DEVICE, DLSS_ENV_OFF},
                                    {FG_SKIP_DEVICE, DLSS_NEEDS_RTX},
                                    {FG_SKIP_DEVICE, DLSS_NEEDS_DRIVER},
                                    {FG_SKIP_DEVICE, DLSS_NO_RUNTIME},
                                    {FG_SKIP_DEVICE, DLSS_RUNTIME_NOT_ASCII},
                                    {FG_SKIP_START, 0},
                                    {FG_SKIP_REFUSED, 0},
                                    {FG_SKIP_NOT_BUILT, 0}};
    constexpr int kCannot = (int)(sizeof cannot / sizeof cannot[0]);
    // (at DLSS FG: 1 .. FG_SKIP_COUNT each reason, FG_SKIP_COUNT the Auto multiplier's; then the paused line)
    const int nows = v == 0 ? (int)FG_SKIP_COUNT + 1 : 0;
    const int forced_at = nows + 1;   // (the switch's line; the reasons it cannot run after it)
    static char b[FG_SKIP_COUNT + kCannot + 4][400];
    int why = FG_SKIP_NONE, dlss = (int)DLSS_OK;
    unsigned most = 0;
    bool off = false;      // (DLSS FG cannot run: not offered)
    bool forced = false;   // (RX_VK_DLSS_FG=1 chooses)
    if (variant >= 0) {
        if (variant == 0) return t[std::clamp(v, 0, 2)];
        if (variant < nows) why = variant;
        else if (variant == nows) return paused;
        else if (variant > forced_at && variant - forced_at <= kCannot) {
            const Cannot& c = cannot[variant - forced_at - 1];
            why = c.why, dlss = c.dlss, off = true;
        } else if (variant == forced_at) {
            forced = true;
        } else {
            return nullptr;
        }
    } else if (v == 1 && !dlss_fg_forced()) {
        return t[1];   // (Built-in FG, chosen)
    } else if ((why = dlss_fg_unavailable()) != FG_SKIP_NONE) {   // (FSR 3.1 FG makes them: why not DLSS FG)
        off = true, dlss = (int)dlss_why();
    } else if (dlss_fg_forced()) {
        forced = true;
    } else {
        if (v != 0) return t[v == 2 ? 2 : 1];
        const FgMaker m = opts.framegen >= 1 ? framegen_maker() : FgMaker::None;
        if (m == FgMaker::DlssPaused) return paused;
        if (m != FgMaker::BuiltIn && m != FgMaker::Fsr3) return t[0];
        why = opts.framegen == 1 ? (int)FG_SKIP_COUNT : dlss_fg_skip(&most);
    }
    char* out = b[variant >= 0 ? variant : FG_SKIP_COUNT + kCannot + 3];
    if (forced) {   // (as the V-Sync row's switches)
        snprintf(out, sizeof b[0], "%s is set: it chooses for this run.", "RX_VK_DLSS_FG=1");
        return out;
    }
    const char* text = off && why == FG_SKIP_DEVICE && menu_dlss_why_text(dlss) ? menu_dlss_why_text(dlss)
                                                                          : framegen_skip_text(why, most);
    if (!text) return t[off || v == 2 ? 2 : v == 1 ? 1 : 0];
    snprintf(out, sizeof b[0], off ? "DLSS FG cannot run: %s" : "Not DLSS FG now: %s", text);
    return out;
}
const char* tx_reflex(int v) {
    static const char* t[] = {"Off", "On", "On + Boost"};
    return t[reflex_available() ? std::clamp(v, 0, 2) : 0];
}
/// Reflex: on the value (the row is shown only where Reflex runs: row_shown)
const char* hn_reflex(int v, int variant) {
    static const char* const t[] = {"The game as without it.", "The driver paces the frames for less latency.",
                                    "As On, and the GPU's clocks are kept up as well."};
    return variant <= 0 ? t[std::clamp(v, 0, 2)] : nullptr;
}
const char* tx_antilag(int v) { return antilag_available() && v ? "On" : "Off"; }
/// Anti-Lag 2: on the value (the row is shown only where Anti-Lag 2 runs: row_shown)
const char* hn_antilag(int v, int variant) {
    static const char* const t[] = {"The game as without it.", "The driver delays each frame's start for less latency."};
    return variant <= 0 ? t[v ? 1 : 0] : nullptr;
}
/// (the step past DLSS FG's top: set_row's, as it was) Frame generation while NVIDIA DLSS FG makes the frames ends at its
/// runtime's most made frames + 1 (vk_framegen.h framegen_capped: 6x on an RTX 50's runtime, 2x on an RTX 40's), as if that
/// were its last value. A saved value past it shows as that top (tx_framegen), so an arrow steps from the value shown; past
/// the top it goes round to Off and down from Off to the top, as at 8x. A value set as it is (the control bridge's) is kept
/// as asked (step_fn is for the arrows alone): past the top it runs as the top
int framegen_step(int now, int dir) {
    const int most = dlss_fg_most(), top = framegen_capped(8, most);
    int v = now >= 2 ? framegen_capped(now, most) + dir : now + dir;
    if (v > top) v = 0;
    else if (v < 0) v = top;
    return v;
}
#endif
const char* tx_fps_cap(int v) {
    if (v == -1) return "30";
    static const char* const t[] = {"60 (the console's)", "90", "120", "144", "165", "240", "Uncapped"};
    return t[v < 0 ? 0 : v > 6 ? 6 : v];
}
} // namespace

MENU_MODULE(framegen) {
    menu::tab("graphics")
        .heading("framegen", "Frame generation and latency", menu::after("resolution"))
        .row({.key = "frame_generation",
              .label = "Frame generation",
              .help = "Makes frames between the game's own, for smoother motion, at a little input delay. Fast motion or camera cuts can "
                      "show artifacts. With NVIDIA DLSS Frame Generation it goes up to what NVIDIA's runtime makes on this GPU: 6x on an "
                      "RTX 50, 2x on an RTX 40.",
              .value = &opts.framegen,
              .lo = 0, .hi = 8,
              .preset_exempt = true,
              .text = MENU_HOOK([](int v) { return std::string(tx_framegen(v)); }),
              .hint = MENU_HOOK(hn_framegen),
              .step_fn = MENU_HOOK(framegen_step)})
        .row({.key = "frame_generation_by",
              .label = "Frame generation by",
              .help = "Who makes the frames at 2x and up. NVIDIA DLSS Frame Generation needs a temporal Upscaler (DLSS, DLAA or FSR) for "
                      "the scene's depth and motion. Changes at once.",
              .value = &opts.framegen_by,
              .lo = 0, .hi = 2,
              .preset_exempt = true,
              .text = MENU_HOOK([](int v) { return std::string(tx_framegen_by(v)); }),
              .hint = MENU_HOOK(hn_framegen_by)})
        .row({.key = "frame_rate_cap",
              .label = "Game frame rate cap",
              .help = "Limits frames rendered by the game. Generated frames are additional. Changes at once. "
                      "Past 60 is experimental: the game keeps real time, but some cutscenes and bosses can misbehave (Gohma, the intro's "
                      "narration). 60 is the safe choice. Uncapped: as fast as the PC can, still at real time; with V-Sync on, the display's "
                      "refresh rate is the limit. 30 FPS always uses real-time game speed, even when Game speed is Per frame.",
              .value = &opts.fps_cap,
              .lo = -1, .hi = 6,
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_fps_cap(v)); }})
        .row({.key = "nvidia_reflex",
              .label = "NVIDIA Reflex",
              .help = "NVIDIA GPUs: each frame starts as late as it can, for less input delay. With frame generation too: it paces the "
                      "game's frames.",
              .value = &opts.reflex,
              .lo = 0, .hi = 2,
              .preset_exempt = true,
              .shown = MENU_HOOK([] { return reflex_available(); }),
              .text = MENU_HOOK([](int v) { return std::string(tx_reflex(v)); }),
              .hint = MENU_HOOK(hn_reflex)})
        .row({.key = "amd_anti-lag_2",
              .label = "AMD Anti-Lag 2",
              .help = "AMD GPUs: each frame starts as late as it can, for less input delay. With frame generation too: it paces "
                      "the game's frames.",
              .value = &opts.antilag,
              .lo = 0, .hi = 1,
              .preset_exempt = true,
              .shown = MENU_HOOK([] { return antilag_available(); }),
              .text = MENU_HOOK([](int v) { return std::string(tx_antilag(v)); }),
              .hint = MENU_HOOK(hn_antilag)});
    menu::tab("debug")
        .row({.key = "frame_pacing",
              .label = "Frame pacing",
              .help = "When each frame is shown.",
              .kind = menu::Kind::Toggle,
              .value = &opts.pace,
              .lo = 0, .hi = 1,
              .hint = hn_pace}, menu::after("save_pipeline_list").order(10));
}

} // namespace mkwvk
