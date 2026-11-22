# NFC tags

The game reads figures' NFC tags through `nn::nfp`. Longshot implements it over tag dumps of the player's own, kept as
files, and, on Windows, over the reader of a Switch 2 Pro Controller plugged in by USB. All of it is in
[`host/nn_nfp.c`](../../../host/nn_nfp.c); the Insert menu's NFC tab (`gpu/vk/menu/nfc.cpp`) is its front.

**No tag dump is ever committed, uploaded or attached to an issue.** The checks build pretend tags in code.

## The tag folder

| Platform | Folder |
|---|---|
| Windows | `%LOCALAPPDATA%\Longshot\nfc` |
| Linux | `$XDG_DATA_HOME/longshot/nfc` (`~/.local/share/longshot/nfc`) |
| Any | `LONGSHOT_NFC_DIR=<folder>` instead |

Every build and the Longshot Launcher share it. One file per tag, directly in the folder. The file name without its
extension is the tag's name in the menu. Names are listed in order, ignoring case, and only the first 256 are kept.
Files starting with `key_` are skipped.

| File | Contents |
|---|---|
| `.bin` | The tag's bytes: 540 (an NTAG215's 135 pages), or 532 or 572 as some tools write them |
| `.nfc` | A Flipper Zero NFC file: `Page N: xx xx xx xx` lines, at least 130 pages |

A file is a figure's tag when its capability container says NTAG215 (`F1 10 FF EE` at `0x0C`) and its model ends in
`02` (byte `0x5B`). Only the plain parts are read: the tag's ID (bytes 0-2 and 4-7) and the model (`0x54`-`0x5B`). The
rest of a real tag is encrypted and not needed. A tag is never written back: the game's application area is kept in
`user/nfc/<tag ID>.area` in the game folder, and every tag shows as registered.

## In the game

The game calls `StartDetection`, polls `GetDeviceState` until a tag is found, mounts and reads it, then stops. In the
NFC tab, **NFC tag** picks a file, **Tap tag** holds it to the reader until the game has read it (so it can be tapped
ahead of time), and **Remove tag** takes it away.

## Switch 2 Pro Controller reader (Windows, USB)

With the controller connected through `host/pad_switch2.c`, while the game waits and no file is held to the reader, a
real tag on the controller is read and, if it is a figure's, given to the game. **Scan tag on controller** saves one to
the folder as `Scanned <ID>.bin`. A tag is read with the figure tags' password first, then without it if refused. The
reader's commands and their answers are documented in a comment in `host/nn_nfp.c`. Nothing is ever written to a tag.

## Switches

| Switch | Effect |
|---|---|
| `LONGSHOT_NFC_DIR` | The tag folder (above) |
| `HOS_NFP_SEARCH=1` | The reader looks for a tag from the start, without the game's NFC screen |
| `HOS_NFP_TRACE=1` | Logs the application-area calls and their results |

`tests/test_nfc_list_cap.py` checks the 256-file list.
