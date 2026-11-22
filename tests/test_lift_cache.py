"""The lift is skipped only when the game, the tools and the settings are the ones it was made from.

python -I -B tests/test_lift_cache.py
"""
import ast
import importlib.util
import os
import tempfile
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("make_game_under_test", ROOT / "nx2" / "make_game.py")
make_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_game)


def check(what, ok):
    if not ok:
        raise SystemExit("FAIL: " + what)


def main():
    with tempfile.TemporaryDirectory(prefix="longshot-lift-key-") as d:
        d = Path(d)
        exefs, work = d / "exefs", d / "work"
        (exefs).mkdir()
        (work / "image").mkdir(parents=True)
        (exefs / "main").write_bytes(b"NSO0 one")
        (work / "hot_funcs.txt").write_text("f_80000000\n")
        (work / "image" / "chunks.txt").write_text("80001000\n")
        env = {"A64_HOT_CHUNK": "1", "OOT_WORK": str(work), "A64_HOT_FUNCS": str(work / "hot_funcs.txt")}
        with mock.patch.dict(os.environ, {k: v for k, v in os.environ.items() if not k.startswith(("A64_", "OOT_", "RX_"))}, clear=True):
            key = make_game.lift_key(str(exefs), str(work), env)
            check("the same inputs give the same key", key == make_game.lift_key(str(exefs), str(work), dict(env)))
            (exefs / "main").write_bytes(b"NSO0 two")
            check("another game module changes the key", make_game.lift_key(str(exefs), str(work), env) != key)
            (exefs / "main").write_bytes(b"NSO0 one")
            (work / "hot_funcs.txt").write_text("f_80000004\n")
            check("another hot list changes the key", make_game.lift_key(str(exefs), str(work), env) != key)
            (work / "hot_funcs.txt").write_text("f_80000000\n")
            (work / "image" / "chunks.txt").write_text("80002000\n")
            check("other chunk boundaries change the key", make_game.lift_key(str(exefs), str(work), env) != key)
            (work / "image" / "chunks.txt").write_text("80001000\n")
            (work / "image" / "hot_pieces.txt").write_text("f_80000000 1\n")   # (split_hot's kept pieces: the pieces' C follows them)
            check("other kept hot pieces change the key", make_game.lift_key(str(exefs), str(work), env) != key)
            (work / "image" / "hot_pieces.txt").unlink()
            check("another lift setting changes the key", make_game.lift_key(str(exefs), str(work), dict(env, A64_HOT_CHUNK="")) != key)
            with mock.patch.dict(os.environ, {"A64_KEEP_FLAGS": "1"}):
                check("a lifter switch in the environment changes the key", make_game.lift_key(str(exefs), str(work), env) != key)
            for name in make_game.COMPILE_ONLY_ENV:   # (the compile's switches: a build at another hot-code level keeps the lift)
                check("%s for the compile does not change the key" % name,
                      make_game.lift_key(str(exefs), str(work), dict(env, **{name: "O1"})) == key)
                with mock.patch.dict(os.environ, {name: "O1"}):
                    check("%s in the environment does not change the key" % name, make_game.lift_key(str(exefs), str(work), env) == key)
            real_open = open
            changed = str(ROOT / "nx2" / "lift.py")

            def edited(path, *a, **k):   # (the translator as if one of its files had changed)
                f = real_open(path, *a, **k)
                if os.path.abspath(path) == changed:
                    f.close()
                    import io
                    return io.BytesIO(b"# an edit\n")
                return f
            with mock.patch("builtins.open", edited):
                check("a change to the translator changes the key", make_game.lift_key(str(exefs), str(work), env) != key)
            for name in ("nx2/whole_write.py", "nx2/oot_patches.py",
                         "third_party/a64-kit/lift/a64_lift.py", "third_party/a64-kit/lift/a64emit.cpp"):
                changed = str(ROOT / name)   # (imported by an import, in a function, or compiled when the lift starts)
                with mock.patch("builtins.open", edited):
                    check("a change to %s changes the key" % name, make_game.lift_key(str(exefs), str(work), env) != key)
            for name in ("nx2/build.py", "nx2/make_game.py", "nx2/pipeline_states.py", "nx2/prompt_assets.py", "nx2/shader_census.py"):
                changed = str(ROOT / name)   # (the builder's other steps: not the lift's)
                with mock.patch("builtins.open", edited):
                    check("a change to %s leaves the key" % name, make_game.lift_key(str(exefs), str(work), env) == key)
            check("the inputs back as they were give the key again", make_game.lift_key(str(exefs), str(work), env) == key)
            import standalone
            names = standalone.implemented()
            with mock.patch.object(standalone, "implemented", lambda host=None: names | {"imp_new_hle"}):
                check("a new HLE function in host/ changes the key", make_game.lift_key(str(exefs), str(work), env) != key)
            host_c = str(ROOT / "host" / "hos_main.c")

            def host_edited(path, *a, **k):   # (host/ code changed, its HLE functions the same)
                if os.path.abspath(path) == host_c:
                    import io
                    return io.StringIO("/* an edit */\n")
                return real_open(path, *a, **k)
            with mock.patch("builtins.open", host_edited):
                check("other host/ code does not change the key", make_game.lift_key(str(exefs), str(work), env) == key)

        gen, image = work / "sa" / "gen", work / "sa" / "image"
        check("no lift yet: not lifted before", not make_game.lifted_before(str(work), key))
        gen.mkdir(parents=True)
        image.mkdir(parents=True)
        make_game.write_lift_key(str(work), key)
        check("a key without the lift's files is not trusted", not make_game.lifted_before(str(work), key))
        (gen / "a64_table.c").write_text("")
        (image / "image.bin").write_bytes(b"")
        check("a key written before the lift's files is not trusted", not make_game.lifted_before(str(work), key))
        make_game.write_lift_key(str(work), key)
        check("the finished lift with this key is reused", make_game.lifted_before(str(work), key))
        (gen / "code_000.c").write_text("/* another builder's lift */")
        check("C changed by another builder's lift is not", not make_game.lifted_before(str(work), key))
        (gen / "code_000.c").unlink()
        check("a key in the old one-line form is not", (gen / make_game.LIFT_KEY).write_text(key + "\n") and
              not make_game.lifted_before(str(work), key))
        make_game.write_lift_key(str(work), key)
        check("a finished lift with another key is not", not make_game.lifted_before(str(work), "0" * 40))
    check_lift_sources()
    check_compile_only()
    check_code_prints()
    check_key_after_lift()
    check_lift_env()
    check_hot_list_override()
    check_selected_hot_list()
    print("ok: the lift key follows the game, the tools (the modules the lift imports), the hot list, the chunks and the settings")


def check_selected_hot_list():
    """Only the selected profile's contents affect translation, including a path inherited from the shell."""
    with tempfile.TemporaryDirectory(prefix="longshot-selected-hot-list-") as folder:
        root = Path(folder)
        exefs, work = root / "exefs", root / "work"
        exefs.mkdir()
        work.mkdir()
        default = work / "hot_funcs.txt"
        selected = root / "external hot list.txt"
        default.write_text("f_80000000\n")
        selected.write_text("f_80000004\n")
        env = {"A64_HOT_FUNCS": str(selected)}
        clean = {k: v for k, v in os.environ.items() if not k.startswith(("A64_", "OOT_", "RX_"))}
        with mock.patch.dict(os.environ, clean, clear=True):
            key = make_game.lift_key(str(exefs), str(work), env)
            inputs = make_game.lift_key(str(exefs), str(work), env, planning=False)
            selected.write_text("f_80000008\n")
            check("editing the same external profile path changes the key",
                  make_game.lift_key(str(exefs), str(work), env) != key)
            check("editing the external profile also invalidates the in-flight input guard",
                  make_game.lift_key(str(exefs), str(work), env, planning=False) != inputs)
            selected.write_text("f_80000004\n")
            default.write_text("f_8000000c\n")
            check("an unselected work profile does not change the key",
                  make_game.lift_key(str(exefs), str(work), env) == key)
            with mock.patch.dict(os.environ, {"A64_HOT_FUNCS": str(selected)}):
                inherited = make_game.lift_key(str(exefs), str(work), {})
                selected.write_text("f_80000008\n")
                check("an inherited external profile's contents change the key",
                      make_game.lift_key(str(exefs), str(work), {}) != inherited)
                disabled = make_game.lift_key(str(exefs), str(work), {"A64_HOT_FUNCS": ""})
                selected.write_text("f_80000010\n")
                default.write_text("f_80000014\n")
                check("an explicit empty child setting hashes neither profile",
                      make_game.lift_key(str(exefs), str(work), {"A64_HOT_FUNCS": ""}) == disabled)
            with mock.patch.object(make_game, "ROOT", str(root)), mock.patch.object(make_game, "lift_sources", return_value=[]):
                relative = {"A64_HOT_FUNCS": selected.name}
                key = make_game.lift_key(str(exefs), str(work), relative)
                selected.write_text("f_80000018\n")
                check("a relative profile is hashed from the child's working directory",
                      make_game.lift_key(str(exefs), str(work), relative) != key)


def check_key_after_lift():
    """A fresh work folder has no image/hot_pieces.txt until its first lift's split_hot writes one, and the key hashes it: the key
    the lift is stored under is taken after the lift, from what the next build reads, or that build translates the game again
    (a no-op rebuild of a new work folder: 319 s on Windows, 290 s of it the lift). make_game.py's translate(), read as text"""
    src = (ROOT / "nx2" / "make_game.py").read_text(encoding="utf-8")
    translate, = (node for node in ast.walk(ast.parse(src))
                  if isinstance(node, ast.FunctionDef) and node.name == "translate")
    body = ast.get_source_segment(src, translate)
    split = body.index('"nx2/split_hot.py"')
    again = body.find("after = lift_key(exefs, work, env)", split)
    write = body.find("write_lift_key(work, key)", split)
    check("the key is taken again after the lift and split_hot, then written", split < again < write)
    check("translate() sets the build's key, which keep_translation uses after the build", "nonlocal key" in body)
    start = body.find("inputs = lift_key(exefs, work, env, planning=False)")
    lift = body.index('"nx2/lift.py"')
    guard = body.find("if lift_key(exefs, work, env, planning=False) == inputs:", split)
    taken = body.find("key = after", guard)
    check("what the lift reads and does not write is taken as it starts", 0 <= start < lift)
    check("the key taken after the lift is kept only if that did not change while it ran", split < guard < taken < write)
    check("the key is taken before that comparison, so an edit between the two is caught by it", again < guard)
    with tempfile.TemporaryDirectory(prefix="longshot-lift-key-after-") as d:
        d = Path(d)
        exefs, work = d / "exefs", d / "work"
        exefs.mkdir()
        (work / "image").mkdir(parents=True)
        (exefs / "main").write_bytes(b"NSO0 one")
        env = {"A64_HOT_CHUNK": "1", "A64_HOT_FUNCS": str(work / "hot_funcs.txt")}
        with mock.patch.dict(os.environ, {k: v for k, v in os.environ.items() if not k.startswith(("A64_", "OOT_", "RX_"))}, clear=True):
            before = make_game.lift_key(str(exefs), str(work), env)
            (work / "image" / "hot_pieces.txt").write_text("f_80000000 1\n")   # (what the first lift's split_hot leaves)
            after = make_game.lift_key(str(exefs), str(work), env)
            check("a fresh folder's key before its first lift is not the next build's", before != after)
            check("the key after the lift is the next build's", after == make_game.lift_key(str(exefs), str(work), dict(env)))
            inputs = make_game.lift_key(str(exefs), str(work), env, planning=False)
            (work / "image" / "hot_pieces.txt").write_text("f_80000004 1\n")
            (work / "image" / "chunks.txt").write_text("80003000\n")
            check("the lift's own files are not among the other inputs", make_game.lift_key(str(exefs), str(work), env, planning=False)
                  == inputs)
            (exefs / "main").write_bytes(b"NSO0 two")
            check("the game's code is", make_game.lift_key(str(exefs), str(work), env, planning=False) != inputs)
            (exefs / "main").write_bytes(b"NSO0 one")
            (work / "hot_funcs.txt").write_text("f_80000004\n")
            check("the hot list is", make_game.lift_key(str(exefs), str(work), env, planning=False) != inputs)


def check_lift_sources():
    """Every module of nx2/ and a64-kit that loading the lift's two scripts really imports is one lift_sources() hashes (its
    import walk reads the source; this imports them, capstone, lz4 and numpy stood in where they are not installed)"""
    import subprocess
    import sys
    code = ("import importlib, os, sys, types\n"
            "class Stub(types.ModuleType):\n    def __getattr__(self, n):\n"
            "        if n.startswith('__'):\n            raise AttributeError(n)\n        return 0\n"
            "for name in ('capstone', 'lz4', 'lz4.block', 'numpy'):\n"
            "    try:\n        importlib.import_module(name)\n    except ImportError:\n"
            "        sys.modules[name] = Stub(name)\n"
            "sys.path.insert(0, sys.argv[1])\nsys.argv[1:] = []\n"
            "import lift, split_hot\n"
            "print('\\n'.join(os.path.abspath(m.__file__) for m in list(sys.modules.values()) if getattr(m, '__file__', None)))\n")
    run = subprocess.run([sys.executable, "-I", "-B", "-c", code, str(ROOT / "nx2")], capture_output=True, text=True)
    check("the lift's scripts import (%s)" % run.stderr.strip()[-2000:], run.returncode == 0)
    out = run.stdout
    tops = (str(ROOT / "nx2") + os.sep, str(ROOT / "third_party" / "a64-kit") + os.sep)
    loaded = {f for f in out.splitlines() if f.startswith(tops)}   # (lines: a path may hold a space)
    check("the lift loads its modules (%d)" % len(loaded), len(loaded) > 12 and str(ROOT / "nx2" / "chunks.py") in loaded)
    sources = {os.path.abspath(f) for f in make_game.lift_sources()}
    missing = sorted(loaded - sources)
    check("every module the lift loads is in its key (missing: %s)" % missing, not missing)
    check("the lift's later imports too", {str(ROOT / "nx2" / f) for f in ("oot_patches.py", "oot_wide.py", "vecfp.py")}
          <= sources)
    check("the builder's other modules are not", str(ROOT / "nx2" / "build.py") not in sources)
    import re
    dynamic = [f for f in sorted(sources) if f.endswith(".py") and re.search(r"\b__import__\(|\bimport_module\(|\bimportlib\b",
                                                                            open(f, encoding="utf-8", errors="replace").read())]
    check("no module of the lift imports by a name made at run time, which lift_sources' walk cannot see (%s)" % dynamic, not dynamic)



def check_lift_env():
    """The key hashes the variables whose values change the lifted C (make_game.py LIFT_ENV_PREFIXES, LIFT_ENV_NAMES), not the
    paths of what it reads by content (LIFT_PATH_ENV), nor the renderer's switches or RX_RELEASE, which the lift does not read;
    and every variable the lift's sources read is one of these, or in LIFT_ENV_NOT_OUTPUT with why it changes nothing.
    The scan for those reads sees a name written out where it is read: os.environ["X"], os.environ.get("X"), getenv("X"),
    env_on("X") and env_int("X"). A read through an alias (e = os.environ; e.get("X")) or of a name built at run time slips past
    it (HumanBean on #22): a variable read that way has to be added to the lists by hand"""
    import re
    with tempfile.TemporaryDirectory(prefix="longshot-lift-env-") as d:
        d = Path(d)
        for top in ("a", "b"):
            (d / top / "exefs").mkdir(parents=True)
            (d / top / "work" / "image").mkdir(parents=True)
            (d / top / "exefs" / "main").write_bytes(b"NSO0 one")
            (d / top / "work" / "hot_funcs.txt").write_text("f_80000000\n")
        exefs, work = str(d / "a" / "exefs"), str(d / "a" / "work")
        env = lambda top: {"A64_HOT_CHUNK": "1", "OOT_WORK": str(d / top / "work"), "OOT_EXEFS": str(d / top / "exefs"),
                           "OOT_ROMFS": str(d / top / "romfs"), "OOT_DUMP": str(d / top), "RX_RELEASE": "1",
                           "A64_HOT_FUNCS": str(d / top / "work" / "hot_funcs.txt")}
        clean = {k: v for k, v in os.environ.items() if not k.startswith(("A64_", "OOT_", "RX_", "SUYU_"))}
        with mock.patch.dict(os.environ, clean, clear=True):
            key = make_game.lift_key(exefs, work, env("a"))
            check("the same game and inputs in other folders give the same key",
                  make_game.lift_key(str(d / "b" / "exefs"), str(d / "b" / "work"), env("b")) == key)
            check("RX_RELEASE does not change the key", make_game.lift_key(exefs, work, dict(env("a"), RX_RELEASE="")) == key)
            for name, changes in (("RX_VK_NO_PROGRAM_CACHE", False), ("RX_MARCH", False), ("SUYU_RECOMP_STRICT", True),
                                  ("RX_TARGET", True), ("A64_NO_VFPX", True), ("A64_VECFP", True),
                                  ("OOT_EXEFS_PATCHES_SHA1", True), ("OOT_EXEFS_PATCHES", False), ("HOS_MODS", False),
                                  ("HOS_OPTIONS_FILE", False)):
                with mock.patch.dict(os.environ, {name: "1"}):
                    check("%s in the environment %s the key" % (name, "changes" if changes else "leaves"),
                          (make_game.lift_key(exefs, work, env("a")) != key) == changes)
            # (run() gives the lift the build's env over the shell's: the key holds the value the lift reads)
            with mock.patch.dict(os.environ, {"A64_HOT_CHUNK": "0"}):
                check("a variable the build sets, also set in the shell: the build's value is in the key",
                      make_game.lift_key(exefs, work, env("a")) == key
                      and make_game.lift_key(exefs, work, dict(env("a"), A64_HOT_CHUNK="2")) != key)
    read = re.compile(r"""(?:environ(?:\.get)?\(?\[?|getenv\(|env_on\(|env_int\()\s*["']([A-Z][A-Z0-9_]+)["']""")
    names = {}
    for f in make_game.lift_sources():
        for m in read.finditer(open(f, encoding="utf-8", errors="replace").read()):
            names.setdefault(m.group(1), os.path.relpath(f, ROOT))
    check("the lift's sources read variables (%d)" % len(names), len(names) > 10)
    unknown = sorted("%s (%s)" % (n, f) for n, f in names.items() if not make_game.lift_reads_env(n)
                     and n not in make_game.LIFT_PATH_ENV and n not in make_game.LIFT_ENV_NOT_OUTPUT)
    check("every variable the lift's sources read is in the key, a path hashed by content, or in LIFT_ENV_NOT_OUTPUT "
          "(not yet: %s)" % ", ".join(unknown), not unknown)


def check_hot_list_override():
    """(HumanBean on #22) A64_HOT_FUNCS is a path the key leaves out: the build sets it to <work>/hot_funcs.txt, whose contents
    the key holds. With no <work>/hot_funcs.txt, a developer's own list named in the shell is what the lift reads, so the key holds
    its contents, or its path when the file is not there; with the build's own list, that one is read and the shell's is not"""
    with tempfile.TemporaryDirectory(prefix="longshot-hot-list-") as d:
        d = Path(d)
        (d / "exefs").mkdir()
        (d / "work" / "image").mkdir(parents=True)
        (d / "elsewhere").mkdir()
        (d / "exefs" / "main").write_bytes(b"NSO0 one")
        mine, same, other = d / "mine.txt", d / "elsewhere" / "same.txt", d / "other.txt"
        for path, text in ((mine, "f_80000000\n"), (same, "f_80000000\n"), (other, "f_80000040\n")):
            path.write_text(text)
        exefs, work, out = str(d / "exefs"), str(d / "work"), str(d / "out")
        env = {"A64_HOT_CHUNK": "1", "OOT_WORK": work}   # (no <work>/hot_funcs.txt: the build sets no A64_HOT_FUNCS)
        clean = {k: v for k, v in os.environ.items() if not k.startswith(("A64_", "OOT_", "RX_", "SUYU_"))}

        def key(hot, share=False, given=env):
            with mock.patch.dict(os.environ, {"A64_HOT_FUNCS": str(hot)} if hot else {}):
                return make_game.lift_share_key(exefs, work, out, given) if share else make_game.lift_key(exefs, work, given)
        with mock.patch.dict(os.environ, clean, clear=True):
            none, k_mine, share_mine = key(None), key(mine), key(mine, share=True)
            check("a hot list named in the shell changes the key", k_mine != none)
            check("...by its contents: the same list elsewhere gives the same key", key(same) == k_mine)
            check("...and another list another key", key(other) != k_mine)
            check("a hot list named in the shell that is not there: its path is in the key",
                  key(d / "gone.txt") not in (none, key(d / "gone2.txt")))
            mine.write_text("f_80000040\n")
            check("an edit to the shell's hot list changes the key", key(mine) != k_mine)
            check("...and the share key", key(mine, share=True) != share_mine)
            (d / "work" / "hot_funcs.txt").write_text("f_80000000\n")
            built = dict(env, A64_HOT_FUNCS=str(d / "work" / "hot_funcs.txt"))
            check("with the build's own hot list, the shell's is not the one read and does not change the key",
                  key(other, given=built) == key(None, given=built))


def check_code_prints():
    """A lift module is hashed by its code (make_game.py code_prints): a comment or a docstring edited alone keeps the key, since
    neither reaches the lifted C, while the code, and the text of a module that reads its own file, still change it. Each rule
    that hashes a module whole is tried on made-up modules; MARKS_NOT_OUTPUT names only lift modules that use a SOURCE_MARK"""
    with tempfile.TemporaryDirectory(prefix="longshot-lift-code-") as d:
        d = Path(d)
        (d / "exefs").mkdir()
        (d / "work").mkdir()
        (d / "exefs" / "main").write_bytes(b"NSO0 one")
        exefs, work = str(d / "exefs"), str(d / "work")
        real_open = open

        def key_with(name, edit):
            path = str(ROOT / name)

            def opened(p, *a, **k):
                if os.path.abspath(p) == path and "b" in (a[0] if a else k.get("mode", "r")):
                    import io
                    with real_open(p, "rb") as f:
                        return io.BytesIO(edit(f.read()))
                return real_open(p, *a, **k)
            with mock.patch("builtins.open", opened):
                return make_game.lift_key(exefs, work, {})
        clean = {k: v for k, v in os.environ.items() if not k.startswith(("A64_", "OOT_", "RX_", "SUYU_"))}
        with mock.patch.dict(os.environ, clean, clear=True):
            key = make_game.lift_key(exefs, work, {})
            for name in ("nx2/lift.py", "nx2/split_hot.py", "third_party/a64-kit/lift/a64_lift.py"):
                check("a comment added to %s keeps the key" % name, key_with(name, lambda b: b + b"\n# a comment\n") == key)
                check("a docstring edited in %s keeps the key" % name,
                      key_with(name, lambda b: b.replace(b'"""', b'"""An edit. ', 1)) == key)
                check("lines moved down in %s keep the key" % name, key_with(name, lambda b: b"\n\n" + b) == key)
                check("code added to %s changes the key" % name, key_with(name, lambda b: b + b"\nX_EDIT = 1\n") != key)
                check("a string changed in %s changes the key" % name,
                      key_with(name, lambda b: b + b"\nX_EDIT = 'a'\n") != key_with(name, lambda b: b + b"\nX_EDIT = 'b'\n"))
            check("oot_patches.py reads its own text (mentioned_addresses): a comment there changes the key",
                  key_with("nx2/oot_patches.py", lambda b: b + b"\n# 0x80001234\n") != key)

    def prints(**modules):
        files = [(str(ROOT / "nx2" / ("%s.py" % n)), text.encode()) for n, text in modules.items()]
        return {os.path.basename(p)[:-3]: v for p, v in make_game.code_prints(files).items()}
    plain = 'import os\nHERE = os.path.dirname(os.path.abspath(__file__))\ndef f():\n    """doc"""\n    return 1  # c\n'
    check("a plain module is hashed by its code", prints(a=plain)["a"] is not None)
    check("...the same with other comments and docstrings",
          prints(a=plain)["a"] == prints(a=plain.replace('"""doc"""', '"""other"""').replace("# c", "# d"))["a"])
    own = "import os\nME = os.path.abspath(__file__)\n"
    check("a module that may open its own file has every module hashed whole", set(prints(a=own, b=plain).values()) == {None})
    # (oot_patches.py's __file__ is in MARKS_NOT_OUTPUT: it reads only its own text, so it alone is hashed whole)
    check("a module whose own file is all it reads is hashed whole", prints(oot_patches=own, b=plain)["oot_patches"] is None)
    check("...and the others still by their code", prints(oot_patches=own, b=plain)["b"] is not None)
    for mark in ("import inspect\n", "import linecache\n", "X = f.__doc__\n", "import sys\nX = sys._getframe().f_lineno\n",
                 "X = getattr(f, '__doc__')\n", "import traceback\n"):
        check("a module that may read source text or line numbers (%r) has every module hashed whole" % mark,
              set(prints(a=mark, b=plain).values()) == {None})
    check("a module another names as a path is hashed whole",
          prints(a="import os\nP = os.path.join('nx2', 'b.py')\n", b=plain)["b"] is None)
    for made in ("'%s.py' % n", "n + '.py'", "f'{n}.py'", "'*.py'"):
        check("a Python file's name made at run time (%s) has every module hashed whole" % made,
              set(prints(a="n = 'b'\nP = %s\n" % made, b=plain).values()) == {None})
    check("a file name inside a message or a C comment is no path",
          prints(a="X = '/* (nx2/b.py: why) */'\n", b=plain)["b"] is not None)
    for mark in ("from inspect import getdoc\n", "from traceback import extract_stack\n"):
        check("a module importing from a source-reading module (%r) has every module hashed whole" % mark,
              set(prints(a=mark, b=plain).values()) == {None})
    for use in ("P = __file__.replace('a.py', 'b.py')\n",
                "import os\nP = os.path.dirname(os.path.dirname(os.path.abspath(__file__) + 'x'))\n",
                "import os\nP = os.path.dirname(__file__ + '/../b')\n"):
        check("__file__ other than the sole argument of dirname() (%r) has every module hashed whole" % use,
              set(prints(a="import os\n" + use, b=plain).values()) == {None})
    for use in ("P = os.path.dirname(__file__)\n", "P = os.path.dirname(os.path.realpath(__file__))\n",
                "from os.path import dirname\nP = dirname(__file__)\n", "P = os.path.join(os.path.dirname(__file__), 'x')\n"):
        check("a folder from dirname(__file__) (%r) keeps every module hashed by its code" % use,
              None not in prints(a="import os\n" + use, b=plain).values())
    allowed = make_game.MARKS_NOT_OUTPUT
    with mock.patch.dict(allowed, {"nx2/a.py": {"__doc__"}}):
        make_game._CODE_PRINTS.clear()   # (its answers are kept by the files alone: MARKS_NOT_OUTPUT is fixed in a build)
        check("a module's MARKS_NOT_OUTPUT covers only its own marks: __doc__ allowed, inspect still hashes every module whole",
              set(prints(a="import inspect\nX = f.__doc__\n", b=plain).values()) == {None})
        check("...and with every mark it names allowed, the others are hashed by their code",
              prints(a="X = f.__doc__\n", b=plain)["b"] is not None)
    sources = {os.path.abspath(f) for f in make_game.lift_sources()}
    for name, marks in allowed.items():
        with open(ROOT / name, "rb") as f:
            tree = ast.parse(f.read())
        check("MARKS_NOT_OUTPUT's %s is a lift module that names each mark it allows (%s)" % (name, sorted(marks)),
              str(ROOT / name) in sources and marks <= make_game._named_marks(tree))


def check_compile_only():
    """The variables the key leaves out are the compile's alone: no file of the translator (lift_sources(), what the key hashes)
    names them, so a translator that starts reading one fails here instead of reusing a lift made without it"""
    for f in make_game.lift_sources():
        text = open(f, encoding="utf-8", errors="replace").read()
        for name in make_game.COMPILE_ONLY_ENV:
            check("%s reads %s, which the lift key leaves out as the compile's alone" % (os.path.relpath(f, ROOT), name), name not in text)


if __name__ == "__main__":
    main()
