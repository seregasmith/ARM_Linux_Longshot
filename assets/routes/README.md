# Routes in the repository

Input recordings that tests and `dev.py ab --route` play back, so a measurement or a test follows the same moving
route on every PC, not Link standing still after a teleport. A route is recorded by the game itself
([input recordings](../../docs/dev/reference/input.md#input-recordings), `HOS_INPUT_ROUTE=1`). It holds no game files
or data: the controller, motion, the mouse camera's turns, the Debug tools' actions and rows, the loads, and Link's
positions and the camera's angles, all as numbers. No guest memory words and no addresses. Like the play profiles in
`assets/profiles/`, that is why a route can be committed.

## The rules

- **Recorded from a repository save**, `dev.py route record --save <name> --name <route>`. It starts the game from that
  save (`assets/saves/`), at the 60 cap with the Per frame step and the fixed clock, records from Link's first update
  after the save's load, and writes `assets/routes/<route>.jsonl` when you close the game.
- **Checked**: `dev.py route check` (and `tests/test_routes.py` in CI) passes. The header is a route's (`profile`
  `route`, a repository `save`, `from` `play`), and every line is one a route holds, with numbers or hex words only.
- **One route or a change to one per pull request**, saying what it does (where it goes, how long, the loads in it) and
  how many replays of it stayed within `HOS_INPUT_SYNC_MAX` on your PC (the log's `input: playback over` lines).

## Using one

- `dev.py ab --route <route>` measures this checkout's change, or a switch, while the route plays (`--sync N`: Link
  and the camera put back at every N-th update, 1 by default).
- `dev.py route list` lists them; the control bridge's `play` command plays one by path, and the Insert menu's Play
  recording page (Debug tab, Input recording) the ones in your folder.
- In a test, the way it is read: [replay a recording](../../docs/dev/developing.md#replay-a-recording).
