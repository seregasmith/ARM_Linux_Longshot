# Textures under memory pressure on the Odin 3: plan (2026-11-20)

The run of 2026-11-20 (build v3.0.0-52-g104045b3, Turnip 26.2.6, Kokiri Forest) had no refused allocations and no missing
textures after the placed-import fix (104045b3), but it still froze for 1.2 to 3.7 s six times. This is the plan for the
cause of those freezes.

**Status (2026-11-20):** built, steps 1 to 5 in one change, with the window's default (20 s) chosen before step 1's
measurement: a Kokiri Forest run with `RX_VK_PRESSURE_KEEP_S=0` is the measurement before, one without it the measurement
after, and the `made again` line of the first sets the default. Step 1 counts megabytes, not milliseconds. Test:
`tests/test_pressure_keep.py`. Changed after a review of the first version: the floor is what the window drew with the
allocations' padding and a quarter more (a pass over the budget goes down to 7/8 of it, which ate into the window); the
eviction itself leaves every texture drawn in the window (its minimum age is the window's presents); "swap short" holds until
the reserve clears by 256 MB, uses the default reserve when `RX_VK_COMMIT_RESERVE_MB=0`, and the keep needs at least as
much swap free as it keeps.

## What the log and the code show

- **The cut starts at the title screen.** Memory was already short there: 1,279 MB available, under the 1,536 MB reserve
  (`RX_VK_AVAIL_RESERVE_MB`), with 47 MB of textures loaded. `PressureHold` (`gpu/vk/linux_pressure.h`) took those 47 MB as
  its baseline, and the textures' budget fell to its 512 MB floor (`kBudgetFloor`).
- **It never lifts.** The hold lets go only once more than 1,792 MB is available again (the reserve plus `kClearMargin`).
  In play the Odin stays between 170 and 1,000 MB, so the budget was 512 MB for the whole session. The log's VRAM line says
  so at the title (`textures 111 MB (budget 512 MB)`) and in the forest (`textures 1691 MB (budget 512 MB)`).
- **The cut can't be met.** Over the budget, `texture_frame` (`gpu/vk/vk_texture.cpp`) lets go of the oldest textures not
  drawn for 60 frames (`RX_VK_EVICT_MIN_UNUSED`). At 20 fps that is only 3 s. The forest draws 1.1 to 1.3 GB of textures in
  any 8 presents (`used in the last 8 presents`), and the cache stayed at about 1.4 GB all session.
- **So it churns.** Each pass drops whatever was not drawn in the last 3 s, and turning the camera brings it straight back:
  - 16,177 textures let go for the budget (`textures retired ...: budget N`);
  - 2 to 15 textures made every frame in play (`textures made a frame`), where standing still should make almost none;
  - every freeze was mostly texture lookups and uploads, the textures converted again on the CPU while little memory was
    free (one frame made 550 textures, 398 MB, in 2.8 s).

  None of this saves memory: the cache held about 1.4 GB the whole time.
- **It can lose picture content.** Textures the GPU drew are let go with no copy of their texels and come back empty
  (`the GPU drew goes with no copy of its texels ... made again over this storage, it starts empty`).

## Earlier work

- Merged, on main as of 2026-11-19:
  - #223 (`f1b17d59`): this memory-pressure rule itself, written for a 16 GB Steam Deck OLED where swap filled up and
    earlyoom killed the game.
  - #637 (`8e3dfde6`): the same Kokiri Forest churn on graphics cards under 8 GB, fixed with an 1,800 MB minimum
    (`vram_compact.h` `texture_min_floor`). It does not help integrated GPUs: the Linux hold's `std::min(budget, held)` comes
    after it.
  - #1559 and #568: related (the integrated budget with ASTC decoded on the CPU; staging arenas kept), not the hold's floor.
- Not checked yet: open pull requests, issues, the team chat and Hookshot (the forge was unreachable from the session that
  wrote this). Before claiming it: `python -I ../Tooling/scripts/forge.py search --state all` with "memory pressure",
  "texture budget", `PressureHold` and `RX_VK_EVICT_MIN_UNUSED`.

## The change

1. **Measure first (log lines only).**
   - Count the textures made again after they were let go for the budget, and how long they were gone: under 3 s, 3 to
     10 s, 10 to 30 s, 30 to 60 s, over 60 s. Key: guest address, format and size.
   - Log the megabytes and milliseconds spent making them again.
   - Log the bytes of textures drawn over the last 3, 10 and 30 s beside `used in the last 8 presents`.

   One Kokiri Forest run with these lines picks the window below and is the measurement before the change.
2. **The floor follows what the game draws.** While the hold is on, the floor becomes the bytes of the textures drawn in
   the last N seconds, plus 10%, at least 512 MB and at most the normal integrated budget (2,560 MB, `device_policy.h`).
   - Measured in seconds, not frames: frames are 1 s at 60 fps and 3 s at 20 fps.
   - Re-checked at every sample (every 30 frames), so the floor rises from the title screen to the forest. Today the hold
     keeps the floor it entered with: `PressureHold::step` applies the floor again at every sample.
   - Anything not drawn for longer than N seconds still goes, oldest first, as now.
3. **The emergency stays.** When swap is nearly gone too (MemAvailable plus SwapFree under the 2,560 MB commit reserve, the
   Steam Deck case the rule was written for), the window drops back to today's 60 frames. The Odin's case (little RAM
   free, 9.8 GB of zram swap free) gets the long window.
4. **The log says why.** The pressure line names the floor and its reason, for example
   `held at 1300 MB: drawn in the last 20 s`.
5. **A switch.** `RX_VK_PRESSURE_KEEP_S`: the window in seconds, on by default with the value step 1 picks; 0 brings back
   today's behaviour. A row in `docs/development.md`. (To be agreed before it is built.)

The window's state (the frame times that turn seconds into frames) lives in static locals of the hold's block in
`texture_frame` and in `linux_pressure.h`, so a test that cuts that block compiles on main as well.

## Test (no game; it fails on main)

- Run `texture_frame`'s hold block, cut out of `vk_texture.cpp`, with stand-ins for `/proc/meminfo` and the textures:
  - **Title screen to forest:** 50 MB of textures and 1,279 MB available, then 1.4 GB of textures, 1.2 GB of them drawn in
    the last 20 s, 250 MB available and 9.8 GB of swap free. The budget must be at least what the forest draws. Main gives
    512 MB.
  - **Steam Deck case (swap nearly full):** the budget falls as it does today.
  - **Limits:** never above the normal budget; the hold lets go as before once memory recovers.
  - **The switch:** `RX_VK_PRESSURE_KEEP_S=0` gives today's budget.
- New cases in `tests/linux_pressure_test.cpp`: the floor rising while the hold is on.

## Measured in the game

The same spot in Kokiri Forest, the same settings, before and after:

- Should improve: textures made per frame (2 to 15 now), textures let go for the budget per minute, freezes over 1 s
  (6 now) and the worst frame time.
- Must not get worse: MemAvailable and SwapFree move by at most the extra textures kept (a few hundred MB); fps does not
  drop; an out-of-memory kill is a failure.

## Left for later

- **Staging arenas:** under pressure they are given back after 5 s idle and made again at 100 to 700 ms each (24 times,
  about 3.9 s in all in this run).
- **The reserve itself:** on a 15 GB device with zram, 1,536 MB of MemAvailable is crossed the whole session, so every
  pressure valve is on from the title screen.
- **The window and the output size:** gamescope gives the game a 1280x720 window while it makes a 2048x1152 frame at 75%
  (1536x864): two scalings, and the GPU draws about 44% more pixels than are shown. The ini's `output_res=1920x1080` was not
  applied (the log says `the game's at graphics scale 1`).

## Risk

Textures keep more RAM while memory is short, so a little more of everything else goes to zram. The window bounds it, and
the emergency case keeps today's behaviour. The Steam Deck case can be checked only in the test, not on hardware here.
