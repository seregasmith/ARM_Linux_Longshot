# Crash window

After a crash, a helper process shows a window saying what happened and offers **Report automatically**, **Report
manually**, **Open the log folder** and **Close**. Nothing leaves the PC unless the player presses **Send** (or opted in
to automatic sending, below). Crash records and logs themselves are covered in [developing](../developing.md#crash-records).

## Parts

| Part | What it does |
|---|---|
| [`host/hos_crash_dialog.c`](../../../host/hos_crash_dialog.c) | The helper: waits for the game to end, reads what it left, builds the report (`build_report`), posts it (`post_report`), drives the pages |
| [`host/hos_crash_dialog.h`](../../../host/hos_crash_dialog.h) | Every decision, testable alone: addresses, when to offer sending, the report's fields and limits, the release flag, the answer |
| [`gpu/vk/vk_crash_window.cpp`](../../../gpu/vk/vk_crash_window.cpp) | The window, in `longshot_vk`, drawn like the Insert menu; entry point `mkwvk_crash_page` ([`crash_window_api.h`](../../../gpu/vk/crash_window_api.h)) |
| [`gpu/vk/compat/longshot_redact.h`](../../../gpu/vk/compat/longshot_redact.h) | The one redaction rule for logs, the crash report and the Insert menu's Bug report |
| [`nx2/addresses.ini`](../../../nx2/addresses.ini) | `crash_reports` (the telemetry endpoint, which keeps the reports where only the team reads them) |
| `longshot_crash.ini` | In the game folder, written by the launcher: `send=1` sends reports without asking |

## How it runs

1. Early in start-up the game starts itself again as
   `longshot --crash-watch <pid> <run id> <game folder> <log> <options>` (`<options>`: the run's `longshot_options.ini`,
   or `-`, and then the game folder's is read). The helper loads no game code and never Vulkan.
2. When the game ends, the helper reads the crash record in `bug-reports/pending/`, the run's log,
   the options file it was given, `longshot_defaults.ini` and `longshot_version.txt`. It never reads the crashed process.
3. It shows the window through SDL's software renderer (SDL3 or SDL2), since the crash may have been the GPU's. On
   Linux it prefers X11 (XWayland on Wayland, where SDL has no software renderer); `SDL_VIDEO_DRIVER` still overrides.
   Without the window it falls back to SDL's message box, then Windows' own.
4. **Report automatically** shows the full report, every field as sent (**Show all of it**), and posts it on **Send**.
   The Sent page says the team has it and links nowhere: the reports are kept where only the team can read them (an
   older relay's `comment` link in the answer is ignored). The answer's status is kept in `bug-reports/sent/<record>`, so
   the same crash is not offered again.
5. **Report manually** puts a redacted text report on the clipboard, saves it as `bug-reports/pending/report-<UTC>.txt`
   and opens the issue form in the browser.

With `send=1` in `longshot_crash.ini` (the launcher's **Send crash reports automatically**), the helper posts the same
report with no window, appends `[hos crash] sent on its own (longshot_crash.ini): <Sent or the reason>` to the run's log,
and keeps a copy of what it sent in `bug-reports/sent/`. If sending fails, the window shows as usual.

A crash record that no helper showed (`bug-reports/shown.txt` lists those shown) is shown at the next start
(`longshot --crash-show <record> <game folder> <log> <options>`).

**Report automatically** is offered only for a crash record (not an unclean end), only when `longshot_version.txt` has
`crash_reports=`, and only until sent. `nx2/make_game.py` writes it, and `source=`, only in a
build from packaged tools; a build from a checkout has no Report automatically.

Posting uses the system's `curl`: `--disable` (ignores `.curlrc`), `--proto =https`, no redirects, 10 s to connect and
25 s in all. On Linux it runs without the game's `LD_LIBRARY_PATH` and `LD_PRELOAD`.

The window ignores key repeats and any key or click in a page's first 0.4 s, so keys held or mashed as the game died
press nothing.

## The report

| Field | From |
|---|---|
| `version` | The log's first line |
| `release` | True when the version is a channel's ([releases](releases.md)) and the log's source fingerprint matches `longshot_version.txt`'s `source=`. A sorting label, not proof |
| `os`, `cpu`, `gpu`, `renderer` | The log's `report:` lines (from `host/rx_backend.c` and `gpu/vk/vk_main.cpp`) |
| `map` | The last `map:` line before the crash |
| `options` | Every setting, changed ones first, up to 4000 characters |
| `crash` | The `CRASH:` lines, up to 80; a library is named by its module, not its path |
| `log_tail` | The 150 lines before the first `CRASH:` line, 300 characters each |
| `client` | Always `"Longshot"` |

**Never sent:** register and memory rows (they hold game memory), key and pad bindings, the microphone's name, hardware
hashes, audio device names, any file. The user's name and home folder are replaced (`~`, `%USERPROFILE%`, `<user>`);
the server does it again. After the run, the helper also rewrites its own run's `longshot_log.txt` with the same rule.

## Out of memory (Linux)

The OOM killer ends the game with SIGKILL, and systemd then stops the game's whole scope, helper included. So the
helper:

- compares its cgroup's `memory.events` `oom_kill` count at start and at the end, and writes `oom_kill: yes` into the
  unclean record when it grew;
- moves itself into its own `longshot-crash-<pid>.scope` (`StartTransientUnit` through `busctl`; not possible in a
  Flatpak);
- holds SIGTERM until the record is written.

If systemd stops it anyway, the next start shows the out-of-memory end.

## Switches

| Switch | Effect |
|---|---|
| `HOS_CRASH_WATCH=0` | No helper and no window (also off for `--prepare-shaders`, `HOS_HEADLESS`, `HOS_PGO` and under gamescope) |
| `HOS_CRASH_TEST=start` or `=<seconds>` | Crashes on purpose to show the window; read from the environment only, not `longshot_env.txt` |
| `HOS_CRASH_MINIDUMP=1` | Windows: also writes a minidump |

`tests/test_crash_dialog.py` covers the decisions and runs the real helper with a stand-in for `curl`, no game needed.
