// The menu's model (#1633, step 2): its pages, sections and settings, described once with their own hooks, and no ImGui or
// Vulkan in it. A module describes its own rows in its own file (MENU_MODULE) and says where they go (after, before, first,
// last, order, at); build() collects every module's rows, then works the order out once, the same for every build; a
// presenter (the ImGui menu now, the game's own menu later) draws snapshot(page) and hands its input to input(). Built and
// used on one thread (the menu's): it takes no locks. tests/test_menu_model.cpp checks it, with no game.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace mkwvk::menu {

enum class Kind {
    Choice,    // one of choices (or lo..hi, its text from text()); Left and Right step it, wrapping at the ends
    Toggle,    // off or on (choices, when given, name them); Left, Right and Activate flip it
    Slider,    // lo..hi in steps of step, held at its ends
    Action,    // a button: Activate calls on_activate
    Page,      // opens another page (opens)
    Heading,   // a section's title (made by heading(); not a row of its own)
    Line,      // a line of text, never selected
    Custom,    // drawn by its own presenter's code (Custom); a presenter without it shows the fallback
};
enum class Input { Up, Down, Left, Right, Activate, Back };

/// One row of the menu. key is its line in longshot_options.ini and its name over the control bridge: written down, never
/// made from the label, so renaming a row never moves its setting. label, help, choices and a locked reason are text ids
/// (today the English text), translated by the model, as hint()'s are; text() gives text already finished.
struct Setting {
    const char* key = nullptr;
    const char* label = nullptr;
    const char* help = nullptr;
    Kind kind = Kind::Choice;
    int* value = nullptr;
    int lo = 0, hi = 1, step = 1;
    std::vector<const char*> choices = {};   // a Choice's or Toggle's names, lo + i for choices[i]
    int host_option = -1;               // the HOS_OPT_* id the host reads it by, -1 none
    bool restart = false;               // a change waits for a restart
    bool preset_exempt = false;         // the Quality preset leaves it alone
    bool stops_at_ends = false;         // a Choice held at its ends, as a Slider, instead of wrapping
    bool wraps = false;                 // a Slider that goes round past its ends, as a Choice (Sharpness, as it always has)
    const char* opens = nullptr;        // a Page row: the page it opens
    std::function<bool()> shown = {};   // hidden while it gives false
    std::function<const char*()> locked = {};   // why it can't change now (a text id), or nullptr
    std::function<std::string(int v)> text = {};   // the value's text, when choices don't name it
    // (a line about the value (a text id), or nullptr: hint(v, -1) the one to show now, hint(v, 0), hint(v, 1), ... every one
    // v can have, until nullptr, so a presenter can keep the height of the tallest)
    std::function<const char*(int v, int variant)> hint = {};
    std::function<int(int v, int dir)> step_fn = {};   // the value one step in dir (-1, 1) from v, when it isn't v + step
    std::function<void(int v)> on_change = {};   // after the value changed
    std::function<void()> on_activate = {};
    std::function<int(int v)> restart_value = {};  // what a restart compares, when not v (two values that run the same)
    const char* verb = nullptr;                    // what Activate does, for a presenter's prompt ("Save", "Tap"; a text id)
    const void* tag = nullptr;          // the describer's own (the ImGui menu: its record of a row it still describes itself)
    bool confirm = false;               // an Action that asks for a second Activate before it acts (Restart, Quit)
    bool commit_on_release = false;     // a heavy Slider: dragging updates visual preview only; commits on mouse release
    bool unsaved = false;               // a UI-only row synced to another saved option (skipped by save() and load())
    std::function<int()> sync = {};                       // refreshes *value from underlying option state before draw/input
    std::function<int()> slider_current = {};             // current slider stop ID when distinct from *value (e.g. Native)
    std::function<int(int v)> slider_permille = {};       // track position (0..1000) for a non-linear or capped Slider
    std::function<int(int permille)> slider_value = {};   // stop value for permille (0..1000) on a non-linear or capped Slider
    std::function<std::string(int v)> slider_text = {};   // value text for slider preview/stop ID when distinct from text()
};

/// A presenter: whatever draws a snapshot (the ImGui menu, the game's menu). A custom item asks it for its name.
struct Presenter {
    virtual ~Presenter() = default;
    virtual const char* name() const = 0;
};
struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
};
/// An item drawn by its own code (the Controls grid, the microphone's meter). draw gives false when it can't draw for that
/// presenter, which then shows the fallback (a Line "In the Insert menu only", say) and leaves input to the model.
struct Custom {
    std::function<bool(Presenter&, Rect)> draw = {};
    std::function<bool(Input)> input = {};   // true when the custom item took the input
    Setting fallback = {};                    // its key, label and kind (Line by default) stand for the item everywhere
};

/// Where an item goes among its group's (a page's tabs, a page's sections, a section's rows). Without one: by order (0),
/// then in the order the items were described (modules run sorted by name), then by key.
struct Place {
    enum class Pin { First, None, Last };
    Pin pin = Pin::None;
    int weight = 0;
    const char* after = nullptr;    // right after this key (and the rows placed after it): the row moves to its section
    const char* before = nullptr;   // right before this key
    int at = -1;                    // a soft position among the group's items (0 the first), for an item with no anchor
    /// The same place with a weight: the items beside one anchor go in their weights' order, whichever module they come
    /// from (before("x").order(20))
    Place order(int w) const {
        Place p = *this;
        p.weight = w;
        return p;
    }
};
Place after(const char* key);
Place before(const char* key);
Place first();
Place last();
Place order(int weight);
Place at(int n);

/// What a module adds to a page. Every call adds to the menu being built; the calls' order is the tie-break.
class PageBuilder {
public:
    explicit PageBuilder(const char* page) : page_(page) {}
    /// A new section on the page, under a heading (label; nullptr for one with no title)
    PageBuilder& heading(const char* id, const char* label, Place p = {});
    /// Rows from here on go in another section (one some module made with heading), by its id
    PageBuilder& section(const char* id);
    PageBuilder& row(Setting s, Place p = {});
    PageBuilder& line(const char* key, const char* text, Place p = {});
    /// A row that opens another page
    PageBuilder& link(const char* key, const char* label, const char* page, Place p = {});
    PageBuilder& custom(Custom c, Place p = {});

private:
    const char* page_;
    const char* section_ = nullptr;   // (nullptr: the page's own section, before every heading)
};
/// A tab: a page in the tab strip. tab(id) with no label adds to a tab some module made.
PageBuilder tab(const char* id, const char* label = nullptr, Place p = {});
/// A page opened by a row (link), not in the tab strip
PageBuilder page(const char* id, const char* label = nullptr);
/// A layout file's word over a module's: moves key's item to this place (and to page and section, when given). Applied after
/// every module ran, so a layout decision is made in one place.
void place(const char* key, Place p, const char* page = nullptr, const char* section = nullptr);

struct Module {
    const char* name;
    void (*describe)();
};
/// MENU_MODULE(name) { menu::tab("video").row({...}); }: registers a module that build() runs
int add_module(const char* name, void (*describe)());
#define MENU_MODULE(name)                                                                                                    \
    static void menu_module_##name();                                                                                         \
    [[maybe_unused]] static const int menu_module_registered_##name =                                                         \
        ::mkwvk::menu::add_module(#name, menu_module_##name);                                                                 \
    static void menu_module_##name()

/// Builds the menu from the registered modules (run sorted by name), or from these. Every build starts from nothing.
void build();
void build(const std::vector<Module>& modules);
/// What the last build found wrong (a missing anchor, a cycle, a duplicate key, a page no one made, a place() for no item),
/// one line each. They are logged as build finds them too (set_log); a game-free check fails on any.
const std::vector<std::string>& diagnostics();
void set_log(void (*log)(const char* line));
/// The text ids' translation (gpu/vk/vk_menu_i18n.h tr); none: the ids as they are
void set_translator(const char* (*tr)(const char* id));
const char* tr(const char* id);

/// The tabs in order: their ids
std::vector<const char*> tabs();
/// A page's label (translated), nullptr for no such page
const char* page_label(const char* page);
/// The setting by its key or by its HOS_OPT_* id (nullptr: none). A Custom's fallback is its setting.
const Setting* find(const char* key);
const Setting* find_option(int host_option);

/// A page's items in the order worked out, shown or not, untranslated: for a presenter that keeps rules of its own (the
/// ImGui menu while its rows move into modules). The pointers hold until the next build.
struct LayoutItem {
    bool heading = false;
    const char* key = nullptr;          // (a heading's: its section's id)
    const char* label = nullptr;
    const Setting* setting = nullptr;   // (nullptr for a heading)
};
std::vector<LayoutItem> layout(const char* page);

/// One item of a page as a presenter shows it: filtered (shown) and translated
struct ItemView {
    const char* key = nullptr;   // (nullptr for a heading or line made with none)
    Kind kind = Kind::Line;
    std::string label;
    std::string value;           // the value's text ("" for a heading, line, action or page)
    std::string help;
    std::string hint;
    std::string locked;          // why it can't change now; "" when it can
    int raw = 0;                 // the value itself
    int lo = 0, hi = 0;
    const char* opens = nullptr;
    bool restart = false;
    bool custom = false;         // a Custom item: its draw may stand in for this view
    bool selectable = false;     // a presenter's cursor stops on it (not a heading or line)
};
/// The page's items, in order, as they are now
std::vector<ItemView> snapshot(const char* page);
/// A custom item's drawing hooks, by its key (nullptr: none)
const Custom* custom_of(const char* key);

struct Outcome {
    bool taken = false;          // the model or the item did something with the input
    bool changed = false;        // the value changed (on_change was called)
    bool restart = false;        // ... and the change waits for a restart
    const char* open = nullptr;  // a page to open
    bool back = false;           // leave the page
    int select = -1;             // Up, Down: the next selectable item's index in the snapshot (-1: none)
};
/// Input on the snapshot's item index (the snapshot as it is now), or on an item by its key
Outcome input(const char* page, int index, Input in);
Outcome input_key(const char* page, const char* key, Input in);

} // namespace mkwvk::menu
