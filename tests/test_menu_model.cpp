// The menu's model (gpu/vk/menu_model.h), compiled with this and run by tests/test_menu_model.py: the order build() works out
// from modules' wishes (after, before, first, last, order, at, place), the same whatever order the modules come in; what it
// diagnoses (a missing anchor, a cycle, a doubled key, a page or section no module made); and what a presenter sees and does
// (shown, locked, the values' text and translation, stepping, actions, pages, custom items). No game, no ImGui.
#include "../gpu/vk/menu_model.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace menu = mkwvk::menu;
using menu::Input;
using menu::Kind;

static int failures = 0;
#define CHECK(cond)                                                                                                           \
    do {                                                                                                                      \
        if (!(cond)) {                                                                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                   \
            failures++;                                                                                                       \
        }                                                                                                                     \
    } while (0)

static void quiet(const char*) {}

/// The page's items as one string of keys ("#" before a heading's), for comparing orders
static std::string keys(const char* page) {
    std::string out;
    for (const auto& v : menu::snapshot(page)) {
        if (!out.empty()) out += ' ';
        if (v.kind == Kind::Heading) out += '#';
        out += v.key ? v.key : "-";
    }
    return out;
}
static std::string tab_list() {
    std::string out;
    for (const char* t : menu::tabs()) out += (out.empty() ? "" : " ") + std::string(t);
    return out;
}
static bool diagnosed(const char* part) {
    for (const auto& d : menu::diagnostics())
        if (d.find(part) != std::string::npos) return true;
    return false;
}
static void show_diagnostics() {
    for (const auto& d : menu::diagnostics()) fprintf(stderr, "  diagnostic: %s\n", d.c_str());
}

// ---- a small menu: the core's two tabs, and modules adding to them ----

static int vsync = 1, quality = 2, volume = 50, fps = 60, mono = 0, hidden_on = 0, locked_on = 0, extra = 0;
static int round_v = 0;   // (a Slider that wraps)
static int changes = 0, last_change = -1, actions = 0;
static bool show_x = true;

static menu::Setting choice(const char* key, int* v) {
    menu::Setting s;
    s.key = key;
    s.label = key;
    s.value = v;
    s.choices = {"Off", "On"};
    return s;
}

static void core() {
    menu::tab("video", "Video", menu::order(100))
        .heading("display", "Display", menu::order(100))
        .row(choice("vsync", &vsync))
        .row({.key = "quality", .label = "Quality", .help = "How it looks", .value = &quality,
              .choices = {"Low", "Medium", "High"}, .restart = true,
              .on_change = [](int v) { changes++, last_change = v; }})
        .row(choice("c", &extra))
        .heading("frame", "Frame rate", menu::order(200))
        .row({.key = "fps", .label = "Frame rate cap", .kind = Kind::Slider, .value = &fps, .lo = 30, .hi = 120, .step = 30,
              .text = [](int v) { return std::to_string(v) + " fps"; }})
        .row(choice("q2", &extra));
    menu::tab("audio", "Audio", menu::order(200))
        .row({.key = "volume", .label = "Volume", .kind = Kind::Slider, .value = &volume, .lo = 0, .hi = 100, .step = 10,
              .host_option = 7})
        .row({.key = "mono", .label = "Mono", .kind = Kind::Toggle, .value = &mono});
}
static void extras() {
    // (declared in Frame rate, but after vsync: it goes to Display, right after vsync)
    menu::tab("video").section("frame").row(choice("x", &extra), menu::after("vsync"));
    menu::tab("video").section("display").row(choice("y", &extra), menu::before("c"))
        .row(choice("z", &extra), menu::first())
        .row(choice("x2", &extra), menu::after("vsync"))   // (beside x, after it: described later)
        .row(choice("x1", &extra), menu::after("x"));      // (a chain: right after x)
    menu::tab("video").section("frame").row(choice("w", &extra), menu::at(1));
    menu::tab("tools", "Tools", menu::before("audio"));
}

static void ordering() {
    menu::build({{"core", core}, {"extras", extras}});
    show_diagnostics();
    CHECK(menu::diagnostics().empty());
    CHECK(tab_list() == "video tools audio");
    CHECK(keys("video") == "#display z vsync x x1 x2 quality y c #frame fps w q2");
    CHECK(keys("audio") == "volume mono");
    // (the same menu whatever order the modules come in, and every time)
    const std::string first_build = keys("video");
    menu::build({{"extras", extras}, {"core", core}});
    CHECK(keys("video") == first_build && tab_list() == "video tools audio");
    menu::build({{"core", core}, {"extras", extras}});
    CHECK(keys("video") == first_build);
}

// ---- what build() diagnoses ----

static int bad = 0;
static void broken() {
    menu::tab("t", "T")
        .row(choice("a", &bad), menu::after("nowhere"))   // (a missing anchor: the end)
        .row(choice("b", &bad))
        .row(choice("p", &bad), menu::after("q"))         // (a cycle: p after q after p)
        .row(choice("q", &bad), menu::after("p"))
        .row(choice("b", &bad))                           // (a doubled key)
        .row({.label = "No key", .value = &bad})          // (a saved row with no key)
        .row({.key = "opener", .label = "Open", .kind = Kind::Page, .opens = "nothing"});
    menu::tab("ghost").row(choice("g", &bad));            // (a tab no module made)
    menu::tab("t").section("nosuch").row(choice("s", &bad));
    menu::tab("t").row({.key = "o1", .label = "O1", .value = &bad, .host_option = 3})
        .row({.key = "o2", .label = "O2", .value = &bad, .host_option = 3});
    menu::place("missing_key", menu::first());
}
static void diagnostics() {
    menu::build({{"broken", broken}});
    CHECK(diagnosed("row a in page t: after nowhere, which isn't there; put at the end"));
    CHECK(diagnosed("a cycle of anchors (p -> q -> p); p put by its own order") ||
          diagnosed("a cycle of anchors (q -> p -> q); p put by its own order"));
    CHECK(diagnosed("key b: two items"));
    CHECK(diagnosed("No key (from broken): a saved row with no key"));
    CHECK(diagnosed("opener: opens page nothing, which no module made"));
    CHECK(diagnosed("page ghost: added to (by broken), but no module made it"));
    CHECK(diagnosed("s (from broken): in section nosuch on page t, which no module made (heading); put in the page's own"));
    CHECK(diagnosed("host option 3: two settings (o1 and o2)"));
    CHECK(diagnosed("place(missing_key): no item, section or tab has that key"));
    // (still a menu: every row is somewhere, the missing anchor's at the end, the cycle's both there)
    const std::string t = keys("t");
    CHECK(t == "b p q b - opener s o1 o2 a");
    CHECK(t.find("p q") != std::string::npos);
    CHECK(t.find(" s") != std::string::npos);
    menu::build({{"same", core}, {"same", extras}});
    CHECK(diagnosed("module same: registered twice"));
}

// ---- what a presenter sees and does ----

static const char* french(const char* id) {
    if (!strcmp(id, "Display")) return "Affichage";
    if (!strcmp(id, "On")) return "Oui";
    if (!strcmp(id, "Off")) return "Non";
    if (!strcmp(id, "Busy now")) return "Occupé";
    return nullptr;   // (not translated: the id)
}
static void presenting() {
    const auto views = [] {
        menu::tab("p", "P")
            .heading("h1", "Display")
            .row({.key = "hidden", .label = "Hidden", .value = &hidden_on, .choices = {"Off", "On"},
                  .shown = [] { return false; }})
            .heading("h2", "Second")
            .row({.key = "shown_x", .label = "X", .value = &vsync, .choices = {"Off", "On"},
                  .shown = [] { return show_x; }})
            .line("note", "A line")
            .row({.key = "locked", .label = "Locked", .value = &locked_on, .choices = {"Off", "On"},
                  .locked = [] { return "Busy now"; }})
            .row({.key = "act", .label = "Do it", .kind = Kind::Action, .on_activate = [] { actions++; }})
            .link("more", "More", "sub")
            .row({.key = "ends", .label = "Ends", .value = &extra, .lo = 0, .hi = 2, .stops_at_ends = true})
            .row({.key = "odd", .label = "Odd", .value = &mono, .lo = 1, .hi = 9,
                  .hint = [](int v, int variant) { return variant <= 0 && v > 5 ? "High" : nullptr; },
                  .step_fn = [](int v, int dir) { return v + 2 * dir; }})
            .custom({.draw = [](menu::Presenter&, menu::Rect) { return false; },
                     .input = [](Input in) { return in == Input::Activate; },
                     .fallback = {.key = "grid", .label = "In the Insert menu only"}});
        menu::page("sub", "Sub page").row(choice("deep", &extra));
        menu::page("round", "Round page")   // (a Slider that wraps: a page of its own, the others' items as checked)
            .row({.key = "round", .label = "Round", .kind = Kind::Slider, .value = &round_v, .lo = 0, .hi = 100, .step = 10,
                  .wraps = true});
    };
    menu::build({{"core", core}, {"views", +views}});
    show_diagnostics();
    CHECK(menu::diagnostics().empty());
    CHECK(keys("p") == "#h2 shown_x note locked act more ends odd grid");   // (h1's only row hidden: no heading)
    show_x = false;
    CHECK(keys("p") == "#h2 note locked act more ends odd grid");
    {   // (the layout: every item, shown or not, a heading's key its section's id)
        std::string all;
        for (const auto& it : menu::layout("p")) all += std::string(all.empty() ? "" : " ") + (it.heading ? "#" : "") + it.key;
        CHECK(all == "#h1 hidden #h2 shown_x note locked act more ends odd grid");
        CHECK(menu::layout("p")[1].setting == menu::find("hidden") && !menu::layout("p")[0].setting);
    }
    show_x = true;
    CHECK(menu::tabs().size() == 3 && keys("sub") == "deep");   // (a page is not a tab)

    // (text: translated by the model; a value's text() as it gives it)
    menu::set_translator(french);
    auto v = menu::snapshot("video");
    CHECK(v[0].kind == Kind::Heading && v[0].label == "Affichage");
    vsync = 1;
    v = menu::snapshot("video");
    CHECK(v[1].key && !strcmp(v[1].key, "vsync") && v[1].value == "Oui" && v[1].raw == 1);
    CHECK(v[2].label == "Quality" && v[2].value == "High" && v[2].help == "How it looks" && v[2].restart);
    CHECK(v[5].value == "60 fps");
    const auto pv = menu::snapshot("p");
    CHECK(pv[3].locked == "Occupé" && pv[3].selectable);
    CHECK(!pv[0].selectable && !pv[2].selectable && pv[2].kind == Kind::Line);
    CHECK(pv[8].custom && pv[8].kind == Kind::Line && pv[8].selectable && pv[8].label == "In the Insert menu only");
    CHECK(menu::custom_of("grid") && menu::custom_of("grid")->draw && !menu::custom_of("vsync"));
    CHECK(!strcmp(menu::page_label("sub"), "Sub page") && !menu::page_label("nope"));
    menu::set_translator(nullptr);

    // (stepping: a Choice wraps, a Slider and stops_at_ends hold at the ends, step_fn steps its own way)
    vsync = 1;
    auto o = menu::input_key("video", "vsync", Input::Right);
    CHECK(o.taken && o.changed && vsync == 0);
    o = menu::input_key("video", "vsync", Input::Left);
    CHECK(o.changed && vsync == 1);
    quality = 2, changes = 0;
    o = menu::input_key("video", "quality", Input::Right);
    CHECK(o.changed && o.restart && quality == 0 && changes == 1 && last_change == 0);
    fps = 120;
    o = menu::input_key("video", "fps", Input::Right);
    CHECK(o.taken && !o.changed && fps == 120);
    o = menu::input_key("video", "fps", Input::Left);
    CHECK(o.changed && fps == 90);
    o = menu::input_key("video", "fps", Input::Activate);
    CHECK(!o.taken && fps == 90);   // (a slider does nothing on Activate)
    extra = 2;
    o = menu::input_key("p", "ends", Input::Right);
    CHECK(o.taken && !o.changed && extra == 2);
    round_v = 100;   // (a Slider that wraps goes round, as a Choice)
    o = menu::input_key("round", "round", Input::Right);
    CHECK(o.changed && round_v == 0);
    o = menu::input_key("round", "round", Input::Left);
    CHECK(o.changed && round_v == 100);
    mono = 5;
    o = menu::input_key("p", "odd", Input::Right);
    CHECK(o.changed && mono == 7 && menu::snapshot("p")[7].hint == "High");
    mono = 0;
    o = menu::input_key("audio", "mono", Input::Activate);
    CHECK(o.changed && mono == 1 && menu::snapshot("audio")[1].value == "On");
    o = menu::input_key("audio", "mono", Input::Activate);
    CHECK(mono == 0 && menu::snapshot("audio")[1].value == "Off");
    // (locked: nothing changes)
    locked_on = 0;
    o = menu::input_key("p", "locked", Input::Right);
    CHECK(!o.taken && locked_on == 0);
    // (actions, pages, back, the custom item's own input, a hidden row)
    actions = 0;
    o = menu::input_key("p", "act", Input::Activate);
    CHECK(o.taken && actions == 1);
    o = menu::input_key("p", "more", Input::Activate);
    CHECK(o.taken && o.open && !strcmp(o.open, "sub"));
    o = menu::input("p", 1, Input::Back);
    CHECK(o.taken && o.back);
    o = menu::input_key("p", "grid", Input::Activate);
    CHECK(o.taken && !o.changed);
    o = menu::input_key("p", "hidden", Input::Right);
    CHECK(!o.taken && hidden_on == 0);
    // (Up and Down: the next selectable item, past headings and lines, wrapping)
    o = menu::input("p", 1, Input::Down);
    CHECK(o.select == 3);   // (shown_x -> past the line -> locked)
    o = menu::input("p", 1, Input::Up);
    CHECK(o.select == 8);   // (wraps past the heading to the custom item)
    o = menu::input("p", 8, Input::Down);
    CHECK(o.select == 1);
    // (the host's way in)
    CHECK(menu::find("volume") && menu::find("volume")->value == &volume && menu::find_option(7) == menu::find("volume"));
    CHECK(!menu::find("nope") && !menu::find_option(99));
}

// ---- a layout's word over a module's ----

static void layout() {
    menu::place("y", menu::last(), "video", "frame");   // (y to the end of Frame rate)
    menu::place("tools", menu::first());                  // (the Tools tab first)
    menu::place("frame", menu::first(), "video");         // (Frame rate before Display)
}
static void overrides() {
    menu::build({{"core", core}, {"extras", extras}, {"zz_layout", layout}});
    show_diagnostics();
    CHECK(menu::diagnostics().empty());
    CHECK(tab_list() == "tools video audio");
    CHECK(keys("video") == "#frame fps w q2 y #display z vsync x x1 x2 quality c");
}

// ---- MENU_MODULE: registered at start-up, run sorted by name ----

static int reg = 0;
MENU_MODULE(b_second) { menu::tab("reg").row(choice("from_b", &reg)); }
MENU_MODULE(a_first) { menu::tab("reg", "Registered").row(choice("from_a", &reg)); }

static void registered() {
    menu::build();
    show_diagnostics();
    CHECK(menu::diagnostics().empty());
    CHECK(tab_list() == "reg" && keys("reg") == "from_a from_b");
}

int main() {
    menu::set_log(quiet);
    ordering();
    diagnostics();
    presenting();
    overrides();
    registered();
    if (failures) {
        fprintf(stderr, "%d failed\n", failures);
        return 1;
    }
    printf("menu model: every check passed\n");
    return 0;
}
