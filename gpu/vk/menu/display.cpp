// The Insert menu's display rows (#1633), the Graphics tab's Screen section: fullscreen or a window, the monitor fullscreen
// uses (the host's monitors listed, the choice kept by its ID and name and told to the host; vk_menu.cpp keeps its lines in
// the options file and follows the window's screen: menu/display.h), V-Sync, and the filter the picture is scaled to the
// window with. V-Sync's text is also Threaded rendering's (tx_vsync, menu_module.h). The parts that ask the host are fenced
// from a layout-only build (MENU_LAYOUT_ONLY, MENU_HOOK: menu_module.h).
#include "menu_module.h"

#include "menu/display.h"

#ifndef MENU_LAYOUT_ONLY
#include "menu/audio.h"     // (output_name_key: a monitor's name on one line, as a device's)
#include "vk_internal.h"    // (host: the host's monitors)
#include "vk_menu_i18n.h"   // (i18n::tr: the unplugged monitor's line)
#endif

#include <algorithm>
#include <string>

namespace mkwvk {

int monitor_pick = 0;   // (the Fullscreen monitor row: 0 Auto, then fullscreen_monitors from 1; past them, the saved one unplugged)

#ifndef MENU_LAYOUT_ONLY
std::vector<monitor_choice::Choice> fullscreen_monitors;
std::string monitor_want, monitor_saved_name;
bool monitor_missing = false;
bool can_monitors() { return host && host->monitor_count && host->monitor_info && host->monitor_select; }
const char* tx_fullscreen_monitor(int value) {
    static std::string text;
    if (value <= 0) return "Auto (current monitor)";
    if (monitor_missing && value == (int)fullscreen_monitors.size() + 1) {
        text = (monitor_saved_name.empty() ? std::string(i18n::tr("Selected monitor")) : monitor_saved_name) +
               i18n::tr(" (not connected)");
        return text.c_str();
    }
    if ((size_t)(value - 1) >= fullscreen_monitors.size()) return "Auto (current monitor)";
    const auto& choice = fullscreen_monitors[value - 1];
    text = choice.name + " (" + std::to_string(choice.width) + "x" + std::to_string(choice.height) + ")";
    int duplicates = 0;
    for (const auto& c : fullscreen_monitors) if (c.name == choice.name) duplicates++;
    if (duplicates > 1) text += " [" + std::to_string(value) + "]";
    return text.c_str();
}
const char* hn_fullscreen_monitor(int, int variant) {
    if (variant > 0) return nullptr;
    if (variant < 0) return "Choose the screen used by Fullscreen, F11 and Alt+Enter. Auto uses the window's current monitor.";
    if (monitor_missing) return "The selected monitor is unavailable. The current monitor is used temporarily; the saved choice is kept.";
    return "While windowed, this chooses the next fullscreen screen. While fullscreen, it moves the game now. Leaving fullscreen restores the window.";
}
/// (longshot) The host's monitors listed again, the row on the chosen one (by its ID, monitor_choice.h pick; past the list,
/// the saved one unplugged), its name kept while it is there; the menu drawn again when any of that changed
void refresh_monitors() {
    const int old_count = (int)fullscreen_monitors.size(), old_pick = monitor_pick;
    const bool old_missing = monitor_missing;
    const std::string old_name = monitor_saved_name;
    fullscreen_monitors.clear();
    if (can_monitors()) {
        const int count = std::min(64, std::max(0, host->monitor_count()));
        for (int i = 0; i < count; i++) {
            MkwVkMonitor monitor{};
            if (!host->monitor_info(i, &monitor)) continue;
            monitor.id[sizeof monitor.id - 1] = monitor.name[sizeof monitor.name - 1] = 0;
            const std::string id = monitor_choice::id_key(monitor.id);
            if (!id.empty()) fullscreen_monitors.push_back({id, output_name_key(monitor.name), monitor.width, monitor.height, monitor.flags});
        }
    }
    monitor_pick = monitor_choice::pick(fullscreen_monitors, monitor_want);
    monitor_missing = monitor_pick > (int)fullscreen_monitors.size();
    if (monitor_pick > 0 && !monitor_missing) monitor_saved_name = fullscreen_monitors[monitor_pick - 1].name;
    menu_row_top(&monitor_pick, (int)fullscreen_monitors.size() + (monitor_missing ? 1 : 0));
    if (old_count != (int)fullscreen_monitors.size() || old_pick != monitor_pick ||
        old_missing != monitor_missing || old_name != monitor_saved_name) menu_redraw();
}
void monitor_chosen() {
    if (!can_monitors()) return;
    if (monitor_pick <= 0) monitor_want.clear(), monitor_saved_name.clear();
    else if ((size_t)(monitor_pick - 1) < fullscreen_monitors.size()) {
        monitor_want = fullscreen_monitors[monitor_pick - 1].id;
        monitor_saved_name = fullscreen_monitors[monitor_pick - 1].name;
    }
    host->monitor_select(monitor_want.c_str());
    refresh_monitors();
}
#endif

const char* tx_vsync(int v) {
    static const char* const t[] = {"Auto", "On", "Off"};
    return t[std::clamp(v, 0, 2)];
}

namespace {
const char* hn_vsync(int v, int variant) {
    static const char* const t[] = {
        "No tearing and no wait where the driver allows it (mailbox); otherwise as On, or on Windows as Off.",
        "Each frame waits for the display's refresh: no tearing, at most the display's rate.",
        "Each frame is shown at once: the least delay, and it can tear. Where the driver cannot, as Auto."};
    if (variant == 1 || (variant < 0 && display_vsync_env())) {
        static char b[96];
        snprintf(b, sizeof b, "%s is set: it chooses for this run.", display_vsync_env() ? display_vsync_env() : "RX_VK_FIFO or RX_VK_IMMEDIATE");
        return b;
    }
    return variant <= 0 ? t[std::clamp(v, 0, 2)] : nullptr;
}
/// The Window scaling filter row's value: its filter, or that none is needed (the saved value stays, for another window size)
const char* tx_filter(int v) { return display_filter_needed() ? display_upscale_text(v) : "Not needed (window = output)"; }
/// The Window scaling filter: its filter, or that none is needed at this window's size
const char* hn_filter(int v, int variant) {
    static const char* const t[] = {"The plain copy.", "Sharper.", "Sharper still; can ring on edges."};
    static const char* const none = "The window is the Output resolution's size: nothing to scale. The choice is kept for "
                                    "another size.";
    if (variant < 0) return display_filter_needed() ? t[std::clamp(v, 0, 2)] : none;
    return variant == 0 ? t[std::clamp(v, 0, 2)] : variant == 1 ? none : nullptr;
}
} // namespace

MENU_MODULE(display) {
    menu::tab("graphics")
        .row({.key = "fullscreen",
              .label = "Fullscreen",
              .help = "Fills the whole screen with the game, without the window's border. The same as F11.",
              .kind = menu::Kind::Toggle,
              .value = &opts.fullscreen,
              .lo = 0, .hi = 1,
              .preset_exempt = true}, menu::before("fullscreen_monitor"))
        .section("screen")   // (the core's Screen heading: the monitor its first row, the others beside it)
        .row({.key = "fullscreen_monitor",
              .label = "Fullscreen monitor",
              .help = "Choose the screen used by Fullscreen, F11 and Alt+Enter. Auto uses the window's current monitor.",
              .value = &monitor_pick,
              .lo = 0, .hi = 0,
              .preset_exempt = true,
              .text = MENU_HOOK([](int v) { return std::string(tx_fullscreen_monitor(v)); }),
              .hint = MENU_HOOK(hn_fullscreen_monitor),
              .on_change = MENU_HOOK([](int) { monitor_chosen(); })})
        .row({.key = "vsync",
              .label = "V-Sync",
              .help = "Whether a frame waits for the display's refresh before it is shown. Changes at once.",
              .value = &opts.vsync,
              .lo = 0, .hi = 2,
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_vsync(v)); },
              .hint = hn_vsync}, menu::after("fullscreen_monitor").order(10))
        .row({.key = "upscaling",
              .label = "Window scaling filter",
              .help = "How the finished picture is scaled to the window, when its size differs from the Output resolution.",
              .value = &opts.upscale,
              .lo = 0, .hi = 2,
              .text = [](int v) { return std::string(tx_filter(v)); },
              .hint = hn_filter}, menu::after("fullscreen_monitor").order(20));
}

} // namespace mkwvk
