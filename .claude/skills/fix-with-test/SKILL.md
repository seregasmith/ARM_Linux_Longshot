---
name: fix-with-test
description: Fix a Longshot bug as two commits - a game-free test that fails on main for the bug's reason, then the fix - and check the test and the fix the way a reviewer will. Use when fixing a bug, a regression or a crash, when adding or changing a check under tests/, or when a pull request needs its "game-free test, failing on main and passing here".
---

# A fix and its game-free test

How to write and run a check is [developing](../../../docs/dev/developing.md#write-a-check); this is the order of the
work and what a passing test doesn't check for you.

## 1. The cause first

Find the cause before the fix: a log line, a trace, a measurement, the code path read through. Search for the same fix
first (the [prior-work skill](../prior-work/SKILL.md)). Can't reproduce or explain it: stop and say so.

## 2. The test

- `tests/test_<topic>.py`, standard library, its docstring saying what it checks and how to run it.
- For C and C++, cut the function out of the real source (`tests/srccut.py`) and compile it with stand-ins; start from a
  check that already reads the file (`git grep -l <file name> tests`). Never test a pasted copy.
- Check the behaviour the bug broke (what the function returns, writes or calls), not the text of the new line.
- A fix behind a switch is tested both ways.
- Fast, and on Linux and Windows where it can run; `RUN_ALL_SKIP` with the reason where it can't.

## 3. Two commits

1. **The test alone**: it fails, for the bug's reason (not because it doesn't compile).
2. **The fix**: it passes.

When review changes either, rewrite the branch so it still reads test, then fix.

## 4. What the test doesn't check

- **A broken fix**: undo the fix's key line, see the test fail, put it back.
- **The changed files whole**: a cut can pass while the file doesn't build. Build what changed (the renderer alone
  needs no game), or compile each changed file with `-fsyntax-only -Wall -Wextra` on the branch and on `main`. Say what
  was compiled and what wasn't.
- **Both compilers**: `CXX=g++` and `CXX=clang++`; the runner falls back silently when `CXX` is missing.
- **Skips**: a skip is not a pass. Read the table, or use `--strict`.
- **Other checks that read the file**: run them, and update one your change breaks rather than weakening it.
- **The whole suite once** (`mise run test`); a failure that also fails on `main` is named in the pull request.
- **What it doesn't model** (threads, timing, the GPU, a driver, the game's data): name it under *Still to test*. For
  a fix in memory ownership, threads, timing or saves, have a second agent review it cold (the diff and the test, not
  your conclusion).

## 5. In the pull request

- Summary: how to see the bug on `main`.
- Testing's *Tested*: the test failing on `main` and passing here, and the in-game run.
- Measurements: the test's result and the in-game evidence (a log line, a screenshot) as `Before: main @ <hash>` and
  `After: this PR @ <hash>`.
- Tick the *A fix* box only after running the test both ways.

## 6. Stop and ask

- When no game-free test can show the bug: say why, and agree on the in-game evidence instead.
- When the fix needs a decision: a new default, a switch, a rule in
  [architecture](../../../docs/dev/reference/architecture.md).
