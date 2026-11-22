// The Insert menu's Audio tab (#1633), after the game volume rows (menu/volume.cpp): where the game plays sound (the host's
// playback devices, host/audio_output_devices.h), the speakers it mixes for (host/nn_audio.c au_menu_output), and the
// microphone the ocarina hears (host/nn_audio_in.c), its gain and a live test of it. The device lists and the choices are this
// module's; vk_menu.cpp keeps their lines in the options file and draws the test's meter (menu/audio.h). The parts that ask
// the host are fenced from a layout-only build (MENU_LAYOUT_ONLY, MENU_HOOK: menu_module.h).
#include "menu_module.h"

#include "menu/audio.h"

#ifndef MENU_LAYOUT_ONLY
#include "../../../host/audio_output_devices.h"
#include "vk_internal.h"    // (host: the host's devices and microphones)
#include "vk_menu_i18n.h"   // (i18n::tr: the unplugged device's line)
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace mkwvk {

int output_pick = 0;   // (the Output device row: 0 Default, then output_devices from 1; past them, the saved one unplugged)
constexpr int kMaxMics = 64;   // (the Microphone row: Off, Default, then at most this many of the host's)

#ifndef MENU_LAYOUT_ONLY
// The playback row stores an endpoint ID, never its position or possibly duplicated display name.
// Menu state uses menu_mu, shared by menu_tick and the control bridge (vk_menu.cpp); the host mailbox has its own lock.
struct OutputChoice { std::string id, name; };
std::vector<OutputChoice> output_devices;
std::string output_want, output_saved_name;
bool output_missing = false;
bool can_outputs() {
    return host && host->audio_output_count && host->audio_output_info && host->audio_output_select && host->audio_output_state;
}
bool output_forced() { const char* e = getenv("HOS_AUDIO_DEVICE"); return e && *e; }
std::string output_id_key(const char* text) {
    std::string s(text ? text : "");
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    char valid[HOS_OUTPUT_ID_CAP];
    return audio_output_copy_id(valid, sizeof valid, s.c_str()) ? s : std::string();
}
std::string output_name_key(const char* text) {
    std::string s(text ? text : "");
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    for (char& c : s) if ((unsigned char)c < 0x20 || c == 0x7f) c = ' ';
    return s;
}
const char* tx_output_device(int v) {
    static std::string text;
    if (output_forced()) return "Overridden by HOS_AUDIO_DEVICE";
    if (v <= 0) return "Default";
    if (output_missing && v == (int)output_devices.size() + 1) {
        text = (output_saved_name.empty() ? std::string(i18n::tr("Selected output")) : output_saved_name) + i18n::tr(" (not connected)");
        return text.c_str();
    }
    if ((size_t)(v - 1) >= output_devices.size()) return "Default";
    text = output_devices[v - 1].name;
    int duplicates = 0;
    for (const auto& d : output_devices) if (d.name == text) duplicates++;
    if (duplicates > 1) text += " [" + std::to_string(v) + "]";
    return text.c_str();
}
const char* hn_output_device(int, int variant) {
    if (variant > 0) return nullptr;
    if (variant < 0) return "Where the game plays sound. A change briefly restarts only the audio stream.";
    if (output_forced()) return "HOS_AUDIO_DEVICE overrides this run. The saved device choice is kept.";
    const int state = can_outputs() ? host->audio_output_state() : 0;
    if (state & HOS_OUTPUT_UNAVAILABLE) return "No playback output could be opened. The game keeps trying.";
    if (output_missing || (state & HOS_OUTPUT_FALLBACK))
        return "Default is used temporarily. The selected output is remembered and retried when available.";
    return output_pick ? "This playback device. Speaker layout changes still require a game restart."
#ifdef _WIN32
                       : "Follow the Windows default playback device.";
#else
                       : "Follow the system default playback device.";
#endif
}

// (longshot) the Microphone row: Off, the system's default, or one of the host's microphones (host/nn_audio_in.c: the
// ocarina's input streams), told to the host by its name (host->mic_select) whenever it changes. The chosen one is kept by
// its name, mic_want, also while it is unplugged: the row then has one more entry for it, "<name> (not connected)", the
// host lets the default stand in and opens it again once it is back, and the name stays in the ini. In the ini the name is
// cut to kMicNameKept bytes (at a character's start), so the line fits the 80-byte readers of the file (host/nn_hid.c,
// nn_applet.c, hos_main.c), and a name is matched by that much of it (the row lists kMaxMics, above, at most)
constexpr size_t kMicNameKept = 60;
std::string mic_want;      // (the chosen microphone's name, cut to kMicNameKept; empty: Off or the default chosen)
bool mic_missing = false;  // (mic_want is not plugged in: the row's last entry, 2 + mic_listed, stands for it)
int mic_listed = 0;        // (the microphones as the host last listed them)
bool can_mic() { return host && host->mic_count && host->mic_name && host->mic_select; }
size_t text_char_start(const std::string& text, size_t at) {
    while (at > 0 && at < text.size() && ((unsigned char)text[at] & 0xc0) == 0x80) at--;
    return at;
}
std::string mic_key(const char* name) {
    std::string k(name ? name : "");
    for (char& c : k)   // (a device's name goes into longshot_options.ini: a line break in it would add lines there)
        if ((unsigned char)c < 0x20 || c == 0x7f) c = ' ';
    if (k.size() > kMicNameKept) k = k.substr(0, text_char_start(k, kMicNameKept));
    // (and no spaces at the end: a saved line's end, made one above, and those an older build saved with the name)
    while (!k.empty() && k.back() == ' ') k.pop_back();
    return k;
}
/// buf cut back to whole UTF-8 characters (snprintf may have cut one)
void utf8_whole(char* buf) {
    size_t n = strlen(buf), i = n;
    while (i > 0 && ((unsigned char)buf[i - 1] & 0xC0) == 0x80) i--;   // (the last character's continuation bytes)
    if (i == 0) return;
    const unsigned char lead = (unsigned char)buf[i - 1];
    const size_t want = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (n - (i - 1) < want) buf[i - 1] = 0;
}
const char* tx_mic(int v) {
    static char buf[160];
    if (v <= 0) return "Off";
    if (v == 1) return "Default";
    if (mic_missing && v == 2 + mic_listed) {
        const int room = (int)sizeof buf - (int)sizeof " (not connected)";
        snprintf(buf, (size_t)room + 1, "%s", mic_want.c_str());
        utf8_whole(buf);
        strcat(buf, " (not connected)");
        return buf;
    }
    const char* n = can_mic() ? host->mic_name(v - 2) : "";
    snprintf(buf, sizeof buf, "%s", n && *n ? n : "Microphone");
    utf8_whole(buf);
    return buf;
}
/// The Microphone row: Off, the default, a microphone of the list, or the chosen one while it is unplugged (variant 1)
const char* hn_mic(int v, int variant) {
    if (v <= 0) return variant <= 0 ? "The game hears silence." : nullptr;
    if (v == 1)
        return variant <= 0 ? "The system's default recording device. On Windows, desktop apps must be allowed to use the "
                              "microphone (Settings, Privacy & security)."
                            : nullptr;
    const bool missing = variant < 0 ? mic_missing && v == 2 + mic_listed : variant == 1;
    if (variant > 1) return nullptr;
    return missing ? "Not plugged in: Default stands in until it is back." : "This microphone, by its name. A change applies at once.";
}
// Audio output: layout read at startup (host/nn_audio.c au_menu_output); Mono averages stereo live.
const char* tx_audio_output(int v) {
    static const char* t[] = {"Auto", "Stereo", "5.1", "Mono"};
    return (unsigned)v < 4 ? t[v] : t[0];
}
void refresh_outputs() {
    output_devices.clear();
    if (can_outputs()) {
        const int count = std::max(0, host->audio_output_count());
        for (int i = 0; i < count && i < 0x7ffffffd; i++) {
            char id[HOS_OUTPUT_ID_CAP], name[HOS_OUTPUT_NAME_CAP];
            if (host->audio_output_info(i, id, sizeof id, name, sizeof name)) output_devices.push_back({id, name});
        }
    }
    output_pick = 0;
    for (size_t i = 0; i < output_devices.size(); i++)
        if (output_devices[i].id == output_want) output_pick = (int)i + 1, output_saved_name = output_devices[i].name;
    output_missing = !output_want.empty() && !output_pick;
    if (output_missing) output_pick = (int)output_devices.size() + 1;
    menu_row_top(&output_pick, (int)output_devices.size() + (output_missing ? 1 : 0));
}
void output_chosen() {
    if (output_forced() || !can_outputs()) return;
    if (output_pick <= 0) output_want.clear(), output_saved_name.clear();
    else if ((size_t)(output_pick - 1) < output_devices.size()) {
        output_want = output_devices[output_pick - 1].id;
        output_saved_name = output_devices[output_pick - 1].name;
    }
    host->audio_output_select(output_want.c_str());
    refresh_outputs();
}
// (longshot) the row's choice to the host: off, the default, or a microphone by its name (and its place in the host's list,
// -1 while it is unplugged: of two with one name, the one chosen). Unplugged, the name is the kept one, cut: the host takes
// a name that starts with it
void mic_tell_host() {
    if (!can_mic()) return;
    if (opts.mic <= 1) host->mic_select(opts.mic <= 0 ? 0 : 1, "", -1);
    else if (mic_missing && opts.mic == 2 + mic_listed) host->mic_select(2, mic_want.c_str(), -1);   // (the kept name, cut)
    else host->mic_select(2, host->mic_name(opts.mic - 2), opts.mic - 2);   // (its whole name: the ini's cut is only the ini's)
}
void mic_row_ends() {
    menu_row_top(&opts.mic, 1 + mic_listed + (mic_missing ? 1 : 0));
}
// (longshot) the microphones listed again (one plugged in since shows up), the row on mic_want's: the one at the row's place
// when it still has that name (two microphones of one name: the second stays chosen), else the first of that name, else
// its "(not connected)" entry while it is unplugged
void refresh_mics() {
    if (!can_mic()) {
        if (opts.mic >= 2) opts.mic = 1;
        return;
    }
    mic_listed = std::min(host->mic_count(), kMaxMics);
    int at = -1;
    if (!mic_want.empty()) {
        if (opts.mic >= 2 && opts.mic - 2 < mic_listed && mic_key(host->mic_name(opts.mic - 2)) == mic_want) at = opts.mic - 2;
        for (int i = 0; i < mic_listed && at < 0; i++)
            if (mic_key(host->mic_name(i)) == mic_want) at = i;
        opts.mic = at >= 0 ? at + 2 : 2 + mic_listed;
    } else if (opts.mic >= 2) {
        opts.mic = 1;
    }
    mic_missing = !mic_want.empty() && at < 0;
    mic_row_ends();
    mic_tell_host();
}
// (the row changed by the player: the choice is now this one, by its name; the "(not connected)" entry is kept only while
// it is the one chosen)
void mic_chosen() {
    if (opts.mic >= 2 && !(mic_missing && opts.mic == 2 + mic_listed)) {
        mic_want = mic_key(host->mic_name(opts.mic - 2)), mic_missing = false;
    } else if (opts.mic < 2) {
        mic_want.clear(), mic_missing = false;
    }
    mic_row_ends();
    mic_tell_host();
}
bool mic_testing = false; // session only, never saved or restored
int mic_levels[2] = {}, mic_test_state = 0;

/* Called at tick exit (vk_menu.cpp menu_tick), after every navigation/close path. Only an explicit button enables capture:
   the test stops off the Audio tab (menu_on_tab: the menu closed, another tab or a page), without the focus or with the
   Microphone Off. */
void mic_test_tick(bool focused) {
    if (!focused || !menu_on_tab("audio") || opts.mic == 0) mic_testing = false;
    mic_levels[0] = mic_levels[1] = 0;
    mic_test_state = host && host->mic_test ? host->mic_test(mic_testing, opts.microphone_gain, mic_levels) : 0;
    if (!(mic_test_state & 1)) mic_testing = false;
}
#endif

/// (Speaker layout's value as a restart compares it: Mono and Stereo share a channel layout and change live)
int speaker_restart_value(int v) { return v == 3 ? 1 : v; }

namespace {
MODULE_HINTS(hn_audio_output, 0, "5.1 when the selected/default output has 5.1 or more speakers at startup, else stereo.",
             "Stereo, whatever the device has.", "5.1, whatever the device has. Mixed down on stereo speakers.",
             "The stereo average in both ears. Changes live on stereo output; restart from 5.1.")
#ifndef MENU_LAYOUT_ONLY
/// Test microphone's button: what a press does now
const char* tx_mic_test(int) {
    return !host || !host->mic_test ? "Unavailable" : opts.mic == 0 ? "Microphone Off" : mic_testing ? "Stop test" : "Start test";
}
/// (a press starts or stops the test; with the Microphone Off, or no test on this host, nothing)
void mic_test_press() {
    if (opts.mic != 0 && host && host->mic_test) mic_testing = !mic_testing;
}
#endif
} // namespace

MENU_MODULE(audio) {
    menu::tab("audio")
        .row({.key = "output_device",
              .label = "Output device",
              .help = "Where the game plays sound. Changes apply without restarting the game.",
              .value = &output_pick,
              .lo = 0, .hi = 0,
              .preset_exempt = true,
              .text = MENU_HOOK([](int v) { return std::string(tx_output_device(v)); }),
              .hint = MENU_HOOK(hn_output_device),
              .on_change = MENU_HOOK([](int) { output_chosen(); })})
        .row({.key = "audio_output",
              .label = "Speaker layout",
              .help = "The speakers the game mixes for. Mono/Stereo changes live on stereo output; channel layout changes need a "
                      "restart.",
              .value = &opts.audio_output,
              .lo = 0, .hi = 3,
              .restart = true,
              .preset_exempt = true,
              .text = MENU_HOOK([](int v) { return std::string(tx_audio_output(v)); }),
              .hint = hn_audio_output,
              .restart_value = speaker_restart_value})
        .row({.key = "microphone",
              .label = "Microphone",
              .help = "What the ocarina hears when the game's microphone option is on (playing an instrument, or humming).",
              .value = &opts.mic,
              .lo = 0, .hi = 2 + kMaxMics,
              .preset_exempt = true,
              .text = MENU_HOOK([](int v) { return std::string(tx_mic(v)); }),
              .hint = MENU_HOOK(hn_mic),
              .on_change = MENU_HOOK([](int) { if (can_mic()) mic_chosen(); })})
        .row({.key = "microphone_gain",
              .label = "Microphone gain",
              .help = "Adjust capture for Longshot only; system volume is unchanged. Use the microphone test to see live levels "
                      "while adjusting gain. Changes apply to new samples.",
              .kind = menu::Kind::Slider,
              .value = &opts.microphone_gain,
              .lo = 0, .hi = 400, .step = 10,
              .preset_exempt = true,
              .text = menu_text::percent})
        .row({.key = "test_microphone",
              .label = "Test microphone",
              .help = "Start or stop microphone capture. No playback or audio files. You can adjust Microphone gain while "
                      "testing. Leaving Audio, closing the menu or selecting Off stops the test; the game can still use the mic.",
              .kind = menu::Kind::Action,
              .text = MENU_HOOK([](int v) { return std::string(tx_mic_test(v)); }),
              .on_activate = MENU_HOOK(mic_test_press)});
}

} // namespace mkwvk
