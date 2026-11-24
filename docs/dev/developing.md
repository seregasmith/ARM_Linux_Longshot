# Developing

The daily loop: build a change quickly, run it against `main`, check it, measure it, profile it, and take it from a
claim to a merged pull request. Each section says what to run; the reference pages say how the parts work. The rules a
change must meet are in [standards](standards.md).

Everything you build or record from the game stays on your PC: bases, packs, runtimes, run folders, profiles, frame
dumps ([the rule with no exceptions](../../CONTRIBUTING.md#the-rule-with-no-exceptions)). Log lines and screenshots are
fine.

## Build quickly

| Build | How | Use it for |
|---|---|---|
| Against a base | `dev.py pack` ([below](#test-a-change-against-a-base)) | Any change you want to run in the game |
| Renderer only | `dev.py vk`, or `RX_VK_ONLY=1` | A change under `gpu/vk/` that the game isn't built from |
| Fast | `dev.py game --quick`, or `RX_HOT_OPT=O1 mise run build-dev` | Checking that something works: a start, a menu, a log, a screenshot |
| Default | `mise run build`, `dev.py game`, `mise run build-dev` | Anything you measure, and every release |

**Never measure a fast build.** It compiles the hot code at `-O1`: no fps, frame times, build times or profiles from it.
`build-dev` compiles an already lifted tree; lift one first with
`python -I nx2/lift.py --standalone --write` ([the steps by hand](reference/build.md#the-steps-by-hand)). `dev.py` is
`scripts/dev.py`: run it with `python -I scripts/dev.py ...` (`mise run dev -- ...` is the same). `dev.py --help` lists
its commands.

### What is kept between builds

- **Objects:** under `dev.py` they go to a store shared by every work folder and base, your
  development folder's `stores/objects` ([where dev data lives](reference/build.md#where-dev-data-lives)), or
  `LONGSHOT_OBJ_STORE` (`0` for none). A plain `make_game.py` build (`mise run build`) uses a store only when
  `LONGSHOT_OBJ_STORE` is set. It hard-links, so keep it on the drive your builds are on.
- **Translations** (the lifted C): the same way, `stores/lifts` under `dev.py`, or `LONGSHOT_LIFT_STORE`, shared by every
  build with the same lift key.
- **Kept files:** in the checkout's own `work/` (`--work work`), or with `LONGSHOT_KEEP_BUILD_FILES=1`, the lifted C and
  objects stay, so the next build compiles only what changed. In any other work folder, `make_game.py`'s default
  included, only the last build's objects (and the translation's smaller files) are kept.

### What builds again

- **An object** is named by its source, flags and the headers it reads (`object_key` in `nx2/build.py`), so a header edit
  recompiles only the files that include it. `hos_main.c` and `vk_menu.cpp` carry the version, a fingerprint of every
  source under `host/`, `gpu/` and `nx2/`, so they compile again whenever any of those changes.
- **The lift** runs again only when its key changes (`lift_key` in `nx2/make_game.py`): the game's code, the
  translator's sources (its Python by code alone: a comment or docstring edit keeps the translation), the HLE functions,
  the hot list, the chunk boundaries, and of the environment only `A64_*`,
  `SUYU_RECOMP_*` and `RX_TARGET`. Folder paths and compile-only switches (`RX_HOT_OPT`, `RX_PGO`, `RX_VK_PGO`) are not
  in it. **Don't set `A64_*` by accident**: the build then can't reuse anyone's translation.
- **A link** is skipped when nothing it reads or writes changed (`<output>.linkkey`; delete it to link again).
- **Button prompts** are kept while their inputs match; `LONGSHOT_FORCE_PROMPTS=1` draws them again.
- **The shader preparation** runs again when the shader translator's stamp changes: any code change (not comments
  or whitespace) to `gpu/vk/eden`,
  `gpu/vk/sirit`, `gpu/vk/compat`, `vk_shader.cpp`, `vk_bank.cpp` or `vk_state.h` makes every player translate every
  program at their next start ([switches](reference/switches.md#adding-or-removing-a-switch)).

Build switches, the compile levels and the compiler memory gate: [the build](reference/build.md).

## Test a change against a base

A full build of a new game folder takes a while, much of it the shader preparation
([build times](reference/build.md#build-times)). A **base** is the game built once; a **pack** is a change built as only
what differs from it; a **runtime** is a game folder to run, a copy of the base with packs over it. `make_game.py` and
`build.py` run as they are, so what players build is unchanged. `dev.py` keeps one shared base per commit of `main`,
builds this checkout as a pack over the nearest one, and puts each step through the [job
queue](#share-a-pc-the-job-queue):

```sh
python -I scripts/dev.py base             # the shared base where this checkout left main (built once per commit)
python -I scripts/dev.py pack             # this checkout as a pack over it
python -I scripts/dev.py rt               # build-linux/rt-<checkout>/, the base with the pack (build-windows\ on Windows)
python -I scripts/dev.py rt --empty       # build-linux/rt-base/, the base alone: the other half of an A/B
python -I scripts/dev.py bases            # the bases, their packs, when each was last used
```

| `dev.py` | What it does |
|---|---|
| `base [REV] [--romfs-in-place]` | Build `REV`'s base (default: where this checkout left `main`) unless someone has: one per commit, in the development folder's `bases/`. `--romfs-in-place`: no `romfs` in its game folder (below) |
| `pack` | Build this checkout as a pack over the nearest base in its history. Renderer-only changes build the renderer alone; docs, checks, CI files and developer tools nothing (`NOT_BUILT` in `scripts/devtools/packs.py`); anything else builds the game, without the renderer when only `host/` C files or the lift changed |
| `rt [--empty]` | A game folder to run: the base's, `romfs` hard-linked, the rest copied (copy-on-write clones on btrfs or XFS), the pack over it. Run again, it refreshes the runtime in place: only files that differ are replaced, and its caches, logs, saves and settings stay |
| `bases` | The shared bases, their packs and when each was last used |
| `game [--quick \| --timing] [--fsr]` | A whole game folder, `build-linux/game/` (`--quick`: `-O1` hot code; `--timing`: a measured build) |
| `vk` | The renderer alone, its exports checked |
| `env` | The build environment as shell lines, for a build of your own |

`dev.py packs` does the same on a base folder you name, without the queue: what `base`, `pack` and `rt` run inside their
job, and what the Test Farm runs.

```sh
python -I scripts/dev.py packs base ../base-main --romfs <romfs> --exefs <exefs> [--seed <older game folder>] [--romfs-in-place]
python -I scripts/dev.py packs pack ../base-main my-fix --tree <the change's checkout>
python -I scripts/dev.py packs rt   ../base-main ../rt-main                   # no pack: main, the other side of an A/B
python -I scripts/dev.py packs rt   ../base-main ../rt-my-fix my-fix
python -I scripts/dev.py packs warm ../base-main --run "<command that plays {runtime}>"
```

- `--seed` copies an older game folder's shader caches into a new base first, so the preparation redoes only what changed.
- `--romfs-in-place` leaves the romfs out of the base's game folder and its runtimes: the preparation and the game are
  started with `--romfs <romfs>` (`rt` prints it; `dev.py ab` passes it). For a romfs the build may only read, such as a
  Windows sandbox, where the hard link is refused and becomes a 26 GB copy.
- A pack that builds the game reads the romfs where it is, so its game folder has none and nothing of it is compared
  with the base. `LONGSHOT_PACK_ROMFS=copy` links it instead, for a change that writes into the romfs.
- Upscalers come from the environment, the same for a base and its packs: `LONGSHOT_NGX_SDK` (NVIDIA DLSS, as
  `make_game.py` reads it), `LONGSHOT_FSR=1` (AMD FSR 4: `--fsr`), and `LONGSHOT_FFX_SDK` or `LONGSHOT_XESS_SDK` (AMD's FSR
  runtime or Intel's XeSS library, already fetched there by `scripts/fetch.py`, whose license was accepted then: copied into
  the game folder, nothing downloaded). A base built without one makes packs without it too, so build a new base to change them.
- `warm` plays a pack-less runtime and copies the caches it grew back into the base, so no runtime compiles pipelines
  for those scenes again.
- One job at a time per base: `base`, `pack`, `rt` and `warm` hold `<dir>/dev_packs.busy` (`dev.py pack` and `rt` wait for the base instead).
- Packs that bring the same file, or a pack from another base, are refused, so the game and the renderer always match
  (`MKWVK_API_VERSION`).
- **For numbers**, compare the change's runtime with one made by `rt` from no pack: same base, same caches, one
  difference. Warm the base for your scenes first, so neither side compiles pipelines while it is measured.

## Keep a game on main

To play or test what `main` has right now, without waiting for Nightly, keep a game folder of your own on its newest
commit:

```
python -I scripts/dev.py update --game <folder>     # once: names the folder (remembered), and builds it
python -I scripts/dev.py update                     # later: builds again only when main moved
python -I scripts/dev.py update --pick 1874,1880    # your folder's commit plus only those merged pull requests
python -I scripts/dev.py update --with 1913,1925    # open pull requests merged on top, to try them before they land
python -I scripts/dev.py update --logs              # its logs, crash records and settings in one zip, for a report
python -I scripts/dev.py update --gui               # all of it in a window: merged and open PRs to tick, searchable
```

- **What it builds:** main's newest commit, from a worktree of its own (`update/tree` in your development folder): your
  checkouts and branches are not touched. The build runs with that commit's own `dev.py`, in the player's build
  environment, through the [job queue](#share-a-pc-the-job-queue), sharing the stores
  ([what is kept between builds](#what-is-kept-between-builds)): when the translation didn't change, only what changed
  compiles.
- **The game files:** a folder that keeps them (`romfs/`, `.longshot_exefs/`, as a launcher or `make_game.py` build
  does) is built again from them (`make_game.py --update`); a new folder takes the files `dev.py` finds (`"romfs"` and
  `"exefs"` in `dev_build.json`).
- **What it keeps:** saves, mods, `longshot_options.ini` and `longshot_env.txt`, as a launcher update does. Before each
  build it backs up the folder's build files into `update/backups/` (the last three); a failed or stopped build is put
  back. `--rollback` puts the last backup back.
- **Picking:** `--pick` (or unticking pull requests in the window) builds the folder's commit with only the picked
  pull requests merged since, cherry-picked in their order. One that needs a pull request you left out is refused and
  named; pick that too, or update to everything.
- **Open pull requests:** `--with` (or ticking them in the window) merges each one's head onto the build, after the
  main commit or your picks; one that conflicts is refused and named before your folder is touched. The list comes from
  the forge without signing in. An update without `--with` leaves out the open ones your build had, and says so.
- **Logs:** `--logs` zips `logs/`, `bug-reports/` (never `bug-reports/local-only/`: its minidumps hold game and private
  data), the settings files and the last build's log into `update/logs/`. Logs can name your user folder: read them
  before you share them.
- **Other options:** `--check` says what's new since the folder's build and builds nothing; `--force` builds even when
  the folder is on the newest commit; `--remote` and `--branch` follow another remote or branch (remembered). Each
  update's new commits go into `update/history.md`. It refuses while the game in that folder is running.

## Share a PC: the job queue

On a PC that several people or agents share, every build and game run goes through `scripts/dev.py queue`. It runs each
job when it fits and keeps measurements apart from everything else. `dev.py base`, `pack`, `rt`, `game` and `vk` put
their own steps through it:

```sh
python -I scripts/dev.py queue run shots --mode shared --desc "menu font at 4K" -- <your run>
python -I scripts/dev.py queue run fps --mode timing --desc "RX_VK_ASYNC=0, Hyrule Field" -- <your measurement>
python -I scripts/dev.py queue status           # who plays, what runs, what waits and why
python -I scripts/dev.py queue history          # the last jobs, and how busy the PC was while a timed one measured
python -I scripts/dev.py queue cancel <name>   # the job and every process it started: a game it started too
```

`--desc` says in a few words what a game run tests: it shows under the version in the menu (F1) and on the start-up
screen, so a screenshot or a stream says which change it is (default: the job's name).

| Mode | For | Runs beside |
|---|---|---|
| `build` | `make_game.py`, `dev.py packs base` / `pack`, the test suites (`mise run test`, `tests/run_all.py`) and any compile-and-run check | 2 at once; never a timed run |
| `light` | Small game-free work: a copy, one quick check, `dev.py vk` | 4 at once; never a timed run |
| `shared` | A game run that is not a number: screenshots, logs, checks | 3 at once, each with its own game and data folder |
| `gpu` | The game alone on the GPU, not a measurement | Builds |
| `timing` | fps, frame times, hitches | Nothing heavy; own CPUs; memory not swapped; not while you play |
| `timing-cpu` | A measured build | GPU runs on the other CPUs |
| `profile` | A profile recorded in the game | As `timing` |

- **Measure with `timing`, run everything else with `shared`.** Keep each GPU job short: a long batch holds the queue.
- **A timed run says when the PC was busy.** While it measures, the queue samples every 2 s what the run's own processes
  stalled on (CPU, memory and disk: its job's cgroup), the other CPUs' use, builds outside the queue and jobs beside it.
  The PC swapping is recorded but no noise by itself: a game holding 18 GB swaps the rest of a 32 GB PC out. A run past any
  limit (`NOISY` in `scripts/devtools/jobqueue.py`) is marked NOISY in `dev.py queue history`, and `dev.py ab` adds a
  **Noisy:** line naming it, judged on its measured stretch only (`--noise FILE` gives a caller the samples). Run a noisy
  run again; its numbers are not the change's.
- **Nothing on a RAM file system.** Builds, game folders, work folders and test output go on a disk, never in `/tmp` or
  another tmpfs. The queue moves a job's `TMPDIR` off tmpfs, and `dev.py`'s builds refuse such a folder.
- **One base per commit of main**, shared in your development folder's `bases/` ([where dev data
  lives](reference/build.md#where-dev-data-lives)). Agents sharing a PC agree before moving it.
- `dev.py`'s builds name any `A64_`, `OOT_` or `RX_` variable you set, finds the game files in `~/Games` (and
  `~\Games\Longshot` on Windows) or `LONGSHOT_DEV_ROMFS` / `LONGSHOT_DEV_EXEFS`, and reads its settings from
  `config/dev_build.json` in the development folder (the queue: `config/job_queue.json`).

## Run the checks

The game-free checks need no dump:

```sh
mise run test                                   # every check, --strict (a missing tool fails instead of skipping)
python -I -B tests/run_all.py ub_               # only the checks whose path contains "ub_"
mise run test-changed                           # only the checks that read a file your branch changed (before you push)
mise run lint                                   # pre-commit and ruff on what the branch changed, and actionlint
mise run test --container                       # Linux: the same in a container with CI's tools (Podman or Docker)
mise run ci                                     # what CI checks: the pinned test tools, every check and the website
```

A check whose tool this PC lacks (clang, spirv-val, glslang, .NET, a sanitizer) fails in `mise run test`. Install the tool,
or use `--container`: `scripts/linux/checks.sh` builds an image with the tools and versions of CI's job (Ubuntu 24.04,
~3 GB, once) and runs the checks in it as your user, with no network and only the checkout to write.
With `--container`, a `--junit` report must be inside the checkout; relative paths are resolved from the directory
where you invoked the command. Other destinations are rejected before the image is built.

`tests/run_all.py` runs every `tests/test_*.py` and `nx2/test_*.py` in its own process, the C++ unit tests under
`gpu/vk/tests` and a byte-compile of every tracked Python file. It keeps going after a failure and ends with a table and
every failure's output. `mise run test-changed` is `run_all.py --strict --changed`: a quick check while you work, never
the whole suite's stand-in. Ruff (`ruff.toml`: pyflakes and a few error rules, 120 columns, no formatting) runs from
`mise run lint` on the Python files a branch changes. Its docstring has every option: `--exact`, `--changed [BASE]` (default: the Longshot remote's
`main`, else `origin/main`), `--strict`, `--no-skip` (also the checks a test skips on this OS), `--list`. **A skip is not a pass**: read the table. C++ checks
compile with `-Werror`; when you change C++ that a check compiles, run them with both compilers (`CXX=g++ ...`, then
`CXX=clang++ ...`). Before a pull request, run the whole suite.

`tests/run_vulkan_fixtures.py` runs write-back, copy and aspect/crop cases on Mesa's software Vulkan driver (lavapipe)
with validation. It needs the loader, headers, `vulkaninfo`, lavapipe and the validation layer, and is CPU Vulkan, not
a GPU or a game run:

```sh
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  python3 -I -B tests/run_vulkan_fixtures.py --cxx g++ --headers /usr/include --device llvmpipe
```

Slow checks that need your dump (`work/dump.txt` from `nx2/dump.py --set`):

| Command | What it checks |
|---|---|
| `python -I nx2/test_insn.py [--per 6 --states 12]` | Every instruction form the game uses: Unicorn against the lifted C |
| `python -I nx2/test_leaf.py [--count 3000 --states 4] [--calls]` | Random whole functions (or call trees): Unicorn against native code |
| `python -I nx2/census.py --nx2` | The instruction census per module |
| `python -I nx2/lower_test.py <folder>` | Shader programs that failed to translate (from `RX_VK_DUMP_FAILED`) |

### Write a check

A fix comes with a game-free test that fails on `main` for the bug's reason ([standards](standards.md#fixes-and-checks);
the order of the work is the [fix-with-test skill](../../.claude/skills/fix-with-test/SKILL.md)).

- Name it `tests/test_<topic>.py` (or `nx2/test_*.py`), standard library only where it can be (declare any package in
  `RUN_ALL_MODULES`), with a docstring that says what it
  checks and how to run it. `tests/run_all.py` picks it up.
- Declare what it needs as plain literals at the top level, after the docstring and imports; the runner reads them
  without importing the test:

  ```python
  RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]   # {cxx}, {vulkan_headers}, {spirv_headers},
                                                                        # {spirv_val}, {glslang}, {dotnet}, {dav1d}
  RUN_ALL_MODULES = ["zstandard"]                                       # packages it imports
  RUN_ALL_SKIP = {"windows": "POSIX signals: the Linux guard"}          # OS -> why it can't run there
  ```

- **Cut from the real source** with `tests/srccut.py`, and compile or run the cut with stand-ins. Never paste a copy of
  the source into the test: it has to fail when the source goes back. Start from a check that already reads the same
  file (`git grep -l <file> tests`).

  ```python
  sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the test's folder off the path)
  from srccut import block, between
  body = block(text, "static int env_line_allowed(")          # the marker to the brace that closes it
  part = between(text, "// begin", "// end")                  # start up to, not including, end
  ```

- **Check the behaviour**, not the text of the new line: what the function returns, writes or calls. A fix behind a
  switch is tested both ways. Keep it to seconds, on Linux and Windows where it can run.

## Run the game for a test

Build the change [against a base](#test-a-change-against-a-base) and run its runtime folder; on a shared PC through
the queue (`--mode shared`, or `--mode timing` for numbers). Ways to drive a run:

- **The control bridge** (`HOS_CONTROL`, `scripts/longshot_ctl.py`): timed inputs, waits, screenshots, menu settings,
  status and quit, from a script or an agent. Prefer it for anything automated: [control bridge](reference/control-bridge.md).
  `longshot_ctl.py launch <game folder> --profile test --repo-save <name>` starts a headless run from one of the
  repository's scrubbed saves ([saves](reference/saves.md#saves-in-the-repository)).
- **`scripts/run-vk.ps1 -Window -Graceful`** (Windows, through PowerShell): a muted windowed run with a screenshot every 300 presents in
  `work/runs/<name>/shots`, closed gracefully at the end. Parameters: `-Seconds`, `-Name`, `-BuildName "what this tests"`,
  `-Sound`, `-ShotEvery`.
- **Switches** in the environment, or in a test's own file named by `HOS_ENV_FILE` with its settings in
  `HOS_OPTIONS_FILE`, so the player's files stay as they are. The switches for test runs (headless, window size, build
  name, fixed time, no sound) are in [switches](reference/switches.md#switches-for-test-runs).

Screenshots and dumps reach gigabytes: look at them on your PC, then delete them.

### Stopping a run

**Quitting is not saving.** No stop route asks the game to save or waits for a save. Save in the game and wait first.

| Route | What happens |
|---|---|
| Control bridge `quit` | The renderer's close hooks run, the log is flushed, the process exits 0. The reply acknowledges the request: wait for the process to end |
| Closing the window | The same close hooks |
| Windows, windowless game: Ctrl+C / Ctrl+Break, `taskkill /PID` without `/F` | Close requested, no deadline |
| Windows: console close, logoff or shutdown | Close requested with a 4-second deadline; past it the process ends with code 124 |
| Linux, windowless game: SIGTERM / SIGINT | **No** close hooks. Use the bridge's `quit` |
| `taskkill /F`, SIGKILL | No close work at all |

Only `stop: close work done ...` in the log **and** an observed exit code 0 mean the close hooks finished.

### Read the log

The log is `logs/longshot_log.txt` in the game folder when the game has no stderr (started from its icon or the
launcher); started from a terminal, it logs to stderr.

- The first line is the version; the next names the run's random `run_id`, which its crash records repeat.
- `vk: switches set (n): ...` lists every switch the run set, and names any `RX_` or `HOS_` name that is no switch
  (misspelt, removed, or from another build). `env: ... left out` says a line of `longshot_env.txt` was not applied.
- `vk: policy:` lines say what the renderer learned of the PC and which hardware rules apply
  ([device policy](reference/architecture.md#device-policy)).
- `vk: driver shader cache:` lines (Linux, `gpu/vk/driver_cache_env.h`) give the size of the driver's own shader cache,
  which folder this start uses and whether RADV forces VRS (which gives every pipeline another key). Each build of the
  noted or predicted pipelines records its environment in `cache/longshot_driver_env.txt` (the last 8); a start whose
  keys match none of them warns that every pipeline is compiled again. Read these first when a start from Steam
  compiles what the preparation built.
- A line is info unless tagged: `hos_log_at(HOS_LOG_TRACE, ...)` (`host/hos.h`) and `logf_at(log_trace, ...)`
  (`gpu/vk/vk_internal.h`) write `[trace]`, `[debug]`, `[warn]` or `[error]` after the thread's tag. Per-item lines a
  player can't act on are debug or trace. The file keeps every line.

What the renderer's frame and timing lines mean: [profiling](reference/profiling.md#reading-the-renderers-log-lines).

### Crash records

After a crash the game writes `bug-reports/pending/crash-<UTC time>.txt` beside the log: version, time, exception or
signal, thread, module offsets (the same numbers as the log's `CRASH:` lines) and bounded host and guest backtraces. A
run that ended without a record (killed, power cut, out of memory) leaves an `unclean-<UTC time>.txt` at the next
start. The newest 15 are kept. A helper then shows the [crash window](reference/crash-window.md).

| Switch | What it does |
|---|---|
| `HOS_CRASH_WATCH=0` | No helper and no window after a crash |
| `HOS_CRASH_TEST=start` or `<seconds>` | Crash on purpose at the start or after that many seconds (environment only) |
| `HOS_CRASH_MINIDUMP=1` | Windows: also a minidump in `bug-reports/local-only/`. It holds game and private data: **never share it** |

### Scripted input

| Switch | What it does |
|---|---|
| `HOS_INPUT_SCRIPT=<presses>` | Presses timed from the game's first controller read |
| `HOS_INPUT_LIVE=<file>` | A file the test appends lines of presses to, read every 30 ms. `!cheat <n>` sends a Cheats page action; `!synclog` logs the NVN syncs |
| `HOS_INPUT_FRAME_CLOCK=1` | The script's time runs at 60 game frames a second instead of the wall clock |
| `HOS_INPUT_EXCLUSIVE=1` | The script alone drives the game until 5 s after its last press |
| `HOS_INPUT_RECORD=<file>` | Records the input where it reaches the game, frame by frame: an [input recording](reference/input.md#input-recordings) (also the Insert menu's Debug tab, Record input) |
| `HOS_INPUT_PLAY=<file>` | Plays a recording back in place of the player's devices, the script and the bridge, and logs where the run first leaves it |
| `HOS_INPUT_SYNC=<n>` | With `HOS_INPUT_PLAY`: Link and the camera put back on the recording at every n-th update (1 by default; 0 never, the exact test) |
| `HOS_INPUT_SYNC_MAX=<m>` | Past this many metres (1) from the recording the replay has diverged: said once, nothing put back after it |
| `HOS_INPUT_ROUTE=1` | With `HOS_INPUT_RECORD`: a [route](reference/input.md#routes), no guest words or addresses; `HOS_INPUT_SAVE` names its save |
| `HOS_INPUT_FROM=boot` | A recording starts at the game's first controller read, not the frame after Link's first update |
| `RX_VK_MENU_SCRIPT=<s>:<key>,...` | Timed Insert-menu keys: `INS UP DOWN LEFT RIGHT ESC ENTER TABL TABR PADS PADE PADW PADN`, `TYPE:<text>`, and in a text field `HOME END BACK DEL` |
| `RX_VK_UI_PROMPTS=pad` or `keys` | The menu's button prompts for a pad or keys (screenshots) |

Presses are `<seconds>:<button>[+<button>...][/<ms>]`, comma-separated: `2:A,5.5:LUP/1500,9:ZL+R`. Buttons:
`A B X Y L R ZL ZR PLUS MINUS LS RS LEFT UP RIGHT DOWN`, and sticks at their edge `LUP LDOWN LLEFT LRIGHT RUP RDOWN
RLEFT RRIGHT`. `SPRINT` holds the Sprint control's key (it runs only with the Controls tab's Sprint row on); the game
never sees it as a button. `MX=<counts>` and `MY=<counts>` move the mouse that many counts a frame while held, for the
native mouse camera (`60:MX=40/1000` turns it for a second). A press is held 200 ms unless `/<ms>` says otherwise; names are case-sensitive. Scripted and bridge
presses count while the window is in the background; the game still has to be in a state that reads the controller.
Switches that name a file or press buttons are taken only from the environment or `HOS_ENV_FILE`, never from
`longshot_env.txt` ([switches](reference/switches.md#longshot_envtxt)).

### Replay a recording

The same input on every run, for a bug to show again, a fix to be checked against it, or a moving route to measure on:
an [input recording](reference/input.md#input-recordings). Every replay says how close it stayed in its log, so a run
that went another way is seen, not taken for a result.

1. **Record it once.** In the game: the Insert menu's Debug tab, Input recording, Record input; press it again to stop.
   The file is `recordings/<date>-<time>-<map>.jsonl` in the run's user folder. Over the bridge: `record` and `record
   action=stop` (its `path`). From the start: `HOS_INPUT_RECORD=<file>`. A recording to commit is a
   [route](reference/input.md#routes): `dev.py route record --save <name> --name <route>`. Record at the 60 cap with the
   Per frame step and `HOS_FIXED_TIME`, as a replay runs.
2. **Play it on the change**, through the queue (`--mode shared`), with `HOS_REALTIME_STEP=0
   HOS_FIXED_TIME=1767225600 HOS_FPS_MAX=60` as it was recorded:
   - **a recording made in the game** (the menu or the bridge's `record`): in a running game, the bridge's `play
     path=<file>` or the Debug tab's Play recording page. Link is taken to the recording's age and map and put at its
     start first, so it plays from wherever the game is. Wait on `status`'s `recorder` until `mode` is `off` again;
   - **one made from the start** (`HOS_INPUT_RECORD`, a route): `HOS_INPUT_PLAY=<file>` from the same save. It starts at
     Link's first update after the save's load, as the recording did, with nothing moved: the exact way. A recording
     made in the game, played from the switches, starts where the save puts Link, not where it began, and diverges.
3. **Read how it went.** The log's `input: playback over at frame N of N` line and the `HOS_INPUT_SYNC` line after it:
   how many updates were apart, the corrections, the largest. `input: the replay diverged at frame N` means the run went
   another way (a hit, a door missed) and nothing after it is the recording's: the result is not a test of the change.
   For a number a script reads, record the replay too (`HOS_INPUT_RECORD=<other file>` beside `HOS_INPUT_PLAY`): its
   last line is `{"cmd":"result",...}`.

| Run | Drift correction | Use |
|---|---|---|
| A test of a fix or a picture along a route | on (`HOS_INPUT_SYNC=1`, the default) | Link and the camera follow the route; small float drift is put back, a divergence still fails |
| Whether the game runs the same twice | off (`HOS_INPUT_SYNC=0`) | the first update apart is the first place the game went its own way |
| A measurement over a route | `dev.py ab --route <route>` ([measure](#measure-a-change)) | A and B on the same route; a run that diverged or missed a load is refused |

A recording other than a route holds the game's memory words: it stays on the PC, like a save (not in a commit, a pull
request, a comment or a test request). Write in the pull request what the recording does (where, how long, its loads)
and what the replays logged, not the file.

### Memory tour

`scripts/memtour.py <game folder> --save <save folder>` (Linux) starts the game headless through the control bridge,
loads the save, teleports through areas (`--dests`, `--dwell`) and samples the game's memory, swap and the renderer's
`vk: budget:` line every 2 s into `mem.csv`, `marks.csv` and `summary.md`. Use it for "the game outgrows my RAM"
reports and A/B runs of a memory change ([memory budget](reference/architecture.md#memory-budget)).

## Debug

| To find | Use |
|---|---|
| A misused Vulkan call | `RX_VK_VALIDATION=1` (the Khronos layer); the other GPU switches are in [switches](reference/switches.md#gpu-debugging) |
| Which pass made a flash | `RX_VK_FRAMELOG=<file>` while `<file>.go` exists, then `python -I scripts/flash.py framelog <file>` finds the flashing frames; `RX_VK_PROBE=1` captures the centre texel after every pass at a flash, and `python -I scripts/flash.py probe <run>/probe_<frame>.txt` names the pass that went wrong. `RX_VK_FLASH=1` with `RX_VK_SHOTS` and `RX_VK_SHOT_EVERY=1` keeps the frames around a flash |
| What one frame draws, step by step | `RX_VK_DUMP=<present>[+<count>]` (PNGs under `RX_VK_DUMP_DIR`; narrow with `RX_VK_DUMP_STEPS`, `RX_VK_DUMP_PROGS`), `RX_VK_PASS_TRACE=1` |
| Whether a draw matters | `RX_VK_SKIP_PROG=<prefix>,...` or `RX_VK_SKIP_CODE=<prefix>,...` leaves those programs' draws out |
| A shader edit without a rebuild | `RX_VK_SHADER_DIR=<folder>` loads `<folder>/<name>.spv` over a built-in shader (`glslangValidator -V <source> -o <folder>/<name>.spv`) |
| A break of the memory-sync rules | `RX_VK_MEM_CHECK=1` ([the memory check](reference/architecture.md#the-memory-check)) |
| The game's state while it runs | The control bridge's `status`, `watch` and `mem read` ([control bridge](reference/control-bridge.md#the-client)) |
| Screenshots on a timer | `RX_VK_SHOTS=<dir>` every `RX_VK_SHOT_EVERY` presents (300) at `RX_VK_SHOT_W` x `RX_VK_SHOT_H` (1280x720) |

`RX_VK_DUMP_FAILED=<dir>` writes the native code of every program that does not translate, for `nx2/lower_test.py`
(game data: keep it local).

## Measure a change

A change's Measurements are `main` against the change, on the same PC, save, scene, settings and build type, at least
two interleaved runs each after a warm-up, median reported ([standards](standards.md#measurements)). `dev.py ab` does
all of it in one command ([measure skill](../../.claude/skills/measure/SKILL.md)):

```sh
python -I scripts/dev.py ab --scene market                     # this checkout's commit against its base (main)
python -I scripts/dev.py ab --env RX_VK_ASYNC=0                # a switch: set against unset, one runtime, no build
python -I scripts/dev.py ab --a HOS_OS_YIELD=0 --b HOS_OS_YIELD=1 --runs 4 --seconds 90
python -I scripts/dev.py ab --scenes                           # the named scenes
python -I scripts/dev.py ab --env RX_VK_ASYNC=0 --dry-run      # the plan, nothing run
```

| Step | What `dev.py ab` does |
|---|---|
| Build | A change: `dev.py base` when missing, `dev.py pack` (only what changed), `rt` and `rt --empty`, each runtime kept while it matches its base and pack. A switch: no build, `rt-<checkout>` (else `rt-base`, or `--runtime`) |
| Start | Each run is one `timing` job of the queue; the game starts once, from the scene's save in a user folder of the run's own, with the title skipped (the Skip title screen row) |
| Scene | `--scene`: a repository save, then the Debug tab's age switch and teleport (and the hour); `--save FOLDER` starts from your own save |
| Settings | One pinned options file (`--options FILE`, else the defaults) copied for every run; `HOS_FPS_MAX=1000`, V-Sync and frame generation off unless `--vsync` / `--framegen`; the Render resolution at 67 % where the file leaves it at Auto, unless `--dynamic`; `--set NAME=VALUE` for both sides |
| Render resolution | Auto is the game's dynamic resolution: it sizes the 3D to the GPU's load, so a GPU saving shows as a larger picture, not as frames, and each run settles at a size of its own. One switch came out +6.0 % and then -6.5 % at Auto (Hyrule Field at 4K, RTX 3090), +1.1 % at 67 %. `--dynamic` keeps Auto, to see what a player on Auto gets |
| Warm-up | `--warmup` seconds at least, then until no frame waited for a pipeline or a shader for 5 s (120 s at most) |
| Measure | `--seconds` (60) from the frame trace, `--runs` (3) pairs interleaved A, B, A, B. A switch whose reader takes `HOS_AB` is flipped within each run instead ([below](#an-ab-within-one-run); `--separate` to not) |
| Check | Refuses to write up runs that differ in a menu setting, a switch, the renderer or the window, apart from what is compared (`--allow KEY` for a row the change adds) |
| Write up | The Measurements section is printed and saved in `runs/ab/` of the development folder (`dev.py paths runs`). Separate runs report each run's fps, 1% low, frame times and hitch count, the medians and observed fps ranges; their verdict cannot distinguish noise with one run a side. In-run comparisons report pooled frame times and hitches per minute of retained steady frames on each side, with a verdict based on an interval across periods; even one launch can provide enough periods. `--threads` is temporarily unavailable because CPU samples cannot yet be matched safely to the measured frames. The timing line reports preparation, the run batch and median run phases, including copy and analysis after stopping; phases (including cleanup on failure) are also in `results.json`. Exit status 1 when a run failed, 3 when the sides differed |

A route that moves: `dev.py ab --route <route>` plays a route of `assets/routes/` in each run and measures while it plays
(`--sync N`, 1 by default), from the route's save; a run whose replay diverged or missed a load is refused
([replay a recording](#replay-a-recording)).

What it can't do, by hand: frame pacing on the screen (PresentMon during the same route), a profiled run first ([profile](#profile)), and an older base (build the merge base
as a pack and compare the two packs). Run each side as `dev.py queue run NAME --mode timing -- <your run>`, with
`HOS_OPTIONS_FILE` pinned and each log's `vk: switches set` line read back.

### An A/B within one run

Two runs of one build can differ by a few fps, more than many changes are worth. `HOS_AB` measures both sides of a
switch in one run ([`host/hos_ab.c`](../../host/hos_ab.c)):

1. Set, in the environment or `longshot_env.txt`:

   ```sh
   HOS_AB=<switch>=<value>[,<seconds>[,<warm-up seconds>]]   # e.g. HOS_AB=HOS_COLLECT_WIDE=0,3,60
   RX_VK_FRAME_TRACE=1
   ```

   After the warm-up (20 s from process start by default; cover boot, save load and scene settling), the switch
   alternates every period (3 s) between its normal value (side 0) and `<value>` (side 1). Slow drift falls on both
   sides alike.
2. Read it: `python -I scripts/perf.py ab <game folder>/logs/longshot_frames.csv [more runs' traces]`. It prints each
   side's frames, mean, median, p99, share of frames of 16.7 ms or more and fps, then side 1 less side 0 with a 95 %
   bootstrap interval and whether it holds 0. The log's closing `ab:` line says if the switch was never read.

Only switches whose reader calls `hos_ab_value` at each use take part: their rows in
[`switches.def`](../../gpu/vk/switches.def) say "HOS_AB may flip it", and
[`tests/test_hos_ab.py`](../../tests/test_hos_ab.py) lists them. Wire another only if it can change between two calls
without breaking what the other value built.

### Measure the build time

A change to the lifter, the compile flags, the shader preparation or `tools.lock` measures the players' build time
([standards](standards.md#performance-and-build-changes)); so does each stable, on its nightly. From a clean build:

1. Use fresh folders so nothing is reused: `XDG_DATA_HOME`, `XDG_CACHE_HOME` and an empty
   `__GL_SHADER_DISK_CACHE_PATH`.
2. Download the release's (or your build's) AppImage and `SHA256SUMS`, check them
   (`python -I ../Tooling/scripts/release.py verify SHA256SUMS --files .`, Tooling's release tool, with `SHA256SUMS.sig` beside it), and extract it
   (`--appimage-extract`).
3. From `squashfs-root/usr/share/longshot`, run `../../../AppRun --cli --nup <your .nup> --out <new folder> --language
   en-US --compile-shaders most`, timestamping its output.
4. Read the times: tools from the start to `@@stage read`; read, lift, compile and finish from the `where the time went:`
   table; shader preparation from `@@stage shaders` to `@@done`.

For a change, do the same with `main` and the change on the same PC (`dev.py game --timing` through the queue's
`timing-cpu` mode), and note the date, the commits and the PC with the numbers. The reference numbers are in
[build times](reference/build.md#build-times).

## Profile

Profiling finds what to change; measure the change without it, since every profiling tool costs frame rate. Which tool
answers which question, the switches and the readers: [profiling](reference/profiling.md).

### Profile a run

1. Set `HOS_PROFILE=1` (environment or `longshot_env.txt`) and play the scene. The log's
   `env: HOS_PROFILE=1: a profile of this run in ...` line names the folder, `logs/profile-<date>-<time>/`.
2. Read it: `python -I scripts/perf.py report <profile folder> [--from-s 60] [--to-s 300]`. It writes `report.md`
   (start at **At a glance**), `trace.json.gz` for [Perfetto](https://ui.perfetto.dev) and `speedscope.json` for
   [speedscope](https://www.speedscope.app); both viewers read the file locally.
3. For a closer look, `scripts/perf.py threads`, `renderer`, `stacks` and `chain` read the samples and recordings
   (`scripts/perf.py --help`). On Windows, pass `--maps <folder>` with the build's `.map` files to name frames.

### Tracy

1. Build with Tracy over your base:

   ```sh
   LONGSHOT_TRACY=1 python -I scripts/dev.py pack
   python -I scripts/dev.py rt
   ```

   or a whole game folder with `LONGSHOT_TRACY=1 python -I scripts/dev.py game`. A Tracy pack always builds the
   game, where Tracy's client is, also from an unchanged tree. The lifted game code is not recompiled, except the
   chunks with listed `calls` functions.
2. Get the Tracy 0.15.0 tools; the protocol is locked to the version. On Linux, `mise run tracy-tools` builds
   `tracy-capture` and `tracy-csvexport` into your development folder's `tools/tracy-0.15.0/bin` (needs cmake, ninja, a
   C++ compiler; `python -I scripts/dev.py paths tools` prints the folder). Build or install the viewer yourself, at
   0.15.0.
3. Capture, starting before the game to include start-up:

   ```sh
   timeout -s INT 30 tracy-capture -a 127.0.0.1 -o build-linux/run.tracy   # or Ctrl-C to stop and save
   tracy -a 127.0.0.1                                                      # or the viewer, live
   ```

   Keep `.tracy` files in an uncommitted folder such as `build-linux/`. `tracy-csvexport run.tracy` gives each zone's
   count and times; `-u`, every zone on its own line with its thread and text. `TRACY_PORT=<n>` in the game's
   environment and `-p <n>` give a second game its own port (8086 by default).

The zones, the game's own functions and how to add a zone: [profiling](reference/profiling.md#tracy).

## Record the renderer's play profile

Every build compiles the renderer with `assets/profiles/longshot_vk.profdata.xz`. Record a new one before a minor's
stable when the renderer changed much, so the nightly testers play has it:

1. Build a counting renderer: `RX_VK_PGO=gen python -I scripts/dev.py packs pack <base> vk-count`. It is slower: never
   measure it.
2. Record: run the game with `RX_VK_PGO_FILE=<file>`; create `<file>.reset` once the place has loaded and `<file>.dump`
   when done. Or let the pipeline tour play the route: `pipeline_tour.py tour ... --vk-profile <dir>`
   ([pipeline states](reference/pipeline-states.md#the-pipeline-tour)).
3. Merge with the same compiler's `llvm-profdata merge`, or `python -I scripts/pipeline_tour.py merge-profile <dir>
   [--publish]`.
4. Ship it in a pull request: `assets/profiles/longshot_vk.profdata.xz` (xz -9e) and its note in
   `assets/profiles/README.md`.

A profile holds function names, shape hashes and branch counts of Longshot's own renderer code, nothing of the game.

## From claim to merge

The same steps for people and agents. Forge commands are Tooling's `forge.py`
([docs/forge.md](https://git.magicbean.dev/MagicBeanDev/Tooling/src/branch/main/docs/forge.md)).

1. **Pick** from the [open issues](https://git.magicbean.dev/MagicBeanDev/Longshot/issues), highest severity first. The
   team's triage bot labels each new issue: `regression` first, then `priority/critical`, `priority/high`,
   `priority/low` (`forge.py issue list --label priority/critical`), and take one nobody has claimed. Where the bot left none, score a problem as
   **severity x reach x confidence**: severity S0 (8: security, crash, device lost, lost saves, the builder can't
   build), S1 (5: can't start or progress), S2 (3: wrong but playable, a frame-rate loss over 20%), S3 (1: polish);
   reach is the share of players on the affected hardware; confidence is 1.0 reproduced with a log, 0.7 two reporters
   with logs, 0.4 one report or no log. A security scan result comes before anything else. Mind the
   [hardware tiers](standards.md#hardware-tiers).
2. **Look for prior work**: `forge.py search --state all <term>` over open, merged and closed pull requests and issues
   (agents: the [prior-work skill](../../.claude/skills/prior-work/SKILL.md), which also covers Hookshot). An open pull
   request that already does it is its author's: help it land rather than writing a competing one.
3. **Claim** it in the team room with the
   [team-updates skill](https://git.magicbean.dev/MagicBeanDev/Tooling/src/branch/main/skills/team-updates/SKILL.md) (or `team_chat.py`): read
   the claims first, then post what, why (the score) and the hardware you can test on, there and on the issue. Say when
   you drop a claim.
4. **Change** it on a branch from current `main` (`fix/...`, `linux/...`, `builder/...`, `docs/...`), cause before fix,
   pushed to your fork (`forge-fork`). The flow with git and jj is Tooling's
   [A change, start to finish](https://git.magicbean.dev/MagicBeanDev/Tooling/src/branch/main/docs/forge.md#a-change-start-to-finish).
5. **Test** it: the checks, then the game on your own PC where the change matters.
6. **Open it as work in progress**: `forge.py pr create <branch> --title "fix(scope): ..." --tested-in-game
   "where it ran and what you saw"`. It starts the description from the [template](standards.md#the-description)
   with a `WIP:` title.
7. **Measure** it and fill in the description. After each push, measurement or test run, `forge.py pr update` edits
   the description and posts a comment saying what changed. Rebase when `main` moves.
8. **Mark it ready** (`forge.py pr ready`) once [Ready for review](standards.md#ready-for-review-out-of-wip) holds;
   `pr status` shows where it stands, `pr wip` puts it back.
9. **Hardware you don't have:** ask the Test Farm on the pull request, `@Beanstalk <what to test, in words>`; a BigBean
   approves it. What it can't do (force-close the game, keep state across starts, judge by eye) goes in *Still to
   test* for a person ([how it works](https://beanstalk.magicbean.dev/how)).
10. **Review:** reviewers take ready pull requests, never WIP ones
    ([Reviewing a pull request](https://git.magicbean.dev/MagicBeanDev/Tooling/src/branch/main/docs/forge.md#reviewing-a-pull-request);
    agents: `/magicbean:review`). Answer every comment, and rebase before asking again.
11. **Merge:** once [Ready to merge](standards.md#ready-to-merge) holds, a maintainer squash-merges (`pr merge`). The
    merge makes the next nightly ([releases](reference/releases.md)). Once a release carries the fix, ask the reporter
    to retest, then close the issue.

## Tools

| Tool | What it does |
|---|---|
| `scripts/dev.py base`, `pack`, `rt`, `bases`, `game`, `vk`, `env` | Shared bases, packs, runtimes and builds through the queue; `env` prints a player's build environment |
| `scripts/dev.py ab` | A switch or this checkout's change against main in the game, written up ([measure a change](#measure-a-change)) |
| `scripts/dev.py queue` | The PC's job queue: `run`, `status`, `history`, `cancel` |
| `scripts/dev.py update` | A game folder you play kept on main's newest commit, `--gui` in a window ([keep a game on main](#keep-a-game-on-main)) |
| `scripts/dev.py packs` | A base folder of your own: `base`, `pack`, `rt`, `warm` |
| `scripts/dev.py paths`, `migrate` | The development folder and its parts; the once-per-PC move into it ([where dev data lives](reference/build.md#where-dev-data-lives)) |
| `tests/run_all.py` (`mise run test`) | Every game-free check; `tests/srccut.py` cuts source for a check |
| `tests/run_vulkan_fixtures.py` | Software-Vulkan fixture cases |
| `scripts/longshot_ctl.py` | The control bridge's command line; its client library is `scripts/longshot_bridge.py` ([control bridge](reference/control-bridge.md)) |
| `scripts/run-vk.ps1` | A Windows test run with screenshots |
| `scripts/memtour.py` | Memory tour |
| `scripts/flash.py framelog\|probe` | Find a flash in a frame log; name the pass from a texel probe |
| `scripts/perf.py report\|threads\|renderer\|stacks\|chain\|hotlist\|cpu\|ab` | Read profiles, samples, recordings and A/B traces ([profiling](reference/profiling.md#tools)) |
| `scripts/collect_bench.py [--cc clang] [--reps N]` | The write marks' walk (`collect_in`, `host/rx_backend.c`) timed game-free, as the word walk and as the vector walk (`HOS_COLLECT_VECTOR`), on mark arrays shaped like the game's: an indication only, warm caches on one thread |
| `scripts/pipeline_tour.py` | The pipeline tour and the play profile's merge, its parts in `scripts/tours/` ([pipeline states](reference/pipeline-states.md)) |
| `scripts/shaders.py crt\|fsr3\|host\|all [--check]` | Regenerate (or check) the checked-in SPIR-V headers; `spirv-operands` regenerates `gpu/vk/spirv_operands.h` |
| `scripts/check_third_party.py` | `third_party/` against its provenance |
| `scripts/check_pr_description.py --file F` / `--pr N` | The pull request description check |
| `../Tooling/scripts/forge.py`, `team_chat.py` | The forge and the team chat ([setup](setup.md#2-get-the-teams-tooling)) |
