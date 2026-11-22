"""The Insert menu's rows through the model (#1633, step 2b): gpu/vk/vk_menu.cpp's rows as drawn (its MenuTab, Row and the
block from kCoreRows to the end of the rows as drawn) compiled over a small table of its own shape, with no game.

- With no module, each tab's order is the table's own, row for row (headings included, a tab's rows apart in the table
  included), and rows[i] is the table's row i: the menu draws as before.
- With a module, its rows go where it asks (after and before a core row, a section and heading of its own), after the
  table's rows in rows[] (so they keep their index), with their text, hint and key from the setting; a
  button with its on_activate and verb, a row that opens a page with row_opens; a kind the menu doesn't draw yet (a line)
  left out and logged.

    python -I -B tests/test_menu_rows_model.py [--cxx g++]
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from srccut import block, between  # noqa: E402

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]

HEAD = r'''
#include "menu_model.h"
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)
static std::vector<std::string> logged;

namespace mkwvk {
void logf(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    logged.emplace_back(line);
}
namespace {
'''

TABLE = r'''
int vx[8];
const char* tx_level(int v) { static const char* t[] = {"Low", "Medium", "High", "Ultra"}; return t[v & 3]; }
namespace core {
Row rows[] = {
    {TAB_GRAPHICS, "Quality preset", &vx[0], 0, 3, 1, tx_level, "Sets the rest."},
    {TAB_GRAPHICS, "Resolution and upscaling", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},
    {TAB_GRAPHICS, "Upscaler", &vx[1], 0, 3, 1, tx_level, "Who draws the picture."},
    {TAB_GAMEPLAY, "Text speed", &vx[2], 0, 3, 1, tx_level, "How fast text goes."},
    {TAB_GRAPHICS, "Screen", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},
    {TAB_GRAPHICS, "HDR", &vx[3], 0, 2, 1, tx_level, "High dynamic range.", nullptr, true},
    {TAB_CONTROLS, "Motion", nullptr, 0, 0, 0, nullptr, "Opens the Motion page."},
    {TAB_MOTION, "Gyro aiming", &vx[4], 0, 1, 1, tx_level, "Aims with the controller."},
    {TAB_GAMEPLAY, "Bug report", nullptr, 0, 0, 0, nullptr, "Saves a report."},   // (the table's last, as vk_menu.cpp's)
};
} // namespace core
'''

INI_KEY = r'''
const char* ini_key(const Row& r) {
    if (r.setting && r.setting->key) return r.setting->key;
    static std::deque<std::string> made;
    std::string k;
    for (const char* c = r.name; *c; c++) k += *c == ' ' ? '_' : (char)tolower((unsigned char)*c);
    made.push_back(k);
    return made.back().c_str();
}
} // namespace
} // namespace mkwvk
using namespace mkwvk;
'''

MODULE = r'''
static int white = 0, peak = 400, extra = 0, pressed = 0;
static bool hdr_on() { return vx[3] != 0; }
MENU_MODULE(hdr_test) {
    menu::tab("graphics").section("screen")
        .row({.key = "hdr_white", .label = "HDR paper white", .help = "How bright white is.", .kind = menu::Kind::Toggle,
              .value = &white, .shown = [] { return hdr_on(); },
              .hint = [](int v, int variant) { return variant <= 0 && v ? "Brighter" : nullptr; }}, menu::after("hdr"))
        .row({.key = "hdr_peak", .label = "HDR peak", .kind = menu::Kind::Slider, .value = &peak, .lo = 100, .hi = 1000,
              .step = 100, .text = [](int v) { return std::to_string(v) + " nits"; }}, menu::before("upscaler"))
        .row({.key = "hdr_test_button", .label = "Test HDR", .kind = menu::Kind::Action, .on_activate = [] { pressed++; },
              .verb = "Test"})
        .row({.key = "hdr_note", .label = "A note", .kind = menu::Kind::Line});
    menu::tab("controls").row({.key = "motion_page", .label = "Motion", .kind = menu::Kind::Page, .opens = "motion"});
    menu::tab("gameplay").heading("extras", "Extras").row({.key = "extra_choice", .label = "Extra", .value = &extra,
                                                           .choices = {"A", "B", "C"}});
}
'''

MAIN = r'''
static std::string order(int t) {
    std::string out;
    for (int i : row_order(t)) out += (out.empty() ? "" : "|") + std::string(rows[i].name);
    return out;
}
int main() {
    // (the table's rows: the same rows, at the same index)
    for (int i = 0; i < kCoreRows; i++)
        CHECK(rows[i].name == core::rows[i].name && rows[i].value == core::rows[i].value && rows[i].text == core::rows[i].text &&
              !rows[i].setting);
    CHECK(!strcmp(rows[kCoreRows - 1].name, "Bug report"));   // (its own table's last row, at its index)
    CHECK(order(TAB_MOTION) == "Gyro aiming" && order(TAB_NFC).empty());
    CHECK(menu::diagnostics().empty());
    for (const auto& d : menu::diagnostics()) fprintf(stderr, "  diagnostic: %s\n", d.c_str());
#ifndef WITH_MODULE
    CHECK(std::size(rows) == (size_t)kCoreRows);
    CHECK(order(TAB_CONTROLS) == "Motion");
    CHECK(order(TAB_GRAPHICS) == "Quality preset|Resolution and upscaling|Upscaler|Screen|HDR");
    CHECK(order(TAB_GAMEPLAY) == "Text speed|Bug report");
    for (int t = 0; t <= TAB_PAGE; t++) {   // (each tab: the table's own rows of it, in the table's order)
        std::vector<int> own;
        for (int i = 0; i < kCoreRows; i++) if (core::rows[i].tab == t) own.push_back(i);
        CHECK(row_order(t) == own);
    }
    CHECK(logged.empty());
    printf("rows as drawn, no module: the table's own order, row for row\n");
#else
    // (the peak, the white, the button, Extras' heading and its row, the Motion page's row; not the line)
    CHECK(std::size(rows) == (size_t)kCoreRows + 6);
    CHECK(order(TAB_GRAPHICS) == "Quality preset|Resolution and upscaling|HDR peak|Upscaler|Screen|HDR|HDR paper white|Test HDR");
    CHECK(order(TAB_CONTROLS) == "Motion|Motion");   // (the table's own Motion row, then the module's)
    CHECK(order(TAB_GAMEPLAY) == "Text speed|Bug report|Extras|Extra");
    int seen = 0;
    for (const Row& r : rows) seen += r.setting != nullptr;
    CHECK(seen == 5);
    for (const Row& r : rows) {
        if (r.setting && !strcmp(r.name, "Test HDR")) {
            CHECK(!r.value && r.setting->verb && !strcmp(r.setting->verb, "Test") && row_opens(r) < 0);
            r.setting->on_activate();
            CHECK(pressed == 1);
        }
        if (r.setting && r.tab == TAB_CONTROLS) CHECK(row_opens(r) == TAB_MOTION && !r.value);
        if (!r.setting) CHECK(row_opens(r) < 0);
    }
    const Row* white_row = nullptr; const Row* peak_row = nullptr; const Row* extra_row = nullptr; const Row* heading = nullptr;
    for (const Row& r : rows) {
        if (!strcmp(r.name, "HDR paper white")) white_row = &r;
        if (!strcmp(r.name, "HDR peak")) peak_row = &r;
        if (!strcmp(r.name, "Extra")) extra_row = &r;
        if (!strcmp(r.name, "Extras")) heading = &r;
    }
    CHECK(white_row && peak_row && extra_row && heading);
    if (white_row && peak_row && extra_row && heading) {
        CHECK(white_row - &rows[0] >= kCoreRows && white_row->tab == TAB_GRAPHICS && white_row->lo == 0 && white_row->hi == 1);
        CHECK(!strcmp(white_row->text(0), "Off") && !strcmp(white_row->text(1), "On"));
        CHECK(white_row->hint && !strcmp(white_row->hint(1, -1), "Brighter") && !white_row->hint(0, -1));
        CHECK(!strcmp(white_row->help, "How bright white is.") && !strcmp(ini_key(*white_row), "hdr_white"));
        CHECK(white_row->setting->shown && !white_row->setting->shown());   // (HDR off: row_shown hides it)
        const std::string lo = peak_row->text(100), hi = peak_row->text(1000);
        CHECK(lo == "100 nits" && hi == "1000 nits" && peak_row->step == 100 && !peak_row->hint);
        CHECK(!strcmp(extra_row->text(2), "C") && extra_row->hi == 2 && extra_row->tab == TAB_GAMEPLAY);
        CHECK(heading->heading && !heading->value && heading->tab == TAB_GAMEPLAY);
    }
    bool told = false;
    for (const auto& l : logged) told |= l.find("hdr_note: a line or custom row, which the Insert menu doesn't draw yet") != std::string::npos;
    CHECK(told);
    printf("rows as drawn, with a module: its rows where it asked, with their own text, hint and key\n");
#endif
    return failures ? 1 : 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    tabs = block(menu, "enum MenuTab {") + ";\n"
    row = block(menu, "struct Row {") + ";\n"
    drawn = between(menu, "constexpr int kCoreRows =", "// ---- end of the rows as drawn ----")
    for must in ("row_order(", "RowTable rows;", "MENU_MODULE(core)", "rows[i].setting->shown"):
        if must not in drawn and must not in menu:
            raise SystemExit("vk_menu.cpp: %s is gone" % must)
    if "for (int i : row_order(t))" not in menu or "(!rows[i].setting || !rows[i].setting->shown" not in menu:
        raise SystemExit("vk_menu.cpp: tab_row_count, tab_row_to_global or row_shown no longer go by the model's order and hooks")
    source = HEAD + tabs + row + TABLE + drawn + INI_KEY
    with tempfile.TemporaryDirectory(prefix="menu-rows-model-") as tmp:
        for with_module in (False, True):
            src = Path(tmp) / ("rows_%d.cpp" % with_module)
            src.write_text(source + (MODULE if with_module else "") + MAIN, encoding="utf-8")
            exe = Path(tmp) / ("rows_%d" % with_module)
            subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                            "-I" + str(ROOT / "gpu" / "vk")] + (["-DWITH_MODULE"] if with_module else []) +
                           [str(src), str(ROOT / "gpu" / "vk" / "menu_model.cpp"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
    print("PASS: the menu's rows through the model")


if __name__ == "__main__":
    main()
