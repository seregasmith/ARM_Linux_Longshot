# Graphics guide image: source review against 81e8a6ba

Retain `settings-graphics.png` after reviewing the two UI-source inputs that changed on main `753fb5af` since the previous
review, and the one they add. The PNG, its dimensions and SHA-256, its caption, capture date, version, `captured_source`,
`fixture_files` and capture conditions are unchanged.

`python -I -B scripts/check_guide_screenshots.py --review-sources` flagged the first two:

| Source | Change and retention decision |
|---|---|
| `gpu/vk/device_policy.h` | The RDNA 3 and RDNA 4 tests move to `gpu_arch.h`, with `arch()`, `rdna1()`, `rdna2()` and `amd_win()` beside them (#2227); the copy rule `bc_copy_per_layer` for AMD's Windows driver and `bc_small_runs` on it too (#2228). Read by `vk_coopmat.cpp`, `vk_texture.cpp` and `vk_main.cpp`; no file of the menu names any of them. No menu row. |
| `gpu/vk/vk_api.h` | API version 45: `vulkan_loader_proc`, a callback at the end of `MkwVkHost` (#2275). No file of the menu calls it. No option or row change. |
| `gpu/vk/gpu_arch.h` (new in the list) | The GPU architecture table `device_policy.h` now includes (#2227). No menu row. |

The pictured Graphics rows (`Quality preset`, `Aspect ratio`, `Output resolution`, `Custom output size`, `Profile`,
`Upscaler`, `Render resolution`, `Sharpness`, `Frame generation`, `Game frame rate cap`, `Fullscreen`), their values,
defaults, help and order, the top tab strip and the keyboard footer come from sources whose hashes did not change. The
version caption is the original capture's, as before. No fresh rendering, pixel comparison, game run, live GPU check or
new capture is claimed.

The review revision in `manifest.json` (`sha256:81e8a6ba...`) is the SHA-256 of the sorted `<path> <sha256>` lines
(UTF-8, final newline) for all 132 entries in `reviewed_source.files`. `captured_source` and `fixture_files` stay
unchanged.
