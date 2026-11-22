// The Insert menu's Bug report (#1633), on the Gameplay tab before Restart and Quit: A or Enter saves what an issue needs in
// a new folder, bug-reports in the game folder (vk_bug_report.h: written on a thread of its own). The report is gathered
// from the whole menu (the options changed, this PC, the logs, the picture: vk_menu.cpp bug_report_save, menu_module.h);
// this is the row, how the last report went and where it is. Fenced from a layout-only build (MENU_LAYOUT_ONLY, MENU_HOOK).
#include "menu_module.h"

#ifndef MENU_LAYOUT_ONLY
#include "vk_bug_report.h"
#endif

#include <string>

namespace mkwvk {

namespace {
#ifndef MENU_LAYOUT_ONLY
/// The Bug report's value cell: what A or Enter does, or how the last report went
const char* bug_report_cell() {
    switch (bug_report::state()) {
    case bug_report::kSaving: return "Saving...";
    case bug_report::kSaved: return "Saved";
    case bug_report::kFailed: return "Not saved";
    default: return "Press to save";
    }
}
/// Under its help once saved: where the report is and what to do with it (the game opens nothing: no browser or file manager is
/// sure to be there, as on a Steam Deck in Game Mode)
std::string bug_report_saved_text(const std::string& folder) {
    return "Saved in the game folder as " + folder + ". To send it, open git.magicbean.dev/MagicBeanDev/Longshot/issues, choose New issue and "
           "Game problem, copy report.txt's answers into the form and drag the folder's files into it: on Windows from File "
           "Explorer, on Linux from a file manager (on a Steam Deck, in Desktop Mode).";
}
/// Under its help once pressed: how the report goes
const char* bug_report_hint() {
    static std::string text;
    switch (bug_report::state()) {
    case bug_report::kSaving: text = "Saving: the logs are copied and the picture taken."; break;
    case bug_report::kSaved: text = bug_report_saved_text(bug_report::shown()); break;
    case bug_report::kFailed: text = "Not saved: " + bug_report::shown() + "."; break;
    default: return nullptr;
    }
    return text.c_str();
}
/// The row's line: how the report goes now (variant -1); variant 0, the longest it can say (saved, with the longest folder
/// name it can have), for the help's height
const char* hn_bug_report(int, int variant) {
    if (variant < 0) return bug_report_hint();
    if (variant > 0) return nullptr;
    static const std::string longest = bug_report_saved_text("bug-reports/2026-12-31_23-59-59-99");
    return longest.c_str();
}
#endif
} // namespace

MENU_MODULE(bug_report) {
    menu::tab("gameplay")
        .row({.key = "bug_report",
              .label = "Bug report",
              .help = "Saves what an issue needs in a new folder, bug-reports in the game folder: report.txt with the issue form's "
                      "answers (this version, where you are, your graphics card and driver, CPU, RAM and system, the options and "
                      "switches you changed), the logs of this run and the last one, and a picture of the game. Your user name and "
                      "home folder are taken out of them. No saves or game files go in, and nothing is sent.",
              .kind = menu::Kind::Action,
              .text = MENU_HOOK([](int) { return std::string(bug_report_cell()); }),
              .hint = MENU_HOOK(hn_bug_report),
              .on_activate = MENU_HOOK(bug_report_save),
              .verb = "Save"}, menu::last());
}

} // namespace mkwvk
