"""The Insert menu's lines in longshot_options.ini (#1633, step 1): every saved row's key is written down in ini_key's table,
by the row's label, and the keys players' files hold stay as they are.

A key was made from the row's label, so renaming a row moved its setting to a new line and the player's value was lost (and a
control bridge script asking for the old name got nothing). Now ini_key looks the label up in its table. This check fails when
a saved row has no entry (a row added, or renamed without its entry following), when an entry names a label no row has, when two
rows share a key, when one of FROZEN's keys is gone or changed, when a key is new (add it to FROZEN on purpose), and when one of
ini_key's own cases by value gives another key than the table. FROZEN holds each key with the value its row saves: a key
that moves to another row (two rows' keys swapped) fails too, and so does an ini_key that stops giving a module row's own
key first. A row a module describes (gpu/vk/menu/*.cpp, #1633) gives
its key itself, .key: those count with the table's. No game, no compiler.

    python -I -B tests/test_menu_ini_keys.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# (the keys of 2026-11-18, main c507b98c, as players' files hold them: odd ones included, "autosave:" and "amd_anti-lag_2")
FROZEN = {   # (a key: the value its row saves, as players' files hold them)
    "amd_anti-lag_2": "&opts.antilag",
    "anisotropic_filtering": "&opts.anisotropy",
    "astc_as_rgba8": "&opts.astc_rgba8",
    "audio_output": "&opts.audio_output",
    "autosave:": "&opts.transition_autosave",
    "button_prompts": "&opts.prompts",
    "camera_speed": "&opts.camera_speed",
    "colour_vision": "&opts.colour_vision",
    "compact_menu_key": "&opts.compact_menu_key",
    "controller_in_background": "&opts.pad_background",
    "controller_menu_button": "&opts.pad_menu",
    "cpu_and_gpu_lines": "&opts.fps_dev_show[11]",
    "render_lines": "&opts.fps_dev_show[12]",
    "crt_filter": "&opts.crt_filter",
    "crt_lines": "&opts.crt_lines",
    "damage_taken": "&opts.damage_taken",
    "debug_tools": "&opts.debug_tools",
    "drift_correction": "&opts.input_replay_sync",
    "descriptor_reuse": "&opts.descriptor_reuse",
    "disable_camera_centering": "&opts.disable_camera_centering",
    "discord_status": "&opts.discord",
    "dispatches_line": "&opts.fps_dev_show[3]",
    "dlss": "&opts.dlss",
    "dlss_model": "&opts.dlss_model",
    "dlss_pre_bloom": "&opts.dlss_pre_bloom",
    "draws_line": "&opts.fps_dev_show[2]",
    "draws_skipped_line": "&opts.fps_dev_show[5]",
    "dualsense_hd_haptics": "&opts.ds_hd_haptics",
    "dynamic_resolution": "&dyn_res_ui",
    "field_of_view": "&opts.fov",
    "fps_counter": "&opts.show_fps",
    "fps_counter_position": "&opts.fps_corner",
    "fps_counter_size": "&opts.fps_size",
    "frame_generation": "&opts.framegen",
    "frame_generation_by": "&opts.framegen_by",
    "frame_generation_multiplier": "&fg_mult_ui",
    "frame_pacing": "&opts.pace",
    "frame_rate_cap": "&opts.fps_cap",
    "fsr4_model": "&opts.fsr4_model",
    "fullscreen": "&opts.fullscreen",
    "fullscreen_monitor": "&monitor_pick",
    "game_language": "&opts.game_lang",
    "game_speed": "&opts.realtime",
    "grass_distance": "&opts.grass_distance",
    "sim_turf_density": "&values[1]",
    "sim_turf_density_multiplier": "&multipliers[1]",
    "sim_turf_fade_start": "&values[0]",
    "sim_turf_fade_start_multiplier": "&multipliers[0]",
    "sim_turf_far_power": "&values[3]",
    "sim_turf_far_power_multiplier": "&multipliers[3]",
    "sim_turf_global_multipliers": "&global",
    "sim_turf_minimum_blades": "&values[2]",
    "sim_turf_minimum_blades_multiplier": "&multipliers[2]",
    "gpu_device": "&opts.gpu_device",
    "gpu_slot_waits_line": "&opts.fps_dev_show[8]",
    "grass_distance": "&opts.grass_distance",
    "handheld_profile": "&opts.handheld",
    "hdr": "&opts.hdr",
    "hdr_contrast": "&opts.hdr_contrast",
    "hdr_paper_white": "&opts.hdr_white",
    "hdr_peak_brightness": "&opts.hdr_peak",
    "hdr_tone_mapper": "&opts.hdr_tonemap",
    "hud_buttons_scale": "&opts.hud_buttons_scale",
    "hud_buttons_x": "&opts.hud_buttons_x",
    "hud_buttons_y": "&opts.hud_buttons_y",
    "hud_health_scale": "&opts.hud_health_scale",
    "hud_health_x": "&opts.hud_health_x",
    "hud_health_y": "&opts.hud_health_y",
    "hud_minimap_scale": "&opts.hud_minimap_scale",
    "hud_minimap_x": "&opts.hud_minimap_x",
    "hud_minimap_y": "&opts.hud_minimap_y",
    "hud_layout": "&opts.hud_layout",
    "intel_quick_compute_builds": "&opts.intel_quick",
    "intel_shader_compiles": "&opts.intel_compiles",
    "invert_camera_x": "&opts.invert_cam_x",
    "invert_camera_y": "&opts.invert_cam_y",
    "invert_motion_x": "&opts.gyro_inv_x",
    "invert_motion_y": "&opts.gyro_inv_y",
    "invert_mouse_horizontal": "&opts.mouse_invert_x",
    "invert_mouse_vertical": "&opts.mouse_invert",
    "layout_preset": "&opts.pad_layout",
    "menu_language": "&opts.menu_lang",
    "menu_shortcut": "&opts.menu_shortcut",
    "menu_size": "&opts.menu_size",
    "menu_theme": "&opts.menu_theme",
    "microphone": "&opts.mic",
    "microphone_gain": "&opts.microphone_gain",
    "motion_controls": "&opts.gyro",
    "motion_deadzone": "&opts.gyro_deadzone",
    "motion_sensitivity_x": "&opts.gyro_sens_x",
    "motion_sensitivity_y": "&opts.gyro_sens_y",
    "pad_speaker_volume": "&opts.pad_speaker_volume",
    "pad_speaker_sync": "&opts.pad_speaker_sync",
    "pad_speaker_headphones": "&opts.pad_speaker_headphones",
    "pad_speaker_headphone_volume": "&opts.pad_speaker_headphone_volume",
    "pad_speaker_navi_voice": "&opts.pad_speaker[0]",
    "pad_speaker_navi_sound": "&opts.pad_speaker[1]",
    "pad_speaker_target": "&opts.pad_speaker[2]",
    "pad_speaker_ocarina": "&opts.pad_speaker[3]",
    "pad_speaker_pickup": "&opts.pad_speaker[4]",
    "pad_speaker_item": "&opts.pad_speaker[5]",
    "pad_speaker_fairy": "&opts.pad_speaker[6]",
    "pad_speaker_ui": "&opts.pad_speaker[7]",
    "pad_speaker_agony": "&opts.pad_speaker[8]",
    "pad_speaker_low_health": "&opts.pad_speaker[9]",
    "pad_speaker_fishing": "&opts.pad_speaker[10]",
    "pad_speaker_bottle": "&opts.pad_speaker[11]",
    "mouse_camera": "&opts.mouse_native",
    "mouse_sensitivity": "&opts.mouse_sens",
    "mute_in_background": "&opts.mute_background",
    "nfc_tag": "&nfc_choice",
    "nvidia_reflex": "&opts.reflex",
    "output_device": "&output_pick",
    "output_res": "&opts.output_res",
    "pipelines_built_line": "&opts.fps_dev_show[7]",
    "present_queue": "&opts.present_queue",
    "quality_preset": "&opts.preset",
    "queue_submits_line": "&opts.fps_dev_show[9]",
    "render_passes_line": "&opts.fps_dev_show[4]",
    "reshade_preset": "&opts.reshade",
    "reshade_user_pick": "&user_pick",
    "resolution": "&opts.resolution",
    "robust_buffers": "&opts.robust",
    "rumble_strength": "&opts.rumble",
    "sampler_cache": "&opts.sampler_cache",
    "shadow_resolution": "&opts.shadow_res",
    "sharpness": "&opts.sharpness",
    "skip_opening": "&opts.skip_opening",
    "skip_title_screen": "&opts.skip_title",
    "sprint": "&opts.sprint",
    "stick_dead_zone": "&opts.stick_dead_zone",
    "text_size": "&opts.text_size",
    "text_speed": "&opts.text_speed",
    "texture_uploads_line": "&opts.fps_dev_show[6]",
    "textures_line": "&opts.fps_dev_show[1]",
    "threaded_rendering": "&opts.threaded_rendering",
    "ultrawide": "&opts.ultrawide",
    "ultrawide_menus": "&opts.wide_menus",
    "upscaling": "&opts.upscale",
    "version_and_hardware_at_every_level": "&opts.fps_version_always",
    "vibration": "&opts.vibration",
    "volume": "&opts.volume",
    "vram_line": "&opts.fps_dev_show[0]",
    "vsync": "&opts.vsync",
    "walk_mode": "&opts.walk_mode",
    "walk_speed": "&opts.walk_speed",
    "write-backs_line": "&opts.fps_dev_show[10]",
}


def fail(message):
    print("FAIL: " + message)
    sys.exit(1)


def module_saved():
    """[(key, value expression)] of the saved rows modules describe: each row with a .key and a .value"""
    sys.path.insert(0, str(ROOT / "tests"))   # (python -I leaves the check's own folder off the path)
    from menu_model_fixture import module_rows
    return [(r["key"].strip('"'), r["value"]) for r in module_rows(ROOT) if "key" in r and "value" in r]


def module_keys():
    """The keys of the saved rows modules describe"""
    return [key for key, _ in module_saved()]


def main():
    src = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    start = src.index("Row rows[] = {")
    table = re.sub(r"//[^\n]*", "", src[start:src.index("\n};", start)])
    rows = re.findall(r'\{\s*(TAB_\w+)\s*,\s*"((?:[^"\\]|\\.)*)"\s*,\s*(&[\w.]+|nullptr)', table)
    saved = [(label, value) for _, label, value in rows if value != "nullptr"]
    fn = src[src.index("const char* ini_key(const Row& r) {"):]
    fn = fn[:fn.index("\n}\n")]
    # (ini_key's table of labels, while the table had saved rows: none now, every saved row a module's with its own key)
    entries = re.findall(r'\{"((?:[^"\\]|\\.)*)", "([^"]+)"\}', fn[:fn.index("};")]) if "};" in fn else []
    by_value = dict(re.findall(r'if \(r\.value == (&[\w.]+)\) return "([^"]+)";', fn))
    if (saved or entries) and not (saved and entries) or not module_keys():
        fail("vk_menu.cpp: the rows table or ini_key's table was not found")
    labels = {label for label, _ in saved}
    table_labels = [label for label, _ in entries]
    missing = [label for label, value in saved if label not in table_labels]
    if missing:
        fail("saved rows with no key in ini_key's table (a row added, or renamed: keep its old key under the new label): %s" % missing)
    stale = [label for label in table_labels if label not in labels]
    if stale:
        fail("ini_key's table names labels no row has: %s" % stale)
    keys = [key for _, key in entries] + module_keys()
    doubled = sorted({key for key in keys if keys.count(key) > 1})
    if doubled:
        fail("two rows share a key: %s" % doubled)
    gone = sorted(set(FROZEN) - set(keys))
    if gone:
        fail("keys players' files hold are gone or changed (their values would be lost): %s" % gone)
    new = sorted(set(keys) - set(FROZEN))
    if new:
        fail("new keys: add them to FROZEN in this file, on purpose: %s" % new)
    # (#1774's review: a key stays with its row's value, so a swap of two rows' keys moves no player's setting; ini_key
    # gives a module row's own key before anything made from its label; its cases by value agree with the modules)
    for key, value in module_saved():
        if FROZEN.get(key, value) != value:
            fail("%s: its row saves %s, where players' files hold %s under it" % (key, value, FROZEN[key]))
    body = fn[fn.index("{") + 1:].lstrip()
    if not body.startswith("if (r.setting && r.setting->key) return r.setting->key;"):
        fail("ini_key: a module row's own key is not what it gives first (a renamed row would move its setting)")
    for key, value in module_saved():
        if value in by_value and by_value[value] != key:
            fail("%s: ini_key gives %s by its value, its module %s" % (value, by_value[value], key))
    table_of = dict(entries)
    for label, value in saved:
        if value in by_value and label in table_of and table_of[label] != by_value[value]:
            fail("%s: ini_key gives %s by its value and %s by its table" % (label, by_value[value], table_of[label]))
    print("PASS: %d saved rows, each with its key; %d keys as players' files hold them" % (len(saved) + len(module_keys()), len(FROZEN)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
