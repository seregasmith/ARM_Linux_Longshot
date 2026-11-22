"""Damage taken belongs beside Heart protection in Debug's Player group, not Developer.

Compile the real Debug rows, visibility, group order and keyboard traversal without a GPU.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]
import sys  # noqa: E402
sys.path.insert(0, str(ROOT / "tests"))
from menu_model_fixture import built_menu, menu_model_prelude, menu_text, unhook  # noqa: E402
from menu_model_fixture import module_rows as described_rows  # noqa: E402

# (the shown() hooks of the Debug tab's module rows, as this fixture answers them: both false, as the stubs below say)
SHOWN_HOOKS = {"intel_compiler_row", "laptop_row"}


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    # (vk_menu.cpp and its modules, their rows as table lines: menu_text, #1633)
    menu = menu_text(ROOT)
    header = (ROOT / "gpu/vk/vk_menu.h").read_text(encoding="utf-8")
    table = block(menu, "Row rows[] = {") + block(menu, "Row module_rows[] = {")
    entries = re.findall(r'\{TAB_DEBUG,[^{}]*\}', table)
    assert len(entries) >= 18
    assert '{TAB_GAMEPLAY, "Damage taken"' not in table
    assert sum('"Damage taken"' in entry for entry in entries) == 1
    # (#1633: the tab's order is the model's, row_order, so the rows go in the order the menu builds them: built_menu. A row
    # with a value as its table line; the others, a module's button or page and the headings, as rows with no value)
    by_label = {re.match(r'\{TAB_DEBUG, "((?:[^"\\]|\\.)*)"', entry).group(1): entry for entry in entries}
    hooks = {r["label"].strip('"'): unhook(r["shown"]) for r in described_rows(ROOT) if r["page"] == "debug" and "shown" in r}
    assert set(hooks.values()) <= SHOWN_HOOKS, hooks   # (a new shown() on the Debug tab: answer it here)
    layout = built_menu(args.cxx, ROOT)[0]["debug"]
    lines = []
    for item in layout:
        if item.heading:
            line = '{TAB_DEBUG, "%s", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true}' % item.label
        elif item.label in by_label:
            line = by_label[item.label]
        else:
            assert item.value in ("", "nullptr"), item
            line = '{TAB_DEBUG, "%s", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, false}' % item.label
        if item.label in hooks:
            line = line[:-1] + ", -1, false, &shown_%s}" % hooks[item.label]
        lines.append(line)
    # (what follows the tools: the Developer rows and the sections' after it, in the tab's order, less the hidden)
    first = [item.heading for item in layout].index(True)
    after_tools = [item.label for item in layout[first + 1:] if not item.heading and item.label not in hooks]
    source = '#include <atomic>\n#include <cassert>\n#include <cstring>\n#include <iterator>\n#include <vector>\n'
    source += menu_model_prelude(ROOT)
    source += block(header, "struct Options {") + ';\nOptions opts;\n'
    source += block(menu, "enum MenuTab {") + ';\n'
    source += block(menu, "struct Row {") + ';\n'
    for name in ("tx_onoff", "tx_damage_taken"):
        source += block(menu, "const char* " + name + "(") + '\n'
    # Help, hints and the other rows' texts are not used by this navigation fixture.
    table = '\n'.join(lines)
    for name in sorted(set(re.findall(r'\b(tx_\w+|hn_\w+)\b', table)) - {"tx_onoff", "tx_damage_taken"}):
        source += ('const char* %s(int, int) { return nullptr; }\n' if name.startswith("hn_") else
                   'const char* %s(int) { return ""; }\n') % name
    source += 'bool intel_compiler_row() { return false; }   // (not Intel graphics on Windows: its two rows hidden)\n'
    source += 'bool laptop_row() { return false; }   // (a dedicated card: no Set up for a weak laptop button)\n'
    for name in sorted(set(hooks.values())):
        source += 'const menu::Setting shown_%s = [] { menu::Setting s; s.shown = %s; return s; }();\n' % (name, name)
    source += 'Row rows[] = {' + ',\n'.join(lines) + '};\n'
    source += 'constexpr int kRows = sizeof rows / sizeof rows[0];\n'
    source += 'const std::vector<int>& row_order(int t) {   // (the Debug tab in the model\'s order; no other tab here)\n'
    source += '    static std::vector<int> debug, none;\n'
    source += '    if (debug.empty()) for (int i = 0; i < kRows; i++) debug.push_back(i);\n'
    source += '    return t == TAB_DEBUG ? debug : none;\n}\n'
    source += 'const char* const after_tools[] = {%s};\n' % ", ".join('"%s"' % label for label in after_tools)
    source += '''
bool cheats_env = false;
bool quick_debug = false;
'''
    for name in ("bool cheats_on(", "bool row_shown(", "int tab_row_count(", "int tab_row_to_global(",
                 "int menu_damage_taken("):
        source += block(menu, name) + '\n'
    source += re.search(r"std::atomic<bool> transition_autosave_enabled\{[^}]+\};", menu).group() + '\n'
    for name in ("void sync_transition_autosave(", "int menu_transition_autosave("):
        source += block(menu, name) + '\n'
    source += menu[menu.index("struct DebugGroup {"):menu.index("/// (longshot) The Debug tab's rows from top down")]
    warning = block(menu, "if (open_now.load() && debug_warning) {")
    source += '''
std::atomic<bool> open_now{true};
bool debug_warning = true, changed = false, mouse = false, moved = false;
int debug_warning_choice = 0, mx = 0, my = 0, saves = 0;
bool press[8]{};
struct { int pressed; } ptr{};
enum { HOS_MOUSE_LEFT = 1, HOS_MOUSE_RIGHT = 2 };
namespace menu_hit { constexpr int kButton = 1; }
struct {
    struct Hit { int part, index; } hit{};
    Hit at(float, float) const { return hit; }
} hits;
void save() { ++saves; }
void warning_input() {
''' + warning + '''
}
'''
    source += r'''
int main() {
    opts.debug_tools = 0;
    opts.transition_autosave = 1;
    sync_transition_autosave();
    press[7] = true;
    warning_input();
    assert(!debug_warning && opts.debug_tools == 0 && menu_transition_autosave() == 1 && saves == 0);
    press[7] = false;
    debug_warning = true;
    press[4] = true;
    warning_input();
    assert(debug_warning_choice == 1);
    warning_input();
    assert(debug_warning_choice == 0);
    press[4] = false;
    press[3] = true;
    warning_input();
    assert(debug_warning_choice == 1);
    press[3] = false;
    press[7] = true;
    warning_input();
    assert(!debug_warning && opts.debug_tools == 1 && opts.transition_autosave == 0);
    assert(menu_transition_autosave() == 0 && saves == 1);
    int order[64], dev, heading;
    const char* expected[] = {"Debug tools", "Autosave:", "Heart protection", "Damage taken", "No-clip flight",
        "Movement speed", "Switch age", "Inventory", "Teleport", "Mark position", "Return to mark",
        "Free camera", "Set time"};
    for (int env = 0; env < 2; ++env) for (int enabled = 0; enabled < 2; ++enabled) {
        cheats_env = env;
        opts.debug_tools = enabled;
        opts.damage_taken = 3;
        for (int autosave = 0; autosave < 2; ++autosave) {
            opts.transition_autosave = autosave;
            sync_transition_autosave();
            assert(menu_transition_autosave() == autosave);
        }
        const int n = debug_rows(order, &dev, &heading);
        assert(heading >= 0);
        assert(dev == (env || enabled ? (int)std::size(expected) : kDebugGeneralRows));
        // Only the Developer rows and the sections' after it follow, in the tab's order (Frame pacing and Threaded rendering
        // among them).
        assert(n == dev + (int)std::size(after_tools));
        for (int i = dev; i < n; ++i)
            assert(!std::strcmp(rows[tab_row_to_global(TAB_DEBUG, order[i])].name, after_tools[i - dev]));
        for (int i = 0; i < dev; ++i) {
            const Row& row = rows[tab_row_to_global(TAB_DEBUG, order[i])];
            assert(!std::strcmp(row.name, expected[i]));
            assert(debug_step(order[i], 1) == order[i + 1]);
            assert(debug_step(order[i + 1], -1) == order[i]);
            if (i == 3) {
                assert(row.value == &opts.damage_taken && row.cheat == -1);
                assert(row.lo == 0 && row.hi == 3 && row.step == 1);
                assert(!row.restart && row.preset_exempt);
                assert(!std::strcmp(row.text(1), "Normal"));
                assert(!std::strcmp(row.text(3), "Quadruple"));
            }
        }
        assert(debug_step(order[n - 1], 1) == order[0]);
        assert(debug_step(order[0], -1) == order[n - 1]);
        assert(opts.damage_taken == 3); // Hiding the tools must not reset the saved option.
        assert(menu_damage_taken() == (env || enabled ? 3 : 1)); // Hidden means Normal for the host.
    }
    assert(kDebugGroups == 3 && kDebugTools == 11);
    assert(!std::strcmp(debug_groups[0].title, "Player"));
    assert(debug_groups[0].from == 0 && debug_groups[0].to == 6);
    assert(debug_groups[1].from == 6 && debug_groups[1].to == 9);
    assert(debug_groups[2].from == 9 && debug_groups[2].to == 11);
}
'''
    with tempfile.TemporaryDirectory(prefix="damage-debug-menu-") as tmp:
        cpp = Path(tmp) / "menu.cpp"
        exe = Path(tmp) / "menu.exe"
        cpp.write_text(source, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: Debug navigation, visibility, and autosave behavior when enabling or cancelling")


if __name__ == "__main__":
    main()
