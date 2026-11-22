// The Insert menu's HDR rows (#1633): the Graphics tab's HDR section, after Screen. HDR is the picture's mode (Off, Expanded,
// Native); the paper white, peak brightness and contrast show with HDR on, the tone mapper with Native alone. Their keys in
// longshot_options.ini are as they were in vk_menu.cpp's table (tests/test_menu_ini_keys.py keeps them).
#include "menu_module.h"

#include <cstdio>
#include <string>

namespace mkwvk {

const char* tx_hdr(int v) { return v == 1 ? "Expanded" : v == 2 ? "Native (the game's)" : "Off"; }

namespace {
// (as a printf format: the translations match a value's text against it)
std::string nits(int v) {
    char b[24];
    snprintf(b, sizeof b, "%d nits", v);
    return b;
}
} // namespace

MENU_MODULE(hdr) {
    menu::tab("graphics")
        .heading("hdr", "HDR", menu::after("screen"))
        .row({.key = "hdr", .label = "HDR",
              .help = "HDR10 out on an HDR display (Windows: Use HDR on in the display settings). Expanded: the game's SDR "
                      "picture, its white at the paper white and its highlights up to the peak. Native: the game's own HDR, "
                      "told the display's peak and paper white.",
              .value = &opts.hdr, .lo = 0, .hi = 2, .preset_exempt = true, .text = [](int v) { return std::string(tx_hdr(v)); },
              .hint = hdr_sdr_hint})
        .row({.key = "hdr_paper_white", .label = "HDR paper white",
              .help = "How bright the picture's white is with HDR on. Around 200 for a normal room; higher in a bright one.",
              .value = &opts.hdr_white, .lo = 80, .hi = 400, .step = 10, .preset_exempt = true,
              .shown = [] { return opts.hdr != 0; }, .text = nits})
        .row({.key = "hdr_peak_brightness", .label = "HDR peak brightness",
              .help = "The brightest highlights with HDR on: set it to your display's peak brightness.",
              .value = &opts.hdr_peak, .lo = 400, .hi = 4000, .step = 100, .preset_exempt = true,
              .shown = [] { return opts.hdr != 0; }, .text = nits})
        .row({.key = "hdr_contrast", .label = "HDR contrast",
              .help = "The HDR picture's contrast around mid grey, as the RenoDX HDR mods grade it: above 100% deeper shadows "
                      "and brighter highlights, which ease into the peak brightness; below it flatter. With Native the game's "
                      "own HDR picture is graded too, under the peak brightness the game is told.",
              .value = &opts.hdr_contrast, .lo = 50, .hi = 200, .step = 5, .preset_exempt = true,
              .shown = [] { return opts.hdr != 0; }, .text = menu_text::percent})
        .row({.key = "hdr_tone_mapper", .label = "HDR tone mapper",
              .help = "With Native, how the game's scene meets the peak brightness. Vanilla: the game's own curve. PsychoV35: "
                      "RenoDX's tone mapper, the same mid grey with highlights that roll off into the peak and keep their "
                      "colour.",
              .value = &opts.hdr_tonemap, .lo = 0, .hi = 1, .choices = {"Vanilla", "PsychoV35"}, .preset_exempt = true,
              .shown = [] { return opts.hdr == 2; }});
}

} // namespace mkwvk
