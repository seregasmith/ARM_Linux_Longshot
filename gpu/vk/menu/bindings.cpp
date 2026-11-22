// The Insert menu's Controls page's own rows (#1633), above its bindings grid: Layout preset, which face button is which (the
// presets bound through the host, host/nn_hid.c hos_bind, and read back: Custom when one could not be had whole, or when a
// button was bound by hand on the grid) and Button prompts, the button icons the game shows (host/prompts.c). The grid, the
// Controls row that opens the page and the options file's pad_/key_ lines are vk_menu.cpp's (gpu/vk/menu/bindings.h: what
// it uses of this). The parts that ask the host are fenced from a layout-only build (MENU_LAYOUT_ONLY, MENU_HOOK).
#include "menu_module.h"

#include "menu/bindings.h"

#ifndef MENU_LAYOUT_ONLY
#include "vk_internal.h"   // (host: the bindings, hos_bind)
#endif

#include <algorithm>
#include <string>

namespace mkwvk {

#ifndef MENU_LAYOUT_ONLY
static_assert(HOS_PROMPT_N == 6, "the Button prompts row's top (its .hi below) is HOS_PROMPT_N, vk_api.h");
bool can_bind() { return host && host->binding && host->bind && host->pad_raw; }
// (the Switch's buttons on a Switch pad's own: host/nn_hid.c pad_default, in HOS_SW_*'s order)
constexpr int kPadOwn[HOS_SW_N] = {HOS_PAD_EAST, HOS_PAD_SOUTH, HOS_PAD_NORTH, HOS_PAD_WEST, HOS_PAD_LSTICK, HOS_PAD_RSTICK,
                                   HOS_PAD_LSHOULDER, HOS_PAD_RSHOULDER, HOS_PAD_LTRIGGER, HOS_PAD_RTRIGGER, HOS_PAD_START,
                                   HOS_PAD_BACK, HOS_PAD_LEFT, HOS_PAD_UP, HOS_PAD_RIGHT, HOS_PAD_DOWN};
void apply_pad_layout(int layout) {
    if (!can_bind() || layout < 0 || layout > 3) return;
    if (layout == 3) { // Switch 2 Pro (and any Switch pad): every button the pad's own, and the Switch's button icons
        // (again while a button is not its own: see below)
        for (int pass = 0; pass < HOS_SW_N; pass++) {
            bool done = true;
            for (int i = 0; i < HOS_SW_N; i++)
                if (host->binding(HOS_BIND_PAD, i, 0) != kPadOwn[i]) {
                    host->bind(HOS_BIND_PAD, i, 0, kPadOwn[i]);
                    done = false;
                }
            if (done) break;
        }
        opts.prompts = 1;   // (Switch: the Button prompts row)
        return;
    }
    static const int face[4] = {HOS_SW_A, HOS_SW_B, HOS_SW_X, HOS_SW_Y};
    static const int want[3][4] = {
        {HOS_PAD_EAST, HOS_PAD_SOUTH, HOS_PAD_NORTH, HOS_PAD_WEST},   // Nintendo (Default: Bottom=B, Right=A, Top=X, Left=Y)
        {HOS_PAD_SOUTH, HOS_PAD_EAST, HOS_PAD_WEST, HOS_PAD_NORTH},   // Xbox / PC Standard (Bottom=A, Right=B, Left=X, Top=Y)
        {HOS_PAD_SOUTH, HOS_PAD_EAST, HOS_PAD_NORTH, HOS_PAD_WEST},   // Swap Face Buttons (Bottom=A, Right=B, Top=X, Left=Y)
    };
    // (again while a face button is not the preset's: the host leaves an empty Input 1 as it is when the input asked for is
    // another button's only one (hos_bind), and that button may take its own new input later in the same pass)
    for (int pass = 0; pass < 4; pass++) {
        bool done = true;
        for (int i = 0; i < 4; i++)
            if (host->binding(HOS_BIND_PAD, face[i], 0) != want[layout][i]) {
                host->bind(HOS_BIND_PAD, face[i], 0, want[layout][i]);
                done = false;
            }
        if (done) break;
    }
}
int detect_pad_layout() {
    if (!can_bind()) return opts.pad_layout;
    const int a = host->binding(HOS_BIND_PAD, HOS_SW_A, 0);
    const int b = host->binding(HOS_BIND_PAD, HOS_SW_B, 0);
    const int x = host->binding(HOS_BIND_PAD, HOS_SW_X, 0);
    const int y = host->binding(HOS_BIND_PAD, HOS_SW_Y, 0);
    if (a == HOS_PAD_EAST && b == HOS_PAD_SOUTH && x == HOS_PAD_NORTH && y == HOS_PAD_WEST) {
        // (the Switch 2 Pro preset: every button the pad's own, with the Switch's icons; else the face buttons' Nintendo,
        // which is also what the row keeps when it says Nintendo over those same inputs)
        bool own = opts.prompts == 1 && opts.pad_layout != 0;
        for (int i = 0; i < HOS_SW_N && own; i++) own = host->binding(HOS_BIND_PAD, i, 0) == kPadOwn[i];
        return own ? 3 : 0;
    }
    if (a == HOS_PAD_SOUTH && b == HOS_PAD_EAST && x == HOS_PAD_WEST && y == HOS_PAD_NORTH) return 1;
    if (a == HOS_PAD_SOUTH && b == HOS_PAD_EAST && x == HOS_PAD_NORTH && y == HOS_PAD_WEST) return 2;
    return 4;
}
#endif

namespace {
const char* tx_pad_layout(int v) {
    static const char* t[] = {"Nintendo (Def)", "Xbox / PC", "Swap Face", "Switch 2 Pro", "Custom"};
    return t[std::min(std::max(v, 0), 4)];
}
#ifndef MENU_LAYOUT_ONLY
const char* tx_prompts(int v) {
    static const char* t[HOS_PROMPT_N + 1] = {"Auto", "Switch", "Xbox", "PlayStation 4", "PlayStation 5", "Steam Deck",
                                              "Keyboard"};
    return t[std::min(std::max(v, 0), (int)HOS_PROMPT_N)];
}
/// (Layout preset's arrows: round the four presets; 4, Custom, is shown only)
int pad_layout_step(int now, int dir) {
    int v = now + dir;
    if (v > 3) v = dir > 0 ? 0 : 3;
    if (v < 0) v = 3;
    return v;
}
/// (the preset chosen: bound, then read back, Custom when it could not be had whole)
void pad_layout_chosen(int v) {
    apply_pad_layout(v);
    opts.pad_layout = detect_pad_layout();
}
#endif
MODULE_HINTS(hn_pad_layout, 0, "The Switch's order: A on the right, B at the bottom, X at the top, Y on the left.",
             "A at the bottom (action and talk), B on the right, X on the left, Y at the top.",
             "A at the bottom, B on the right; X and Y as on the Switch.",
             "For a Switch 2 Pro Controller, or any Switch pad: every button its own (the sticks, shoulders, triggers, + and -, the "
             "D-pad too, whatever was bound before), and the Switch's button icons.",
             "Bindings of your own, from the Controls page.")
MODULE_HINTS(hn_prompts, 0,
             "Those of the first controller, from its type and name (SDL's). With none, the keyboard's (the Switch's "
             "with XInput alone).",
             "The Switch's buttons.", "An Xbox controller's.", "A PlayStation 4 controller's.", "A PlayStation 5 controller's.",
             "A Steam Deck's.", "The keys and mouse buttons your controls put each action on.")
} // namespace

MENU_MODULE(bindings) {
    menu::page("controls_page")
        .row({.key = "layout_preset",
              .label = "Layout preset",
              .help = "Which face button is which. Binding one yourself on the Controls page makes it Custom.",
              .value = &opts.pad_layout,
              .lo = 0, .hi = 4,
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_pad_layout(v)); },
              .hint = hn_pad_layout,
              .step_fn = MENU_HOOK(pad_layout_step),
              .on_change = MENU_HOOK(pad_layout_chosen)})
        .row({.key = "button_prompts",
              .label = "Button prompts",
              .help = "The button icons the game shows, in its texts and menus: those of this controller, for the buttons your controls "
                      "put each action on.",
              .value = &opts.prompts,
              .lo = 0, .hi = 6,   // (HOS_PROMPT_N)
              .restart = true,
              .preset_exempt = true,
              .text = MENU_HOOK([](int v) { return std::string(tx_prompts(v)); }),
              .hint = hn_prompts});
}

} // namespace mkwvk
