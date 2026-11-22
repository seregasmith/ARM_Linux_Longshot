# Releases

How Longshot is versioned, released and delivered to players. A release is made by pushing a tag: CI checks the
number, builds both builders, signs and publishes. There are no release pull requests, no release branches and no
changelog file.

The Longshot Launcher is released the same way from its own repository,
[MagicBeanDev/LongshotLauncher](https://git.magicbean.dev/MagicBeanDev/LongshotLauncher), with its own numbers (from
`v1.0.0`) and the same `release.py`. This page covers Longshot.

`release.py` is Tooling's ([scripts/release.py](https://git.magicbean.dev/MagicBeanDev/Tooling/src/branch/main/scripts/release.py)):
run it from this repository's root as `python -I ../Tooling/scripts/release.py`. CI clones Tooling at the commit
[`.tooling-rev`](../../../.tooling-rev) names, `main`'s for a tag's checks: a change to the release is a Tooling pull
request, then one here that moves `.tooling-rev` to its commit.

## What a release is

One Forgejo release of MagicBeanDev/Longshot: a tag, a page with the notes, and these files.

| File | What it is |
|---|---|
| `LongshotBuilder-<v>-windows-x64.zip` | the Windows builder: the setup as `LongshotBuilder.exe`, the builder and Longshot's sources in `LongshotBuilder\`, the prebuilt third-party objects, `tools.lock`, `VERSION`, `recomplauncher.json` |
| `Longshot-<v>-x86_64.AppImage` | the Linux builder: the builder, its Python, Longshot's sources, the prebuilt third-party objects, `tools.lock`, `recomplauncher.json` |
| `tools.lock` | the build tools the builders download ([tools.lock](#toolslock)) |
| `SHA256SUMS`, `SHA256SUMS.sig` | each file's SHA-256 (`sha256sum` format), and its signature by the release key |

Neither builder carries the game, its build tools or a Vulkan back end: the player's own build compiles the back end
from the sources for their PC. A release's files never change: a fix is the next version. Never put game files, keys,
builds or anything made from a dump in a release ([CONTRIBUTING.md](../../../CONTRIBUTING.md#the-rule-with-no-exceptions)).

Players install with the Longshot Launcher, which follows a channel, checks signatures and updates both the builders
and itself. Longshot's release pages therefore open with links to the launcher of the same channel and tell players
not to download the files below.

## Versions

Longshot follows [Semantic Versioning](https://semver.org/) from `v3.0.0` (earlier numbers are Hookshot's). One
version covers the game, the Vulkan back end, the lift pipeline and the builders: they are built from one commit.

| Channel | Tag | Made by |
|---|---|---|
| `stable` | `vX.Y.Z` | a maintainer's tag on a tested nightly's commit of `main` |
| `nightly` | `nightly-<count>-<8 hex>` | every push to `main` that brings something for the notes |

A nightly's count is the number of commits since the fork (`e2a4d45`, "Rename the project to Longshot"). A build from a
checkout is named by `git describe --tags --always --dirty` (or `unknown`), an AppImage built from a checkout by
`git describe --tags --long` (or `dev`). Neither is on a channel, and neither is ever offered as an update.

### The number a tag must have

Pull requests are squash-merged with Conventional Commit titles, so the commits on `main` say what changed.
`release.py check-tag` (CI's first step) refuses a tag lower than they call for:

| Since the last stable, a title with | The next version is at least |
|---|---|
| `!` after the type or scope, or a `BREAKING CHANGE:` footer | the next major: `v4.0.0` after `v3.2.1` |
| `feat` | the next minor: `v3.3.0` |
| `fix`, `perf` or any other type | the next patch: `v3.2.2` |

It also refuses a tag that isn't `vX.Y.Z`, isn't on `main`, or isn't above every release there is. An old release
candidate tag (`vX.Y.Z-rc.N`) is on no channel.

### What the version protects

The public surface is the command line of `nx2/make_game.py` and the builders, the `RX_` and `HOS_` switches,
`longshot_options.ini`, the files a build writes into a game folder (`longshot_env.txt`, `language.txt`,
`longshot_version.txt`), saves, the release files' names, `SHA256SUMS`, `tools.lock`, `recomplauncher.json` and the
format of `longshot_log.txt`. C sources and internal headers are not an API.

### Where the version shows

| Place | Comes from |
|---|---|
| `longshot_log.txt`'s first line and the Insert menu | `nx2/version.py`: `LONGSHOT_VERSION`, then a `VERSION` or `version.txt` file, then `git describe --tags --always --dirty --match "v[0-9]*"`, else `unknown` |
| the builders and their files | the release workflow's version name (`VERSION` in the zip and the AppImage) |
| `longshot_version.txt` in a game folder | the `VERSION` file, written by `nx2/make_game.py` |

`nx2/version.py` also appends a fingerprint of the sources, `(source 1a2b3c4d)`, which tells two builds of one version
apart.

## The release workflow

`.forgejo/workflows/release.yml` runs on every push to `main` and every `v*` tag:

| Job | What it does | Run by |
|---|---|---|
| The version | checks `tools.lock` has every tool each builder needs, then names the version: a tag is checked; a push gets the nightly name, after waiting for more merges and only when it brings something for the notes; a tag of a commit released already names that release | `tools.py check --require`, `release.py check-tag`, `released-as`, `nightly-age`, `nightly-news`, `nightly-name` |
| Linux builder | builds the AppImage in Ubuntu 22.04 (glibc 2.35), compiles the Vulkan back end to check it (with and without AMD FSR 4), and starts a player's build from the image | `build-linux.yml`: `scripts/linux/deps.sh`, `scripts/linux/appimage.sh`, `scripts/linux/check_player_build.sh` |
| Windows builder | cross-builds the setup, the builder (`dotnet publish -r win-x64`) and the zip on Linux with llvm-mingw's Linux build, checks them as Windows files, starts them under Wine, checks the `cpm` package's dav1d under Wine, and compiles the Vulkan back end to check it | `build-windows.yml`: `scripts/builder_package.py`, `scripts/check_windows_files.py` |
| Sign and publish | writes and signs `SHA256SUMS`, checks the signature with the key the launcher trusts, writes the notes, uploads everything to a draft and publishes it; a nightly also deletes the nightlies past the newest 5 | `release.py sums`, `sign`, `verify`, `notes`, `publish`, `prune` |

- **Drafts make a release appear at once.** Until the last step no launcher sees it; a failed run leaves a draft, which
  a re-run of the same tag reuses.
- **A commit released already is rebuilt but not rechecked.** Every file carries its version, so a tag always builds
  again. When the commit's nightly is still published (the newest 5 are kept), `release.py released-as` checks its
  signed `SHA256SUMS`, and the build checks that ship no file are skipped.
- **Every job runs on Linux.** The Windows files are cross-built; what only Windows can check is a
  [Windows tester's](#windows-tester).

## Making a release

You need maintainer rights to push tags, the remote `forge` (set up by Tooling's `forge.py login`), and the full
history (`git fetch --unshallow --tags` in a shallow clone). Agents: the
[release skill](https://git.magicbean.dev/MagicBeanDev/Tooling/src/branch/main/skills/release/SKILL.md)
(`/magicbean:release stable`) does these steps and writes the page's "What's new".

```sh
export GIT_CLIFF=$(python -I ../Tooling/scripts/release.py git-cliff "$(python -I scripts/dev.py paths tools)/git-cliff")   # the pinned git-cliff
python -I ../Tooling/scripts/release.py next       # the number the commits call for, the next stable's tag, its notes
python -I ../Tooling/scripts/release.py channels   # what each channel offers now
```

### A nightly

Nothing to do. A merge to `main` makes one, about 5 minutes after the last of several merges in a row (at least once
an hour while merges keep coming). A push whose commits since the last nightly are only `ci:` or `chore:` makes none
(`release.py nightly-news`). If a nightly fails, read the failed step on the run (*Actions* > *Release*): fix code in a
pull request, or *Re-run* for a runner or network failure. Players keep the last good nightly meanwhile.

### A stable

A stable is a tag on a commit of `main` that testers played: normally a nightly's commit (the nightly channel is where
the next version is tried). There are no release candidates.

1. **Before a minor's stable**, if the renderer changed much, record the renderer's play profile again and merge it
   first, so the nightly testers play has it ([recording it](../developing.md#record-the-renderers-play-profile)). An
   old profile still builds.
2. **Preview** on an up-to-date `main`: `release.py next`. Read the notes: a wrong title is a wrong line, and the commit
   on `main` keeps the title it was merged with.
3. **Get it tested:** testers on the nightly channel, and a [Windows tester](#windows-tester) on the nightly to be
   tagged.
4. **Tag** the tested nightly's commit (`nightly-<count>-<commit>` names it; jj doesn't push tags, so tag with git in
   the colocated clone):

   ```sh
   git tag -a v3.1.0 -m "Longshot v3.1.0" nightly-412-1a2b3c4d^{}
   python -I ../Tooling/scripts/release.py check-tag v3.1.0   # optional: the same check CI runs
   git push forge v3.1.0
   ```

   Or in the web UI: *Releases* > *New release*, type the tag, target the commit, **Create tag** only. Never *Publish
   release* or *Save draft*: CI makes the release, and one made by hand has no files.
5. **Check it:** the run is green; the page has every file, `SHA256SUMS` and `SHA256SUMS.sig`; `release.py channels`
   shows the tag on `stable`; downloaded files pass `release.py verify SHA256SUMS --files <folder>`; a launcher on the
   stable channel offers it.

CI rebuilds it from that commit and publishes it on `stable`. Its notes cover everything since the previous stable.
Tagging a later commit works, but then it isn't what testers tried.

### A hotfix

Fix it on `main` in a `fix:` pull request. `release.py next` gives the patch (or the minor if a `feat` merged since).
Let its nightly be played quickly (on Windows too), then tag the stable on its commit; in an emergency, tag it
without waiting. Releases come only from `main`: revert anything on `main` not ready to ship, in a pull request, first.

### Pulling a bad release

1. On its release page, **Edit** and tick *This is a draft*, or **Delete** it. Its channel falls back to the version
   before at once.
2. Launchers that installed it keep it (they never downgrade by themselves). Tell players in an issue and the chat to
   build the previous version, or for the launcher, run `--rollback`.
3. Release the fix as the next number. Never move or reuse a tag: launchers that installed it wouldn't see the fixed
   files as an update.

### When CI refuses a tag

The *version* job says why, and nothing was built. Delete the tag (`git push forge :refs/tags/<tag>` and
`git tag -d <tag>`, or *Tags* in the web UI), then:

| The error says | Do |
|---|---|
| `is not vX.Y.Z` | a typo, or a number below `v3`: tag the right name |
| `is not on main` | tag a commit of `forge/main` |
| `is not above every release there is` | the number is taken or lower: `release.py next` gives the next one |
| `is lower than the commits since vX call for: at least vY` | a `feat` or breaking change came in: tag `vY` |
| `no tag ... here` | the tag isn't in this clone: `git fetch --tags`, or make the tag before checking it |

### Windows tester

CI only shows that the Windows files load. Before each stable, someone on Windows 10 or 11 tests the nightly to be
tagged:

- **Build and play:** with the launcher, build that nightly from their own copy and play to the title screen. Then unzip the
  builder and run `LongshotBuilder.exe --cli --prepare --game <your copy>` (as RecompLauncher does). Started by a
  double-click, the setup and `LongshotBuilder\LongshotBuilder.exe` each show only a message pointing to the launcher.
- **Note what Windows says:** nothing is signed, so record what SmartScreen and the antivirus do.
- **Run the Windows-only checks:** `python -I -B tests/run_all.py --strict` on Windows' Python (the checks Linux skips
  name why in their `RUN_ALL_SKIP`), and from a Visual Studio 2022 developer prompt
  `cl /nologo /std:c++17 /W4 /WX /EHsc gpu\vk\tests\present_rect_test.cpp && present_rect_test.exe` and
  `python -I -B tests/test_invalidate_vcore.py --cxx cl`.

A failure is an issue and blocks the stable.

## Release notes

git-cliff writes them at release time from the commit titles since the previous tag, with [cliff.toml](../../../cliff.toml)
(`release.py notes`). Headings: Features, Fixes, Performance, Reverted, Documentation, Code, Build, Tests; `ci` and
`chore` are left out. A stable's notes cover everything since the previous stable. A nightly after the first lists what
is new since the nightly before, then *Earlier changes*, everything since the last stable. The notes go on the release
page only.

## Signing

CI signs `SHA256SUMS` with the release key (ECDSA P-256, the organization secret `RELEASE_SIGNING_KEY_SEALED`, opened
only on the server's runner: [CI](ci.md#secrets-only-our-runners-can-open)), then checks
the signature against the public keys the launcher trusts (Longshot's [release_keys.json](../../../scripts/release_keys.json),
the launcher's `Core/ReleaseTrust.cs`): a key the launcher doesn't trust stops the release in CI. The launcher checks
the signature, then each file it downloads against `SHA256SUMS`, then each tool against `tools.lock`.
`LONGSHOT_LAUNCHER_TRUST=<public .pem>` makes a launcher trust one more key, for trying a test release.

Uploads go through `RELEASE_UPLOAD_API` (an organization variable, passed to `release.py` as `FORGEJO_UPLOAD_API`): the
API without Cloudflare in front, which cuts uploads at 100 MB. Releases are made with the job's own token; no other
secret is needed.

### Rotating the key

When it may have leaked, or on a schedule:

1. `python -I ../Tooling/scripts/release.py keygen <safe folder>` makes `release.pem` and `release.pub.pem` and prints the lines.
2. **The launcher first:** add the new key beside the old one in `Core/ReleaseTrust.cs` and `scripts/release_keys.json`,
   and release the launcher **signed with the old key**.
3. **Then switch,** once most players have that launcher: replace `RELEASE_SIGNING_KEY_SEALED` with the new
   `release.pem`, sealed (`scripts/ci_secret.sh seal --key seal.key < release.pem`),
   remove the old key from both files, and release the launcher with the new key.

Older launchers then refuse new releases and the player downloads the launcher by hand. If the old key leaked, do
steps 2 and 3 at once and tell players.

## tools.lock

The builders' build tools aren't in the release. [tools.lock](../../../tools.lock) names each one: name, version, system
(`windows-x64` or `linux-x64`), SHA-256, size, where it goes in the install, the folder in the archive, and its URLs.

| System | Tools |
|---|---|
| Windows | `llvm-mingw` (clang, lld, the LLVM tools), `python` (with the prompt packages), `cpm` (FFmpeg, Vulkan and SPIR-V headers, VMA, dav1d), `third-party` (prebuilt `third_party` files) |
| Linux | `emsdk` (LLVM's clang and lld), `sysroot` (glibc 2.35), `ffmpeg` (with dav1d), `sdl2`, `cpm` (Vulkan, SPIR-V and VMA headers) |

The Windows setup, the AppImage's first start and the launcher download each tool from the first URL that answers,
check its size and SHA-256, and unpack it; a tool another installed version has is reused. The first URLs are
Longshot's copy, a file of an organization generic package `<name>-<system>` (from `git-cdn.magicbean.dev`, then
`git.magicbean.dev`); upstream is the fallback. A package version's files never change and are never pruned.
`release.yml` refuses a `tools.lock` that lacks a tool or whose package files are missing (`tools.py check --require`).

### Bumping a tool

A bump is a pull request that changes the tool's line (`build(windows): llvm-mingw 20261201`). Uploads take your own
token as `RELEASE_TOKEN`; one over 100 MB needs `FORGEJO_UPLOAD_API` set to the address without Cloudflare.

- **An upstream archive** (llvm-mingw): edit the line's version, `sha256`, size, prefix and upstream URL, then
  `tools.py mirror <name>` uploads the copy and makes it the first URL. For llvm-mingw, also update
  `LINUX_LLVM_MINGW` in `scripts/builder_package.py` to the same release's Linux build, which CI cross-builds with
  (`tests/test_windows_on_linux.py` checks they match).
- **An archive Longshot makes** (Windows `python`, `cpm`, `third-party`): `tools.py assemble python --requirements
  nx2/prompt_requirements.txt --out <zip>`, `assemble cpm --add <CPM folder> --out <zip>` or `assemble third-party --from
  <archive with tools/third_party> --out <zip>`, then `tools.py add <name> <version> <zip> --dest <folder> --prefix <folder>`. Check nothing made from the game is in it.
- **A Linux tool:** change its version and `sha256` pin in `scripts/linux/deps.sh`, run `scripts/linux/docker.sh tools`
  (it builds and packs each into `build-linux/docker-deps/tools/` and writes the `tools.py add` lines to
  `tools/add.sh`), run the lines for the tools you changed, then `scripts/linux/docker.sh` to build the AppImage with
  them.

Finish with `tools.py check --require`. The next release uses the new line; installed versions keep theirs. Play
profiles are not tools: they are committed in `assets/profiles/`.

RenHDrum isn't a tool either. The release builds `renhdrum.dll` and `renhdrum.so` from `third_party/renhdrum`
(`scripts/renhdrum.py build` in `build-windows.yml` and `build-linux.yml`, with a pinned Rust, its build crate by its
`Cargo.lock` SHA-256 and libopus from `third_party/opus`) and ships them in the
builders, where a player's build finds them. A newer RenHDrum: its `PROVENANCE.md` says how. A newer Rust:
`RUST_VERSION` and the parts' SHA-256 in `scripts/renhdrum.py`.

## Update channels

The launcher reads the channels from the repository's list of published releases, by the same rules as
`release.py channels`:

| Channel | Offers |
|---|---|
| `stable` | the newest published `vX.Y.Z` |
| `nightly` | the newest published `nightly-*`, by its count |

A draft is on no channel, and so is an old `vX.Y.Z-rc.N`. Each build in the launcher follows its own channel, and the
launcher updates itself from a channel of its own (*Settings*). It never moves a build or itself to a lower version;
going down is the player's choice. The builders and the game don't look for updates themselves. How the launcher installs builders and updates itself is
documented in [LongshotLauncher](https://git.magicbean.dev/MagicBeanDev/LongshotLauncher).

**Download links.** `https://longshot.magicbean.dev/download` (served by `website/worker/index.ts`) points to the newest
launcher: `/download/launcher/<os>` for the best channel there is (stable, else nightly),
`/download/launcher/<channel>` for a channel's release page, `/download/launcher/<channel>/<os>` for its file.
`<channel>` is `stable` or `nightly` (an old `rc` link goes to `stable`); `<os>` is `windows`, `windows-portable` or
`linux`. There are no links to the builder files: only the launcher downloads them.

**RecompLauncher** can also run both builders through their `recomplauncher.json`
([recomplauncher.json](../../../recomplauncher.json), `tests/test_recomplauncher_json.py`): `--cli --prepare
--game=<copy> --out <the game folder> --work <the install folder>/recompdata/cache/work`, then `--play`. The work folder
is in the install folder, so it moves and uninstalls with the game, and RecompLauncher's backups leave its `cache` out.
The build is finished when the folder has its `image.bin` (*prepare*'s `done`). With *prepare*'s `keeps_copy`, a player
who deleted their copy isn't asked for it again: the launcher runs the same line without `--game`, and the game is built
from the game files its folder kept, as an update is. RecompLauncher keeps a game set up through an update and builds it
again only when *prepare*'s `version` changes, so each release's copy of the file carries the release's version there
(`scripts/builder_package.py` `release_manifest`, for the zip and the AppImage): after an update, the next Play builds
the game again, from what the folder kept.

## One-time setup

What a repository needs before its first release (the organization's secret is shared by Longshot and the launcher):

1. **The key.** On a PC you trust, `python -I ../Tooling/scripts/release.py keygen <safe folder>`. Put the printed line in the
   launcher's `Core/ReleaseTrust.cs` and in [`scripts/release_keys.json`](../../../scripts/release_keys.json), a pull
   request to each. Keep `release.pem` backed up offline: losing it means a launcher with a new key that every player
   downloads by hand.
2. **The secret** (MagicBeanDev > *Settings* > *Actions* > *Secrets*): `RELEASE_SIGNING_KEY_SEALED`, `release.pem`
   sealed with the server's `seal.key` (`scripts/ci_secret.sh seal --key seal.key < release.pem`; [CI](ci.md#secrets-only-our-runners-can-open)).
   No token secret or bot account: `release.yml` makes the release with the job's own token, which Forgejo
   gives write access to its own repository on a push or a tag. Don't protect `nightly-*` tags, or the prune can't delete
   them. `tools.py` and `release.py` run by hand (`mirror`, `add`, `prune`) take your own token as `RELEASE_TOKEN`.
3. **The upload address.** Uploads through Cloudflare are cut at 100 MB, and the builders are larger. Set the
   organization variable `RELEASE_UPLOAD_API` (passed to `release.py` as `FORGEJO_UPLOAD_API`, since Forgejo refuses
   variable names that start with `FORGEJO_`) to the API without Cloudflare, as the runners reach it. Unset, uploads go
   to `https://git.magicbean.dev/api/v1`.
4. **The attachment limits** in Forgejo's `app.ini`: `[attachment] MAX_SIZE` at least the largest file, and
   `[repository.release] ALLOWED_TYPES` empty, so `.AppImage`, `.exe`, `.lock` and `SHA256SUMS` are accepted.
5. **Actions and runners:** Actions enabled on the repository, and Linux runners registered (the Windows files are
   cross-built on Linux; the runners are in [CI](ci.md#runners)).
6. **The tools' packages:** `python -I scripts/tools.py check --require` passes; `release.yml` stops in its first job
   until it does.
7. **Try it** on a fork, or with a throwaway key: `LONGSHOT_LAUNCHER_TRUST=<test .pub.pem>` makes a launcher accept a
   release signed with it.

## Troubleshooting

| Symptom | Fix |
|---|---|
| *SHA256SUMS, signed*: "the sealed secret is empty" | `RELEASE_SIGNING_KEY_SEALED` isn't set or visible to the repository ([one-time setup](#one-time-setup), step 2) |
| *SHA256SUMS, signed*: "no /run/magicbean-secrets/seal.key" | the job ran on a runner without `seal.key`: only the server's quick runner should carry `deploy` ([CI](ci.md#secrets-only-our-runners-can-open)) |
| *SHA256SUMS, signed*: "bad decrypt" | the secret was sealed with another `seal.key`: seal it again with the server's |
| `verify`: "not signed by a trusted key" | the secret's key isn't in `release_keys.json`: set the matching key, or rotate |
| *Publish*: HTTP 413 or a cut connection | an upload over Cloudflare's 100 MB limit: set `RELEASE_UPLOAD_API` |
| *Publish*: "upload N of 4 failed", HTTP 5xx | `release.py` tries each upload four times; if all fail, check Forgejo's log and re-run (the draft is reused) |
| *Publish*: HTTP 400/422 | Forgejo's attachment size or type limits (`[attachment] MAX_SIZE`, `[repository.release] ALLOWED_TYPES` empty) |
| *Publish*: HTTP 401/403 | the run isn't a push to this repository: release from a tag or push here only |
| "is already published" | a re-run of a released version: nothing to do |
| *The version*: "tools.lock is missing" / "not in Longshot's store" | [bump the tool](#bumping-a-tool) |
| Too many nightlies | the prune failed on a protected `nightly-*` tag: unprotect it, or `RELEASE_TOKEN=<token> release.py prune` |
| `next` or `notes`: "git-cliff is needed" | `export GIT_CLIFF=$(python -I ../Tooling/scripts/release.py git-cliff <folder>)` |
| A launcher offers nothing on a channel | `release.py channels`, then check that release's files and signature |
| A draft is left after a failed run | re-run it (the draft is reused), or delete the draft |
| The launcher: "not signed by a key this launcher trusts" | a release signed with a key the launcher lacks: release the launcher with the key first ([rotating the key](#rotating-the-key)) |
| A launcher doesn't see a new release | it checks at start and on Retry, and never sees a draft: check the publish finished |
| A Windows install fails at a tool | its upstream URL is gone and it has no mirror: `tools.py mirror <name>`, or bump it |

## Tools

| Tool | What it does |
|---|---|
| `python -I ../Tooling/scripts/release.py` | versions (`next`, `check-tag`, `nightly-name`), notes, sums and signatures (`sums`, `sign`, `verify`, `keygen`), publishing (`publish`, `prune`, `released-as`, `channels`); `--help` lists all |
| `python -I scripts/tools.py` | `tools.lock`: `check`, `fetch`, `install`, `mirror`, `add`, `pack`, `assemble` |
| `python -I scripts/builder_package.py` | the Windows builder's folder and zip, and the pinned llvm-mingw |
| `python -I scripts/check_windows_files.py` | the Windows zip checked as Windows files, and started under Wine |
| `scripts/linux/deps.sh`, `appimage.sh`, `docker.sh` | the AppImage's dependencies, the AppImage, both in an Ubuntu 22.04 container; `deps.sh tools` builds the Linux tools |
| [cliff.toml](../../../cliff.toml) | git-cliff's settings for the release notes |
