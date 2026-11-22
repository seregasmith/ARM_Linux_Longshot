"""Sprint (host/nn_hid.c, the Controls page's line, the Controls tab's Sprint row, gpu/vk/menu/sprint.cpp, host/oot_sprint.c): a
control past the Switch's buttons with keys and gamepad inputs that sets the game's dash input in Link's flags, the game's own
run after a roll, while held or from a press to the next with its Toggle. Off by default.

The bindings: host/nn_hid.c's binding table is compiled alone (C), as tests/test_bound_keys.py does, and linked with the
Sprint module's apply_sprint_layout cut out of gpu/vk/menu/sprint.cpp (C++), against a fake longshot_options.ini for each case. A new
install's defaults are the ones from before Sprint (Shift B, the left stick's press LS) and give Sprint nothing; the row
turned on gives it Shift and the press (B to V, LS keeping its other input) and turned off puts them back; a player who
rebound B, Shift or the press, or whose V is taken, or who bound Sprint, keeps theirs. The sources are checked for the
rest: read_input reads Sprint's key and gamepad input (and a test run's SPRINT, which never reaches the game), npad_states hands host/oot_sprint.c its input only with the row on, the
module's row is on the Controls tab and lays the controls out as it changes, the Controls page has Sprint's line, writes
its pad_ line and lays them out again as it resets.
host/oot_sprint.c is compiled alone against a fake game (Link's state chain, the actor and event managers): the dash input
while held or toggled, ended by Link's state, ZL, standing still (by the frames' time) or the host's reset, kept through
a roll; A never added; in a conversation the left stick's press handed to the game as LS. nx2/oot_patches.py's two calls
after the loads of Link's input flags run on text written as the lift writes it.

Physical review: compare ae8e78b0910d with the updated head using an Xbox controller and keyboard on Windows and a
controller on Linux or Steam Deck. With Sprint Off and On, release Sprint while steering, press A during Sprint and press the left stick in a conversation;
capture menu screenshots before and after. Report the Linux build, applicable Tier 1 coverage, lint and CI, and name
any suite failures also present on the baseline. This fixture does not exercise physical L3 or either platform's input.
python -I -B tests/test_sprint.py   (needs a C and a C++ compiler)
"""
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from compiler_command import compiler_command, windows_arguments  # noqa: E402
import test_bound_keys  # noqa: E402  (its PRELUDE and the helpers that cut code out of a source)




MAIN = r"""
void sprint_layout(int on);   /* (the Sprint module's apply_sprint_layout, layout.cpp) */
int sprint_moved_while_on(void);   /* (layout.cpp: bindings it moved while the input could see Sprint on) */
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s line %d: %s\n", which, __LINE__, #c); fails++; } } while (0)
#define KEY(b) hos_binding(HOS_BIND_KEY, b, 0)
#define KEY2(b) hos_binding(HOS_BIND_KEY, b, 1)
#define PAD(b) hos_binding(HOS_BIND_PAD, b, 0)
#define PAD2(b) hos_binding(HOS_BIND_PAD, b, 1)
#define S HOS_KEY_SPRINT
int main(int argc, char **argv) {
    (void)argc;
    const char *which = argv[1];
    binds_load();
    if (!strcmp(which, "new")) {
        /* the defaults are those from before Sprint: it has nothing */
        CHECK(KEY(S) == 0 && KEY2(S) == 0 && PAD(S) == -1 && PAD2(S) == -1);
        CHECK(KEY(HOS_SW_B) == VK_SHIFT && PAD(HOS_SW_LS) == HOS_PAD_LSTICK);
        CHECK(hos_binding(HOS_BIND_SPRINT_ON, 0, 0) == 0);   /* (no sprint= line: off) */
        sprint_layout(1);
        CHECK(KEY(S) == VK_SHIFT && PAD(S) == HOS_PAD_LSTICK);
        CHECK(KEY(HOS_SW_B) == 'V' && PAD(HOS_SW_B) == HOS_PAD_SOUTH);
        CHECK(PAD(HOS_SW_LS) == -1 && PAD2(HOS_SW_LS) == -1 && KEY(HOS_SW_LS) == VK_CONTROL);
        CHECK(hos_binding(HOS_BIND_SPRINT_ON, 0, 0) == 1);   /* (on, the layout whole) */
        sprint_layout(1);   /* (on again: nothing moves) */
        CHECK(KEY(S) == VK_SHIFT && KEY(HOS_SW_B) == 'V' && PAD(S) == HOS_PAD_LSTICK && PAD(HOS_SW_LS) == -1);
        sprint_layout(0);
        CHECK(hos_binding(HOS_BIND_SPRINT_ON, 0, 0) == 0);
        CHECK(KEY(S) == 0 && KEY2(S) == 0 && PAD(S) == -1 && PAD2(S) == -1);
        CHECK(KEY(HOS_SW_B) == VK_SHIFT && PAD(HOS_SW_LS) == HOS_PAD_LSTICK);
        CHECK(!KEY2(HOS_SW_B));   /* (V gone with it) */
        /* a gamepad input bound to Sprint trades places with the Switch button that had it */
        sprint_layout(1);
        hos_bind(HOS_BIND_PAD, S, 0, HOS_PAD_SOUTH);
        CHECK(PAD(S) == HOS_PAD_SOUTH && PAD(HOS_SW_B) == HOS_PAD_LSTICK);
        hos_bind(HOS_BIND_KEY, S, 1, 'E');   /* a key bound to Sprint is taken from the button that had it */
        CHECK(KEY2(S) == 'E' && KEY(HOS_SW_A) == 0);
        hos_bind(HOS_BIND_PAD, HOS_KEY_MOVE_UP, 0, HOS_PAD_NORTH);   /* no gamepad input for a keys-only control */
        CHECK(PAD(HOS_KEY_MOVE_UP) == -1 && PAD(HOS_SW_X) == HOS_PAD_NORTH);
        hos_bind(HOS_BIND_KEY, -1, 0, 0);   /* Reset to defaults: Sprint's go (the menu then lays them out again) */
        CHECK(KEY(S) == 0 && PAD(S) == -1 && KEY(HOS_SW_B) == VK_SHIFT && PAD(HOS_SW_LS) == HOS_PAD_LSTICK);
    } else if (!strcmp(which, "ls-two")) {   /* LS has a second gamepad input: it keeps it */
        sprint_layout(1);
        CHECK(PAD(S) == HOS_PAD_LSTICK && PAD(HOS_SW_LS) == HOS_PAD_RPADDLE1 && PAD2(HOS_SW_LS) == -1);
        sprint_layout(0);
        CHECK(PAD(S) == -1 && PAD(HOS_SW_LS) == HOS_PAD_LSTICK && PAD2(HOS_SW_LS) == HOS_PAD_RPADDLE1);
    } else if (!strcmp(which, "rebound")) {   /* B on the B key, Shift on X, LS on the right stick's press: all kept */
        sprint_layout(1);
        CHECK(KEY(HOS_SW_B) == 'B' && KEY(HOS_SW_X) == VK_SHIFT && KEY(S) == 0);
        CHECK(PAD(HOS_SW_LS) == HOS_PAD_RSTICK && PAD(HOS_SW_RS) == HOS_PAD_LSTICK && PAD(S) == -1);
        sprint_layout(0);
        CHECK(KEY(HOS_SW_B) == 'B' && KEY(HOS_SW_X) == VK_SHIFT && PAD(HOS_SW_RS) == HOS_PAD_LSTICK);
    } else if (!strcmp(which, "v-taken")) {   /* V already Y's: B keeps Shift, Sprint gets no key, the press still */
        sprint_layout(1);
        CHECK(KEY(HOS_SW_B) == VK_SHIFT && KEY2(HOS_SW_Y) == 'V' && KEY(S) == 0);
        CHECK(PAD(S) == HOS_PAD_LSTICK && PAD(HOS_SW_LS) == -1);
        sprint_layout(0);
        CHECK(KEY(HOS_SW_B) == VK_SHIFT && KEY2(HOS_SW_Y) == 'V' && PAD(HOS_SW_LS) == HOS_PAD_LSTICK);
    } else if (!strcmp(which, "own")) {   /* Sprint bound by the player: the row leaves everything as it is */
        CHECK(KEY(S) == 'G' && PAD(S) == HOS_PAD_LPADDLE1);
        sprint_layout(1);
        CHECK(KEY(S) == 'G' && PAD(S) == HOS_PAD_LPADDLE1 && KEY(HOS_SW_B) == VK_SHIFT && PAD(HOS_SW_LS) == HOS_PAD_LSTICK);
        sprint_layout(0);
        CHECK(KEY(S) == 'G' && PAD(S) == HOS_PAD_LPADDLE1 && KEY(HOS_SW_B) == VK_SHIFT && PAD(HOS_SW_LS) == HOS_PAD_LSTICK);
    } else if (!strcmp(which, "save-auto") || !strcmp(which, "save-user")) {
        sprint_layout(1);
        if (!strcmp(which, "save-user")) {
            /* Same-value rebinds transfer ownership to the player, then survive a restart. */
            hos_bind(HOS_BIND_KEY, S, 0, VK_SHIFT);
            hos_bind(HOS_BIND_PAD, S, 0, HOS_PAD_LSTICK);
        }
        FILE *out = fopen("longshot_options.ini", "w");
        CHECK(out != NULL);
        if (!out) return 1;
        fprintf(out, "sprint_auto_bind=%d\nkey_b=%d,0\nkey_sprint=%d,0\npad_ls=%d,%d\npad_sprint=%d\n",
                hos_binding(HOS_BIND_SPRINT_AUTO, 0, 0), KEY(HOS_SW_B), KEY(S), PAD(HOS_SW_LS), PAD2(HOS_SW_LS), PAD(S));
        fclose(out);
    } else if (!strcmp(which, "restore-auto")) {
        sprint_layout(0);
        CHECK(KEY(S) == 0 && KEY(HOS_SW_B) == VK_SHIFT);
        CHECK(PAD(S) == -1 && PAD(HOS_SW_LS) == HOS_PAD_LSTICK && PAD2(HOS_SW_LS) == HOS_PAD_RPADDLE1);
    } else if (!strcmp(which, "restore-user")) {
        sprint_layout(0);
        CHECK(KEY(S) == VK_SHIFT && KEY(HOS_SW_B) == 'V');
        CHECK(PAD(S) == HOS_PAD_LSTICK && PAD(HOS_SW_LS) == HOS_PAD_RPADDLE1 && PAD2(HOS_SW_LS) == -1);
    } else if (!strcmp(which, "rebind-source")) {
        sprint_layout(1);
        hos_bind(HOS_BIND_KEY, HOS_SW_B, 0, 'V');   /* the player now owns V on B */
        hos_bind(HOS_BIND_PAD, HOS_SW_LS, 0, HOS_PAD_RPADDLE1);
        sprint_layout(0);
        CHECK(KEY(S) == 0 && KEY(HOS_SW_B) == 'V');
        CHECK(PAD(S) == HOS_PAD_LSTICK && PAD(HOS_SW_LS) == HOS_PAD_RPADDLE1);
    } else if (!strcmp(which, "rebound-sprint")) {
        /* Sprint's key rebound by the player after the row moved Shift: theirs stays, and B, which they left alone, goes
           back to Shift, now free */
        sprint_layout(1);
        hos_bind(HOS_BIND_KEY, S, 0, 'G');
        sprint_layout(0);
        CHECK(KEY(S) == 'G' && KEY(HOS_SW_B) == VK_SHIFT);
        CHECK(PAD(S) == -1 && PAD(HOS_SW_LS) == HOS_PAD_LSTICK);
    } else if (!strcmp(which, "shift-taken")) {
        /* Sprint rebound to G, then Shift given to X by the player: B stays on V rather than take Shift from X */
        sprint_layout(1);
        hos_bind(HOS_BIND_KEY, S, 0, 'G');
        hos_bind(HOS_BIND_KEY, HOS_SW_X, 1, VK_SHIFT);
        sprint_layout(0);
        CHECK(KEY(S) == 'G' && KEY(HOS_SW_B) == 'V' && KEY2(HOS_SW_X) == VK_SHIFT);
    } else if (!strcmp(which, "saved-positive") || !strcmp(which, "saved-negative")) {
        CHECK(hos_binding(HOS_BIND_SPRINT_ON, 0, 0) == !strcmp(which, "saved-positive"));
        CHECK(KEY(S) == 'G');
    } else if (!strcmp(which, "own-defaults")) {
        sprint_layout(1);
        sprint_layout(0);
        CHECK(KEY(S) == VK_SHIFT && PAD(S) == HOS_PAD_LSTICK);
    } else {
        CHECK(!"no such case");
    }
    CHECK(sprint_moved_while_on() == 0);   /* (every layout move was inside the update) */
    if (!fails) printf("PASS %s\n", which);
    return fails != 0;
}
"""

# (the Sprint module's layout, cut out of gpu/vk/menu/sprint.cpp, with the host table above as its MkwVkHost)
LAYOUT = r"""
#include "vk_api.h"
extern "C" int hos_binding(int set, int button, int slot);
extern "C" void hos_bind(int set, int button, int slot, int code);
/* Every binding the layout moves must be inside the update. */
static int moved_while_on;
static bool updating;
static void bind_checked(int set, int button, int slot, int code) {
    if (set == HOS_BIND_SPRINT_BEGIN) updating = true;
    if ((set == HOS_BIND_KEY || set == HOS_BIND_PAD) && !updating) moved_while_on++;
    hos_bind(set, button, slot, code);
    if (set == HOS_BIND_SPRINT_ON) updating = false;
}
extern "C" int sprint_moved_while_on(void) { return moved_while_on; }
struct Host { int version; int (*binding)(int, int, int); void (*bind)(int, int, int, int); } host_value{39, hos_binding, bind_checked},
    *host = &host_value;
static bool can_bind() { return true; }
%s
extern "C" void sprint_layout(int on) { apply_sprint_layout(on != 0); }
"""

CASES = {
    "own-defaults": "key_sprint=16,0\npad_sprint=7\n",
    "new": None,
    "saved-positive": "sprint=2\nkey_sprint=71,0\n",
    "saved-negative": "sprint=-1\nkey_sprint=71,0\n",
    "ls-two": "pad_ls=7,17\n",
    "rebind-source": "pad_ls=7,17\n",
    "rebound": "key_b=66,0\nkey_x=16,0\npad_ls=8\npad_rs=7\n",
    "v-taken": "key_b=16,0\nkey_y=1,86\n",
    "own": "key_sprint=71,0\npad_sprint=18\n",
    "rebound-sprint": None,
    "shift-taken": None,
}

class SprintTests(unittest.TestCase):
    def test_windows_compiler_arguments(self):
        self.assertEqual(windows_arguments(r'clang -I"C:\Program Files\SDK"'),
                         ["clang", r"-IC:\Program Files\SDK"])
        self.assertEqual(windows_arguments(r'"C:\Program Files\LLVM\clang.exe" -DNAME="a b"'),
                         [r"C:\Program Files\LLVM\clang.exe", "-DNAME=a b"])
        self.assertEqual(windows_arguments(r'clang "C:\dir\\" "a\"b"'),
                         ["clang", "C:\\dir\\", 'a"b'])

    def test_sources(self):
        hid = (ROOT / "host" / "nn_hid.c").read_text(encoding="utf-8")
        read = test_bound_keys.block(hid, "static void read_input(")
        self.assertIn("bound_down(HOS_KEY_SPRINT, captured)", read, "read_input does not read Sprint's keys")
        self.assertEqual(read.count("sprint_now = pad_sprint_down();"), 2,
                         "read_input does not read Sprint's gamepad input, in front and behind another window")
        npad = test_bound_keys.block(hid, "static void npad_states(")
        enabled = "sprint_frame = (int)__atomic_load_n(&sprint_on, __ATOMIC_ACQUIRE);"
        self.assertLess(read.index("binds_load();"), read.index("AcquireSRWLockShared(&input_bind_lock);"))
        self.assertLess(read.index("AcquireSRWLockShared(&input_bind_lock);"), read.index(enabled))
        self.assertLess(read.index(enabled), read.index("bound_down(HOS_KEY_SPRINT, captured)"))
        self.assertLess(read.index("ReleaseSRWLockShared(&input_bind_lock);"), read.index("hos_oot_autoload_input();"))
        self.assertLess(read.index("input_pads[i][k] = pad_bind[i][k]"), read.index("ReleaseSRWLockShared(&input_bind_lock);"))
        self.assertLess(npad.index("read_input(&buttons, &taps, stick);"), npad.index("const int sprint_enabled = sprint_frame;"))
        self.assertIn("const int sprint_down = (sprint_enabled ? sprint_now : 0) | (sprint_enabled && synthetic_sprint ? SPRINT_KEY : 0);",
                      npad, "Sprint runs without the Controls tab's Sprint row on")
        self.assertIn("buttons = hos_sprint_input(buttons, sprint_enabled,", npad,
                      "npad_states does not give Sprint the buttons as the game will get them")
        # (a script's or the bridge's SPRINT: Sprint's key for a test run, never a button the game or the cheats see)
        self.assertIn('{"SPRINT", B_SPRINT}', test_bound_keys.block(hid, "static int parse_presses(const char *s, int64_t base_ms, int live) {"),
                      "HOS_INPUT_SCRIPT has no SPRINT")
        control = (ROOT / "host" / "control.c").read_text(encoding="utf-8")
        self.assertIn('{"SPRINT", 48, 0, 0}', control, "the control bridge has no SPRINT")
        self.assertIn("enum { B_SPRINT = 48 };", hid, "the bridge's SPRINT bit is not nn_hid.c's")
        self.assertIn("& ~(pad_after_menu | hud_pad_mask());", test_bound_keys.block(hid, "static int pad_sprint_down("),
                      "Sprint's gamepad input fires with the HUD shortcut's, which owns its inputs")
        took = npad.index("const int synthetic_sprint = buttons >> B_SPRINT & 1;")
        self.assertLess(npad.index("read_input(&buttons, &taps, stick);"), took)
        self.assertLess(took, npad.index("buttons &= ~(1ULL << B_SPRINT);"))
        self.assertLess(npad.index("buttons &= ~(1ULL << B_SPRINT);"), npad.index("hos_hid_buttons = buttons;"),
                        "SPRINT reaches the game's buttons or the cheats")
        menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
        module = (ROOT / "gpu" / "vk" / "menu" / "sprint.cpp").read_text(encoding="utf-8")
        row = test_bound_keys.block(module, "MENU_MODULE(sprint)")
        self.assertIn('menu::tab("controls")', row, "the Sprint row is not on the Controls tab")
        self.assertIn('.key = "sprint"', row, "the Sprint row does not save as sprint=")
        self.assertIn("changes your controls", row, "the Sprint row does not warn that it changes the controls")
        self.assertNotIn('"Sprint", &opts.sprint', menu, "the Sprint row is in vk_menu.cpp's table, not its module")
        self.assertIn("int sprint = 0;", (ROOT / "gpu" / "vk" / "vk_menu.h").read_text(encoding="utf-8"),
                      "Sprint is not off by default")
        self.assertIn(".on_change = MENU_HOOK(sprint_chosen)", row, "turning the row on or off does not lay out the controls")
        self.assertIn("void sprint_chosen(int v) { apply_sprint_layout(v != 0); }", module,
                      "the row's on_change does not lay out the controls")
        self.assertIn("if (opts.sprint) apply_sprint_layout(true);", menu, "Reset to defaults leaves Sprint with nothing")
        replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
        self.assertIn("if (id == HOS_OPT_SPRINT) return opts.sprint;", replay, "the host cannot see the Sprint row")
        names = test_bound_keys.block(hid, "static const char *const bind_names[HOS_KEY_N]")
        menu_names = test_bound_keys.block(menu, "const char* const bind_names[HOS_KEY_N]")
        self.assertEqual(re.findall(r'"(\w+)"', names), re.findall(r'"(\w+)"', menu_names),
                         "the menu writes other names than host/nn_hid.c reads")
        self.assertEqual(re.findall(r'"(\w+)"', names)[-1], "sprint", "Sprint's lines are not pad_sprint and key_sprint")
        self.assertIn('{HOS_KEY_SPRINT, "Sprint"}', menu, "the Controls page has no Sprint row")
        self.assertIn("i == HOS_KEY_SPRINT) {\n                const int alternate", menu, "the menu does not write pad_sprint")
        api = (ROOT / "gpu" / "vk" / "vk_api.h").read_text(encoding="utf-8")
        self.assertIn("HOS_KEY_TOGGLE_HUD, HOS_KEY_SPRINT, HOS_KEY_N };", api,
                      "Sprint is not the control after the HUD shortcut")

    def test_bindings(self):
        cc = compiler_command("CC", "gcc", "clang")
        cxx = compiler_command("CXX", "g++", "clang++")
        if not cc or not cxx or not cc[0] or not cxx[0]:
            self.skipTest("no C and C++ compiler")
        hid = (ROOT / "host" / "nn_hid.c").read_text(encoding="utf-8")
        code = test_bound_keys.between(hid, "static const char *const bind_names[HOS_KEY_N]", "void hos_bind(int set, int button, int slot, int code) {")
        code = code.replace("GetModuleFileNameA(NULL,", "GetModuleFileNameA(0,")
        module = (ROOT / "gpu" / "vk" / "menu" / "sprint.cpp").read_text(encoding="utf-8")
        layout = test_bound_keys.block(module, "void apply_sprint_layout(")
        include = ["-I", str(ROOT / "gpu" / "vk"), "-I", str(ROOT / "gpu")]
        with tempfile.TemporaryDirectory(prefix="longshot-sprint-") as tmp:
            tmp = Path(tmp)
            prelude = test_bound_keys.PRELUDE
            start = prelude.index("static const char *hos_options_file(void)")
            end = prelude.index("static int held[256];", start)
            prelude = prelude[:start] + 'static const char *hos_options_file(void) { return "longshot_options.ini"; }\n' + prelude[end:]
            (tmp / "t.c").write_text(prelude + code + MAIN, encoding="utf-8")
            (tmp / "layout.cpp").write_text(LAYOUT % layout, encoding="utf-8")
            binary = tmp / ("t.exe" if sys.platform == "win32" else "t")
            for command in ([*cc, "-std=gnu11", "-Wall", "-Wno-unused-function", *include, "-c", str(tmp / "t.c"),
                             "-o", str(tmp / "t.o")],
                            [*cxx, "-std=c++17", "-Wall", "-Wextra", *include, "-c", str(tmp / "layout.cpp"),
                             "-o", str(tmp / "layout.o")],
                            [*cxx, str(tmp / "t.o"), str(tmp / "layout.o"), "-o", str(binary)]):
                build = subprocess.run(command, text=True, capture_output=True)
                self.assertEqual(build.returncode, 0, build.stderr)
            for name, ini in CASES.items():
                with self.subTest(case=name):
                    folder = tmp / name
                    folder.mkdir()
                    if ini is not None:
                        (folder / "longshot_options.ini").write_text(ini, encoding="utf-8")
                    run = subprocess.run([str(binary), name], cwd=folder, text=True, capture_output=True, timeout=30)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            for case in ("auto", "user"):
                with self.subTest(restart=case):
                    folder = tmp / case
                    folder.mkdir()
                    (folder / "longshot_options.ini").write_text(CASES["ls-two"], encoding="utf-8")
                    for action in ("save", "restore"):
                        run = subprocess.run([str(binary), f"{action}-{case}"], cwd=folder,
                                             text=True, capture_output=True, timeout=30)
                        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


# host/oot_sprint.c against a fake game: Link's pointer chain and the actor and event managers in a small memory map. Each
# frame reads the controller (hos_sprint_input, as npad_states hands it Sprint's input) and then runs Link's movement
# update (hos_sprint_link, at 0x804ce798) and his stance update (hos_sprint_stance, at 0x80c27f3c), which must find the
# dash input (bit 4) in his flags exactly while the sprint is on.
ENGINE_PRELUDE = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { uint64_t x[32]; } Ctx;
#define g_module_base 0ull
static struct { uint64_t a, v; } cells[64];
static int ncells;
static int reads;
static void put(uint64_t a, uint64_t v) {
    for (int i = 0; i < ncells; i++)
        if (cells[i].a == a) { cells[i].v = v; return; }
    cells[ncells].a = a, cells[ncells].v = v, ncells++;
}
static uint64_t get(uint64_t a) {
    for (int i = 0; i < ncells; i++)
        if (cells[i].a == a) return cells[i].v;
    return 0;
}
int hos_mem_region(uint64_t a, uint64_t size) {
    (void)size;
    reads++;
    for (int i = 0; i < ncells; i++)
        if (cells[i].a == a) return 1;
    return 0;
}
#define G64(a) get(a)
#define G32(a) ((uint32_t)get(a))
volatile uint32_t hos_frame_seconds_bits;
static int menu_open;
int hos_oot_menu_sprint_active(void) { return menu_open; }
#include <stdarg.h>
/* (hos_log: the toggled sprint's starts and ends, each a line; counted, and shown when an assert fails) */
static int starts, ends;
void hos_log(const char *fmt, ...) {
    char line[256];
    va_list a;
    va_start(a, fmt), vsnprintf(line, sizeof line, fmt, a), va_end(a);
    starts += strstr(line, "sprint starts") != NULL, ends += strstr(line, "sprint ends") != NULL;
    puts(line);
}
"""

ENGINE_GAME = r"""
enum { LINK = 0x1000, CHAIN_A = 0x2000, CHAIN_B = 0x3000, CHAIN_C = 0x4000, CHAIN_D = 0x5000, MGR = 0x6000, CTL = 0x7000,
       EVENTS = 0x8000, OTHER_A = 0x9000 };
/* (the game's own ids, the names its state table gives them) */
enum { S_WALK = 1, S_JUMP = 13, S_LAND = 32, S_SLIDE = 35, S_ROLL = 99, S_WAIT = 40, S_ATTACK = 112, S_LOCK_ON = 65,
       S_CLIMB = 70, S_LADDER = 71, S_CLIFF = 25, S_TALK = 75, S_SWIM = 84, S_ITEM = 238, S_MUSIC = 235, S_RIDE = 24,
       S_TURN_AROUND = 21, S_TURN = 22, S_QUICK_TURN = 188 };
#define BIT_A (1ull << 0)
#define BIT_LS (1ull << 4)
#define BIT_ZL (1ull << 8)

static void game(void) {
    put(LINK, 0x84b6dfc0ull), put(LINK + 0x18, CHAIN_A), put(CHAIN_A + 0x270, CHAIN_B), put(CHAIN_B + 0x188, CHAIN_C);
    put(CHAIN_C + 0x1c0, CHAIN_D), put(CHAIN_D + 0x28, S_WAIT), put(CHAIN_D + 0x4c, 0);
    put(0x84c9e0d0ull, 0x84c9e0d8ull), put(0x84c9e0d8ull, MGR), put(MGR, 0x84bffe68ull), put(MGR + 0x470, CTL);
    put(CTL, 0x84b69698ull), put(CTL + 0x18, CHAIN_A);
    put(0x84c9e090ull, 0x84c9e098ull), put(0x84c9e098ull, EVENTS), put(EVENTS + 0x68, 0);
}
static void state(int id) { put(CHAIN_D + 0x28, (uint64_t)id); }
static void event(int on) { put(EVENTS + 0x68, on ? 0x1234 : 0); }
/* Link's place across the ground (x and z at +0x31c and +0x324 in the actor at Link +0x18): each frame he goes pace
   metres along x, 0.1 a 60th of a second (6 m/s, a sprint) unless a check stands him still */
static float link_x, pace = 0.1f;
static void walk_on(void) {
    uint32_t bits;
    link_x += pace;
    memcpy(&bits, &link_x, 4), put(CHAIN_A + 0x31c, bits);
    put(CHAIN_A + 0x324, 0);
}

/* What npad_states hands over this frame (the Sprint row, its Toggle, the left stick's press as Sprint's input) */
/* (blocked: host/oot_sprint.c's SPRINT_BLOCKED and SPRINT_AWAY; Insert open is both, the focus lost AWAY) */
enum { BLOCKED = 1, AWAY = 2, PAUSE = 4 };   /* (and PAUSE: Plus or Minus pressed) */
static int on = 1, toggle, l3 = 1, blocked, reset;
/* one frame: the controller read with Sprint held or not and these buttons (what reaches the game in *game_saw), then
   Link's two updates; 1 when both found the dash input */
static int frame(int sprint, uint64_t buttons, uint64_t *game_saw) {
    const uint64_t seen = hos_sprint_input(buttons, on, sprint, l3 && sprint, toggle, reset, blocked);
    if (game_saw) *game_saw = seen;
    walk_on();
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[19] = LINK, c.x[10] = 0x1, c.x[8] = 0x1;   /* (other flags stay) */
    hos_sprint_link(&c);
    const int link = (c.x[10] & 0x10) != 0;
    assert(c.x[10] & 0x1);
    hos_sprint_stance(&c);
    const int stance = (c.x[8] & 0x10) != 0;
    assert(link == stance);
    return link;
}
/* a press: down for a frame, then up */
static int tap(void) { int r = frame(1, 0, NULL); return frame(0, 0, NULL) || r; }

int main(void) {
    game();
    uint64_t saw;

    /* the row off: nothing happens, and the buttons go through as they are */
    on = 0, state(S_WALK);
    reads = 0;
    assert(!frame(1, BIT_A, &saw) && saw == BIT_A);
    assert(reads == 0);   /* neither hook looks up actors or state while disabled */
    frame(0, 0, NULL);
    on = 1;

    /* held (Toggle off): on while held, off when let go; A is never added, the player's own A goes through */
    toggle = 0, state(S_WALK);
    frame(0, 0, NULL);
    assert(frame(1, 0, &saw) && saw == 0);
    assert(frame(1, BIT_A, &saw) && saw == BIT_A);   /* (their A, a roll: theirs, the sprint goes on) */
    state(S_ROLL), assert(frame(1, 0, NULL));
    state(S_WALK), assert(frame(1, 0, NULL));
    assert(!frame(0, 0, NULL));
    state(S_ATTACK), assert(!frame(1, 0, NULL));   /* (held into an attack: off) */
    frame(0, 0, NULL), state(S_WALK);
    /* ... Insert open (blocked, away) ends it as before, and the press held on when it closes is not Sprint's */
    assert(!frame(0, 0, NULL) && frame(1, 0, NULL));
    blocked = BLOCKED | AWAY, assert(!frame(1, 0, NULL)), blocked = 0;
    assert(!frame(1, 0, NULL));
    frame(0, 0, NULL);

    /* Toggle: a press while walking starts it; it goes on through jumps, landings, slides and rolls */
    toggle = 1;
    frame(0, 0, NULL);
    assert(frame(1, 0, &saw) && saw == 0);
    assert(frame(0, 0, NULL));
    int keep[] = {S_JUMP, S_LAND, S_SLIDE, S_ROLL, S_WALK};
    for (int i = 0; i < 5; i++) state(keep[i]), assert(frame(0, 0, NULL));
    /* the next press ends it */
    assert(!tap());
    /* any other state ends it at once: an attack, Z-targeting, climbing, a ledge, talking, swimming, an item, the
       ocarina, Epona */
    int other[] = {S_ATTACK, S_LOCK_ON, S_CLIMB, S_LADDER, S_CLIFF, S_TALK, S_SWIM, S_ITEM, S_MUSIC, S_RIDE};
    for (int i = 0; i < 10; i++) {
        assert(tap());
        state(other[i]), assert(!frame(0, 0, NULL));
        state(S_WALK), assert(!frame(0, 0, NULL));
    }
    /* ZL held ends it */
    assert(tap());
    assert(!frame(0, BIT_ZL, NULL)), assert(!frame(0, 0, NULL));
    /* the host's reset (the row turned off, the bindings laid out again) ends it */
    assert(tap());
    reset = 1, assert(!frame(0, 0, NULL)), reset = 0;
    assert(!frame(0, 0, NULL));
    /* Blocked with the player still there (the ocarina waiting for a note, a test run's own input keeping the player's
       Sprint out): no press is Sprint's, and the sprint is Link's state's to end (the ocarina's is PlayMusic, which does).
       Events (a conversation, a cutscene) end it, including before the next Link update. */
    assert(tap());
    blocked = BLOCKED;
    for (int i = 0; i < 10; i++) assert(frame(0, 0, NULL));
    assert(frame(1, 0, NULL) && frame(0, 0, NULL));   /* (a press then: not Sprint's, the sprint goes on) */
    state(S_MUSIC), assert(!frame(0, 0, NULL)), state(S_WALK), blocked = 0;
    assert(!frame(0, 0, NULL));
    assert(tap());
    event(1), assert(!frame(0, 0, NULL)), event(0);
    assert(!frame(0, 0, NULL));
    /* Insert open or the focus lost (blocked and away): the sprint is held as it is, however long Link stands there
       (the game goes on without the player's input); a press then is neither Sprint's nor eaten later: held on as the
       menu closes it starts nothing, and the next press is the one that ends the sprint */
    assert(tap());
    blocked = BLOCKED | AWAY;
    state(S_WAIT);
    for (int i = 0; i < 300; i++) assert(frame(0, 0, NULL));
    assert(frame(1, 0, NULL));
    blocked = AWAY;   /* (the menu closed, the focus still elsewhere) */
    for (int i = 0; i < 100; i++) assert(frame(0, 0, NULL));
    assert(frame(1, 0, NULL));
    blocked = 0;
    assert(frame(1, 0, NULL) && frame(0, 0, NULL));
    /* ... back, standing: the time a press gives to start moving (90 frames), then four frames once he has */
    for (int i = 0; i < 80; i++) assert(frame(0, 0, NULL));
    state(S_WALK), assert(frame(0, 0, NULL));
    state(S_WAIT);
    for (int i = 0; i < 4; i++) assert(frame(0, 0, NULL));
    assert(!frame(0, 0, NULL));
    /* ... and an event while away ends it all the same */
    state(S_WALK), assert(tap());
    blocked = AWAY, event(1), assert(!frame(0, 0, NULL)), event(0), blocked = 0;
    assert(!frame(0, 0, NULL));
    /* The game's screen holds the sprint for as long as it is open. */
    state(S_WALK), assert(tap());
    blocked = PAUSE, assert(frame(0, 0, NULL)), blocked = 0;   /* (Plus: the pause opens a few frames later) */
    for (int i = 0; i < 3; i++) assert(frame(0, 0, NULL));
    pace = 0.0f, state(S_WAIT), menu_open = 1;
    for (int i = 0; i < 180; i++) assert(frame(0, i >= 2 && i < 10 ? BIT_ZL : 0, NULL));
    assert(frame(1, 0, NULL) && frame(0, 0, NULL));
    menu_open = 0; /* close without Plus or Minus, no state or position change */
    for (int i = 0; i < 80; i++) assert(frame(0, 0, NULL));
    pace = 0.1f, state(S_WALK), assert(frame(0, 0, NULL));
    state(S_WAIT);
    for (int i = 0; i < 4; i++) assert(frame(0, 0, NULL));
    assert(!frame(0, 0, NULL));
    /* ... the stick let go for Plus: Link stands while the pause opens (here, ten frames): not a stop */
    state(S_WALK), assert(tap());
    blocked = PAUSE, assert(frame(0, 0, NULL)), blocked = 0;
    state(S_WAIT);
    for (int i = 0; i < 10; i++) assert(frame(0, 0, NULL));
    pace = 0.0f, menu_open = 1;
    for (int i = 0; i < 40; i++) assert(frame(0, 0, NULL));
    menu_open = 0, pace = 0.1f, state(S_WALK);   /* (closed, running on) */
    for (int i = 0; i < 60; i++) assert(frame(0, 0, NULL));
    assert(!tap());
    /* Closing without moving accepts the next Sprint press immediately. */
    state(S_WAIT), pace = 0.0f, assert(tap());
    blocked = PAUSE, assert(frame(0, 0, NULL)), blocked = 0;
    menu_open = 1;
    for (int i = 0; i < 180; i++) assert(frame(0, 0, NULL));
    menu_open = 0;
    assert(!frame(1, 0, NULL));
    frame(0, 0, NULL);
    /* ZL also takes effect on the first stationary update after closing. */
    assert(tap());
    menu_open = 1, assert(frame(0, 0, NULL));
    menu_open = 0, assert(!frame(0, BIT_ZL, NULL));
    assert(tap());
    menu_open = 1, assert(frame(0, 0, NULL));
    menu_open = 0;
    for (int i = 0; i < 90; i++) assert(frame(0, 0, NULL));
    assert(!frame(0, 0, NULL));
    pace = 0.1f;
    /* ... Plus or Minus with no pause after it (Link goes on running): nothing held, standing in Walk ends it as ever */
    state(S_WALK), assert(tap());
    blocked = PAUSE, assert(frame(0, 0, NULL)), blocked = 0;
    for (int i = 0; i < 40; i++) assert(frame(0, 0, NULL));
    pace = 0.001f;
    for (int i = 0; i < 20; i++) assert(frame(0, 0, NULL));
    assert(!frame(0, 0, NULL));
    pace = 0.1f;
    /* a load (no update of Link for eight reads or more): the sprint goes on, with the time a press gives to start
       moving again */
    assert(tap());
    for (int i = 0; i < 40; i++) hos_sprint_input(0, on, 0, 0, toggle, 0, 0);
    state(S_WAIT);
    for (int i = 0; i < 80; i++) assert(frame(0, 0, NULL));
    state(S_WALK), assert(frame(0, 0, NULL));
    state(S_WAIT);
    for (int i = 0; i < 4; i++) assert(frame(0, 0, NULL));
    assert(!frame(0, 0, NULL));
    /* ... a load that ends with Link left standing: ninety frames, then it ends */
    state(S_WALK), assert(tap());
    for (int i = 0; i < 40; i++) hos_sprint_input(0, on, 0, 0, toggle, 0, 0);
    state(S_WAIT);
    for (int i = 0; i < 90; i++) assert(frame(0, 0, NULL));
    assert(!frame(0, 0, NULL));
    state(S_WALK);
    /* standing still: four 60 Hz frames are kept, the fifth ends it (said once, starting and ending) */
    const int starts_was = starts, ends_was = ends;
    assert(tap());
    state(S_WAIT);
    for (int i = 0; i < 4; i++) assert(frame(0, 0, NULL));
    assert(!frame(0, 0, NULL));
    assert(starts == starts_was + 1 && ends == ends_was + 1);
    /* ... at 240 fps by the frames' time, not by updates: 16 updates of a 240th are four 60 Hz frames */
    {
        float quarter = 1.0f / 240.0f;
        memcpy((void *)&hos_frame_seconds_bits, &quarter, 4);
        state(S_WALK), assert(tap());
        state(S_WAIT);
        for (int i = 0; i < 15; i++) assert(frame(0, 0, NULL));
        frame(0, 0, NULL);   /* (the 16th: at the edge, 16 floats of a 240th add up to a hair over four frames) */
        assert(!frame(0, 0, NULL));
        hos_frame_seconds_bits = 0;
    }
    /* standing in Walk (held against a wall, a stick resting a little off its centre: 0.06 m/s): standing all the same,
       once it lasts sixteen frames, then the four */
    state(S_WALK), assert(tap());
    pace = 0.001f;
    for (int i = 0; i < 20; i++) assert(frame(0, 0, NULL));
    assert(!frame(0, 0, NULL));
    /* ... a U-turn stands him still in Walk for about six frames, then he runs back: the sprint goes on; so do the turn
       states */
    pace = 0.1f;
    assert(tap());
    for (int i = 0; i < 30; i++) assert(frame(0, 0, NULL));
    pace = 0.0f;
    for (int i = 0; i < 8; i++) assert(frame(0, 0, NULL));
    pace = 0.1f;
    for (int i = 0; i < 100; i++) assert(frame(0, 0, NULL));
    int turns[] = {S_TURN_AROUND, S_TURN, S_QUICK_TURN};
    for (int i = 0; i < 3; i++) state(turns[i]), assert(frame(0, 0, NULL) && frame(0, 0, NULL));
    state(S_WALK), assert(frame(0, 0, NULL));
    assert(!tap());
    /* ... a slow walk (the Walk key's, 2 m/s) is moving: the sprint goes on, and the next press ends it */
    pace = 2.0f / 60.0f;
    assert(tap());
    for (int i = 0; i < 200; i++) assert(frame(0, 0, NULL));
    assert(!tap());
    pace = 0.1f;
    /* a press while standing: ninety frames to start moving */
    state(S_WAIT);
    assert(tap());
    for (int i = 0; i < 80; i++) assert(frame(0, 0, NULL));
    state(S_WALK), assert(frame(0, 0, NULL));
    state(S_WAIT);
    for (int i = 0; i < 4; i++) assert(frame(0, 0, NULL));
    assert(!frame(0, 0, NULL));

    /* a conversation (an event runs, Link's update goes on): the left stick's press is the game's LS, it hides the text
       box, for as long as it is held; no sprint */
    state(S_WALK), event(1);
    assert(!frame(1, 0, &saw) && saw == BIT_LS);
    assert(!frame(1, 0, &saw) && saw == BIT_LS);
    assert(!frame(0, 0, &saw) && saw == 0);
    event(0);
    assert(!frame(0, 0, NULL));
    /* ... Sprint on a key or another button: nothing goes to the game */
    l3 = 0, event(1);
    assert(!frame(1, 0, &saw) && saw == 0);
    frame(0, 0, NULL), event(0), l3 = 1;
    /* blocked (Insert open, the ocarina waiting for a note): neither Sprint's nor the game's */
    blocked = 1;
    assert(!frame(1, 0, &saw) && saw == 0);
    frame(0, 0, NULL), blocked = 0;
    /* a menu, a cutscene or a load (no update of Link for eight reads): the game's, and no sprint after */
    for (int i = 0; i < 8; i++) hos_sprint_input(0, on, 0, 0, toggle, 0, 0);
    assert(hos_sprint_input(0, on, 1, 1, toggle, 0, 0) == BIT_LS);
    assert(!frame(1, 0, &saw) && saw == BIT_LS);   /* (the same press held on into gameplay stays the game's) */
    frame(0, 0, NULL);
    /* Missing or invalid manager and controlled actor pointers never accept either Link update. */
    {
        Ctx c = {0};
        c.x[19] = LINK;
        assert(tap());
        for (int i = 0; i < 6; i++) {
            put(0x84c9e0d8ull, i == 0 ? 0 : MGR);
            put(MGR, i == 1 ? 0 : 0x84bffe68ull);
            put(MGR + 0x470, i == 2 ? 0 : CTL);
            put(CTL, i == 3 ? 0 : 0x84b69698ull);
            put(CTL + 0x18, i == 4 ? 0 : (i == 5 ? OTHER_A : CHAIN_A));
            c.x[10] = c.x[8] = 0;
            hos_sprint_link(&c), hos_sprint_stance(&c);
            assert(!(c.x[10] & 0x10) && !(c.x[8] & 0x10));
        }
        put(CTL + 0x18, CHAIN_A);
        reset = 1, assert(!frame(0, 0, NULL)), reset = 0;
    }
    /* a Link the player does not control does not sprint */
    put(CTL + 0x18, OTHER_A);
    frame(1, 0, NULL);
    assert(!frame(0, 0, NULL));
    put(CTL + 0x18, CHAIN_A);
    reset = 1, frame(0, 0, NULL), reset = 0;   /* (that press is still owed to the controlled Link: ended here) */
    assert(!frame(0, 0, NULL));
    /* the Toggle changed mid-sprint: the sprint ends */
    assert(tap());
    toggle = 0, assert(!frame(0, 0, NULL));
    puts("ok");
    return 0;
}
"""


def engine_source():
    text = (ROOT / "host" / "oot_sprint.c").read_text(encoding="utf-8")
    text = re.sub(r"^#include.*$", "", text, flags=re.M)
    text = text.replace("extern volatile uint32_t hos_frame_seconds_bits;", "")
    text = text.replace("int hos_mem_region(uint64_t addr, uint64_t size);", "")
    return ENGINE_PRELUDE + text + ENGINE_GAME


class SprintEngineTests(unittest.TestCase):
    """host/oot_sprint.c: the game's dash input while Sprint runs, held or toggled, against a fake game"""

    def test_engine(self):
        compiler = compiler_command("CC", "gcc", "clang")
        if not compiler or not compiler[0]:
            self.skipTest("no C compiler")
        with tempfile.TemporaryDirectory(prefix="longshot-sprint-engine-") as tmp:
            tmp = Path(tmp)
            (tmp / "t.c").write_text(engine_source(), encoding="utf-8")
            binary = tmp / ("t.exe" if sys.platform == "win32" else "t")
            build = subprocess.run([*compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                                    str(tmp / "t.c"), "-o", str(binary)], text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], text=True, capture_output=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_wiring(self):
        hid = (ROOT / "host" / "nn_hid.c").read_text(encoding="utf-8")
        npad = test_bound_keys.block(hid, "static void npad_states(")
        self.assertIn("const int sprint_toggle = sprint_enabled && sprint_toggle_frame;", npad)
        self.assertIn("buttons = hos_sprint_input(buttons, sprint_enabled, sprint_down & (SPRINT_KEY | SPRINT_PAD),", npad,
                      "npad_states does not hand Sprint's input to host/oot_sprint.c")
        self.assertLess(npad.index("buttons = hold_toggle_apply("), npad.index("buttons = hos_sprint_input("),
                        "Sprint must see the buttons as the game will get them")
        self.assertIn("SPRINT_L3", test_bound_keys.block(hid, "static int pad_sprint_down("),
                      "Sprint does not say when its gamepad input is the left stick's press")
        for gone in ("hos_sprint_press_mask", "sprint_step(", "sprint_toggle_step(", "SPRINT_ROLL_GAP_MS"):
            self.assertNotIn(gone, hid, "the old Sprint, A held without its press, is still in host/nn_hid.c")
        menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
        self.assertIn("|| button == HOS_KEY_SPRINT;", test_bound_keys.block(menu, "bool has_toggle(int button) {"),
                      "the Controls page has no Toggle for Sprint")
        self.assertIn('if (i < HOS_SW_N || i == HOS_KEY_SPRINT) fprintf(f, "toggle_%s=%d\\n"', menu,
                      "the menu does not save toggle_sprint")
        self.assertIn("A toggled sprint ends when Link stops, attacks, targets with ZL", menu,
                      "the Toggle's help does not say Link's state ends a toggled sprint")
        self.assertIn("opening Insert, pausing or losing focus does not", menu,
                      "the Toggle's help does not say a menu, a pause or the focus lost leave a toggled sprint on")
        replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
        self.assertIn("host->binding(HOS_BIND_SPRINT_TOGGLE, 0, 0)", replay, "the host cannot see Sprint's Toggle")


# Independent synthetic functions and anchors for the patch machinery.
LINK_FN = ('#include "a64.h"\n' + "void f_80001234(Ctx *c) {\n"
           "    prepare();\n    sample_value();\n    finish();\n}\n")
STANCE_FN = ("void f_80005678(Ctx *c) {\n"
             "    begin();\n    sample_byte();\n    check_value();\n}\n")


class SprintPatchTests(unittest.TestCase):
    """nx2/oot_patches.py: matching and insertion on independently authored synthetic text"""

    def setUp(self):
        sys.path.insert(0, str(ROOT / "nx2"))
        import oot_patches
        self.p = oot_patches
        # Exercise both public patch paths and touches with invented sites.
        for name, value in {
            "SPRINT_LINK_HEAD": "void f_80001234(Ctx *c) {\n",
            "SPRINT_LINK_AT": r"prepare\(\);\s*(?P<site>sample_value\(\);\s*)finish\(\);",
            "SPRINT_STANCE_HEAD": "void f_80005678(Ctx *c) {\n",
            "SPRINT_STANCE_AT": r"begin\(\);\s*(?P<site>sample_byte\(\);\s*)check_value\(\);",
        }.items():
            old = getattr(oot_patches, name)
            self.addCleanup(setattr, oot_patches, name, old)
            setattr(oot_patches, name, value)

    def test_patch(self):
        p = self.p
        out, n = p.patch_sprint_link(LINK_FN)
        self.assertEqual(n, 1)
        self.assertIn(p.SPRINT_LINK_CALL + "finish();", out)
        self.assertEqual(out.count(p.SPRINT_DECL), 1)
        self.assertEqual(p.patch_sprint_link(out), (out, 1), "a second run changes the code")
        out, n = p.patch_sprint_stance(out + STANCE_FN)
        self.assertEqual(n, 1)
        self.assertIn(p.SPRINT_STANCE_CALL + "check_value();", out)
        self.assertEqual(out.count(p.SPRINT_STANCE_CALL), 1)
        self.assertEqual(out.count(p.SPRINT_DECL), 1, "declared once for both")
        whole, _, _ = p.patch_file(LINK_FN + STANCE_FN, "t.c", True)
        self.assertEqual((whole.count(p.SPRINT_LINK_CALL), whole.count(p.SPRINT_STANCE_CALL)), (1, 1),
                         "patch_file does not run the two")

    def test_drifted(self):
        """another build's code: left as it was, and the count says so (report() warns)"""
        p = self.p
        plain = "void f_80000000(Ctx *c) {\n}\n"
        self.assertEqual(p.patch_sprint_link(plain), (plain, 0))
        other = LINK_FN.replace("sample_value()", "other_value()")
        self.assertEqual(p.patch_sprint_link(other), (other, 0))
        other = STANCE_FN.replace("check_value()", "other_check()")
        self.assertEqual(p.patch_sprint_stance(other), (other, 0))
        duplicate = LINK_FN.replace("    finish();", "    finish();\n    prepare(); sample_value(); finish();")
        self.assertEqual(p.patch_sprint_link(duplicate), (duplicate, 0))
        report = (ROOT / "nx2" / "oot_patches.py").read_text(encoding="utf-8")
        self.assertIn('(total["sprint link"], total["sprint stance"]) != (1, 1)', report,
                      "report() does not warn when a site is missing")
        self.assertNotIn("hos_sprint_press_mask", report, "the old press mask is still patched in")

    def test_kept_from_locals(self):
        """nx2/guest_locals.py keeps the context in a function the patches change: both are found by touches()"""
        p = self.p
        self.assertTrue(p.touches(LINK_FN.replace('#include "a64.h"\n', "")))
        self.assertTrue(p.touches(STANCE_FN))


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
