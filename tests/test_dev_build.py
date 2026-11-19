"""scripts/dev_build.py on both systems, with no build, no network and no game: where its shared and per-checkout folders go
(Linux's as they were: ~/.cache, build-linux/; Windows' where dev_packs.py and the job queue keep theirs, %LOCALAPPDATA%\\Longshot,
and build-windows/, which git ignores), the build tools of tools.lock's lines for the system (the Windows builder's variables,
its Python), the renderer's name and the Windows check of its exports, the launcher's NGX SDK, the shell lines `env` prints,
and the prebuilt third_party files a Windows tree gets.

    python -I -B tests/test_dev_build.py
"""
import contextlib
import hashlib
import importlib.util
import io
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(ROOT, "scripts", "dev_build.py")


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


db = load("dev_build_under_test", SCRIPT)
tools = load("tools_under_test", os.path.join(ROOT, "scripts", "tools.py"))


def lock_key(system):
    with open(os.path.join(ROOT, "tools.lock"), encoding="utf-8") as f:
        lines = [l for l in f if l.startswith("tool ") and " %s " % system in l]
    return hashlib.sha1("".join(lines).encode()).hexdigest()[:12]


class OnSystem(unittest.TestCase):
    windows = False

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="dev_build_test_")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        p = mock.patch.object(db, "WINDOWS", self.windows)
        p.start()
        self.addCleanup(p.stop)
        env = {k: v for k, v in os.environ.items() if not k.startswith(("LONGSHOT_", "XDG_"))}
        env.update(XDG_CACHE_HOME=os.path.join(self.tmp, "xdg"), LOCALAPPDATA=os.path.join(self.tmp, "local"),
                   APPDATA=os.path.join(self.tmp, "roaming"))
        e = mock.patch.dict(os.environ, env, clear=True)
        e.start()
        self.addCleanup(e.stop)


class Linux(OnSystem):
    windows = False

    def test_folders_as_they_were(self):
        x = os.path.join(self.tmp, "xdg")
        self.assertEqual(db.home(), os.path.join(x, "longshot-dev"))
        self.assertEqual(db.cache("longshot-objects"), os.path.join(x, "longshot-objects"))
        self.assertEqual(db.cache("longshot-lifts"), os.path.join(x, "longshot-lifts"))
        os.environ["LONGSHOT_DEV_HOME"] = "rel/home"   # (taken as it is, as before)
        self.assertEqual(db.home(), "rel/home")
        self.assertEqual(db.own_dir(), os.path.join(db.ROOT, "build-linux"))
        self.assertEqual(db.renderer_name(), "longshot_vk.so")
        self.assertEqual(db.game_roots(), [os.path.expanduser("~/Games")])

    def test_tools_lock_linux_lines(self):
        self.assertEqual(db.system(), "linux-x64")
        self.assertEqual(db.tools_lock_key(), lock_key("linux-x64"))
        self.assertEqual(db.builder_dir(), os.path.join(db.home(), "builder", lock_key("linux-x64")))

    def test_python_from_build_linux_deps(self):
        c = db.python_candidates()
        self.assertEqual(c[0], os.path.join(db.ROOT, "build-linux", "deps", "python", "bin", "python3"))
        self.assertTrue(all(p.endswith(os.path.join("build-linux", "deps", "python", "bin", "python3")) for p in c))

    def test_ngx_and_shell(self):
        ngx, lib = db.launcher_ngx_sdk()
        self.assertEqual(ngx, os.path.expanduser("~/.local/share/longshot/launcher/ngx-sdk"))
        self.assertEqual(lib, "libnvsdk_ngx.a")
        self.assertEqual(db.shell_line("A", "b c'd"), "export A='b c'\"'\"'d'")

    def test_vk_takes_the_exported_jobs(self):
        """An exported LONGSHOT_JOBS goes through environment() to the renderer's --jobs and to the build's environment (the
        game's build reads it there), while the queue's own LONGSHOT_JOB, LONGSHOT_JOB_* and an older queue's OOT_STATUS_*
        are still dropped: a job that kept an outer job's LONGSHOT_JOB would take itself for that job"""
        ran = []

        class Queued(Exception):
            pass

        def queued(name, mode, cmd, env, cwd=None):
            ran.append((cmd, env))
            raise Queued

        os.environ.update(LONGSHOT_JOB="4242", LONGSHOT_JOB_NAME="outer", LONGSHOT_JOB_MODE="build",
                          OOT_STATUS_LOCK="older")
        for jobs in ("1", "14"):
            os.environ["LONGSHOT_JOBS"] = jobs
            with mock.patch.object(db, "own_dir", return_value=self.tmp), mock.patch.object(db, "not_on_ram"), \
                    mock.patch.object(db, "config", return_value={}), mock.patch.object(db, "toolchain", return_value={}), \
                    mock.patch.object(db, "find_python", return_value="/the/python"), \
                    mock.patch.object(db, "queued", queued), \
                    mock.patch.object(db.os, "cpu_count", return_value=16), self.assertRaises(Queued):
                db.cmd_vk(mock.Mock(copy_to=None))
            cmd, env = ran[-1]
            self.assertEqual(cmd[-2:], ["--jobs", jobs])
            self.assertEqual(env.get("LONGSHOT_JOBS"), jobs)
            for name in ("LONGSHOT_JOB", "LONGSHOT_JOB_NAME", "LONGSHOT_JOB_MODE", "OOT_STATUS_LOCK"):
                self.assertNotIn(name, env)


class Windows(OnSystem):
    windows = True

    def test_folders_beside_dev_packs(self):
        local = os.path.join(self.tmp, "local")
        self.assertEqual(db.home(), os.path.normpath(os.path.join(local, "Longshot", "dev")))
        # (dev_packs.py object_store / lift_store on Windows, and the queue's %LOCALAPPDATA%\Longshot\queue)
        self.assertEqual(db.cache("longshot-objects"), os.path.join(local, "Longshot", "objects"))
        self.assertEqual(db.cache("longshot-lifts"), os.path.join(local, "Longshot", "lifts"))
        with open(os.path.join(ROOT, "scripts", "dev_packs.py"), encoding="utf-8") as f:
            packs = f.read()
        for name in ('"Longshot", "objects"', '"Longshot", "lifts"'):
            self.assertIn(name, packs)
        self.assertEqual(db.own_dir(), os.path.join(db.ROOT, "build-windows"))
        self.assertEqual(db.renderer_name(), "longshot_vk.dll")
        self.assertIn(os.path.expanduser(os.path.join("~", "Games", "Longshot")), db.game_roots())

    def test_build_windows_ignored(self):
        with open(os.path.join(ROOT, ".gitignore"), encoding="utf-8") as f:
            self.assertIn("build-windows/", f.read().split())

    def test_tools_lock_windows_lines(self):
        self.assertEqual(db.system(), "windows-x64")
        self.assertEqual(db.tools_lock_key(), lock_key("windows-x64"))
        self.assertNotEqual(db.tools_lock_key(), lock_key("linux-x64"))
        dirs = db.windows_tool_dirs()
        for t in tools.of_system(tools.load(), "windows-x64"):
            self.assertEqual(dirs[t.name], os.path.join(db.builder_dir(), *t.dest.split("/")))
        # (every variable the Windows builder sets comes from a tool its setup requires)
        names = [n for n, _ in db.WINDOWS_TOOL_ENV]
        self.assertEqual([v for _, v in db.WINDOWS_TOOL_ENV], ["LONGSHOT_MINGW", "RX_CPM"])
        self.assertTrue(set(names + ["python", "third-party"]) <= set(tools.REQUIRED["windows-x64"]))
        with open(os.path.join(ROOT, "builder", "LongshotBuilder", "Runner.cs"), encoding="utf-8") as f:
            runner = f.read()
        self.assertIn('var sets = new[] { ("RX_CPM", "cpm") };', runner)

    def test_windows_toolchain_env(self):
        dirs = db.windows_tool_dirs()
        for d in dirs.values():
            os.makedirs(d)
        with mock.patch.object(tools, "install") as inst, mock.patch.dict(sys.modules, tools=tools):
            env, got = db.windows_toolchain()
        inst.assert_called_once()
        self.assertEqual(env, {"LONGSHOT_MINGW": dirs["llvm-mingw"], "RX_CPM": dirs["cpm"]})
        self.assertEqual(got, dirs)
        self.assertEqual(db.python_candidates(dirs["python"]), [os.path.join(dirs["python"], "python.exe")])
        self.assertEqual(db.python_candidates(None), [])

    def test_deps_folder_is_linux_only(self):
        os.environ["LONGSHOT_DEV_DEPS"] = self.tmp
        with self.assertRaises(SystemExit) as e:
            db.environment()
        self.assertIn("Linux", str(e.exception))

    def test_ngx_and_shell(self):
        ngx, lib = db.launcher_ngx_sdk()
        self.assertEqual(ngx, os.path.join(self.tmp, "local", "LongshotLauncher", "ngx-sdk"))
        with open(os.path.join(ROOT, "nx2", "build.py"), encoding="utf-8") as f:
            self.assertIn('"libnvsdk_ngx.a" if linux else "%s"' % lib, f.read())
        self.assertEqual(db.shell_line("A", r"C:\x y\it's"), r"$env:A = 'C:\x y\it''s'")

    def test_prebuilt_into_the_tree(self):
        src = db.windows_tool_dirs()["third-party"]
        for rel in db.WINDOWS_PREBUILT:
            p = os.path.join(src, *rel.split("/"))
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "wb") as f:
                f.write(b"MZ " + rel.encode())
        tree = os.path.join(self.tmp, "tree")
        with contextlib.redirect_stdout(io.StringIO()):
            db.windows_prebuilt(tree)
        for rel in db.WINDOWS_PREBUILT:
            with open(os.path.join(tree, "third_party", *rel.split("/")), "rb") as f:
                self.assertEqual(f.read(), b"MZ " + rel.encode())
            self.assertTrue(rel.endswith(".dll"))   # (git ignores *.dll: a pack or a base sees no change)
        # (the names the third-party archive holds: scripts/tools.py assemble third-party)
        with open(os.path.join(ROOT, "scripts", "tools.py"), encoding="utf-8") as f:
            text = f.read()
        for rel in db.WINDOWS_PREBUILT:
            self.assertIn(os.path.basename(rel), text)

    def test_dll_exports(self):
        text = ("\nThe Data Directory\n\nExport Table:\n DLL name: longshot_vk.dll\n Ordinal base: 1\n"
                " Ordinal      RVA  Name\n       1 0x27cf70  mkwvk_close_asked\n       2 0x27cff0  mkwvk_init\n\n"
                "The Import Tables:\n  lookup 0x1 time 0x0 fwd 0x0 name 0x2 addr 0x3\n")
        self.assertEqual(db.dll_exports(text), {"mkwvk_close_asked", "mkwvk_init"})
        self.assertEqual(db.dll_exports(text.replace("mkwvk_init", "other")), {"mkwvk_close_asked", "other"})
        with open(os.path.join(ROOT, "host", "rx_backend.c"), encoding="utf-8") as f:
            host = f.read()
        for name in db.RENDERER_EXPORTS:
            self.assertIn('GetProcAddress(m, "%s")' % name, host)


class Both(unittest.TestCase):
    def test_no_refusal(self):
        with open(SCRIPT, encoding="utf-8") as f:
            self.assertNotIn("Linux only", f.read())

    def test_base_lock(self):
        d = tempfile.mkdtemp(prefix="dev_build_lock_")
        self.addCleanup(shutil.rmtree, d, True)
        with db.BaseLock(os.path.join(d, "bases", "abc")) as l:
            self.assertTrue(l.path.endswith(os.path.join("bases", "abc.lock")))
        with db.BaseLock(os.path.join(d, "bases", "abc") + os.sep):   # (again, after it was let go)
            pass

    def test_vk_compiles_several_files(self):
        """dev_build.py vk builds the renderer as a pack of it does: nx2/build.py run by itself compiles one file at a time, so
        the command carries --jobs (LONGSHOT_JOBS, else one for every four CPU threads), run by the build's own Python"""
        ran = []

        class Queued(Exception):
            pass

        def queued(name, mode, cmd, env, cwd=None):
            ran.append((mode, cmd))
            raise Queued

        d = tempfile.mkdtemp(prefix="dev_build_vk_")
        self.addCleanup(shutil.rmtree, d, True)
        for env, cpus, jobs in (({"LONGSHOT_JOBS": "6"}, 16, "6"), ({}, 16, "4"), ({}, 2, "1")):
            with mock.patch.object(db, "own_dir", return_value=d), mock.patch.object(db, "not_on_ram"), \
                    mock.patch.object(db, "queued", queued), \
                    mock.patch.object(db, "environment", return_value=("/the/python", dict(env))), \
                    mock.patch.object(db.os, "cpu_count", return_value=cpus), self.assertRaises(Queued):
                db.cmd_vk(mock.Mock(copy_to=None))
            mode, cmd = ran[-1]
            self.assertEqual(mode, "light")
            self.assertEqual(cmd[:4], ["/the/python", "-I", "nx2/build.py", "--standalone"])
            self.assertEqual(cmd[-2:], ["--jobs", jobs])


if __name__ == "__main__":
    unittest.main()
