// The Insert menu. Drawn with Dear ImGui over the window's image at the last step of a present (menu_record; vk_imgui.h, in the
// game's style: vk_ui.h), so it costs nothing while closed and it never touches the game's picture, nor what frame interpolation
// sees. The FPS counter and the start-up screen are drawn the same way. F1 always opens and closes it, Insert too by default.
// Up and Down choose, Left and Right change, Esc closes; the game gets no keyboard input while it is open.
// (longshot) A gamepad's Home button, or Select and Start together, opens it too; the D-pad chooses and changes, the button
// the Switch's A is bound to changes too (and opens a page), the one B is bound to goes back (vk_menu_keys.h). The game gets no gamepad input while it is open either.
// Its Controls page binds the Switch's buttons to gamepad inputs and keys.
// (longshot) The mouse is free while it is open (the window lets it go: WM_APP + 8, host/display.c and
// host/posix/display_sdl.c): the row under the cursor is selected, a click on a tab shows it, on a value's left or right
// half changes it down or up, on a toggle flips it, on a slider's track sets it (or drag it); the wheel scrolls the rows
// (never a value, wherever the cursor is); a right click is Esc. On the Cheats page and its lists a click is Enter (on a
// choice's or an amount's left or right half, Left or Right) and a right click goes back. The drawing records each control's
// place and the mouse is tested against that (vk_menu_hit.h).
// (longshot) The rows that take a restart (Row::restart) are tagged "After restart" while their value is not the one the game
// started with, and the Graphics tab's help lists them. Closing the menu then asks whether to restart now (menu_restart.h):
// Later keeps the saved settings for the next start and is not asked again until they change; Restart now saves and
// closes the window as the player would, and the game starts again (WM_APP + 9: host/hos_relaunch.c).
#include <atomic>
#include <chrono>
#include <cmath>
#include <cerrno>
#include <utility>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <deque>
#include <array>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <system_error>
#include <vector>

#include "vk_paths.h"
#include "vk_menu.h"
#include "../../host/hos_atomic_file.h"
#include "vk_menu_hit.h"
#include "vk_fps.h"    // (the FPS counter's levels and numbers)
#include "vk_draw.h"   // (pipeline_cache_save: Restart now keeps what was built)
#include "vk_texture.h"   // (the FPS counter's Dev level: texture_count, texture_bytes, texture_stats)
#include "oot_cheat_items.h"
#include "vk_progress.h"   // (StartupView, version_text: the start-up screen)
#include "vk_imgui.h"   // (the renderer's side of the overlay: begin, fonts, logo, record)
#include "vk_ui.h"
#include "vk_menu_keys.h"   // (which gamepad buttons confirm and go back: those A and B are bound to)         // (how it looks: Dear ImGui, vk_imgui.h)
#include "vk_dlss.h"
#include "vk_framegen.h"   // (Frame generation by: who makes the frames now, framegen_maker)
#include "vk_bug_report.h"   // (the Gameplay tab's Bug report: what it writes, and how)
#include "../../host/hos_wide_size.h"   // (the widescreen picture's size, as host/hos_main.c works it out)
#include "../../host/audio_output_devices.h"
#include "monitor_choice.h"
#include "menu/controller.h"   // (the host's report of what plays the controller, which Test vibration shows: #1633)
#include "vk_fsr3.h"      // (the Upscaler row: whether AMD FSR 3.1 runs on this GPU)
#include "vk_fsr3fi.h"    // (the Frame generation row: whether FSR 3.1 makes the frame)
#include "vk_fsr4.h"
#include "vk_xess.h"
#include "vk_temporal.h"
#include "upscale_route.h"   // (the FPS counter footer's record of the gameplay upscale as drawn)
#include "vk_reflex.h"
#include "vk_antilag.h"
#include "upscaler_rows.h"
#include "menu_module.h"   // (#1633: the model, the rows modules describe and the order of every row)
#include "menu/audio.h"   // (the Audio module's, still used here: the options file's lines, set_row, the test's meter)
#include "menu/display.h"   // (the display module's, still used here: the options file's lines, the screen followed)
#include "menu/preset.h"   // (the Quality preset module's, still used here: Auto at the start, its tuning, set_row)
#include "menu/output.h"   // (the output module's, still used here: the options file, the bug report, the pipeline share)
#include "menu/upscaler.h"   // (the upscaler module's, still used here: the pipeline share's line, the bug report)
#include "menu/bindings.h"   // (the Controls page's module, still used here: the grid, the options file, menu_tick)
#include "menu/sprint.h"   // (the Sprint module's, still used here: the Controls page's Reset lays Sprint out again)
#include "menu/sim_turf.h"   // (the Simulated turf page's area observation, persistence and live resource edits)
#include "menu/pipeline_list.h"   // (Save my pipeline list's, still used here: its worker finished, its line measured)
#include "menu/nfc.h"   // (the NFC module's, still used here: its tags listed when the menu opens, a scan followed)
#include "menu/framegen.h"   // (the frame generation module's, still used here: the pipeline share's line, set_row, row_fixed)
#include "menu/reshade.h"
#include "auto_settings.h"
#include "menu_restart.h"
#include "threaded_rendering.h"   // (the Threaded rendering row's rule, and the one gate over its Auto)
#include "vk_menu_i18n.h"
#include "display_modes.h"   // (the Aspect ratio and Output resolution rows: the screen's sizes and their shapes)
#ifndef LONGSHOT_ENV_H   // (env_on: the switches' one rule)
#include "longshot_env.h"
#endif
#include "pipeline_share.h"   // (Save my pipeline list: the zip for issue 14)
#ifndef _WIN32   // (Save my pipeline list: the desktop's programs. On Windows, shell32 and ole32 are loaded when it is used)
#include <fcntl.h>
#include <spawn.h>
#include <csignal>
#include <cerrno>
#include <pthread.h>
#include <sys/stat.h>   // (save: the options file's mode)
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)
const char* mkwvk_version();   // (vk_version.cpp: the one renderer file nx2/build.py gives the version to)
#ifndef LONGSHOT_VERSION   // (a test or a screenshot fixture that compiles this file may give its own)
#define LONGSHOT_VERSION mkwvk_version()
#endif

namespace mkwvk {
int64_t slot_wait_ns();   // (vk_replay.cpp: the replay's waits for a recording slot so far, the FPS counter's Dev line)
int display_refresh_mhz();   // (vk_main.cpp: the window's display, asked once a second at most: the FPS counter's V-Sync target)
HWND app_window();
extern std::atomic<bool> present_native_pq;   // (vk_replay.cpp: the presented picture is Native HDR, bypassing the SDR CRT filters)

Options opts;

namespace {
enum MenuTab {
    TAB_GRAPHICS = 0,
    TAB_AUDIO = 1,
    TAB_GAMEPLAY = 2,
    TAB_CONTROLS = 3,
    TAB_NFC = 4,     // (longshot: the player's NFC tags, held to the game's reader: host/nn_nfp.c)
    TAB_MODS = 5,    // (the mods folder's mods and their cheats, shown when there are mods: draw_mods)
    TAB_DEBUG = 6,   // (the OoT debug tools, a tab of their own)
    TAB_COUNT = 7,
    TAB_MOTION = TAB_COUNT,   // (longshot: not a tab of the strip, the Motion page that the Controls tab's Motion row
                              // opens: one past the strip's tabs, so a tab added before TAB_COUNT moves it along)
    TAB_SPEAKER = TAB_MOTION + 1,   // (longshot: not a tab either, the Controller speaker page, from the Audio tab)
    TAB_FPS_DEV = TAB_SPEAKER + 1,   // (longshot: not a tab either, the FPS counter's Dev lines page, from the Gameplay tab)
    TAB_RECORDINGS = TAB_FPS_DEV + 1,   // (longshot: nor this, the input recordings to play, from the Debug tab: menu/input_recording.cpp)
    TAB_SIM_TURF = TAB_RECORDINGS + 1,   // (the Simulated turf page, opened by the Graphics tab's row)
    TAB_PAGE = TAB_SIM_TURF + 1   // (longshot: the Controls page's rows, above its grid; no tab shows them)
};

struct Row {
    MenuTab tab;
    const char* name;
    int* value;
    int lo, hi, step;
    const char* (*text)(int v);
    const char* help;   // (shown under the list while the row is selected: what the row is, the same whatever its value)
    // (a line about the value chosen, under the help, or nullptr: hint(v, -1) is the one to show now for v, hint(v, 0),
    // hint(v, 1), ... every one v can have (those that depend on the other rows or on the GPU), until nullptr, so the
    // menu keeps the height of the tallest: measure_help)
    const char* (*hint)(int v, int variant) = nullptr;
    bool restart = false;   // (read once, at the start: a change applies after a restart)
    bool preset_exempt = false;   // (a change here leaves the Quality preset alone: every row apply_preset does not set,
                                  // tests/test_preset_exempt.py)
    signed char cheat = -1;   // (a debug tool, on the Debug tab with the tools on: its index in the cheat_ tables, -1 none)
    bool heading = false;     // (a section's title on its tab, not a row: the selection steps over it)
    const menu::Setting* setting = nullptr;   // (#1633: a row a module describes (menu_model.h): its hooks; nullptr for this
                                              // file's own rows)
};
const char* tx_onoff(int v) { return v ? "On" : "Off"; }
/// HDR: on, when the swapchain stayed SDR, why (dev.hdr_sdr_why, vk_main.cpp create_swapchain). Variants: each reason
const char* hn_hdr(int v, int variant) {
    static const char* const t[] = {
        "The picture stays SDR: the graphics driver offers no HDR output (no VK_EXT_swapchain_colorspace).",
        "The picture stays SDR: HDR is off for this display in Windows' display settings.",
        ("The picture stays SDR: Windows has HDR on, but the graphics driver offers the game no HDR output. Reinstall the "
         "driver with a clean install, then restart the PC (longshot.magicbean.dev/help)."),
        "The picture stays SDR: the display offers no HDR output. Is HDR on in the display settings?"};
    const int reasons = hdr_sdr_count - 1;   // (every HdrSdrWhy but hdr_sdr_none)
    static_assert(sizeof t / sizeof *t == hdr_sdr_count - 1, "hn_hdr: a line for each HdrSdrWhy but hdr_sdr_none");
    if (variant >= 0) return variant < reasons ? t[variant] : nullptr;
    return v && dev.hdr_sdr_why != hdr_sdr_none ? t[std::clamp((int)dev.hdr_sdr_why, 1, reasons) - 1] : nullptr;
}
// (longshot) Motion controls: the choice, and whether a connected controller has a gyroscope (host/pad_sdl.c). Either can be
// chosen whatever is connected: a pad plugged in later is read when it comes
const char* tx_gyro(int v) {
    const int status = host && host->gyro_status ? host->gyro_status() : -1;
    if (status == 2) return v ? "On (gyro found)" : "Off (gyro found)";
    if (status == 1) return v ? "On (no gyro on this pad)" : "Off (no gyro on this pad)";
    if (status == 0) return v ? "On (no controller)" : "Off (no controller)";
    return v ? "On" : "Off";
}
// (the game's picture and the window it is shown in, as the last present had them: menu_note_present_sizes)
std::atomic<uint64_t> present_sizes{0};
bool filter_needed() {
    const uint64_t v = present_sizes.load(std::memory_order_relaxed);
    return upscaler_rows::filter_needed((unsigned)(v >> 48), (unsigned)(v >> 32) & 0xffff, (unsigned)(v >> 16) & 0xffff,
                                        (unsigned)v & 0xffff);
}
const char* tx_upscale(int v) {
    static const char* t[] = {"Off", "Catmull-Rom", "Lanczos"};
    if ((unsigned)v >= sizeof t / sizeof t[0]) return t[0];   // (a setting past the row: the label, not the next bytes)
    return t[v];
}
// The Upscaler (opts.dlss): what scales the 3D up to the output. DLSS and DLAA only where dlss_why() lets them run, FSR 3.1
// only where vk_fsr3.cpp says it does, FSR 4 and its native AA only where fsr4_why() does, Intel XeSS and its Native AA only
// where xess_why() does, Off (no upscaler: vk_temporal.cpp temporal_off) everywhere; the menu shows (and steps from) what runs,
// and keeps the saved choice for a PC where it can
namespace ur = upscaler_rows;
} // namespace   // (#1633: asked by the modules too, menu_module.h's "what modules ask the renderer")
/// One bit per upscaler that runs here (upscaler_rows.h bit). Each backend caches its own answer, so this costs nothing
unsigned upscalers_available() {
    return ur::bit(ur::GAME) | ur::bit(ur::OFF) | ur::dlss_bits(dlss_why() == DLSS_OK) |
           (fsr3::supported() ? ur::bit(ur::FSR3) : 0u) | ur::fsr4_bits(fsr4_why() == FSR_OK) |
           ur::xess_bits(xess_why() == XESS_OK);
}
/// (FSR 3.1 has inputs of its own; DLSS, FSR 4 and XeSS run on the frame chain they share, temporal_mode; Off is neither)
int upscaler() { return fsr3_wanted() ? (int)ur::FSR3 : temporal_off() ? (int)ur::OFF : temporal_mode(); }
namespace {   // (#1633: the anonymous namespace again, after what the modules ask)
/// Why DLSS and DLAA do not run here (vk_dlss.h DlssWhy), for the Upscaler row and Frame generation by; nullptr for DLSS_OK
const char* dlss_why_text(int why) {
    static const char* const t[DLSS_WHY_COUNT] = {
        nullptr,
        "DLSS and DLAA: not in this build (built without NVIDIA's DLSS SDK, see the readme).",
        "DLSS and DLAA: off for this run (RX_VK_DLSS=0).",
        "DLSS and DLAA: need an NVIDIA RTX GPU.",
        "DLSS and DLAA: need a newer NVIDIA driver.",
        "DLSS and DLAA: DLSS's runtime was not found (the game folder's ngx folder, see the readme).",
        "DLSS and DLAA: NVIDIA's NGX on Linux loads DLSS's runtime only from a folder named in ASCII (see the readme).",
    };
    return why > 0 && why < DLSS_WHY_COUNT ? t[why] : nullptr;
}
constexpr const char* kMicLevelHelp =
    "Shows microphone signal strength, not note recognition.\n\n"
    "- First percentage: incoming peak before gain.\n"
    "- Second percentage: peak after Microphone gain, also shown by the bar.\n\n"
    "At 100% gain they match. Increasing gain also raises noise and can clip the signal.";
namespace dm = display_modes;
Out output_preview_target{};
bool output_preview_from_field = false;
/// A row with nothing else to choose here: shown without its arrows, Left and Right leave it (and the saved value) alone
bool module_locked(const int* value);   // (#1633: a module's row with this value, locked by its own locked(): below)
bool row_fixed(const int* value) {
    // Output and the settings that can change its shape or invalidate the preview path wait for the trial.
    if (output_preview_target.w && (value == &opts.output_res || value == &opts.ultrawide ||
        value == &opts.handheld || value == &opts.preset || value == &opts.vsync || value == &opts.framegen ||
        value == &opts.hdr || value == &opts.reflex || value == &opts.dlss)) return true;
    return module_locked(value) || (value == &opts.upscale && !filter_needed()) ||
           (value == &opts.framegen_by && framegen_by_fixed()) ||   // (its line says why)
           (value == &opts.hud_layout && getenv("HOS_WIDE_UI") && *getenv("HOS_WIDE_UI"));   // (host/hos_wide_ui.c's, first)
}
// (longshot) Walk mode: how the Walk key (the Controls page) makes Link walk with the movement keys (host/nn_hid.c walk_held)
// (longshot) Menu size: how large the overlays are drawn (vk_ui.h unit), Medium unless the player chooses Small or Large. It
// was Auto by default, Large in fullscreen at 1080 lines and up while a controller opened the menu: the menu changed size with
// the input that opened it, and the version in its banner no longer fitted. An options file's menu_size=0 (that Auto): Medium
constexpr float kVersionMinPt = 10;   // (the banner's version at Small and Medium: shrunk to fit, no smaller than this, in points)
bool pad_last = false;   // (a controller was the menu's last input: its buttons in the prompts)
int pad_confirm = 0, pad_back = 1;   // (the face buttons that confirm and go back, HOS_PAD_*: menu_keys::confirm_back, each tick)
int pad_paste = 3;   // (longshot: the one that pastes into a text field while it has the keyboard: menu_keys::paste)
std::string gpu_saved_card;           // (the GPU device row's card as the options file saved it, gpu_device_id=: gpu_choice.h)
bool gpu_card_missing = false;        // (that card is not among this run's GPUs: menu_set_gpu_count)
/// (longshot) Who made the made frames lately (vk_framegen.cpp framegen_maker), with Frame generation on; nullptr: none made.
/// One name each, "<engine> FG", on the Frame generation by row and the FPS counter alike; "DLSS FG paused" where DLSS FG
/// showed the game's frames alone (the game's own menus: vk_dlss.cpp dlss_fg_holds)
const char* framegen_maker_name() {
    if (opts.framegen < 1) return nullptr;
    switch (framegen_maker()) {
    case FgMaker::Dlss: return "DLSS FG";
    case FgMaker::Fsr3: return "FSR 3.1 FG";
    case FgMaker::BuiltIn: return "Built-in FG";
    case FgMaker::DlssPaused: return "DLSS FG paused";
    default: return nullptr;
    }
}
/// (the environment's switch for this run, over the row: menu_vsync)
const char* vsync_env() {
    if (env_on("RX_VK_FIFO")) return "RX_VK_FIFO";
    if (env_on("RX_VK_IMMEDIATE")) return "RX_VK_IMMEDIATE";
    return nullptr;
}

// ---- (longshot) text fields: rows whose value is typed (MkwVkHost.take_text: WM_CHAR on Windows, SDL's text input on Linux;
// RX_VK_MENU_SCRIPT's TYPE). A field holds printable ASCII alone, as the menu's glyphs are baked for it (vk_imgui.cpp
// bake_some): full-width digits and x are folded to ASCII, other characters are left out, and allow() can take fewer.
// While editing (menu_tick), keys type into the field only: Left, Right, Home and End move the caret, Backspace and
// Delete remove a character, Ctrl+V and Shift+Insert paste the clipboard's first line (the host does: take_text), Enter sets
// the text (the field's apply, which can refuse it and say why), Esc puts the text back as it was. A controller's confirm and
// back are Enter and Esc, its D-pad moves the caret and cycles characters, its shoulders add and remove a character,
// its paste button (menu_keys::paste) pastes; a click elsewhere is Enter, a right click Esc; closing the menu is Esc too ----
struct TextField {
    char text[64];                     // (NUL-terminated)
    int len = 0, caret = 0;            // (in bytes, a character each: ASCII)
    int max = 63;                      // (the most it holds, below sizeof text)
    bool (*allow)(char c) = nullptr;   // (of the printable ASCII, what it takes; nullptr all of it)
    const char* (*pad_chars)(const TextField&) = nullptr;   // (characters offered at the caret, in cycling order)
};
enum { kTextGoOn, kTextDone, kTextCancel };
/// One input to a field: a character typed (a Unicode code point) or HOS_TEXT_KEY | an editing key (vk_api.h). The text and its
/// caret changed; kTextDone for Enter, kTextCancel for Esc, else kTextGoOn
int text_edit(TextField& f, uint32_t in) {
    f.len = std::clamp(f.len, 0, (int)sizeof f.text - 1), f.caret = std::clamp(f.caret, 0, f.len);
    if (in & HOS_TEXT_KEY) {
        switch (in & ~(uint32_t)HOS_TEXT_KEY) {
        case VK_RETURN: return kTextDone;
        case VK_ESCAPE: return kTextCancel;
        case VK_LEFT: f.caret = std::max(f.caret - 1, 0); break;
        case VK_RIGHT: f.caret = std::min(f.caret + 1, f.len); break;
        case VK_HOME: f.caret = 0; break;
        case VK_END: f.caret = f.len; break;
        case VK_BACK:
            if (f.caret > 0) memmove(f.text + f.caret - 1, f.text + f.caret, (size_t)(f.len - f.caret + 1)), f.caret--, f.len--;
            break;
        case VK_DELETE:
            if (f.caret < f.len) memmove(f.text + f.caret, f.text + f.caret + 1, (size_t)(f.len - f.caret)), f.len--;
            break;
        case VK_UP:
        case VK_DOWN:
        case VK_INSERT: {
            if (!f.pad_chars) break;
            const char* chars = f.pad_chars(f);
            const int n = (int)strlen(chars), key = (int)(in & ~(uint32_t)HOS_TEXT_KEY);
            if (!n) break;
            const char c = f.caret < f.len ? f.text[f.caret] : 0;
            const char* at = c ? strchr(chars, c == 'X' ? 'x' : c) : nullptr;
            const int next = key == VK_INSERT ? 0 : at ? ((int)(at - chars) + (key == VK_UP ? 1 : n - 1)) % n
                                                                      : key == VK_UP ? 0 : n - 1;
            if (key == VK_INSERT || f.caret == f.len) {
                const int caret = f.caret;
                text_edit(f, (uint32_t)chars[next]);
                f.caret = caret;   // (the new character is the one to cycle)
            } else if (!f.allow || f.allow(chars[next])) f.text[f.caret] = chars[next];
            break;
        }
        }
        return kTextGoOn;
    }
    if (in >= 0xff10 && in <= 0xff19) in = '0' + in - 0xff10;
    else if (in == 0xff58) in = 'x';
    if (in < 0x20 || in > 0x7e || f.len >= std::min(f.max, (int)sizeof f.text - 1) || (f.allow && !f.allow((char)in)))
        return kTextGoOn;   // (not printable ASCII, not this field's, or no room: left out)
    memmove(f.text + f.caret + 1, f.text + f.caret, (size_t)(f.len - f.caret + 1));
    f.text[f.caret++] = (char)in;
    f.len++;
    return kTextGoOn;
}
/// A field's text set whole, cut to what it holds, the caret at its end
void text_set(TextField& f, const char* t) {
    snprintf(f.text, sizeof f.text, "%.*s", std::clamp(f.max, 0, (int)sizeof f.text - 1), t ? t : "");
    f.len = f.caret = (int)strlen(f.text);
}

// (#1633) This file's own rows: the core module's (MENU_MODULE(core) below). The menu draws `rows`, these and the rows
// modules describe, in the order the model works out
namespace core {
// (#1633: every other row is a module's, gpu/vk/menu/. What is left here is the core's section headings and the ImGui menu's
// custom section: the rows it draws and presses with its own code until the panels' rework (bespoke_row: the model has them
// as Custom, their label a fallback for another presenter): Custom output size's text field, the Controls row's bindings
// grid, and the Debug tools' panel)
Row rows[] = {
    // Graphics, in sections: a heading row is a section's title (draw_options draws it, the selection steps over it). The
    // Quality preset first, as it sets rows in every section. (Reflex, Anti-Lag 2, the Upscaler and the DLSS model only where
    // they run here; HDR's paper white and peak brightness only with HDR on: row_shown)
    // (the picture's way, top to bottom: the output's size, what fills it from the 3D, the 3D's size, its sharpening; then
    // the frames, and the window it is shown in)
    {TAB_GRAPHICS, "Resolution and upscaling", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},
    {TAB_GRAPHICS, "Custom output size", nullptr, 0, 0, 0, nullptr,   // (longshot: a text field, field_rows)
     "An Output resolution of your own, typed as width x height: your monitor's size, such as 3440x1440, 2560x1080 or "
     "2560x1600. Each side is rounded down to a multiple of 8; a size the Output resolution offers is chosen there. Empty: "
     "none. Docked only. Applies after a restart."},
    // (the output's rows: gpu/vk/menu/output.cpp's, Aspect ratio and Output resolution before Custom output size, Profile after)
    {TAB_GRAPHICS, "Screen", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},
    // (its rows: gpu/vk/menu/display.cpp, crt.cpp and widescreen.cpp)
    // (the HDR section: gpu/vk/menu/hdr.cpp, after Screen)
    // (the GPU and memory section: gpu/vk/menu/gpu.cpp, last. No Mesh shaders row: changing it translated every shader
    // again at the next start, for nothing a player gains. A launch switch now, RX_VK_MESH_SHADERS: gpu/vk/mesh_mode.h)

    // Audio

    // Gameplay

    // Controls (17 rows; Motion opens the 6 TAB_MOTION rows, Controls its page with the 2 TAB_PAGE rows)
    // (with Mouse camera on Native the game's own camera inversion, which turns the right stick around, never reaches
    // the mouse: this row is how its left and right are swapped, in both modes. No line about which way the camera
    // then turns: As the stick turns it as the game turns it for the right stick, which need not be Native's way)
    // (the Controls page's own rows, above its grid: gpu/vk/menu/bindings.cpp's, Layout preset and Button prompts)
    {TAB_CONTROLS, "Controls", nullptr, 0, 0, 0, nullptr,   // (longshot: opens its page)
     "The layout preset, the button prompts, and which gamepad button and which keys press each of the Switch's "
     "buttons."},
    // Debug: the tools' switch, Autosave and Damage taken (gpu/vk/menu/debug_tools.cpp), the tools themselves below it while
    // they are on (cheat: menu_tick does what they do, draw_debug_rows lays them out), then the developer's rows
    {TAB_DEBUG, "Heart protection", nullptr, 0, 0, 0, nullptr,
     "Protects Link's hearts. Hits can still knock him back.", nullptr, false, true, 0},
    {TAB_DEBUG, "No-clip flight", nullptr, 0, 0, 0, nullptr,
     "Enable double-tap Jump for takeoff and landing. Hold Jump to climb or L to descend.", nullptr, false, true, 1},
    {TAB_DEBUG, "Mark position", nullptr, 0, 0, 0, nullptr,
     "Remember Link's position for this loaded scene.", nullptr, false, true, 2},
    {TAB_DEBUG, "Return to mark", nullptr, 0, 0, 0, nullptr,
     "Teleport Link to the marked position in the current scene.", nullptr, false, true, 3},
    {TAB_DEBUG, "Set time", nullptr, 0, 0, 0, nullptr,
     "Choose an hour with Left or Right. Enter sets the world clock.", nullptr, false, true, 4},
    {TAB_DEBUG, "Inventory item", nullptr, 0, 0, 0, nullptr,
     "Choose a consumable with Left or Right.", nullptr, false, true, 5},
    {TAB_DEBUG, "Change quantity", nullptr, 0, 0, 0, nullptr,
     "Choose how many to add or remove. Enter applies the change.", nullptr, false, true, 6},
    {TAB_DEBUG, "Free camera", nullptr, 0, 0, 0, nullptr,
     "Move and look with your usual controls. Page Up / Page Down or R / L changes height.", nullptr, false, true, 7},
    {TAB_DEBUG, "Teleport", nullptr, 0, 0, 0, nullptr,
     "Opens the map list. Link travels through the game's own map change, in his current age.", nullptr, false, true, 8},
    {TAB_DEBUG, "Switch age", nullptr, 0, 0, 0, nullptr,
     "Swaps Link between child and adult and reloads this map in the other age (the Market when this map has no match).", nullptr, false, true, 9},
    {TAB_DEBUG, "Inventory", nullptr, 0, 0, 0, nullptr,
     "Enter opens the inventory editor: unlock or relock equipment, items, upgrades and quest items, and change amounts.", nullptr, false, true, 10},
    {TAB_DEBUG, "Movement speed", nullptr, 0, 0, 0, nullptr,
     "How fast Link walks, runs and swims, as a multiple of his own speed. Choose with Left or Right.", nullptr, false, true, 11},
    {TAB_DEBUG, "Developer", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},
    // (its rows: gpu/vk/menu/pipeline_list.cpp's Save my pipeline list first, then the features' (framegen.cpp's Frame pacing,
    // threads.cpp, replay_cache.cpp, gpu.cpp's Robust buffers and ASTC as RGBA8, intel.cpp), laptop.cpp's last)
    // (the Gameplay tab's last rows: Bug report, gpu/vk/menu/bug_report.cpp; Restart and Quit, session.cpp)
};
} // namespace core
constexpr int kCoreRows = sizeof core::rows / sizeof core::rows[0];

// ---- (#1633) the rows as drawn: this file's and the modules', through the model (menu_model.h) ----
// rows[i] is core::rows[i] for i < kCoreRows (the same index, so the table's indices stay what they were), then the rows
// modules describe. Each tab's order is the model's (row_order); nothing else changes for this file's rows. Built the first
// time anything asks, never while the DLL starts: a module in another file registers then, in an order nothing promises.
// tests/test_menu_rows_model.py checks it.
const char* (ini_key)(const Row& r);   // (spelled apart from its definition: the checks cut that by its text)
/// The model's page for each MenuTab: the strip's tabs, then the pages a row opens
const char* const kTabIds[TAB_PAGE + 1] = {"graphics", "audio",  "gameplay",    "controls",     "nfc",
                                           "mods",     "debug",  "motion",      "controller_speaker", "fps_details",
                                           "recordings", "sim_turf_settings", "controls_page"};
const char* const kTabLabels[TAB_PAGE + 1] = {"Graphics", "Audio", "Gameplay", "Controls",            "NFC",
                                              "Mods",     "Debug", "Motion",   "Controller speaker",  "FPS counter details",
                                              "Play recording", "Simulated turf", "Controls"};
int page_tab(const char* id);   // (a page's MenuTab by its id: below)
// (rows modules describe, headings included; past it a row is left out, and logged. Every row is a module's: 110 of them
// when the last moved, so room to grow; tests/test_menu_module_slots.py keeps the menu within it)
constexpr int kModuleRowsMax = 256;
constexpr int kMaxRows = kCoreRows + kModuleRowsMax;
std::unordered_map<std::string, int>& core_headings() {   // (a heading row's section id: its index)
    static std::unordered_map<std::string, int> m;
    return m;
}
/// The core's sections, by their heading: ids modules place theirs by, as by a module's (menu_module.h names them)
const char* const kCoreSections[][2] = {{"Resolution and upscaling", "resolution"}, {"Screen", "screen"}, {"Developer", "developer"}};
/// A core row's key in the model: its ini key for a row with a value, a heading's section id, "@" and its name for the rest
const char* core_key(const Row& r) {
    if (r.heading)
        for (const auto& s : kCoreSections)
            if (!strcmp(r.name, s[0])) return s[1];
    static std::deque<std::string> keys;   // (a deque: the keys handed out stay where they are)
    keys.push_back(r.value ? std::string(ini_key(r)) : "@" + std::string(r.name));
    return keys.back().c_str();
}
/// (the ImGui menu's own rows, drawn and pressed by its own code until the panels' rework: the Debug tools' panel (cheat),
/// Custom output size's text field (field_rows), the Controls row's bindings grid (page 1). Custom to the model)
bool bespoke_row(const Row& r) {
    return r.cheat >= 0 ||
           (!r.value && (!strcmp(r.name, "Custom output size") || (r.tab == TAB_CONTROLS && !strcmp(r.name, "Controls"))));
}
MENU_MODULE(core) {
    core_headings().clear();
    for (int t = 0; t <= TAB_PAGE; t++) {
        menu::PageBuilder b = t < TAB_COUNT ? menu::tab(kTabIds[t], kTabLabels[t], menu::order((t + 1) * 100))
                                            : menu::page(kTabIds[t], kTabLabels[t]);
        for (int i = 0; i < kCoreRows; i++) {
            const Row& r = core::rows[i];
            if (r.tab != t) continue;
            if (r.heading) {
                const char* id = core_key(r);
                core_headings()[id] = i;
                b.heading(id, r.name);
                continue;
            }
            menu::Setting st;
            st.key = core_key(r);
            st.label = r.name;
            st.help = r.help;
            st.kind = r.value ? menu::Kind::Choice : bespoke_row(r) ? menu::Kind::Custom : menu::Kind::Action;
            st.value = r.value;
            st.lo = r.lo, st.hi = r.hi, st.step = r.step;
            st.restart = r.restart;
            st.preset_exempt = r.preset_exempt;
            st.tag = &r;
            b.row(std::move(st));
        }
    }
}
// (a module's row: its text and hint as this file's rows have them, function pointers, one pair a slot)
const menu::Setting* module_settings[kModuleRowsMax];
const char* module_text(int slot, int v) {
    const menu::Setting& s = *module_settings[slot];
    // (a few at once: a caller may hold the lo and the hi ends' texts. Each thread its own: the menu's drawing and the
    // control bridge's thread, bridge_fill, both ask)
    static thread_local std::string said[kModuleRowsMax][4];
    static thread_local int next[kModuleRowsMax];
    if (!s.text && !s.choices.empty()) return s.choices[std::clamp(v - s.lo, 0, (int)s.choices.size() - 1)];
    if (!s.text && s.kind == menu::Kind::Toggle) return v ? "On" : "Off";
    std::string& out = said[slot][next[slot]++ & 3];
    out = s.text ? s.text(v) : std::to_string(v);
    return out.c_str();
}
const char* module_hint(int slot, int v, int variant) {
    const menu::Setting& s = *module_settings[slot];
    return s.hint ? s.hint(v, variant) : nullptr;
}
template <int I> const char* module_text_at(int v) { return module_text(I, v); }
template <int I> const char* module_hint_at(int v, int variant) { return module_hint(I, v, variant); }
template <int... I> constexpr auto module_texts(std::integer_sequence<int, I...>) {
    return std::array<const char* (*)(int), sizeof...(I)>{module_text_at<I>...};
}
template <int... I> constexpr auto module_hints(std::integer_sequence<int, I...>) {
    return std::array<const char* (*)(int, int), sizeof...(I)>{module_hint_at<I>...};
}
std::vector<int>* tab_orders() {
    static std::vector<int> order[TAB_PAGE + 1];
    return order;
}
std::vector<Row>& row_table() {
    static std::vector<Row> table;
    static std::once_flag once;
    std::call_once(once, [] {
        static constexpr auto texts = module_texts(std::make_integer_sequence<int, kModuleRowsMax>{});
        static constexpr auto hints = module_hints(std::make_integer_sequence<int, kModuleRowsMax>{});
        menu::set_log([](const char* line) { logf("menu: %s", line); });
        menu::build();
        table.reserve(kMaxRows);
        table.assign(std::begin(core::rows), std::end(core::rows));
        int slots = 0;
        for (int t = 0; t <= TAB_PAGE; t++) {
            std::vector<int>& order = tab_orders()[t];
            for (const menu::LayoutItem& it : menu::layout(kTabIds[t])) {
                const Row* own = it.setting ? (const Row*)it.setting->tag : nullptr;
                if (own >= core::rows && own < core::rows + kCoreRows) {
                    order.push_back((int)(own - core::rows));
                    continue;
                }
                if (it.heading) {
                    const auto found = core_headings().find(it.key);
                    if (found != core_headings().end()) {
                        order.push_back(found->second);
                        continue;
                    }
                }
                if (it.heading && !it.label) continue;   // (a section with no title)
                const menu::Setting* st = it.setting;
                if (st && st->kind != menu::Kind::Choice && st->kind != menu::Kind::Toggle && st->kind != menu::Kind::Slider &&
                    st->kind != menu::Kind::Action && !(st->kind == menu::Kind::Page && page_tab(st->opens) > TAB_COUNT - 1)) {
                    logf("menu: %s: a %s row, which the Insert menu doesn't draw yet: left out", it.key ? it.key : "(no key)",
                         st->kind == menu::Kind::Page ? "page (one the menu has no tab for)" : "line or custom");
                    continue;
                }
                if (slots == kModuleRowsMax || (int)table.size() == kMaxRows) {   // (a heading takes a row, not a slot)
                    logf("menu: %s: more than %d rows from modules: left out", it.key ? it.key : "(no key)", kModuleRowsMax);
                    continue;
                }
                Row r{(MenuTab)t, it.label, nullptr, 0, 0, 0, nullptr, nullptr};
                if (it.heading) {
                    r.heading = true;
                } else {
                    module_settings[slots] = st;
                    r.value = st->value;
                    r.lo = st->lo, r.hi = st->hi, r.step = st->step;
                    r.text = texts[slots];
                    r.help = st->help;
                    r.hint = st->hint ? hints[slots] : nullptr;
                    r.restart = st->restart;
                    r.preset_exempt = st->preset_exempt;
                    r.setting = st;
                    slots++;
                }
                order.push_back((int)table.size());
                table.push_back(r);
            }
            // (this file's rows keep their own order: only a place() could move them, and none does)
            int last = -1;
            for (int i : order)
                if (i < kCoreRows) {
                    if (i < last) logf("menu: %s: this file's rows out of their order (%s after %s)", kTabIds[t],
                                       core::rows[i].name, core::rows[last].name);
                    last = i;
                }
        }
    });
    return table;
}
/// The MenuTab of a page by its id (kTabIds), -1 none
int page_tab(const char* id) {
    for (int t = 0; id && t <= TAB_PAGE; t++)
        if (!strcmp(kTabIds[t], id)) return t;
    return -1;
}
/// A module's row with this value, locked now by its own locked() (row_fixed asks: its cases are by value)
bool module_locked(const int* value) {
    if (!value) return false;
    for (const Row& r : row_table())
        if (r.setting && r.value == value && r.setting->locked && r.setting->locked()) return true;
    return false;
}
/// The page a module's row opens (Kind::Page): its MenuTab, -1 none
int row_opens(const Row& r) {
    return r.setting && r.setting->kind == menu::Kind::Page ? page_tab(r.setting->opens) : -1;
}
/// The rows of a tab (or a page a row opens), in order: their indices in rows
const std::vector<int>& row_order(int t) {
    row_table();
    return tab_orders()[t];
}
/// rows: this file's rows and the modules', as an array (rows[i], for (const Row& r : rows), std::size(rows))
struct RowTable {
    Row& operator[](int i) const { return row_table()[(size_t)i]; }
    Row* begin() const { return row_table().data(); }
    Row* end() const { return row_table().data() + row_table().size(); }
    size_t size() const { return row_table().size(); }
};
RowTable rows;
// ---- end of the rows as drawn ----
const Row* pending = nullptr;   // (Restart or Quit: shows Confirm until its second press)
bool cheats_env = false;   // (HOS_OOT_CHEATS=1: on whatever the Debug tab says)
/// The debug tools: the Debug tab's row, or HOS_OOT_CHEATS=1
bool cheats_on() { return cheats_env || opts.debug_tools; }
std::atomic<bool> transition_autosave_enabled{true};
std::atomic<int> grass_distance_now{0};
void sync_transition_autosave() {
    transition_autosave_enabled.store(opts.transition_autosave != 0, std::memory_order_relaxed);
}
std::atomic<int> protect_hearts{-1};
// -1 disables the shortcut, 0 enables it on the ground, and 1 means flying.
std::atomic<int> player_flight{-1};
std::atomic<int> free_camera{0};
std::atomic<int> cheat_action{0};
std::atomic<int> link_age{-1};
// (Movement speed: the choice, an index of link_speeds; the game side reads it in percent, HOS_OPT_CHEAT_SPEED)
constexpr int link_speeds[] = {100, 125, 150, 200, 300};
constexpr const char* link_speed_names[] = {"1x", "1.25x", "1.5x", "2x", "3x"};
constexpr int kLinkSpeeds = sizeof link_speeds / sizeof link_speeds[0];
std::atomic<int> link_speed{0};   // (the game side's report, OOT_CHEAT_AGE_ID: 0 child, 1 adult, -1 no save loaded)
int clock_hour = 12;
int pouch_item = 0;
int pouch_delta = 1;
constexpr const char* pouch_items[] = {"Deku Seeds", "Deku Nuts", "Deku Sticks", "Bombs", "Arrows"};
constexpr int kPouchItems = sizeof pouch_items / sizeof pouch_items[0];
// (teleport list: generated by work/oot-teleport-tools/write_destinations.py)
// (the order of oot_destinations in host/hos_main.c; Enter sends 1000 + the index)
constexpr const char* teleport_names[] = {
    "Kokiri Forest",
    "Hyrule Field",
    "Kakariko Village",
    "Market",
    "Hyrule Castle",
    "Lost Woods",
    "Sacred Forest Meadow",
    "Lake Hylia",
    "Graveyard",
    "Death Mountain Trail",
    "Death Mountain Crater",
    "Goron City",
    "Zora's River",
    "Zora's Domain",
    "Zora's Fountain",
    "Gerudo Valley",
    "Gerudo Fortress",
    "Haunted Wasteland",
    "Desert Colossus",
    "Inside the Deku Tree",
    "Dodongo's Cavern",
    "Jabu-Jabu's Belly",
    "Forest Temple",
    "Fire Temple",
    "Water Temple",
    "Shadow Temple",
    "Spirit Temple",
    "Bottom of the Well",
    "Ice Cavern",
    "Gerudo Training Ground",
    "Ganon's Castle"};
constexpr const char* teleport_notes[] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "adult only", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
struct TeleportSection { const char* name; int first; };
constexpr TeleportSection teleport_sections[] = {{"Overworld", 0}, {"Dungeons", 19}};
constexpr int kTeleportSections = sizeof teleport_sections / sizeof teleport_sections[0];
// (end of the generated teleport list)
constexpr int kTeleports = sizeof teleport_names / sizeof teleport_names[0];
int teleport_choice = 0;
int teleport_columns = 1, teleport_page_rows = 1;   // (as last drawn: the cards in a row, the rows that fit: Page Up / Down)
float teleport_scroll_px = 0, teleport_want_px = 0;   // (the cards' scroll, eased toward teleport_want_px)
bool teleport_follow = true;   // (the scroll follows the choice; the wheel lets go of it until the choice moves)
int teleport_wheel = 0;        // (the wheel's notches since the last drawing: it scrolls by them)
int teleport_section_of(int i) {
    int section = 0;
    while (section + 1 < kTeleportSections && i >= teleport_sections[section + 1].first) ++section;
    return section;
}
int teleport_section_end(int section) {
    return section + 1 < kTeleportSections ? teleport_sections[section + 1].first : kTeleports;
}
/// The destination rows rows down (up when negative) from i, in the grid the Teleport page draws: each section's cards
/// teleport_columns to a row, under the section's heading; the column kept where the next row has it
int teleport_step_rows(int i, int rows) {
    const int cols = std::max(1, teleport_columns);
    for (; rows > 0; --rows) {
        const int section = teleport_section_of(i), first = teleport_sections[section].first, end = teleport_section_end(section);
        const int p = i - first;
        if (p + cols < end - first) i += cols;
        else if (p / cols < (end - first - 1) / cols) i = end - 1;   // (the section's last row, a short one)
        else if (section + 1 < kTeleportSections) i = std::min(end + p % cols, teleport_section_end(section + 1) - 1);
        else break;
    }
    for (; rows < 0; ++rows) {
        const int section = teleport_section_of(i), first = teleport_sections[section].first, p = i - first;
        if (p >= cols) i -= cols;
        else if (section > 0) {
            const int prev = teleport_sections[section - 1].first, n = first - prev;
            i = std::min(prev + (n - 1) / cols * cols + p, first - 1);
        } else break;
    }
    return i;
}
// the Inventory page (gpu/vk/oot_cheat_items.h): items and their section headings, counts from the game side
#define OOT_MENU_ITEM_LABEL(label, name, kind) label,
#define OOT_MENU_ITEM_KIND(label, name, kind) kind,
#define OOT_MENU_NO_SECTION(name)
constexpr const char* item_names[] = {OOT_CHEAT_ITEMS(OOT_MENU_NO_SECTION, OOT_MENU_ITEM_LABEL)};
constexpr unsigned char item_kinds[] = {OOT_CHEAT_ITEMS(OOT_MENU_NO_SECTION, OOT_MENU_ITEM_KIND)};
constexpr int kItems = sizeof item_names / sizeof item_names[0];
// (each section's first item: a counter that a section heading reads and an item bumps)
constexpr int item_section_count() {
    int n = 0;
#define OOT_MENU_COUNT_SECTION(title) n++;
#define OOT_MENU_SKIP_ITEM(label, name, kind)
    OOT_CHEAT_ITEMS(OOT_MENU_COUNT_SECTION, OOT_MENU_SKIP_ITEM)
    return n;
}
constexpr int kItemSections = item_section_count();
struct ItemSections { TeleportSection s[kItemSections]; };
constexpr ItemSections make_item_sections() {
    ItemSections r{};
    int section = 0, item = 0;
#define OOT_MENU_ADD_SECTION(title) r.s[section].name = title, r.s[section].first = item, section++;
#define OOT_MENU_ADD_ITEM(label, name, kind) item++;
    OOT_CHEAT_ITEMS(OOT_MENU_ADD_SECTION, OOT_MENU_ADD_ITEM)
    return r;
}
constexpr ItemSections item_sections = make_item_sections();
std::atomic<int> item_counts[kItems];   // (-1 until the game side reports: OOT_CHEAT_ITEM_BASE + i)
int item_choice = 0;
int inventory_columns = 1, inventory_capacity = 1;   // (as last drawn: the cards in a row, on a page)
int inventory_filter = 0;   // (All, then the item sections)
int inventory_page = 0;     // (the page shown: the choice's, or another the wheel turned to)
bool inventory_amount = false;   // (Left and Right change the chosen item's amount)
int inventory_begin() { return inventory_filter ? item_sections.s[inventory_filter - 1].first : 0; }
int inventory_end() { return inventory_filter && inventory_filter < kItemSections ? item_sections.s[inventory_filter].first : kItems; }
void inventory_reveal() { inventory_page = (item_choice - inventory_begin()) / std::max(1, inventory_capacity); }
void choose_inventory_filter(int filter) {
    inventory_filter = filter;
    item_choice = inventory_begin();
    inventory_amount = false;
    inventory_reveal();
}
constexpr int kCheatRows = 12;   // (the debug tools' rows: Row::cheat 0 to 11)
/// A debug tool's value as its row shows it ("" an action: confirm does it)
std::string cheat_value(int i) {
    char t[16];
    switch (i) {
    case 0: return tx_onoff(protect_hearts.load() == 1);
    case 1: return tx_onoff(player_flight.load() >= 0);
    case 4: return snprintf(t, sizeof t, "%02d:00", clock_hour), t;
    case 5: return pouch_items[pouch_item];
    case 6: return snprintf(t, sizeof t, "%+d", pouch_delta), t;
    case 7: return tx_onoff(free_camera.load());
    case 11: return link_speed_names[std::clamp(link_speed.load(), 0, kLinkSpeeds - 1)];
    default: return "";
    }
}
/// The tools that choose with Left and Right (arrows at their value's ends): Set time, Inventory item, Change quantity,
/// Movement speed
bool cheat_chooses(int i) { return i == 4 || i == 5 || i == 6 || i == 11; }
/// Switch age as Kid | Adult, Link's age lit: once the game side has said it (a save loaded)
bool age_shown(int i) { return i == 9 && link_age.load(std::memory_order_relaxed) >= 0; }
/// What confirm does on a tool's row, for its prompt (nullptr: nothing)
const char* cheat_confirm(int i) {
    static const char* const says[kCheatRows] = {"Toggle", "Toggle", "Apply", "Apply", "Set", nullptr, "Apply", "Toggle", "Open list",
                                                 "Switch", "Open", nullptr};
    return i >= 0 && i < kCheatRows ? says[i] : nullptr;
}
/// The button label on an action tool's row ("": not a button action)
const char* cheat_button_text(int i) {
    static const char* const buttons[kCheatRows] = {"", "", "Mark", "Return", "", "", "", "", "Choose map", "Switch", "Edit items", ""};
    return i >= 0 && i < kCheatRows ? buttons[i] : "";
}

int ultrawide_read = 1;   // (ultrawide= as read: saved again while HOS_OUTPUT_RES holds the Output resolution and the row is as it was)
/// (longshot) The Aspect ratio and Output resolution rows' ends from the screen's sizes and the player's own (known once the
/// options and HOS_OUTPUT_RES are read), and the Aspect ratio of the size chosen: a size of a shape of its own is that shape,
/// a 16:9 one is the monitor's shape or 16:9, as ultrawide= says (menu_init, before restart_init: what the run starts with)
void outputs_sync() {
    const std::vector<dm::Shape> shapes = output_shapes();
    for (Row& r : rows) {
        if (r.value == &opts.output_res) r.hi = last_choice();
        if (r.value == &opts.ultrawide) r.hi = aspect_last();
    }
    ultrawide_read = std::clamp(opts.ultrawide, 0, 1);
    opts.ultrawide = aspect_started = aspect_of_choice(opts.output_res, ultrawide_read);
    output_started = opts.output_res;
    std::string list;
    for (const dm::Shape& s : shapes) list += (list.empty() ? "" : ", ") + s.name;
    logf("vk: the Output resolution's sizes: %d (%s, with the common sizes of their shapes, and the game's own)%s; shapes %s",
         custom_index(), getenv("HOS_DISPLAY_MODES") ? "HOS_DISPLAY_MODES" : "the screen's modes",
         custom_output.w ? ", and one of the player's own" : "", list.c_str());
}

int cur_tab = TAB_GRAPHICS;
int tab_selected[TAB_SIM_TURF + 1] = {};   // (and the pages a row opens: Motion, Controller speaker, FPS details, recordings and Simulated turf)
bool is_motion_row(const Row& r) {
    return row_opens(r) == TAB_MOTION || (r.tab == TAB_CONTROLS && !r.value && !r.setting && !strcmp(r.name, "Motion"));
}
bool is_speaker_row(const Row& r) { return row_opens(r) == TAB_SPEAKER; }   // (longshot: the Audio tab's Controller speaker)
bool is_fps_dev_row(const Row& r) {
    return row_opens(r) == TAB_FPS_DEV || (r.tab == TAB_GAMEPLAY && !r.value && !r.setting && !strcmp(r.name, "FPS counter details"));
}
bool is_recordings_row(const Row& r) { return row_opens(r) == TAB_RECORDINGS; }
bool is_sim_turf_row(const Row& r) { return row_opens(r) == TAB_SIM_TURF; }
/// (the page a page of the menu goes back to: Motion to Controls, FPS details to Gameplay, recordings to Debug and simulated
/// turf settings to Graphics; -1 a tab of the strip)
int page_parent(int tab) {
    return tab == TAB_MOTION       ? TAB_CONTROLS
         : tab == TAB_SPEAKER      ? TAB_AUDIO
         : tab == TAB_FPS_DEV      ? TAB_GAMEPLAY
         : tab == TAB_RECORDINGS   ? TAB_DEBUG
         : tab == TAB_SIM_TURF     ? TAB_GRAPHICS
                                   : -1;
}

// ---- the rows that take a restart: their values at the start (menu_init, the GPU's at menu_set_gpu_count) ----
namespace mr = menu_restart;
mr::State restart_state;
int mods_slot = -1;   // (hookshot: the Mods tab's slot in restart_state: its order and switches)
bool can_mods();      // (the Mods tab, below: whether the host has mods' functions)
int restart_slot[kMaxRows];          // (a row's slot in restart_state, -1 none)
const char* restart_names[mr::kMax];
/// (longshot) A row's value as a restart compares it: the Output resolution's size itself, so another size of the player's own
/// (Custom output size), on the same choice, is a change too. Mono and Stereo share a channel layout and change live.
int row_restart_value(const Row& r) {
    if (r.setting && r.setting->restart_value) return r.setting->restart_value(*r.value);   // (#1633: a module's row's own)
    if (r.value != &opts.output_res) return *r.value;
    const Out o = output_at(*r.value);
    return (int)(o.w << 16 | o.h);
}
void restart_init() {
    static bool done = false;
    if (done) return;
    done = true;
    opts.game_lang = i18n::game_lang_index();   // (language.txt's, the language the game started in)
    for (int i = 0; i < (int)std::size(rows); i++) {
        restart_slot[i] = rows[i].restart && rows[i].value ? restart_state.add(row_restart_value(rows[i])) : -1;
        if (restart_slot[i] >= 0) restart_names[restart_slot[i]] = rows[i].name;
    }
    mods_slot = restart_state.add(0);   // (hookshot: the Mods tab's order and switches, 0 as the game started)
    if (mods_slot >= 0) restart_names[mods_slot] = "the mods";
}
int restart_slot_of(const Row& r) { return restart_slot[&r - &rows[0]]; }
/// A row that takes a restart, changed from what the game started with (a row past menu_restart.h's kMax has no slot: never)
bool restart_changed(const Row& r) {
    const int slot = restart_slot_of(r);
    return r.value && r.restart && slot >= 0 && row_restart_value(r) != restart_state.start[slot];
}
void restart_values(int* now) {
    for (int i = 0; i < (int)std::size(rows); i++)
        if (restart_slot[i] >= 0) now[restart_slot[i]] = row_restart_value(rows[i]);
    if (mods_slot >= 0) {   // (the mods' order and switches: 0 while they are the start's, else a number for them)
        const uint64_t k = can_mods() ? host->mods_changed() : 0;
        now[mods_slot] = (int)(k ^ k >> 32) | (k ? 1 : 0);
    }
}
/// The rows whose value is not the one the game runs with (a bit a slot)
unsigned restart_pending() {
    int now[mr::kMax];
    restart_values(now);
    return restart_state.pending(now);
}
/// Whether closing the menu now asks to restart: something pending, not declined with Later, and a window to close
bool restart_ask() {
    int now[mr::kMax];
    restart_values(now);
    return app_window() && restart_state.ask(now);
}
/// The rows' names in the menu's language, for the restart line and question
const char* const* tr_names() {
    static const char* t[mr::kMax];
    for (int i = 0; i < restart_state.n; i++) t[i] = i18n::tr(restart_names[i]);
    return t;
}
std::string restart_list(const char* last) {
    return mr::names(tr_names(), restart_state.n, restart_pending(), ", ", last);
}
int ask_sel = 0;   // (the safe answer is first in both confirmation dialogs)
int output_preview_countdown{};
std::mutex options_save_mu;

/// A row on its tab: the debug tools only with them on; a row for what this PC cannot run not at all (Reflex and Anti-Lag 2
/// only where the driver has them, the Upscaler only where more than the game's own runs, the DLSS model only where DLSS
/// runs); HDR's paper white, peak brightness and contrast only with HDR on. A row that comes or goes moves those after it: the
/// selection is a place on its tab (tab_selected), so a row hidden after the selected one leaves the selection where it is,
/// and menu_tick brings back one past the end
bool row_shown(int i) {
    const int* v = rows[i].value;
    return ((rows[i].cheat < 0 && v != &opts.damage_taken) || cheats_on()) &&
           // (longshot) who makes the frames only with Frame generation on; the CRT raster only with a CRT filter
           (v != &opts.framegen_by || opts.framegen != 0) &&
           // (#1633) a module's row: its own shown()
           (!rows[i].setting || !rows[i].setting->shown || rows[i].setting->shown());
}
int tab_row_count(int t) {
    int c = 0;
    for (int i : row_order(t)) if (row_shown(i)) c++;
    return c;
}

int tab_row_to_global(int t, int local_idx) {
    int count = 0;
    for (int i : row_order(t)) {
        if (row_shown(i)) {
            if (count == local_idx) return i;
            count++;
        }
    }
    return 0;
}

std::atomic<bool> open_now{false};
bool tune_mode = false;
bool quick_debug = false;
bool menu_refresh = false;
// (a value too long for its row, a GPU's name: the selected row's slides through it, a character a step, resting at
// each end; the other rows end it with "...". The drawing (fit) says how far it can slide, menu_tick moves it)
int marquee_pos = 0, marquee_max = 0;
int64_t marquee_next = 0;
constexpr int64_t kMarqueeStep = 150000000LL, kMarqueeRest = 1200000000LL;   // (ns)
// (longshot) the mouse (menu_tick): the tab strip's part under the cursor (a tab 0 to TAB_COUNT - 1, TAB_COUNT the previous,
// TAB_COUNT + 1 the next; -1 none); the slider held with the left button (its row in the tab, -1 none)
int hover_tab = -1;
int drag_row = -1;
bool mouse_read = false;   // (the menu has read the mouse since it opened)
bool is_slider(const Row& r) { return (r.setting && r.setting->kind == menu::Kind::Slider); }
/// (a scale: the arrows and a click stop at its ends; Rumble strength would go from Off to its strongest)
// (a Slider unless it wraps, as Sharpness; a Choice that says so: tests/test_menu_slider_ends.py. One line: checks read it so)
bool stops_at_ends(const Row& r) { return r.setting && ((r.setting->kind == menu::Kind::Slider && !r.setting->wraps) || r.setting->stops_at_ends); }
/// (the ini's Vibration strength, rumble_strength=, on the menu's steps: 25 to 200, the nearest 25. 0 was Off before the
/// Vibration row, which menu_init turns into Vibration Off)
int rumble_step(int v) { return (std::min(std::max(v, 25), 200) + 12) / 25 * 25; }
// (longshot) A run's environment over a saved row (RX_VK_PACE=1, HOS_OUTPUT_RES=...): for that run only. save() writes the
// player's own value while the row still holds the environment's; once the player changes the row, theirs is kept. (These
// were copied into the row and the next save wrote them into longshot_options.ini, so a test run's switch stayed for good)
struct EnvOverride {
    int* value;
    int saved, forced;
};
EnvOverride env_overrides[16];
int env_override_count = 0;
void env_override(int* value, int v) {
    for (int i = 0; i < env_override_count; i++)
        if (env_overrides[i].value == value) {   // (the same row twice: the player's value is the first one)
            *value = env_overrides[i].forced = v;
            return;
        }
    if (env_override_count < (int)(sizeof env_overrides / sizeof env_overrides[0]))
        env_overrides[env_override_count++] = {value, *value, v};
    *value = v;
}
/// The value save() writes for a row: the player's, not the environment's
int saved_value(int* value) {
    for (int i = 0; i < env_override_count; i++) {
        EnvOverride& o = env_overrides[i];
        if (o.value != value) continue;
        if (*value == o.forced) return o.saved;
        o.value = nullptr;   // (changed in the menu: the player's choice from now on)
    }
    return *value;
}
} // namespace   // (#1633: asked by the modules too, menu_module.h's "what modules ask the renderer")
/// The menu set the row (the player or a preset): its value is saved from now on, even where it is the
/// one the environment forced
void env_release(const int* value) {
    for (int i = 0; i < env_override_count; i++)
        if (env_overrides[i].value == value) env_overrides[i].value = nullptr;
}
namespace {   // (#1633: the anonymous namespace again, after what the modules ask)
/// Whether the environment still holds the row (saved_value has not seen the player change it)
bool env_holds(const int* value) {
    for (int i = 0; i < env_override_count; i++)
        if (env_overrides[i].value == value) return true;
    return false;
}
Out player_custom_output{0, 0};   // (the player's own Output resolution size when HOS_OUTPUT_RES names another of its own)
std::string ini_path() { return options_path(); }   // (the user folder's when --user names one: vk_paths.h)
/// A row's line in longshot_options.ini (and its key over the control bridge, option_get and option_set). (longshot, #1633)
/// Every saved row is a module's, its key its own (Setting.key), as players' files hold it: tests/test_menu_ini_keys.py
/// keeps them as they are, so renaming a row never moves its setting. A row with none (a check's own rows) gets the key
/// made from its label, as before.
const char* ini_key(const Row& r) {
    if (r.setting && r.setting->key) return r.setting->key;   // (#1633: a module's row: its own key)
    if (r.value == &monitor_pick) return "fullscreen_monitor";
    if (r.value == &opts.output_res) return "output_res";   // (host/nn_applet.c reads it at the start)
    if (r.value == &opts.vsync) return "vsync";
    // (rows renamed since their values were first saved keep their keys: an options file from before still loads)
    if (r.value == &opts.audio_output) return "audio_output";
    if (r.value == &opts.resolution) return "resolution";
    if (r.value == &opts.fps_cap) return "frame_rate_cap";   // (host/nn_vi.c reads it at the start, then HOS_OPT_FPS_CAP)
    if (r.value == &opts.upscale) return "upscaling";
    if (r.value == &opts.dlss) return "dlss";   // (as opts.dlss and RX_VK_DLSS)
    if (r.value == &opts.dlss_pre_bloom) return "dlss_pre_bloom";   // (as RX_VK_DLSS_PRE_BLOOM)
    if (r.value == &opts.handheld) return "handheld_profile";   // (the Profile row; host/nn_applet.c reads it at the start)
    if (r.value == &opts.ultrawide) return "ultrawide";   // (the Aspect ratio row; host/hos_main.c reads it at the start)
    // (longshot) Vibration (host/vibration.h), and its strength under the key it had as Rumble strength
    if (r.value == &opts.vibration) return "vibration";
    if (r.value == &opts.rumble) return "rumble_strength";
    static char k[48];
    size_t n = 0;
    for (const char* p = r.name; *p && n + 1 < sizeof k; p++) k[n++] = *p == ' ' ? '_' : (char)tolower(*p);
    k[n] = 0;
    return k;
}
// (longshot) the controls, kept by the host (host/nn_hid.c reads them back from here): pad_<button>, key_<button>
const char* const bind_names[HOS_KEY_N] = {"a", "b", "x", "y", "ls", "rs", "l", "r", "zl", "zr", "plus", "minus",
                                           "left", "up", "right", "down", "move_up", "move_down", "move_left",
                                           "move_right", "cam_up", "cam_down", "cam_left", "cam_right",
                                           "note_l", "note_r", "note_a", "note_y", "note_x", "walk", "toggle_hud",
                                           "sprint"};
bool toggle_on(int button);   // (the Controls page's Toggle column: below, with the page)
void save() {
    // (one writer at a time: the present's thread, the game's (menu_option_set) and the control bridge's, menu_setting_set)
    std::lock_guard lk(options_save_mu);
    // (longshot) Nothing written by a --prepare-shaders run (the Builder's shader preparation, LONGSHOT_PREPARE_ONLY). It has
    // no window, so the screen is 0 x 0 to it: its first start chose Auto (this PC) for that screen and saved the Output
    // resolution as 2560x1440, which every start then took for the player's choice. A 1920 x 1080 AYN Odin 3 drew at 1440p
    // and scaled down (2026-11-19). The game's own first start chooses with the screen it shows on, and saves then
    static const bool preparing = env_on("LONGSHOT_PREPARE_ONLY");
    if (preparing) {
        static bool told = false;   // (under options_save_mu)
        if (!told)
            logf("vk: options: not saved by the shader preparation (no screen to choose for): the game's first start chooses");
        told = true;
        return;
    }
    // (longshot) the lines that are not the menu's are kept: the controls, pad_ and key_ (host/nn_hid.c), and their toggle_ flags
    // (and every other line the menu does not write, such as discord_app_id=: they were dropped at the next save)
    std::string kept, others;
    if (FILE* f = fopen(ini_path().c_str(), "r")) {
        char line[4096];
        while (fgets(line, sizeof line, f)) {
            if (!strchr(line, '\n') && !feof(f)) {
                int c; while ((c = fgetc(f)) != '\n' && c != EOF) {}
                continue;
            }
            if (!strncmp(line, "pad_", 4) || !strncmp(line, "key_", 4) || !strncmp(line, "toggle_", 7)) {
                kept += line;
                continue;
            }
            const char* eq = strchr(line, '=');
            if (!eq || eq == line) continue;
            const std::string key(line, (size_t)(eq - line));
            if (key == "sprint_auto_bind" && !can_bind()) {
                kept += line;
                continue;
            }
            // (memory_budget and auto_for too: save() writes them, and the file's old line, kept after it, won at the next start;
            // auto_output, the Output resolution Auto once set, is no longer written: its old line is dropped)
            bool mine = key == "audio_output_device_id" || key == "audio_output_device_name" || key == "output_device" ||
                        key == "microphone_device" || key == "options_version" || key == "memory_budget" || key == "auto_for" ||
                        key == "auto_tuned" || key == "auto_output" || key == "auto_steps";
            mine |= key == "sprint_auto_bind";
            mine |= key == "fullscreen_monitor_id";
            mine |= key == "fullscreen_monitor_name";
            // (and the Mesh shaders row's line, no longer read (gpu/vk/mesh_mode.h): dropped, not kept for nothing)
            mine |= key == "mesh_shaders";
            // (and the VRAM budget row's, removed: one rule by the card, texture_budget.h and mirror_budget.h)
            mine |= key == "vram_budget";
            mine |= key == "gpu_device_id";   // (the GPU device row's card: written below)
            for (const Row& r : rows)
                if (!mine && r.value && key == ini_key(r)) mine = true;
            if (!mine) others += line, others += others.back() == '\n' ? "" : "\n";
        }
        fclose(f);
    }
    const auto write_lines = [&](FILE* f) {
        for (const Row& r : rows) {
            if (r.value == &opts.output_res) {   // (the size itself: host/nn_applet.c reads it)
                const int v = saved_value(r.value);
                const Out o = v == custom_index() && player_custom_output.w && env_holds(r.value) ? player_custom_output : output_at(v);
                fprintf(f, "%s=%ux%u\n", ini_key(r), o.w, o.h);
            } else if (r.value == &opts.ultrawide) {   // (the Aspect ratio: 1 the monitor's own shape, else 0; output_res holds the rest)
                // (a run whose Output resolution HOS_OUTPUT_RES holds took the row from the environment's size: the player's own
                // ultrawide= is kept, unless the row was changed in the menu)
                const int v = saved_value(r.value);
                fprintf(f, "%s=%d\n", ini_key(r), env_holds(&opts.output_res) && v == aspect_started ? ultrawide_read : aspect_follows_host(v) ? 1 : 0);
            } else if (r.value && r.value != &opts.game_lang && r.value != &output_pick && r.value != &monitor_pick) { // Device choices are saved by ID below.
                fprintf(f, "%s=%d\n", ini_key(r), saved_value(r.value));
            }
        }
        if (!mic_want.empty()) fprintf(f, "microphone_device=%s\n", mic_want.c_str());   // (longshot: also while it is unplugged)
        if (!output_want.empty()) {
            fprintf(f, "audio_output_device_id=%s\n", output_want.c_str());
            fprintf(f, "audio_output_device_name=%s\n", output_saved_name.c_str());
        }
        if (!monitor_want.empty()) {
            fprintf(f, "fullscreen_monitor_id=%s\n", monitor_want.c_str());
            fprintf(f, "fullscreen_monitor_name=%s\n", monitor_saved_name.c_str());
        }
        // (not a row: the preset Custom came from, whose mip trimming it keeps: opts.budget)
        fprintf(f, "memory_budget=%d\n", opts.budget);
        // (the GPU device row's card itself, not only its place in the list: gpu_choice.h. A saved card not here this run, the
        // row still at the Auto it started on, stays saved for when it is back)
        const std::string card = opts.gpu_device > 0 ? gpu_choice::card_id(gpu_device_cards, opts.gpu_device - 1)
                                 : opts.gpu_device == 0 && gpu_card_missing ? gpu_saved_card : std::string();
        if (!card.empty()) fprintf(f, "gpu_device_id=%s\n", card.c_str());
        if (auto_for) fprintf(f, "auto_for=%016llx\n", (unsigned long long)auto_for);   // (the hardware Auto (this PC) chose for)
        if (auto_tuned) fprintf(f, "auto_tuned=%016llx\n", (unsigned long long)auto_tuned);   // (and measured in play for)
        // (the tuning's steps so far and whether one went down: the no-seesaw rule and the two steps hold across restarts)
        if (auto_for && auto_tuning.steps) fprintf(f, "auto_steps=%d,%d\n", auto_tuning.steps, auto_tuning.went_down ? 1 : 0);
        // (2: game_speed below 2 was only ever the old default, see menu_init; 3: the mouse's buttons are bindings, see
        // host/nn_hid.c binds_load; 4: Descriptor reuse on by default, see menu_init and vk_draw.cpp desc_slotgen_on; 5: the
        // Menu shortcut "Insert, F1 or Esc" by default, see menu_init; 6: the Aspect ratio's 16:9 from before moves on a
        // screen narrower than 16:9, see menu_init and host/hos_wide_size.h; 7: Sprint's Toggle on by default, see
        // menu_init)
        fprintf(f, "options_version=8\n");
        fputs(others.c_str(), f);
        if (!can_bind()) fputs(kept.c_str(), f);
        else if (const int sprint_auto = host->binding(HOS_BIND_SPRINT_AUTO, 0, 0))
            fprintf(f, "sprint_auto_bind=%d\n", sprint_auto);
        for (int i = 0; i < HOS_KEY_N && can_bind(); i++) {   // (the stick's and camera's rows, HOS_KEY_MOVE_*/CAM_*, have no pad_ line; the ocarina note rows, the HUD's and Sprint do)
            if (i < HOS_SW_N || (i >= HOS_KEY_OC_NOTE_L && i <= HOS_KEY_OC_NOTE_X) || i == HOS_KEY_TOGGLE_HUD ||
                i == HOS_KEY_SPRINT) {
                const int alternate = host->binding(HOS_BIND_PAD, i, 1);
                fprintf(f, "pad_%s=%d", bind_names[i], host->binding(HOS_BIND_PAD, i, 0));
                if (alternate >= 0) fprintf(f, ",%d", alternate);
                fputc('\n', f);
            }
            if (i < HOS_SW_N || i == HOS_KEY_SPRINT) fprintf(f, "toggle_%s=%d\n", bind_names[i], toggle_on(i) ? 1 : 0);
            fprintf(f, "key_%s=%d,%d\n", bind_names[i], host->binding(HOS_BIND_KEY, i, 0), host->binding(HOS_BIND_KEY, i, 1));
        }
    };
    // (written beside its target, a link followed so that a synced file stays linked, as <name>.new, and moved over it once
    // whole: written in place, a full disk or a close mid-write left it cut short, and the next start took the defaults past
    // the cut, or, cut to nothing, chose Auto (this PC) over the player's preset. Where nothing can be made beside it or moved
    // over it (a folder that is not writable, the file held open by another program), in place as before. A read-only
    // file is kept. Each failure is logged once a run, with the file's whole path (the launcher's user folder's, --user, or the
    // game folder's: options_path): a save comes with every arrow press. No file this run (options_path empty, logged at the
    // start by host/options_path.c): nothing saved, and nothing made in the folder the game runs in)
    std::string path = ini_path();
    if (path.empty()) return;
    static bool told[5];
    const auto say_once = [](int k, const std::string& what) {
        if (!told[k]) told[k] = true, logf("menu: %s", what.c_str());
    };
#ifdef _WIN32
    const auto why = [] { return "Windows error " + std::to_string(GetLastError()); };
#else
    const auto why = [] { return std::string(strerror(errno)); };
#endif
    bool in_place = false;
#ifdef _WIN32
    const DWORD attrs = GetFileAttributesA(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)) {
        say_once(0, path + " is read-only: the settings are not saved");
        return;
    }
    in_place = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT);   // (a link: written through it)
#else
    if (char* real = realpath(path.c_str(), nullptr)) path = real, free(real);
    struct stat st{};
    const bool exists = stat(path.c_str(), &st) == 0;
    if (exists && access(path.c_str(), W_OK) != 0) {
        const std::string e = why();   // (taken before the text is put together: that may touch errno)
        say_once(0, path + " cannot be written (" + e + "): the settings are not saved");
        return;
    }
#endif
    const std::string part = path + ".new";
    if (!in_place) {
        FILE* f = fopen(part.c_str(), "w");
        if (!f) {
            const std::string e = why();
            say_once(1, part + " cannot be made beside it (" + e + "): " + path + " is written in place");
            in_place = true;
        } else {
#ifndef _WIN32
            if (exists) (void)fchmod(fileno(f), st.st_mode & 07777);   // (its permissions, not the umask's)
#endif
            write_lines(f);
            const bool whole = fflush(f) == 0 && !ferror(f);
            std::string failed = whole ? std::string() : why();
            if (fclose(f) != 0 && failed.empty()) failed = why();
            if (!failed.empty()) {   // (not whole: the old file stays as it was)
                remove(part.c_str());
                say_once(2, part + " could not be written whole (" + failed + "): " + path + " is kept as it was");
                return;
            }
#ifdef _WIN32
            if (MoveFileExA(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return;
#else
            if (rename(part.c_str(), path.c_str()) == 0) return;
#endif
            const std::string moved = why();
            remove(part.c_str());
            say_once(3, part + " could not be moved over it (" + moved + "): " + path + " is written in place");
            in_place = true;
        }
    }
    FILE* f = fopen(path.c_str(), "w");
    if (!f) {
        const std::string e = why();
        say_once(4, path + " could not be opened to write (" + e + "): the settings are not saved");
        return;
    }
    write_lines(f);
    const bool whole = fflush(f) == 0 && !ferror(f);
    std::string failed = whole ? std::string() : why();
    if (fclose(f) != 0 && failed.empty()) failed = why();
    if (!failed.empty()) say_once(4, path + " could not be written in place (" + failed + ")");
}

/// (longshot) The Button prompts row: a new face shows at once where the game has every part of the prompts from
/// host/prompts.c, nothing to restart for then; else, as the other restart rows, at the next start
void prompts_at_once(const Row& r) {
    if (r.value == &opts.prompts && host && host->prompts_live &&
        (host->prompts_live() & HOS_PROMPTS_LIVE_ALL) == HOS_PROMPTS_LIVE_ALL)
        restart_state.restart_value(restart_slot_of(r), *r.value);
}


// ---- (longshot) Save my pipeline list (the Debug tab, under Developer): longshot_pipeline_states.bin and the "Submit your pipelines" issue's
// template, filled in from this run, zipped into the Downloads folder (pipeline_share.h; the builders do the same with
// nx2/pipeline_share.py). Then the report on the clipboard, the folder shown and the issue opened in the browser, where the platform
// has them; never with HOS_HEADLESS (no screen to show them on). Nothing is uploaded: the player attaches the zip ----
// (longshot) Set up for a weak laptop (the Debug tab, integrated graphics: row_shown): Minimum's rows and mip trimming, then
// lighter still. An i3-1115G4's UHD Graphics (2 cores, 4 threads, 16 GB shared) ran Kokiri Forest and the Deku Tree at 3-6 fps
// with 120-200 ms of GPU time a frame on the game's own dynamic resolution, docked at 2048 x 1152
} // namespace   // (#1633: asked by the modules too, menu_module.h's "what modules ask the renderer")
bool laptop_applied = false;
void weak_laptop_apply() {
    apply_preset(1);   // (Minimum: its scaling rows, the handheld profile, its mip trimming on a small or shared GPU)
    env_release(&opts.dlss);
    opts.dlss = 0;   // (the game's own upscaler: no DLSS or FSR on these GPUs, and the lightest)
    opts.resolution = ur::snap_render(upscaler(), 6);   // (33%: auto_settings.h RES_ULTRA_PERFORMANCE)
    opts.upscale = 0, opts.sharpness = 0;   // (the plain window scale: as Auto's lightest tier, RX_VK_LOW_SPEC)
    for (int* v : {&opts.framegen, &opts.intel_compiles, &opts.intel_quick}) env_release(v);
    opts.framegen = 0;
    opts.intel_compiles = 2, opts.intel_quick = 1;   // (2: one at a time, whatever the row's Auto is on this GPU)
    opts.preset = 0;   // (Custom, Minimum's mip trimming kept: opts.budget)
    samplers_changed();   // (the upscalers' texture LOD bias follows the 3D's share)
    laptop_applied = true;
    menu_refresh = true;
    {   // (the handheld profile and Intel's shader compiles take a restart: asked at the next close, as for a row set by hand)
        int now[mr::kMax];
        restart_values(now);
        restart_state.note(now);
    }
    save();
    logf("menu: Set up for a weak laptop: Render resolution %d%%, the game's own upscaler, plain scale, handheld profile (after a "
         "restart), Minimum's mip trimming", ur::percent(opts.resolution));
}
namespace {   // (#1633: the anonymous namespace again, after what the modules ask)
bool debug_warning = false;
int debug_warning_choice = 0;

static bool request_native_preview(Out target, bool from_field = false);
void set_row(Row& r, int v, int dir, bool keep = true) {
    if (r.value == &opts.debug_tools && !opts.debug_tools && v > 0) {
        debug_warning = true;
        debug_warning_choice = 0;
        menu_refresh = true;
        return;
    }
    if (r.value == &output_pick && output_forced()) return;
    // (nothing else to choose: no change, no save. A saved choice that cannot run here is kept)
    if (row_fixed(r.value)) return;
    // (#1633) a module's row: stepped by its own step_fn
    // (an arrow's step alone, the value one step from the row's: a value set as it is, or a jump, is kept as asked)
    if (r.setting && r.setting->step_fn && dir && v == *r.value + dir * r.step) v = r.setting->step_fn(*r.value, dir);
    // (the scaling rows: only what can run here; the Render resolution row through the Upscaler's own steps)
    // (the Upscaler by engine; the Render resolution's Native is the engine's full-size form: upscaler_rows.h)
    if (r.value == &opts.dlss) v = ur::step_engine(upscaler(), dir, upscalers_available());
    if (r.value == &opts.resolution) {
        int up = upscaler(), res = *r.value;
        ur::step_render(upscaler(), ur::snap_render(upscaler(), *r.value), dir, &up, &res);
        if (up != upscaler()) opts.dlss = up;   // (Native in or out: DLAA / DLSS, FSR 4's native AA / FSR 4, XeSS's / XeSS)
        v = ur::full_scale(up) ? *r.value : res;   // (Native changes the mode, not the saved numeric scale)
    }
    if (r.value == &opts.output_res) {   // (smallest to largest: step_output_choice)
        v = step_output_choice(*r.value, dir);
        if (v == *r.value || (keep && request_native_preview(picture_for(v)))) return;
    }
    // (longshot) Frame generation by where DLSS FG cannot run: FSR 3.1 FG (0; a 2 is the same) and Built-in FG (1)
    if (r.value == &opts.framegen_by && !framegen_by_dlss_offered()) v = (v % 2 + 2) % 2;
    if (stops_at_ends(r)) v = std::min(std::max(v, r.lo), r.hi);   // (a scale: it stops at its ends)
    if (r.value == &opts.preset) {
        *r.value = v > r.hi ? r.lo : v < r.lo ? r.hi : v;
        apply_preset(*r.value);
    } else {
        *r.value = v > r.hi ? r.lo : v < r.lo ? r.hi : v;   // (Game language: written when the row is left, game_lang_tick)
        // (another shape: the Output resolution moves to one of its sizes, the screen's own when it has that shape)
        if (r.value == &opts.ultrawide) {
            const int out = output_for_aspect(*r.value);
            if (out != opts.output_res) opts.output_res = out, env_release(&opts.output_res);
        }
        if (!r.preset_exempt) {   // (rows no preset sets: the controls, the window, the counter...)
            // (Custom, still the preset it came from in opts.budget: Minimum's mip trimming on a small card stays with it)
            opts.preset = 0; // Custom
        }
    }
    if (r.setting && r.setting->on_change) r.setting->on_change(*r.value);   // (#1633: a module's row's own)
    if (r.value == &opts.transition_autosave) sync_transition_autosave();
    if (r.value == &opts.grass_distance) grass_distance_now.store(opts.grass_distance, std::memory_order_relaxed);
    // (longshot: the samplers made again, at once: their anisotropy, and the upscalers' LOD bias, vk_draw.cpp upscale_lod_bias)
    if (r.value == &opts.anisotropy || r.value == &opts.dlss || r.value == &opts.resolution || r.value == &opts.preset)
        samplers_changed();
    if (r.value == &opts.dlss || r.value == &opts.resolution || r.value == &opts.dlss_model)
        temporal_reset_failure();
    prompts_at_once(r);   // (longshot)
    {   // (a row that takes a restart, or a preset that sets one: asked again at the next close)
        int now[mr::kMax];
        restart_values(now);
        restart_state.note(now);
    }
    if (keep) save();
}

static bool request_native_preview(Out target, bool from_field) {
    if (output_preview_target.w || opts.handheld || env_holds(&opts.output_res) ||
        target.w != 1024 || target.h != 576) return false;
    // The preview needs an atomic save; linked settings keep the regular restart flow which writes through the link.
    const std::string path = ini_path();
    if (path.empty() || hos_af_is_link(path.c_str())) return false;
    const Out original = shown_output();
    if (original.w != 1280 || original.h != 720 || !output_preview_request(target.w, target.h)) return false;
    output_preview_target = target;
    output_preview_from_field = from_field;
    return true;
}

// Replace only the resolution setting. Decline on a failed write so the old output remains effective.
static bool save_preview_size(Out target) {
    std::lock_guard lk(options_save_mu);
    const std::string path = ini_path();
    if (path.empty() || hos_af_is_link(path.c_str())) return false;
    FILE* f = fopen(path.c_str(), "rb");
    std::string old;
    if (!f) {
        if (errno != ENOENT) return false;
    } else {
        char buffer[4096];
        while (const size_t n = fread(buffer, 1, sizeof buffer, f)) old.append(buffer, n);
        const bool read = !ferror(f);
        fclose(f);
        if (!read) return false;
    }
    const std::string value = "output_res=" + std::to_string(target.w) + "x" + std::to_string(target.h);
    std::string replaced;
    bool written = false;
    for (size_t from = 0; from < old.size();) {
        const size_t newline = old.find('\n', from);
        const size_t to = newline == std::string::npos ? old.size() : newline + 1;
        if (old.compare(from, 11, "output_res=") == 0) {
            replaced += value;
            if (newline != std::string::npos) replaced += newline > from && old[newline - 1] == '\r' ? "\r\n" : "\n";
            written = true;
        } else {
            replaced.append(old, from, to - from);
        }
        from = to;
    }
    if (!written) {
        if (!replaced.empty() && replaced.back() != '\n') replaced += '\n';
        replaced += value + "\n";
    }
    char temp[HOS_AF_PATH];
    if (!hos_af_name(temp, sizeof temp, path.c_str(), HOS_AF_NEW)) return false;
    const HANDLE out = hos_af_open_new(temp);
    if (out == INVALID_HANDLE_VALUE) return false;
    const bool wrote = hos_af_write_all(out, replaced.data(), replaced.size()) && FlushFileBuffers(out);
    const bool closed = CloseHandle(out);
    const bool moved = wrote && closed && hos_af_move(temp, path.c_str());
    if (!moved) DeleteFileA(temp);
    return moved;
}

extern int page;
void output_field_show();
static void answer_native_preview(bool keep) {
    const Out selected = output_preview_target;
    const bool accepted = keep && output_preview_answer(true);
    if (accepted && save_preview_size(selected)) {
        opts.output_res = output_started = output_choice_of(selected.w, selected.h);
        for (Row& r : rows)
            if (r.value == &opts.output_res) {
                r.hi = last_choice();
                const int index = restart_slot_of(r);
                if (index >= 0) restart_state.restart_value(index, row_restart_value(r));
            }
        output_field_show();
        int values[mr::kMax];
        restart_values(values);
        restart_state.note(values);
    } else {
        if (accepted) logf("menu: could not save the new output resolution; restoring the previous size");
        output_preview_answer(false);
    }
    output_preview_target = {};
    output_preview_countdown = 0;
    page = 0;
    menu_refresh = true;
}

// A preview refused before activation falls back to the normal restart setting.
static void apply_custom_output(unsigned w, unsigned h, bool has_size);
static void unavailable_native_preview() {
    const Out selected = output_preview_target;
    const bool from_field = output_preview_from_field;
    answer_native_preview(false);
    if (from_field) apply_custom_output(selected.w, selected.h, true);
    else {
        opts.output_res = output_choice_of(selected.w, selected.h);
        for (Row& r : rows)
            if (r.value == &opts.output_res) r.hi = last_choice();
        output_field_show();
        env_release(&opts.output_res);
        int values[mr::kMax];
        restart_values(values);
        restart_state.note(values);
        save();
    }
    if (restart_ask()) {
        page = 5;
        ask_sel = 0;
        open_now = true;
    }
    menu_refresh = true;
}

// ---- (longshot) the rows that are text fields (text_edit): their fields, what Enter on each sets, the one being edited ----
/// Custom output size: the Output resolution's size of the player's own (custom_output), typed, as output_res= in
/// longshot_options.ini sets it (parse_output: the same rules)
bool output_size_char(char c) { return (c >= '0' && c <= '9') || c == 'x' || c == 'X'; }
const char* output_pad_chars(const TextField& f) {
    // (one separator, after 1 to 5 width digits; partial text is fine until Done)
    if (f.caret < 1 || f.caret > 5 || f.len - f.caret - (f.caret < f.len) > 5) return "0123456789";
    for (int i = 0; i < f.len; i++)
        if (i != f.caret && (f.text[i] == 'x' || f.text[i] == 'X')) return "0123456789";
    return "0123456789x";
}
TextField output_field{{}, 0, 0, 11, output_size_char, output_pad_chars};
/// The field as the setting is: the size of the player's own, or empty
void output_field_show() {
    char t[24] = "";
    if (custom_output.w) snprintf(t, sizeof t, "%ux%u", custom_output.w, custom_output.h);
    text_set(output_field, t);
}
/// "<width>x<height>", each side 1 to 5 digits, nothing else: into *w, *h
bool size_text(const char* t, unsigned* w, unsigned* h) {
    unsigned* side[2] = {w, h};
    for (int k = 0; k < 2; k++) {
        int digits = 0;
        for (*side[k] = 0; *t >= '0' && *t <= '9'; t++, digits++) *side[k] = *side[k] * 10 + (unsigned)(*t - '0');
        const bool end = k ? *t == 0 : *t == 'x' || *t == 'X';   // (the width ends at the x, the height at the text's end)
        if (digits < 1 || digits > 5 || !end) return false;
        t += !k;
    }
    return true;
}
static void apply_custom_output(unsigned w, unsigned h, bool has_size) {
    // (changing the custom size can reorder the shape list: keep the startup and Later choices by shape, not index)
    int aspect_slot = -1;
    for (const Row& r : rows)
        if (r.value == &opts.ultrawide) aspect_slot = restart_slot_of(r);
    const bool follow_host = aspect_follows_host(opts.ultrawide), started_monitor = aspect_started == aspect_monitor_value();
    const std::string started_shape = aspect_shape(aspect_started);
    const bool later = aspect_slot >= 0 && restart_state.later;
    const int declined = later ? restart_state.declined[aspect_slot] : 0;
    const bool declined_monitor = later && declined == aspect_monitor_value();
    const std::string declined_shape = later ? aspect_shape(declined) : std::string();
    const bool start_custom = output_started == custom_index() && custom_output.w;
    const Out started = output_at(output_started);
    if (has_size) opts.output_res = output_choice_of(w, h);
    // (an offered size, including one found on a new screen, or an empty field: no separate custom choice)
    if (opts.output_res != custom_index() || !has_size)
        custom_output = {0, 0}, opts.output_res = output_choice(opts.output_res);
    // (the run's original custom size must still be reachable when returning to its starting shape)
    if (start_custom && (custom_output.w != started.w || custom_output.h != started.h)) {
        output_extras.push_back(started);
        output_started = last_choice();
    }
    aspect_started = started_monitor ? aspect_monitor_value() : aspect_index(started_shape);
    opts.ultrawide = aspect_of_choice(opts.output_res, follow_host);
    for (Row& r : rows) {
        if (r.value == &opts.output_res) r.hi = last_choice();
        if (r.value == &opts.ultrawide) r.hi = aspect_last();
    }
    if (aspect_slot >= 0) restart_state.restart_value(aspect_slot, aspect_started);
    if (later) restart_state.declined[aspect_slot] = declined_monitor ? aspect_monitor_value() : aspect_index(declined_shape);
    menu_refresh = true;
    output_field_show();   // (as taken: each side a multiple of 8)
    // (the player's from now on, also over HOS_OUTPUT_RES; the preset is unchanged, as for the Output resolution row)
    env_release(&opts.output_res);
    int now[mr::kMax];
    restart_values(now);
    restart_state.note(now);
    save();
}

/// Enter on Custom output size: the Output resolution row on the size typed (one it offers, or the player's own), or, emptied,
/// none of the player's own; saved. nullptr when taken, else why not
const char* output_field_apply() {
    if (output_preview_target.w) {
        output_field_show();
        return nullptr;
    }
    unsigned w = 0, h = 0;
    if (output_field.len) {
        if (!size_text(output_field.text, &w, &h)) return "Type the width, an x, then the height: 3440x1440.";
        w &= ~7u, h &= ~7u;
        if (!hos_output_size_valid(w, h))
            return "Not a size the game can draw: at least 640x360, at most 5120x2880's pixels, from 5:4 to 48:9 in shape.";
    }
    if (request_native_preview({w, h}, true)) {
        output_field_show();
        return nullptr;
    }
    apply_custom_output(w, h, output_field.len != 0);
    return nullptr;
}
struct FieldRow {
    const char* name;
    TextField* field;
    const char* (*apply)();
};
const FieldRow field_rows[] = {{"Custom output size", &output_field, output_field_apply}};
/// A row that is a text field (a row with no value, by its name, as is_motion_row): its field and apply; nullptr any other
const FieldRow* field_row(const Row& r) {
    for (const FieldRow& f : field_rows)
        if (!r.value && !strcmp(r.name, f.name)) return &f;
    return nullptr;
}
TextField* editing = nullptr;            // (the field that has the keyboard, menu_tick; nullptr none)
const char* (*edit_apply)() = nullptr;   // (what its Enter sets)
char edit_kept[sizeof TextField::text];  // (its text before: Esc puts it back)
const char* edit_why = nullptr;          // (why Enter did not take its text: the help's line, until the next key)
bool edit_enter_held = false;            // (the Enter that started it is still down: its repeats are not the field's Enter)
void edit_start(TextField& f, const char* (*apply)()) {
    editing = &f, edit_apply = apply, edit_why = nullptr, edit_enter_held = true;
    memcpy(edit_kept, f.text, sizeof edit_kept);
    f.caret = f.len;
}
/// The field lets the keyboard go: done (Enter: its apply takes the text, or says why not and it keeps the keyboard) or not (its
/// text back as it was)
void edit_end(bool done) {
    if (!editing || (done && edit_apply && (edit_why = edit_apply()))) return;
    if (!done) text_set(*editing, edit_kept), edit_why = nullptr;
    editing = nullptr, edit_apply = nullptr;
}

// ---- (longshot) the Controls page: what presses each of the Switch's buttons, a gamepad input and up to two keys ----
struct Control {
    int button;   // HOS_SW_*, or HOS_KEY_* (the stick's and camera's: keys only)
    const char* name;
};
const Control controls[] = {{HOS_SW_A, "A"}, {HOS_SW_B, "B"}, {HOS_SW_X, "X"}, {HOS_SW_Y, "Y"}, {HOS_SW_L, "L"},
                            {HOS_SW_R, "R"}, {HOS_SW_ZL, "ZL"}, {HOS_SW_ZR, "ZR"}, {HOS_SW_PLUS, "Plus"},
                            {HOS_SW_MINUS, "Minus"}, {HOS_SW_LS, "Left stick press"}, {HOS_SW_RS, "Right stick press"},
                            {HOS_SW_UP, "D-pad up"}, {HOS_SW_DOWN, "D-pad down"}, {HOS_SW_LEFT, "D-pad left"},
                            {HOS_SW_RIGHT, "D-pad right"}, {HOS_KEY_MOVE_UP, "Move forward"},
                            {HOS_KEY_MOVE_DOWN, "Move back"}, {HOS_KEY_MOVE_LEFT, "Move left"},
                            {HOS_KEY_MOVE_RIGHT, "Move right"}, {HOS_KEY_CAM_UP, "Camera up"},
                            {HOS_KEY_CAM_DOWN, "Camera down"}, {HOS_KEY_CAM_LEFT, "Camera left"},
                            {HOS_KEY_CAM_RIGHT, "Camera right"}, {HOS_KEY_WALK, "Walk"}, {HOS_KEY_SPRINT, "Sprint"},
                            {HOS_KEY_OC_NOTE_L, "Ocarina note (L)"},
                            {HOS_KEY_OC_NOTE_R, "Ocarina note (R)"}, {HOS_KEY_OC_NOTE_A, "Ocarina note (A)"},
                            {HOS_KEY_OC_NOTE_Y, "Ocarina note (Y)"}, {HOS_KEY_OC_NOTE_X, "Ocarina note (X)"},
                            {HOS_KEY_TOGGLE_HUD, "Toggle HUD"}};
constexpr int kControls = sizeof controls / sizeof controls[0];   // (then "Reset to defaults")
// (the stick's and camera's directions and Walk: keys only; the ocarina's note rows and Sprint take a gamepad input too,
// host/nn_hid.c)
bool keys_only(int i) {
    return i < kControls && ((controls[i].button >= HOS_KEY_MOVE_UP && controls[i].button <= HOS_KEY_CAM_RIGHT) ||
                             controls[i].button == HOS_KEY_WALK);
}
// (the controls with a Toggle column: the Switch's buttons but Plus and Minus (opts.hold_toggle, a bit each), and Sprint
// (opts.sprint_toggle keeps Sprint out of the Switch-button mask and avoids its sign bit))
bool has_toggle(int button) {
    return (button < HOS_SW_N && button != HOS_SW_PLUS && button != HOS_SW_MINUS) || button == HOS_KEY_SPRINT;
}
bool toggle_on(int button) { return button == HOS_KEY_SPRINT ? opts.sprint_toggle != 0 : ((opts.hold_toggle >> button) & 1) != 0; }
// (a control's Toggle as Reset puts it: on for Sprint, off for the buttons)
bool toggle_default(int button) { return button == HOS_KEY_SPRINT; }
void toggle_set(int button, bool on) {
    if (button == HOS_KEY_SPRINT) {
        opts.sprint_toggle = on;
        if (host && host->version >= 40 && host->bind) host->bind(HOS_BIND_SPRINT_TOGGLE, 0, 0, on);
    }
    else opts.hold_toggle = on ? opts.hold_toggle | (1 << button) : opts.hold_toggle & ~(1 << button);
}
// a pad input's name on the controller of the button prompts (the Button prompts row's choice; Auto: the face in use),
// else by its place
const char* pad_name(int p) {
    if (p < 0 || p >= HOS_PAD_N) return "None";
    static const char* paddles[] = {"R4", "L4", "R5", "L5"};
    if (p >= HOS_PAD_RPADDLE1) return paddles[p - HOS_PAD_RPADDLE1];
    static const char* t[HOS_PROMPT_KEYBOARD + 1][HOS_PAD_RPADDLE1] = {   // (by place, then HOS_PROMPT_*; the keyboard's: by place)
        {"Bottom button", "Right button", "Left button", "Top button", "Select / Back", "Home", "Start", "L3", "R3",
         "L1 / LB", "R1 / RB", "D-pad up", "D-pad down", "D-pad left", "D-pad right", "L2 / LT", "R2 / RT"},
        {"B", "A", "Y", "X", "Minus", "Home", "Plus", "L3", "R3", "L", "R", "D-pad up",
         "D-pad down", "D-pad left", "D-pad right", "ZL", "ZR"},
        {"A", "B", "X", "Y", "View", "Xbox", "Menu", "L3", "R3", "LB", "RB", "D-pad up",
         "D-pad down", "D-pad left", "D-pad right", "LT", "RT"},
        {"Cross", "Circle", "Square", "Triangle", "Share", "PS", "Options", "L3", "R3", "L1", "R1", "D-pad up", "D-pad down",
         "D-pad left", "D-pad right", "L2", "R2"},
        {"Cross", "Circle", "Square", "Triangle", "Create", "PS", "Options", "L3", "R3", "L1", "R1", "D-pad up", "D-pad down",
         "D-pad left", "D-pad right", "L2", "R2"},
        {"A", "B", "X", "Y", "View", "Steam", "Menu", "L3", "R3", "L1", "R1", "D-pad up", "D-pad down", "D-pad left",
         "D-pad right", "L2", "R2"}};
    const int face = opts.prompts ? opts.prompts - 1 : host && host->prompt_face ? host->prompt_face() : -1;
    return t[face >= 0 && face < HOS_PROMPT_KEYBOARD ? face + 1 : 0][p];
}
const char* key_name(int vk) {
    static char b[16];
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return b[0] = (char)vk, b[1] = 0, b;
    if (vk >= VK_F1 && vk <= VK_F12) return snprintf(b, sizeof b, "F%d", vk - VK_F1 + 1), b;
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD0 + 9) return snprintf(b, sizeof b, "Num %d", vk - VK_NUMPAD0), b;
    switch (vk) {
    case VK_SPACE: return "Space";
    case VK_SHIFT: return "Shift";
    case VK_CONTROL: return "Ctrl";
    case VK_MENU: return "Alt";
    case VK_TAB: return "Tab";
    case VK_RETURN: return "Enter";
    case VK_BACK: return "Backspace";
    case VK_CAPITAL: return "Caps Lock";
    case VK_UP: return "Up";
    case VK_DOWN: return "Down";
    case VK_LEFT: return "Left";
    case VK_RIGHT: return "Right";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "Page Up";
    case VK_NEXT: return "Page Down";
    case VK_ADD: return "Num +";
    case VK_SUBTRACT: return "Num -";
    case VK_MULTIPLY: return "Num *";
    case VK_DIVIDE: return "Num /";
    case VK_DECIMAL: return "Num .";
    case VK_OEM_1: return ";";
    case VK_OEM_PLUS: return "=";
    case VK_OEM_COMMA: return ",";
    case VK_OEM_MINUS: return "-";
    case VK_OEM_PERIOD: return ".";
    case VK_OEM_2: return "/";
    case VK_OEM_3: return "`";
    case VK_OEM_4: return "[";
    case VK_OEM_5: return "\\";
    case VK_OEM_6: return "]";
    case VK_OEM_7: return "'";
    case VK_LBUTTON: return "Left click";
    case VK_RBUTTON: return "Right click";
    case VK_MBUTTON: return "Middle click";
    case 0x05: return "Mouse 4";   // (VK_XBUTTON1)
    case 0x06: return "Mouse 5";
    }
    return snprintf(b, sizeof b, "Key %d", vk), b;
}
// the keys a binding can take (Esc cancels, Insert and F1 are the menu's, Delete clears)
const std::vector<int>& bindable_keys() {
    static std::vector<int> k;
    if (k.empty()) {
        for (int c = 'A'; c <= 'Z'; c++) k.push_back(c);
        for (int c = '0'; c <= '9'; c++) k.push_back(c);
        for (int c = VK_F2; c <= VK_F12; c++) k.push_back(c);
        for (int c = VK_NUMPAD0; c <= VK_NUMPAD0 + 9; c++) k.push_back(c);
        for (int c : {VK_SPACE, VK_SHIFT, VK_CONTROL, VK_MENU, VK_TAB, VK_RETURN, VK_BACK, VK_CAPITAL, VK_UP, VK_DOWN,
                      VK_LEFT, VK_RIGHT, VK_HOME, VK_END, VK_PRIOR, VK_NEXT, VK_ADD, VK_SUBTRACT, VK_MULTIPLY,
                      VK_DIVIDE, VK_DECIMAL, VK_OEM_1, VK_OEM_PLUS, VK_OEM_COMMA, VK_OEM_MINUS, VK_OEM_PERIOD,
                      VK_OEM_2, VK_OEM_3, VK_OEM_4, VK_OEM_5, VK_OEM_6, VK_OEM_7, VK_LBUTTON, VK_RBUTTON, VK_MBUTTON,
                      0x05, 0x06})   // (the mouse's buttons too: they count while the mouse is captured)
            k.push_back(c);
    }
    return k;
}
int page = 0;          // 0 the options, 1 the controls, 3 the teleport list, 4 the inventory, 5 the restart question (2: was the
                       // debug tools' own page, now on the Debug tab)
int csel = 0, ccol = menu_hit::kCtlPrimary;   // the page's row (options, then bindings and reset) and binding column
int blocked_reset_row = -1;

enum class ControlAction { None, Rebind, ResetRow, ResetAll };
struct ControlConfirmation {
    ControlAction action = ControlAction::None;
    int row = -1, set = 0, slot = 0, code = 0, choice = 0;
    uint64_t conflicts = 0;
};
ControlConfirmation control_confirmation;

bool control_column_enabled(int row, int col) {
    using enum menu_hit::CtlColumn;
    if (col == kCtlReset) {
        const int button = controls[row].button;
        return host && host->version >= 44 && host->reset_binding && host->binding_is_default &&
               ((has_toggle(button) && toggle_on(button) != toggle_default(button)) || !host->binding_is_default(button));
    }
    if (col == kCtlToggle) return has_toggle(controls[row].button);
    return col == kCtlKeyboard || !keys_only(row);
}
// (Layout preset and Button prompts: a preset shows in the grid at once. Counted the first time it is asked, not while the
// DLL starts: the rows are built then, after every module registered)
int top_rows() {
    static const int n = tab_row_count(TAB_PAGE);
    return n;
}
int next_control_selection(int selection, int col, int direction) {
    using enum menu_hit::CtlColumn;
    const int first = top_rows(), row = selection - first;
    if (row < 0 || row >= kControls || (col != kCtlToggle && col != kCtlReset)) {
        const int count = first + kControls + 1;
        return (selection + count + direction) % count;
    }
    // Stay in this column on binding rows, wrapping past the ends of the grid.
    int next = row;
    for (int step = 1; step < kControls; ++step) {
        next = (next + kControls + direction) % kControls;
        if (control_column_enabled(next, col)) return first + next;
    }
    return selection;
}
int listening = 0;     // waiting for the new input: 1 a gamepad's, 2 a key
bool listen_armed = false;   // (once what started it was let go)
int64_t listen_end = 0;
std::string keys_text(int button) {
    const int a = host->binding(HOS_BIND_KEY, button, 0), b = host->binding(HOS_BIND_KEY, button, 1);
    std::string t = a ? key_name(a) : "";
    if (b) t += std::string(a ? ", " : "") + key_name(b);
    return t.empty() ? "None" : t;
}

// ---- what is drawn: Dear ImGui over the window's image (vk_imgui.h) in the game's own style (vk_ui.h). Built anew at each
// present while something shows, from the menu's state (menu_tick and menu_record run on the presenting thread, one at a time:
// present_image's lock); nothing at all while nothing shows ----
std::mutex startup_mu;   // (the start-up screen's view, set from its own thread)
StartupView startup;
std::atomic<bool> startup_on{false};
std::atomic<float> startup_fade{0};   // (the start-up screen's fade to black at its end, 0 to 1: startup_fade_to)
std::atomic<int64_t> game_frames{0}, shown_frames{0};
char fps_text[48] = "";
char fps_sub[96] = "";   // (UTF-8: a translation's line can be three bytes a character)
// (the FPS counter's levels past the number, written every half second by menu_tick: the frame time and its 1% low; the
// Breakdown's parts, the GPU's time, the shaders built and draws left out over the graph's 4 s)
char fps_ms_text[24] = "", fps_low_text[24] = "";
double fps_ms_now = 0;   // (the frame time, for its colour)
double fps_rate_now = 0;   // (the number's rate, the game's own with frame generation: its colour)
char fps_part_text[fps_stats::kParts][24];
char fps_gpu_text[24] = "";
char fps_rt_text[fps_stats::kRtParts][24];   // (the render thread's parts, fps_texts)
char fps_gpu_busy_text[24] = "";              // (the GPU's share of the frame)
bool fps_rt_on = false;                       // (a render thread was measured)
char fps_built_text[16] = "", fps_left_text[16] = "";
char fps_dev_text[fps_stats::kDevLines][32];   // (the Dev level's values, fps_texts)
bool fps_vram_over = false;                    // (the VRAM line in red: past what the driver allows)
/// The FPS counter's target frame time, ms: the Game frame rate cap row's (HOS_FPS_MAX over it, as host/nn_vi.c, which follows
/// the row in play), or the display's refresh with V-Sync On and no frame generation when that is slower; 0 for none (Uncapped
/// without V-Sync)
/// The frame rate cap in effect: HOS_FPS_MAX's over the Game frame rate cap row's
int fps_cap_now() {
    static const int env_max = getenv("HOS_FPS_MAX") ? std::max(1, atoi(getenv("HOS_FPS_MAX"))) : 0;
    return fps_stats::cap_rate(opts.fps_cap, env_max);
}
double fps_target_ms() {
    const int cap = fps_cap_now();
    const bool fifo = menu_vsync() == 1 && opts.framegen == 0;
    return fps_stats::target_ms_for(cap, fifo, fifo ? (display_refresh_mhz() + 500) / 1000 : 0);
}
menu_hit::Map hits;      // (the menu's controls as last drawn: the mouse's, menu_tick)
float scroll_at[TAB_SIM_TURF + 2];   // (the first row shown, eased toward the selection: the tabs and subpages, then
                                   // the Controls page: kControlsSlot)
constexpr int kControlsSlot = TAB_SIM_TURF + 1;
int wheel_rows[TAB_SIM_TURF + 2];   // (the mouse wheel's notches since the last drawing, a slot each: scroll_rows scrolls by them)
int scrolled_to[TAB_SIM_TURF + 2] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};   // (selection last brought into view)
float scroll_want[TAB_SIM_TURF + 2];   // (where scroll_at is going: the wheel moves it, a new selection too)
float ease_step = 0;     // (this drawing's share of the way: the time since the last one)

ui::Ctx ui_ctx(uint32_t w, uint32_t h) {
    ui::Ctx c{ImGui::GetForegroundDrawList(), ui::fonts(), 1, (float)w, (float)h, 0, pad_last};
    c.confirm_dot = menu_keys::dot(pad_confirm), c.back_dot = menu_keys::dot(pad_back);
    c.paste_dot = menu_keys::dot(pad_paste);   // (longshot: a text field's Paste)
    c.s = ui::unit((float)h, opts.menu_size, &c.safe);
    ui::set_theme(opts.menu_theme);   // (the Menu theme's palette, before anything is drawn)
    return c;
}
/// The banner's height
float banner_h(const ui::Ctx& c) { return std::max(52 * c.s, c.safe + 36 * c.s); }

/// (the Bug report's line under its help once saved, below: measure_help makes room for the longest)
bool mic_help_row(const Row& r) {
    return r.tab == TAB_AUDIO && (!strcmp(r.name, "Test microphone") || !strcmp(r.name, "Microphone gain"));
}
std::string mic_help_text(const char* help) {
    // Whole-string lookup: translate each complete paragraph before joining the shared explanation.
    return std::string(help ? i18n::tr(help) : "") + "\n\n" + i18n::tr(kMicLevelHelp);
}
std::string mic_meter_label(int state, int incoming, int adjusted) {
    if (!(state & 1)) return i18n::tr("Test stopped");
    char text[64];
    snprintf(text, sizeof text, "Testing: %d%% -> %d%%", std::clamp(incoming, 0, 1000) / 10, std::clamp(adjusted, 0, 1000) / 10);
    return i18n::tr(text);
}
std::string mic_meter_status(int state) {
    if (!(state & 1)) return i18n::tr("Test stopped");
    std::string text = i18n::tr(!(state & 2) ? "Microphone unavailable" : !(state & 8) ? "Waiting for samples" :
                              (state & 16) ? "Clipping" : "");
    if (state & 4) text += i18n::tr(text.empty() ? "Default fallback" : " (Default fallback)");
    return text;
}
float mic_meter_height(ui::Ctx& c, float width) {
    float status = 0;
    for (int state : {0, 1, 5, 3, 7, 11, 15, 27, 31})
        status = std::max(status, ui::para(c, c.f.body, 16 * c.s, ImVec2(), 0, mic_meter_status(state).c_str(), width, false));
    return 42 * c.s + 8 * c.s + status + 14 * c.s;
}
void mic_help_meter(ui::Ctx& c, ImVec2 a, ImVec2 b);
/// The tallest help a row of the options can have, in pixels at width: its help and the tallest line any of its values can
/// have: the help box keeps that height whichever row is selected and whatever its value
float measure_help(ui::Ctx& c, float width) {
    float most = 0;
    for (const Row& r : rows) {
        float hint = 0;
        for (int v = r.lo; r.value && r.hint && v <= r.hi; v++)
            for (int k = 0; const char* t = r.hint(v, k); k++)
                hint = std::max(hint, ui::para(c, c.f.body, 16 * c.s, ImVec2(), 0, t, width - 18 * c.s, false));
        // (#1633: a module's button's lines, every one it can say (hint(0, k)): the Bug report's once saved, NFC's scan)
        for (int k = 0; !r.value && r.setting && r.setting->hint; k++) {
            const char* t = r.setting->hint(0, k);
            if (!t) break;
            hint = std::max(hint, ui::para(c, c.f.body, 16 * c.s, ImVec2(), 0, t, width - 18 * c.s, false));
        }
        if (r.help) {
            const bool mic = mic_help_row(r);
            const std::string explanation = mic ? mic_help_text(r.help) : r.help;
            most = std::max(most, ui::para(c, c.f.body, 18 * c.s, ImVec2(), 0, explanation.c_str(), width, false) +
                                  (hint ? 14 * c.s + hint : 0) + (mic ? mic_meter_height(c, width) : 0));
        }
    }
    return most;
}
/// label in room pixels: as it is when it fits; the selected row's slides through it (marquee_pos, marquee_max for menu_tick);
/// another's ends with "..."
/// (UTF-8: the byte offsets where its characters start, and the end; labels are cut and slid a character at a time, never
/// inside one: a microphone's name can be in any script)
std::vector<size_t> char_starts(const std::string& t) {
    std::vector<size_t> at;
    for (size_t i = 0; i < t.size(); i++)
        if (((unsigned char)t[i] & 0xC0) != 0x80) at.push_back(i);
    at.push_back(t.size());
    return at;
}
std::string fit(ui::Ctx& c, ImFont* f, float px, float room, const std::string& label, bool sel) {
    auto width = [&](const std::string& t) { return ui::size_of(f, px, t.c_str()).x; };
    if (width(label) <= room) return label;
    const std::vector<size_t> at = char_starts(label);
    const int chars = (int)at.size() - 1;
    if (sel) {   // (marquee_pos and marquee_max count characters)
        int start = chars;   // (the last start that still shows the label's end)
        while (start > 0 && width(label.substr(at[start - 1])) <= room) start--;
        marquee_max = start;
        const int from = std::min(marquee_pos, marquee_max);
        int end = chars;
        while (end > from + 1 && width(label.substr(at[from], at[end] - at[from])) > room) end--;
        return label.substr(at[from], at[end] - at[from]);
    }
    int end = chars;
    while (end > 1 && width(label.substr(0, at[end]) + "...") > room) end--;
    std::string t = label.substr(0, at[end]);
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t + "...";
}
/// A list of rows from top to bottom (pixels), n of them pitch apart, the selection sel kept in view: the first one shown,
/// eased toward its place (scroll_at[slot]); *shown how many fit
float scroll_rows(int slot, int n, int sel, float top, float bottom, float pitch, int* shown) {
    const int fit = std::max(1, (int)((bottom - top) / pitch));
    *shown = fit;
    float& at = scroll_at[slot];
    float& want = scroll_want[slot];
    const int wheel = std::exchange(wheel_rows[slot], 0);
    if (n <= fit) want = 0;   // (nothing to scroll: the wheel does nothing)
    else {
        want -= (float)wheel;   // (the wheel scrolls the rows and leaves the selection, and every value, as they are)
        static int fits[sizeof scroll_at / sizeof *scroll_at];   // (and after a resize: the rows that fit changed)
        if (sel != scrolled_to[slot] || fit != std::exchange(fits[slot], fit)) {   // (a new selection, by the keys or the pad: brought
                                                                                  // into view; the mouse's marks itself scrolled to)
            const int margin = fit > 4 ? 1 : 0;   // (a row of context above and below the selection)
            if (sel - margin < want) want = (float)(sel - margin);
            if (sel + margin + 1 > want + fit) want = (float)(sel + margin + 1 - fit);
        }
        want = std::round(std::clamp(want, 0.0f, (float)(n - fit)));
    }
    scrolled_to[slot] = sel;
    at += (want - at) * ease_step;
    if (std::fabs(want - at) < 0.01f) at = want;
    return at;
}
/// The scroll bar at x, beside a list of n rows of which shown fit, the first at `first`
void scroll_bar(ui::Ctx& c, float x, float top, float bottom, int n, int shown, float first) {
    if (n <= shown) return;
    const float s = c.s, span = bottom - top, y0 = top + span * first / n, y1 = top + span * (first + shown) / n;
    c.d->AddRectFilled(ImVec2(x - 2 * s, top), ImVec2(x + 2 * s, bottom), ui::rgba(0, 0, 0, 140), 2 * s);
    c.d->AddRectFilled(ImVec2(x - 2 * s, y0), ImVec2(x + 2 * s, y1), ui::kAccent, 2 * s);
}
/// The banner with a page's title in the middle (the Controls, Cheats and list pages), its height
float title_banner(ui::Ctx& c, const char* title, const char* right) {
    const float hh = banner_h(c), s = c.s;
    ui::banner(c, hh);
    ui::text_mid_centre(c, c.f.heading, 24 * s, c.w * 0.5f, hh * 0.5f + s, ui::kBannerInk, title);
    if (right) ui::text_mid_right(c, c.f.body, 17 * s, c.w - c.safe, hh * 0.5f + s, ui::kBannerInk, right);
    return hh;
}
/// Keep translated microphone explanations readable in the available help area. Other help keeps its normal size.
float mic_help_font_size(ui::Ctx& c, const char* help, float width, float height) {
    for (float px = 18; px > 14; px -= 0.5f)
        if (!help || ui::para(c, c.f.body, px * c.s, ImVec2(), 0, help, width, false) <= height) return px * c.s;
    return 14 * c.s;
}
/// The help box from (x0, y0) to x1: a title, the help, a line set off by a bar, a gold line at its foot. min_h: its height at
/// least (the tallest help the page can have, so it does not jump)
constexpr float kHelpCentre = -1;   // (help_box's y0: the box centred in its room)
void help_box(ui::Ctx& c, float x0, float x1, float y0, const char* title, const char* help, const char* hint, const char* gold_line,
              float min_h = 0, bool after_restart = false, float y_min = 0, float y_max = FLT_MAX, bool microphone = false) {
    // (y_max: the box ends above it, the prompt strip's top: moved up as far as y_min (under the banner) when it is taller than
    // the room below y0, its reserved height (min_h) given up first, its text cut at its foot only if it still does not fit)
    const float s = c.s, w = x1 - x0 - 48 * s;
    const std::string explanation = microphone ? mic_help_text(help) : "";
    if (microphone) help = explanation.c_str();
    const float meter_h = microphone ? mic_meter_height(c, w) : 0;
    const float hh = hint ? ui::para(c, c.f.body, 16 * s, ImVec2(), 0, hint, w - 18 * s, false) : 0;
    const float note = after_restart ? 30 * s : 0;
    const float fixed = 22 * s + (title ? 32 * s : 0) + note + (gold_line ? 52 * s : 0) + 22 * s;
    const float help_px = microphone ? mic_help_font_size(c, help, w, y_max - y_min - fixed - meter_h - (hint ? 14 * s + hh : 0)) : 18 * s;
    const float th = help ? ui::para(c, c.f.body, help_px, ImVec2(), 0, help, w, false) : 0;
    const float text_h = th + (hint ? 14 * s + hh : 0) + meter_h;
    float body = std::max(min_h, text_h);
    if (y0 < 0) y0 = std::max(y_min, (y_min + y_max - fixed - body) * 0.5f);   // (kHelpCentre: centred between y_min and y_max)
    if (y0 + fixed + body > y_max) y0 = std::max(y_min, y_max - fixed - body);
    if (y0 + fixed + body > y_max) body = std::max(text_h, y_max - y0 - fixed);
    const float y1 = std::min(y0 + fixed + body, y_max);
    ui::box(c, ImVec2(x0, y0), ImVec2(x1, y1));
    c.d->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
    float y = y0 + 22 * s;
    if (title) ui::text(c, c.f.heading, 21 * s, ImVec2(x0 + 24 * s, y), ui::kInk, title), y += 32 * s;
    if (after_restart) {   // (this row's change applies at the next start: said here, not in the row, where it would cut the name)
        ui::diamond(c, ImVec2(x0 + 29 * s, y + 10 * s), 4 * s, ui::kGoldHi);
        ui::text_mid(c, c.f.strong, 16 * s, x0 + 42 * s, y + 10 * s, ui::kGoldHi, "Takes effect after a restart");
        y += note;
    }
    if (microphone && mic_testing) {
        mic_help_meter(c, ImVec2(x0 + 24 * s, y), ImVec2(x1 - 24 * s, y + 42 * s));
        y += meter_h;
    }
    if (help) ui::para(c, c.f.body, help_px, ImVec2(x0 + 24 * s, y), ui::kInk, help, w);
    if (hint) {
        const float hy = y + th + 14 * s;
        c.d->AddRectFilled(ImVec2(x0 + 24 * s, hy + 2 * s), ImVec2(x0 + 26.5f * s, hy + hh - 2 * s), ui::kAccent, s);
        ui::para(c, c.f.body, 16 * s, ImVec2(x0 + 40 * s, hy), ui::kSoft, hint, w - 18 * s);
    }
    if (gold_line) {
        y = y0 + 22 * s + (title ? 32 * s : 0) + note + body + 20 * s;
        c.d->AddLine(ImVec2(x0 + 24 * s, y), ImVec2(x1 - 24 * s, y), ui::alpha(ui::kEdge, 0.8f), std::max(1.0f, s));
        ui::diamond(c, ImVec2(x0 + 29 * s, y + 20 * s), 4 * s, ui::kGoldHi);
        ui::text_mid(c, c.f.strong, 16 * s, x0 + 42 * s, y + 20 * s, ui::kGoldHi, gold_line);
    }
    c.d->PopClipRect();
}
/// The menu's column: from the safe margin to about 58% of the width (wide enough for a row at any unit); the help box beside it,
/// or, on a narrow window, over the column's foot (*narrow)
void columns(const ui::Ctx& c, float* x0, float* x1, float* hx0, float* hx1, bool* narrow) {
    const float s = c.s;
    *x0 = c.safe + 6 * s;
    *x1 = std::min(c.w - c.safe, std::max(c.w * 0.585f, *x0 + 560 * s));
    *hx0 = *x1 + 28 * s, *hx1 = c.w - c.safe;
    *narrow = *hx1 - *hx0 < 260 * s;
    if (*narrow) *hx0 = *x0 + 20 * s, *hx1 = *x1 - 20 * s;
}
void column(ui::Ctx& c, float x0, float x1, float top) {
    const float s = c.s;
    c.d->AddRectFilled(ImVec2(x0, top), ImVec2(x1, c.h), ui::kColumn);
    c.d->AddLine(ImVec2(x0, top), ImVec2(x0, c.h), ui::alpha(ui::kEdge, 1), 2 * s);
    c.d->AddLine(ImVec2(x1, top), ImVec2(x1, c.h), ui::alpha(ui::kEdge, 1), 2 * s);
}
/// A row's name cell, selected or not, its tag (After restart) at its right end
/// A row's name cell, selected or not; pending: a change that applies after a restart, a small gold diamond at its right end
/// (the help says it in words)
ImVec2 name_end(-1, -1);   // (the last name cell's top right: a value cell starting there joins it, in Modern dark one box)
/// (Modern dark) One half of a row's box, the name's (left) or the value's: drawn whole, rounded, past the seam, and cut at it on
/// a whole pixel, so neither half's anti-aliased fringe or edge falls on the seam (no line where the two meet)
void half_box(ui::Ctx& c, ImVec2 a, ImVec2 b, ImU32 fill, bool left) {
    const float s = c.s, r = ui::kRound * s, ext = r + 4 * s;
    const float seam = std::floor((left ? b.x : a.x) + 0.5f);
    if (left) c.d->PushClipRect(ImVec2(std::floor(a.x) - 2, a.y - 2), ImVec2(seam, b.y + 2), true);
    else c.d->PushClipRect(ImVec2(seam, a.y - 2), ImVec2(std::ceil(b.x) + 2, b.y + 2), true);
    const ImVec2 p = left ? a : ImVec2(seam - ext, a.y), q = left ? ImVec2(seam + ext, b.y) : b;
    c.d->AddRectFilled(p, q, fill, r);
    c.d->AddRect(p, q, ui::kEdge, r, 0, std::max(1.0f, s));
    c.d->PopClipRect();
}
/// joined: a value cell follows at its right end (Modern dark: one box with it, its right end left open for it)
void name_cell(ui::Ctx& c, ImVec2 a, ImVec2 b, const char* name, bool sel, bool pending = false, float px = 18.5f, bool joined = true) {
    const float s = c.s, mid = (a.y + b.y) * 0.5f;
    name_end = joined ? ImVec2(b.x, a.y) : ImVec2(-1, -1);
    const bool open = joined && ui::kRound > 0;
    const float r = ui::kRound * s;
    if (open) half_box(c, a, b, sel ? ui::kCellNameSel : ui::kCellName, true);
    if (sel && r > 0) {   // (Modern dark: a rounded cell, lit by a soft wash from its bar)
        if (!open) c.d->AddRectFilled(a, b, ui::kCellNameSel, r);
        c.d->AddRectFilledMultiColor(a + ImVec2(4 * s, 0), ImVec2((a.x + b.x) * 0.5f, b.y), ui::alpha(ui::kSelStart, 0.7f),
                                     ui::alpha(ui::kSelStart, 0), ui::alpha(ui::kSelStart, 0), ui::alpha(ui::kSelStart, 0.7f));
    } else if (sel)   // (selected: warmer at the gold bar, fading to the usual cell at its right)
        c.d->AddRectFilledMultiColor(a, b, ui::kSelStart, ui::kCellNameSel, ui::kCellNameSel, ui::kSelStart);
    else if (!open) c.d->AddRectFilled(a, b, ui::kCellName, r);
    if (!open) c.d->AddRect(a, b, ui::kEdge, r, 0, std::max(1.0f, s));
    if (sel) c.d->AddRectFilled(a, ImVec2(a.x + 4 * s, b.y), ui::kGoldHi, r, ImDrawFlags_RoundCornersLeft);
    float room = b.x - a.x - 36 * s;
    if (pending) {
        ui::diamond(c, ImVec2(b.x - 12 * s, mid), 4 * s, ui::kGoldHi);
        room -= 14 * s;
    }
    ImFont* f = sel ? c.f.strong : c.f.body;
    const std::string shown = fit(c, f, px * s, room, name, false);
    ui::text_mid(c, f, px * s, a.x + 20 * s, mid, sel ? ui::kInkBright : ui::kInk, shown.c_str());
}
/// A value cell's frame. Classic: a dark cell with a fine edge. Modern dark: next to its row's name cell (name_end), the name's
/// box carried on over it (its seam covered), the value in a dark well inset in it; on its own, a rounded dark cell
void value_frame(ui::Ctx& c, ImVec2 a, ImVec2 b, bool sel) {
    const float s = c.s, r = ui::kRound * s, in = 4 * s;
    if (r <= 0) {
        c.d->AddRectFilled(a, b, ui::kCellDark);
        c.d->AddRect(a, b, ui::kEdge, 0, std::max(1.0f, s));
        return;
    }
    if (std::fabs(a.x - name_end.x) < 1 && std::fabs(a.y - name_end.y) < 1) {
        half_box(c, a, b, sel ? ui::kCellNameSel : ui::kCellName, false);
        c.d->AddRectFilled(a + ImVec2(in, in), b - ImVec2(in, in), ui::kCellDark, std::max(0.0f, r - s));
    } else {
        c.d->AddRectFilled(a, b, ui::kCellDark, r);
        c.d->AddRect(a, b, ui::kEdge, r, 0, std::max(1.0f, s));
    }
}
/// A value cell's selection: Classic, the ring around the cell; Modern dark, a crisp ring on its inner well
void value_glow(ui::Ctx& c, ImVec2 a, ImVec2 b) {
    const float s = c.s;
    if (ui::kRound <= 0) return ui::glow(c, a, b);
    c.d->AddRect(a + ImVec2(4 * s, 4 * s), b - ImVec2(4 * s, 4 * s), ui::kOutline, std::max(0.0f, (ui::kRound - 1) * s), 0, std::max(1.0f, 1.75f * s));
}
/// A value cell: a choice with arrows at its ends (left, right: whether it can go that way), gold when selected
void choice_cell(ui::Ctx& c, ImVec2 a, ImVec2 b, const std::string& value, bool sel, bool left, bool right, bool fixed = false) {
    const float s = c.s, mid = (a.y + b.y) * 0.5f, in = 4 * s, px = 18 * s;
    value_frame(c, a, b, sel);
    if (sel && !fixed) ui::gold(c, a + ImVec2(in, in), b - ImVec2(in, in));
    const ImU32 ink = sel && !fixed ? ui::kGoldInk : fixed ? ui::kDim : ui::kInk, arrow = sel && !fixed ? ui::kGoldInk : ui::kDim;
    if (left) ui::triangle(c, ImVec2(a.x + in + 11 * s, mid), 9 * s, 0, arrow);
    if (right) ui::triangle(c, ImVec2(b.x - in - 11 * s, mid), 9 * s, 1, arrow);
    ImFont* f = sel ? c.f.strong : c.f.body;
    const std::string shown = fit(c, f, px, b.x - a.x - 2 * in - (left || right ? 48 : 16) * s, value, sel);   // (room for arrows)
    ui::text_mid_centre(c, f, px, (a.x + b.x) * 0.5f, mid, ink, shown.c_str());
    if (sel) value_glow(c, a, b);
}
/// A value cell of two halves (a row of two values: On and Off), the chosen one filled
void halves_cell(ui::Ctx& c, ImVec2 a, ImVec2 b, const char* lo, const char* hi, bool high, bool sel, bool fixed) {
    const float s = c.s, mid = (a.y + b.y) * 0.5f, in = 4 * s, px = 18 * s, xm = (a.x + b.x) * 0.5f;
    value_frame(c, a, b, sel);
    const ImVec2 fa = high ? ImVec2(xm, a.y + in) : a + ImVec2(in, in), fb = high ? b - ImVec2(in, in) : ImVec2(xm, b.y - in);
    if (sel && !fixed) ui::gold(c, fa, fb);
    else c.d->AddRectFilled(fa, fb, ui::kBronze, ui::kRound * s);
    const ImU32 on = sel && !fixed ? ui::kGoldInk : ui::kInk;
    ui::text_mid_centre(c, high ? c.f.body : c.f.strong, px, (a.x + xm) * 0.5f, mid, high ? ui::kDim : on, lo);
    ui::text_mid_centre(c, high ? c.f.strong : c.f.body, px, (xm + b.x) * 0.5f, mid, high ? on : ui::kDim, hi);
    if (sel) value_glow(c, a, b);
}
/// A slider's value cell; a display-only meter omits adjustment arrows and mouse track bounds.
void slider_cell(ui::Ctx& c, ImVec2 a, ImVec2 b, const char* value, int permille, bool sel, float* t0, float* t1, bool adjustable = true) {
    const float s = c.s, mid = (a.y + b.y) * 0.5f, in = 4 * s, px = 18 * s;
    value_frame(c, a, b, sel);
    if (sel) ui::gold(c, a + ImVec2(in, in), b - ImVec2(in, in));
    const float x0 = a.x + in + 24 * s, x1 = b.x - in - 24 * s, xv = x0 + (x1 - x0) * permille / 1000.0f, ty = b.y - in - 5.5f * s;
    const ImU32 track = sel ? ui::kTrackSel : ui::rgba(255, 255, 255, 40), done = sel ? ui::kGoldInk : ui::kAccent;
    c.d->AddRectFilled(ImVec2(x0, ty - 1.5f * s), ImVec2(x1, ty + 1.5f * s), track, 1.5f * s);
    c.d->AddRectFilled(ImVec2(x0, ty - 1.5f * s), ImVec2(xv, ty + 1.5f * s), done, 1.5f * s);
    ui::diamond(c, ImVec2(xv, ty), 4.5f * s, done);
    ui::text_mid_centre(c, c.f.strong, px, (a.x + b.x) * 0.5f, mid - 3 * s, sel ? ui::kGoldInk : ui::kInk, value);
    if (adjustable) {
        const ImU32 arrow = sel ? ui::kGoldInk : ui::kDim;
        ui::triangle(c, ImVec2(a.x + in + 11 * s, mid), 9 * s, 0, arrow), ui::triangle(c, ImVec2(b.x - in - 11 * s, mid), 9 * s, 1, arrow);
    }
    if (sel) value_glow(c, a, b);
    *t0 = adjustable ? x0 : 0, *t1 = adjustable ? x1 : 0;
}
/// Read-only live meter in help, with no mouse-hit registration or adjustment arrows.
void mic_help_meter(ui::Ctx& c, ImVec2 a, ImVec2 b) {
    const std::string label = mic_meter_label(mic_test_state, mic_levels[0], mic_levels[1]);
    const std::string status = mic_meter_status(mic_test_state);
    float unused0 = 0, unused1 = 0;
    slider_cell(c, a, b, label.c_str(), std::clamp(mic_levels[1], 0, 1000), false, &unused0, &unused1, false);
    if (!status.empty()) ui::para(c, c.f.body, 16 * c.s, ImVec2(a.x, b.y + 8 * c.s), ui::kSoft, status.c_str(), b.x - a.x);
}
/// (longshot) A text field's value cell: its text from the left (cut with "..." when too long); empty, the placeholder, dim.
/// While it has the keyboard (typing): light text on the dark cell inside a gold frame, a steady caret at its place, the text slid
/// left to keep the caret in view
void text_cell(ui::Ctx& c, ImVec2 a, ImVec2 b, const TextField& f, bool sel, bool typing, const char* placeholder) {
    const float s = c.s, mid = (a.y + b.y) * 0.5f, in = 4 * s, px = 18 * s, x0 = a.x + in + 12 * s, room = b.x - in - 12 * s - x0;
    value_frame(c, a, b, sel);
    if (sel && !typing) ui::gold(c, a + ImVec2(in, in), b - ImVec2(in, in));
    ImFont* font = sel ? c.f.strong : c.f.body;
    const std::string t(f.text, (size_t)std::clamp(f.len, 0, (int)sizeof f.text - 1));
    if (t.empty() && !typing) {
        ui::text_mid(c, c.f.body, px, x0, mid, sel ? ui::kGoldInk : ui::kDim, placeholder);
    } else if (!typing) {
        ui::text_mid(c, font, px, x0, mid, sel ? ui::kGoldInk : ui::kInk, fit(c, font, px, room, t, false).c_str());
    } else {
        const float caret = ui::size_of(font, px, t.substr(0, (size_t)std::clamp(f.caret, 0, (int)t.size())).c_str()).x;
        const float shift = std::max(0.0f, caret - room);
        c.d->PushClipRect(ImVec2(x0 - 3 * s, a.y), ImVec2(x0 + room + 3 * s, b.y), true);
        ui::text_mid(c, font, px, x0 - shift, mid, ui::kInkBright, t.c_str());
        c.d->AddRectFilled(ImVec2(x0 - shift + caret, mid - 11 * s), ImVec2(x0 - shift + caret + 2 * s, mid + 11 * s), ui::kGoldHi);
        c.d->PopClipRect();
        c.d->AddRect(a + ImVec2(in * 0.5f, in * 0.5f), b - ImVec2(in * 0.5f, in * 0.5f), ui::kGoldHi, 0, 2.5f * s);
    }
    if (sel) value_glow(c, a, b);
}
/// "Restart needed: " and the pending rows, on one line of width: the last ones as "and N more" when they do not fit
std::string restart_line(ui::Ctx& c, float px, float width) {
    const unsigned all = restart_pending();
    std::string line;
    for (int keep = restart_state.n; keep >= 0; keep--) {   // (the first `keep` pending rows by name)
        unsigned shown = 0;
        int named = 0, more = 0;
        for (int i = 0; i < restart_state.n; i++)
            if ((all >> i) & 1) (named < keep ? (shown |= 1u << i, named++) : more++);
        const char* names[mr::kMax];
        for (int i = 0; i < restart_state.n; i++) names[i] = i18n::tr(restart_names[i]);
        line = i18n::tr("Restart needed: ") + mr::names(names, restart_state.n, shown);
        if (more) {   // (" settings (many)": the form Russian and Polish take for 5 and up, i18n::plural; never asked in English)
            const int form = i18n::plural(more);
            line += std::string(named ? i18n::tr(" and ") : "") + std::to_string(more) +
                    i18n::tr(named ? " more" : form == 0 ? " setting" : form == 1 ? " settings" : " settings (many)");
        }
        if (ui::size_of(c.f.strong, px, line.c_str()).x <= width) break;
    }
    return line;
}

/// (longshot) The Debug tab's order: the tools' rows by their group (Row::cheat), Up and Down walking them so; Inventory item
/// and Change quantity are not shown (the Inventory page edits every amount)
struct DebugGroup { const char* title; int from, to; };   // (debug_tools_order[from, to))
// (-1 is the saved Damage taken option, not a cheat action: beside Heart protection in Player)
constexpr int debug_tools_order[] = {0, -1, 1, 11, 9, 10, 8, 2, 3, 7, 4};
constexpr DebugGroup debug_groups[] = {{"Player", 0, 6}, {"Travel", 6, 9}, {"Camera & world", 9, 11}};
constexpr int kDebugGroups = sizeof debug_groups / sizeof debug_groups[0];
constexpr int kDebugGeneralRows = 2;
constexpr int kDebugTools = sizeof debug_tools_order / sizeof debug_tools_order[0];
/// (the sections after the Developer one, a module's (menu/input_recording.cpp's Input recording): each heading's row and
/// where its rows begin in debug_rows' list; debug_rows fills them)
int debug_sections[8][2];
int debug_section_count;
/// The Debug tab's rows as shown (its row numbers, tab_row_to_global's): the switch, the tools in debug_tools_order while they
/// are on, then the Developer rows and those of the sections after it (debug_sections); *dev where those start, *dev_head the
/// Developer heading's row (-1 none). Their count
int debug_rows(int* out, int* dev, int* dev_head) {
    const int count = tab_row_count(TAB_DEBUG);
    int n = 0;
    *dev_head = -1;
    for (int k = 0; k < count; ++k) {
        const Row& r = rows[tab_row_to_global(TAB_DEBUG, k)];
        if (r.value == &opts.debug_tools || r.value == &opts.transition_autosave) out[n++] = k;
    }
    for (int t = 0; t < kDebugTools; ++t)
        for (int k = 0; k < count; ++k) {
            const Row& r = rows[tab_row_to_global(TAB_DEBUG, k)];
            if (debug_tools_order[t] == -1 ? r.value == &opts.damage_taken : r.cheat == debug_tools_order[t]) out[n++] = k;
        }
    *dev = n;
    debug_section_count = 0;
    if (quick_debug) return n;
    for (int k = 0; k < count; ++k) {
        const Row& r = rows[tab_row_to_global(TAB_DEBUG, k)];
        if (r.heading && *dev_head < 0) *dev_head = k;
        else if (r.heading && debug_section_count < 8)   // (a section after Developer: its heading, where its rows begin)
            debug_sections[debug_section_count][0] = k, debug_sections[debug_section_count++][1] = n;
        else if (*dev_head >= 0 && !r.heading && r.cheat < 0 && r.value != &opts.debug_tools) out[n++] = k;
    }
    return n;
}
/// The Debug tab's row after sel (dir 1) or before it (dir -1), in debug_rows' order
int debug_step(int sel, int dir) {
    int order[64], dev = 0, head = -1;
    const int n = debug_rows(order, &dev, &head);
    if (n == 0) return sel;   // (#1633: no row shown on the tab: nothing to step to)
    int at = 0;
    while (at < n && order[at] != sel) at++;
    if (at == n) return order[0];
    return order[(at + n + dir) % n];
}
/// (longshot) The Debug tab's rows from top down to bottom, sel selected: Debug tools across the top; the tools under their
/// headings, Player in the first column and Travel and Camera & world in the second when two fit (else one under another);
/// the Developer rows across the foot. Each value cell a column's width, so their edges line up. The hits are the tab's row
/// numbers, as the list's: menu_tick's clicks and keys are the other tabs'
void draw_debug_rows(ui::Ctx& c, float cx0, float cx1, float top, float bottom, int sel) {
    const float s = c.s, px = 18 * s, pitch = 48 * s, rh = 40 * s, gap = 24 * s;   // (the other tabs' rows: 40 high, 48 apart)
    const float left = cx0 + 44 * s, right = cx1 - 44 * s, mid = (left + right) * .5f;
    int order[64], dev = 0, dev_head = -1;
    const int n = debug_rows(order, &dev, &dev_head);
    // (two columns only when each fits its longest name whole (name_cell's room, the selected's bold) and its value cells at
    // their narrowest; the cells then as wide as the room lets them, up to what they ask)
    const float halves_w = 2 * std::max(ui::size_of(c.f.strong, px, "Off").x, ui::size_of(c.f.strong, px, "On").x);
    auto need = [&](const Row& r, bool tight) {
        const float pad = (tight ? 36 : 64) * s;
        if (r.cheat == 9 && age_shown(9))
            return 2 * std::max(ui::size_of(c.f.strong, px, "Kid").x, ui::size_of(c.f.strong, px, "Adult").x) + pad;
        if (r.value && r.hi - r.lo != r.step) {   // (a row of more than two values: its widest, arrows)
            float w = 0;
            for (int v = r.lo; v <= r.hi; v += r.step) w = std::max(w, ui::size_of(c.f.strong, px, r.text(v)).x);
            return w + (tight ? 56 : 68) * s;
        }
        if (r.cheat == 0 || r.cheat == 1 || r.cheat == 7 || r.value) return halves_w + pad;
        if (!r.value && r.setting && r.setting->kind == menu::Kind::Page)   // (a page it opens: its text, room for the arrow)
            return ui::size_of(c.f.strong, px, r.setting->text ? r.setting->text(0).c_str() : "").x + (tight ? 64 : 80) * s;
        if (!r.value && r.setting && !r.setting->choices.empty()) {   // (#1633: a module's button: the widest text its cell shows)
            float w = 0;
            for (const char* t : r.setting->choices) w = std::max(w, ui::size_of(c.f.strong, px, t).x);
            return w + (tight ? 56 : 60) * s;
        }
        const std::string v = r.cheat == 11 ? "1.25x" : cheat_value(r.cheat);   // (Movement speed: its widest, the cell kept still)
        const char* const says = cheat_button_text(r.cheat);
        return ui::size_of(c.f.strong, px, v.empty() ? says : v.c_str()).x + (cheat_chooses(r.cheat) ? 68 : tight ? 56 : 60) * s;   // (choice_cell keeps 48 for arrows either way)
    };
    float name_need[2] = {0, 0}, cell_full[2] = {halves_w + 64 * s, halves_w + 64 * s}, cell_tight[2] = {halves_w + 36 * s, halves_w + 36 * s};
    for (int i = kDebugGeneralRows; i < dev; ++i) {
        const Row& r = rows[tab_row_to_global(TAB_DEBUG, order[i])];
        const int col = i - kDebugGeneralRows >= debug_groups[0].to ? 1 : 0;   // (Player in the first, the rest in the second)
        name_need[col] = std::max(name_need[col], ui::size_of(c.f.strong, 18.5f * s, r.name).x + 40 * s);
        cell_full[col] = std::max(cell_full[col], need(r, false)), cell_tight[col] = std::max(cell_tight[col], need(r, true));
    }
    const float col_w = (right - left - gap) * .5f;
    const bool two = col_w >= name_need[0] + cell_tight[0] && col_w >= name_need[1] + cell_tight[1];
    // (where each goes: a row number of the layout and a column)
    int vrow[64] = {}, vcol[64] = {}, head_row[kDebugGroups] = {}, head_col[kDebugGroups] = {};
    vrow[1] = 1;
    int next_row[2] = {kDebugGeneralRows, kDebugGeneralRows};
    const bool tools = dev > kDebugGeneralRows;
    for (int g = 0; tools && g < kDebugGroups; ++g) {
        const int col = two && g > 0 ? 1 : 0;
        if (next_row[col] > kDebugGeneralRows) next_row[col]++;   // (a gap over a heading that follows another group)
        head_row[g] = next_row[col]++, head_col[g] = col;
        for (int t = debug_groups[g].from; t < debug_groups[g].to; ++t) vrow[kDebugGeneralRows + t] = next_row[col]++, vcol[kDebugGeneralRows + t] = col;
    }
    int foot = std::max(next_row[0], next_row[1]);
    if (tools) foot++;
    const int dev_row = foot;
    int extra = 0, sections = 0, section_row[8] = {};   // (a section after Developer: a gap, then its heading over its rows)
    for (int i = dev; i < n; ++i) {
        while (sections < debug_section_count && debug_sections[sections][1] == i)
            extra += 2, section_row[sections++] = dev_row + (i - dev) + extra;
        vrow[i] = dev_row + 1 + (i - dev) + extra;
    }
    const int visual_count = n > dev ? dev_row + 1 + (n - dev) + extra : foot;
    int sel_at = 0;
    for (int i = 0; i < n; ++i)
        if (order[i] == sel) sel_at = vrow[i];
    int shown = 0;
    const float first = scroll_rows(TAB_DEBUG, visual_count, sel_at, top, bottom, pitch, &shown);
    scroll_bar(c, cx0 + 20 * s, top, top + shown * pitch - (pitch - rh), visual_count, shown, first);
    // (the value cells' widths, a column's alike: what they ask, given up toward their narrowest while a name needs the room)
    float cell_w[2];
    if (two) {
        for (int k = 0; k < 2; ++k) cell_w[k] = std::max(cell_tight[k], std::min(cell_full[k], col_w - name_need[k]));
    } else {   // (one column: as wide as any of the tools asks)
        const float w = std::max(cell_full[0], cell_full[1]);
        cell_w[0] = cell_w[1] = std::max(std::min(w, right - left - std::max(name_need[0], name_need[1])), std::max(cell_tight[0], cell_tight[1]));
    }
    auto heading = [&](float x0, float x1, int row, const char* title) {   // (as the list's Developer heading: gold, a rule after)
        const float y = top + (row - first) * pitch, ty = y + rh * 0.62f, tw = ui::size_of(c.f.heading, 19 * s, title).x;
        if (y + pitch <= top || y >= bottom) return;
        ui::text_mid(c, c.f.heading, 19 * s, x0 + 2 * s, ty, ui::kGoldHi, title);
        if (x0 + tw + 16 * s < x1) c.d->AddLine(ImVec2(x0 + tw + 16 * s, ty), ImVec2(x1, ty), ui::alpha(ui::kEdge, 0.9f), std::max(1.0f, s));
    };
    c.d->PushClipRect(ImVec2(cx0, top), ImVec2(cx1, std::max(top, bottom)), true);
    for (int g = 0; tools && g < kDebugGroups; ++g)
        heading(head_col[g] ? mid + gap * .5f : left, !two || head_col[g] ? right : mid - gap * .5f, head_row[g], debug_groups[g].title);
    if (dev_head >= 0 && n > dev) heading(left, right, dev_row, rows[tab_row_to_global(TAB_DEBUG, dev_head)].name);
    for (int k = 0; k < sections; ++k)   // (only those with a row shown: a heading over nothing is left out)
        heading(left, right, section_row[k], rows[tab_row_to_global(TAB_DEBUG, debug_sections[k][0])].name);
    for (int i = 0; i < n; ++i) {
        const int k = order[i];
        const Row& r = rows[tab_row_to_global(TAB_DEBUG, k)];
        const float y = top + (vrow[i] - first) * pitch;
        if (y + pitch <= top || y >= bottom) continue;
        const bool in_tools = i >= kDebugGeneralRows && i < dev;
        const float x0 = two && in_tools && vcol[i] ? mid + gap * .5f : left;
        const float x1 = !two || !in_tools || vcol[i] ? right : mid - gap * .5f;
        const bool selected = k == sel;
        const float width = in_tools ? cell_w[vcol[i]] : std::max(need(r, false), cell_w[two ? 1 : 0]);
        const ImVec2 va(x1 - width, y), vb(x1, y + rh);
        name_cell(c, ImVec2(x0, y), ImVec2(va.x, y + rh), r.name, selected, restart_changed(r));   // (boxed, as on the other tabs)
        if (r.value && r.hi - r.lo != r.step) {   // (a choice with arrows, as on the other tabs)
            const bool fixed = row_fixed(r.value);
            choice_cell(c, va, vb, r.text(*r.value), selected, !fixed, !fixed, fixed);
        } else if (r.value) {   // (Debug tools and the Developer rows: values of the options, Off and On)
            halves_cell(c, va, vb, std::string(r.text(r.lo)).c_str(), std::string(r.text(r.hi)).c_str(), *r.value == r.hi, selected,
                        row_fixed(r.value));
        } else if (r.setting && r.setting->kind == menu::Kind::Action && r.setting->text) {   // (#1633: a module's button's text)
            choice_cell(c, va, vb, r.setting->text(0), selected, false, false);
        } else if (r.setting && r.setting->kind == menu::Kind::Page) {   // (a page it opens: its text, and the arrow, as elsewhere)
            choice_cell(c, va, vb, r.setting->text ? r.setting->text(0) : "", selected, false, false);
            ui::triangle(c, ImVec2(vb.x - 15 * s, (va.y + vb.y) * 0.5f), 10 * s, 1, selected ? ui::kGoldInk : ui::kInk);
        } else if (r.cheat == 0 || r.cheat == 1 || r.cheat == 7) {
            halves_cell(c, va, vb, "Off", "On", cheat_value(r.cheat) == "On", selected, false);
        } else if (age_shown(r.cheat)) {   // (Link's age now: the other half switches)
            halves_cell(c, va, vb, "Kid", "Adult", link_age.load(std::memory_order_relaxed) == 1, selected, false);
        } else {
            const std::string v = cheat_value(r.cheat);
            choice_cell(c, va, vb, v.empty() ? cheat_button_text(r.cheat) : v, selected, cheat_chooses(r.cheat), cheat_chooses(r.cheat), r.cheat == 9);
        }
        hits.add(x0, std::max(y, top), x1, std::min(y + rh, bottom), menu_hit::kRow, k, va.x, vb.x);
    }
    c.d->PopClipRect();
}

// A report cannot format Options{} as a live list index, or format a hypothetical
// default through a callback that reads the current engine/display (the Output
// resolution, Upscaler, Render resolution, Window scaling filter). The Quality
// preset's is Auto (this PC) at a first start. The other rows a preset sets have
// the built-in defaults only for a Custom player Auto never chose for (auto_for 0);
// else Auto's for this PC, which only apply_auto knows, and running it here would
// change the live options the renderer reads. Keep their current values, but do
// not call them changes or invent a default. GPU 0 and microphone 1 stay stable;
// report_default has the Aspect ratio's, the Game language's and Frame generation by's.
// The DLSS model's depends on RX_VK_DLSS_PRESET, and on the card's memory (CNN (E) under 7 GB).
bool report_default_depends_on_pc(const Row& r) {
    if (r.value == &opts.sharpness || r.value == &opts.pace || r.value == &opts.realtime ||
        r.value == &opts.handheld)
        return opts.preset != 0 || auto_for != 0;
    return r.value == &opts.output_res || r.value == &opts.resolution || r.value == &opts.dlss ||
           r.value == &opts.preset || r.value == &opts.upscale ||
           (r.value == &opts.dlss_model && (dlss_preset_env() || ur::cnn_for_vram(ur::MODEL_K, device_vram_bytes())));
}
// The report's default for row r: the default options' value (fixed), or the one the
// menu knows better. The Aspect ratio's is the monitor's shape, as outputs_sync makes
// it from the default Output resolution and ultrawide=1; the Game language's is
// language.txt's, else the first (the settings table). Frame generation by with nothing to
// choose (framegen_by_fixed) is the run's, whatever is saved: no change of the player's.
int report_default(const Row& r, int fixed) {
    static const Options defaults{};
    if (r.value == &opts.ultrawide) return aspect_of_choice(defaults.output_res, defaults.ultrawide);
    if (r.value == &opts.game_lang) return i18n::game_lang_index();
    if (r.value == &opts.framegen_by && framegen_by_fixed()) return *r.value;
    return fixed;
}
// (longshot) the Gameplay tab's Bug report: the rows not at their defaults, as the menu shows them, for its "Changes from the
// defaults"
std::string changed_rows_text(std::string& pc_options) {
    static const Options defaults{};
    std::string out;
    pc_options.clear();
    for (const Row& r : rows) {
        const char* at = (const char*)r.value;
        if (!r.value || !r.text || r.heading || at < (const char*)&opts || at >= (const char*)(&opts + 1)) continue;
        const int was = report_default(r, *(const int*)((const char*)&defaults + (at - (const char*)&opts)));
        const bool depends = report_default_depends_on_pc(r);
        if (!depends && *r.value == was) continue;
        std::string& to = depends ? pc_options : out;
        to += r.name;
        to += ": ";
        // (a microphone chosen by its name, not the name: a device's name can be a person's)
        to += r.value == &opts.mic && *r.value >= 2 ? "a microphone by its name" : r.text(*r.value);
        to += depends ? " (default depends on this PC" : " (default: ";
        if (!depends) to += r.text(was);
        if (restart_changed(r)) to += "; takes effect after a restart";
        to += ")\n";
    }
    return out;
}
/// (the report's picture: the control bridge's kind of shot, of the next present, vk_replay.cpp shot_ask_next: an image and a
/// buffer of its own, 1280x720, copied from the game's frame, which the menu is never drawn into, read back once the GPU is past it)
bool bug_report_picture(const char* path, uint64_t ticket) {
    return shot_ask_next(path, [](void* asked, int64_t, int ok) { bug_report::picture_written((uint64_t)(uintptr_t)asked, ok != 0); },
                         (void*)(uintptr_t)ticket) != 0;
}
} // namespace   // (#1633: asked by the modules too, menu_module.h's "what modules ask the renderer")
/// A or Enter on the Bug report: the report gathered here (the menu's part) and written on a thread of its own
void bug_report_save() {
    if (bug_report::state() == bug_report::kSaving) return;
    if (host && host->log_flush) host->log_flush();   // (the log's queued lines into the file first)
    bug_report::Inputs in;
    bug_report::fill_here(in);
    in.version = LONGSHOT_VERSION;
    in.gpu = device_report_text();
    in.options = changed_rows_text(in.pc_options);
    in.game_dir = game_folder_path();   // (its logs/ and bug-reports/: vk_paths.h)
    in.log = [](const char* line) { logf("%s", line); };
    in.ask_picture = bug_report_picture;
    logf("menu: bug report asked for");
    bug_report::start(std::move(in));
}
namespace {   // (#1633: the anonymous namespace again, after what the modules ask)

const char* const tab_titles[TAB_COUNT] = {"Graphics", "Audio", "Gameplay", "Controls", "NFC", "Mods", "Debug"};
inline const char* tab_title(int t) { return t >= 0 && t < TAB_COUNT ? tab_titles[t] : "Settings"; }
inline bool cheat_is_toggle(int c) { return c == 0 || c == 1 || c == 7; }

/// Draws a row's value cell (slider, toggle, choice, or subpage arrow), shared between the full menu and tuning bar
void row_value_cell(ui::Ctx& c, const Row& r, ImVec2 va, ImVec2 vb, bool selected, float* t0, float* t1) {
    const float s = c.s, mid = (va.y + vb.y) * 0.5f;
    const std::string tool = r.cheat >= 0 ? cheat_value(r.cheat) : "";
    if (r.cheat >= 0 && cheat_is_toggle(r.cheat)) {
        halves_cell(c, va, vb, "Off", "On", tool == "On", selected, false);
    } else if (r.cheat >= 0 && age_shown(r.cheat)) {
        halves_cell(c, va, vb, "Kid", "Adult", link_age.load(std::memory_order_relaxed) == 1, selected, false);
    } else if (r.cheat >= 0 && !tool.empty()) {
        choice_cell(c, va, vb, tool, selected, cheat_chooses(r.cheat), cheat_chooses(r.cheat));
    } else if (r.cheat >= 0 && *cheat_button_text(r.cheat)) {
        choice_cell(c, va, vb, cheat_button_text(r.cheat), selected, false, false, r.cheat == 9);
    } else if (const FieldRow* f = field_row(r)) {
        text_cell(c, va, vb, *f->field, selected, editing == f->field, "None");
    } else if (!r.value) {
        // (#1633: a module's button with a text of its own, as Test microphone's: Start test, Stop test)
        if (r.setting && r.setting->kind == menu::Kind::Action && r.setting->text)
            choice_cell(c, va, vb, r.setting->text(0), selected, false, false);
        else if (pending == &r) choice_cell(c, va, vb, "Press again to confirm", selected, false, false);
        else {
            choice_cell(c, va, vb, "", selected, false, false);
            ui::triangle(c, ImVec2(vb.x - 15 * s, mid), 10 * s, 1, selected ? ui::kGoldInk : ui::kInk);
        }
    } else {
        const bool fixed = row_fixed(r.value);
        const std::string lo = r.text(r.lo), hi = r.text(r.hi);
        const float half = (vb.x - va.x) * 0.5f - 18 * s;
        if (is_slider(r)) {
            slider_cell(c, va, vb, r.text(*r.value), menu_hit::slider_permille(*r.value, r.lo, r.hi), selected, t0, t1);
        } else if (r.hi - r.lo == r.step && !fixed && ui::size_of(c.f.strong, 18 * s, lo.c_str()).x <= half &&
                   ui::size_of(c.f.strong, 18 * s, hi.c_str()).x <= half) {
            halves_cell(c, va, vb, lo.c_str(), hi.c_str(), *r.value == r.hi, selected, fixed);
        } else {
            const bool output_row = r.value == &opts.output_res;
            const bool left = !fixed && (!output_row || step_output_choice(*r.value, -1) != *r.value) && (!stops_at_ends(r) || *r.value > r.lo);
            const bool right = !fixed && (!output_row || step_output_choice(*r.value, 1) != *r.value) && (!stops_at_ends(r) || *r.value < r.hi);
            choice_cell(c, va, vb, r.text(*r.value), selected, left, right, fixed);
        }
    }
}

// ---- the Mods tab: the mods of the mods folder (host/mods.c), in their order, each on or off, and the cheats of those
// that were on at the start (host/cheats.c). Shown only when there are mods. Left and Right (or a click on a half) switch the
// selected one; Enter picks a mod up, Up and Down move it, Enter or Esc puts it down. The order and the mods' switches apply at the
// next start (the restart question names them); a cheat's switch at once.
bool can_mods() {
    return host && host->version >= 35 && host->mod_info && host->mod_edit && host->mods_changed && host->cheat_info && host->cheat_set;
}
bool mods_present() {
    const char *n, *d, *o;
    int e;
    return can_mods() && host->mod_info(0, &n, &d, &o, &e);
}
bool tab_shown(int t) { return t != TAB_MODS || mods_present(); }
struct ModLine {
    char kind;   // 'm' a mod (index: its place), 'h' the heading over a mod's cheats, 's' a cheat file's own heading, 'c' a cheat
    int index;   // (the cheat's, for 'h', 's' and 'c')
};
std::vector<ModLine> mod_lines() {
    std::vector<ModLine> out;
    if (!can_mods()) return out;
    const char *n, *d, *o, *mod, *last = nullptr;
    int e, kind, on;
    for (int i = 0; host->mod_info(i, &n, &d, &o, &e); i++) out.push_back({'m', i});
    for (int j = 0; host->cheat_info(j, &n, &mod, &o, &kind, &on); j++) {
        if (!last || strcmp(last, mod)) out.push_back({'h', j}), last = mod;
        out.push_back({kind == MKWVK_CHEAT_HEADING ? 's' : 'c', j});
    }
    return out;
}
bool mod_line_chosen(const ModLine& l) { return l.kind == 'm' || l.kind == 'c'; }
int mods_sel = 0;     // (the selected line: a mod or a cheat)
int mods_grab = -1;   // (the mod picked up to move: its place; -1 none)
/// The line after (dir 1) or before (dir -1) sel that can be chosen; sel itself at the ends
int mods_step(const std::vector<ModLine>& lines, int sel, int dir) {
    for (int i = sel + dir; i >= 0 && i < (int)lines.size(); i += dir)
        if (mod_line_chosen(lines[i])) return i;
    return sel;
}
/// The Mods tab's keys (menu_tick): the selection, the switches and the order
void mods_tick(const bool* press, bool& changed) {
    const std::vector<ModLine> lines = mod_lines();
    if (lines.empty()) return;
    mods_sel = std::clamp(mods_sel, 0, (int)lines.size() - 1);
    if (!mod_line_chosen(lines[mods_sel])) mods_sel = mods_step(lines, mods_sel, 1);
    const ModLine l = lines[mods_sel];
    if (mods_grab >= 0) {   // (a mod picked up: Up and Down move it a place, Enter or Esc put it down)
        if (press[1] && mods_grab > 0) host->mod_edit(mods_grab, 2), mods_grab--, mods_sel = mods_grab, changed = true;
        if (press[2] && mods_grab + 1 < (int)std::count_if(lines.begin(), lines.end(), [](const ModLine& x) { return x.kind == 'm'; }))
            host->mod_edit(mods_grab, 3), mods_grab++, mods_sel = mods_grab, changed = true;
        if (press[7] || press[5]) mods_grab = -1, changed = true;
        return;
    }
    if (press[1]) mods_sel = mods_step(lines, mods_sel, -1), changed = true;
    if (press[2]) mods_sel = mods_step(lines, mods_sel, 1), changed = true;
    if (press[1] || press[2]) return;
    const char *n, *d, *o;
    int on = 0, kind = 0;
    if (l.kind == 'm' && host->mod_info(l.index, &n, &d, &o, &on)) {
        if (press[3] || press[4]) host->mod_edit(l.index, press[4] ? 1 : 0), changed = true;
        if (press[7]) mods_grab = l.index, changed = true;
    } else if (l.kind == 'c' && host->cheat_info(l.index, &n, &d, &o, &kind, &on) && kind == MKWVK_CHEAT_DATA) {
        if (press[3] || press[4] || press[7]) host->cheat_set(l.index, press[7] ? !on : press[4] ? 1 : 0), changed = true;
    }
}
/// The Mods tab's rows and help, under the tab strip (draw_options)
void draw_mods(ui::Ctx& c, float cx0, float cx1, float hx0, float hx1, bool narrow, float hh) {
    const float s = c.s;
    const std::vector<ModLine> lines = mod_lines();
    const int count = (int)lines.size();
    mods_sel = count ? std::clamp(mods_sel, 0, count - 1) : 0;
    if (count && !mod_line_chosen(lines[mods_sel])) mods_sel = mods_step(lines, mods_sel, 1);
    // (the help: the selected mod's two lines, or the cheat's mod and state)
    const char *name = "", *detail = "", *note = "", *mod = "";
    int on = 0, kind = 0;
    std::string help, hint;
    if (count && lines[mods_sel].kind == 'm' && host->mod_info(lines[mods_sel].index, &name, &detail, &note, &on)) {
        help = detail;
        hint = *note ? std::string(note) + " " : "";
        hint += mods_grab >= 0 ? "Up or Down moves it; Enter puts it down."
                               : "A mod lower in the list wins a file two mods have. Its switch and its place apply at the next start.";
    } else if (count && host->cheat_info(lines[mods_sel].index, &name, &mod, &note, &kind, &on)) {
        help = std::string("A cheat of ") + mod + ".";
        hint = kind == MKWVK_CHEAT_DATA ? "On and Off apply at once."
             : kind == MKWVK_CHEAT_MASTER ? "Its file's master code: always on while its mod is."
                                          : std::string("Not supported: ") + note + ".";
    }
    const std::string pending_line = restart_pending() ? restart_line(c, 16 * s, hx1 - hx0 - 66 * s) : "";
    ui::Prompt prompt_list[5] = {{ui::kUpDown, "Choose"}, {ui::kLeftRight, "On / Off"}, {ui::kConfirm, "Move"}, {ui::kTabs, "Tabs"},
                                 {ui::kBack, "Close"}};
    int prompts_n = 5;
    if (mods_grab >= 0) prompt_list[0] = {ui::kUpDown, "Move"}, prompt_list[1] = {ui::kConfirm, "Put down"}, prompts_n = 2;
    else if (count && lines[mods_sel].kind == 'c') {
        prompt_list[2] = prompt_list[3], prompt_list[3] = prompt_list[4], prompts_n = 4;
        if (kind != MKWVK_CHEAT_DATA) prompt_list[1] = prompt_list[2], prompt_list[2] = prompt_list[3], prompts_n = 3;
    }
    const float bar_top = ui::prompt_bar_top(c);
    float bottom = std::min(c.h - c.safe, bar_top) - 10 * s;
    const char* gold = pending_line.empty() ? nullptr : pending_line.c_str();
    if (narrow) {
        const float help_h = (bottom - hh) * 0.4f;
        bottom = std::min(bar_top, c.h - c.safe) - help_h - 16 * s;
        help_box(c, hx0, hx1, bottom + 8 * s, name, help.c_str(), hint.empty() ? nullptr : hint.c_str(), gold, 0, false, bottom + 8 * s,
                 bar_top - 10 * s);
    } else {
        help_box(c, hx0, hx1, kHelpCentre, name, help.c_str(), hint.empty() ? nullptr : hint.c_str(), gold, 0,
                 false, hh + 18 * s, bar_top - 12 * s);
    }
    const float rh = 40 * s, pitch = 48 * s, top = hh + 24 * s, rx0 = cx0 + 44 * s, rx1 = cx1 - 44 * s, split = rx0 + (rx1 - rx0) * 0.62f;
    int shown = 0;
    const float first = scroll_rows(TAB_MODS, count, mods_sel, top, bottom, pitch, &shown);
    scroll_bar(c, cx0 + 20 * s, top, top + shown * pitch - (pitch - rh), count, shown, first);
    marquee_max = 0;
    c.d->PushClipRect(ImVec2(cx0, top - 8 * s), ImVec2(cx1, bottom + 8 * s));
    for (int i = 0; i < count; i++) {
        const float y = top + (i - first) * pitch;
        if (y + rh < top - 8 * s || y > bottom + 8 * s) continue;
        const ModLine& l = lines[i];
        const char *n = "", *d = "", *o = "", *m = "";
        int e = 0, k = 0;
        if (l.kind == 'm') host->mod_info(l.index, &n, &d, &o, &e);
        else host->cheat_info(l.index, &n, &m, &o, &k, &e);
        if (l.kind == 'h' || l.kind == 's') {   // (a heading: gold for a mod's cheats, quieter for its file's own)
            const std::string title = l.kind == 'h' ? std::string("Cheats: ") + m : std::string(n);
            const float ty = y + rh * 0.62f, px = (l.kind == 'h' ? 19 : 17) * s;
            const std::string t = fit(c, c.f.heading, px, rx1 - rx0 - 40 * s, title, false);
            const float tw = ui::size_of(c.f.heading, px, t.c_str()).x;
            ui::text_mid(c, c.f.heading, px, rx0 + 2 * s, ty, l.kind == 'h' ? ui::kGoldHi : ui::kSoft, t.c_str());
            if (l.kind == 'h') c.d->AddLine(ImVec2(rx0 + tw + 16 * s, ty), ImVec2(rx1, ty), ui::alpha(ui::kEdge, 0.9f), std::max(1.0f, s));
            continue;
        }
        const bool selected = i == mods_sel;
        name_cell(c, ImVec2(rx0, y), ImVec2(split, y + rh), n, selected);
        const ImVec2 va(split, y), vb(rx1, y + rh);
        if (l.kind == 'm' && mods_grab == l.index) choice_cell(c, va, vb, "Moving", selected, l.index > 0, true);
        else if (l.kind == 'm' || k == MKWVK_CHEAT_DATA) halves_cell(c, va, vb, "Off", "On", e != 0, selected, false);
        else choice_cell(c, va, vb, k == MKWVK_CHEAT_MASTER ? "Always on" : "Not supported", selected, false, false, true);
        hits.add(rx0, std::max(y, top - 8 * s), rx1, std::min(y + rh, bottom + 8 * s), menu_hit::kRow, i, va.x, vb.x);
    }
    c.d->PopClipRect();
    ui::prompt_bar(c, prompt_list, prompts_n);
}

void draw_options(ui::Ctx& c) {
    const float s = c.s, hh = banner_h(c);
    ui::banner(c, hh);
    // the tabs in the banner: dark ink on the tan, the shown one light on a dark inset; the tab keys at their ends (a click there
    // is the previous or the next tab)
    float tw[TAB_COUNT], total = 0;
    const float tab_px = 20 * s, tab_pad = 22 * s;   // (the tabs' ink and the room each side of it)
    for (int i = 0; i < TAB_COUNT; i++) tw[i] = tab_shown(i) ? ui::size_of(c.f.heading, tab_px, tab_titles[i]).x + 2 * tab_pad : 0, total += tw[i];
    float x = (c.w - total) * 0.5f;
    const float ty = hh * 0.5f + s;
    const char* const keys[2] = {c.pad ? "L" : "Q", c.pad ? "R" : "E"};
    for (int i = 0; i < 2; i++) {
        const float cw = 23 * s, cx = i ? x + total + 18 * s : x - 18 * s - cw;
        const bool hover = hover_tab == TAB_COUNT + i;
        c.d->AddRectFilled(ImVec2(cx, ty - 11.5f * s), ImVec2(cx + cw, ty + 11.5f * s), hover ? ui::kKeyCapHi : ui::kKeyCap, 4 * s);
        ui::text_mid_centre(c, c.f.strong, 13 * s, cx + cw * 0.5f, ty, ui::kInk, keys[i]);
        hits.add(i ? cx - 10 * s : 0, 0, i ? c.w : cx + cw + 10 * s, hh, i ? menu_hit::kTabNext : menu_hit::kTabPrev, -1);
    }
    for (int i = 0; i < TAB_COUNT; i++) {
        // (the Motion page: under Controls; the FPS counter details: under Gameplay)
        if (!tab_shown(i)) continue;   // (the Mods tab without mods)
        const bool shown = i == (page_parent(cur_tab) >= 0 ? page_parent(cur_tab) : cur_tab);
        if (shown || i == hover_tab) {
            // (an inset from a little below the banner's top, its upper corners rounded; the shown one with a gold line under it)
            const ImVec2 a(x + 2 * s, 7 * s), b(x + tw[i] - 2 * s, hh - 3 * s);
            c.d->AddRectFilled(a, b, shown ? ui::kTabInset : ui::alpha(ui::kTabInset, 0.35f), 5 * s, ImDrawFlags_RoundCornersTop);
            if (shown) {
                c.d->AddRect(a, b + ImVec2(0, 1), ui::kTabEdge, 5 * s, ImDrawFlags_RoundCornersTop, std::max(1.0f, s));
                c.d->AddRectFilled(ImVec2(a.x + 6 * s, b.y - 3 * s), ImVec2(b.x - 6 * s, b.y), ui::kGoldHi, 1.5f * s);
            }
        }
        ui::text_mid(c, c.f.heading, tab_px, x + tab_pad, ty, shown ? ui::kInkBright : ui::kBannerInk, tab_titles[i]);
        hits.add(x, 0, x + tw[i], hh, menu_hit::kTab, i);
        x += tw[i];
    }
    {   // (the banner's left: the Longshot logo and its name, a little larger than the tabs' ink; at Small and Medium smaller
        // until it fits, at Large at its size or not at all, as the version)
        const float room = (c.w - total) * 0.5f - 60 * s - c.safe, px = 24 * s;
        float aspect = 1;
        ImTextureData* logo = ui::logo(&aspect);
        const auto width = [&](float p) {
            return (logo ? p * 1.25f * aspect + 10 * s : 0) + ui::size_of(c.f.heading, p, "Longshot").x;
        };
        const float fit = ui::fit_px(px, opts.menu_size == 3 ? px : 14 * s, 0.5f * s, room, width);
        if (fit > 0) {
            float lx = c.safe;
            if (logo) {
                const float lh = fit * 1.25f;
                c.d->AddImage(logo->GetTexRef(), ImVec2(lx, ty - lh * 0.5f), ImVec2(lx + lh * aspect, ty + lh * 0.5f));
                lx += lh * aspect + 10 * s;
            }
            ui::text_mid(c, c.f.heading, fit, lx, ty, ui::kBannerInk, "Longshot");
        }
    }
    {   // (which build, at the banner's right, so a report can say it without opening the log: as players see it, version_text,
        // "v3.0.0-rc.1" or "Nightly 46 · 434712bb"; the log and the bug report keep the whole name. Under it, a development
        // run's own name for what it tests, LONGSHOT_BUILD_NAME (dev.py queue run --desc), when the run sets one; draw_startup shows it too)
        const std::string ver = version_text(LONGSHOT_VERSION);
        const char* named = getenv("LONGSHOT_BUILD_NAME");
        const std::string name = named ? named : "";
        const float room = (c.w - total) * 0.5f - 60 * s - c.safe, px = 15 * s, right = c.w - c.safe;
        const auto width = [&](float p) { return ui::size_of(c.f.body, p, ver.c_str()).x; };
        // (still too long, as a source build's "v2.0.11-821-g7b81752-dirty": at Small and Medium smaller
        // until it fits, on half-point steps, 10 points at the least; at Large, the size for a TV across the room, at its size
        // or not at all)
        const float fit = ui::fit_px(px, opts.menu_size == 3 ? px : kVersionMinPt * s, 0.5f * s, room, width);
        const auto name_width = [&](float p) { return ui::size_of(c.f.body, p, name.c_str()).x; };
        const float name_fit = name.empty() ? 0 : ui::fit_px(13 * s, opts.menu_size == 3 ? 13 * s : kVersionMinPt * s, 0.5f * s, room, name_width);
        const float vy = name_fit > 0 ? ty - 8 * s : ty;
        if (fit > 0) ui::text_mid_right(c, c.f.body, fit, right, vy, ui::kBannerInk, ver.c_str());
        if (name_fit > 0) ui::text_mid_right(c, c.f.body, name_fit, right, ty + 9 * s, ui::alpha(ui::kBannerInk, 0.8f), name.c_str());
    }
    // the rows, in the column; the help beside it
    float cx0, cx1, hx0, hx1;
    bool narrow;
    columns(c, &cx0, &cx1, &hx0, &hx1, &narrow);
    column(c, cx0, cx1, hh);
    if (cur_tab == TAB_MODS) {   // (the Mods tab: its own rows, from the host)
        draw_mods(c, cx0, cx1, hx0, hx1, narrow, hh);
        return;
    }
    const int count = tab_row_count(cur_tab), sel = tab_selected[cur_tab];
    const Row& selr = rows[tab_row_to_global(cur_tab, sel)];
    const bool mic_help = mic_help_row(selr);
    const FieldRow* field = field_row(selr);   // (longshot: a text field: why Enter did not take it, while it is edited)
    const char* hint = selr.value && selr.hint ? selr.hint(*selr.value, -1) : field && editing == field->field ? edit_why : nullptr;
    // (#1633: a module's button's own line, as the NFC tab's: what its press did, or what its scan is doing)
    if (!selr.value && selr.setting && selr.setting->hint) hint = selr.setting->hint(0, -1);
    const std::string pending_line = (cur_tab == TAB_GRAPHICS || cur_tab == TAB_DEBUG) && restart_pending() ? restart_line(c, 16 * s, hx1 - hx0 - 66 * s) : "";
    static float help_tall = 0, help_tall_w = 0;
    static std::string help_said;   // (Save my pipeline list's line as measured: measured again when it changes)
    if (help_tall_w != hx1 - hx0 || help_said != share_said)
        help_tall_w = hx1 - hx0, help_said = share_said, help_tall = measure_help(c, hx1 - hx0 - 48 * s);
    // (a row that opens a page (Motion, Controls, Open debug tools): A or Enter opens it; Restart and Quit: A or Enter twice)
    const bool confirms = selr.setting && selr.setting->confirm;   // (Restart, Quit: press again to confirm)
    ui::Prompt prompt_list[7] = {{ui::kUpDown, "Choose"}};
    int prompts_n = 1;
    const char* compact_key_name = opts.compact_menu_key == 1 ? "RCtrl" : "RShift";
    if (field && editing == field->field) {
        prompts_n = 0;
        prompt_list[prompts_n++] = {ui::kLeftRight, "Caret"};
        if (c.pad) {
            prompt_list[prompts_n++] = {ui::kUpDown, "Character"};
            prompt_list[prompts_n++] = {ui::kAddChar, "Add"};
            prompt_list[prompts_n++] = {ui::kRemoveChar, "Remove"};
        }
        prompt_list[prompts_n++] = {ui::kPaste, "Paste"};
        prompt_list[prompts_n++] = {ui::kConfirm, "Done"}, prompt_list[prompts_n++] = {ui::kBack, "Cancel"};
    } else {
        if (field) {
            prompt_list[prompts_n++] = {ui::kConfirm, "Edit"};
        } else if (selr.value || (selr.cheat >= 0 && (selr.cheat <= 1 || selr.cheat == 7 || cheat_chooses(selr.cheat) || age_shown(selr.cheat)))) {
            prompt_list[prompts_n++] = {ui::kLeftRight, "Change"};
            if (selr.cheat >= 0) if (const char* says = cheat_confirm(selr.cheat)) prompt_list[prompts_n++] = {ui::kConfirm, says};
            prompt_list[prompts_n++] = {ui::kCompact, "Compact", compact_key_name};
        } else if (selr.cheat >= 0) {
            if (const char* says = cheat_confirm(selr.cheat)) prompt_list[prompts_n++] = {ui::kConfirm, says};
            prompt_list[prompts_n++] = {ui::kCompact, "Compact", compact_key_name};
        } else {
            prompt_list[prompts_n++] = {ui::kConfirm, confirms ? (pending == &selr ? "Confirm" : selr.name) : selr.setting && selr.setting->verb ? selr.setting->verb : "Open"};
        }
        if (page_parent(cur_tab) >= 0) prompt_list[prompts_n++] = {ui::kBack, "Back"};   // (Esc: to Controls, Audio, Gameplay or Debug)
        else {
            prompt_list[prompts_n++] = {ui::kTabs, "Tabs"}, prompt_list[prompts_n++] = {ui::kBack, "Close"};
        }
    }
    const float bar_top = ui::prompt_bar_top(c);   // (the strip itself drawn last, over all: below)
    float bottom = std::min(c.h - c.safe, bar_top) - 10 * s;   // (the rows end above the prompts)
    const bool after = restart_changed(selr);
    if (narrow) {   // (help over the column's foot: normally half the room, up to 65% for the mic meter and its explanation)
        const float help_h = std::min(22 * s + 32 * s + help_tall + (after ? 30 * s : 0) + (pending_line.empty() ? 0 : 52 * s) + 22 * s,
                                      (bottom - hh) * (mic_help ? 0.65f : 0.5f));
        bottom = std::min(bar_top, c.h - c.safe) - help_h - 16 * s;
        help_box(c, hx0, hx1, bottom + 8 * s, selr.name, selr.help, hint, pending_line.empty() ? nullptr : pending_line.c_str(), help_tall,
                 after, bottom + 8 * s, bar_top - 10 * s, mic_help);
    } else {
        help_box(c, hx0, hx1, kHelpCentre, selr.name, selr.help, hint,
                 pending_line.empty() ? nullptr : pending_line.c_str(), help_tall, after, hh + 18 * s, bar_top - 12 * s, mic_help);
    }
    const float rh = 40 * s, pitch = 48 * s, top = hh + 24 * s, rx0 = cx0 + 44 * s, rx1 = cx1 - 44 * s, split = rx0 + (rx1 - rx0) * 0.55f;
    if (cur_tab == TAB_DEBUG) {   // (the Debug tab: its own layout of the same rows)
        marquee_max = 0;
        draw_debug_rows(c, cx0, cx1, top, bottom, sel);
        ui::prompt_bar(c, prompt_list, prompts_n);
        return;
    }
    int shown = 0;
    const float first = scroll_rows(cur_tab, count, sel, top, bottom, pitch, &shown);
    scroll_bar(c, cx0 + 20 * s, top, top + shown * pitch - (pitch - rh), count, shown, first);
    marquee_max = 0;   // (set again below while the selected row's value is too long)
    c.d->PushClipRect(ImVec2(cx0, top - 8 * s), ImVec2(cx1, bottom + 8 * s));
    for (int i = 0; i < count; i++) {
        const float y = top + (i - first) * pitch;
        if (y + rh < top - 8 * s || y > bottom + 8 * s) continue;
        const Row& r = rows[tab_row_to_global(cur_tab, i)];
        if (r.heading) {   // (a section's title: gold, a rule after it to the column's end; no cell, nothing to click)
            const float ty = y + rh * 0.62f, tw = ui::size_of(c.f.heading, 19 * s, r.name).x;
            ui::text_mid(c, c.f.heading, 19 * s, rx0 + 2 * s, ty, ui::kGoldHi, r.name);
            c.d->AddLine(ImVec2(rx0 + tw + 16 * s, ty), ImVec2(rx1, ty), ui::alpha(ui::kEdge, 0.9f), std::max(1.0f, s));
            continue;
        }
        const bool selected = i == sel;
        name_cell(c, ImVec2(rx0, y), ImVec2(split, y + rh), r.name, selected, restart_changed(r));
        const ImVec2 va(split, y), vb(rx1, y + rh);
        float t0 = 0, t1 = 0;
        row_value_cell(c, r, va, vb, selected, &t0, &t1);
        hits.add(rx0, std::max(y, top - 8 * s), rx1, std::min(y + rh, bottom + 8 * s), menu_hit::kRow, i, va.x, vb.x, t0, t1);
    }
    c.d->PopClipRect();
    ui::prompt_bar(c, prompt_list, prompts_n);   // (the prompts, on top of the help and the rows)
}

/// The tuning bar: a compact TV-style picture settings bar docked at the bottom of the screen,
/// leaving 90% of the game scene visible so visual settings can be judged live.
void draw_tune_bar(ui::Ctx& c) {
    if (cur_tab == TAB_MODS) {   // (the Mods tab has no rows of the bar's kind, its lines are the host's: its full page, the
                                 // game behind dimmed as under the full menu, which draw_menu leaves out for the bar)
        c.d->AddRectFilled(ImVec2(0, 0), ImVec2(c.w, c.h), ui::kShade);
        draw_options(c);
        return;
    }
    const float s = c.s;
    const int count = tab_row_count(cur_tab);
    if (count == 0) return;   // (#1633: a tab with no row shown: no bar, and no count to step by)
    int& sel = tab_selected[cur_tab];
    if (sel >= count) sel = count - 1;
    auto heading_at = [&](int k) { return rows[tab_row_to_global(cur_tab, k)].heading; };
    while (heading_at(sel)) sel = (sel + 1) % count;
    Row& r = rows[tab_row_to_global(cur_tab, sel)];

    const float bw = std::min(c.w - 2 * c.safe, 740 * s);
    const float bx0 = (c.w - bw) * 0.5f, bx1 = bx0 + bw;
    const float bh = 104 * s;
    const float by1 = c.h - c.safe - 16 * s;
    const float by0 = by1 - bh;

    // Dark panel with subtle edge
    c.d->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1), ui::kPanel, 6 * s);
    c.d->AddRect(ImVec2(bx0, by0), ImVec2(bx1, by1), ui::alpha(ui::kEdge, 0.9f), 6 * s, std::max(1.0f, s));

    // Header strip inside card: Tab Name and restart status
    const char* cur_name = cur_tab == TAB_MOTION ? "Motion" : tab_title(cur_tab);
    if (cur_tab == TAB_FPS_DEV) cur_name = "FPS counter details";
    if (cur_tab == TAB_RECORDINGS) cur_name = "Play recording";
    if (cur_tab == TAB_SPEAKER) cur_name = "Controller speaker";
    if (cur_tab == TAB_SIM_TURF) cur_name = "Simulated turf";
    const float header_y = by0 + 13 * s;
    ui::text_mid(c, c.f.heading, 14 * s, bx0 + 16 * s, header_y, ui::kGoldHi, cur_name);

    char tab_hint[32];
    if (c.pad) snprintf(tab_hint, sizeof tab_hint, " (LB / RB)");
    else snprintf(tab_hint, sizeof tab_hint, " (Q / E)");
    const float thw = ui::size_of(c.f.heading, 14 * s, cur_name).x;
    ui::text_mid(c, c.f.body, 12 * s, bx0 + 16 * s + thw + 4 * s, header_y, ui::kDim, tab_hint);

    if (restart_changed(r)) {
        ui::diamond(c, ImVec2(bx1 - 20 * s, header_y), 4 * s, ui::kGoldHi);
        ui::text_mid_centre(c, c.f.body, 12 * s, bx1 - 68 * s, header_y, ui::kGoldHi, "Restart needed");
    }

    // Active Row widget: Name cell and Value cell
    const float ry0 = by0 + 26 * s, ry1 = ry0 + 38 * s;
    const float rx0 = bx0 + 14 * s, rx1 = bx1 - 14 * s;
    const float split = rx0 + (rx1 - rx0) * 0.46f;
    name_cell(c, ImVec2(rx0, ry0), ImVec2(split, ry1), r.name, true, restart_changed(r));

    const ImVec2 va(split, ry0), vb(rx1, ry1);
    float t0 = 0, t1 = 0;
    row_value_cell(c, r, va, vb, true, &t0, &t1);
    hits.add(rx0, ry0, rx1, ry1, menu_hit::kRow, sel, va.x, vb.x, t0, t1);

    // Prompts line
    const char* compact_key_name = opts.compact_menu_key == 1 ? "RCtrl" : "RShift";
    ui::Prompt tune_prompts[4] = {
        {ui::kUpDown, "Setting"},
        {ui::kLeftRight, "Adjust"},
        {ui::kCompact, "Full menu", compact_key_name},
        {ui::kBack, "Close"}
    };
    const bool confirms = r.setting && r.setting->confirm;
    if (r.cheat >= 0 && cheat_confirm(r.cheat)) {
        tune_prompts[1] = {ui::kConfirm, cheat_confirm(r.cheat)};
    } else if (!r.value && r.cheat < 0) {
        tune_prompts[1] = {ui::kConfirm, confirms ? (pending == &r ? "Confirm" : r.name) : r.setting && r.setting->verb ? r.setting->verb : "Open"};
    }
    const float pw = ui::prompts(c, ImVec2(0, 0), tune_prompts, 4, false);
    const float px0 = bx0 + (bw - pw) * 0.5f;
    ui::prompts(c, ImVec2(px0, by1 - 32 * s), tune_prompts, 4, true);
}

void draw_reset_arrow(ui::Ctx& c, ImVec2 centre, ImU32 ink) {
    const float start = 4.06f, end = -0.92f;   // screen angles, leaving a gap at the top
    const float radius = 7.5f * c.s;
    const ImVec2 radial(std::cos(end), std::sin(end));
    const ImVec2 tangent(radial.y, -radial.x);
    const ImVec2 head = centre + radial * radius;
    c.d->PathArcTo(centre, radius, start, end, 20);
    c.d->PathStroke(ink, ImDrawFlags_None, std::max(1.0f, 1.8f * c.s));
    const ImVec2 base = head - tangent * (3.0f * c.s);
    const ImVec2 wing = radial * (3.3f * c.s);
    c.d->AddTriangleFilled(head + tangent * (3.3f * c.s), base + wing, base - wing, ink);
}

void draw_controls(ui::Ctx& c) {
    using enum menu_hit::CtlColumn;
    // (the page's own rows first, top_rows() of them (the layout preset, the button prompts), then a row per control and Reset to
    // defaults: one list, csel its selection)
    const float s = c.s, hh = title_banner(c, "Controls", nullptr);
    float cx0, cx1, hx0, hx1;
    bool narrow;
    columns(c, &cx0, &cx1, &hx0, &hx1, &narrow);
    column(c, cx0, cx1, hh);
    const int n = top_rows() + kControls + 1, sel_i = csel - top_rows();   // (sel_i: the control's, kControls Reset; negative: a page row)
    const int selected_col = sel_i >= 0 && sel_i < kControls && !control_column_enabled(sel_i, ccol) ? kCtlKeyboard : ccol;
    const ui::Prompt idle[] = {{ui::kUpDown, "Choose"}, {ui::kLeftRight, "Column"}, {ui::kConfirm, "Bind"}, {ui::kClear, "Clear"},
                               {ui::kBack, "Back"}};
    const ui::Prompt option[] = {{ui::kUpDown, "Choose"}, {ui::kLeftRight, "Change"}, {ui::kBack, "Back"}};
    const ui::Prompt wait[] = {{ui::kBack, "Cancel"}};
    const ui::Prompt reset[] = {{ui::kUpDown, "Choose"}, {ui::kLeftRight, "Column"}, {ui::kConfirm, "Reset"},
                                {ui::kBack, "Back"}};
    const float bar_top = ui::prompt_bar_top(c);   // (the strip itself drawn last, over all: below)
    const Row* page_row = sel_i < 0 ? &rows[tab_row_to_global(TAB_PAGE, csel)] : nullptr;
    const bool l3_sprints = opts.sprint && sel_i >= 0 && sel_i < kControls && controls[sel_i].button == HOS_SW_LS &&
                            (host->binding(HOS_BIND_PAD, HOS_KEY_SPRINT, 0) == HOS_PAD_LSTICK ||
                             host->binding(HOS_BIND_PAD, HOS_KEY_SPRINT, 1) == HOS_PAD_LSTICK) &&
                            host->binding(HOS_BIND_PAD, HOS_KEY_TOGGLE_HUD, 0) != HOS_PAD_LSTICK &&
                            host->binding(HOS_BIND_PAD, HOS_KEY_TOGGLE_HUD, 1) != HOS_PAD_LSTICK;
    const char* title = page_row ? page_row->name : sel_i < kControls ? controls[sel_i].name : "Reset to defaults";
    const char* help = listening ? (listening == 1 ? "Press the gamepad button to use. Esc cancels." : "Press the key to use. Esc cancels.")
                       : page_row ? page_row->help
                       : sel_i < kControls && selected_col == kCtlReset
                           ? (blocked_reset_row == sel_i
                                  ? "Another control uses one of these defaults. Clear that binding first."
                                  : "Restore this control's default gamepad and keyboard bindings and its default Toggle (on for "
                                    "Sprint, off for the others).")
                       : sel_i < kControls && controls[sel_i].button == HOS_KEY_SPRINT && selected_col == kCtlToggle
                           ? "Toggle for Sprint: On, a press of its key or button starts Sprint and the next ends it; Off, Link "
                             "sprints only while you hold it. A toggled sprint ends when Link stops, attacks, targets with ZL, uses "
                             "an item, talks, swims or climbs; a roll keeps it, and opening Insert, pausing or losing focus does not "
                             "end it. On by default."
                       : sel_i < kControls && controls[sel_i].button == HOS_KEY_SPRINT
                           ? "Enable Sprint in the Controls tab to sprint without rolling. With Toggle off, hold the assigned input "
                             "while moving. With Toggle on, press it to start or stop. Enabling Sprint moves L3 to Sprint if Sprint "
                             "has no gamepad input and L3 is still the primary Switch left-stick-press binding."
                       : sel_i < kControls && controls[sel_i].button < HOS_SW_N && controls[sel_i].button != HOS_SW_PLUS &&
                         controls[sel_i].button != HOS_SW_MINUS && ccol == kCtlToggle
                           ? "Hold-to-toggle: press once to hold this Switch button, again to release it. Either gamepad input or a "
                             "bound key works. Opening Insert, pressing Plus or Minus, focus loss or rebinding releases it. Off by default."
                       : l3_sprints
                           ? "L3 is assigned to Sprint. Use the inputs shown on this row to press the Switch left stick. "
                             "Enter binds the selected input and Delete clears it."
                       : "Either gamepad input or an assigned key presses the selected Switch button. Enter binds the "
                             "selected input and Delete clears it. Confirming a conflict removes the previous binding. "
                             "The game's button icons follow after a restart.";
    const char* hint = page_row ? (page_row->hint ? page_row->hint(*page_row->value, -1) : nullptr)
                                : "Gamepad: the D-pad moves, A binds, B goes back. Ocarina note keys are only notes while "
                                  "the ocarina waits for one; away from it they do their other jobs.";
    float bottom = std::min(c.h - c.safe, bar_top) - 10 * s;   // (the rows end above the prompts)
    if (narrow) {
        bottom = std::min(bar_top, c.h - c.safe) - 200 * s;
        help_box(c, hx0, hx1, bottom + 8 * s, nullptr, help, hint, nullptr, 0, page_row && restart_changed(*page_row), bottom + 8 * s,
                 bar_top - 10 * s);
    } else {
        help_box(c, hx0, hx1, kHelpCentre, title, help, hint, nullptr, 0, page_row && restart_changed(*page_row),
                 hh + 18 * s, bar_top - 12 * s);
    }
    const float rx0 = cx0 + 44 * s, rx1 = cx1 - 44 * s, width = rx1 - rx0;
    const float rh = 36 * s, pitch = 42 * s, gutter = 3 * s;
    const float action_gap = std::min(8 * s, width * 0.015f);
    const float reset_side = std::min(rh, width * 0.105f);
    const float reset_x = rx1 - reset_side;
    const float bindings_width = reset_x - gutter - action_gap - rx0;
    const float pad_x = rx0 + bindings_width * (0.27f / 0.895f);
    const float key_x = rx0 + bindings_width * (0.59f / 0.895f);
    const float toggle_x = rx0 + bindings_width * (0.79f / 0.895f) + action_gap;
    const float col_x[] = {pad_x, (pad_x + key_x) * 0.5f, key_x, toggle_x, reset_x - gutter, rx1};
    const float top = hh + 22 * s, header_height = 43 * s;
    int shown = 0;
    const float first = scroll_rows(kControlsSlot, n, csel, top + header_height, bottom, pitch, &shown);
    const float header_top = top + std::max(0.0f, top_rows() - first) * pitch;
    const float bindings_clip_top = header_top + header_height - 8 * s;
    scroll_bar(c, cx0 + 20 * s, top, top + header_height + shown * pitch - (pitch - rh), n, shown, first);
    c.d->PushClipRect(ImVec2(cx0, top - 8 * s), ImVec2(cx1, bottom + 8 * s));
    ui::text_mid_centre(c, c.f.body, 15 * s, col_x[kCtlAlternate], header_top + 8 * s, ui::kDim, "Gamepad");
    ui::text_mid(c, c.f.body, 15 * s, rx0 + 20 * s, header_top + 27 * s, ui::kDim, "Button");
    const char* titles[kCtlReset] = {"Input 1", "Input 2", "Keyboard", "Toggle"};
    for (int col = kCtlPrimary; col < kCtlReset; col++) {
        const float left = col_x[col];
        const float right = col_x[col + 1] - (col == kCtlKeyboard ? action_gap : 0.0f);
        const auto title = fit(c, c.f.body, 15 * s, right - left - 2 * gutter, i18n::tr(titles[col]), false);
        ui::text_mid_centre(c, c.f.body, 15 * s, (left + right) * 0.5f, header_top + 27 * s, ui::kDim, title.c_str());
    }
    for (int j = 0; j < n; j++) {
        const int i = j - top_rows();
        if (i == 0) c.d->PushClipRect(ImVec2(cx0, bindings_clip_top), ImVec2(cx1, bottom + 8 * s), true);
        const float clip_top = i < 0 ? top - 8 * s : bindings_clip_top;
        const float y = top + (j - first) * pitch + (i < 0 ? 0.0f : header_height);
        if (y + rh <= clip_top || y >= bottom + 8 * s) continue;
        const bool sel = j == csel;
        const float y0 = std::max(y, clip_top), y1 = std::min(y + rh, bottom + 8 * s);
        if (i < 0) {   // (one of the page's own rows: a name and a choice, as on the tabs)
            const Row& r = rows[tab_row_to_global(TAB_PAGE, j)];
            // (their names wider than a control's, as wide as the tabs' rows': "Button prompts" whole at any size)
            const float opt_x = rx0 + (rx1 - rx0) * 0.5f;
            name_cell(c, ImVec2(rx0, y), ImVec2(opt_x, y + rh), r.name, sel, restart_changed(r), 17);
            choice_cell(c, ImVec2(opt_x, y), ImVec2(rx1, y + rh), r.text(*r.value), sel, *r.value > r.lo, *r.value < r.hi);
            hits.add(rx0, y0, rx1, y1, menu_hit::kCtlOption, j, opt_x, rx1);
            continue;
        }
        if (i == kControls) {
            const float end = col_x[kCtlKeyboard] - 3 * s;
            choice_cell(c, ImVec2(rx0, y), ImVec2(end, y + rh), i18n::tr("Reset to defaults"), sel, false, false);
            hits.add(rx0, y0, end, y1, menu_hit::kCtlRow, i);
            continue;
        }
        const Control& ctl = controls[i];
        name_cell(c, ImVec2(rx0, y), ImVec2(pad_x, y + rh), ctl.name, sel, false, 17, false);   // (its cells stand apart)
        hits.add(rx0, y0, pad_x, y1, menu_hit::kCtlRow, i);
        for (int col = kCtlPrimary; col < kCtlColumns; col++) {
            const bool keyboard = col == kCtlKeyboard, toggle = col == kCtlToggle;
            const bool disabled = !control_column_enabled(i, col);
            if (col == kCtlReset) {
                const float reset_y = y + (rh - reset_side) * 0.5f;
                const ImVec2 va(reset_x, reset_y), vb(rx1, reset_y + reset_side);
                const bool here = sel && !disabled && selected_col == kCtlReset;
                choice_cell(c, va, vb, "", here, false, false, disabled);
                draw_reset_arrow(c, (va + vb) * 0.5f, here ? ui::kGoldInk : disabled ? ui::kDim : ui::kInk);
                const float hit_top = std::max(y0, va.y), hit_bottom = std::min(y1, vb.y);
                if (!disabled && hit_top < hit_bottom)
                    hits.add(va.x, hit_top, vb.x, hit_bottom, menu_hit::kCtlCell, i, 0, 0, 0, 0, col);
                continue;
            }
            const float a = col_x[col] + (col == kCtlPrimary ? 0 : gutter);
            const float b = col_x[col + 1] - gutter - (keyboard ? action_gap : 0.0f);
            const std::string v = disabled ? "-" : keyboard ? keys_text(ctl.button) : toggle
                ? tx_onoff(toggle_on(ctl.button)) : pad_name(host->binding(HOS_BIND_PAD, ctl.button, col));
            const bool here = sel && !disabled && col == selected_col;
            const ImVec2 va(a, y), vb(b, y + rh);
            if (here && listening) {   // (waiting for the new input: amber, the outline as a selected value's)
                const float in = 4 * s;
                c.d->AddRectFilled(va, vb, ui::kCellDark);
                c.d->AddRect(va, vb, ui::kEdge, 0, std::max(1.0f, s));
                c.d->AddRectFilled(va + ImVec2(in, in), vb - ImVec2(in, in), ui::kListen);
                const std::string prompt = fit(c, c.f.strong, 16.5f * s, b - a - 2 * in, keyboard ? "Press key" : "Press button", false);
                ui::text_mid_centre(c, c.f.strong, 16.5f * s, (a + b) * 0.5f, y + rh * 0.5f, ui::kInkBright, prompt.c_str());
                ui::glow(c, va, vb);
            } else {
                choice_cell(c, va, vb, v, here, false, false, disabled);
            }
            if (!disabled) hits.add(a, y0, b, y1, menu_hit::kCtlCell, i, 0, 0, 0, 0, col);
        }
    }
    c.d->PopClipRect();
    c.d->PopClipRect();
    (void)(listening ? ui::prompt_bar(c, wait, 1) : sel_i < 0 ? ui::prompt_bar(c, option, 3) :
           sel_i == kControls || selected_col == kCtlReset ? ui::prompt_bar(c, reset, 4) : ui::prompt_bar(c, idle, 5));
}

/// A small square page arrow at a (its size side): dir 0 back, 1 on; dim when it cannot go that way
void page_arrow(ui::Ctx& c, ImVec2 a, float side, int dir, bool can) {
    const float s = c.s;
    const ImVec2 b = a + ImVec2(side, side);
    c.d->AddRectFilled(a, b, can ? ui::kCellName : ui::alpha(ui::kCellName, 0.4f), 3 * s);
    c.d->AddRect(a, b, can ? ui::kEdge : ui::alpha(ui::kEdge, 0.4f), 3 * s, std::max(1.0f, s));
    ui::triangle(c, (a + b) * 0.5f, 10 * s, dir, can ? ui::kInkBright : ui::kDim);
}

/// A tag at the right end of a card (its right edge x, its middle y): the text on an accent plate; its width
float card_tag(ui::Ctx& c, float x, float y, const char* tag, bool draw = true) {
    const float s = c.s, tw = ui::size_of(c.f.strong, 13 * s, tag).x + 14 * s;
    if (draw) {
        const ImVec2 g(x - tw, y - 10 * s);
        c.d->AddRectFilled(g, g + ImVec2(tw, 20 * s), ui::kAccent, 2 * s);
        ui::text_mid(c, c.f.strong, 13 * s, g.x + 7 * s, y, ui::kGoldInk, tag);
    }
    return tw;
}

void draw_inventory(ui::Ctx& c) {
    const float s = c.s;
    const int begin = inventory_begin(), end = inventory_end();
    bool loaded = false;   // (a save's counts reported: -1 for all until the game side has a save to read)
    for (int i = 0; i < kItems && !loaded; ++i) loaded = item_counts[i].load(std::memory_order_relaxed) >= 0;
    char count_text[48];
    snprintf(count_text, sizeof count_text, "%d items", end - begin);
    const float hh = title_banner(c, "Inventory", count_text);
    const int have = item_counts[item_choice].load(std::memory_order_relaxed);
    const char* edit = item_kinds[item_choice] ? (inventory_amount ? "Done" : "Edit amount") : (have ? "Relock" : "Unlock");
    ui::Prompt prompts[5] = {{ui::kUpDown, "Move"}, {ui::kLeftRight, inventory_amount ? "Amount" : "Move"}, {ui::kTabs, "Section"}};
    int prompts_n = 3;
    if (have >= 0) prompts[prompts_n++] = {ui::kConfirm, edit};
    prompts[prompts_n++] = {ui::kBack, inventory_amount ? "Done" : "Back"};
    float cx0, cx1, hx0, hx1;
    bool narrow;
    columns(c, &cx0, &cx1, &hx0, &hx1, &narrow);
    column(c, cx0, cx1, hh);
    const float bar = ui::prompt_bar_top(c);   // (the strip itself drawn last, over all: below)
    const float left = cx0 + 24 * s, right = cx1 - 24 * s;
    const float gap = 10 * s;
    // the sections, as tabs over the grid (Q and E, or a click)
    // (on one line: their padding, then their text, made smaller as far as needed for the column's width)
    float tpx = 16 * s, tpad = 26 * s, tgap = 8 * s;
    auto tabs_w = [&] {
        float w = 0;
        for (int i = 0; i <= kItemSections; ++i) w += ui::size_of(c.f.strong, tpx, i ? item_sections.s[i - 1].name : "All").x + tpad + tgap;
        return w - tgap;
    };
    if (tabs_w() > right - left) tpad = 16 * s, tgap = 6 * s;
    if (const float w = tabs_w(); w > right - left) {
        const float text = w - (kItemSections + 1) * tpad - kItemSections * tgap;
        tpx = std::max(12 * s, tpx * (right - left - (w - text)) / text);
    }
    float x = left, y = hh + 18 * s;
    for (int i = 0; i <= kItemSections; ++i) {
        const char* label = i ? item_sections.s[i - 1].name : "All";
        const float w = ui::size_of(c.f.strong, tpx, label).x + tpad;
        if (x > left && x + w > right) x = left, y += 36 * s;
        const ImVec2 a(x, y), b(std::min(x + w, right), y + 30 * s);
        const bool active = i == inventory_filter;
        if (active) ui::gold(c, a, b);
        else c.d->AddRectFilled(a, b, ui::kCellName, 4 * s);
        c.d->AddRect(a, b, active ? ui::kGoldHi : ui::kEdge, 4 * s, std::max(1.0f, s));
        ui::text_mid_centre(c, c.f.strong, tpx, (a.x + b.x) * .5f, y + 15 * s, active ? ui::kGoldInk : ui::kInkBright, label);
        hits.add(a.x, a.y, b.x, b.y, menu_hit::kRow, i);
        x += w + tgap;
    }
    float top = y + 44 * s;
    const float pitch = (loaded ? 62 : 46) * s;   // (the title screen: the names alone, no line under them)
    if (!loaded) {   // (the title screen: one note for the page, not "Load a save" on every card)
        ui::diamond(c, ImVec2(left + 5 * s, top - 2 * s), 4 * s, ui::kGoldHi);
        ui::text_mid(c, c.f.body, 16 * s, left + 18 * s, top - 2 * s, ui::kSoft, "Load a save to edit the inventory.");
        top += 30 * s;
    }
    const float strip = 58 * s, detail = bar - 14 * s - strip, grid_bottom = std::max(top, detail - 8 * s);
    const char* help = have < 0 ? "Load a save to edit this item." : item_kinds[item_choice] ?
        "Choose Edit amount, then use Left and Right to change the quantity." : "Unlock or relock this item.";
    if (!narrow)
        help_box(c, hx0, hx1, std::max(hh + 26 * s, c.h * .30f), item_names[item_choice], help, nullptr, nullptr, 0, false,
                 hh + 18 * s, bar - 12 * s);
    // (as many columns as fit the longest name, at most 5; whole pages of cards, the range and the page under them)
    static float name_w = 0, name_s = 0;
    if (name_s != s) {
        name_s = s, name_w = 0;
        for (int i = 0; i < kItems; ++i) name_w = std::max(name_w, ui::size_of(c.f.strong, 15.5f * s, item_names[i]).x);
    }
    inventory_columns = std::clamp((int)((right - left + gap) / std::max(name_w + 26 * s + gap, 150 * s)), 1, 5);
    inventory_capacity = inventory_columns * std::max(1, (int)((grid_bottom - top + gap) / pitch));
    const int pages_n = std::max(1, (end - begin + inventory_capacity - 1) / inventory_capacity);
    inventory_page = std::clamp(inventory_page, 0, pages_n - 1);
    const int first = begin + inventory_page * inventory_capacity;
    const float width = (right - left + gap) / inventory_columns;
    c.d->PushClipRect(ImVec2(left - 12 * s, top - 12 * s), ImVec2(right + 12 * s, grid_bottom + 12 * s));
    for (int i = first; i < std::min(end, first + inventory_capacity); ++i) {
        const int cell = i - first;
        const ImVec2 a(left + (cell % inventory_columns) * width, top + (cell / inventory_columns) * pitch);
        const ImVec2 b(a.x + width - gap, a.y + pitch - gap);
        const int count = item_counts[i].load(std::memory_order_relaxed);
        const bool selected = i == item_choice, owned = count > 0, editing = selected && inventory_amount;
        c.d->AddRectFilled(a, b, owned ? ui::kCellNameSel : ui::kCellName, 4 * s);
        c.d->AddRect(a, b, owned ? ui::kAccent : ui::alpha(ui::kEdge, count < 0 ? 0.5f : 1.0f), 4 * s, std::max(1.0f, s));
        if (owned) c.d->AddRectFilled(a + ImVec2(0, 4 * s), ImVec2(a.x + 3 * s, b.y - 4 * s), ui::kAccent);
        c.d->PushClipRect(a + ImVec2(8 * s, 0), b - ImVec2(8 * s, 0), true);
        ui::text_mid(c, c.f.strong, 15.5f * s, a.x + 13 * s, loaded ? a.y + 17 * s : (a.y + b.y) * 0.5f, count < 0 ? ui::kDim : ui::kInkBright, item_names[i]);
        char amount[40];
        if (count < 0) snprintf(amount, sizeof amount, "%s", loaded ? "Unavailable" : "");
        else if (item_kinds[i]) snprintf(amount, sizeof amount, "Quantity %d", count);
        else snprintf(amount, sizeof amount, "%s", count ? "Unlocked" : "Locked");
        const float ly = a.y + 37 * s;
        if (editing) {   // (the amount being changed: its arrows, gold)
            ui::triangle(c, ImVec2(a.x + 18 * s, ly), 8 * s, 0, ui::kGoldHi);
            ui::text_mid(c, c.f.strong, 15 * s, a.x + 28 * s, ly, ui::kGoldHi, amount);
            ui::triangle(c, ImVec2(a.x + 38 * s + ui::size_of(c.f.strong, 15 * s, amount).x, ly), 8 * s, 1, ui::kGoldHi);
        } else {
            ui::text_mid(c, c.f.body, 15 * s, a.x + 13 * s, ly, owned ? ui::kAccent : ui::kDim, amount);
        }
        c.d->PopClipRect();
        if (selected) ui::glow(c, a, b);
        if (a.y < grid_bottom) hits.add(a.x, a.y, b.x, std::min(b.y, grid_bottom), menu_hit::kListLine, i);
    }
    c.d->PopClipRect();
    if (detail >= top) {   // (the page: its arrows, Page n of m and the range; the selected item's action at the right)
        c.d->AddLine(ImVec2(left, detail), ImVec2(right, detail), ui::alpha(ui::kEdge, 0.8f));
        const int pages = (end - begin + inventory_capacity - 1) / inventory_capacity, at = (first - begin) / inventory_capacity;
        const float my = detail + strip * 0.5f + 2 * s, side = 32 * s;
        float px0 = left;
        if (pages > 1) {
            page_arrow(c, ImVec2(px0, my - side * 0.5f), side, 0, at > 0);
            hits.add(px0, my - side * 0.5f, px0 + side, my + side * 0.5f, menu_hit::kButton, 2);
            char page_text[32];
            snprintf(page_text, sizeof page_text, "Page %d of %d", at + 1, pages);
            const float tw = ui::size_of(c.f.strong, 17 * s, page_text).x;
            ui::text_mid(c, c.f.strong, 17 * s, px0 + side + 12 * s, my, ui::kInkBright, page_text);
            const float nx = px0 + side + 24 * s + tw;
            page_arrow(c, ImVec2(nx, my - side * 0.5f), side, 1, at + 1 < pages);
            hits.add(nx, my - side * 0.5f, nx + side, my + side * 0.5f, menu_hit::kButton, 3);
            px0 = nx + side + 18 * s;
        }
        char range[64];
        snprintf(range, sizeof range, "%d–%d of %d", first - begin + 1, std::min(end, first + inventory_capacity) - begin, end - begin);
        ui::text_mid(c, c.f.body, 15 * s, px0, my, ui::kSoft, range);
        if (have >= 0) {
            const float bw = ui::size_of(c.f.strong, 18 * s, "Edit amount").x + 36 * s;
            const ImVec2 a(right - bw, my - 17 * s), b(right, my + 17 * s);
            ui::button(c, a, b, edit, false);
            hits.add(a.x, a.y, b.x, b.y, menu_hit::kButton, 0);
            if (inventory_amount) {
                char amount[32];
                snprintf(amount, sizeof amount, "%d", have);
                const ImVec2 av(a.x - 118 * s, a.y), bv(a.x - 10 * s, b.y);
                choice_cell(c, av, bv, amount, true, true, true);
                hits.add(av.x, av.y, bv.x, bv.y, menu_hit::kButton, 1, av.x, bv.x);
            }
        }
    }
    ui::prompt_bar(c, prompts, prompts_n);   // (the prompts, on top of the help and the cards)
}

void draw_teleport(ui::Ctx& c) {
    const float s = c.s;
    char count[32];
    snprintf(count, sizeof count, "%d of %d", teleport_choice + 1, kTeleports);
    const float hh = title_banner(c, "Teleport", count);
    float cx0, cx1, hx0, hx1;
    bool narrow;
    columns(c, &cx0, &cx1, &hx0, &hx1, &narrow);
    column(c, cx0, cx1, hh);
    const ui::Prompt prompts[] = {{ui::kUpDown, "Move"}, {ui::kLeftRight, "Move"}, {ui::kTabs, "Section"},
        {ui::kConfirm, "Travel"}, {ui::kBack, "Back"}};
    const float bar = ui::prompt_bar_top(c);   // (the strip itself drawn last, over all: below)
    const float left = cx0 + 24 * s, right = cx1 - 24 * s, top = hh + 16 * s;
    const float strip = 58 * s, detail = bar - 14 * s - strip;
    const float bottom = std::max(top, detail - 6 * s), gap = 10 * s, pitch = 48 * s, head = 40 * s;
    const char* const note = teleport_notes[teleport_choice];
    if (!narrow)
        help_box(c, hx0, hx1, std::max(hh + 26 * s, c.h * .30f), teleport_names[teleport_choice],
                 "Select a destination, then Travel (or Enter) to go to its map entrance. Link keeps his current age.", note, nullptr, 0,
                 false, hh + 18 * s, bar - 12 * s);
    // (as many columns as fit the longest name and its tag, at most 4; each section under its heading, the cards scrolled to
    // keep the selected one in view)
    static float card_w = 0, card_s = 0;
    if (card_s != s) {
        card_s = s, card_w = 0;
        for (int i = 0; i < kTeleports; ++i)
            card_w = std::max(card_w, ui::size_of(c.f.body, 17 * s, teleport_names[i]).x +
                                          (teleport_notes[i] ? card_tag(c, 0, 0, teleport_notes[i], false) + 12 * s : 0));
    }
    teleport_columns = std::clamp((int)((right - left + gap) / (card_w + 36 * s + gap)), 1, 4);
    const int cols = teleport_columns;
    const float width = (right - left + gap) / cols;
    // (the y of each section's heading and first row, from the top of the list)
    float section_y[kTeleportSections + 1];
    float total = 0;
    for (int k = 0; k < kTeleportSections; ++k) {
        section_y[k] = total;
        const int n = teleport_section_end(k) - teleport_sections[k].first;
        total += head + (n + cols - 1) / cols * pitch + (k + 1 < kTeleportSections ? 10 * s : 0);
    }
    section_y[kTeleportSections] = total;
    auto card_y = [&](int i) {
        const int k = teleport_section_of(i);
        return section_y[k] + head + (i - teleport_sections[k].first) / cols * pitch;
    };
    const float view = bottom - top;
    teleport_page_rows = std::max(1, (int)(view / pitch) - 1);
    float& at = teleport_scroll_px;
    float& want = teleport_want_px;
    if (teleport_wheel) want -= teleport_wheel * pitch, teleport_wheel = 0;   // (the wheel: the cards move, the choice stays)
    if (total <= view) want = 0;
    else if (!teleport_follow) want = std::clamp(want, 0.0f, total - view);
    else {
        const int k = teleport_section_of(teleport_choice);
        const bool first_row = teleport_choice - teleport_sections[k].first < cols;
        const float y0 = first_row ? section_y[k] : card_y(teleport_choice) - pitch * 0.5f, y1 = card_y(teleport_choice) + pitch * 1.5f;
        if (y0 < want) want = y0;
        if (y1 > want + view) want = y1 - view;
        want = std::clamp(want, 0.0f, total - view);
    }
    at += (want - at) * ease_step;
    if (std::fabs(want - at) < 0.5f) at = want;
    if (total > view) scroll_bar(c, cx0 + 12 * s, top, bottom, (int)total, (int)view, at);
    c.d->PushClipRect(ImVec2(left - 12 * s, top), ImVec2(right + 12 * s, bottom), true);
    for (int k = 0; k < kTeleportSections; ++k) {
        const float hy = top + section_y[k] - at + head * 0.5f;
        if (hy > bottom + head || hy < top - head) continue;
        ui::text_mid(c, c.f.heading, 20 * s, left, hy, ui::kInkBright, teleport_sections[k].name);
        const float tw = ui::size_of(c.f.heading, 20 * s, teleport_sections[k].name).x;
        char n_text[16];
        snprintf(n_text, sizeof n_text, "%d", teleport_section_end(k) - teleport_sections[k].first);
        const float nw = ui::size_of(c.f.body, 15 * s, n_text).x;
        ui::text_mid(c, c.f.body, 15 * s, right - nw, hy, ui::kSoft, n_text);
        c.d->AddLine(ImVec2(left + tw + 14 * s, hy + s), ImVec2(right - nw - 12 * s, hy + s), ui::alpha(ui::kAccent, 0.45f),
                     std::max(1.0f, s));
    }
    for (int i = 0; i < kTeleports; ++i) {
        const int k = teleport_section_of(i), cell = i - teleport_sections[k].first;
        const ImVec2 a(left + cell % cols * width, top + card_y(i) - at);
        const ImVec2 b(a.x + width - gap, a.y + pitch - 8 * s);
        if (b.y < top || a.y > bottom) continue;
        const bool selected = i == teleport_choice;
        c.d->AddRectFilled(a, b, selected ? ui::kCellNameSel : ui::kCellName, 3 * s);
        c.d->AddRect(a, b, ui::kEdge, 3 * s, std::max(1.0f, s));
        if (selected) c.d->AddRectFilled(a, ImVec2(a.x + 4 * s, b.y), ui::kGoldHi);
        float name_end = b.x - 10 * s;
        if (teleport_notes[i]) name_end -= card_tag(c, b.x - 8 * s, (a.y + b.y) * 0.5f, teleport_notes[i]) + 8 * s;
        c.d->PushClipRect(a, ImVec2(name_end, b.y), true);
        ui::text_mid(c, selected ? c.f.strong : c.f.body, 17 * s, a.x + 16 * s, (a.y + b.y) * 0.5f, selected ? ui::kInkBright : ui::kInk,
                     teleport_names[i]);
        c.d->PopClipRect();
        if (selected) ui::glow(c, a, b);
        hits.add(a.x, std::max(a.y, top), b.x, std::min(b.y, bottom), menu_hit::kListLine, i);
    }
    c.d->PopClipRect();
    if (detail >= top) {   // (the selection, its section and note, and Travel: what Enter does)
        c.d->AddLine(ImVec2(left, detail), ImVec2(right, detail), ui::alpha(ui::kEdge, 0.8f));
        const float my = detail + strip * 0.5f + 2 * s;
        const float bw = ui::size_of(c.f.strong, 20 * s, "Travel").x + 56 * s;
        const ImVec2 a(right - bw, my - 18 * s), b(right, my + 18 * s);
        c.d->PushClipRect(ImVec2(left, detail), ImVec2(a.x - 12 * s, detail + strip), true);
        ui::text_mid(c, c.f.strong, 18 * s, left, my - 10 * s, ui::kInkBright, teleport_names[teleport_choice]);
        char where[64];
        snprintf(where, sizeof where, note ? "%s · %s" : "%s", teleport_sections[teleport_section_of(teleport_choice)].name, note);
        ui::text_mid(c, c.f.body, 15 * s, left, my + 12 * s, ui::kAccent, where);
        c.d->PopClipRect();
        ui::button(c, a, b, "Travel", false);
        hits.add(a.x, a.y, b.x, b.y, menu_hit::kButton, 0);
    }
    ui::prompt_bar(c, prompts, 5);   // (the prompts, on top of the help and the cards)
}

void draw_quick_debug(ui::Ctx& c) {
    const float s = c.s, width = std::min(1040 * s, c.w - 2 * c.safe);
    const float left = (c.w - width) * .5f, right = left + width;
    const float top = std::max(c.safe, c.h * .10f), bottom = std::min(c.h - c.safe, top + 540 * s);
    c.d->AddRectFilled(ImVec2(left, top), ImVec2(right, bottom), ui::kCellDark);
    c.d->AddRect(ImVec2(left, top), ImVec2(right, bottom), ui::kEdge);
    ui::text_mid(c, c.f.heading, 23 * s, left + 24 * s, top + 25 * s, ui::kGoldHi, "Debug Tools");
    draw_debug_rows(c, left, right, top + 52 * s, bottom - 12 * s, tab_selected[TAB_DEBUG]);
    const ui::Prompt prompts[] = {{ui::kUpDown, "Choose"}, {ui::kLeftRight, "Change"},
        {ui::kConfirm, "Select"}, {ui::kBack, "Close"}};
    ui::prompt_bar(c, prompts, 4);
}

void draw_confirmation(ui::Ctx& c, const char* title, const char* message, const char* accept, int choice) {
    const float s = c.s, width = std::min(600 * s, c.w - 2 * c.safe);
    const float body = ui::para(c, c.f.body, 18 * s, {}, ui::kInk, message, width - 48 * s, false);
    const float height = std::max(234 * s, body + 142 * s);
    const ImVec2 a((c.w - width) * .5f, (c.h - height) * .5f), b = a + ImVec2(width, height);
    c.d->AddRectFilled(a, b, ui::kCellDark);
    c.d->AddRect(a, b, ui::kEdge, 0, 2 * s);
    c.d->AddRectFilled(a, ImVec2(b.x, a.y + 48 * s), ui::kBanner);
    ui::text_mid_centre(c, c.f.heading, 23 * s, (a.x + b.x) * .5f, a.y + 24 * s, ui::kBannerInk, title);
    ui::para(c, c.f.body, 18 * s, a + ImVec2(24 * s, 62 * s), ui::kInk, message, width - 48 * s);
    const char* labels[] = {"Cancel", accept};
    const float button_w = (width - 60 * s) / 2;
    for (int i = 0; i < 2; ++i) {
        const ImVec2 p(a.x + 24 * s + i * (button_w + 12 * s), b.y - 66 * s);
        const ImVec2 q = p + ImVec2(button_w, 42 * s);
        ui::button(c, p, q, labels[i], i == choice);
        hits.add(p.x, p.y, q.x, q.y, menu_hit::kButton, i);
    }
}

void draw_debug_warning(ui::Ctx& c) {
    draw_confirmation(c, "Enable Debug Tools?",
                      "Debug tools can change your progress. Use a backup save. Enabling Debug tools turns off autosave. "
                      "You can turn it back on in the Debug tab.", "Enable Debug Tools", debug_warning_choice);
}

void draw_control_confirmation(ui::Ctx& c) {
    const auto& ask = control_confirmation;
    const char* title = "Reset bindings?", *accept = "Reset";
    std::string message;
    if (ask.action == ControlAction::ResetAll) {
        message = i18n::tr("Restore the default bindings and toggles for all controls?");
    } else {
        const char* target = i18n::tr(controls[ask.row].name);
        char text[2048];
        if (ask.action == ControlAction::ResetRow) {
            snprintf(text, sizeof text, i18n::tr("Restore the default bindings and toggle for %s?"), target);
        } else {
            title = "Binding already in use";
            accept = "Rebind";
            const char* input = i18n::tr(ask.set == HOS_BIND_PAD ? pad_name(ask.code) : key_name(ask.code));
            std::string names;
            for (const auto& control : controls) {
                if (!(ask.conflicts >> control.button & 1)) continue;
                if (!names.empty()) names += ", ";
                names += i18n::tr(control.name);
            }
            snprintf(text, sizeof text, i18n::tr("%s is already bound to %s. Remove that assignment and bind it to %s?"),
                     input, names.c_str(), target);
        }
        message = text;
    }
    c.d->AddRectFilled(ImVec2(0, 0), ImVec2(c.w, c.h), ui::rgba(0, 0, 0, 150));
    hits.clear(0, 0, c.w, c.h);
    draw_confirmation(c, title, message.c_str(), accept, ask.choice);
}

void rebind_control(int row, int set, int slot, int code, uint64_t approved = 0) {
    if (host->version < 43 || !host->rebind_control) return;
    uint64_t conflicts = 0;
    if (host->rebind_control(set, controls[row].button, slot, code, approved, &conflicts)) {
        if (set == HOS_BIND_PAD) opts.pad_layout = detect_pad_layout();
        save();
    } else if (conflicts) control_confirmation = {ControlAction::Rebind, row, set, slot, code, 0, conflicts};
}

void draw_ask(ui::Ctx& c) {
    const float s = c.s, W = 560 * s, H = 232 * s;
    const ImVec2 p((c.w - W) * 0.5f, (c.h - H) * 0.5f), q = p + ImVec2(W, H);
    ui::shadow(c, p, q, 30 * s, 1.0f, 8 * s);
    c.d->AddRectFilled(p, q, ui::kBox, ui::kRound * 1.5f * s);
    c.d->AddRect(p, q, ui::kBoxEdge, ui::kRound * 1.5f * s, 0, 2 * s);
    const float hb = 50 * s;   // (a tan title strip, as the banner, its title in dark ink)
    c.d->AddRectFilledMultiColor(p, ImVec2(q.x, p.y + hb), ui::kBannerTop, ui::kBannerTop, ui::kBanner, ui::kBanner);
    c.d->AddRectFilled(ImVec2(p.x, p.y + hb - 3 * s), ImVec2(q.x, p.y + hb), ui::kBannerEdge);
    const bool preview = output_preview_countdown > 0;
    ui::text_mid_centre(c, c.f.heading, 23 * s, (p.x + q.x) * 0.5f, p.y + hb * 0.5f, ui::kBannerInk,
                       preview ? "Keep changes?" : "Restart Needed");
    if (preview) {
        char countdown[160];
        snprintf(countdown, sizeof countdown, i18n::tr("Restoring in %d seconds."), output_preview_countdown);
        ui::para(c, c.f.body, 19 * s, ImVec2(p.x + 28 * s, p.y + 64 * s), ui::kInk, countdown, W - 56 * s);
    } else {
        const std::string ask = i18n::tr("Restart to apply ") + restart_list(i18n::tr(" and ")) + "?";
        ui::para(c, c.f.body, 19 * s, ImVec2(p.x + 28 * s, p.y + 64 * s), ui::kInk, ask.c_str(), W - 56 * s);
        ui::text_mid(c, c.f.body, 16 * s, p.x + 28 * s, p.y + 124 * s, ui::kSoft, "Progress since your last save is lost.");
    }
    const float bw = 210 * s, bh = 46 * s, gap = 24 * s, by = p.y + 160 * s, x0 = (p.x + q.x) * 0.5f - bw - gap * 0.5f;
    const char* labels[2] = {preview ? "No" : "Later", preview ? "Yes" : "Restart Now"};
    for (int i = 0; i < 2; i++) {
        const ImVec2 a(x0 + i * (bw + gap), by), b = a + ImVec2(bw, bh);
        ui::button(c, a, b, labels[i], i == ask_sel);
        hits.add(a.x, a.y, b.x, b.y, menu_hit::kButton, i);
    }
    const ui::Prompt prompt_list[] = {{ui::kLeftRight, "Choose"}, {ui::kConfirm, "Confirm"}, {ui::kBack, preview ? "No" : "Later"}};
    ui::prompt_bar(c, prompt_list, 3);
}

void draw_menu(ui::Ctx& c);
/// The menu's text sizes at this window's unit, baked ahead by vk_imgui.cpp a few a frame: the first open then draws with no
/// glyphs to make (it took 2.6 ms at 1080p)
float prebaked[2] = {0, 0};   // (the unit and the width prebake last did)
/// The menu's unit now for a window w x h: whether prebake has not done it yet
bool prebake_needed(uint32_t w, uint32_t h) {
    float m = 0;
    return ui::unit((float)h, opts.menu_size, &m) != prebaked[0] || (float)w != prebaked[1];
}
void prebake(const ui::Ctx& c) {
    if (c.s == prebaked[0] && c.w == prebaked[1]) return;
    prebaked[0] = c.s, prebaked[1] = c.w;
    static const float points[] = {12, 12.5f, 13, 13.6f, 14, 14.28f, 14.5f, 15, 15.5f, 16, 16.5f, 17, 18, 18.5f, 19, 20, 21, 23, 24};
    constexpr int n = sizeof points / sizeof points[0];
    float px[n];
    for (int i = 0; i < n; i++) px[i] = points[i] * c.s;
    ui::prebake(px, n);
    if (open_now.load(std::memory_order_relaxed)) return;
    // (and the page itself drawn once into a list of its own, thrown away: what the first open would measure and bake (the
    // help's height over every row, the exact sizes) done now, the menu's state as it was)
    const menu_hit::Map saved_hits = hits;
    const int saved_marquee = marquee_max;
    const int saved_tele[2] = {teleport_columns, teleport_page_rows}, saved_inv[3] = {inventory_columns, inventory_capacity, inventory_page};
    const float saved_tele_px[2] = {teleport_scroll_px, teleport_want_px};
    const float saved_ease = ease_step;
    float saved_scroll[sizeof scroll_at / sizeof *scroll_at], saved_want[sizeof scroll_at / sizeof *scroll_at];
    int saved_to[sizeof scroll_at / sizeof *scroll_at];   // (the scratch drawing's scroll, given back: the next is the menu's)
    memcpy(saved_scroll, scroll_at, sizeof scroll_at);
    memcpy(saved_want, scroll_want, sizeof scroll_want);
    memcpy(saved_to, scrolled_to, sizeof scrolled_to);
    ImDrawList scratch(ImGui::GetDrawListSharedData());
    scratch._ResetForNewFrame();
    scratch.PushTexture(ImGui::GetIO().Fonts->TexRef);
    scratch.PushClipRectFullScreen();
    ease_step = 0;
    {
        ui::Ctx dry = c;
        dry.d = &scratch;
        draw_menu(dry);
    }
    hits = saved_hits, marquee_max = saved_marquee, ease_step = saved_ease;
    teleport_columns = saved_tele[0], teleport_page_rows = saved_tele[1], teleport_scroll_px = saved_tele_px[0], teleport_want_px = saved_tele_px[1];
    inventory_columns = saved_inv[0], inventory_capacity = saved_inv[1], inventory_page = saved_inv[2];
    memcpy(scroll_at, saved_scroll, sizeof scroll_at);
    memcpy(scroll_want, saved_want, sizeof scroll_want);
    memcpy(scrolled_to, saved_to, sizeof scrolled_to);
}

float menu_ox = 0;   // (the menu's column's left edge in the window: more than 0 only on a window wider than 16:9)
/// The menu: the game dimmed across the whole window, the pages in a column as wide as a 16:9 screen of this height, in the
/// middle (a window of 16:9 or narrower: the whole of it). The pages draw from 0 as on 16:9; their vertices and clip
/// rectangles are moved over after, and menu_tick moves the mouse back by menu_ox before it tests the hits
void draw_menu(ui::Ctx& c) {
    if (reshade_menu::comparing()) {
        menu_ox = 0;
        reshade_menu::draw_comparison(c, hits);
        return;
    }
    if (!(tune_mode && page == 0)) c.d->AddRectFilled(ImVec2(0, 0), ImVec2(c.w, c.h), ui::kShade);   // (the game behind, dimmed)
    ui::Ctx k = c;
    k.w = std::min(c.w, std::floor(c.h * 16.0f / 9.0f + 0.5f));
    menu_ox = std::floor((c.w - k.w) * 0.5f);
    const int vtx0 = c.d->VtxBuffer.Size, cmd0 = c.d->CmdBuffer.Size;
    hits.clear(0, 0, k.w, k.h);
    if (debug_warning) draw_debug_warning(k);
    else if (page == 5) draw_ask(k);
    else if (page == 1) {
        draw_controls(k);
        if (control_confirmation.action != ControlAction::None) draw_control_confirmation(k);
    }
    else if (page == 3) draw_teleport(k);
    else if (page == 4) draw_inventory(k);
    else if (quick_debug && cur_tab == TAB_DEBUG) draw_quick_debug(k);
    else if (tune_mode) draw_tune_bar(k);
    else draw_options(k);
    if (menu_ox <= 0) return;
    for (int i = vtx0; i < c.d->VtxBuffer.Size; i++) c.d->VtxBuffer[i].pos.x += menu_ox;
    // (a clip rectangle of the column's: moved with it; the whole window's (the dim's, and the draw list's own) stays, as it
    // holds the column wherever it is)
    for (int i = std::max(0, cmd0 - 1); i < c.d->CmdBuffer.Size; i++) {
        ImVec4& r = c.d->CmdBuffer[i].ClipRect;
        if (r.x <= 0 && r.z >= c.w) continue;
        r.x += menu_ox, r.z += menu_ox;
    }
    // (the draw list's current header, for whatever is drawn next: the window's again after the pages' pops)
}

/// Text with a dark outline (each side, one step of the scale), as the counter always had: readable over any scene
void outlined_right(ui::Ctx& c, ImFont* font, float px, float right, float top, ImU32 col, const char* t) {
    const float x = right - ui::size_of(font, px, t).x, o = std::max(1.0f, c.s);
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if (dx || dy) c.d->AddText(font, px, ImVec2(x + dx * o, top + dy * o), ui::rgba(0, 0, 0, 230), t);
    c.d->AddText(font, px, ImVec2(x, top), col, t);
}

/// (the FPS counter's lines past the number) Text with a soft shadow: a thin dark outline, lighter than the number's, and a
/// darker copy a step down, so it reads over sand and sky as over a dungeon. Digits on even steps (tabular: the widest
/// digit's advance), so a changing value does not shift
struct Tab {
    ImFont* font;
    float px, digit;
    Tab(ImFont* f, float size) : font(f), px(size), digit(0) {
        char d[2] = {0, 0};
        for (char k = '0'; k <= '9'; k++) d[0] = k, digit = std::max(digit, ui::size_of(f, size, d).x);
    }
    static bool dig(char c) { return c >= '0' && c <= '9'; }
    /// Each piece of t: a digit alone (in its even cell), or a run of what is not one (drawn whole). fn(begin, end, x, width)
    template <class Fn>
    float pieces(const char* t, float x, Fn fn) const {
        const float x0 = x;
        for (const char* q = t; *q;) {
            const char* e = q + 1;
            if (!dig(*q))
                while (*e && !dig(*e)) e++;
            const float w = dig(*q) ? digit : font->CalcTextSizeA(px, FLT_MAX, 0, q, e).x;
            fn(q, e, x, w);
            x += w, q = e;
        }
        return x - x0;
    }
    float width(const char* t) const { return pieces(t, 0, [](const char*, const char*, float, float) {}); }
    /// Each piece five times under itself (a faint copy at each diagonal step, a darker one a step down) and once in col: a
    /// dozen strings make a few hundred quads' worth of glyphs, not a glyph each ten times
    void draw(ImDrawList* d, float x, float y, ImU32 col, const char* t, float s) const {
        const float o = std::max(1.0f, s);
        const int a = std::max(200, (int)((col >> IM_COL32_A_SHIFT) & 0xff));
        const ImU32 shade = ui::rgba(0, 0, 0, a * 210 / 255), faint = ui::rgba(0, 0, 0, a * 160 / 255);
        pieces(t, x, [&](const char* q, const char* e, float px0, float w) {
            if (*q == ' ' && e == q + 1) return;
            const float gx = px0 + (dig(*q) ? (w - font->CalcTextSizeA(px, FLT_MAX, 0, q, e).x) * 0.5f : 0);
            static const float dx[4] = {-1, 1, -1, 1}, dy[4] = {-1, -1, 1, 1};
            for (int k = 0; k < 4; k++) d->AddText(font, px, ImVec2(gx + dx[k] * o * 0.8f, y + dy[k] * o * 0.8f), faint, q, e);
            d->AddText(font, px, ImVec2(gx, y + o), shade, q, e);
            d->AddText(font, px, ImVec2(gx, y), col, q, e);
        });
    }
};

/// The processor's name without its marks, clock and core count: "Intel(R) Core(TM) i7-8700K CPU @ 3.70GHz" is "Intel Core
/// i7-8700K", "AMD Ryzen 5 5600X 6-Core Processor" is "AMD Ryzen 5 5600X"
static std::string hardware_cpu_name(std::string t) {
    for (const char* mark : {"(R)", "(TM)", "(tm)"})
        for (size_t at; (at = t.find(mark)) != std::string::npos;) t.erase(at, strlen(mark));
    if (const size_t at = t.find(" @ "); at != std::string::npos) t.erase(at);
    while (!t.empty() && t.back() == ' ') t.pop_back();
    const auto drop_end = [&](const char* w) {
        const size_t n = strlen(w);
        if (t.size() > n && t.compare(t.size() - n, n, w) == 0) t.erase(t.size() - n);
    };
    drop_end(" Processor");
    drop_end(" CPU");
    if (const size_t sp = t.rfind(' '); sp != std::string::npos && t.size() > 5 && t.compare(t.size() - 5, 5, "-Core") == 0) t.erase(sp);
    std::string out;
    for (char ch : t)
        if (ch != ' ' || (!out.empty() && out.back() != ' ')) out += ch;
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

/// The GPU's name without its marks and the driver's note in brackets at its end, which the driver line says:
/// "AMD Radeon RX 6750 XT (RADV NAVI22)" is "AMD Radeon RX 6750 XT", "Intel(R) Arc(tm) A770 Graphics (DG2)" "Intel Arc A770 Graphics"
static std::string hardware_gpu_name(std::string t) {
    for (const char* mark : {"(R)", "(TM)", "(tm)"})
        for (size_t at; (at = t.find(mark)) != std::string::npos;) t.erase(at, strlen(mark));
    while (!t.empty() && t.back() == ' ') t.pop_back();
    if (const size_t open = t.rfind(" ("); open != std::string::npos && open > 0 && !t.empty() && t.back() == ')') t.erase(open);
    return t;
}

static_assert(sizeof opts.fps_dev_show / sizeof *opts.fps_dev_show == fps_stats::kDevLines + 2,
              "a details page toggle for each Dev line and the footer's CPU, GPU and driver, and render lines");

/// Whether the FPS counter's footer shows its render lines (Dev, or the version and hardware at every level, with the Render
/// lines row on): vk_temporal.cpp and vk_draw.cpp note the gameplay upscale's sizes only then
/// The FPS counter's footer lines under the driver: the gameplay upscale as it was last drawn (upscale_route.h, published by
/// whichever path drew it, within the last half second), with the Upscaler and Render resolution rows for what was asked for
static void fps_render_lines(std::string out[2]) {
    upscale_route::Sample r;
    const bool drawn = upscale_route::latest(&r, fps_stats::kSecond / 2);
    upscale_route::footer_lines(drawn ? &r : nullptr, std::clamp(upscaler(), 0, ur::kUpscalers - 1), opts.resolution, out);
}

/// The FPS counter's scale (pixels per point): the menu's with Auto; else a fixed one, Small, Medium and Large the counter's
/// size in a 720p, 1080p and 1440p window at the Medium menu size, at most what Auto gives at the Large menu size
float fps_scale(const ui::Ctx& c) {
    if (opts.fps_size < 1 || opts.fps_size > 3) return c.s;
    static const float fixed[] = {1.0f, 1.5f, 2.0f};
    return std::min(fixed[opts.fps_size - 1], c.h / 720.0f * 1.3f);
}

/// The FPS counter, in the corner the FPS counter position row chooses (the safe area's; under the banner at the top right
/// while the menu is open, unless it fits beside the menu's column on a very wide window). The number as it always looked:
/// green outlined bold, "FPS" small beside it, the frame generation line under them, no plate.
/// Past FPS (the row's levels, fps_stats.h), a tidy column under it, aligned to the same edge, with the same soft-shadowed
/// text and no plate either: the frame time (green, amber or red against the cap's) and the 1% low; the graph of the last
/// 4 s, a thin line over a soft fill, one column a frame span with its slowest frame (a stutter never falls between two), a
/// faint dashed line at the cap's frame time, red ticks over frames past twice it; the Breakdown, where the time goes
/// stacked in the graph's columns, its legend with the values, the GPU's time and the stutters' causes (their columns get a
/// red tick too). Dev adds the renderer's own numbers, and at the foot the version, the CPU, GPU and driver.
void draw_fps(ui::Ctx& c, bool under_banner) {
    namespace fs = fps_stats;
    int level = fs::level_of(opts.show_fps);
    const float s = fps_scale(c), big = 28 * s, small = 13 * s, label = 12 * s;
    const ImU32 green = ui::rgba(72, 255, 110), ink = ui::kInkBright, dim = ui::kSoft, soft = ui::kInk;
    const Tab num(c.f.strong, big), tab(c.f.strong, small), lab(c.f.strong, label);
    const float nw = num.width(fps_text), lw = ui::size_of(c.f.strong, small, "FPS").x;
    const float sw = fps_sub[0] ? tab.width(fps_sub) : 0;
    const float gw = (fs::level_of(opts.show_fps) >= fs::kBreakdown ? 280 : 208) * s;   // (the column's width from the graph on; wider with the lanes)
    // (while the menu is open: the top right under the banner, the number and the frame time only, unless the window is so much
    // wider than 16:9 that the column fits beside the menu's, in its own corner)
    const bool beside = menu_ox >= (level >= fs::kGraph ? gw : nw + lw) + 2 * c.safe + 8 * s;
    int corner = std::clamp(opts.fps_corner, 0, 3);
    if (under_banner && !tune_mode && !beside) corner = 0, level = std::min(level, (int)fs::kFrameTime);
    const bool left = corner & 1, bottom = corner >= 2;
    // (the column's height, to anchor it at the bottom)
    const float row = 17 * s, gh = 50 * s, lh = 16 * s;
    float h = 32 * s + (fps_sub[0] ? row : 0);
    if (level >= fs::kFrameTime && fps_ms_text[0]) h += row;
    if (level >= fs::kGraph) h += 8 * s + gh + 4 * s;
    const bool rt_lane = fps_render_thread_on();   // (no render thread, threaded rendering off: no lane, no legend section)
    if (level >= fs::kBreakdown) h += (rt_lane ? 36 * s + 6 * lh : 0) + 10 * s + 11 * lh + 12 * s;   // (the render lane, the legend's rows)
    // (the Dev lines the details page leaves on, laid out by their width: VRAM on a row of its own, the rest two to a row while the
    // name and the value fit in half the column, a row of their own when not ("2089, 3.9 GB"). A line that went wide stays wide
    // until it fits with room to spare: its value changes every half second, and it would jump between the two)
    struct DevRow { int a, b; };   // (b: the right half's line; -1 a row of its own, -2 the left half alone)
    static DevRow dev_layout[fs::kDevLines];
    static bool dev_wide[fs::kDevLines];
    int dev_shown = 0, dev_rows = 0;
    if (level >= fs::kDev) {
        const float room = gw * 0.5f - 8 * s;
        int pending = -1;
        for (int i = 0; i < fs::kDevLines; i++) dev_shown += opts.fps_dev_show[i] != 0;
        if (opts.fps_dev_show[fs::kVram]) dev_layout[dev_rows++] = {fs::kVram, -1};
        for (int g : fs::kDevGrid) {
            if (!opts.fps_dev_show[g]) continue;
            const float need = lab.width(fs::dev_short(g)) + tab.width(fps_dev_text[g]) + 8 * s;
            dev_wide[g] = need > room || (dev_wide[g] && need > room * 0.85f);
            if (dev_wide[g]) dev_layout[dev_rows++] = {g, -1};
            else if (pending < 0) pending = g;
            else dev_layout[dev_rows++] = {pending, g}, pending = -1;
        }
        if (pending >= 0) dev_layout[dev_rows++] = {pending, -2};
    }
    if (level >= fs::kDev && dev_shown) h += 6 * s + dev_rows * lh;
    // (the footer, under everything: the version, the CPU, the GPU and its driver, what the counter ran on: at Dev, or at every
    // level with Version and hardware at every level; the CPU, GPU and driver, and render rows turn theirs off)
    const bool footer = level >= fs::kDev || opts.fps_version_always;
    // (the footer's lines, by their toggles: 0 the version, 1 the CPU, 2-3 the GPU and its driver, 4-5 the render size and the
    // upscaler (fps_render_lines))
    int foot_lines[6], foot = 0;
    if (footer) {
        foot_lines[foot++] = 0;
        if (opts.fps_dev_show[fs::kDevLines]) foot_lines[foot++] = 1, foot_lines[foot++] = 2, foot_lines[foot++] = 3;
        if (opts.fps_dev_show[fs::kDevLines + 1]) foot_lines[foot++] = 4, foot_lines[foot++] = 5;
    }
    if (foot) h += 6 * s + foot * lh;
    // (a column taller than the window gives up the footer's last lines first, the render lines, then the hardware's: the
    // counter's own numbers keep their place at either anchor)
    while (foot > 1 && h > c.h - 2 * c.safe) foot--, h -= lh;
    const bool render_lines = std::find(foot_lines, foot_lines + foot, 4) != foot_lines + foot;
    const float edge = left ? c.safe : c.w - c.safe;
    const float top = bottom ? c.h - c.safe - h : under_banner && !tune_mode && page != 5 && !beside ? banner_h(c) + 10 * s : c.safe;
    // (x for a width at the column's edge)
    const auto at = [&](float w) { return left ? edge : edge - w; };
    // (the number's colour: at FPS the classic green whatever the rate, the counter as it always was; from Frame time on
    // green at the cap's rate, amber under it, red under two thirds of it, as the frame time beside it)
    const double target = fps_target_ms();   // (0: none, Uncapped: the colours against the console's 60, no line)
    const ImU32 grade_col[3] = {green, ui::rgba(255, 200, 60), ui::rgba(255, 95, 80)};
    const ImU32 num_col = level >= fs::kFrameTime && fps_rate_now > 0 ? grade_col[fs::rate_grade(fps_rate_now, target)] : green;
    {   // (the number and "FPS", as they always looked: outlined, the number's digits on even steps)
        const float x = at(nw + 4 * s + lw), o = std::max(1.0f, s);
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                if (dx || dy) {
                    float gx = x;
                    char one[2] = {0, 0};
                    for (const char* q = fps_text; *q; q++) {
                        one[0] = *q;
                        const float adv = ui::size_of(c.f.strong, big, one).x;
                        c.d->AddText(c.f.strong, big, ImVec2(gx + (num.digit - adv) * 0.5f + dx * o, top + dy * o), ui::rgba(0, 0, 0, 230), one);
                        gx += num.digit;
                    }
                    c.d->AddText(c.f.strong, small, ImVec2(x + nw + 4 * s + dx * o, top + 14 * s + dy * o), ui::rgba(0, 0, 0, 230), "FPS");
                }
        float gx = x;
        char one[2] = {0, 0};
        for (const char* q = fps_text; *q; q++) {
            one[0] = *q;
            const float adv = ui::size_of(c.f.strong, big, one).x;
            c.d->AddText(c.f.strong, big, ImVec2(gx + (num.digit - adv) * 0.5f, top), num_col, one);
            gx += num.digit;
        }
        c.d->AddText(c.f.strong, small, ImVec2(x + nw + 4 * s, top + 14 * s), num_col, "FPS");
    }
    float y = top + 32 * s;
    if (fps_sub[0]) tab.draw(c.d, at(sw), y, soft, fps_sub, s), y += row;
    if (level >= fs::kFrameTime && fps_ms_text[0]) {   // ("16.7 ms" coloured by its distance from the cap's frame time, "1% low 20.1 ms" beside it)
        const ImU32 ms_col = grade_col[fps_ms_now > 0 ? fs::rate_grade(1000.0 / fps_ms_now, target) : 0];
        const float mw = tab.width(fps_ms_text), gap = 10 * s, low_w = tab.width(fps_low_text);
        const float x = at(mw + gap + low_w);
        tab.draw(c.d, x, y, ms_col, fps_ms_text, s);
        tab.draw(c.d, x + mw + gap, y, soft, fps_low_text, s);
        y += row;
    }
    if (foot) {   // (the footer, at the column's foot: the version, as the banner says it; at Dev the CPU, the GPU, its driver. A line
        // too long for the column's width loses its end)
        static std::string cpu, gpu, drv, fit[6], fit_lines[2];
        static const std::string ver = std::string("Longshot ") + version_text(LONGSHOT_VERSION);
        static float fit_w = 0;
        if (gpu.empty()) cpu = hardware_cpu_name(bug_report::cpu_name()), gpu = hardware_gpu_name(device_name_text()), drv = driver_text(), fit_w = 0;
        const auto shorten = [&](std::string t, float room) {
            if (lab.width(t.c_str()) <= room) return t;
            while (!t.empty() && lab.width((t + "...").c_str()) > room) {
                t.pop_back();
                while (!t.empty() && ((unsigned char)t.back() & 0xC0) == 0x80) t.pop_back();
            }
            while (!t.empty() && t.back() == ' ') t.pop_back();
            return t + "...";
        };
        if (fit_w != gw) {
            fit_w = gw;
            fit[0] = shorten(ver, gw);
            fit[1] = shorten(cpu.empty() ? "-" : cpu, gw);
            fit[2] = shorten(gpu.empty() ? "-" : gpu, gw);
            fit[3] = shorten(drv.empty() ? "-" : drv, gw);
            fit[4] = shorten(fit_lines[0], gw), fit[5] = shorten(fit_lines[1], gw);
        }
        if (render_lines) {   // (the render size and the upscaler change in play: made again when their text does)
            upscale_route::want();   // (the drawing paths publish what they drew while this shows)
            static int64_t render_at = 0;
            const int64_t now_at = fps_clock();
            if (!render_at || now_at < render_at || now_at - render_at >= fs::kSecond / 4) {
                render_at = now_at;
                std::string now[2];
                fps_render_lines(now);
                if (now[0] != fit_lines[0] || now[1] != fit_lines[1])
                    fit_lines[0] = now[0], fit_lines[1] = now[1], fit[4] = shorten(now[0], gw), fit[5] = shorten(now[1], gw);
            }
        }
        float fy = top + h - foot * lh;
        for (int i = 0; i < foot; i++) {
            const std::string& t = fit[foot_lines[i]];
            lab.draw(c.d, at(lab.width(t.c_str())), fy + 1 * s, soft, t.c_str(), s), fy += lh;
        }
    }
    if (level < fs::kGraph) return;
    // (the graph)
    constexpr int kCols = 64;   // (wider columns, the lanes' parts readable)
    static fs::Frame col[kCols];
    static bool mark[kCols];
    // (the columns from the ring at most 60 times a second: at the cap Uncapped's 1000 fps, 4 s of frames each present cost
    // more than the drawing; a 4 s graph moves a column every 40 ms)
    static double topms = 0, scroll = 0;
    static int64_t graph_at = 0;
    if (const int64_t now = fps_clock(); now - graph_at >= fs::kSecond / 60 || now < graph_at) {
        graph_at = now;
        (void)fps_graph(now, kCols, col, mark, &scroll);   // (fixed slots, scrolled by how far into the newest one now is)
        static float ms[kCols];   // (the scale from the usual frame, not the slowest: a spike past the top is drawn at it, and marked)
        int k = 0;
        for (int i = 0; i < kCols; i++)
            if (col[i].ms > 0) ms[k++] = col[i].ms;
        // (up at once, down slowly, so the whole graph does not rescale with each frame that scrolls out: 0.5% a 60th of a second,
        // whatever the frame rate the counter is drawn at)
        const double want = fs::graph_top(target, fs::typical_ms(ms, k));
        static int64_t decayed_at = 0;
        const double ticks = decayed_at && now > decayed_at ? std::min(60.0, (now - decayed_at) / (fs::kSecond / 60.0)) : 1.0;
        decayed_at = now;
        topms = want >= topms ? want : std::max(want, topms * std::pow(0.995, ticks));
    }
    const bool parts = level >= fs::kBreakdown;
    const float x0 = at(gw), x1 = x0 + gw, gy0 = y + 8 * s, gy1 = gy0 + gh;
    const float cw = gw / kCols, py = (float)(gh / topms);
    const float thin = std::max(1.0f, 1.75f * s);
    const ImU32 line_col = ui::rgba(130, 240, 150), fill_top = ui::rgba(120, 225, 140, 80), fill_bot = ui::rgba(120, 225, 140, 0);
    // (the floor: a faint line; the cap's frame time: a dashed one, its value at the inner end. The dashes are drawn last, over
    // the columns and the line, with a dark line under them: in view over any part's colour)
    c.d->AddLine(ImVec2(x0, std::floor(gy1) + 0.5f), ImVec2(x1, std::floor(gy1) + 0.5f), ui::rgba(255, 255, 255, 60), std::max(1.0f, s));
    const auto cap_line = [&](float ty) {
        const float dash = 4 * s, gap = 3 * s, w = std::max(1.0f, 1.25f * s);
        c.d->AddLine(ImVec2(x0, ty + w), ImVec2(x1, ty + w), ui::rgba(0, 0, 0, 140), w);
        for (float x = x0; x < x1; x += dash + gap)
            c.d->AddLine(ImVec2(x, ty), ImVec2(std::min(x + dash, x1), ty), ui::rgba(255, 255, 255, 230), w);
    };
    const float cap_y = target > 0 ? std::floor(gy1 - (float)target * py) + 0.5f : -1;   // (Uncapped without V-Sync: no line)
    {
        // (the scale's two values outside the graph, on its inner side: the cap's frame time at its line, the top at the top)
        char t[16];
        if (target > 0) {
            const float ty = cap_y;
            snprintf(t, sizeof t, "%.1f", target);
            lab.draw(c.d, left ? x1 + 5 * s : x0 - 5 * s - lab.width(t), ty - label * 0.55f, ui::rgba(235, 235, 235), t, s);
        }
        snprintf(t, sizeof t, topms < 10 ? "%.1f" : "%.0f", topms);
        lab.draw(c.d, left ? x1 + 5 * s : x0 - 5 * s - lab.width(t), gy0 - label * 0.55f, ui::rgba(200, 200, 200), t, s);
    }
    // (the soft fill: a column each, fading down; then the line over it, broken where a column had no frame, a faint dark line
    // under it for the bright scenes. with the Breakdown, each column its frame's parts stacked instead of the fill, and
    // with a render thread a lane under the graph on the same time axis, the render thread's frame)
    static ImVec2 pts[kCols];
    const auto yv = [&](float ms) { return std::max(gy0, gy1 - ms * py); };
    const auto cx_of = [&](int i) { return x0 + (i + 0.5f - (float)scroll) * cw; };   // (scrolled smoothly)
    // (the parts' colours, one meaning in every lane: blue the game's work, red the waits for the GPU, purple the render thread,
    // grey the pacing and idle; the lighter shade of a family its smaller part)
    static const ImU32 part_col[fs::kParts] = {ui::rgba(96, 160, 232), ui::rgba(160, 200, 245), ui::rgba(246, 150, 130),
                                                ui::rgba(120, 120, 120), ui::rgba(168, 132, 230), ui::rgba(232, 72, 60),
                                                ui::rgba(56, 108, 196)};
    static const char* const names[fs::kParts] = {"Game", "Renderer", "GPU present", "Pacing", "Render thread", "GPU fence",
                                                  "Game's main thread"};
    static const bool part_waits[fs::kParts] = {false, false, true, false, true, true, true};   // (under the legend's "Waiting on")
    static const int order[fs::kParts] = {fs::kGame, fs::kRender, fs::kWaitMain, fs::kWaitRender, fs::kWaitGpu, fs::kGpuWait,
                                          fs::kPace};
    static const ImU32 rt_col[fs::kRtParts] = {ui::rgba(168, 132, 230), ui::rgba(232, 72, 60), ui::rgba(110, 110, 110)};
    static const char* const rt_names[fs::kRtParts] = {"Recording", "GPU slot", "Idle"};
    const auto alpha = [](ImU32 c, int a) { return (c & 0x00FFFFFF) | ((ImU32)a << 24); };
    c.d->PushClipRect(ImVec2(x0, gy0 - 8 * s), ImVec2(x1, gy1 + 1), true);
    if (parts)
        for (int i = 0; i < kCols; i++) {
            if (col[i].ms <= 0) continue;
            const float l = cx_of(i) - cw * 0.5f + 0.5f, r = l + cw - 0.5f;   // (a hairline between columns)
            float yb = gy1;
            for (int o = 0; o < fs::kParts && yb > gy0; o++) {
                const float hgt = col[i].part[order[o]] * py;
                if (hgt <= 0) continue;
                const float yt = std::max(gy0, yb - hgt);
                c.d->AddRectFilled(ImVec2(l, yt), ImVec2(r, yb), alpha(part_col[order[o]], 190));
                yb = yt;
            }
        }
    // (the columns with a frame, joined across those without: a slow stretch has fewer frames than columns)
    int n = 0;
    for (int i = 0; i < kCols; i++)
        if (col[i].ms > 0) pts[n++] = ImVec2(cx_of(i), yv(col[i].ms));
    if (n >= 2 && !parts) {   // (the fill: a quad under each segment, its colour at the line fading to nothing at the floor)
        const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
        c.d->PrimReserve((n - 1) * 6, (n - 1) * 4);
        for (int k = 0; k + 1 < n; k++) {
            const ImDrawIdx i0 = (ImDrawIdx)c.d->_VtxCurrentIdx;
            c.d->PrimWriteVtx(pts[k], uv, fill_top);
            c.d->PrimWriteVtx(pts[k + 1], uv, fill_top);
            c.d->PrimWriteVtx(ImVec2(pts[k + 1].x, gy1), uv, fill_bot);
            c.d->PrimWriteVtx(ImVec2(pts[k].x, gy1), uv, fill_bot);
            c.d->PrimWriteIdx(i0), c.d->PrimWriteIdx((ImDrawIdx)(i0 + 1)), c.d->PrimWriteIdx((ImDrawIdx)(i0 + 2));
            c.d->PrimWriteIdx(i0), c.d->PrimWriteIdx((ImDrawIdx)(i0 + 2)), c.d->PrimWriteIdx((ImDrawIdx)(i0 + 3));
        }
    }
    if (n == 1) pts[n++] = ImVec2(pts[0].x + 1, pts[0].y);
    if (n >= 2) {
        static ImVec2 under[kCols];
        for (int k = 0; k < n; k++) under[k] = ImVec2(pts[k].x, pts[k].y + std::max(1.0f, s));
        c.d->AddPolyline(under, n, ui::rgba(0, 0, 0, 150), ImDrawFlags_None, thin + std::max(1.0f, s));
        c.d->AddPolyline(pts, n, line_col, ImDrawFlags_None, thin);
    }
    if (cap_y > gy0) cap_line(cap_y);
    // (red ticks: spikes, frames past twice the cap's and half again the graph's usual (a steady slow stretch is not a row of
    // them), and with the Breakdown the frames that built shaders or left draws out. A frame's tick is decided once, the first
    // time a column shows it, and kept while it scrolls by: decided again at every drawing from the columns on screen, a frame
    // near the line went on and off as others scrolled in and out and the scale settled)
    float usual = 0;
    {
        int k = 0;
        for (int i = 0; i < kCols; i++)
            if (col[i].ms > 0) usual += col[i].ms, k++;
        usual = k ? usual / k : 0;
    }
    static std::unordered_map<int64_t, bool> spike_of;   // (a frame's at_ns: its tick)
    if (spike_of.size() > 4 * kCols) {   // (the frames past the window's left end)
        int64_t oldest = INT64_MAX;
        for (int i = 0; i < kCols; i++)
            if (col[i].ms > 0) oldest = std::min(oldest, col[i].at_ns);
        for (auto it = spike_of.begin(); it != spike_of.end();) it = it->first < oldest ? spike_of.erase(it) : std::next(it);
    }
    for (int i = 0; i < kCols; i++) {
        if (col[i].ms <= 0) continue;
        auto [it, fresh] = spike_of.try_emplace(col[i].at_ns, false);
        if (fresh)   // (no target: past half again the usual)
            it->second = (col[i].ms > 2 * target && col[i].ms > 1.5f * usual) || col[i].ms > topms;
        if (it->second || (parts && mark[i])) {
            const float cx = cx_of(i);
            c.d->AddCircleFilled(ImVec2(cx, gy0 - 3 * s), 2.2f * s, ui::rgba(0, 0, 0, 120));
            c.d->AddCircleFilled(ImVec2(cx, gy0 - 3 * s), 1.6f * s, ui::rgba(240, 100, 85));
        }
    }
    c.d->PopClipRect();
    if (!parts) return;
    // (longshot) the render thread's lane: its frame in ms on the frame graph's scale (its own height), the work and the waits
    // stacked and idle left empty, so a column's height is what the render thread spent of the frame; the cap's line and the
    // top's value beside it, as the frame graph's
    const float lane_gap = 6 * s, rh = rt_lane ? 30 * s : 0;
    const float ry0 = gy1 + (rt_lane ? lane_gap : 0), ry1 = ry0 + rh, rpy = (float)(rh / topms);
    if (rt_lane) {
        const auto scale_label = [&](const char* t, float ty, ImU32 col_) {
            lab.draw(c.d, left ? x1 + 5 * s : x0 - 5 * s - lab.width(t), ty - label * 0.55f, col_, t, s);
        };
        c.d->AddLine(ImVec2(x0, std::floor(ry1) + 0.5f), ImVec2(x1, std::floor(ry1) + 0.5f), ui::rgba(255, 255, 255, 60), std::max(1.0f, s));
        {
            char t[16];
            if (target > 0) {
                const float ty = std::floor(ry1 - (float)target * rpy) + 0.5f;
                snprintf(t, sizeof t, "%.1f", target);
                scale_label(t, ty, ui::rgba(235, 235, 235));
            }
            snprintf(t, sizeof t, topms < 10 ? "%.1f" : "%.0f", topms);
            scale_label(t, ry0, ui::rgba(200, 200, 200));
        }
        c.d->PushClipRect(ImVec2(x0, ry0), ImVec2(x1, ry1 + 1), true);
        for (int i = 0; i < kCols && fps_rt_on; i++) {
            if (col[i].ms <= 0) continue;
            const float l = cx_of(i) - cw * 0.5f + 0.5f, r = l + cw - 0.5f;
            float yb = ry1;
            for (int k = 0; k < fs::kRtParts && yb > ry0; k++) {
                if (k == fs::kRtIdle) continue;   // (idle: the space above)
                const float hgt = col[i].rt[k] * rpy;
                if (hgt <= 0) continue;
                const float yt = std::max(ry0, yb - hgt);
                c.d->AddRectFilled(ImVec2(l, yt), ImVec2(r, yb), alpha(rt_col[k], 190));
                yb = yt;
            }
        }
        if (target > 0) {   // (over the lane's columns, as the frame graph's)
            const float ty = std::floor(ry1 - (float)target * rpy) + 0.5f;
            if (ty > ry0) cap_line(ty);
        }
        c.d->PopClipRect();
    }
    // (the legend: the frame's parts first, adding up to the frame time shown under the number, then the render thread's to the same
    // span on its own clock, the GPU's beside them; the render thread and the GPU run in parallel with the frame, not after it.
    // The waits under a "Waiting on" sub-heading of their own, which has no value)
    float by = ry1 + 10 * s;
    const float half_w = gw * 0.5f;
    const auto swatch = [&](float sx, ImU32 col) {
        c.d->AddRectFilled(ImVec2(sx, by + lh * 0.5f - 3 * s), ImVec2(sx + 6 * s, by + lh * 0.5f + 3 * s), col, 1.5f * s);
    };
    const auto cell = [&](int column, ImU32 dot, bool indent, const char* name, const char* value, ImU32 value_col) {
        const float cx0 = column < 0 ? x0 : x0 + column * half_w, cx1 = column < 0 ? x1 : cx0 + half_w - (column ? 0 : 8 * s);   // (-1: the whole width)
        float nx = cx0 + (indent ? 10 * s : 0);   // (indent: a part under "Waiting on")
        if (dot) swatch(nx, dot), nx += 10 * s;
        lab.draw(c.d, nx, by + 1 * s, soft, name, s);
        tab.draw(c.d, cx1 - tab.width(value), by, value_col, value, s);
    };
    const auto header = [&](const char* t, const char* value, ImU32 value_col) {
        lab.draw(c.d, x0, by + 1 * s, ui::rgba(255, 255, 255, 200), t, s);
        if (value) tab.draw(c.d, x1 - tab.width(value), by, value_col, value, s);
        by += lh;
    };
    // (a sub-heading over the waits, without a value of its own: the values in the column are the parts, and they add up)
    const auto waiting_on = [&] {
        lab.draw(c.d, x0, by + 1 * s, soft, "Waiting on", s);
        by += lh;
    };
    for (int o = 0; o < fs::kParts; o++) {   // (the work first, then the waits under their sub-heading, then the pacing)
        if (part_waits[order[o]] && (o == 0 || !part_waits[order[o - 1]])) waiting_on();
        cell(-1, part_col[order[o]], part_waits[order[o]], names[order[o]], fps_part_text[order[o]], ink);
        by += lh;
    }
    if (rt_lane) {
        header("Render thread", "in parallel", dim);
        for (int k = 0; k < fs::kRtParts; k++) {
            const bool waits = k == fs::kRtGpu;
            if (k == fs::kRtGpu) waiting_on();
            cell(-1, rt_col[k], waits, rt_names[k], fps_rt_text[k], ink);
            by += lh;
        }
    }
    header("GPU", "in parallel", dim);
    {
        char busy[48];
        snprintf(busy, sizeof busy, "%s  %s", fps_gpu_text, fps_gpu_busy_text);
        cell(-1, 0, false, "GPU time", busy, ink);
        by += lh;
    }
    c.d->AddLine(ImVec2(x0, std::floor(by + 3 * s) + 0.5f), ImVec2(x1, std::floor(by + 3 * s) + 0.5f), ui::rgba(255, 255, 255, 40), std::max(1.0f, s));
    by += 6 * s;
    const ImU32 red = ui::rgba(240, 100, 85);
    cell(0, 0, false, i18n::tr("Shaders built"), fps_built_text, strcmp(fps_built_text, "0") ? red : dim);
    cell(1, 0, false, i18n::tr("Draws left out"), fps_left_text, strcmp(fps_left_text, "0") ? red : dim);
    by += lh;
    if (level < fs::kDev || !dev_shown) return;
    // (Dev: the renderer's own numbers, for debugging; those the details page leaves on)
    c.d->AddLine(ImVec2(x0, std::floor(by + 3 * s) + 0.5f), ImVec2(x1, std::floor(by + 3 * s) + 0.5f), ui::rgba(255, 255, 255, 40), std::max(1.0f, s));
    by += 6 * s;
    const auto dev_cell = [&](int column, int i) {
        const bool alarm = (i == fs::kVram && fps_vram_over) || (i == fs::kSkipped && strcmp(fps_dev_text[i], "0") && strcmp(fps_dev_text[i], "-"));
        cell(column, 0, false, fs::dev_short(i), fps_dev_text[i], alarm ? red : ink);
    };
    for (int r = 0; r < dev_rows; r++) {   // (as laid out with the column's height, above)
        const DevRow& d = dev_layout[r];
        dev_cell(d.b == -1 ? -1 : 0, d.a);
        if (d.b >= 0) dev_cell(1, d.b);
        by += lh;
    }
}

/// The start-up screen: the logo, the wordmark, what the start is doing, a bar, on the Menu theme's dark. A little below the middle; what
/// is under the logo at 0.86 of the logo's scale. startup_fade, 0 to 1, darkens it all to black as it ends
void draw_startup(ui::Ctx& c) {
    StartupView v;
    {
        std::lock_guard lk(startup_mu);
        v = startup;
    }
    const float s = c.s, w = c.w, h = c.h, cx = w * 0.5f, k = s * 0.86f;   // (k: the scale of what is under the logo)
    c.d->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), ui::kStartBg);
    const float lh = 112 * s, top = h * 0.5f - 176 * s;
    for (int i = 40; i >= 1; i--) c.d->AddCircleFilled(ImVec2(cx, top + lh * 0.5f + 70 * s), h * 0.021f * i, ui::kStartGlow, 128);
    float aspect = 1;
    ImTextureData* logo = ui::logo(&aspect);
    if (logo) c.d->AddImage(logo->GetTexRef(), ImVec2(cx - lh * aspect * 0.5f, top), ImVec2(cx + lh * aspect * 0.5f, top + lh));
    float y = top + lh + 16 * s;
    ui::spaced(c, c.f.heading, 42 * k, ImVec2(cx, y), ui::kWordmark, "LONGSHOT", 6 * k);
    ui::text_centre(c, c.f.body, 14.5f * k, ImVec2(cx, y + 64 * k), ui::kDim, v.version.c_str());
    {   // (under it, as under the menu's version: a development run's name for what it tests, LONGSHOT_BUILD_NAME, so a
        // screenshot or a stream of the first start says which change it is)
        static const std::string run_name = getenv("LONGSHOT_BUILD_NAME") ? getenv("LONGSHOT_BUILD_NAME") : "";
        if (!run_name.empty())
            ui::text_centre(c, c.f.body, 13 * k, ImVec2(cx, y + 82 * k), ui::alpha(ui::kDim, 0.8f), run_name.c_str());
    }
    if (!v.title.empty()) {
        y += 120 * k;
        if (!v.step.empty()) {   // ("STEP 2 OF 2" as "Step 2 of 2")
            std::string step = v.step;
            for (size_t i = 1; i < step.size(); i++) step[i] = (char)tolower((unsigned char)step[i]);
            ui::text_centre(c, c.f.body, 15 * k, ImVec2(cx, y), ui::kAccent, step.c_str());
        }
        ui::text_centre(c, c.f.heading, 24 * k, ImVec2(cx, y + 20 * k), ui::kInk, v.title.c_str());
        const float x0 = cx - 270 * k, x1 = cx + 270 * k, by = y + 64 * k;
        float under = by;
        if (v.fraction >= 0) {   // (the bar as a value cell: a dark cell, gold inside, the percentage on it)
            const float bh = 26 * k, in = 4 * k, fx = x0 + in + (x1 - x0 - 2 * in) * (float)std::min(v.fraction, 1.0);
            // (its corners: square in Classic; in Modern dark rounded around the fill's, ui::gold's, the same gap all round)
            const float r = ui::kRound > 0 ? ui::kRound * s + in : 0;
            c.d->AddRectFilled(ImVec2(x0, by), ImVec2(x1, by + bh), ui::kCellDark, r);
            c.d->AddRect(ImVec2(x0, by), ImVec2(x1, by + bh), ui::kEdge, r, 0, std::max(1.0f, s));
            if (fx > x0 + in) ui::gold(c, ImVec2(x0 + in, by + in), ImVec2(fx, by + bh - in));
            // (the percentage in the middle of the whole bar, drawn twice: dark ink where the gold is under it, light where the
            // empty bar is, each clipped to its part, so every letter keeps its contrast as the edge passes through it)
            const float split = std::max(fx, x0 + in);
            c.d->PushClipRect(ImVec2(x0, by), ImVec2(split, by + bh), true);
            ui::text_mid_centre(c, c.f.strong, 15 * k, cx, by + bh * 0.5f, ui::kGoldInk, v.pct.c_str());
            c.d->PopClipRect();
            c.d->PushClipRect(ImVec2(split, by), ImVec2(x1, by + bh), true);
            ui::text_mid_centre(c, c.f.strong, 15 * k, cx, by + bh * 0.5f, ui::kInk, v.pct.c_str());
            c.d->PopClipRect();
            under = by + bh;
        }
        const float px = 16.5f * k;
        ui::text(c, c.f.body, px, ImVec2(v.fraction >= 0 ? x0 : cx - ui::size_of(c.f.body, px, v.left.c_str()).x * 0.5f, under + 12 * k),
                 ui::kSoft, v.left.c_str());
        if (!v.right.empty())
            ui::text(c, c.f.body, px, ImVec2(x1 - ui::size_of(c.f.body, px, v.right.c_str()).x, under + 12 * k), ui::kSoft, v.right.c_str());
        if (!v.hint.empty()) ui::text_centre(c, c.f.body, 15.5f * k, ImVec2(cx, by + 92 * k), ui::kDim, v.hint.c_str());
    }
    if (const float f = startup_fade.load(std::memory_order_relaxed); f > 0)   // (the end: to black, the game's first frame next)
        c.d->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), ui::rgba(0, 0, 0, (int)(std::min(f, 1.0f) * 255 + 0.5f)));
}

} // namespace

// (#1633) What modules ask the renderer (menu_module.h): the HDR row's hint, why HDR on stays SDR, which the device
// knows; the Motion controls row's text, which asks the host about the controller; the display rows' (menu/display.cpp)
// V-Sync switch for the run, whether the window needs scaling and a filter's name; whether the game presents Native HDR
// (menu/crt.cpp); Test vibration's press, the host's rumble_test (as a strength change gives)
const char* hdr_sdr_hint(int v, int variant) { return hn_hdr(v, variant); }
const char* display_vsync_env() { return vsync_env(); }
bool display_filter_needed() { return filter_needed(); }
const char* display_upscale_text(int v) { return tx_upscale(v); }
bool crt_native_pq() { return present_native_pq.load(std::memory_order_relaxed); }
const char* menu_dlss_why_text(int why) { return dlss_why_text(why); }
void menu_row_top(const int* value, int hi) {
    for (Row& r : rows)
        if (r.value == value) r.hi = hi;
}
void menu_save_options() {
    sim_turf_settings_flush();
    save();
}
void menu_redraw() { menu_refresh = true; }
const char* menu_version() { return LONGSHOT_VERSION; }
/// (as the restart question's Restart now: the window closes, then the game starts again, host/hos_relaunch.c)
void menu_restart_game() {
    logf("menu: restart");
    open_now = false;
    if (const HWND window = app_window()) PostMessageA(window, WM_APP + 9, 0, 0);
}
void menu_quit_game() {
    if (const HWND window = app_window()) PostMessageA(window, WM_CLOSE, 0, 0);
}
bool menu_on_tab(const char* tab) { return open_now.load() && page == 0 && !strcmp(kTabIds[cur_tab], tab); }
const char* motion_gyro_text(int v) { return tx_gyro(v); }
void menu_rumble_test() {
    if (host && host->version >= 32 && host->rumble_test) host->rumble_test();
}
// (longshot, issue 1113) the input recordings' rows (menu/input_recording.cpp): the host's input_record, input_play and
// input_status (host/input_movie.c)
static const char* input_refused;
static bool input_status_stale = true;   // (a start or a stop: asked again at once)
MenuInputStatus menu_input_status() {
    // (asked by many rows' cells and shown hooks each frame: the host's answer kept for 50 ms, its lock taken once)
    static MenuInputStatus kept;
    static std::chrono::steady_clock::time_point at;
    const auto now = std::chrono::steady_clock::now();
    if (!input_status_stale && now - at < std::chrono::milliseconds(50)) return kept;
    input_status_stale = false, at = now;
    MenuInputStatus s;
    static char why[128];
    if (host && host->version >= 42 && host->input_status) {
        MkwVkInputStatus h{};
        host->input_status(&h);
        s.mode = h.mode, s.started = h.started, s.held = h.held, s.diverged = h.diverged;
        s.frame = h.frame, s.end = h.end, s.syncs = h.syncs, s.largest = h.largest;
        snprintf(s.path, sizeof s.path, "%s", h.path);
        snprintf(why, sizeof why, "%s", h.why);
    }
    s.refused = input_refused ? input_refused : why[0] ? why : nullptr;   // (or why the host gave the last one up)
    kept = s;
    return s;
}
void menu_input_record(int start) {
    if (!host || host->version < 42 || !host->input_record) return;
    input_refused = nullptr, input_status_stale = true;
    if (!host->input_record(start) && start) input_refused = "A recording or a playback is running: stop it first.";
}
/// The recordings folder's files, newest first, asked of the host at most once a second (the page draws every frame)
const char* menu_input_recording(int index) {
    static std::vector<std::string> files;
    static std::chrono::steady_clock::time_point at;
    static bool have;
    const auto now = std::chrono::steady_clock::now();
    if (!have || now - at > std::chrono::seconds(1)) {
        files.clear();
        char path[1024];
        for (int i = 0; i < 32 && host && host->version >= 42 && host->input_recording && host->input_recording(i, path, sizeof path); i++)
            files.emplace_back(path);
        have = true, at = now;
    }
    return index >= 0 && index < (int)files.size() ? files[index].c_str() : nullptr;
}
static void input_play(int start, int index) {
    if (!host || host->version < 42 || !host->input_play) return;
    static const int syncs[] = {0, 1, 60, 300};   // (Drift correction: Off, every update, every second, every 5 s)
    const char* path = index >= 0 ? menu_input_recording(index) : nullptr;
    if (start && index >= 0 && !path) return;
    const int r = host->input_play(start, syncs[std::clamp(opts.input_replay_sync, 0, 3)], path);
    input_status_stale = true;
    if (start && r == 1 && open_now.load()) {   // (the game in view while it plays: the compact bar, on Stop playback)
        tune_mode = true;
        if (cur_tab == TAB_RECORDINGS) tab_selected[TAB_RECORDINGS] = 0;
        menu_refresh = true;
    }
    input_refused = !start || r == 1 ? nullptr
                    : r == -1      ? "That recording could not be played (made from the game's start, or not a recording)."
                    : r == -2      ? "That recording starts on a map no teleport goes to: go there first."
                                   : "A recording or a playback is running: stop it first.";
}
void menu_input_play(int start) { input_play(start, -1); }
void menu_input_play_file(int index) { input_play(1, index); }

void startup_fade_to(float f) { startup_fade.store(f, std::memory_order_relaxed); }

// (longshot) The Upscaler row's one-time questions: NVIDIA's NGX started (dlss_probe), AMD's FSR 4 loaded
// (fsr4_probe), Intel's XeSS context made once (xess_probe), FSR 3.1's GPU check. Asked at the menu's first open, on the
// frame's thread, NGX's start alone holds that frame about 3 s: the game stands still and its sound runs on, out of
// step with a cutscene. So they are asked once on a thread of their own while the start-up screen shows, where nobody waits on
// them; each is locked and asked once (ngx_init, fsr4's load, fsr3::supported's static), so the menu's own calls at its
// open return at once, or wait for the rest of the one under way.
static void probe_upscalers_soon() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (!dev.device) return;
        std::thread([] {
            const auto t0 = std::chrono::steady_clock::now();
            dlss_probe();
            dlss_fg_probe();   // (after DLSS's own answer: whether the Frame generation by row offers DLSS FG)
            fsr4_probe();
            xess_probe();
            (void)fsr3::supported();
            logf("menu: the upscalers asked in the background while the start-up screen shows (%.0f ms)",
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }).detach();
    });
}

void upscalers_probe_start() { probe_upscalers_soon(); }

void startup_screen(const StartupView* view) {
    if (!view) {
        startup_on.store(false, std::memory_order_release);
        startup_fade.store(0, std::memory_order_relaxed);
        return;
    }
    StartupView v = *view;
    if (v.version.empty()) v.version = version_text(LONGSHOT_VERSION);   // ("v2.0.5"; "v2.0.5 dev" between releases)
    {
        std::lock_guard lk(startup_mu);
        startup = v;
    }
    startup_on.store(true, std::memory_order_release);
    probe_upscalers_soon();
}

static bool first_start = false;   // (menu_init: no options file yet. menu_first_start_for_gpu picks the preset for the card)
void menu_init() {
    static bool done;
    if (done) return;
    done = true;
    for (auto& count : item_counts) count.store(-1, std::memory_order_relaxed);
    bool has_options = false;
    bool has_output = false;   // (an output_res= line: the Output resolution chosen; none: hos_output_default, as the host)
    bool has_memory_budget = false;   // (a file from before memory_budget: its preset's)
    const char* cheats = getenv("HOS_OOT_CHEATS");
    cheats_env = cheats && !strcmp(cheats, "1");
    if (cheats_env) {
        const char* hearts = getenv("HOS_OOT_PROTECT_HEARTS");
        protect_hearts.store(hearts && !strcmp(hearts, "1"));
    }
    if (FILE* f = fopen(ini_path().c_str(), "r")) {
        char line[4096]; // Endpoint IDs are opaque and must not be cut into multiple settings.
        bool has_resolution = false;
        bool has_vibration = false;   // (longshot: an options file from before the Vibration row has no vibration= line)
        bool v2 = false;
        int version = 0;
        int old_dynres = -1;   // (the row before Resolution: 0, the full scale, is its 100%; 1 is Auto)
        bool menu_auto = false;   // (menu_size=0, the Menu size Auto there was before: Medium, not the Small the row's clamp gives)
        while (fgets(line, sizeof line, f)) {
            if (!strchr(line, '\n') && !feof(f)) {
                if (!strncmp(line, "audio_output_device_id=", 23)) output_want.clear();
                int c; while ((c = fgetc(f)) != '\n' && c != EOF) {}
                continue;
            }
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            // (a file with only the builders' Compile shaders in it, compile_shaders=, which make_game.py writes into a new
            // folder before the game ever started: still a first start, so Auto (this PC) chooses as it would with no file)
            has_options |= strcmp(line, "compile_shaders") != 0;
            for (Row& r : rows) {
                if (!r.value || r.value == &opts.game_lang || strcmp(line, ini_key(r))) continue;
                const int v = atoi(eq + 1);
                *r.value = r.value == &opts.output_res ? parse_output(eq + 1) : std::min(std::max(v, r.lo), r.hi);
            }
            if (!strncmp(line, "toggle_", 7))
                for (int i = 0; i < HOS_KEY_N; i++)
                    if (has_toggle(i) && !strcmp(line + 7, bind_names[i])) {
                        char* end;
                        const long value = strtol(eq + 1, &end, 10);
                        while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
                        toggle_set(i, end != eq + 1 && !*end && value == 1);
                    }
            has_resolution |= !strcmp(line, "resolution");
            has_vibration |= !strcmp(line, "vibration");   // (longshot)
            has_output |= !strcmp(line, "output_res");
            v2 |= !strcmp(line, "options_version");
            if (!strcmp(line, "options_version")) version = atoi(eq + 1);
            if (!strcmp(line, "dynamic_resolution")) old_dynres = atoi(eq + 1);
            if (!strcmp(line, "menu_size") && atoi(eq + 1) == 0) menu_auto = true;
            if (!strcmp(line, "auto_for")) auto_for = strtoull(eq + 1, nullptr, 16);
            if (!strcmp(line, "gpu_device_id")) gpu_saved_card.assign(eq + 1, strcspn(eq + 1, " \t\r\n"));   // (menu_set_gpu_count)
            if (!strcmp(line, "auto_tuned")) auto_tuned = strtoull(eq + 1, nullptr, 16);
            if (!strcmp(line, "auto_steps")) {
                int steps = 0, down = 0;
                if (sscanf(eq + 1, "%d,%d", &steps, &down) == 2)
                    auto_tuning.steps = std::max(steps, 0), auto_tuning.went_down = down != 0;
            }
            if (!strcmp(line, "memory_budget")) opts.budget = std::min(std::max(atoi(eq + 1), 0), 5), has_memory_budget = true;
            if (!strcmp(line, "microphone_device")) {   // (longshot: the microphone, by its name)
                mic_want = mic_key(eq + 1);
            }
            if (!strcmp(line, "audio_output_device_id")) output_want = output_id_key(eq + 1);
            if (!strcmp(line, "audio_output_device_name")) output_saved_name = output_name_key(eq + 1);
            if (!strcmp(line, "fullscreen_monitor_id")) monitor_want = monitor_choice::id_key(eq + 1);
            if (!strcmp(line, "fullscreen_monitor_name")) monitor_saved_name = output_name_key(eq + 1);
        }
        fclose(f);
        // (the saved microphone by its name, not its place; the host told the row's choice. Off or the default: no list, no SDL
        // audio at the start)
        refresh_outputs();
        if (can_outputs()) host->audio_output_select(output_want.c_str());
        refresh_monitors();
        if (can_monitors()) host->monitor_select(monitor_want.c_str());
        if (!mic_want.empty()) refresh_mics();
        else {
            if (opts.mic >= 2) opts.mic = 1;
            mic_tell_host();
        }
        if (!has_resolution && old_dynres >= 0) opts.resolution = old_dynres ? 0 : 1;
        if (menu_auto) opts.menu_size = 2;
        // (longshot) Rumble strength's Off, from before the Vibration row: Vibration Off, at the strength it had by default
        if (!has_vibration && opts.rumble <= 0) opts.vibration = 2, opts.rumble = 100;
        opts.vibration = std::min(std::max(opts.vibration, 0), 2);
        opts.rumble = rumble_step(opts.rumble);   // (a hand-edited 60: 50, on the arrows' steps)
        // (Game speed is Real time by default now: Per frame played at half speed at 30 fps, the "slow motion" players
        // reported. An options file from before kept the old default whether chosen or not: Real time for it too)
        if (!v2) opts.realtime = 1;
        // (Descriptor reuse is on by default from options_version 4: an older file wrote its 0 whether chosen or not, as
        // the off default it then was; on, it gives fewer stutters and a better 1% low)
        if (version < 4) opts.descriptor_reuse = 1;
        // (Esc opens the menu by default from options_version 5, as a PC game's pause menu: a file from before wrote its
        // Menu shortcut 0, Insert or F1, whether chosen or not, and moves to Insert, F1 or Esc, where Insert and F1 still
        // work. F1 only was chosen: kept)
        if (version < 5 && opts.menu_shortcut == 0) opts.menu_shortcut = 2;
        // (the Aspect ratio from options_version 6: a file from before wrote ultrawide=0, the 16:9 default then, whether
        // chosen or not. On a screen narrower than 16:9 host/hos_main.c starts the game at the monitor's shape for it
        // (host/hos_wide_size.h hos_aspect_on), and the row follows what the host started with: a picture narrower than
        // 16:9 there. On Linux too, where the menu has no screen size of its own. Not with HOS_ASPECT, a debugging switch
        // the host takes instead of the file: the shape it started with is then the switch's, not a move, and the next save
        // would keep a choice the player never made)
        if (version < 6 && opts.ultrawide == 0 && !getenv("HOS_ASPECT") && host && host->aspect_ratio) {
            const float started = host->aspect_ratio();
            if (started > 1.0f && started < 16.0f / 9.0f * 0.99f) opts.ultrawide = 1;
        }
        // (Sprint's Toggle is on by default from options_version 7: a file from before wrote toggle_sprint=0, the held
        // default then, whether chosen or not)
        if (version < 7) opts.sprint_toggle = 1;
    }
    toggle_set(HOS_KEY_SPRINT, opts.sprint_toggle != 0);   // (the host's as loaded: the default with no line)
    first_start = !has_options;
    sim_turf_settings_init();
    menu_colour_vision_changed(opts.colour_vision);
    menu_sync_hud_placement();
    sync_transition_autosave();
    grass_distance_now.store(opts.grass_distance, std::memory_order_relaxed);
    const char* handheld_env = getenv("HOS_HANDHELD");   // (host/nn_applet.c handheld(): the environment over the file's)
    if (!has_output && !(handheld_env ? !env_value_off(handheld_env) : opts.handheld != 0)) {
        // (none chosen, a new install: the size host/nn_applet.c took at boot, the game's 2560x1440 or the main screen's where
        // it has fewer pixels (a Steam Deck's 1280x800), hos_output_default. Saved from now on, so the two never part.
        // HOS_OUTPUT_RES, below, is over it, as over a chosen one)
        uint32_t sw = 0, sh = 0, w = 0, h = 0;
        host_screen_size(&sw, &sh);   // (not known: 0 x 0, the game's 2560x1440)
        hos_output_default(sw, sh, &w, &h);
        opts.output_res = output_choice_of(w, h);
        logf("vk: the Output resolution: none chosen, %ux%u (the game's 2560x1440, or the screen's where smaller)", w, h);
    }
    // An opt-in profile for 4 GB cards: keep existing options authoritative, but make a first run usable on
    // low-VRAM hardware without requiring the menu to render first.
    if (!has_options && env_on("RX_VK_LOW_SPEC")) {
        apply_preset(1);
        opts.upscale = 0;
        opts.sharpness = 0;
    }
    if (opts.preset > 0 && !has_memory_budget) opts.budget = opts.preset;   // (a file from before memory_budget: its preset's)
    if (opts.preset == kAutoPreset && !has_memory_budget) opts.budget = 0;   // (Auto: its tier's, menu_auto_for_gpu)
    menu_debug_rows(opts.descriptor_reuse, opts.sampler_cache);   // (the Debug rows that apply in play: what was loaded)
    fps_level_now.store(fps_stats::level_of(opts.show_fps), std::memory_order_relaxed);
    output_field_show();   // (longshot: Custom output size, the size of the player's own, as the file has it)
    // (the environment over the saved rows, for this run only: env_override, save)
    if (const char* e = getenv("RX_VK_FRAMEGEN_MULT")) env_override(&opts.framegen, std::min(std::max(atoi(e), 2), 8));
    if (const char* e = getenv("RX_VK_PACE")) env_override(&opts.pace, !env_value_off(e));
    if (const char* e = getenv("RX_VK_DLSS_PRE_BLOOM"); e && *e) env_override(&opts.dlss_pre_bloom, !env_value_off(e));
    if (const char* e = getenv("RX_VK_UPSCALE"))
        for (Row& r : rows)
            if (r.value == &opts.upscale) env_override(r.value, std::min(std::max(atoi(e), r.lo), r.hi));
    if (const char* e = getenv("HOS_REALTIME_STEP")) env_override(&opts.realtime, !env_value_off(e));
    if (const char* e = getenv("HOS_DYNRES")) env_override(&opts.resolution, env_value_off(e) ? 1 : 0);
    if (const char* e = getenv("HOS_SKIP_OPENING")) env_override(&opts.skip_opening, !env_value_off(e));
    // (vk_texture.cpp reads RX_VK_ASTC_RGBA8 over the row: the row shows it, fixed (row_fixed), and the ini keeps the player's)
    if (const char* e = getenv("RX_VK_ASTC_RGBA8")) env_override(&opts.astc_rgba8, !env_value_off(e));
    // (vk_main.cpp create_device reads RX_VK_PRESENT_QUEUE over the Present queue row, an empty value not: the same here)
    if (const char* e = getenv("RX_VK_PRESENT_QUEUE"); e && *e) env_override(&opts.present_queue, !env_value_off(e));
    if (const char* e = getenv("RX_VK_NO_INTEL_SERIAL")) env_override(&opts.intel_compiles, env_value_off(e) ? 2 : 1);
    if (const char* e = getenv("RX_VK_COMPUTE_QUICK_FIRST")) env_override(&opts.intel_quick, !env_value_off(e));
    if (const char* e = getenv("HOS_OUTPUT_RES")) {
        player_custom_output = custom_output;   // (parse_output replaces the player's own size with the environment's)
        env_override(&opts.output_res, parse_output(e));
    }
    // (the FSR 4 model for this run: RX_VK_FSR4_MODEL over the saved row; the engine's live value follows the row)
    const int fsr4_env = fsr4_model_env();
    if (fsr4_env >= 0) env_override(&opts.fsr4_model, fsr4_env);
    fsr4_set_model(opts.fsr4_model ? FSR4_MODEL_LITE : FSR4_MODEL_AUTO);
    outputs_sync();   // (the Aspect ratio follows the size read)
    restart_init();   // (what this run started with: the rows that take a restart differ from it once changed)
}

/// (longshot) The Quality preset "Auto (this PC)", once the card is chosen (vk_main.cpp pick_device): the profile auto_settings.h
/// picks from the hardware. A first start (no options file yet) starts on it. With Auto on, the profile is applied when the
/// hardware it was chosen for changes (another card, its memory, the screen's size, a build with or without DLSS or FSR 4):
/// the fingerprint is kept in the options file (auto_for=). Changing a row yourself comes back to Custom, and then nothing here
/// touches the settings; the rows no preset sets (the Output resolution) leave it on Auto, and Auto never sets the Output
/// resolution. The VRAM budgets are no preset's: one rule by the card (texture_budget.h, mirror_budget.h). An older or
/// smaller card does not start with the full-size budget, which can lose the device at the first scene.
/// RX_VK_AUTO_PRESET=0: a first start keeps the defaults.
/// RX_VK_AUTO_PRESET_VRAM_MB=<n>: the card's memory taken as n MB, to try the choice for a smaller or larger card
void menu_auto_for_gpu(const VkPhysicalDeviceProperties& props, ::auto_settings::Hardware hw) {
    const bool first = first_start;
    first_start = false;
    if (const char* e = getenv("RX_VK_AUTO_PRESET_VRAM_MB")) hw.vram_mb = (uint32_t)atoll(e);
    auto_choice = ::auto_settings::choose(hw);
    const uint64_t fp = auto_fp = ::auto_settings::fingerprint(hw, props.vendorID, props.deviceID);
    if (first && (!env_int("RX_VK_AUTO_PRESET", 1) || env_on("RX_VK_LOW_SPEC"))) return;   // (off, or chosen already: menu_init)
    char hw_line[200];
    snprintf(hw_line, sizeof hw_line, "%s, %u MB of video memory, %s, screen %dx%d at %d Hz, %u MB of RAM, %d threads",
             props.deviceName, (unsigned)hw.vram_mb, hw.integrated ? "integrated" : "dedicated", hw.screen_w, hw.screen_h,
             hw.refresh_hz, (unsigned)hw.ram_mb, hw.threads);
    if (first) opts.preset = kAutoPreset;
    if (opts.preset != kAutoPreset) {
        logf("vk: Auto (this PC) would be: %s (%s)", auto_choice.why, hw_line);   // (for a bug report: what Auto would choose)
        return;
    }
    // (an options file from the VRAM budget row, set under Auto: memory_budget=0, the preset Auto's tier is again, for Minimum's
    // mip trimming: low_vram_texture_mips)
    if (opts.budget == 0) opts.budget = auto_choice.tier;
    if (fp == auto_for) {
        logf("vk: Auto (this PC): the settings chosen for this PC before (%s)", hw_line);
        return;
    }
    apply_auto();   // (the hardware changed)
    // (a row that takes a restart Auto sets, the Profile (docked), applies at the next start, a first start's too: host/hos_main.c
    // read it at boot, before the device, and this run keeps what menu_init recorded. It is Auto's, not the player's: closing the
    // menu does not ask to restart for it, as after Later. The row still says After restart, and a change of the player's own to
    // any restart row asks again, about all of them)
    {
        int now[mr::kMax];
        restart_values(now);
        if (restart_state.pending(now)) restart_state.choose_later(now);
    }
    save();   // (for the next start: host/nn_applet.c read handheld_profile and output_res once, at boot in host/hos_main.c, before this)
    logf("vk: Auto (this PC): %s: %s (%s)", first ? "first start" : "the hardware changed", auto_choice.why, hw_line);
}

// (the switches read once, at the first present: they are for the whole run, and each present asked twice)
int menu_vsync() { static const int run = env_on("RX_VK_FIFO") ? 1 : env_on("RX_VK_IMMEDIATE") ? 2 : 0; return run ? run : opts.vsync; }
/// (longshot) The Threaded rendering row, resolved once for the run (threaded_rendering.h): the row as the options file had it
/// at the start (menu_init, asked here too: a reader can come before the device is picked), the physical cores among the
/// logical processors the process may run on (usable_cores: its affinity mask, a core's SMT siblings counted once; the
/// logical count where the cores cannot be read) and, for Auto, whether the mode is safe on this run. Known at the start and
/// read here: the capture off (RX_VK_CAPTURE=0: a replay behind the game is right only from the capture),
/// RX_VK_LOW_SPEC, a frame dump (RX_VK_DUMP), a benchmark replay (RX_VK_BENCH), and buffer pools with no mirrors at all
/// (pools_mirrored: RX_VK_MIRROR=0, an integrated GPU, GCN), asked last and only when Auto would otherwise pick a thread, in
/// either of its tiers, since it reads the device. Not known at the start, so not here: one pool refused its mirror or over the
/// mirrors' budget while the game runs (that needs the run-time fallback, which is not written). On is the player's word:
/// nothing is checked for it. Each reader takes its half: menu_render_thread, menu_recorder_thread
static const threaded::Choice& threaded_choice() {
    static const threaded::Choice choice = [] {
        menu_init();
        const unsigned cpus = usable_cpus(), cores = usable_cores();
        const char* capture = getenv("RX_VK_CAPTURE");
        // (vk_memory.cpp capture_on's reading: 0 none, anything else the capture)
        const char* unsafe = capture && atoi(capture) == 0 ? "RX_VK_CAPTURE" :
                             env_on("RX_VK_LOW_SPEC") ? "RX_VK_LOW_SPEC" : getenv("RX_VK_DUMP") ? "RX_VK_DUMP" :
                             getenv("RX_VK_BENCH") ? "RX_VK_BENCH" : nullptr;
        threaded::Choice c = threaded::choose(opts.threaded_rendering, cores, unsafe != nullptr);
        if (threaded::auto_picked(c) && !pools_mirrored()) {
            c = threaded::Choice{false, false, threaded::Why::AutoUnsafe};
            unsafe = "the buffer pools have no mirrors";
        }
        char line[256];
        threaded::describe(line, sizeof line, c, cores, cpus, unsafe);
        logf("vk: Threaded rendering: %s", line);
        return c;
    }();
    return choice;
}
bool menu_render_thread() { return threaded_choice().render_thread; }
bool menu_recorder_thread() { return threaded_choice().recorder_thread; }
bool resolution_auto() { return opts.resolution == 0; }
int resolution_scale() {
    const float s = upscaler_rows::percent(ur::snap_render(upscaler(), opts.resolution)) / 100.0f;
    return s > 0 ? (int)(twelfths(s) * 10000.0f / 12.0f + 0.5f) : 0;   // (the twelfth game_size labels)
}

/// The usable GPUs counted (vk_main.cpp pick_device, gpu_device_cards filled): the row offers them, and a card the options file
/// saved (gpu_device_id=) is found where it is in this run's list, whatever its place was (gpu_choice.h). One not here (taken
/// out, an eGPU unplugged) is Auto for the run, and stays saved (save) until the row is changed. A file from before the card
/// was saved keeps its place (gpu_device=), and saves the card at its next save
void menu_set_gpu_count(int count) {
    if (!gpu_saved_card.empty()) {
        const int at = gpu_choice::menu_of_card(gpu_device_cards, gpu_saved_card);
        gpu_card_missing = at == 0;
        if (at > 0 && at != opts.gpu_device)
            logf("vk: GPU device: the saved card (%s) is %d: %s now, not %d", gpu_saved_card.c_str(), at - 1,
                 gpu_device_names[at - 1].c_str(), opts.gpu_device - 1);
        else if (!at) logf("vk: GPU device: the saved card (%s) is not here: Auto for this run, kept for when it is back", gpu_saved_card.c_str());
        opts.gpu_device = at;
    }
    for (Row& r : rows) {
        if (r.value == &opts.gpu_device) {
            r.hi = count;
            if (opts.gpu_device > count) opts.gpu_device = 0;
        }
    }
    restart_init();
    for (int i = 0; i < (int)std::size(rows); i++)   // (the row's value the game started with, the cards counted)
        if (rows[i].value == &opts.gpu_device) restart_state.restart_value(restart_slot[i], opts.gpu_device);
}

bool menu_is_open() { return open_now.load(std::memory_order_relaxed); }
int menu_grass_distance() { return grass_distance_now.load(std::memory_order_relaxed); }
double menu_target_ms() { return fps_target_ms(); }   // (the performance summary's: perf_report.cpp)
int menu_fps_cap() { return fps_cap_now(); }          // (likewise: its fps_cap label)
bool scene_tonemap_on() { return dev.hdr && opts.hdr == 2 && opts.hdr_tonemap == 1; }
// (the tools off on the Debug tab: flight, the free camera and heart protection put away; -1, heart protection never set, is the
// host's own HOS_OOT_PROTECT_HEARTS)
int menu_heart_protection() {
    const int v = protect_hearts.load(std::memory_order_relaxed);
    return cheats_on() || v < 0 ? v : 0;
}
int menu_player_flight() { return cheats_on() ? player_flight.load(std::memory_order_relaxed) : -1; }
int menu_free_camera() { return cheats_on() ? free_camera.load(std::memory_order_relaxed) : 0; }
int menu_link_speed() { return cheats_on() ? link_speeds[std::clamp(link_speed.load(std::memory_order_relaxed), 0, kLinkSpeeds - 1)] : 100; }
int menu_memory_tier() {
    if (opts.budget > 0) return opts.budget;
    if (opts.preset > 0 && opts.preset < kAutoPreset) return opts.preset;   // (a file with a preset and memory_budget=0)
    return auto_choice.tier;
}
/// The size the host draws the sun's cascades at (HOS_OPT_SHADOW_RES: 0 the game's 1600, 1 2200, 2 3072): the row's, or for
/// Auto the one for the card's memory tier, the preset's (opts.budget) or else Auto's own for this PC (auto_choice): the
/// game's on Minimum and Low (4 and 6 GB, where 113 MB is a lot), 2200 on Balanced (8 GB), 3072 from High (12 GB) up
int menu_shadow_res() {
    if (opts.shadow_res > 0) return std::min(opts.shadow_res, 3) - 1;
    const int tier = opts.budget > 0 ? opts.budget : auto_choice.tier;
    return tier == 1 || tier == 2 ? 0 : tier >= 4 ? 2 : 1;   // (an unknown tier: 2200, the middle)
}
int menu_transition_autosave() { return transition_autosave_enabled.load(std::memory_order_relaxed); }
int menu_damage_taken() { return cheats_on() ? opts.damage_taken : 1; }   // (Normal while the tools are off; the choice is kept)
int menu_take_cheat_action() { return cheat_action.exchange(0, std::memory_order_relaxed); }
// (longshot) what plays the controller used last, as the host wrote it back (menu_option_set)
int menu_vibration_status_option() { return vibration_status_cache.load(std::memory_order_relaxed); }

void menu_note_present_sizes(uint32_t picture_w, uint32_t picture_h, uint32_t window_w, uint32_t window_h) {
    const auto c = [](uint32_t v) { return (uint64_t)std::min(v, 0xffffu); };
    present_sizes.store(c(picture_w) << 48 | c(picture_h) << 32 | c(window_w) << 16 | c(window_h), std::memory_order_relaxed);
}
void menu_note_frame(bool game_frame, int shown) {
    if (shown) shown_frames.fetch_add(shown, std::memory_order_relaxed);
    if (game_frame) game_frames.fetch_add(1, std::memory_order_relaxed);
}

// The Fullscreen row and F11 are one setting: the row asks the window's thread (host/display.c), F11 shows in the row
void fullscreen_tick() {
    static int applied = 0, settle = 0;
    const HWND main_window = app_window();
    if (!main_window) return;
    static const bool env = getenv("HOS_FULLSCREEN") != nullptr;   // (a test's own choice: the row is left alone)
    if (env) return;
    if (opts.fullscreen != applied) {
        applied = opts.fullscreen;
        settle = 60;
        PostMessageA(main_window, WM_APP + 7, (WPARAM)applied, 0);
        return;
    }
    if (settle > 0) { settle--; return; }
    const int now = !(GetWindowLongA(main_window, GWL_STYLE) & WS_CAPTION);
    if (now != applied) {
        opts.fullscreen = applied = now;
        menu_refresh = true;
        save();
    }
}

/// The FPS counter's lines past the number, four times a second: the game's frame time over the last half second and
/// the 1% low over 10 s; the Breakdown's parts and the GPU's time over the half second, the shaders built and the draws left
/// out over the graph's 4 s
void fps_texts(int64_t now) {
    namespace fs = fps_stats;
    const int level = fs::level_of(opts.show_fps);
    if (level < fs::kFrameTime) return;
    const fs::Summary half = fps_summary(now, fs::kSecond / 2, fs::kLowWindow);
    fs::frame_time_text(fps_ms_text, sizeof fps_ms_text, half.avg_ms);
    fs::low_text(fps_low_text, sizeof fps_low_text, half.low_fps);
    if (!half.frames) fps_ms_text[0] = 0;
    fps_ms_now = half.avg_ms;
    if (level < fs::kBreakdown) return;
    for (int p = 0; p < fs::kParts; p++) fs::frame_time_text(fps_part_text[p], sizeof fps_part_text[p], half.part[p]);
    fs::frame_time_text(fps_gpu_text, sizeof fps_gpu_text, half.gpu_ms);
    for (int k = 0; k < fs::kRtParts; k++) fs::frame_time_text(fps_rt_text[k], sizeof fps_rt_text[k], half.rt[k]);
    fps_rt_on = half.rt[fs::kRtWork] + half.rt[fs::kRtGpu] + half.rt[fs::kRtIdle] > 0;
    snprintf(fps_gpu_busy_text, sizeof fps_gpu_busy_text, "%.0f%%", half.avg_ms > 0 ? std::min(100.0, 100.0 * half.gpu_ms / half.avg_ms) : 0.0);
    const fs::Summary graph = fps_summary(now, fs::kGraphWindow, 0);
    snprintf(fps_built_text, sizeof fps_built_text, "%d", graph.pipelines);
    snprintf(fps_left_text, sizeof fps_left_text, "%d", graph.left_out);
    if (level < fs::kDev) return;
    // (Dev: the renderer's counters since the last time, every half second, by the frames presented in that time)
    static int64_t at = 0, last[8] = {};
    if (at && now - at < fs::kSecond / 2 && now >= at) return;
    int64_t draws = 0, skipped = 0, dispatches = 0, pipelines = 0, passes = 0, mesh = 0, uploads = 0, astc = 0, bytes = 0;
    double pipeline_ms = 0;
    draw_stats(&draws, &skipped, &dispatches, &pipelines);
    draw_stats2(&passes, &mesh, &pipeline_ms);
    texture_stats(&uploads, &astc, &bytes);
    int64_t mirrors = 0, sync_b = 0, sync_c = 0, wb_b = 0;
    uint64_t mirror_mb = 0;
    memory_stats(&mirrors, &mirror_mb, &sync_b, &sync_c, &wb_b);
    const int64_t counts[8] = {draws, dispatches, passes, skipped, bytes, slot_wait_ns(), prof_ns[PROF_QSUBMIT], wb_b};
    const bool first = !at || now < at || now - at > 2 * fs::kSecond;   // (Dev just chosen again: no stretch from before)
    const double seconds = first ? 0 : (double)(now - at) / fs::kSecond;
    const int frames = first ? 0 : fps_summary(now, now - at, 0).frames;
    uint64_t used = 0, allowed = 0;
    const bool known = vram_usage(&used, &allowed) && dev.memory_budget;   // (the status line asks the driver anew: vram_usage_now)
    fps_vram_over = known && allowed && used > allowed;
    fs::vram_line(fps_dev_text[fs::kVram], sizeof fps_dev_text[0], used, allowed, known);
    fs::textures_line(fps_dev_text[fs::kTextures], sizeof fps_dev_text[0], texture_count(), texture_bytes());
    fs::per_frame_line(fps_dev_text[fs::kDraws], sizeof fps_dev_text[0], counts[0] - last[0], frames);
    fs::per_frame_line(fps_dev_text[fs::kDispatches], sizeof fps_dev_text[0], counts[1] - last[1], frames);
    fs::per_frame_line(fps_dev_text[fs::kPasses], sizeof fps_dev_text[0], counts[2] - last[2], frames);
    fs::per_frame_line(fps_dev_text[fs::kSkipped], sizeof fps_dev_text[0], counts[3] - last[3], frames);
    fs::rate_line(fps_dev_text[fs::kUploads], sizeof fps_dev_text[0], counts[4] - last[4], seconds);
    snprintf(fps_dev_text[fs::kPipelines], sizeof fps_dev_text[0], "%lld", (long long)pipelines);
    const auto per_frame = [&](int line, int64_t d, double unit, const char* fmt) {   // (ms or MB a frame; "-" with no frames yet)
        if (frames > 0) snprintf(fps_dev_text[line], sizeof fps_dev_text[0], fmt, d / unit / frames);
        else snprintf(fps_dev_text[line], sizeof fps_dev_text[0], "-");
    };
    per_frame(fs::kSlotWait, counts[5] - last[5], 1e6, "%.2f ms");
    per_frame(fs::kQueueSubmit, counts[6] - last[6], 1e6, "%.2f ms");
    per_frame(fs::kWriteBack, counts[7] - last[7], 1048576.0, "%.1f MB");
    at = now;
    std::memcpy(last, counts, sizeof last);
}

/// A rate over the half-second that just ended. The first call only starts that interval: the frames since launch,
/// divided by one second, were shown as the rate the moment the counter was turned on.
bool fps_take(int64_t now, int64_t shown, int64_t game, int64_t* next, int64_t* last_shown, int64_t* last_game,
              double* shown_rate, double* game_rate) {
    if (now < *next) return false;
    const bool have = *next != 0;
    if (have) {
        const double secs = (now - *next + 500000000) / 1e9;
        *shown_rate = (shown - *last_shown) / secs;
        *game_rate = (game - *last_game) / secs;
    }
    *last_shown = shown;
    *last_game = game;
    *next = now + 500000000;
    return have;
}

// (longshot) the rows' values and the options file, kept apart between menu_tick (the present's thread), menu_option_set
// (the game's) and the control bridge's menu_setting and menu_setting_set (its own thread). Recursive: a setter reached from
// inside menu_tick's frame on the same thread does not lock itself out
std::recursive_mutex menu_mu;

// The native window was rebuilt after Yes. The next valid stock frame supplies the running size.
static void menu_output_rebuilt(uint32_t width, uint32_t height) {
    const Out saved_custom = custom_output;
    output_started = output_choice_of(width, height);
    if (output_started == custom_index() && (saved_custom.w != width || saved_custom.h != height)) {
        output_extras.push_back({width, height});
        custom_output = saved_custom;
        output_started = last_choice();
    }
    for (Row& r : rows) {
        if (r.value != &opts.output_res) continue;
        r.hi = last_choice();
        const int slot = restart_slot_of(r);
        const int running = (int)(width << 16 | height);
        restart_state.restart_value(slot, running);
        if (restart_changed(r)) restart_state.later = false;
        break;
    }
    menu_refresh = true;
}

void menu_option_set(int id, int value) {
    if (id == OOT_CHEAT_AGE_ID) {   // (Link's age: the Switch age row shows it)
        if (link_age.exchange(value, std::memory_order_relaxed) != value && page == 0 && cur_tab == TAB_DEBUG) menu_refresh = true;
        return;
    }
    if (id >= OOT_CHEAT_ITEM_BASE && id < OOT_CHEAT_ITEM_BASE + kItems) {   // (the game side's inventory counts)
        if (item_counts[id - OOT_CHEAT_ITEM_BASE].exchange(value, std::memory_order_relaxed) != value && page == 4)
            menu_refresh = true;
        return;
    }
    if (id == HOS_OPT_CHEAT_FLIGHT) {
        if (cheats_on()) {
            int expected = value ? 0 : 1;
            player_flight.compare_exchange_strong(expected, value != 0, std::memory_order_relaxed);
        }
        return;
    }
    if (id == HOS_OPT_VIBRATION_STATUS) {   // (longshot: what plays the controller used last, host/pad_sdl.c)
        vibration_status_cache.store(value, std::memory_order_relaxed);
        return;
    }
    if (id != 7) return;
    std::lock_guard lk(menu_mu);   // (the counts above are atomics: the game's thread waits for no menu frame for them)
    opts.mouse_sens = std::min(std::max(value, 10), 500);   // (the keys work while the menu is closed: it shows it when opened)
    save();
}

// ---- (longshot) the control bridge's option_get and option_set (host/control.c, docs/dev/reference/control-bridge.md) ----
// The settings rows (a value, not a heading or a debug tool, shown on this PC: row_shown), found by their key in
// longshot_options.ini (ini_key), which stays the same while rows come and go or are renamed. The bridge calls these from
// its own thread: menu_mu keeps them apart from menu_tick (the present's thread) and from menu_option_set (the game's)
bool bridge_row(int i) { return rows[i].value && !rows[i].heading && rows[i].cheat < 0 && row_shown(i); }
void bridge_fill(int i, MkwVkSetting* out) {
    static const char* const tabs[] = {"graphics", "audio", "gameplay", "controls", "nfc", "mods", "debug", "motion",
                                       "audio", "gameplay", "debug", "graphics", "controls"};
    static_assert(sizeof tabs / sizeof tabs[0] == TAB_PAGE + 1, "a name for each MenuTab");
    const Row& r = rows[i];
    *out = MkwVkSetting{};
    snprintf(out->key, sizeof out->key, "%s", ini_key(r));
    snprintf(out->name, sizeof out->name, "%s", r.name);
    snprintf(out->tab, sizeof out->tab, "%s", r.tab >= 0 && r.tab <= TAB_PAGE ? tabs[r.tab] : "");
    out->value = *r.value, out->lo = r.lo, out->hi = r.hi, out->step = r.step;
    out->restart = r.restart, out->fixed = row_fixed(r.value);
    snprintf(out->text, sizeof out->text, "%s", r.text ? r.text(out->value) : "");
}
// (the Upscaler row's questions, asked before menu_mu is taken: row_shown and row_fixed ask upscalers_available(), which
// waits for the start-up probe (NGX's start, ~3 s) while it runs. Under menu_mu that held menu_tick, and the present with
// it; this way only the bridge's thread waits, and once answered the questions return at once)
static void bridge_probe_first() { (void)upscalers_available(), (void)dlss_why(); }
/// The index-th settings row (an enumeration, for listing them; the index is not a name: rows come and go). 0: past the last
int menu_setting(int index, MkwVkSetting* out) {
    bridge_probe_first();
    std::lock_guard lk(menu_mu);
    for (int i = 0, n = 0; i < (int)std::size(rows); i++)
        if (bridge_row(i) && n++ == index) return bridge_fill(i, out), 1;
    return 0;
}
/// A row whose choices are not in the order of their numbers (the output sizes go by width, the Render resolution's slider by
/// size, the Upscaler by engine), walked in the arrows' own order to the value asked for: one way, then the other, each way
/// until at() holds, the row stops at an end (state() unchanged) or it comes round to where that way began. A value none of
/// its steps reach here leaves the row as it was. state(): all a step changes (the Render resolution's Native moves the
/// Upscaler, not the value); step(dir): one arrow press. Stepping towards the number stopped short: from 2048x1152, 1920x1080
/// (3) went on to 5120x1440 (4), and from DLSS Auto any Render resolution stopped at Native, which leaves the value as it was
template <class State, class At, class Step>
bool walk_to(State state, At at, Step step, int limit) {
    const auto start = state();
    for (const int dir : {1, -1}) {
        const auto from = state();
        for (int k = 0; k < limit && !at(); k++) {
            const auto was = state();
            step(dir);
            if (state() == was || state() == from) break;
        }
        if (at()) return true;
    }
    for (int k = 0; k < limit && state() != start; k++) step(1);   // (back where it was: an end-stopped row is at its first)
    return false;
}

/// (longshot) The Quality preset Auto (this PC)'s measured tuning in play (auto_settings.h tune): each of the replay's 120-present
/// windows (vk_replay.cpp), the Render resolution one step lighter while frames are missed with the GPU over 90% of the cap's
/// frame time, or one heavier while the cap holds with the GPU under 50%, up to the best mode that still upscales (never
/// native); at most two steps (kept across restarts, auto_steps=), then done for this PC (auto_tuned=). The CPU holding the
/// frame rate ends it for this run only. Not with Frame generation on (the frames shown are not the game's) nor with another
/// preset. RX_VK_AUTO_TUNE=0: off. Before the tuning, in play (the upscaler has run, NGX has answered): the Render resolution for
/// the upscaler that runs, when it is not the one Auto chose (auto_settings.h resolution_for)
void menu_auto_tune(double gpu_ms, double draws, double fps) {
    static const bool on = env_int("RX_VK_AUTO_TUNE", 1) != 0;
    ::auto_settings::Tuning& tuning = auto_tuning;
    std::lock_guard lk(menu_mu);
    if (opts.preset != kAutoPreset || !auto_for) return;
    if (draws >= 150) {
        const ::auto_settings::Choice& c = auto_choice;
        const int u = upscaler(), v = opts.resolution;
        const int to = ::auto_settings::resolution_for(c, ur::engine(u), v, c.resolution ? ur::snap_render(c.upscaler, c.resolution) : 0);
        if (to && ur::snap_render(u, to) != v) {
            opts.resolution = ur::snap_render(u, to);
            env_release(&opts.resolution);
            samplers_changed();
            menu_refresh = true;
            tuning.n = 0;   // (the windows gathered were at the other share)
            static const char* const name[ur::kUpscalers] = {"the game's own", "DLSS", "DLAA", "FSR 3.1", "FSR 4", "FSR 4", "Off",
                                                             "XeSS", "XeSS"};
            logf("vk: Auto (this PC): %s runs, not the %s chosen: Render resolution %d%% -> %d%%",
                 name[std::clamp(u, 0, ur::kUpscalers - 1)], name[std::clamp(c.upscaler, 0, ur::kUpscalers - 1)], ur::percent(v),
                 ur::percent(opts.resolution));
            save();
            return;
        }
    }
    if (!on || auto_tuned == auto_for || opts.framegen || tuning.done) return;
    // (the frame time the game runs to: the cap it started with, HOS_FPS_MAX over it, or the display's refresh with V-Sync On;
    // as the FPS counter's line. Uncapped without V-Sync: the display's refresh, else 60)
    const double target = fps_target_ms();
    const int refresh = (display_refresh_mhz() + 500) / 1000;
    const float cap = target > 0 ? (float)(1000.0 / target) : refresh > 0 ? (float)refresh : 60.0f;
    const int u = upscaler(), v = opts.resolution;
    if (ur::full_scale(u) || v == 0) return;   // (Native, or Auto's dynamic share: no steps to measure along)
    // (heavier: up to the best mode that still upscales, past what the profile chose; never native: auto_settings.h best_step)
    const int ceiling = ::auto_settings::best_step(u);
    const int lighter = ur::step(u, v, 1), heavier = ur::step(u, v, -1);
    const bool can_lighter = lighter != 0 && ur::percent(lighter) < ur::percent(v);
    const bool can_heavier = heavier != 0 && ur::percent(heavier) > ur::percent(v) && ur::percent(heavier) <= ur::percent(ceiling);
    const auto verdict = ::auto_settings::tune(tuning, (float)gpu_ms, (float)draws, (float)fps, cap, can_lighter, can_heavier);
    if (verdict == ::auto_settings::WAIT) return;
    if (verdict == ::auto_settings::LIGHTER || verdict == ::auto_settings::HEAVIER) {
        opts.resolution = verdict == ::auto_settings::LIGHTER ? lighter : heavier;
        env_release(&opts.resolution);
        samplers_changed();   // (the upscalers' texture LOD bias follows the 3D's share, as when the row is set)
        menu_refresh = true;
        logf("vk: Auto (this PC), measured in play: %.0f fps of %.0f, the GPU at %.1f ms a frame of %.1f: Render resolution %d%% -> "
             "%d%%", tuning.fps_mid, cap, tuning.gpu_mid, 1000.0 / cap, ur::percent(v), ur::percent(opts.resolution));
    } else if (verdict == ::auto_settings::CPU_BOUND) {
        // (this run only: early in play pipelines may still be compiling and textures streaming in; the next start measures again)
        logf("vk: Auto (this PC), measured in play: the CPU holds the frame rate (%.0f fps of %.0f, the GPU at %.1f ms a frame of "
             "%.1f): Render resolution %d%% kept for this run", tuning.fps_mid, cap, tuning.gpu_mid, 1000.0 / cap, ur::percent(v));
        return;
    } else {
        auto_tuned = auto_for;
        logf("vk: Auto (this PC), measured in play: settled (%.0f fps of %.0f, the GPU at %.1f ms a frame of %.1f): Render resolution "
             "%d%% kept", tuning.fps_mid, cap, tuning.gpu_mid, 1000.0 / cap, ur::percent(v));
    }
    save();   // (after a step too: its steps, auto_steps=, so a restart before the next verdict keeps the no-seesaw rule)
}

/// The settings row whose ini key is key, set to value as the menu's Left and Right set it (set_row: cut to its range, the
/// Quality preset made Custom unless the row is exempt, a restart noted, kept in the options file). The rows the arrows step
/// through a list of their own (the Upscaler, Render resolution, the output resolution) are walked to value: walk_to. out: the
/// row as it is now. 1 set, 0 no such row, -1 nothing to choose on this PC (row_fixed), -2 none of a list row's steps reach
/// value here (the row and the rest as they were)
int menu_setting_set(const char* key, int value, MkwVkSetting* out) {
    bridge_probe_first();
    std::lock_guard lk(menu_mu);
    int i = 0;
    while (i < (int)std::size(rows) && !(bridge_row(i) && !strcmp(ini_key(rows[i]), key))) i++;
    if (i == (int)std::size(rows)) return 0;
    Row& r = rows[i];
    if (row_fixed(r.value)) return bridge_fill(i, out), -1;
    const int want = std::min(std::max(value, r.lo), r.value == &opts.pad_layout ? std::min(r.hi, 3) : r.hi);   // (4: Custom, shown only)
    if (r.value == &opts.output_res && output_choice(want) == want &&
        dm::ratio_name(size_of(output_at(want))) != dm::ratio_name(size_of(output_at(opts.output_res)))) {
        // (a size of another shape: the Aspect ratio moves to it first, as a player would, which takes the Output resolution
        // into that shape's sizes; the walk below goes on from there)
        for (Row& a : rows)
            if (a.value == &opts.ultrawide) set_row(a, aspect_for_choice(want), 1, false);
    }
    if (r.value == &opts.dlss || r.value == &opts.resolution || r.value == &opts.output_res) {
        // (the Render resolution's Native is a place of its own, DLAA or FSR 4's native AA, whatever the value it kept; the
        // Upscaler that runs, as set_row steps it: a saved DLAA that cannot run here is the game's own, and its values count)
        const auto native = [&] { return r.value == &opts.resolution && ur::full_scale(upscaler()); };
        // (a value none of the steps reach: everything the steps on the way changed is put back, the Quality preset and the
        // rows the environment holds too)
        const Options opts_was = opts;
        EnvOverride env_was[sizeof env_overrides / sizeof env_overrides[0]];
        std::copy(std::begin(env_overrides), std::end(env_overrides), env_was);
        const mr::State restart_was = restart_state;
        if (!walk_to([&] { return std::pair(native() ? -1 : *r.value, opts.dlss); }, [&] { return *r.value == want && !native(); },
                     [&](int dir) { set_row(r, *r.value + dir * r.step, dir, false); }, 64)) {
            opts = opts_was;
            toggle_set(HOS_KEY_SPRINT, opts.sprint_toggle != 0);
            std::copy(std::begin(env_was), std::end(env_was), env_overrides);
            restart_state = restart_was;
            samplers_changed();
            menu_refresh = true;
            bridge_fill(i, out);
            return -2;
        }
        save();
    } else if (want != *r.value) {
        // (Frame generation: the value as asked, dir 0, not an arrow's step from the value shown (set_row: DLSS FG's top))
        set_row(r, want, r.value == &opts.framegen ? 0 : want > *r.value ? 1 : -1);
    }
    menu_refresh = true;
    bridge_fill(i, out);
    return 1;
}

/// (longshot) the mouse on the inventory: a section tab chooses it, a card the item (a click selects it, it does not edit),
/// the page arrows turn the page with the choice, the action button and the amount's halves act; the wheel turns the page
/// shown and leaves the choice where it is; a right click goes back
void inventory_mouse(const MkwVkPointer& ptr, int mx, int my, bool* press, bool& changed) {
    const menu_hit::Hit hit = hits.at((float)mx, (float)my);
    if (ptr.pressed & HOS_MOUSE_LEFT) {
        if (hit.part == menu_hit::kRow && hit.index >= 0 && hit.index <= kItemSections) choose_inventory_filter(hit.index), changed = true;
        else if (hit.part == menu_hit::kListLine && hit.index >= inventory_begin() && hit.index < inventory_end())
            item_choice = hit.index, inventory_amount = false, changed = true;
        else if (hit.part == menu_hit::kButton && hit.index == 0) press[7] = true;
        else if (hit.part == menu_hit::kButton && hit.index == 1 && hit.side) press[hit.side < 0 ? 3 : 4] = true;
        else if (hit.part == menu_hit::kButton && (hit.index == 2 || hit.index == 3))   // (the page arrows)
            press[hit.index == 2 ? 13 : 14] = true, inventory_amount = false;
    }
    if (ptr.pressed & HOS_MOUSE_RIGHT) press[5] = true;
    if (ptr.wheel) {
        const int was = inventory_page;
        inventory_page = std::max(0, inventory_page - ptr.wheel);   // (drawing keeps it to the pages there are)
        changed |= inventory_page != was;
    }
}
/// (longshot) the mouse on the teleport list: a click on a card selects it (Travel, or confirm, goes), the Travel button
/// goes; the wheel scrolls the cards and leaves the choice where it is; a right click goes back
void teleport_mouse(const MkwVkPointer& ptr, int mx, int my, bool* press, bool& changed) {
    const menu_hit::Hit hit = hits.at((float)mx, (float)my);
    if (ptr.pressed & HOS_MOUSE_LEFT) {
        if (hit.part == menu_hit::kListLine && hit.index >= 0 && hit.index < kTeleports)
            teleport_choice = hit.index, teleport_follow = true, changed = true;
        else if (hit.part == menu_hit::kButton) press[7] = true;
    }
    if (ptr.pressed & HOS_MOUSE_RIGHT) press[5] = true;
    if (ptr.wheel) teleport_wheel += ptr.wheel, teleport_follow = false, changed = true;
}
/// A debug tool's row on the Debug tab (Row::cheat i): Left and Right, and confirm (press[7]: A, Enter, a click). Teleport and
/// Inventory open their pages; back from them comes to the Debug tab, the row still selected
void cheat_press(int i, const bool* press, bool& changed) {
    if (i < 2 && (press[3] || press[4] || press[7])) {   // (Left Off, Right On, confirm flips)
        const bool was = i == 0 ? protect_hearts.load() == 1 : player_flight.load() >= 0;
        const bool on = press[7] ? !was : press[4];
        if (i == 0) protect_hearts.store(on ? 1 : 0);
        else if (on != was) player_flight.store(on ? 0 : -1);
        changed = true;
    }
    if (i == 4 && (press[3] || press[4])) {
        clock_hour = (clock_hour + (press[4] ? 1 : 23)) % 24;
        changed = true;
    }
    if (i == 5 && (press[3] || press[4])) {
        pouch_item = (pouch_item + kPouchItems + (press[4] ? 1 : -1)) % kPouchItems;
        changed = true;
    }
    if (i == 11 && (press[3] || press[4])) {   // (Movement speed: stops at 1x and 3x)
        link_speed.store(std::clamp(link_speed.load() + (press[4] ? 1 : -1), 0, kLinkSpeeds - 1));
        changed = true;
    }
    if (i == 6 && (press[3] || press[4])) {
        const int step = press[4] ? 1 : -1;
        pouch_delta = std::clamp(pouch_delta + step, -10, 10);
        if (!pouch_delta) pouch_delta = step;
        changed = true;
    }
    if (press[7] && i >= 2 && i <= 4) cheat_action.store(i == 4 ? 100 + clock_hour : i);
    if (press[7] && i == 6) cheat_action.store(200 + pouch_item * 32 + pouch_delta + 10);
    if ((press[3] || press[4] || press[7]) && i == 7) {   // (an On / Off as the first two: Left Off, Right On, confirm flips)
        free_camera.store(press[7] ? !free_camera.load() : press[4] ? 1 : 0);
        changed = true;
    }
    if (press[7] && i == 8) page = 3, teleport_follow = true, changed = true;
    if (press[7] && i == 10) page = 4, choose_inventory_filter(0), changed = true;
    if (i == 9 && (press[3] || press[4])) {   // (Kid | Adult: Left Kid, Right Adult, a switch when Link is the other age)
        const int age = link_age.load(std::memory_order_relaxed);
        if (age >= 0 && age != (press[4] ? 1 : 0)) {
            cheat_action.store(999);
            open_now = false;
            changed = true;
        }
    }
    if (press[7] && i == 9) {
        cheat_action.store(999);   // (host/hos_main.c OOT_AGE_ACTION)
        open_now = false;
        changed = true;
    }
}

/// (longshot) The Game language row's choice written to language.txt once the player leaves the row (another row, page or
/// tab, or the menu closed), not at each step on the way to it. When it cannot be written (a read-only folder) the row goes
/// back to what language.txt says, so it never shows a language that was not saved
void game_lang_tick() {
    static const int row = [] {
        for (int i = 0; i < (int)std::size(rows); i++)
            if (rows[i].value == &opts.game_lang) return i;
        return -1;
    }();
    if (row < 0 || opts.game_lang == i18n::game_lang_index()) return;
    const bool on_row = open_now.load() && page == 0 && cur_tab == rows[row].tab &&
                        tab_row_to_global(cur_tab, tab_selected[cur_tab]) == row;
    if (on_row) return;
    if (!i18n::set_game_language(opts.game_lang)) {
        logf("menu: language.txt could not be written: the game language stays %s", i18n::game_lang_code(i18n::game_lang_index()));
        opts.game_lang = i18n::game_lang_index();
        menu_refresh = true;
    }
}

/// (longshot) The screen the window is on, followed while the game runs (every 30 presents): moved to another monitor, or its
/// desktop size changed, the sizes it has that are not offered yet (its modes, its own size, the common sizes of their shapes)
/// join the Output resolution's choices after the player's own, so the numbers chosen keep their sizes; the Aspect ratio row
/// keeps the shape it is at (the list can gain one), and so do the run's start and the restart question's. HOS_DISPLAY_MODES:
/// the list as given
void outputs_follow_screen() {
    static int every = 0;
    static std::string followed_monitor;
    if (++every % 30) return;
    const std::string previous_monitor = followed_monitor;
    const int previous_pick = monitor_pick;
    refresh_monitors();
    const std::string current_monitor = monitor_choice::current_id(fullscreen_monitors);
    if (previous_pick != monitor_pick || previous_monitor != current_monitor) menu_refresh = true;
    if (getenv("HOS_DISPLAY_MODES")) return;
    uint32_t sw = 0, sh = 0;
    if (!screen_size(&sw, &sh) || (sw == outputs_screen.w && sh == outputs_screen.h && previous_monitor == current_monitor)) return;
    outputs_screen = {sw, sh};
    followed_monitor = current_monitor;
    const int slot = [] {
        for (const Row& r : rows)
            if (r.value == &opts.ultrawide) return restart_slot_of(r);
        return -1;
    }();
    // (the shapes the row's values name now: the same shapes after the list grows)
    const bool at_monitor = opts.ultrawide == aspect_monitor_value();
    const std::string now = aspect_shape(opts.ultrawide), started = aspect_shape(aspect_started),
                      restart_from = slot >= 0 ? aspect_shape(restart_state.start[slot]) : std::string();
    std::vector<dm::Size> modes = screen_modes();
    add_screen_size(modes, sw, sh);
    int added = 0;
    for (dm::Size s : dm::with_common(dm::offered(modes))) {
        const int before = last_choice();
        if (s.w == kGameOutput.w && s.h == kGameOutput.h) continue;
        const Out kept = custom_output;
        const int at = output_choice_of(s.w, s.h);
        custom_output = kept;   // (output_choice_of takes a size it does not have as the player's own: not this one)
        if (at == custom_index() && !(kept.w == s.w && kept.h == s.h)) output_extras.push_back({s.w, s.h}), added++;
        (void)before;
    }
    if (added) {
        for (Row& r : rows) {
            if (r.value == &opts.output_res) r.hi = last_choice();
            if (r.value == &opts.ultrawide) r.hi = aspect_last();
        }
        opts.ultrawide = at_monitor && !monitor_shape().empty() ? aspect_index(monitor_shape()) : aspect_index(now);
        aspect_started = aspect_index(started);
        if (slot >= 0) restart_state.restart_value(slot, aspect_index(restart_from));
        menu_refresh = true;
    }
    logf("vk: the window's screen is %ux%u (%s): %d sizes added to the Output resolution's", sw, sh,
         dm::ratio_name({sw, sh}).c_str(), added);
}

void menu_tick() {
    std::lock_guard lk(menu_mu);   // (the control bridge's settings: menu_setting_set)
    share_finish();   // (Save my pipeline list's worker, done)
    fullscreen_tick();
    game_lang_tick();
    outputs_follow_screen();
    sim_turf_settings_tick(prof_now());
    if (const uint64_t size = output_preview_take_rebuilt_size())
        menu_output_rebuilt((uint32_t)(size >> 32), (uint32_t)size);
    if (output_preview_target.w) {
        const int remaining = output_preview_seconds();
        if (remaining == -2) answer_native_preview(false);
        else if (remaining == 0) {
            edit_end(false);
            listening = 0;
            unavailable_native_preview();
        }
        else if (remaining > 0) {
            if (!output_preview_countdown) {
                edit_end(false);
                listening = 0;
                page = 5;
                ask_sel = 0;
                open_now = true;
                logf("menu: live output confirmation opened");
            }
            output_preview_countdown = remaining;
            menu_refresh = true;
        }
    }
    enum { K_N = 18 };
    static bool held[K_N];
    static const int keys[K_N] = {VK_INSERT, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_ESCAPE, VK_F1,   // (F1: for keyboards without Insert)
                                  VK_RETURN, VK_DELETE, VK_BACK,
                                  'Q', 'E', VK_TAB,   // (the tabs: Q and E, Tab and Shift+Tab)
                                  VK_PRIOR, VK_NEXT, VK_HOME, VK_END, VK_F10};   // (the long lists: a page, the ends)
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    const bool mine = pid == GetCurrentProcessId();
    struct MicTick { bool focused; ~MicTick() { mic_test_tick(focused); } } mic_tick{mine};
    bool press[K_N] = {};
    for (int i = 0; i < K_N; i++) {
        const bool down = mine && (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        const bool queued = host && host->take_key_press && host->take_key_press(keys[i]);
        // Read both keys even if one is disabled, so old presses cannot toggle the menu after a change.
        // (F1 always opens the menu; Menu shortcut's F1 only leaves Insert to an overlay)
        const bool enabled = keys[i] != VK_INSERT || opts.menu_shortcut != 1;   // (Insert: all but F1 only)
        press[i] = enabled && (host && host->take_key_press ? mine && queued : down && !held[i]);
        held[i] = down;
        if (press[i]) pad_last = false;   // (the keys: their names in the prompts)
    }
    // (longshot) the gamepads, by position: Home, or Select and Start together, as Insert; the D-pad as the arrows; the
    // buttons the Switch's A and B are bound to as Enter and Esc, as in the game (vk_menu_keys.h; with the Nintendo layout A
    // is the right face button), not while a binding waits for its button
    static uint32_t pad_held = 0;
    const uint32_t raw = mine && host && host->pad_raw ? host->pad_raw() : 0, pad_press = raw & ~pad_held;
    const uint32_t chord = 1u << HOS_PAD_BACK | 1u << HOS_PAD_START;
    const bool chord_now = (raw & chord) == chord && (pad_held & chord) != chord;
    pad_held = raw;
    if (pad_press) pad_last = true;   // (a controller: its buttons in the prompts)
#ifndef VK_RSHIFT
#define VK_RSHIFT 0xA1
#endif
#ifndef VK_RCONTROL
#define VK_RCONTROL 0xA3
#endif
    const int tune_key = opts.compact_menu_key == 1 ? VK_RCONTROL : VK_RSHIFT;
    const int tune_parent_key = opts.compact_menu_key == 1 ? VK_CONTROL : VK_SHIFT;
    static bool tune_held = false, tune_cancelled = false;
    const bool tune_down = mine && (GetAsyncKeyState(tune_key) & 0x8000) != 0;
    if (host && host->take_key_press) {
        (void)host->take_key_press(tune_key);
        (void)host->take_key_press(tune_parent_key);
    }
    bool tune_release = false;
    if (tune_down && !tune_held) {
        tune_cancelled = false;
    } else if (!tune_down && tune_held) {
        if (!tune_cancelled && open_now.load() && page == 0 && listening == 0) tune_release = true;
    }
    tune_held = tune_down;
    if (!open_now.load() || page != 0 || listening != 0 || (tune_down && (GetAsyncKeyState(VK_TAB) & 0x8000)))
        tune_cancelled = true;
    if (tune_release) pad_last = false;
    {   // (RX_VK_UI_PROMPTS=pad or keys: the prompts for screenshots of test runs, which have no controller and no focus)
        static const char* force = getenv("RX_VK_UI_PROMPTS");
        if (force) pad_last = !strcmp(force, "pad");
    }
    auto pad = [&](int p) { return listening != 1 && (pad_press >> p & 1) != 0; };
    // (longshot) a DualShock 4's or DualSense's touchpad click is F1: it opens and closes the menu as the key does
    if (pad(HOS_PAD_TOUCHPAD)) press[6] = true;
    if (host && host->binding)   // (A and B as bound now: the Controls page or the Layout preset can change them)
        menu_keys::confirm_back(host->binding(HOS_BIND_PAD, HOS_SW_A, 0), host->binding(HOS_BIND_PAD, HOS_SW_B, 0), &pad_confirm,
                                &pad_back);
    pad_paste = menu_keys::paste(pad_confirm, pad_back);   // (longshot: a text field's paste, on a button neither is on)
    {   // (the controller's own button for the menu: the Controller menu button row)
        const int menu_pad = opts.pad_menu == 1 ? (int)HOS_PAD_GUIDE : opts.pad_menu == 2 ? (int)HOS_PAD_LPADDLE1
                           : opts.pad_menu == 3 ? (int)HOS_PAD_RPADDLE1 : -1;
        if ((menu_pad >= 0 && pad(menu_pad)) || chord_now) press[0] = true;
    }
    {   // holding an arrow or the D-pad repeats it: after 0.35 s, 12 a second, then 30 a second after 1.5 s held
        static int64_t held_since[5], next_repeat[5];
        const int pads[5] = {0, HOS_PAD_UP, HOS_PAD_DOWN, HOS_PAD_LEFT, HOS_PAD_RIGHT};
        const int64_t t = prof_now();
        for (int i = 1; i <= 4; i++) {
            const bool down = held[i] || (open_now.load() && listening != 1 && (raw >> pads[i] & 1));
            if (!down) { held_since[i] = 0; continue; }
            if (!held_since[i]) { held_since[i] = t, next_repeat[i] = t + 350000000LL; continue; }
            if (t >= next_repeat[i] && open_now.load() && listening == 0) {
                press[i] = true;
                next_repeat[i] = t + (t - held_since[i] > 1500000000LL ? 33000000LL : 83000000LL);
            }
        }
    }
    const bool can_clear_control = page == 1 && listening == 0 && control_confirmation.action == ControlAction::None;
    if (open_now.load() && listening != 1) {
        press[1] |= pad(HOS_PAD_UP), press[2] |= pad(HOS_PAD_DOWN), press[3] |= pad(HOS_PAD_LEFT);
        press[4] |= pad(HOS_PAD_RIGHT), press[5] |= pad(pad_back), press[7] |= pad(pad_confirm);
        press[8] |= can_clear_control && pad(pad_paste);
    }
    std::vector<uint32_t> scripted;   // (longshot: RX_VK_MENU_SCRIPT's typing, for the text field that has the keyboard)
    {   // RX_VK_MENU_SCRIPT (test runs, which never have the focus): timed menu keys, "<seconds>:<INS|UP|DOWN|LEFT|RIGHT|ESC|ENTER|TABL|TABR>,...";
        // and the gamepad's face buttons by position, PADS PADE PADW PADN (bottom, right, left, top), as a controller's press.
        // (longshot) TYPE:<text> types the text up to the next comma, as the keyboard would; while a text field has the
        // keyboard ENTER, ESC, LEFT, RIGHT, HOME, END, BACK (Backspace) and DEL are its keys, and the pad's confirm and back too
        static const char* next = getenv("RX_VK_MENU_SCRIPT");
        static const bool at_confirmation = next && !strncmp(next, "confirm:", 8);
        static int64_t start = at_confirmation ? 0 : prof_now();
        if (at_confirmation && !start && output_preview_countdown) {
            next += 8;
            start = prof_now();
        }
        double at;
        char key[8];
        int n = 0;
        if (next && start && sscanf(next, "%lf:%7[A-Z]%n", &at, key, &n) == 2 && (prof_now() - start) / 1e9 >= at) {
            static const char* names[] = {"INS", "UP", "DOWN", "LEFT", "RIGHT", "ESC", "", "ENTER", "", "", "TABL", "TABR"};
            static const struct { const char* name; int vk; } edit_keys[] = {
                {"ENTER", VK_RETURN}, {"ESC", VK_ESCAPE}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT}, {"HOME", VK_HOME},
                {"END", VK_END}, {"BACK", VK_BACK}, {"DEL", VK_DELETE}};
            if (!strcmp(key, "TYPE") && next[n] == ':') {
                const size_t len = strcspn(next + n + 1, ",");
                for (size_t i = 0; i < len; i++) scripted.push_back((unsigned char)next[n + 1 + i]);
                n += (int)len + 1;
            }
            for (const auto& k : edit_keys)
                if (editing && !strcmp(key, k.name)) scripted.push_back(HOS_TEXT_KEY | (uint32_t)k.vk);
            for (int i = 0; i < 12; i++) press[i] |= *names[i] && !strcmp(key, names[i]);
            static const char* pads[] = {"PADS", "PADE", "PADW", "PADN"};
            for (int p = 0; p < 4; p++)
                if (!strcmp(key, pads[p]) && open_now.load() && listening != 1) {
                    press[5] |= p == pad_back, press[7] |= p == pad_confirm;
                    press[8] |= can_clear_control && p == pad_paste;
                    if (editing && (p == pad_back || p == pad_confirm))
                        scripted.push_back(HOS_TEXT_KEY | (uint32_t)(p == pad_confirm ? VK_RETURN : VK_ESCAPE));
                    pad_last = true;
                }
            next = next[n] == ',' ? next + n + 1 : nullptr;
        }
    }
    // (longshot) the text field that has the keyboard (editing): what was typed since the last frame, in order, and a paste asked
    // with the controller's paste button; the host is told whether one has it, so that the window takes text only then
    uint32_t typed[64];
    int typed_n = 0;
    const bool pasting = editing && open_now.load() && pad(pad_paste);
    if (host && host->take_text) typed_n = host->take_text(!editing ? HOS_TEXT_OFF : pasting ? HOS_TEXT_PASTE : HOS_TEXT_ON, typed, 64);
    if (editing && press[0] && !(pad(HOS_PAD_GUIDE) || chord_now)) press[0] = false;   // (Insert is the field's: Shift+Insert pastes)
    if (editing && (press[0] || press[6])) edit_end(false);   // (F1 or Home: the menu closes, the text as it was)
    // (the keyboard's Enter or Esc that ended an edit, as the menu's own press too: its queued press or its key state can come a
    // frame after the text's, and would open the field again or close the menu. Not taken until both keys are up, 50 ms on)
    static int64_t edit_keys_until = 0;
    if (edit_keys_until && (prof_now() < edit_keys_until || ((GetAsyncKeyState(VK_RETURN) | GetAsyncKeyState(VK_ESCAPE)) & 0x8000)))
        press[5] = press[7] = false;
    else edit_keys_until = 0;
    bool tab_left = press[10] || pad(HOS_PAD_LSHOULDER) || pad(HOS_PAD_LTRIGGER);
    bool tab_right = press[11] || press[12] || pad(HOS_PAD_RSHOULDER) || pad(HOS_PAD_RTRIGGER);
    if (press[12] && (GetAsyncKeyState(VK_SHIFT) & 0x8000)) tab_left = true, tab_right = false;
    bool changed = menu_refresh;
    menu_refresh = false;
    // (longshot) the mouse, while the menu is open: the cursor in the window image's pixels (mx, my), tested against the
    // controls as last drawn (hits, vk_menu_hit.h), whether it moved since the last frame, and its clicks and wheel since then
    MkwVkPointer ptr{};
    int mx = -1, my = -1;
    bool mouse = false, moved = false;
    if (open_now.load() && host && host->pointer) {
        mouse = host->pointer(&ptr) != 0;
        int ix = 0, iy = 0;
        menu_hit::to_image(ptr.x, ptr.y, ptr.width, ptr.height, (int)dev.swap_extent.width, (int)dev.swap_extent.height,
                           &ix, &iy);
        if (mouse) mx = ix - (int)menu_ox, my = iy;   // (the menu's column's pixels: draw_menu)
        static int last_x = -1, last_y = -1;
        moved = mouse && mouse_read && (mx != last_x || my != last_y);   // (not the first read: a cursor resting over the
        last_x = mx, last_y = my, mouse_read = true;                     // menu as it opens leaves the selection alone)
        if (moved) pad_last = false;
        if (!mouse) ptr.pressed = 0, ptr.wheel = 0;
    }
    // (closing with a setting that takes a restart changed: the restart question first, Later already chosen on it)
    auto later = [&] {
        int now[mr::kMax];
        restart_values(now);
        restart_state.choose_later(now);
        open_now = false, page = 0, tune_mode = false, changed = true;
    };
    // (Menu shortcut "Insert, F1 or Esc", the default: Esc on a closed menu opens it, the keyboard's and RX_VK_MENU_SCRIPT's
    // ESC alone (the gamepad's back is Esc only while it is open, above). The press that opened it does not go back a page
    // below; with the compact menu key held it opens the compact bar, as Insert and F1 do (tune_mode). No
    // control can be bound to Esc, so nothing in play changes; the window gives the captured mouse back on the close)
    const bool esc_opens = opts.menu_shortcut == 2 && press[5] && !open_now.load();
    if (esc_opens) press[5] = false;
    const bool comparing = open_now.load() && reshade_menu::comparing();
    if (comparing && (press[0] || press[6] || press[17])) {
        press[5] = true;
        press[0] = press[6] = press[17] = false;
    }
    const bool home_debug = !comparing && press[15] && (!open_now.load() || (quick_debug && page == 0));
    if ((press[17] || home_debug) && !editing && !listening && !debug_warning && page != 5) {
        if (home_debug) press[15] = false;
        if (open_now.load() && quick_debug && page == 0) press[5] = true;
        else {
            quick_debug = true;
            tune_mode = false;
            open_now = true;
            page = 0;
            cur_tab = TAB_DEBUG;
            tab_selected[TAB_DEBUG] = 0;
            changed = true;
            press[5] = press[7] = false;
        }
    }
    if (quick_debug && page == 0) tab_left = tab_right = false;
    if ((press[0] || press[6]) && debug_warning) {
        debug_warning = false;
        changed = true;
    } else if (output_preview_target.w && !output_preview_countdown &&
               (press[0] || press[5] || press[6] || (open_now.load() && page == 0 && (ptr.pressed & HOS_MOUSE_RIGHT)))) {
        answer_native_preview(false);
        if (ptr.pressed & HOS_MOUSE_RIGHT) ptr.pressed &= ~HOS_MOUSE_RIGHT;
    } else if ((press[0] || press[6]) && open_now.load() && page == 5) {
        if (output_preview_countdown) answer_native_preview(false);
        else later();
    } else if ((press[0] || press[6]) && open_now.load() && restart_ask()) {
        page = 5, ask_sel = 0, listening = 0, changed = true;
    } else if (press[0] || press[6] || esc_opens) {
        quick_debug = false;
        open_now = !open_now.load(), changed = true;
        if (open_now.load()) tune_mode = tune_down;
        else tune_mode = false;
        page = 0, listening = 0;
        // (the layout row shows the controls as they are: host/nn_hid.c keeps them, the ini's pad_ lines, and a saved
        // layout is not applied over them at start)
        if (open_now.load()) opts.pad_layout = detect_pad_layout();
        // (whether the Upscaler row can offer DLSS, FSR and XeSS here, and Frame generation by DLSS FG: asked once)
        if (open_now.load()) dlss_probe(), dlss_fg_probe(), fsr4_probe(), xess_probe();
        if (open_now.load()) refresh_mics();   // (the microphones as they are now)
        if (open_now.load()) refresh_outputs();
        if (open_now.load()) refresh_monitors();
        if (open_now.load()) refresh_nfc();    // (and the NFC tags of the player's folder)
    }
    if (control_confirmation.action != ControlAction::None && (!open_now.load() || page != 1 || !mine)) {
        control_confirmation = {};
        changed = true;
    }
    if (comparing) {
        const auto click = mouse && (ptr.pressed & HOS_MOUSE_LEFT) ? hits.at((float)mx, (float)my) : menu_hit::Hit{};
        const int horizontal = int(press[4]) - int(press[3]);
        const int vertical = int(press[2] || tab_right) - int(press[1] || tab_left);
        const bool cancel = press[5] || (mouse && (ptr.pressed & HOS_MOUSE_RIGHT));
        changed |= reshade_menu::update_comparison(horizontal, vertical, press[7], cancel, click);
    } else if (open_now.load() && debug_warning) {
        if (press[3] || press[4]) debug_warning_choice = 1 - debug_warning_choice, changed = true;
        if (mouse) {
            const auto hit = hits.at((float)mx, (float)my);
            if (hit.part == menu_hit::kButton && hit.index >= 0 && hit.index < 2 && (moved || ptr.pressed)) {
                debug_warning_choice = hit.index;
                changed = true;
                if (ptr.pressed & HOS_MOUSE_LEFT) press[7] = true;
            }
            if (ptr.pressed & HOS_MOUSE_RIGHT) press[5] = true;
        }
        if (press[5] || press[7]) {
            if (!press[5] && debug_warning_choice != 0) {
                opts.debug_tools = 1;
                opts.transition_autosave = 0;
                sync_transition_autosave();
                save();
            }
            debug_warning = false;
            changed = true;
        }
    } else if (open_now.load() && editing) {   // (longshot) a text field has the keyboard: the keys type into it (text_edit), nothing else
        int done = kTextGoOn;
        auto feed = [&](uint32_t in) {
            if (done == kTextGoOn) done = text_edit(*editing, in), edit_why = nullptr, changed = true;
        };
        if (!(GetAsyncKeyState(VK_RETURN) & 0x8000)) edit_enter_held = false;
        for (int i = 0; i < typed_n; i++)
            if (!edit_enter_held || typed[i] != (HOS_TEXT_KEY | VK_RETURN)) feed(typed[i]);
        if (done != kTextGoOn) edit_keys_until = prof_now() + 50000000LL;   // (the keyboard's Enter or Esc: above)
        for (const uint32_t in : scripted) feed(in);
        // (the controller edits without a keyboard; the D-pad's repeats use the existing repeat clock)
        const int pads[] = {HOS_PAD_UP, HOS_PAD_DOWN, HOS_PAD_LEFT, HOS_PAD_RIGHT};
        const int keys[] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT};
        for (int i = 0; i < 4; i++)
            if (pad(pads[i]) || (press[i + 1] && (raw >> pads[i] & 1))) feed(HOS_TEXT_KEY | (uint32_t)keys[i]);
        if (pad(HOS_PAD_RSHOULDER)) feed(HOS_TEXT_KEY | VK_INSERT);
        if (pad(HOS_PAD_LSHOULDER)) feed(HOS_TEXT_KEY | (editing->caret < editing->len ? VK_DELETE : VK_BACK));
        if (pad(pad_confirm)) feed(HOS_TEXT_KEY | VK_RETURN);
        if (pad(pad_back)) feed(HOS_TEXT_KEY | VK_ESCAPE);
        if (mouse && done == kTextGoOn) {   // (a click elsewhere sets the text, a right click puts it back; no wheel scrolling)
            const menu_hit::Hit hit = hits.at((float)mx, (float)my);
            const bool on_field = hit.part == menu_hit::kRow && hit.index == tab_selected[cur_tab];
            if ((ptr.pressed & HOS_MOUSE_LEFT) && !on_field) done = kTextDone;
            if (ptr.pressed & HOS_MOUSE_RIGHT) done = kTextCancel;
        }
        if (done != kTextGoOn) edit_end(done == kTextDone), changed = true;
    } else if (open_now.load() && page == 5) {   // (longshot) the restart question: Later (the default) or Restart now
        if (mouse) {   // (a button under the cursor chosen, a click on it pressed, a right click is Later; no list: the wheel does nothing)
            const menu_hit::Hit hit = hits.at((float)mx, (float)my);
            if (hit.part == menu_hit::kButton && (moved || ptr.pressed) && hit.index != ask_sel) ask_sel = hit.index, changed = true;
            if ((ptr.pressed & HOS_MOUSE_LEFT) && hit.part == menu_hit::kButton) press[7] = true;
            if (ptr.pressed & HOS_MOUSE_RIGHT) press[5] = true;
        }
        if (press[1] || press[2] || press[3] || press[4] || tab_left || tab_right) ask_sel ^= 1, changed = true;
        if (output_preview_countdown && (press[5] || press[7])) {
            answer_native_preview(!press[5] && ask_sel != 0);
        } else if (press[5] || (press[7] && ask_sel == 0)) {
            later();
        } else if (press[7]) {   // (Restart now: everything saved, then the window closes as the player would close it)
            logf("menu: restart now, to apply %s", restart_list(", ").c_str());
            save();
            pipeline_cache_save(true);
            open_now = false, page = 0, tune_mode = false, changed = true;
            PostMessageA(app_window(), WM_APP + 9, 0, 0);
        }
    } else if (open_now.load() && page == 1) {   // (longshot) the Controls page
        using enum menu_hit::CtlColumn;
        const int previous_row = csel, previous_column = ccol;
        if (!can_bind()) {
            static int warned;
            if (!warned) warned = 1, logf("menu: the Controls page asked for the host's bindings and got none");
            page = 0;
        }
        else if (control_confirmation.action != ControlAction::None) {
            if (press[1] || press[2] || press[3] || press[4])
                control_confirmation.choice ^= 1, changed = true;
            if (mouse) {
                const auto hit = hits.at((float)mx, (float)my);
                if (hit.part == menu_hit::kButton) {
                    if (moved || ptr.pressed) control_confirmation.choice = hit.index, changed = true;
                    if (ptr.pressed & HOS_MOUSE_LEFT) press[7] = true;
                }
                if (ptr.pressed & HOS_MOUSE_RIGHT) press[5] = true;
            }
            if (press[5] || press[7]) {
                const auto ask = control_confirmation;
                control_confirmation = {};
                changed = true;
                if (!press[5] && ask.choice == 1) {
                    if (ask.action == ControlAction::Rebind)
                        rebind_control(ask.row, ask.set, ask.slot, ask.code, ask.conflicts);
                    else if (ask.action == ControlAction::ResetAll) {
                        host->bind(HOS_BIND_PAD, -1, 0, 0);
                        opts.pad_layout = detect_pad_layout();
                        opts.hold_toggle = 0;
                        toggle_set(HOS_KEY_SPRINT, toggle_default(HOS_KEY_SPRINT));
                        if (opts.sprint) apply_sprint_layout(true);
                        save();
                    } else if (control_column_enabled(ask.row, kCtlReset)) {
                        const int button = controls[ask.row].button;
                        if (host->reset_binding(button)) {
                            blocked_reset_row = -1;
                            if (has_toggle(button)) toggle_set(button, toggle_default(button));
                            opts.pad_layout = detect_pad_layout();
                            save();
                        } else blocked_reset_row = ask.row;
                    }
                }
            }
        }
        else if (listening) {
            if (press[5] || prof_now() > listen_end) listening = 0, changed = true;   // (Esc, or nothing for a while)
            else if (listening == 1) {
                if (!listen_armed) listen_armed = (raw & ~(1u << HOS_PAD_GUIDE)) == 0;
                else if (const uint32_t p = pad_press & ~(1u << HOS_PAD_GUIDE) & ~(1u << HOS_PAD_TOUCHPAD)) {
                    int bit = 0;
                    while (!(p >> bit & 1)) bit++;
                    rebind_control(csel - top_rows(), HOS_BIND_PAD, ccol, bit);
                    listening = 0, changed = true;
                }
            } else {
                int down = 0;
                for (int k : bindable_keys())
                    if (mine && (GetAsyncKeyState(k) & 0x8000)) {
                        down = k;
                        break;
                    }
                if (!listen_armed) listen_armed = !down;
                else if (down) {
                    rebind_control(csel - top_rows(), HOS_BIND_KEY, 0, down);
                    listening = 0, changed = true;
                }
            }
        } else {
            if (mouse) {   // (the mouse: the row under it selected, a click on a binding listens for its new input, on
                           // Reset resets; the wheel scrolls the page, a right click goes back)
                const menu_hit::Hit hit = hits.at((float)mx, (float)my);
                const bool option = hit.part == menu_hit::kCtlOption;
                const bool cell = hit.part == menu_hit::kCtlCell && control_column_enabled(hit.index, hit.side);
                const bool on = option || hit.part == menu_hit::kCtlRow || cell;
                const int at = option ? hit.index : top_rows() + hit.index;   // (the page's row: its own rows first)
                if (on && (moved || ptr.pressed)) {
                    if (at != csel) csel = at, changed = true;
                    if (cell && ccol != hit.side) ccol = hit.side, changed = true;
                }
                if (ptr.pressed & HOS_MOUSE_LEFT) {
                    if (option) press[hit.side < 0 ? 3 : 4] = true;   // (a value's halves: Left and Right; the name: Right)
                    else if (cell || (hit.part == menu_hit::kCtlRow && hit.index == kControls))
                        press[7] = true;   // (as Enter)
                }
                if (ptr.pressed & HOS_MOUSE_RIGHT) press[5] = true;
                if (ptr.wheel) wheel_rows[kControlsSlot] += ptr.wheel, changed = true;   // (scrolls; the selection stays)
            }
            if (press[1] != press[2]) {
                const int next = next_control_selection(csel, ccol, press[1] ? -1 : 1);
                changed |= next != csel;
                csel = next;
            }
            const int i = csel - top_rows();   // (i: the control's, or reset's)
            if (press[5]) page = 0, changed = true;
            else if (i < 0 && (press[3] || press[4] || press[7])) {   // (the page's own rows: Enter as Right)
                Row& r = rows[tab_row_to_global(TAB_PAGE, csel)];   // (Custom is only what the bindings are: set_row skips it)
                set_row(r, *r.value + (press[3] ? -r.step : r.step), press[3] ? -1 : 1);
                changed = true;
            } else if (i >= 0) {
                if (i < kControls && !control_column_enabled(i, ccol)) ccol = kCtlKeyboard, changed = true;
                if ((press[3] || press[4]) && i < kControls) {
                    do ccol = (ccol + kCtlColumns + (press[3] ? -1 : 1)) % kCtlColumns;
                    while (!control_column_enabled(i, ccol));
                    changed = true;
                }
                if (csel != previous_row || ccol != previous_column) blocked_reset_row = -1;
                const int col = ccol;
                if (press[7]) {
                    if (i == kControls) control_confirmation = {ControlAction::ResetAll};
                    else if (col == kCtlToggle) {
                        toggle_set(controls[i].button, !toggle_on(controls[i].button));
                        save();
                    }
                    else if (col == kCtlReset) control_confirmation = {ControlAction::ResetRow, i};
                    else listening = col == kCtlKeyboard ? 2 : 1, listen_armed = false, listen_end = prof_now() + 6000000000LL;
                    changed = true;
                }
                if (!press[7] && (press[8] || press[9]) && i < kControls && col != kCtlReset) {
                    if (col == kCtlToggle) toggle_set(controls[i].button, false);
                    else if (col != kCtlKeyboard) {
                        host->bind(HOS_BIND_PAD, controls[i].button, col, -1);
                        opts.pad_layout = detect_pad_layout();
                    }
                    else for (int k = 0; k < 2; k++) host->bind(HOS_BIND_KEY, controls[i].button, k, 0);
                    changed = true;
                    save();
                }
            }
        }
    } else if (open_now.load() && page == 4) {   // the inventory
        const int chosen = item_choice;   // (the page follows the choice when it moves; the wheel's page stays otherwise)
        if (mouse) inventory_mouse(ptr, mx, my, press, changed);
        if (press[5] && inventory_amount) inventory_amount = false, changed = true;   // (back: out of the amount first)
        else if (press[5]) page = 0, changed = true;   // (to the Debug tab, Inventory still selected)
        else if (tab_left || tab_right) {   // (another section, from editing an amount too: that ends)
            const int filters = kItemSections + 1;
            choose_inventory_filter((inventory_filter + filters + (tab_left ? -1 : 1)) % filters);
            changed = true;
        } else if (inventory_amount) {
            if (press[7]) inventory_amount = false, changed = true;
            else if (press[3] || press[4]) cheat_action.store(OOT_CHEAT_ITEM_ACTION + item_choice * 256 + 64 + (press[4] ? 1 : -1));
        } else {
            // (the cards as drawn, inventory_columns to a row: the arrows move, Page Up and Down a page, Home and End the ends)
            const int begin = inventory_begin(), end = inventory_end();
            const int step = (press[4] - press[3]) + (press[2] - press[1]) * inventory_columns + (press[14] - press[13]) * inventory_capacity;
            if (step || press[15] || press[16]) {
                item_choice = press[15] ? begin : press[16] ? end - 1 : std::clamp(item_choice + step, begin, end - 1);
                inventory_reveal();
                changed = true;
            } else if (press[7] && item_counts[item_choice].load(std::memory_order_relaxed) >= 0) {
                if (item_kinds[item_choice]) inventory_amount = true, changed = true;   // (an amount: Left and Right change it)
                else cheat_action.store(OOT_CHEAT_ITEM_ACTION + item_choice * 256 + 200);   // (unlock or relock)
            }
            if (item_choice != chosen) inventory_reveal();   // (a click on a card: its page)
        }
    } else if (open_now.load() && page == 3) {   // the teleport list
        if (mouse) teleport_mouse(ptr, mx, my, press, changed);
        if (press[5]) page = 0, changed = true;   // (to the Debug tab, Teleport still selected)
        else if (press[7]) {
            cheat_action.store(1000 + teleport_choice);
            open_now = false, page = 0;   // (back to the game: the map change starts with an event)
            changed = true;
        } else if (tab_left || tab_right) {   // (the first map of the previous or next section)
            const int section = (teleport_section_of(teleport_choice) + kTeleportSections + (tab_left ? -1 : 1)) % kTeleportSections;
            teleport_choice = teleport_sections[section].first, teleport_follow = true;
            changed = true;
        } else {
            // (the cards as drawn, each section's in rows under its heading: Up and Down a row, the column kept; Left and Right the
            // previous and next card; Page Up and Down the rows that fit; Home and End the ends)
            const int rows = (press[2] - press[1]) + (press[14] - press[13]) * teleport_page_rows;
            const int was = teleport_choice;
            if (press[15]) teleport_choice = 0;
            else if (press[16]) teleport_choice = kTeleports - 1;
            else teleport_choice = std::clamp(teleport_step_rows(teleport_choice, rows) + (press[4] - press[3]), 0, kTeleports - 1);
            if (teleport_choice != was) teleport_follow = true, changed = true;
        }
    } else if (open_now.load()) {
        if (mouse) {   // (the mouse: see the top of this file)
            const menu_hit::Hit hit = hits.at((float)mx, (float)my);
            const int ht = hit.part == menu_hit::kTab ? hit.index : hit.part == menu_hit::kTabPrev ? TAB_COUNT
                         : hit.part == menu_hit::kTabNext ? TAB_COUNT + 1 : -1;
            if (ht != hover_tab) hover_tab = ht, changed = true;
            int& sel = cur_tab == TAB_MODS ? mods_sel : tab_selected[cur_tab];   // (the Mods tab: its line, draw_mods)
            const bool on_row = hit.part == menu_hit::kRow || hit.part == menu_hit::kSlider;
            if (drag_row >= 0) {   // (a slider held: it follows the cursor's x until let go)
                Row& d = rows[tab_row_to_global(cur_tab, drag_row)];
                if (!(ptr.held & HOS_MOUSE_LEFT) || !d.value) {
                    drag_row = -1;
                    save();
                } else {
                    const int permille = hits.drag((float)mx, drag_row);
                    const int v = permille < 0 ? *d.value : menu_hit::slider_value(permille, d.lo, d.hi, d.step);
                    if (v != *d.value) set_row(d, v, v > *d.value ? 1 : -1, false), changed = true;
                }
            } else if (on_row && (moved || ptr.pressed) && hit.index != sel) {
                // (the row under the cursor is in view already: no scrolling to it. Brought into view as the keys' choice
                // is, with its row of context, the top row slid down under a cursor that stayed put, and the next click took
                // the row above: picking the Upscaler changed the Output resolution, and the menu asked to restart)
                sel = hit.index, scrolled_to[cur_tab] = sel, changed = true;
            }
            if ((ptr.pressed & HOS_MOUSE_LEFT) && drag_row < 0) {
                if (hit.part == menu_hit::kTabPrev) tab_left = true;
                else if (hit.part == menu_hit::kTabNext) tab_right = true;
                else if (hit.part == menu_hit::kTab && hit.index != cur_tab) cur_tab = hit.index, changed = true;
                else if (hit.part == menu_hit::kRow && cur_tab == TAB_MODS) {
                    if (hit.side) press[hit.side < 0 ? 3 : 4] = true;   // (a switch's halves: Off and On)
                } else if (hit.part == menu_hit::kSlider) {
                    Row& d = rows[tab_row_to_global(cur_tab, hit.index)];
                    drag_row = hit.index;
                    const int v = menu_hit::slider_value(hit.permille, d.lo, d.hi, d.step);
                    if (v != *d.value) set_row(d, v, v > *d.value ? 1 : -1, false);
                    changed = true;
                } else if (hit.part == menu_hit::kRow) {
                    Row& d = rows[tab_row_to_global(cur_tab, hit.index)];
                    if (!d.value && d.cheat >= 0 && (cheat_chooses(d.cheat) || age_shown(d.cheat) || d.cheat <= 1 || d.cheat == 7) &&
                        hit.side != 0)
                        press[hit.side < 0 ? 3 : 4] = true;   // (a tool's choice: its halves as Left and Right)
                    else if (!d.value) press[7] = true;   // (a page, a tool: as Enter)
                    else switch (menu_hit::row_click(true, d.hi - d.lo == d.step, hit.side, *d.value == d.hi)) {
                    case menu_hit::kClickConfirm: press[7] = true; break;
                    case menu_hit::kClickRight: press[4] = true; break;     // (the right half as Right; a toggle's other half flips it)
                    case menu_hit::kClickLeft: press[3] = true; break;      // (the left half as Left)
                    case menu_hit::kClickSelect: break;   // (the name, or a toggle's half already chosen: selected above, the value as it is)
                    }
                }
            }
            if (ptr.pressed & HOS_MOUSE_RIGHT) press[5] = true;   // (as Esc)
            // (the wheel scrolls the rows, wherever the cursor is; it never changes a value, the selection or the tab: the
            // values change by a click on their arrows or halves, a slider's drag, Left and Right, A and Enter)
            if (ptr.wheel && drag_row < 0) wheel_rows[cur_tab] += ptr.wheel, changed = true;
        }
        if (tune_down && (press[12] || tab_left || tab_right || press[1] || press[2] || press[3] || press[4] || press[5] || press[7] || ptr.pressed))
            tune_cancelled = true;
        const bool tune_toggle = tune_release || pad(HOS_PAD_WEST);
        if (tune_toggle && listening == 0) tune_mode = !tune_mode, changed = true;
        if (press[5] && cur_tab == TAB_MOTION) cur_tab = TAB_CONTROLS, press[5] = false, changed = true;   // (back)
        if (press[5] && cur_tab == TAB_FPS_DEV) cur_tab = TAB_GAMEPLAY, press[5] = false, changed = true;   // (back)
        if (press[5] && cur_tab == TAB_RECORDINGS) cur_tab = TAB_DEBUG, press[5] = false, changed = true;   // (back)
        if (press[5] && cur_tab == TAB_SPEAKER) cur_tab = TAB_AUDIO, press[5] = false, changed = true;   // (back)
        if (press[5] && cur_tab == TAB_SIM_TURF) cur_tab = TAB_GRAPHICS, press[5] = false, changed = true;   // (back)
        if (press[5] && cur_tab == TAB_MODS && mods_grab >= 0) mods_grab = -1, press[5] = false, changed = true;   // (puts the mod down)
        if (press[5] && pending) pending = nullptr, changed = true;   // (a Restart or Quit waiting for its second press: cancelled)
        else if (press[5] && cur_tab == TAB_MOTION) cur_tab = TAB_CONTROLS, press[5] = false, changed = true;   // (back)
        else if (press[5] && cur_tab == TAB_FPS_DEV) cur_tab = TAB_GAMEPLAY, press[5] = false, changed = true;   // (back)
        else if (press[5] && cur_tab == TAB_RECORDINGS) cur_tab = TAB_DEBUG, press[5] = false, changed = true;   // (back)
        else if (press[5] && cur_tab == TAB_SPEAKER) cur_tab = TAB_AUDIO, press[5] = false, changed = true;   // (back)
        else if (press[5] && cur_tab == TAB_SIM_TURF) cur_tab = TAB_GRAPHICS, press[5] = false, changed = true;   // (back)
        else if (press[5] && restart_ask()) page = 5, ask_sel = 0, changed = true;   // (the restart question first)
        else if (press[5]) open_now = false, tune_mode = false, quick_debug = false;
        // (the tabs: LB and RB, Q and E, or Tab; from the Motion page, as from Controls)
        if ((tab_left || tab_right) && cur_tab == TAB_MOTION) cur_tab = TAB_CONTROLS;
        if ((tab_left || tab_right) && cur_tab == TAB_FPS_DEV) cur_tab = TAB_GAMEPLAY;
        if ((tab_left || tab_right) && cur_tab == TAB_RECORDINGS) cur_tab = TAB_DEBUG;
        if ((tab_left || tab_right) && cur_tab == TAB_SPEAKER) cur_tab = TAB_AUDIO;
        if ((tab_left || tab_right) && cur_tab == TAB_SIM_TURF) cur_tab = TAB_GRAPHICS;
        if (tab_left || tab_right) {   // (the tabs shown: the Mods tab only with mods)
            do cur_tab = (cur_tab + (tab_left ? TAB_COUNT - 1 : 1)) % TAB_COUNT;
            while (!tab_shown(cur_tab));
            changed = true;
        }
        if (!tab_shown(cur_tab)) cur_tab = TAB_GRAPHICS;   // (the Mods tab, its mods gone: never, as the list is the start's)
        if (cur_tab != TAB_MODS) mods_grab = -1;
        if (cur_tab == TAB_MODS) {   // (the Mods tab: its own lines, draw_mods)
            if (pending) pending = nullptr, changed = true;   // (a Restart or Quit waiting for its second press: not on this tab)
            mods_tick(press, changed);
        } else if (tab_row_count(cur_tab) == 0) {
            // (#1633: a tab with no row shown, every row's shown() false: nothing to select or press, and no count to step
            // by; tests/test_menu_empty_tab.py)
        } else {
            const int tab_count = tab_row_count(cur_tab);
            int& selected = tab_selected[cur_tab];
            if (selected >= tab_count) selected = tab_count - 1;   // (a row gone since: the debug tools turned off, HDR turned off: row_shown)
            auto heading_at = [&](int k) { return rows[tab_row_to_global(cur_tab, k)].heading; };   // (stepped over)
            if (heading_at(selected)) selected = (selected + 1) % tab_count;
            if (cur_tab == TAB_DEBUG && (press[1] || press[2])) {   // (the Debug tab: in its layout's order, debug_rows)
                selected = debug_step(selected, press[2] ? 1 : -1);
                changed = true;
            } else {
                if (press[1]) {
                    do selected = (selected + tab_count - 1) % tab_count;
                    while (heading_at(selected));
                    changed = true;
                }
                if (press[2]) {
                    do selected = (selected + 1) % tab_count;
                    while (heading_at(selected));
                    changed = true;
                }
            }
            Row& r = rows[tab_row_to_global(cur_tab, selected)];
            const bool confirm_selected = r.setting && r.setting->confirm;   // (#1633: Restart, Quit: gpu/vk/menu/session.cpp)
            if (pending && (pending != &r || !mine)) pending = nullptr, changed = true;
            if (press[7] && confirm_selected) {
                if (mine && !press[5]) {
                    if (pending != &r) {
                        pending = &r, changed = true;
                    } else {   // (as Restart now: saved first, as the window's own close does not force the pipeline cache)
                        save();
                        pipeline_cache_save(true);
                        pending = nullptr;
                        if (r.setting->on_activate) r.setting->on_activate();   // (Restart's, Quit's: menu_restart_game, menu_quit_game)
                        changed = true;
                    }
                }
            } else if (r.cheat >= 0) cheat_press(r.cheat, press, changed);   // (a debug tool: on the Debug tab itself)
            else if (press[7] && is_motion_row(r)) cur_tab = TAB_MOTION, tab_selected[TAB_MOTION] = 0, press[7] = false, changed = true;
            else if (press[7] && is_fps_dev_row(r)) cur_tab = TAB_FPS_DEV, tab_selected[TAB_FPS_DEV] = 0, press[7] = false, changed = true;
            else if (press[7] && is_recordings_row(r))
                cur_tab = TAB_RECORDINGS, tab_selected[TAB_RECORDINGS] = 0, press[7] = false, changed = true;
            else if (press[7] && is_speaker_row(r)) cur_tab = TAB_SPEAKER, tab_selected[TAB_SPEAKER] = 0, press[7] = false, changed = true;
            else if (press[7] && is_sim_turf_row(r)) cur_tab = TAB_SIM_TURF, tab_selected[TAB_SIM_TURF] = 0, press[7] = false, changed = true;
            else if (press[7] && r.setting && r.setting->on_activate &&   // (#1633: a module's button, unless locked)
                     !(r.setting->locked && r.setting->locked()))
                r.setting->on_activate(), press[7] = false, changed = true;
            else if (press[7] && r.setting && (r.setting->kind == menu::Kind::Action || r.setting->kind == menu::Kind::Page))
                press[7] = false;   // (#1633: a module's button or page that did nothing (locked): not the bindings page below)
            else if (const FieldRow* f = press[7] ? field_row(r) : nullptr) edit_start(*f->field, f->apply), changed = true;   // (longshot)
            else if (press[7] && !r.value && can_bind()) page = 1, csel = 0, ccol = menu_hit::kCtlPrimary, changed = true;
            if (nfc_scan_moved()) changed = true;   // (a scan ends by itself: its line drawn again: gpu/vk/menu/nfc.cpp)
            if ((press[3] || press[4] || press[7]) && r.value) {   // (Enter, or A, as Right)
                if (press[7]) press[4] = true;
                set_row(r, *r.value + (press[4] ? r.step : -r.step), press[4] ? 1 : -1);
                changed = true;
            }
        }
        const int64_t now = prof_now();
        if (changed) {   // (a new row or value: its name from the start, after a rest)
            marquee_pos = 0, marquee_next = now + kMarqueeRest;
        } else if (marquee_max > 0 && now >= marquee_next) {
            if (marquee_pos >= marquee_max) marquee_pos = 0, marquee_next = now + kMarqueeRest;   // (back to the start)
            else if (++marquee_pos >= marquee_max) marquee_next = now + kMarqueeRest;            // (rest at the end)
            else marquee_next = now + kMarqueeStep;
            changed = true;
        }
    }
    {   // (longshot) the window lets the mouse go while the menu is open, and takes it back after (host/display.c)
        static bool told = false;
        const bool now_open = open_now.load();
        if (!now_open) {
            tune_mode = false;
            if (drag_row >= 0) save();   // (a slider let go off the window: kept now)
            sim_turf_settings_flush();
            hover_tab = -1, drag_row = -1, mouse_read = false;
            pending = nullptr;
            edit_end(false);   // (longshot: a text field being edited, its text as it was)
        }
        if (now_open != told && app_window()) {
            told = now_open;
            PostMessageA(app_window(), WM_APP + 8, (WPARAM)now_open, (LPARAM)(now_open && esc_opens));
        }
    }
    fps_level_now.store(fps_stats::level_of(opts.show_fps), std::memory_order_relaxed);   // (what vk_fps.cpp measures)
    if (opts.show_fps) {
        static int64_t next = 0, last_game = 0, last_shown = 0;
        const int64_t now = prof_now();
        double shown = 0, game = 0;
        if (fps_take(now, shown_frames.load(), game_frames.load(), &next, &last_shown, &last_game, &shown, &game)) {
            snprintf(fps_text, sizeof fps_text, "%.0f", shown);
            fps_rate_now = opts.framegen >= 1 ? game : shown;   // (the game's own against its cap, with frame generation too)
            // (the game's own rate, "Base", and who makes the frames: DLSS FG, FSR 3.1 FG or Built-in FG, or DLSS FG paused)
            const char* who = framegen_maker_name();
            if (opts.framegen >= 1 && who) snprintf(fps_sub, sizeof fps_sub, "Base %.0f, %s", game, who);
            else if (opts.framegen >= 1) snprintf(fps_sub, sizeof fps_sub, "Base %.0f", game);
            else fps_sub[0] = 0;
            // (in the menu's language: the counter draws with Tab, past ui::'s translation, so it is put in here)
            if (const char* t = i18n::tr(fps_sub); t != fps_sub) snprintf(fps_sub, sizeof fps_sub, "%s", t);
        }
        static int64_t texts_next = 0;   // (the lines past the number: four times a second, each over the last half second)
        if (now >= texts_next) texts_next = now + fps_stats::kSecond / 4, fps_texts(now);
    }
}

/// (longshot) Once a present: the Vibration strength row changed with the menu open, and the new strength is felt at once,
/// a short buzz, which also shows that the controller vibrates. Whatever changed it: the keys, the mouse
void menu_rumble_tick() {
    static int was = -1;
    if (was >= 0 && opts.rumble != was && menu_is_open() && host && host->version >= 32 && host->rumble_test) host->rumble_test();
    was = opts.rumble;
}
void menu_record(VkCommandBuffer cmd, uint32_t index, uint32_t w, uint32_t h, int colour_vision) {
    const bool start = startup_on.load(std::memory_order_acquire);
    const bool fps = opts.show_fps && !start;   // (no game frames yet: not "0 FPS" over the start-up screen)
    const bool open = open_now.load(std::memory_order_relaxed);
    if (!start && !fps && !open) {   // (nothing shows: no ImGui frame, no render pass)
        // (but a new window size or Menu size since the last bake: one frame with nothing drawn that bakes the menu's glyphs
        // and measures its page for the new size, so the next open has no hitch whatever was resized while nothing showed)
        if (prebake_needed(w, h) && ui::begin(w, h)) {
            ui::Ctx c = ui_ctx(w, h);
            prebake(c);
            ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (Dear ImGui records into cmd directly, its backend through its own pointers: what this thread recorded through vk_work.h reaches cmd first)
            ui::record(cmd, index, w, h, colour_vision);
            return;
        }
        ui::idle();
        return;
    }
    if (!ui::begin(w, h)) return;
    {   // (the easing's step: the time since the last drawing, a quarter of the way in 1/60 s)
        static int64_t last = 0;
        const int64_t now = prof_now();
        ease_step = last ? std::min(1.0f, (float)((now - last) / 1e9) * 15.0f) : 1.0f;
        last = now;
    }
    ui::Ctx c = ui_ctx(w, h);
    prebake(c);   // (the menu's glyphs at this size, before it opens: no hitch then)
    if (start) draw_startup(c);   // (under the menu)
    if (open) draw_menu(c);
    if (fps) draw_fps(c, open);
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (Dear ImGui records into cmd directly, its backend through its own pointers: what this thread recorded through vk_work.h reaches cmd first)
    ui::record(cmd, index, w, h, colour_vision);
}

void menu_record_image(VkCommandBuffer cmd, VkImage image, VkImageView view, uint32_t w, uint32_t h) {
    const bool open = open_now.load(std::memory_order_relaxed);
    const bool fps = opts.show_fps && !startup_on.load(std::memory_order_acquire);
    if (!open && !fps) return;
    if (!ui::begin(w, h)) return;
    ui::Ctx c = ui_ctx(w, h);
    if (open) draw_menu(c);
    if (fps) draw_fps(c, open);
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (Dear ImGui records into cmd directly, its backend through its own pointers: what this thread recorded through vk_work.h reaches cmd first)
    ui::record_to(cmd, image, view, w, h);
}

} // namespace mkwvk
