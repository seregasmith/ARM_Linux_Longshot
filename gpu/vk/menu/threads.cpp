// The Insert menu's rows for the renderer's threads and queues (#1633), in the Debug tab's Developer section: Threaded
// rendering (threaded_rendering.h: the render thread and the recorder thread, one row: On both, Auto by the cores, Off; its
// texts V-Sync's, tx_vsync; read once at the start, vk_menu.cpp threaded_choice) and Present queue (the presents on a queue of
// their own, vk_main.cpp create_device; off by default since #1549, here so a player can try it; RX_VK_PRESENT_QUEUE holds it
// for the run).
#include "menu_module.h"

#include "threaded_rendering.h"
#include <string>

namespace mkwvk {

namespace {
/// (the environment's switch for this run, over the Threaded rendering row: RX_VK_ASYNC chooses the render thread, and
/// RX_VK_RECORDER the recorder, each for its own half; with both set the first is named)
const char* threaded_env() {
    if (getenv("RX_VK_ASYNC")) return "RX_VK_ASYNC";
    if (getenv("RX_VK_RECORDER")) return "RX_VK_RECORDER";
    return nullptr;
}
/// (Auto's line says what Auto is: the CPU's rule with threaded::kAutoEnabled on, Off everywhere were it off. Its tiers are
/// threaded::kAutoRenderCores and kAutoRecorderCores, 4 and 6 cores: tests/test_threaded_rendering.py holds the text to them)
const char* hn_threaded(int v, int variant) {
    static const char* const t[] = {
        threaded::kAutoEnabled
            ? "On with 6 or more processor cores for the game, the render thread alone with 4 or 5, Off with fewer or where "
              "ruled out."
            : "The same as Off, on every PC.",
        "The picture is drawn on threads of its own while the game goes on to its next frame. Still being tested: it "
        "can show flashes.",
        "The picture is drawn on the game's own thread, as before."};
    if (variant == 1 || (variant < 0 && threaded_env())) {
        static char b[96];
        snprintf(b, sizeof b, "%s is set: it chooses for this run.", threaded_env() ? threaded_env() : "RX_VK_ASYNC or RX_VK_RECORDER");
        return b;
    }
    return variant <= 0 ? t[std::clamp(v, 0, 2)] : nullptr;
}
/// (the Present queue row's line while RX_VK_PRESENT_QUEUE holds it, as RX_VK_ASTC_RGBA8 the ASTC row)
const char* hn_present_queue(int, int variant) {
    const char* e = getenv("RX_VK_PRESENT_QUEUE");
    return variant <= 0 && e && *e ? "Set by RX_VK_PRESENT_QUEUE (longshot_env.txt or the environment): remove that line to choose here."
                                   : nullptr;
}
} // namespace

MENU_MODULE(threads) {
    menu::tab("debug")
        .row({.key = "threaded_rendering",
              .label = "Threaded rendering",
              .help = "Draws the picture on threads of its own, beside the game's, on a processor with threads to spare. Applies after a "
                      "restart.",
              .value = &opts.threaded_rendering,
              .lo = 0, .hi = 2,
              .restart = true,
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_vsync(v)); },
              .hint = hn_threaded}, menu::after("save_pipeline_list").order(20))
        .row({.key = "present_queue",
              .label = "Present queue",
              .help = "The presents on a GPU queue of their own, beside the game's: frame generation's frames more even with V-Sync Off on some "
                      "cards, the GPU lost with frame generation on others (an RTX 5080). Applies after a restart.",
              .kind = menu::Kind::Toggle,
              .value = &opts.present_queue,
              .lo = 0, .hi = 1,
              .restart = true,
              .preset_exempt = true,
              .locked = []() -> const char* { return hn_present_queue(0, -1); },   // (why: its line while RX_VK_PRESENT_QUEUE holds it)
              .hint = hn_present_queue}, menu::after("save_pipeline_list").order(60));
}

} // namespace mkwvk
