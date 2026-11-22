# System requirements

**Your game copy:** program `0400486024DEC000` · update **v131072** · build `e0f3ccfa`. Use a `.nup` file, or the
`romfs` and `exefs` folders. Other game versions are not supported. The launcher checks the files you choose before it
builds; if it reports another build, compare its message with `e0f3ccfa`. The launcher's own version (such as
`v1.0.0-rc.4`) has nothing to do with the game's update or build.

These are estimates from players' reports, not a promise of a frame rate: the game runs on more than this, and
performance depends on the resolution and settings you choose.

| Component | Minimum | Recommended |
|---|---|---|
| **Operating system** | **64-bit Windows 10 / 11**, or **64-bit Linux** with glibc 2.35 or newer. | Same. |
| **Processor** | A 6-core x86-64 CPU, such as an AMD Ryzen 5 2600 or Intel Core i5-8400. AVX2 is not required. | AMD Ryzen 7 7800X3D or Intel Core i5-13600K. |
| **Graphics card** | **Vulkan 1.3.** NVIDIA GeForce GTX 1060 6 GB or GTX 1650, or AMD Radeon RX 5500 XT. | **Vulkan 1.3 with mesh shaders.** NVIDIA GeForce RTX 4070 or AMD Radeon RX 7800 XT. [Common problems](common-issues.md) has each maker's known issues. |
| **Mesh shaders** | Not required: without them, Longshot draws through a slower compute fallback. | **Strongly recommended:** NVIDIA RTX 20 series and newer, or AMD RX 6000 series and newer. |
| **Video memory (VRAM)** | **4 GB**, with the in-game Minimum (4GB) quality preset. | **12 GB or more.** |
| **System memory (RAM)** | **16 GB.** See the memory note below. | **32 GB or more.** |
| **Storage** | **About 60 GB free while building.** A hard drive works, but builds are much slower. | **An SSD with about 60 GB free.** |

**Memory:** recent runs used roughly 13 to 19 GiB for the game alone, depending on the build, the driver and the scene,
so even a 16 GB PC can need swap. Leave room for the desktop and other programs. Swap can prevent an out-of-memory exit,
but it is not extra RAM: heavy swapping can make the whole desktop unresponsive.

**Below the minimum:** a laptop with Intel UHD graphics and a 4-thread CPU has been reported to start, at about 35 fps on
the title screen and with crashes in Intel's driver. See
[Windows, Intel: the game closes in the first scene](common-issues.md#windows-intel-the-game-closes-in-the-first-scene)
for what to set.

**Drivers:** an up-to-date graphics driver with Vulkan 1.3. On Linux, Mesa or NVIDIA's driver; Ubuntu 22.04, Debian 12,
Fedora, Arch and SteamOS all work.

**Download:** an internet connection. The launcher installs the builder, which brings its own compiler and tools: there
is nothing else to install.

**We do not host, store, or distribute game files.** You need your own supported copy, and Longshot builds it on your
PC. Next: [install Longshot](getting-started.md).
