// The Insert menu's rows for the replay's caches (#1633), in the Debug tab's Developer section: Descriptor reuse and
// Sampler cache, which apply in play (vk_draw.cpp menu_debug_rows: told when either changes, and by vk_menu.cpp when the
// options are loaded). The call into the renderer is fenced from a layout-only build (MENU_LAYOUT_ONLY, MENU_HOOK).
#include "menu_module.h"

#ifndef MENU_LAYOUT_ONLY
#include "vk_draw.h"   // (menu_debug_rows)
#endif

#include <string>

namespace mkwvk {

namespace {
#ifndef MENU_LAYOUT_ONLY
/// (either row changed: both told to the renderer, which applies them in play)
void debug_rows_changed(int) { menu_debug_rows(opts.descriptor_reuse, opts.sampler_cache); }
#endif
} // namespace

MENU_MODULE(replay_cache) {
    menu::tab("debug")
        .row({.key = "descriptor_reuse",
              .label = "Descriptor reuse",
              .help = "A draw keeps its descriptors while the texture slots it samples are unchanged, so streaming elsewhere in the game no longer "
                      "rebuilds every set each frame. Changes at once.",
              .kind = menu::Kind::Toggle,
              .value = &opts.descriptor_reuse,
              .lo = 0, .hi = 1,
              .preset_exempt = true,
              .on_change = MENU_HOOK(debug_rows_changed)}, menu::after("save_pipeline_list").order(30))
        .row({.key = "sampler_cache",
              .label = "Sampler cache",
              .help = "What a sampler holds is kept beside it on the replay thread, so resolving a sampler that was not registered anew takes no "
                      "lock. Helps while the game streams textures. Changes at once.",
              .kind = menu::Kind::Toggle,
              .value = &opts.sampler_cache,
              .lo = 0, .hi = 1,
              .preset_exempt = true,
              .on_change = MENU_HOOK(debug_rows_changed)}, menu::after("save_pipeline_list").order(40));
}

} // namespace mkwvk
