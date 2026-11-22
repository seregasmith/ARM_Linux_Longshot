// The Insert menu's Sprint row, on the Controls tab (thegrandao's #196, as a module): Link runs, the game's own run after a
// roll, while Sprint's key or gamepad input is held, without the roll (host/oot_sprint.c: the game's dash input,
// HOS_OPT_SPRINT). Turning the row on gives Sprint Shift and the left stick's press, where they are still the defaults'; off
// puts them back. Sprint's own controls are the Controls page's Sprint line (vk_menu.cpp's grid, pad_sprint and key_sprint
// in the options file). The parts that ask the host are fenced from a layout-only build (MENU_LAYOUT_ONLY, MENU_HOOK).
#include "menu_module.h"

#include "menu/sprint.h"

#ifndef MENU_LAYOUT_ONLY
#include "menu/bindings.h"   // (can_bind)
#include "vk_internal.h"     // (host: the bindings, hos_bind)
#endif

#include <string>

namespace mkwvk {

#ifndef MENU_LAYOUT_ONLY
/// The Sprint row turned on: Sprint gets Shift and the left stick's press where they are still the defaults' (Shift on B,
/// which moves to V when no control has V; the press on LS, which keeps its other input), and only when Sprint has none of
/// its own. Off: those go back to B and LS. A control the player rebound is left as it is
/// Publish Sprint and its bindings together after all the moves.
void apply_sprint_layout(bool on) {
    if (!can_bind()) return;
    host->bind(host->version >= 39 ? HOS_BIND_SPRINT_BEGIN : HOS_BIND_SPRINT_ON, 0, 0, 0);
    int moved = host->binding(HOS_BIND_SPRINT_AUTO, 0, 0);
    constexpr int kShift = 0x10, kV = 'V';   // (Windows key codes)
    auto key = [](int b, int s) { return host->binding(HOS_BIND_KEY, b, s); };
    auto pad = [](int b, int s) { return host->binding(HOS_BIND_PAD, b, s); };
    auto key_used = [&](int code) {
        for (int i = 0; i < HOS_KEY_N; i++)
            for (int s = 0; s < 2; s++)
                if (key(i, s) == code) return true;
        return false;
    };
    const int S = HOS_KEY_SPRINT;
    if (on) {
        if (!key(S, 0) && !key(S, 1)) {
            if (key(HOS_SW_B, 0) == kShift && !key_used(kV)) {
                host->bind(HOS_BIND_KEY, HOS_SW_B, 0, kV);
                moved |= HOS_SPRINT_AUTO_B;
            }
            if (!key_used(kShift)) {
                host->bind(HOS_BIND_KEY, S, 0, kShift);
                moved |= HOS_SPRINT_AUTO_KEY;
            }
        }
        if (pad(S, 0) < 0 && pad(S, 1) < 0 && pad(HOS_SW_LS, 0) == HOS_PAD_LSTICK) {
            const int other = pad(HOS_SW_LS, 1);   // (LS's Input 2 moves into its Input 1)
            host->bind(HOS_BIND_PAD, HOS_SW_LS, 1, -1);
            host->bind(HOS_BIND_PAD, HOS_SW_LS, 0, other);
            host->bind(HOS_BIND_PAD, S, 0, HOS_PAD_LSTICK);
            if (pad(S, 0) == HOS_PAD_LSTICK)
                moved |= HOS_SPRINT_AUTO_PAD | ((other + 1) << HOS_SPRINT_AUTO_LS_SHIFT);
        }
        host->bind(HOS_BIND_SPRINT_AUTO, 0, 0, moved);
        host->bind(HOS_BIND_SPRINT_ON, 0, 0, 1);   // (after the moves: the input sees Sprint on with its layout whole)
        return;
    }
    if (moved & HOS_SPRINT_AUTO_B)
        host->bind(HOS_BIND_KEY, HOS_SW_B, 0, kShift);   // (taken from Sprint; V is free again)
    else if (moved & HOS_SPRINT_AUTO_KEY) host->bind(HOS_BIND_KEY, S, 0, 0);
    if (moved & HOS_SPRINT_AUTO_PAD) {
        const int other = pad(HOS_SW_LS, 0);
        host->bind(HOS_BIND_PAD, S, 0, -1);
        host->bind(HOS_BIND_PAD, HOS_SW_LS, 0, HOS_PAD_LSTICK);
        if (other >= 0) host->bind(HOS_BIND_PAD, HOS_SW_LS, 1, other);
    }
    host->bind(HOS_BIND_SPRINT_AUTO, 0, 0, 0);
    host->bind(HOS_BIND_SPRINT_ON, 0, 0, 0);
}

namespace {
/// (the row changed by the player: the bindings follow it)
void sprint_chosen(int v) { apply_sprint_layout(v != 0); }
} // namespace
#endif

MENU_MODULE(sprint) {
    menu::tab("controls")
        .row({.key = "sprint",
              .label = "Sprint",
              .help = "With Sprint On, by default hold Shift, or click and hold the left stick, while moving: Link runs without rolling, talking or "
                      "picking things up, and moves as before once you let go. The Controls page's Sprint Toggle cell changes this to a press to start and stop. Turning Sprint on changes your controls: Shift moves from B to Sprint (B goes "
                      "to V) and the left stick's press from LS to Sprint. Turning it off puts them back. Controls you rebound "
                      "yourself are left alone; the Controls page's Sprint line rebinds it.",
              .kind = menu::Kind::Toggle,
              .value = &opts.sprint,
              .lo = 0, .hi = 1,
              .preset_exempt = true,
              .on_change = MENU_HOOK(sprint_chosen)}, menu::before("@Controls").order(240));
}

} // namespace mkwvk
