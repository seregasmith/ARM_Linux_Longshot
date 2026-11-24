"""The checks in a container (tests/run_all.py --container, scripts/linux/checks.sh and checks.Dockerfile): the tools are CI's and
mise.toml's, what runs has no network and writes only the checkout, and run_all.py's other arguments reach it as they are.

A stand-in for podman and docker writes down what it is called with: nothing is built, fetched or started.

    python -I -B tests/test_checks_container.py
"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUN_ALL_SKIP = {"windows": "a bash script for Linux containers"}
DOCKERFILE = (ROOT / "scripts/linux/checks.Dockerfile").read_text(encoding="utf-8")
CI = re.search(r"\n  python:\n(?:(?:    .*)?\n)+", (ROOT / ".forgejo/workflows/ci.yml").read_text(encoding="utf-8")).group(0)   # (the checks' job)
MISE = (ROOT / "mise.toml").read_text(encoding="utf-8").split("\n[tasks.test-deps]\n")[1].split("\n[")[0]
ENGINE = """#!%s
import json, os, sys
name = os.path.basename(sys.argv[0])
if sys.argv[1:] == ["--version"]:
    print(name + " version 1" if name != "docker" else "Docker version 1")
elif sys.argv[1] == "info":
    print(os.environ.get("FAKE_INFO", "[]"))
else:
    with open(os.environ["FAKE_LOG"], "a") as f:
        f.write(json.dumps([name] + sys.argv[1:]) + "\\n")
    sys.exit(int(os.environ.get("FAKE_" + sys.argv[1].upper(), "0")))
"""


class VersionTests(unittest.TestCase):
    def same(self, pattern, *texts):
        found = [sorted(set(re.findall(pattern, t))) for t in (DOCKERFILE, *texts)]
        self.assertTrue(found[0], pattern)
        for other in found[1:]:
            self.assertEqual(found[0], other, pattern)

    def test_the_tools_are_cis(self):
        packages = re.search(r"apt-get install -y (g\+\+ clang [a-z0-9 +-]+)", CI).group(1).split()
        installed = re.search(r"apt-get install -y (g\+\+ clang [a-z0-9 +-]+)", DOCKERFILE).group(1).split()
        self.assertEqual(sorted(installed), sorted(packages))
        runner = "FROM " + re.search(r"os: (ubuntu)-([\d.]+)", CI).expand(r"\1:\2")
        self.assertIn(runner + "\n", DOCKERFILE)
        self.assertIn("dotnet-sdk-" + re.search(r"dotnet-version: '(\d+\.\d+)\.x'", CI).group(1), DOCKERFILE)

    def test_the_pins_are_cis_and_mises(self):
        self.same(r"(?:zstandard|pillow|numpy|lz4|unicorn)==[\d.]+", CI, MISE)
        self.same(r"KhronosGroup/(\w+-Headers) (?:\\\n *)?([0-9a-f]{40})", CI, MISE)
        self.same(r"glslang/releases/download/[\w./-]+\.tar\.gz", CI, MISE)
        self.same(r"echo '([0-9a-f]{64}) ", CI, MISE)


@unittest.skipUnless(shutil.which("bash") and shutil.which("git"), "bash and git")
class RunTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="longshot-checks-")
        self.addCleanup(temporary.cleanup)
        self.top = Path(temporary.name).resolve()
        self.log = self.top / "log"
        self.bin = self.top / "bin"
        self.bin.mkdir()
        # (the scripts' own tools, and none of the PC's engines)
        for tool in ("bash", "env", "git", "dirname", "grep", "id", "cat", "realpath"):
            os.symlink(shutil.which(tool), self.bin / tool)
        self.root = self.checkout(self.top / "a checkout")

    def checkout(self, root):
        for name in ("scripts/linux/checks.sh", "scripts/linux/checks.Dockerfile", "tests/run_all.py", "tests/compiler_command.py"):
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(ROOT / name, root / name)
        return root

    def engine(self, name):
        path = self.bin / name
        path.write_text(ENGINE % sys.executable)
        path.chmod(0o755)

    def run_all(self, *args, root=None, cwd=None, **environment):
        self.log.write_text("")
        env = {"PATH": str(self.bin), "HOME": str(self.top), "FAKE_LOG": str(self.log), "LC_ALL": "C", **environment}
        done = subprocess.run([sys.executable, "-I", "-B", str((root or self.root) / "tests/run_all.py"), *args], env=env,
                              cwd=cwd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
        self.calls = [json.loads(line) for line in self.log.read_text().splitlines()]
        return done

    def command(self):
        self.assertEqual([c[1] for c in self.calls], ["build", "run"])
        return self.calls[1]

    def test_the_other_arguments_reach_it_as_they_are(self):
        self.engine("podman")
        done = self.run_all("--strict", "--container", "a name", "--exact")
        self.assertEqual(done.returncode, 0, done.stderr)
        run = self.command()
        at = run.index("python3")
        self.assertEqual(run[at:], ["python3", "-I", "-B", "tests/run_all.py", "--strict", "a name", "--exact"])
        self.assertEqual(run[at - 1], self.calls[0][self.calls[0].index("-t") + 1], "the image it built")
        self.assertIn("scripts/linux/checks.Dockerfile", self.calls[0])

    def test_no_network_and_only_the_checkout(self):
        self.engine("podman")
        self.run_all("--container")
        run = self.command()
        self.assertEqual(run[run.index("--network") + 1], "none")
        mounts = [run[i + 1] for i, a in enumerate(run) if a in ("-v", "--volume", "--mount")]
        self.assertEqual(mounts, ["%s:%s" % (self.root, self.root)])
        self.assertEqual(run[run.index("-w") + 1], str(self.root))
        self.assertIn("--rm", run)
        self.assertFalse([a for a in run if "privileged" in a or "docker.sock" in a])

    def test_junit_paths_use_the_callers_directory_and_stay_in_the_checkout(self):
        self.engine("podman")
        done = self.run_all("--container", "--junit", "a checkout/results/report.xml", cwd=self.top)
        self.assertEqual(done.returncode, 0, done.stderr)
        run = self.command()
        self.assertEqual(run[run.index("--junit") + 1], str(self.root / "results/report.xml"))
        done = self.run_all("--container", "--juni=results/report.xml", cwd=self.root)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("--juni=" + str(self.root / "results/report.xml"), self.command())
        (self.root / "outside").symlink_to(self.top, target_is_directory=True)
        for path in (str(self.top / "report.xml"), "../report.xml", "outside/report.xml"):
            done = self.run_all("--container", "--junit", path, cwd=self.root)
            self.assertEqual(done.returncode, 2, path)
            self.assertIn("--junit destination must be inside the checkout", done.stderr)
            self.assertEqual(self.calls, [])

    def test_a_worktree_has_its_repository_read_only(self):
        self.engine("podman")
        git = ["git", "-c", "user.name=a", "-c", "user.email=a@example.com", "-C", str(self.root)]
        for args in (["init", "-q"], ["add", "-A"], ["commit", "-qm", "a"], ["worktree", "add", "-q", str(self.top / "other")]):
            subprocess.run(git + args, check=True, env={"PATH": os.environ["PATH"], "HOME": str(self.top)})
        self.run_all("--container")   # (the main checkout: its .git is in it)
        self.assertEqual(sum(a == "-v" for a in self.command()), 1)
        self.run_all("--container", root=self.top / "other")
        run = self.command()
        mounts = [run[i + 1] for i, a in enumerate(run) if a == "-v"]
        self.assertEqual(mounts, ["{0}:{0}".format(self.top / "other"), "{0}:{0}:ro".format(self.root / ".git")])

    def test_podman_before_docker_and_the_one_asked_for(self):
        self.engine("docker")
        self.run_all("--container")
        self.assertEqual(self.command()[0], "docker")
        self.engine("podman")
        self.run_all("--container")
        self.assertEqual(self.command()[0], "podman")
        self.run_all("--container", DOCKER="docker")
        self.assertEqual(self.command()[0], "docker")

    def test_it_runs_as_the_caller(self):
        self.engine("podman")
        self.engine("docker")
        self.run_all("--container")
        self.assertIn("--userns=keep-id", self.command())
        self.run_all("--container", DOCKER="docker")
        run = self.command()
        self.assertEqual(run[run.index("--user") + 1], "%d:%d" % (os.getuid(), os.getgid()))
        self.run_all("--container", DOCKER="docker", FAKE_INFO="[name=seccomp name=rootless]")
        self.assertNotIn("--user", self.command())   # (rootless Docker's root is the caller)

    def test_a_compiler_chosen_is_passed_on(self):
        self.engine("podman")
        self.run_all("--container", CXX="g++")
        run = self.command()
        self.assertEqual([run[i + 1] for i, a in enumerate(run) if a == "-e"], ["CXX"])

    def test_what_fails_is_the_result(self):
        self.engine("podman")
        self.assertEqual(self.run_all("--container", FAKE_RUN="1").returncode, 1)
        self.assertEqual(self.run_all("--container", FAKE_BUILD="3").returncode, 3)
        self.assertEqual([c[1] for c in self.calls], ["build"], "nothing runs in an image that was not built")

    def test_no_engine_says_so(self):
        done = self.run_all("--container")
        self.assertEqual(done.returncode, 2)
        self.assertIn("podman", done.stderr)
        self.assertEqual(self.calls, [])


if __name__ == "__main__":
    unittest.main()
