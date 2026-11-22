## Summary

<!-- In plain words a player could follow: what was wrong or missing, what this changes, and the result. For a fix,
     how to see the bug on main (where, the steps, what happens). Then the files or functions a reviewer should read
     first. "Fixes #N" closes an issue on merge, "See #N" links one. -->

<what was wrong or missing, what this changes, the result>

## Testing

<!-- Tested: where you ran it (OS, GPU, driver), what you did and what you saw. A change the game runs ran in the
     game on your own PC; for a fix, the bug's steps no longer show it. Then the checks.
     Still to test: what this could affect that hasn't run, one item each: where (OS, GPU, or a Test Farm tag), what
     to do, what to expect, and who runs it: the Test Farm ("@Beanstalk <what to test>" as a comment) or a person,
     "by hand". "None" when Tested covers it. -->

**Tested:** <where, what you did, what you saw>

**Still to test:**
- <where>: <what to do>, expecting <the result> (<@Beanstalk ..., or by hand>)

## Measurements

<!-- main against this PR on the same PC, scene and settings, at least two interleaved runs each, the median
     (docs/dev/standards.md, "Measurements"). A perf change must show its gain. A switch this adds or changes: the
     default it ships at, and the numbers that chose it. Nothing to measure (docs, a test): replace the whole section
     with one line saying why. -->

**Verdict:** <did it help, by how much>

Setup: <OS, GPU, driver; the scene and settings; what was measured, how>

| | Before: main @ <commit> | After: this PR @ <commit> |
|---|---|---|
| <what was measured> | | |

## Risks

<!-- What could break, and where; how to turn it off (a switch) or that a revert is enough; what a player notices
     after updating (saves, longshot_options.ini, the caches), or "nothing". Any cost (speed, memory, a setup that
     worked before) is measured above and named here. -->

<what could break, how to turn it off, what changes after updating>

## Checklist

<!-- Ready for review: every box ticked before it leaves WIP (forge.py pr ready checks them). When it applies: delete
     the boxes that don't, tick the rest. Ready to merge: ticked by you or the maintainer who merges. -->

### Ready for review

- [ ] One topic, on current `main`; the title is a Conventional Commit.
- [ ] Tested on my own PC, in the game for a change the game runs, as Testing says.
- [ ] `mise run test` passes (any failure that also fails on `main` is named).
- [ ] Measured against `main`, or Measurements says why there is nothing to measure.
- [ ] No game files, build output or hand-edited generated files; `third_party/` untouched; nothing GPL-incompatible.

### When it applies

- [ ] A fix: a game-free test that fails on `main` and passes here.
- [ ] A switch added or changed: it ships at the default the measurements chose, with its row in `gpu/vk/switches.def`.
- [ ] A check under `tests/` that reads changed source is updated, not weakened.
- [ ] `MKWVK_API_VERSION` bumped (`MkwVkHost` or `MkwVkRenderer` in `gpu/vk/vk_api.h` changed).
- [ ] The SPIR-V headers regenerated (a shader changed).
- [ ] Host code builds on Windows (llvm-mingw) and Linux, with UTF-8 paths.
- [ ] Screenshots of the same spot on `main` and here, for a change that shows on screen.
- [ ] Docs updated.
- [ ] AI assistance recorded with `Assisted-by:` / `Co-Authored-By:` trailers.

### Ready to merge

- [ ] Everything in Still to test has run, its results in Tested.
- [ ] Tier 1 hardware it can affect is tested (docs/dev/standards.md, "Hardware tiers").
- [ ] Approved in review, every comment answered.
- [ ] CI passes on the head; on current `main`, measured again or a comment says why the numbers still hold.
