// The Insert menu's controller rows (#1633), on the Controls tab: a controller read while another window has the focus,
// the sticks' dead zone, and its vibration (#405): how, how strongly, and a test of it, which says what plays it.
#include "menu_module.h"
#include "menu/controller.h"

#include <atomic>
#include <string>

namespace mkwvk {

// (longshot) Vibration (host/vibration.h): what plays each controller, and how strongly. Auto plays the best a controller
// can do, Basic its motors, Off nothing; the strength is one percent of what the game asks for, every backend's. The host
// writes back what plays the controller used last (HOS_OPT_VIBRATION_STATUS: its kind times 16, plus what it says), which
// Test vibration's cell and help show.
const char* tx_vibration(int v) {
    static const char* t[] = {"Auto", "Basic", "Off"};
    return (unsigned)v < 3 ? t[v] : t[0];
}
std::atomic<int> vibration_status_cache{0};   // (no controller until the host reports)
int vibration_says() { return vibration_status_cache.load(std::memory_order_relaxed) % 16; }
const char* vibration_cell() {
    static const char* t[] = {"No controller", "Off", "Motors", "HD rumble", "Fine haptics (cable)",
                              "Fine haptics (Bluetooth)", "Four motors", "HD rumble (RenHDrum)", "Motors: no sound card",
                              "Motors: no RenHDrum", "Motors (Bluetooth)", "Grips (RenHDrum)"};
    const int s = vibration_says();
    return (unsigned)s < sizeof t / sizeof t[0] ? t[s] : t[2];
}
const char* vibration_why(int s) {
    static const char* t[] = {
        "No controller is connected.", "Vibration is Off.", "Its two motors play the game's vibration.",
        "Its own HD rumble plays the game's vibration as it is.",
        "Its voice coils play the game's fine vibration, through its USB cable.",
        "Its voice coils play the game's fine vibration over Bluetooth.",
        "All four of its motors play, the triggers' too.",
        "RenHDrum makes its HD rumble.",
        "Its fine haptics play through the sound card it has on a USB cable, which was not found: its motors play "
        "meanwhile.",
        "Its fine haptics need RenHDrum beside the game (renhdrum.dll, or renhdrum.so on Linux), which is not there: its "
        "motors play.",
        "Its Bluetooth connection can't be written to here, so its motors play; on a USB cable, its voice coils play the "
        "game's fine vibration.",
        "Its grips play the game's fine vibration. Experimental: made from one person's measurements of the controller."};
    return (unsigned)s < sizeof t / sizeof t[0] ? t[s] : nullptr;
}
// (under Test vibration's help: what plays the controller now; every one, for the help's height)
const char* hn_vibration_test(int v, int variant) {
    (void)v;
    return vibration_why(variant < 0 ? vibration_says() : variant);
}
// (end of the vibration texts: tests/test_vibration_menu.py compiles them)

namespace {
MODULE_HINTS(hn_background, 0, "Only while the game's window has the focus.",
             "Also behind other windows, for streaming or other apps. The keyboard and mouse still need the game's window.")
const char* tx_stick_dead_zone(int v) {
    static const char* t[] = {"Default (24%)", "5%", "10%", "15%", "20%", "30%"};
    return (unsigned)v < 6 ? t[v] : t[0];
}
} // namespace

MENU_MODULE(controller) {
    menu::tab("controls")
        .row({.key = "controller_in_background",
              .label = "Controller in background",
              .help = "Whether the controller plays the game while another window has the focus.",
              .kind = menu::Kind::Toggle,
              .value = &opts.pad_background,
              .lo = 0, .hi = 1,
              .preset_exempt = true,
              .hint = hn_background}, menu::before("@Controls").order(100))
        .row({.key = "stick_dead_zone",
              .label = "Stick dead zone",
              .help = "How far a stick moves from center in any direction before the game sees it. "
                      "Lower for a precise stick, higher for one that drifts. "
                      "The game's own dead zone still follows.",
              .value = &opts.stick_dead_zone,
              .lo = 0, .hi = 5,
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_stick_dead_zone(v)); }}, menu::before("@Controls").order(190))
        .row({.key = "vibration",
              .label = "Vibration",
              .help = "How the controller vibrates. Auto plays the best it can: a DualSense its voice coils, a Switch controller "
                      "and an Xbox controller RenHDrum's HD rumble, a Steam Controller (2026) its grips, every other controller "
                      "its motors. Basic plays the motors on every controller. Off stops it.",
              .value = &opts.vibration,
              .lo = 0, .hi = 2,
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_vibration(v)); }}, menu::before("@Controls").order(200))
        // (Vibration strength: under the key it had as Rumble strength)
        .row({.key = "rumble_strength",
              .label = "Vibration strength",
              .help = "How strongly the controller vibrates, in percent of what the game asks for. A change gives a short buzz at the new "
                      "strength.",
              .value = &opts.rumble,
              .lo = 25, .hi = 200,
              .step = 25,
              .preset_exempt = true,
              .stops_at_ends = true,
              .text = menu_text::percent}, menu::before("@Controls").order(201))
        .row({.key = "test_vibration",
              .label = "Test vibration",
              .help = "A short buzz on the controller used last. The value says what plays its vibration now.",
              .kind = menu::Kind::Action,
              .text = [](int) { return std::string(vibration_cell()); },   // (its cell: what plays the controller now)
              .hint = hn_vibration_test,   // (its help line: the same, in words; every one, for the help's height)
              .on_activate = MENU_HOOK(menu_rumble_test),
              .verb = "Test"}, menu::before("@Controls").order(202))
        // (RenHDrum's HD Haptics for a DualSense it plays: host/rumble_renhdrum.c, HOS_OPT_DS_HD_HAPTICS)
        .row({.key = "dualsense_hd_haptics",
              .label = "DualSense HD Haptics",
              .help = "A DualSense also taps its speaker and kicks back on L2 and R2 while you hold them, from the game's "
                      "vibration, with deeper heavy hits. Off: its fine haptics alone, as before.",
              .kind = menu::Kind::Toggle,
              .value = &opts.ds_hd_haptics,
              .lo = 0, .hi = 1,
              .preset_exempt = true}, menu::before("@Controls").order(203));
}

} // namespace mkwvk
