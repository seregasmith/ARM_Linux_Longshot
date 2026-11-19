<h1 align="center">
  <br>
  <a>
  <img src="assets/longshot.png" alt="Longshot" width="200">
</a>

  <br>
  <b>Longshot</b>
  <br>
</h1>

<h4 align="center">A supported console game running natively on Windows & Linux, without emulation.
</h4>

<p align="center">
  <a href="https://longshot.magicbean.dev/"><b>Website</b></a> |
  <a href="https://longshot.magicbean.dev/download"><b>Download</b></a> |
  <a href="https://discord.gg/magicbean"><b>Discord</b></a> |
  <a href="#project-status">Status</a> |
  <a href="#install-and-play">Install and play</a> |
  <a href="#build-from-source">Build from source</a> |
  <a href="#how-it-works">How it works</a> |
  <a href="#want-to-help">Want to help?</a> |
  <a href="#license">License + Credits</a>
</p>

<p align="center">This website is our Git hosting, similar to GitHub.</p>

Longshot is a static recompilation of a supported console game. The game's code is translated to C and compiled into a Windows or Linux program, rebuilding the graphics on Vulkan, with the console's system software reimplemented. All of this happens **on your PC, from your own copy of the game**.

Longshot is a continuation of [Hookshot](https://github.com/NotRexed/Hookshot). The top maintainers have moved to this project.

**[Join us on Discord](https://discord.gg/magicbean)!**
<br>

> [!IMPORTANT]
> This repository **does not contain the game, keys or an executable**, and it does not explain how to get them. You
> need your own copy. Everything the build creates (`longshot.exe`, the generated code, the shader caches and your
> saves) comes from your game and stays on your PC. Never share those files.

## Project status

Experimental. Players have reported on many PCs, from Kokiri Forest to the dungeons: the table says what works and
what does not yet. Open problems are collected in tracking issues, linked below.

| What | Status |
|---|---|
| Kokiri Forest: reaching gameplay and walking around | ✅ Works |
| Sound, controller, keyboard and saves | ✅ Works |
| In-game menu, frame generation and upscaling | ✅ Works |
| Flicker (the picture flashing too bright while moving) | ✅ Fixed in v2.0.2 ([#645](https://github.com/NotRexed/Hookshot/pull/645)) |
| Ground fog vanishing when looking down | ✅ Fixed in v2.0.2 ([#638](https://github.com/NotRexed/Hookshot/pull/638)) |
| Models with missing pieces (doors, fences, parts of trees) | ✅ Fixed where tested, more play needed |
| Stutter when entering new areas | ⚠️ Still happens the first time you visit a place. The next visits are smooth |
| Performance | ⚠️ Depends on the PC: some reach a steady 60 fps, others stay below it, more so in Hyrule Field |
| Hyrule Field, Castle Town, Kakariko and the dungeons | ⚠️ Players got there, with open bugs: lighting and shadows ([#1004](https://github.com/NotRexed/Hookshot/issues/1004)), crashes and freezes ([#2507](https://github.com/NotRexed/Hookshot/issues/2507)) |
| Ultrawide and 4:3 | ✅ Works. The Aspect ratio row's default, Monitor's shape, widens the 3D to the screen ([#2119](https://github.com/NotRexed/Hookshot/pull/2119)); the HUD keeps its 16:9 size with its corners at the screen's edges, or in the middle 16:9 with the HUD layout row; movies stay in the middle 16:9 |
| NVIDIA cards | ✅ Works. The most tested |
| AMD cards | ⚠️ Tested on several Radeon cards. Compatibility and performance vary by card, driver and operating system. Longshot automatically uses rendering workarounds on affected Windows cards and AMD GPUs on Linux (see [technical details](docs/development.md#switches-for-testing-and-debugging)). Crashes and severe slowdowns remain reported on some configurations ([RX 6600](https://git.magicbean.dev/MagicBeanDev/Longshot/issues/933), [Radeon 860M](https://git.magicbean.dev/MagicBeanDev/Longshot/issues/1040)). |
| Intel cards | ❓ Few reports, many of them black screens or crashes ([#999](https://github.com/NotRexed/Hookshot/issues/999)). On Windows, Intel's shader compiler is entered by one thread, and a shader it faults on is left out instead of closing the game, on later starts too ([#252](https://git.magicbean.dev/MagicBeanDev/Longshot/pulls/252)). Made for an Intel UHD laptop with 4 threads and 16 GB that crashed in `igvk64.dll` (see [Intel laptops](#intel-laptops)): not yet tested on an Intel GPU |
| Pre-rendered movies on a slow CPU | ⚠️ Shown rather than skipped when the decoder is slower than the movie, so the picture can fall behind the sound. With 4 logical processors or fewer the decoder uses 2 threads, leaving the game its own. The first picture can still be black on some PCs |
| Cards without mesh shaders (such as NVIDIA GTX 10 and AMD RX 5000) | ⚠️ Run through the compute fallback (since v2.0.6), which runs the mesh programs as compute shaders on cards with buffer device addresses: slower, and it leaves out the mesh draws that take their count from a buffer ([#640](https://github.com/NotRexed/Hookshot/issues/640)) |
| Linux | ✅ Works with the AppImage. Tested on several distributions (CachyOS, Fedora, Bazzite, SteamOS) and cards, with open problems on some AMD cards ([#1000](https://github.com/NotRexed/Hookshot/issues/1000)) |

## Install and play

### [🪟 &nbsp;Download for Windows](https://longshot.magicbean.dev/download/launcher/windows)

### [🐧 &nbsp;Download for Linux (AppImage)](https://longshot.magicbean.dev/download/launcher/linux)

Windows without installing: [portable zip](https://longshot.magicbean.dev/download/launcher/windows-portable) · Not sure?
[longshot.magicbean.dev/download](https://longshot.magicbean.dev/download) picks for your computer. These links give the
newest launcher (stable, or until there is one, the release candidate); for each channel's own, see
[release channels](#release-channels).

**Download the launcher, not the builder.** The files on [Longshot's releases](https://git.magicbean.dev/MagicBeanDev/Longshot/releases)
(`LongshotBuilder-…-windows-x64.zip`, `Longshot-…-x86_64.AppImage`, `tools.lock`) are what the launcher installs for
you. Windows may flag the launcher as a Trojan: it isn't, it's just not code-signed yet. See
[antivirus warnings](https://longshot.magicbean.dev/help/antivirus/).

Longshot doesn't come ready to play: it is built on your PC, from your own copy of the game. The
**[Longshot Launcher](https://longshot.magicbean.dev/download)** does all of it for you:

1. Check that your PC and your copy of the game match [what you need](#what-you-need).
2. Download the launcher for [Windows](#on-windows) or [Linux](#on-linux) and start it.
3. Point it at your game and press **Build**. It downloads this version's Longshot Builder, checks it against the
   release's signed checksums, and builds the game: about 15 minutes on a 12-core desktop, longer on a slower PC
   ([build times](docs/development.md#build-times)).
4. Press **Play**. From then on the launcher keeps the game, and itself, [up to date](#updating).

Longshot's first version will be **v3.0.0**; the launcher's first will be **v1.0.0** (its numbers are its own).

### What you need

<!-- website/src/content/docs/requirements.mdx copies this section: when one changes, change the other to match. -->

**Your game copy:** program `0400486024DEC000` · update **v131072** · build `e0f3ccfa`.
Use a `.nup` file, or the `romfs` and `exefs` folders. Other game versions are not supported.

**Check your copy before building:** choose your own game file or folders in the launcher. Its game-file check
reports whether the build is supported; if it reports another build, compare that message with `e0f3ccfa` above.
The launcher version (such as `v1.0.0-rc.4`) is separate from the game update and build identifiers.

These are estimates from the [tested configurations catalogue](https://longshot.magicbean.dev/tested-configurations/), not a promise of a particular
frame rate: the game runs on more than this, and performance depends on the resolution and settings you choose.

| Component | Minimum | Recommended |
|---|---|---|
| **Operating system** | **64-bit Windows 10 / 11**, or **64-bit Linux** with glibc 2.35 or newer. | Same. |
| **Processor** | A 6-core x86-64 CPU, such as an AMD Ryzen 5 2600 or Intel Core i5-8400. AVX2 is not required with current builds (v3.0.0-rc.4 or newer). | AMD Ryzen 7 7800X3D or Intel Core i5-13600K. |
| **Graphics card** | **Vulkan 1.3.** NVIDIA GeForce GTX 1060 6 GB or GTX 1650, or AMD Radeon RX 5500 XT. | **Vulkan 1.3 with mesh shaders.** NVIDIA GeForce RTX 4070 or AMD Radeon RX 7800 XT. Check the [project status](#project-status) for each maker's known issues. |
| **Mesh shaders** | Not required: without them, Longshot draws through a slower compute fallback. | **Strongly recommended:** NVIDIA RTX 20 series and newer, or AMD RX 6000 series and newer. |
| **Video memory (VRAM)** | **4 GB**, with the in-game Minimum (4GB) quality preset. | **12 GB or more.** |
| **System memory (RAM)** | **16 GB.** See the memory guidance below. | **32 GB or more.** |
| **Storage** | **About 60 GB free while building.** A hard drive works, but builds are much slower. | **An SSD with about 60 GB free.** |

**Below the minimum:** a laptop with Intel UHD graphics and a 4-thread CPU is below both minimums. The game has been
reported to start on one (Intel UHD, 4 threads, 16 GB, Windows), at about 35 fps on the title and with crashes
in Intel's driver that [#252](https://git.magicbean.dev/MagicBeanDev/Longshot/pulls/252) is meant to fix. See
[Intel laptops](#intel-laptops) for what to set and what to send.

Memory use depends on the build, driver and scene: recent Linux/NVIDIA runs used roughly
13–19 GiB for the game alone (see the [tested configurations catalogue](https://longshot.magicbean.dev/tested-configurations/)), so even a 16 GB PC can need
swap. Leave room for the desktop and other programs.
Swap can prevent an out-of-memory exit, but it is not extra RAM; heavy swapping can make the whole desktop unresponsive.

**Drivers:** use an up-to-date graphics driver with Vulkan 1.3. On Linux, use Mesa or NVIDIA's driver. Linux examples
include Ubuntu 22.04, Debian 12, Fedora, Arch and SteamOS.

**Download:** an internet connection is required. The launcher installs the builder, which brings its own compiler and tools;
there is nothing else to install.

### On Windows

1. [Download the launcher for Windows](https://longshot.magicbean.dev/download/launcher/windows)
   (`LongshotLauncher-<version>-windows-x64-setup.exe`) and run it. It asks once: **Install** (for you, no administrator
   rights, with a Start menu shortcut) or **Run portable**. To skip the question, take
   [the portable zip](https://longshot.magicbean.dev/download/launcher/windows-portable), unzip it and run
   `LongshotLauncher.exe`.
2. Press **Build the game** and choose the **Stable** channel.
3. Pick your game. The launcher finds `.nup` files in your Downloads, Desktop and Documents folders; if yours isn't
   listed, point it at the file, or at the `romfs` and `exefs` folders. It checks the version while the builder downloads.
4. Choose where the game goes (`Games\Longshot` in your user folder by default), its language and the options.
5. Press **Build**, then **Play**. The launcher can also add desktop and Start menu shortcuts.

The build prepares the game's shaders at its end, which takes a while; later starts are faster. To keep everything on an
external SSD, run [the launcher's portable zip](https://longshot.magicbean.dev/download/launcher/windows-portable) from
there: its data stays in `LongshotLauncher-data` beside it.

#### Compile shaders

**Compile shaders** (in the launcher's settings, or `make_game.py --compile-shaders`) chooses what that
preparation builds:

- **Most** (recommended, the default): every shader and the pipelines the game is known to use (the shipped list and
  what your own play noted). Compute programs on those lists have all their texture variants warmed. An unlisted
  compute program stays translated on disk and the driver builds its pipeline on first dispatch. With no compute
  programs listed, preparation warms them all.
- **All**: the predicted pipelines too, about 50,000 guesses at pipelines no list names. The preparation takes about 40%
  longer (415 s against 292 s on an RTX 3090 with a 12-core CPU), and play almost never uses them.

A preparation the game runs at a launch (after "Prepare shaders now" was unticked, or after an update or a new GPU driver
changed what the shaders are translated for) follows the same choice. A build whose preparation stopped ("the GPU
driver stopped responding") leaves `longshot_prepare_skipped` instead, and the game then prepares nothing at a launch,
building each shader when first drawn, until that file is deleted or a build prepares them. The choice is kept as
`compile_shaders` in `longshot_options.ini` (0 Most, 1 All). `--compile-shaders all` or `most` on the game's command line, or
`RX_VK_COMPILE_SHADERS=all` or `most`, sets it for one run.

### On Linux

1. [Download the launcher for Linux](https://longshot.magicbean.dev/download/launcher/linux)
   (`LongshotLauncher-<version>-x86_64.AppImage`).
2. Let it run as a program: in your file manager, open its **Properties** and turn on "Allow executing file as program"
   (the wording differs between desktops), or run `chmod +x LongshotLauncher-*-x86_64.AppImage`.
3. Start it and build the game as [on Windows](#on-windows). The game goes to `~/Games/Longshot` by default, and the
   launcher can add it to your applications menu as "Longshot".

Each version's builder is also released on its own, as `Longshot-<version>-x86_64.AppImage` (about 95 MB) on
[Longshot's releases](https://git.magicbean.dev/MagicBeanDev/Longshot/releases). It has no window: it is what the
launcher builds with, and it builds and starts the game from the command line, over SSH or from a script (see
[Linux, the AppImage from the command line](#build-from-source)). Its first `--cli` downloads its build tools (the
compiler, the C library it links against, FFmpeg and SDL2: about 170 MB) into `~/.local/share/longshot/builder/toolchains`,
and later versions reuse the ones that didn't change; the launcher downloads the same tools itself.

#### Advanced Linux setup

**Faster GPU memory on AMD and Intel (`/dev/udmabuf`).** Longshot imports GPU memory fastest through the kernel's
`udmabuf` device. Without access to it, it falls back to host-pointer imports, which can be much slower on AMD (RADV),
and warns about it on stderr and in `longshot_log.txt`. On NVIDIA's own driver you can ignore that warning: NVIDIA refuses
these imports, and host-pointer imports are not slow there (the check runs at start, before the game knows its GPU).
Inside a container that shows `/dev/udmabuf` but hides `/sys/module` (Flatpak Steam, for one), the game can't read
udmabuf's size limit; it takes the module's default, 64 MB, and says so in the log (`its default, 64 MB, assumed`).

Check with `ls -l /dev/udmabuf` and `id`. If the device is missing, load the module first (`sudo modprobe udmabuf`).
Then give the game's user access, in one of these ways:

1. **On a desktop (systemd-logind):** a rule that grants it to the active local user. Put
   `SUBSYSTEM=="misc", KERNEL=="udmabuf", TAG+="uaccess"` in `/etc/udev/rules.d/70-longshot-udmabuf.rules` (it must come
   before `73-seat-late.rules`).
2. **Over SSH or as a service:** create a dedicated `udmabuf` group, add only the game's user, and use
   `SUBSYSTEM=="misc", KERNEL=="udmabuf", GROUP="udmabuf", MODE="0660"` in that file instead.
3. **The device's owning group:** add yourself to it, only if its other devices should be yours too (`kvm`, for one,
   also grants virtualization).

Apply a rule with `sudo udevadm control --reload-rules` and `sudo udevadm trigger --action=add /sys/class/misc/udmabuf`,
and log out and back in (or restart the service) after a group change. Never run the game as root or make the device
world-writable.

**How Play starts the game.** When the desktop gives the launcher its own systemd service, **Play** waits up to two
seconds for the game's separate scope before closing its window. If that hand-off fails, it starts the game directly and
records the fallback in `longshot_startup.log` once the game exits. A direct start still ends with the desktop's
service: a desktop that stops its children stops the game too.

### Updating

The launcher checks for updates when it starts. When a newer version is out on your channel, press **Update**: it
downloads the new builder (only the parts that changed) and builds the game again from the game files the first build
kept, so you don't need your `.nup` again. It updates itself the same way. What changed is on
[Longshot's releases](https://git.magicbean.dev/MagicBeanDev/Longshot/releases).

Before updating, copy your saves somewhere safe: the `user` folder in the game folder on Windows,
`~/.local/share/longshot` on Linux.

| An update keeps | An update replaces |
|---|---|
| Your saves | The game in `bin` and its files |
| `longshot_options.ini`: the menu's choices and your controls | `image.bin` and the shader list |
| `longshot_env.txt` | `language.txt`, written again with the game's language |
| | `texts`: the texts reworded for a PC, made again |
| `mods` | |

Without the launcher: from a clone, `git pull`, then `python -I nx2/make_game.py --update <game folder>`; a builder
does the same with `--cli --update <game folder>`.

### Release channels

The launcher builds from one of three channels:

| Channel | What it is | The launcher |
|---|---|---|
| **Stable** | The released version. Choose this to play. | [Windows](https://longshot.magicbean.dev/download/launcher/stable/windows) · [portable](https://longshot.magicbean.dev/download/launcher/stable/windows-portable) · [Linux](https://longshot.magicbean.dev/download/launcher/stable/linux) |
| **Release candidate** | The next version, before it is released. | [Windows](https://longshot.magicbean.dev/download/launcher/rc/windows) · [portable](https://longshot.magicbean.dev/download/launcher/rc/windows-portable) · [Linux](https://longshot.magicbean.dev/download/launcher/rc/linux) |
| **Nightly** | Built from `main` after every change; the newest five stay on the releases page. Untested: it may break your saves or settings, so copy them first. | [Windows](https://longshot.magicbean.dev/download/launcher/nightly/windows) · [portable](https://longshot.magicbean.dev/download/launcher/nightly/windows-portable) · [Linux](https://longshot.magicbean.dev/download/launcher/nightly/linux) |

A nightly launcher builds Longshot's nightlies by default; any launcher can build from any channel. To go back, build a
version from the **Stable** channel.

### Another way: RecompLauncher

Each version also ships the files [RecompLauncher](https://recomplauncher.com) installs (`recomplauncher.json`, in
`LongshotBuilder-<version>-windows-x64.zip` and in the Linux AppImage). RecompLauncher only finds releases on GitHub, and
Longshot's are on git.magicbean.dev, so for now add a downloaded file to it by hand
([docs/releases.md](docs/releases.md#recomplauncher-longshot)).

### Where the files go

The build reads your game and never changes it.

| What | Windows | Linux |
|---|---|---|
| The game | The folder you chose (`Games\Longshot` by default) | The folder you chose (`~/Games/Longshot` by default) |
| Your saves | `user` in the game folder | `~/.local/share/longshot` |
| The launcher, each version's builder and its tools (`toolchains`), the build files (`work`) | `%LOCALAPPDATA%\LongshotLauncher` (the installed program in `%LOCALAPPDATA%\Programs\LongshotLauncher`; the portable one's data in `LongshotLauncher-data` beside it) | `~/.local/share/longshot/launcher` (the portable AppImage's in `<AppImage>.home` beside it) |
| A builder without the launcher (`--cli`) and its tools | `%LOCALAPPDATA%\LongshotBuilder` | `~/.local/share/longshot/builder` and `~/.cache/longshot-builder` |
| The in-game menu's choices and your controls | `longshot_options.ini` in `%APPDATA%\Longshot` when the launcher starts the game, the same for every version (a portable launcher's in `LongshotLauncher-data\user`); in the game folder when you start the game yourself | `longshot_options.ini` in the game folder |
| The shader and pipeline caches, and the logs | `cache` and `logs` in the game folder | `cache` and `logs` in the game folder |

- **Caches and logs:** the game keeps its shader and pipeline caches in the game folder's `cache` folder (several GB, made
  for this version and this graphics card) and its logs in `logs`. `cache` can be deleted at any time: the next start
  prepares the shaders again (minutes on some PCs). A game folder from an older build has them loose beside the game: the
  first start moves them into these folders.
- **Saves elsewhere:** start the game with `--user <folder>` ([command line arguments](#running-the-game-from-the-command-line)).
  A `user` folder from an earlier Linux build is moved to `~/.local/share/longshot` once. To carry on with a save from
  Hookshot, an emulator (Lichii, Eglantine, Orchid, ...) or NX2navi, or to take yours there, see the
  [saves guide](docs/saves.md).
- **Build files:** the translated code and compiled objects (about 4 GB) are deleted once the game is ready; none are
  needed to play. To keep them, so that an update compiles only what changed, set `LONGSHOT_KEEP_BUILD_FILES=1` for the
  build; **Free up space** in the launcher's settings deletes them later. Their folder can be moved there before a build
  (**Change**, **Default**). The romfs is hard-linked instead of copied where the disk allows it.
- **Uninstall:** uninstall the launcher (Windows: *Apps & features*), then delete its folder, the game folder and the
  shortcuts.

### What Longshot connects to

Longshot uploads nothing about you or your PC, and reads no browser data, passwords or session keys. This is
everything it does on the network or with other programs:

- **Downloads, from git.magicbean.dev.** The launcher reads the release lists of Longshot and of the launcher, downloads a
  version's files, and checks each one against the SHA-256 in the release's signed `SHA256SUMS`. The Windows builder's
  tools come from the links in `tools.lock` (their makers' releases, or Longshot's `tools` release), and its compiler
  from its makers' releases (llvm-mingw, Python), all checked by SHA-256. With **DLSS** on, NVIDIA's DLSS library comes
  from `github.com/NVIDIA/DLSS`. The game itself checks for nothing when it starts.
- **Your Discord status, through the Discord app on this PC.** While Discord runs, it shows "Playing Longshot" with the
  time played, through the Discord app's own local connection (a named pipe on Windows; the socket `discord-ipc-0` in
  `$XDG_RUNTIME_DIR`, `$TMPDIR` or `/tmp` on Linux). It sends where Link is and his age (Kakariko Village - Adult) with his hearts, Gold Skulltula
  tokens and pieces of heart below it while you play, the Longshot version until he is in a map, the start time and the icon's name, or the text and
  picture you set yourself (below). Turn it off with **Gameplay → Discord status** in the in-game menu.
- **Files outside the game folder:** the builder's folder, the settings and the saves (above), and shortcuts if you ask
  for them. **Bug report** (Gameplay tab) writes a folder in `bug-reports` in the game folder, with your home folder and
  user name taken out of its logs, and **Save my pipeline list** saves a zip in your Downloads folder: you decide whether
  to send them anywhere.

Only download Longshot from [the launcher's releases](https://git.magicbean.dev/MagicBeanDev/LongshotLauncher/releases)
and [Longshot's](https://git.magicbean.dev/MagicBeanDev/Longshot/releases) on git.magicbean.dev, or through
[longshot.magicbean.dev/download](https://longshot.magicbean.dev/download), which sends you there. Each version's files
are listed with their SHA-256 in its `SHA256SUMS`, signed by the release key, which the launcher checks. A copy from
anywhere else may not be ours.

### If something goes wrong

**Start with the log:** `longshot_log.txt` in the game folder's `logs` folder holds the last run, and `longshot_log.1.txt` to `.4.txt`
the four before it (started from a terminal, the game prints there instead). Every 600 frames it writes a `vk: status:`
line with the video memory in use against what the driver allows, and DLSS's details while it draws;
`vk: over the driver's video-memory allowance` says when the game went past it.
The logs are safe to send as they are: your home folder is written `~` (`%USERPROFILE%` on Windows) and your user name
`<user>` in their paths; the rest of each path stays, so a helper still sees where things are. The game's own lines are
written so; what other programs print into the log (the Vulkan loader, SDL) is cleaned once the game has closed, by
the window that watches for a crash, or else at the next start. The builder's `--log` file is written the same way.
A game started from a terminal prints there instead, as it is.

| Problem | What to do |
|---|---|
| **Building** | |
| "Longshot supports build e0f3ccfa" or "This version isn't supported" | Check that you chose the right game, with its update installed. |
| "Your antivirus removed part of the build tools" | A false alarm on unsigned programs. Restore the files from Windows Security's Protection history, and add `%LOCALAPPDATA%\LongshotLauncher` and `%LOCALAPPDATA%\Programs\LongshotLauncher` to its exclusions, then build again ([common problems](docs/common-issues.md#your-antivirus-removed-part-of-the-build-tools)). |
| **Starting** | |
| The game opens and closes, or the log says "device lost" | Update your graphics driver. AMD cards have known crashes ([#708](https://github.com/NotRexed/Hookshot/issues/708), [#614](https://github.com/NotRexed/Hookshot/issues/614)). |
| Windows, Intel: the game closes in the first scene, and the log names `igvk64.dll` | Set the Debug tab's **Intel shader compiles** row to **One at a time** (it is what **Auto**, the default, means on every Intel GPU but the Arc B-series) and update the graphics driver. See [Intel laptops](#intel-laptops). |
| The build said "the GPU driver stopped responding", or the log says `the shaders' preparation was skipped at the build` | The game then builds every shader when it is first drawn, and stutters or leaves things out for a long time. Delete `longshot_prepare_skipped` from the game folder's `cache` folder so that the next start prepares them, or build again with **Prepare shaders now** ticked. |
| Linux: the game doesn't start, or the picture is broken | The game needs a Vulkan 1.3 driver, ideally with mesh shaders: `vulkaninfo \| grep mesh` should list `VK_EXT_mesh_shader`. Update Mesa or the NVIDIA driver. |
| Linux: low-memory warning, or the whole desktop becomes unresponsive while loading | Close other programs and check `free -h`: the `Mem` and `Swap` rows are separate. Below 10 GiB of physical RAM the game warns even with plenty of swap; absence of that warning does not mean a run will fit. Do not keep retrying a load that locks up the PC. After restarting, keep the failed session's log before launching again, and check `journalctl -k -b -1` for an out-of-memory kill or a GPU reset (if the previous boot's journal was saved). |
| **Graphics** | |
| Parts of the picture are missing, or a message says your card has no mesh shader support | The card or its driver has no mesh shaders, so Longshot draws through its compute fallback, which leaves some draws out. Update the driver; a card with mesh shaders draws everything ([#640](https://github.com/NotRexed/Hookshot/issues/640)). |
| A place has a wrong texture, or a floor or wall is missing | Look for `longshot_damaged_files.txt` in the game folder. The build writes it when your dump is incomplete (about 23.3 GB instead of 23.8 GB), listing each damaged file and what was put in its place. Dump the game again and build from the new dump; the file is then removed. |
| AMD: blocky textures, solid rectangles in the sky or unusually dark courtyard surfaces | Try the [AMD texture workaround](#amd-texture-workaround). It uses more graphics memory. |
| Windows: HDR is on, but the game stays SDR | The **HDR** row's help in the menu, and the `vk: HDR:` line in the log, say why. "HDR is off for this display": turn on **Use HDR** in Windows' **Settings > System > Display**. "Windows has HDR on, but the graphics driver offers the game no HDR output": reinstall the graphics driver with a clean install and restart the PC ([steps](docs/common-issues.md#windows-hdr-is-on-but-the-game-stays-sdr)), then [report it](https://git.magicbean.dev/MagicBeanDev/Longshot/issues/new) if it stays. |
| **Performance** | |
| Stutter the first time you see something | Normal for now: shaders are being prepared, and it is smooth the next time you pass. You can help remove it for everyone: [send your pipeline list](#send-your-pipeline-list). |
| Windows, NVIDIA: stutter on every start, in places you have seen | The driver's shader cache is too small to keep them all (the first start builds about 2 GB of pipelines). In the NVIDIA Control Panel, under Manage 3D settings, set **Shader Cache Size** to **Unlimited** (or 10 GB). On Linux the game raises the driver's limit itself. |
| Low FPS | See [Low FPS or stutter](#low-fps-or-stutter) below. |
| A movie stutters, falls behind its sound, or starts on a black picture | On a CPU with 4 logical processors or fewer, the decoder uses 2 threads; `HOS_DAV1D_THREADS=<n>` in `longshot_env.txt` sets another count. Run once with `HOS_VIDEO_LOG=1` and attach the log: its `video:` lines say how long each picture took to decode and how far the picture was behind the sound. |
| Steam Deck: the game freezes and closes in Kokiri Forest or the Great Deku Tree (`gfx_0.0.0 timeout` in `journalctl -k`) | Add `RX_VK_MESH_NOCULL=2c56dc1006942f74` to `longshot_env.txt`: it gets past it with every model drawn, by turning off the culling pass that runs for seconds. `RX_VK_SKIP_CODE=2c56dc1006942f74` also gets past it, but the models those draws make go missing ([development guide](docs/development.md#switches-for-testing-and-debugging), [#817](https://github.com/NotRexed/Hookshot/issues/817)). |
| **Other** | |
| The game is in the wrong language | Change the code in `language.txt` in the game folder. The file lists them all, and an update keeps the one you set. |

After a crash, at the start or in play, a window says what happened. **Report manually** copies a report (no saves or
game files, your home folder and user name taken out) and opens the issue form, for you to paste it into. In a build the
Longshot Launcher made, **Report automatically** shows the crash report it would send (the version, your PC, the
settings, the crash and the log before it), and posts it to [#81](https://git.magicbean.dev/MagicBeanDev/Longshot/issues/81)
only when you press it ([crash-report-window.md](docs/crash-report-window.md)). Still stuck?
[Report it](https://git.magicbean.dev/MagicBeanDev/Longshot/issues/new) with the version, where you were in
the game, your graphics card, `longshot_log.txt` and a screenshot or video, and never game files. **Gameplay → Bug
report** in the [in-game menu](#in-game-menu) gathers all of it in one press.

#### Low FPS or stutter

1. After the shaders are prepared, walk through the same spot twice. Note whether the slowdown clears the second time,
   or lasts until you restart the game.
2. Turn off **Frame generation**, so the FPS counts the game's own frames, and keep the resolution, upscaler and frame
   rate cap the same between runs. For a slowdown after an update, compare the same spot and settings on both versions
   if you still have the earlier build.
3. Attach the log from the slow run (and from the earlier build, if you have it). Say where the FPS dropped, and keep the
   lines around the drop that show `presents`, `GPU time`, `waits for shaders or pipelines`, `VRAM:` and `memory heaps`.
   Compare the total and per-heap memory with the driver's allowances; the texture cache has its own budget on the
   `VRAM:` line.
4. If the GPU time stays high, run once more with `RX_VK_PASS_TIMING=1` in `longshot_env.txt`: every 600 presents it
   reports which GPU passes take the time. That timing adds work itself, so keep the first log for the FPS comparison,
   and remove the switch afterwards. The [development guide](docs/development.md#time-and-profiles) has more switches.
5. To find out why, add `HOS_PROFILE=1` to `longshot_env.txt`: the game records where its time goes and what each stutter
   was into a folder in `logs` ([report slow performance](docs/performance-report.md#1-turn-on-the-profiling-mode)).

#### Intel laptops

What [#252](https://git.magicbean.dev/MagicBeanDev/Longshot/pulls/252) changes was made from the logs of a laptop with
Intel UHD Graphics, a 4-thread CPU, 16 GB and Windows. There the game closed in Intel's driver (`igvk64.dll`) in the
background save of the pipeline cache while other threads built pipelines into the same cache; main now fetches a
cache's data with no build under way on Intel ([#190](https://git.magicbean.dev/MagicBeanDev/Longshot/pulls/190)).
The build's shader preparation had stopped, so every run built its shaders in play, and the title ran at about 35 fps.
It has not been tested on an Intel GPU yet. On such a PC:

1. Build with **Prepare shaders now** ticked. If the game folder's `cache` folder has a `longshot_prepare_skipped` file
   from an earlier build, delete it first.
2. Keep the Debug tab's **Intel shader compiles** row on **Auto** (the default: one at a time on these laptops) or **One
   at a time**. Side by side builds faster on a CPU with many cores, but Intel's driver can crash the game then.
3. A shader the driver faulted on is listed in `longshot_driver_faults.txt` (in the `cache` folder) and left out, with
   its draws, on later starts on the same driver. After a driver update the list starts again; delete the file to try
   them all again sooner.
4. Leave **ASTC as RGBA8** off: it is the [AMD texture workaround](#amd-texture-workaround), and its larger textures
   come out of the memory that integrated graphics share with the rest of the PC.
5. If it still crashes or runs slowly, [report it](https://git.magicbean.dev/MagicBeanDev/Longshot/issues/new) with
   `longshot_log.txt` from a run with `HOS_VIDEO_LOG=1` in `longshot_env.txt`, the build's details, and the FPS on the
   title and in the first scene.

#### AMD texture workaround

Some AMD texture artifacts go away when decoded ASTC textures are kept as RGBA8 instead of being re-encoded to BC3
([#57](https://github.com/NotRexed/Hookshot/issues/57), [#1006](https://github.com/NotRexed/Hookshot/issues/1006)):
turn on **ASTC as RGBA8** in the menu's Debug tab, under Developer, and restart the game. It is a workaround for these
artifacts, not a lighting fix, and it can use much more graphics memory, causing stalls when the card runs out. To undo
it, turn the row off and restart.

`RX_VK_ASTC_RGBA8=1` in `longshot_env.txt` does the same, as earlier versions of this readme said; while that line is
there, the menu row shows its value and can't be changed, so remove the line to choose in the menu.
`RX_VK_ASTC_BC7=1` keeps those textures as BC7 instead of BC3: the same memory (8 bits a texel), but BC7's mode 6 keeps
gradients and alpha much better, so textures look cleaner where BC3 shows bands or blocks. It is off unless set, and
`RX_VK_ASTC_RGBA8=1` wins when both are.

## In-game menu

By default, press <kbd>Insert</kbd>, <kbd>F1</kbd> or <kbd>Esc</kbd> in game. On a controller, press Home or Select and
Start together; on a DualShock 4 or DualSense, clicking the touchpad does the same as <kbd>F1</kbd>.
The same keys close it; with every Menu shortcut, <kbd>Esc</kbd> goes back a page or closes the open menu.
<kbd>F1</kbd> always works. **Controls → Menu shortcut** can leave <kbd>Insert</kbd> free for another overlay (F1 only).
With Insert or F1, or with F1 only, <kbd>Esc</kbd> does not open the menu: it only lets the mouse go. Options saved by
an older version on Insert or F1, the default then, move to Insert, F1 or Esc; F1 only stays.
The menu has five tabs, Graphics, Audio, Gameplay, Controls and Debug:
<kbd>Q</kbd> and <kbd>E</kbd> (or <kbd>Tab</kbd> and <kbd>Shift</kbd>+<kbd>Tab</kbd>) switch between them, LB and RB on
a controller (or the triggers). <kbd>Up</kbd> and <kbd>Down</kbd> choose a row, <kbd>Left</kbd> and <kbd>Right</kbd>
change it (<kbd>Enter</kbd> changes it too); beside the rows, the help says what the row is and, on a line of its own,
what the value chosen does. On a controller the D-pad chooses and changes, A changes too and opens a page (Motion,
Controls, the Teleport list, the inventory), and B goes back or closes the menu: A and B are the buttons your layout puts the Switch's A and B
on (with the Nintendo layout, A is the right face button). The mouse is free while the menu is open: click a tab, click a row's name to choose it (its
value stays), click the left or right half of a value to lower or raise it, drag a slider, turn the wheel to scroll the rows (it never changes a value), and right-click to go back. The
button prompts at the bottom show the keys or the controller's buttons, whichever you used last. Your choices are saved
in `longshot_options.ini`: with your saves when the launcher starts the game on Windows, so every version keeps them, else in
the game folder ([Where the files go](#where-the-files-go)).

The menu, the FPS counter and the start-up screen are drawn in the style of the game's own menus. They use the game's
own menu font, read into memory from your game files (the romfs folder the game runs from) the first time one of them is
drawn. It is never copied anywhere, and Longshot does not include it. Without it, or if it has no Latin letters, they use
Libertinus, which Longshot includes.

The rows marked "after a restart" below are read when the game starts. While one is changed, it is tagged "After
restart" and the help on the Graphics tab lists it; closing the menu then asks whether to restart now. **Later** keeps
the change for the next start (and is not asked again until you change one of those rows); **Restart now** saves your
choices and starts the game again, so progress since your last save is lost.

A row for something this PC cannot use is not shown: NVIDIA Reflex and AMD Anti-Lag 2 without a GPU and driver that
have them, the DLSS model where DLSS does not run. HDR paper white and
HDR peak brightness and HDR contrast show only while HDR is on, the HDR tone mapper only with HDR set to Native.

The Graphics tab groups its rows under section titles: Resolution and upscaling, Frame generation and latency, Screen,
HDR, Detail, and GPU and memory (last: the rows to change when something is wrong). The table lists them in that order.

| Tab | Option | Choices | Default |
|---|---|---|---|
| Graphics | Quality preset | Custom, Minimum (4GB), Low (6GB), Balanced (8GB), High (12GB), Ultra (16GB+), Auto (this PC) | Auto (this PC) on a first start; Custom in an options file from before it |
| | Aspect ratio | The shapes your screen's resolutions have, narrowest first (5:4, 4:3, 16:10, 16:9, 21:9, 32:9, ...), your monitor's own marked "(monitor's)" (after a restart) | Your monitor's |
| | Output resolution | The Aspect ratio's sizes: your screen's resolutions and the common ones (1440p, 4K, 5K at 16:9; 1920x1200, 2560x1600 at 16:10; 3440x1440 at 21:9; ...), and 2048x1152 (the game's own reduced size, 80% of its 2560x1440, marked "(reduced)") at 16:9; your screen's own size is marked "(native)" (after a restart) | 2560x1440, the game's own picture, or your screen's size where it is smaller (a Steam Deck's 1280x800, a 1080p monitor), at its own shape (a 2560x1080 ultrawide starts at 2560x1080, a 2256x1504 laptop at 2256x1504); Auto (this PC) leaves it as it is |
| | Custom output size | A size of your own, typed as `<width>x<height>` (after a restart) | None |
| | Profile | Docked, Handheld (after a restart) | Docked; a quality preset sets it |
| | Upscaler | Game's own, NVIDIA DLSS (only in builds with NVIDIA's DLSS SDK, below, on NVIDIA RTX GPUs), AMD FSR 3.1 (every build, on GPUs it runs on, below), AMD FSR 4 (only in builds with FSR 4, below, on GPUs AMD's files cover), Off (no upscaling), Intel XeSS (Windows, with Intel's `libxess.dll` beside the game, below). NVIDIA DLAA, AMD FSR 4 Native AA and Intel XeSS Native AA are DLSS's, FSR 4's and XeSS's Native, chosen on the Render resolution row | Game's own; Auto (this PC) chooses DLSS or FSR 4 where they run, never XeSS |
| | DLSS model | Transformer (K) (CNN (E) in its place on cards with less than 7 GB of video memory), Transformer 2 (M), Transformer 2 (L), NVIDIA default (shown where DLSS runs; used by DLSS and DLAA) | Transformer (K); CNN (E) under 7 GB |
| | DLAA before the bloom | Off, On: where NVIDIA DLAA smooths the picture, after the game's bloom and tone map or before them (shown with NVIDIA DLSS; changeable with Render resolution Native (DLAA) only; changes at once; `RX_VK_DLSS_PRE_BLOOM` over it for one run) | Off |
| | Render resolution | Auto (the game's own dynamic resolution), 100%, 75%, 67%, 50% (each with its size). With DLSS or FSR 4: Auto, Native (DLAA or FSR 4 Native AA: the 3D at full size), 92%, 83%, 75%, Quality (67%), Balanced (58%), Performance (50%), Ultra Performance (33%). With Intel XeSS the same stops, named by the XeSS mode each runs under: Native (XeSS Native AA), Ultra Quality Plus (92%, 83%), 75%, Ultra Quality (67%), Balanced (58%, 50%), Ultra Performance (33%). With FSR 3.1: Auto, 100% (its native AA), then the same as DLSS from 92%. With Off: Auto, 100%, 75%, 67%, 50%, 33% | Auto; a quality preset sets it |
| | Sharpness | 0 to 100%, in steps of 10 | 0%; a quality preset sets it |
| | Frame generation | Off, Auto (it shows who makes its frames: FSR 3.1 FG or Built-in FG), 2x to 8x; with DLSS FG up to what NVIDIA's runtime makes on the GPU (6x on an RTX 50, 2x on an RTX 40) | Off |
| | Frame generation by | Shown while Frame generation is on. DLSS FG (NVIDIA DLSS Frame Generation: RTX 40 and newer, with a temporal Upscaler), FSR 3.1 FG (AMD FSR 3.1's frame interpolation, from the scene's depth and motion: any GPU), Built-in FG. DLSS FG is offered only where it runs; elsewhere the row has FSR 3.1 FG and Built-in FG, and its line at FSR 3.1 FG says why not DLSS FG (the log has NVIDIA's answer). At DLSS FG, FSR 3.1 makes the frames DLSS FG does not (Frame generation's Auto). At DLSS FG, the game's own menus, movies and loading screens show only the game's own frames (they have no Upscaler picture to make frames from): the row's line and the FPS counter say DLSS FG paused. With an Upscaler that is not temporal, the built-in one makes the frames and the line says why. `RX_VK_DLSS_FG=1` fixes the row at DLSS FG for a run. Changes at once | DLSS FG where it runs, else FSR 3.1 FG (an options file's Auto from before is this default) |
| | Game frame rate cap | 30, 60 (the console's), 90, 120, 144, 165, 240, Uncapped (changes at once) | 60 |
| | NVIDIA Reflex | Off, On, On + Boost (shown only on NVIDIA GPUs with `VK_NV_low_latency2`) | Off |
| | AMD Anti-Lag 2 | Off, On (shown only on AMD GPUs with `VK_AMD_anti_lag`) | Off |
| | Fullscreen | Off, On (the same as <kbd>F11</kbd>) | Off |
| | Fullscreen monitor | Auto (current monitor), or a named connected screen. While windowed, chooses the next fullscreen destination; while fullscreen, moves the game. An unavailable choice is retained with a temporary fallback. | Auto (current monitor) |
| | V-Sync | Auto, On, Off | Auto |
| | Window scaling filter | Off, Catmull-Rom, Lanczos (shows "Not needed (window = output)" while the window is the Output resolution's size) | Off; a quality preset sets it |
| | CRT filter | Off, Lottes aperture grille, Lottes consumer TV, Hyllian Fast, zfast CRT (changes at once; Native HDR bypasses it) | Off |
| | CRT lines | 240, 480, 720 virtual lines (shown with a CRT filter on; changes at once; capped at half the destination height) | 240 |
| | Ultrawide menus | Full width; Centred, blurred sides; Centred, black bars (with the Aspect ratio at the monitor's shape) | Centred, blurred sides |
| | HUD layout | Screen's edges; Middle 16:9 (on a screen that is not 16:9; `HOS_WIDE_UI`, when set, decides) | Screen's edges |
| | HDR | Off, Expanded, Native (the game's) | Off |
| | HDR paper white | 80 to 400 nits, in steps of 10 (while HDR is on) | 200 nits |
| | HDR peak brightness | 400 to 4000 nits, in steps of 100 (while HDR is on) | 1000 nits |
| | HDR contrast | 50% to 200% around mid grey, in steps of 5 (while HDR is on) | 100% |
| | HDR tone mapper | Vanilla (the game's curve), PsychoV35 (RenoDX's; with HDR set to Native). Compatible DLSS/DLAA, FSR 4/native AA and shared-chain FSR 3.1 upscaling run automatically; `RX_VK_PSYCHOV35_DLSS=0` disables that compatibility route. Off and the spatial/FXAA TAAU replacement support PsychoV35 directly. FSR 3.1 FG remains separately opt-in and 2x only; legacy own-input FSR upscaling remains unsupported | Vanilla |
| | Anisotropic filtering | Game's, 2x, 4x, 8x, 16x | 16x |
| | Shadow resolution | Auto (for the card's memory), Game's (1600), High (2200), Highest (3072): the sun's shadow maps, applies at once | Auto |
| | GPU device | Auto, or each graphics card by name (after a restart) | Auto |
| Audio | Volume | 0 to 100%, in steps of 5 | 100% |
| | Mute in background | Off, On | Off |
| | Output device | Default or a named Windows playback device; applies live | Default |
| | Speaker layout | Auto, Stereo, 5.1, Mono (layout changes after a restart; Mono/Stereo live on stereo output) | Auto |
| | Microphone | Off, Default, or each microphone by name | Default |
| | Microphone gain | Software input level, 0–400%; applies to new samples without changing system volume | 100% |
| | Test microphone | Start/stop capture; live levels appear in the help box for Test microphone and Microphone gain | Stopped |
| Gameplay | FPS counter | Off, FPS, Frame time, Graph, Breakdown, Dev | Off |
| | FPS counter position | Top right, Top left, Bottom right, Bottom left | Top right |
| | FPS counter size | Auto, Small, Medium, Large | Auto |
| | FPS counter details | Opens the FPS counter details page: Version and hardware at every level (Off, On; Off), then a line each of the Dev level (VRAM, Textures, Draws, Dispatches, Render passes, Draws skipped, Texture uploads, Pipelines built, GPU slot waits, Queue submits, Write-backs) and the CPU and GPU lines (Off, On; On) | Version and hardware off, every line on |
| | Menu size | Small, Medium, Large | Medium |
| | Game speed | Per frame, Real time | Real time |
| | Field of view | 50% to 150%, in steps of 5; applies immediately during gameplay | 100% (original) |
| | Text speed | Normal, 2x, 4x, Instant | Normal |
| | Text size | Original, 110%, 125%, 150%, 175% (dialogue, Navi and signs) | Original |
| | Skip title screen | Off, On (after a restart) | Off |
| | Game language | The game's 16 languages, each by its own name (after a restart) | `language.txt`'s, else English (US) |
| | Menu language | Auto (the game's language), or English, Português (Brasil), Español (Latinoamérica), Español (España), Français, Deutsch, Italiano, Nederlands, Polski, Русский, 日本語, 한국어, 简体中文, 繁體中文 | Auto |
| | Discord status | Off, On ("Playing Longshot" and the time played in your Discord status, while Discord runs on this PC) | On |
| | Skip opening | Off, On (skips the new-game opening: the story movie and Navi waking Link; later cutscenes and the title screen stay) | Off |
| | Bug report | Saves a report into a new folder in `bug-reports` (below) | |
| Controls | Controller in background | Off, On | Off |
| | Mouse camera | As the stick, Native, Off | As the stick |
| | Disable camera centering | Off, On | Off |
| | Mouse sensitivity | 0.10 to 5.00, in steps of 0.10 | 1.00 |
| | Invert mouse vertical | Off, On | Off |
| | Invert mouse horizontal | Off, On | Off |
| | Invert camera X | Off, On | Off |
| | Invert camera Y | Off, On | Off |
| | Camera speed | 50% to 400%, in steps of 25 | 100% |
| | Stick dead zone | Default (24%), 5%, 10%, 15%, 20%, 30% | Default (24%) |
| | Rumble strength | Off, 25% to 150%, in steps of 25 | 100% |
| | Layout preset | Nintendo (Def), Xbox / PC, Swap Face, Switch 2 Pro; Custom once you bind a button yourself (on the Controls page) | Nintendo (Def) |
| | Button prompts | Auto, Switch, Xbox, PlayStation 4, PlayStation 5, Steam Deck, Keyboard (on the Controls page; changes at once where the game allows it, else after a restart) | Auto |
| | Motion | Opens the Motion page: Motion controls (Off, On; On), Motion sensitivity X (10% to 600%) and Y (10% to 300%, in steps of 10; 100%), Invert motion X and Y (Off, On; Off), Motion deadzone (Low, Standard, High; Standard) | |
| | Walk mode | Hold, Toggle | Hold |
| | Walk speed | 15% to 90%, in steps of 5 | 45% |
| | Controls | Opens the Controls page | |
| | Menu shortcut | Insert or F1; F1 only; Insert, F1 or Esc | Insert, F1 or Esc |
| | Controller menu button | None, Home, Back left (GL), Back right (GR) | Home |
| | Compact menu key | Right Shift, Right Ctrl | Right Shift |
| NFC | NFC tag | The tag files of your NFC folder, by their names | The first |
| | Tap tag | Holds the chosen tag to the game's reader | |
| | Remove tag | Takes it off the reader | |
| | Scan tag on controller | Reads a tag you own with a Switch 2 Pro Controller (USB) and saves it in your NFC folder | |
| Debug | Debug tools | Off, On (while on, the tools below it: Heart protection, No-clip flight, Mark position, Return to mark, Set time, Inventory item, Change quantity, Free camera, Teleport, Switch age, Inventory, Movement speed) | Off |
| | Autosave: | On or Off, available with Debug tools off. Enabling Debug tools switches Autosave to Off. Off skips supported autosaves, including the verified map transitions. You can still save manually. | On |
| | Player: Damage taken | Half, Normal, Double, Quadruple (live, while Debug tools is on; Half rounds damage up to a quarter heart; Heart protection wins) | Normal |
| | Developer: Save my pipeline list | Saves a zip of the pipelines your play built into Downloads ([below](#send-your-pipeline-list)) | |
| | Developer: Frame pacing | Off, On | Off; a quality preset turns it on |
| | Developer: Threaded rendering | Auto, On, Off (after a restart). Auto is On with 6 or more processor cores for the game, and the render thread alone with 4 or 5 | Auto |
| | Developer: Descriptor reuse | Off, On (changes at once) | On |
| | Developer: Sampler cache | Off, On (changes at once) | Off |
| | Developer: Robust buffers | Auto, Off, On (after a restart) | Auto |
| | Developer: ASTC as RGBA8 | Off, On (after a restart) | Off |
| | Developer: Intel shader compiles | Auto, Side by side, One at a time (after a restart; Intel graphics on Windows only) | Auto |

- **Quality preset** sets Render resolution, Window scaling filter, Sharpness, Frame pacing, Game speed, Profile and
  Shadow resolution (back to Auto) for a card with that much memory; how much video memory the textures use comes from the card itself.
  It leaves the Upscaler and Frame generation as you chose them: with DLSS, FSR 3.1 or FSR 4, the preset's resolution
  becomes their nearest mode. Changing most other rows yourself sets it back to Custom.
- **Auto (this PC)**, which a first start begins on, chooses for the PC it runs on: the memory tier from the card's
  video memory (an integrated GPU, a Steam Deck or a card without mesh shaders at Minimum), DLSS on an NVIDIA RTX card
  or FSR 4 on an AMD RX 9000 where the build has them (else the game's own upscaler). It leaves the Output resolution as
  it is: on a first start the game's own 2560x1440, or the screen's size where that is smaller. In the first minute of play it then measures the GPU: while
  frames are missed with the GPU over 90% busy it lowers the Render resolution a step, and while the frame rate cap holds
  with the GPU under half busy it raises it a step, up to the best mode that still upscales (DLSS or FSR Quality, or 100%
  with the game's own; never DLAA or native AA). At most two steps; a PC that holds its cap keeps its picture. It chooses again when the card or the screen changes. `longshot_log.txt`'s
  `Auto (this PC)` lines say what it chose and why.
- **Menu size** is how large the menu, the FPS counter and the start-up screen are drawn: Medium unless you choose Small
  (a desk) or Large (a TV across the room). It stays the same whether you use a controller or the keyboard and mouse.
- **Field of view** changes the gameplay camera's vertical angle: 100% keeps the game's view, higher shows more of the
  world and lower brings it closer. The game's zoom changes remain relative to your choice.
  Aiming has been tested in the 50% to 150% range, including with ultrawide enabled.
  Cutscenes, the HUD and movies keep their own view. The setting is saved as `field_of_view` in `longshot_options.ini`.
- **Skip title screen** starts the game with your most recent save: Longshot presses A through the title screen and its
  file list as soon as the game has a save. Until you have saved once, it leaves the title screen alone, so a new game can
  still be started. It applies after a restart; turn it off to watch the intro and use the title.
- **GPU device** matters on a PC with more than one graphics card (a laptop's integrated and dedicated GPUs, or a
  second card for Lossless Scaling). Auto prefers a dedicated one, and only cards with Vulkan 1.3 are listed. The card
  you choose is saved as that card (`gpu_device_id=` in `longshot_options.ini`), not its place in the list, so it stays
  chosen when the list's order changes (a driver update, a card added, an update into a new folder). A chosen card that
  is not there (an eGPU unplugged) is Auto until it is back. `longshot_log.txt` lists every card as `vk: device <n>`;
  `RX_VK_DEVICE=<n>` chooses one for a single run and wins over the menu.
- **Frame generation** makes extra frames between the game's own. It adds a little input delay, and fast motion can
  show artifacts. **Frame generation by** chooses who makes them: **DLSS FG**, NVIDIA DLSS Frame Generation where it
  runs (RTX 40 and newer, at 2x up to what NVIDIA's runtime makes on the GPU: 6x on an RTX 50, 2x on an RTX 40; the
  Frame generation row stops there, and a higher value saved before shows and runs as that top. AMD FSR 3.1 makes the
  frames it does not, such as Frame generation's Auto), and the default there; **FSR 3.1 FG**, AMD FSR 3.1's frame interpolation (any GPU it runs on, at every multiplier and at Auto,
  from the scene's depth and motion; below), the default everywhere else; **Built-in FG**, the renderer's own. **Frame pacing** shows frames at an even interval: less stutter, up to one frame of delay. It paces
  the frames that frame generation shows, so it has no effect while Frame generation is Off. A fixed multiplier shows each
  frame at its own time on the game's timeline whether Frame pacing is On or Off (with V-Sync On one a refresh at most;
  where the window's swapchain holds all of a set's frames at once: up to 6x on an NVIDIA card). For the most even of those
  frames, turn on **Present queue** on the Debug tab (or put `RX_VK_PRESENT_QUEUE=1` in `longshot_env.txt`) and restart:
  the presents then get a queue of their own, and no made frame waits there behind the game's next frame. It is off by
  default: on some cards it loses the GPU with frame generation on (an RTX 5080 on NVIDIA's 621.24; an RTX 4090 had a
  black screen, Hookshot issue 1161, before the fork). If the game crashes or the screen goes black with it and you
  cannot reach the menu, put `RX_VK_PRESENT_QUEUE=0` in `longshot_env.txt` (it overrides the row), then turn the row
  off and take the line out.
- **Game frame rate cap** limits the frames the game renders. With a cap of 60 and Frame generation at 2x, the output
  can reach 120 FPS. Choose **Auto** in Frame generation to target the display's refresh rate instead of a fixed multiplier.
  A cap **past 60 is experimental**: the game keeps real time, but some cutscenes and bosses still count frames and can
  misbehave (the intro's narration running ahead: #1742). 60 is the safe choice.
  Gohma's ceiling-positioning fault is fixed, tested in complete fights at about 98-99 FPS and at 60.
  The frozen Zoras under the ice in Zora's Domain stand as still above 60 as at 60 (they twitched: #988).
  Scarecrow's Song keeps the timing of its eight-note recording and playback above 60. Saved songs are not retimed;
  re-record a song after updating if its recorded rhythm was lost.
  Epona jumps fences above 60 as she does at 60 (she had galloped through them, more often the higher the cap).
  The Shadow Temple's barge carries Link and the enemies on it above 60 as at 60 (above 100 FPS they slid to its back).
  After you play a song, its notes show one at a time with its melody above 60, as at 60.
  **Uncapped** renders as fast as the PC can (the game still keeps real time). A display that waits for its refresh still
  holds the frames to its refresh rate: V-Sync on, or the driver's FIFO mode. `HOS_FPS_MAX=30` or `HOS_FPS_MAX=<61-1000>` sets a cap for one
  run.
  The 30 FPS cap uses host pacing and real-time stepping, including when Game speed is Per frame. It is saved as `frame_rate_cap=-1`; existing cap values 0–6 keep their meanings.
- **Bug report** writes a folder `bug-reports/<date>_<time>` in the game folder with what an issue needs: `report.txt`,
  which answers the [game problem form](https://github.com/NotRexed/Hookshot/issues/new?template=1-game-problem.yml)'s
  questions under its own headings (the version, the map you are in, your graphics card and driver, CPU, RAM and
  system, the menu options and the `RX_`/`HOS_` switches you changed) and ends with a link that opens the form with them
  filled in (the switches by name only); `longshot_log.txt` (this run's log so far) and `longshot_log.1.txt` (the run
  before's, which has the crash if the game crashed last time); and `picture.png`, the game's picture without the menu.
  Settings whose defaults depend on the display, upscaler, Auto preset or other run-time context are listed after the
  changes, in the same answer, under a **Depends on this PC** line, with their current value and “default depends on this
  PC”. They are not counted as changed settings; the report does not invent a default from a run-time list index. The rows
  a preset sets keep their fixed defaults on Custom while Auto (this PC) has never chosen for the options file; with a
  preset, or once Auto has chosen, they depend on this PC.
  Fixed defaults, including GPU Auto, the system-default microphone and Frame generation by's DLSS FG, still report actual
  changes (Frame generation by where DLSS FG cannot run: FSR 3.1 FG); the Aspect ratio's default is
  the monitor's shape, and the Game language's `language.txt`'s.
  Your home folder is written `~` (`%USERPROFILE%` on Windows) and your user name `<user>` in the logs' paths. A switch's
  value is written only when it is a number, a size or on/off, and never for a name with `TOKEN`, `KEY`, `ROOM`,
  `SECRET`, `PASS`, `SERVER` or `USER` in it; other values read `(value left out)`. Nothing else goes in (no saves, no
  game files, not `longshot_options.ini` or `longshot_env.txt` themselves), and nothing is sent: open the form, write
  what happened, and drag the folder's files into it (from File Explorer on Windows, a file manager on Linux; on a Steam
  Deck, in Desktop Mode). The row then shows the folder's name. A game that stops before its menu can be used leaves its
  log as `longshot_log.1.txt`: start it again, or attach that file yourself.
- **The scaling rows**, in the picture's order: **Output resolution** is the size of the game's whole frame, HUD
  included. The **Upscaler** fills it from the 3D: the game's own network, NVIDIA DLSS or DLAA (the 3D at full size,
  edges smoothed), AMD FSR 3.1, or AMD FSR 4 or its Native AA (below). DLSS and DLAA look softer than the game's own: set
  **Sharpness** to around 80% with them. **Off** is no upscaler at all: the game's own network is left out too, and the 3D
  is stretched to the output with a plain linear filter (the fastest choice, below). **DLSS model**
  chooses their network (below). **Render
  resolution** is the 3D's share of the output. The **Window scaling filter** then scales the frame to your window,
  and **Sharpness** sharpens it, by the picture's pixels: when the window is much larger than the picture (the Handheld
  Profile's 1536x864 on a 4K screen), a Sharpness kept from an older version is stronger than it was, so a lower one may
  suit.
  Changing the Upscaler keeps your chosen render scale, including across a restart. An engine without that scale
  temporarily uses and shows its nearest supported size: DLSS Balanced (58%) shows 50% with the game's own, then 58%
  again on returning to DLSS. Changing Render resolution or choosing a Quality preset replaces that saved scale.
- **Aspect ratio** and **Output resolution:** the menu lists the resolutions your screen offers (its display modes, as
  Windows lists them, DSR's included), with the common sizes of each of those shapes your screen does not list (1440p, 4K
  and 5K at 16:9, 1920x1200 and 2560x1600 at 16:10, 3440x1440 at 21:9, ...), grouped by shape. **Aspect ratio** lists
  the shapes, your monitor's own marked "(monitor's)" (the default: 21:9 on an ultrawide, 16:10 on a 16:10 screen);
  **Output resolution** then lists that shape's sizes, your screen's own marked "(native)". A size larger than your
  screen is drawn at that size and scaled down to the window. The marks are for the monitor the game's window is on, and
  follow it: moved to another monitor, or the desktop's resolution changed while playing, the menu adds that screen's
  sizes. On Linux the screen's size comes from SDL (the primary display). A portrait screen has no shape the game can
  draw, so the row shows Monitor's shape as a value of its own there. Docked only. A shape other than 16:9 widens the cameras (21:9, 32:9) or narrows the
  picture (16:10, 4:3, 5:4); the HUD keeps its 16:9 size with its corners at the screen's edges (**HUD layout**: or in
  the middle 16:9, as before), and movies stay in the middle 16:9. At 16:9 on a 5120x1440 (32:9) monitor, the
  game's 16:9 picture is stretched across the 32:9 output, sharp but wide. Options saved by an older version at 16:9,
  the default then, move to Monitor's shape on a screen narrower than 16:9 (16:10 such as the Steam Deck's, or 4:3),
  where 16:9 only adds bars above and below; an ultrawide keeps its 16:9. The output's rows below show the sizes for the
  output you chose. At your screen's own size the HUD and text map pixel for pixel. Above 2560x1440 the game's render
  targets get a bigger GPU pool, so a full-size 3D fits. Where the screen's modes are not known (Linux), the menu offers
  1920x1080, 2560x1440, 3840x2160, 5120x1440 and 5120x2880. For a size the menu does not offer, type it in
  **Custom output size** (Enter or the pad's confirm button to edit and set it, Esc or the back button to cancel).
  On a controller, D-pad Left/Right moves the caret, Up/Down cycles its digit or separator, the right shoulder inserts
  a digit and the left shoulder removes a character. At the end, Up/Down adds a character. The shown Paste button
  still pastes; with a keyboard, Ctrl+V or Shift+Insert does the same. Full-width digits and `ｘ` become ASCII.
  Or set `output_res=<width>x<height>` in `longshot_options.ini`: each side a multiple of 8, at least 640x360,
  at most 5120x2880's pixels, and between 5:4 and 48:9 in shape. The menu says why it refuses a size; in the file,
  anything else uses the game's reduced 2048x1152. The Output resolution row chooses the typed size from its list when it is
  already there, including sizes found after a monitor change, or offers it as your own choice and selects its shape.
  Typing a 16:9 size keeps Aspect ratio on the monitor's shape when it already follows the monitor.
  The size this run started with stays available until the next start, so returning to the starting shape still
  restores that size even after a custom-size edit.
  A picture wider or narrower than 16:9 is kept to 5120x2160's pixels, so above that a size of another shape is
  not a native frame. A size's own shape sets the camera's, even when the OS reports a single 16:9 primary monitor. For
  three 16:9 monitors, use `5760x1080` or `7680x1440` and a window or logical display
  that spans all three. Both render at full size: 7680x1440 has 5120x2160's pixels, the most a wide picture takes.
  For one run, set `HOS_OUTPUT_RES` to a size, such as `HOS_OUTPUT_RES=3840x2160`.
- **Window scaling filter** matters only when your window's size differs from the Output resolution: for the sharpest
  picture, set the Output resolution to your screen's, and the row shows "Not needed (window = output)". It and
  **Sharpness** also work when Frame generation is Off.
- **CRT filter** adds scanlines and a phosphor mask to the finished SDR picture, including the game's HUD, after
  window scaling and sharpening. Longshot's menu and overlays stay clean. Lottes aperture grille is a flat PVM-style
  look, not a calibrated Sony model; Lottes consumer TV adds curved glass, a shadow mask and beam bloom. Hyllian Fast
  uses cubic reconstruction and a magenta/green mask; zfast CRT has softer scanlines and a monochrome aperture mask.
  **CRT lines** chooses a virtual raster, independent of Render resolution: 240, 480 or 720 lines, capped at half the
  destination height (at least one line), with width following its aspect. Exact integer multiples give the most regular
  lines; non-integer scaling can cause moire. Neither option forces 4:3 or integer scaling. Both apply at once and
  quality presets, including Auto, preserve them. Expanded HDR expands the filtered SDR picture; Native HDR (PQ)
  bypasses CRT and keeps the choice for SDR. **Off**, the default, adds no CRT passes or GPU images.
- **Anisotropic filtering** keeps textures sharp on surfaces seen at a slant: the paths and floors ahead of Link. The game
  asks for 1x or 2x, which at a PC's resolutions smears the ground a few metres off; 16x costs little on most cards, and
  Game's is the console's own. `RX_VK_ANISO=<n>` sets it for one run (0: the game's). It is for the game's textures:
  a picture the game drew itself and reads back (one level, no mipmaps) keeps the game's own anisotropy.
- **Shadow resolution** sets the size of the sun's shadow maps. The game draws them 1600 pixels a side, which at a PC's
  resolutions turns the shadows of thin things near Link (the drawbridge's chains, fences, ivy) into stair steps. High
  (2200) and Highest (3072) are the game's own larger sizes, from its own table: by their size, Highest takes about
  113 MB of video memory against the game's 31 MB, and on an RTX 4090 0.05 to 0.07 ms more GPU time a frame. Auto, the
  default, picks by the card's memory, the Quality preset's tier or else the one Auto (this PC) chose: the game's size on
  Minimum and Low (4 and 6 GB), High on Balanced (8 GB), Highest from High (12 GB) up. Choosing a preset puts the row back
  on Auto. It applies at once and is saved as `shadow_resolution`.
- **Upscaler Off** (`dlss=6` in `longshot_options.ini`) is for slow GPUs and for measuring: no DLSS, no FSR, and not the
  game's own network either, which otherwise runs on every frame (at the game's usual 3/4 share it scales the 3D up and
  smooths its edges; at 100% a pass of the same kind still smooths them). In its place the 3D is stretched to the output
  with a linear filter, and the HUD is drawn over it at the output's size as always. Nothing is kept from frame to frame, so
  there is no ghosting, but no anti-aliasing either: edges are jagged, and below 100% the picture is soft. The Render
  resolution then sets the 3D's size alone (Auto, 100%, 75%, 67%, 50%, 33%): lower is faster. RX_VK_DLSS, RX_VK_FSR4 and
  RX_VK_FSR set for a run go over it, as over the row's other choices.
- **Texture detail with DLSS, FSR 3.1 or FSR 4:** DLSS and DLAA sample the world's textures as NVIDIA's DLSS guide asks:
  at the output's size and one mip step finer (the game's own bias + log2(3D / output) - 1), which DLSS resolves over its
  frames, so they keep the detail of the full-size picture instead of the 3D's smaller size. FSR 3.1 and FSR 4 sample them
  one finer mip step for each halving of the 3D below 75% (the game already does so for its own 75%).
  `RX_VK_UPSCALE_LOD_BIAS=0` turns it off; `RX_VK_DLSS_LOD_OFFSET=<steps>` sets DLSS's extra step for a run (-1 by default,
  0 for none, down to -3).
- **NVIDIA Reflex** Off is the game as without it. `RX_VK_REFLEX_LOG=1` logs the driver's latency timings every 300
  frames while it is on.
- **AMD Anti-Lag 2** Off is the game as without it. It works with Frame generation too: each game frame's present
  stage is given when that frame itself is shown, not at the made frames before it.
- **Profile** runs the game as the console docked (at the Output resolution) or Handheld (1536x864): lighter on the GPU.
- **Mesh shaders** (how the game's mesh programs, much of the scenery, run) is no longer a menu row: changing it made
  the next start translate every shader again. The game uses the GPU's own mesh shaders where it has them, and the
  compute path where it has not and on Linux AMD cards (issue 2245). For troubleshooting, `RX_VK_MESH_SHADERS=compute`
  in `longshot_env.txt` uses the compute path on every GPU (try it if your card crashes or leaves objects out; usually a
  little slower), and `RX_VK_MESH_SHADERS=native` the GPU's own for every draw. A `mesh_shaders=` line left in
  `longshot_options.ini` is no longer read.
- **Ultrawide menus:** with the Aspect ratio at the monitor's shape, the pause, item and map menus stay in the middle
  16:9, with what the game draws beside them out of focus, or black; Full width stretches them over the whole screen,
  as before. Loading screens and pre-rendered movies get the same sides. It changes at once.
- **V-Sync:** Auto is mailbox where the driver has it (no tearing, no wait for the display); otherwise On, or on Windows
  Off. On waits for each refresh (FIFO): no tearing, at most the display's rate. Off shows each frame at once and can
  tear (where the driver cannot, it is Auto). It changes at once. `RX_VK_FIFO=1` (On) or `RX_VK_IMMEDIATE=1` (Off) chooses
  for one run.
- **Threaded rendering:** On draws the picture on threads of its own (a render thread and a recorder thread) while
  the game goes on to its next frame, for a higher frame rate on a processor with threads to spare; it is still being
  tested and can show flashes. Off draws it on the game's own thread, as before. Auto chooses by the processor's cores
  for the game, each core counted once however many threads it runs: On with 6 or more, the render thread alone with 4
  or 5 (where the recorder thread beside it was slower), Off with fewer, and Off wherever the mode is not safe. It
  applies after a restart. `RX_VK_ASYNC` and `RX_VK_RECORDER`, when set, choose for
  one run.
- **Robust buffers** (Debug tab, Developer): a read past a buffer's end is dropped instead of stopping the GPU. Auto is
  on, on every card; Off can be faster on some NVIDIA cards.
- **ASTC as RGBA8** (Debug tab, Developer): the AMD texture workaround below.
- **Present queue** (Debug tab, Developer): the presents on a GPU queue of their own. Off by default; On makes frame
  generation's frames more even with V-Sync Off on some cards (an RTX 5090), but lost the GPU on others with frame
  generation on (an RTX 5080). It applies after a restart; `RX_VK_PRESENT_QUEUE` in `longshot_env.txt` or the environment
  chooses over it at every start while it is set (the row then shows it and cannot be changed).
- **Intel shader compiles** (Debug tab, Developer; Intel graphics on Windows only): One at a time lets one thread at a
  time into Intel's shader compiler; Side by side is faster on many cores but can crash in the driver. Auto, the default,
  is side by side on the Arc B-series with driver 101.9154 or newer, where it was measured, and one at a time elsewhere. See
  [Intel laptops](#intel-laptops). It applies after a restart.
- **Game speed:** with Per frame, the game runs slower whenever it is below 60 fps. Real time keeps it at normal speed
  down to 15 fps.
- **FPS counter** shows in a corner (**FPS counter position**; at the top right while the menu is open), as large as
  **FPS counter size** says (Auto grows with the window, as the menu does; Small, Medium and Large keep the size Auto has
  in a 720p, 1080p and 1440p window, whatever the window), at one of
  five levels. **FPS** is the number alone (an options file's old On); with frame generation on, it also shows the
  game's own rate and who makes the frames: `Base 60, DLSS FG` (NVIDIA DLSS Frame Generation), `FSR 3.1 FG` or `Built-in FG`. **Frame time** adds the game's frame time and its 1% low (the mean time of the slowest 1% of frames
  over the last 10 seconds). From this level on, the number and the frame time are green at the Game frame rate cap's rate,
  amber under it and red under two thirds of it. (The cap's rate is the display's refresh instead with V-Sync On when
  that is slower; with Uncapped and V-Sync not On there is no line, and the colours are against 60.) **Graph** adds
  the frame times of the last 4 seconds (each point the slowest frame of its span, so no stutter is missed), with a
  dashed line at the Game frame rate cap's frame time and a red mark over frames that took more than twice it; its
  scale follows the usual frame, so a spike is drawn at the top rather than flattening the rest. **Breakdown** draws
  each of the graph's frames as the parts its time went to, stacked, with their values under the graph; the values add up
  to the frame time: the game's own code (Game), the renderer's part on the game's thread (Renderer: handing
  over the frame's work, or all of its recording with Threaded rendering off), then the waits under
  Waiting on: with Threaded rendering, for the game's main thread to finish writing the frame (Game's main thread), for
  the render thread (Render thread: work it had not recorded yet, or the frame before it had not replayed), for the GPU to
  finish work the game waits on (GPU fence) and for the GPU at the present (GPU present), and last the frame's turn (Pacing: the frame rate cap,
  vsync, Reflex). The colours keep one meaning: blue the game's work, red the GPU, purple the render thread, grey pacing
  and idle. With Threaded rendering on, a second lane under the graph shows the render thread's own frame in parallel,
  on the same scale: recording, waiting for the parallel replay's workers and for a GPU slot, and idle. Then the GPU's
  own time per frame and its share of the frame (it runs in parallel too), and the shaders built and draws left out in
  the last 4 seconds (red marks on the graph where they happened, the usual cause of a stutter). **Dev** adds the
  renderer's own numbers, for debugging and bug reports: video memory used of what the driver allows (red past it), the
  textures held and their memory, draws, dispatches and render passes a frame, draws skipped a frame (red when any),
  texture uploads a second, the pipelines built since the start, and the renderer's pressure a frame: its waits for a
  recording slot (the GPU behind), its queue submits (the driver's and the kernel's time) and the GPU's results written
  back to the game's memory (VRAM on a line of its own, the rest two to a line while they fit, else on a line of
  their own); at the counter's foot four lines: the
  version (as the menu's banner shows it), the CPU, the GPU and its driver, at every level too with **FPS counter details**'
  Version and hardware at every level. The details page turns each of the Dev level's lines on or off.
- **Volume** scales the game's whole sound, on top of the game's own sound settings and Windows' volume. Its steps
  follow a curve so they sound about even: 50% is a quarter of the level (-12 dB). **Mute in background** silences the
  game while another window has the focus; an overlay such as Steam's or Discord's does not count as another window
  while the game was in front.
- **Output device**, under Audio, selects a playback endpoint, with Default following
  the system output. Changes briefly restart the audio stream. A missing endpoint falls back to Default and is retried;
  its saved choice is retained. `HOS_AUDIO_DEVICE` overrides selection for that run without changing the saved choice.
  Windows saves the endpoint's persistent ID. Linux uses the loaded SDL2/SDL3 backend and device name, resolved again
  when opening playback; device numbers are not saved. A renamed device is treated as missing. Identically named
  Linux outputs are ambiguous and are not offered; use Default and the system mixer to route those.
- **Speaker layout** is the speakers the game mixes its sound for. Auto is 5.1 when the selected/default output device has 5.1
  or more speakers, else stereo; Stereo or 5.1 chooses one whatever the device has (5.1 on stereo speakers is mixed
  down). Auto counts the speakers Windows names, not the channels: an audio interface whose outputs are not set up as
  surround speakers (a Focusrite Scarlett's 8 outputs, for one) is stereo, so voices and the ocarina do not go to an
  output with nothing connected. Mono averages the stereo channels into both ears, for one-sided hearing or one earbud. Mono/Stereo
  changes apply live while output is stereo; changing the channel layout (from or to 5.1) needs a restart.
  Auto never applies the mono average. Saved `audio_output` values remain 0 Auto, 1 Stereo, 2 5.1; Mono is 3.
  For one run, `HOS_AUDIO_CHANNELS=2` or `6` chooses the channel layout over the menu (6 leaves Mono inactive).
- **Microphone**, also under Audio, is what the ocarina hears when the game's own microphone option is on (an instrument, or humming).
  Default follows Windows' default recording device; the list shows up to 64 recording devices by name, and a change
  applies at once. Physical and virtual inputs share that limit; devices after the first 64 reported by the audio backend
  are not listed. A chosen microphone that is unplugged stays chosen (shown as not connected): Default stands in until it is
  back. On Linux (SDL2) two microphones with the same name cannot be told apart: the first is used. Windows must allow desktop apps to use the microphone
  (Settings, Privacy & security, Microphone). `HOS_MIC_LOG=1` logs what the game reads, a line a second.
- **Microphone gain** changes captured samples for Longshot only, from 0% to 400% (100% unchanged); it does not change
  the system's input volume. Boosting raises noise too, and excessive levels clip. It applies to newly captured samples;
  samples already buffered for the game keep their previous gain.
- **Test microphone** starts/stops capture explicitly. During a test, the help box for Test microphone and Microphone gain
  shows a read-only bar labeled Testing, with the incoming percentage before gain and adjusted percentage after gain.
  The same explanation and percentage bullets appear on both rows. The meter reports
  clipping, a default-device fallback, unavailable capture, or no recent samples. Opening Audio alone never starts a test;
  Microphone Off prevents one. Leaving Audio, closing the overlay or losing focus stops it at the next menu tick.
  The test shares the game's 48 kHz mono capture when active; stopping a test leaves capture needed by the game running.
  No microphone sound is played back or saved. Test-only samples are drained before a first game stream joins.
- **Game language** is the game's own text, `language.txt` in the game folder: the row writes it once you leave the row,
  and the game reads it at its next start (`HOS_LANGUAGE`, when set, wins over the file, as for the game). Where the
  folder cannot be written the row goes back to the file's language. **Menu language** is this menu's: Auto follows the
  language the game runs in until the restart (English (UK) and French (Canada) take English and French). The
  translations are machine-made and want checking by speakers of each language: each is
  one line in `gpu/vk/menu_i18n.h`. Place and item names stay English for now. Japanese, Chinese and Korean letters come
  from the game's own fonts for them, with Windows' (Yu Gothic, Microsoft YaHei, Microsoft JhengHei, Malgun Gothic) or
  Linux's Noto CJK where a game font lacks one.
- **Discord status** shows "Playing Longshot" in bold, and under it where Link is and his age while you play
  (Kakariko Village - Adult, Inside the Deku Tree - Child, Fighting Queen Gohma - Child) with his hearts, Gold Skulltula
  tokens and the pieces of heart he has found below that (❤️ 2.75/3 · 🕷️ 12 · 💗 5), or the version (v3.0.0 RC7) until he is in a map, and the
  time played; the version is also the picture's hover text. It is on by default and does nothing unless a Discord client is running on this PC; with the row Off nothing is sent to it. What it says is yours to change in
  `longshot_options.ini` (the menu keeps these lines):

  | Line | What it sets | Default |
  |---|---|---|
  | `discord_text=` | the status's two lines | `{location\|{location} - {age}{break}{hearts\|❤️ {hearts}/{max_hearts} · }{skulltulas\|🕷️ {skulltulas} · }{heart_pieces\|💗 {heart_pieces}}}{!location\|{version} {build}{!version\| {commit}}}` |
  | `discord_image=` | the large picture: an art asset's name in the Discord application, or an `https://` link to a picture | `longshot` |
  | `discord_image_text=` | the picture's text, shown on hover | `Longshot {version} {build}{!version\| {commit}}` |
  | `discord_app_id=` | the Discord application: the bold name ("Playing Longshot") and its art; Discord shows no other name there | Longshot's |

  In the status, `{location}` is where Link is, `{age}` Child or Adult, `{map}` the game's own name for the map,
  `{hearts}` and `{max_hearts}` his hearts (quarters as .25 .5 .75), `{rupees}` his rupees, `{skulltulas}` his Gold Skulltula
  tokens and `{heart_pieces}` the pieces of heart he has found (all of them, not only the 0 to 3 he holds: worked out from his hearts and the boss heart containers he has taken): all empty on the title screen. Emoji work as they are. Longshot's version comes in parts: `{version}` the release (v3.0.0), `{build}` a
  release candidate or nightly made readable (RC7, N486; nothing on a release), `{commit}` the commits since it and
  their hash (25-g7a029028; the defaults show it only on a nightly, N486 09a20cd6, which has no version number) and `{release}` all of it as the menu shows it (v3.0.0-rc.7-25-g7a029028). `{break}` ends the
  first line: what follows is the smaller second line. `{location|...}` shows what is inside only when there is a
  location (any of the names works: `{commit| [{commit}]}`), `{!location|...}` only when there is none, and these
  nest. A part that is empty also takes its brackets
  and extra space with it, so `Longshot {version} {build} [{commit}]` reads "Longshot v3.0.0 RC7 [25-g7a029028]" on a test
  build and "Longshot v3.0.0" on a release. Write a brace as `{{`. For example:
  - `discord_text={location|{location}{break}}{version} {build} {commit}`: the place without the age or the stats, and
    the version with its commit;
  - `discord_text={location|{location} - {age}}`: the place and the age only, nothing on the title screen;
  - `discord_text=`: no text, the picture and the time played only.
- **Send performance statistics** (a tick on the launcher's page, off by default): the game counts how its frames,
  hitches and movies went on each map, with one-second samples of Link's X/Y/Z position and camera yaw/pitch. The
  launcher sends five-minute batches to the team at its next start, with the GPU, driver, CPU, OS and renderer but no
  paths, user name or ID. The game sends positions rounded to the nearest 100 units, and angles as the middle of their
  45-degree yaw and 30-degree pitch bin, never the exact place. It costs a few nanoseconds a frame; unticking it deletes
  what hasn't been sent. What is in a summary: [performance statistics](docs/performance-statistics.md).
- **Controller in background:** On keeps the controller working while another window has the focus. The keyboard and
  mouse still need the game's window.
- **Mouse camera:** Native turns the camera with the mouse one to one, as in a PC game. As the stick uses the mouse in
  place of the right stick. The <kbd>-</kbd> and <kbd>=</kbd> keys change the sensitivity in game too.
  When another camera takes over, Native leaves it alone; on returning, mouse look starts from the game's current
  pitch and yaw rather than restoring the old view. Off leaves the mouse out of play: a click in the window doesn't
  capture it, so it neither turns the camera nor presses buttons. Play with a controller or the keyboard.
- **Disable camera centering:** On stops the game gradually turning the camera toward your movement, so it does not fight
  native mouse look. Off keeps the game's standard behavior. Changes apply immediately with either Mouse camera mode.
  Manual controls, explicit recentering and scripted camera headings still work; automatic pitch adjustment is unchanged.
- **Camera speed** is how fast the right stick turns the camera, in percent of the game's own speed. The camera keys
  follow it, and so does the mouse with Mouse camera on As the stick; Native mouse look has its own sensitivity. The
  game's own camera speed option still picks the speed this multiplies.
- **Stick dead zone:** how far a stick moves from center in any direction before it counts. Default is about 24%. Use
  a lower value for a precise stick or a higher one for a stick that drifts. The game also applies its own dead zone.
- **Rumble strength** scales the controller's rumble: 100% is the game's own, Off turns it off. The game's HD rumble
  goes to a controller's heavy and light motors by its frequency.
- **Motion controls** (Controls tab, Motion page): the controller's gyroscope aims where the game uses motion, as on the Switch. It works with
  Switch Pro controllers and pads in Switch mode, DualSense and DualShock 4 (through SDL). Its row
  says whether the connected pad has a gyroscope; Off leaves the game seeing a pad lying still. **Motion sensitivity X/Y**
  scale the left-right and up-down aim (100% is the console's), **Invert motion X/Y** turn either direction around. The game gets the motion
  state as a Switch (and Eden) builds it, so how you hold the pad when motion starts does not matter. Motion follows the pad used
  last. The gyroscope's drift is learned while the pad rests on a table, so set it down for a second if the aim
  creeps. `HOS_MOTION_DEBUG=1` (in `longshot_env.txt`) logs what the game gets twice a second.
- **Layout preset:** Nintendo has A on the right face button, Xbox / PC and Swap Face put A on the bottom one. Changing
  a button yourself sets it to Custom.
- **Button prompts** (on the Controls page): the button icons in the game's texts, hints and menus. Auto shows the first
  controller's; with no controller connected, the keyboard's. Keyboard shows the key or mouse button your controls put each
  action on (the first of its two), and your movement keys and the mouse for the sticks; the ocarina's notes and the pause
  menu's controller keep the Switch's buttons. A prompt for several directions at once shows W A S D while those are the
  movement keys, and the stick's own icon with other keys.
- **Save my pipeline list** (Debug tab, under Developer): see [Send your pipeline list](#send-your-pipeline-list).
- **Walk mode** and **Walk speed:** on the keyboard the movement keys push the stick all the way, so Link runs. While the
  Walk key is held (<kbd>Caps Lock</kbd> unless you change it on the Controls page), they tilt it only Walk speed of the
  way, and he walks; with Walk mode on Toggle, one press walks and the next runs again. 15% is his slowest walk, 45% to
  50% his fastest; from 55% up he jogs, then runs. Controllers, the mouse and the camera keys are not changed. Caps Lock
  still switches capitals on and off in other programs at each press: bind another key if that bothers you.
  Walk mode is separate from the Controls page's Toggle column, which holds Switch buttons such as ZL, not Walk.
  They can be used together without toggling twice. In Toggle mode, walking stays selected across Insert and focus
  changes, but a Walk key held when play resumes must be released before a fresh press changes that selection.

### Controls

Use a controller, or the keyboard and mouse. On the keyboard, <kbd>W</kbd> <kbd>A</kbd> <kbd>S</kbd> <kbd>D</kbd> move
(hold <kbd>Caps Lock</kbd> to walk) and the arrow keys (or <kbd>I</kbd> <kbd>J</kbd> <kbd>K</kbd> <kbd>L</kbd>) turn the
camera.
With the default keyboard bindings, press <kbd>E</kbd> to select **New Game** on the title screen.

**Mouse in the game's menus (Windows):** the title screen and its file list, the pause menu, the Key Items, Threads of
Time and Map pages, the options and the save dialogs free the mouse while they are shown. Point at an entry to select
it, left-click to choose it (A) and right-click to go back (B); the tabs, the L and R signs and the Back prompt can be
clicked too. The wheel turns the pause menu's pages and scrolls lists; on the map it changes floors and zooms. Drag
with the left button to pan the map and the tapestry or turn a stone on Key Items. The cursor stays hidden while you
play on a controller or the keyboard, until the mouse moves.

**Rumble:** a Nintendo Switch Pro Controller or Joy-Con plays the game's HD rumble as the Switch does, each side and
both frequency bands as the game asks, with SDL3 (the Windows game brings it; on Linux the system's SDL3, where it has
one). Other controllers, Switch 2 controllers, a pair of Joy-Cons used as one controller, and SDL2's own library get
the game's rumble on their two motors. The menu's Rumble strength scales it too. `HOS_HD_RUMBLE=0` sends the Switch
controllers the motor rumble too. On Linux, SDL talks to the Switch controller itself through its hidraw device, which
needs read and write access for your user; Steam's udev rules (the `steam-devices` package on most distributions) give
it. Without them the kernel's hid-nintendo driver runs the controller, and it gets the motor rumble. HD rumble goes to
anything SDL reports as a Switch Pro Controller or Joy-Con, and that includes third-party controllers in Switch mode
(8BitDo, GameSir and others; SDL gives them Nintendo's ids). Some of them may turn it into weak rumble or none: set
`HOS_HD_RUMBLE=0` to give them the plain motor rumble.

**NFC tags:** where the game asks for a figure's NFC tag, the Insert menu's **NFC** tab gives it one. The tags are files
in a folder of yours, dumps of tags you own: `.bin` (an NTAG215's 540 bytes; 532 and 572 byte ones too) or a Flipper
Zero's `.nfc`. The folder is `%LOCALAPPDATA%\Longshot\nfc` on Windows and `~/.local/share/longshot/nfc` on Linux
(`LONGSHOT_NFC_DIR` names another); the Longshot Launcher's NFC page adds and removes them, or copy them in yourself.
Choose one in the **NFC tag** row and press **Tap tag** while the game waits for one (or before: it stays at the reader
until the game reads it). With a Nintendo Switch 2 Pro Controller plugged in over USB (Windows), a real tag held on the
controller is read too, and **Scan tag on controller** saves one into the folder. Only a tag's ID and its figure's model
are read from a file; what a game writes to a tag is kept beside your saves instead (`user/nfc`), and no tag, real or
file, is ever written. [docs/nfc.md](docs/nfc.md) has the details.

**The Nintendo Switch 2 Pro Controller over USB (Windows):** Windows has no driver a game can use for it, so the game
reads it itself: plugged in with its cable, it is a controller with its motion sensor and rumble like any other, with no
bridge program. Pick **Switch 2 Pro** as the Layout preset (Controls page) for its own buttons and the Switch's icons;
its Home button opens this menu (**Controller menu button**, which can also be GL or GR, or none). Only one program can
have the controller: close Steam (it takes it for Steam Input) and any bridge such as Switch2Connect first, or the log
says another program has it. A controller that stops taking rumble (it happens; the log says so) plays its own buzz and
click instead until it is unplugged and plugged in again. Not over Bluetooth, where the controller speaks a pairing of
Nintendo's own. `HOS_SWITCH2=0` leaves the controller alone.

To change which gamepad button or key presses a Switch button, or which keys move, walk and turn the camera, open the
Controls tab's Controls page. Choose a row and a column (Gamepad or Keyboard), press <kbd>Enter</kbd> (or the bottom
face button), then the new button or key. <kbd>Esc</kbd> cancels, <kbd>Delete</kbd> clears it, and Reset to defaults
puts them all back. A gamepad button taken from another Switch button swaps with it. The page saves them in
`longshot_options.ini`.

**Toggle:** on the Controls page, set a Switch button's Toggle column to On to hold it with one press and release it
with the next. Both gamepad inputs, bound keys and mouse buttons share the latch. Every Toggle starts Off.
Plus, Minus, stick directions, Walk and ocarina note rows have no Toggle. Opening Insert, pressing Plus or Minus,
losing focus, rebinding or Reset to defaults releases latched buttons. With Controller in background On, a new
press can latch again behind another window. On Linux, any menu screen receiving pane focus also clears the latch,
not only the pause page. Insert always releases it. There is no separate on-screen latch indicator.

**Ocarina notes:** the page gains five rows, `Ocarina note (L)`, `(R)`, `(A)`, `(Y)`, `(X)`, unset by default: the game's own
buttons play until you bind a note to a key or gamepad input of your own, and a bound note then answers only that input while
the game waits for it. A dialogue or cutscene that takes the buttons ends the wait, and a song lesson keeps the game's own too.

<details>
<summary>Writing the controls in longshot_options.ini yourself</summary>

```ini
pad_a=0
key_a=69,13
toggle_a=0
```

`pad_<button>` is a gamepad input by position: 0 bottom face button, 1 right, 2 left, 3 top, 4 Select or Back, 5 Home,
6 Start, 7 and 8 the stick presses, 9 and 10 the shoulder buttons, 11 to 14 the D-pad (up, down, left, right), 15 and
16 the triggers, -1 none. `key_<button>` is one or two
[Windows key codes](https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes) in decimal (69 is
<kbd>E</kbd>, 13 <kbd>Enter</kbd>), 0 none; the mouse's buttons are 1 (left), 2 (right), 4 (middle), 5 and 6 (side
buttons), and count while the mouse is captured; the side buttons also in the game's own menus, where the other three
click. The buttons are `a` `b` `x` `y` `l` `r` `zl` `zr` `plus` `minus` `ls` `rs` `left` `up` `right` `down`, for keys
only the left stick's and the camera's directions and walking: `move_up` `move_down` `move_left` `move_right` `cam_up`
`cam_down` `cam_left` `cam_right` `walk`, and the ocarina's five notes,
which also take a `pad_` line: `note_l` `note_r` `note_a` `note_y` `note_x`. A button the file leaves out keeps
its default. `toggle_<button>=0|1` sets that button's Toggle (0 Off, the default; 1 On). It accepts the Switch
buttons above except `plus` and `minus`; stick directions, Walk and note rows do not accept it. Other values mean Off.
The file saves enabled flags, not held state; a restart starts with no button latched.

</details>

## Mods

Put mods in a `mods` folder in the game folder, beside `longshot.exe` (or point `HOS_MODS` at another folder), in the
layout emulators use: `mods/<name>/romfs/...` holds files that replace or add to the game's own. Deeper layouts such as
`mods/contents/<title id>/<name>/romfs` work too. A folder whose name starts with `-` or `_` is off. When two mods
have the same file, the one whose whole path relative to `mods` sorts last (ignoring case) wins. Use `01-...`, `02-...`
at the same level to set the order. Stop the game before changing mods, then restart.

One of the game's texts names the console: the hint for playing the ocarina by microphone says to use its built-in one.
The build rewords it for a PC ("your microphone") in your own text files, in each of the game's languages, into the game
folder's `texts` folder, and the game reads those instead. A mod's own text file wins, and `HOS_TEXTS=0` in
`longshot_env.txt` shows the game's own.

Only RomFS mods are loaded. `exefs` patches (`.pchtxt`, `.ips`) are left out: Longshot compiles the game's code ahead
of time, so patching its instructions would need a rebuild. The log (`longshot_log.txt`) names each mod and the files
read from it.

A mod file larger than the game's own does not load unless the resource size table allows for it:
`System/Resource/ResourceSizeTable.Product.111.Nin_Ounce_NVN2.rsizetable.zs` gives each file the memory the game sets
aside for it (by the CRC32 of its path without `.zs`; for a texture, its decompressed size + 280). Past that, the game
goes on without the mod's file, or a loading screen waits for good. A mod that makes files bigger (HD
textures, longer parameter files) ships the table with its entries grown. Since only one mod's table is used, mods that
each grow files need one table with all their entries, in the mod that sorts last. Longshot merges the button prompts'
entries into that table (each file gets the larger of the two), so your controller's icons stay in texts; the log says
`(with the button prompts' sizes merged in)` after the table's line. A table it cannot read that way (not the game's
format, or a zstd frame that does not give its size) is used as it is, with the Switch's icons.
If a mod includes tables for several game versions, the prompts use the table name from the game's own RomFS,
not the first table found in the mod. Other versions' tables do not change the prompts' font or its size entries.

Players: the [mods guide](docs/using-mods.md) covers installing, turning off and checking mods. For authors: the
[existing file contract and game-free example](docs/mods.md) cover discovery, conflicting files,
disabling and what this loader does not provide. The example contains only invented text, not a seed or game data.

## Build from source

For developers, and for players who prefer the command line. The launcher and the builders run the same script,
`nx2/make_game.py`. [CONTRIBUTING.md](CONTRIBUTING.md) has more.

<details>
<summary>Windows, from the command line</summary>

```powershell
python -I nx2/make_game.py --nup "C:\path\your-game.nup" --out "C:\Games\Longshot" --language en-US
python -I nx2/make_game.py --romfs "C:\path\romfs" --exefs "C:\path\exefs" --out "C:\Games\Longshot" --language en-US
```

Add `--check` with your `.nup` or `--exefs` to only check the game's version. It builds nothing.
Add `--log "C:\path\build.log"` to also save the build's output to a file (UTF-8, replaced if it exists); it works
with `--check` and `--update` too, and on Linux.
`LONGSHOT_JOBS=<n>` (or `--jobs <n>`) limits how many files compile at once.
`LONGSHOT_MEM_LIMIT_GB=<n>` lets the compilers take that many GB together, whatever is free now; up to 4 always run.

You need:

- Python 3 from python.org (the Microsoft Store's Python installs packages where `python -I` does not look), with
  `pip install -r requirements.txt` (core tools plus the button-prompt packages).
- [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (the `ucrt-x86_64` build), unpacked as `llvm-mingw` in the
  clone or with `LONGSHOT_MINGW` set to its folder. Without it the build uses the clang from
  [emsdk](https://github.com/emscripten-core/emsdk) at `C:\emsdk` with Visual Studio's C++ tools and the Windows SDK
  (`LONGSHOT_MSVC=1` forces that route).
- What the builder's `tools` folder carries and a clone does not. From an unpacked release zip, run
  `LongshotBuilder.exe --cli --help` once (it installs the tools, then runs `make_game.py --help`): the folder is
  `%LOCALAPPDATA%\LongshotBuilder\versions\<version>\LongshotBuilder\tools`. The builder sets the variables itself;
  from the command line, set them before you run `make_game.py`:
  - `tools\cpm`: the Vulkan headers and FFmpeg. Set `RX_CPM` to this folder. Without it (and without system
    Vulkan / SPIR-V / VMA headers or `LONGSHOT_*_HEADERS`), the standalone build stops instead of shipping a
    game with no renderer.
  - `tools\third_party`: the prebuilt files a clone lacks (`a64emit`, `lld-link`, `zbic` and SDL2). Copy its `.dll`,
    `.exe`, `.lib` and `.sha1` files to the same places under `third_party\`.
  - `tools\work`: copy it to `work\`. It holds the hot-function list, the link order, the profile and the icon;
    without them the game builds but runs slower.
- .NET 8, only if you want to change the builder's `--cli` program itself (`builder/LongshotBuilder`).

</details>

<details>
<summary>Linux, the AppImage from the command line</summary>

```sh
./Longshot-<version>-x86_64.AppImage --cli --nup ~/your-game.nup --out ~/Games/Longshot   # make_game.py's options
./Longshot-<version>-x86_64.AppImage --play ~/Games/Longshot   # the game in that folder
./Longshot-<version>-x86_64.AppImage --cli --update ~/Games/Longshot # that folder built again with this release
```

`LONGSHOT_JOBS=<n>` (or `--jobs <n>` with `--cli`) limits how many files compile at once.
`LONGSHOT_MEM_LIMIT_GB=<n>` lets the compilers take that many GB together, whatever is free now. Linux stops a compiler
that does not fit; it is then compiled again alone at the end.

In a game folder, `longshot` is the launcher and the game itself is `bin/longshot`. Started with no arguments, the
AppImage only says to get the Longshot Launcher.

</details>

<details>
<summary>Linux, without the AppImage</summary>

You need `clang` and `lld` (LLVM 23 or newer reads the play profile; an older one builds without it), Python 3 with
`pip install -r requirements.txt`, FFmpeg's development files (with dav1d, for the movies) and SDL3 (SDL2 works too).
Then run `python -I nx2/make_game.py ...` as on Windows. Vulkan / SPIR-V / VMA headers come from the distro, from
`LONGSHOT_*_HEADERS`, or from a CPM cache via `RX_CPM`.

`scripts/linux/deps.sh tools` fetches and builds all of these into `build-linux/deps` (the AppImage's own tools,
`tools.lock`'s Linux lines), and `scripts/linux/appimage.sh` makes the AppImage. A folder built from a source checkout has no launcher: `longshot` there is the game.

</details>

<details>
<summary>The Windows game, built on Linux</summary>

Unpack llvm-mingw's Linux build (`ucrt-ubuntu-22.04-x86_64`), then run `make_game.py` with `RX_TARGET=windows` and
`LONGSHOT_MINGW` set to its folder. The objects go to `~/.cache/longshot-builder-windows`, apart from the Linux game's.
The game folder runs under Proton or on Windows.

</details>

### Running the game from the command line

The built game is `bin\longshot.exe` on Windows and `bin/longshot` in a packaged Linux game folder. A build from a
source checkout has `longshot.exe` or `longshot` in its build folder, with no launcher beside it. The `longshot.exe`
or `longshot` launcher in the game folder does not pass these arguments to the game, so start the game directly to
use them.

| Argument | What it does |
|---|---|
| `image.bin` | The game image to run. Without it the game uses the `image.bin`, `romfs` and `user` in its folder |
| `--romfs <folder>` | The extracted RomFS folder |
| `--user <folder>` | Where the game keeps per-user data: saves, caches and temp files (and `mouse_sens.txt` when `HOS_MOUSE_SENS` is set; the menu keeps the sensitivity otherwise). Default: `user` in the game folder on Windows, `~/.local/share/longshot` on Linux; with an explicit image path, `user` in the current folder |
| `--trace` | Log every Horizon OS call (the same as `HOS_TRACE=1`) |
| `--prepare-shaders` | Prepare the shaders as the first start does, then exit without starting the game (no build step runs it) |
| `--compile-shaders all` or `most` | What a preparation of this run builds, over `compile_shaders` in `longshot_options.ini` (the same as `RX_VK_COMPILE_SHADERS`): `all` the predicted pipelines too, `most` without them. See [Compile shaders](#compile-shaders) |

For example, keep saves somewhere else:

```powershell
cd "C:\Games\Longshot"
.\bin\longshot.exe --user "D:\Longshot data"
```

```sh
cd ~/Games/Longshot
./bin/longshot --user "$HOME/longshot-data"
```

`longshot_env.txt` and the other test switches are documented below.

<details>
<summary>AMD FSR 3.1 (every build)</summary>

**AMD FSR 3.1** in the Graphics tab's **Upscaler** row (`dlss=3` in `longshot_options.ini`; `RX_VK_FSR=1` for one run,
`RX_VK_FSR=0` never) upscales in place of the game's own network, and with **Frame generation** at 2x its frame
interpolation makes the frames between the game's at every Frame generation setting, Auto and 2x to 8x, wherever NVIDIA DLSS
Frame Generation does not (Frame generation by at FSR 3.1 FG, or at DLSS FG; `RX_VK_FSR_FG=0` never, `=1` over the row). The game draws its HUD
into the same picture and has no HUD-less colour, so AMD's own passes would move the HUD with the scene: here a pixel the
same in both frames while the scene behind it moves is kept where it is (`RX_VK_FSR_FG_HUD=0`: AMD's as they were). Both are AMD's FidelityFX
SDK 1.1.4 shaders (`third_party/fidelityfx-sdk`, MIT), compiled into the renderer: no AMD runtime or DLL is needed, and
the row offers FSR 3.1 on any Vulkan 1.3 GPU with push descriptors, extended storage image formats and storage writes
without a format (`vk: fsr3: ... can run on this GPU` in the log). It upscales on the same inputs as DLSS and FSR 4: the
scene's colour, its depth, motion from the camera with the skinned characters' own drawn over it (Link, his shield and
the people around him keep their outline while they move), and the same camera jitter (`RX_VK_FSR_JITTER=0` turns it
off). Things that move by themselves and are not skinned (Navi's body, props) can still trail. `RX_VK_FSR3_CHAIN=0` gives
it the camera's motion alone, as before.

</details>

<details>
<summary>NVIDIA DLSS (optional, NVIDIA's SDK under NVIDIA's license)</summary>

A game built with NVIDIA's DLSS SDK offers **NVIDIA DLSS** and **NVIDIA DLAA** in the Graphics tab's **Upscaler** row on
NVIDIA RTX cards (`dlss=` in `longshot_options.ini`, `RX_VK_DLSS=0/1/2` for one run): DLSS Super Resolution in place of
the game's own upscaler, with camera jitter (**NVIDIA Reflex** needs no SDK and is in
every build). NVIDIA's files are NVIDIA's, under its own license, the
[NVIDIA RTX SDKs license](https://github.com/NVIDIA/DLSS/blob/9981c6350dc58c0ce21f024fd65e31e851c31689/LICENSE.txt).
Longshot never includes or redistributes them: each build downloads them for itself, and only after you accept that
license. Never share a game folder built with them, and do not post builds made with them.

- **The Longshot Launcher, Windows and Linux:** turn on **DLSS** in the build's settings. It is off by default and shows
  NVIDIA's license when you turn it on. The build downloads the SDK into the launcher's folder and puts DLSS's runtime
  (`nvngx_dlss.dll`, `libnvidia-ngx-dlss.so.<version>` on Linux) and DLSS Frame Generation's (`nvngx_dlssg.dll`,
  `libnvidia-ngx-dlssg.so.<version>`) in the game folder's `ngx` folder. An update of that folder keeps DLSS.
- **A builder from the command line:** `--cli` with make_game.py's `--fetch-ngx-sdk --accept-nvidia-license`
  (below: what they do). The SDK goes to `work/ngx-sdk` in the Longshot folder, or to `LONGSHOT_NGX_SDK` when that is set
  (the Builder drops a variable set on the PC, so a launcher has none).
- **Windows, the command line:** `python -I scripts/ngx_sdk.py --platform windows <sdk folder>` asks you to accept
  NVIDIA's license (or pass `--accept-nvidia-license`) and fetches the headers, `nvsdk_ngx_s.lib`, `nvngx_dlss.dll` and
  `nvngx_dlssg.dll`, each checked against its SHA-256. Then build with `LONGSHOT_NGX_SDK=<sdk folder>`: `make_game.py`
  copies both runtimes into `<game folder>\ngx`. NGX is also pointed at `%LOCALAPPDATA%\Longshot\ngx`, and at
  `RX_VK_DLSS_DIR` when it is set, and uses the newest of each it finds across them (to try a given one, make sure no
  other folder has a newer one). Without `nvngx_dlssg.dll` there, frame generation takes the driver's own copy, which
  may be older: one that makes 1 frame at most, or refuses every set. Never copy NGX's core, `_nvngx.dll`, beside
  the game: it comes with NVIDIA's driver.
- **Linux, a build from source:** `scripts/linux/ngx-sdk.sh <sdk folder>` (it runs `scripts/ngx_sdk.py --platform
  linux`) asks you to accept NVIDIA's license and fetches `libnvsdk_ngx.a` for the build,
  `libnvidia-ngx-dlss.so.<version>`, DLSS's runtime, and `libnvidia-ngx-dlssg.so.<version>`, frame generation's. Then
  run the build with `LONGSHOT_NGX_SDK=<sdk folder>` set: `make_game.py` puts both runtimes in the game folder's `ngx`
  folder. NGX's core comes with NVIDIA's Linux driver.
- **Linux, a game folder with other than plain English letters in its path:** NVIDIA's NGX on Linux loads DLSS's
  runtimes only from a folder whose whole path is ASCII. A game in `~/Games/Longshot` needs nothing more. A game in a
  folder such as `~/Games/Longshot-Café` (the `é` is not ASCII) cannot use the runtimes in its own `ngx` folder, and the
  log and the Insert menu say so: copy `libnvidia-ngx-dlss.so.*` and `libnvidia-ngx-dlssg.so.*` from there into
  `$XDG_DATA_HOME/longshot/ngx`
  (`~/.local/share/longshot/ngx`), which the game also looks in, or into any other folder with an ASCII path and name
  that folder in `RX_VK_DLSS_DIR`.

The **DLSS model** row chooses DLSS's network, by NVIDIA's preset letter, for every mode: **Transformer (K)**, the
transformer model and the one tested most here (the default; on a card with less than 7 GB of video memory **CNN (E)**
in its place, NVIDIA's CNN model, for less video memory and GPU time); **Transformer 2 (M)**, NVIDIA's second-generation
transformer as it picks it for Performance; **Transformer 2 (L)**, the same generation as tuned for Ultra Performance;
**NVIDIA default**, which lets DLSS's runtime pick for each mode, so it can change with a runtime update. The runtime
the SDK comes with (310.9.16) has all of them. A change applies at once, without a restart (`dlss_model=` in
`longshot_options.ini`, 0 to 3 in that order). `RX_VK_DLSS_PRESET=<letter>` (or `0` for NVIDIA's default) chooses
the preset for one run and wins over the row; the log's `vk: NVIDIA DLSS: ... preset <letter>` line says which one was
asked for, and with `RX_VK_DLSS_LOG=1` NGX's own `Using App hint Preset <letter>` lines say which it took.

**8 GB cards:** DLAA draws the 3D at full size, and DLSS and DLAA add images of their own beside the game's. In busy
areas that can pass what the driver allows the game, and the game then stutters: Quality or Balanced is safer there.
The log's `vk: status:` line every 600 frames shows the video memory in use of the allowance, with DLSS's details
(its images' size, the jitter and the actor motion) appended while it draws, and a `vk: over the driver's
video-memory allowance` line says when the game passed it.

The game's log says whether DLSS started, and why not when it did not (`vk: NVIDIA DLSS:` lines in `longshot_log.txt`).
NGX starts on demand: when the Insert menu first opens (to know whether the Upscaler can offer DLSS) or at the first
gameplay upscale with DLSS chosen, so on the title screen the log has no NGX lines yet.
With `RX_VK_DLSS_LOG=1` NGX's own lines go there too (`ngx:`). Some of them look like errors but are not:
`failed to load NGXCore: 126` (twice) is NVIDIA's library looking for `_nvngx.dll` and `nvngx.dll` beside the game, which
are not there on purpose, before it loads the driver's; `Feature ... failed to load from cache` is NGX finding no model
of its own downloads (`C:\ProgramData\NVIDIA\NGX\models`) before it uses the game folder's `nvngx_dlss.dll`.

</details>

<details>
<summary>AMD FSR 4 (optional, AMD's DLLs not included)</summary>

A game built with FSR 4 offers **AMD FSR 4** and **AMD FSR 4 Native AA** in the Graphics tab's **Upscaler** row: AMD's
machine-learned temporal upscaler in place of the game's own, on the same inputs as DLSS (the scene's colour and depth, the
camera's and the characters' motion, and the camera jitter). Native AA draws the 3D at full size and smooths its edges.
With HDR on **Native**, it takes the game's HDR colour as linear light, so highlights above the paper white are kept
(`RX_VK_FSR4_HDR=0`: cut at the paper white, as before).
Unlike FSR 3.1 (above), it runs AMD's own DLLs, so it needs them beside the game. Choose it in the menu (`dlss=4` or `5`
in `longshot_options.ini`, `RX_VK_FSR4=1` or `2` for one run, `RX_VK_FSR4=0` to leave it out).

- **Windows, the Longshot Launcher:** turn on **FSR 4** in the build's settings. It shows AMD's license first and stays
  on only if you accept; the build then fetches AMD's DLLs for you (below), into the launcher's folder and the game
  folder.
- **The command line:** `make_game.py --fsr` (or `LONGSHOT_FSR=1` for `nx2/build.py`). Rebuilding a folder built with
  it (`--update`) keeps it; `--no-fsr` takes it out.
- **AMD's files:** Longshot includes only AMD's MIT-licensed API headers (`third_party/fidelityfx`). Put
  `amd_fidelityfx_loader_vk.dll` and `amd_fidelityfx_upscaler_vk.dll` from the
  [FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) 2.4 release (`Kits/FidelityFX/signedbin`)
  beside the game's exe (`bin\longshot.exe`) or in the game folder. (`RX_VK_FSR_DIR` names another folder, for testing:
  set it in the environment or a `HOS_ENV_FILE` file; `longshot_env.txt` takes no paths.) Without them the Upscaler row
  says so and offers the rest. On Linux, a Vulkan FSR 4 library (`amd_fidelityfx_upscaler_vk.so`, such as fsr4-vk's)
  takes their place: AMD publishes none for Linux, so you put it beside the game yourself. A Linux build with FSR 4
  (`--fsr`) compiles it into the Vulkan back end, which every build compiles.
- **Or fetched for you (Windows):** `make_game.py --fsr --fetch-fsr-runtime --accept-amd-license` downloads those two
  DLLs and AMD's license from AMD's FidelityFX SDK repository at the v2.4.0 tag, checks each against its SHA-256, and
  puts them in the game folder (`LONGSHOT_FFX_SDK=<folder>` keeps them between builds, default `workfx-sdk`).
  `python -I scripts/ffx_sdk.py --accept-amd-license --game <game folder>` does the same for a game built already.
  Without `--accept-amd-license` nothing is downloaded (the script asks when run by hand), as with NVIDIA's license for
  DLSS. AMD DLLs already in the game folder that are not v2.4.0's (a newer SDK put there by hand) are left alone, and
  the build says so. They are AMD's, under
  [AMD's FSR SDK license](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/7539ae8b6568ad29ed78a753404351daf3b5cca7/Kits/FidelityFX/docs/license.md)
  (`LICENSE-AMD-FSR-SDK.md` beside them). On Linux the flag does nothing but say so.

The log's `vk: AMD FSR 4:` lines say which of AMD's upscalers it found for the card and which it uses (the newest FSR 4;
never an older FSR in its place), and why not when it did not start. Tested on an RX 6700 XT (RDNA 2).

</details>

<details>
<summary>Intel XeSS (optional, Windows, Intel's DLL not included)</summary>

Every Windows build offers **Intel XeSS** and **Intel XeSS Native AA** in the Graphics tab's **Upscaler** row once Intel's
`libxess.dll` is beside the game: XeSS Super Resolution, Intel's machine-learned temporal upscaler, through its Vulkan
API, in place of the game's own, on the same inputs as DLSS and FSR 4 (the scene's colour and depth, the camera's and the
characters' motion, and the camera jitter). Native AA draws the 3D at full size and smooths its edges. It runs on Intel
Arc and Iris Xe GPUs, and on other GPUs whose Vulkan driver has what XeSS asks for (storage image writes without a format,
`VK_EXT_mutable_descriptor_type`). With HDR on **Native**, it takes the game's HDR colour as linear light, with XeSS's own
exposure (`RX_VK_XESS_HDR=0`: cut at the paper white, as with the other upscalers before).
Choose it in the menu (`dlss=7` or `8` in `longshot_options.ini`, `RX_VK_XESS=1` or `2` for one run, `RX_VK_XESS=0` to
leave it out: then the library is not even loaded). The Auto (this PC) preset never chooses it; it says in the log when
the DLL is there.

- **Intel's file:** Longshot includes only Intel's MIT-licensed XeSS API headers (`third_party/xess`). `libxess.dll` is
  Intel's, under the Intel Simplified Software License, and Longshot does not ship it. Take it from the
  `bin` folder of a recent [Intel XeSS SDK](https://github.com/intel/xess/releases) release (one whose `inc` folder has
  `xess_vk.h`: the Vulkan path), or from a game that ships it, and put it beside the game's exe (`bin\longshot.exe`) or in the game folder.
  (`RX_VK_XESS_DIR` names another folder, for testing: set it in the environment or a `HOS_ENV_FILE` file;
  `longshot_env.txt` takes no paths.) Without it the Upscaler row does not offer XeSS and its hint says why.
- **Or fetched for you:** RecompLauncher's **Intel XeSS** tick (Windows; off until you accept Intel's license, which it
  shows), or `make_game.py --fetch-xess-runtime --accept-intel-license`, downloads `libxess.dll` 2.0.2.68 (78 MB, signed
  by Intel) with Intel's license and third-party notices from Intel's XeSS SDK repository, at the commit
  `third_party/xess`'s headers come from, checks each against its SHA-256, and puts them in the game folder
  (`LONGSHOT_XESS_SDK=<folder>` keeps them between builds, default `work/xess-sdk`).
  `python -I scripts/xess_sdk.py --accept-intel-license --game <game folder>` does the same for a game built already.
  Without `--accept-intel-license` nothing is downloaded (the script asks when run by hand), as with AMD's and NVIDIA's
  licenses. A `libxess.dll` already in the game folder that is not this one (a newer SDK's, put there by hand) is left
  alone, and the build says so. It is Intel's, under the
  [Intel Simplified Software License](https://github.com/intel/xess/blob/de0fb9c1c510661c571164e1418ceca8101dab69/LICENSE.txt)
  (`LICENSE-INTEL-XESS-SDK.txt` beside it). On Linux the flag does nothing but say so.
- **Windows only.** Intel makes no XeSS library for Linux, so a Linux build has no XeSS, and the row says so. XeSS Frame
  Generation and Intel's latency reduction (XeLL) are Direct3D 12 only and are not in Longshot.

The Render resolution row's stops run under the XeSS mode whose range holds them (since XeSS 1.3 each mode takes any size
from its own ratio up to the output: Ultra Quality Plus 1.3x, Ultra Quality 1.5x, Quality 1.7x, Balanced 2x, Performance
2.3x, Ultra Performance 3x), so the game's dynamic resolution at Auto runs under one mode too. The log's
`vk: Intel XeSS:` lines say which DLL it loaded and its version, the extensions and features it asked of the Vulkan
driver, the mode it was made with, and why not when it did not start. Not yet run in the game: reports from Intel GPUs
are welcome.

</details>

<details>
<summary>Switches for testing (longshot_env.txt)</summary>

Switches to test (`RX_VK_NOOPT=1` and the others) go in `longshot_env.txt` in the game folder, one `NAME=VALUE` a
line, on Windows and Linux. The running game reads it, not the builder. A test run can name another file with
`HOS_ENV_FILE=<path>`; the game then reads that one instead and leaves `longshot_env.txt` alone.
`longshot_env.txt` sets only the game's own switches (names that start with `RX_`, `HOS_`, `LONGSHOT_`, `EDEN_`,
`NVN_` or `SDL_`, but not SDL's library names such as `SDL_VULKAN_LIBRARY`), and no value that is a path except
`RX_VK_DLSS_DIR`'s (a folder on this PC, not a network share), and no value at all for the switches that name a file
or folder (`HOS_AUDIO_DUMP`, `RX_VK_SHOTS` and the like) or press buttons on a timer (`HOS_INPUT_SCRIPT`,
`RX_VK_MENU_SCRIPT`); other lines are left out, and the log says so. Switches that need a path, or other variables, go
in a file named by `HOS_ENV_FILE`.
A switch set in the environment the game is started with (a shell's `HOS_FULLSCREEN=0 ./longshot`, a launcher's own)
goes over the same name in the file, and the log says so; within the file, a later line replaces an earlier one. A
switch that stands in for a menu row (`RX_VK_PACE`, `RX_VK_UPSCALE`, `RX_VK_FRAMEGEN_MULT`, `HOS_REALTIME_STEP`,
`HOS_DYNRES`, `HOS_OUTPUT_RES`) applies to that run only: `longshot_options.ini` keeps the value chosen in the menu.

To enable the debug tools, turn **Debug tools** on in the menu's Debug tab (or add `HOS_OOT_CHEATS=1` to
`longshot_env.txt` in the game folder). The tools then show below it on the same tab. **Damage taken** is in the
**Player** group beside **Heart protection**: Half, Normal (the default), Double or Quadruple (`damage_taken=0/1/2/3`
in `longshot_options.ini`). It applies only while Debug tools is on: off, Link takes Normal damage, and the saved
choice returns when the tools do. Heart protection takes priority while enabled. Half scales the full hit before life
is clamped to zero: with two quarter hearts left, a four-quarter hit still kills. The tab's Developer section, always
shown, has the Descriptor reuse, Sampler cache, Robust buffers and ASTC as RGBA8 rows, and on Intel graphics on
Windows the Intel shader compiles row.

A save whose minimap stopped showing after a Teleport on an earlier build (the area left had hidden it, and the flag
stayed set in the save) is mended by the Teleport itself: teleport to an outdoor area, wait until you can move, then
save from the game's System menu. The minimap then stays after a restart.

`RX_VK_UI_GAME_FONT=0` draws the menu with the included font (Libertinus) even when the game's files have the game's own
menu font. `RX_VK_UI_PROMPTS=pad` (or `keys`) shows the controller's (or the keyboard's) prompts whatever was used last,
for screenshots.

For a first run on a 4 GB card such as a GTX 1650, `RX_VK_LOW_SPEC=1` starts with the Minimum (4GB) preset, with
frame generation and upscaling off. It applies only when there is no `longshot_options.ini` yet; after that, change
the settings in the in-game menu.

On **Minimum (4GB)**, when the driver reports a nonzero VRAM budget of at most 4.5 GiB, or the GPU is integrated
and the PC has 16 GB of RAM or less (its "VRAM" is that RAM, whatever budget the driver reports), sampled ASTC textures
use a 1024 dimension limit and an 8 MiB decoded-size threshold for leaving out their top mip levels. This lowers texture
detail to reduce the working set. The same holds on Auto (this PC) at Minimum, and on Custom after either; Custom with
no preset behind it (chosen on the Quality preset row, or an options file from before `memory_budget` existed) goes by
what Auto picks for this PC, which is Minimum on every integrated GPU and on cards of 4.5 GB or less. Applying Low or a
higher preset, or `RX_VK_LOW_VRAM_MIPS=0`, keeps full-size textures there. Other presets and larger or unknown budgets
keep the 2048 / 64 MiB defaults;
uncompressed render targets, depth images and the separate `RX_VK_TEX_MAX_MB` allocation limit keep their handling.
The driver budget is sampled at the first texture creation and kept for the session. It is a heuristic, not a physical
VRAM measurement: other applications can lower the budget even on a larger card.

`RX_VK_LOW_VRAM_MIPS=0` (also `off`, `false` or `no`) disables the automatic rule. `RX_VK_TEX_MAX_DIM` overrides only
the dimension limit, so setting it to 2048 still leaves the automatic 8 MiB threshold active on Minimum with a small
budget. `RX_VK_TEX_SKIP_MB` overrides that threshold using its existing parsing. For a more aggressive experiment,
use `RX_VK_TEX_MAX_DIM=512 RX_VK_TEX_SKIP_MB=2`. Changes to these environment switches take effect after a restart.

`RX_VK_MOVIE_UPSCALE=linear` replaces the movies' neural upscale with the plain linear one. The default, `auto`, keeps
the networks and falls back only when a movie frame takes the GPU more than 40 ms. That is on NVIDIA cards: on other
cards, or where the GPU cannot multiply the networks' matrices itself, the movies always get the linear upscale. It does
not change the gameplay upscaler, and movies can look softer. `RX_VK_NETWORK_KEEP=all` takes precedence and keeps the
movie networks.

A movie whose pictures take longer to decode than the time between them is shown, rather than skipped until the sound
has moved on: on a slow CPU the opening used to drop most of its pictures. How fast the decoder is counts from its recent
pictures, so a slow first picture does not stop a decoder that keeps up from catching up later, and catching up takes at
most one picture's time in a frame. With a decoder slower than the movie, the picture can fall behind the sound.
`HOS_VIDEO_NO_CATCHUP=1` skips nothing. Each picture is waited for for about a tenth of a second
(`HOS_VIDEO_ACQUIRE_TRIES=1`: a count of sleeps instead of the clock). After a movie stops or is flushed, and nothing new
has been submitted, there is no wait. The first picture can still be black on some PCs; why is not known yet.

A PC with 4 logical processors or fewer decodes the movies on 2 threads rather than one per processor, so the decoder
leaves the game's own threads their time. A 4-thread laptop with Intel UHD Graphics showed the title and the opening
movie at about 35 fps of 60, with the game's thread waiting 14 to 24 ms on most frames; that report was not run again
with this change. `HOS_DAV1D_THREADS=<n>` sets the count on any PC.

On Windows, Intel's compiler (UHD and Arc) is entered by one thread. A fault inside it leaves that shader out instead
of closing the game, and the program is written to `longshot_driver_faults.txt` so that later starts on the same
driver leave it out too (delete the file to try again). This covers the game's shaders and the renderer's own (texture
decoding, upscaling, frame generation). A draw that only asks whether its pipeline is ready never waits behind another
thread's compile: while one is compiling, the answer is "not yet", even for a pipeline already in a cache, and the
draw is left out until a worker has made it (the log's `vk: asked cold: Intel's gate busy` lines count these). The
renderer's own pipelines made at their first use (the movie's upscale, the texture decoders, a masked clear, the
write-back and late-page fixes) do wait for a compile under way. Whether the driver keeps working after a caught fault
has not been tested. The first scene can still pause while shaders compile, especially when they were not prepared at
the build. If a build's preparation stopped ("the GPU driver stopped responding"), the game leaves a
`longshot_prepare_skipped` file in its `cache` folder and every start after that builds shaders only when first drawn.
Build again with **Prepare shaders now** ticked, or delete that file, to prepare them with this fix.
The Debug tab's **Intel shader compiles** row (Developer section, shown on Intel graphics on Windows) chooses: **One at
a time**, the safe choice on a CPU with 4 cores or fewer; **Side by side**, faster builds on a CPU with many cores, but
Intel's driver can crash the game while building them; or **Auto**, the default: side by side on the Arc B-series with
driver 101.9154 or newer (on a B580 a new area's first visit froze the picture for four to seven minutes one at a time,
and not at all side by side, with no fault in ten areas), one at a time on every other Intel GPU. It applies after a
restart. `RX_VK_NO_INTEL_SERIAL=1` is Side by side for a run and `=0` One at a time, whatever the row says; the Longshot
Launcher's build settings set `=1` for the build's shader preparation and the game when their **Intel shader compiles**
is on Side by side. NVIDIA, AMD, and Intel on other systems are unchanged.

`RX_VK_SKIP_CODE=<prefix,...>` leaves out the draws of every program with a stage whose native code key starts with one
of the prefixes (the keys the log prints after "its code:"). On a Steam Deck, `RX_VK_SKIP_CODE=2c56dc1006942f74`
avoids a GPU hang in Kokiri Forest and the Great Deku Tree: the GPU runs for seconds in the programs that share that
task shader (a culling pass) until the kernel resets it ([#817](https://github.com/NotRexed/Hookshot/issues/817)).
It is a workaround, not a fix: those are task + mesh programs, so the geometry they draw goes with them, and key models
are missing while it is set. Off unless set.

The switches for scripted test runs, screenshots, frame dumps and profiles are in the
[development guide](docs/development.md#switches-for-testing-and-debugging).

`RX_VK_WB_MAX_KB=<KB>` (off by default; 1024 is the tested value) is the size from which a written GPU binding may go
without most of its write-back. The game
writes tens of MB of culling lists and per-frame buffers through the GPU that the CPU never reads, and copying them back
sat on every submit's memory sync: in Hyrule Field on a 10 GB RTX 3080, about 66 MB and 4.4 ms a submit, holding the
scene at 45-48 fps. The CPU does read and write parts of some large ones, so a binding over the size is written back
whole for the first `RX_VK_WB_PROBATION` submissions that write it (default 8), and after that only the ranges the CPU
read or wrote meanwhile, and its first and last page when it shares them, are written back (`gpu/vk/wb_learn.h`). The
trade-off: a range the CPU first touches only after that (read more rarely than every 8 frames) gets the stale copy
once, and is written back from then on; the log names each one (`in a binding whose write-back was left out`).
Unset or `0`, every written binding is copied back whole. It is off by default because, measured so far (an RTX 3060 Ti
on Windows, an RX 580 on Linux with its mirror forced on), it copies back ~50 MB a frame less at the same frame rate:
results from laptops and other slow PCIe links are wanted. `RX_VK_LOG_WB_SKIPS=1` lists the bindings left out every
600 submits.

`RX_VK_PROGRAM_CACHE_ZSTD=1` keeps the translated shader programs in `longshot_programs.zst.bin`, each record
compressed with zstd and unpacked when it is read. The uncompressed cache is about 6.9 GB; in the reference cache
(46,587 programs) zstd's level 3 made it 1.83 GB (3.75x). It is a file of its own: an existing
`longshot_programs.bin` is left alone, and the first run translates the programs into the compressed cache.

</details>

## How it works

1. `nx2/lift.py` translates the game's ARM64 code to C.
2. `nx2/build.py` compiles that C together with `host/`, which plays the part of the console's system.
3. `host/nvn_native.c` records the game's NVN2 graphics calls, and `gpu/vk/` replays them on Vulkan.
4. Switch 2 Ampere shaders are lowered to Maxwell (`lower_to_maxwell.h`) and translated to SPIR-V by Eden's shader
   recompiler.

More detail is in the [development guide](docs/development.md) and the [design notes](docs/design.md).

## Want to help?

Testing on other graphics cards and reporting the result already helps a lot, and so does playing further into the
game than anyone has yet.

### Send your pipeline list

The first time the game meets a pipeline it builds it on the spot, which stutters. Releases build a list of known
pipelines before the title, and every player's play adds to it. To send yours:

1. In the game, open the menu (Insert or F1), go to the **Debug** tab and choose **Save my pipeline list** (under Developer). Or,
   from a clone, run `python -I nx2/pipeline_share.py <game folder>`.
2. It saves `longshot-pipelines-<date>.zip` in your Downloads folder, copies a filled-in report to the clipboard and
   opens the [Submit your pipelines](https://git.magicbean.dev/MagicBeanDev/Longshot/issues/14) issue in your browser.
3. Attach the zip to a comment there, paste the report and fill in where you played.

The zip holds only `longshot_pipeline_states.bin` (shader hashes, formats and draw state, and the hashes of the compute
programs your play ran, so that a release can keep those ready too: no game code, game data or personal info) and the
report (the version, OS, GPU and driver, output and render resolution, upscaler, frame
generation, HDR and the mods the game loaded). Nothing is uploaded for you. If the list is missing or damaged, it says
so and saves nothing. In a run without a window (`HOS_HEADLESS`) the menu saves the zip but opens nothing.

Bigger ways to help:

- **Linux testing.** The Linux build (`host/posix`) has been tested on several distributions and cards: reports from
  others still help.
- **Fixes in your own fork.** Fork the project, fix what you find and send the change back. Keep game files, keys and
  anything generated from your game out of it.

[CONTRIBUTING.md](CONTRIBUTING.md) has the setup, the test commands, the code conventions and what a good pull request
looks like.

## License

Longshot is free software under the [GNU General Public License, version 3 or later](LICENSE) (GPL-3.0-or-later).
It is a modified version of Hookshot, which NotRexed released under the same license; the changes made since the fork
are Longshot's.

    Hookshot: Copyright (C) 2026 NotRexed and the Hookshot contributors
    Longshot: Copyright (C) 2026 the Longshot contributors

It includes code from Eden and suyu, which are under the GPL too. Third-party parts under other licenses that
work with the GPL (Sirit, the Vulkan Memory Allocator, SDL, Opus, Dear ImGui, zstd, the Libertinus fonts, zbic and the
a64-kit tools) keep their own notices in their folders.

Longshot is an unofficial fan project, not affiliated with or endorsed by Nintendo. The Legend of Zelda and Ocarina of
Time are trademarks of Nintendo.

## Credits

Longshot is a fork of NotRexed's [Hookshot](https://github.com/NotRexed/Hookshot), and exists thanks to the work of many
people:

- **NotRexed**: [Hookshot](https://github.com/NotRexed/Hookshot), which Longshot is a fork of, BlueShell (Mario Kart World),
  Skybound (Tears of the Kingdom) and the a64-kit tools.
- **[Eden](https://git.eden-emu.dev/eden-emu/eden)** and **Eglantine**: the shader recompiler and Switch 2 shader work.
- **suyu**: the graphics decoders and the AArch64 translator.
- **[seedds/nxnative](https://github.com/seedds/nxnative)**: reference for how task and mesh shaders work.
- **[EvilBoyce / evilmfkr](https://github.com/evilmfkr/Hookshot)** (EvilOOT): the GPU culling that brings back Kokiri's foliage
  (trees, ferns, grass, falling leaves, fireflies), the ambient light probes, the pond's water, the game's own fog, the freeze
  after Load (Horizon's mutex handoff) and the exposure flicker fix.
- **[zortos293/Lichii](https://github.com/zortos293/Lichii)**: reference for Ampere shader lowering, and its compare-exchange retry fix.
- **[mstorsjo/llvm-mingw](https://github.com/mstorsjo/llvm-mingw)**: the bundled Windows compiler (no Visual Studio needed).
- **NVIDIA DLSS**: builds made with NVIDIA's DLSS SDK (optional, downloaded by each build, never bundled) use NVIDIA DLSS
  Super Resolution, NVIDIA's technology. NVIDIA and DLSS are trademarks of NVIDIA Corporation.
- **AMD FSR 3.1 and FSR 4**: every build has AMD FidelityFX Super Resolution 3.1, built from AMD's MIT-licensed FidelityFX
  SDK shaders; builds made with FSR 4 (optional; AMD's DLLs are never bundled) use AMD FidelityFX Super Resolution 4
  through AMD's MIT-licensed FSR API headers. Both are AMD's technology. AMD, Radeon and FidelityFX are trademarks of Advanced Micro
  Devices, Inc.
- **[kinnay/zbic](https://github.com/kinnay/zbic)**, **Opus** and **SDL**: decompression, audio and controllers.
- **[sostenesfreitas/ocarina-recomp](https://github.com/sostenesfreitas/ocarina-recomp)**: this README's layout, and its notes
  confirmed the mesh stage's control section.
- **[MythosOfs/Orchid](https://github.com/MythosOfs/Orchid)**: the fog and exposure workaround the game used before
  its fog and lighting were fixed.
- **Mesa (NAK)**: the reference for Ampere's instruction encoding.
- Everyone who sent fixes: SkippySteve (Linux and the AppImage, and many more), monsterlag, klimovich008, hckrman101 (NVIDIA
  DLSS, DLAA and Reflex, the exposure flicker and fog fixes, build speed, CI), madebyjordan (AMD FSR 4.1), N3oTraX (FSR 4 on
  Linux), shayderrr, gab9281, Wpaif, nicholasgower, ZurielAtr, Joey-Magnus, Zorkats,
  d3zessete, chrispository, vzeol, leonidasmark, Segmentree, ShyVortex, hiKareeem and i900gaming, among others.
