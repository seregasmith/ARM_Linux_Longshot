// The Insert menu's output (#1633), the Graphics tab's Resolution and upscaling section's first rows: the picture's shape (the
// Aspect ratio), its size (the Output resolution: the game's own, the screen's display modes and the common sizes of their
// shapes, or the player's own, typed in Custom output size, vk_menu.cpp's text field) and the console's Profile. The output
// engine with them: the screen's sizes, the sizes offered, the shapes, the picture each choice makes and the size shown
// (gpu/vk/menu/output.h: what the menu's options file, the bug report, the pipeline share and the upscaler rows use of it).
// The engine asks the host and the window: fenced from a layout-only build (MENU_LAYOUT_ONLY, MENU_HOOK: menu_module.h).
#include "menu_module.h"

#include "menu/output.h"

#ifndef MENU_LAYOUT_ONLY
#include "../../host/hos_wide_size.h"   // (the widescreen picture's size, as host/hos_main.c works it out)
#include "vk_internal.h"   // (host: its screen)
#ifdef _WIN32
#include <windows.h>
#endif
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace mkwvk {

#ifndef MENU_LAYOUT_ONLY
#ifdef _WIN32
HWND app_window();   // (vk_main.cpp: the game's window)
#endif
namespace dm = display_modes;
/// The host's screen: the main one, whose shape host/hos_main.c gives a 16:9 Output resolution at the start with ultrawide=1
/// (Windows: the primary's size; Linux: SDL's primary desktop, MkwVkHost.screen_size): false where it is not known
bool host_screen_size(uint32_t* w, uint32_t* h) {
#ifdef _WIN32
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    if (sw > 0 && sh > 0) return *w = (uint32_t)sw, *h = (uint32_t)sh, true;
#endif
    int hw = 0, hh = 0;
    if (host && host->screen_size && host->screen_size(&hw, &hh) && hw > 0 && hh > 0)
        return *w = (uint32_t)hw, *h = (uint32_t)hh, true;
    return false;
}
#ifdef _WIN32
/// The monitor the game's window is on (the nearest), with its size; false before there is a window
bool window_monitor(MONITORINFOEXW& mi) {
    const HWND wnd = app_window();
    mi = {};
    mi.cbSize = sizeof mi;
    return wnd && GetMonitorInfoW(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &mi) && mi.rcMonitor.right > mi.rcMonitor.left &&
           mi.rcMonitor.bottom > mi.rcMonitor.top;
}
#endif
/// The screen the Aspect ratio and Output resolution rows mark ("(monitor's)", "(native)"): the one the game's window is on,
/// its size now (a desktop size changed while running is followed: outputs_follow_screen); the host's before there is a
/// window, and on Linux. False where neither is known
bool screen_size(uint32_t* w, uint32_t* h) {
#ifdef _WIN32
    MONITORINFOEXW mi;
    if (window_monitor(mi))
        return *w = (uint32_t)(mi.rcMonitor.right - mi.rcMonitor.left), *h = (uint32_t)(mi.rcMonitor.bottom - mi.rcMonitor.top), true;
#endif
    return host_screen_size(w, h);
}
/// That screen's display modes, each as Windows lists it (every refresh rate and depth: display_modes.h keeps each size once);
/// the primary's before there is a window; none where they are not known (Linux)
std::vector<display_modes::Size> screen_modes() {
    std::vector<display_modes::Size> v;
#ifdef _WIN32
    MONITORINFOEXW mi;
    const wchar_t* device = window_monitor(mi) ? mi.szDevice : nullptr;
    DEVMODEW m{};
    m.dmSize = sizeof m;
    for (DWORD i = 0; EnumDisplaySettingsW(device, i, &m); i++) v.push_back({(uint32_t)m.dmPelsWidth, (uint32_t)m.dmPelsHeight});
#endif
    return v;
}
// The Output resolution's choices: 0 the game's own (2048x1152), then the sizes the main screen offers (display_modes.h: its
// display modes and its own size, each side rounded down to 8, those the game accepts, with the common sizes of the shapes
// they have, 1440p, 4K and 5K at 16:9 on a 1080p screen too, by width then height; HOS_DISPLAY_MODES=<w>x<h>,... for a run;
// where they are not known, the sizes offered before: 1920x1080, 2560x1440, 3840x2160, 5120x1440, 5120x2880), then, last,
// a size of the player's own from longshot_options.ini (output_res=<width>x<height>, host/nn_applet.c hos_output_parse: the
// same rules) when it is none of them. The ini keeps the size, not the choice's number: output_res=3840x2160 (the game's
// own: kGameOutput, menu/output.h)
// (the sizes offered before the screen's: output_res=0 to 5 as saved then, and where the screen's are not known, by width then
// height. nx2/pipeline_share.py OUTPUTS mirrors it)
constexpr Out kOutputs[] = {{2048, 1152}, {2560, 1440}, {3840, 2160}, {1920, 1080}, {5120, 1440}, {5120, 2880}};
Out outputs_screen{0, 0};   // (the window's screen's size the choices were read for: outputs_follow_screen)
/// A screen's own size among the modes, and its shape at the most pixels a picture of another shape takes when the size itself
/// is past what the game accepts: a 7680x2160 32:9 screen gets 6256x1760, so its shape is still offered
void add_screen_size(std::vector<dm::Size>& modes, uint32_t sw, uint32_t sh) {
    modes.push_back({sw, sh});
    unsigned w = 0, h = 0;
    if (sh && !hos_output_size_valid(sw & ~7u, sh & ~7u)) {
        hos_wide_size(sh, hos_wide_clamp((double)sw / sh), hos_wide_max_px(), &w, &h);
        if (dm::ratio_name({w, h}) == dm::ratio_name({sw, sh})) modes.push_back({w, h});
    }
}
const std::vector<Out>& outputs() {
    static const std::vector<Out> v = [] {
        const char* e = getenv("HOS_DISPLAY_MODES");
        std::vector<Out> o{kGameOutput};
        std::vector<dm::Size> modes = e ? dm::parse_list(e) : screen_modes();
        if (!e) {
            uint32_t sw = 0, sh = 0, hw = 0, hh = 0;
            const bool screen = screen_size(&sw, &sh), host_screen = host_screen_size(&hw, &hh);
            // (no modes but a screen's size (Linux): the sizes offered before, with the screen's)
            if (modes.empty() && (screen || host_screen))
                for (int i = 1; i < (int)std::size(kOutputs); i++) modes.push_back({kOutputs[i].w, kOutputs[i].h});
            if (screen) add_screen_size(modes, sw, sh), outputs_screen = {sw, sh};
            if (host_screen) add_screen_size(modes, hw, hh);   // (the host's shape, which ultrawide=1 gives a 16:9 size)
        }
        for (dm::Size s : dm::with_common(dm::offered(modes)))
            if (!(s.w == kGameOutput.w && s.h == kGameOutput.h)) o.push_back({s.w, s.h});
        if (o.size() == 1) {
            o.insert(o.end(), std::begin(kOutputs) + 1, std::end(kOutputs));
            std::sort(o.begin() + 1, o.end(), [](Out a, Out b) { return a.w != b.w ? a.w < b.w : a.h < b.h; });
        }
        return o;
    }();
    return v;
}
int custom_index() { return (int)outputs().size(); }   // (the player's own size's choice: after the sizes offered)
Out custom_output{0, 0};   // (the player's own size; 0 x 0: none)
// (a screen's sizes found while running, the window moved to another monitor or its desktop size changed: after the player's
// own, so the choices' numbers before them keep their sizes; outputs_follow_screen)
std::vector<Out> output_extras;
int last_choice() { return custom_index() + (int)output_extras.size(); }
int output_choice(int v) {
    const int c = custom_index();
    return v >= 0 && (v < c || (v == c && custom_output.w) || (v > c && v <= last_choice())) ? v : 0;
}
Out output_at(int v) {
    v = output_choice(v);
    const int c = custom_index();
    return v < c ? outputs()[v] : v == c ? custom_output : output_extras[v - c - 1];
}
/// The choice of size w x h: one of the sizes offered (or found since), else the player's own from now on
int output_choice_of(uint32_t w, uint32_t h) {
    for (int i = 0; i < custom_index(); i++)
        if (outputs()[i].w == w && outputs()[i].h == h) return i;
    for (int i = 0; i < (int)output_extras.size(); i++)
        if (output_extras[i].w == w && output_extras[i].h == h) return custom_index() + 1 + i;
    custom_output = {w, h};
    return custom_index();
}
/// output_res= (or HOS_OUTPUT_RES) to a choice: "<width>x<height>" (each side a multiple of 8, rounded down; at least 640 x 360,
/// at most 5120 x 2880's pixels, 5:4 to 48:9), one of the sizes offered or the player's own (custom_output); the numbers saved
/// before (0 to 5: 2048x1152, 2560x1440, 3840x2160, 1920x1080, 5120x1440, 5120x2880) as those sizes; anything else the game's
/// own (host/nn_applet.c hos_output_parse: the same rules)
int parse_output(const char* text) {
    unsigned w = 0, h = 0;
    if (text && sscanf(text, "%u%*[xX]%u", &w, &h) == 2) {
        w &= ~7u, h &= ~7u;
        if (!hos_output_size_valid(w, h))
            return 0;
        return output_choice_of(w, h);
    }
    const int v = text ? atoi(text) : 0;
    return v >= 1 && v < (int)std::size(kOutputs) ? output_choice_of(kOutputs[v].w, kOutputs[v].h) : 0;
}
dm::Size size_of(Out o) { return {o.w, o.h}; }
/// Every choice there is, in order (the player's own when there is one, then the sizes found while running)
std::vector<int> output_ids() {
    std::vector<int> v;
    for (int i = 0; i <= last_choice(); i++)
        if (output_choice(i) == i) v.push_back(i);
    return v;
}
/// Their sizes, in the same order
std::vector<dm::Size> output_sizes() {
    std::vector<dm::Size> v;
    for (int i : output_ids()) v.push_back(size_of(output_at(i)));
    return v;
}
/// The shapes of the choices, narrowest first: made again only when the player's own size changes (the sizes offered are
/// fixed once read), as the rows ask for them at every frame they are drawn
const std::vector<dm::Shape>& output_shapes() {
    static std::vector<dm::Shape> v;
    static Out made_for{~0u, ~0u};
    static size_t extras = ~size_t(0);
    if (made_for.w != custom_output.w || made_for.h != custom_output.h || extras != output_extras.size())
        v = dm::shapes(output_sizes()), made_for = custom_output, extras = output_extras.size();
    return v;
}
/// The shape (a place in output_shapes) of choice v
int output_shape_of(int v) { return dm::shape_index(output_shapes(), size_of(output_at(v))); }
/// The monitor's shape: the main screen's (as host/hos_main.c takes it), when it is one of output_shapes; "" where the screen's
/// size is not known (Linux) or its shape is not offered (a portrait screen): the row then has a Monitor's shape value of its own
std::string monitor_shape() {
    uint32_t sw = 0, sh = 0;
    if (!screen_size(&sw, &sh)) return std::string();
    const std::string name = dm::ratio_name({sw, sh});
    for (const dm::Shape& s : output_shapes())
        if (s.name == name) return name;
    return std::string();
}
/// The Aspect ratio row's values: output_shapes' places, the monitor's own marked; where monitor_shape is not known, one more
/// after them, Monitor's shape (ultrawide=1 with a 16:9 size, as host/hos_main.c takes the screen's shape), as on main
int aspect_monitor_value() { return monitor_shape().empty() ? (int)output_shapes().size() : -1; }
int aspect_last() { return std::max(0, (int)output_shapes().size() - 1 + (aspect_monitor_value() >= 0 ? 1 : 0)); }
/// The shape the Aspect ratio row's value v names (Monitor's shape where the monitor's is not known: 16:9 sizes)
std::string aspect_shape(int v) {
    const std::vector<dm::Shape>& shapes = output_shapes();
    if (shapes.empty() || v == aspect_monitor_value()) return std::string("16:9");
    return shapes[std::clamp(v, 0, (int)shapes.size() - 1)].name;
}
/// The row's value for shape name (16:9's when it is not one of them)
int aspect_index(const std::string& name) {
    const std::vector<dm::Shape>& shapes = output_shapes();
    for (int i = 0; i < (int)shapes.size(); i++)
        if (shapes[i].name == name) return i;
    for (int i = 0; i < (int)shapes.size(); i++)
        if (shapes[i].name == "16:9") return i;
    return 0;
}
/// Whether the row's value v is the monitor's shape: the window's screen's, marked "(monitor's)"
bool aspect_is_monitors(int v) {
    const std::string m = monitor_shape();
    return m.empty() ? v == aspect_monitor_value() : aspect_shape(v) == m;
}
/// The host's screen's shape when it is one of output_shapes ("" otherwise): what ultrawide=1 gives a 16:9 size at the start
std::string host_shape() {
    uint32_t w = 0, h = 0;
    if (!host_screen_size(&w, &h)) return std::string();
    const std::string name = dm::ratio_name({w, h});
    for (const dm::Shape& s : output_shapes())
        if (s.name == name) return name;
    return std::string();
}
/// Whether the row's value v is saved as ultrawide=1: the host's shape (a 16:9 size then takes it at the next start, as it
/// shows), or Monitor's shape where the monitor's is not known. The same as the mark where the window is on the host's screen
bool aspect_follows_host(int v) {
    if (v == aspect_monitor_value()) return true;
    const std::string h = host_shape();
    return h.empty() ? aspect_is_monitors(v) : aspect_shape(v) == h;
}
/// The Aspect ratio row's value for Output resolution choice v: a size of a shape of its own is that shape; a 16:9 one is the
/// monitor's shape when ultrawide (0 or 1, as host/hos_main.c started with it) follows the monitor, else 16:9
int aspect_of_choice(int v, int ultrawide) {
    const Out o = output_at(v);
    if (hos_output_own_shape(o.w, o.h)) return aspect_index(dm::ratio_name(size_of(o)));
    if (ultrawide != 1) return aspect_index("16:9");
    const std::string h = host_shape(), m = monitor_shape();   // (the shape the host gives it: its screen's)
    return !h.empty() ? aspect_index(h) : m.empty() ? aspect_monitor_value() : aspect_index(m);
}
/// The Aspect ratio row's value that choice v needs (the control bridge, before it walks the Output resolution to a size of
/// another shape): its own shape; a 16:9 size, 16:9 itself, not the monitor's shape the row may be at
int aspect_for_choice(int v) { return aspect_of_choice(v, 0); }
/// The shape the Output resolution's arrows walk from choice id: its own; but a 16:9 size kept from before at the host's
/// shape (ultrawide=1, the host widening it) walks into that shape's own sizes, the list the Aspect ratio row names
std::string step_shape(int id) {
    const std::string own = dm::ratio_name(size_of(output_at(id))), h = aspect_shape(opts.ultrawide);
    return own == "16:9" && h != own && opts.ultrawide != aspect_monitor_value() && aspect_follows_host(opts.ultrawide) ? h : own;
}
Out picture_for(int v, bool* native);
// (the choices of id's shape (step_shape), from smallest to largest by the size each shows (picture_for), width then height:
// 1920x1080, 2048x1152, 2560x1440, 3840x2160 at 16:9, the player's own in its place; id itself among them. A 16:9 size walked
// at the host's wider shape is where its picture is: the game's own 2048x1152 at a 5120x2160 screen's 21:9 draws 2728x1152,
// between 2560x1080 and 3008x1264, not before them, and reached from either. What is saved is the choice, as before)
int step_output_choice(int id, int dir) {
    const std::string shape = step_shape(id);
    std::vector<std::pair<Out, int>> order;
    for (int i = 0; i <= last_choice(); i++) {
        if (output_choice(i) != i) continue;
        const Out shown = picture_for(i, nullptr);
        // (the game's own is in the walk of the shape it is drawn at, as its row shows it)
        if (i == output_choice(id) || dm::ratio_name(size_of(output_at(i))) == shape || (i == 0 && dm::ratio_name(size_of(shown)) == shape))
            order.push_back({shown, i});
    }
    std::stable_sort(order.begin(), order.end(), [](const std::pair<Out, int>& a, const std::pair<Out, int>& b) {
        return a.first.w != b.first.w ? a.first.w < b.first.w : a.first.h < b.first.h;
    });
    int at = 0;
    while (at < (int)order.size() && order[at].second != output_choice(id)) at++;
    return order[std::clamp(at + dir, 0, (int)order.size() - 1)].second;
}
/// The Output resolution the Aspect ratio row's value v takes: the one the game started with at its shape; the one chosen when
/// it has that shape; else the screen's own size when it has it; else the one whose height is nearest the screen's
/// (display_modes.h pick)
int aspect_started = -1;   // (the Aspect ratio row's value at the start, from the size the run started with: outputs_sync)
int output_started = 0;    // (the Output resolution the run started with)
int output_for_aspect(int v) {
    // (back at the shape the game started with: the size it started with, so nothing is left to restart for. A first start on a
    // 21:9 screen is 21:9 with the game's own size, which ultrawide=1 widens: stepping away and back took 5120x2160 (native))
    if (v == aspect_started && output_choice(output_started) == output_started) return output_started;
    const std::string name = aspect_shape(v);
    if (dm::ratio_name(size_of(output_at(opts.output_res))) == name) return opts.output_res;
    uint32_t sw = 0, sh = 0;
    screen_size(&sw, &sh);
    const int i = dm::pick(output_sizes(), name, {sw & ~7u, sh & ~7u});
    return i < 0 ? opts.output_res : output_ids()[i];
}
/// The picture an Output resolution choice makes: a size not 16:9 (the Aspect ratio row's 21:9, 16:10, 4:3...) takes its own
/// shape; a 16:9 one, at the monitor's own Aspect ratio, the screen's. Both keep at most hos_wide_max_px() pixels, matching
/// host/hos_main.c at startup (host/hos_wide_size.h); otherwise the 16:9 output. The game's own choice (0) as the game draws it,
/// its 2048x1152 widened the same way (2728x1152 at a 5120x2160 screen's 21:9). *native: the screen's own size
Out picture_for(int v, bool* native) {
    v = output_choice(v);
    Out o = output_at(v);
    uint32_t sw = 0, sh = 0;
    if (native) *native = false;
    const bool own = hos_output_own_shape(o.w, o.h);
    screen_size(&sw, &sh);
    uint32_t hw = 0, hh = 0;
    const bool have_host = host_screen_size(&hw, &hh);
    if (!opts.handheld && (own || aspect_follows_host(opts.ultrawide))) {
        const double want = own ? (double)o.w / o.h : have_host ? (double)hw / hh : 0.0;   // (the host's screen: hos_main.c)
        const double game = 16.0 / 9.0;
        if (want > game * 1.01 || (want > 1.0 && want < game * 0.99)) {
            unsigned w = 0, h = 0;
            hos_wide_size(o.h, hos_wide_clamp(want), hos_wide_max_px(), &w, &h);
            o = Out{w, h};
        }
    }
    if (native && v && !opts.handheld) *native = sw && o.w == (sw & ~7u) && o.h == (sh & ~7u);
    return o;
}
Out shown_output() {
    return opts.handheld ? Out{1536, 864} : picture_for(opts.output_res);   // (the real picture)
}
// The size the game draws the 3D at for a fixed scale of its output, a twelfth of it: every twelfth asked came out exact at the three
// outputs (2048x1152: 0.58 drew 1195x672, 0.6667 1366x768). So the scale the game is given is the twelfth too: 0.67 drew 2615x1471 at
// 3840x2160, not the 2560x1440 of 8/12.
int twelfths(float scale) { return std::min(std::max((int)(scale * 12.0f + 0.5f), 1), 12); }
void game_size(float scale, uint32_t& w, uint32_t& h) {
    const Out o = shown_output();
    const int k = twelfths(scale);
    w = (o.w * k + 11) / 12, h = o.h * k / 12;
}
const char* tx_output_res(int v) {
    static char b[40];
    v = output_choice(v);
    // (the picture this choice makes (picture_for: at the monitor's shape when the Aspect ratio takes it), "(native)"
    // when it is the screen's own size; the game's own reduced size (0, 80% of its 2560x1440: hos_output_parse) marked so,
    // not the default since a new install takes 2560x1440, host/hos_wide_size.h hos_output_default)
    bool native = false;
    const Out o = picture_for(v, &native);
    snprintf(b, sizeof b, "%ux%u%s", o.w, o.h, native ? " (native)" : v ? "" : " (reduced)");
    return b;
}
/// The Aspect ratio row (ultrawide=): the shapes of the sizes offered, narrowest first, the monitor's own named so
const char* tx_ultrawide(int v) {
    static char b[48];
    if (v == aspect_monitor_value()) return "Monitor's shape";   // (the monitor's own not known: as on main)
    // (printed from a format, so menu_i18n.h's "%s (monitor's)" translates it: vk_menu_i18n.cpp match_format)
    if (aspect_is_monitors(v)) snprintf(b, sizeof b, "%s (monitor's)", aspect_shape(v).c_str());
    else snprintf(b, sizeof b, "%s", aspect_shape(v).c_str());
    return b;
}
/// The Aspect ratio: the monitor's own shape, the game's 16:9, or another shape, wider or narrower than 16:9
const char* hn_ultrawide(int v, int variant) {
    static const char* const t[] = {
        "Your monitor's own shape: the picture fills it. The HUD keeps its 16:9 shape in the middle.",
        "The game's own shape.",
        "Wider than 16:9: the camera sees more at the sides. The HUD and menus keep the middle 16:9.",
        "Narrower than 16:9: the picture keeps its height, and the HUD fills its width."};
    if (variant >= 0) return variant < (int)std::size(t) ? t[variant] : nullptr;
    if (aspect_is_monitors(v)) return t[0];
    const std::vector<dm::Shape> shapes = output_shapes();
    const double r = shapes.empty() ? 16.0 / 9.0 : shapes[std::clamp(v, 0, (int)shapes.size() - 1)].r, game = 16.0 / 9.0;
    return r > game * 1.01 ? t[2] : r < game * 0.99 ? t[3] : t[1];
}
/// The Output resolution: the game's own reduced size, the player's own, or one of the screen's sizes as it compares with the
/// screen (its own, larger, smaller; nothing where the screen's size is not known)
const char* hn_output_res(int v, int variant) {
    static const char* const t[] = {
        "The game's own reduced size, 80% of its 2560x1440: lighter on the GPU.",
        "Your screen's own size: the HUD and text pixel for pixel, nothing to scale to the window.",
        "Larger than your screen: drawn bigger, then scaled down to the window. Sharper, and slower. The game's render "
        "targets get a bigger GPU pool, so a full-size 3D fits.",
        "Smaller than your screen: faster, and scaled up to the window.",
        "Your own size, from Custom output size below."};
    if (variant >= 0) return variant < (int)std::size(t) ? t[variant] : nullptr;
    v = output_choice(v);
    if (!v) return t[0];
    if (v == custom_index()) return t[4];
    uint32_t sw = 0, sh = 0;
    if (!screen_size(&sw, &sh)) return nullptr;
    bool native = false;
    const Out o = picture_for(v, &native);
    return native ? t[1] : (uint64_t)o.w * o.h > (uint64_t)sw * sh ? t[2] : t[3];
}
#endif

namespace {
const char* tx_profile(int v) { return v ? "Handheld" : "Docked"; }
MODULE_HINTS(hn_handheld, 0, "As on a TV: the game at the Output resolution's size.",
             "As in the hands: the game at the console's handheld size (1536x864), lighter on the GPU.")
} // namespace

MENU_MODULE(output) {
    // (the shape, then its sizes: the screen's display modes, display_modes.h; outputs_sync sets both rows' ends at the start)
    menu::tab("graphics")
        .row({.key = "ultrawide",
              .label = "Aspect ratio",
              .help = "The picture's shape: your monitor's own (the default: wider on an ultrawide, narrower on 4:3 or 16:10), the "
                      "game's 16:9, or another your screen has. The Output resolution lists that shape's sizes. Applies after a "
                      "restart.",
              .value = &opts.ultrawide,
              .lo = 0, .hi = 1,
              .restart = true,
              .preset_exempt = true,
              .stops_at_ends = true,
              .text = MENU_HOOK([](int v) { return std::string(tx_ultrawide(v)); }),
              .hint = MENU_HOOK(hn_ultrawide)}, menu::before("@Custom output size").order(10))
        .row({.key = "output_res",
              .label = "Output resolution",
              .help = "The game's whole frame, HUD included, before it is scaled to the window. Docked only. On supported Windows "
                      "setups, 1024x576 can be previewed from 1280x720. Other changes apply after a restart.",
              .value = &opts.output_res,
              .lo = 0, .hi = 0,
              .restart = true,
              .preset_exempt = true,
              .text = MENU_HOOK([](int v) { return std::string(tx_output_res(v)); }),
              .hint = MENU_HOOK(hn_output_res)}, menu::before("@Custom output size").order(20))
        .row({.key = "handheld_profile",
              .label = "Profile",
              .help = "Runs the game as the console docked or handheld. Applies after a restart.",
              .value = &opts.handheld,
              .lo = 0, .hi = 1,
              .restart = true,
              .text = [](int v) { return std::string(tx_profile(v)); },
              .hint = hn_handheld}, menu::after("@Custom output size"));
}

} // namespace mkwvk
