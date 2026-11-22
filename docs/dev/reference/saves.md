# Saves

Players move saves with the launcher: [moving your saves](../../user/saves.md), and
[updates and saves](../../user/updates-and-saves.md) for what an update keeps. This page is what a developer needs: the
files, where they live, how the save script works and how a save gets into the repository.

## Tools

| Tool | What it does |
|---|---|
| [`scripts/longshot_saves.py`](../../../scripts/longshot_saves.py) | `list`, `import`, `export` between Longshot, Hookshot and the emulators; `scrub` and `check-repo-saves` for the repository's saves. Standard library only |

## The files

Longshot, Hookshot and the emulators that run the game keep the same files, so a transfer is a byte-for-byte copy:

```
<save folder>/
  account.sav
  option.sav            the game's own settings
  slot_NN/              one per save slot
    progress.sav
    caption.sav         the Load screen's header and picture
```

The save folder is `save/<user id>/` under the user folder. The user id is `5a3c9e1f0b7d24680000000000000001` in every
Longshot and Hookshot build (`user_uid` in `host/nn_applet.c`; `host/nn_fs.c` mounts `save/<id>`).

| Where | User folder |
|---|---|
| Linux | `$XDG_DATA_HOME/longshot` (`~/.local/share/longshot`), so every build shares it (`saves_dir` in `host/hos_main.c` moves an old `user/` beside the executable there once) |
| Windows, launched by the launcher | `%APPDATA%\Longshot` (the launcher passes `--user`) |
| Windows, started directly | `<game folder>\user` |
| Any, with `--user <folder>` | `<folder>` |

## The script

```sh
python -I scripts/longshot_saves.py list
python -I scripts/longshot_saves.py import <name> [--profile ID] [--keep-settings] [--dry-run] [--force]
python -I scripts/longshot_saves.py export <name> [...same options]
python -I scripts/longshot_saves.py import --from DIR      # or: export --to DIR
```

`<name>` is `hookshot`, `lichii`, `frizti` (or `fritchii`), `orchid`, `pokedinner`, `eden`, `eglantine` or `nx2navi`.
`--longshot DIR` and `--hookshot DIR` name those sides' folders; `--backup-dir` moves the backup.

How it works:

- **Finding saves.** Each program has a list of data folders per OS (`EMULATORS`, `LONGSHOT`, `HOOKSHOT` at the top of
  the script). Under each, every layout in `LAYOUTS` is searched, and a profile found by more than one
  is taken from the first: Eden's
  `nand/user/save/account/<id>/0400486024DEC000/0`, then `nand/user/save/0000000000000000/<id>/0400486024DEC000`,
  NX2navi's `profile/save/<id>`, then `user/save/<id>` and `save/<id>`. More than one profile with saves stops the
  script until you pass `--profile`.
- **Safety.** It refuses while Longshot, Hookshot or an emulator runs, while NX2navi has a save waiting in
  `profile/save-tx`, or when the save holds a link (`--force` skips the first two; `--dry-run` also skips the running
  check). Importing from Hookshot checks the save as the launcher does: only `account.sav`, `option.sav` and `slot_NN`
  folders (each with a `progress.sav`, and no other file but `caption.sav`), no link, no empty file, none over 4 MB.
- **Backup, then swap.** The side being overwritten is copied to `<import|export>-<name>-<YYYYmmdd-HHMMSS>/` in the
  launcher's backups folder (`$XDG_DATA_HOME/longshot/save-backup` on Linux, `%LOCALAPPDATA%\Longshot\oot-save-backups`
  on Windows). The new save is copied into a folder inside the target and swapped in only when the copy worked. All the
  target's `slot_*` are replaced, and its `account.sav` and `option.sav` when the source has them (`--keep-settings`
  keeps `option.sav`).

`tests/test_longshot_saves.py` covers discovery for every program, the copies, backups and checks on made-up saves,
including the Windows rules run on Linux.

## Saves in the repository

`assets/saves/` holds a few scrubbed saves so the Test Farm and the tests start from the same spot
(`scripts/longshot_ctl.py launch <game folder> --repo-save <name>`, see [control bridge](control-bridge.md)). The
rules and layout are in [assets/saves/README.md](../../../assets/saves/README.md). To make one:

1. Start the game with a new, empty user folder, play to the spot, save and quit.
2. `python -I scripts/longshot_saves.py scrub <save folder> --slot NN --out assets/saves/<name>` (`<name>`: a-z, 0-9,
   `-`). It writes `slot_00/progress.sav`, `slot_00/caption.sav` and a `README.md` to fill in, prints every change, and
   writes nothing if a step fails.
3. `python -I scripts/longshot_saves.py check-repo-saves` must pass.

What `scrub` changes:

- **The picture.** `caption.sav`'s JPEG becomes a flat mid-grey JPEG of the same size, with its length updated.
- **The date.** Six 8-byte entries (a 32-bit key, a 32-bit value, little-endian) in `caption.sav`'s header hold the
  date: year `0x9811a3f7`, month `0xdfd840d3`, day `0xbd46f485`, hour `0x23f3d75e`, minute `0x27853bf7`, second
  `0x5ee50bed`. Each must appear once before offset 1392, all six on one 8-byte grid; all are set to 2026-01-01 00:00:00. Both files' modification
  times are set to one moment.
- **Nothing else.** The saves hold no player name (`--name-field FILE:OFFSET:LENGTH[:ascii|utf16le]` is there for one
  found later; `--name NAME`, default `FARM`, does nothing until then). `progress.sav` is copied as it is. `account.sav` and `option.sav` are never shipped: the game makes
  them when it loads a lone slot.

`caption.sav`'s layout (from `caption_json` in `host/control.c`): magic `04 03 02 01`; the scene's and map's names at
684 and 748; a 32-bit 1392 at 540; the JPEG's length (32-bit) at 1392; the JPEG from 1396 to the end. `scrub` and
`check-repo-saves` refuse any other layout. `check-repo-saves` also caps `progress.sav` at 512 KB and `caption.sav` and
each `README.md` at 16 KB, and decodes every picture to check it is flat grey.
