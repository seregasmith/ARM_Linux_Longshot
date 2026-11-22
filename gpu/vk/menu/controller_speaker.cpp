// The Insert menu's Controller speaker (host/pad_speaker.c): on the Audio tab, the row that opens its page; on that page,
// the speaker's volume, the sync between it and the TV, headphones in the controller's jack, and where each kind of the
// game's sounds plays. The keys are longshot_options.ini's pad_speaker_* lines and the host's HOS_PAD_SPEAKER_* switches'
// names; the host reads the values by gpu/vk/vk_api.h's HOS_OPT_PAD_SPEAKER_* (gpu/vk/vk_replay.cpp).
#include "menu_module.h"

#include <string>

namespace mkwvk {

namespace {
const char* const kSpeakerHelp =
    "Where this kind of sound plays with a DualSense on a USB cable. Off: with the rest of the game's sound. Controller "
    "only: on the controller's speaker, and no longer on the TV. Controller + TV: on both.";

// (the kinds' rows: host/audio_source.h's classes in order from Navi's voice, opts.pad_speaker[n])
// (above 0 the controller waits, below 0 the TV's copy of its sounds does)
std::string tx_sync(int v) {
    char b[48];
    if (v < 0) snprintf(b, sizeof b, "TV waits %d ms", -v);
    else if (v > 0) snprintf(b, sizeof b, "Controller waits %d ms", v);
    else snprintf(b, sizeof b, "%d ms", 0);
    return b;
}

} // namespace

MENU_MODULE(controller_speaker) {
    menu::tab("audio")
        .row({.key = "@Controller speaker",
              .label = "Controller speaker",
              .help = "Plays Navi and other game sounds on a DualSense's own speaker, or on headphones in its jack: which "
                      "sounds, and how loud. The controller must be on a USB cable. Wireless: works with a Pico 2 W dongle "
                      "running DS5Dongle (awalol).",
              .kind = menu::Kind::Page,
              .opens = "controller_speaker"}, menu::after("audio_output"));
    menu::page("controller_speaker")
        .row({.key = "pad_speaker_volume",
              .label = "Speaker volume",
              .help = "How loud the controller's speaker plays. A short tone plays on it when you change this.",
              .value = &opts.pad_speaker_volume,
              .lo = 0, .hi = 200, .step = 10,
              .preset_exempt = true,
              .stops_at_ends = true,
              .text = menu_text::percent}, menu::order(10))
        .row({.key = "pad_speaker_sync",
              .label = "Controller sync",
              .help = "Lines up the sounds set to Controller + TV. If the controller plays later than the TV, go below 0: the "
                      "TV waits. If the TV plays later, go above 0: the controller waits.",
              .value = &opts.pad_speaker_sync,
              .lo = -100, .hi = 150, .step = 10,
              .preset_exempt = true,
              .stops_at_ends = true,
              .text = tx_sync}, menu::order(20))
        .row({.key = "pad_speaker_headphones",
              .label = "Headphones on the controller",
              .help = "With headphones in the controller's jack. All sound: the whole game plays on them and the TV is silent, "
                      "as on the console. Controller sounds: only the sounds set to play on the controller go to them. Off: "
                      "they are left out.",
              .value = &opts.pad_speaker_headphones,
              .lo = 0, .hi = 2,
              .choices = {"Off", "Controller sounds", "All sound"},
              .preset_exempt = true}, menu::order(30))
        .row({.key = "pad_speaker_headphone_volume",
              .label = "Headphone volume",
              .help = "How loud the headphones in the controller's jack play.",
              .value = &opts.pad_speaker_headphone_volume,
              .lo = 0, .hi = 100, .step = 10,
              .preset_exempt = true,
              .stops_at_ends = true,
              .text = menu_text::percent}, menu::order(40))
        .row({.key = "pad_speaker_navi_voice",
              .label = "Navi's voice",
              .help = kSpeakerHelp,
              .value = &opts.pad_speaker[0],
              .lo = 0, .hi = 2,
              .choices = {"Off", "Controller only", "Controller + TV"},
              .preset_exempt = true}, menu::order(100))
        .row({.key = "pad_speaker_navi_sound",
              .label = "Navi's flight sounds",
              .help = kSpeakerHelp,
              .value = &opts.pad_speaker[1],
              .lo = 0, .hi = 2,
              .choices = {"Off", "Controller only", "Controller + TV"},
              .preset_exempt = true}, menu::order(110))
        .row({.key = "pad_speaker_target",
              .label = "Z-targeting",
              .help = kSpeakerHelp,
              .value = &opts.pad_speaker[2],
              .lo = 0, .hi = 2,
              .choices = {"Off", "Controller only", "Controller + TV"},
              .preset_exempt = true}, menu::order(120))
        .row({.key = "pad_speaker_ocarina",
              .label = "Ocarina",
              .help = kSpeakerHelp,
              .value = &opts.pad_speaker[3],
              .lo = 0, .hi = 2,
              .choices = {"Off", "Controller only", "Controller + TV"},
              .preset_exempt = true}, menu::order(130))
        .row({.key = "pad_speaker_pickup",
              .label = "Pick-ups",
              .help = kSpeakerHelp,
              .value = &opts.pad_speaker[4],
              .lo = 0, .hi = 2,
              .choices = {"Off", "Controller only", "Controller + TV"},
              .preset_exempt = true}, menu::order(140))
        .row({.key = "pad_speaker_item",
              .label = "Items",
              .help = kSpeakerHelp,
              .value = &opts.pad_speaker[5],
              .lo = 0, .hi = 2,
              .choices = {"Off", "Controller only", "Controller + TV"},
              .preset_exempt = true}, menu::order(150))
        .row({.key = "pad_speaker_fairy",
              .label = "Fairies",
              .help = kSpeakerHelp,
              .value = &opts.pad_speaker[6],
              .lo = 0, .hi = 2,
              .choices = {"Off", "Controller only", "Controller + TV"},
              .preset_exempt = true}, menu::order(160))
        .row({.key = "pad_speaker_ui",
              .label = "Menu sounds",
              .help = kSpeakerHelp,
              .value = &opts.pad_speaker[7],
              .lo = 0, .hi = 2,
              .choices = {"Off", "Controller only", "Controller + TV"},
              .preset_exempt = true}, menu::order(170))
        .row({.key = "pad_speaker_agony",
              .label = "Stone of Agony",
              .help = kSpeakerHelp,
              .value = &opts.pad_speaker[8],
              .lo = 0, .hi = 2,
              .choices = {"Off", "Controller only", "Controller + TV"},
              .preset_exempt = true}, menu::order(180))
        .row({.key = "pad_speaker_low_health",
              .label = "Low health alarm",
              .help = kSpeakerHelp,
              .value = &opts.pad_speaker[9],
              .lo = 0, .hi = 2,
              .choices = {"Off", "Controller only", "Controller + TV"},
              .preset_exempt = true}, menu::order(190))
        .row({.key = "pad_speaker_fishing",
              .label = "Fishing",
              .help = kSpeakerHelp,
              .value = &opts.pad_speaker[10],
              .lo = 0, .hi = 2,
              .choices = {"Off", "Controller only", "Controller + TV"},
              .preset_exempt = true}, menu::order(200))
        .row({.key = "pad_speaker_bottle",
              .label = "Bottles",
              .help = kSpeakerHelp,
              .value = &opts.pad_speaker[11],
              .lo = 0, .hi = 2,
              .choices = {"Off", "Controller only", "Controller + TV"},
              .preset_exempt = true}, menu::order(210));
}

} // namespace mkwvk
