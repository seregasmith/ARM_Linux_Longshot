"""The settings page's table (docs/user/graphics-and-controls.md, which the website publishes) has a line for every row of
the Insert menu's Graphics, Audio, Gameplay, Controls and NFC tabs, in the menu's order.

The rows are the menu as built (gpu/vk/vk_menu.cpp's own and the modules', gpu/vk/menu/, in the order the model works
out: tests/menu_model_fixture.py built_menu compiles and runs it, with no game) and the lines are the page's
"| Tab | Option | Choices | Default |" table. Restart and Quit are actions with no choice to list; the
Controls page's two rows have lines of their own among the Controls tab's; the Motion page's rows are on the Motion
line, the Graphics tab's section titles have none, and the Debug tab's lines at the table's end are left out.

    python -I -B tests/test_settings_table.py [--cxx g++]
"""
import argparse
from functools import lru_cache
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from menu_model_fixture import built_menu  # noqa: E402

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]
PAGE = (ROOT / "docs" / "user" / "graphics-and-controls.md").read_text(encoding="utf-8")
ACTIONS = {"Restart", "Quit"}

PAGES = {"GRAPHICS": "graphics", "AUDIO": "audio", "GAMEPLAY": "gameplay", "CONTROLS": "controls", "PAGE": "controls_page",
         "NFC": "nfc", "DEBUG": "debug"}
CXX = "g++"


@lru_cache(maxsize=None)
def built():
    pages, diags = built_menu(CXX)
    if diags:
        raise SystemExit("the menu as built has diagnostics: %s" % diags)
    return pages


def headings():
    return [it.label for items in built().values() for it in items if it.heading]


def menu_rows(tabs="GRAPHICS|AUDIO|GAMEPLAY|CONTROLS|PAGE|NFC"):
    """(tab, label) of the tabs' rows, each tab's in the menu's order, the tabs as named"""
    return [(tab, it.label) for tab in tabs.split("|") for it in built().get(PAGES[tab], []) if not it.heading]


def table_lines(include_debug=False):
    start = PAGE.index("| Tab | Option | Choices | Default |")
    lines = PAGE[start:PAGE.index("\n\n", start)].split("\n")[2:]
    rows = [[cell.strip() for cell in line.strip().strip("|").split("|")] for line in lines]
    debug = next((i for i, cells in enumerate(rows) if cells[0] == "Debug"), len(rows))
    return rows if include_debug else rows[:debug]   # (the Debug tab's lines end the table: its Developer rows, not checked here)


class SettingsTableTests(unittest.TestCase):
    def test_the_tables_are_found(self):
        self.assertGreater(len(menu_rows()), 40)
        self.assertGreater(len(table_lines()), 40)
        self.assertIn(("GRAPHICS", "Quality preset"), menu_rows())
        self.assertIn("Resolution and upscaling", headings())

    def test_every_row_of_the_four_tabs_has_a_line(self):
        listed = {cells[1] for cells in table_lines()}
        missing = [name for _, name in menu_rows() if name not in listed and name not in ACTIONS]
        self.assertEqual(missing, [], "menu rows with no line in the settings page's table")

    def test_every_line_is_a_row_the_menu_has(self):
        rows = {name for _, name in menu_rows()}
        gone = [cells[1] for cells in table_lines() if cells[1] not in rows]
        self.assertEqual(gone, [], "lines in the settings page's table for rows the menu no longer has")

    def test_the_lines_keep_the_menus_order(self):
        page = {name for _, name in menu_rows("PAGE")}   # (the Controls page's rows: listed with the Controls tab's)
        order = [name for _, name in menu_rows("GRAPHICS|AUDIO|GAMEPLAY|CONTROLS|NFC") if name not in ACTIONS and name not in page]
        listed = [cells[1] for cells in table_lines() if cells[1] not in page]
        self.assertEqual(listed, order)

    def test_damage_taken_is_listed_in_debug_player(self):
        tab = ""
        found = []
        for cells in table_lines(include_debug=True):
            tab = cells[0] or tab
            if "Damage taken" in cells[1]:
                found.append((tab, cells[1], cells[3]))
        self.assertEqual(found, [("Debug", "Player: Damage taken", "Normal")])
        self.assertIn(("DEBUG", "Damage taken"), menu_rows("DEBUG"))
        self.assertNotIn(("GAMEPLAY", "Damage taken"), menu_rows("GAMEPLAY"))

    def test_every_line_has_its_four_cells_and_a_default(self):
        for cells in table_lines():
            with self.subTest(option=cells[1]):
                self.assertEqual(len(cells), 4)
                self.assertTrue(cells[2])
                self.assertTrue(cells[3] or cells[1] in ("Motion", "Controller speaker", "Controls", "Bug report", "Tap tag", "Remove tag",
                                          "Scan tag on controller", "Compare ReShade Preset",
                                          "Rescan ReShade presets"))  # actions have no default


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--cxx", default="g++")
    known, rest = parser.parse_known_args()
    CXX = known.cxx
    unittest.main(argv=[sys.argv[0]] + rest)
