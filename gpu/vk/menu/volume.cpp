// The Insert menu's game volume rows (#1633): the Audio tab's first two, how loud the game plays and whether it is
// silent while another window has the focus.
#include "menu_module.h"

#include <string>

namespace mkwvk {

MENU_MODULE(volume) {
    menu::tab("audio")
        .row({.key = "volume",
              .label = "Volume",
              .help = "How loud the game plays. The game's own sound settings and Windows' volume still apply.",
              .kind = menu::Kind::Slider,
              .value = &opts.volume,
              .lo = 0, .hi = 100,
              .step = 5,
              .preset_exempt = true,
              .text = menu_text::percent}, menu::before("output_device"))
        .row({.key = "mute_in_background",
              .label = "Mute in background",
              .help = "Silences the game while another window has the focus. The sound comes back when you switch to the game.",
              .kind = menu::Kind::Toggle,
              .value = &opts.mute_background,
              .lo = 0, .hi = 1,
              .preset_exempt = true}, menu::before("output_device"));
}

} // namespace mkwvk
