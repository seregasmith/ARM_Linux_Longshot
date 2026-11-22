"""`dev.py migrate` and `dev.py paths` (scripts/devtools/home.py) on made-up folders, no build and no game: the old places
(~/.cache/longshot-{dev,objects,lifts,queue}, job_queue.json and dev_build.json of ~/.config/longshot) move into the
development folder, by a rename or, across drives, by a checked copy that keeps hard links; a link is left at each old
place (Linux), base.json names the new folders, and the markers it held are gone. Tooling's files beside the settings stay.
A busy base or a job in the queue stops it before anything moves; a part the development folder has already is merged,
and a name both have stays where it was. A second run moves nothing.

    python -I -B tests/test_dev_migrate.py
"""
import argparse
import contextlib
import errno
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
from devtools import common, home, jobqueue, packs  # noqa: E402

LINKS = os.name != "nt"   # (Windows: no symbolic link without a privilege; migrate leaves a junction there instead)


def write(path, text=""):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


class Migrate(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="dev_migrate_test_")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        t = self.tmp
        env = {k: v for k, v in os.environ.items() if not k.startswith(("LONGSHOT_", "XDG_"))}
        env.update(XDG_CACHE_HOME=os.path.join(t, "cache"), XDG_CONFIG_HOME=os.path.join(t, "config"),
                   XDG_DATA_HOME=os.path.join(t, "data"),
                   LONGSHOT_QUEUE_DIR=os.path.join(t, "run", "longshot-queue"))
        for p in (mock.patch.dict(os.environ, env, clear=True), mock.patch.object(common, "WINDOWS", False),
                  mock.patch.object(common, "on_ram", return_value=False)):   # (the temporary folder may be a tmpfs)
            p.start()
            self.addCleanup(p.stop)
        self.home = os.path.join(t, "data", "longshot-dev")
        self.old = {"dev": os.path.join(t, "cache", "longshot-dev"), "objects": os.path.join(t, "cache", "longshot-objects"),
                    "lifts": os.path.join(t, "cache", "longshot-lifts"), "queue": os.path.join(t, "cache", "longshot-queue"),
                    "config": os.path.join(t, "config", "longshot")}
        base = os.path.join(self.old["dev"], "bases", "0123456789ab")
        write(os.path.join(base, "base.json"), json.dumps({"commit": "0123456789ab", "work": os.path.join(base, "work"),
                                                           "game": os.path.join(base, "game"), "romfs": "/games/romfs"}))
        write(os.path.join(base, "game", "longshot"), "game")
        write(os.path.join(self.old["dev"], "bases", "0123456789ab.lock"))
        write(os.path.join(self.old["dev"], "builder", "k", "clang"), "tool")
        write(os.path.join(self.old["objects"], "ab", "obj.o"), "object")
        os.makedirs(os.path.join(base, "work", "objcache"))
        os.link(os.path.join(self.old["objects"], "ab", "obj.o"), os.path.join(base, "work", "objcache", "obj.o"))
        write(os.path.join(self.old["lifts"], "lift", "a.c"), "lifted")
        write(os.path.join(self.old["queue"], "history.jsonl"), "{}\n")
        write(os.path.join(self.old["config"], "job_queue.json"), '{"LONGSHOT_QUEUE_BUILD_SLOTS": 3}')
        write(os.path.join(self.old["config"], "dev_build.json"), '{"fsr": true}')
        write(os.path.join(self.old["config"], "forge.json"), "token")   # (Tooling's sign-in: never moved)
        self.base = base

    def migrate(self, dry_run=False):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = home.cmd_migrate(argparse.Namespace(dry_run=dry_run))
        return rc, out.getvalue()

    def check_moved(self):
        h, nb = self.home, os.path.join(self.home, "bases", "0123456789ab")
        self.assertEqual(read(os.path.join(nb, "game", "longshot")), "game")
        self.assertEqual(read(os.path.join(h, "builder", "k", "clang")), "tool")
        self.assertEqual(read(os.path.join(h, "stores", "lifts", "lift", "a.c")), "lifted")
        self.assertEqual(read(os.path.join(h, "queue", "history.jsonl")), "{}\n")
        self.assertEqual(json.loads(read(os.path.join(h, "config", "dev_build.json"))), {"fsr": True})
        self.assertTrue(os.path.samefile(os.path.join(h, "stores", "objects", "ab", "obj.o"),
                                         os.path.join(nb, "work", "objcache", "obj.o")))
        b = json.loads(read(os.path.join(nb, "base.json")))
        self.assertEqual((b["work"], b["game"], b["romfs"]), (os.path.join(nb, "work"), os.path.join(nb, "game"), "/games/romfs"))
        self.assertFalse(os.path.exists(os.path.join(nb, packs.BUSY)))
        self.assertEqual(read(os.path.join(self.old["config"], "forge.json")), "token")
        self.assertFalse(os.path.islink(self.old["config"]))
        if LINKS:
            for name, new in (("dev", h), ("objects", os.path.join(h, "stores", "objects")),
                              ("lifts", os.path.join(h, "stores", "lifts")), ("queue", os.path.join(h, "queue"))):
                self.assertTrue(os.path.islink(self.old[name]), name)
                self.assertTrue(os.path.samefile(self.old[name], new), name)
            self.assertTrue(os.path.samefile(os.path.join(self.old["config"], "job_queue.json"),
                                             os.path.join(h, "config", "job_queue.json")))
        # (what the tools use now, and nothing left to move)
        self.assertEqual(packs.lift_store(), os.path.join(h, "stores", "lifts"))
        self.assertEqual(jobqueue.settings()["LONGSHOT_QUEUE_BUILD_SLOTS"], "3")
        self.assertEqual(home.plan(jobqueue.queue_dir(jobqueue.settings())), [])
        self.assertIn("nothing to move", self.migrate()[1])

    def test_dry_run_moves_nothing(self):
        rc, said = self.migrate(dry_run=True)
        self.assertEqual(rc, 0)
        self.assertIn(self.old["objects"] + " -> " + os.path.join(self.home, "stores", "objects"), said)
        self.assertFalse(os.path.exists(self.home))
        self.assertEqual(packs.lift_store(), self.old["lifts"])   # (read where it is until then)

    def test_rename(self):
        rc, said = self.migrate()
        self.assertEqual(rc, 0, said)
        self.check_moved()

    def test_across_drives(self):
        """Every move is a copy, as from one drive to another: checked, hard links kept, the old one deleted"""
        rename = os.rename

        def other_drive(src, dst):
            if not src.endswith(".part"):
                raise OSError(errno.EXDEV, "Invalid cross-device link")
            rename(src, dst)
        with mock.patch.object(home.os, "rename", other_drive):
            rc, said = self.migrate()
        self.assertEqual(rc, 0, said)
        self.check_moved()

    @unittest.skipUnless(LINKS, "hard links in the temporary folder")
    def test_hard_linked_files_across_drives(self):
        """Two top-level files hard-linked to each other, moved one after the other across drives: the second becomes a link to
        the first's new path, not to the .part it was copied to (gone after its rename)"""
        a, b = os.path.join(self.tmp, "old", "a.json"), os.path.join(self.tmp, "old", "b.json")
        write(a, "same")
        os.link(a, b)
        rename = os.rename

        def other_drive(src, dst):
            if not src.endswith(".part"):
                raise OSError(errno.EXDEV, "Invalid cross-device link")
            rename(src, dst)
        new_a, new_b = os.path.join(self.tmp, "new", "a.json"), os.path.join(self.tmp, "new", "b.json")
        seen = {}
        with mock.patch.object(home.os, "rename", other_drive):
            self.assertEqual(home.move(a, new_a, seen), [])
            self.assertEqual(home.move(b, new_b, seen), [])
        self.assertEqual(read(new_b), "same")
        self.assertTrue(os.path.samefile(new_a, new_b))
        self.assertFalse(os.path.lexists(a) or os.path.lexists(b))

    def test_merge_into_a_development_folder_already_there(self):
        write(os.path.join(self.home, "stores", "lifts", "lift", "a.c"), "newer")
        write(os.path.join(self.old["lifts"], "lift", "b.c"), "b")
        rc, said = self.migrate()
        self.assertEqual(rc, 0, said)
        self.assertEqual(read(os.path.join(self.home, "stores", "lifts", "lift", "a.c")), "newer")
        self.assertEqual(read(os.path.join(self.home, "stores", "lifts", "lift", "b.c")), "b")
        self.assertEqual(read(os.path.join(self.old["lifts"], "lift", "a.c")), "lifted")   # (both had it: left, said)
        self.assertIn("left " + os.path.join(self.old["lifts"], "lift", "a.c"), said)
        self.assertFalse(os.path.islink(self.old["lifts"]))
        self.assertTrue(os.path.isdir(os.path.join(self.home, "bases", "0123456789ab")))

    def test_busy_base_stops_it(self):
        write(os.path.join(self.base, packs.BUSY), "4242 pack 2026")
        with self.assertRaises(SystemExit) as e:
            self.migrate()
        self.assertIn("busy", str(e.exception))
        self.assertFalse(os.path.exists(self.home))
        self.assertEqual(read(os.path.join(self.base, packs.BUSY)), "4242 pack 2026")

    def test_job_in_the_queue_stops_it(self):
        with mock.patch.object(jobqueue, "read_tickets", return_value=[{"name": "base-0123"}]):
            with self.assertRaises(SystemExit) as e:
                self.migrate()
        self.assertIn("base-0123", str(e.exception))
        self.assertFalse(os.path.exists(self.home))

    def test_paths(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            home.cmd_paths(argparse.Namespace(name="work"))
            home.cmd_paths(argparse.Namespace(name="stores/lifts"))
            home.cmd_paths(argparse.Namespace(name=None))
        lines = out.getvalue().splitlines()
        self.assertEqual(lines[:2], [os.path.join(self.home, "work"), self.old["lifts"]])
        self.assertIn("dev.py migrate moves", out.getvalue())
        with mock.patch.dict(os.environ, RX_TARGET="windows"):
            self.assertEqual(home.work_dir(), os.path.join(self.home, "work-windows"))


if __name__ == "__main__":
    unittest.main()
