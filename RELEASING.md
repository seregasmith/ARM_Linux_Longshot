# Releasing

How a maintainer releases Longshot is in [docs/dev/reference/releases.md](docs/dev/reference/releases.md): what a
release is, versions and the two channels (stable and nightly), and the steps.

| To | Read |
|---|---|
| Make a nightly, a stable or a hotfix | [Making a release](docs/dev/reference/releases.md#making-a-release) |
| Pull a bad release | [Pulling a bad release](docs/dev/reference/releases.md#pulling-a-bad-release) |
| Fix a tag CI refused | [When CI refuses a tag](docs/dev/reference/releases.md#when-ci-refuses-a-tag) |
| Get the nightly to be tagged tested on Windows | [Windows tester](docs/dev/reference/releases.md#windows-tester) |
| Bump a tool in `tools.lock` | [Bumping a tool](docs/dev/reference/releases.md#bumping-a-tool) |
| Rotate the release key | [Rotating the key](docs/dev/reference/releases.md#rotating-the-key) |
| Set up a repository's secret, upload address and limits | [One-time setup](docs/dev/reference/releases.md#one-time-setup) |
| Fix a failed release run | [Troubleshooting](docs/dev/reference/releases.md#troubleshooting) |

Agents: the [release skill](https://git.magicbean.dev/MagicBeanDev/Tooling/src/branch/main/skills/release/SKILL.md)
(`/magicbean:release stable`). The Longshot Launcher is released the same way from
[MagicBeanDev/LongshotLauncher](https://git.magicbean.dev/MagicBeanDev/LongshotLauncher), which has its own RELEASING.md.
