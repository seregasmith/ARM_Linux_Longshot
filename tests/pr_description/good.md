<!-- A sample of a good pull request description, for tests/test_check_pr_description.py and Tooling's
     tests/test_forge_pr.py. The PR and its numbers are made up; the shape is .forgejo/pull_request_template.md's. -->
## Summary

At 4K the pause menu's text ran off the right edge, so the last option of each row could not be read: load any save
at 3840x2160 and press Start. The menu's font now grows with the screen's height, and `RX_VK_MENU_FONT_SCALE` sets it
by hand for testing. Read first: `menu_font_scale()` in `gpu/vk/vk_menu.cpp`. Fixes #42.

## Testing

**Tested:** CachyOS, RX 6700 XT, Mesa 25.2 RADV: the pause, Insert and save menus at 3840x2160 and 1920x1080, every
row read to its end; `mise run test`, with the new `tests/test_menu_font_scale.py` failing on main and passing here.

**Still to test:**
- Windows, NVIDIA, any recent driver: open the pause menu at 3840x2160, expecting every row inside the screen (by
  hand: read each row's end)
- Steam Deck (steamdeck): open the pause menu at 1280x800, expecting the text no smaller than on main (Test Farm:
  @Beanstalk steamdeck, the pause menu's text size against main)

## Measurements

**Verdict:** fixed: the widest row is 302 px narrower at 4K and fits; the menu's frame time is unchanged.
`RX_VK_MENU_FONT_SCALE` ships unset (the scale follows the height): it is for testing only.

Setup: CachyOS, RX 6700 XT, Mesa 25.2; the pause menu in Kokiri Forest at 3840x2160, no upscaler or frame generation;
the menu's frame time from the FPS counter's Dev level, 60 s each, two interleaved runs, the median.

| | Before: main @ 7afe6ae1 | After: this PR @ 992a2420 | Δ |
|---|---|---|---|
| widest row at 4K (px) | 3912 (cut off) | 3610 | -302 |
| menu frame time (run 1 / run 2) | 0.41 / 0.42 ms | 0.41 / 0.41 ms | 0 |
| test_menu_font_scale.py | 0 of 4 pass | 4 of 4 pass | +4 |

```
[menu] font scale 1.00 (fixed), widest row 3912 px > 3840     main @ 7afe6ae1
[menu] font scale 2.00 (height 2160), widest row 3610 px       this PR @ 992a2420
```

## Risks

On a small screen the menu could come out too large if the height were read wrong; `RX_VK_MENU_FONT_SCALE=1` brings
back the old size, and a revert is enough. Nothing changes for players after updating: saves, `longshot_options.ini`
and the caches are untouched.

## Checklist

### Ready for review

- [x] One topic, on current `main`; the title is a Conventional Commit.
- [x] Tested on my own PC, in the game for a change the game runs, as Testing says.
- [x] `mise run test` passes (any failure that also fails on `main` is named).
- [x] Measured against `main`, or Measurements says why there is nothing to measure.
- [x] No game files, build output or hand-edited generated files; `third_party/` untouched; nothing GPL-incompatible.

### When it applies

- [x] A fix: a game-free test that fails on `main` and passes here.
- [x] A switch added or changed: it ships at the default the measurements chose, with its row in `gpu/vk/switches.def`.
- [x] A check under `tests/` that reads changed source is updated, not weakened.
- [x] Host code builds on Windows (llvm-mingw) and Linux, with UTF-8 paths.
- [x] Screenshots of the same spot on `main` and here, for a change that shows on screen.
- [x] Docs updated.

### Ready to merge

- [ ] Everything in Still to test has run, its results in Tested.
- [ ] Tier 1 hardware it can affect is tested (docs/dev/standards.md, "Hardware tiers").
- [ ] Approved in review, every comment answered.
- [ ] CI passes on the head; on current `main`, measured again or a comment says why the numbers still hold.
