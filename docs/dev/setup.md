# Setup

How to take a PC from nothing to a built game folder: the tools, the team's Tooling repository, the compiler for your
OS, and a first build from your own dump. The daily loop after that is in [developing](developing.md).

**No game files are in this repository, and nothing built from your dump leaves your PC**: not a game folder, a base,
a pack, a cache or a profile ([the rule with no exceptions](../../CONTRIBUTING.md#the-rule-with-no-exceptions)).
Everything a build makes from the dump goes to its `--out` folder and its work folder: with `mise run build`, `work/` in
your development folder (`~/.local/share/longshot-dev`, `%LOCALAPPDATA%\longshot-dev` on Windows; [where dev data
lives](reference/build.md#where-dev-data-lives)), or the checkout's git-ignored `work/` with `--work work`.

## 1. Get the code and set it up

1. Install [mise](https://mise.jdx.dev/getting-started.html). It pins every tool the repository uses.
2. Clone the repository. A shallow clone is enough for everything but cutting a release and naming a build by its tag:

   ```sh
   git clone --depth 1 https://git.magicbean.dev/MagicBeanDev/Longshot.git     # jj: jj git clone --colocate --depth 1 ...
   ```

   `git fetch --unshallow --tags` once gives you the history (`git log`, `git blame`, version names, and Tooling's `scripts/release.py`).
3. In the checkout, run `mise install`, then `mise run setup`. It does what it can without an account, ends with a list
   of what it did and what is left for you, and is safe to run again:

   | Step | What it does |
   |---|---|
   | Tooling | Clones [MagicBeanDev/Tooling](https://git.magicbean.dev/MagicBeanDev/Tooling) beside the checkout as `../Tooling`, or updates it with `git pull --ff-only`. A jj repository or a detached HEAD there is left alone |
   | Hooks | Installs the pre-commit and commit-msg hooks |
   | Python | Installs `requirements.txt` into mise's Python |
   | Checks' tools | On Linux, fetches the pinned Vulkan and SPIR-V headers and glslang that the game-free checks use |
   | Claude Code | If `claude` is installed and the `magicbean` plugin isn't, prints the two commands to install it |
   | Compiler | Checks what a build needs on this OS (Linux: the build's own prerequisite check; Windows: llvm-mingw and the headers) and says how to get what is missing ([section 3](#3-set-up-the-compiler)) |

4. **Sign in** once, from this checkout: `python -I ../Tooling/scripts/forge.py login`. Click **Sign in with Discord**
   and approve. The one sign-in covers git on every repository (the remote `forge` points at Longshot), the team chat
   and Test Farm requests. Setup never signs in for you.
5. **Set up pushing:** `python -I ../Tooling/scripts/forge.py fork` makes your fork and the remote `forge-fork`
   (`pr create` does it too if you haven't).

`mise tasks` lists the tasks: `setup`, `build`, `build-dev`, `dev`, `test`, `lint`, `tracy-tools` (and `ci:smoke`, CI's). Run any Python script from the checkout root with `python -I` (or
`mise exec -- python -I ...` for the pinned interpreter).

## 2. Get the team's tooling

The sign-in, `forge.py` (forks, pull requests, issues, reviews), the team chat and the agents' skills are shared by every
MagicBeanDev repository and live in [MagicBeanDev/Tooling](https://git.magicbean.dev/MagicBeanDev/Tooling), which
`mise run setup` clones as `../Tooling` (elsewhere: use that path wherever these pages say `../Tooling`). Its README and
[docs/forge.md](https://git.magicbean.dev/MagicBeanDev/Tooling/src/branch/main/docs/forge.md) are the reference.

Keep it current at the start of each session: `mise run setup` again, or `git -C ../Tooling pull --ff-only`.

Run Tooling's scripts from this checkout; never copy them in:

| Script | What it is for |
|---|---|
| `../Tooling/scripts/forge.py` | `login`, `fork`, `search`, `pr create/update/ready/wip/status/view/review/merge`, `issue ...` ([docs/forge.md](https://git.magicbean.dev/MagicBeanDev/Tooling/src/branch/main/docs/forge.md)) |
| `../Tooling/scripts/team_chat.py` | Read and post in the team's Matrix room, `#agent-updates:magicbean.dev` |
| `../Tooling/scripts/workspaces.py` | jj workspaces of the repositories beside it: `new`, `list`, `clean`, `remove` |
| `../Tooling/scripts/farm_run.py` | A Test Farm run of two commits or nightlies outside a pull request: `submit` and `cancel` (BigBeans), `status` and `wait` (anyone) |

### Claude Code: the `magicbean` plugin

Tooling is a Claude Code plugin marketplace. Install it once per PC (`mise run setup` prints these when it is missing):

```
/plugin marketplace add https://git.magicbean.dev/MagicBeanDev/Tooling.git
/plugin install magicbean@magicbean
```

`claude plugin list` shows `magicbean@magicbean` once it is in; `/plugin marketplace update magicbean` updates it.
Other agents read `../Tooling/skills/<name>/SKILL.md`. Longshot's own skills are in `.claude/skills/` and load by
themselves in this checkout.

| Skill | Use it to |
|---|---|
| `/magicbean:team-updates` | Read the room, then post a claim, finding, pull request, blocker or finished task |
| `/magicbean:review` | Review a pull request, a ranked pass over the open ones, or your own branch cold |
| `/magicbean:release` | Cut a stable ([releases](reference/releases.md)) |
| `/magicbean:farm-run` | Queue a Test Farm comparison of two commits or nightlies (BigBeans) |
| `prior-work` (Longshot) | Find pull requests, issues, claims and Hookshot changes that already do what you plan |
| `fix-with-test` (Longshot) | Fix a bug as a game-free test that fails on `main`, then the fix |
| `measure` (Longshot) | Measure a change or a switch in the game against `main` and write up the numbers |

## 3. Set up the compiler

The build makes two modules: the game (`longshot.exe`, `longshot` on Linux) and the Vulkan renderer (`longshot_vk.dll`,
`longshot_vk.so`). Every build compiles the renderer, so its headers and clang++ are always needed.

**The quickest way to get exactly a player's tools** is `python -I scripts/dev.py env`: it installs
`tools.lock`'s build tools once in your development folder's `builder/` and prints the environment that uses them, for
your shell to apply. On Linux it needs a Python with the build's packages: run `scripts/linux/deps.sh` once first (or
set `LONGSHOT_DEV_PYTHON`). `dev.py`'s builds (`base`, `pack`, `rt`, `game`, `vk`) set the environment up
for you ([developing](developing.md#test-a-change-against-a-base)).

### Windows

1. Install Python 3 from [python.org](https://www.python.org/downloads/), not the Microsoft Store build (`python -I`
   doesn't see packages installed there), then `python -m pip install -r requirements.txt`.
2. Get [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (the `ucrt-x86_64` build) and set `LONGSHOT_MINGW` to its
   folder. Without it the build takes an `llvm-mingw\` folder at the checkout's root, else falls back to emsdk's clang
   at `C:\emsdk` (or `EMSDK`) with Visual Studio's C++ tools and the Windows SDK; `LONGSHOT_MSVC=1` forces that route.
3. Copy what a clone doesn't carry from the Builder's `tools` folder. Run the newest
   [release's](https://git.magicbean.dev/MagicBeanDev/Longshot/releases) Windows Builder once; the folder is
   `%LOCALAPPDATA%\LongshotBuilder\versions\<version>\LongshotBuilder\tools`.

   | From `tools` | To | Why |
   |---|---|---|
   | `cpm` | anywhere; `RX_CPM` points at it | Vulkan, SPIR-V and VMA headers and FFmpeg. Without it the build stops, unless `LONGSHOT_*_HEADERS` are set |
   | `third_party` | `third_party\` | `sdl2\SDL2.dll` and `SDL3.dll` (the controllers), which nothing here builds; `renhdrum\renhdrum.dll` (RenHDrum, some controllers' vibration; `python -I scripts/renhdrum.py build` also makes it from `third_party/renhdrum`, with a pinned Rust it downloads); `a64emit`, `zbic` and `lld-link` are built from source when missing |
   | `work\hot_order.txt`, `work\longshot_pieces.prof` | `work\` | the hot code's link order and its profile (without them the game builds but runs slower) |

   Don't copy the rest of its `work`: the hot list and the renderer's profile are committed in `assets/profiles/`, and a
   copied `work\hot_funcs.txt` or `work\longshot_vk.profdata` overrides the committed one, however stale.

### Linux

The build needs clang, clang++ and lld, the Vulkan, SPIR-V and VMA headers (from `RX_CPM`, else
`LONGSHOT_VULKAN_HEADERS` / `LONGSHOT_SPIRV_HEADERS` / `LONGSHOT_VMA_HEADERS`, else `/usr/local` and `/usr`) and FFmpeg
(`LONGSHOT_FFMPEG` with `lib/libavcodec.a`, else `pkg-config`). `make_game.py` checks every prerequisite before it reads
the dump and lists everything missing in one error. To get the environment the AppImage is built in:

```sh
scripts/linux/deps.sh          # the AppImage's Python (with the build's packages), appimagetool and runtime
scripts/linux/deps.sh tools    # also LLVM, FFmpeg with dav1d, SDL2 and the headers, packed for tools.lock
scripts/linux/appimage.sh      # the AppImage, in build-linux/
scripts/linux/docker.sh        # both, in scripts/linux/Dockerfile's Ubuntu 22.04 (DOCKER=podman for Podman)
scripts/linux/docker.sh shell  # a shell in that environment, with the checkout at /src
```

The AppImage carries Ubuntu 22.04's C library, so it runs on glibc 2.35 and newer with a Vulkan 1.3 driver (mesh
shaders recommended). On another distribution, build in the container. Where SELinux is enforcing, `docker.sh` runs the
container with `--security-opt label=disable`.

`RX_TARGET=windows` builds the Windows game on Linux, with llvm-mingw's Linux build in `LONGSHOT_MINGW`.

### Upscaler SDKs (optional)

Longshot never commits or ships vendor SDKs. `scripts/fetch.py` downloads each from its vendor at one pinned commit,
checks every file's SHA-256, and asks you to accept the vendor's license first (`--help` has every option):

| Command | Gives you |
|---|---|
| `python -I scripts/fetch.py dlss [--platform linux\|windows]` | NVIDIA's DLSS SDK, in your development folder's `sdk/ngx-<platform>`, where `dev.py`'s builds take it; for other builds, `LONGSHOT_NGX_SDK=<sdk folder>` |
| `python -I scripts/fetch.py fsr [--game <game folder>]` | AMD's FSR 4 runtime (Windows); build with `--fsr` |
| `python -I scripts/fetch.py xess [--game <game folder>]` | Intel's `libxess.dll` (Windows) |

Each takes another folder as its last argument.

Never share a game folder built with the DLSS SDK. On Windows, never put NGX's core (`_nvngx.dll`) beside the game.
A change to the pinned DLSS release updates `scripts/fetch.py` (`DLSS_COMMIT` and its table), `tests/test_ngx_sdk.py`
and the release named in the Longshot Launcher's license text (`App/ViewModels/SettingsStepViewModel.cs` in
MagicBeanDev/LongshotLauncher).

## 4. Build a game folder from your dump

You need your own dump of the supported version: program `0400486024DEC000`, update v131072, build `e0f3ccfa`. Other
versions don't work, and the build reads the dump where it lies without changing it. A build needs about 60 GB free;
build on an SSD if you can (a hard drive works, much slower, and `make_game.py` warns). For memory at run time, see the
[requirements](../user/requirements.md).

`mise run build` installs `requirements.txt` into mise's Python, then runs `nx2/make_game.py` with your arguments:

```sh
mise run build --nup "path/game.nup" --out build --language en-US      # from a .nup
mise run build --romfs <romfs> --exefs <exefs> --out <game folder>     # from extracted folders
mise run build --check --nup "path/game.nup"                           # check the game and version only
```

Useful options (`--help` lists all): `--work <folder>` (the work folder, one per dump; `--work work` keeps the lifted
C and objects in the checkout), `--jobs N`, `--compile-shaders most|all`, `--no-prepare-shaders`,
`--update <game folder>`, `--fsr`, and `--romfs-from <folder>` (hard-link the game files of another folder unpacked
from the same `.nup`). How long a first build takes: [build times](reference/build.md#build-times). The built game's own
arguments are in [advanced setup](../user/advanced.md#start-the-game-from-the-command-line).

Once you have one build, don't build whole game folders to try a change: build it
[against a base](developing.md#test-a-change-against-a-base).

## Portable Builder on Windows

Players use the [Longshot Launcher](https://longshot.magicbean.dev/download); the Builder itself is command-line only.
To keep a Builder on an external SSD, unpack the release zip into a writable folder there and pass `--portable` first:

```powershell
.\LongshotBuilder.exe --portable --cli --nup "E:\your-game.nup"
```

It creates a marked `LongshotBuilder-data` folder beside itself (the installed Builder, tools, build files). Keep it and
its `.longshot-portable` marker. Later `--cli` runs find it without the flag, even after the drive letter changes.
Child processes get `LONGSHOT_SETUP_BASE` and `LONGSHOT_BUILDER_PORTABLE=1`. A portable update needs room for both
versions.

## Repository layout

| Path | What |
|---|---|
| `nx2/` | The lift pipeline and `make_game.py`, which the launcher and the builders run ([build](reference/build.md)) |
| `host/` | The Horizon OS runtime and the native NVN2 layer (`nvn_native.c`); `host/posix/` has the Win32 calls on Linux |
| `gpu/vk/` | The Vulkan renderer, with Eden's shader recompiler and sirit ([architecture](reference/architecture.md)) |
| `gpu/render/`, `gpu/third_party/` | The op stream the host records and the renderer replays; suyu's headers, kept whole (none of its sources is compiled) |
| `builder/` | The command-line Builder: the Windows `--cli` program and its setup, the Linux AppImage's `AppRun` |
| `scripts/` | Development builds, test runs, profile readers, release tooling; `scripts/linux/` (dependencies, AppImage) |
| `tests/` | The game-free checks ([developing](developing.md#run-the-checks)) |
| `third_party/` | Unmodified upstream code, each part with its `PROVENANCE.md` |
| `assets/` | Shipped data: the hot list and renderer profile (`profiles/`), pipeline state lists (`pipeline_states/`), scrubbed saves (`saves/`) |
| `docs/dev/` | These pages; `docs/user/` is the player docs the website publishes |
