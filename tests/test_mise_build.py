"""The mise tasks: the front door is a handful of them, and the build and development ones forward their arguments to the
scripts unchanged; `mise run build` adds a work folder of the development folder's (`dev.py paths work`) when none is given,
never the player's builder folder."""

import json
import os
import shutil
import subprocess
import tempfile
import tomllib
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUN = """work=1
for a in "$@"; do case "$a" in --work|--work=*) work= ;; esac; done
# (not the player's builder folder, ~/.cache/longshot-builder or %LOCALAPPDATA%\\LongshotBuilder: `dev.py paths work`)
[ -z "$work" ] || set -- --work "$(python3 -I scripts/dev.py paths work)" "$@"
python3 -I nx2/make_game.py "$@"
"""


class MiseBuildTest(unittest.TestCase):

    def test_task_declaration(self):
        with (ROOT / "mise.toml").open("rb") as f:
            tasks = tomllib.load(f)["tasks"]
        build = tasks["build"]
        self.assertEqual(build["depends"], ["build-deps"])
        self.assertIn("-r requirements.txt", tasks["build-deps"]["run"])
        self.assertEqual(build["shell"], "bash -c")
        self.assertEqual(build["run"], RUN)
        self.assertIn("nx2/make_game.py", build["description"])

        # (build-dev: LONGSHOT_JOBS, else make_game.py's default_jobs, one compiler per CPU thread but one; build.py alone
        # compiles one file at a time)
        expected = {
            "build-dev": "\njobs=\"${LONGSHOT_JOBS:-0}\"\n"
                         "[ \"$jobs\" != 0 ] || jobs=\"$(python3 -I -c 'import os; print(max(1, (os.cpu_count() or 2) - 1))')\"\n"
                         "python3 -I nx2/build.py --standalone --jobs \"$jobs\" \"$@\"\n",
            "dev": "python3 -I scripts/dev.py \"$@\"",
        }
        for name, run in expected.items():
            with self.subTest(task=name):
                task = tasks[name]
                self.assertEqual(task["shell"], "bash -c")
                self.assertEqual(task["run"].lstrip("\n"), run.lstrip("\n"))
                self.assertEqual(task.get("depends"), ["build-deps"])
        self.assertEqual(tasks["setup"]["depends"], ["build-deps", "test-deps"])
        self.assertEqual(tasks["setup"]["run"], "python3 -I scripts/setup.py")
        self.assertIn("never measured", tasks["build-dev"]["description"])
        # (a non-Linux setup goes on without the checks' Linux tools)
        self.assertIn("exit 0", tasks["test-deps"]["run"].split("fi", 1)[0])
        shown = sorted(n for n, t in tasks.items() if not t.get("hide"))
        self.assertEqual(shown, ["build", "build-dev", "ci", "ci:smoke", "dev", "lint", "setup", "test", "test-changed", "tracy-tools"],
                         "a new task is one more thing to learn: fold it into one of these, or hide a helper")

    def test_windows_tooling_and_graceful_launcher(self):
        """pre-commit comes from PyPI (Aqua has no Windows build), and scripts/run-vk.ps1 -Graceful closes the game's window
        before it stops it."""
        with (ROOT / "mise.toml").open("rb") as f:
            config = tomllib.load(f)
        self.assertEqual(config["tools"]["pypi:pre-commit"], "4.0.1")
        self.assertNotIn("aqua:pre-commit/pre-commit", config["tools"])
        self.assertNotIn("aqua:pre-commit/pre-commit", (ROOT / "mise.lock").read_text(encoding="utf-8"))

        launcher = (ROOT / "scripts" / "run-vk.ps1").read_text(encoding="utf-8")
        self.assertIn("[switch]$Graceful", launcher)
        self.assertIn("-Graceful requires -Window", launcher)
        self.assertIn("if ($Graceful)", launcher)
        self.assertIn("$p.CloseMainWindow()", launcher)
        self.assertIn("$p.WaitForExit(10000)", launcher)
        self.assertLess(launcher.index("$p.CloseMainWindow()"), launcher.index("Stop-Process -Id $p.Id"))
        self.assertIn("if ($Graceful -and -not $exited) { exit 1 }", launcher)
        self.assertIn("if ($Graceful) { exit $p.ExitCode }", launcher)

    @unittest.skipUnless(shutil.which("mise") and shutil.which("bash") and shutil.which("python3"),
                         "standalone task help needs mise, bash and python3")
    def test_standalone_and_dev_help(self):
        """build-dev and dev reach nx2/build.py's and dev.py's help without starting a build."""
        for name, args, expected in (("build-dev", ["--help"], "usage: build.py"), ("dev", ["packs", "--help"], "usage: dev.py packs")):
            with self.subTest(task=name):
                done = subprocess.run(["mise", "run", "--yes", name, "--", *args], cwd=ROOT,
                                      text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
                self.assertEqual(done.returncode, 0, done.stdout)
                self.assertIn(expected, done.stdout)

    @unittest.skipUnless(shutil.which("mise") and shutil.which("bash") and shutil.which("python3"),
                         "the mise build task needs mise, bash and python3")
    def test_arguments_arrive_unchanged(self):
        """Run a fake builder via mise, with no dump or real build, covering spaces and a trailing backslash: the arguments
        arrive as given, after the development folder's --work when they name none."""
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "nx2").mkdir()
            (root / "scripts").mkdir()
            # (TOML's basic strings take JSON's escapes)
            (root / "mise.toml").write_text(
                '[tasks.build]\ndescription = "argv recorder"\nshell = "bash -c"\nrun = %s\n' % json.dumps(RUN),
                encoding="utf-8")
            (root / "nx2" / "make_game.py").write_text(
                "import json, sys\nprint(json.dumps(sys.argv[1:]))\n", encoding="utf-8")
            (root / "scripts" / "dev.py").write_text(
                "import sys\nassert sys.argv[1:] == ['paths', 'work']\nprint('/dev home/work')\n", encoding="utf-8")
            args = ["--nup", "Game [v131072] with spaces.nup", "--out", "C:\\Fake Folder\\", "--language", "en-US"]
            for given, expected in ((args, ["--work", "/dev home/work"] + args),
                                    (args + ["--work", "my work"], args + ["--work", "my work"]),
                                    (["--work=w"] + args, ["--work=w"] + args)):
                with self.subTest(given=given):
                    done = subprocess.run(["mise", "run", "--yes", "build", *given], cwd=root, env=os.environ.copy(),
                                          text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
                    output = done.stdout.strip().splitlines()
                    self.assertEqual(done.returncode, 0, done.stdout)
                    self.assertTrue(output, "mise produced no argv to check")
                    try:
                        actual = json.loads(output[-1])
                    except json.JSONDecodeError:
                        self.fail("the fake builder did not print JSON argv: " + done.stdout)
                    self.assertEqual(actual, expected)


if __name__ == "__main__":
    unittest.main()
