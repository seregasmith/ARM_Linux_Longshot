# Mods

Longshot loads mods that replace or add the game's files: textures, models, texts, sounds and other data. Mods that
change the game's code with patch files work too, once the game has been built again with them
([below](#mods-that-change-the-games-code)). This page covers installing mods, turning them off, their order and
checking that they loaded.

## Before you start

- **Back up your saves.** A mod can change what gets saved, and removing it later doesn't undo progress made with it.
  See [where Longshot keeps them](saves.md#where-longshot-keeps-them).
- **Stop the game.** Mods are found once, when the game starts: add, remove, rename or edit them with the game
  closed, then start it again.

## Install a mod

1. **Open the game folder.** In the launcher, select the version and choose **Open folder**. Each version has its
   own game folder, so each has its own mods.
2. **Make a `mods` folder** there, beside `bin`, if there isn't one yet.
3. **Put each mod in a folder of its own** inside `mods`, with the mod's `romfs` folder in it (and its `exefs` and
   `cheats` folders, when it has them):

   ```text
   mods/
     01-hd-textures/
       romfs/
         ...
     02-new-music/
       romfs/
         ...
   ```

   Most mods made for emulators already come in this layout: unpack them into `mods` as they are. Their deeper
   layouts work too, such as `mods/contents/<title id>/<name>/romfs`.
4. **Start the game.**

An update of the game keeps your `mods` folder. A mod made for an older version of the game files may need an update
of its own.

## Turn a mod off

Open Longshot's menu in the game (<kbd>F1</kbd>) and go to the **Mods** tab (it is there when you have mods). It
lists your mods, each On or Off: Left and Right switch the selected one. The change applies at the next start, and
closing the menu offers the restart.

With the game closed you can also rename a mod's folder so that it starts with `_` or `-`, for example
`_02-new-music`. Everything inside it is then left out, and the mod is not on the tab. Remove the `_` to bring it
back. Delete the folder to remove the mod for good.

The tab's switches and order are kept in `mods.json`, beside your settings file (`longshot_options.ini`),
so an update of the game keeps them.

## When two mods change the same file

The one lower in the Mods tab's list wins. On the tab, Enter picks the selected mod up and Up and Down move it; the
new order applies at the next start. Until you move something, the list is in the order of the mods' paths inside
`mods`, ignoring case, so numbering the folders (`01-...`, `02-...`) still sets it: here `02-new-music` wins over
`01-hd-textures` for any file they both have. Files that only one mod has are used either way, and the tab says
how many of a mod's files a lower mod takes.

## Mods that make files bigger

The game sets aside a fixed amount of memory for each of its files, in a resource size table
(`System/Resource/ResourceSizeTable.Product.111.Nin_Ounce_NVN2.rsizetable.zs`). A mod with larger files, such as HD
textures, ships that table with its entries grown. Without it the game leaves the mod's file out, or a loading screen
waits forever.

You don't have to combine tables yourself. At each start Longshot makes one table from the game's and every enabled
mod's (the larger entry wins), and raises the entry of any mod file that is bigger than its entry says, so mods that
each make files bigger load together. The button prompts' sizes are in it too, so your controller's icons keep
working. The first start after you add large mods can take a few seconds longer while their files are measured.

## Check that a mod loaded

Open `longshot_log.txt` in the game folder's `logs` folder after a start:

| The log says | It means |
|---|---|
| `mods: 2 mods from ...` | The mods found, and the folder they came from |
| `mods: the list (the order and the switches) is ...` | Which `mods.json` this start read |
| `mods: 01-hd-textures: 154 RomFS files (...)` | That mod is on, with this many files |
| `mods: ... <- ...` | The game read that file from a mod |
| `mods: none in ...` | A `mods` folder with no mod in it: check that each mod has its `romfs` folder |
| `mods: ...: off (the Mods tab)` | A mod you switched off on the tab |
| `mods: ... is off (its name starts with _)` | A folder you turned off by its name |
| `mods: resource size table: ...` | The one size table made from your mods' tables |
| `mods: ...: ExeFS ...: built into the game` | That code patch is in this build of the game |
| `mods: ...: ExeFS ...: not in the game yet: ...` | That code patch waits for a build (below) |

No `mods:` lines at all means there is no `mods` folder in this version's game folder.

## Mods that change the game's code

Some mods have an `exefs` folder with patch files (`.ips`, `.pchtxt`): a new menu page, a gameplay change, a frame
rate patch. Longshot builds the game's code ahead of time, so such a patch is not read at a start. It goes in when
the game is built:

1. Put the mod in `mods` as any other.
2. **Build the game again** into the same folder with Longshot Builder. The build reads your mods and their order,
   and builds the patches of the mods that are on into the game.
3. Start the game. The Mods tab says for each mod whether its patches are built into the game or still wait for a
   build.

Until that build, the mod's files load and its code does not, so the mod can look half there (a menu page that does
nothing). The same is true the other way round: switch such a mod off and its files stop loading at the next start,
while its code stays in the game until the next build. Build again after adding, removing or switching a mod with
patches. Mods without an `exefs` folder never need a build.

A patch made for another version of the game is left out, and the tab says so.

**Cheats** in a mod's `cheats` folder (Atmosphere's text format) are listed under the mod on the Mods tab and
switched there, at once. Cheats that change the game's code are not supported and are listed as such.

## What doesn't work

- **Code modules.** A mod whose `exefs` folder holds a `subsdk` file brings its own program code. That part is not
  loaded; the mod's other files still are.
- **Installers.** Nothing downloads or unpacks mods for you: you put them in the `mods` folder, and the Mods tab
  switches and orders what is there.

Longshot rewords one of the game's texts for a PC (a hint that names the console's built-in microphone). A mod that
has its own copy of that text file wins over the rewording.

## For mod authors

The [file mod contract](../dev/reference/mods.md) covers discovery, the order, conflicting files, the size table,
disabling and what the loader does not provide, with an example checked without the game. Also:

- **Code patches** apply when their file's name (IPS) or its `@nsobid` line (pchtxt) is the game code's build id,
  `E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`. `exefs_patches.txt` in the game folder lists what the build applied.
  Bytes a patch writes into a word the game's loader relocates are left out, and that file says how many.
- **Cheats** run as Atmosphere runs them, every frame (opcodes 0 to A and C0 to C3), from
  `mods/<name>/cheats/E0F3CCFAF50065AE.txt`. `{Master}` codes are always on.
- **The size table** Longshot makes at each start is written to `modcache/rsizetable.zs` in the game folder.
- **Elsewhere:** `HOS_MODS=<folder>` in `longshot_env.txt` names the folder that holds the mods, not one mod's
  `romfs`. `HOS_TEXTS=0` shows the game's own texts instead of the ones reworded for a PC.
