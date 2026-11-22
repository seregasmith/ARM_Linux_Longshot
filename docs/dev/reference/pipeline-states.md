# Pipeline state lists

Every release ships a list of the pipelines play asks for, so the first start's shader preparation builds them before
the title instead of in play. This page covers the lists, how one is added, and the pipeline tour that records the main
one. The record format is in [`assets/pipeline_states/README.md`](../../../assets/pipeline_states/README.md).

## The lists

The sources are in `assets/pipeline_states/`:

- `tour.bin.xz`, written only by the pipeline tour and replaced whole each time;
- `contrib/<name>.bin.xz`, one per contributed list.

Each has a `<name>.md` note (who, GPU, driver, build, settings, route). `nx2/make_game.py` merges them into the game
folder's `longshot_pipeline_states_shipped.bin` at every build. A record holds shader hashes, formats and draw state,
never game code.

A game run writes `longshot_pipeline_states.bin` into the game folder's `cache/`. To add it as a list:

```sh
python -I nx2/pipeline_states.py add <name> <game folder>/cache/longshot_pipeline_states.bin --who ... --gpu ... --build ... --settings ... --route ...
python -I nx2/pipeline_states.py check      # every source and the merge, as the release checks read them
```

`add` keeps only pipelines no other contributed list has. A list depends on its settings: one recorded at 1440p with
DLSS shares few pipelines with one at 360p. Players send theirs with the Insert menu's **Save my pipeline list** (Debug
tab), or `nx2/pipeline_share.py <game folder>`.

Refresh the tour's list before each release, and after a change to the shader translation or to the state a pipeline is
made from. When the record format changes (`MAGIC2` and `SIZE2` in `nx2/pipeline_states.py` for the current version 2
records, `HKSTATE2`; `MAGIC` and `SIZE` are version 1's), convert or re-record every source.

## The pipeline tour

`scripts/pipeline_tour.py` plays the game unattended on Linux through the [control bridge](control-bridge.md) and
writes the tour's list:

```sh
python -I scripts/pipeline_tour.py tour --game <game folder> --out <tour dir> --name play --publish
python -I scripts/pipeline_tour.py run --game <copy> --out <dir> --part child-overworld --first "Market" --stops 3 --skip-opening
python -I scripts/pipeline_tour.py run --game x --out x --dry-run         # the route, without the game
python -I scripts/pipeline_tour.py compare <new>/play.bin <old>/play.bin  # what the new list lost and gained
```

- `tour` runs the parts (`--parts`: opening, child overworld, dungeons, adult overworld) one game at a time in a copy of
  the game folder (`<game folder>-tour`), at nice 19, a fresh game every `--chunk` stops (8). `--publish` writes
  `assets/pipeline_states/tour.bin.xz` and `tour.md`.
- Each game starts with no save, its own data folder and none of your switches. It runs headless at settings that change
  no pipeline's key, uses the Debug tab's inventory and teleports, looks around at each stop, and moves on once no new
  pipeline came for `--settle-quiet` frames (60).
- A memory guard waits for 8 GB available and stops a game that grows too large (`--mem key=value`; `--no-mem-guard`).
- Each run writes `states.bin`, `report.txt` / `report.json` (each step's new pipelines, frames, seconds, memory),
  `log.txt`, `steps.txt` and small pictures in `shots/`. The game folder's own lists are put aside for the run and
  restored after.
- `value <run dirs>` shows each step's pipelines no other step or contributed list has, to decide what to cut.
- Check a changed route or wait by its lists, not the clock: a tour each way on the same PC, then `compare` (or
  `tour --compare-to <old list>`). A change that only saves time loses nothing.
- `--vk-profile <dir>` also records the renderer's play profile
  ([developing](../developing.md#record-the-renderers-play-profile)).

`tests/test_pipeline_tour.py` checks the tour against a fake bridge.
