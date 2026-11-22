# Advanced setup

What the [settings](graphics-and-controls.md) don't cover: the build's options, the upscalers whose files come from
their makers, switches in `longshot_env.txt`, and the game and the builder from the command line.

## Build options

These are in the launcher's build settings.

- **Compile shaders:** what the build prepares at its end. **Most** (the default) prepares every shader and the
  pipelines the game is known to use; **All** adds about 50,000 predicted pipelines that play almost never uses, for
  about 40% more preparation time. A preparation the game runs at a start (after an update or a new graphics driver)
  follows the same choice, kept as `compile_shaders` in `longshot_options.ini` (0 Most, 1 All).
- **Prepare shaders now:** off, the first start prepares them instead. A build whose preparation stopped ("the GPU driver
  stopped responding") leaves `longshot_prepare_skipped` in the game folder's `cache` folder, and the game then builds
  each shader when it is first drawn, with stutter, until you delete that file or build again.
- **Build files:** the translated code and objects (about 4 GB) are deleted once the game is ready. To keep them, so an
  update compiles only what changed, set `LONGSHOT_KEEP_BUILD_FILES=1` for the build; **Free up space** deletes them
  later.

## Optional upscalers

AMD FSR 3.1 is in every build. NVIDIA DLSS, AMD FSR 4 and Intel XeSS run their makers' own files, under their makers'
licenses: Longshot never includes them, and you never share a game folder built with them. Choose them in the
**Upscaler** row once they are there; the log's `vk: NVIDIA DLSS:`, `vk: AMD FSR 4:` and `vk: Intel XeSS:` lines say
whether each started, and why not.

- **NVIDIA DLSS** (RTX cards): turn on **DLSS** in the launcher's build settings. It shows NVIDIA's license first; the
  build then downloads NVIDIA's SDK and puts DLSS's and DLSS Frame Generation's runtimes in the game folder's `ngx`
  folder. An update keeps them. On Linux, NVIDIA's library loads them only from a folder whose whole path is plain
  ASCII: for a game folder such as `~/Games/Longshot-Café`, the menu says so; copy `libnvidia-ngx-dlss*.so.*` from its
  `ngx` folder into `~/.local/share/longshot/ngx`.
- **AMD FSR 4** (Windows): turn on **FSR 4** in the launcher's build settings. It shows AMD's license first, then
  fetches AMD's two DLLs into the game folder. On Linux, put a Vulkan FSR 4 library (`amd_fidelityfx_upscaler_vk.so`)
  beside the game yourself: AMD publishes none for Linux.
  **FSR 4 model** (`fsr4_model=0` or `1`; `RX_VK_FSR4_MODEL=auto` or `lite` for one run) chooses AMD's network: **Auto**
  keeps AMD's own pick for the GPU (FP8 on RDNA 4, full INT8 on RDNA 3 and Strix Halo, the RDNA 2 model on RX 6000, Lite
  on Strix Point and Van Gogh). Forcing **Lite (INT8)** requires a supported Windows runtime with a verified Lite provider;
  the current build leaves forcing disabled until the provider ABI is verified. Linux can retain the saved Lite value but
  cannot force it, so Auto is used.
- **Intel XeSS** (Windows): put Intel's `libxess.dll`, from the `bin` folder of a recent
  [Intel XeSS SDK](https://github.com/intel/xess/releases) release, beside `bin\longshot.exe` or in the game folder. It
  runs on Intel Arc and Iris Xe GPUs, and on other GPUs whose Vulkan driver has what XeSS needs. There is no XeSS for
  Linux.

## Switches: longshot_env.txt

Some workarounds are switches: lines of `NAME=VALUE` in a text file named `longshot_env.txt` in the game folder, read
when the game starts. It takes only the game's own switches (names starting `RX_`, `HOS_`, `LONGSHOT_`, `EDEN_`,
`NVN_` or `SDL_`) and no paths; the log names any line it left out. A switch that stands in for a menu row wins over
it while the line is there, and `longshot_options.ini` keeps your menu choice. Delete the line to undo it. On Windows, check
that the file isn't really called `longshot_env.txt.txt` (File Explorer: **View → Show → File name extensions**).

The switches worth knowing as a player are on the pages that need them ([common problems](common-issues.md),
[the Steam Deck](steam-deck-playbook.md), [slow performance](performance-report.md)). The full list is for developers:
[switches](../dev/reference/switches.md).

## Start the game from the command line

The game itself is `bin\longshot.exe` on Windows and `bin/longshot` on Linux, in the game folder. The `longshot`
launcher beside `bin` doesn't pass arguments on: start the game directly to use them.

| Argument | What it does |
|---|---|
| `--user <folder>` | Where the game keeps your saves, caches and temporary files. Default: `user` in the game folder on Windows, `~/.local/share/longshot` on Linux |
| `--romfs <folder>` | The game files' folder, when it is not the game folder's `romfs` |
| `--prepare-shaders` | Prepare the shaders as the first start does, then exit |
| `--compile-shaders all` or `most` | What this run's preparation builds ([build options](#build-options)) |

```sh
cd ~/Games/Longshot
./bin/longshot --user "$HOME/longshot-data"
```

## The builder from the command line

The launcher builds with the Longshot Builder, which also runs on its own from the command line, over SSH or in a
script. Each version's builder is on [Longshot's releases](https://git.magicbean.dev/MagicBeanDev/Longshot/releases).

```sh
./Longshot-<version>-x86_64.AppImage --cli --nup ~/your-game.nup --out ~/Games/Longshot   # build
./Longshot-<version>-x86_64.AppImage --cli --update ~/Games/Longshot                      # build again with this version
./Longshot-<version>-x86_64.AppImage --play ~/Games/Longshot                              # play
```

On Windows, unpack `LongshotBuilder-<version>-windows-x64.zip` and run `LongshotBuilder.exe --cli` with the same
options. `--check` only checks your game's version; `--log <file>` saves the build's output; `--jobs <n>` (or
`LONGSHOT_JOBS=<n>`) limits how many files compile at once, and how many CPU threads every other step of the build uses
(translating, the button prompts, preparing the shaders), so the PC stays usable while it builds. To build from a clone of this repository, see the
[developer setup](../dev/setup.md).

## Linux: faster GPU memory (/dev/udmabuf)

Longshot imports GPU memory fastest through the kernel's `udmabuf` device. Without access to it the game warns, on
stderr and in `longshot_log.txt`, and imports more slowly, much more on AMD (RADV). On NVIDIA's driver you can ignore the
warning.

Check with `ls -l /dev/udmabuf` and `id`. If the device is missing, load it with `sudo modprobe udmabuf`. Then give
your user access in one of these ways:

1. **On a desktop:** put `SUBSYSTEM=="misc", KERNEL=="udmabuf", TAG+="uaccess"` in
   `/etc/udev/rules.d/70-longshot-udmabuf.rules`. It grants the device to the user at the screen.
2. **Over SSH or as a service:** create a `udmabuf` group with only the game's user in it, and use
   `SUBSYSTEM=="misc", KERNEL=="udmabuf", GROUP="udmabuf", MODE="0660"` in that file instead.

Apply the rule with `sudo udevadm control --reload-rules` and
`sudo udevadm trigger --action=add /sys/class/misc/udmabuf`, then log out and back in after a group change. Never run
the game as root or make the device writable for everyone.
