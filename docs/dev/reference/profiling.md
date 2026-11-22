# Profiling

How to find where Longshot's time goes: the profiling mode, Tracy, the samplers and traces, and the scripts that read
them. To measure a change against `main` once you know what to change, follow the
[measure skill](../../../.claude/skills/measure/SKILL.md); this page is about finding what to change.

**Profiles never leave your PC.** A sample file, a profile folder, its `report.md`, a Perfetto or speedscope trace, a
`.tracy` capture and a screenshot of any of them name the game's functions by guest address: they are made from the
game ([CONTRIBUTING.md](../../../CONTRIBUTING.md#the-rule-with-no-exceptions)). Share what you concluded, in words and
numbers.

## Which tool for which question

| Question | Use |
|---|---|
| Where does a whole run's time go, and what was each stutter? | [The profiling mode](#the-profiling-mode) (`HOS_PROFILE=1`) |
| Every thread, wait, lock and renderer stage on one timeline | [Tracy](#tracy) (`LONGSHOT_TRACY=1` build) |
| Which thread or wait actually holds the frame? | `HOS_CHAIN` ([switches](#switches)), read with `scripts/perf.py chain` |
| What do the renderer's own threads spend time in? | `HOS_SAMPLE_ALL`, read with `scripts/perf.py renderer` |
| What happened in the rare frames over 50 ms? | `HOS_SLOW_FRAMES` (the slow-frame recorder) |
| GPU time by pass and program | `RX_VK_PASS_TIMING=1` (or `2`), in the log |
| What each program's draws produce on the GPU in one frame | [Draw statistics](#draw-statistics) |
| Is switch A faster than switch B, within the noise? | [An A/B within one run](../developing.md#an-ab-within-one-run) (`HOS_AB`) |
| Where CPU time goes by category, for a publishable report | [CPU profile](#cpu-profile) |
| How does Longshot run on players' PCs? | [Performance statistics](#performance-statistics) |

Every profiling tool costs frame rate. Use them to find what to optimise, then measure the change without them.

## The profiling mode

`HOS_PROFILE=1` (in the environment or `longshot_env.txt`) turns on the samplers, traces and timing logs for one run and
records them in `logs/profile-<date>-<time>/` in the game folder ([`host/profiling.c`](../../../host/profiling.c)). The
log's `env: HOS_PROFILE=1: a profile of this run in ...` line names the folder. It is also what
[Report slow performance](../../user/performance-report.md) asks players to turn on.

It sets these defaults; a switch you set yourself keeps your value (`RX_VK_PASS_TIMING=0` leaves the pass timing out):

| Switch it sets | Records |
|---|---|
| `HOS_SAMPLE_ALL=<folder>/all.txt`, `HOS_SAMPLE_PACKED=1` | `all.txt`: every game and renderer thread's stack every 2 ms, packed (each distinct stack once). Delete `all.txt.go` to pause, recreate it to resume |
| `HOS_PRESENT_TRACE` | `presents.csv`: the game's presents |
| `RX_VK_FRAME_TRACE=1` | `frames.csv`: the frame trace (below) |
| `HOS_PROFILE_EVERY=5`, `HOS_THREAD_TIMES=5` | busiest threads and each thread's CPU, in the log every 5 s |
| `HOS_FPS_LOG=1` | the frame limiter, the game's pacing mode (`+0x24f`) and the game clock's speed, in the log |
| `RX_VK_PASS_TIMING=1` | GPU time per pass, in the log every 600 presents |
| `RX_VK_SLOW_LOG=50` | the replay's times for each frame over 50 ms, in the log |

The folder also gets `cpu.csv` (the process's CPU and memory, and the system's available memory, every second),
`events.txt` (each wait of `HOS_VCORE_LOG_MS`, default 50 ms, or more for a virtual core, with its cause), and
`process.json` plus `longshot_options.ini` (the run's switches and settings). All times are on the host's clock
(`hos_now_ns`), so samples, frames, presents and events line up.

Read it:

```sh
python -I scripts/perf.py report <game folder>/logs/profile-<date>-<time> [--from-s 60] [--to-s 300]
```

This writes `report.md` into the folder. Start at **At a glance** (frame rate, what limits the frame, the hottest code
on the main and `Presentation_Thread` threads, what stutters are spent on, memory). The other sections break the run
down over time, by frame-trace column, by thread, by function, stutter by stutter, and by GPU pass (the log's last
pass-timing block). `--from-s` leaves out loading and the first shader preparation.

It also writes two files for browser viewers (both read the file locally, nothing is uploaded):

- `trace.json.gz` for [Perfetto](https://ui.perfetto.dev): the run as a timeline, with each thread's stack as a flame
  chart, a Frames track (stutters named `STUTTER`), long virtual-core waits, and counters for frame time, cores and
  memory. Select a stutter to see what every thread ran in it.
- `speedscope.json` for [speedscope](https://www.speedscope.app): flame graphs per thread and a self/total table
  ("Sandwich"). Stacks are summed, so its "Time Order" is not the run's order.

Traces are several MB a minute; `--from-s`/`--to-s` keep the part that matters, `--no-trace` writes none.

**Symbols.** The scripts name frames from the same build: `--maps <folder>`, else the game folder the profile is in
(Linux: `longshot` and `longshot_vk.so` beside it), else `RX_MAPS=<folder>` (Windows: a folder with that
build's `longshot.map` and `longshot_vk.map`). Without them frames stay `module+offset`.

**Cost.** The sampler interrupts every thread every 2 ms, and pass timing makes the GPU finish each pass before the next.
Read the GPU numbers as shares, not as an unprofiled frame's time, and never take frame-rate numbers from this mode.

## The frame trace

`RX_VK_FRAME_TRACE=1` writes `logs/longshot_frames.csv` (or `frames.csv` in the profile folder), one line a present:

- what the frame waited for: pipelines, shader translation, uploads, the memory sync, the GPU, read-backs, the capture;
- the FPS counter's Breakdown: `game_ms`, `render_ms`, `gpu_wait_ms`, `pace_ms`, `wait_render_ms`, `wait_gpu_ms`,
  `wait_main_ms`;
- the game's main thread in the frame: on the CPU (`main_cpu_ms`), blocked (`main_blocked_ms`), waiting for a virtual
  core (`main_vcore_ms`); the rest is time runnable but off the CPU;
- `at_ns` (the present on the host's clock) and `ab_side` ([an A/B within one run](../developing.md#an-ab-within-one-run)).

Texture step columns `create_ms`, `reuse_ms`, `astc_stage_ms`, `astc_record_ms`, `astc_view_ms`, and `astc_flush_ms` are CPU wall-time deltas in milliseconds since the preceding present; `astc_flushes` and `create_n` are count deltas. View and flush time are excluded from record time. Pool columns `pool_reused` (exact images), `pool_resized` (new images on pooled memory), `pool_new` (new allocations for pool-eligible images), `pool_put`, `pool_idle`, and `pool_destroyed` are count deltas. `pool_holds` and `dead_waiting` are image-count snapshots, and `compact_pass` is a 0/1 snapshot.

`RX_VK_FRAME_LOG=<csv>` is a lighter alternative: frame, seconds and frame time per present (it works headless too).

## Reading the renderer's log lines

- `vk: <n> presents, <fps> fps; a frame: ...` gives draws, dispatches, passes, submits and uploads a frame. Its
  `GPU-bound wait` is **recording-slot reuse only**, not a test of whether the GPU is the bottleneck.
- `vk: host time in GPU resource waits` sums host time inside Vulkan fence, timeline, query and idle waits
  (capture late pages, read-backs, recording fences, sparse binds); pacing and the driver blocking in submit or present
  are not in it. It is thread wall time: it can overlap across threads and exceed
  a frame. A low value does not prove the GPU is idle; read it with GPU timestamps, present intervals and CPU profiles.
- The pass-timing block's `by kind` lines group spans by their label's first word. Where task and mesh programs run as
  compute shaders (`vk: mesh shaders:` says the compute path), the work before each fallback draw is kind `fallback`
  (`fallback prog <key>` among the labels), and an indirect mesh draw's argument conversion is kind `meshconv`: neither
  is counted in `dispatch` or `barrier`.
  With `RX_VK_PASS_TIMING=2`, each 600-present report selects its two most expensive fallback programs for the
  next window. Their `fallback-stage` lines break the parent time into buffer preparation (including the wait
  for the previous draw), the optional task dispatch, and the mesh dispatch. Each dispatch includes its following
  barrier. These lines are subdivisions of the existing fallback total.

## Switches

The profiling switches beyond those the profiling mode sets. Defaults and the full list:
[switches](switches.md) and [`switches.def`](../../../gpu/vk/switches.def).

Switches that name a file (`HOS_SAMPLE_FILE`, `HOS_SAMPLE_ALL`, `HOS_SLOW_FRAMES`, `HOS_CHAIN`, `HOS_PRESENT_TRACE`,
`RX_VK_FRAME_LOG`) take their value from the environment only, never from `longshot_env.txt`.

| Switch | What it records |
|---|---|
| `HOS_SAMPLE=<thread>` + `HOS_SAMPLE_FILE=<file>` | One thread's stacks every millisecond (needs `HOS_PROFILE_EVERY`) |
| `HOS_SAMPLE_ALL=<file>` (+ `HOS_SAMPLE_PACKED=1`) | Every game and renderer thread every 2 ms while `<file>.go` exists |
| `HOS_SLOW_FRAMES=<path>` (+ `HOS_SLOW_FRAME_MS`, `HOS_SLOW_FRAMES_EVERY_MS`, `HOS_SLOW_FRAMES_MAX`) | All threads sampled every 4 ms into a ring; a frame 50 ms or more after the one before writes its samples and the 250 ms before to `<path>-<n>.txt` (40 files a run at most). Not with `HOS_PROFILE=1` or `HOS_SAMPLE_ALL` ([`host/slow_frames.c`](../../../host/slow_frames.c)) |
| `HOS_CHAIN=<file>` | While `<file>.go` exists, every guest wait that blocks, every signal that can end one, every sleep and present; written when `<file>.go` goes away or its 64 MB buffer fills ([`host/hos_chain.c`](../../../host/hos_chain.c)) |
| `HOS_HLE_STATS=1` | Calls of each HLE function and host call, about every 5 s (`hle:` lines). Take performance numbers from a run without it |
| `HOS_PROFILE_EVERY=<s>`, `HOS_THREAD_TIMES=<s>` | Busiest threads; threads over 5 % of a core |
| `HOS_FPS_LOG=1`, `HOS_PRESENT_TRACE=<csv>` | Frame limiter and game clock speed (1.000 is real time); one CSV line per present with host-clock (CPU, not scanout) times |
| `RX_VK_PASS_TRACE=1` | The passes of the last frames and why each ended, including guest barrier flags. A depth attachment cleared on load shows `load=CLEAR`. Depth-clear candidates report the next guest opcode (the `NOP_*` enum in `gpu/render/rx_nvn.h`, or -1 at a replay boundary), masks and eligibility. A full depth copy can omit the preceding clear when adjacent or when separated by one ordinary timestamp report, optionally followed by a redundant `RENDER_ENABLE(1)`. Across a timestamp, both textures and the report must have disjoint contiguous backing. When a texture copy ends a depth-only clear pass with no draws, the numbered `depth clear interval` lines list every guest packet after the first clear through the copy. They include counter types and addresses, render-enable values and barrier flags. Texture copy requests show both image IDs, guest addresses, subresources and regions in replay order. Logged at a flash or a dark frame (`RX_VK_PASS_TRACE_AT=<n>`: every n-th present) |
| `RX_VK_LOG_RAM=1` or `=2` | Linux: where the RAM goes, log lines only, from a thread of its own at the lowest priority (`gpu/vk/ram_census.h`). Every 30 s, `vk: RAM:` the machine (`/proc/meminfo`): available, free, the page cache and its shared memory, anonymous memory, the kernel's, swap used, and with zram the data swapped there, its compressed size and the RAM it takes; `vk: RAM, this process:` resident by kind and in swap (`/proc/self/status`), the device's memory placed under the game's pools and the pools left in ordinary memory (`RX_VK_PLACED_LATER_MB`), the DRM driver's own count of its memory (`drm-total-`, `drm-resident-` in fdinfo, where the kernel gives it) and the device heap as the renderer sees it; `vk: RAM, the other programs:` the eight that take the most, by name (Steam's web helper is many processes), resident and in swap. Every 2 s, when it moved by 32 MB or the pipelines made by 250 (or a minute on), `vk: RAM, the driver:` the driver's memory by client, how much of it is outside the device heap (the driver's own: pipelines' code and state, command streams, descriptor pools), the pipelines made and linked from libraries, the textures, and since the last line the KB outside the heap per pipeline made. `=2` also splits the process's memory at the game's (`/proc/self/smaps`: the game's memory resident and in swap, the rest of the process apart): a read walks every mapping, 20 to 36 s on the AYN Odin 3. There Kokiri Forest left 340-540 MB available with 6 GB in swap; the driver held 7.4 GB the kernel cannot swap, 3.1 GB of it outside the device heap |
| `RX_VK_LOG_PIPELINE_RAM=1` | Linux: what each pipeline costs the driver in memory of its own, outside its device heap: the driver's own count (the DRM file's fdinfo) less the heap's (VK_EXT_memory_budget), read before and after the pipeline is made, the pipelines made one at a time meanwhile (loading is slower with it; the textures and buffers other threads make meanwhile are in the heap, left out). `vk: RAM, a compute pipeline took N KB` for each of 1 MB or more (the first 300, and every one of 8 MB or more), with the thread, the program it is of (the game's compute program, a task or mesh program's compute fallback, or none named: the renderer's own) and its shaders (stage, SPIR-V size and hash, the private memory an invocation asks for, shared memory, workgroup: `gpu/vk/spirv_shape.h`); `vk: RAM, pipelines weighed:` every 1000, by size; `vk: RAM, program ...: the game's first draw` (or dispatch) `with it` for a program whose pipeline took 8 MB or more (none: play never needed it); with `RX_VK_LOG_RAM` the driver line says how much the pipelines weighed took. On the AYN Odin 3 one compute pipeline of the title screen took 1.5 GiB, 373 MB another: Turnip's private memory, one buffer for the device sized for the hungriest shader so far times every invocation the GPU holds, never shrunk |
| `RX_VK_LOG_BIG_POOLS=1` | Where the guest's pools are placed in the device's own memory (Turnip on the AYN Odin 3: no host pointers, no udmabuf), the placements of 64 MB or more followed, log lines only (`vk: big placement`): as each is made, MemAvailable before its memory is allocated, once it is, and once its contents are copied in and it is placed; the GPU's bindings in it, read and written (`gpu_buffer_bind`), and the textures' GPU reads (`gpu_buffer`), counted with the 1 MB stretches of it they reach; and every 60 s, from a thread of its own at the lowest priority, which of its 64 KB blocks hold data (read 8 MB at a time under the placements' lock: the game's thread waits that long at most to make or let go of a pool), how much of it the CPU had mapped in before that read, and a line with the sum and MemAvailable. Each line ends with a map of the placement in 64 characters: `W` bound for the GPU to write, `R` bound for it to read, `T` read by the textures' GPU path, `d` data the GPU did not reach, `.` none of these. A placement given back, or taken into a larger one, gets its line then. To tell whether a big pool can be placed in part, or later (`docs/arm64-port-plan.md`) |
| `RX_VK_PASS_TIMING=1` or `2` | GPU time per pass (`2`: per program too), logged every 600 presents. The GPU's ASTC decode is a span a texture, kind `astc`, in four rows: decoded only or re-encoded to BC too, and whether the upload budget can put it off (`RX_VK_UPLOAD_BUDGET_MS`) or never does (a layer array, or over 1024x1024) |
| `RX_VK_SLOW_LOG=<ms>` | The replay's times for each frame over `<ms>` |

## Draw statistics

`RX_VK_DRAW_STATS=<folder>`: create `<folder>/draw_stats.now` while the game runs and every draw of the next frame runs
inside a pipeline-statistics query (vertex, task and mesh invocations, primitives into and out of clipping, fragment
invocations). Eight frames later `<folder>/draw_stats_<frame>.txt` gets one line per program, with programs that run
but output nothing first, plus `code` rows with each program's native stage keys. `RX_VK_DRAW_STATS_PROGS=<key>,...`
also dumps those programs' uniform and storage buffers. The queries perturb frame timing
([`gpu/vk/vk_draw.cpp`](../../../gpu/vk/vk_draw.cpp), `RX_VK_DRAW_STATS`).

## Tracy

A build with `LONGSHOT_TRACY=1` carries the [Tracy](https://github.com/wolfpld/tracy) 0.15.0 client
([`third_party/tracy`](../../../third_party/tracy)): every game and renderer thread, the host's waits, hand-offs and
locks, the NVN calls and the renderer's stages on one timeline, with a frame mark at each game present. A build without
it compiles no Tracy code: the macros in [`host/hos_tracy.h`](../../../host/hos_tracy.h) are empty.

The client listens on 127.0.0.1 only, announces nothing, sends no machine code, and records only while a viewer or
`tracy-capture` is connected (`TRACY_ONLY_LOCALHOST`, `TRACY_NO_BROADCAST`, `TRACY_NO_CODE_TRANSFER`,
`TRACY_ON_DEMAND`; set in [`nx2/build.py`](../../../nx2/build.py)).

### Build and capture

How to build a Tracy game and capture a run: [developing](../developing.md#tracy).

On Linux, Tracy's sampler runs at 10 kHz; `TRACY_SAMPLING_HZ=<n>` sets the rate, `TRACY_NO_SAMPLING=1` turns it off.

On Windows, `longshot_vk.dll` links against the executable's import library (`longshot_tracy.lib` in the work folder),
and the renderer's zones reach the capture through it. Tracy's client asks for its system libraries by
`#pragma comment(lib, ...)` lines under `_MSC_VER`, which llvm-mingw's clang doesn't define, so `nx2/build.py` links
`ws2_32` and `secur32` itself ([`tests/test_tracy_win_libs.py`](../../../tests/test_tracy_win_libs.py) checks every
library those lines ask for is linked). Sampling and context switches on Windows go through Event Tracing for Windows,
which Tracy's manual says needs an administrator: untried; run as a user with `TRACY_NO_SAMPLING=1`.

### The zones

Waits are dark red, contended locks bright red; a wait's text has the object's guest address and the guest caller.

| Zones | Source |
|---|---|
| frame marks | `nvnQueuePresentTexture` |
| `event wait`, `semaphore wait`, `queue receive wait`, `WaitAny`, `cv wait`, `mutex wait`, and their signal sides | [`host/nn_os_sync.c`](../../../host/nn_os_sync.c) |
| `kernel wait`, `sleep`, `YieldThread`, `vcore wait`, `vcore yield`, `asleep`, `thread join` | [`host/hos_thread.c`](../../../host/hos_thread.c) |
| `lock wait` (text names the lock) | host locks in `hos_thread.c`, `nn_os_sync.c`, `nvn_native.c`, `hos_svc.c`, `hos_mem.c` |
| `nvnQueueSubmitCommands`, `nvnQueuePresentTexture`, `nvnSyncWait`, ... | [`host/nvn_native.c`](../../../host/nvn_native.c) |
| `render job`, `record_segment`, `submit_recording`, `present_*`, `recording_wait`, `sync_drain`, ... | `gpu/vk/vk_replay.cpp` |
| `draw`, `build_descriptors`, `bind_descriptors`, `program_for` (per draw; off with `RX_VK_TRACY_DRAWS=0`) | `gpu/vk/vk_draw.cpp` |
| `memory_sync`, `take_capture`, `mirror_sync`, ... | `gpu/vk/vk_memory.cpp` |
| `texture create`, `texture upload*` | `gpu/vk/vk_texture.cpp` |
| `build_graphics`, `gpl_link`, `cached_*`; `worker job`, `serial build` | `vk_draw.cpp`; `vk_jobs.cpp` |

Plots: `frame ms (renderer)`, and with `RX_VK_FRAME_TRACE=1` six of the frame trace's columns (`game_ms`, `render_ms`,
`wait_gpu_ms`, `wait_render_ms`, `wait_main_ms`, `memsync_ms`). There are no GPU zones (use
`RX_VK_PASS_TIMING`) and no allocation tracking.

### The game's own functions

Linux builds only. Lifted functions get zones (blue) from a list: [`nx2/tracy_funcs.txt`](../../../nx2/tracy_funcs.txt) (the job loops and
the main thread's frame phases), plus `LONGSHOT_TRACY_FUNCS=<file>` of your own at build time. Format, one a line:

```
<guest address> [calls] [zone name]
```

- A listed function is a zone each time it runs; its text is the object it ran on and the caller.
- `calls` also makes each indirect call it makes a zone (`game call`), for dispatch points such as job loops. Its chunk
  is recompiled.
- Objects are named by RTTI class name where there is one, else a short string the object points at, else its vtable.

A list of your own is made from the game: keep it outside the checkout. `HOS_TRACY_GAME=0` leaves the game zones out of
a capture (`funcs` or `calls` keeps one kind). A zone left open by `longjmp`, or a function inlined into a caller in its
own chunk, gets no clean zone. [`nx2/tracy_funcs.py`](../../../nx2/tracy_funcs.py) builds the wrappers.

If a capture stops with "Invalid order of zone begin and end events", look in the log for `tracy: a game zone ... ended
out of turn` or `tracy: a fiber switch ... with N game call zones open`; for the second, build with
`LONGSHOT_TRACY_FIBERS=1` (fibers as Tracy fibers; much slower, so off by default).

### Adding a zone

Include `host/hos_tracy.h` (C or C++); its top lists the macros. Put `HT_SCOPE("name");` at the start of a block, inside
`#ifdef TRACY_ENABLE` ... `#endif`, so builds without Tracy and the source-cutting checks never see it
([`tests/test_tracy_funcs.py`](../../../tests/test_tracy_funcs.py)). Use `HT_SCOPE_DRAW` for anything per draw, and keep
zones off loops that run more than a few thousand times a frame.

### Cost

Nothing is recorded without a viewer. During a capture expect a large fps drop. Read times as shares; compare with `RX_VK_TRACY_DRAWS=0` before
trusting a per-draw number. The profiling mode can run at the same time for its plots, but not for frame-rate numbers.

## CPU profile

`scripts/perf.py cpu` turns sampled runs into a report of CPU time by category, thread and function, with all lifted
game code as one row (`game/lifted`), so the result can be published. Categories are rules in
[`scripts/profiling/cpu_categories.py`](../../../scripts/profiling/cpu_categories.py) (game, runtime, os, render, driver,
system, crt, external, tooling, unclassified), keyed on module, object file and symbol so they survive rebuilds. Hot
functions are described in `scripts/profiling/cpu_functions.json`, savings candidates in
`scripts/profiling/cpu_candidates.json`. It reads Windows captures with lld-link maps only.

```sh
python -I scripts/perf.py cpu <sampled run folder>... --maps <maps folder> --out report.md \
    [--measured <unsampled run>...] [--readobj <llvm-readobj>] [--calls <HOS_HLE_STATS run>]
```

A run folder holds `all.txt`, and for "ms a frame" `load.json` and `presents.csv` (the profile report writes
`load.json` into a profile folder). When the report lists unclassified code, add a rule to the categories file;
`--guest-map`, `--functions` and `--candidates` take local files that split lifted code further, which then stay on your
PC.

**Shares are occupancy.** A sample counts as on the CPU unless its leaf is a waiting system call, so a preempted thread
counts and kernel time does not. Take fps and CPU ms a frame from unsampled runs.

## Performance statistics

Players can tick **Send performance statistics** in the launcher (off by default). The game then sends a summary of how
its frames went to the team's relay: frame and GPU time
histograms, hitches by cause, pacing misses, renderer events and movie stats, per map and sliced by GPU, driver, CPU, OS
and renderer, plus coarse one-second samples of Link's position (to 100 units) and camera angle (binned). No paths,
user name, saves, logs, game files or IDs are sent.

| Part | Where |
|---|---|
| Counting, summaries, sending (system curl, off the render thread) | [`gpu/vk/perf_report.cpp`](../../../gpu/vk/perf_report.cpp), [`perf_curl.cpp`](../../../gpu/vk/perf_curl.cpp) |
| The tick | `send=1` in the game folder's `longshot_perf.ini` (`keep_sent=0` keeps no copies), written by the launcher |
| Waiting and sent summaries | `perf_pending/` (48 at most), `perf_sent/` (each summary the relay took, as sent; the game keeps them all, `nx2/perf_report.py` keeps the last 48) |
| Relay address | `perf_reports=` in `longshot_version.txt`, stamped from [`nx2/addresses.ini`](../../../nx2/addresses.ini) |
| Map, movies and Link's pose | the host's `hos_perf_info` (`MkwVkHost::perf_info`) |
| Local stand-in sender, for checks and testing | see [Tools](#tools) |

A summary goes every `RX_VK_PERF_INTERVAL` seconds of play (default 300, 60 to 840) and at exit, holds at most 16 maps,
and stays under 60 KiB (`perf_report::kMaxBody`; samples are thinned to fit). The log says `vk: performance statistics
on (...)` at the first frame. `RX_VK_PERF_TEST_CURL=<path>` (test builds only) replaces curl in the checks.
The stand-in sender reads its own switches: `LONGSHOT_PERF_URL`, `LONGSHOT_PERF_TIMEOUT`, `LONGSHOT_PERF_MAX_SENDS`
(at most 5 a start by default: the ingest route takes 6 a minute with a burst of 6 per address — the two numbers live together, if you
raise one, raise the other; one sender at a time, holding `perf_pending/.sending`, a lease it renews while it sends),
`LONGSHOT_PERF_BACKOFF`, `LONGSHOT_PERF_KEEP_SENT`.

## Tools

Sample files are `HOS_SAMPLE_ALL` output (`all.txt`, plain or packed) unless noted. `scripts/perf.py --help` describes
every reader; the readers themselves are the `scripts/profiling/` package. Readers that name frames take `--maps <folder>`
or `RX_MAPS=<folder>` (the game folder on Linux, a folder of `.map` files on Windows).

| Command | Input | Question it answers |
|---|---|---|
| `scripts/perf.py report` | a profile folder (`HOS_PROFILE=1`) | Where did this run's time go, and what was each stutter? Writes `report.md`, the run as a timeline (Perfetto `trace.json.gz`) and flame graphs (`speedscope.json`), and `load.json` when the folder has none |
| `scripts/perf.py stacks` | an `HOS_SAMPLE_FILE` file (one thread) | Which functions is this thread in, inclusive and self? |
| `scripts/perf.py threads` | a sample file | Each thread's running share; when no game thread runs, what everyone waits in; when the main thread is blocked, what runs |
| `scripts/perf.py renderer` | a sample file | What do the renderer's threads (`mkw_vk*`) run? |
| `scripts/perf.py hotlist` | a sample file | Which lifted functions are hottest? Writes the list for the lift's `A64_HOT_FUNCS` |
| `scripts/perf.py chain` | an `HOS_CHAIN` file (+ a sample file of the same run) | The frame's critical path: which thread ran, waited or slept on it, and the hand-offs |
| `scripts/perf.py cpu` | sampled run folders + Windows maps | CPU time by category, thread and function, publishable (lifted code as one row) |
| `scripts/perf.py ab` | frame traces of `HOS_AB` runs | Is side 1 faster than side 0, with a confidence interval? |
| `nx2/perf_report.py` | a game folder | What would the performance statistics send (`--send` sends it, `--tick on\|off` sets the tick, with `--keep-sent on\|off`)? |
| `mise run tracy-tools` | | Builds `tracy-capture` and `tracy-csvexport` 0.15.0 |
