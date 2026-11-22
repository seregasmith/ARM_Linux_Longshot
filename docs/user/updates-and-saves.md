# Updates and saves

The Longshot Launcher keeps the game up to date: it rebuilds it from the game files the first build kept, so you don't
need your copy of the game again, and your saves and settings stay as they are.

## How updates work

When the launcher starts, it checks for a newer version on your channel. If there is one, press **Update**: it
downloads the new builder (only the parts that changed) and builds the game again, usually faster than the first time.
The launcher updates itself the same way. What changed in each version is on
[Longshot's releases](https://git.magicbean.dev/MagicBeanDev/Longshot/releases).

## Before you update

1. **Save and quit** the game from its own menu.
2. **Back up your saves.** Copy the save folder somewhere safe: an update keeps it, but a copy costs nothing. See
   [where Longshot keeps them](saves.md#where-longshot-keeps-them).
3. **After updating, load your slot** to check your adventure. If something is wrong, stop and put your backup back
   ([undo a copy](saves.md#undo-a-copy)).

## What an update keeps

| An update keeps | An update replaces |
|---|---|
| Your saves | The game itself (`bin`) and its files |
| Your menu choices and controls (`longshot_options.ini`) | `image.bin` and the shader list |
| `longshot_env.txt` | `language.txt`, written again with the game's language |
| Your `mods` folder, and `mods.json` (their order and switches) | `texts`, the game's texts reworded for a PC |
| | `exefs_patches.txt`, the list of the mods' code patches the build applied |

The first start after an update prepares the game's shaders again, so it's slower than the next ones.

## Channels

The launcher builds from one of two channels:

| Channel | What it is |
|---|---|
| **Stable** | The released version. Choose this to play. |
| **Nightly** | Built after every change; the newest five stay on the releases page. Untested: it may break your saves or settings, so back them up first. |

Any launcher can build from either channel. To go back to a released version, build one from the **Stable** channel.
When you report a problem, say the exact version: it's on the launcher's home page, and on the first line of
`longshot_log.txt`. [The release guide](../dev/reference/releases.md#versions) explains the numbers.

Each version also ships the files [RecompLauncher](https://recomplauncher.com) installs (`recomplauncher.json`). It only
finds releases on GitHub, so for now add a downloaded release to it by hand.

## Where the files go

The build reads your game and never changes it.

| What | Windows | Linux |
|---|---|---|
| The game | The folder you chose (`Games\Longshot` in your user folder by default) | The folder you chose (`~/Games/Longshot` by default) |
| Your saves | `%APPDATA%\Longshot\save` when the launcher starts the game; `user` in the game folder when you start it yourself ([details](saves.md#where-longshot-keeps-them)) | `~/.local/share/longshot` |
| Your settings (`longshot_options.ini`) and mods' order (`mods.json`) | `%APPDATA%\Longshot` when the launcher starts the game (a portable launcher's `LongshotLauncher-data\user`); the game folder when you start it yourself | The game folder |
| The shader caches and the logs | `cache` and `logs` in the game folder | `cache` and `logs` in the game folder |
| The launcher, each version's builder and its tools, the build files | `%LOCALAPPDATA%\LongshotLauncher` (the portable launcher's `LongshotLauncher-data` beside it) | `~/.local/share/longshot/launcher` |
| NFC tags | `%LOCALAPPDATA%\Longshot\nfc` | `~/.local/share/longshot/nfc` |

- **Caches:** `cache` (several GB, made for this version and this graphics card) can be deleted at any time; the next
  start prepares the shaders again, which takes minutes on some PCs.
- **Saves elsewhere:** start the game with `--user <folder>` ([command line](advanced.md#start-the-game-from-the-command-line)).
- **`.hosnew` files** are settings the game was writing when it stopped: the next start removes the ones it knows, and
  you can delete any other.
- **Uninstall:** uninstall the launcher (Windows: *Apps & features*), then delete its folder, the game folder and the
  shortcuts.

## What Longshot connects to

Longshot sends nothing about you or your PC unless you choose to, and reads no browser data, passwords or keys. This is
everything it does on the network or with other programs. What the reports hold, who reads them and how long they are
kept: [privacy](privacy.md).

- **Downloads from git.magicbean.dev:** the launcher reads the release lists, downloads a version's files and checks
  each against the SHA-256 in the release's signed `SHA256SUMS`. The builder's tools come from the links in `tools.lock`,
  each checked by SHA-256. With DLSS on, NVIDIA's DLSS library comes from `github.com/NVIDIA/DLSS`. The game itself
  checks for nothing when it starts.
- **Your Discord status**, through the Discord app on this PC while it runs: "Playing Longshot", where Link is and the
  time played. Turn it off with **Gameplay → Discord status**.
- **Performance statistics**, only if you tick **Send performance statistics** in the launcher (off by default): the
  game counts how its frames, hitches and movies went on each map, with Link's position rounded to 100 units, and the
  launcher sends them to the team with your GPU, driver, CPU and OS, but no paths, user name or ID. Unticking it deletes
  what hasn't been sent.
- **Crash reports**, only when you press **Send** in the crash window after a crash, or on their own if you tick **Send
  crash reports automatically** in the launcher (off by default). **Failed build reports**, only when you press **Send
  to the team** on the launcher's problem page. Both go to the team, and only the team can read them.
- **Files outside the game folder:** the launcher's folder, the settings and the saves (above), and shortcuts if you ask
  for them. **Bug report** and **Save my pipeline list** write files for you to send yourself: nothing is sent for you.

Only download Longshot from [the launcher's releases](https://git.magicbean.dev/MagicBeanDev/LongshotLauncher/releases),
[Longshot's](https://git.magicbean.dev/MagicBeanDev/Longshot/releases), or
[longshot.magicbean.dev/download](https://longshot.magicbean.dev/download), which sends you there. A copy from anywhere
else may not be ours.

## Mods

Mods go in a `mods` folder in the game folder, and an update keeps them. A mod made for an older version may need
updating too. The [mods guide](using-mods.md) covers installing them, turning them off and checking that they loaded.

## Moving saves

To bring a save from an emulator or Hookshot, export one, or copy saves between Windows builds, see
[moving your saves](saves.md).
