"""A tab with no row shown divides by nothing (gpu/vk/vk_menu.cpp, #1633).

The menu steps its selection round a tab's rows by their count (`% tab_count`), skipping headings; with no row shown
(every row's shown() false, or a table without the tab's rows) the count is 0 and the step divided by it: the first run
of the model's menu crashed so, opening the NFC tab with its rows left out (tests/test_menu_module_slots.py). Each place
that steps by a tab's row count first leaves a tab without rows alone: menu_tick's selection (before its row handling),
the tuning bar's drawing, and the Debug tab's own step (debug_step). The ImGui menu runs only with the game, so this reads
the three places; tests/test_menu_module_slots.py keeps the rows within the table.

    python -I -B tests/test_menu_empty_tab.py
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from srccut import block  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def fail(why):
    raise SystemExit("FAIL: " + why)


def before(text, guard, use, where):
    if guard not in text or use not in text:
        fail("%s: no %r before %r" % (where, guard, use))
    if text.index(guard) > text.index(use):
        fail("%s: %r comes after %r" % (where, guard, use))


def main():
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    tick = block(menu, "void menu_tick() {")
    before(tick, "} else if (tab_row_count(cur_tab) == 0) {", "% tab_count", "menu_tick")
    bar = block(menu, "void draw_tune_bar(ui::Ctx& c) {")
    before(bar, "if (count == 0) return;", "% count", "draw_tune_bar")
    step = block(menu, "int debug_step(int sel, int dir) {")
    before(step, "if (n == 0) return sel;", "% n", "debug_step")
    print("PASS: a tab with no row shown is left alone before its rows are stepped by their count")


if __name__ == "__main__":
    main()
