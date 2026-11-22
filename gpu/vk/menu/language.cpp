// The Insert menu's language rows (#1633), on the Gameplay tab: the game's language (language.txt, written when the row is
// left: vk_menu.cpp game_lang_tick) and this menu's.
#include "menu_module.h"

#include "vk_menu_i18n.h"
#include <string>

namespace mkwvk {

namespace {
/// The Game language row (opts.game_lang): language.txt's language, written once the row is left (game_lang_tick); the game
/// reads it at its start
const char* tx_game_lang(int v) { return i18n::game_lang_name(v); }
/// The Menu language row (opts.menu_lang): Auto, then each language by its own name (vk_menu_i18n.h)
const char* tx_menu_lang(int v) {
    if (v <= 0) return "Auto (game's language)";
    return i18n::lang_name(v - 1);
}
const char* hn_menu_lang(int v, int variant) {
    if (variant > 0) return nullptr;
    return v <= 0 ? "Follows the game's language (language.txt in the game folder)." : "The language of this menu.";
}
} // namespace

MENU_MODULE(language) {
    menu::tab("gameplay")
        .row({.key = "game_language",
              .label = "Game language",
              .help = "The game's own language, its text. Applies after a restart.",
              .value = &opts.game_lang,
              .lo = 0, .hi = i18n::game_lang_count() - 1,
              .restart = true,
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_game_lang(v)); }}, menu::before("bug_report").order(400))
        .row({.key = "menu_language",
              .label = "Menu language",
              .help = "The language of this menu.",
              .value = &opts.menu_lang,
              .lo = 0, .hi = i18n::lang_count(),
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_menu_lang(v)); },
              .hint = hn_menu_lang}, menu::before("bug_report").order(410));
}

} // namespace mkwvk
