# Mods

Longshot loads mods in the layout emulators use, so mods made for them work as they are: RomFS files, ExeFS patches and
Atmosphere cheats. The player page is [using mods](../../user/using-mods.md). This page is the loader's contract: what
counts as a mod, which file wins, and what the loader does not do.

| Code | What it does |
|---|---|
| [`host/mods.c`](../../../host/mods.c) | Finds mods (`find_mods`), reads their order and switches from `mods.json` (`read_state`), indexes their files once at start (`index_romfs`), answers lookups (`hos_mods_romfs`), merges the size tables (`fit_sizes`), and gives the Mods tab its list (`hos_mod_info`, `hos_mod_edit`) |
| [`host/mods_list.c`](../../../host/mods_list.c) | Reads and writes `mods.json` (its shape: [`host/mods_list.h`](../../../host/mods_list.h)) |
| [`host/nn_fs.c`](../../../host/nn_fs.c) | Asks `hos_mods_romfs` for every RomFS open, and lists mod-only entries in a folder (`hos_mods_dir_extras`) |
| [`host/prompts.c`](../../../host/prompts.c) | Merges the button prompts' entries into the last mod's own size table when no merged table is served |
| [`host/cheats.c`](../../../host/cheats.c) | Runs the mods' Atmosphere cheats |
| [`nx2/exefs_patches.py`](../../../nx2/exefs_patches.py) | Reads the mods' ExeFS patches at build time; [`nx2/image.py`](../../../nx2/image.py) writes them into `main` before it is relocated and lifted |
| `gpu/vk/vk_menu.cpp` | The Insert menu's Mods tab (`draw_mods`, `mods_tick`), shown when there are mods |

## What a mod is

A folder holding a `romfs`, `exefs` or `cheats` folder. Files under `romfs` replace or add to the game's RomFS; the
game still has to understand them. There is no manifest, format check or file-type allowlist, and no host code runs
from a mod.

- **ExeFS patches** (`exefs/*.ips`, IPS32, `exefs/*.pchtxt`) are applied when the game is built, since its code is
  compiled ahead of time: `nx2/make_game.py` reads the patches of the mods that are on, the lift writes them into
  `main` before it relocates and translates it, and `exefs_patches.txt` in the game folder lists what was applied. A
  file is for the game when its build id (an IPS file's name, a pchtxt's `@nsobid`) is `main`'s. A new or changed
  patch file, or a mod switched on or off, applies at the next build; the Mods tab and the log say which files wait.
  The patches' hash is in the lift key (`OOT_EXEFS_PATCHES_SHA1`). A write outside `main`'s image or into the dynamic
  linker's tables makes the whole file unsupported; bytes into a relocated word are left out and counted in the file's
  line. Where two files write the same byte, the later one in the list wins.
- **Cheats** (`cheats/<build id>.txt`, the first 8 bytes of the code's build id in hex, any case) are listed on the Mods
  tab under their mod and switched there. Data cheats run every presented frame in a VM after Atmosphere's (opcodes 0 to
  A and C0 to C3); `{Master}` cheats are always on. A cheat that writes the game's code is listed as not supported.

## Discovery and order

- **Root.** `mods` beside the game executable, or `HOS_MODS=<folder>`. Either names the folder that holds mods, not one
  mod's `romfs`.
- **Depth.** A mod sits one to four levels below the root, e.g. `mods/<name>/romfs` or
  `mods/contents/<title id>/<name>/romfs`. The search does not go below a folder it took as a mod.
- **Off.** A folder whose name starts with `-` or `_` is skipped with everything below it (logged as off); one starting
  with `.` is skipped silently. Prefixes inside a mod's `romfs` mean nothing. The Mods tab switches a mod off too.
- **Order.** The order is `mods.json`'s: the Mods tab's, the last to supply a path wins. A mod the file does not name is
  on and goes after the named ones, by its path below the root, ignoring case. With no `mods.json` the order is the
  paths', so numbering mods (`01-...`, `02-...`) at one level still sets it.
- **The list.** `mods.json` sits beside `longshot_options.ini` (the user folder, else the game folder):
  `{ "mods": [ { "id": "local/<path under mods>", "enabled": true }, ... ], "cheats": { "<key>": true } }`. The game
  writes only what the tab changed, onto the file as it is then; keys it doesn't know and entries of mods that aren't
  there are kept. A file that isn't a list leaves every mod on for that start and is kept as `mods.json.bad` once the
  player changes something; files of 1 MB or more are refused. A game folder's `longshot_mods.txt` is read once and
  renamed `longshot_mods.txt.imported`. The build reads the same list by the same rules (`make_game.py --user
  <folder>`, `HOS_OPTIONS_FILE`, or the note the game leaves in `modcache/mods_json.txt`).
- **Lookup.** Paths are matched with `/` and lower case. Two files in one mod that differ only by case: which wins is
  not defined.
- **Links.** Inside a mod, links to folders (including a linked `romfs`) are not followed; links to files are. Finding
  the mods does not check for links: a mod folder, or a folder above it, that is a link is followed.
- **Limits.** At most 32 folder levels inside a `romfs`; paths are bounded by `MAX_PATH`.
- **Once.** Mods are indexed at start. A change, on the tab or on disk, applies at the next start.

A path no mod supplies falls back to the game's file; a folder cannot replace a file. `HOS_FS_OVERLAY` (a test switch)
is looked at after mods and wins over them. Last, a texture an incomplete dump damages (zeros where its zstd frame
starts) is read as the stand-in the builder would have put there (`host/oot_damaged_files.c`), whether a mod, the
overlay or the game's romfs supplied it.

## Resource size table

The game sets aside a fixed size per file in its `System/Resource/ResourceSizeTable.*.rsizetable.zs`
(`ResourceSizeTable.Product.111.Nin_Ounce_NVN2.rsizetable.zs` in the current version). At a start with mod files,
Longshot serves one table made from the game's own, with the button prompts' font entry raised in it, every enabled mod's
own table merged in (the larger entry wins) and the entry of every file a mod serves raised to what its size needs; a
file the game doesn't have gets an estimate unless a mod's table gives it. Two mods that each grow files load together.
The table is written to `modcache/rsizetable.zs` in the game folder. A mod file whose path is in another case than the
game's takes the game's entry. A mod's table for another game version only raises the entries of its own mod's files.

If the table can't be made, the last enabled mod's own table is served, with `host/prompts.c` merging the button
prompts' entries into it (the larger size of each), keyed by the game's own table name; a table it cannot merge is
served as it is, and the game's own button icons are used.

Without mods, `host/prompts.c` serves the table the game opens as the RomFS's own table of that name with the font's
entry raised, written to `cache/prompts/rsizetable.zs` at the start. No table is made at the build: the RomFS can hold
another version's tables beside the game's (a base dump with an update over it), or be another dump than the one the
build read, and a table that isn't the game's own makes it drop files it needs at the start (a crash reading guest
`0x1030`, #1109). When the RomFS has no table of the name the game opens, or no font entry in it, the game gets its own
table and its own icons. The prompts' copies of the textures and layouts are left out the same way when they are not the
size of the RomFS's files.

`host/texts.c` rewords one of the game's texts for PC; a mod with its own copy of that file wins.

## Log lines

`mods: <n> mods from <root>: <n> RomFS entries`, `mods: <mod>: <n> RomFS files (<n> replace the game's, <n> added)`
(with `, ExeFS patches` and `, cheats` when it has them), `mods: <path> <- <file>` (the first 200 files read from mods),
`mods: none in <root>`, `mods: ... is off`, `mods: <mod>: off (the Mods tab)`, `mods: the list (the order and the
switches) is <file>`, `mods: <mod>: ExeFS <file>: <state>`, `mods: resource size table: ...`,
`mods: <path> is a link to a folder: left out`.

## The checked example

`tests/test_mods_size_table.py` builds the files below in a temporary folder, runs the real `host/mods.c` and
`host/prompts.c` against them through a Linux file-system stand-in, and checks the results. It reads this block, so
keep its markers. The paths are invented; this is test input, not a format Longshot reads. There is no `mods.json`, so
the order is the paths'.

<!-- romfs-example -->
```json
{
  "mods/01-example/romfs/Example/message.txt": "first mod\n",
  "mods/01-example/romfs/Example/only-first.txt": "kept from the first mod\n",
  "mods/02-override/romfs/Example/message.txt": "second mod\n",
  "mods/_disabled/romfs/Example/off.txt": "not enabled\n",
  "mods/contents/demo-title/00-nested/romfs/Example/nested.txt": "nested mod\n",
  "mods/contents/demo-title/00-nested/romfs/Example/message.txt": "nested mod wins by its full path\n",
  "mods/03-code-only/exefs/ignored.txt": "not a real patch; not loaded\n"
}
```
<!-- /romfs-example -->

| Open | Result |
|---|---|
| `Example/message.txt`, `example\MESSAGE.TXT` | The nested mod's: `contents/...` sorts after `02-override` |
| `Example/only-first.txt` | The first mod's |
| `Example/off.txt`, `ignored.txt`, `Example`, `Example/missing.txt` | No replacement |

It also disables mods one by one, runs with and without `HOS_MODS`, and checks that `HOS_MODS` pointed at one mod
loads nothing. On Windows and macOS the file-system part is skipped.

## Not part of the contract

No version field, dependencies, catalog, per-mod save profiles or code modules (`exefs/subsdk*`). Removing a mod does
not undo what was saved with it, and an ExeFS patch stays in the game until it is built again.
