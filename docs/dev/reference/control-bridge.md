# The control bridge

The control bridge is a local socket inside the game process. Tools, tests and coding agents use it to drive a running
game and read its state: presses timed in frames, screenshots, the Insert menu's settings, the OoT debug teleport, save
listings, game snapshots and, with a second opt-in, guest memory. Every reply carries the frame it was made at (except
the `busy` sent to a client over the limit).

## Tools

| Tool | What it is |
|---|---|
| [`host/control.c`](../../../host/control.c) | The bridge, in the game. Its hooks live in `host/nn_hid.c` (controller read), `gpu/vk/vk_replay.cpp` (shots), `gpu/vk/vk_menu.cpp` (settings), `host/hos_main.c` (start, map, teleport, age), `host/oot_state_tap.c` (game snapshots and flags), `host/hos_mem.c` (guest memory bounds), `host/nn_fs.c` (save folder), `host/hos_relaunch.c` (relaunch) and `host/rx_backend.c` (renderer) |
| [`scripts/longshot_ctl.py`](../../../scripts/longshot_ctl.py) | The command line: starts a game with the bridge on, sends commands, manages run folders |
| [`scripts/longshot_bridge.py`](../../../scripts/longshot_bridge.py) | The client library the command line, the tours and the tests import: the connection, run folders and their supervisor. Standard library only |
| [`scripts/memtour.py`](../../../scripts/memtour.py) | A memory tour built on the client (Linux): loads a save, teleports through areas and samples RAM, swap and the renderer's `vk: budget:` line |
| [`scripts/pipeline_tour.py`](../../../scripts/pipeline_tour.py) | The pipeline tour, another client (its parts in `scripts/tours/`); see [pipeline states](pipeline-states.md#the-pipeline-tour) |
| `tests/test_control.py`, `tests/control_test.c` | The bridge built with a fake game, driven over the real socket or pipe |
| `tests/test_longshot_ctl.py` | The command line and the client library against a fake bridge, including run folders, readiness and stopping |
| `tests/test_oot_state_tap.py` | Snapshots, flag watches and changes against fake guest memory |
| `tests/test_control_pipe_security.py` | The Windows pipe's DACL in the source, and a Windows compile of the bridge when a compiler is present |

## Turning it on

| Switch | Value |
|---|---|
| `HOS_CONTROL` | The socket path (Linux) or pipe name (Windows: `\\.\pipe\<name>`; a bare name gets the prefix). Off when unset |
| `HOS_CONTROL_MEM` | Guest memory access: `ro` (aliases `r`, `read`) or `rw` (alias `readwrite`), any case. Unset or any other value: off |
| `HOS_CONTROL_PEER_SID` | Windows only: one more user account allowed into the pipe. Environment only, never from `longshot_env.txt` |

```sh
HOS_CONTROL=$XDG_RUNTIME_DIR/longshot/control.sock ./longshot
```

The log says `control: listening on <path> (...)` once it serves, or `control: off (...)` and why it could not. The
easiest way to get a correctly set-up socket is `longshot_ctl.py launch` ([below](#the-client)).

See [switches](switches.md) for how switches are set.

## Security

- **Local and this user only.** A Unix socket on Linux, a named pipe on Windows, never TCP. The socket is mode 0600 and
  clients of another user are refused (`SO_PEERCRED`). The pipe refuses remote clients (`PIPE_REJECT_REMOTE_CLIENTS`),
  claims its name first (`FILE_FLAG_FIRST_PIPE_INSTANCE`), and has a protected DACL for the current user, plus:
  - the game's AppContainer SID, when the game runs in one (each new pipe instance is checked with the game's token);
  - `HOS_CONTROL_PEER_SID`'s account, read and write. It must be a real user's SID (`S-1-5-21-…` with a RID of 1000 or
    more, resolvable on this PC, not a group or well-known SID). A refused value is logged
    (`control: HOS_CONTROL_PEER_SID refused: ...`) and the pipe stays yours alone. Write access lets that account
    create pipe instances too, so set it only to an account at least as trusted as the game's.
- **A private folder.** On Linux the socket's folder must be owned by you and not group- or world-writable (so not a
  shared `/tmp`); otherwise the bridge stays off and logs why. A non-socket file at the path is never replaced, nor is a
  socket a live game answers on; a stale socket is removed.
- **What a client can do:** anything the player can do with a controller and the Insert menu's settings, plus the
  teleport and age switch, writing a PNG anywhere you can write, listing the save folder, reading every GameDataMgr
  flag through `subscribe` (no `HOS_CONTROL_MEM` needed), and quitting or relaunching the game.
- **Guest memory is a second opt-in** (`HOS_CONTROL_MEM`). `ro` exposes all game and runtime memory; `rw` can corrupt
  or crash the game, and the corruption can be saved. Use a throwaway save. Any same-user process could already reach
  this memory: the opt-in limits what the bridge does, not who connects.
- **The game never waits for a client.** All socket I/O is on the bridge's own thread; the controller read takes one
  short lock the bridge never holds over I/O.
- **The client trusts the game no further than it must.** `slots --thumbs` accepts only digit slot names and a bounded
  slice of `caption.sav`; plain output shows control characters from the game as `?`; `launch --save` refuses a save
  folder holding links or anything but plain files and folders.

### Limits

| Limit | Value |
|---|---|
| Request line | 64 KB (longer: `too_long`, rest of the line skipped) |
| `id` | fewer than 200 characters as JSON |
| Clients | 8 |
| Requests waiting | 64 |
| Inputs queued | 256 |
| Memory requests | 8 in flight, 1 MB each, 64 ranges per `mem_read_many` |
| Unread replies | 8 MB, then the client is dropped |
| Any `timeout` | capped at 3600 s |

## Protocol

Newline-delimited JSON. Each request is one object on one line; each reply is one line with the request's `id` (any
JSON value, `null` when absent), `ok`, and `frame` (the game's presents so far):

```
> {"id": 1, "cmd": "status"}
< {"id":1,"ok":true,"frame":5321,"protocol":1,"fps":59.8,"state":"play",...}
```

- Replies on one connection may come out of order (a `status` sent during a pending `wait` is answered first): match
  them by `id`.
- Lines with an `"event"` key and no `"ok"` are subscription events.
- `protocol` is `1`. New fields, commands and opt-in event kinds may be added; ignore fields you don't know.
- On Linux a client may shut its sending side after its requests: the bridge ends its subscription, answers what it
  waits for, then closes.

### Errors

```
< {"id":7,"ok":false,"frame":5400,"error":"bad_request","message":"frames: 1 to 216000"}
```

| `error` | When |
|---|---|
| `bad_json` | The line is not JSON (`id` is `null`) |
| `too_long` | A line over 64 KB |
| `bad_request` | Not an object, no `cmd`, `id` too long, or a bad argument |
| `unknown_cmd` | No such command |
| `timeout` | A `wait`, `shot`, input, teleport, age switch or memory request did not finish in time |
| `busy` | Too many requests waiting, inputs queued, memory requests or clients |
| `unavailable` | The renderer is not up or not Vulkan (shots, settings); the game lacks the feature; `symbols` or an unknown `mem_*`; memory not served by this build |
| `forbidden` | A memory command `HOS_CONTROL_MEM` does not allow |
| `out_of_range` | A memory range outside mapped guest memory |
| `not_found` | No such setting or teleport destination |
| `failed` | A shot could not be written; a setting has no step reaching the value or is fixed; the save folder could not be read |
| `refused` | The game turned a teleport or age switch down; `message` says why |
| `cancelled` | A waited-on input was let go by `release`, or a teleport/age switch was replaced by a later one |

### Frames and inputs

A frame is one of the game's own presents (frame generation's do not count). An input starts at the first controller
read after its request, plus `at` frames, and is held for `frames` frames (1 to 216000; `at` 0 to 216000).

- **Names** are `HOS_INPUT_SCRIPT`'s: buttons `A B X Y L R ZL ZR PLUS MINUS LS RS LEFT UP RIGHT DOWN`; stick directions
  `LUP LDOWN LLEFT LRIGHT RUP RDOWN RLEFT RRIGHT`; axes `LX= LY= RX= RY=` from -32767 to 32767 (positive y is up);
  `SPRINT`, the Sprint control's key held (with the Controls tab's Sprint row on; never a button the game sees). Give
  them as `"A+B"` or `["A", "B"]`, any case.
- **Mixing:** the bridge's buttons add to the player's. On the axes it sets, its sticks replace an input script's; the
  player's pad, keys and mouse still count (larger deflection wins) unless `"exclusive": true`, which shuts them out
  until the input ends.
- **Reply:** once the input is over, with `start` and `end` (the first frame without it). With `"wait": false`, at once:
  `{"queued": 1, "frames": 8}` (`frames`: the longest step's `at` plus its frames). A no-wait input outlives its client; a waited-on one is let go when its client leaves.
- **Default `timeout`:** 30 s plus the inputs' frames at 10 fps.

For a Sprint change, record the exact revision and build used for any bridge run. Compare the base and updated head
using a physical Xbox controller and keyboard on Windows, and a controller on Linux or Steam Deck. With Sprint Off and
On, check release while steering, the player's own A press while sprinting, and the left stick's press in a
conversation; capture the menu before and after. Report the
Linux build, applicable Tier 1 coverage, lint and CI on the updated head, and identify any suite failures also present
on the baseline. A bridge input does not replace the physical-device comparison.

## Commands

Every command the bridge accepts. Common optional arguments: `timeout` (seconds) and `wait` (default `true`) where
listed.

| `cmd` | Arguments | Reply fields |
|---|---|---|
| `status` | none | see [status](#status) |
| `press` | `buttons` (or `button`), `frames` (8), `at` (0), `exclusive`, `wait`, `timeout` | `start`, `end` |
| `stick` | `stick` (`L` default or `R`), `x` and `y` (-1 to 1; at least one required), optional `buttons`, then as `press` | `start`, `end` |
| `sequence` | `steps` (list of press/stick objects, each with its own `at`, also `lx ly rx ry`) or `script`; `units` (`frames` or `hos`); `exclusive`, `wait`, `timeout` | `start` (first step), `end` (last step) |
| `release` | `all` (default `false`) | `released`, `all` |
| `shot` | `path`, `after` (frame), `timeout` (30) | `present`, `path`, `width`, `height` |
| `wait` | `event` (alias `for`), its argument, `timeout` (60) | `state`, `waited`; `map` for `event: map` |
| `option_get` | `name` (optional) | `option`, or `options` (all rows) |
| `option_set` | `name`, `value` (number or boolean) | `was`, `option` |
| `subscribe` | `events`, `every` (60), `log`, `flags`, `changes` | `events`, `every`; then events |
| `unsubscribe` | none | none |
| `teleport` | `to` (index or map name; omit to list), `wait`, `timeout` (10) | `destinations`, or `teleport`, `child`, `adult`, `sent`, `map`, `accepted` |
| `age` | `to` (`child` or `adult`; omit for the other age), `wait`, `timeout` (10) | `age`, `sent`, `map`, `accepted` |
| `saves` | none | `path`, `account`, `option`, `slots` |
| `quit` | none | `quitting` |
| `relaunch` | none | `relaunching` |
| `record` | `action` (`start` default, or `stop`); `path` (default: the player's `recordings/` folder), `route` (`false`: a [route](input.md#routes) holds no guest words) | `path`; with `stop`, `written` (`false`: it had not started yet, or the file could not be written) |
| `play` | `action` (`start` default, or `stop`); `path` (default: the last recording made), `sync` (1: updates between corrections, 0 none), `sync_max` (1 m) | none; errors `busy`, `not_found`, `wrong_map` (it starts on a map no teleport goes to). Link is taken to the recording's age and map first, so a start can take a load: wait on `status`'s `recorder` |
| `mem_read` | `addr`, `size`, `format` (`hex` or `base64`), `timeout` (10) | `format`, `addr`, `size`, `data` |
| `mem_read_many` | `ranges` (list of `{addr, size}`), `format`, `timeout` (10) | `format`, `ranges` (each `addr`, `size`, `data`); a single range replies like `mem_read` |
| `mem_write` | `addr`, `hex` or `base64`, `timeout` (10) | `wrote`, `addr` |

`symbols` and any other `mem_*` name reply `unavailable`.

### status

```
< {"id":1,"ok":true,"frame":5321,"protocol":1,"fps":59.8,"state":"play","stalled":false,"draws":2410.3,
   "replayed":5320,"window":{"width":1280,"height":720},
   "game_picture":{"texture_width":2560,"texture_height":1440,"width":1920,"height":1080},"uptime":95.2,"clients":1,
   "input":{"queued":0,"exclusive":false},"map":"KokiriForest_Child",
   "game":{"event":false,"loading":false,"age":"child",...},"version":"...","renderer":"Vulkan: ..."}
```

| Field | Meaning |
|---|---|
| `fps` | Frame rate over the last second |
| `draws` | Draws a frame over the last ~300 ms (`null` without the Vulkan renderer, with `replayed` and `window`) |
| `replayed` | The renderer's presents (what a shot's `present` counts) |
| `stalled` | No frame for a second |
| `map` | The last map known (`Sequence_CurrentBanc`), read every 250 ms in Link's update; kept through loads; `null` until Link is first seen |
| `game` | The game's snapshot from Link's update (below), or `null` |
| `recorder` | The [input recording](input.md#input-recordings) now: `mode` (`off`, `recording`, `playing`), `started`, `waiting` (a replay waiting for a load), `frame` and `end`, `link_apart` of `link_checked` updates, `syncs` and the `largest` correction in metres, `diverged`, `anchors` met of `anchors_wanted`, and `path` (with `off`, the last recording made) |

`game_picture` records the last native game's present texture dimensions and its crop dimensions. With no crop, both
pairs are the texture extent. The value is `null` until the first valid sample. Interpolated frames leave it unchanged.
`window` gives the swapchain extent. Confirming that a specific frame reached the screen requires a presentation receipt.

`state` is a guess from draws a frame; a new state must hold 300 ms before it is taken (leaving `boot` or `unknown`
happens at once):

| `state` | Draws a frame |
|---|---|
| `boot` | nothing presented yet |
| `intro` | under 11, before any menu |
| `menu` | 11 to 149 |
| `loading` | under 11, after a menu or play |
| `play` | 150 or more |
| `unknown` | no Vulkan renderer to count draws |

Each `status` asks for a fresh snapshot and waits up to 250 ms for Link's next update. On the title, a pause or a load
none comes, so the last snapshot is returned with its original `frame` and `t`. Compare `game.frame` with the reply's
`frame` before steering, and wait for fresh samples with `event` and `loading` false.

| `game` field | Meaning |
|---|---|
| `frame`, `t` | Present number and host monotonic seconds when sampled |
| `map` | The sampled map, or `null` |
| `pos` | Link's `[x, y, z]` in metres (in a cutscene, the actor's position) |
| `facing`, `camera` | Link's and the gameplay camera's yaw in degrees (0 toward +z, 90 toward +x) |
| `event`, `loading`, `age` | An event playing (cutscene, conversation, map change), a map loading, `"child"` or `"adult"` |
| `life`, `max_life`, `rupees` | Quarter hearts, maximum quarter hearts, rupees |
| `prompt`, `music` | The A button's action and the music, as the game names them |

Unreadable fields are `null`, except that `status` keeps `event`, `loading` and `age` typed from their last known
values; with no value known for them, `game` is `null`. Guest reads happen in Link's update, never on the socket thread. A snapshot is not atomic across the whole game.

### wait

| `event` | Until |
|---|---|
| `present` (alias `frames`) | `frames` (alias `n`, default 1) more frames |
| `frame` | frame number `frame` |
| `in_play` | `state` is `play` (at once if it is) |
| `state` | `state` is the given `state` |
| `loading_start` | the next change into `loading` |
| `loading_end` | the next change out of `loading` |
| `change` | the next change of `state` |
| `map` | the map is `map` (any case; at once if it is), or without `map` the next map change |

A timeout replies `timeout` with the `state` at that time.

### shot

A PNG of the first present after the request (or after frame `after`), at the size `RX_VK_SHOTS` writes (1280x720, or
`RX_VK_SHOT_W` x `RX_VK_SHOT_H`). It shows the game's picture without the Insert menu or FPS counter. The file is
written under another name and renamed, so the reply's path is complete.

- No `path`: `shots/shot_<n>_f<frame>.png` in the socket's folder (made 0700).
- A relative `path` is taken from the socket's folder; on Windows from `%TEMP%\longshot-control-<pipe name>\`.

### option_get and option_set

A `name` is the row's key in `longshot_options.ini` (`text_speed`) or its displayed name (`"Text speed"`), any case,
with space or `-` read as `_`. The rows are those the menu shows on this PC with a value: no headings, no Debug tab
tools. Each row is `{key, name, tab, value, min, max, step, restart, fixed, text}`; `restart` means it applies at the
next start, `fixed` means there is nothing to choose on this PC.

`option_set` goes through the menu's own setter, as Left and Right do: clamped to the range, Quality preset turned to
Custom where the menu would, a restart noted, saved to `longshot_options.ini`. Rows with their own step list (Upscaler,
Render resolution, output resolution) are stepped until they reach `value`; if no step does, nothing changes and the
reply is `failed`. A `fixed` row also replies `failed`. For Render resolution, `value` is the saved scale choice and
`text` the active upscaler's supported size.

For a Grass distance farm run, leave `RX_VK_GRASS_DISTANCE` unset so it does not override the row. With `CTL` and
`HOS_CONTROL` set as in the client example below, use `$CTL option set grass_distance 1` for 2x or
`$CTL option set grass_distance 0` for Game's, then `$CTL option get grass_distance` and check both `value` and `text`.
Values 2 and 3 select 3x and 4x. The row is saved, so restore the previous value after the run.

The first `option_get` or `option_set` after start can wait about 3 s for the upscalers' start-up check, holding up
other replies meanwhile.

### subscribe

Streams events on this connection until `unsubscribe`, another `subscribe` (which replaces it) or disconnect.

| Argument | Meaning |
|---|---|
| `events` | Any of `present`, `state`, `log`, `map`, `oot`. Default `["present", "state"]` |
| `every` | Frames between `present` events and between `oot` samples (default 60, 1 to 216000) |
| `log` | Only log lines containing this text (turns `log` on) |
| `flags` | With `oot`: up to 64 flag names (1 to 47 of letters, digits, `_`, `.`) or `0x` plus eight hex digits |
| `changes` | With `oot`: `true` adds changed Bool/Int/Enum/UInt flags by hash |

Events carry `sub`, the subscribe request's `id`:

```
< {"event":"present","sub":10,"frame":5600,"fps":59.9,"state":"play","draws":2405.0,"map":"..."}
< {"event":"state","sub":10,"frame":5720,"state":"loading","from":"play","draws":5.0}
< {"event":"log","sub":10,"frame":5721,"line":"..."}
< {"event":"log_dropped","sub":10,"lines":40}
< {"event":"map","sub":10,"frame":5900,"map":"HyruleField_Child","from":"KokiriForest_Child"}
< {"event":"oot","sub":11,"frame":6002,"game":{...,"flags":{"PlayerStatus.Life":12},"changed":{}}}
```

About `oot` events:

- The first sample comes at the next Link update, then no sooner than `every` presents after the last. `game.frame` dates
  the sample; the envelope's `frame` is the bridge's current present. Nothing is sent while Link is not updated.
- `flags` are looked up in Bool, Int, Float, Enum, String64 and UInt order; missing values are `null`. An Enum value is
  `"#"` plus eight hex digits (MurmurHash3 of its name).
- `changes`: the first sample reports `{}`. The four tables are scanned 4096 slots per present against one shared
  baseline, so a full pass takes `ceil(slots / 4096)` presents, and a value that changes and changes back between scans
  is missed. The log line `control: oot changes scan: ...` gives the real table sizes and presents per pass. Each
  subscriber holds up to 32 pending changes; a nonzero `more` counts the ones dropped, and means re-read the flags you
  care about. Tables over 2^22 slots are rejected.

### teleport and age

`teleport` is the Debug tab's teleport. Without `to`, it lists destinations as `{index, child, adult}` (`null` for an
age with no map). With `to` (an index or either age's map name, any case) the game takes it at Link's next update in
play and goes to the map for Link's age. `age` is the Debug tab's Switch age: Link's age flipped and the current map
reloaded. Both share one ticket, so a later teleport or age switch replaces one not yet taken (`cancelled`).

| Outcome | Reply |
|---|---|
| Taken | `"sent": true`, `map` (where it goes), `accepted` (the game's event manager's answer) |
| Already that age (`age` only) | `"sent": false`; nothing is done |
| Refused | `refused`: an event or load in progress, another teleport in the last 8 s, no map for this age |
| Not taken in time | `timeout`: Link is not being updated (title, file select, pause, load). The request is withdrawn |
| `"wait": false` | at once, `"sent": null` |

After a teleport, `wait map` tells when it arrived. After an age switch, use `wait loading_end`: a dungeon's map has
the same name for both ages. `age` replies `unavailable` before a save is loaded.

### saves

Lists the save folder the game mounts (`<data>/longshot/save/<user id>` on Linux), without reading game memory or
needing `HOS_CONTROL_MEM`. `unavailable` until the game has opened its saves.

```
< {"id":50,"ok":true,"frame":900,"path":".../save/5a3c9e1f0b7d24680000000000000001","account":true,"option":true,
   "slots":[{"slot":"03","mtime":1762900000,"newest":true,"files":[{"name":"progress.sav","size":120724,"mtime":...}],
   "caption":{"scene":"KokiriForest","map":"KokiriForest/KokiriForest_Child",
   "thumbnail":{"format":"jpeg","offset":1396,"size":40843}}}]}
```

Slots come newest first; `newest` is what Continue loads. Times are Unix seconds. `caption` appears only when
`caption.sav` has the known layout; its thumbnail is the JPEG at that offset. Publication replaces files one at a
time. The bridge waits for publication to finish and refuses the request while recovery is pending or the save is
mounted in place. Other programs reading the save folder can observe intermediate states.

### quit and relaunch

- `quit` replies, closes the bridge, runs the renderer's close hooks and the shared close path (which waits up to 3 s
  for a file the game is still writing), and exits with code 0. It never restarts and does not ask the game to save: a
  save the game has not started writing is lost. Watch the process exit separately.
- `relaunch` replies, releases the socket and restarts the game with its original command and environment, as the
  Insert menu's Restart now does. Linux: same process (`execve`). Windows: a new process that waits for the old one.
  The new game serves the same `HOS_CONTROL`. If restarting fails, the game stays exited.

### Guest memory

| `HOS_CONTROL_MEM` | Commands allowed |
|---|---|
| unset, empty or invalid | none (`forbidden`, with how to turn it on) |
| `ro` | `mem_read`, `mem_read_many` |
| `rw` | the reads and `mem_write` |

- Addresses are guest addresses (image at `0x80000000`, heap at `0x1000000000`). `addr` and `size` are whole JSON
  numbers or strings of decimal digits or `0x` hex (use a string past 2^53).
- A range outside mapped memory (image, system region, heap, address regions) is `out_of_range`, never a fault.
- Requests run on the game thread at a controller read: a read is one frame's memory, all of `mem_read_many`'s ranges
  come from the same pass, and a write lands between frames.
- Writes are logged (`control: wrote N bytes to guest 0x...`) and go through the soft write-watch, so the GPU's mirror
  picks them up next frame.

```
> {"id":30,"cmd":"mem_read","addr":2147483648,"size":8}
< {"id":30,"ok":true,"frame":5000,"format":"hex","addr":2147483648,"size":8,"data":"030000141c300000"}
> {"id":31,"cmd":"mem_write","addr":"0x1000000000","hex":"2a000000"}
< {"id":31,"ok":true,"frame":5001,"wrote":4,"addr":68719476736}
```

For Link's position, life, rupees and flags, use `status` and `subscribe` instead.

## The client

`longshot_ctl.py` maps each command to a subcommand. `--sock` names the socket (default `$HOS_CONTROL`; with `launch`,
the socket to start the game on), `--run DIR`
attaches to a run folder instead, and `--json` prints raw replies. Exit status: 0 done, 1 an error reply, 2 no game or
bad usage, 3 a timeout. `python -I scripts/longshot_ctl.py --help` lists everything.

```sh
CTL="python -I scripts/longshot_ctl.py"
export HOS_CONTROL=$($CTL launch ~/Games/Longshot --data /tmp/oot-data --headless)
$CTL wait frame --frame 1 --timeout 900        # a first start prepares shaders before the first frame
until [ "$($CTL --json status | jq -r .state)" = play ]; do
    $CTL press A --frames 8; $CTL wait present --frames 60
done
$CTL stick L 0 1 --frames 90 --exclusive
$CTL sequence '0:A/8,60:A/8'
$CTL sequence '0:LUP/2000,2:A' --hos            # seconds and milliseconds, as HOS_INPUT_LIVE takes them
$CTL shot out.png
$CTL option set text_speed 3
$CTL teleport 1 --arrive 30                     # no argument lists the destinations
$CTL age adult --arrive 30
$CTL watch --events oot --every 6 --flags PlayerStatus.Life,0x31ab5580 --changes
$CTL raw '{"cmd": "wait", "event": "frame", "frame": 9000}'
$CTL mem read 0x1000000000 16 --format u32      # needs HOS_CONTROL_MEM=ro or rw
$CTL relaunch --timeout 120
$CTL quit
```

| Subcommand | Notes |
|---|---|
| `status` | |
| `press`, `stick` | `--frames` (8), `--at`, `--[no-]exclusive`, `--no-wait`, `--timeout` |
| `sequence SCRIPT` | `<at>:<names>[/<held>],...` in frames (8 held by default). `--hos` takes seconds and milliseconds (200 ms default) and converts them to frames at 60 a second, nearest frame with halves up, at least one frame |
| `release [--all]` | This client's inputs and those of clients gone; `--all`, everyone's |
| `shot [PATH] [--after F]` | Prints the path |
| `wait EVENT` | `--frames`, `--frame`, `--state`, `--map`, `--timeout` (60) |
| `option get [NAME]`, `option set NAME VALUE` | `VALUE` is a number, or `on`, `off`, `true`, `false` |
| `teleport [INDEX\|MAP]`, `age [child\|adult]` | `--timeout` (10), `--no-wait`, `--arrive SECS` (then waits for `map`, or `loading_end` for `age`) |
| `watch` | `--events`, `--every`, `--log TEXT`, `--count N`, `--flags`, `--changes` |
| `mem read ADDR SIZE`, `mem write ADDR HEX` | `--format hex\|base64\|u8\|u16\|u32\|u64\|f32` for reads |
| `saves`, `slots [--thumbs DIR]` | `slots` prints one line a slot (`*` newest); `--thumbs` writes `slot_NN.jpg` pictures. `saves --repo [DIR]` lists the repository's saves without a game |
| `relaunch [--timeout 120]` | Waits until a `status` answers from the new process (its `uptime` shorter than the wait), then prints `relaunched: ...`; exit 3 if it does not come back |
| `quit`, `raw JSON` | |

### Launching

`launch GAME_DIR` starts the game with the bridge on, waits for it, and prints the socket path. It runs
`bin/longshot` (`bin/longshot.exe` on Windows), and refuses a folder whose `bin/` lacks the game rather than run the
top-level launcher; a folder with no `bin/` at all (a source build) runs its top-level `longshot`. Each launch also creates a run folder (see [Runs](#runs)).

| Option | Effect |
|---|---|
| `--exe FILE` | A development executable instead |
| `--image FILE`, `--romfs DIR`, `--user DIR` | Passed to the game as its own arguments |
| `--cwd DIR` | Working and cache folder; needs `--image` |
| `--data DIR` | `XDG_DATA_HOME` for the run, so it has its own saves |
| `--headless` | `HOS_HEADLESS=1` |
| `--env NAME=VALUE` | Any extra switch (repeatable) |
| `--save FOLDER [--slot NN]` | Copies a save folder (or one slot, `03` or `slot_03`) into the run's data before start. The source is only read; a destination that already has saves is refused. On Windows, use with `--profile test` or `--user` |
| `--repo-save NAME [--repo DIR]` | Copies `assets/saves/NAME/slot_00` from the repository ([assets/saves](../../../assets/saves/README.md)) as slot 00; the game creates `account.sav` and `option.sav` itself |
| `--profile test` | `HOS_HEADLESS=1`, `HOS_AUDIO=0`, `--user <run>/user`, and `HOS_ENV_FILE` pointing to an empty file so `longshot_env.txt` is not read. Overrides conflicting `--env`; refuses `--data` and `--user`. Inputs sent through the run default to `exclusive` |
| `--ready bridge\|present\|play` | Wait for a `status` reply (default), a first present, or `play` with `stalled`, `game.event` and `game.loading` false. `play` does not press through the title for you |
| `--timeout SECS` | Readiness timeout (default 600). On failure the game is stopped and the run marked failed |
| `--run DIR`, `--root DIR` | A new or empty run folder, or the parent for a unique one (default `runs/control` in the [development folder](build.md#where-dev-data-lives)) |
| `--wrapper JSON` | An argv prefix around the lifetime supervisor (for admission, such as a job queue). It must run the rest of the argv unchanged and wait for it |

On Linux the socket lives in a short private folder (`hctl-*` under `$XDG_RUNTIME_DIR`, else the temporary folder, or
`/tmp` when that path is over 48 bytes); on Windows the pipe is named
`longshot-control-<random>`.

### Runs

A run folder (mode 0700 on Linux) holds `manifest.json`, `log.txt` and `shots/`. A small supervisor process stays the
game's parent, records its exit code and outlives the launching shell.

```sh
$CTL --json launch GAME_DIR --profile test --run work/my-run --ready present
$CTL runs                                  # (default: runs/control in the development folder)
$CTL --run work/my-run show
$CTL --run work/my-run press A --frames 8
$CTL --run work/my-run log --since 1234 --expect 'some line' --timeout 10
$CTL --run work/my-run stop --timeout 10
```

| Subcommand | Effect |
|---|---|
| `runs [ROOT]` | Lists run manifests |
| `show` | Prints the manifest; works after the game exits |
| `log` | Reads `log.txt` without the bridge: `--lines` (100), `--since CURSOR` (a byte offset from an earlier read), `--bytes` (65536, at most 1 MiB), `--expect TEXT [--regex]` (needs `--since`), `--timeout` |
| `stop` | Asks the owner to send `quit` and waits for the game and supervisor. On timeout exits 3 with the game still running; `--terminate` lets the owner terminate, then kill, it |
| `finish` | Marks a run finished whose game and owner are both gone (exit code unknown) |

`--run DIR` checks the game's pid and start time before connecting, and never signals a pid read from disk. Shots
taken through a run without a path go to its `shots/`. After `relaunch`, the run follows the game (on Windows, the new
process) so `show`, `stop`, `log` and later requests keep working.

`manifest.json` (schema version 1) is replaced atomically under `manifest.lock`; readers reject other versions. It
records the run, game, executable, command, explicit environment changes, profile, user and save source, socket, pid
and start identity, owner, times, `state` (`queued`, `starting`, `running`, `stopping`, `finished`, `failed`), readiness,
exit code and stop method, acknowledged shots in `artifacts`, and the last `relaunch` ticket. `save_drain` is never
`clean`: `quit` has no save barrier, so a copied save is not a verified checkpoint.

A run folder is not a sandbox: do not load manifests, wrapper commands or executable paths from an untrusted caller.

### From Python

Import `scripts/longshot_bridge.py`: `start_session(...)`, `list_runs(root)`, `Session.attach(folder)`, `Session.info()`,
`Session.request(cmd, timeout=None, **args)`, `Session.log(...)`, `Session.expect_log(...)`,
`Session.stop(timeout=10, terminate=False)`, `Session.finish()`, and `Session.connect()` for a separate `Bridge`
connection (keep subscriptions on their own connection). Errors raise `Failed` with `code` 1 (an error reply),
2 (usage, files) or 3 (timeout).

## Known limits

- The Windows pipe, `launch` on Windows and remote-client refusal are tested only when someone runs the suite on
  Windows.
- `state` is a draw-count guess and can misread unusual scenes; `game.event`, `game.loading` and `map` are the game's
  own.
- Samples stop whenever Link is not updated, and contain no actor list.
- `HOS_CONTROL_MEM=rw` writes are checked against guest memory, not against what the GPU then draws.
