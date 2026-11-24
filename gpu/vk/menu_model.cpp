// The menu's model (menu_model.h): what modules describe, the order build() works out from it, and the snapshot and input a
// presenter uses. (longshot, #1633)
#include "menu_model.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mkwvk::menu {

namespace {

struct PageRec {
    const char* id = nullptr;
    const char* label = nullptr;
    bool tab = false;
    Place place;
    int seq = 0;
    const char* module = nullptr;    // (the module that made it: gave its label)
    const char* used_by = nullptr;   // (the first module that added to it)
};
struct SectionRec {
    const char* id = nullptr;        // (nullptr: the page's own section, before every heading)
    const char* page = nullptr;
    const char* label = nullptr;
    Place place;
    int seq = 0;
    const char* module = nullptr;
    bool made = false;               // (made by heading(); a section() alone only asks for it)
};
struct ItemRec {
    Setting s;
    Place place;
    const char* page = nullptr;
    const char* section = nullptr;
    int seq = 0;
    const char* module = nullptr;
    std::shared_ptr<Custom> custom;
};
struct Override {
    const char* key;
    Place place;
    const char* page;
    const char* section;
};
struct Entry {
    bool heading;
    int index;   // (heading: into sections; else into items)
};

struct Model {
    std::vector<PageRec> pages;
    std::vector<SectionRec> sections;
    std::vector<ItemRec> items;
    std::vector<Override> overrides;
    std::vector<int> tab_order;                                  // (into pages)
    std::unordered_map<std::string, std::vector<Entry>> layout;  // (by page id)
    std::vector<std::string> diags;
    const char* module = nullptr;
    int seq = 0;
};
Model& model() {
    static Model m;
    return m;
}
std::vector<Module>& registry() {
    static std::vector<Module> r;
    return r;
}

void default_log(const char* line) { fprintf(stderr, "menu: %s\n", line); }
void (*g_log)(const char*) = default_log;
const char* (*g_tr)(const char*) = nullptr;

bool same(const char* a, const char* b) { return a == b || (a && b && !strcmp(a, b)); }
const char* or_none(const char* s) { return s ? s : "(none)"; }
const char* module_name(const char* m) { return m ? m : "(no module)"; }

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
void diag(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    model().diags.emplace_back(line);
    if (g_log) g_log(line);
}

PageRec& page_rec(const char* id) {
    Model& m = model();
    for (auto& p : m.pages)
        if (same(p.id, id)) return p;
    PageRec p;
    p.id = id;
    p.seq = m.seq++;
    m.pages.push_back(p);
    return m.pages.back();
}
int page_index(const char* id) {
    const Model& m = model();
    for (size_t i = 0; i < m.pages.size(); i++)
        if (same(m.pages[i].id, id)) return (int)i;
    return -1;
}
int section_index(const char* page, const char* id) {
    const Model& m = model();
    for (size_t i = 0; i < m.sections.size(); i++)
        if (same(m.sections[i].page, page) && same(m.sections[i].id, id)) return (int)i;
    return -1;
}
SectionRec& section_rec(const char* page, const char* id) {
    const int i = section_index(page, id);
    if (i >= 0) return model().sections[i];
    SectionRec s;
    s.id = id;
    s.page = page;
    s.seq = model().seq++;
    model().sections.push_back(s);
    return model().sections.back();
}
void declare_page(const char* id, const char* label, bool tab, Place p) {
    PageRec& r = page_rec(id);
    if (!r.used_by) r.used_by = model().module;
    if (!label) return;
    if (r.label) {
        diag("page %s: made twice (by %s and %s); the first one's kept", id, module_name(r.module), module_name(model().module));
        return;
    }
    r.label = label;
    r.tab = tab;
    r.place = p;
    r.module = model().module;
}

// ---- the order ----

/// One of a group's items, for arrange: its key, its place and when it was described
struct Node {
    const char* key;
    Place place;
    int seq;
    int group;   // (the group it was described in: a page's section, for rows)
};
int pin_rank(Place::Pin p) { return p == Place::Pin::First ? 0 : p == Place::Pin::None ? 1 : 2; }
/// The order with no anchors: pin, then weight, then the order described, then key
bool base_less(const Node& a, const Node& b) {
    if (pin_rank(a.place.pin) != pin_rank(b.place.pin)) return pin_rank(a.place.pin) < pin_rank(b.place.pin);
    if (a.place.weight != b.place.weight) return a.place.weight < b.place.weight;
    if (a.seq != b.seq) return a.seq < b.seq;
    return strcmp(a.key ? a.key : "", b.key ? b.key : "") < 0;
}

/// The groups' items in order. An item with after(k) or before(k) goes right beside k, in k's group (the items beside one
/// anchor in their own base order); the rest by base_less; at(n) after that, at the first block boundary from n. A missing
/// anchor puts the item at its group's end, and a cycle is broken at its first item by base_less: both diagnosed. what names
/// the group's kind in the diagnostics.
std::vector<std::vector<int>> arrange(std::vector<Node> nodes, int groups, const char* what, const char* where) {
    const int n = (int)nodes.size();
    std::unordered_map<std::string, int> by_key;
    for (int i = 0; i < n; i++)
        if (nodes[i].key) by_key.emplace(nodes[i].key, i);   // (a doubled key: its first; diagnosed by the caller)
    std::vector<int> parent(n, -1);
    std::vector<bool> is_after(n, false);
    for (int i = 0; i < n; i++) {
        Place& p = nodes[i].place;
        if (p.after && p.before) {
            diag("%s %s in %s: after %s and before %s; only after is kept", what, or_none(nodes[i].key), where, p.after, p.before);
            p.before = nullptr;
        }
        const char* anchor = p.after ? p.after : p.before;
        if (!anchor) continue;
        const auto it = by_key.find(anchor);
        if (it == by_key.end()) {
            diag("%s %s in %s: %s %s, which isn't there; put at the end", what, or_none(nodes[i].key), where,
                 p.after ? "after" : "before", anchor);
            p.pin = Place::Pin::Last;
            continue;
        }
        parent[i] = it->second;
        is_after[i] = p.after != nullptr;
    }
    // (cycles: each one's first item by base_less becomes a root of its own group)
    for (;;) {
        std::vector<int> mark(n, -1);
        int cycle_at = -1;
        for (int i = 0; i < n && cycle_at < 0; i++) {
            for (int j = i; j >= 0; j = parent[j]) {
                if (mark[j] == i) {
                    cycle_at = j;
                    break;
                }
                if (mark[j] >= 0) break;   // (reached a chain walked already: no cycle through it from here)
                mark[j] = i;
            }
        }
        if (cycle_at < 0) break;
        std::vector<int> members{cycle_at};
        for (int j = parent[cycle_at]; j != cycle_at; j = parent[j]) members.push_back(j);
        int pick = members[0];
        std::string names;
        for (int j : members) {
            if (base_less(nodes[j], nodes[pick])) pick = j;
            names += (names.empty() ? "" : " -> ") + std::string(or_none(nodes[j].key));
        }
        diag("%s in %s: a cycle of anchors (%s -> %s); %s put by its own order", what, where, names.c_str(),
             or_none(nodes[cycle_at].key), or_none(nodes[pick].key));
        parent[pick] = -1;
    }
    std::vector<std::vector<int>> before_of(n), after_of(n);
    for (int i = 0; i < n; i++)
        if (parent[i] >= 0) (is_after[i] ? after_of : before_of)[parent[i]].push_back(i);
    const auto less = [&](int a, int b) { return base_less(nodes[a], nodes[b]); };
    for (int i = 0; i < n; i++) {
        std::sort(before_of[i].begin(), before_of[i].end(), less);
        std::sort(after_of[i].begin(), after_of[i].end(), less);
    }
    const auto expand = [&](auto&& self, int i, std::vector<int>& out) -> void {
        for (int c : before_of[i]) self(self, c, out);
        out.push_back(i);
        for (int c : after_of[i]) self(self, c, out);
    };
    std::vector<std::vector<int>> out(groups);
    for (int g = 0; g < groups; g++) {
        std::vector<int> roots, placed;
        for (int i = 0; i < n; i++)
            if (parent[i] < 0 && nodes[i].group == g) (nodes[i].place.at >= 0 ? placed : roots).push_back(i);
        std::sort(roots.begin(), roots.end(), less);
        std::stable_sort(placed.begin(), placed.end(), [&](int a, int b) {
            return nodes[a].place.at != nodes[b].place.at ? nodes[a].place.at < nodes[b].place.at : less(a, b);
        });
        std::vector<std::vector<int>> blocks;
        for (int r : roots) {
            blocks.emplace_back();
            expand(expand, r, blocks.back());
        }
        for (int r : placed) {
            std::vector<int> block;
            expand(expand, r, block);
            size_t b = 0;
            for (int count = 0; b < blocks.size() && count < nodes[r].place.at; b++) count += (int)blocks[b].size();
            blocks.insert(blocks.begin() + (std::ptrdiff_t)b, std::move(block));
        }
        for (auto& b : blocks) out[g].insert(out[g].end(), b.begin(), b.end());
    }
    return out;
}

// ---- after every module ran ----

void apply_overrides() {
    Model& m = model();
    for (const Override& o : m.overrides) {
        bool found = false;
        for (auto& it : m.items) {
            if (!same(it.s.key, o.key)) continue;
            it.place = o.place;
            if (o.page) it.page = o.page;
            if (o.page || o.section) it.section = o.section;
            found = true;
            break;
        }
        if (!found)
            for (auto& s : m.sections) {
                if (!s.id || !same(s.id, o.key) || (o.page && !same(o.page, s.page))) continue;
                s.place = o.place;
                found = true;
                break;
            }
        if (!found)
            for (auto& p : m.pages) {
                if (!same(p.id, o.key)) continue;
                p.place = o.place;
                found = true;
                break;
            }
        if (!found) diag("place(%s): no item, section or tab has that key", o.key);
    }
}

void check() {
    Model& m = model();
    std::unordered_map<std::string, const ItemRec*> keys;
    std::unordered_map<int, const ItemRec*> options;
    for (const auto& it : m.items) {
        const Setting& s = it.s;
        if (s.key) {
            const auto [prev, fresh] = keys.emplace(s.key, &it);
            if (!fresh)
                diag("key %s: two items (%s, from %s; and %s, from %s)", s.key, or_none(prev->second->s.label),
                     module_name(prev->second->module), or_none(s.label), module_name(it.module));
        } else if (s.value) {
            diag("%s (from %s): a saved row with no key", or_none(s.label), module_name(it.module));
        }
        if (s.host_option >= 0) {
            const auto [prev, fresh] = options.emplace(s.host_option, &it);
            if (!fresh) diag("host option %d: two settings (%s and %s)", s.host_option, or_none(prev->second->s.key), or_none(s.key));
        }
        if ((s.kind == Kind::Choice || s.kind == Kind::Toggle || s.kind == Kind::Slider) && !s.value && !it.custom)
            diag("%s (from %s): a value row with no value", or_none(s.key), module_name(it.module));
        if (s.kind == Kind::Slider && (s.step <= 0 || s.hi < s.lo))
            diag("%s: a slider from %d to %d in steps of %d", or_none(s.key), s.lo, s.hi, s.step);
        if (s.kind == Kind::Page && page_index(s.opens) < 0) diag("%s: opens page %s, which no module made", or_none(s.key), or_none(s.opens));
    }
    for (auto& it : m.items) {
        const int sec = it.section ? section_index(it.page, it.section) : -1;
        if (!it.section || (sec >= 0 && m.sections[sec].made)) continue;
        diag("%s (from %s): in section %s on page %s, which no module made (heading); put in the page's own", or_none(it.s.key),
             module_name(it.module), it.section, it.page);
        it.section = nullptr;
    }
    for (auto& p : m.pages)
        if (!p.label) diag("page %s: added to (by %s), but no module made it", p.id, module_name(p.used_by));
}

void lay_out() {
    Model& m = model();
    // (the tabs)
    std::vector<Node> tab_nodes;
    std::vector<int> tab_pages;
    for (size_t i = 0; i < m.pages.size(); i++)
        if (m.pages[i].tab) {
            tab_nodes.push_back({m.pages[i].id, m.pages[i].place, m.pages[i].seq, 0});
            tab_pages.push_back((int)i);
        }
    const auto tab_groups = arrange(tab_nodes, 1, "tab", "the tab strip");
    for (int t : tab_groups[0]) m.tab_order.push_back(tab_pages[t]);
    // (each page: its sections in order, the page's own first; then each section's rows)
    for (const auto& page : m.pages) {
        if (section_index(page.id, nullptr) < 0) {
            SectionRec own;
            own.page = page.id;
            own.place = first();
            own.made = true;
            m.sections.push_back(own);
        }
        std::vector<Node> sec_nodes;
        std::vector<int> secs;
        for (size_t i = 0; i < m.sections.size(); i++) {
            const SectionRec& s = m.sections[i];
            if (!same(s.page, page.id)) continue;
            Node nd{s.id, s.place, s.seq, 0};
            if (!s.id) {   // (the page's own section: always before every heading)
                nd.place = first();
                nd.seq = -1;
            }
            sec_nodes.push_back(nd);
            secs.push_back((int)i);
        }
        std::string where = std::string("page ") + page.id;
        const std::vector<int> sec_order = arrange(sec_nodes, 1, "section", where.c_str())[0];
        std::vector<int> group_of_sec(m.sections.size(), -1);
        for (size_t g = 0; g < secs.size(); g++) group_of_sec[secs[g]] = (int)g;
        std::vector<Node> row_nodes;
        std::vector<int> rows;
        for (size_t i = 0; i < m.items.size(); i++) {
            const ItemRec& it = m.items[i];
            if (!same(it.page, page.id)) continue;
            row_nodes.push_back({it.s.key, it.place, it.seq, group_of_sec[section_index(page.id, it.section)]});
            rows.push_back((int)i);
        }
        const auto by_section = arrange(row_nodes, (int)secs.size(), "row", where.c_str());
        std::vector<Entry>& out = m.layout[page.id];
        for (int g : sec_order) {
            const SectionRec& s = m.sections[secs[g]];
            if (s.id && s.label) out.push_back({true, secs[g]});
            for (int r : by_section[g]) out.push_back({false, rows[r]});
        }
    }
}

// ---- what a presenter sees ----

bool item_shown(const ItemRec& it) { return !it.s.shown || it.s.shown(); }

/// The page's entries as shown now: its rows shown, and each heading with something shown under it
std::vector<Entry> visible(const char* page) {
    std::vector<Entry> out;
    const auto found = model().layout.find(page ? page : "");
    if (found == model().layout.end()) return out;
    const std::vector<Entry>& all = found->second;
    for (size_t i = 0; i < all.size(); i++) {
        if (!all[i].heading) {
            if (item_shown(model().items[all[i].index])) out.push_back(all[i]);
            continue;
        }
        for (size_t j = i + 1; j < all.size() && !all[j].heading; j++)
            if (item_shown(model().items[all[j].index])) {
                out.push_back(all[i]);
                break;
            }
    }
    return out;
}

std::string text_of(const char* id) { return id ? tr(id) : ""; }

std::string value_text(const Setting& s, int v) {
    if (s.text) return s.text(v);
    if (!s.choices.empty()) {
        const int i = std::clamp(v - s.lo, 0, (int)s.choices.size() - 1);
        return text_of(s.choices[i]);
    }
    if (s.kind == Kind::Toggle) return text_of(v ? "On" : "Off");
    return std::to_string(v);
}

bool selectable(const ItemRec& it) {
    if (it.s.kind == Kind::Heading || it.s.kind == Kind::Line) return it.custom && it.custom->input;
    return true;
}

ItemView view_of(const Entry& e) {
    ItemView v;
    if (e.heading) {
        const SectionRec& s = model().sections[e.index];
        v.key = s.id;
        v.kind = Kind::Heading;
        v.label = text_of(s.label);
        return v;
    }
    const ItemRec& it = model().items[e.index];
    const Setting& s = it.s;
    v.key = s.key;
    v.kind = s.kind;
    v.label = text_of(s.label);
    v.help = text_of(s.help);
    v.lo = s.lo, v.hi = s.hi;
    v.opens = s.opens;
    v.restart = s.restart;
    v.custom = it.custom != nullptr;
    v.selectable = selectable(it);
    if (s.value && (s.kind == Kind::Choice || s.kind == Kind::Toggle || s.kind == Kind::Slider)) {
        if (s.sync) *s.value = s.sync();
        v.raw = *s.value;
        v.value = value_text(s, v.raw);
    }
    if (s.hint) v.hint = text_of(s.hint(v.raw, -1));
    if (s.locked) v.locked = text_of(s.locked());
    return v;
}

int item_index(const char* key) {
    const auto& items = model().items;
    for (size_t i = 0; i < items.size(); i++)
        if (same(items[i].s.key, key)) return (int)i;
    return -1;
}

/// The value one step in dir from v: step_fn's, or v + dir * step held at the ends (a Slider unless it wraps,
/// stops_at_ends) or wrapping
int stepped(const Setting& s, int v, int dir) {
    if (s.step_fn) return s.step_fn(v, dir);
    const int step = s.kind == Kind::Toggle ? 1 : std::max(1, s.step);
    const int next = v + dir * step;
    if ((s.kind == Kind::Slider && !s.wraps) || s.stops_at_ends) return std::clamp(next, s.lo, s.hi);
    if (next > s.hi) return s.lo;
    if (next < s.lo) return s.hi;
    return next;
}

Outcome change(const Setting& s, int dir) {
    Outcome o;
    o.taken = true;
    if (s.sync && s.value) *s.value = s.sync();
    const int v = *s.value, next = stepped(s, v, dir);
    if (next == v) return o;
    *s.value = next;
    if (s.on_change) s.on_change(next);
    o.changed = true;
    o.restart = s.restart;
    return o;
}

Outcome act(const std::vector<Entry>& shown, int index, Input in) {
    Outcome o;
    if (index < 0 || index >= (int)shown.size()) return o;
    if (in == Input::Back) {
        o.taken = o.back = true;
        return o;
    }
    if (in == Input::Up || in == Input::Down) {
        const int n = (int)shown.size(), dir = in == Input::Down ? 1 : -1;
        for (int k = 1; k <= n; k++) {
            const int j = ((index + dir * k) % n + n) % n;
            if (!shown[j].heading && selectable(model().items[shown[j].index])) {
                o.taken = true;
                o.select = j;
                break;
            }
        }
        return o;
    }
    if (shown[index].heading) return o;
    const ItemRec& it = model().items[shown[index].index];
    if (it.custom && it.custom->input && it.custom->input(in)) {
        o.taken = true;
        return o;
    }
    const Setting& s = it.s;
    if (s.locked && s.locked()) return o;
    const bool valued = s.value && (s.kind == Kind::Choice || s.kind == Kind::Toggle || s.kind == Kind::Slider);
    if (in == Input::Left || in == Input::Right) return valued ? change(s, in == Input::Right ? 1 : -1) : o;
    // (Activate)
    if (valued && s.kind != Kind::Slider) return change(s, 1);
    if (s.kind == Kind::Action && s.on_activate) {
        s.on_activate();
        o.taken = true;
    } else if (s.kind == Kind::Page && s.opens) {
        o.taken = true;
        o.open = s.opens;
    }
    return o;
}

} // namespace

Place after(const char* key) {
    Place p;
    p.after = key;
    return p;
}
Place before(const char* key) {
    Place p;
    p.before = key;
    return p;
}
Place first() {
    Place p;
    p.pin = Place::Pin::First;
    return p;
}
Place last() {
    Place p;
    p.pin = Place::Pin::Last;
    return p;
}
Place order(int weight) {
    Place p;
    p.weight = weight;
    return p;
}
Place at(int n) {
    Place p;
    p.at = n;
    return p;
}

PageBuilder& PageBuilder::heading(const char* id, const char* label, Place p) {
    if (!id) {
        diag("page %s: a heading with no id (from %s)", page_, module_name(model().module));
        return *this;
    }
    SectionRec& s = section_rec(page_, id);
    if (s.made) {
        diag("section %s on page %s: made twice (by %s and %s); the first one's kept", id, page_, module_name(s.module),
             module_name(model().module));
    } else {
        s.label = label;
        s.place = p;
        s.module = model().module;
        s.made = true;
    }
    section_ = id;
    return *this;
}
PageBuilder& PageBuilder::section(const char* id) {
    if (id) section_rec(page_, id);
    section_ = id;
    return *this;
}
PageBuilder& PageBuilder::row(Setting s, Place p) {
    Model& m = model();
    if (s.kind == Kind::Toggle) s.lo = 0, s.hi = 1;
    if (!s.choices.empty()) s.hi = s.lo + (int)s.choices.size() - 1;
    ItemRec it;
    it.s = std::move(s);
    it.place = p;
    it.page = page_;
    it.section = section_;
    it.seq = m.seq++;
    it.module = m.module;
    m.items.push_back(std::move(it));
    PageRec& pr = page_rec(page_);
    if (!pr.used_by) pr.used_by = m.module;
    return *this;
}
PageBuilder& PageBuilder::line(const char* key, const char* text, Place p) {
    Setting s;
    s.key = key;
    s.label = text;
    s.kind = Kind::Line;
    return row(std::move(s), p);
}
PageBuilder& PageBuilder::link(const char* key, const char* label, const char* page, Place p) {
    Setting s;
    s.key = key;
    s.label = label;
    s.kind = Kind::Page;
    s.opens = page;
    return row(std::move(s), p);
}
PageBuilder& PageBuilder::custom(Custom c, Place p) {
    Setting s = c.fallback;
    if (s.kind == Kind::Choice && !s.value) s.kind = Kind::Line;   // (a fallback with no value: a line of text)
    row(std::move(s), p);
    model().items.back().custom = std::make_shared<Custom>(std::move(c));
    model().items.back().custom->fallback = model().items.back().s;
    return *this;
}

PageBuilder tab(const char* id, const char* label, Place p) {
    declare_page(id, label, true, p);
    return PageBuilder(id);
}
PageBuilder page(const char* id, const char* label) {
    declare_page(id, label, false, {});
    return PageBuilder(id);
}
void place(const char* key, Place p, const char* page, const char* section) {
    model().overrides.push_back({key, p, page, section});
}

int add_module(const char* name, void (*describe)()) {
    registry().push_back({name, describe});
    return (int)registry().size();
}

void build() { build(registry()); }
void build(const std::vector<Module>& modules) {
    model() = Model{};
    std::vector<Module> run = modules;
    std::stable_sort(run.begin(), run.end(), [](const Module& a, const Module& b) { return strcmp(a.name, b.name) < 0; });
    for (size_t i = 1; i < run.size(); i++)
        if (!strcmp(run[i - 1].name, run[i].name)) diag("module %s: registered twice", run[i].name);
    for (const Module& mod : run) {
        model().module = mod.name;
        mod.describe();
    }
    model().module = nullptr;
    apply_overrides();
    check();
    lay_out();
}

const std::vector<std::string>& diagnostics() { return model().diags; }
void set_log(void (*log)(const char*)) { g_log = log; }
void set_translator(const char* (*translate)(const char*)) { g_tr = translate; }
const char* tr(const char* id) {
    if (!id) return "";
    const char* t = g_tr ? g_tr(id) : nullptr;
    return t ? t : id;
}

std::vector<const char*> tabs() {
    std::vector<const char*> out;
    for (int i : model().tab_order) out.push_back(model().pages[i].id);
    return out;
}
const char* page_label(const char* page) {
    const int i = page_index(page);
    return i < 0 ? nullptr : tr(model().pages[i].label ? model().pages[i].label : model().pages[i].id);
}
const Setting* find(const char* key) {
    const int i = key ? item_index(key) : -1;
    return i < 0 ? nullptr : &model().items[i].s;
}
const Setting* find_option(int host_option) {
    for (const auto& it : model().items)
        if (host_option >= 0 && it.s.host_option == host_option) return &it.s;
    return nullptr;
}

std::vector<LayoutItem> layout(const char* page) {
    std::vector<LayoutItem> out;
    const auto found = model().layout.find(page ? page : "");
    if (found == model().layout.end()) return out;
    for (const Entry& e : found->second) {
        if (e.heading) {
            const SectionRec& s = model().sections[e.index];
            out.push_back({true, s.id, s.label, nullptr});
        } else {
            const ItemRec& it = model().items[e.index];
            out.push_back({false, it.s.key, it.s.label, &it.s});
        }
    }
    return out;
}

std::vector<ItemView> snapshot(const char* page) {
    std::vector<ItemView> out;
    for (const Entry& e : visible(page)) out.push_back(view_of(e));
    return out;
}
const Custom* custom_of(const char* key) {
    const int i = key ? item_index(key) : -1;
    return i < 0 ? nullptr : model().items[i].custom.get();
}

Outcome input(const char* page, int index, Input in) { return act(visible(page), index, in); }
Outcome input_key(const char* page, const char* key, Input in) {
    const std::vector<Entry> shown = visible(page);
    for (size_t i = 0; i < shown.size(); i++)
        if (!shown[i].heading && key && same(model().items[shown[i].index].s.key, key)) return act(shown, (int)i, in);
    return {};
}

} // namespace mkwvk::menu
