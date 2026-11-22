# CI

CI is moving from Forgejo Actions (`.forgejo/workflows/`) to CircleCI (`.circleci/`). Until the cutover both run:
CircleCI runs the checks, and Forgejo Actions still publishes the releases and deploys the website. Neither needs the
game: every check is game-free.

**Run CI's checks yourself:** `mise run ci` runs what CI checks on every change: the pinned test tools (`test-deps`),
every game-free check (`tests/run_all.py --strict`, the same line CI runs in parts) and the website's build and checks
(`scripts/website_check.sh`). `mise run test` runs the checks alone, and takes `run_all.py`'s arguments.

## CircleCI

CircleCI uses the team's orb, `magicbeandev/ci` (alias `mb`). `.circleci/config.yml` has one `mb/setup` job per event;
it compares the changed files with `.circleci/mapping.conf` and continues with that event's workflow:

| Event | Workflow | What runs |
|---|---|---|
| Pull request (same repository) | `events/pr.yml` | The checks the changed paths call for, a website preview, then `gate` |
| Push to `main` | `events/main.yml` | Every check, then the nightly release (dry run until the cutover) and the website deploy (dry run) |
| Tag `v*` | `events/release.yml` | Every check, the Windows ones included, before anything is built, signed or published (dry run) |
| Schedule `nightly` | `events/nightly.yml` | `third-party-upstream`: the `third_party/` parts against their upstream archives |
| Schedule `website_refresh` | `events/scheduled.yml` | The website deploy (dry run) |
| API run | `events/manual.yml` | A release dry run, or the Windows Vulkan back end linked with DLSS |
| Fork pull request | `events/fork.yml` | The pull request checks, with no secrets (below) |

Every workflow ends at `mb/gate`, which posts the `ci/circleci: gate` status on the commit.

The checks, in `.circleci/shared.yml`:

| Job | What it checks |
|---|---|
| `lint` | pre-commit on the changed files, and actionlint (`mise run lint`) |
| `third-party` | `scripts/check_third_party.py` |
| `guide-screenshots` | the player guide's screenshots and their source review |
| `test-linux` | `tests/run_all.py`, in 4 parts on x86-64 |
| `builder-cli` | the Windows builder's `--cli` program |
| `spirv-fixtures` | the shaders' SPIR-V fixtures |
| `linux-appimage` | the Linux builder's AppImage |
| `windows-builder` | the Windows builder's zip, its PE files and a start under Wine |
| `windows-vulkan` | the Windows Vulkan back end, cross-compiled with llvm-mingw |
| `website` | the website's build and checks |

`mapping.conf` turns on areas by path (`tests`, `gpu`, `linux`, `windows`, `website`, or `full` for everything);
`lint`, `third-party`, `guide-screenshots` and `website` (the site's checks) always run, and a docs-only change runs
nothing else; `website` turns on the site's preview (pull requests) and deploy (`main`). `main`, tags and API
runs run everything.

**Fork pull requests.** CircleCI's Forgejo integration skips them, so a second CircleCI project (`Longshot-forks`)
runs `.circleci/forks.yml` from a Forgejo webhook. A maintainer adds the label `ci: approved` after reading the change;
the doorbell job checks the pull request (open, from a fork, labelled, its head commit) and runs `events/fork.yml` on
that commit with no secrets. Every push runs again while the label is on; remove it to stop, add it again to rerun.
After 14 days with no push the label is removed.

**Contexts** (secrets): `release-signing`, `forgejo-bot`, `cloudflare-prod`, `forgejo-comment`.

**Checking a change to the config:** `MB_ORB_DIR=<a checkout of MagicBeanDev/orb> pre-commit run --hook-stage manual circleci-config-validate`.

## Forgejo Actions

| Workflow | When | What it does |
|---|---|---|
| `ci.yml` | Pull request, push to `main` | The pull request's title, actionlint, the website's checks (`scripts/website_check.sh`) and `tests/run_all.py` in two parts |
| `pr-description.yml` | Pull request | The description against the template (`scripts/check_pr_description.py`) |
| `third-party.yml` | Pull request, push, nightly | `scripts/check_third_party.py`, and the upstream comparison at night |
| `guide-screenshots.yml` | Pull request, push | The guide screenshots' source review |
| `release.yml` | Push to `main`, tag `v*` | The nightly or the stable release: calls `build-linux.yml` and `build-windows.yml`, signs and publishes ([releases](releases.md)) |
| `website.yml` | Push to `main` that changes the site's sources, hourly, pull request | Deploys longshot.magicbean.dev; a pull request gets a preview |
| `vulkan-dlss.yml` | By hand | The Windows Vulkan back end built with NVIDIA's DLSS SDK and AMD FSR 4 |
| `vulkan-fixtures.yml` | By hand | The authored Vulkan fixtures on Mesa's software driver |

### Runners

Every job runs on Linux; there is no Windows runner. Windows files are cross-compiled with llvm-mingw's Linux build and
started under Wine. The labels the workflows ask for, all on the image `docker://ghcr.io/catthehacker/ubuntu:act-24.04`:

| Label | For |
|---|---|
| `ubuntu-latest`, `ubuntu-24.04` | the checks. No secrets: a runner anyone in the team lends can carry these |
| `quick` | jobs of a few minutes (title, description, third-party, actionlint, guide screenshots), on a runner kept for them so they never wait behind a build |
| `release` | the builders' builds, on a runner that carries no other label, so CI never takes it. It needs about 50 GB of disk |
| `deploy` | the jobs that sign, publish or deploy: a release's version and publish, the website's deploy and previews. Only the server's quick runner, which alone has `seal.key` ([below](#secrets-only-our-runners-can-open)) |

Register a runner with `forgejo-runner register --instance https://git.magicbean.dev --token <token> --labels ...` from
MagicBeanDev's *Settings > Actions > Runners*, and run `forgejo-runner daemon`. A runner lent by someone else gets
`ubuntu-latest` and `ubuntu-24.04` only. git.magicbean.dev is behind Cloudflare, which cuts uploads at 100 MB: the
organization variable `RELEASE_UPLOAD_API` gives a release runner the server's direct address ([one-time
setup](releases.md#one-time-setup)).

### Secrets only our runners can open

A label only routes a job: a runner says its own labels each time it starts, so anyone's runner can ask for `deploy`
or `release`, and Forgejo hands it the job and the job's secrets. So the secrets that sign or deploy are kept in
Forgejo **sealed** ([scripts/ci_secret.sh](../../../scripts/ci_secret.sh), AES-256 with PBKDF2), and the key that
opens them, `seal.key`, is a file on the server only: in the runners' Docker (dind) at `/magicbean-secrets/seal.key`,
which only the quick runner lets a job mount (`container.valid_volumes`, MagicBeanDev/Infra's `runner/quick/config.yml`).
A `deploy` job mounts it at `/run/magicbean-secrets` and opens its secret there.

- A runner without `seal.key` that takes such a job gets text it can't open: the job fails, and signs or deploys nothing.
- A job that can mount `seal.key` but gets no secrets (a fork's pull request) has nothing to open.

| Secret (organization) | Sealed | Used by |
|---|---|---|
| `RELEASE_SIGNING_KEY_SEALED` | the release key's private PEM, `release.pem` | `release.yml` publish ([signing](releases.md#signing)) |
| `CLOUDFLARE_API_TOKEN_SEALED` | the Cloudflare token | `website.yml`, deploy and preview |
| `PREVIEW_COMMENT_TOKEN_SEALED` | optional, a Forgejo token with `write:issue` | `website.yml`, the preview's comment on a fork's PR |

`CLOUDFLARE_ACCOUNT_ID` is not sealed: it opens nothing. To seal a secret, on a PC you trust with a copy of `seal.key`:
`scripts/ci_secret.sh seal --key seal.key < release.pem`, and paste the line into the Forgejo secret. A new
`seal.key` (`scripts/ci_secret.sh newkey`) means sealing every secret again.

What sealing doesn't cover:

- A fork's pull request runs its own copy of the workflows, so it can ask for `deploy` and mount `seal.key`. It gets no
  secrets to open, but `seal.key` itself would leave with it; whoever also has the sealed text (a runner that took a
  `deploy` job) could then open it. So a fork's run waits for a maintainer's approval: read its `.forgejo/` changes
  before you approve one, and don't mark a fork's author as always trusted.
- A job's own token (`forgejo.token`) comes with every job, whichever runner takes it, and on a push it may write to
  the repository (releases, tags).
- A builder that asks for `release` builds the files publish signs.

Lend runners only to people you'd trust that far.

## Before you push

Run `mise run lint` and `mise run test` and say so in the pull request. CircleCI's runs, logs and reruns are visible to
members of the MagicBeanDev organization on CircleCI ([CONTRIBUTING.md](../../../CONTRIBUTING.md#ci)).
