# Fullscreen monitor guide-image review

The canonical first-view Graphics illustration is recaptured from clean fullscreen-monitor topic
`54d25c79ead4d07e22cdcf5f07e481e19f90f78c`, based on main `e335bd5cb3f0b95cedbfa00ccc407621d333fabb`.
The maintained UI-only helper compiles the real menu, translations and bundled ImGui, renders through SDL2 software
geometry, and loads only bundled Libertinus fonts. The image is 1280×720, English, Medium scale, keyboard prompts,
illustrative Custom defaults and a synthetic 16:9 display. No game, Vulkan device, monitor probe, settings file,
save, private path or game-derived asset is used. This is not an official-binary or gameplay screenshot.

The old a06f7e2e image and final first-view illustration were visually compared, including the Quality preset help,
labels and navigation. Current version text and scrollbar reflect the newer source. The first Graphics
view remains the guide's Quality-preset illustration. The Custom values are examples rather than recommendations.
The PNG is copied unchanged from the helper output. Its caption, compiler-discovered source dependencies and actual
compiler/SDL2/libpng versions are recorded in the manifest. New monitor_choice.h is included in the captured inputs.
The content-derived reviewed revision hashes the sorted path=hash pairs with final newlines, separately from the
exact capture commit. Launcher images, original capture identities and historical reviews are otherwise unchanged.

The PR also carries a matched Screen-section pair: before e335bd5c and after 54d25c79. Both use the helper from
54d25c79, identical defaults and display geometry, with Fullscreen selected and settled scroll at the Screen heading.
The after image shows Fullscreen monitor below Fullscreen, defaulting to Auto (current monitor). All displayed labels
and help come from the maintained source; no text is overpainted. No named monitors are simulated or claimed by these
captures. Actual Windows multi-monitor and Linux single-display behavior is documented separately in the PR.

The original first focused-capture attempt selected Fullscreen without settling its scroll position, leaving that
row offscreen. It is retained only as private failed evidence and is not published. The fixture-only correction
does not change game behavior or the standard first-view Graphics capture.

Offline source/asset checks and rendered website review are recorded in the PR. A successful image hash check
does not establish runtime monitor behavior. Website build failures also present on matching main remain explicitly
separate from guide-image integrity and source review; no unavailable backend or display coverage is claimed.

## Website validation

Longshot image integrity/source review and all 14 screenshot regression tests pass. Focused catalogue integration
checks pass; the complete Graphics test file retains two matching-main fixture failures (HDR Native's negative
case and the old Threaded rendering restart expectation). The normal website build stops on existing multi-row
tested-configuration fragments, whose schema requires one data row per file. Those upstream records are unchanged.

A source-only preview of committed 29267466 excluded exactly four invalid baseline catalogue fragments using the
production validator, retaining 486 valid records. The guide, menu, README and canonical PNG were unchanged in this
private catalogue fixture. Its build generated 20 pages. Six desktop/mobile/light/dark states and eight native
100/125/150/200-percent zoom states passed image loading/hash, overflow, caption, keyboard image-link navigation
and monitor-setting-card checks. Expanded monitor choices were also captured in all six viewport/theme states;
sample desktop/mobile/200-percent images were visually inspected. Owned headless browsers exited normally with
no page-console errors or HTTP 404s; no desktop input, game or user profile was used.

The catalogue-fixture preview is layout evidence, not a passing production build. Its `npm run check` stops on the
existing progress-feed validation failure before reaching the complete Node suite. Neither production guard is
disabled or relaxed by this PR. Those gates and unavailable runtime/backend coverage remain explicit in the PR.

## Retention after rebase onto 8e9cfc3a

Source review at 0aa01320 retains the original 54d25c79 captures and their exact PNG/capture identities.
Compiler preprocessing confirms the same dependency set. The four changed inputs are upscaler_rows.h
(optional DLSS CNN choices), vk_api.h (main-thread timing and appended monitor callbacks, ABI 28), vk_fps.h
(frame-trace declarations) and vk_menu.cpp (DLSS model choices and an Intel compiler hint outside this view).
The illustrated first Graphics view and Screen controls/defaults/help are unchanged. Reviewed-source hashes
advance to the inspected content; no capture, website preview, game build or runtime test is relabeled as new.
The SDL2 retarget correction changes host behavior, not the illustrated menu. The earlier native measurements
remain evidence for their original instrumented e335bd5c base, not fresh FPS measurements of the newer renderer.

## Retention after rebase onto f88e6e3a

Preprocessing adds only `gpu/vk/vk_new.h` and removes no input. The changed existing inputs are
`vk_api.h` (upstream out-of-memory callback and appended ABI-29 monitor callbacks), `vk_internal.h`
(allocation-handler declarations) and `vk_recorder.h` (nothrow-allocation guards using the new header).
These allocation and interface changes do not alter the illustrated menu rows, values or help.
The reviewed-source dependency hashes advance; the original capture revision, fixture and PNG identities
remain unchanged. No new image capture, game build, runtime check or performance measurement is claimed.

## Retention after the performance sample interface (ABI 30)

The only changed input is `gpu/vk/vk_api.h`: `MKWVK_API_VERSION` 30, `perf_info` gains the player pose
outputs, and an appended `perf_tracking` callback turns the opt-in per-second sample on and off. No other
recorded menu input or capture helper calls either. The illustrated first Graphics view, its rows, values and help
are unchanged. The reviewed-source hash advances to the inspected content; the capture revision, fixture and PNG
identities remain unchanged. No new image capture, game build, runtime check or performance measurement is claimed.

## Retention after the perf sample follow-up

The only changed input is `gpu/vk/vk_api.h`, and only in comments: `perf_info`'s comment now gives the pose's order and
units and when `has_pose` is 0, and `perf_tracking`'s says when it is called. No declaration, value or recorded menu
input changes. The illustrated first Graphics view, its rows, values and help are unchanged. The reviewed-source hash
advances to the inspected content; the capture revision, fixture and PNG identities remain unchanged. No new image
capture, game build, runtime check or performance measurement is claimed.

## Retention after #1309 (2026-11-17)

Retained by comparison, not by recapture. `scripts/check_guide_screenshots.py --review-sources` failed on main with seven
changed recorded inputs of `settings-graphics.png`; the last main commit it passed on is `b50f49771` (#1383). The maintained
helper (`scripts/screenshots/capture_menu.py`, main's copy and fixture for every render, so only the source differs)
rendered the Graphics view from `b50f49771`, from `67efc4d3e` (#1309), from `c14641a6f` (#1362 on top of #1309) and from main
`597c5a4e0` (#1464 on top) with the same compiler and libraries, and the pictures were looked at. From `b50f49771` to
`597c5a4e0` the two renderings differ only in a 135 by 14 pixel box of the top banner, the build's version text (976 of
921,600 pixels); no tab, row, value, help text, button prompt or layout differs. The comparison is between renderings, not
against the published PNG:
a render on the current Linux toolchain (clang 23.1.3, SDL2 as sdl2-compat 2.32.74, libpng 1.6.59; the manifest records
clang 23.1.6, SDL2 2.32.10, libpng 1.6.57 for the PNG) differs from the PNG in 60,473 of 921,600 pixels (the version text, the
rows' and the help's edges, the footer's prompts), also when rendered from #1277's own merge commit, so that difference is
not from the changes reviewed here.

The changed recorded inputs: `vk_internal.h` (from #1309: `#include "device_policy.h"`, `Device` gains `driver`, `facts` and
`policy`, a comment loses a switch name; from #1362: `swap_slots`, `kPresentSlotsMax`, `present_slots_for`), `vk_reflex.h`
(#1309: a comment only), and `vk_framegen.h`, `vk_fsr3fi.h` and `vk_texture.h` (#1464: the declarations `framegen_hud_before`,
a `hud_mask` member and `texture_alive`, for FSR 3.1 frame generation's HUD mask), none of which draws anything; `vk_menu.cpp`
and `menu_i18n.h` (#1362: one sentence added to the two values' hints of the Frame pacing row, and to their translations). The
Frame pacing row is on the Debug tab, not on the Graphics tab this image shows. The illustrated first Graphics view, its rows,
values and help are unchanged, and so are the guide's alt text and the steps beside the image.

The dependency list gains the ten headers `device_policy.h` brings in through `vk_internal.h` (`astc_bc_policy.h`,
`device_policy.h`, `fallback_size_policy.h`, `gpu_clock.h`, `mesh_mode.h`, `mirror_budget.h`, `retired_give_back.h`,
`target_pool.h`, `texture_budget.h`, `vram_compact.h`), found by the helper's compiler pass: they were not recorded
because `vk_internal.h` did not include `device_policy.h` before #1309. The reviewed-source hashes and the review revision (the
SHA-256 of the sorted file/hash lines) advance to the inspected content; the capture revision, fixture and PNG identities
remain unchanged. No new image capture, game build, runtime check, website preview or performance measurement is claimed.

## Recapture after #1629 and #280 (2026-11-18)

Recaptured, not retained. The menu's tab strip and banner changed since the last review, so the first Graphics view the guide
shows no longer matches the published picture: the NFC tab (#280) makes the strip six tabs and spaces it for six, and #1629
moved the banner (the Longshot logo and name at the left of the tabs and the build's version at the right; it was
"Longshot" and the version at the left). `scripts/check_guide_screenshots.py --review-sources` failed on main `5e33d3b71a`
with 24 lines for `settings-graphics.png`: 22 recorded sources changed since the previous review (#1466, `415a8944c`, the
last main commit the check passed on), `gpu/vk/vk_vendor.h` was deleted (#1390), and the fixture `scripts/screenshots/menu.cpp`
changed (#1486).

The capture helper did not link on main: #1629's banner calls `ui::logo`, which `gpu/vk/vk_imgui.cpp` defines and the fixture
does not compile (`undefined reference to mkwvk::ui::logo(float*)` on `2f1c0c9c1`; before #1629 only the start-up screen called
it and the linker dropped that). The fixture now defines it with `assets/longshot.png`, decoded to the straight RGBA bytes the
renderer's header holds, and the helper records that file among the capture inputs. Rendering `310e7c9df` (before #1629) with
main's fixture and with the new one gives byte-identical PNGs, so nothing a source without the logo draws changes.

What was compared. The maintained helper (`scripts/screenshots/capture_menu.py`, this change's copy and fixture for every render,
so only the source differs) rendered the Graphics view from `415a8944c`, `310e7c9df`, `6aa5d6536` and `da91e7702` (#1629 and its
parent), `2f1c0c9c1` (the commit tagged v3.0.0-rc.8, which the picture is rendered from) and `5e33d3b71` (main now, one commit
later: #1675), on one toolchain (clang 23.1.3, SDL2 2.32.74 as sdl2-compat, libpng 1.6.59). Each render was compared pixel by
pixel and the pictures were looked at:

| Pair of renders | Differing pixels of 921,600 | Where |
|---|---|---|
| `415a8944c` and `2f1c0c9c1` | 13,901 | x 18 to 1261, y 0 to 54: the tab strip, the logo, the name and the version text |
| `415a8944c` and `5e33d3b71` | 13,999 | x 18 to 1260, y 0 to 54: the same parts |
| `310e7c9df` and `6aa5d6536` (before #1629) | 1,409 | x 18 to 215, y 25 to 38: the version text only |
| `6aa5d6536` and `da91e7702` (#1629 alone) | 2,692 | x 18 to 1258, y 17 to 43: the banner |
| `310e7c9df` rendered with main's fixture and with the new one | 0 | the fixture's logo changes nothing a source without it draws |
| `2f1c0c9c1` and `5e33d3b71` (#1675) | 444 | x 1184 to 1261, y 25 to 35: the version text only |
| `2f1c0c9c1` rendered twice | 0 | byte-identical PNG and `menu-capture.json` |

So the whole difference is the banner and #280's NFC tab: rows 0 to 54. Every row, value, help sentence, button prompt, the
scrollbar and the layout below the strip are pixel-identical from `415a8944c` to main now. The published PNG differs from a render
of the old source on this toolchain in about 60,000 pixels (glyph edges and borders a pixel off; the manifest recorded clang
23.1.6, SDL2 2.32.10 and libpng 1.6.57 for it), which is why the comparison is between renders and why the picture is replaced
by a render on the current toolchain, recorded as such. The new picture was read whole at full size, and the banner of the
published picture and of the new one side by side at twice the size: the logo, the name, the six tab names and the version read
as the menu draws them. The cause of the pixel offsets is inferred, not tested against the recorded toolchain, which is not
installed here.

The picture is rendered from `2f1c0c9c1` (v3.0.0-rc.8) and `reviewed_source` follows the tree after it: #1675 changed one
recorded file, `vk_temporal.h`, in two comment lines, and its render differs from the picture only in the version text, which
reads "v3.0.0-rc.8 dev" for a build one commit after the tag.

The recorded inputs, one by one, with what changed since `415a8944c` and whether it alters what the picture shows:

- `assets/longshot.png`: new input, no change to the file: the logo the banner draws since #1629, loaded by the fixture. The helper records it from now on, so a changed logo asks for a review.
- `gpu/vk/device_policy.h` (#1664, #1559, #1390, #1296, #426): no. Rules the renderer applies to a device (the texture budget when ASTC is decoded on the CPU, Threaded rendering's Auto by cores, the vendor fixes moved here from `vk_vendor.h`, Mesa Intel's limits) and the start's log text; the menu does not read them and the fixture has no device.
- `gpu/vk/fallback_size_policy.h` (#426): no. Phi and call limits for Mesa's Intel driver, used by the shader preparation.
- `gpu/vk/menu_i18n.h` (#1486, #1318, #1442): no. The other languages' tables and a helper that takes each table's length; the English strings are the lookup keys in `vk_menu.cpp`, and the picture is English.
- `gpu/vk/mirror_budget.h` (#1339): no. A 6 GB card's share of the buffer mirrors and a comment, used by the renderer's memory setup.
- `gpu/vk/texture_budget.h` (#1559): no. An integrated GPU's texture budget when ASTC is decoded on the CPU.
- `gpu/vk/threaded_rendering.h` (#1664): no. Auto's tiers by cores (4 and 6) in place of 12 logical processors, and the start's log line. The Threaded rendering row is on the Debug tab, and its hint is not the one this view shows.
- `gpu/vk/vk_api.h` (#1663, #280, #1150): no. `MKWVK_API_VERSION` 30 to 34: the host's rumble test, NFC and `ab_frame` callbacks and the renderer's `nvn_invalidate_quick`; the fixture's host is null.
- `gpu/vk/vk_dlss.h` (#1486): no. Declares `dlss_fg_most` and rewords comments; the fixture answers 0.
- `gpu/vk/vk_draw.h` (#391): no. `ub_overlay` takes a function pointer and an alignment.
- `gpu/vk/vk_fps.h` (#1530): no. `perf_frame` takes the pipeline waits of the frame.
- `gpu/vk/vk_framegen.h` (#1538, #1496, #1486): no. Declares `fg_latency_swapchain_gone`, `framegen_capped`, `framegen_now` and gives `framegen_made_image` an index; the fixture's `framegen_capped` returns the value as set, so Frame generation stays "Off".
- `gpu/vk/vk_fsr3fi.h` (#1487): no. `Inputs::present`, an image view for FSR 3.1 frame generation's HUD-less mode.
- `gpu/vk/vk_internal.h` (#1664, #1530, #1650, #424, #1339, #1536, #1484, #1390, #560, #1150, #1442, #426): no. Includes, `Device` members (`hdr_sdr_why` for the HDR hint, `mesh_only_fallback`, `memory_priority`), declarations for memory checks, frame overlap and the preparation watchdog, and the `vk_vendor.h` include goes; the fixture's `Device` is value-initialised, so `hdr_sdr_why` is none and no HDR hint shows.
- `gpu/vk/vk_menu.cpp` (#1664, #1629, #424, #1554, #1497, #1486, #280, #1318, #1442, #1388): yes, two things. `tab_titles` and `TAB_COUNT` gain the NFC tab (#280), so the strip lists six tabs, and `draw_options` draws the banner as #1629 describes. Everything else is on a row or a help line this view does not show: the HDR row's hint for a swapchain that stayed SDR (#424; the row is below the first view and not selected), the Frame generation hints (#1486, #1497; the row is not selected and its value stays "Off"), the Frame pacing hints, Layout preset, the Controller menu button, the NFC rows, the pipeline-list message, the memory tier, the Present queue row (#1554) and Threaded rendering's Auto line (#1664), both on the Debug tab, and the language names.
- `gpu/vk/vk_menu.h` (#1664, #1554, #280, #1388): no. `Options::pad_menu` (default 1) and `present_queue`, comments, and the declarations of `menu_render_thread`, `menu_recorder_thread`, `menu_memory_tier` and `menu_rumble_tick`; no default of a row this view shows changes.
- `gpu/vk/vk_menu_i18n.cpp` (#1562): no. The Language rows (Gameplay tab) note the names with letters past Latin so the renderer can merge in a font for them; no string or layout of the Graphics view changes.
- `gpu/vk/vk_menu_i18n.h` (#1562): no. The declaration of `names_shown_since`, the other half of the same change.
- `gpu/vk/vk_progress.h` (#1618): no. `PrepareWork`, the shader preparation's progress arithmetic, for the start-up screen, not for the menu.
- `gpu/vk/vk_recorder.h` (#1664, #1543): no. Profiler zone hooks (Tracy builds only) and a comment about the recorder thread's Auto.
- `gpu/vk/vk_temporal.h` (#1447, #1652, #1675): no. Declarations for the hidden-mesh rule and for DLAA before the bloom, and #1675's two comment lines (that DLAA's default is now off). The picture is rendered from the commit before #1675; rendered from #1675's too, the views differ only in the version text (444 pixels).
- `gpu/vk/vk_texture.h` (#1162, #1339, #1553, #1150, #1122): no. Texture cache members and declarations (generation counters widened to 64 bits, `texture_complete_writes`, the `texture_ahead_*` functions); the menu draws none of them.
- `gpu/vk/vk_vendor.h` (#1390): deleted, no. Its three fixes are in `device_policy.h` and nothing includes it; the helper's compiler pass no longer lists it.
- `host/posix/windows.h` (#1553): no. The POSIX shim's `CONDITION_VARIABLE` gains a waiter count; nothing the menu draws.
- `scripts/screenshots/capture_menu.py`: fixture. It passes the logo's path to the fixture and records `assets/longshot.png` among the capture inputs.
- `scripts/screenshots/menu.cpp`: fixture. #1486 added two stubs (`dlss_fg_most` 0 and `framegen_capped`, the value as set), which draw nothing; this change adds `ui::logo` and the logo's texture, as above.

Recorded inputs added: `assets/longshot.png`; left: `gpu/vk/vk_vendor.h`. The capture identity moves with the picture: `captured_source` is `2f1c0c9c19` (`v3.0.0-rc.8`), its files and the fixture hashes are the helper's `menu-capture.json`; `reviewed_source` is this tree's, and its review revision (the SHA-256 of the sorted file/hash lines) is `sha256:bcf6cb05c43b...`.

The published picture's caption in `docs/graphics-and-controls.md` names the new build. The guide's alt text and the steps beside
the picture are unchanged: the view still shows the Quality preset selected, its rows on the left, help on the right and the key
prompts below. The guide's text does not list the tabs.

Not claimed: a website build or a rendered-page inspection, Windows, a live Vulkan frame, any other tab or row of the menu (only
the first Graphics view is published), or a render with the toolchain the previous PNG recorded.

## Retention after #1678, #1701, #919, #902 and #1502 (2026-11-19)

Retained by comparison, not by recapture. `scripts/check_guide_screenshots.py --review-sources` printed lines for `settings-graphics.png` against main `1013b4347`, and again as main moved to `abd4894a6`, after the recapture above was made on `1055077d6`: `vk_menu.cpp`, `vk_menu.h`, `upscaler_rows.h` and `vk_temporal.h` (#1678, DLAA before the bloom), `vk_internal.h` (#1701, #919 and #902, declarations) and `vk_api.h` (#1502, a comment). #1678 adds a row to the Graphics tab, "DLAA before the bloom", which the menu lists only with NVIDIA DLSS chosen where it runs; the fixture's `dlss_why()` is `DLSS_NOT_BUILT`, so the first Graphics view has the same rows. The helper's render of `1013b4347` differs from the published picture in 444 of 921,600 pixels, x 1184 to 1261, y 25 to 35: the version text ("v3.0.0-rc.8 dev"), nothing else; the helper's dependency list is unchanged (77 files). The fixture also frees the logo's texture before its read error is thrown; the helper renders `2f1c0c9c1` with it to a PNG byte-identical to the published one.

- `gpu/vk/upscaler_rows.h` (#1678): no. `pre_bloom_shown` and `pre_bloom_fixed`, the rules of the new DLAA before the bloom row on the Graphics tab: the row is shown only with NVIDIA DLSS chosen where it runs, and the fixture's `dlss_why()` is `DLSS_NOT_BUILT`, so it is hidden in this view.
- `gpu/vk/vk_api.h` (#1502): no. Comment only: `perf_info`'s comment gives the pose's units and range and the cases where `has_pose` is 0; no declaration or value changes.
- `gpu/vk/vk_internal.h` (#902 #919 #1701): no. Declarations, `texture_pipelines_warm()` (#1701) and `driver_env_note()` (#902), and a `Device` member, `max_vertex_divisor` (#919); the fixture's `Device` is empty.
- `gpu/vk/vk_menu.cpp` (#1678): no. The DLAA before the bloom row with its hint, its saved key (`dlss_pre_bloom`) and the environment override; the row is listed only where `ur::pre_bloom_shown(dlss_why() == DLSS_OK, upscaler())` is true, which it is not in the fixture, so the first Graphics view lists the same rows. The render of main now differs from the picture in the version text only.
- `gpu/vk/vk_menu.h` (#1678): no. `Options::dlss_pre_bloom` (default 0) and its comment.
- `gpu/vk/vk_temporal.h` (#1678): no. `temporal_pre_bloom` returns a bool now, and its comment names the new row; declarations the menu does not draw from.
- `scripts/screenshots/menu.cpp` (ae2b9d69d8): the fixture frees the logo's texture before its read error is thrown (a review nit on the first commit of this change). The error path only: rendering `2f1c0c9c1` with this fixture gives a PNG byte-identical to the published one.

The reviewed-source hashes and the review revision (the SHA-256 of the sorted file/hash lines) advance to the inspected content; the capture revision, fixture identity and PNG remain unchanged. No new image capture, game build, runtime check or performance measurement is claimed.

## Retention after #134, #405, #755, #980, #1556, #1586 and #1590 (2026-11-19)

Retained by comparison, not by recapture. The recapture above was made on `2f1c0c9c1` and its retention followed on `abd4894a6`. After the rebase onto main `a433db328`, `scripts/check_guide_screenshots.py --review-sources` printed lines for `settings-graphics.png` for eight recorded sources: `longshot_env.h` (#980), `device_policy.h` (#1586, #1590), `menu_i18n.h` and `vk_menu.h` (#405), `vk_api.h` (#134, #405), `vk_internal.h` (#755, #1586, #1556, #1590), `vk_menu.cpp` (#134, #405, #980) and `vk_recorder.h` (#755). #134 adds a Mods tab to the strip, but `tab_shown` lists it only when `mods_present()` is true, which needs a host with mods' functions, and the fixture's `host` is null, so the first Graphics view keeps six tabs spaced for six. #405 replaces the Controls tab's rumble and haptic rows with Vibration, Vibration strength and Test vibration; no Graphics row is touched.

What was compared. The maintained helper (`scripts/screenshots/capture_menu.py`, this branch's copy and fixture) rendered the Graphics view of this branch with the toolchain the recapture records (clang 23.1.3, SDL2 2.32.74, libpng 1.6.59). The render differs from the published picture in 1109 of 921,600 pixels, all inside x 1065 to 1261, y 25 to 38: the banner's version text, which is the build's own `git describe` and is longer than the picture's `v3.0.0-rc.8`. Every other pixel, the six tabs, the rows, the help sentence, the button prompts and the scrollbar, is identical. The helper's compiler pass lists the same inputs as the record (no new and no dropped one).

- `gpu/vk/compat/longshot_env.h` (#980): no. A new inline `env_on_or(name, def)` beside `env_on`: a switch that reads as on until it is set. No menu label, row, default or help text is in this header.
- `gpu/vk/device_policy.h` (#1586 #1590): no. The device policy: Intel Mesa's own defaults (the equal-depth bias is for Intel's Windows driver alone) and the software matrix multiplies tried on RDNA 4 with AMD's Windows driver. The fixture gives no device and no Graphics row of the picture reads these fields.
- `gpu/vk/menu_i18n.h` (#405): no. The Vibration rows' texts come into the 13 tables (29 entries) and the old Rumble strength entry and its help go. No Graphics text changes, and the fixture runs in English.
- `gpu/vk/vk_api.h` (#134 #405): no. `MKWVK_API_VERSION` is 35: `HOS_OPT_VIBRATION` (63) and `HOS_OPT_VIBRATION_STATUS` (64) (#405) and five Mods tab functions (`mod_info`, `mod_edit`, `mods_changed`, `cheat_info`, `cheat_set`) with the `MKWVK_CHEAT_*` kinds (#134). Nothing a Graphics row reads moves, and the fixture's host is null.
- `gpu/vk/vk_internal.h` (#755 #1586 #1556 #1590): no. Declarations: `is_rdna4`, the `Device` members `subgroup_min` and `max_workgroup_subgroups`, `pins_32` for the 32-lane stages, and `prof_t` as `constinit`. The fixture's `Device` is empty and none of it is drawn.
- `gpu/vk/vk_menu.cpp` (#134 #405 #980): no. #134: the Mods tab (`TAB_MODS`, `draw_mods`, `tab_shown`), listed only when `mods_present()` is true, which the fixture's null host never makes it, so the strip is six tabs as in the picture; the compact bar draws its page when it is the current tab. #405: the Controls tab's Rumble strength and haptic rows become Vibration, Vibration strength and Test vibration, with the options-file migration. #980: the environment switches read through `env_on`. No Graphics row, value, default or help text changes, and the helper's render matches the picture except the version text.
- `gpu/vk/vk_menu.h` (#405): no. `Options::rumble`'s comment (Vibration strength, 25 to 200 percent), `Options::vibration = 0` and `menu_vibration_status_option()`. No Graphics default changes.
- `gpu/vk/vk_recorder.h` (#755): no. The recorder's thread-local arena is reached through a function-local static, `t_arena()`, so its initializer stays with its use under COFF ThinLTO. No menu or Graphics control.

The reviewed-source hashes and the review revision (the SHA-256 of the sorted file/hash lines) advance to the inspected content; the capture revision, fixture identity and PNG remain unchanged. No new image capture, game build, runtime check or performance measurement is claimed.

## Retention after #1633, #1802 and the helper's module build (2026-11-20)

Retained by comparison, not by recapture. After the rebase onto main `068fb8ebe2` (the docs revamp, #1802) `scripts/check_guide_screenshots.py --review-sources` printed 22 lines for `settings-graphics.png`: 21 recorded sources and the fixture `scripts/screenshots/capture_menu.py`. Two changes on main cause them. #1802's comments-only sweep touched 20 recorded headers and `vk_menu.cpp`; a comparison of each file with its comments stripped (the version this record was made from against main's) finds code unchanged in all of them except `vk_menu.cpp`: `astc_bc_policy.h`, `auto_settings.h`, `device_policy.h`, `gpu_clock.h`, `mesh_mode.h`, `mirror_budget.h`, `retired_give_back.h`, `vk_api.h`, `vk_bug_report.h`, `vk_dlss.h`, `vk_framegen.h`, `vk_fsr3.h`, `vk_fsr3fi.h`, `vk_internal.h`, `vk_menu.h`, `vk_new.h`, `vk_progress.h`, `vk_temporal.h`, `vk_xess.h`, `vram_compact.h`. A comment is no menu label, row, default or help text.

- `gpu/vk/vk_menu.cpp` (#1633, #1802): the Insert menu's rows moved out of this file: each feature's rows are a module in `gpu/vk/menu/` (41 files), described through a model (`gpu/vk/menu_model.cpp`, `menu_model.h`, `menu_module.h`) that orders every tab, and this file draws them from the model's table. The row texts, values, defaults and the order are the modules' and the model's now, so they are in the record: 44 files join it (the three model files and the 41 module sources and headers), none leaves.
- `scripts/screenshots/capture_menu.py` and `menu.cpp` (the fixture): the helper stopped linking again (the menu's headers are found beside `gpu/vk`, and the rows come from the modules), so it compiles `menu_model.cpp` and every module, draws the model's rows, gives the modules' renderer hooks stand-ins that do nothing (no window, device, listed folder or bug report) and fails closed where a module would list a folder or post to a window; its host is empty but for an idle NFC scan, because the NFC Scan row asks the host whether a scan is under way until its text says no more. The fixture hashes advance.

What was compared. The maintained helper rendered the Graphics view of this tree with the toolchain the recapture records (clang 23.1.3, SDL2 2.32.74, libpng 1.6.59). The render differs from the published picture in 308 of 921,600 pixels, all inside x 1207 to 1261, y 25 to 38: the banner's version text, which is the build's own `git describe`. Every other pixel, the six tabs, the rows, the help sentence, the button prompts and the scrollbar, is identical, so the modules' rows draw as the picture shows them. The helper's compiler pass lists 121 inputs: the 77 of the record, less none, plus the 44 above.

The reviewed-source hashes, the fixture hashes and the review revision (the SHA-256 of the sorted file/hash lines) advance to the inspected content; the capture revision and the PNG remain unchanged. No new image capture, game build, runtime check or performance measurement is claimed.

## Recapture on main f1f2dc5b

The game-free fixture rendered the first Graphics view at 1280×720 from main `f1f2dc5b` (nightly 680). Compared with the previous proposed rc.8 image, 61,552 of 921,600 pixels differ. The visible setting labels, values, Quality preset help and keyboard commands still match. The banner version, scrollbar and layout pixels changed, so the guide uses the new capture and its matching caption.

The manifest records the compiler-discovered source files and fixture hashes. The Linux smoke check rendered the same view after the logo validation and cleanup changes. Its PNG matched the guide image byte for byte, so the image and capture revision were retained and the fixture hashes were updated.

The live comparison used the isolated Windows Sprint build. Its executable was built from `cd57d785` and its renderer includes the Controls-only changes in `cbfa7a18`. Both share the Graphics menu code with the capture source `f1f2dc5b`. The maintainer confirmed the six tabs, visible setting labels and order, Quality preset help and keyboard commands match the guide image. The fixture uses synthetic defaults, bundled fonts and an empty background, so the live build version and selected values can differ.

The Settings guide was checked in a local website preview that combined the capture with the HUD visibility and website fixes in this update. Microsoft Edge rendered the image and caption at 1440×1000 and 390×844 in light and dark mode. The image stayed within the content area, had descriptive alternative text, and linked to the unchanged 1280×720 PNG. At the smaller size the full-size link makes the menu labels readable. The site build generated 20 pages, the published-site audit passed, and all 90 website tests passed under Linux.

The Longshot screenshot source check passed. Launcher source checks were skipped because a Launcher checkout is only needed for review of those separate images.
