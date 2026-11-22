"""Exercise real scripted parsing, routing and Npad serialization with authored files/API spies.

The parser, LIVE reader, input routing and Npad writer are extracted unmodified from production. Clock,
foreground-window, keyboard, mouse and pad APIs are fakes; no real device, desktop or game is accessed.
Each case starts a fresh process because production caches environment settings. Frame-clock cases retain holds
through a seven-second wall pause, test native-frame boundaries/live offsets/slot reuse, and keep default/0/empty controls.

Controllers counted only while the game's window was in front, so a game streamed from a screen of its own (Sunshine
and Moonlight), or left running behind another window, ignored the pad. The Insert menu's "Controller in background"
row, or HOS_INPUT_BACKGROUND=1, lets the pads through; the keyboard and mouse stay with the window in front.

menu-side-buttons: while one of the game's menus has the mouse, its side buttons stay their controls' (Mouse 4 on A
confirms a choice, Longshot #1306); the left button stays that menu's click.

held-input: an input held down is the player's input (the game menus' cursor hides by it) when it goes down, not at every
read while it stays down, so the mouse's hover works while ZL is held for the ocarina's song sheet (Longshot #1253).

Every Npad state is checked to carry the buttons alone, with no stick direction bits (16-23) whatever the sticks: with
them, the left stick pushed forward (bit 17) reached the game's controllers as a held D-pad bit, and A pressed while
running forward did not roll (sprint's cases: A with the stick fully forward). stick-directions: each stick pushed all the
way in each direction and diagonal with A held, and the D-pad's four with the stick forward: the buttons alone, and the
D-pad still reaches the state.

python -I -B tests/test_input_background.py [--cc gcc]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from menu_model_fixture import menu_text  # noqa: E402
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402
from compiler_command import compiler_command, parse_compiler  # noqa: E402

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/longshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "longshot_env.h")

RUN_ALL_ARGS = ["--cc", "{cc}"]

ROOT = Path(__file__).resolve().parents[1]
CASES = ("foreground", "script-background", "script-background-switch", "headless", "noactivate", "live-background",
         "no-script-background", "exclusive-foreground", "exclusive-background", "pads-env", "pads-menu", "pads-off",
         "frame-hold", "live-frame", "reuse-frame", "wall", "zero", "empty", "hold-toggle", "hold-mouse", "hold-background", "walk-toggle",
         "hold-taps", "menu-side-buttons", "deck-mouse", "deck-motion", "held-input", "sprint-toggle", "stick-directions")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default=None)
    p.add_argument("--case", choices=CASES)
    a = p.parse_args()
    hid = (ROOT / "host" / "nn_hid.c").read_text(encoding="utf-8")
    replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
    menu = menu_text()   # (vk_menu.cpp and its modules' rows, #1633)

    read = block(hid, "static void read_input(")
    behind = read[read.index("if (!input_in_front)"):]
    behind = behind[:behind.index("goto done;") + len("goto done;")]
    if "pads_in_background(" not in behind or "pads_read(" not in behind:
        raise SystemExit("behind another window the pads are never read")
    if "key(" in behind or "mouse" in behind:
        raise SystemExit("behind another window the keyboard or mouse would reach the game")
    option = replay[replay.index("t.option = "):]
    option = option[:option.index("};") + 2]
    if "id == 4 ? opts.pad_background" not in option:
        raise SystemExit("the host cannot see the menu's row (option 4)")
    if '"Controller in background", &opts.pad_background' not in menu:
        raise SystemExit("the Insert menu has no row for it")

    api = (ROOT / "gpu/vk/vk_api.h").read_text(encoding="utf-8")
    constants = block(hid, "enum { B_A") + ";\n" + block(api, "enum { HOS_SW_A") + ";\n"
    constants += block(hid, "enum { B_SPRINT") + ";\n"   # (a script's SPRINT: Sprint's key)
    constants += block(hid, "enum { SPRINT_BLOCKED") + ";\n"   # (why Sprint takes no press: host/oot_sprint.c's)
    constants += block(api, "enum { HOS_PAD_SOUTH") + ";\n"   # (Sprint's gamepad input)
    constants += block(api, "enum { HOS_KEY_MOVE_UP") + ";\n"   # (the keys-only controls, and Sprint)
    constants += block(api, "enum { HOS_OPT_CHEAT_HEARTS") + ";\n"
    constants += block(api, "enum { HOS_OPT_HOLD_TOGGLE") + ";\n"
    constants += block(api, "enum { HOS_OPT_SPRINT = ") + ";\n"   # (the Controls tab's Sprint row: the fixture's rx_option says on)
    constants += block(api, "enum { HOS_OPT_SPRINT_TOGGLE") + ";\n"   # (Sprint's Toggle: the fixture's rx_option says off)
    constants += block(api, "enum { HOS_BIND_PAD") + ";\n"   # (the live input's !bind)
    constants += block(api, "enum { HOS_OPT_WALK_MODE") + ";\n"   # (the Walk key's rows: read_input asks for them)
    for name in ("STATE_SIZE", "NPAD_NO1", "ATTR_CONNECTED"):
        matches = re.findall(r"^#define " + name + r"\s+[^\n]+", hid, re.M)
        assert len(matches) == 1, name
        constants += matches[0] + "\n"
    start = hid.index("typedef struct { int64_t start_ms, end_ms;")
    script = block(hid, "static void scripted_input(")
    end = hid.index(script) + len(script)
    cheat_state = re.search(r"^static uint64_t flight_input;\nstatic uint64_t free_camera_axes;\nstatic int free_camera_height;\n"
                            r"static int move_stick_held;[^\n]*\nstatic int sprint_now;", hid, re.M).group(0)
    cheat_state += "\n" + block(hid, "enum { SPRINT_KEY") + ";\n"   # (Sprint; its gamepad input leaves the HUD shortcut's out)
    cheat_state += "static uint64_t hud_pad_mask(void);\n" + block(hid, "static int pad_sprint_down(")
    # (Sprint on for the input, as the row's layout publishes it: the fixture's rx_option says the row is on)
    cheat_state += "\nstatic volatile LONG sprint_on = 1, sprint_toggle_mode;\n"
    # (npad_states hands Sprint's input to host/oot_sprint.c hos_sprint_input, which the game's buttons pass through:
    # tests/test_sprint.py runs it against a fake game; here a spy notes what it is handed)
    cheat_state += """
static int spy_sprint_on, spy_sprint_down, spy_sprint_l3, spy_sprint_toggle, spy_sprint_reset, spy_sprint_blocked;
uint64_t hos_sprint_input(uint64_t buttons, int on, int down, int l3, int toggle, int reset, int blocked) {
    spy_sprint_on = on, spy_sprint_down = down, spy_sprint_l3 = l3, spy_sprint_toggle = toggle;
    spy_sprint_reset = reset, spy_sprint_blocked = blocked;
    return buttons;
}
"""
    cheat_spies = """
volatile int hos_oot_live_action;
int hos_oot_flight_shortcut_ready(void) { CHECK(!"unexpected cheat readiness query"); return 0; }
void rx_option_set(int id, int value) { (void)id; (void)value; CHECK(!"unexpected cheat option change"); }
void hos_oot_autoload_input(void) {}
void hos_gameplay_hud_toggle(void) { CHECK(!"unexpected HUD toggle with the shortcut unbound"); }
"""
    # (the host's own stick, for the game's menus' mouse (host/oot_menu.c): none here; the player's input time it hides
    # the mouse by: not looked at. Its presses, hos_hid_tap and host_taps, are production's, below)
    host_spies = """
static void host_stick_read(int32_t stick[4]) { (void)stick; }
static void host_drag_read(int32_t stick[4]) { (void)stick; }
static void player_used(void) { player_stamps++; }
"""
    actual = "\n".join(("static HoldToggle toggles;", cheat_state, cheat_spies, host_spies, block(hid, "static int key("), block(hid, "static int kmap_down("),
        block(hid, "static int game_in_front("), hid[start:end],
        re.search(r"^static SRWLOCK tap_lock = SRWLOCK_INIT;\nstatic char tap_text\[256\];", hid, re.M).group(0),
        block(hid, "void hos_hid_tap("), block(hid, "static void host_taps(void) {"), block(hid, "static void stick_max("),
        block(hid, "static int pads_in_background("),
        # (the bound keys' helpers read_input calls: a mouse button counts only while the mouse is captured)
        block(hid, "static int mouse_vk_slot("), re.search(r"^static int mouse_armed\[5\];", hid, re.M).group(0),
        block(hid, "static void mouse_buttons_arm("), block(hid, "static int bound_down("),
        block(hid, "static uint64_t hud_pad_mask("), block(hid, "static void hud_shortcut("),
        block(hid, "static void oc_notes("), block(hid, "static uint64_t oc_pad_word("),
        # (walking on the keyboard: the movement keys' tilt, read_input asks for it)
        block(hid, "static int walk_held("), block(hid, "enum { WALK_SPEED_DEFAULT") + ";",
        block(hid, "static int walk_percent("), block(hid, "static void move_keys_stick("),
        block(hid, "static int flight_jump_shortcut("), read,
        block(hid, "static void npad_states(")))
    glue = (ROOT / "host/mkw_glue.c").read_text(encoding="utf-8")
    actual += "\n" + "\n".join(block(glue, "uint64_t " + name + "(")
                                  for name in ("hos_title_input", "hos_frame_step_buttons"))
    env = os.environ.copy()
    for name in list(env):
        if name.startswith("HOS_INPUT_") or name in ("HOS_HEADLESS", "HOS_WINDOW_NOACTIVATE"):
            del env[name]
    flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS if os.name == "nt" else 0
    def run(command, environment=None):
        result = subprocess.run(command, env=environment, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", creationflags=flags)
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        result.check_returncode()
    with tempfile.TemporaryDirectory(prefix="input-background-") as tmp:
        folder = Path(tmp).resolve()
        assert folder.parent == Path(tempfile.gettempdir()).resolve() and folder.name.startswith("input-background-")
        (folder / "input_constants.inc").write_text(constants, encoding="utf-8")
        (folder / "input_actual.inc").write_text(actual, encoding="utf-8")
        exe = folder / "fixture.exe"
        # Production's button table deliberately leaves its unused axis fields zero-initialized.
        cc = parse_compiler(a.cc) if a.cc else compiler_command("CC", "gcc", "clang")
        if not cc or not cc[0]:
            raise SystemExit("missing C compiler (set CC or install gcc or clang)")
        run(cc + ["-std=c11", "-include", ENV_ON_H, "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
             "-D_CRT_SECURE_NO_WARNINGS",
             "-I" + str(folder), str(ROOT / "tests/input_background_test.c"), "-o", str(exe)])
        for case in ((a.case,) if a.case else CASES):
            case_env = dict(env)
            if case not in ("live-background", "no-script-background", "live-frame", "reuse-frame", "hold-toggle", "hold-mouse", "hold-background", "walk-toggle", "hold-taps", "menu-side-buttons", "deck-mouse", "deck-motion", "held-input", "sprint-toggle", "stick-directions"):
                case_env["HOS_INPUT_SCRIPT"] = "0:A+LUP/200"
            if case in ("frame-hold", "live-frame", "reuse-frame"):
                case_env["HOS_INPUT_FRAME_CLOCK"] = "1"
            elif case in ("zero", "empty"):
                case_env["HOS_INPUT_FRAME_CLOCK"] = "0" if case == "zero" else ""
            if case.startswith("exclusive-") or case in ("frame-hold", "wall", "zero", "empty"):
                case_env["HOS_INPUT_EXCLUSIVE"] = "1"
            if case in ("pads-env", "pads-off"):
                case_env["HOS_INPUT_BACKGROUND"] = "1" if case == "pads-env" else "0"
            if case == "script-background-switch":   # (the player typing in another window: still kept out)
                case_env["HOS_INPUT_SCRIPT_BACKGROUND"] = "1"
            if case == "headless":
                case_env["HOS_HEADLESS"] = "1"
            if case == "noactivate":
                case_env["HOS_WINDOW_NOACTIVATE"] = "1"
            if case in ("live-background", "live-frame"):
                live = folder / (case + ".txt")
                live.write_bytes(b"")
                case_env["HOS_INPUT_LIVE"] = str(live)
            run([str(exe), case], case_env)
        assert folder.parent == Path(tempfile.gettempdir()).resolve() and folder.name.startswith("input-background-")


if __name__ == "__main__":
    main()
