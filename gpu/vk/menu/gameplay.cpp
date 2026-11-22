// The Insert menu's rows for how the game plays and starts (#1633), on the Gameplay tab: its speed, the camera's field of
// view, the text's speed and size, and skipping the title screen and the opening.
#include "menu_module.h"

#include <string>

namespace mkwvk {

namespace {
const char* tx_speed(int v) { return v ? "Real time" : "Per frame"; }
MODULE_HINTS(hn_speed, 0, "A step each frame: the game slows down whenever it runs below 60 fps.",
             "Normal speed down to 15 fps, by taking longer steps.")
const char* tx_fov(int v) { return v == 100 ? "100% (original)" : menu_text::percent_c(v); }
const char* tx_text_speed(int v) {
    static const char* t[] = {"Normal", "2x", "4x", "Instant"};
    if ((unsigned)v >= sizeof t / sizeof t[0]) return t[0];
    return t[v];
}
MODULE_HINTS(hn_text_speed, 0, "The game's own pace, with its pauses.", "Twice as fast.", "Four times as fast.",
             "Each page at once, as pressing A does.")
const char* tx_text_size(int v) {
    static const char* t[] = {"Original", "110%", "125%", "150%", "175%"};
    if ((unsigned)v >= sizeof t / sizeof t[0]) return t[0];
    return t[v];
}
MODULE_HINTS(hn_text_size, 0, "The game's original size.", "A little larger.", "Larger.", "Much larger.", "Largest.")
} // namespace

MENU_MODULE(gameplay) {
    menu::tab("gameplay")
        .row({.key = "game_speed",
              .label = "Game speed",
              .help = "How the game keeps time when it runs below 60 fps. 30 FPS always uses real-time game speed, even when Game speed is Per frame.",
              .value = &opts.realtime,
              .lo = 0, .hi = 1,
              .text = [](int v) { return std::string(tx_speed(v)); },
              .hint = hn_speed}, menu::before("bug_report").order(300))
        .row({.key = "field_of_view",
              .label = "Field of view",
              .help = "How much of the world you see: 100% is the original view. Higher is wider, lower is closer. Changes apply at once "
                      "during gameplay. Cutscenes keep their own view.",
              .kind = menu::Kind::Slider,
              .value = &opts.fov,
              .lo = 50, .hi = 150,
              .step = 5,
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_fov(v)); }}, menu::before("bug_report").order(310))
        .row({.key = "text_speed",
              .label = "Text speed",
              .help = "How fast the letters of dialogue and other messages appear.",
              .value = &opts.text_speed,
              .lo = 0, .hi = 3,
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_text_speed(v)); },
              .hint = hn_text_speed}, menu::before("bug_report").order(320))
        .row({.key = "text_size",
              .label = "Text size",
              .help = "How large the text of dialogue, Navi and signs is drawn.",
              .value = &opts.text_size,
              .lo = 0, .hi = 4,
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_text_size(v)); },
              .hint = hn_text_size}, menu::before("bug_report").order(330))
        .row({.key = "skip_title_screen",
              .label = "Skip title screen",
              .help = "Starts a launch with the most recent saved game: presses through the title screen and its file list. Does nothing "
                      "until the game has a save. Applies after a restart.",
              .kind = menu::Kind::Toggle,
              .value = &opts.skip_title,
              .lo = 0, .hi = 1,
              .restart = true,
              .preset_exempt = true}, menu::before("bug_report").order(340))
        .row({.key = "skip_opening",
              .label = "Skip opening",
              .help = "Skips the new-game opening (the story movie and Navi waking Link) and leaves later cutscenes, the Deku Tree "
                      "meeting, and the title screen alone.",
              .kind = menu::Kind::Toggle,
              .value = &opts.skip_opening,
              .lo = 0, .hi = 1,
              .preset_exempt = true}, menu::before("bug_report").order(600));
}

} // namespace mkwvk
