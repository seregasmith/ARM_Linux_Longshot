// The Insert menu's Discord row (#1633), on the Gameplay tab: whether Discord shows what you are playing.
#include "menu_module.h"

#include <string>

namespace mkwvk {

MENU_MODULE(discord) {
    menu::tab("gameplay")
        .row({.key = "discord_status",
              .label = "Discord status",
              .help = "Shows \"Playing Longshot\" and the time played in your Discord status, while Discord runs on this PC.",
              .kind = menu::Kind::Toggle,
              .value = &opts.discord,
              .lo = 0, .hi = 1,
              .preset_exempt = true}, menu::before("bug_report").order(500));
}

} // namespace mkwvk
