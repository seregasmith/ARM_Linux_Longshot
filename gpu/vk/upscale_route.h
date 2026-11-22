// The gameplay upscale as it was drawn, for the FPS counter's footer: each path that draws it (the game's network, its
// stand-ins, an engine, Off's stretch) publishes what it recorded, after recording it, with the 3D's size and the picture's.
// The footer reads the latest record. Nothing here feeds the upscalers: their camera, output size and gates are their own.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "upscaler_rows.h"

namespace mkwvk::upscale_route {

enum class Route : uint8_t {
    Network,    // (the game's own network, as itself)
    StandIn,    // (a typed upscale in place of a network that does not translate or draws wrong: upscale_stub, taau_stand_in)
    Blit,       // (a linear blit in place of one: upscale_stub's last resort)
    Engine,     // (DLSS, DLAA, FSR 3.1, FSR 4, XeSS: engine says which)
    Stretch,    // (the Upscaler's Off)
};

struct Sample {
    Route route = Route::Network;
    int engine = upscaler_rows::GAME;   // (Route::Engine: the upscaler that drew, upscaler_rows' numbering)
    bool fxaa = false;                  // (Route::StandIn: FXAA on its input)
    uint32_t in_w = 0, in_h = 0, out_w = 0, out_h = 0;
};

// (display thread) The footer shows its render lines now: the drawing paths publish for a second after the last call
void want();
// (replay thread) Whether any path should publish: one relaxed load and a clock read
bool wanted();
// (replay thread) A draw into what may be the 3D scene (a colour target in the scene's formats, with depth and a camera): the
// largest such size lately is how a network known by its shape only is told to be the gameplay upscale
void note_scene(uint32_t w, uint32_t h);
bool is_scene(uint32_t w, uint32_t h);
// (replay thread) What a path recorded into the picture. Only while wanted()
void publish(const Sample& s);
// (display thread) The latest record, if one came in the last max_age_ns (none in menus, movies and loading screens)
bool latest(Sample* out, int64_t max_age_ns);

// The footer's two lines for a record (null: none lately). The first says the resolutions only, the second the method only:
// "Upscaling:" where the picture is scaled up, "AA:" where it is drawn at full size and only anti-aliased; anti-aliasing named
// only where the method does not imply it (the fallbacks, Off). upscaler and resolution are the Upscaler and Render resolution
// rows as set: the record says what drew, these only what was asked for (an engine that did not draw, Auto). English on
// purpose: screenshots and bug reports use the same words in every language
inline const char* engine_name(int u) {
    namespace ur = upscaler_rows;
    switch (u) {
    case ur::DLSS: return "DLSS";
    case ur::DLAA: return "DLAA";
    case ur::FSR3: return "FSR 3.1";
    case ur::FSR4: case ur::FSR4_AA: return "FSR 4";
    case ur::XESS: case ur::XESS_AA: return "XeSS";
    case ur::OFF: return "off";
    default: return "Built-in";
    }
}

/// The Render resolution stop a width share is (within a point), as the engine names it; null when none
inline const char* stop_name(int engine, int share) {
    namespace ur = upscaler_rows;
    for (int v = 1; v < ur::kValues; v++) {
        const int p = ur::percent(v);
        if (p <= 0 || p >= 100 || share < p - 1 || share > p + 1) continue;
        return ur::is_xess(engine) ? ur::xess_mode_name(v) : ur::dlss_mode_name(v);
    }
    return nullptr;
}

inline void footer_lines(const Sample* s, int upscaler, int resolution, std::string out[2]) {
    namespace ur = upscaler_rows;
    char b[128];
    if (!s) {
        out[0] = "Resolution: not in gameplay";
        out[1] = std::string("Upscaling: ") + engine_name(upscaler) + " (chosen)";
        return;
    }
    const bool native = s->in_w == s->out_w && s->in_h == s->out_h;
    const int share = s->out_w ? (int)((200u * s->in_w + s->out_w) / (2u * s->out_w)) : 0;
    if (native) snprintf(b, sizeof b, "Resolution %ux%u (100%%)", s->in_w, s->in_h);
    else snprintf(b, sizeof b, "Resolution %ux%u to %ux%u (%d%%)", s->in_w, s->in_h, s->out_w, s->out_h, share);
    out[0] = b;
    const std::string label = native ? "AA: " : "Upscaling: ";
    const bool asked_engine = upscaler != ur::GAME && upscaler != ur::OFF;
    const std::string missing = asked_engine ? std::string(" (") + engine_name(upscaler) + " not running)" : "";
    switch (s->route) {
    case Route::Network:
        out[1] = label + "Built-in" + missing;
        return;
    case Route::StandIn:
        out[1] = label + "Built-in fallback, " + (s->fxaa ? "FXAA" : "no AA") + missing;
        return;
    case Route::Blit:
        out[1] = label + "linear blit, no AA" + missing;
        return;
    case Route::Stretch:
        out[1] = native ? "Upscaling: off, no AA" : "Upscaling: off (stretched)";
        return;
    case Route::Engine:
        break;
    }
    if (native) {
        out[1] = label + engine_name(s->engine);
        return;
    }
    const char* stop = stop_name(s->engine, share);
    if (stop) snprintf(b, sizeof b, "Upscaling: %s %s", engine_name(s->engine), stop);
    else snprintf(b, sizeof b, "Upscaling: %s %d%%", engine_name(s->engine), share);
    out[1] = b;
    if (ur::percent(resolution) == 0) out[1] += " (Auto)";
}

}   // namespace mkwvk::upscale_route
