"""`dev.py packs` (scripts/devtools/packs.py): how a change is built, which modules and files a pack keeps, and a runtime from a base and packs.

python -I -B tests/test_dev_packs.py
"""
import contextlib
import importlib.util
import io
import json
import os
import re
import shutil
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


sys.path.insert(0, str(ROOT / "scripts"))
from devtools import common, packs  # noqa: E402
dev = load("dev_under_test", ROOT / "scripts" / "dev.py")   # (its devtools are the ones above: one module each)
PACKS_SOURCE = ROOT / "scripts" / "devtools" / "packs.py"


def packs_main(argv):
    """`dev.py packs ARGV`"""
    return dev.main(["packs"] + list(argv))
make_game = load("make_game_under_test", ROOT / "nx2" / "make_game.py")


def check(what, ok):
    if not ok:
        raise SystemExit("FAIL: " + what)


def write(p, data=b"x"):
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data)


def test_route():
    r = packs.route
    check("the replay alone builds the renderer alone", r(["gpu/vk/vk_replay.cpp", "tests/test_x.py"]) == "renderer")
    check("the recompiler builds the renderer alone", r(["gpu/vk/eden/shader_recompiler/ir_opt/x.cpp"]) == "renderer")
    check("imgui builds the renderer alone", r(["third_party/imgui/imgui.cpp"]) == "renderer")
    check("a host change builds the game", r(["host/hos_thread.c"]) == "game")
    check("a game patch builds the game", r(["nx2/oot_patches.py"]) == "game")
    check("a renderer file and a host file build the game (both modules)", r(["gpu/vk/vk_draw.cpp", "host/nvn_native.c"]) == "game")
    check("gpu/render (the op stream host/ includes) builds the game", r(["gpu/render/x.h"]) == "game")
    check("a build script builds the game", r(["nx2/build.py"]) == "game")
    check("docs and tests build nothing", r(["docs/dev/setup.md", "tests/test_a.py", "README.md"]) is None)
    check("a renderer shader and its generated header build the renderer alone (only the renderer is kept)",
          r(["gpu/vk/shaders/fg_copy.comp", "gpu/vk/shaders/fg_copy_comp_spv.h"]) == "renderer")
    check("the forge's workflows build nothing", r([".forgejo/workflows/ci.yml", ".forgejo/pull_request_template.md"]) is None)
    check("an unknown path builds the game", r(["somewhere/new.c"]) == "game")
    check("developer tools, CI's settings and nx2/'s checks build nothing",
          r(["scripts/perf.py", "scripts/profiling/cpu.py", "mise.toml", ".circleci/config.yml", "nx2/test_insn.py"]) is None)
    for f in ("scripts/fetch.py", "scripts/tools.py", "scripts/helpers.py", "scripts/linux/deps.sh", "scripts/dev.py",
              "scripts/devtools/build.py", "scripts/devtools/packs.py", "scripts/devtools/common.py", "scripts/devtools/jobqueue.py",
              "builder/linux/linux_tools.py", "tools.lock", "requirements.txt", "nx2/lower_test.py", "nx2/test_data.txt"):
        check("%s (read by the build or its environment) builds the game" % f, r([f]) == "game")


def test_not_built():
    """No file of the build or of the environment dev.py gives it (BUILD_READERS) names a path of packs.NOT_BUILT: as a string
    that is that path or ends in it, or as os.path.join's parts in a row. Messages and docstrings may name them (they hold a space)"""
    import ast
    readers = [f for f in (ROOT / "nx2").glob("*.py") if not f.name.startswith("test_")]
    readers += list((ROOT / "third_party" / "a64-kit").rglob("*.py"))
    readers += [ROOT / f for f in ("builder/linux/linux_tools.py", "scripts/dev.py", "scripts/fetch.py", "scripts/tools.py",
                                   "scripts/helpers.py", "scripts/devtools/build.py", "scripts/devtools/packs.py",
                                   "scripts/devtools/common.py", "scripts/devtools/jobqueue.py")]
    named = []
    for f in readers:
        tree = ast.parse(f.read_bytes(), str(f))
        tree.body = [n for n in tree.body if not (isinstance(n, ast.Assign) and   # (the list itself)
                                                  any(getattr(t, "id", None) == "NOT_BUILT" for t in n.targets))]
        strings =[[a.value.replace("\\", "/") for a in n.args if isinstance(a, ast.Constant) and isinstance(a.value, str)]
                   for n in ast.walk(tree) if isinstance(n, ast.Call)]
        strings += [[n.value.replace("\\", "/")] for n in ast.walk(tree) if isinstance(n, ast.Constant) and isinstance(n.value, str)]
        for parts in strings:
            joined = "/".join(p.strip("/") for p in parts if p and " " not in p)
            for e in packs.NOT_BUILT:
                e = e.rstrip("/")
                if joined == e or joined.endswith("/" + e) or joined.startswith(e + "/") or ("/" + e + "/") in joined:
                    named.append("%s names %s" % (f.relative_to(ROOT), e))
    check("the build reads nothing route() builds nothing for (%s)" % "; ".join(sorted(set(named))), not named)
    check("packs.py itself is not in NOT_BUILT", not any("scripts/devtools/packs.py".startswith(e) for e in packs.NOT_BUILT))


def test_clone(d):
    """A runtime's copies (packs.clone): the same bytes, times and mode as the base's file, and a write to the copy leaves the
    base's file as it was (a copy-on-write clone where the file system has them, else a copy)"""
    src, dst = d / "game.bin", d / "copy.bin"
    src.write_bytes(b"base" * 4096)
    os.chmod(src, 0o755)
    os.utime(src, (1_000_000_000, 1_000_000_000))
    check("clone gives back the copy's path", packs.clone(str(src), str(dst)) == str(dst))
    check("the same bytes", dst.read_bytes() == src.read_bytes())
    check("the same time", int(dst.stat().st_mtime) == 1_000_000_000)
    check("the same mode", os.name == "nt" or (dst.stat().st_mode & 0o777) == 0o755)
    with open(dst, "r+b") as f:
        f.write(b"play")
    check("a write to the copy leaves the base's file", src.read_bytes() == b"base" * 4096)


def test_stamp_matches_build(d):
    """A runtime prepares again when its build's shader stamp moved: the files nx2/translator_files.py names (the same module
    nx2/build.py makes LONGSHOT_SHADER_STAMP with), read from the checkout packed, or that module itself; a checkout from before
    it at any change"""
    src = (ROOT / "nx2" / "build.py").read_text(encoding="utf-8")
    check("nx2/build.py makes LONGSHOT_SHADER_STAMP with translator_files.stamp",
          'shader_stamp = "-DLONGSHOT_SHADER_STAMP=0x%sull" % translator_files.stamp(vk)' in src)
    check("cmd_pack prepares by translator_changed and cache_format_changed",
          'prepare = translator_changed(tree, changed) or cache_format_changed(tree, b["commit"], changed) or SEED in extra' in
          PACKS_SOURCE.read_text(encoding="utf-8"))
    tree, old = d / "tree", d / "old"
    write(tree / "nx2" / "translator_files.py", (ROOT / "nx2" / "translator_files.py").read_bytes())
    old.mkdir()
    for path, prepares in (("gpu/vk/eden/shader_recompiler/ir_opt/texture_pass.cpp", True), ("gpu/vk/sirit/src/a.cpp", True),
                           ("gpu/vk/compat/longshot_env.h", True), ("gpu/vk/vk_shader.cpp", True),
                           ("gpu/vk/vk_bank.cpp", True), ("gpu/vk/vk_state.h", True), ("nx2/translator_files.py", True),
                           ("gpu/vk/vk_api.h", False), ("gpu/vk/vk_draw.cpp", False), ("gpu/vk/vk_menu.cpp", False),
                           ("nx2/build.py", False)):
        check("%s %s the runtime prepare again" % (path, "makes" if prepares else "does not make"),
              packs.translator_changed(str(tree), [path]) == prepares)
    check("nothing changed: no prepare", not packs.translator_changed(str(tree), []))
    # (a checkout too old to name its stamped files prepares again at any change, as packs.translator_changed says)
    check("a checkout without nx2/translator_files.py prepares again",
          packs.translator_changed(str(old), ["gpu/vk/vk_api.h"]) and packs.translator_changed(str(old), ["gpu/vk/vk_draw.cpp"]))


def test_cache_format(d):
    """A new kProgramCacheFormat drops every cache the base prepared (program_cache_stamp holds it), so a runtime prepares
    again, though the translator's stamp is the same: found in the renderer's source as the game reads it"""
    src = (ROOT / packs.PROGRAM_CACHE).read_text(encoding="utf-8")
    check("kProgramCacheFormat found in %s" % packs.PROGRAM_CACHE, packs.CACHE_FORMAT.search(src))
    check("program_cache_stamp holds kProgramCacheFormat", "program_cache_stamp_of(kProgramCacheFormat)" in src)
    import subprocess
    repo = d / "fmt"
    path = repo / "gpu" / "vk" / "vk_progcache.cpp"
    def run(*args):
        subprocess.run(["git", "-C", str(repo)] + list(args), check=True, capture_output=True)
    write(path, b"// a\nconstexpr uint32_t kProgramCacheFormat = 1;\n")
    run("init", "-q")
    run("add", "-A")
    run("-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "base")
    base = subprocess.run(["git", "-C", str(repo), "rev-parse", "HEAD"], check=True, capture_output=True, text=True).stdout.strip()
    changed = [packs.PROGRAM_CACHE]
    write(path, b"// b\nconstexpr uint32_t kProgramCacheFormat = 1;\n")
    check("an edit that keeps the format does not prepare", not packs.cache_format_changed(str(repo), base, changed))
    write(path, b"// b\nconstexpr uint32_t kProgramCacheFormat = 2;\n")
    check("a new format prepares", packs.cache_format_changed(str(repo), base, changed))
    check("the file not changed: no prepare", not packs.cache_format_changed(str(repo), base, ["gpu/vk/vk_draw.cpp"]))


def test_renderer_files_the_game_reads():
    """The renderer files the game is built from take the full route, so the runtime gets the game built from them too"""
    found = packs.renderer_read_by_game(str(ROOT))
    for f in ("gpu/vk/vk_api.h", "gpu/vk/dlss_math.h", "gpu/vk/compat/longshot_env.h"):
        check("%s found as read by the game (%s)" % (f, sorted(found)), f in found)
        check("%s takes the full route" % f, packs.route([f]) == "game")
    check("a renderer file the game does not read stays renderer-only", "gpu/vk/vk_replay.cpp" not in found)


def test_stamped_objects_match_build():
    """The objects named in MODULES as holding the version are the files nx2/build.py gives it to"""
    src = (ROOT / "nx2" / "build.py").read_text(encoding="utf-8")
    stamped = re.findall(r'\[stamp_def\] if os\.path\.basename\((?:s|src)\) == "([^"]+)"', src)
    check("nx2/build.py's version files found (anchor: '[stamp_def] if os.path.basename(...) == ...')", stamped)
    check("they are packs.STAMPED_SOURCES", sorted(stamped) == sorted(os.path.basename(s) for s in packs.STAMPED_SOURCES))


def test_caches_named_in_renderer():
    text = "".join(p.read_text(encoding="utf-8", errors="replace") for g in ("*.cpp", "*.h") for p in (ROOT / "gpu" / "vk").glob(g))
    for c in packs.CACHES + (packs.SEED,):
        # (some are a named file with a suffix the renderer adds: .predicted and .built to the notes, .idx for .bin)
        name = c.replace(".predicted_keys", "").replace(".predicted", "").replace(".built", "").replace(".zst.idx", ".zst.bin")
        check("the renderer still names %s" % name, name in text)
    check("the predicted and built notes' suffixes, and the index's", all(s in text for s in ('".predicted"', '".predicted_keys"', '".built"', '".idx"')))
    # (and the other way round: every .bin or .idx the renderer names beside the game is a cache, or one of the two files a
    # build makes (the shader seed, the shipped state list); a new cache file in the renderer fails here until it is classified)
    named = set(re.findall(r'"(longshot_[A-Za-z0-9_.]*\.(?:bin|idx))"', text))
    unknown = sorted(n for n in named if not packs.is_cache(n) and n not in (packs.SEED, "longshot_pipeline_states_shipped.bin"))
    check("every file the renderer keeps beside the game is classified (unknown: %s)" % unknown, not unknown)


def test_module_changed(d):
    base, mine = d / "base-build", d / "pack-build"
    for build in (base, mine):
        write(build / "obj" / "a.o", b"same a")
        write(build / "obj" / "host_version.o", b"version " + build.name.encode())
        (build / "objs.rsp").write_text("".join('"%s"\n' % (build / "obj" / n) for n in ("a.o", "host_version.o")), encoding="utf-8")
    mc = lambda changed_source=False: packs.module_changed(str(base), str(mine), "objs.rsp", "host_version.o", changed_source)
    check("only the version differs: the base's module", not mc())
    check("the version's own source changed: the pack's", mc(True))
    write(mine / "obj" / "a.o", b"other a")
    check("another object differs: the pack's", mc())
    write(mine / "obj" / "a.o", b"same a")
    write(mine / "obj" / "b.o", b"new")
    (mine / "objs.rsp").write_text("".join('"%s"\n' % (mine / "obj" / n) for n in ("a.o", "b.o", "host_version.o")), encoding="utf-8")
    check("another set of objects: the pack's", mc())
    check("no link list in the base: the pack's", packs.module_changed(str(d / "none"), str(mine), "objs.rsp", "host_version.o", False))


def test_inputs_beside_the_objects():
    lift = {os.path.relpath(f, ROOT).replace(os.sep, "/") for f in make_game.lift_sources()}
    build = packs.build_scripts(str(ROOT))
    for f in ("nx2/build.py", "nx2/win_manifest.py", "nx2/version.py", "third_party/a64-kit/tools/toolchain.py"):
        check("%s found among the build's scripts (%s)" % (f, sorted(build)), f in build)
    check("the lift's own scripts are not the build's", "nx2/lift.py" not in build)
    lic = lambda *p: packs.link_inputs_changed(list(p), lift, build)
    check("a game patch shows in the objects", not lic("nx2/oot_patches.py", "host/hos_thread.c"))
    check("toolchain.py's link options, though the lift reads it too: both modules", lic("third_party/a64-kit/tools/toolchain.py"))
    check("make_game.py's own helpers show in the game folder", not lic("nx2/pipeline_states.py"))
    check("make_game.py sets build.py's environment (the hot order): both modules", lic("nx2/make_game.py"))
    check("the Windows manifest does not: both modules", lic("nx2/win_manifest.py"))
    check("nor build.py's flags", lic("nx2/build.py"))
    check("nor a library", lic("third_party/sdl2/SDL2.lib"))
    check("nor a shader source", lic("gpu/vk/shaders/blit.comp"))
    data = sorted(f for f in lift if f.endswith((".txt", ".json")))
    check("a translator data file shows in the objects (%s)" % data[:1], data and not lic(data[0]))
    md = packs.stamped_may_differ
    check("the version object's own source", md(["host/version.c"], "host/version.c"))
    check("a header: the version files include none", not md(["gpu/vk/oot_cheat_items.h"], "host/version.c"))
    check("another source alone: only the version differs there", not md(["host/nvn_native.c"], "host/version.c"))
    for src in packs.STAMPED_SOURCES:
        check("%s includes nothing, so no other file reaches its object" % src,
              "#include" not in (ROOT / src).read_text(encoding="utf-8"))


def test_renderer_untouched():
    """A game pack builds the game alone (RX_GAME_ONLY) only when no file the renderer is built from changed"""
    lift = {os.path.relpath(f, ROOT).replace(os.sep, "/") for f in make_game.lift_sources()}
    ru = lambda paths, env=None: packs.renderer_untouched(paths, lift, env or {})
    check("a host C file the renderer does not compile", ru(["host/hos_thread.c", "docs/dev/setup.md"]))
    check("the version's own file", ru(["host/version.c"]))
    check("a game patch (a file of the lift)", ru(["nx2/oot_patches.py"]))
    check("a host header may reach the renderer", not ru(["host/hos.h"]))
    check("the renderer's own copy of the POSIX layer", not ru(["host/posix/win32_posix.c"]))
    check("the renderer's counters' writer", not ru(["host/pgo_rt.c"]))
    check("a renderer file", not ru(["host/hos_thread.c", "gpu/vk/vk_draw.cpp"]))
    check("the op stream", not ru(["gpu/render/x.h"]))
    check("a build script", not ru(["nx2/build.py"]))
    check("a renderer profile for the pack", not ru(["host/hos_thread.c"], {"RX_VK_PGO": "gen"}))
    check("LONGSHOT_TRACY's zones are the renderer's too", not ru(["host/hos_thread.c"], {"LONGSHOT_TRACY": "1"}))
    check("the game's profile alone", ru([], {"RX_PGO": "x.profdata"}))
    src = (ROOT / "nx2" / "build.py").read_text(encoding="utf-8")
    check("nx2/build.py builds no renderer under RX_GAME_ONLY",
          'game_only = kit.env_on("RX_GAME_ONLY")' in src and "build_vk = a.standalone and not game_only and" in src)
    check("RX_GAME_ONLY is the compile's alone (not in the lift key)", "RX_GAME_ONLY" in make_game.COMPILE_ONLY_ENV)
    check("cmd_pack keeps only the game's module of a game-only build",
          "MODULES[:1] if game_only else MODULES" in PACKS_SOURCE.read_text(encoding="utf-8"))


def refused(what, argv, reason):
    """Refused for that reason, not another check's. argv: a `dev.py packs` command line, or a function to call"""
    try:
        argv() if callable(argv) else packs_main(argv)
    except SystemExit as e:
        check(what + " refused because %s (%r)" % (reason, e.code), isinstance(e.code, str) and reason in e.code)
    else:
        check(what + " refused", False)


def test_guards(d):
    base = d / "base"
    write(base / "game" / "user" / "save0.bin", b"played")
    refused("a base built into a played game folder", ["base", str(base), "--romfs", "r", "--exefs", "e", "--tree", str(ROOT)],
            "new game folder")
    f = packs.pack_folders(str(base))
    refused("a base whose work folder is its packs' own",
            lambda: packs.check_folders(str(base), {"work": f["game_work"], "game": str(base / "game")}), "packs are built")
    write(base / "game" / packs.GAME, b"g")
    write(base / "game" / packs.RENDERER, b"r")
    packs.write_base(str(base), {"commit": "c" * 40, "work": str(d / "w"), "game": str(base / "game"), "romfs": "r", "exefs": "e"})
    refused("a base whose game folder is inside its packs' output folder",
            lambda: packs.check_folders(str(d / "b2"), {"work": str(d / "w"),
                                                        "game": os.path.join(packs.pack_folders(str(d / "b2"))["game_out"], "g")}),
            "where its packs are built")
    check("folders on two drives do not overlap (no error)", not packs.overlaps("C:\\a", "D:\\a") if os.name == "nt" else True)
    link = d / "alias"
    target = packs.pack_folders(str(d / "b3"))["game_out"]
    os.makedirs(target)
    try:
        os.symlink(target, link, target_is_directory=True)
    except OSError:   # (Windows without the right to make links: a junction, which needs none)
        import _winapi
        _winapi.CreateJunction(target, str(link))
    refused("a base whose game folder is its packs' output folder through a link",
            lambda: packs.check_folders(str(d / "b3"), {"work": str(d / "w"), "game": str(link)}), "where its packs are built")
    write(d / "launcher" / "bin" / packs.GAME, b"g")
    refused("a launcher's game folder (modules in bin/)",
            lambda: packs.write_base(str(d / "b4"), {"commit": "c" * 40, "game": str(d / "launcher")}), "plain game folder")
    refused("a runtime inside the base's game folder", ["rt", str(base), str(base / "game" / "runtime")], "folder of its own")
    check("...and nothing written there", not (base / "game" / "runtime").exists())
    for name in ("../game", "..", "a/b", str(d)):
        refused("the pack name %r" % name, ["pack", str(base), name, "--tree", str(ROOT)], "pack's name")
    check("the base is still there", (base / "game" / "user" / "save0.bin").exists())


def test_pack_folders(d):
    """Every pack of a base builds in the same two folders, so a game pack after the first finds the translation there. The lift
    key leaves the folders' paths out (make_game.py LIFT_PATH_ENV): the same in another folder"""
    f = packs.pack_folders(str(d))
    check("the same folders for every pack", f == packs.pack_folders(str(d)) and not f["game_work"].startswith(os.path.join(str(d), "packs")))
    exefs, work, out = d / "exefs", Path(f["game_work"]), Path(f["game_out"])
    write(exefs / "main", b"NSO0 one")
    env = lambda w, o: {"OOT_WORK": str(w), "OOT_DUMP": str(o), "RX_RELEASE": "1", "A64_HOT_CHUNK": "1"}
    key = make_game.lift_key(str(exefs), str(work), env(work, out))
    check("the key is the same in the same folders", key == make_game.lift_key(str(exefs), str(work), env(work, out)))
    check("...and in another (the folders' paths are not in it)",
          key == make_game.lift_key(str(exefs), str(d / "other"), env(d / "other", out)))
    base = d / "base-work"
    write(base / "objcache" / "ab" / "abcdef.o", b"object")
    write(base / "system_headers.txt", b"headers")
    write(base / "sa" / "gen" / "code_0001.c", b"lifted")
    packs.link_objects(str(base), str(work))
    check("the base's objects, hard-linked", os.path.samefile(work / "objcache" / "ab" / "abcdef.o", base / "objcache" / "ab" / "abcdef.o"))
    check("the compiler's header note copied", (work / "system_headers.txt").read_bytes() == b"headers")
    check("no lifted C of the base's (another folder's key)", not (work / "sa").exists())
    (work / "objcache" / "ab" / "mine.o").write_bytes(b"pack")
    packs.link_objects(str(base), str(work))
    check("a work folder already there is left as it is", (work / "objcache" / "ab" / "mine.o").exists())
    real_link = os.link

    def cross_drive(src, dst):
        raise OSError(18, "Invalid cross-device link")
    os.link = cross_drive
    try:
        packs.link_objects(str(base), str(d / "other-drive"))
    finally:
        os.link = real_link
    check("on another drive the objects are copied", (d / "other-drive" / "objcache" / "ab" / "abcdef.o").read_bytes() == b"object")


def test_generated_and_runtime(d):
    game_mod, vk_mod = packs.GAME, packs.RENDERER
    base_dir, game, out = d / "base", d / "base" / "game", d / "out"
    write(game / "romfs" / "a" / "data.bin", b"romfs")
    write(game / "romfs" / "prompts" / "icon.bntx", b"base icon")
    write(game / game_mod, b"base game")
    write(game / vk_mod, b"base renderer")
    write(game / "cache" / "longshot_programs.bin", b"cache")
    write(game / "cache" / "prompts" / "Common.bntx.zs", b"made at run time")   # (the game's, never a build's output)
    write(game / "logs" / "longshot_log.txt", b"a log")
    write(game / "language.txt", b"en-US")
    write(game / "prompts" / "Common.bntx", b"base prompts")
    write(game / "longshot_pipeline_states_shipped.bin", b"shipped")
    write(game / "longshot_options.ini", b"[menu]")
    write(game / "cache" / "longshot_vk_pipelines_play_2.bin", b"seeded play cache")   # (what --seed brings, before the build records outputs)
    write(game / "cache" / "longshot_vk_pipelines.stamp", b"the pipeline cache's stamp")
    write(game / "longshot_pipelines.bin.predicting", b"a prediction a crash left unfinished")
    if packs.MODULES[0][1]:   # (the game's map: Windows only)
        write(game / packs.MODULES[0][1], b"map")
    outputs = packs.build_outputs(str(game))
    check("a play cache and the pipeline stamp are not build outputs (%s)" % outputs,
          "cache/longshot_vk_pipelines_play_2.bin" not in outputs and "cache/longshot_vk_pipelines.stamp" not in outputs
          and "longshot_pipelines.bin.predicting" not in outputs)
    write(game / "user" / "save0.bin", b"a save, made in play")
    write(game / "longshot_env.txt", b"HOS_X=1")
    os.makedirs(out / "romfs" / "a")
    os.link(game / "romfs" / "a" / "data.bin", out / "romfs" / "a" / "data.bin")
    write(out / "romfs" / "prompts" / "icon.bntx", b"new icon")
    write(out / game_mod, b"built game")
    write(out / "cache" / "longshot_programs.bin", b"other cache")
    write(out / "cache" / "longshot_vk_pipelines_play_5.bin", b"a play cache the pack's build has")
    write(out / "language.txt", b"en-US")
    gen, gone = packs.generated_changes(str(game), str(out), outputs)
    check("a regenerated file is kept, the same ones, the modules and the caches are not: %s" % gen, gen == ["romfs/prompts/icon.bntx"])
    check("generated files the build no longer makes are gone; saves, settings and the game's own files are not: %s" % gone,
          gone == ["longshot_pipeline_states_shipped.bin", "prompts/Common.bntx"])
    check("a base that recorded no outputs loses nothing", packs.generated_changes(str(game), str(out))[1] == [])
    # (a list recorded before caches and the preparation's recovery markers were filtered: a crash mid-prediction leaves one)
    legacy = outputs + ["cache/longshot_vk_pipelines.stamp", "cache/longshot_vk_pipelines_play_2.bin",
                        "longshot_pipelines.bin.predicting", "longshot_pipelines.bin.running", "longshot_pipelines.bin.built.warming"]
    gone = packs.generated_changes(str(game), str(out), legacy)[1]
    check("an older base's outputs list never deletes a cache or a recovery marker, and still a generated file the build no "
          "longer makes: %s" % gone,
          gone == ["longshot_pipeline_states_shipped.bin", "prompts/Common.bntx"])
    write(out / "stale.bin", b"an earlier pack's")
    write(out / "image.bin", b"image")
    write(out / packs.SEED, b"shader list")
    write(out / ".longshot_installed.json", b"{}")
    work = d / "pack-work"
    work.mkdir()
    keep = packs.kept_outputs(make_game, str(work), str(game / "romfs"), str(out))
    check("no shader list note: the list is not kept (%s)" % sorted(keep), packs.SEED not in keep)
    make_game.note_shader_list(str(work), str(game / "romfs"), str(out / packs.SEED))
    keep = packs.kept_outputs(make_game, str(work), str(game / "romfs"), str(out))
    check("the modules, image.bin, the install note and a current shader list are kept (%s)" % sorted(keep),
          keep == {packs.GAME, packs.RENDERER, "image.bin", make_game.INSTALLED_NOTE, packs.SEED})
    packs.clear_output(str(out), keep)
    check("the output folder keeps romfs and what the next build checks itself (%s)" % sorted(os.listdir(out)),
          sorted(os.listdir(out)) == sorted(["romfs", packs.GAME, "image.bin", packs.SEED, make_game.INSTALLED_NOTE]))
    packs.clear_output(str(out))
    check("the output folder keeps only romfs for the next build", sorted(os.listdir(out)) == ["romfs"])
    check("...its links intact", os.path.samefile(out / "romfs" / "a" / "data.bin", game / "romfs" / "a" / "data.bin"))
    b = {"commit": "c" * 40, "work": str(base_dir / "work"), "game": str(game), "romfs": "r", "exefs": "e"}
    packs.write_base(str(base_dir), b)
    for name, files, generated, on in (("cpu", {game_mod: "h"}, ["romfs/prompts/icon.bntx"], b["commit"]),
                                       ("cpu2", {game_mod: "h"}, [], b["commit"]), ("old", {vk_mod: "h"}, [], "o" * 40)):
        p = base_dir / "packs" / name
        for m in files:
            write(p / m, b"pack " + name.encode())
        for rel in generated:
            write(p / "files" / rel, b"new icon")
        (p / "pack.json").write_text(json.dumps({"base": on, "files": files, "generated": generated, "prepare": False,
                                                 "deleted": ["prompts/Common.bntx"] if name == "cpu" else []}))
    rt = d / "rt"
    packs_main(["rt", str(base_dir), str(rt), "cpu"])
    check("the pack's game", (rt / game_mod).read_bytes() == b"pack cpu")
    check("the base's renderer", (rt / vk_mod).read_bytes() == b"base renderer")
    check("romfs hard-linked", os.path.samefile(rt / "romfs" / "a" / "data.bin", game / "romfs" / "a" / "data.bin"))
    check("the caches copied, not linked (the game writes them)", not os.path.samefile(rt / "cache" / "longshot_programs.bin", game / "cache" / "longshot_programs.bin"))
    check("the pack's generated file", (rt / "romfs" / "prompts" / "icon.bntx").read_bytes() == b"new icon")
    check("...not written through the base's hard link", (game / "romfs" / "prompts" / "icon.bntx").read_bytes() == b"base icon")
    check("the pack's deleted file is not in the runtime", not (rt / "prompts" / "Common.bntx").exists())
    check("...and still in the base", (game / "prompts" / "Common.bntx").exists())
    for asked, what, why in ((["cpu", "cpu2"], "two packs with the same module", "both bring"),
                             (["old"], "a pack on another base", "not the base")):
        refused(what, ["rt", str(base_dir), str(d / ("rt-" + asked[-1])), *asked], why)
    victim = d / "outside.txt"
    write(victim, b"not the runtime's")
    for name, meta, why in (("abs", {"generated": [str(victim)]}, "outside the runtime"),
                            ("up", {"deleted": ["../outside.txt"]}, "outside the runtime"),
                            ("deep", {"generated": ["romfs/../../outside.txt"]}, "outside the runtime"),
                            ("mod", {"files": {"../outside.txt": "h"}}, "names modules")):
        p = base_dir / "packs" / name
        (p / "files").mkdir(parents=True, exist_ok=True)
        (p / "pack.json").write_text(json.dumps(dict({"base": b["commit"], "files": {}, "generated": [], "deleted": [],
                                                      "prepare": False}, **meta)))
        refused("a pack.json naming %s" % meta, ["rt", str(base_dir), str(d / ("rt-" + name)), name], why)
        check("...and nothing made", not (d / ("rt-" + name)).exists())
    check("the file outside is untouched", victim.read_bytes() == b"not the runtime's")
    refused("a pack.json naming ..\\outside.txt (a backslash path)",
            ["rt", str(base_dir), str(d / "rt-bs"), *_pack(base_dir, b, "bs", {"deleted": ["..\\outside.txt"]})], "outside the runtime")
    # A link in the base's romfs is cloned into the runtime. A relative one that names a file from the base names a folder
    # beside the runtime once cloned (../../outside from base/game/romfs is base/outside, from rt-link/romfs it is outside/),
    # so a deleted path through it reads as inside and is not. The runtime is made, then refused at that file.
    write(base_dir / "outside", b"a file beside the base's game folder")
    write(d / "outside" / "victim.txt", b"beside the runtime")
    try:
        os.symlink(os.path.join("..", "..", "outside"), game / "romfs" / "escape")
    except OSError:   # (no right to make a file link here, as on Windows without developer mode: nothing to check)
        print("  skipped: a file link in the base's romfs (this account cannot make one)")
    else:
        try:
            packs_main(["rt", str(base_dir), str(d / "rt-link"), _pack(base_dir, b, "link", {"deleted": ["romfs/escape/victim.txt"]})[0]])
        except SystemExit as e:
            if os.path.islink(d / "rt-link" / "romfs" / "escape"):
                check("a deleted path through a cloned link refused (%r)" % e.code, isinstance(e.code, str) and "through a link" in e.code)
        check("the file beside the runtime is untouched", (d / "outside" / "victim.txt").read_bytes() == b"beside the runtime")
        os.remove(game / "romfs" / "escape")


def test_rt_refresh(d):
    """rt over an existing runtime refreshes it in place: files that differ are replaced, files no longer wanted removed, and
    its caches, logs, saves and settings kept; a folder that is not a runtime is refused, one of an older layout made again"""
    base_dir, game = fake_base(d)
    write(game / "texts" / "a.txt", b"base text")
    b = packs.read_base(str(base_dir))
    rt = d / "rt"
    _pack(base_dir, b, "p", {"generated": ["romfs/new.bin", "texts/a.txt"], "files": {packs.GAME: "h"}})
    write(base_dir / "packs" / "p" / packs.GAME, b"pack game, longer")
    write(base_dir / "packs" / "p" / "files" / "romfs" / "new.bin", b"generated")
    write(base_dir / "packs" / "p" / "files" / "texts" / "a.txt", b"pack text, longer")
    packs_main(["rt", str(base_dir), str(rt), "p"])
    check("the pack's game", (rt / packs.GAME).read_bytes() == b"pack game, longer")
    renderer_ino = (rt / packs.RENDERER).stat().st_ino
    write(rt / "cache" / "longshot_programs.bin", b"grown in play")
    write(rt / "logs" / "longshot_log.txt", b"a log")
    write(rt / "user" / "save0.bin", b"a save")
    write(rt / "longshot_options.ini", b"[menu]")
    packs_main(["rt", str(base_dir), str(rt)])   # (the same runtime, now with no pack)
    check("the base's game again", (rt / packs.GAME).read_bytes() == b"base game")
    check("the base's text again", (rt / "texts" / "a.txt").read_bytes() == b"base text")
    check("the pack's generated file is gone", not (rt / "romfs" / "new.bin").exists())
    check("romfs still hard-linked", os.path.samefile(rt / "romfs" / "a" / "data.bin", game / "romfs" / "a" / "data.bin"))
    check("an unchanged module is not copied again", (rt / packs.RENDERER).stat().st_ino == renderer_ino)
    check("the caches play grew are kept", (rt / "cache" / "longshot_programs.bin").read_bytes() == b"grown in play")
    check("the logs, saves and settings are kept", (rt / "logs" / "longshot_log.txt").exists() and
          (rt / "user" / "save0.bin").exists() and (rt / "longshot_options.ini").read_bytes() == b"[menu]")
    made = json.loads((rt / "dev_packs.json").read_text())
    check("dev_packs.json names the runtime's files and layout", made.get("layout") == packs.RT_LAYOUT and
          packs.GAME in made["files"] and "romfs/new.bin" not in made["files"] and made["packs"] == [])
    with open(rt / packs.GAME, "r+b") as f:   # (a module rewritten by hand: another time, replaced)
        f.write(b"HAND")
    os.utime(rt / packs.GAME, (1, 1))
    packs_main(["rt", str(base_dir), str(rt)])
    check("a module that differs is replaced", (rt / packs.GAME).read_bytes() == b"base game")
    check("...and the base's is untouched", (game / packs.GAME).read_bytes() == b"base game")
    # (a runtime from before the layout: no files list. Everything but its play state is made again)
    made.pop("layout"); made.pop("files")
    (rt / "dev_packs.json").write_text(json.dumps(made))
    write(rt / "stale-from-an-old-pack.bin", b"old")
    packs_main(["rt", str(base_dir), str(rt)])
    check("an older runtime's unknown file goes", not (rt / "stale-from-an-old-pack.bin").exists())
    check("...its caches and saves stay", (rt / "cache" / "longshot_programs.bin").read_bytes() == b"grown in play" and
          (rt / "user" / "save0.bin").exists())
    write(d / "not-a-runtime" / "precious.txt", b"mine")
    refused("a folder that is not a runtime", ["rt", str(base_dir), str(d / "not-a-runtime")], "not a runtime")
    check("...untouched", (d / "not-a-runtime" / "precious.txt").read_bytes() == b"mine")


def test_resolves_inside(d):
    """The check made where each file is written: a folder link inside the runtime that leads out is outside"""
    rt, out = d / "rt", d / "out"
    (rt / "romfs" / "real").mkdir(parents=True)
    out.mkdir()
    try:
        os.symlink(out, rt / "romfs" / "door", target_is_directory=True)
    except OSError:   # (Windows without the right to make links: a junction, which needs none)
        import _winapi
        _winapi.CreateJunction(str(out), str(rt / "romfs" / "door"))
    check("a real folder in the runtime is inside", packs.resolves_inside(str(rt), str(rt / "romfs" / "real")))
    check("a folder not made yet below the runtime is inside", packs.resolves_inside(str(rt), str(rt / "romfs" / "new" / "deeper")))
    check("a folder link in the runtime that leads out is outside", not packs.resolves_inside(str(rt), str(rt / "romfs" / "door")))
    check("...and a path through it", not packs.resolves_inside(str(rt), str(rt / "romfs" / "door" / "x")))
    check("the folder beside the runtime is outside", not packs.resolves_inside(str(rt), str(out)))
    upper = str(rt).upper() + os.sep + "ROMFS"
    if os.path.exists(upper) and os.path.samefile(upper, rt / "romfs"):   # (a case-insensitive folder: the usual one on Windows)
        check("a path written in another case is the same folder (compared by identity, not by name)",
              packs.resolves_inside(str(rt), upper))
    for rel, ok in (("romfs/a.bin", True), ("longshot.exe", True), ("..", False), ("a/../b", False), ("a\\..\\b", False),
                    (".", False), ("", False), ("/x", False), ("romfs/foo:evil", False), ("romfs/foo::$DATA", False)):
        check("in_runtime(%r) is %s" % (rel, ok), packs.in_runtime(str(rt), rel) == ok)


def _pack(base_dir, b, name, meta):
    """A pack folder with this pack.json; returns [name] for an rt command line"""
    p = base_dir / "packs" / name
    (p / "files").mkdir(parents=True, exist_ok=True)
    (p / "pack.json").write_text(json.dumps(dict({"base": b["commit"], "files": {}, "generated": [], "deleted": [], "prepare": False},
                                                 **meta)))
    return [name]


def test_romfs_in_place(d):
    """A pack reads the romfs where it is unless LONGSHOT_PACK_ROMFS=copy; a --romfs-in-place base records it, its preparation
    names the romfs, and its runtimes get no romfs, say the game takes --romfs and record it, so dev.py ab starts it so"""
    check("a pack's build reads the romfs in place", packs.pack_env({}) == {"LONGSHOT_NO_PREPARE": "1", "LONGSHOT_ROMFS_IN_PLACE": "1"})
    check("LONGSHOT_PACK_ROMFS=copy links it", packs.pack_env({"LONGSHOT_PACK_ROMFS": "copy"}) == {"LONGSHOT_NO_PREPARE": "1"})
    ab_source = (ROOT / "scripts" / "devtools" / "ab.py").read_text(encoding="utf-8")
    check("dev.py ab starts a runtime with its romfs", re.search(r"start_session\(\s*s\[\"runtime\"\], romfs=packs\.runtime_romfs\(s\[\"runtime\"\]\)",
                                                             ab_source) is not None)
    if packs.CROSS:
        return
    got, calls = {}, []

    def fake_make_game(tree, b, work, game, env):
        got.update(env)
        for m in packs.MODULES:
            write(Path(game) / m[0], b"built")
            if m[1]:
                write(Path(work) / "sa" / "build" / m[1], b"map")
        write(Path(game) / "cache" / "longshot_programs.bin", b"programs")

    def fake_run(argv, **kw):
        calls.append(list(argv))
        return packs.subprocess.CompletedProcess(argv, 0)
    saved = packs.make_game, packs.changed_files, packs.head, packs.subprocess.run, os.environ.pop("LONGSHOT_NO_PREPARE", None)
    packs.make_game, packs.changed_files, packs.head = fake_make_game, (lambda tree, commit: set()), (lambda tree, *a: "c" * 40)
    packs.subprocess.run = fake_run
    romfs = d / "dump" / "romfs"
    try:
        packs_main(["base", str(d / "b"), "--romfs", str(romfs), "--exefs", "e", "--tree", str(ROOT), "--romfs-in-place"])
        b = json.loads((d / "b" / "base.json").read_text())
        check("the base's build reads the romfs in place (%s)" % got, got.get("LONGSHOT_ROMFS_IN_PLACE") == "1")
        check("base.json says so", b.get("romfs_in_place") is True)
        check("its preparation names the romfs (%s)" % calls,
              len(calls) == 1 and calls[0][-2:] == ["--romfs", os.path.abspath(romfs)])
        rt = d / "rt"
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            packs_main(["rt", str(d / "b"), str(rt)])
        check("its runtime has no romfs", not (rt / "romfs").exists())
        check("...and the base's files", (rt / "cache" / "longshot_programs.bin").read_bytes() == b"programs")
        check("...and says the game takes --romfs (%s)" % out.getvalue(), "--romfs " + os.path.abspath(romfs) in out.getvalue())
        check("...and dev.py ab passes it", packs.runtime_romfs(str(rt)) == os.path.abspath(romfs))
        with contextlib.redirect_stdout(io.StringIO()):
            packs_main(["rt", str(d / "b"), str(rt)])   # (refreshed in place: still no romfs, still recorded)
        check("a refreshed runtime still has none", not (rt / "romfs").exists() and packs.runtime_romfs(str(rt)) == os.path.abspath(romfs))
    finally:
        packs.make_game, packs.changed_files, packs.head, packs.subprocess.run = saved[:4]
        if saved[4] is not None:
            os.environ["LONGSHOT_NO_PREPARE"] = saved[4]
    base_dir, _ = fake_base(d / "plain")
    packs_main(["rt", str(base_dir), str(d / "rt-plain")])
    check("a runtime with its own romfs is started without --romfs", packs.runtime_romfs(str(d / "rt-plain")) is None)


def fake_base(d):
    """A base with a game folder: romfs, both modules, two caches"""
    base_dir, game = d / "base", d / "base" / "game"
    write(game / "romfs" / "a" / "data.bin", b"romfs")
    write(game / packs.GAME, b"base game")
    write(game / packs.RENDERER, b"base renderer")
    write(game / "cache" / "longshot_programs.bin", b"programs")
    write(game / "cache" / "longshot_vk_pipelines.bin", b"pipelines")
    packs.write_base(str(base_dir), {"commit": "c" * 40, "work": str(base_dir / "work"), "game": str(game), "romfs": "r",
                                     "exefs": "e"})
    return base_dir, game


def test_renderer_build_jobs():
    """A pack of the renderer alone in the job queue compiles several files at once: nx2/build.py run by itself compiles one at a
    time, so the command carries --jobs: LONGSHOT_JOBS when it is set, as the game's build takes it, else the CPU threads shared
    among the job queue's slots of its kind. Outside the queue with LONGSHOT_JOBS unset (the Test Farm) it has no --jobs, as
    before"""
    cmd = packs.renderer_build_cmd
    q = {"LONGSHOT_JOB_MODE": "build"}
    check("the renderer is built alone by build.py", cmd(q, 16)[1:4] == ["-I", "nx2/build.py", "--standalone"])
    check("outside the queue, LONGSHOT_JOBS unset: no --jobs", cmd({}, 16)[1:] == ["-I", "nx2/build.py", "--standalone"])
    check("...nor with it empty", "--jobs" not in cmd({"LONGSHOT_JOBS": ""}, 16))
    check("outside the queue, LONGSHOT_JOBS is the compilers at once", cmd({"LONGSHOT_JOBS": "14"}, 16)[-2:] == ["--jobs", "14"])
    check("...and 0 is automatic", cmd({"LONGSHOT_JOBS": "0"}, 16)[-2:] == ["--jobs", "4"])
    check("LONGSHOT_JOBS is the compilers at once", cmd(dict(q, LONGSHOT_JOBS="14"), 16)[-2:] == ["--jobs", "14"])
    check("in the queue, unset: one for every four threads", cmd(q, 16)[-2:] == ["--jobs", "4"])
    check("a pack in one of two build slots: half the threads", cmd(q, 24, slots=2)[-2:] == ["--jobs", "12"])
    check("LONGSHOT_JOBS over the slots", cmd(dict(q, LONGSHOT_JOBS="5"), 24, slots=2)[-2:] == ["--jobs", "5"])
    from unittest import mock
    with mock.patch.object(common, "read_config", lambda name: {}):   # (not this PC's job_queue.json)
        check("outside the queue: four", packs.queue_slots({}) == 4)
        check("a build job: the build slots", packs.queue_slots({"LONGSHOT_JOB_MODE": "build"}) ==
              int(packs.jobqueue.DEFAULTS["LONGSHOT_QUEUE_BUILD_SLOTS"]))
        check("...as the queue's settings say",
              packs.queue_slots({"LONGSHOT_JOB_MODE": "build", "LONGSHOT_QUEUE_BUILD_SLOTS": "3"}) == 3)
        check("a light job: the light slots", packs.queue_slots({"LONGSHOT_JOB_MODE": "light", "LONGSHOT_QUEUE_LIGHT_SLOTS": "6"}) == 6)
        check("a GPU job: four", packs.queue_slots({"LONGSHOT_JOB_MODE": "shared"}) == 4)
    check("0 and empty are automatic too", cmd(dict(q, LONGSHOT_JOBS="0"), 24)[-2:] == ["--jobs", "6"]
          and cmd(dict(q, LONGSHOT_JOBS=""), 8)[-2:] == ["--jobs", "2"])
    check("never none", cmd(q, 1)[-2:] == ["--jobs", "1"] and cmd(q, 3)[-2:] == ["--jobs", "1"])
    for bad in ("many", "-2", "1.5"):
        try:
            cmd({"LONGSHOT_JOBS": bad}, 16)
        except SystemExit as e:
            check("a LONGSHOT_JOBS that is no count is named", "LONGSHOT_JOBS" in str(e))
        else:
            check("LONGSHOT_JOBS=%s is refused" % bad, False)


def test_outputs_before_prepare(d):
    """The base's outputs are what make_game.py made, recorded before the preparation: a file the preparation writes beside the
    game (here one no list names) is never an output, so no pack can delete it; the preparation gets the Compile shaders level"""
    calls = []

    def fake_make_game(tree, b, work, game, env):
        calls.append(("make_game", env.get("LONGSHOT_NO_PREPARE")))
        for m in packs.MODULES:
            write(Path(game) / m[0], b"built")
            if m[1]:
                write(Path(work) / "sa" / "build" / m[1], b"map")
        write(Path(game) / "longshot_shaders.bin", b"seed")

    def fake_run(argv, **kw):
        calls.append(("prepare", list(argv[1:])))
        game = Path(kw["cwd"])
        write(game / "cache" / "longshot_vk_pipelines.stamp", b"stamp")
        write(game / "cache" / "longshot_pipelines.bin.predicted_keys", b"keys")
        write(game / "longshot_some_future_cache.dat", b"a file a later renderer writes")
        return packs.subprocess.CompletedProcess(argv, 0)
    saved = packs.make_game, packs.changed_files, packs.head, packs.subprocess.run
    packs.make_game, packs.changed_files, packs.head = fake_make_game, (lambda tree, commit: set()), (lambda tree, *a: "c" * 40)
    packs.subprocess.run = fake_run
    try:
        packs_main(["base", str(d / "b"), "--romfs", "r", "--exefs", "e", "--tree", str(ROOT)])
    finally:
        packs.make_game, packs.changed_files, packs.head, packs.subprocess.run = saved
    outputs = json.loads((d / "b" / "base.json").read_text())["outputs"]
    check("make_game.py builds without preparing (%s)" % calls, calls and calls[0] == ("make_game", "1"))
    if not packs.CROSS:
        prep = [c for c in calls if c[0] == "prepare"]
        check("the preparation runs once, after the build, with the Compile shaders level (%s)" % prep,
              len(prep) == 1 and prep[0][1][:1] == ["--prepare-shaders"] and "--compile-shaders" in prep[0][1])
        check("what the preparation wrote is not an output (%s)" % outputs,
              not {"cache/longshot_vk_pipelines.stamp", "cache/longshot_pipelines.bin.predicted_keys", "longshot_some_future_cache.dat"}
              & set(outputs))
    check("what make_game.py made is (%s)" % outputs, "longshot_shaders.bin" in outputs)


def test_prepare_outcomes(d):
    """A base whose preparation fails (exit 3: no usable Vulkan) is kept, base.json written with the reason, as make_game.py's
    own preparation leaves the work to the first start; LONGSHOT_NO_PREPARE skips it by value (0 still prepares)"""
    if packs.CROSS:
        return

    def fake_make_game(tree, b, work, game, env):
        for m in packs.MODULES:
            write(Path(game) / m[0], b"built")
            if m[1]:
                write(Path(work) / "sa" / "build" / m[1], b"map")
    for name, env, code, prepares, why in (("vulkan", None, 3, True, "no usable Vulkan here"),
                                           ("oserror", None, OSError("cannot start"), True, "cannot start"),
                                           ("skip", "1", 0, False, "LONGSHOT_NO_PREPARE is set"),
                                           ("zero", "0", 0, True, None)):
        calls = []

        def fake_run(argv, **kw):
            calls.append(argv)
            if isinstance(code, OSError):   # (the game could not start)
                raise code
            if kw.get("check") and code:   # (as subprocess.run does)
                raise packs.subprocess.CalledProcessError(code, argv)
            return packs.subprocess.CompletedProcess(argv, code)
        saved = packs.make_game, packs.changed_files, packs.head, packs.subprocess.run, os.environ.get("LONGSHOT_NO_PREPARE")
        packs.make_game, packs.changed_files, packs.head = fake_make_game, (lambda tree, commit: set()), (lambda tree, *a: "c" * 40)
        packs.subprocess.run = fake_run
        if env is None:
            os.environ.pop("LONGSHOT_NO_PREPARE", None)
        else:
            os.environ["LONGSHOT_NO_PREPARE"] = env
        try:
            packs_main(["base", str(d / name), "--romfs", "r", "--exefs", "e", "--tree", str(ROOT)])
        except (packs.subprocess.CalledProcessError, OSError):
            pass
        finally:
            packs.make_game, packs.changed_files, packs.head, packs.subprocess.run = saved[:4]
            if saved[4] is None:
                os.environ.pop("LONGSHOT_NO_PREPARE", None)
            else:
                os.environ["LONGSHOT_NO_PREPARE"] = saved[4]
        bj = d / name / "base.json"
        check("%s: the base is kept (base.json written)" % name, bj.exists())
        b = json.loads(bj.read_text()) if bj.exists() else {}
        check("%s: the preparation %s (%d runs)" % (name, "runs" if prepares else "is skipped", len(calls)), (len(calls) == 1) == prepares)
        check("%s: base.json says why it was not prepared (%r)" % (name, b.get("not_prepared")), b.get("not_prepared") == why)


def test_seed(d):
    """--seed copies every cache the seed folder's cache/ holds, the play files too (recent pipelines can live only there),
    and nothing else"""
    seed = d / "seed"
    for name, data in (("longshot_vk_pipelines.bin", b"p"), ("longshot_vk_pipelines_play_4.bin", b"play"),
                       ("longshot_vk_pipelines.stamp", b"stamp"), ("longshot_log.txt", b"not a cache")):
        write(seed / "cache" / name, data)
    for name, data in (("longshot_programs.bin", b"loose beside the game: not a cache of this layout"), ("image.bin", b"game")):
        write(seed / name, data)

    def fake_make_game(tree, b, work, game, env):
        for m in packs.MODULES:
            write(Path(game) / m[0], b"built")
            if m[1]:
                write(Path(work) / "sa" / "build" / m[1], b"map")
    saved = packs.make_game, packs.changed_files, packs.head, packs.subprocess.run
    packs.make_game, packs.changed_files, packs.head = fake_make_game, (lambda tree, commit: set()), (lambda tree, *a: "c" * 40)
    packs.subprocess.run = lambda argv, **kw: packs.subprocess.CompletedProcess(argv, 0)
    try:
        packs_main(["base", str(d / "b"), "--romfs", "r", "--exefs", "e", "--tree", str(ROOT), "--seed", str(seed)])
    finally:
        packs.make_game, packs.changed_files, packs.head, packs.subprocess.run = saved
    game = d / "b" / "game"
    for name in ("longshot_vk_pipelines.bin", "longshot_vk_pipelines_play_4.bin", "longshot_vk_pipelines.stamp"):
        check("the seed's cache/%s is copied into cache/" % name, (game / "cache" / name).exists())
    for name in ("longshot_programs.bin", "image.bin", "cache/longshot_log.txt"):
        check("the seed's %s is not" % name, not (game / name).exists())


def test_warm(d):
    """warm plays a runtime with no pack, copies back the caches play made or grew and nothing else, and deletes the runtime"""
    base_dir, game = fake_base(d)
    play = d / "play.py"
    play.write_text("import sys, os\n"
                    "rt = sys.argv[1]\n"
                    "open(os.path.join(rt, 'cache', 'longshot_programs.bin'), 'ab').write(b' and more')\n"
                    "open(os.path.join(rt, 'cache', 'longshot_vk_pipelines_play_1.bin'), 'wb').write(b'played')\n"
                    "os.makedirs(os.path.join(rt, 'logs'), exist_ok=True)\n"
                    "open(os.path.join(rt, 'logs', 'longshot_log.txt'), 'w').write('a log')\n"
                    "open(os.path.join(rt, 'cache', 'longshot_log.txt'), 'w').write('not a cache either')\n"
                    "open(os.path.join(rt, %r), 'wb').write(b'not a cache')\n" % packs.GAME)
    cmd = '"%s" -I "%s" "{runtime}"' % (sys.executable.replace("\\", "/"), str(play).replace("\\", "/"))
    packs_main(["warm", str(base_dir), "--run", cmd])
    check("a grown cache comes back", (game / "cache" / "longshot_programs.bin").read_bytes() == b"programs and more")
    check("a new play file comes back", (game / "cache" / "longshot_vk_pipelines_play_1.bin").read_bytes() == b"played")
    check("an unchanged cache stays", (game / "cache" / "longshot_vk_pipelines.bin").read_bytes() == b"pipelines")
    check("a log is not a cache", not (game / "logs").exists() and not (game / "cache" / "longshot_log.txt").exists())
    check("a module is never copied back", (game / packs.GAME).read_bytes() == b"base game")
    check("the warm-up's runtime is gone", not (base_dir / "warm-runtime").exists())
    b = json.loads((base_dir / "base.json").read_text())
    check("base.json records the warm-up (%s)" % b.get("warmed"),
          b.get("warmed", {}).get("files") == ["longshot_programs.bin", "longshot_vk_pipelines_play_1.bin"])
    check("the busy marker is gone after the job", not (base_dir / packs.BUSY).exists())
    packs_main(["rt", str(base_dir), str(base_dir / "warm-runtime")])
    packs_main(["warm", str(base_dir), "--run", cmd])
    check("a runtime made beforehand by rt is warmed and deleted", not (base_dir / "warm-runtime").exists())
    packs_main(["rt", str(base_dir), str(base_dir / "warm-runtime")])
    p = base_dir / "packs" / "x"
    write(p / packs.GAME, b"pack game")
    (p / "pack.json").write_text(json.dumps({"base": "c" * 40, "files": {packs.GAME: "h"}, "generated": [], "deleted": [],
                                            "prepare": False}))
    (base_dir / "warm-runtime" / "dev_packs.json").write_text(json.dumps({"base": "c" * 40, "packs": ["x"]}))
    refused("warming a runtime with a pack in it", ["warm", str(base_dir), "--run", cmd], "not this base's runtime with no pack")
    shutil.rmtree(base_dir / "warm-runtime")
    # (a cache in the runtime that is a link to a file outside it is never followed: its bytes stay out of the base)
    outside = d / "outside.bin"
    outside.write_bytes(b"not a cache of this runtime")
    packs_main(["rt", str(base_dir), str(base_dir / "warm-runtime")])
    lnk = base_dir / "warm-runtime" / "cache" / "longshot_vk_pipelines_play_7.bin"
    try:
        os.symlink(outside, lnk)
    except OSError:
        lnk = None
    if lnk is None:
        print("  skipped: a file link in the warm-up's runtime (this account cannot make one)")
        shutil.rmtree(base_dir / "warm-runtime")
    else:
        packs_main(["warm", str(base_dir), "--run", '"%s" -c "pass"' % sys.executable.replace("\\", "/")])
        check("a linked cache file is not copied back", not (game / "cache" / "longshot_vk_pipelines_play_7.bin").exists())


def test_busy(d):
    """A second job on a busy base is refused, and the other job's marker is left alone"""
    base_dir, game = fake_base(d)
    (base_dir / packs.BUSY).write_text("1234 pack 2026-11-12T12:00:00Z")
    refused("a runtime while the base is busy", ["rt", str(base_dir), str(d / "rt-x")], "busy")
    refused("a warm-up while the base is busy", ["warm", str(base_dir), "--run", "x"], "busy")
    check("nothing made", not (d / "rt-x").exists() and not (base_dir / "warm-runtime").exists())
    check("the other job's marker is untouched", (base_dir / packs.BUSY).read_text() == "1234 pack 2026-11-12T12:00:00Z")


def test_busy_folders_above_hidden(d):
    """The Beanstalk agent's Windows sandbox may open the base's folder but not see the folders above it (they look missing,
    and making one is refused): taking and leaving the busy marker, and making a folder inside, still work"""
    base = (d / "granted" / "base").resolve()
    base.mkdir(parents=True)
    real_exists, real_isdir, real_mkdir = os.path.exists, os.path.isdir, os.mkdir

    def inside(p):
        p = os.path.abspath(p)
        return p == str(base) or p.startswith(str(base) + os.sep)

    def exists(p):
        return real_exists(p) if inside(p) else False

    def isdir(p):
        return real_isdir(p) if inside(p) else False

    def mkdir(p, *args, **kwargs):
        if not inside(p):
            raise PermissionError(13, "Access is denied", p)
        return real_mkdir(p, *args, **kwargs)

    os.path.exists, os.path.isdir, os.mkdir = exists, isdir, mkdir
    try:
        with packs.Busy(str(base), "pack"):
            check("the marker is taken", real_exists(base / packs.BUSY))
        common.ensure_dir(str(base))
        common.ensure_dir(str(base / "new"))
    finally:
        os.path.exists, os.path.isdir, os.mkdir = real_exists, real_isdir, real_mkdir
    check("and left", not (base / packs.BUSY).exists())
    check("a folder inside is still made", (base / "new").is_dir())


def main():
    test_route()
    test_not_built()
    test_renderer_files_the_game_reads()
    test_stamped_objects_match_build()
    test_caches_named_in_renderer()
    test_inputs_beside_the_objects()
    test_renderer_untouched()
    test_renderer_build_jobs()
    with tempfile.TemporaryDirectory(prefix="longshot-dev-packs-") as d:
        d = Path(d)
        for name, t in (("mc", test_module_changed), ("pf", test_pack_folders), ("rt", test_generated_and_runtime),
                        ("guard", test_guards), ("inside", test_resolves_inside), ("outputs", test_outputs_before_prepare), ("prepare", test_prepare_outcomes), ("seed", test_seed), ("warm", test_warm), ("busy", test_busy), ("hidden", test_busy_folders_above_hidden),
                        ("stamp", test_stamp_matches_build), ("format", test_cache_format), ("clone", test_clone),
                        ("refresh", test_rt_refresh), ("in-place", test_romfs_in_place)):
            (d / name).mkdir()
            t(d / name)
    print("test_dev_packs: ok")


if __name__ == "__main__":
    main()
