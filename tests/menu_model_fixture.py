"""For the checks of the Insert menu (#1633), whose rows are gpu/vk/vk_menu.cpp's own (the core module) and the feature modules'
(gpu/vk/menu/*.cpp, menu_model.h). Standard library only.

    sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
    from menu_model_fixture import menu_model_prelude, menu_model_member, menu_sources, built_menu

menu_model_prelude(root): the #include of root's gpu/vk/menu_model.h and `namespace menu = mkwvk::menu;`, to put first in a
fixture that compiles vk_menu.cpp's Row, ini_key, row_shown, is_slider or set_row by their text (a row there can be a
module's, Row.setting); "" for a checkout without the model (an older one a check is run against, --source).
menu_model_member(root): the member a fixture's own Row stub needs for those to compile, "" likewise.

menu_sources(root): the menu's source text, vk_menu.cpp's and every module's, for a check that looks for the menu's texts.

built_menu(cxx): the menu as the model orders it, tab by tab: the model and every module compiled (gpu/vk/menu_module.h with
vk_menu.h's Options in place of vk_menu.h, no Vulkan; MENU_LAYOUT_ONLY, so a module's hooks that ask the renderer, MENU_HOOK, are none) over a stand-in for the core module made from vk_menu.cpp's own text
(core_module), run, and read back: {page: [Item]}, plus build()'s diagnostics. For a check of the menu's order or sections.
"""
from collections import namedtuple
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from srccut import block  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
TABS = ["TAB_GRAPHICS", "TAB_AUDIO", "TAB_GAMEPLAY", "TAB_CONTROLS", "TAB_NFC", "TAB_MODS", "TAB_DEBUG", "TAB_MOTION", "TAB_SPEAKER", "TAB_FPS_DEV", "TAB_RECORDINGS", "TAB_SIM_TURF", "TAB_PAGE"]

# (an item of a page as built: heading, its key (a section's id), label, shown (a module's row with a shown() of its own, which
# can hide it), and value: the core row's value expression in vk_menu.cpp ("&opts.x", "nullptr"), "" for a module's)
Item = namedtuple("Item", "heading key label shown value")


def _vk(root):
    return Path(root or ROOT) / "gpu" / "vk"


def menu_model_prelude(root=None):
    header = _vk(root) / "menu_model.h"
    if not header.exists():
        return ""
    return '#include "%s"\nnamespace menu = mkwvk::menu;\n' % header.resolve().as_posix()


def menu_model_member(root=None):
    return "const menu::Setting* setting = nullptr; " if (_vk(root) / "menu_model.h").exists() else ""


def menu_sources(root=None):
    vk = _vk(root)
    return "\n".join(p.read_text(encoding="utf-8") for p in [vk / "vk_menu.cpp", vk / "menu_module.h"] +
                     sorted((vk / "menu").glob("*.cpp")))


TAB_OF_PAGE = {"graphics": "TAB_GRAPHICS", "audio": "TAB_AUDIO", "gameplay": "TAB_GAMEPLAY", "controls": "TAB_CONTROLS",
               "nfc": "TAB_NFC", "mods": "TAB_MODS", "debug": "TAB_DEBUG", "motion": "TAB_MOTION", "controller_speaker": "TAB_SPEAKER",
               "fps_details": "TAB_FPS_DEV", "recordings": "TAB_RECORDINGS", "sim_turf_settings": "TAB_SIM_TURF",
               "controls_page": "TAB_PAGE"}


def _fields(text):
    """A C++ initializer's fields at its top level (strings, parens, braces, comments respected)"""
    out, cur, depth, i = [], [], 0, 0
    while i < len(text):
        c = text[i]
        if c == '"':
            j = i + 1
            while text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            cur.append(text[i:j + 1])
            i = j + 1
            continue
        if text.startswith("//", i):
            i = text.find("\n", i) if text.find("\n", i) >= 0 else len(text)
            continue
        depth += c in "({["
        depth -= c in ")}]"
        if c == "," and depth == 0:
            out.append("".join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    if "".join(cur).strip():
        out.append("".join(cur).strip())
    return out


def module_rows(root=None):
    """The rows the modules describe (gpu/vk/menu/*.cpp), in each file's order: dicts of their designated fields (as
    written) and "page", "file". A row is a .row({...}) after a menu::tab("x") or menu::page("x")"""
    out = []
    for path in sorted((_vk(root) / "menu").glob("*.cpp")):
        text = path.read_text(encoding="utf-8")
        page = None
        for m in re.finditer(r'menu::(?:tab|page)\("(\w+)"|\.row\(\{', text):
            if m.group(1):
                page = m.group(1)
                continue
            depth, i = 1, m.end()
            while depth:
                c = text[i]
                if c == '"':
                    j = i + 1
                    while text[j] != '"':
                        j += 2 if text[j] == "\\" else 1
                    i = j + 1
                    continue
                depth += c == "{"
                depth -= c == "}"
                i += 1
            row = {"page": page, "file": path.name}
            for f in _fields(text[m.end():i - 1]):
                if f.startswith(".") and "=" in f:
                    name, value = f[1:].split("=", 1)
                    row[name.strip()] = value.strip()
            out.append(row)
    return out


def unhook(text):
    """MENU_HOOK(x) as x (menu_module.h)"""
    return text[len("MENU_HOOK("):-1] if isinstance(text, str) and text.startswith("MENU_HOOK(") and text.endswith(")") else text


def table_line(row):
    """A module's row as vk_menu.cpp's table wrote one: {TAB_X, "Label", value, lo, hi, step, text, help, hint, restart,
    preset_exempt} (the text function by its name: tx_onoff for a Toggle with no text of its own, tx_percent for
    menu_text::percent, the tx_ function a .text lambda returns)"""
    row = {k: unhook(v) for k, v in row.items()}   # (a hook that asks the renderer, MENU_HOOK(...): what it wraps)
    text = row.get("text", "")
    m = re.match(r"\[\]\(int v\) \{ return std::string\((\w+)\(v\)\); \}$", text)
    name = m.group(1) if m else "tx_percent" if text == "menu_text::percent" else text or (
        "tx_onoff" if row.get("kind") == "menu::Kind::Toggle" else "nullptr")
    value = row.get("value", "nullptr")
    lo, hi, step = (row.get("lo", "0"), row.get("hi", "1"), row.get("step", "1")) if value != "nullptr" else ("0", "0", "0")
    help_ = re.sub(r'"\s+"', "", row.get("help", "nullptr"))
    return "    {%s, %s, %s, %s, %s, %s, %s,\n     %s, %s, %s, %s}," % (
        TAB_OF_PAGE.get(row["page"], "TAB_" + str(row["page"]).upper()), row.get("label", '""'), value, lo, hi, step, name,
        help_, row.get("hint", "nullptr"), row.get("restart", "false"), row.get("preset_exempt", "false"))


def output_engine(root=None, end=None):
    """The output engine as vk_menu.cpp had it from struct Out, for a check that compiles part of it (#1633: the output
    module's now): struct Out and kGameOutput (gpu/vk/menu/output.h), the display_modes alias, then gpu/vk/menu/output.cpp's
    engine from the Output resolution's choices up to end (a marker of output.cpp; by default the engine's end, before the
    rows' own texts)"""
    vk = _vk(root)
    header = (vk / "menu" / "output.h").read_text(encoding="utf-8")
    at = header.index("struct Out {")
    types = header[at:header.index("\n", header.index("inline constexpr Out kGameOutput", at)) + 1]
    src = (vk / "menu" / "output.cpp").read_text(encoding="utf-8")
    start = src.index("// The Output resolution's choices:")
    stop = src.index(end or "const char* tx_output_res(int v) {", start)
    # (picture_for's default, which output.h's declaration carries, on its definition as vk_menu.cpp had it)
    engine = src[start:stop].replace("Out picture_for(int v, bool* native) {", "Out picture_for(int v, bool* native = nullptr) {")
    return types.replace("inline constexpr", "constexpr") + "namespace dm = display_modes;\n" + engine


def unbroken(text):
    """text without vk_menu.cpp's breaks out of its anonymous namespace (#1633: what modules ask the renderer, outside it):
    for a span a check compiles that crosses one"""
    return text.replace('} // namespace   // (#1633: asked by the modules too, menu_module.h\'s "what modules ask the renderer")\n', "").replace('namespace {   // (#1633: the anonymous namespace again, after what the modules ask)\n', "")


def menu_text(root=None):
    """The menu's sources (menu_sources) and then the modules' rows as vk_menu.cpp's table lines (table_line): for a check
    that reads a row, or a tx_ or hn_ function, by its text wherever it is described now (#1633)"""
    return (menu_sources(root) + "\n// (the modules' rows as table lines: tests/menu_model_fixture.py menu_text)\nRow module_rows[] = {\n" +
            "\n".join(table_line(r) for r in module_rows(root)) + "\n};\n")


def core_rows(menu):
    """vk_menu.cpp's own rows in the table's order: (tab, label, value expression, heading)"""
    table = re.sub(r"//[^\n]*", "", block(menu, "Row rows[] = {"))
    rows = re.findall(r'\{\s*(TAB_\w+)\s*,\s*"((?:[^"\\]|\\.)*)"\s*,\s*(&[\w.]+|nullptr)', table)
    headings = set(re.findall(r'\{\s*TAB_\w+\s*,\s*"((?:[^"\\]|\\.)*)"\s*,\s*nullptr\s*,\s*0\s*,\s*0\s*,\s*0\s*,\s*nullptr\s*,\s*nullptr'
                              r'\s*,\s*nullptr\s*,\s*(?:true|false)\s*,\s*(?:true|false)\s*,\s*-1\s*,\s*true\s*\}', table))
    return [(tab, name, value, name in headings and value == "nullptr") for tab, name, value in rows]


def core_module(menu):
    """The core module as vk_menu.cpp registers it (MENU_MODULE(core)), from its text: C++, and the value of each row by its
    key ({key: value expression})"""
    tab_ids = re.findall(r'"([a-z_]+)"', block(menu, "const char* const kTabIds[TAB_PAGE + 1] = {"))
    sections = dict(re.findall(r'\{"([^"]+)", "([a-z_]+)"\}', block(menu, "const char* const kCoreSections[][2] = {")))
    if len(tab_ids) != len(TABS) or not sections:
        raise SystemExit("vk_menu.cpp: kTabIds or kCoreSections not as menu_model_fixture.py reads them")
    fn = block(menu, "const char* ini_key(const Row& r) {")
    # (its table of labels and keys, while one was left: every saved row is a module's now, #1633)
    keys = dict(re.findall(r'\{"((?:[^"\\]|\\.)*)", "([^"]+)"\}', fn[:fn.index("};")])) if "};" in fn else {}
    by_value = dict(re.findall(r'if \(r\.value == (&[\w.]+)\) return "([^"]+)";', fn))
    rows = core_rows(menu)
    values = {}
    out = ["[[maybe_unused]] static int core_value[%d];   // (the core's value rows: none left, #1633)" % max(1, len(rows)), "MENU_MODULE(core) {"]
    for t, tab in enumerate(TABS):
        label = tab_ids[t]
        out.append('    { auto b = %s;' % ('menu::tab("%s", "%s", menu::order(%d))' % (label, label, (t + 1) * 100) if t < 6 else
                                           'menu::page("%s", "%s")' % (label, label)))
        for i, (row_tab, name, value, heading) in enumerate(rows):
            if row_tab != tab:
                continue
            if heading:
                key = sections.get(name, "@" + name)
                out.append('      b.heading("%s", "%s");' % (key, name))
            elif value != "nullptr":
                key = by_value.get(value) or keys.get(name)
                if not key:
                    raise SystemExit("vk_menu.cpp: the row %s has no key in ini_key's table" % name)
                out.append('      b.row({.key = "%s", .label = "%s", .value = &core_value[%d]});' % (key, name, i))
            else:
                key = "@" + name
                out.append('      b.row({.key = "%s", .label = "%s", .kind = menu::Kind::Action});' % (key, name))
            values[key] = value
        out.append("      (void)b;")   # (a page whose rows all moved to modules)
        out.append("    }")
    out.append("}")
    return "\n".join(out), values


def module_header(root=None):
    """gpu/vk/menu_module.h as a module compiles it with no Vulkan: vk_menu.h's Options in place of vk_menu.h"""
    vk = _vk(root)
    module_h = (vk / "menu_module.h").read_text(encoding="utf-8")
    if '#include "vk_menu.h"\n' not in module_h:
        raise SystemExit("gpu/vk/menu_module.h does not include vk_menu.h as menu_model_fixture.py replaces it")
    options = block((vk / "vk_menu.h").read_text(encoding="utf-8"), "struct Options {") + ";\n"
    return ('#pragma once\n#include "%s"\nnamespace mkwvk {\n%s\nextern Options opts;\n}\n' % ((vk / "menu_model.h").as_posix(), options) +
            module_h.split('#include "vk_menu.h"\n', 1)[1])


def renderer_stand_ins(root=None):
    """C++ standing in for what modules ask the renderer (menu_module.h's section of it): each function returning nothing,
    "" or 0"""
    module_h = (_vk(root) / "menu_module.h").read_text(encoding="utf-8")
    start = module_h.index("// ---- what modules ask the renderer")
    section = module_h[start:module_h.index("// ---- end of what modules ask the renderer", start)]
    out = ["namespace mkwvk {"]
    for line in section.splitlines():
        if re.match(r"namespace \w+ \{", line) or line.startswith("}"):   # (a namespace of them: i18n)
            out.append(line.split("//")[0].strip())
            continue
        m = re.match(r"([\w:<>*& ]+?)\s*\b(\w+)\(([^)]*)\);", line)
        if not m:
            continue
        ret, name, params = m.groups()
        value = '""' if ret.replace(" ", "") == "constchar*" else "" if ret.strip() == "void" else "{}"
        unnamed = ", ".join(re.sub(r"\s*\w+$", "", p.strip()) for p in params.split(",") if p.strip())
        out.append("%s %s(%s) { return %s; }" % (ret.strip(), name, unnamed, value) if value else
                   "%s %s(%s) {}" % (ret.strip(), name, unnamed))
    out.append("}")
    return "\n" + "\n".join(out) + "\n"


def compile_menu(cxx, main_cpp, tmp, root=None):
    """The model, every module and main_cpp (which includes "menu_module.h" and has // CORE where the core module goes)
    compiled in tmp: the executable's path"""
    vk = _vk(root)
    core, _ = core_module((vk / "vk_menu.cpp").read_text(encoding="utf-8"))
    (Path(tmp) / "menu_module.h").write_text(module_header(root), encoding="utf-8")
    (Path(tmp) / "main.cpp").write_text(main_cpp.replace("// CORE", core) + renderer_stand_ins(root), encoding="utf-8")
    exe = Path(tmp) / "menu"
    # (the stand-in menu_module.h first, then gpu/vk for a module's own includes: fps_stats.h)
    subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-DMENU_LAYOUT_ONLY", "-I" + str(tmp), "-I" + str(vk),
                    str(Path(tmp) / "main.cpp")] +
                   [str(m) for m in sorted((vk / "menu").glob("*.cpp"))] + [str(vk / "menu_model.cpp"), "-o", str(exe)],
                   check=True)
    return exe


LAYOUT_MAIN = r'''#include "menu_module.h"
#include <cstdio>
namespace mkwvk { Options opts; }
using namespace mkwvk;
// CORE
int main() {
    mkwvk::menu::set_log([](const char*) {});
    mkwvk::menu::build();
    for (const auto& d : mkwvk::menu::diagnostics()) printf("D\t%s\n", d.c_str());
    for (const char* page : {"graphics", "audio", "gameplay", "controls", "nfc", "mods", "debug", "motion", "controller_speaker", "fps_details",
                             "sim_turf_settings", "controls_page"})
        for (const auto& it : mkwvk::menu::layout(page))
            printf("I\t%s\t%d\t%s\t%s\t%d\n", page, it.heading ? 1 : 0, it.key ? it.key : "", it.label ? it.label : "",
                   it.setting && it.setting->shown ? 1 : 0);
    return 0;
}
'''


def built_menu(cxx, root=None):
    """({page: [Item]}, [diagnostic]) of the menu as built"""
    _, values = core_module((_vk(root) / "vk_menu.cpp").read_text(encoding="utf-8"))
    with tempfile.TemporaryDirectory(prefix="menu-built-") as tmp:
        exe = compile_menu(cxx, LAYOUT_MAIN, tmp, root)
        out = subprocess.run([str(exe)], check=True, capture_output=True, encoding="utf-8").stdout
    pages, diags = {}, []
    for line in out.splitlines():
        cells = line.split("\t")
        if cells[0] == "D":
            diags.append(cells[1])
        elif cells[0] == "I":
            _, page, heading, key, label, shown = cells
            pages.setdefault(page, []).append(Item(heading == "1", key, label, shown == "1", values.get(key, "")))
    return pages, diags
