# Switches

A switch is an environment variable the game reads: `RX_` names in the renderer, `HOS_` names in the host runtime, and a
few `LONGSHOT_` names. Every one has a row in [`gpu/vk/switches.def`](../../../gpu/vk/switches.def), which says what the game
does with it unset and what setting it does. That file is the full reference; this page explains how switches are read and
lists the ones developers use.

Build switches (read by `make_game.py` and `build.py`, not the game) are in [the build](build.md#build-switches). Profiling
switches are in [profiling](profiling.md). When to add a switch and how to choose its default is in
[standards](../standards.md#switches).

## How the game reads them

At start the game takes switches from, in order:

1. **The environment.** A name already set when the game starts wins.
2. **`longshot_env.txt`** beside `image.bin`, or the file `HOS_ENV_FILE=<path>` names instead. A line for a name the
   environment already set is left out, and the log says so.

Settings from the Insert menu live in a separate file, [`longshot_options.ini`](#the-options-file). A `player` switch that
stands in for a menu row overrides it for that run only; the options file keeps the player's own value.

### longshot_env.txt

- One `NAME=VALUE` a line. `#` starts a note, on a line of its own or after a value and a space. A UTF-8 BOM is fine.
- Players pass this file around, so it only takes the game's own names (`RX_`, `HOS_`, `LONGSHOT_`, `EDEN_`, `NVN_`,
  `SDL_`) and **no paths**. Left out:
  - any value with a `/`, `\`, drive letter or `~`;
  - switches ending in `_FILE` or `_DIR`, and those that write files or press buttons on a timer (`HOS_INPUT_SCRIPT`,
    `HOS_INPUT_LIVE`, `RX_VK_MENU_SCRIPT`, `RX_VK_SHOTS`, `RX_VK_DUMP`, `HOS_CRASH_TEST` and others: `env_line_allowed`
    in `host/hos_main.c`);
  - `HOS_ENV_FILE`, `HOS_CONTROL_PEER_SID` and SDL's library overrides.

  The one path it takes is `RX_VK_DLSS_DIR`'s, a folder on this PC (not a network share).
- The file `HOS_ENV_FILE` names has none of these limits: use it for a scripted or A/B run, so the player's file stays as
  it is. `HOS_OPTIONS_FILE=<path>` does the same for the options file (set in the environment or the `HOS_ENV_FILE` file).
- Lines left out are logged as `env: longshot_env.txt: <name> left out (...)`.

### On and off

An on/off switch is **off** when it is unset, empty, `0`, `false`, `off` or `no` (any case), and on for anything else
(`env_on` in `gpu/vk/compat/longshot_env.h`; `kit.env_on` in Python). A switch that is on by default (`HOS_AUDIO=0`,
`HOS_VCORES=0`) is turned off by the same words (`env_on_or`). Some older `gpu/vk` switches are still read as numbers, where
only `0` is off. A switch that takes a number or a list reads its value as its row says.

### What the log says

- `vk: switches set (n): NAME=value (kind), ...` lists every switch the run set. A set `RX_` or `HOS_` name that is no
  switch (misspelt, from another build) gets a line of its own, and a removed one says what replaced it.
- `vk: policy:` lines show the hardware rules and mark with `*` any value a switch set ([architecture](architecture.md)).

## The switch table

Each row of `gpu/vk/switches.def` is `SW(name, kind, type, "unset", "set")`, sorted by name; a removed switch is
`GONE(name, "YYYY-MM-DD", "what a run that still sets it gets")`. The kinds (`gpu/vk/switches.h`):

| Kind | Meaning |
|---|---|
| `diag` | Logging, statistics, dumps, self-checks, fault injection: nothing changes unless set |
| `tune` | A number with one built-in default for every PC |
| `kill` | Turns off (or back to an older way) something that is on for everyone |
| `attempt` | Turns on something off for everyone: an experiment nobody ships |
| `hw` | Unset, it follows the PC (GPU vendor or driver, VRAM, CPU count, OS) |
| `player` | A setting the player has (Insert menu, options file), or one the launcher or Builder sets |

The comment where a switch is read says how it works in detail.

## Adding or removing a switch

The rules (when, which default, when to remove) are in [standards](../standards.md#switches). The mechanics:

1. **Add a row** to `switches.def` for every new `RX_`, `HOS_` or `LONGSHOT_` name the game reads, sorted by name, with
   its kind. A default that depends on the PC is read in [device policy](architecture.md#device-policy).
2. **To remove one**, delete its reads and turn its row into `GONE(...)`.

`tests/test_switch_table.py` fails on a name read without a row, a row nothing reads, and a removed name still read.

**Switches the shader translator reads** are part of the program cache's stamp (`translator_stamp` in
`gpu/vk/vk_shader.cpp`; `tests/test_program_stamp_knobs.py`), so changing one translates every program again. The stamp
also covers the translator's sources (`nx2/translator_files.py`: `gpu/vk/{eden,sirit,compat}/`, `vk_shader.cpp`,
`vk_bank.cpp`, `vk_state.h`). A code change there makes every player translate every program at their next start, so code
the translator doesn't read belongs elsewhere.

## The options file

`longshot_options.ini` holds the Insert menu's settings. It lives in the user folder the game is given, with the saves
(`host/options_path.c`):

1. `HOS_OPTIONS_FILE=<path>`, if set (in the environment or the `HOS_ENV_FILE` file);
2. else the `--user` folder (the launcher's `%APPDATA%\Longshot` on Windows);
3. else beside the game.

The log's `options: the settings file is <path>` says which. Only `compile_shaders=`, the build's choice, stays in the game
folder's own file, where `make_game.py` writes it.

`gpu/vk/vk_menu.cpp` (`save`) writes the whole file at every change:

- Each row as `<key>=<number>`: its module's row's own `.key` (`frame_generation=2`, `invert_camera_x=1`;
  [the Insert menu](architecture.md#the-insert-menu)). Output resolution is `output_res=<width>x<height>`.
  `tests/test_menu_ini_keys.py` keeps the keys players' files hold.
- On/off rows are `0` and `1`. A value outside a row's range is clamped when read.
- Also `options_version=6`, `memory_budget=`, device choices by ID (`gpu_device_id=`, `audio_output_device_id=`,
  `microphone_device=`, `fullscreen_monitor_id=`), and the controls (`pad_<button>`, `key_<button>`, `toggle_<button>`;
  the settings page's [Controls](../../user/graphics-and-controls.md#controls)).

Keys whose numbers are not obvious from the row:

| Key | Values |
|---|---|
| `grass_distance` | The short decorative blades' reach: 0 Game's (default; radius 22 m, blades thinned from 12 m to about 20 m), 1 2x, 2 3x, 3 4x. The radius and both fades extend by that factor at once. It does not change the large-scale simulated turf, whose separate **Simulated turf** page uses adaptive record lists (`gpu/vk/vk_draw.cpp` `grass_reach`; `RX_VK_GRASS_DISTANCE` overrides the row) |
| `hud_layout` | HUD layout. 0 Screen's edges (default), 1 Middle 16:9, 2 Custom. Read at each present (`host/hos_wide_ui.c`). `HOS_WIDE_UI` overrides it |
| `hud_health_x/y/scale`, `hud_buttons_x/y/scale`, `hud_minimap_x/y/scale` | Custom HUD placement, with horizontal and vertical offsets in percent of the picture and scale in percent of the usual size |
| `key_toggle_hud`, `pad_toggle_hud` | Session HUD shortcut, unbound by default. Uses the Controls page's normal binding format |
| `walk_mode` | 0 Hold, 1 Toggle |
| `threaded_rendering` | The Debug tab's Threaded rendering row (`gpu/vk/threaded_rendering.h`): 0 Auto (default), 1 On (`RX_VK_ASYNC=1` and `RX_VK_RECORDER=thread`), 2 Off. Auto goes by the physical cores the process may run on (`usable_cores` in `gpu/vk/vk_progcache.cpp`): 4 or more cores, the render thread and the recorder thread, as On; fewer, Off. Where only buffer-pool mirrors are absent (integrated GPUs where `!f.discrete()`, or `RX_VK_MIRROR=0`), Auto keeps the render thread off and runs the recorder thread alone with 6 or more physical cores (`kAutoRecorderOnlyCores`), and Off with fewer. Auto is also Off, whatever the cores, where the mode is not safe on the run (the capture off, `RX_VK_LOW_SPEC`, a frame dump, a benchmark replay). `threaded::kAutoEnabled` turned off makes Auto Off on every PC |

A new row on the Graphics, Audio, Gameplay, Controls or NFC tab needs its line in the settings table of
[docs/user/graphics-and-controls.md](../../user/graphics-and-controls.md#every-setting), in the menu's order
(`tests/test_settings_table.py`; the website builds its settings page from that table). Saved keys are names, independent of the option IDs in `gpu/vk/vk_api.h`, so
renumbering an option doesn't break a player's file.

## Switches developers use

The switches players are pointed to (`RX_VK_ASTC_RGBA8`, `RX_VK_MESH_NOCULL` on the Steam Deck, `HOS_DAV1D_THREADS`,
`HOS_OOT_CHEATS`) are explained on the player pages that need them
([advanced setup](../../user/advanced.md#switches-longshot_envtxt), [common problems](../../user/common-issues.md)).
Scripted input, screenshots, frame dumps and crash switches are in [developing](../developing.md#run-the-game-for-a-test).
Upscaler switches are in [upscalers](upscalers.md#switches).

### Switches for test runs

| Switch | What it does |
|---|---|
| `HOS_HEADLESS=1` | No window: the renderer draws offscreen |
| `HOS_WINDOW_SIZE=<w>x<h>` | Window size (1280x720) |
| `HOS_WINDOW_NOACTIVATE=1` | The window opens behind the others without taking focus (Windows) |
| `LONGSHOT_BUILD_NAME=<text>` | Shown under the version at the menu's top right (F1) and on the start-up screen, so a screenshot says which change it was. `dev.py queue run --desc` sets it for a game run (default: the job's name) |
| `HOS_AUDIO=0` | Open no sound device (the mixer still runs). `HOS_AUDIO_MUTE=1` opens it at volume 0 |
| `HOS_CONTROL=<socket>` | Start the [control bridge](control-bridge.md) |
| `HOS_ENV_FILE=<path>`, `HOS_OPTIONS_FILE=<path>` | A test's own switches and settings files |
| `HOS_SKIP_OPENING=1` | Skip the new-game opening |
| `HOS_OOT_CHEATS=1` | The Debug tab's tools (flight, free camera, teleport, inventory) |
| `HOS_FIXED_TIME=<POSIX seconds>` | The game's clock starts there, so runs compare |
| `HOS_LANGUAGE=<code>` | The game's language for this run |
| `HOS_DISPLAY_MODES=<w>x<h>,...` | Pretend the screen has these modes, to check the size rows for a screen you lack |
| `HOS_NO_SDL=1` | No SDL controller thread: no SDL pads |
| `HOS_TRACE=1` | Log every Horizon call (as `--trace`) |
| `HOS_FS_TRACE=1` | File opens and writable changes in call order: create, write offset/size/result, resize, delete, rename and close; save Commit counts, open writers, result and milliseconds |
| `HOS_TEST_ALLOC_REFUSE=<label text>` | Refuse host allocations whose label contains the text, to see what a player sees at the commit limit |
| `RX_VK_UI_GAME_FONT=0` | The menu, counter and start-up screen in the included Libertinus, even where the game's files have its own fonts |
| `RX_VK_UI_PROMPTS=pad\|keys` | The menu's button prompts for a controller or the keyboard, whatever was used last (screenshots) |
| `RX_VK_ACQUIRE_AHEAD=0/1` | Acquire ahead, for an A/B: the presenter thread asks for the window's next image right after each present instead of the render thread at the next one. On by default with every driver; `0` off (#1458's order, each image asked for at its present). `1` also logs `vk: the window's image (600 presents)`. It acts only where there is a window (not with `HOS_HEADLESS=1`), and not under frame generation or V-Sync On ([threads](threads.md#the-presenter-thread)) |

### Settings for one run

These override a menu row for the run only (kind `player`).

| Switch | Menu row |
|---|---|
| `HOS_FULLSCREEN=0/1` | Fullscreen |
| `HOS_OUTPUT_RES=<w>x<h>`, `HOS_ASPECT=<ratio>`, `HOS_HANDHELD=1` | Output resolution, Aspect ratio, handheld profile |
| `HOS_DYNRES=0/1`, `HOS_REALTIME_STEP=0/1`, `HOS_TEXT_SPEED=0..3` | Resolution (Auto), Game speed, Text speed |
| `HOS_FPS_MAX=<30 or 61..1000>` | Game frame rate cap |
| `HOS_WIDE_UI=0/1` | HUD layout on a screen that is not 16:9 |
| `HOS_PAD_SPEAKER_VOLUME=<0-200>`, `HOS_PAD_SPEAKER_HEADPHONE_MODE=<0-2>`, `HOS_PAD_SPEAKER_<CLASS>=<0-2>` | Controller speaker: Speaker volume, Headphones on the controller (0 Off, 1 Controller sounds, 2 All sound), a kind of sound's row (`NAVI_VOICE`, `NAVI_SOUND`, `TARGET`, `OCARINA`, `PICKUP`, `ITEM`, `FAIRY`, `UI`, `AGONY`, `LOW_HEALTH`, `FISHING`, `BOTTLE`: 0 Off, 1 Controller only, 2 Controller + TV) |
| `RX_VK_DEVICE=<n>` | GPU device (the index of `vk: device <n>` in the log) |
| `RX_VK_FIFO=1`, `RX_VK_IMMEDIATE=1` | V-Sync on, off |
| `RX_VK_PACE=0/1`, `RX_VK_UPSCALE=0..2` | Frame pacing, upscaling filter |
| `RX_VK_DLSS=0/1/2`, `RX_VK_FSR4=0/1/2`, `RX_VK_XESS=0/1/2`, `RX_VK_FSR=0/1` | Upscaler: `0` never, `1` the upscaler, `2` its native AA |
| `RX_VK_FSR4_MODEL=auto\|lite` | The FSR 4 network for one run: Auto lets AMD pick, Lite forces the INT8 Lite provider |
| `RX_VK_FSR4_STANDIN=0` | No FSR 4 on a GPU AMD's library lists no FSR 4 model for (by default the INT8 Lite provider stands in there) |
| `RX_VK_FRAMEGEN=0`, `RX_VK_FRAMEGEN_MULT=2..8`, `RX_VK_DLSS_FG=0/1`, `RX_VK_FSR_FG=0/1` | Frame generation (`RX_VK_FRAMEGEN=0` keeps it off whatever the menu says) |
| `RX_VK_ASYNC=0/1`, `RX_VK_RECORDER=sync\|defer\|thread` | Threaded rendering: the render thread, the recorder (the parallel replay is gone: [threads.md](threads.md)) |
| `RX_VK_COMPILE_SHADERS=most\|all` | Compile shaders |
| `RX_VK_NO_INTEL_SERIAL=1/0` | Intel shader compiles: Side by side, One at a time ([below](#intels-compiler-on-windows)) |
| `RX_VK_ASTC_RGBA8=1` | ASTC as RGBA8 (Debug tab): ASTC textures decoded to RGBA8 instead of BC, which takes much more memory |

### The controller speaker

`host/pad_speaker.c`, `host/audio_source.c` (the Controller speaker page; its rows' own switches are under the next
heading).

| Switch | What it does |
|---|---|
| `HOS_PAD_SPEAKER=0` or `1` | `0` never opens a DualSense's audio endpoint (the pad keeps its sound); `1` opens it even with every kind of sound Off. `HOS_PAD_SPEAKER_TEST=1`: a tone on the pad when it opens |
| `HOS_AUDIO_SOURCE=0`, `HOS_AUDIO_SOURCE_TRACE=1` | No sound classed (nothing read from the game's sound archives: every sound stays on the TV), or each classed sound logged as it plays, `audio: voice N plays <checksum>: <class>` |
| `HOS_PAD_SPEAKER_GAIN=<0-7>`, `HOS_PAD_SPEAKER_PATH=<0-3>`, `HOS_PAD_SPEAKER_BUFFER_MS=<5-100>` | The pad's preamp gain (4), its output path for the speaker (3, the speaker alone; 2 speaker and jack; 0 the jack), the buffer before the sync (20 ms). While RenHDrum plays the pad on its cable, the game's sounds are added to its stream's first two channels (HD Haptics' taps on the second kept) and the speaker is opened and given back by RenHDrum's speaker block alone (`rh_usb_speaker`, with HD Haptics off too), at this gain while they play there; the path is then RenHDrum's (3) |
| `HOS_PAD_SPEAKER_HPF=0`, `HOS_PAD_SPEAKER_LIMITER=0`, `HOS_PAD_SPEAKER_LOW_LATENCY=0` | Each of the speaker's sound fixes turned back for a run: the bass kept, the earlier sample-by-sample curve, the earlier 40 ms buffer |
| `HOS_PAD_SPEAKER_TV_DELAY=0`, `HOS_PAD_SPEAKER_OCARINA_RUMBLE=<0-100>` | A Controller sync below 0 never holds the TV back (`host/nn_audio.c`); the vibration's strength while an ocarina sound plays on the speaker, in percent (50; 100 as it is) |
| `HOS_PAD_SPEAKER_HEADPHONES=0` or `1` | Headphones out of or in the pad's jack for a run, over what its input report says (`hos_sdl_pad_headphones`): for a pad or bridge that does not say |

On a cabled DualSense whose vibration plays its voice coils (`host/rumble_renhdrum.c`), the speaker's two channels ride on
the vibration's stream (`hos_pad_speaker_fill`); otherwise the speaker opens its own, the voice coils' channels silent.

### GPU debugging

| Switch | What it does |
|---|---|
| `RX_VK_VALIDATION=1` | The Khronos validation layer |
| `RX_VK_CHECKPOINTS=0` | No NVIDIA draw checkpoints (on by default: after a lost device they name the draw) |
| `RX_VK_FAULT_INFO=1` | At a device loss, also query the driver's fault info |
| `RX_VK_GPU_HANG_SEC=<s>` | GPU work that finishes nothing for this long (30) is a hang that ends the game; `0` only logs |
| `RX_VK_HANG_DUMP=<file>` | Every thread's stack to the file once no frame was shown for `RX_VK_HANG_SEC` seconds |
| `RX_VK_NOOPT=1` | Build pipelines without the driver's optimizer |
| `RX_VK_SKIP_PROG=<prefix>,...` | Leave out draws of these programs (`-`: none) |
| `RX_VK_SKIP_CODE=<prefix>,...` | Leave out draws of programs with a stage whose native code key (the log's "its code:") starts with a prefix. For a task + mesh program, its models go with it: to get past a hang in a culling task shader, `RX_VK_MESH_NOCULL` ([below](#mesh-shaders)) keeps them |
| `RX_VK_NAN_GUARD=0` | No NaN guard on float colour outputs |

### Shaders and pipelines

| Switch | What it does |
|---|---|
| `RX_VK_PRECOMPILE=0/1` | The shader preparation before the title (on in release builds, off in development builds) |
| `RX_VK_NO_PROGRAM_CACHE=1` | Neither read nor write `longshot_programs.bin` and the pipeline notes |
| `RX_VK_WARM_CHECK=0` | At a start that loads at create calls with no marker (the first after a preparation, whose own process leaves none), a sample of 256 noted pipelines and 64 compute programs is first asked of the caches as a load at a create call asks them, without compiling (`gpu/vk/warm_check.h`). Every one answered (Mesa's drivers answer from the preparation's pipeline cache file), the marker is written and the warm pass is left out: it built every pipeline again into the cache object, which held every shader built through it for the session, before the game began (a 15 GB AYN Odin 3 went down after it). One not answered (NVIDIA, a driver changed since the preparation), the warm runs as before. The log's `precompile: the caches answer N of 256` line. `0`: not asked, warmed as before |
| `RX_VK_FALLBACK_LATER_PHIS=<n>` | Turnip by default (10000), any driver when set: a mesh fallback program with a compute stage of more than this many phis is not built before the title but at its first draw, which waits up to 50 ms for it and is left out while it builds, as any fallback draw whose pipelines are not ready. On Turnip a shader's private memory (its register spills) is one buffer for the whole device, sized for the hungriest shader made so far and never shrunk: 93e3e999f2fb9fd8 (79,947 phis) took it to 1.5 GiB and 5c9454152ebde335 (16,837) added 373 MB before the title, for programs a Kokiri Forest session never drew (AYN Odin 3). `vk: mesh fallback programs with a compute stage of more than N phis are built at their first draw` at the start, `left to their first draw` in the precompile's line, and `vk: fallback: <key>'s compute pipelines built at its first draw in N ms` when one is. `0`: every one before the title (as before). A `--prepare-shaders` run builds every one it reaches, whatever the switch; under `RX_VK_LOAD=create` its warm reaches none, so after a driver or translator update the first draw of one of these compiles it in play, its draws left out meanwhile |
| `RX_VK_PLACED_LATER_MB=<MB>` | Where the game's pools are placed in the device's own memory (Turnip on the AYN Odin 3: no host pointers, no udmabuf), the pools of this size or more (64 by default) are left in ordinary memory when the game makes them, and placed when the GPU first binds them; `0` places every pool when it is made, as before. A placement takes its whole size in RAM when its memory is allocated, pages the driver holds and the kernel never swaps; in Kokiri Forest 3.5 GB of the 4.3 GB in pools of 64 MB or more were never bound in 5 minutes, 2.4 GB of it the textures' pool, which only the CPU reads (`docs/placed-later-plan.md`). A large texture in such a pool is unswizzled on the CPU. At the first binding the pool is made read-only while its contents are copied (`gpu/vk/placed_memory.h` `place_live`): a write by the game meanwhile waits in the renderer's fault handler and then lands in the placement, and a file read into it (the posix `ReadFile`) is tried again until it is done. The log says `left in ordinary memory until the GPU binds it` for each such pool, and `placed in the device's memory at its first GPU binding in N ms` with the writes held back when one is placed |
| `RX_VK_PCACHE_LOAD=0/1` | Never read or save the pipeline cache file (nor AMD FSR 4's own, `cache/longshot_fsr4_pipelines.bin`), or always read it. On Linux Turnip with up to 20 GB of RAM and load at create, shader preparation builds into Mesa’s cache when available. Play uses it alone once warm. |
| `RX_VK_LOAD=create\|start` | Load programs when the game makes them, or every listed one before the title |
| `RX_VK_PIPE_SKIP=0/1/2` | `1` (default): a draw waits for its translation; a pending pipeline is skipped only for draws that write no memory. `0` always waits for the pipeline too; `2` brings back the old skip of untranslated draws on persistent targets |
| `RX_VK_QUICK_MS=<ms>\|required` | Temporary unoptimized pipelines while the full one builds: `<ms>` a frame per recording thread, or `required` (default) only for draws that must wait (`0`: none) |
| `RX_VK_TRANSLATE_WAIT_MS=<ms>` | Bound the wait for translating shaders, then leave their draws out (off by default) |
| `RX_VK_DRIVER_FAULTS=0/2` | Ask the driver again for programs it faulted on (`0`), or build one at a time on every driver (`2`) |
| `RX_VK_PROGRAM_CACHE_ZSTD=1` | Keep translated programs zstd-compressed in `longshot_programs.zst.bin`, a file of its own: `longshot_programs.bin` is left alone, and the first run translates every program again. Its records are only read whole, so programs load before the title (`RX_VK_LOAD=start`'s way, `load_default.h`) |
| `LONGSHOT_SHADER_STORE=<folder>\|0` | Where the shader store keeps the programs' SPIR-V once for every game folder (`spirv.pack`, `spirv.idx`, `spirv.lock`; [architecture](architecture.md)). Unset: `%LOCALAPPDATA%\Longshot\shaders`, on Linux `$XDG_CACHE_HOME/longshot/shaders` (else `~/.cache/longshot/shaders`). `0`: none, each record keeps its SPIR-V as before. A cache written for another store starts over at the next start; one whose store did not open this run (a file held by another program) is kept for the next. Kept for the game the builders start (`make_game.GAME_ENV_KEEP`: the preparation at the build, the AppImage's Play), so it is set where the build runs too. Not with `RX_VK_PROGRAM_CACHE_ZSTD` |

#### Intel's compiler on Windows

Intel's Windows compiler is entered by one thread at a time (`IntelDriverGate`, `vk_internal.h`): it faulted with two
in it. A draw that only asks whether its pipeline is ready never waits behind a worker's compile there: the answer is
"not yet", even for a pipeline already in a cache, and the draw is left out until a worker has made it. The log counts
these (`vk: asked cold: Intel's gate busy`, the first 8, then at each power of two).

The Debug tab's Intel shader compiles row lifts the gate after a restart: Auto (default) is side by side on the Arc
B-series (PCI device `0xE2xx`) with driver 101.9154 or newer, one at a time on every other Intel GPU
(`intel_compiles.h`). `RX_VK_NO_INTEL_SERIAL=1` is side by side for a run, `=0` one at a time.

The shader preparation (the launcher's Prepare shaders, and the start-up screen) goes further on Auto: from driver
101.9175 its graphics pipelines and shader modules go side by side on every Intel GPU (`IntelPrepareLane`,
`intel_prepare_side_by_side_chosen`), while its compute pipelines and everything play builds keep the row's choice. One
at a time, the preparation took hours on an Arc A730M and a Core Ultra's Arc iGPU (#1706, #2011). The fault the gate was
put in for was the pipeline cache's data fetched beside builds (#252), which `pcache_fetch_lock` keeps apart by itself.
Side by side there, the threads that build pipelines are held to the free memory, 768 MB each past 4 GB
(`intel_prepare_workers`): 16 on a 16 GB A730M ran into the commit limit (#1717). The log says it once a step
(`vk: preparation (...): Intel's compiler takes the graphics pipelines side by side on N threads of M`). One at a time
on the row, or `RX_VK_NO_INTEL_SERIAL=0`, keeps the preparation one at a time too.

### Mesh shaders

| Switch | What it does |
|---|---|
| `RX_VK_MESH_SHADERS=native\|compute` | The GPU's own mesh shaders, or the compute fallback, for every draw (Automatic otherwise) |
| `RX_VK_FORCE_MESH_FALLBACK=1` | The compute fallback on a GPU that has mesh shaders |
| `RX_VK_TASK_MESH_FALLBACK=0/1` | Task + mesh draws through the fallback (on by default for AMD Navi 21-23 on Windows) |
| `RX_VK_MESH_NOCULL=<prefix>,...` | Turn off culling in these task shaders, so every meshlet is drawn |

Changing any `RX_VK_MESH*` switch translates every program again.

Whether the driver offers mesh shaders is a separate question from whether this run uses them (`RX_VK_MESH_SHADERS`
above): the device line (`vk: <GPU>, Vulkan ...; mesh shaders ...`) says `used`, `offered by the driver but not used`
(a Steam Deck by default, with the reason) or `not offered by the driver`.

### Memory

| Switch | What it does |
|---|---|
| `RX_VK_MIRROR=0/1` | VRAM mirrors of the game's buffer pools (on for discrete GPUs) |
| `RX_VK_VRAM_MB=<MB>` | The texture budget, in place of the hardware rule |
| `RX_VK_TEST_VRAM_MB=<MiB>` | Treat the card as having at most this much VRAM, to play a smaller card |
| `RX_VK_TEST_ALLOWED_MB=<MiB>` | Cap the VRAM allowance the renderer sees, to exercise eviction |
| `RX_VK_LOW_SPEC=1` | A first run (no `longshot_options.ini` yet) starts on Minimum (4GB) with frame generation and the upscaling filter off, keeps no staging blocks and uses no capture copy helpers |
| `RX_VK_WB_MAX_KB=<KB>` | Size from which a GPU-written binding is written back only where the CPU reads or writes it (1024; `0`: every written binding written back whole) |
| `RX_VK_WB_PROBATION=<n>` | Submissions that write such a binding back whole before it may be left out (8) |
| `RX_VK_ASTC_BC7=1` | Re-encode big ASTC textures as BC7 instead of BC3: the same memory, better gradients and alpha. `RX_VK_ASTC_RGBA8=1` wins over it |
| `RX_VK_ASTC_CHUNK_MPX=<n>`, `RX_VK_ASTC_CPU=1` | Decoded megatexels per GPU ASTC upload submission before flushing (16; `0`: one submission), or decode ASTC on the CPU to RGBA8 instead |
| `RX_VK_LOW_VRAM_MIPS=0` | No smaller texture limits on low-VRAM PCs (below) |
| `RX_VK_TEX_MAX_DIM=<px>` | Width above which a big decoded ASTC texture leaves out top mip levels (2048; 1024 under the low-VRAM rule) |
| `RX_VK_TEX_SKIP_MB=<MB>` | Size as RGBA8 from which such a texture loses top levels (64; 8 under the low-VRAM rule) |
| `RX_VK_TEX_MAX_MB=<MB>` | Size down to which any texture leaves out top levels, whatever its width (1536) |
| `RX_VK_TARGET_REUSE_MEMORY=0/1/2` | A render target whose create info is not in the render target pool (OoT's shadow maps, their side new every frame) takes the memory of the smallest pooled image it fits in that has waited there a present: of a memory type it can use, aligned, up to 1.5 times its size (or its size class, which under 1 MB may be more), only a share of the targets' blocks (`RX_VK_TARGET_REUSE_MEMORY=1`, the default on a discrete GPU; `gpu/vk/target_pool.h` `verdict`, `takeable_by_size`; the log's `reused by size`, and `missed after their image was taken by size` for the shapes it took from). While it is on, on an integrated GPU, the pool holds its images 64 presents, not 8 (`idle_presents`): there each new allocation was a kernel call that compacts and swaps on a Steam Deck, and the held images are the next shapes' memory; the pool's byte cap and its pressure rules bound it. `=2`, the default on an integrated GPU (measured on a Steam Deck OLED, Minimum, Render 50%; `=1` with the hold there is less measured, not shown worse), also makes a target of the pooled kind, while the pool is not pressed, on an allocation of its size class (`class_bytes`: 64 KiB multiples in steps of a quarter, a block at most, no fewer to a block where fewer than 8 fit), so one a few percent larger takes it: at most a quarter over the requirement rounded to 64 KiB, 6-9% on average, counted in the budget and the pool's bound. A second status line every 600 presents says why the rest missed (`render targets not reused by size:`). `RX_VK_TARGET_REUSE_MEMORY=0` turns the take, the hold and the classes off: a new allocation of the image's size, as before; what stays on is the check that the targets' pool's memory type suits an image (VMA does not make it for a pool), which sends one that cannot use it to an allocation of its own |

**Written bindings** (`gpu/vk/wb_learn.h`): the game writes tens of MB of culling lists and per-frame buffers through
the GPU that the CPU never reads. A binding over `RX_VK_WB_MAX_KB` is written back whole for its first
`RX_VK_WB_PROBATION` submissions; after that, only the ranges the CPU read or wrote meanwhile (and its first and last
page when it shares them). A range the CPU first touches later gets the stale copy once, is written back from then on,
and is logged (`in a binding whose write-back was left out`). At start the log says
`vk: written bindings over <n> KB are written back only where the CPU reads or writes them`.

**The low-VRAM rule** (`low_vram_texture_mips` in `vk_texture.cpp`) lowers `RX_VK_TEX_MAX_DIM` and `RX_VK_TEX_SKIP_MB`
to 1024 and 8 when both hold:

- the driver's budget, sampled once at the first texture, is nonzero and at most 4.5 GiB, or the GPU is integrated and
  the PC has 16 GB of RAM or less;
- the settings are Minimum's: the Minimum (4GB) preset, Auto (this PC) at Minimum, Custom after either, or Custom with
  no preset behind it where Auto would pick Minimum. The preset is checked for each new texture.

Each of the two switches overrides only its own limit; both apply to decoded ASTC textures only. The switches are read
once: change them before a start.

The memory budget and its `vk: budget:` lines are in [architecture](architecture.md).

### Sun shadows

| Switch | What it does |
|---|---|
| `HOS_SHADOW_STABLE=0` | Fit the sun's nearest shadow cascade as shipped at every Shadow resolution, following the view each frame. `HOS_SHADOW_STABLE=1` uses the steady fit at every resolution. By default the steady fit runs only on Highest (3072 texels); below it the game's fit is kept because the steady square is coarser standing still. The steady fit (`host/oot_shadow_fit.c`, `nx2/oot_shadow_fit.py`) keeps its basis independent of the camera, following the sun by the smallest turn from a point moving in whole texels, and fits a steady square around the slice's bounding sphere on whole texels. The log says `shadow: the sun's nearest cascade is <n> m a side` when the size changes. |

### Threads and cores

| Switch | What it does |
|---|---|
| `HOS_VCORES=0` | No virtual cores |
| `HOS_CORES=<1..8>` | The cores the game sees (6) |
| `HOS_RESERVE_CORE=0/1` | Keep a core for the replay (one is kept with 8 or more logical processors, but none on Windows CPUs whose L3 caches differ, unless `=1`) |
| `HOS_AB=<switch>=<value>[,<s>[,<warm-up s>]]` | Alternate a switch within one run for an A/B ([developing](../developing.md#an-ab-within-one-run)) |

### Movies

| Switch | What it does |
|---|---|
| `RX_VK_MOVIE_UPSCALE=linear` | The movies' neural upscale always replaced by a linear one (by default only where the networks can't run or measure too slow). `RX_VK_NETWORK_KEEP=all` wins over it |
| `HOS_VIDEO_NO_CATCHUP=1` | Never skip pictures that are behind the movie's sound (by default they are, decoding ahead at most a picture's time in a frame, and not while the decoder is slower than the movie) |
| `HOS_VIDEO_NO_HOLD=1` | Hand a movie's next picture out whenever the game asks for it, even when it is ahead of the movie's clock (by default one more than 25 ms early waits, and the last picture is shown again: issue 1964) |
| `HOS_VIDEO_ACQUIRE_TRIES=1` | Wait for a picture by 50 sleeps of 2 ms (about 250 ms, as sleeps overrun) instead of about 100 ms by the clock. Either way there is no wait after a stop or flush with nothing submitted since |
| `HOS_DAV1D_THREADS=<n>` | The decoder's threads (one per logical core; 2 with 4 or fewer) |
| `HOS_VIDEO_LOG=1` | `video:` lines: how long each picture took to decode |

### Logging

| Switch | What it logs |
|---|---|
| `RX_VK_LOG_WB_SKIPS=1` | The written bindings left out of the write-back, by guest range, every 600 submits |
| `RX_VK_REFLEX_LOG=1`, `RX_VK_DLSS_LOG=1` | NVIDIA Reflex's latency timings, NGX's own lines ([upscalers](upscalers.md#dlsss-log-and-memory)) |
| `HOS_MIC_LOG=1` | Each microphone stream's frames read and its peak, once a second (`audio in:`) |
| `HOS_MOTION_DEBUG=1` | The motion sensors: what the game gets, twice a second |
