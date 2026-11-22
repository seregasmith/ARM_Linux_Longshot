"""scripts/devtools/vcs.py: the development tools' commit, changed files and history questions, in a git checkout (a real one
made in a temporary folder) and in a jj workspace (its jj commands faked: the checks need no jj).

    python -I -B tests/test_dev_vcs.py
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
from devtools import vcs  # noqa: E402


def git(tree, *args):
    return subprocess.run(["git", "-C", tree, "-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgsign=false"]
                          + list(args), check=True, capture_output=True, text=True).stdout.strip()


def write(tree, name, text):
    with open(os.path.join(tree, name), "w", encoding="utf-8") as f:
        f.write(text)


@unittest.skipUnless(shutil.which("git"), "no git")
class Git(unittest.TestCase):
    def setUp(self):
        self.tree = tempfile.mkdtemp(prefix="longshot-vcs-")
        self.addCleanup(shutil.rmtree, self.tree, True)
        git(self.tree, "init", "-q")
        write(self.tree, ".gitignore", "ignored\n")
        write(self.tree, "a", "1")
        git(self.tree, "add", ".")
        git(self.tree, "commit", "-qm", "one")
        self.first = git(self.tree, "rev-parse", "HEAD")
        write(self.tree, "b", "2")
        git(self.tree, "add", "b")
        git(self.tree, "commit", "-qm", "two")

    def test_git_not_jj(self):
        self.assertFalse(vcs.is_jj(self.tree))

    def test_commit_is_head(self):
        self.assertEqual(vcs.commit(self.tree), (git(self.tree, "rev-parse", "HEAD"), "HEAD"))
        self.assertEqual(vcs.resolve(self.tree, "HEAD~1"), self.first)

    def test_changed_files_committed_uncommitted_and_new(self):
        write(self.tree, "a", "changed")
        write(self.tree, "new", "x")
        write(self.tree, "ignored", "x")
        self.assertEqual(vcs.changed_files(self.tree, self.first), {"a", "b", "new"})

    def test_clean_ignores_untracked(self):
        self.assertTrue(vcs.clean(self.tree))
        write(self.tree, "new", "x")
        self.assertTrue(vcs.clean(self.tree))
        write(self.tree, "a", "changed")
        self.assertFalse(vcs.clean(self.tree))

    def test_history(self):
        self.assertTrue(vcs.is_ancestor(self.tree, self.first))
        self.assertEqual(vcs.distance(self.tree, self.first), 1)
        git(self.tree, "checkout", "-q", self.first)
        self.assertFalse(vcs.is_ancestor(self.tree, vcs.resolve(self.tree, "@{-1}")))


class FakeJj:
    """Answers the jj commands vcs.py runs from a table of (argument tuple → output); anything else fails the test"""

    def __init__(self, answers):
        self.answers, self.calls = answers, []

    def __call__(self, cmd, **kw):
        self.calls.append(cmd)
        if cmd[0] != "jj":
            raise AssertionError("ran %r in a jj workspace" % cmd)
        for key, answer in self.answers.items():
            if all(k in cmd for k in key):
                return subprocess.CompletedProcess(cmd, 0 if answer is not None else 1, answer or "", "")
        raise AssertionError("unexpected %r" % cmd)


class Jj(unittest.TestCase):
    def setUp(self):
        top = tempfile.mkdtemp(prefix="longshot-vcs-jj-")
        self.addCleanup(shutil.rmtree, top, True)
        os.mkdir(os.path.join(top, ".jj"))
        self.tree = os.path.join(top, "deeper")
        os.mkdir(self.tree)

    def fake(self, answers):
        f = FakeJj(answers)
        p = mock.patch.object(vcs.subprocess, "run", f)
        p.start()
        self.addCleanup(p.stop)
        return f

    def test_jj_found_up_the_tree(self):
        self.assertTrue(vcs.is_jj(self.tree))

    def test_empty_working_copy_is_its_parent(self):
        self.fake({("diff", "--summary"): "", ("-r", "@-"): "p" * 40 + "\n"})
        self.assertEqual(vcs.commit(self.tree), ("p" * 40, "@-"))
        self.assertTrue(vcs.clean(self.tree))

    def test_working_copy_with_changes_is_itself(self):
        self.fake({("diff", "--summary"): "M a\n", ("-r", "@"): "w" * 40 + "\n"})
        self.assertEqual(vcs.commit(self.tree), ("w" * 40, "@"))
        self.assertFalse(vcs.clean(self.tree))

    def test_changed_files_against_the_working_copy(self):
        f = self.fake({("diff", "--name-only"): "a\nnew\n"})
        self.assertEqual(vcs.changed_files(self.tree, "b" * 40), {"a", "new"})
        self.assertIn(["--from", "b" * 40, "--to", "@"], [c[-4:] for c in f.calls])

    def test_history(self):
        self.fake({("b" * 40 + " & ::@",): "b" * 40, ("c" * 40 + " & ::@",): "", ("b" * 40 + "..@",): "x\ny\n"})
        self.assertTrue(vcs.is_ancestor(self.tree, "b" * 40))
        self.assertFalse(vcs.is_ancestor(self.tree, "c" * 40))
        self.assertEqual(vcs.distance(self.tree, "b" * 40), 2)

    def test_a_revision_that_is_not_one_commit_fails(self):
        self.fake({("-r", "nothing"): ""})
        with self.assertRaises(subprocess.CalledProcessError):
            vcs.resolve(self.tree, "nothing")


if __name__ == "__main__":
    unittest.main()
