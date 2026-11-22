# Standards

The rules a change meets before it merges. Reviewers check against this page. Above all of them is
[the rule with no exceptions](../../CONTRIBUTING.md#the-rule-with-no-exceptions): no game files, keys or anything made
from them leave your PC. How to do each thing is in [developing](developing.md).

## Hardware tiers

A change must not break a higher tier to help a lower one.

| Tier | Hardware | Means |
|---|---|---|
| 1 | NVIDIA RTX 20-50 and AMD RX 6000-9000 on Windows and Linux (NVIDIA driver, Mesa RADV); Steam Deck | Must keep working: tested before merge; a regression is S0 or S1 |
| 2 | AMD APUs and handhelds (ROG Ally, Legion Go, 780M/890M/8060S), Intel Arc | Fixed when reported, tested when someone with the hardware can |
| 3 | Cards without mesh shaders (GTX 10/16, RX 5000 and older) | Best effort through the fallback, never at a Tier 1 cost |

## Switches

A switch is an environment variable the game reads ([how switches work](reference/switches.md)).

- **Add one only for an A/B, or as a kill switch for a risky change.** Not for a setting we can decide ourselves: no
  player-facing knob for something the team can measure and settle.
- **Every switch has its row** in [`gpu/vk/switches.def`](../../gpu/vk/switches.def): `RX_` names in the renderer,
  `HOS_` in the host runtime, `LONGSHOT_` for the few others (`tests/test_switch_table.py`).
- **Its default is right for everyone**, and is chosen per platform (GPU vendor, driver, OS) from measurements. A
  default that depends on the PC is a `hw` rule in [device policy](reference/architecture.md#device-policy). Where a
  platform is unmeasured or unclear, ask the Test Farm (`@Beanstalk` on the pull request) to run it rather than guess.
- **It ships at that value from the branch's first push**, WIP or not: the Test Farm, testers and your own A/B runs
  test the branch as it is. If the measurements go against it, turn it off or drop the change before it leaves WIP, and
  say which. Never off just because it is new or unmeasured.
- **The pull request's Measurements** name it, the default it ships at, and the numbers that chose it.
- **Once settled, remove it**: delete its reads and turn its row into `GONE(...)`.

## Code

- **Match the file you edit.** There is no formatter: follow the indentation, comments and naming around you, and
  don't reformat unrelated code in the same change.
- **One topic per change.** A fix doesn't carry a refactor; two unrelated fixes are two pull requests. Find the cause
  (a log, a trace, a measurement) before the fix.
- **Game-specific code goes in new files** where possible. `nx2/`, `host/` and `gpu/` stay close to mkw-recomp so
  upstream fixes apply as a patch; each sync names its mkw-recomp base commit in the commit message. Treat local
  mkw-recomp and Eglantine checkouts as read-only references.
- **The GPU replay's rules** in [architecture](reference/architecture.md#rules-from-mkw) are not suggestions: nothing
  the replay reads comes from live guest memory, waits are honoured per queue, and texture and sampler registrations
  stay in submission order.
- **Two platforms.** Windows builds with llvm-mingw, Linux with clang/gcc. Keep host code portable, and paths UTF-8
  (no `CP_ACP`).
- **Python runs with `-I`.** Tests use the standard library only. The pipeline's dependencies (lz4, numpy, capstone,
  unicorn, zstandard) are the exception, not a reason to add more; so are the button prompts' (fontTools, Pillow,
  astc-encoder-py, texture2ddecoder: `nx2/prompt_requirements.txt`), for the button prompts (`nx2/prompt_assets.py`,
  `nx2/prompt_draw.py`) and Pillow for the game's icon (`nx2/make_icon.py`, `nx2/game_title.py`), at build time. Line
  endings are LF (`.gitattributes`; `*.bat` files are CRLF).
- **`third_party/` is not modified.** Its parts are upstream copies with their notices in `PROVENANCE.md`. A fix goes
  outside them, in a new folder with its own provenance note. For a deliberate refresh from upstream, say so in the
  part's note and run `python -I scripts/check_third_party.py --update <part>` in the same commit.
- **New code is GPL-compatible** ([Licensing](../../CONTRIBUTING.md#licensing)).
- **Recorded user-guide screenshots:** a change to recorded menu, Launcher, font or shared renderer inputs triggers the
  [user-guide screenshot source review](../user/images/guide/README.md#developer-maintenance-plan).

### Rules the checks enforce

| Rule | Check |
|---|---|
| **Tests read the source.** Many checks cut a function out of a source file by its text (`tests/srccut.py`'s `block` and `between`) and compile it with stand-ins. Renaming or reshaping a function can break a check far from your change: update the check in the same pull request rather than weaken it | the check that reads the file (`git grep -l <file> tests`) |
| **A `doc:` comment names a rule.** `// doc: docs/<file>.md#<anchor>` (`# doc:` in Python) above a function says where the rule it keeps is written. Read the rule before you simplify a capture, a wait or a pass split away; a renamed heading is fixed in the same change | `tests/test_doc_anchors.py` |
| **The renderer's interface is versioned.** A change to `MkwVkHost` or `MkwVkRenderer` in `gpu/vk/vk_api.h` bumps `MKWVK_API_VERSION` | ([architecture](reference/architecture.md#the-renderer)) |
| **A switch has a row** in `gpu/vk/switches.def`; a removed one is a `GONE(...)` row | `tests/test_switch_table.py` |
| **Generated files are generated.** The SPIR-V headers in `gpu/vk/shaders/` and `gpu/vk/host_shaders/` (`*_spv.h`) are compiled from the shader beside them, never edited: the CRT, FSR 3 and host headers with `python -I -B scripts/shaders.py crt\|fsr3\|host`, the others (`clear_*`, `dlss_*`, `fg_*` ...) with the glslang command in their test | `tests/test_crt_shaders.py`, `test_fsr3_shaders.py`, `test_host_shaders.py`, and each other shader's test (`tests/test_dlss_actor.py` ...) |
| **`third_party/` matches** the `tree-sha256` in each part's `PROVENANCE.md` | `python -I scripts/check_third_party.py` |
| **Paths are UTF-8** (no `CP_ACP`) | `tests/test_utf8_paths.py` |
| A variable the lift reads is in its key, or listed as not changing its output | `tests/test_lift_cache.py` |
| A switch the shader translator reads is in the program cache's stamp | `tests/test_program_stamp_knobs.py` |
| A new Graphics, Audio, Gameplay, Controls or NFC row has its line in the settings table (`docs/user/graphics-and-controls.md`), in the menu's order | `tests/test_settings_table.py` |
| Log lines carry the right level tag | `tests/test_log_levels.py` |
| **Thread-local state links under COFF ThinLTO.** An inline `thread_local` at namespace scope is `constinit` (no dynamic initializer); the recorder's `Arena`, which has one, is reached only through `rec::t_arena()` (`gpu/vk/vk_recorder.h`) | `tests/test_thinlto_tls.py` |
| A build entry script handles long Windows paths (`long_paths.install()`, `ENTRY_SCRIPTS`) | `tests/test_build_long_paths.py` |

## Fixes and checks

- **A fix comes with a game-free test that fails on `main`** for the bug's reason and passes with the fix
  ([fix-with-test skill](../../.claude/skills/fix-with-test/SKILL.md); [writing one](developing.md#write-a-check)).
  Where the bug can't be tested without the game, say so in the pull request and what stands in for it.
- **A fix behind a switch is tested both ways.**
- **All the checks pass** (`mise run test`), any failure that also fails on `main` named. A skip is not a pass.

## Performance and build changes

- **A performance change shows its measured gain against `main`** in the pull request, on the scenes and settings
  players use (the Quality preset Auto picks on that PC, at the device's own resolution), following the
  [measure skill](../../.claude/skills/measure/SKILL.md). **No measured gain, no merge.** Runs whose spreads overlap are
  "within the spread", not a gain.
- **Measure every area the change touches**, not only the one it is for:

  | The change touches | Measure |
  |---|---|
  | what the game does each frame | fps (average, 1% low) and frame time in the heavy areas and the target area |
  | the build (lifter, translator, compiler flags, shader or pack steps) | the game's build time and size, from a clean build |
  | loading, caches or startup | load or first-run time and cache size, cold and warm |
  | memory, images or buffers | RAM and VRAM |
  | the launcher, a tool or a script | that program's own time, size or output |

- **Changes to the lifter or the build flags must not lengthen players' game build much**: measure the build time
  ([how](developing.md#measure-the-build-time)). A flag change that alters the game's code is measured in the game too.
- **A cost the change has** (speed, latency, memory, a setup that worked before) is measured and stated in Risks, not
  left for the review to find.

## Pull requests

`main` takes reviewed pull requests only, squash-merged. **Group a track's steps into one pull request**: later steps
go on its branch, not into new pull requests. Only the title reaches `main`, so commits inside a branch can say
anything, though the commit-msg hook that `mise run setup` installs still asks git for a Conventional Commit.

### Titles

The title is a [Conventional Commit](https://www.conventionalcommits.org/): a type, an optional scope in parentheses
(`vk`, `linux`, `builder`, `menu`, `video`, `runtime` ...), a colon and a short description. It becomes the commit on
`main`, a line of the release notes, and decides the next version (`forge.py pr create` and the checks refuse anything
else).

| Type | For | Next version at least |
|---|---|---|
| `feat` | something new for players or modders | minor |
| `fix` | a bug fix | patch |
| `perf` | faster, or less memory | patch |
| `refactor` | no effect on behaviour | patch |
| `docs`, `test` | documentation, tests only | patch |
| `ci`, `build` | the workflows; the build scripts, AppImage, Builder package | patch |
| `chore` | anything else | patch |
| `revert` | undoing an earlier change, the reverted PR in the body | patch |

A `!` after the type or scope (`feat(builder)!: ...`), or a `BREAKING CHANGE:` footer, calls for the next major. `ci`
and `chore` are left out of the release notes ([releases](reference/releases.md#versions)).

### Tested in the game before it opens

**A change the game runs is tested in the game on your own PC before its pull request opens**, where it matters (its
area, your GPU, driver and OS): `forge.py pr create --tested-in-game "where it ran and what you saw"`, posted as the
first comment. `pr create` refuses a branch that touches `gpu/`, `host/`, `nx2/`, `assets/` or `third_party/` without it
(`GAME_PATHS` in `scripts/check_pr_description.py`); `--force` skips that. A change the game doesn't run (docs, tests, a
script) runs the way it is used.

### The description

Every pull request uses the [template](../../.forgejo/pull_request_template.md), which `forge.py pr create` starts
from. It has four sections and a checklist:

| Section | What goes in it |
|---|---|
| Summary | What was wrong or missing, what this changes, the result, in plain words; for a fix, how to see the bug on `main`; the files to read first; `Fixes #N` / `See #N` |
| Testing | **Tested:** where you ran it and what you saw. **Still to test:** what hasn't run, each item with where, what to expect and who runs it (`@Beanstalk ...` or by hand), or None |
| Measurements | A **Verdict:**, the setup and a table of `main` against this PR ([below](#measurements)), or one line saying why there is nothing to measure |
| Risks | What could break, how to turn it off, what a player notices after updating |
| Checklist | *Ready for review*: all ticked. *When it applies*: only the boxes that apply, ticked. *Ready to merge*: ticked during review |

The description is the pull request as it is now; comments are its history: post one for each push, measurement or
test run (`forge.py pr update` does both). `scripts/check_pr_description.py` checks the description (`--file body.md`
to run it yourself), and `pr ready` refuses until it passes.

Screenshots of the game are fine, game files never are: upload them with `pr comment --attach`
([Pictures in a comment](https://git.magicbean.dev/MagicBeanDev/Tooling/src/branch/main/docs/forge.md#pictures-in-a-comment)).

### Ready for review: out of WIP

A pull request opens as work in progress (a `WIP:` title) and leaves it with `pr ready` once the template's *Ready for
review* boxes, and the *When it applies* ones it keeps, are true:

1. **It is done**: one topic, nothing more planned, on current `main`, the title a Conventional Commit.
2. **You tested it on your own PC**: in the game for a change the game runs, as Testing's *Tested* says. What you
   can't run (hardware you don't have, a rare path) goes in *Still to test*.
3. **The checks pass** (`mise run test`), any failure that also fails on `main` named; a fix has its game-free test.
4. **It is measured** against `main`, or Measurements says why there is nothing to measure.
5. **The description is complete** for the head as it is now.

It goes back (`pr wip`) as soon as one stops holding. Runs on other hardware, a Test Farm result and an approval come
during review.

### Ready to merge

A maintainer squash-merges a ready pull request once its *Ready to merge* boxes are true, ticked by the author or by
them:

1. **Everything in *Still to test* has run**, its results in *Tested*.
2. **Tier 1 hardware it can affect is tested** ([hardware tiers](#hardware-tiers)).
3. **It is approved**, every comment answered.
4. **CI passes on the head**, on current `main`, measured again or a comment says why the numbers still hold.

A performance change with no measured gain doesn't merge ([performance and build changes](#performance-and-build-changes)).

### Measurements

Every pull request shows its effect with numbers or proof, `main` against this PR:

| Change | What counts as its measurement |
|---|---|
| `perf` | fps (average and 1% low), median and p99 frame time, and the number the change targets |
| `fix` | the bug reproduced on `main` and gone with this PR: a log line, a screenshot, or a test failing on `main` and passing here |
| `feat` | it working in the game, and no cost when off: fps or frame time with it on against off |
| `build`, tools | time or size on `main` and with this PR, or test output |
| `docs`, `test` only | test output on `main` and with this PR, or one line saying there is nothing to measure |

The section is a **Verdict:** line (did it help, by how much), a **Setup:** line (OS, GPU, driver; the scene and
settings; what was measured and how), and a table whose columns name what ran with its commit: `Before: main @ <hash>`
and `After: this PR @ <hash>` (`pr create` fills both in). A variant gets its own column
(`After, RX_VK_X=0: this PR @ 992a2420`); when the before side isn't `main`, a `Baseline:` line says what it is and
why. For the two sides:

- `main` at a named commit (the merge base);
- the same PC, save, scene, settings and build type;
- at least 2 runs each, interleaved, after a warm-up, the median reported;
- no overlays, ReShade or shader-cache tools;
- the log lines the numbers come from pasted below the table.

The numbers come from the FPS counter's **Breakdown** or **Dev** level, the `fps` lines of `longshot_log.txt`,
`RX_VK_DRAW_STATS`, or a test ([measure skill](../../.claude/skills/measure/SKILL.md)). An example, which compares
against another baseline and so says so:

```
**Verdict:** faster: +31% fps and 4.15 ms off the median frame time, the readback waits gone.

Baseline: nightly @ 55b976c0, the last nightly before #2602, which had merged already.
Setup: Windows 11, RTX 3080; a demanding outdoor area, Ultra preset, uncapped; the FPS counter's Breakdown, 120 s each after a warm-up, two interleaved pairs, the median

| | Before: nightly @ 55b976c0 | After: main @ 87991bc8 (with #2602) | Δ |
|---|---|---|---|
| fps (run 1 / run 2) | 59.7 / 60.8 | 78.4 / 79.3 | +31% |
| median frame time | 16.35 ms | 12.2 ms | -4.15 ms |
| readback waits | ~5.7 ms | <0.1 ms | -5.6 ms |
```

followed by the log lines themselves.
