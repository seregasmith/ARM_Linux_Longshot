# Input

The game sees one Pro Controller on Npad No. 1. [`host/nn_hid.c`](../../../host/nn_hid.c) builds it by merging every
source: SDL controllers, XInput, the keyboard and the mouse. Buttons map by position (the bottom face button is the
Switch's bottom one, B). The player's bindings come from the Insert menu's Controls page and are saved in
`longshot_options.ini` (`binds_load`). Player-facing controls are in [Settings](../../user/graphics-and-controls.md).

| Source | Code | Notes |
|---|---|---|
| SDL controllers | [`host/pad_sdl.c`](../../../host/pad_sdl.c) | SDL3, else SDL2, loaded at run time. Its own thread polls every 4 ms (on Linux, the window's thread). Also motion and rumble |
| XInput | `nn_hid.c` | Polled only while SDL has no controller open (`HOS_XINPUT=1` polls it always), on a thread of its own (`xinput_main`): every 2 ms while a pad is there, every 100 ms while none is. Never on the game's thread: asking an empty slot is slow, and the game hitched once a second for players with no controller |
| Switch 2 Pro Controller | [`host/pad_switch2.c`](../../../host/pad_switch2.c) | Windows, USB only. Read through WinUSB and given to SDL3 as a virtual gamepad (needs SDL3) |
| Steam Input | [`host/steam_pads.h`](../../../host/steam_pads.h) | Under Steam, Valve's pads are opened too; while Steam's stand-in pad is open they give only their motion. The Steam Deck's built-in controls stay ignored |
| Keyboard, captured mouse | `nn_hid.c`, [`host/mouse_native.c`](../../../host/mouse_native.c) | Only while the window has the focus. Mouse motion drives the right stick, or the camera directly with Mouse camera set to Native |

Controllers also count while the window is in the background when Controller in background is on
(`HOS_INPUT_BACKGROUND`).

Controller software can also send the game *keys*: Steam Input's Desktop Layout, active over a game launched outside
Steam, sends a pad's A as Enter, B and Menu as Escape and View as Tab. The keyboard is read globally, so those presses are
the player's. Plus is Tab alone for that reason, a **deliberate default change** (Enter was its second key until
options_version 8: a pad's A opened the pause menu,
[#1992](https://git.magicbean.dev/MagicBeanDev/Longshot/issues/1992)): an old file's default pair moves to Tab, a Plus
chosen otherwise keeps its keys, and Tab and the pad's Start always pause. The general rule for every synthesized key, a
Keyboard row that can leave the keyboard out while a controller is connected, is
[#152](https://git.magicbean.dev/MagicBeanDev/Longshot/pulls/152).

## Vibration

The game sends a value per side: a low and a high band, an amplitude and a frequency each.
[`host/vibration.h`](../../../host/vibration.h) picks what plays each controller, and `host/pad_sdl.c` plays it:

| Vibration row | Plays |
|---|---|
| Auto (default) | Through RenHDrum, every pad it plays: a DualSense's voice coils (on a cable its own sound card, over Bluetooth HID report `0x36`), an Xbox One or Series pad's four motors, a Switch Pro Controller's or Joy-Con pair's HD rumble, a Steam Controller (2026)'s grips (HID report `0x88`, experimental in RenHDrum). Without RenHDrum, or what it needs: a Switch pad (and a Joy-Con alone, always) its own HD rumble, every other pad its two motors (`rumble_map` in `nn_hid.c`) |
| Basic | Every pad's motors |
| Off | Nothing |

DualSense HD Haptics (`dualsense_hd_haptics=`, on by default, `HOS_OPT_DS_HD_HAPTICS`) is RenHDrum's for a DualSense
it plays: its speaker taps (channel 1 of the cable's sound card, or Opus inside Bluetooth report `0x36`) and a held L2 or
R2 kicks back (`rh_ds_triggers`, effects blocks through SDL's `SendGamepadEffect` on the cable, inside `0x36` over
Bluetooth), the motors' heavy hits a little deeper. Off: the speaker at 0, the triggers released and Bluetooth's plain
`0x32`, as RenHDrum 0.4 played.

Vibration strength (`rumble_strength=`, 25 to 200 %) applies to every backend. RenHDrum
([`host/rumble_renhdrum.c`](../../../host/rumble_renhdrum.c), source in
[`third_party/renhdrum`](../../../third_party/renhdrum)) is loaded by full path from beside the game (`renhdrum.dll`,
`renhdrum.so` on Linux); without it, every pad keeps its motors or its own HD rumble. `python -I scripts/renhdrum.py
build` makes it; `nx2/build.py` copies it beside the game from `third_party/renhdrum/`.

## Sticks

1. The Stick dead zone applies to each stick as a pair, as a circle (`hos_filter_stick`).
2. (SDL controllers) An axis steers only once it has been seen at rest since the pad was opened: inside the dead zone, or pushed past its
   centre the other way (a worn stick). This stops a device whose axis reads held for good (a trigger mapped onto a
   stick, a second virtual pad) from moving Link or the camera. Rest is judged per axis, with a fixed orientation.
3. Camera inversion is applied after rest detection (SDL controllers; XInput pads get only the dead zone and
   inversion), to the output only. Inverting before it would let a settings change
   look like movement and permanently admit a phantom axis.

## Triggers

An SDL controller's trigger presses ZL or ZR past 8000 of 32767, and only once it has read let go (8000 or below) for a
second without a break since the pad was opened, each trigger on its own (`trigger_pull` in `host/pad_sdl.c`). A device
whose trigger reads held for good (#2327: a ShanWan USB receiver, 2563:0575) would otherwise hold ZL and ZR from every
pad. It would also keep the Controls page from listening for a binding, since that waits for every input to be let go.
And once the Insert menu closed, it would keep the player's own triggers from the game, since an input down at the
close waits to be let go. One let-go reading is not enough: with that receiver, "seen let go once" still let its
triggers through. A real trigger rests when its pad opens, so it counts a second later. A pull in that second, or one
held as the pad opens, counts once the trigger has been let go for a second. A DualSense's HD Haptics triggers read the
same admitted value.

The log names the pad and the reading, once each per pad and trigger:

- `hid: "<pad>": its left trigger reads 16384 of 32767 and has not read let go for a second since it was opened, so it
  does not count until it does`: not counting 3 s after the pad opened;
- `... its left trigger has read let go for a second (0 of 32767): it counts from now`: counting after that line;
- `... its left trigger has read held (16384 of 32767) for 30 s without a break`: held 30 s once it counts. This is a
  player's long pull, or a device whose trigger was let go for a while and then held, which the rule lets through.

At debug, each trigger's first reading and its first changes between let go and held (nine lines at most per trigger),
each with its time since the pad opened: `... its left trigger reads 16384 of 32767 (held), 0.004 s after it was
opened`. They show how a device's trigger reads as it opens.

## The free pointer

While the Insert menu (or, on Windows, one of the game's own menus) is open, the mouse is released and the renderer
reads a pointer sample instead of the window system. [`host/display_pointer.h`](../../../host/display_pointer.h) holds
it: position and window size in physical pixels, held buttons, pending presses and wheel motion.

- The window thread publishes (`hos_pointer_publish`); the renderer consumes (`hos_pointer_take`). The lock is a short
  SRW lock (a pthread rwlock on Linux), and no window or SDL call runs while it is held.
- A take consumes presses and whole wheel notches once and keeps fractional notches. The first pending press keeps the
  position and size of its own event, so a later move or resize cannot move the click.
- Reset (focus loss, minimise, mouse capture) discards input but keeps the window size, which the renderer's
  drawable-size callback reads.
- Windows samples on its DPI-aware window thread; button messages publish their own client coordinates. The game's
  menus get a separate mailbox, so they never drain the Insert menu's input. The Insert menu has wheel priority.
- SDL (Linux, [`host/posix/display_sdl.c`](../../../host/posix/display_sdl.c)) queries window size, pixel size and the
  cursor on the main thread only. Coordinates are scaled to pixels before rounding down, so fractional and negative
  positions survive. A zero or invalid size accepts no input. Linux has no game-menu pointer.

The Insert menu works in swapchain coordinates, independent of the game's letterbox (`vk_menu_hit.h`).

## Input recordings

An input recording writes down what the player's input does to the game, where it reaches it, and gives it back: the
same route and the same presses on every run, for tests, bug reports and `dev.py ab --route`
([`host/input_movie.h`](../../../host/input_movie.h) the format and its logic, [`host/input_movie.c`](../../../host/input_movie.c)
the switches, the controls and the file). It holds:

- each read of controller No.1 (`npad_states`): the button word and the sticks after every source and filter (pads, keys,
  the mouse as a stick, the script, the bridge, the toggles, the menus' filters), and the flight and free camera cheats'
  inputs;
- each motion read (`GetSixAxisSensorStates`): the gyroscope's readings the game is handed;
- each player camera update (`hos_mouse_camera`, `host/mouse_native.c`): whether the native mouse camera was on, whether
  the camera was free, whether a stick turned it, and the mouse's turn in degrees;
- the mouse in the game's menus (`host/oot_menu.c`, Windows): the focus changes and Items page choices it makes by
  calling the game (its clicks and scrolls are the controller's). A replay hands a focus only to one of the screen's
  buttons then, and an item only when it is one of Key Items'; any other is counted as not met;
- the Debug tools' actions where the game takes them (`oot_update_player_cheats`, `host/hos_main.c`): teleports, the age
  switch, items, Mark and Return, the hour, from the Insert menu, `HOS_INPUT_LIVE`'s `!cheat` or the control bridge;
- the Debug tools' rows that change what the game does (heart protection, flight, the free camera, Link's speed, damage
  taken), at the start and at each change;
- the loads (anchors), Link's positions and the camera's state (the fingerprints, below).

While a recording plays, the game gets it all in place of the player's devices, the script, the bridge, the mouse and the
Debug tools' own actions and rows until it ends; then the input is the player's again. The player's menu changes nothing
in the replay (`rx_option` answers those rows with the recording's), nor the replay the player's menu.

### Starting and stopping

- **In the game:** the Insert menu's Debug tab, its Input recording section (`gpu/vk/menu/input_recording.cpp`).
  **Record input** starts a recording from Link's next update and stops it (to the player's folder,
  `recordings/<date>-<time>-<map>.jsonl`). **Play recording** opens a page: Stop playback while one plays (where it is,
  the corrections), Last recording (after a restart, the folder's newest), then the folder's recordings newest first, or
  No recordings yet. A recording plays from where it started: Link is taken to its age and map first (the Debug tab's age
  switch and teleport, each asked again after 12 s, up to three times) and put at its start; only a map no teleport goes
  to is refused. Link and the camera are held to the recording for its first 30 updates (`MOVIE_SETTLE`: the game's move
  of Link lands an update late), not counted as corrections. While it plays the menu is the compact bar, the pads still
  reach the menu (not the game), and pressing any of the page's rows stops it. **Drift correction** is its `HOS_INPUT_SYNC` (Off, every
  update, every second, every 5 s). The rows work with the Debug tools on or off.
- **Over the control bridge:** `record` and `play` ([the bridge](control-bridge.md)), and `status`'s `recorder`: an
  agent's or a test's way ([replay a recording](../developing.md#replay-a-recording)).
- **From the start, with the switches:**

```
HOS_INPUT_RECORD=run.jsonl HOS_REALTIME_STEP=0 HOS_FIXED_TIME=1767225600 ./longshot      # play, then quit
HOS_INPUT_PLAY=run.jsonl   HOS_REALTIME_STEP=0 HOS_FIXED_TIME=1767225600 ./longshot      # the same input again
```

### How a replay holds to its recording

- **Frames.** `f` is the game frame, by the game clock (its sixtieths of a second), in every line. The game reads the
  controller twice in most frames, from its jobs, while Link updates on another worker, so a frame has one pad:
  recording, its first read takes the input and its later reads get the same; a replay gives every read of a frame that
  frame's pad. The reads each frame had are written down (`frames` lines), and a replay counts the frames whose count
  differs. So a recorded run is not quite an unrecorded one: a frame's later reads skip the merge, and the script, the
  hold toggles and `HOS_INPUT_LIVE`'s file move on once a frame instead of at every read. At a cap above 60 or with Real
  time, several of the game's frames share one of the recording's: a recording made at 60 plays at its own speed in an
  uncapped run, approximately (the log says so at the start).
- **Frame 0** is the frame after Link's first update, so a load that takes more or fewer frames does not move the input.
  `HOS_INPUT_FROM=boot` starts at the game's first read instead (two runs of one nightly took 768 and 551 frames to load).
- **Loads (anchors).** Each of Link's first updates after 10 frames without one (a teleport, a door, the age switch) is
  an anchor, written down with its frame. A replay re-bases its frames at its own anchor to the recording's, and when its
  load takes longer it waits at the anchor's frame (giving that frame's pad again) until its own. The input after a load
  is where it was against the game, however long the load took. A load the recording does not have, or one that has not
  ended 15 s after the recording's, is a divergence.
- **Fingerprints.** After each of Link's updates the recording notes his position when it moved (`mark`), and at each
  camera update its state block (`view`: 14 words from `+0x178`) and inputs (`in`: 13 words from `+0x45c` of the object
  at `0x84c9ede0`). A replay holds each update to them exactly, by its frame and its call within the frame (`i`), so one
  more or one fewer camera update, motion read or Link update in a frame (the jobs' race) leaves the frames after it
  alone. It logs the first that differs, and ends with `input: playback over at frame N of N: ...`.
- **Drift correction (`HOS_INPUT_SYNC`, on by default).** Where a replay leaves a fingerprint, Link is moved back to the
  mark through the game's own move of his character controller (`OneShotPhysicsCctChangePosition`, as Mark and Return
  do), and the camera's block and inputs are written back, at every update (`HOS_INPUT_SYNC=<n>`: every n-th; `0`: never,
  the exact test). Only while Link is within `HOS_INPUT_SYNC_MAX` (1 m) of the mark: further than that, the replay went
  another way (a hit taken, a door missed), which is not float noise. It is said once (`input: the replay diverged at
  frame N: ...`) and nothing is put back after it, so a test fails on it rather than being steered back. Velocities, other
  actors and timers stay the replay's own: a corrected replay follows a route, it does not prove the run. The end gives
  the corrections' count, the largest and the mean.
- **The host's own time-based choices.** A run that records or plays takes the OptimizeChangeBank hold
  (`host/oot_ocb_hold.c`) and the texture heap's compaction gap (`host/oot_tex_compact.c`) by the game clock's time
  instead of the wall's (`hos_input_movie_time`), so they land on the same frame of a replay as of its recording. One
  that records or plays from its start has no movie pacing (`host/nn_vi.c`).
- **An exact replay** needs the same game: the 60 cap with Per frame (`HOS_REALTIME_STEP=0`), `HOS_FIXED_TIME`, the same
  save, settings and build, and `HOS_INPUT_SYNC=0`. The game's jobs and loads still run at the PC's pace, so a replay can
  leave the recording where the game itself does not run the same twice; the fingerprints say where.
- **A replay recorded too.** `HOS_INPUT_RECORD` beside `HOS_INPUT_PLAY` records the replay's own fingerprints, to another
  file, and ends it with a `result` line (how many updates apart, the corrections, a divergence, the anchors). One that is
  the file played (its name, another spelling of it or a link to it) is refused, so the recording is kept, and the replay
  goes on unrecorded.

### Routes

A route is a recording that can be committed: `HOS_INPUT_ROUTE=1` (or the bridge's `record` with `route`) writes no guest
words and no addresses (the camera's angles alone, no menu mouse lines) and names the repository save it starts from in
its header (`HOS_INPUT_SAVE`). `dev.py route record --save <name> --name <route>` records one from a repository save into
`assets/routes/`, `dev.py route check` holds it to what a route may contain (as `tests/test_routes.py` does), and
`dev.py ab --route <route>` measures while it plays. The rules are in [assets/routes](../../../assets/routes/README.md).

### Format

ls-input-recording/1, the JSON lines of mooki's [ls-workbench](https://github.com/mooki000/ls-workbench)
(`play/pilot/inputlog.py`): a header, then one object a line with its frame `f` and `cmd` (`pad`, `cam`, `gyro`,
`motion`, `focus`, `itemkey`, `frames`, `view`, `mark`, `action`, `option`, `anchor`; a replay's own recording also
`result`). A replay passes over that format's bridge commands (`press`, `stick`, `sequence`, `live`), and refuses a
file with a NaN or an infinity in it. A recording other than a route holds the game's memory words: it stays on the PC.

**With nothing recorded or played** every hook returns after one load of the run's mode: nothing is locked, read from
the game or copied for a recording.

## Switches

| Switch | Effect |
|---|---|
| `HOS_NO_SDL=1` | No SDL controllers, XInput only |
| `HOS_XINPUT=1` | Poll XInput as well as SDL |
| `HOS_SWITCH2=0` | Leave a Switch 2 Pro Controller to a bridge program |
| `HOS_INPUT_BACKGROUND` | Controllers count in the background (1) or never (0), over the menu |
| `HOS_RUMBLE_BACKEND` | `renhdrum`: also a Joy-Con alone, and a DualSense whose connection SDL can't tell, through RenHDrum; `motors`: every pad's motors; `hd`: no RenHDrum (a Switch pad its own HD rumble) |
| `HOS_MOUSE_NATIVE`, `HOS_MOUSE_SENS`, `HOS_MOUSE_INVERT_X`, `HOS_MOUSE_INVERT_Y` | Mouse camera settings over the menu |

Scripted input for test runs (`HOS_INPUT_SCRIPT`, `HOS_INPUT_LIVE`) is in [developing](../developing.md#scripted-input). The full list is in
[`switches.def`](../../../gpu/vk/switches.def).

## Checks

| Check | Covers |
|---|---|
| `tests/test_stick_rest.py`, `tests/test_stick_inversion_rest.py`, `tests/test_stick_dead_zone.py` | Rest detection, inversion and dead zones |
| `tests/test_trigger_rest.py` | A trigger presses only once seen let go; one held for good never does |
| `tests/test_display_pointer.py` | Pointer sampling and consumption on both hosts, DPI ratios, concurrent samples |
| `tests/test_menu_hit.py`, `tests/test_game_menu_mouse_buttons.py` | Menu hit regions and the game-menu mailbox |
| `tests/test_steam_pads.py` | Steam's ignore list |
| `tests/test_plus_binding.py` | Plus's default keys and a pad's A: Enter (Steam Input's Desktop layout key for A) does not open the pause, the pad still maps by position |
| `tests/test_input_movie.py` | Input recordings: the format, the recorder, playback, the frame alignment, loads, actions and rows, drift correction and divergence, starting and stopping at run time, routes |
| `tests/test_routes.py` | Routes hold only what a route may (`assets/routes/`) |

None of these needs a controller; check a physical device in the game.
