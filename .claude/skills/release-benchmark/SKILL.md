---
name: release-benchmark
description: Compare the latest stable release against current main (or two other releases) in the game, the numbers a release's What's new quotes - fps, 1% low, p99 - at 1080p (CPU-bound) and 4K (GPU-bound), in several areas, from release builds. Use when asked to benchmark a release, compare stable against main or a nightly, or get the numbers for a release page.
---

# Benchmark a release against main

Both sides are **release builds**: each built by its release's AppImage, the way a player's launcher builds it, never a
dev pack. Ask before the runs (they run the game). Nothing built leaves the PC.

## 1. The two builds

1. The tags: the newest `v*` stable, and main's newest `nightly-<n>-<hash>` (on main's head, or say which commit).
2. Download each release's `Longshot-<tag>-x86_64.AppImage` and `SHA256SUMS` into a folder of its own on a disk (never
   `/tmp`); `sha256sum -c --ignore-missing SHA256SUMS`.
3. Build each, through the queue, beside an existing game folder of the same dump so the 25 GB romfs is hard-linked:

   ```sh
   python -I scripts/dev.py queue run rel-<side>-build --mode build -- <AppImage> --cli --nup <your .nup> \
       --out <dir>/game-<side> --work <dir>/work-<side> --language en-US --romfs-from <a game folder> --compile-shaders most
   ```

## 2. The runs

One options file per resolution, native, no upscaler, uncapped:

```ini
output_res=1920x1080   ; 3840x2160 for the 4K set
resolution=1
dlss=6
frame_rate_cap=6
vsync=2
frame_generation=0
```

```sh
python -I scripts/release_bench.py --a <dir>/game-stable --b <dir>/game-main --label-a v3.1.1 --label-b nightly-786 \
    --options <dir>/options-1080p.ini      # then the same with options-4k.ini
```

Two pairs (A, B, A, B), one game session a run visiting Kokiri Forest, the Market and Hyrule Field. A first start of
each new folder rebuilds its pipeline cache (a few minutes); the area's warm-up waits it out.

## 3. Report

- `summary.md` per resolution: each area's median fps, the Δ, 1% low and p99. Quote fps as the release notes do
  ("the Market 92.7 → 105.2 fps (+13 %), 1% low 60 → 76").
- A run marked **NOISY** (the queue saw the PC busy while it measured) is left out of the medians: run that side again
  (`--pairs 1`, or the run alone) rather than quote it. Overlapping runs are "within the spread", not a gain.
- Say the PC, GPU, driver, the two tags and commits, and that both are release builds.
