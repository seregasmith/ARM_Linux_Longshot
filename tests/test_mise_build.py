"""The mise build and development tasks forward their arguments to the existing scripts unchanged."""

import json
import os
import shutil
import subprocess
import tempfile
import tomllib
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUN = "python3 -I nx2/make_game.py \"$@\""
RUN_WIN = "powershell -NoProfile -ExecutionPolicy RemoteSigned -File scripts/run-vk.ps1 -Window -Graceful \"$@\""


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

        expected = {
            "build-dev": ("python3 -I nx2/build.py --standalone --jobs 4 \"$@\"", True),
            "build-fast": ("RX_HOT_OPT=O1 python3 -I nx2/build.py --standalone --jobs 8 \"$@\"", True),
            "packs": ("python3 -I scripts/dev_packs.py \"$@\"", True),
            "run-win": (RUN_WIN, False),
        }
        for name, (run, needs_deps) in expected.items():
            with self.subTest(task=name):
                task = tasks[name]
                self.assertEqual(task["shell"], "bash -c")
                self.assertEqual(task["run"], run)
                self.assertEqual(task.get("depends"), ["build-deps"] if needs_deps else None)
        self.assertEqual(tasks["setup"]["depends"], ["build-deps"])
        self.assertIn("never for measurements or releases", tasks["build-fast"]["description"])
        self.assertIn("Windows-only", tasks["run-win"]["description"])

    def test_windows_tooling_and_graceful_launcher(self):
        """Run-win must not depend on Aqua or a machine-wide PowerShell policy change."""
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
                         "the PowerShell argv recorder needs mise, bash and python3")
    def test_windows_task_arguments(self):
        """A fake PowerShell records the policy, -Graceful and user args without launching a game."""
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "bin").mkdir()
            recorder = root / "bin" / "powershell"
            recorder.write_text("#!/usr/bin/env python3\nimport json, sys\nprint(json.dumps(sys.argv[1:]))\n",
                                encoding="utf-8")
            recorder.chmod(0o755)
            env = dict(os.environ, PATH=str(root / "bin") + os.pathsep + os.environ.get("PATH", ""))
            done = subprocess.run(["mise", "run", "--yes", "run-win", "-Seconds", "2", "-Name", "Fake Folder\\"],
                                  cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  timeout=60)
            self.assertEqual(done.returncode, 0, done.stdout)
            actual = json.loads(done.stdout.strip().splitlines()[-1])
            self.assertEqual(actual[:3], ["-NoProfile", "-ExecutionPolicy", "RemoteSigned"])
            self.assertEqual(actual[3], "-File")
            self.assertTrue(actual[4].replace("\\", "/").endswith("scripts/run-vk.ps1"), actual)
            self.assertEqual(actual[5:], ["-Window", "-Graceful", "-Seconds", "2", "-Name", "Fake Folder\\"])

    @unittest.skipUnless(shutil.which("mise") and shutil.which("bash") and shutil.which("python3"),
                         "standalone task help needs mise, bash and python3")
    def test_standalone_and_packs_help(self):
        """The new task entry points reach nx2/build.py and dev_packs.py's help without starting a build."""
        for name in ("build-dev", "build-fast", "packs"):
            with self.subTest(task=name):
                done = subprocess.run(["mise", "run", "--yes", name, "--", "--help"], cwd=ROOT,
                                      text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
                self.assertEqual(done.returncode, 0, done.stdout)
                expected = "usage: build.py" if name.startswith("build-") else "usage: dev_packs.py"
                self.assertIn(expected, done.stdout)

    @unittest.skipUnless(shutil.which("mise") and shutil.which("bash") and shutil.which("python3"),
                         "the mise build task needs mise, bash and python3")
    def test_arguments_arrive_unchanged(self):
        """Run a fake builder via mise, with no dump or real build, covering spaces and a trailing backslash."""
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "nx2").mkdir()
            (root / "mise.toml").write_text(
                '[tasks.build]\ndescription = "argv recorder"\nshell = "bash -c"\nrun = \'%s\'\n' % RUN,
                encoding="utf-8")
            (root / "nx2" / "make_game.py").write_text(
                "import json, sys\nprint(json.dumps(sys.argv[1:]))\n", encoding="utf-8")
            expected = ["--nup", "Game [v131072] with spaces.nup", "--out", "C:\\Fake Folder\\", "--language", "en-US"]
            done = subprocess.run(["mise", "run", "--yes", "build", *expected], cwd=root, env=os.environ.copy(),
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
