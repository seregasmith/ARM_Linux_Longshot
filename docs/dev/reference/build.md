# The build

How `nx2/make_game.py` and `nx2/build.py` turn a dump into a game folder: the steps, what each part is compiled with,
the switches the build reads, and how it schedules compilers. To set up a PC and run a build, see [setup](../setup.md);
for quick builds, caches and what builds again, see [developing](../developing.md#build-quickly). The stages from dump to
game folder are drawn in [architecture](architecture.md#from-dump-to-game-folder).

## The steps by hand

`make_game.py` runs all of these. Each also runs alone:

| Command | What it does |
|---|---|
| `python -I nx2/nup.py <file.nup> --extract work/dump` | Unpack a `.nup` to `exefs/` and `romfs/` (newest title) |
| `python -I nx2/dump.py --dump <folder> --set` | Say what the dump is, and record it in `work/dump.txt` |
| `python -I nx2/lift.py --standalone --write` | Lift the game to C in `work/sa/gen` |
| `python -I nx2/build.py --standalone --jobs 4` (`mise run build-dev`: `LONGSHOT_JOBS`, else a compiler per CPU thread but one) | Compile the lifted tree into `work/sa/build/` |
| `RX_VK_ONLY=1 python -I nx2/build.py --standalone --jobs 4` | The renderer alone, no lift |

`OOT_DUMP` (or `OOT_EXEFS` / `OOT_ROMFS`) and `OOT_WORK` override `work/dump.txt` and the work folder.

Changing translator code changes the lift key: an update with a cached base translation must lift again. Unchanged
generated objects can still be reused from the object cache.

## What the build compiles

- **Lifted chunks** compile at `-O1`. The **hot list** (`assets/profiles/hot_funcs.txt`, the functions the game spends
  most of its time in) goes into eight hot pieces built at `-O2` (`RX_HOT_OPT`). A list named by `A64_HOT_FUNCS` (empty:
  none), else a checkout's own `work/hot_funcs.txt`, goes first; `LONGSHOT_HOT_LIST=0` builds with none. A new list
  comes from `scripts/perf.py hotlist` over all-thread samples of a build with no list ([profiling](profiling.md)).
- **The shared A64 helpers** (`a64_rt.c`, `a64_suyu.c`) compile at `-O2`; `RX_A64_RT_O2=0` builds them at the lifted
  chunks' level.
- **Giant functions** (256 KB of C or more) compile as pieces (`LONGSHOT_SPLIT_GIANTS`, 8 KB).
- **An all-`-O2` build**: `LONGSHOT_CHUNK_OPT=all` (or a file naming `code_*.c` chunks or `f_` functions), cut into parts of
  `LONGSHOT_CHUNK_PART_MB` (6) to keep the compile time down.
- **The renderer** is built with a play profile, `assets/profiles/longshot_vk.profdata.xz`; a checkout's own
  `work/longshot_vk.profdata`, or `RX_VK_PGO` set by hand, goes first. A function changed since the recording compiles
  without profile data ([recording one](../developing.md#record-the-renderers-play-profile)).
- **CPU level**: `-march=native` on a CPU with `x86-64-v3`'s features, else `x86-64-v2`, else `x86-64` (`RX_MARCH`).

Each build writes `work/sa/build/compile_commands.json` for clangd and clang-tidy (the lifted C left out).

## Build switches

Read by the build, not the game, so they are not in `switches.def`.

| Switch | What it does |
|---|---|
| `LONGSHOT_JOBS=<n>` (or `--jobs`) | Compilers at once. Default: one per CPU thread but one, each started when memory holds it ([below](#the-compiler-memory-gate); `LONGSHOT_MEM_GATE=0` turns that off). Given (not 0), it is also the most workers of every other step `make_game.py` runs side by side: the lift's processes and its discovery, the prompts' faces, and the shader preparation, started on that many CPUs (`nx2/build_jobs.py`). Not given: each step keeps its own count |
| `LONGSHOT_MEM_LIMIT_GB=<n>` | Memory the compilers may reserve together, whatever is free now |
| `LONGSHOT_LIFT_JOBS=<n>` | Processes that write the lifted C |
| `LONGSHOT_KEEP_BUILD_FILES=1` | Keep the lifted C and objects after a build |
| `LONGSHOT_OBJ_STORE=<dir>` | One store of compiled objects shared by every work folder (hard links: same drive) |
| `LONGSHOT_LIFT_STORE=<dir>` | Translations shared by every work folder with the same lift key |
| `LONGSHOT_NO_PREPARE=1` | Skip the shader preparation. A run that dies or hangs inside the GPU driver's compiler is started again without that shader: [shader preparation](shader-preparation.md) |
| `LONGSHOT_ROMFS_IN_PLACE=1` (or `--romfs-in-place`) | With `--romfs` and `--exefs`: read the romfs where it is; the game folder has no `romfs`, so start the game with `--romfs <folder>`. Passed over by a `.nup` or `--update` build (the switch with them is refused). `dev.py packs` sets it for packs |
| `LONGSHOT_HOT_LIST=0` | No hot list (lifts again) |
| `A64_HOT_FUNCS=<file>` or empty | This hot list (a relative path from the checkout's root), or none; the lift key hashes the list's contents |
| `RX_A64_RT_O2=0` | The shared A64 helpers at the lifted chunks' level instead of `-O2` (compile only: not in the lift key) |
| `RX_HOT_OPT=<level>` | The hot pieces' level (`O2`; `O1` for a fast build) |
| `LONGSHOT_CHUNK_OPT=all` or `<file>` | Lifted chunks at `-O2` (`LONGSHOT_CHUNK_LEVEL`, `LONGSHOT_CHUNK_PART_MB`) |
| `LONGSHOT_SPLIT_GIANTS=<KB>` | Piece size for giant functions (8; `0` off) |
| `LONGSHOT_SPLIT_DIRECT=0` | The pieces of a `LONGSHOT_SPLIT_GIANTS` function go through its dispatch loop only, as before this switch: each goto into another piece returns to the loop, which calls the piece that owns the label. On by default, the pieces jump to each other themselves (`musttail`, with clang's `preserve_none` calling convention, the function's guest registers and flags as arguments in registers), and come back to the loop only for a guest return or tail call; a compiler without those attributes builds the loop either way. A function with guest locals (`nx2/guest_locals.py`) is split in both modes: its locals cross a cut as the pieces' arguments, or through a struct of the loop's in this mode. Compile-time; the objects of each mode are kept apart (the flag names it) |
| `LONGSHOT_LIGHT_PASSES=0` | Compile small chunks with every module pass (light passes need LLVM 22+) |
| `RX_MARCH=<level>` | CPU level |
| `RX_VK_ONLY=1` | The renderer alone |
| `RX_GAME_ONLY=1` | The game alone, the renderer neither compiled nor linked (`dev.py pack` sets it when a change can't reach the renderer; compile only: not in the lift key) |
| `RX_RELEASE=1` | A release build: no console window, shaders prepared before the title (`make_game.py` sets it) |
| `RX_PGO=gen` or `<file>`, `RX_VK_PGO=gen` or `<file>` | Count branches for a profile, or build with one: the hot pieces, the renderer |
| `LONGSHOT_THIRD_PARTY_OBJECTS=0` or `<dir>` | The prebuilt third-party objects a release ships (`0`: compile them) |
| `LONGSHOT_TRACY=1` | A Tracy build ([profiling](profiling.md#tracy)) |
| `LONGSHOT_NGX_SDK=<dir>`, `LONGSHOT_FSR=1` | Build with NVIDIA DLSS, AMD FSR 4 ([setup](../setup.md#upscaler-sdks-optional)) |
| `A64_FASTFP`, `A64_LOCAL_REGS`, `A64_LOCAL_FLAGS`, `A64_LOCAL_FUNCS` | Lift-time switches (`nx2/fastfp.py`, `nx2/guest_locals.py`); they change the lift key |
| `A64_CAMERA_DIAGNOSTICS=1` | Lift-time camera-clearance counters and trace; changes the lift key. In the game's working directory, create `camera-clearance.trace` to log up to 256 hits per site, then remove it and allow a hit before creating it again. Counters continue between traces. Diagnostic builds check for the marker on every hit. |
| `A64_FASTFP_STORES=0`, `A64_FASTFP_VMOVES=0` | Fast float regions (`nx2/fastfp.py`) stop at a guest store, or at a vector move or shuffle (`nx2/fastvec.py`'s `a64_vld16`, `a64_vdup`, `A64_EXT16`, `A64_EXT8`, `a64_tbl1`), as before. Both on by default: a region may hold stores but no load after one, and its fast copy keeps their addresses and values and stores them, in order, once its MXCSR check passed (its exact copy is unchanged). Lift-time switches: they change the lift key |
| `A64_VECFP=0` or a list of `elem`, `vec`, `minmax`, `fcvt` | The SSE fast paths for the float forms the translator otherwise does lane by lane (`nx2/vecfp.py`); each keeps a result only where it is provably Arm's, else the instruction's own C runs. All on by default; `0` lifts exactly as without them. A lift-time switch: it changes the lift key |
| `A64_FPXKEEP=0` | Without `nx2/fpxkeep.py`: float ops with a NaN or infinite operand, and multiply-adds that cancel exactly, go to the software bodies as before. On by default: inside the float gate (round to nearest, DN = 0, IXC set) their exact results are kept from the operands' bits; invalid ones (inf - inf, inf * 0) still take the bodies, and a binary64 multiply-add that cancels is kept only with a host FMA. A lift-time switch: it changes the lift key |
| `A64_SUB_V=0` | A flag-setting subtraction's V (SUBS, CMP, CCMP) written as the result's sign xor (a < b, signed), which is the same bit, so that the compiler folds the signed conditions (LT, GE, GT, LE: N against V) to one host compare (`nx2/cmpflags.py`). On by default; `0` lifts exactly as without it. A lift-time switch: it changes the lift key |

## The compiler memory gate

`MemoryGate` in `nx2/build.py` starts each compiler by the memory there is, not only by `--jobs`:

- A compile starts when the free memory, less what the running compilers may still take on top of what they hold now,
  keeps its own peak (`mem_need`, an estimate from the file's size and its largest function) and
  `LONGSHOT_MEM_RESERVE_GB` (1.5) free. With `LONGSHOT_MEM_LIMIT_GB` the needs of those running and its own must fit in
  that instead, whatever is free.
- Some always run: up to 4 on Windows (half the threads on smaller CPUs), which pages rather than fails; one on Linux.
- **What a running compiler holds** is `rss_mb`: the resident memory of the compiler driver and all its descendants, not
  the small driver alone. A compiler whose `Popen.poll()` says it has ended counts 0, before any counter is read, so a
  stale process-tree sample or a reused PID never adds memory. Counters that can't be read count 0 too.

`tests/test_rss_liveness.py` and `tests/test_mem_gate_children.py` check this without a compiler or the game.

## Build times

A player's first build on Linux (Ryzen 9 7900X, 30 GB, RTX 3090, NVMe SSD, 24 jobs, Compile shaders Most) takes about
**14 minutes**: about 1 minute to read the game, 1 to lift, 5 to compile and link, 1 to finish the folder and 5 to prepare
the shaders. Peak memory is about 15 GB above what is in use.

These numbers are quoted in the player docs (`docs/user/`): change them together. How to measure them
again: [developing](../developing.md#measure-the-build-time).

## Where dev data lives

Everything the development tools keep goes in one **development folder**, apart from the player's data
(`~/.local/share/longshot`, `%APPDATA%\Longshot`): `~/.local/share/longshot-dev` on Linux (`$XDG_DATA_HOME`),
`%LOCALAPPDATA%\longshot-dev` on Windows, or `LONGSHOT_DEV_HOME`. `python -I scripts/dev.py paths` prints it and each
part; `dev.py paths <part>` prints one, for scripts.

| Part | What | Before | Its own variable |
|---|---|---|---|
| `bases/` | Shared bases, one per commit of main (`dev.py base`) | `~/.cache/longshot-dev/bases` | |
| `builder/` | `tools.lock`'s build tools, one folder per `tools.lock` | `~/.cache/longshot-dev/builder` | |
| `stores/objects/` | Compiled objects every base and pack shares | `~/.cache/longshot-objects` | `LONGSHOT_OBJ_STORE` |
| `stores/lifts/` | Translations every base and pack shares | `~/.cache/longshot-lifts` | `LONGSHOT_LIFT_STORE` |
| `queue/` | The job queue's `history.jsonl`, jobs' `tmp/`, `nvcache/` | `~/.cache/longshot-queue` | `LONGSHOT_QUEUE_STATE` |
| `config/` | `dev_build.json`, `job_queue.json` | `~/.config/longshot/` | |
| `work/` | `mise run build`'s work folder (`work-windows/`: `RX_TARGET=windows`) | `~/.cache/longshot-builder`, the player builder's | `--work` |
| `sdk/` | `scripts/fetch.py`'s SDKs: `ngx-linux/`, `ngx-windows/`, `ffx/`, `xess/` | `build-linux/ngx-sdk`, `work/*-sdk` | `LONGSHOT_NGX_SDK` |
| `runs/` | Game runs: `control/` (`longshot_ctl.py`), `memtour-*`, `cover-*` | `work/control-runs`, `work/memtour-*` | `--root`, `--out` |
| `update/` | `dev.py update`'s worktree `tree/`, `work/`, the game folder's `backups/`, `history.md` | | |
| `tools/` | glslang (`mise run setup`) and Tracy's capture tools (`mise run tracy-tools`) | `~/glslang-16.6.0`, `_deps/tracy-0.15.0` | |

On Windows the old places were `%LOCALAPPDATA%\Longshot\{dev,objects,lifts,queue}` and `%APPDATA%\Longshot\` for the
two settings files. The queue's tickets stay where every version of the queue looks: `$XDG_RUNTIME_DIR/longshot-queue`
(Windows: `%LOCALAPPDATA%\Longshot\queue\tickets`). Tooling's sign-in and chat files stay in `~/.config/longshot`
(`%APPDATA%\Longshot`). A checkout keeps its own `build-linux/` (`build-windows\`: runtimes, `dev.py game`'s
folders, `deps/`) and `work/` (`nx2/build.py` and the steps by hand).

**On a PC set up before the development folder**, run `python -I scripts/dev.py migrate` once, when `dev.py queue
status` shows no job (`--dry-run` says what would move). It moves the old places of the first six parts in, by a rename
on one drive (a checked copy across drives), holding every base while it does, and leaves a link at each old place, so a
session still running older tools keeps working. Until then the tools read the old places. A part the development
folder has already is merged; a name both have stays at the old place, and `migrate` names it. The other parts start
empty: fetch an SDK and build the tools again (`mise run setup`, `mise run tracy-tools`); the first `mise run build`
compiles everything. The Test Farm sets its stores and `HOME` itself and is not affected.

## Tools

| Tool | Purpose |
|---|---|
| `nx2/make_game.py` (`mise run build`) | A game folder from your dump |
| `nx2/build.py` (`mise run build-dev`) | Compile the lifted tree and the renderer |
| `nx2/nup.py`, `nx2/dump.py`, `nx2/lift.py` | Unpack, identify and lift the dump |
| `scripts/coverage/cover.py` | Which lifted functions a play session runs (Windows), the source of `nx2/ran_funcs.txt` |
