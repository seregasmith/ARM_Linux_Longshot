// The Insert menu's NFC tab (#1633): the NFC tags in the player's folder (host/nn_nfp.c: the Longshot Launcher's NFC page puts
// them there), one chosen and held to the game's reader or taken away, and a tag scanned with a Switch 2 Pro Controller's
// reader into the folder. The list is the host's, asked again each time the menu opens (refresh_nfc, by vk_menu.cpp:
// menu/nfc.h); the choice is kept in the options file as any row's, by its place in the list, which refresh_nfc keeps
// within the list. The parts that ask the host are fenced from a layout-only build (MENU_LAYOUT_ONLY, MENU_HOOK).
#include "menu_module.h"

#include "menu/nfc.h"

#ifndef MENU_LAYOUT_ONLY
#include "vk_internal.h"   // (host: the tags, the reader, the scan)
#endif

#include <algorithm>
#include <cstdio>
#include <string>

namespace mkwvk {

int nfc_choice = 0;   // (the NFC tag row: the chosen tag's place in the host's list)

#ifndef MENU_LAYOUT_ONLY
// (longshot) the NFC tab: the NFC tags in the player's folder (host/nn_nfp.c: the Longshot Launcher's NFC page puts them there),
// one chosen and held to the game's reader. The list is the host's, asked again each time the menu opens (refresh_nfc)
constexpr int kMaxNfc = 256;
int nfc_listed = 0;
std::string nfc_said;   // (what the last tap did, under the row's help)
bool can_nfc() { return host && host->nfc_count && host->nfc_name && host->nfc_tap && host->nfc_state; }
const char* tx_nfc(int v) {
    static char b[160];
    if (!can_nfc() || !nfc_listed) return "None in your NFC folder";
    snprintf(b, sizeof b, "%s", host->nfc_name(std::clamp(v, 0, nfc_listed - 1)));
    return b;
}
/* The NFC tag row: what the game's reader is doing now (variant 0: the longest, for the help's height) */
const char* hn_nfc(int, int variant) {
    static const char* const waiting = "The game is waiting for an NFC tag: Tap tag holds the chosen one to it.";
    if (variant > 0) return nullptr;
    if (variant == 0 || !can_nfc()) return waiting;
    const int st = host->nfc_state();
    static int said_for = -1;   // (what a tap said stays until another tag is chosen)
    if (said_for != nfc_choice) said_for = nfc_choice, nfc_said.clear();
    if (!nfc_said.empty()) return nfc_said.c_str();
    if (!nfc_listed) return "Add tag files on the Longshot Launcher's NFC page, or put them in the folder yourself.";
    if (st & 8) return "The game has read the NFC tag.";
    if (st & 2) return waiting;
    if (st & 4) return "An NFC tag is at the reader: the game reads it when it next asks for one.";
    if (st & 1) return "The game is not asking for an NFC tag now. Open its NFC screen first, or tap one ahead.";
    return "The game has not started its NFC reader yet.";
}
/* The Scan row: what the scan is doing, or did (variant 0: the longest; 1: the one now, when the scan has said something; no
   more: the menu asks for the next line until nullptr, measure_help) */
const char* nfc_scan_says(int variant) {
    static char b[200];
    static const char* const looking = "Hold the tag to the controller: on the logo in the middle of a Switch 2 Pro Controller, for up to ten seconds.";
    if (variant > 1) return nullptr;
    if (variant == 0 || !host || !host->nfc_scan) return looking;
    switch (host->nfc_scan(0)) {
    case 1: return looking;
    case 2:
        snprintf(b, sizeof b, "Saved as \"%s\": it is in the NFC tag row's list now. The launcher's NFC page renames it.",
                 host->nfc_scan_name ? host->nfc_scan_name() : "");
        return b;
    case 3: return "No tag was found in ten seconds. Hold it flat on the controller's logo and scan again.";
    case 4: return "That tag was read, but it is not a figure's tag: nothing was saved.";
    case 5: return "No Switch 2 Pro Controller over USB: plug it in, with Steam and any bridge program closed.";
    case 6: return "The tag could not be read, or its file could not be written. Scan again.";
    default: return nullptr;
    }
}
/// The NFC tags listed again (the menu opened): the row's range is theirs, the choice kept by its name
void refresh_nfc() {
    if (!can_nfc()) return;
    const std::string was = nfc_listed ? host->nfc_name(std::clamp(nfc_choice, 0, nfc_listed - 1)) : "";
    nfc_listed = std::min(host->nfc_count(), kMaxNfc);
    nfc_choice = 0;
    for (int i = 0; i < nfc_listed && !was.empty(); i++)
        if (was == host->nfc_name(i)) nfc_choice = i;
    menu_row_top(&nfc_choice, std::max(0, nfc_listed - 1));
    nfc_said.clear();
}
void nfc_press(bool tap) {
    if (!can_nfc()) return;
    if (!tap) {
        host->nfc_tap(-1);
        nfc_said = "The NFC tag is off the reader.";
    } else if (!nfc_listed) {
        nfc_said = "There is no tag to tap: add one on the Longshot Launcher's NFC page.";
    } else if (!host->nfc_tap(nfc_choice)) {
        nfc_said = std::string("\"") + host->nfc_name(nfc_choice) + "\" is not an NFC tag's dump.";
    } else {
        nfc_said = std::string("\"") + host->nfc_name(nfc_choice) + ((host->nfc_state() & 2) || (host->nfc_state() & 8)
                       ? "\" is at the reader: close the menu and the game reads it."
                       : "\" is at the reader. The game is not asking for one yet: it reads it when it does.");
    }
}
/// (the scan's state since the last ask: changed, the tab drawn again; ended with a tag saved, the list asked again)
bool nfc_scan_moved() {
    static int scan_was = 0;
    const int scan = host && host->nfc_scan ? host->nfc_scan(0) : 0;
    if (scan == scan_was) return false;
    if (scan == 2) refresh_nfc();
    scan_was = scan;
    return true;
}
#endif

namespace {
#ifndef MENU_LAYOUT_ONLY
/// (Tap tag's and Remove tag's line: what the last press did, until another tag is chosen)
const char* hn_nfc_said(int, int variant) { return variant < 0 && !nfc_said.empty() ? nfc_said.c_str() : nullptr; }
void nfc_scan_start() {
    if (host && host->nfc_scan) host->nfc_scan(1);
}
#endif
} // namespace

MENU_MODULE(nfc) {
    menu::tab("nfc")
        .row({.key = "nfc_tag",
              .label = "NFC tag",
              .help = "Which of your NFC tags to use: the files of your NFC folder, by their names. The Longshot Launcher's NFC page "
                      "adds and removes them; dumps of your own figures (.bin, or a Flipper Zero's .nfc).",
              .value = &nfc_choice,
              .lo = 0, .hi = 0,
              .preset_exempt = true,
              .text = MENU_HOOK([](int v) { return std::string(tx_nfc(v)); }),
              .hint = MENU_HOOK(hn_nfc)})
        .row({.key = "tap_tag",
              .label = "Tap tag",
              .help = "Holds the chosen NFC tag to the reader, as tapping the figure on the controller does. It stays there until "
                      "the game has read it.",
              .kind = menu::Kind::Action,
              .hint = MENU_HOOK(hn_nfc_said),
              .on_activate = MENU_HOOK([] { nfc_press(true); }),
              .verb = "Tap"})
        .row({.key = "remove_tag",
              .label = "Remove tag",
              .help = "Takes the NFC tag off the reader.",
              .kind = menu::Kind::Action,
              .hint = MENU_HOOK(hn_nfc_said),
              .on_activate = MENU_HOOK([] { nfc_press(false); }),
              .verb = "Remove"})
        .row({.key = "scan_tag",
              .label = "Scan tag on controller",
              .help = "Reads a tag you own with a Switch 2 Pro Controller's reader (over USB) and saves it in your NFC folder, so it "
                      "can be tapped from this tab from then on. The tag itself is not changed.",
              .kind = menu::Kind::Action,
              .hint = MENU_HOOK([](int, int variant) { return nfc_scan_says(variant); }),
              .on_activate = MENU_HOOK(nfc_scan_start),
              .verb = "Scan"});
}

} // namespace mkwvk
