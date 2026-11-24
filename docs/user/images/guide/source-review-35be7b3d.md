# Graphics guide image: source review against 35be7b3d

Retain `settings-graphics.png` after reviewing the 10 UI-source inputs that changed on main `a2dc5385` since the previous
review. The PNG, its dimensions and SHA-256, its caption, capture date, version, `captured_source`, `fixture_files` and
capture conditions are unchanged.

`python -I -B scripts/check_guide_screenshots.py --review-sources` flagged these inputs:

| Source | Change and retention decision |
|---|---|
| `gpu/render/rx_nvn.h` | The C11/C++ static-assert macro (`NVN_STATIC_ASSERT`). Internal to the renderer headers; no menu row. |
| `gpu/vk/device_policy.h` | The ASTC-to-BC small-run policy (`bc_small_runs`) for RADV and NVIDIA's driver. Device policy, no menu row. |
| `gpu/vk/menu/reshade.cpp` | A help line for **Rescan ReShade presets**, on the Post-processing section below this Graphics view. |
| `gpu/vk/menu/sprint.cpp` | Sprint's comment now names `host/oot_sprint.c`; the row is on the Controls tab, not Graphics. |
| `gpu/vk/menu_i18n.h` | Translations for the Output resolution preview, Sprint's Toggle and the default Toggle. The fixture is English; the pictured Graphics labels, values and help are unchanged. |
| `gpu/vk/vk_api.h` | Comment-only updates naming `host/oot_sprint.c`; no ABI, option or row change. |
| `gpu/vk/vk_internal.h` | An `import_stats_log` declaration. Internal to the renderer; no menu row. |
| `gpu/vk/vk_menu.cpp` | Adds the **Simulated turf** page and its `TAB_SIM_TURF` plumbing, `options_version=7`, and Sprint's default Toggle. The Simulated turf row is below this first unscrolled Graphics view; the top tab strip, Quality preset, Resolution and upscaling, Frame generation and latency, and Screen rows are unchanged. |
| `gpu/vk/vk_menu.h` | Sprint's comment and its default Toggle; no pictured Graphics default changes. |
| `gpu/vk/vk_texture.h` | Two test timing declarations. Internal to the renderer; no menu row. |

The pictured Graphics rows (`Quality preset`, `Aspect ratio`, `Output resolution`, `Custom output size`, `Profile`,
`Upscaler`, `Render resolution`, `Sharpness`, `Frame generation`, `Game frame rate cap`, `Fullscreen`), their values,
defaults, help and order, the top tab strip and the keyboard footer remain what the image and the guide show. The
version caption is the original capture's, as before. No fresh rendering, pixel comparison, game run, live GPU check or
new capture is claimed.

The review revision in `manifest.json` (`sha256:35be7b3d...`) is the SHA-256 of the sorted `<path> <sha256>` lines
(UTF-8, final newline) for all 131 entries in `reviewed_source.files`. `captured_source` and `fixture_files` stay
unchanged.
