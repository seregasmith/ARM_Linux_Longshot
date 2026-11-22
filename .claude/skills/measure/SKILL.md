---
name: measure
description: Measure a Longshot change or a switch in the game against main - fps, frame time, GPU time, memory, a log line - and write the numbers up for the pull request's Measurements. Use when the user asks to measure, benchmark or A/B a change or a switch, when a perf, fix or feat pull request needs its before and after, or before marking such a pull request ready.
---

# Measure a change against main

What counts and how the section reads: [standards](../../../docs/dev/standards.md#measurements). The command:
[Measure a change](../../../docs/dev/developing.md#measure-a-change). This is the order, and the mistakes that have
cost whole sessions.

**Bases, packs, runtimes, run folders and frame dumps never leave the PC**; log lines and screenshots are fine. A run
needs the person's own game dump: with none here, say so and stop. Ask before each `dev.py ab` (it runs the game).

## 1. Before the first run

- **Measure the commits the pull request will have**: land review fixes and the rebase first and commit; `dev.py ab`
  refuses uncommitted files (the pack counts them, so the hash would not name what ran).
- **Know what holds the frame**: one profiled run first (`HOS_PROFILE=1`, read with `scripts/perf.py report`;
  [profiling](../../../docs/dev/reference/profiling.md)). A GPU saving moves no fps while the frame waits on the CPU or
  a readback.
- **Pick the scene and settings players use**: `dev.py ab --scenes`; the defaults Auto picks on this PC, or
  `--options <a copy of a player's longshot_options.ini>`. Two setups are two comparisons, never mixed.
- `--dry-run` first: it prints the sides, the runtimes, the scene and the order without running anything.

## 2. Run it

```sh
python -I scripts/dev.py ab --scene hyrule-field            # a change: this checkout against its base
python -I scripts/dev.py ab --env RX_VK_X=0 --scene market  # a switch: set against unset
```

- It builds what is missing (base, pack, runtimes), runs every game through the queue in `timing` mode, A, B, A, B,
  warms each run up until no frame waits for a pipeline, and checks that both sides ran the same settings.
- The base is where this checkout left `main`; agents on one PC share it: ask before moving it.
- A switch: check that unset logs what `main` logs. A switch wired to `HOS_AB` is flipped within each run.
- More runs (`--runs 5`) or longer ones (`--seconds 120`) when the verdict is "within the spread" and the gap matters.
- Exit status 3 (the sides differed) names what differed: fix the setup, don't `--allow` it away unless the change
  itself adds that row.

## 3. What it can't do, by hand

- **Frame pacing is measured on the screen**, with PresentMon during the same scripted route, not from the log
  ([architecture](../../../docs/dev/reference/architecture.md#frame-generations-presents)).
- A route that moves: `dev.py ab --route <route>` (a route of `assets/routes/`, recorded once with `dev.py route record`);
  it refuses a run whose replay diverged. Record a route for it if none goes where you need
  ([replay a recording](../../../docs/dev/developing.md#replay-a-recording)).
- A place no scene, save or route reaches: `dev.py queue run NAME --mode timing -- <your run>`, the
  same save and route each time (the control bridge, `HOS_INPUT_PLAY` or `HOS_INPUT_SCRIPT`), `HOS_OPTIONS_FILE` pinned, interleaved, at
  least two runs each, the median; read back each log's `vk: switches set` line.
- With frame generation on (`--framegen`), count which engine made the frames in the measured stretch only.
- Steam's Game Mode changes RADV's pipeline keys: warm and measure in the same kind of session. A handheld or laptop
  keeps the same power settings.
- An older base: build the merge base as a pack too and compare the two packs.

## 4. Write it up

- Paste `measurements.md` from the run folder (`dev.py ab` prints its path): **Verdict:**, **Setup:** (add the driver),
  the table, the log lines. A switch: the default it ships at and the numbers that chose it.
- Overlapping runs are "within the spread", not a gain. Keep the run folders until the pull request merges.
- Rebased onto a newer `main` later: measure again, or say why the numbers still hold.
- `forge.py pr update <n> --body-file <file> --comment "..."`.

## 5. Stop and ask

- Before using a device someone plays on, or moving a shared base.
- When a run needs eyes (flicker) or hands (a route the bridge can't drive and no recording has: ask the person you
  work for to record one, Debug tab, Input recording).
- When the numbers don't back the claim: report them as they are.
- When the hardware isn't here: *Still to test* names it for the Test Farm or a tester.
