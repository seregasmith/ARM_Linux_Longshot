// The NFC module's (gpu/vk/menu/nfc.cpp, #1633) that the rest of the menu uses (vk_menu.cpp): the tags listed again when
// the menu opens, and a scan followed each tick.
#pragma once

namespace mkwvk {
extern int nfc_choice;   // (the NFC tag row's value: saved as any row's)
void refresh_nfc();      // (the host's NFC tags listed again, the choice kept by its name)
bool nfc_scan_moved();   // (the scan's state changed since the last ask: the tab to be drawn again)
} // namespace mkwvk
