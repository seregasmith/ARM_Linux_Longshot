"""Which rows of the Insert menu stop at their ends and which go round (gpu/vk/menu/*.cpp, #1633).

The arrows (and a click on a value's half) step a row by its step; at its last value a Choice goes round to its first,
and a scale stops there: a Slider (the model's rule, menu_model.cpp step) or a row with stops_at_ends. Sharpness is a
slider that has always gone round, 100% to 0%: it says so with wraps. The rows that stop are the ones that stopped
before the menu was a model (vk_menu.cpp's stops_at_ends list then: Microphone gain, Aspect ratio, Mouse sensitivity,
Vibration strength, Motion sensitivity X and Y, Volume, Camera speed, Field of view), and the ImGui menu and the model
both follow the rows' own flags. The Custom HUD position and size sliders stop at their limits too, as do the
Simulated turf page's sliders (#2093): an area's four values and their four multipliers.

    python -I -B tests/test_menu_slider_ends.py
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from menu_model_fixture import module_rows  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
STOPS = {"microphone_gain", "pad_speaker_volume", "pad_speaker_sync", "pad_speaker_headphone_volume",
         "ultrawide", "mouse_sensitivity", "rumble_strength", "motion_sensitivity_x",
         "motion_sensitivity_y", "volume", "camera_speed", "field_of_view",
         "resolution", "frame_generation_multiplier",
         "hud_health_x", "hud_health_y", "hud_health_scale", "hud_buttons_x", "hud_buttons_y", "hud_buttons_scale",
         "hud_minimap_x", "hud_minimap_y", "hud_minimap_scale",
         "sim_turf_fade_start", "sim_turf_density", "sim_turf_minimum_blades", "sim_turf_far_power",
         "sim_turf_fade_start_multiplier", "sim_turf_density_multiplier", "sim_turf_minimum_blades_multiplier",
         "sim_turf_far_power_multiplier"}
WRAPS = {"sharpness"}


def fail(why):
    raise SystemExit("FAIL: " + why)


def main():
    rows = module_rows(ROOT)
    slider = lambda r: r.get("kind") == "menu::Kind::Slider"
    stops = {r["key"].strip('"') for r in rows
             if (slider(r) and r.get("wraps") != "true") or r.get("stops_at_ends") == "true"}
    wraps = {r["key"].strip('"') for r in rows if slider(r) and r.get("wraps") == "true"}
    if stops != STOPS:
        fail("rows that stop at their ends: %s more, %s fewer than before" % (sorted(stops - STOPS), sorted(STOPS - stops)))
    if wraps != WRAPS:
        fail("sliders that go round: %s, not %s" % (sorted(wraps), sorted(WRAPS)))
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    line = re.search(r"^bool stops_at_ends\(const Row& r\) \{.*\}$", menu, re.M)
    if not line or "!r.setting->wraps" not in line.group(0) or "r.setting->stops_at_ends" not in line.group(0):
        fail("vk_menu.cpp: stops_at_ends does not follow the row's wraps and stops_at_ends")
    model = (ROOT / "gpu/vk/menu_model.cpp").read_text(encoding="utf-8")
    if "if ((s.kind == Kind::Slider && !s.wraps) || s.stops_at_ends) return std::clamp(next, s.lo, s.hi);" not in model:
        fail("menu_model.cpp: a step does not hold a Slider at its ends unless it wraps")
    print("PASS: %d rows stop at their ends; Sharpness goes round" % len(stops))


if __name__ == "__main__":
    main()
