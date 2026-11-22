"""The Insert menu's sections: heading rows group a tab's rows, and none can be left empty (gpu/vk/vk_menu.cpp).

A heading row (Row::heading) is a section's title: draw_options draws it (a gold title and a rule, nothing to click) and
menu_tick's keys step over it. The Graphics tab is grouped as Resolution and upscaling, Frame generation and latency,
Screen, HDR, Accessibility, Post-processing, Detail, and GPU and memory, after the Quality preset that sets rows in all of them. Each section must keep a
row on every PC: a row row_shown can hide (Reflex, Anti-Lag 2, the Upscaler, the DLSS model, HDR's levels) cannot be a
section's only row, or a GPU without it would show a title over nothing. The rows are the menu as built (#1633: vk_menu.cpp's
own and the modules', gpu/vk/menu/, in the model's order: tests/menu_model_fixture.py built_menu compiles and runs it, with no
game); a module's row can be hidden when it has a shown() of its own.

    python -I -B tests/test_menu_sections.py [--cxx g++]
"""
import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from menu_model_fixture import built_menu, module_rows  # noqa: E402

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]
PAGES = {"TAB_GRAPHICS": "graphics", "TAB_AUDIO": "audio", "TAB_GAMEPLAY": "gameplay", "TAB_CONTROLS": "controls",
         "TAB_NFC": "nfc", "TAB_MODS": "mods", "TAB_DEBUG": "debug", "TAB_MOTION": "motion", "TAB_SPEAKER": "controller_speaker", "TAB_FPS_DEV": "fps_details", "TAB_PAGE": "controls_page"}

GRAPHICS = [
    ("Resolution and upscaling", ["Aspect ratio", "Output resolution", "Profile", "Upscaler", "DLSS model", "Render resolution", "Sharpness"]),
    ("Frame generation and latency", ["Frame generation", "Frame generation by", "Game frame rate cap",
                                      "NVIDIA Reflex", "AMD Anti-Lag 2"]),
    ("Screen", ["Fullscreen", "Fullscreen monitor", "V-Sync", "Window scaling filter", "CRT filter", "CRT lines", "Ultrawide menus"]),
    ("HDR", ["HDR", "HDR paper white", "HDR peak brightness", "HDR contrast", "HDR tone mapper"]),
    ("Accessibility", ["Colour vision correction"]),
    ("Post-processing", ["ReShade preset", "Compare ReShade Preset", "User preset file", "Rescan ReShade presets"]),
    ("Detail", ["Anisotropic filtering", "Shadow resolution"]),
    ("GPU and memory", ["GPU device"]),
]


def function(text, marker):
    start = text.index(marker)
    depth = 0
    for at in range(text.index("{", start), len(text)):
        depth += text[at] == "{"
        depth -= text[at] == "}"
        if depth == 0:
            return text[start:at + 1]
    raise SystemExit("unterminated: " + marker)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    pages, diags = built_menu(args.cxx)
    if diags:
        raise SystemExit("FAIL: the menu as built has diagnostics: %s" % diags)
    rows = [(tab, it.label, it.value, it.heading, it.shown) for tab, page in PAGES.items() for it in pages.get(page, [])]
    if len(rows) < 40:
        raise SystemExit("FAIL: the menu's rows were not found (%d rows read)" % len(rows))

    # the values row_shown can hide, and a module's row with a shown() of its own: a section needs a row that is neither
    hidable = set(re.findall(r"&opts\.\w+", function(menu, "bool row_shown(int i) {")))
    # (#1633: a module's row with a shown() in its source: one asking the renderer is a MENU_HOOK, absent from the layout-only
    # build the rows above come from: the Upscaler's, Reflex's)
    module_shown = {row["label"].strip('"') for row in module_rows() if "shown" in row and "label" in row}
    if ("&opts.dlss" not in hidable and "Upscaler" not in module_shown) or not (module_shown or any(shown for *_, shown in rows)):
        raise SystemExit("FAIL: row_shown, or a module's shown(), no longer reads as expected (the Upscaler, HDR's levels)")

    tabs = {}
    for tab, name, value, heading, shown in rows:
        tabs.setdefault(tab, []).append((name, "(shown)" if shown or name in module_shown else value, heading))
    hidable.add("(shown)")
    for tab, entries in tabs.items():
        for i, (name, value, heading) in enumerate(entries):
            if not heading:
                continue
            section = []
            for later in entries[i + 1:]:
                if later[2]:
                    break
                section.append(later)
            if not section:
                raise SystemExit("FAIL: %s: the section %r has no rows" % (tab, name))
            if all(v in hidable for _, v, _ in section):
                raise SystemExit("FAIL: %s: every row of the section %r can be hidden (row_shown): a GPU without them "
                                 "would show its title over nothing" % (tab, name))

    graphics = tabs.get("TAB_GRAPHICS", [])
    if not graphics or graphics[0][0] != "Quality preset" or graphics[0][2]:
        raise SystemExit("FAIL: the Graphics tab does not start with the Quality preset")
    titles = [name for name, _, heading in graphics if heading]
    want = [title for title, _ in GRAPHICS]
    if titles != want:
        raise SystemExit("FAIL: the Graphics tab's sections are %r, not %r" % (titles, want))
    section_of, current = {}, None
    for name, value, heading in graphics:
        if heading:
            current = name
        elif current:
            section_of[name] = current
    for title, members in GRAPHICS:
        for name in members:
            if section_of.get(name) != title:
                raise SystemExit("FAIL: Graphics: %r is under %r, not %r" % (name, section_of.get(name), title))

    # the heading rows are drawn as titles and stepped over by the keys (no code of their own per tab)
    draw = function(menu, "void draw_options(ui::Ctx& c) {")
    if "if (r.heading) {" not in draw:
        raise SystemExit("FAIL: draw_options no longer draws heading rows as titles")
    tick = function(menu, "void menu_tick() {")
    if "while (heading_at(selected));" not in tick:
        raise SystemExit("FAIL: menu_tick's keys no longer step over heading rows")
    print("menu sections: OK (%d headings; Graphics in %d sections)" % (sum(r[3] for r in rows), len(titles)))


if __name__ == "__main__":
    main()
