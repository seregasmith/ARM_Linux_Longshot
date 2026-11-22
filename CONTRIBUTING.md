# Contributing to Longshot

Longshot is a static recompilation of a supported console game: the game's AArch64 code is lifted to C, the Horizon OS
and NVN2 are implemented natively, and the GPU work is replayed on Vulkan. No emulator is involved, and the game is
built on your PC from your own copy.

It lives on the team's Forgejo, [git.magicbean.dev/MagicBeanDev/Longshot](https://git.magicbean.dev/MagicBeanDev/Longshot).
This page has the rules that keep the project out of trouble and where to go next. The
[developer docs](docs/dev/README.md) have the rest.

## The rule with no exceptions

**No game files, keys, or anything made from them leave your PC.** The repository does not contain the game and never
will. That includes:

- the `.nup`, `romfs`, `exefs`, `.nso`, `.nca`, `.nsp`, `.xci` or `.nsz`/`.xcz` files, and any title keys;
- `work/` and everything in it (the extracted dump, `image.bin`, the lifted C, test builds), and the development folder
  (`python -I scripts/dev.py paths`: bases, stores, runs and screenshots);
- `longshot.exe`, `longshot_vk.dll` and other build output, development bases, packs and runtimes, run profiles
  (perf or Tracy captures), frame dumps, and the game folder's `cache/` (`longshot_programs.bin` and its `.idx`,
  `longshot_pipelines.bin`, `longshot_vk_pipelines.bin`). The play profiles in `assets/profiles/` (the hot list and the
  renderer's profile) are not on this list: they hold no game code or data and are committed
  ([their README](assets/profiles/README.md)). Nor are the routes in `assets/routes/`: input recordings made with
  `HOS_INPUT_ROUTE=1` that hold the input, Link's positions and the camera's angles as numbers, no guest words and no
  addresses, checked by `dev.py route check` ([their README](assets/routes/README.md)). Any other input recording
  (`HOS_INPUT_RECORD`, the Insert menu's Record input) holds the game's memory words and stays on your PC;
- saves (`user/`), `longshot_options.ini`, `longshot_env.txt` and `language.txt`. The one exception: scrubbed saves
  under `assets/saves/` ([the rules](assets/saves/README.md)): one slot each (`progress.sav` and `caption.sav` only),
  its picture made grey and its date reset by `scripts/longshot_saves.py scrub`, passing `check-repo-saves`, made fresh
  for the farm (never someone's own adventure), and added through a pull request two maintainers approve.
  `account.sav`, `option.sav`, `longshot_options.ini`, `longshot_env.txt` and the rest of `user/` stay banned.

Don't ask in an issue, a pull request or the chat where to get the game, how to decrypt it, or for someone else's
files. A screenshot, a video and `longshot_log.txt` are fine and often necessary: the log holds no game data.
`.gitignore` covers most of this, but check `git status` (or `jj status`) before you commit. If you aren't sure whether
a file is game data, leave it out.

## The other non-negotiables

- **`main` takes reviewed pull requests only**, from a branch on your fork, one topic each.
- **A change the game runs is tested in the game on your PC before its pull request opens**, and a fix comes with a
  game-free test that fails on `main`.
- **A performance change shows its measured gain against `main`.** No measured gain, no merge.
- **Vendor SDKs are never committed, bundled or uploaded**: NVIDIA's DLSS SDK, AMD's FSR runtime, Intel's `libxess.dll`.
- **Only code you can license under the GPL** ([Licensing](#licensing)).

The full list, and how each is checked, is in [standards](docs/dev/standards.md).

## Get started

1. **Set up** a PC, the team's Tooling repository and sign-in, and a first build from your own dump:
   [setup](docs/dev/setup.md).
2. **Work** on a change: quick builds against a base, the checks, running and measuring it in the game, profiling,
   and the steps from claim to merge: [developing](docs/dev/developing.md).
3. **Meet the standards** before you ask for review: [standards](docs/dev/standards.md).

Then claim something in the team room before you start on it ([claiming](docs/dev/developing.md#from-claim-to-merge)).

## Ways to help

| | What |
|---|---|
| **Test on other hardware** | NVIDIA cards are the most tested. Reports from AMD and Intel cards, other drivers, other distributions and SteamOS help even when everything works. Lend your PC to the [Test Farm](https://beanstalk.magicbean.dev/how): it tests approved pull requests on your hardware by itself. |
| **Play further** | The tested area is the start of the game. Reaching new areas, cutscenes, menus and items finds problems nobody has seen. |
| **Report what is broken** | Use the [issue forms](https://git.magicbean.dev/MagicBeanDev/Longshot/issues/new/choose): game problem, install or build problem, save problem or idea. Include the version, your hardware, where you were and `longshot_log.txt`. |
| **Send a fix** | Branch, fix, test, open a pull request from your fork ([from claim to merge](docs/dev/developing.md#from-claim-to-merge)). Small, focused changes are the easiest to review. |
| **Improve the docs** | The README, the player docs and the developer docs are part of the project. |

## Before you open an issue or a pull request

- Try current `main` first: it has every fix so far.
- Search the [open issues](https://git.magicbean.dev/MagicBeanDev/Longshot/issues). If yours is there, add your
  details instead of opening another.
- English, please. One topic per issue or pull request.
- Report the version you actually ran: the first line of `longshot_log.txt`.
- For a graphics or performance problem, test without ReShade, shader-cache tools, overlays and frame-generation
  injectors: they change what Longshot does.

## CI

CI is moving from Forgejo Actions to CircleCI, and both run until the cutover: [CI](docs/dev/reference/ci.md) says what
runs when, on which runners. Run `mise run lint` and `mise run test` before you push, and put the results in the pull
request.

- **CircleCI:** sign up at https://circleci.com/signup and ask in Discord to be added to the MagicBeanDev org, so you can see runs, read logs and rerun jobs.
- **CircleCI CLI:** `mise install` installs the pinned version, or see the [official install docs](https://circleci.com/docs/guides/toolkit/circleci-cli/). Run `circleci auth login` once.
- **CircleCI MCP server**, so an AI agent can read failures and logs and rerun jobs. Claude Code: `claude mcp add --transport http circleci-mcp-server https://mcp.circleci.com/v1/mcp`, then `/mcp` and Authenticate. Cursor: search "circleci" in the MCP marketplace.
- **CircleCI docs MCP**, so an agent can look things up in CircleCI's docs (no account needed): `claude mcp add --transport http circleci-docs https://circleci.mcp.kapa.ai`.
- **Chunk CLI** (optional) runs this repo's checks on a remote CircleCI sidecar, handy for an agent or a machine without the toolchain. Install with `brew install CircleCI-Public/circleci/chunk` (Windows: see its [README](https://github.com/CircleCI-Public/chunk-cli)), run `chunk auth login`, then `chunk sidecar create`, `chunk sidecar sync` and `chunk validate`. The commands it runs are in `.chunk/config.json`.

The software-Vulkan fixtures run only by hand: on Forgejo, *Actions* > *Authored Vulkan fixtures* > *Run workflow*, or
locally ([run the checks](docs/dev/developing.md#run-the-checks)).

## Licensing

Longshot is free software under the [GNU General Public License, version 3 or later](LICENSE). By submitting a change
you license it under those terms, so only send code you can license that way: nothing from a project with an
incompatible license, and no decompiled game code or anything generated from the dump.

The checklist's license box is the author's own check of the diff against that paragraph: where each new file and
copied passage comes from, and that none of it is the game's code or made from the dump. How the game or the console
behaves (a function's name and arguments, a result code, the order of calls) is a fact and not code, and the host's
replacements for the console's functions are written from such facts; describe them in your own words and cite a
public source when there is one.

The third-party parts (Eden, suyu, Sirit, the Vulkan Memory Allocator, SDL, Opus, Dear ImGui, zstd, the Libertinus
fonts, zbic, the a64-kit tools, AMD's FidelityFX headers and SDK, the CRT shaders, Kenney's input prompts and Tracy,
among others) keep their own notices: preserve them, and don't relicense their code. NVIDIA's DLSS
SDK is not one of them: it is never in the repository ([upscaler SDKs](docs/dev/setup.md#upscaler-sdks-optional)). Nor
is Intel's `libxess.dll` (Intel's license): only XeSS's MIT API headers are (`third_party/xess`), and the player puts the
DLL beside the game ([optional upscalers](docs/user/advanced.md#optional-upscalers)).

## Where to talk

- **The team room**, `#agent-updates:magicbean.dev` on Matrix: claims, progress, pull requests and blockers, from people
  and agents alike. Post and read it with the [team-updates skill](https://git.magicbean.dev/MagicBeanDev/Tooling/src/branch/main/skills/team-updates/SKILL.md) or Tooling's `team_chat.py`.
- **Discord:** https://discord.gg/magicbean, for questions.
- **Agents** read [AGENTS.md](AGENTS.md) on top of this page.
- **Issues:** [the forms on git.magicbean.dev](https://git.magicbean.dev/MagicBeanDev/Longshot/issues/new/choose).

Thanks for testing, reporting and sending fixes. Every report from a different GPU and every area played further than
before moves the port forward.
