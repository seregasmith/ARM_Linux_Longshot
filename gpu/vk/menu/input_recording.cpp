// The Insert menu's input recordings (issue 1113, host/input_movie.c), the Debug tab's Input recording section after the
// Developer one (the tab draws its rows itself: vk_menu.cpp debug_rows, the tools' group, the Developer's, then this): Record
// input starts a recording of everything the player's input does to the game (the controller, the gyroscope, the mouse
// camera, the Debug tools' actions and rows) from Link's next update, and stops it; Play recording opens the Play recording
// page: Stop while one plays, the last recording, then the recordings folder's, newest first (an empty state, which records,
// when there is none). A recording plays from where it started: Link is taken to its age and map first (the Debug tab's age switch and
// teleport) and put at its start, with Drift correction's choice (HOS_INPUT_SYNC's, the switch's for a playback from the
// switches). Recordings go to the player's folder, recordings/. The rows work with the Debug tools on or off. Fenced from a
// layout-only build (MENU_LAYOUT_ONLY, MENU_HOOK).
#include "menu_module.h"

#include <functional>
#include <string>

namespace mkwvk {

namespace {
constexpr int kListed = 16;   // (the recordings the page lists, newest first)

const char* tx_input_sync(int v) {
    static const char* const names[] = {"Off", "Every update", "Every second", "Every 5 s"};
    return names[v >= 0 && v < 4 ? v : 1];
}
MODULE_HINTS(hn_input_sync, 0,
             "Off: a replay puts nothing back, so it shows exactly where the game first goes its own way.",
             "Link and the camera are put back on the recording at every update while Link is within 1 m of it.",
             "Link and the camera are put back on the recording once a second while Link is within 1 m of it.",
             "Link and the camera are put back on the recording every 5 seconds while Link is within 1 m of it.")

#ifndef MENU_LAYOUT_ONLY
std::string clock_text(unsigned frames) {
    char b[32];
    snprintf(b, sizeof b, "%u:%02u", frames / 3600, frames / 60 % 60);
    return b;
}
/// A recording's file as the page names it: "21 Nov 13:13 · HyruleField Child" from 20261121-131333-HyruleField_Child.jsonl
std::string recording_text(const char* path) {
    if (!path || !*path) return "";
    const char* name = path;
    for (const char* p = path; *p; p++)
        if (*p == '/' || *p == '\\') name = p + 1;
    static const char* const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int y, mo, d, h, mi, s;
    char where[128] = "";
    if (sscanf(name, "%4d%2d%2d-%2d%2d%2d-%127[^.]", &y, &mo, &d, &h, &mi, &s, where) >= 6 && mo >= 1 && mo <= 12) {
        for (char* p = where; *p; p++)
            if (*p == '_') *p = ' ';
        char b[192];
        snprintf(b, sizeof b, "%d %s %02d:%02d%s%s", d, months[mo - 1], h, mi, where[0] ? " \xc2\xb7 " : "", where);
        return b;
    }
    std::string t = name;   // (a file named otherwise: its name, without .jsonl)
    if (t.size() > 6 && t.compare(t.size() - 6, 6, ".jsonl") == 0) t.resize(t.size() - 6);
    return t;
}
const char* last_path();
/// Record input's value cell: what A or Enter does, or the recording's length so far
std::string record_cell() {
    const MenuInputStatus s = menu_input_status();
    if (s.mode == 1) return s.started ? "Recording " + clock_text(s.frame) : "Starting...";
    if (s.mode == 2) return "Playing";
    return "Start";
}
/// Under its help: the file
const char* hn_record(int, int variant) {
    if (variant > 0) return nullptr;
    static std::string text;
    if (variant == 0) return "Recording to user/recordings/20261231-235959-a_map_name_of_some_length.jsonl";
    const MenuInputStatus s = menu_input_status();
    if (s.mode != 1 || !s.path[0]) return s.refused;
    text = std::string("Recording to ") + s.path;
    return text.c_str();
}
/// Play recording's cell on the Debug tab: what plays, or how many there are
std::string play_cell() {
    const MenuInputStatus s = menu_input_status();
    if (s.mode == 2) return s.started ? "Playing " + clock_text(s.frame) : "Starting...";
    if (s.mode == 1) return "Recording";
    int n = 0;
    while (n <= kListed && menu_input_recording(n)) n++;   // (one past the page's: "16+" only when there are more)
    if (!n) return last_path()[0] ? "1 recording" : "None yet";
    return (n > kListed ? std::to_string(kListed) + "+" : std::to_string(n)) + (n == 1 ? " recording" : " recordings");
}
/// The page's Stop row (shown while one plays): where it is and how it goes
std::string stop_cell() {
    const MenuInputStatus s = menu_input_status();
    if (!s.started) return "Starting...";
    if (s.held) return "Waiting for the load";
    return (s.diverged ? "Diverged " : "") + clock_text(s.frame) + " / " + clock_text(s.end);
}
const char* hn_stop(int, int variant) {
    if (variant > 0) return nullptr;
    static std::string text;
    if (variant == 0) return "Diverged: nothing put back since. 123 corrections, the largest 0.123 m";
    const MenuInputStatus s = menu_input_status();
    if (s.mode != 2) return nullptr;
    char b[160];
    snprintf(b, sizeof b, "%s%u corrections, the largest %.3f m", s.diverged ? "Diverged: nothing put back since. " : "",
             s.syncs, (double)s.largest);
    text = b;
    return text.c_str();
}
int pressed = -2;   // (the recording row pressed last: -1 Last recording, else its index; -2 none)
/// A recording row's line: why the last press of it did not play it (or why the host gave it up), if so
std::function<const char*(int, int)> hn_refused(int row) {
    return [row](int, int variant) -> const char* {
        if (variant == 0) return "That recording could not be played (made from the game's start, or not a recording).";
        return variant < 0 && pressed == row ? menu_input_status().refused : nullptr;
    };
}
bool playing() { return menu_input_status().mode == 2; }
/// The last recording: made this run, or the folder's newest ("" none), as the host plays it
const char* last_path() {
    static MenuInputStatus s;
    s = menu_input_status();
    return s.mode == 0 ? s.path : "";
}
bool listed(int i) { return menu_input_recording(i) != nullptr; }
/// A recording row pressed: played, or while one plays, that one stopped (whichever row the selection is on)
void play_or_stop(int index) {
    if (playing()) {
        menu_input_play(0);
        return;
    }
    pressed = index;
    if (index < 0) menu_input_play(1);
    else menu_input_play_file(index);
}
void record_press() { menu_input_record(menu_input_status().mode != 1); }
#endif

// (the page's rows for the folder's recordings: a key each, the same label, the file in the value cell)
const char* const kRecordingKeys[kListed] = {"recording_1",  "recording_2",  "recording_3",  "recording_4",
                                             "recording_5",  "recording_6",  "recording_7",  "recording_8",
                                             "recording_9",  "recording_10", "recording_11", "recording_12",
                                             "recording_13", "recording_14", "recording_15", "recording_16"};
} // namespace

MENU_MODULE(input_recording) {
    menu::tab("debug")
        .heading("input_recording", "Input recording", menu::after("developer"))   // (a section of its own, after Developer)
        .row({.key = "record_input",
              .label = "Record input",
              .help = "Records everything your input does to the game from Link's next update until you stop it: the "
                      "controller, motion, the mouse camera and the Debug tools' teleports and switches. For tests and bug "
                      "reports: Play recording gives it back. The file goes in your folder, recordings.",
              .kind = menu::Kind::Action,
              .text = MENU_HOOK([](int) { return record_cell(); }),
              .hint = MENU_HOOK(hn_record),
              .on_activate = MENU_HOOK(record_press),
              .verb = "Record"})
        .row({.key = "play_recording",
              .label = "Play recording",
              .help = "The last recording and the ones in your folder, to play back from where they started: Link is taken "
                      "to that map and age and put there first. Your controller does nothing until it ends or you stop it.",
              .kind = menu::Kind::Page,
              .opens = "recordings",
              .text = MENU_HOOK([](int) { return play_cell(); })})
        .row({.key = "drift_correction",
              .label = "Drift correction",
              .help = "How often a replay puts Link and the camera back on the recording while they are close to it. The "
                      "game does not always run the same twice; Off shows where it first goes its own way.",
              .value = &opts.input_replay_sync,
              .lo = 0, .hi = 3,
              .preset_exempt = true,
              .text = [](int v) { return std::string(tx_input_sync(v)); },
              .hint = hn_input_sync});
    auto page = menu::page("recordings");
    page.row({.key = "stop_playback",
              .label = "Stop playback",
              .help = "Stops the recording playing now: the game is yours again from the next frame.",
              .kind = menu::Kind::Action,
              .shown = MENU_HOOK(playing),
              .text = MENU_HOOK([](int) { return stop_cell(); }),
              .hint = MENU_HOOK(hn_stop),
              .on_activate = MENU_HOOK([] { menu_input_play(0); }),
              .verb = "Stop"}, menu::order(5))
        .row({.key = "last_recording",
              .label = "Last recording",
              .help = "The recording made last (this run's, or after a restart your folder's newest), from where it started.",
              .kind = menu::Kind::Action,
              .shown = MENU_HOOK([] { return playing() || last_path()[0]; }),
              .text = MENU_HOOK([](int) { return recording_text(playing() ? menu_input_status().path : last_path()); }),
              .hint = MENU_HOOK(hn_refused(-1)),
              .on_activate = MENU_HOOK([] { play_or_stop(-1); }),
              .verb = "Play"}, menu::order(10))
        .row({.key = "no_recordings",
              .label = "No recordings yet",
              .help = "Record input, on the Debug tab, records what you do from Link's next update until you press it again "
                      "(here too: A or Enter starts one). The recordings are listed here, newest first, and kept in your "
                      "folder, recordings.",
              .kind = menu::Kind::Action,
              .shown = MENU_HOOK([] { return !playing() && !last_path()[0] && !menu_input_recording(0); }),
              .text = MENU_HOOK([](int) { return std::string(menu_input_status().mode == 1 ? "Recording" : "Record one"); }),
              .on_activate = MENU_HOOK([] { menu_input_record(menu_input_status().mode != 1); }),
              .verb = "Record"}, menu::order(11));
    for (int i = 0; i < kListed; i++)
        page.row({.key = kRecordingKeys[i],
                  .label = "Recording",
                  .help = "A recording in your folder, recordings, by when it was made and the map it starts in: Link is taken "
                          "there and put where it started first.",
                  .kind = menu::Kind::Action,
                  .shown = MENU_HOOK([i] { return listed(i); }),
                  .text = MENU_HOOK([i](int) { return recording_text(menu_input_recording(i)); }),
                  .hint = MENU_HOOK(hn_refused(i)),
                  .on_activate = MENU_HOOK([i] { play_or_stop(i); }),
                  .verb = "Play"}, menu::order(20 + i));
}

} // namespace mkwvk
