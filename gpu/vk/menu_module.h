// What a module of the Insert menu includes (#1633): the model (menu_model.h) and the options its rows set (vk_menu.h's
// Options, opts). A module is one feature's rows in a file of gpu/vk/menu/ (one day, its feature's own folder): it describes
// them with MENU_MODULE and says where they go among the core's sections and rows (vk_menu.cpp: "graphics", "audio",
// "gameplay", "controls", "nfc", "mods", "debug"; the core's sections "resolution", "screen", "developer", and the modules' "framegen",
// "hdr", "detail", "gpu" (menu/framegen.cpp, hdr.cpp, detail.cpp, gpu.cpp); a row by its ini key). docs/development.md, "A row
// of the Insert menu", says how.
#pragma once

#include "menu_model.h"
#include "vk_menu.h"

#include <algorithm>   // (what a module's texts and hints commonly use)
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>

namespace mkwvk {
// (texts a module's rows share with the rest of the menu)
const char* tx_hdr(int v);   // (menu/hdr.cpp: the HDR row's choice, also on the pipeline share's line)
const char* tx_vsync(int v);   // (menu/display.cpp: V-Sync's choice, also Threaded rendering's)

/// (an input recording now, the host's input_status: menu/input_recording.cpp's rows; refused: why the last start was not)
struct MenuInputStatus {
    int mode = 0, started = 0, held = 0, diverged = 0;   // (mode 0 nothing, 1 recording, 2 playing)
    unsigned frame = 0, end = 0, syncs = 0;
    float largest = 0;
    char path[512] = {};
    const char* refused = nullptr;
};
// ---- what modules ask the renderer: vk_menu.cpp defines these; tests/menu_model_fixture.py stands in for them ----
const char* hdr_sdr_hint(int v, int variant);   // (the HDR row's hint: why HDR on stays SDR, which the device knows)
const char* display_vsync_env();   // (the V-Sync switch set for this run, RX_VK_FIFO or RX_VK_IMMEDIATE, or nullptr)
bool display_filter_needed();   // (the window is not the Output resolution's size: the Window scaling filter has work)
const char* display_upscale_text(int v);   // (the Window scaling filter's name for a value)
const char* menu_dlss_why_text(int why);   // (why NVIDIA DLSS does not run here, for DLSS's DlssWhy: the Upscaler's and DLSS FG's lines)
bool crt_native_pq();   // (the game presents Native HDR (PQ): the CRT filters are bypassed)
const char* motion_gyro_text(int v);   // (the Motion controls row: On or Off, and whether the controller has a gyroscope)
void menu_row_top(const int* value, int hi);   // (the row of this value ends at hi now: a list counted again)
int upscaler();   // (the upscaler that runs now, upscaler_rows.h's: what the Upscaler row shows)
unsigned upscalers_available();   // (one bit per upscaler that runs here: upscaler_rows.h bit)
void env_release(const int* value);   // (a row the environment held for the run, set by the menu: saved from now on)
void weak_laptop_apply();   // (Set up for a weak laptop: Minimum's rows and lighter still, the restart asked, saved)
extern bool laptop_applied;   // (it was, this run)
void bug_report_save();   // (the Bug report: gathered from the whole menu, written on a thread: vk_bug_report.h)
void menu_restart_game();   // (the game started again: its window closes, host/hos_relaunch.c starts it; the menu saved first)
void menu_quit_game();      // (the game's window closed, as the player would; the menu saved first)
const char* menu_version();   // (this build's version: nx2/build.py gives it to gpu/vk/vk_version.cpp alone)
void menu_save_options();
void menu_redraw();   // (the menu drawn again at its next frame: a list it shows changed)
bool menu_on_tab(const char* tab);   // (the menu is open on this tab (its id), not on a page of it)
void menu_rumble_test();   // (Test vibration: a short buzz on the controller used last, the host's rumble_test)
MenuInputStatus menu_input_status();
void menu_input_record(int start);   // (Record input: start 1, stop 0; the host's input_record)
void menu_input_play(int start);   // (the last recording played with Drift correction's choice, start 0 stops: input_play)
void menu_input_play_file(int index);   // (the folder's index-th newest recording played, likewise)
const char* menu_input_recording(int index);   // (the folder's index-th newest recording's path, nullptr past the last)
namespace i18n {   // (vk_menu_i18n.h: the languages the language rows choose between)
int lang_count();
const char* lang_name(int index);
int game_lang_count();
const char* game_lang_name(int index);
} // namespace i18n
// ---- end of what modules ask the renderer ----

namespace menu_text {
/// A percentage, "%d%%": the translations match a value's text against its printf format
inline std::string percent(int v) {
    char b[16];
    snprintf(b, sizeof b, "%d%%", v);
    return b;
}
/// The same as text a function returns (a module's moved tx_ functions: vk_menu.cpp's tx_percent and tx_onoff)
inline const char* percent_c(int v) {
    static char b[16];
    snprintf(b, sizeof b, "%d%%", v);
    return b;
}
inline const char* onoff(int v) { return v ? "On" : "Off"; }
/// A row's hints from a table, the value lo first (nullptr: none for that value): hint(v, -1) the one now, hint(v, 0) the
/// one v can have, as vk_menu.cpp's hint_in
inline const char* hint_in(const char* const* t, int n, int lo, int v, int variant) {
    return variant <= 0 && v - lo >= 0 && v - lo < n ? t[v - lo] : nullptr;
}
} // namespace menu_text
} // namespace mkwvk

/// A module's hook that asks the renderer (its own code under #ifndef MENU_LAYOUT_ONLY, with the renderer's headers): the hook
/// in the game, none where the menu is built for its layout alone (tests/menu_model_fixture.py compiles every module with
/// MENU_LAYOUT_ONLY, without Vulkan: the rows, keys, texts, sections and order checked, the hooks not)
#ifdef MENU_LAYOUT_ONLY
#define MENU_HOOK(...) nullptr
#else
#define MENU_HOOK(...) __VA_ARGS__
#endif

/// A module's row's hints, one a value from lo (as vk_menu.cpp's MENU_HINTS): MODULE_HINTS(hn_x, 0, "...", nullptr, "...")
#define MODULE_HINTS(fn, lo, ...)                                                                                            \
    const char* fn(int v, int variant) {                                                                                     \
        static const char* const t[] = {__VA_ARGS__};                                                                        \
        return ::mkwvk::menu_text::hint_in(t, (int)(sizeof t / sizeof t[0]), lo, v, variant);                                \
    }
