"""CRT rows use the menu's real defaults, labels, keys, read/write and preset paths.

Compile production Options, Row, the two CRT rows (gpu/vk/menu/crt.cpp's settings, made into the menu's rows as
vk_menu.cpp's row_table makes a module's), their labels/hints, ini_key, save, menu_init's file-reading block, set_row,
and the Quality preset's apply_preset and apply_auto (gpu/vk/menu/preset.cpp). Only unrelated device, output-mode and
host hooks are stand-ins; no CRT option behavior is replaced. Where the rows are is read from the Graphics tab as the
menu model lays it out (tests/menu_model_fixture.py built_menu). No game or GPU is opened. --source ROOT runs the same
fixture against another checkout.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))   # (python -I leaves the check's own folder off the path)
from menu_model_fixture import built_menu, menu_model_prelude  # noqa: E402
from srccut import between  # noqa: E402


def block(text, start):
    opening = text.index("{", start)
    depth = 0
    for at in range(opening, len(text)):
        depth += text[at] == "{"
        depth -= text[at] == "}"
        if depth == 0:
            return text[start:at + 1]
    raise SystemExit("FAIL: unterminated source block at %d" % start)


def definition(text, name):
    match = re.search(re.escape(name) + r"\([^;{]*\)\s*\{", text)
    if not match:
        raise SystemExit("FAIL: missing production definition: " + name)
    return block(text, match.start())


def declaration(text, pattern, what):
    match = re.search(pattern, text, re.S)
    if not match:
        raise SystemExit("FAIL: missing production " + what)
    return match.group(0)


FIXTURE = r'''
/* MODEL */
#include "upscaler_rows.h"
#include "auto_settings.h"
#include "menu_restart.h"
#include "menu/preset.h"
#include "monitor_choice.h"
#ifdef _WIN32   // (save's: the file written beside its target and moved over it)
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <atomic>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
/* MODULE_HINTS */
namespace mkwvk {
/* OPTIONS */
Options opts;
/* TABS */
/* ROW */
/* AUTO_PRESET */
std::atomic<bool> present_native_pq{false};
bool crt_native_pq() { return present_native_pq.load(); }   // (vk_menu.cpp's, for the module: the game presents PQ)
/* MENU_TEXT */
namespace {
/* CRT_TEXTS */
}
// (the module's two rows, gpu/vk/menu/crt.cpp's text, and the Rows the menu makes of them as row_table makes one)
const menu::Setting crt_settings[] = {menu::Setting/* CRT_FILTER_SETTING */, menu::Setting/* CRT_LINES_SETTING */};
template <int I> const char* crt_text(int v) {
    static std::string s;
    s = crt_settings[I].text(v);
    return s.c_str();
}
template <int I> const char* crt_hint(int v, int variant) { return crt_settings[I].hint(v, variant); }
template <int I> Row crt_row() {
    const menu::Setting* st = &crt_settings[I];
    Row r{TAB_GRAPHICS, st->label, nullptr, 0, 0, 0, nullptr, nullptr};
    r.value = st->value;
    r.lo = st->lo, r.hi = st->hi, r.step = st->step;
    r.text = crt_text<I>;
    r.help = st->help;
    r.hint = st->hint ? crt_hint<I> : nullptr;
    r.restart = st->restart;
    r.preset_exempt = st->preset_exempt;
    r.setting = st;
    return r;
}
Row rows[] = {crt_row<0>(), crt_row<1>()};
constexpr int kRows = sizeof rows / sizeof rows[0];

// Output enumeration stands in for the monitor, not for any CRT operation.
struct Out { uint32_t w, h; };
const std::vector<Out>& outputs() {
    static const std::vector<Out> modes = {{2048, 1152}, {1920, 1080}};
    return modes;
}
Out output_at(int v) { return outputs().at((size_t)v); }
int custom_index() { return (int)outputs().size(); }
int parse_output(const char*) { CHECK(false); return 0; }
int step_output_choice(int, int) { CHECK(false); return 0; }
int output_for_aspect(int) { CHECK(false); return 0; }
// (the Frame generation rows in set_row: DLSS FG offered (framegen_by_dlss_offered); no DLSS FG top here (vk_dlss.cpp
// dlss_fg_most, vk_framegen.cpp framegen_capped))
bool framegen_by_dlss_offered() { return true; }
int dlss_fg_most() { return 0; }
int framegen_capped(int framegen, int) { return framegen; }
bool aspect_follows_host(int) { return false; }
int aspect_started = 0, ultrawide_read = 1;
void logf(const char*, ...) {}

namespace ur = upscaler_rows;
int upscaler() { return opts.dlss; }
// (the Output resolution's preview in play, none; a module's row locked by its own locked(), none: the CRT rows have none)
Out output_preview_target{};
bool module_locked(const int*) { return false; }
// (the device rows' choices, saved by their ids: none here)
int output_pick = 0, monitor_pick = 0;
std::string output_want, output_saved_name, monitor_want, monitor_saved_name;
namespace gpu_choice { struct Card {}; inline std::string card_id(const std::vector<Card>&, int) { return {}; } }
std::vector<gpu_choice::Card> gpu_device_cards;
std::string gpu_saved_card;
bool gpu_card_missing = false;
std::mutex options_save_mu;
std::string output_id_key(const char* id) { return id; }
std::string output_name_key(const char* name) { return name; }
void refresh_outputs() {}
bool can_outputs() { return false; }
void refresh_monitors() {}
bool can_monitors() { return false; }
int mods_slot = -1;   // (the Mods tab's restart slot: no mods here)
bool can_mods() { return false; }
// (set_row's other rows' actions: none is a CRT row's)
bool debug_warning = false, menu_refresh = false;
int debug_warning_choice = 0;
bool output_forced() { return false; }
Out picture_for(int v) { return output_at(v); }
bool request_native_preview(Out) { CHECK(false); return false; }
void sync_transition_autosave() { CHECK(false); }
std::atomic<int> grass_distance_now{0};
unsigned upscalers_available() { return ur::bit(ur::GAME) | ur::dlss_bits(true); }
bool filter_needed() { return true; }
const char* dlss_preset_env() { return nullptr; }
bool framegen_by_fixed() { return false; }   // (vk_menu.cpp: Frame generation by with nothing to choose)
/* ROW_FIXED */
/* STOPS */
namespace {   // (vk_menu.cpp's anonymous namespace, which the helpers close and open again around env_release)
/* ENV_HELPERS */
}
[[maybe_unused]] void (*const env_override_unused)(int*, int) = env_override;   // (an environment's override: no CRT case sets one)
/* PRESET_ENGINE */

std::string ini_file;
std::string ini_path() { return ini_file; }
std::string mic_want;
std::string mic_key(const char* name) { return name; }
void refresh_mics() {}
void mic_tell_host() {}
namespace i18n { int game_lang_index() { return 0; } }   // no external language.txt in this fixture
/* RUMBLE_STEP */
constexpr int HOS_KEY_N = 1, HOS_SW_N = 1, HOS_SW_PLUS = 10, HOS_SW_MINUS = 11, HOS_BIND_PAD = 0, HOS_BIND_KEY = 1, HOS_BIND_SPRINT_AUTO = 2;
constexpr int HOS_KEY_OC_NOTE_L = 1, HOS_KEY_OC_NOTE_X = 1, HOS_KEY_TOGGLE_HUD = 1, HOS_KEY_SPRINT = 1, HOS_BIND_SPRINT_TOGGLE = 5;
const char* const bind_names[HOS_KEY_N] = {"a"};
struct Host { int (*binding)(int, int, int); float (*aspect_ratio)(); int version; void (*bind)(int, int, int, int);
              void (*audio_output_select)(const char*); void (*monitor_select)(const char*); uint64_t (*mods_changed)(); };
Host host_value{}, *host = &host_value;
bool can_bind() { return false; }
/* INI_KEY */
/* TOGGLE_HELPERS */
/* SAVE */

// The real read block, including old-file migrations, can run more than once
// without menu_init's process-wide once flag or its unrelated device setup.
bool load_options() {
    opts = Options{};
    mic_want.clear();
    bool has_options = false, has_output = false, has_vram_budget = false, has_memory_budget = false;
/* LOAD_OPTIONS */
    (void)has_output;
    (void)has_vram_budget;
    (void)has_memory_budget;
    return has_options;
}

namespace mr = menu_restart;
mr::State restart_state;
int restart_slot[kRows];
const char* restart_names[mr::kMax];
/* ROW_RESTART_VALUE */
/* RESTART_INIT */
/* RESTART_VALUES */
/* RESTART_PENDING */

// CRT rows must not invoke host/device actions belonging to other rows.
void apply_pad_layout(int) { CHECK(false); }
int detect_pad_layout() { return opts.pad_layout; }   // (set_row asks it after a Layout preset, as main's own fixtures stand in)
bool can_mic() { return false; }
void mic_chosen() { CHECK(false); }
void samplers_changed() { CHECK(false); }
void temporal_reset_failure() { CHECK(false); }
void menu_debug_rows(int, int) { CHECK(false); }
void prompts_at_once(const Row& r) { CHECK(r.value != &opts.prompts); }
/* SET_ROW */

void write_ini(const char* text) {
    FILE* f = std::fopen(ini_file.c_str(), "w");
    CHECK(f);
    CHECK(std::fputs(text, f) >= 0);
    CHECK(std::fclose(f) == 0);
}
std::string saved_text() {
    FILE* f = std::fopen(ini_file.c_str(), "r");
    CHECK(f);
    std::string text;
    char line[256];
    while (std::fgets(line, sizeof line, f)) text += line;
    CHECK(!std::ferror(f));
    CHECK(std::fclose(f) == 0);
    return text;
}
void check_saved(int filter, int lines) {
    const std::string text = saved_text();
    const auto once = [&](const char* key, int value) {
        const std::string prefix = std::string(key) + "=";
        const size_t at = text.find(prefix);
        CHECK(at != std::string::npos && (at == 0 || text[at - 1] == '\n'));
        CHECK(text.find(prefix, at + prefix.size()) == std::string::npos);
        CHECK(text.compare(at, prefix.size() + std::to_string(value).size() + 1,
                           prefix + std::to_string(value) + "\n") == 0);
    };
    once("crt_filter", filter);
    once("crt_lines", lines);
}
void test_rows_and_labels() {
    CHECK(opts.crt_filter == 0 && opts.crt_lines == 0);
    CHECK(rows[0].value == &opts.crt_filter && rows[1].value == &opts.crt_lines);
    CHECK(rows[0].lo == 0 && rows[0].hi == 4 && rows[0].step == 1);
    CHECK(rows[1].lo == 0 && rows[1].hi == 2 && rows[1].step == 1);
    CHECK(!std::strcmp(ini_key(rows[0]), "crt_filter"));
    CHECK(!std::strcmp(ini_key(rows[1]), "crt_lines"));
    for (const Row& r : rows) {
        CHECK(r.tab == TAB_GRAPHICS && r.preset_exempt && !r.restart);
        CHECK(!r.heading && r.cheat < 0 && !row_fixed(r.value));
        CHECK(r.text && r.help && r.hint);
    }
    const char* const filters[] = {"Off", "Lottes aperture grille", "Lottes consumer TV", "Hyllian Fast", "zfast CRT"};
    const char* const lines[] = {"240", "480", "720"};
    for (int v = 0; v <= 4; v++) CHECK(!std::strcmp(rows[0].text(v), filters[v]));
    for (int v = 0; v <= 2; v++) CHECK(!std::strcmp(rows[1].text(v), lines[v]));
    for (int v : {INT_MIN, -1, 5, INT_MAX}) CHECK(!std::strcmp(rows[0].text(v), "Off"));
    for (int v : {INT_MIN, -1, 3, INT_MAX}) CHECK(!std::strcmp(rows[1].text(v), "240"));
    for (const Row& r : rows) {
        for (int v : {INT_MIN, -1, INT_MAX}) CHECK(!r.hint(v, 0));
        for (int v = r.lo; v <= r.hi; v++) CHECK(r.hint(v, -1) && r.hint(v, 0));
    }
    CHECK(std::strstr(rows[0].help, "HUD") && std::strstr(rows[0].help, "overlays"));
    CHECK(std::strstr(rows[1].help, "half the destination height") && std::strstr(rows[1].help, "moire"));
    CHECK(std::strstr(rows[0].hint(1, 0), "not a calibrated Sony model"));
    // (CRT lines only with a filter on: its shown(); the filter's row always)
    CHECK(!rows[0].setting->shown && rows[1].setting->shown && !rows[1].setting->shown());
    opts.crt_filter = 1;
    CHECK(rows[1].setting->shown());
    opts.crt_filter = 0;
    restart_init();
    // (no restart slot of the CRT rows'; the one there is the Mods tab's, restart_init's own)
    CHECK(restart_slot[0] < 0 && restart_slot[1] < 0 && restart_state.n == (mods_slot >= 0 ? 1 : 0) && restart_pending() == 0);
    std::puts("PASS: CRT rows default to Off/240, use ordinary keys, stay live and have bounded labels");
}
void test_hdr_hints() {
    for (int filter = 0; filter <= 4; filter++) {
        opts.crt_filter = filter;
        const std::string chosen = rows[0].text(filter);
        for (int hdr = 0; hdr <= 2; hdr++) {
            opts.hdr = hdr;
            present_native_pq = false;   // A saved Native choice is not itself a PQ picture.
            CHECK(!std::strstr(rows[0].hint(filter, -1), "Bypassed"));
        }
        present_native_pq = true;
        CHECK((std::strstr(rows[0].hint(filter, -1), "Bypassed") != nullptr) == (filter != 0));
        CHECK(std::strstr(rows[0].hint(filter, 1), "Native HDR (PQ)"));
        CHECK(!rows[0].hint(filter, 2));   // The menu's help-height enumeration terminates.
        CHECK(chosen == rows[0].text(filter) && opts.crt_filter == filter);
    }
    for (int v = 0; v <= 2; v++) CHECK(!rows[1].hint(v, 1));
    present_native_pq = false;
    opts = Options{};
    std::puts("PASS: Native PQ explains the bypass without changing the saved CRT choice or its label");
}
void test_persistence() {
    CHECK(!load_options());   // No options file: production defaults.
    CHECK(opts.crt_filter == 0 && opts.crt_lines == 0);
    write_ini("compile_shaders=0\n");
    CHECK(!load_options() && opts.crt_filter == 0 && opts.crt_lines == 0);
    write_ini("options_version=4\nupscaling=2\n");
    CHECK(load_options() && opts.crt_filter == 0 && opts.crt_lines == 0);
    write_ini("crt_filter=4\n");
    CHECK(load_options() && opts.crt_filter == 4 && opts.crt_lines == 0);
    write_ini("crt_lines=2\n");
    CHECK(load_options() && opts.crt_filter == 0 && opts.crt_lines == 2);
    write_ini("crt_filter=not-a-number\ncrt_lines=\n");
    CHECK(load_options() && opts.crt_filter == 0 && opts.crt_lines == 0);
    for (int v : {INT_MIN, -1, 0, 1, 2, 3, 4, 5, 720, INT_MAX}) {
        char text[96];
        std::snprintf(text, sizeof text, "crt_filter=%d\ncrt_lines=%d\n", v, v);
        write_ini(text);
        CHECK(load_options());
        CHECK(opts.crt_filter == std::clamp(v, 0, 4));
        CHECK(opts.crt_lines == std::clamp(v, 0, 2));
        save();
        check_saved(opts.crt_filter, opts.crt_lines);
    }
    // save replaces stale duplicate row keys while preserving unrelated lines.
    write_ini("crt_filter=4\ncrt_lines=2\ncrt_filter=1\ncrt_lines=0\ncustom_note=keep\npad_a=4\n");
    CHECK(load_options() && opts.crt_filter == 1 && opts.crt_lines == 0);
    for (int filter = 0; filter <= 4; filter++) {
        for (int lines = 0; lines <= 2; lines++) {
            set_row(rows[0], filter, 1);
            set_row(rows[1], lines, 1);
            check_saved(filter, lines);
            CHECK(saved_text().find("custom_note=keep\n") != std::string::npos);
            CHECK(saved_text().find("pad_a=4\n") != std::string::npos);
            CHECK(load_options() && opts.crt_filter == filter && opts.crt_lines == lines);
            CHECK(restart_pending() == 0);
        }
    }
    std::puts("PASS: CRT keys round-trip every choice, retain missing-key defaults and clamp through menu_init");
}
void test_presets_and_live_changes() {
    for (int filter = 0; filter <= 4; filter++) {
        for (int lines = 0; lines <= 2; lines++) {
            for (int preset = 0; preset <= 5; preset++) {
                opts = Options{};
                opts.crt_filter = filter, opts.crt_lines = lines, opts.preset = preset;
                apply_preset(preset);
                CHECK(opts.crt_filter == filter && opts.crt_lines == lines);
            }
            for (int tier = 1; tier <= 5; tier++) {
                auto_choice = {};
                auto_choice.tier = tier;
                // Auto no longer chooses the output size; keep the player's chosen mode.
                opts = Options{};
                opts.crt_filter = filter, opts.crt_lines = lines;
                opts.output_res = 1;
                apply_preset(kAutoPreset);   // The production Auto path, not a no-op stand-in.
                CHECK(opts.preset == kAutoPreset && opts.crt_filter == filter && opts.crt_lines == lines);
                CHECK(opts.output_res == 1);
                apply_auto();               // Choosing again after the hardware changes also preserves CRT.
                CHECK(opts.crt_filter == filter && opts.crt_lines == lines);
                CHECK(opts.output_res == 1);
            }
        }
    }
    for (int preset = 1; preset <= kAutoPreset; preset++) {
        opts = Options{};
        opts.preset = preset;
        for (Row& r : rows) {
            for (int v = r.lo; v <= r.hi; v++) {
                set_row(r, v, 1);
                CHECK(*r.value == v && opts.preset == preset && restart_pending() == 0);
            }
            set_row(r, r.hi + r.step, 1);
            CHECK(*r.value == r.lo && opts.preset == preset);
            set_row(r, r.lo - r.step, -1);
            CHECK(*r.value == r.hi && opts.preset == preset);
        }
    }
    check_saved(opts.crt_filter, opts.crt_lines);
    std::puts("PASS: every quality preset and Auto preserve CRT; live row changes and wrapping preserve the preset");
}
int run(int argc, char** argv) {
    CHECK(argc == 2);
    ini_file = argv[1];   // Always inside the driver's new temporary directory.
    test_rows_and_labels();
    test_hdr_hints();
    test_persistence();
    test_presets_and_live_changes();
    return 0;
}
} // namespace mkwvk
int main(int argc, char** argv) { return mkwvk::run(argc, argv); }
'''


def fixture(source):
    menu = (source / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    header = (source / "gpu/vk/vk_menu.h").read_text(encoding="utf-8")
    # (#1633: the CRT rows are gpu/vk/menu/crt.cpp's, the Quality preset's engine gpu/vk/menu/preset.cpp's, and what a module
    # shares, menu_module.h's)
    crt = (source / "gpu/vk/menu/crt.cpp").read_text(encoding="utf-8")
    preset = (source / "gpu/vk/menu/preset.cpp").read_text(encoding="utf-8")
    module_h = (source / "gpu/vk/menu_module.h").read_text(encoding="utf-8")
    settings = {}
    for key in ("crt_filter", "crt_lines"):
        at = crt.find('.row({.key = "%s",' % key)
        if at < 0:
            raise SystemExit("FAIL: missing production %s row" % key)
        row = block(crt, at)
        settings[key] = row[row.index("{"):]
    init = definition(menu, "void menu_init")
    load_at = init.index('    if (FILE* f = fopen(ini_path().c_str(), "r")) {')
    hints_at = module_h.index("#define MODULE_HINTS(")
    engine_at = preset.index("#ifndef MENU_LAYOUT_ONLY\nnamespace ur = upscaler_rows;")
    replacements = {
        "/* MODEL */": menu_model_prelude(source),
        "/* MODULE_HINTS */": module_h[hints_at:module_h.index("    }\n", hints_at) + 6],
        "/* OPTIONS */": declaration(header, r"struct Options \{.*?\n\};", "Options"),
        "/* TABS */": declaration(menu, r"enum MenuTab \{.*?\n\};", "MenuTab"),
        "/* ROW */": declaration(menu, r"struct Row \{.*?\n\};", "Row"),
        "/* AUTO_PRESET */": declaration(header, r"constexpr int kAutoPreset = \d+;", "kAutoPreset"),
        "/* MENU_TEXT */": block(module_h, module_h.index("namespace menu_text {")),
        "/* CRT_TEXTS */": between(crt, "namespace {\n", "} // namespace\n")[len("namespace {\n"):],
        "/* CRT_FILTER_SETTING */": settings["crt_filter"],
        "/* CRT_LINES_SETTING */": settings["crt_lines"],
        "/* ROW_FIXED */": definition(menu, "bool row_fixed"),
        "/* STOPS */": definition(menu, "bool stops_at_ends"),
        "/* ENV_HELPERS */": menu[menu.index("struct EnvOverride {"):menu.index("std::string ini_path() {")],
        "/* PRESET_ENGINE */": preset[engine_at + len("#ifndef MENU_LAYOUT_ONLY\n"):preset.index("#endif", engine_at)],
        "/* RUMBLE_STEP */": definition(menu, "int rumble_step"),
        "/* INI_KEY */": definition(menu, "const char* ini_key"),
        "/* TOGGLE_HELPERS */": definition(menu, "bool has_toggle") + "\n" + definition(menu, "bool toggle_on") + "\n" + definition(menu, "void toggle_set"),
        "/* SAVE */": definition(menu, "void save"),
        "/* LOAD_OPTIONS */": init[load_at:init.index("    first_start = !has_options;", load_at)],
        "/* ROW_RESTART_VALUE */": definition(menu, "int row_restart_value"),
        "/* RESTART_INIT */": definition(menu, "void restart_init"),
        "/* RESTART_VALUES */": definition(menu, "void restart_values"),
        "/* RESTART_PENDING */": definition(menu, "unsigned restart_pending"),
        "/* SET_ROW */": definition(menu, "void set_row"),
    }
    result = FIXTURE
    for marker, value in replacements.items():
        result = result.replace(marker, value)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    source = args.source.resolve()
    # (where the rows are: the Graphics tab as the menu model lays it out, gpu/vk/menu/crt.cpp's place for them)
    pages, _ = built_menu(args.cxx, source)
    keys = [item.key for item in pages["graphics"]]
    window = keys.index("upscaling")   # (the Window scaling filter's key, ini_key)
    if keys[window + 1:window + 3] != ["crt_filter", "crt_lines"]:
        raise SystemExit("FAIL: CRT filter and CRT lines must immediately follow Window scaling filter")
    code = fixture(source)
    with tempfile.TemporaryDirectory(prefix="crt-options-") as tmp:
        cpp = Path(tmp) / "crt_options.cpp"
        exe = Path(tmp) / ("crt_options.exe" if os.name == "nt" else "crt_options")
        cpp.write_text(code, encoding="utf-8")
        includes = str(source / "gpu/vk")
        if Path(args.cxx).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
            command = [args.cxx, "/nologo", "/std:c++20", "/EHsc", "/W4", "/WX", "/D_CRT_SECURE_NO_WARNINGS",
                       "/I" + includes, str(cpp), "/Fo" + str(Path(tmp) / "crt_options.obj"), "/Fe" + str(exe)]
        else:
            command = [args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
                       "-I", includes, str(cpp), "-o", str(exe)]
        subprocess.run(command, cwd=tmp, check=True)
        subprocess.run([str(exe), str(Path(tmp) / "longshot_options.ini")], cwd=tmp, check=True)


if __name__ == "__main__":
    main()
