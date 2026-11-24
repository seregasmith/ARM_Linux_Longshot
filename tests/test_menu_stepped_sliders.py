"""Stepped slider mapping, commit-on-release drag deferral, Dynamic resolution toggle, and Frame generation mode split.

Checks the pure C++ helpers in gpu/vk/vk_menu_hit.h, gpu/vk/upscaler_rows.h, and gpu/vk/framegen_rows.h
plus the menu module row declarations in gpu/vk/menu/upscaler.cpp and gpu/vk/menu/framegen.cpp.
No game, no Vulkan device.

    python -I -B tests/test_menu_stepped_sliders.py [--cxx g++]
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from menu_model_fixture import module_rows  # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]

CPP_TEST = r'''
#include "gpu/vk/vk_menu_hit.h"
#include "gpu/vk/upscaler_rows.h"
#include "gpu/vk/framegen_rows.h"

#include <cstdio>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, msg); failures++; } } while (0)

/* DRAG_INTEGRATION */

int main() {
    namespace mh = mkwvk::menu_hit;
    namespace ur = mkwvk::upscaler_rows;
    namespace fgr = mkwvk::framegen_rows;
    drag_integration::test();

    // 1. Stop-index <-> permille mapping: 1-stop, 2-stop, 5-stop, 8-stop, 9-stop, clamping, round-trips
    CHECK(mh::slider_stop_permille(0, 1) == 0, "1-stop permille is 0 (no div-by-zero)");
    CHECK(mh::slider_stop_permille(5, 1) == 0, "1-stop permille clamps");
    CHECK(mh::slider_stop_index(0, 1) == 0 && mh::slider_stop_index(500, 1) == 0 && mh::slider_stop_index(1000, 1) == 0,
          "1-stop index always 0");
    CHECK(mh::slider_stop_permille(-3, 5) == 0 && mh::slider_stop_permille(99, 5) == 1000, "permille clamps out-of-range index");
    CHECK(mh::slider_stop_index(-50, 5) == 0 && mh::slider_stop_index(1500, 5) == 4, "index clamps out-of-range permille");

    for (int count : {2, 4, 5, 7, 8, 9}) {
        for (int i = 0; i < count; i++) {
            const int p = mh::slider_stop_permille(i, count);
            CHECK(p >= 0 && p <= 1000, "permille in [0, 1000]");
            if (i == 0) CHECK(p == 0, "first stop at 0");
            if (i == count - 1) CHECK(p == 1000, "last stop at 1000");
            CHECK(mh::slider_stop_index(p, count) == i, "round-trip stop index -> permille -> index");
        }
    }

    // Non-linear ID list mapping (e.g. FSR 3.1 fixed stops 33%..100%)
    const int fsr3_ids[] = {6, 4, 5, 3, 2, 8, 9, 1};
    for (int i = 0; i < 8; i++) {
        CHECK(mh::slider_find_stop(fsr3_ids[i], fsr3_ids, 8) == i, "find_stop locates non-linear ID");
        const int p = mh::slider_stop_permille(i, 8);
        CHECK(mh::slider_stop_value(p, fsr3_ids, 8) == fsr3_ids[i], "stop_value round-trips non-linear ID");
    }

    // 2. commit_on_release DragState: immediate vs deferred commit, drag-back no-op, cancel
    {
        mh::DragState d;
        CHECK(!d.active(), "fresh drag state inactive");
        // Immediate slider (Sharpness): commits on begin and update, nothing extra on release
        CHECK(d.begin(3, 40, 20, false), "immediate slider commits on begin");
        CHECK(d.active_on(3) && d.preview == 40, "immediate slider tracks preview");
        CHECK(!d.update(40, 40), "no-op update does not recommit");
        CHECK(d.update(60, 40), "changed update commits immediately");
        int committed = -1;
        CHECK(!d.release(&committed), "immediate slider does not defer-commit on release");
        CHECK(!d.active(), "release clears drag state");

        // Deferred slider (Render resolution / Framegen multiplier): zero live commits while dragging
        CHECK(!d.begin(4, 5, 3, true), "deferred slider does not commit on begin");
        CHECK(d.active_on(4) && d.preview == 5, "deferred preview updates on begin");
        for (int stop : {4, 6, 2, 8, 9}) {
            CHECK(!d.update(stop, 3), "deferred slider never commits during drag");
            CHECK(d.preview == stop, "deferred preview tracks every stop smoothly");
        }
        CHECK(d.release(&committed) && committed == 9, "deferred slider commits final preview once on release");
        CHECK(!d.active(), "release clears deferred drag state");

        // Deferred slider dragged away and back to live value: release is a no-op
        CHECK(!d.begin(4, 6, 3, true), "deferred begin");
        CHECK(!d.update(3, 3), "dragged back to live value");
        committed = -1;
        CHECK(!d.release(&committed), "dragging back to starting value does not trigger commit");

        // Cancel (menu close / tab switch): discards uncommitted preview
        CHECK(!d.begin(4, 9, 3, true), "deferred begin before cancel");
        d.cancel();
        CHECK(!d.active(), "cancel clears drag state");
        CHECK(!d.release(&committed), "release after cancel commits nothing");
    }

    // 3. Option C: Dynamic resolution toggle <-> opts.resolution == 0 & pure ascending fixed Render resolution
    for (int up : {(int)ur::GAME, (int)ur::OFF, (int)ur::FSR3, (int)ur::DLSS, (int)ur::FSR4, (int)ur::XESS}) {
        int n = 0;
        const int* stops = ur::fixed_steps(up, n);
        CHECK(stops != nullptr && n >= 4, "every upscaler has >= 4 fixed stops");
        int prev_pct = 0;
        for (int i = 0; i < n; i++) {
            CHECK(stops[i] != 0, "Auto (0) is NEVER in fixed_steps");
            const int pct = stops[i] == ur::kNativeStop ? 100 : ur::percent(stops[i]);
            CHECK(pct > prev_pct, "fixed_steps are strictly monotonically ascending");
            prev_pct = pct;
        }
        if (ur::native_of(ur::engine(up)) >= 0)
            CHECK(stops[n - 1] == ur::kNativeStop, "Native-capable upscaler tops out at kNativeStop");
        else
            CHECK(ur::percent(stops[n - 1]) == 100, "non-Native upscaler tops out at 100%");
    }

    // Dynamic resolution toggle state & round-trip
    CHECK(ur::dynamic_resolution_on(ur::FSR3, 0), "resolution=0 on FSR3 -> Dynamic resolution On");
    CHECK(!ur::dynamic_resolution_on(ur::FSR3, 3), "resolution=3 on FSR3 -> Dynamic resolution Off");
    CHECK(!ur::dynamic_resolution_on(ur::DLAA, 0), "DLAA (full_scale Native) -> Dynamic resolution Off even when saved res=0");
    {
        int last_fixed = 0;
        int out_up = ur::FSR3;
        // On -> Off with no prior fixed stop defaults to Quality 67% (3)
        int res = ur::toggle_dynamic_resolution(ur::FSR3, 0, false, &last_fixed, &out_up);
        CHECK(res == 3 && out_up == ur::FSR3, "On -> Off defaults to 67% (3)");
        // Off (at 58% = 5) -> On remembers 5 and sets res = 0
        res = ur::toggle_dynamic_resolution(ur::FSR3, 5, true, &last_fixed, &out_up);
        CHECK(res == 0 && last_fixed == 5 && out_up == ur::FSR3, "Off -> On remembers last fixed ID and sets 0");
        // On -> Off restores remembered 58% (5)
        res = ur::toggle_dynamic_resolution(ur::FSR3, 0, false, &last_fixed, &out_up);
        CHECK(res == 5 && out_up == ur::FSR3, "On -> Off restores remembered fixed stop");
        // Off (at DLAA Native) -> On leaves DLAA back to DLSS and sets res = 0
        res = ur::toggle_dynamic_resolution(ur::DLAA, 3, true, &last_fixed, &out_up);
        CHECK(res == 0 && out_up == ur::DLSS && last_fixed == ur::kNativeStop, "DLAA -> Dynamic On remembers Native");
        res = ur::toggle_dynamic_resolution(out_up, res, false, &last_fixed, &out_up);
        CHECK(res == 0 && out_up == ur::DLAA, "Dynamic Off restores DLAA");
        for (int engine : {(int)ur::DLSS, (int)ur::FSR4, (int)ur::XESS}) {
            res = ur::toggle_dynamic_resolution(ur::native_of(engine), 9, true, &last_fixed, &out_up);
            CHECK(res == 0 && out_up == engine && last_fixed == ur::kNativeStop, "Native enables Dynamic");
            res = ur::toggle_dynamic_resolution(out_up, res, false, &last_fixed, &out_up);
            CHECK(res == 0 && out_up == ur::native_of(engine) && !ur::dynamic_resolution_on(out_up, res),
                  "Dynamic Off restores the engine's Native mode");
        }
        res = ur::toggle_dynamic_resolution(ur::FSR3, 0, false, &last_fixed, &out_up);
        CHECK(res == 3 && out_up == ur::FSR3, "Native memory falls back to Quality on an engine without Native");
    }

    // Monotonic fixed stepping (stops at ends, never wraps to Auto)
    {
        int up = ur::FSR3, res = 6; // 33% (leftmost)
        ur::step_fixed_render(up, res, -1, &up, &res);
        CHECK(res == 6, "FSR3 left at 33% stops at 33%");
        ur::step_fixed_render(up, res, 1, &up, &res);
        CHECK(res == 4, "FSR3 right from 33% -> 50%");
        res = 1; // 100% (rightmost)
        ur::step_fixed_render(up, res, 1, &up, &res);
        CHECK(res == 1, "FSR3 right at 100% stops at 100%");

        // DLSS: stepping right from 92% (9) enters DLAA without clobbering saved res; stepping left returns to 92%
        up = ur::DLSS; res = 9;
        ur::step_fixed_render(up, res, 1, &up, &res);
        CHECK(up == ur::DLAA && res == 9, "DLSS right from 92% enters DLAA preserving saved res");
        ur::step_fixed_render(up, res, 1, &up, &res);
        CHECK(up == ur::DLAA && res == 9, "DLAA right stops at Native");
        ur::step_fixed_render(up, res, -1, &up, &res);
        CHECK(up == ur::DLSS && res == 9, "DLAA left returns to DLSS 92%");
    }

    // 4. Frame generation Mode (Off / Auto / Fixed) + Multiplier stepped slider
    CHECK(fgr::mode_of(0) == fgr::MODE_OFF && fgr::mode_of(1) == fgr::MODE_AUTO, "mode_of Off/Auto");
    for (int m = 2; m <= 8; m++) CHECK(fgr::mode_of(m) == fgr::MODE_FIXED, "mode_of 2..8 is Fixed");
    {
        int last_fixed = 2;
        CHECK(fgr::step_mode(0, 1, &last_fixed, 0) == 1, "Off + Right -> Auto");
        CHECK(fgr::step_mode(1, 1, &last_fixed, 0) == 2, "Auto + Right -> Fixed (default 2x)");
        CHECK(fgr::step_mode(4, 1, &last_fixed, 0) == 0 && last_fixed == 4, "Fixed(4x) + Right -> Off, remembering 4x");
        CHECK(fgr::step_mode(0, -1, &last_fixed, 0) == 4, "Off + Left -> Fixed restoring remembered 4x");
        CHECK(fgr::step_mode(4, -1, &last_fixed, 0) == 1, "Fixed + Left -> Auto");
        CHECK(fgr::step_mode(1, -1, &last_fixed, 0) == 0, "Auto + Left -> Off");
        // RTX 40 cap (dlss_most = 1 -> top = 2x): restoring remembered 4x caps to 2x
        CHECK(fgr::step_mode(1, 1, &last_fixed, 1) == 2, "Auto + Right caps remembered 4x to 2x on RTX 40");
    }

    // Multiplier slider mapping & 1-stop edge case (RTX 40)
    CHECK(fgr::fixed_top(0) == 8 && fgr::fixed_stop_count(0) == 7, "uncapped: 2x..8x (7 stops)");
    CHECK(fgr::fixed_top(5) == 6 && fgr::fixed_stop_count(5) == 5, "RTX 50: 2x..6x (5 stops)");
    CHECK(fgr::fixed_top(1) == 2 && fgr::fixed_stop_count(1) == 1, "RTX 40: 2x only (1 stop)");
    CHECK(fgr::multiplier_permille(2, 1) == 0 && fgr::multiplier_from_permille(500, 1) == 2,
          "RTX 40 1-stop multiplier permille/value safe with zero div-by-zero");
    for (int most : {0, 5}) {
        const int top = fgr::fixed_top(most);
        for (int v = 2; v <= top; v++) {
            const int p = fgr::multiplier_permille(v, most);
            CHECK(fgr::multiplier_from_permille(p, most) == v, "multiplier permille round-trip");
        }
        CHECK(fgr::step_multiplier(2, -1, most) == 2, "multiplier left stops at 2x");
        CHECK(fgr::step_multiplier(top, 1, most) == top, "multiplier right stops at top");
        CHECK(fgr::step_multiplier(2, 1, most) == 3, "multiplier right steps 2x -> 3x");
    }

    if (failures) return 1;
    std::puts("PASS: stepped sliders, commit-on-release, dynamic resolution toggle, and framegen mode split");
    return 0;
}
'''

DRAG_INTEGRATION = r'''
namespace drag_integration {
namespace ur = mkwvk::upscaler_rows;
int cur_tab = 0, drag_tab = 0, drag_visible_row = 0;
int resolution = 3, sharpness = 40, up = ur::FSR3, last_fixed = 3;
struct Row { int* value; };
Row rows[] = {{&resolution}, {&sharpness}};
mkwvk::menu_hit::DragState drag_state;
bool row_shown(int i) { return i != 0 || !ur::dynamic_resolution_on(up, resolution); }
int tab_row_count(int) { return row_shown(0) ? 2 : 1; }
int tab_row_to_global(int, int local) { return row_shown(0) ? local : 1; }
/* VALID */
struct Pointer { int held = 0, pressed = 0; } ptr;
enum { HOS_MOUSE_LEFT = 1, HOS_MOUSE_RIGHT = 2 };
bool tab_left = false, tab_right = false, press[6] = {}, changed = false;
int mx = 0, commits = 0, saves = 0;
struct Hits { int drag(float, int) { return 1000; } } hits;
void save() { ++saves; }
void row_sync(Row&) {}
int slider_current_value(Row& r) { return *r.value; }
int slider_value_for(Row&, int) { return 9; }
void set_row(Row& r, int v, int, bool) { *r.value = v; ++commits; }
void tick() {
    /* GUARD */
    /* DRAG */
}
void test() {
    drag_state.begin(0, 9, resolution, true);
    ptr.held = HOS_MOUSE_LEFT;
    tick();
    CHECK(drag_state.active() && resolution == 3 && commits == 0, "drag previews without committing");
    // The bridge enables Dynamic resolution between the mouse press and release.
    resolution = ur::toggle_dynamic_resolution(up, resolution, true, &last_fixed, &up);
    ptr.held = 0;
    tick();
    CHECK(!drag_state.active() && resolution == 0 && sharpness == 40 && commits == 0 && saves == 0,
          "hiding the dragged row cancels instead of committing to the next row");
    resolution = 3;
    drag_state.begin(0, 9, resolution, true);
    cur_tab = 1;
    tick();
    CHECK(!drag_state.active() && commits == 0, "changing tabs cancels the drag");
    cur_tab = 0;
    drag_state.begin(0, 9, resolution, true);
    tick();
    CHECK(!drag_state.active() && resolution == 9 && sharpness == 40 && commits == 1,
          "a visible dragged row commits on release");
}
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()

    # Check required headers exist before compiling so failures on pre-PR main are concise and clear
    for rel in ("gpu/vk/vk_menu_hit.h", "gpu/vk/upscaler_rows.h", "gpu/vk/framegen_rows.h"):
        if not (ROOT / rel).is_file():
            raise SystemExit("FAIL: missing %s (PR B stepped-slider helpers not implemented)" % rel)

    # Check menu module row declarations
    rows = {r["key"].strip('"'): r for r in module_rows(ROOT) if "key" in r}
    for key, kind, commit in (
        ("dynamic_resolution", "menu::Kind::Toggle", None),
        ("resolution", "menu::Kind::Slider", "true"),
        ("frame_generation_multiplier", "menu::Kind::Slider", "true"),
    ):
        row = rows.get(key)
        if not row:
            raise SystemExit("FAIL: missing menu row %r" % key)
        if row.get("kind") != kind:
            raise SystemExit("FAIL: row %r kind is %r, expected %r" % (key, row.get("kind"), kind))
        if commit is not None and row.get("commit_on_release") != commit:
            raise SystemExit("FAIL: row %r commit_on_release is %r, expected %r" % (key, row.get("commit_on_release"), commit))

    # Check vk_menu.cpp wiring regressions: slider_cell text fitting, row_sync in bridge/options, drag cancel on Esc
    vk_menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    from srccut import block  # noqa: E402
    slider_cell_fn = block(vk_menu, "void slider_cell(", where="gpu/vk/vk_menu.cpp")
    if "fit(" not in slider_cell_fn:
        raise SystemExit("FAIL: slider_cell() does not call fit() (long Render resolution labels would overflow)")
    for fn_sig in ("void bridge_fill(", "int menu_setting_set(", "void draw_options("):
        if "row_sync(" not in block(vk_menu, fn_sig, where="gpu/vk/vk_menu.cpp"):
            raise SystemExit("FAIL: %s does not call row_sync() (UI mirror rows would be stale)" % fn_sig)
    if "drag_state.cancel()" not in block(vk_menu, "void menu_tick(", where="gpu/vk/vk_menu.cpp"):
        raise SystemExit("FAIL: menu_tick() does not cancel drag_state")

    # Compile the real drag validation and mouse handling with changing row visibility.
    tick = block(vk_menu, "void menu_tick(", where="gpu/vk/vk_menu.cpp")
    integration = DRAG_INTEGRATION.replace("/* VALID */", block(vk_menu, "bool drag_row_valid(", where="gpu/vk/vk_menu.cpp"))
    integration = integration.replace("/* GUARD */", block(tick, "if (drag_state.active() && (!drag_row_valid()", where="menu_tick"))
    integration = integration.replace("/* DRAG */", block(tick, "if (drag_state.active()) {   // (a slider held", where="menu_tick"))

    with tempfile.TemporaryDirectory(prefix="menu-stepped-sliders-") as tmp:
        src = Path(tmp) / "stepped_sliders.cpp"
        exe = Path(tmp) / ("stepped_sliders.exe" if os.name == "nt" else "stepped_sliders")
        src.write_text(CPP_TEST.replace("/* DRAG_INTEGRATION */", integration), encoding="utf-8")
        if Path(args.cxx).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
            cmd = [args.cxx, "/nologo", "/std:c++20", "/EHsc", "/I" + str(ROOT), str(src),
                   "/Fo" + str(Path(tmp) / "stepped_sliders.obj"), "/Fe" + str(exe)]
        else:
            cmd = [args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT), str(src), "-o", str(exe)]
        subprocess.run(cmd, cwd=tmp, check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
