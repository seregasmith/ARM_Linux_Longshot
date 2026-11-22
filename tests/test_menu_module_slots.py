"""Every row the modules describe has its slot in the Insert menu's table (gpu/vk/vk_menu.cpp, #1633).

vk_menu.cpp's row table holds its own rows and, after them, at most kModuleRowsMax rows from the modules (gpu/vk/menu/,
headings included): a module row's value text and hint go through a function of its slot's own (module_text_at<I>,
module_hint_at<I>), so the number is fixed when the renderer is built. A row past it is left out of the menu and logged
("more than N rows from modules: left out"). Once every row was a module's there were 110, past a limit of 64: the
Controls tab lost its motion, walk and menu-key rows, the Debug tab its developer rows, and the NFC tab every row, and
opening a tab with no row left divided by zero in menu_tick (its `% tab_count`) and took the game down.

The rows are the menu as built (tests/menu_model_fixture.py built_menu compiles and runs the model with every module, with
no game): every item of every page but vk_menu.cpp's own rows counts against the limit. That counts a little over what
row_table takes (it skips a section with no title, and a kind the menu doesn't draw), never under.

    python -I -B tests/test_menu_module_slots.py [--cxx g++]
"""
import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from menu_model_fixture import built_menu, core_rows  # noqa: E402

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def fail(why):
    raise SystemExit("FAIL: " + why)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    found = re.findall(r"^constexpr int kModuleRowsMax = (\d+);", menu, re.M)
    if len(found) != 1:
        fail("vk_menu.cpp: kModuleRowsMax, the module rows' slots, was not found once")
    slots = int(found[0])
    if "if (slots == kModuleRowsMax || (int)table.size() == kMaxRows) {" not in menu:
        fail("vk_menu.cpp: row_table does not stop at kModuleRowsMax's text slots and at kMaxRows rows (a module's heading "
             "takes a row but no slot: past kMaxRows, restart_slot[kMaxRows] is written out of its bounds)")
    # (a module row's value text: written by the menu's drawing and by the control bridge's thread (bridge_fill), so each
    # thread its own buffers; a std::string written by both at once can break the heap)
    text_fn = menu[menu.index("const char* module_text(int slot, int v) {"):]
    text_fn = text_fn[:text_fn.index("\n}\n")]
    if ("static thread_local std::string said[kModuleRowsMax][4];" not in text_fn or
            "static thread_local int next[kModuleRowsMax];" not in text_fn):
        fail("vk_menu.cpp: module_text's buffers are shared by the drawing and the control bridge's threads")
    pages, _ = built_menu(args.cxx)
    items = sum(len(v) for v in pages.values())
    own = len(core_rows(menu))
    if items <= own:
        fail("the menu as built has %d items, no more than vk_menu.cpp's own %d rows: the pages were not read" % (items, own))
    from_modules = items - own
    if from_modules > slots:
        fail("the modules describe %d rows and headings, past kModuleRowsMax (%d): %d left out of the menu, and a tab with "
             "none left crashes menu_tick" % (from_modules, slots, from_modules - slots))
    print("PASS: %d rows and headings from the modules, within the table's %d slots" % (from_modules, slots))


if __name__ == "__main__":
    main()
