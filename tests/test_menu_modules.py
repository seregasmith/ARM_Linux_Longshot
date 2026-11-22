"""The Insert menu's feature modules (gpu/vk/menu/*.cpp, #1633), compiled with the model and run with no game, over a stand-in
for the core module made from gpu/vk/vk_menu.cpp's own text (tests/menu_model_fixture.py core_module: its tabs and pages, its
rows in their order with their ini keys, its sections). So:

- every module's place resolves (no missing anchor, no cycle, no doubled key, no page or section no one made): build()'s
  diagnostics are empty;
- each module's rows are where they belong (the HDR section after Screen, its rows in order), show when they should, and
  give the text the rows had in vk_menu.cpp.

    python -I -B tests/test_menu_modules.py [--cxx g++]
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from menu_model_fixture import compile_menu  # noqa: E402

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"

MAIN = r'''#include "menu_module.h"
#include <cstdio>
#include <cstring>
#include <string>
using namespace mkwvk;
namespace mkwvk { Options opts; }
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// CORE

static std::string order(const char* page, bool shown_only) {
    std::string out;
    if (shown_only) {
        for (const auto& v : menu::snapshot(page)) out += std::string(out.empty() ? "" : " ") + (v.kind == menu::Kind::Heading ? "#" : "") + v.key;
    } else {
        for (const auto& it : menu::layout(page)) out += std::string(out.empty() ? "" : " ") + (it.heading ? "#" : "") + it.key;
    }
    return out;
}
static std::string value_of(const char* page, const char* key) {
    for (const auto& v : menu::snapshot(page))   // (a row's: a section can have the same name, as HDR's)
        if (v.kind != menu::Kind::Heading && v.key && !strcmp(v.key, key)) return v.value;
    return "(not shown)";
}

int main() {
    menu::set_log([](const char*) {});
    menu::build();
    for (const auto& d : menu::diagnostics()) fprintf(stderr, "  diagnostic: %s\n", d.c_str());
    CHECK(menu::diagnostics().empty());
    // (HDR: its section right after Screen's rows, before Detail, its rows in order)
    const std::string g = order("graphics", false);
    const size_t hdr = g.find("#hdr hdr hdr_paper_white hdr_peak_brightness hdr_contrast hdr_tone_mapper #accessibility colour_vision #post_processing");
    CHECK(hdr != std::string::npos && g.find("#screen") < hdr);
    opts.hdr = 0;
    std::string shown = order("graphics", true);
    CHECK(shown.find("#hdr hdr #accessibility colour_vision #post_processing reshade_preset reshade_compare #detail") != std::string::npos);
    opts.hdr = 1, opts.hdr_white = 200, opts.hdr_peak = 1000, opts.hdr_contrast = 100;
    shown = order("graphics", true);
    CHECK(shown.find("#hdr hdr hdr_paper_white hdr_peak_brightness hdr_contrast #accessibility colour_vision #post_processing") != std::string::npos);
    CHECK(value_of("graphics", "hdr") == "Expanded" && value_of("graphics", "hdr_paper_white") == "200 nits" &&
          value_of("graphics", "hdr_peak_brightness") == "1000 nits" && value_of("graphics", "hdr_contrast") == "100%");
    opts.hdr = 2, opts.hdr_tonemap = 1;
    shown = order("graphics", true);
    CHECK(shown.find("#hdr hdr hdr_paper_white hdr_peak_brightness hdr_contrast hdr_tone_mapper #accessibility colour_vision #post_processing") != std::string::npos);
    CHECK(value_of("graphics", "reshade_preset") == "Off");
    opts.reshade = 3;
    CHECK(order("graphics", true).find("reshade_preset reshade_compare reshade_user_pick reshade_rescan #detail") != std::string::npos);
    const auto* compare = menu::find("reshade_compare");
    CHECK(compare && compare->kind == menu::Kind::Action);
    const auto* preset = menu::find("reshade_preset");
    CHECK(preset && preset->hi == 26 && preset->choices.size() == 27);
    CHECK(value_of("graphics", "reshade_preset") == "User");
    // (the numbers saved in longshot_options.ini, gpu/vk/reshade/fx_builtin.h: the Clarity looks from 7, the last at 26)
    opts.reshade = 4;
    CHECK(value_of("graphics", "reshade_preset") == "HDR Vibrance");
    opts.reshade = 7;
    CHECK(value_of("graphics", "reshade_preset") == "Serfrost (Recommended)");
    opts.reshade = 26;
    CHECK(value_of("graphics", "reshade_preset") == "Clarity Base");
    CHECK(value_of("graphics", "hdr") == "Native (the game's)" && value_of("graphics", "hdr_tone_mapper") == "PsychoV35");
    CHECK(!strcmp(tx_hdr(0), "Off") && !strcmp(tx_hdr(2), "Native (the game's)"));
    for (const char* key : {"hdr", "hdr_paper_white", "hdr_peak_brightness", "hdr_contrast", "hdr_tone_mapper"})
        CHECK(menu::find(key) && menu::find(key)->preset_exempt && !menu::find(key)->restart);
    CHECK(menu::find("colour_vision") && menu::find("colour_vision")->preset_exempt && !menu::find("colour_vision")->restart);
    if (failures) {
        fprintf(stderr, "the Graphics tab as built: %s\n", g.c_str());
        return 1;
    }
    printf("menu modules: every place resolves; HDR where it was, shown and named as it was\n");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    modules = sorted((VK / "menu").glob("*.cpp"))
    if not modules:
        raise SystemExit("gpu/vk/menu/: no module")
    with tempfile.TemporaryDirectory(prefix="menu-modules-") as tmp:
        subprocess.run([str(compile_menu(args.cxx, MAIN, tmp))], check=True)
    print("PASS: the menu's modules (%s)" % ", ".join(m.stem for m in modules))


if __name__ == "__main__":
    main()
