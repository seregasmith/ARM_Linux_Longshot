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
  <a href="#features">Features</a> |
  <a href="#install-and-play">Install and play</a> |
  <a href="#help">Help</a> |
  <a href="#contributing">Contributing</a> |
  <a href="#license">License + Credits</a>
</p>

<p align="center">This website is our Git hosting, similar to GitHub.</p>

Longshot is a static recompilation tool. It translates a supported console game's code to C and compiles it into a
native Windows or Linux program, with the graphics rebuilt on Vulkan and the console's system software reimplemented.
All of this happens **on your PC, from your own copy of the game**.

Longshot is a continuation of [Hookshot](https://github.com/NotRexed/Hookshot). The top maintainers have moved to this
project.

**[Join us on Discord](https://discord.gg/magicbean)!**

> [!IMPORTANT]
> This repository **does not contain the game, keys or an executable**, and it does not explain how to get them. You
> need your own copy. Everything the build creates (`longshot.exe`, the generated code, the shader caches and your
> saves) comes from your game and stays on your PC. Never share those files.

## Install and play

### [🪟 &nbsp;Download for Windows](https://longshot.magicbean.dev/download/launcher/windows)

### [🐧 &nbsp;Download for Linux (AppImage)](https://longshot.magicbean.dev/download/launcher/linux)

Windows without installing: [portable zip](https://longshot.magicbean.dev/download/launcher/windows-portable) · Not
sure? [longshot.magicbean.dev/download](https://longshot.magicbean.dev/download) picks for your computer.

Longshot doesn't come ready to play: the **Longshot Launcher** builds it on your PC, from your own copy of the game, and
keeps it up to date. Download the launcher, not the builder: the files on
[Longshot's releases](https://git.magicbean.dev/MagicBeanDev/Longshot/releases) are what the launcher installs for you.

**You need** your own copy of the game (program `0400486024DEC000`, update v131072, build `e0f3ccfa`), 64-bit Windows
10/11 or Linux, a Vulkan 1.3 graphics card with 4 GB of video memory (mesh shaders and 12 GB recommended), 16 GB of RAM
and about 60 GB free while building. The [system requirements](docs/user/requirements.md) have the details.

The player guide, also at [longshot.magicbean.dev](https://longshot.magicbean.dev/):

| Page | What it covers |
|---|---|
| [Getting started](docs/user/getting-started.md) | Download the launcher, build, play |
| [System requirements](docs/user/requirements.md) | The game version, the PC, drivers and memory |
| [Playing on the Steam Deck](docs/user/steam-deck-playbook.md) | The build, the settings and the switches that help on the Deck |
| [Settings](docs/user/graphics-and-controls.md) | The in-game menu: every row, and the controls |
| [Advanced setup](docs/user/advanced.md) | Build options, DLSS, FSR 4 and XeSS, `longshot_env.txt`, the command line |
| [Updates and saves](docs/user/updates-and-saves.md) | Updates, channels, where the files go, what Longshot connects to |
| [Moving your saves](docs/user/saves.md) | Saves from and to emulators, Hookshot and other builds |
| [Mods](docs/user/using-mods.md) | Installing, ordering and checking mods |

## Features

- **Native on Windows and Linux**, including the Steam Deck: no emulator, built on your PC by the Longshot Launcher,
  which keeps the game and itself up to date (stable and nightly channels).
- **Modern graphics:** Vulkan, any resolution, ultrawide and 4:3 with the HUD kept in place, frame rates above 60, and
  upscaling and frame generation with NVIDIA DLSS, AMD FSR 3.1 and FSR 4, and Intel XeSS.
- **Controllers, keyboard and mouse:** any controller SDL supports, rumble and DualSense haptics, motion, a mouse
  camera, and figure tags read from files or a Switch 2 Pro Controller.
- **Mods:** file mods, ExeFS patches and cheats, ordered and switched on a Mods tab in the in-game menu.
- **An in-game menu** for graphics, audio, gameplay and controls ([settings](docs/user/graphics-and-controls.md)), and
  crash and bug reports you can send in one press.

## Current goals

- **Performance:** a steady high frame rate in the heaviest areas, on every supported GPU.
- **Stability:** no crashes or freezes, on AMD, Intel and NVIDIA, Windows and Linux.
- **Mods and multiplayer.**

What is being worked on now: the [open issues](https://git.magicbean.dev/MagicBeanDev/Longshot/issues).

## Help

- [Common problems](docs/user/common-issues.md): what you see, and what to do about it.
- [Report a bug](docs/user/reporting-and-contributing.md): what to include. **Gameplay → Bug report** in the in-game
  menu gathers it in one press.
- [Report slow performance](docs/user/performance-report.md): have the game measure itself.
- [Discord](https://discord.gg/magicbean): ask the team and other players.

## Contributing

Testing on other graphics cards, playing further into the game than anyone has, and
[sending your pipeline list](docs/user/reporting-and-contributing.md#send-your-pipeline-list) all help.
[CONTRIBUTING.md](CONTRIBUTING.md) is the way in for code and docs: it points to the [developer docs](docs/dev/README.md),
which cover building from source, the [setup](docs/dev/setup.md), the [daily loop](docs/dev/developing.md) and
[how Longshot works](docs/dev/reference/architecture.md).

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
