"""A module's button that is locked does nothing on Enter (gpu/vk/vk_menu.cpp menu_tick, #1633).

Enter (or A) on a row goes down menu_tick's chain of presses: a module's button runs its on_activate unless its locked()
gives a reason; the chain's last case, a row with no value, opens the Controls page's bindings (the Controls row's press).
A locked module button skipped its own case and fell to that one, so Enter on it would open the bindings grid. A module's
Action or Page row ends the chain whether it ran or not; the Controls row (the core's custom section) keeps the last case.

    python -I -B tests/test_menu_locked_button.py
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from srccut import block  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
ENDS = ("else if (press[7] && r.setting && (r.setting->kind == menu::Kind::Action || r.setting->kind == menu::Kind::Page))")
RUNS = "else if (press[7] && r.setting && r.setting->on_activate &&"
BINDS = "else if (press[7] && !r.value && can_bind()) page = 1"


def fail(why):
    raise SystemExit("FAIL: " + why)


def main():
    tick = block((ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8"), "void menu_tick() {")
    for part in (ENDS, RUNS, BINDS):
        if tick.count(part) != 1:
            fail("menu_tick: %r is not there once" % part)
    if not tick.index(RUNS) < tick.index(ENDS) < tick.index(BINDS):
        fail("menu_tick: a module's button that is locked falls through to the bindings page")
    print("PASS: a locked module button ends menu_tick's chain of presses before the bindings page")


if __name__ == "__main__":
    main()
