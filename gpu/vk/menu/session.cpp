// The Insert menu's Restart and Quit (#1633): the Gameplay tab's last rows, after Bug report. Each asks for a second press
// (Setting.confirm: "Press again to confirm"; Esc or B cancels), then the menu saves its options and the pipeline cache, as
// the window's own close does not force the cache, and the game starts again or closes (menu_restart_game, menu_quit_game:
// vk_menu.cpp's, menu_module.h).
#include "menu_module.h"

namespace mkwvk {

MENU_MODULE(session) {
    menu::tab("gameplay")
        .row({.key = "restart",
              .label = "Restart",
              .help = "Starts the game again. Save your progress first. Press Enter or A twice to restart. Esc or B cancels.",
              .kind = menu::Kind::Action,
              .on_activate = MENU_HOOK(menu_restart_game),
              .confirm = true}, menu::after("bug_report").order(10))
        .row({.key = "quit",
              .label = "Quit",
              .help = "Save your progress first. Press Enter or A twice to quit. Esc or B cancels.",
              .kind = menu::Kind::Action,
              .on_activate = MENU_HOOK(menu_quit_game),
              .confirm = true}, menu::after("bug_report").order(20));
}

} // namespace mkwvk
