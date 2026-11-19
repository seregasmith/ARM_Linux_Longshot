# Shaders on the Odin 3: memory plan (2026-11-19)

## What the logs of 2026-11-18 show

Runs in order, oldest first:

- **Build** (setup entry, Mesa 27.0.0-devel, no mesh shaders): 2.6 h. Translation 964 s, then the warm 8,366 s.
  - Memory was already short when the warm began: 1,491 MB available, about 5 GB in swap.
  - It fell to 86 MB available. The memory hold kept it alive by running one worker at a time, which is why it was slow.
  - Mesa's cache started empty: 2 MB in the new `cache/driver`.
- **longshot_log.3** (game entry, **Mesa 26.2.6**, a different driver from the build): the check answered 0 of 257, so the
  warm ran. Memory was short after 20 s. The window was closed after about 30 s.
- **longshot_log.2** (game entry, **Mesa 26.2.6**): the check answered 11 of 257, so the warm ran. Memory was short from the
  first second.
  - It ran for over 70 minutes, stuck at 3,247 of 4,059 compute pipelines.
  - The log ends with no close line, so the process died, or the system went down.
- **longshot_log.1** (game entry, **27.0.0-devel**, the same driver as the build): the check answered 257 of 257 and
  269 of 269, so the warm was skipped (the fix worked).
  - The 2.2 GB `longshot_vk_pipelines.bin` was read at start.
  - The game then sat in `precompile_at_create` with nothing in the log, and the window was closed.
- **longshot_log.txt** (27.0.0-devel): the marker was there, and the driver's cache alone answered 48 of 48. The 2.2 GB file
  was read again. The game sat in `precompile_at_create` again and the window was closed.

**Conclusion.** The driver mismatch and the rebuild that wasn't needed are fixed. The rest is memory:

- The 2.2 GB pipeline cache file is read whole and handed to the driver, which copies it. That comes on top of about
  1.5 GB available.
- Turnip's cache object holds every pipeline built into it for the whole process.
- `precompile_at_create` compiles and keeps the mesh-fallback compute pipelines on 8 threads, with no memory hold and
  nothing in the log.
- The logs have no timestamps and no memory figures, so how long each step took is unknown.

## Done (in the tree, not committed)

Round 1 (already on the device and in the logs above):

- `host/hos_main.c` `driver_cache_shared`: Mesa's cache goes in the game folder's `cache/driver` for every start.
  `driver_env_note` writes the driver's environment to the log.
- `gpu/vk/warm_check.h` and `vk_draw.cpp` `notes_answered_by_caches`: the first start skips the warm when the caches
  answer a sample. `RX_VK_WARM_CHECK=0` turns the check off.
- `gpu/vk/warm_bounded.h` hold: one worker goes on while memory is short. The warm's workers run below normal priority.
- Tests: `test_warm_check.py`, `test_driver_cache_shared.py`, `test_warm_bounded.py`.

Round 2 (written after the logs above, compiled for aarch64, game-free tests pass; **not yet run on the device**):

1. **Memory in the log.** `linux_pressure.h` `parse_self_status` and `memory_words`, and `vk_draw.cpp`
   `start_memory_words`.
   - Every progress line ends with `; memory: this process N MB (M MB more in swap), A MB available, S MB of swap free`.
   - That includes `prepare_progress_log` and vk_main's "still working on" line.
   - Test: `tests/test_start_memory_words.py`.
2. **Turnip on 20 GB of RAM or less: the 2.2 GB file is neither read nor written** (`pcache_load.h`).
   - The new reasons are `kSmallPcTurnip` and `kPreparingIntoDriver`.
   - `mesa_cache()` measures `MESA_SHADER_CACHE_DIR`'s folder, or Mesa's under `~/.cache`.
   - The preparation builds into Mesa's own cache alone. The game doesn't read the file when Mesa's cache holds 512 MB or
     more.
   - `RX_VK_PCACHE_LOAD=1` brings back the old behaviour. Test: `tests/test_pcache_load.py`, with Turnip cases.
3. **`DriverOnlyBuilds`** (vk_draw.cpp): where the rule above applies (`pcache_driver_builds`), builds made only for the
   driver's cache go through a pipeline cache of their own, let go after each program.
   - So nothing accumulates in pcache.
   - Used by the warm (graphics and compute) and by the preparation's compute pipelines (`warm_compute`).
   - Test: `tests/test_driver_only_builds.py`. Mutation checks done: 3 of 4 mutations caught, but I hadn't finished reading
     the last one's result when stopped.
4. **`precompile_at_create`** now runs on `warm_bounded::run`.
   - The memory hold and below-normal priority apply there too.
   - It logs a line every 10 s: "mesh fallback programs' compute pipelines", then "kept programs' noted pipelines".
   - It also logs a line after reading the programs, and memory at the end.
5. `test_warm_bounded.py`'s needle is changed to `own.compute(&ci, &made)`.

## Still to do

- [x] Re-ran the tests that read the changed files, on main (`~/sx/m6`) and on main plus these changes (`~/sx/w6`), with the
      Vulkan headers. The failures that remain also fail on main.
  - Two tests needed updating: `test_intel_quick_compute.py` (the warm's compute build is now `own.compute`) and
    `test_prepare_stall.py` (a stand-in for `start_memory_words`).
  - Mutation checks for `test_driver_only_builds.py`: all 4 caught.
- [x] Docs rows: the `RX_VK_PCACHE_LOAD` row in `docs/development.md`, and the fifth run in `docs/arm64-port-plan.md`.
- [ ] A cold second-agent review of round 2 (fix-with-test skill: threads and memory). Not done yet.
- [ ] Commit, the tests first, then the fix.
- [ ] Remove the leftover temp files from the index:
      `git rm --cached -q third_party/a64-kit/lift/.fuse_hidden* third_party/a64-kit/lift/liba64emit.so.tmp*`.
- [ ] Optional, on the Odin: delete the old 2.2 GB `cache/longshot_vk_pipelines.bin` in the game folder. Under the new rule
      nothing reads or writes it, but nothing deletes it either.

## Next run on the Odin

1. Keep **both Steam entries on the same driver** (27.0.0-devel, the one the build used).
2. Build the AppImage. **Build again** in the setup: the preparation now builds into Mesa's cache alone, and the 2.2 GB
   file isn't made.
3. Start the game and let it run. Check the log for:
   - `pipeline cache file not read: Turnip, ...`
   - `precompile: the caches answer N of 257`
   - `mesh fallback programs' compute pipelines: ... ; memory: ...` lines every 10 s.
4. Send the build log and the game logs. The memory figures will show where the RAM goes.
5. If memory is short before the shaders even start: look at what holds RAM before them (program cache index, placements,
   Steam's Fossilize layer). A next step could be `DISABLE_VK_LAYER_VALVE_steam_fossilize_1=1` in `longshot_env.txt` as a
   test.
