"""`dev.py update` (scripts/devtools/update.py) with no build, no network and no game: a temporary repository stands for
upstream and a stand-in for the build writes the game folder. What is new since the folder's commit; an update that builds,
records the commit and the history and keeps the backups to three; one that is up to date builds nothing; a failed, a
stopped and an interrupted build put the folder back; --rollback; the settings remembered; --pick (only the picked
pull requests onto the folder's commit, a pick that needs one left out refused and named, every one picked = the newest);
the window's JSON list; --with (open pull requests' heads merged on top, with --pick too; one that conflicts refused and
named; an update without it leaves them out); the window's search; the forge's API from the remote's URL; --logs (logs and
crash records zipped, the minidumps and profiles left out); make_game.py's command for a
folder that keeps its game files and for one that doesn't; the build handed to a newer tree's own dev.py; a dirty worktree
refused; a running game refused.

    python -I -B tests/test_dev_update.py
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
from devtools import build, common, update  # noqa: E402

EXE = update.GAME_EXE


def git(cwd, *args):
    return subprocess.run(["git", "-C", cwd] + list(args), check=True, capture_output=True, text=True).stdout.strip()


def args(**kw):
    a = dict(game=None, remote=None, branch=None, check=False, rollback=False, gui=False, build_here=False, force=False,
             commit=None, pick=None, logs=False, list_json=False, open=False, with_=None)
    a.update(kw)
    return argparse.Namespace(**a)


class Update(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="dev_update_test_")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        env = {k: v for k, v in os.environ.items() if not k.startswith(("LONGSHOT_", "XDG_"))}
        env.update(LONGSHOT_DEV_HOME=os.path.join(self.tmp, "dev"), GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t",
                   GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")
        e = mock.patch.dict(os.environ, env, clear=True)
        e.start()
        self.addCleanup(e.stop)
        # (upstream: a repository with main; the checkout: a clone of it, as dev.py's ROOT)
        self.up = os.path.join(self.tmp, "upstream")
        os.makedirs(self.up)
        git(self.up, "init", "-q", "-b", "main")
        self.commit_up("first")
        self.repo = os.path.join(self.tmp, "checkout")
        subprocess.run(["git", "clone", "-q", self.up, self.repo], check=True, capture_output=True)
        p = mock.patch.object(update, "ROOT", self.repo)
        p.start()
        self.addCleanup(p.stop)
        self.game = os.path.join(self.tmp, "game")
        os.makedirs(self.game)
        self.write(EXE, "old game")
        self.write("longshot_env.txt", "RX_VK_FRAME_TRACE=1\n")
        self.builds = []
        r = mock.patch.object(update, "running_in", lambda g: False)
        r.start()
        self.addCleanup(r.stop)

    def commit_up(self, msg):
        with open(os.path.join(self.up, "f.txt"), "a") as f:
            f.write(msg + "\n")
        git(self.up, "add", "f.txt")
        git(self.up, "commit", "-q", "-m", msg)
        return git(self.up, "rev-parse", "HEAD")

    def commit_file(self, name, text, msg):
        with open(os.path.join(self.up, name), "w") as f:
            f.write(text)
        git(self.up, "add", name)
        git(self.up, "commit", "-q", "-m", msg)
        return git(self.up, "rev-parse", "HEAD")

    def tree_file(self, name):
        p = os.path.join(update.part("tree"), name)
        if not os.path.exists(p):
            return None
        with open(p) as f:
            return f.read()

    def pull(self, number, name, text, msg, base="main"):
        """An open pull request #number on upstream: a commit off base, its head at refs/pull/<number>/head"""
        git(self.up, "checkout", "-q", "-b", "pr%d" % number, base)
        sha = self.commit_file(name, text, msg)
        git(self.up, "checkout", "-q", "main")
        git(self.up, "update-ref", "refs/pull/%d/head" % number, sha)
        return sha

    def write(self, name, text):
        with open(os.path.join(self.game, name), "w", encoding="utf-8") as f:
            f.write(text)

    def read(self, name):
        with open(os.path.join(self.game, name), encoding="utf-8") as f:
            return f.read()

    def fake_build(self, rc=0, raise_=None):
        def run_build(tree, game, commit):
            self.builds.append((tree, game, commit))
            self.assertEqual(git(tree, "rev-parse", "HEAD"), commit)   # (the worktree is on the commit it builds)
            self.assertTrue(os.path.isfile(update.pending_file()))
            with open(os.path.join(game, EXE), "w", encoding="utf-8") as f:
                f.write("game " + commit[:8])
            if raise_:
                raise raise_
            return rc
        return mock.patch.object(update, "run_build", run_build)

    def test_update_records_and_keeps_three_backups(self):
        with self.fake_build():
            self.assertEqual(update.cmd_update(args(game=self.game)), 0)
            new = git(self.up, "rev-parse", "HEAD")
            self.assertEqual(self.read(EXE), "game " + new[:8])
            self.assertEqual(self.read(update.MARK), new)
            self.assertFalse(os.path.exists(update.pending_file()))
            self.assertEqual(len(update.backups()), 1)
            with open(os.path.join(update.backups()[0], EXE), encoding="utf-8") as f:
                self.assertEqual(f.read(), "old game")
            # (up to date: nothing built)
            update.cmd_update(args())
            self.assertEqual(len(self.builds), 1)
            for i in range(4):
                c = self.commit_up("change %d" % i)
                update.cmd_update(args())
                self.assertEqual(self.read(update.MARK), c)
        self.assertEqual(len(self.builds), 5)
        self.assertEqual(len(update.backups()), update.KEEP)
        with open(update.part("history.md"), encoding="utf-8") as f:
            hist = f.read()
        self.assertIn("change 3", hist)
        self.assertEqual(self.read("longshot_env.txt"), "RX_VK_FRAME_TRACE=1\n")

    def test_check_lists_new_commits_and_builds_nothing(self):
        with self.fake_build():
            update.cmd_update(args(game=self.game))
            self.commit_up("second")
            self.commit_up("third")
            out = self.output(lambda: update.cmd_update(args(check=True)))
        self.assertEqual(len(self.builds), 1)
        self.assertIn("2 new commits", out)
        self.assertIn("second", out)
        self.assertIn("third", out)

    def test_force_builds_when_up_to_date(self):
        with self.fake_build():
            update.cmd_update(args(game=self.game))
            update.cmd_update(args(force=True))
        self.assertEqual(len(self.builds), 2)

    def test_failed_build_puts_the_folder_back(self):
        with self.fake_build(rc=1), self.assertRaises(SystemExit):
            update.cmd_update(args(game=self.game))
        self.assertEqual(self.read(EXE), "old game")
        self.assertFalse(os.path.exists(os.path.join(self.game, update.MARK)))
        self.assertFalse(os.path.exists(update.pending_file()))

    def test_interrupted_build_puts_the_folder_back(self):
        with self.fake_build(raise_=KeyboardInterrupt()), self.assertRaises(KeyboardInterrupt):
            update.cmd_update(args(game=self.game))
        self.assertEqual(self.read(EXE), "old game")
        self.assertFalse(os.path.exists(update.pending_file()))

    def test_stopped_update_is_put_back_by_the_next_command(self):
        bk = update.back_up(self.game, "first")
        self.write(EXE, "half built")
        with open(update.pending_file(), "w", encoding="utf-8") as f:   # (a process that is gone: pid 0 / no start)
            json.dump({"pid": 999999999, "start": 123, "backup": bk, "commit": "abc"}, f)
        update.cmd_update(args(game=self.game, check=True))
        self.assertEqual(self.read(EXE), "old game")
        self.assertFalse(os.path.exists(update.pending_file()))

    def test_running_update_is_not_put_back(self):
        bk = update.back_up(self.game, "first")
        with open(update.pending_file(), "w", encoding="utf-8") as f:   # (this process stands for an update under way)
            json.dump({"pid": os.getpid(), "start": update.procs.proc_start(os.getpid()), "backup": bk, "commit": "abc"}, f)
        self.write(EXE, "being built")
        with self.assertRaises(SystemExit):
            update.cmd_update(args(game=self.game, check=True))
        self.assertEqual(self.read(EXE), "being built")

    def test_rollback(self):
        with self.fake_build():
            update.cmd_update(args(game=self.game))
        update.cmd_update(args(rollback=True))
        self.assertEqual(self.read(EXE), "old game")
        self.assertFalse(os.path.exists(os.path.join(self.game, update.MARK)))   # (it had none before)

    def test_settings_remembered(self):
        with self.fake_build():
            update.cmd_update(args(game=self.game, branch="main"))
        c = common.read_config(build.CONFIG)
        self.assertEqual(c["update"], {"game": os.path.abspath(self.game), "remote": "origin", "branch": "main"})
        self.assertEqual(update.settings()["game"], os.path.abspath(self.game))

    def test_no_game_named(self):
        with self.assertRaises(SystemExit):
            update.cmd_update(args(check=True))

    def test_dirty_worktree_refused(self):
        with self.fake_build():
            update.cmd_update(args(game=self.game))
        with open(os.path.join(update.part("tree"), "f.txt"), "a") as f:
            f.write("edit\n")
        self.commit_up("next")
        with self.fake_build(), self.assertRaises(SystemExit):
            update.cmd_update(args())
        self.assertEqual(len(self.builds), 1)

    def test_running_game_refused(self):
        with mock.patch.object(update, "running_in", lambda g: True), self.fake_build(), self.assertRaises(SystemExit):
            update.cmd_update(args(game=self.game))
        self.assertEqual(self.builds, [])
        self.assertEqual(self.read(EXE), "old game")

    def test_build_command(self):
        tree = os.path.join(self.tmp, "tree")
        env = mock.patch.object(build, "environment", lambda: ("py", {}))
        files = mock.patch.object(build, "game_files", lambda c: ("R", "E"))
        pre = mock.patch.object(build, "windows_prebuilt", lambda t: None)
        with env, files, pre:
            py, e, cmd = update.build_cmd(tree, self.game, "L")   # (no game files in the folder: dev.py's)
            self.assertEqual(cmd[:9], ["py", "-I", "nx2/make_game.py", "--romfs", "R", "--exefs", "E", "--out", self.game])
            self.assertEqual(e["LONGSHOT_KEEP_BUILD_FILES"], "1")
            os.makedirs(os.path.join(self.game, "romfs"))
            os.makedirs(os.path.join(self.game, ".longshot_exefs"))
            py, e, cmd = update.build_cmd(tree, self.game, "L")   # (the folder's own)
            self.assertEqual(cmd[:5], ["py", "-I", "nx2/make_game.py", "--update", self.game])
        self.assertEqual(cmd[cmd.index("--work") + 1], update.part("work"))

    def test_newer_tree_builds_with_its_own_dev_py(self):
        tree = os.path.join(self.tmp, "tree")
        os.makedirs(os.path.join(tree, "scripts", "devtools"))
        open(os.path.join(tree, "scripts", "devtools", "update.py"), "w").close()
        with mock.patch.object(update.subprocess, "call", return_value=0) as call:
            self.assertEqual(update.run_build(tree, self.game, "0123456789abcdef"), 0)
        cmd = call.call_args[0][0]
        self.assertEqual(cmd[2], os.path.join(tree, "scripts", "dev.py"))
        self.assertEqual(cmd[3:], ["update", "--build-here", "--game", self.game, "--commit", "0123456789abcdef"])

    def test_running_in(self):
        exe = os.path.join(self.game, EXE)
        other = os.path.join(self.tmp, "other", EXE)
        with mock.patch.object(update.procs, "exes", lambda: {1: EXE, 2: "python"}):
            with mock.patch.object(update.procs, "argv_of", lambda pid: [exe]):
                self.assertTrue(self.real_running_in(self.game))
            with mock.patch.object(update.procs, "argv_of", lambda pid: [other]):
                self.assertFalse(self.real_running_in(self.game))
            with mock.patch.object(update.procs, "argv_of", lambda pid: []):   # (unreadable: counted as running)
                self.assertTrue(self.real_running_in(self.game))

    def test_pick_some(self):
        with self.fake_build():
            update.cmd_update(args(game=self.game))
            base = self.read(update.MARK)
            self.commit_file("a.txt", "a\n", "feat(x): a (#11)")
            b = self.commit_file("b.txt", "b\n", "fix(y): b (#12)")
            self.commit_file("c.txt", "c\n", "perf(z): c (#13)")
            update.cmd_update(args(pick="13,#11"))
            self.assertEqual(self.tree_file("a.txt"), "a\n")
            self.assertIsNone(self.tree_file("b.txt"))
            self.assertEqual(self.tree_file("c.txt"), "c\n")
            self.assertEqual(self.read(update.MARK), base)   # (the folder's branch commit stays; the picks beside it)
            self.assertEqual(len(json.loads(self.read(update.PICKS))), 2)
            rows = update.merged(base, b, json.loads(self.read(update.PICKS)))
            self.assertEqual([r["picked"] for r in rows], [True, False])
            # (every one picked: the newest commit itself, no picks left)
            update.cmd_update(args(pick="11,12,13"))
            self.assertEqual(self.read(update.MARK), git(self.up, "rev-parse", "HEAD"))
            self.assertFalse(os.path.exists(os.path.join(self.game, update.PICKS)))
        with open(update.part("history.md"), encoding="utf-8") as f:
            self.assertIn("with picks", f.read())

    def test_pick_that_needs_one_left_out(self):
        with self.fake_build():
            update.cmd_update(args(game=self.game))
            self.commit_file("a.txt", "one\n", "feat(x): a (#21)")
            self.commit_file("a.txt", "two\n", "fix(x): a again (#22)")
            before = len(self.builds)
            with self.assertRaises(SystemExit) as e:
                update.cmd_update(args(pick="22"))
        self.assertIn("#21", str(e.exception.code))
        self.assertEqual(len(self.builds), before)
        self.assertEqual(self.read(EXE).startswith("game "), True)   # (the folder as it was: not backed up, not built)
        self.assertEqual(update.git("status", "--porcelain", cwd=update.part("tree")), "")

    def test_pick_unknown(self):
        with self.fake_build():
            update.cmd_update(args(game=self.game))
            self.commit_file("a.txt", "a\n", "feat(x): a (#31)")
            with self.assertRaises(SystemExit):
                update.cmd_update(args(pick="99"))

    def test_list_json(self):
        with self.fake_build():
            update.cmd_update(args(game=self.game))
        self.commit_file("a.txt", "a\n", "feat(x): a (#41)")
        d = json.loads(self.output(lambda: update.cmd_update(args(list_json=True))))
        self.assertTrue(d["known"])
        self.assertEqual([(r["pr"], r["subject"]) for r in d["commits"]], [(41, "feat(x): a (#41)")])

    def test_logs(self):
        for rel, text in (("logs/longshot_log.txt", "line"), ("logs/longshot_frames.csv", "f"),
                          ("logs/profile-1/all.txt", "big"), ("bug-reports/pending/crash-1.txt", "crash"),
                          ("bug-reports/local-only/x.dmp", "private")):
            os.makedirs(os.path.dirname(os.path.join(self.game, rel)), exist_ok=True)
            self.write(rel, text)
        z = update.gather_logs(self.game)
        import zipfile
        with zipfile.ZipFile(z) as zf:
            names = [n.replace("\\", "/") for n in zf.namelist()]
            summary = zf.read("summary.txt").decode()
        for want in ("game/logs/longshot_log.txt", "game/logs/longshot_frames.csv", "game/bug-reports/pending/crash-1.txt",
                     "game/longshot_env.txt", "summary.txt"):
            self.assertIn(want, names)
        self.assertFalse([n for n in names if "local-only" in n or "profile-" in n], names)
        self.assertIn("game folder built from", summary)

    def test_with_open_pull_requests(self):
        no_forge = mock.patch.object(update, "open_prs", lambda s: ([{"pr": 51, "title": "feat: fifty-one", "author": "a",
                                                                      "sha": "", "draft": False, "mergeable": True}], ""))
        with self.fake_build(), no_forge:
            update.cmd_update(args(game=self.game))
            a = self.pull(51, "open.txt", "open\n", "feat: fifty-one")
            self.pull(52, "other.txt", "other\n", "fix: fifty-two")
            update.cmd_update(args(with_="#52, 51"))
            self.assertEqual(self.tree_file("open.txt"), "open\n")
            self.assertEqual(self.tree_file("other.txt"), "other\n")
            got = json.loads(self.read(update.OPEN))
            self.assertEqual([(o["pr"], o["sha"]) for o in got][0], (51, a))
            self.assertEqual(got[0]["title"], "feat: fifty-one")
            self.assertEqual(self.read(update.MARK), git(self.up, "rev-parse", "main"))   # (main's commit; the PRs beside it)
            d = json.loads(self.output(lambda: update.cmd_update(args(list_json=True))))
            self.assertEqual(d["open_in_build"], [51, 52])
            self.assertTrue(d["open"][0]["in_build"])
            # (an update without --with: built again, without them)
            out = self.output(lambda: update.cmd_update(args()))
            self.assertIn("left out", out)
            self.assertIsNone(self.tree_file("open.txt"))
            self.assertFalse(os.path.exists(os.path.join(self.game, update.OPEN)))
        with open(update.part("history.md"), encoding="utf-8") as f:
            self.assertIn("open #51 feat: fifty-one", f.read())

    def test_with_and_pick(self):
        with self.fake_build(), mock.patch.object(update, "open_prs", lambda s: ([], "")):
            update.cmd_update(args(game=self.game))
            base = self.read(update.MARK)
            self.commit_file("a.txt", "a\n", "feat(x): a (#61)")
            self.commit_file("b.txt", "b\n", "fix(y): b (#62)")
            self.pull(63, "c.txt", "c\n", "perf: c", base=base)   # (off the folder's commit: it brings no main commit)
            update.cmd_update(args(pick="61", with_="63"))
            self.assertEqual(self.tree_file("a.txt"), "a\n")
            self.assertIsNone(self.tree_file("b.txt"))
            self.assertEqual(self.tree_file("c.txt"), "c\n")
            self.assertEqual(len(json.loads(self.read(update.PICKS))), 1)
            self.assertEqual([o["pr"] for o in json.loads(self.read(update.OPEN))], [63])

    def test_with_that_conflicts(self):
        with self.fake_build(), mock.patch.object(update, "open_prs", lambda s: ([], "")):
            update.cmd_update(args(game=self.game))
            self.pull(71, "f.txt", "theirs\n", "fix: f")   # (f.txt, which main changes too)
            self.commit_up("main's change")
            before = len(self.builds)
            with self.assertRaises(SystemExit) as e:
                update.cmd_update(args(with_="71"))
            self.assertIn("#71", str(e.exception.code))
            self.assertIn("f.txt", str(e.exception.code))
            self.assertEqual(len(self.builds), before)
            self.assertEqual(update.git("status", "--porcelain", cwd=update.part("tree")), "")
            with self.assertRaises(SystemExit):   # (not a pull request there)
                update.cmd_update(args(with_="999"))

    def test_search(self):
        merged = {"pr": 1874, "subject": "perf(vk): faster (#1874)", "short": "abc1234"}
        opened = {"pr": 1932, "title": "WIP: feat(dev): dev.py update", "author": "GummiNaomi"}
        self.assertTrue(update.matches(merged, "187"))
        self.assertTrue(update.matches(merged, "#1874"))
        self.assertFalse(update.matches(merged, "1932"))
        self.assertTrue(update.matches(opened, " 193 "))
        self.assertTrue(update.matches(opened, "dev.py"))
        self.assertTrue(update.matches(opened, "gumminaomi"))
        self.assertTrue(update.matches(merged, ""))
        self.assertFalse(update.matches({"pr": None, "subject": "x", "short": "1234567"}, "12"))

    def test_forge_api(self):
        for url, want in (("https://git.magicbean.dev/MagicBeanDev/Longshot.git",
                           "https://git.magicbean.dev/api/v1/repos/MagicBeanDev/Longshot"),
                          ("git@git.magicbean.dev:MagicBeanDev/Longshot.git",
                           "https://git.magicbean.dev/api/v1/repos/MagicBeanDev/Longshot"),
                          ("https://github.com/someone/Longshot", "https://api.github.com/repos/someone/Longshot"),
                          ("ssh://git@example.org:2222/o/r.git", "https://example.org/api/v1/repos/o/r")):
            with mock.patch.object(update, "git", lambda *a, **k: url):
                self.assertEqual(update.forge_api({"remote": "origin"}), want, url)

    def real_running_in(self, game):
        return REAL_RUNNING_IN(game)

    def output(self, f):
        import contextlib
        import io
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            f()
        return buf.getvalue()


REAL_RUNNING_IN = update.running_in

if __name__ == "__main__":
    unittest.main()
