"""A development build as a base and small packs over it, in a base folder you name, without the job queue (`dev.py
packs`): build the game once, then build only what a change touches and run it from a copy of that base. For development
and A/B tests only: it runs nx2/make_game.py and nx2/build.py as they are and changes nothing in what players build
(docs/dev/developing.md, *Test a change against a base*). `dev.py base`, `pack` and `rt` (build.py) run these on the
PC's shared bases through the queue; the Test Farm runs them on its own, so their command lines and files stay as they are.

    dev.py packs base DIR --romfs R --exefs E [--tree T] [--seed GAME] [--romfs-in-place]
        Build the checkout T (default this one; a commit: no uncommitted or untracked sources) into DIR/game, keeping its
        work folder DIR/work. --seed copies the caches of an earlier game folder first, as a player's update keeps them:
        they are stamped by the shader translator, so the preparation redoes only what changed. --romfs-in-place: the game
        folder gets no romfs (make_game.py --romfs-in-place), nor do its runtimes; the game is started with --romfs R
        (where a hard link is refused, as in a sandbox that may only read the dump, the link would be a full copy).
    dev.py packs pack DIR NAME [--tree T]
        Build a change (a checkout whose history holds the base's commit; committed, uncommitted and untracked files all
        count) into DIR/packs/NAME: renderer files the game is not built from build the renderer alone (RX_VK_ONLY: no
        lift); docs, checks and developer tools no build reads nothing (route); anything else builds the game with
        make_game.py in one work folder every pack of the base shares, so the lift is skipped while the translator and the
        hot list stay the same, and without the renderer when the change cannot reach it (renderer_untouched:
        RX_GAME_ONLY); it reads the romfs where it is (pack_env; LONGSHOT_PACK_ROMFS=copy links it, for a change that
        writes into the romfs). The pack keeps a module when an object it is linked from differs from the base's
        (module_changed), and every other file of the game folder that differs.
    dev.py packs rt DIR RUNTIME [PACK...]
        A game folder to run: the base's (romfs hard-linked, none for a --romfs-in-place base; the rest copied, as clones where the file system has them: the
        game writes its caches), the packs' files over it, the shaders prepared again when a pack changed the translator,
        the program cache's format (kProgramCacheFormat) or the shader seed. A runtime this made before is refreshed in
        place: only the files that differ are replaced,
        the ones no longer wanted removed, and its caches, logs, saves and settings kept.
    dev.py packs warm DIR --run "CMD with {runtime}" [--run ...]
        Play a runtime with no pack once per command (split as a POSIX shell would), copy the caches play made or grew
        into the base's game folder and delete the runtime: every later runtime starts warm for those scenes.

A pack's modules come from the same base as the rest of the runtime, so the game and the renderer always match
(MKWVK_API_VERSION). Two packs that bring the same file are refused: build one pack of both branches. base, pack, rt and
warm hold DIR/dev_packs.busy while they run, so a second job on a base is refused. That marker, base.json, pack.json and
a runtime's dev_packs.json keep their names and fields: bases are shared with older checkouts, and the Test Farm reads them.
"""
import argparse
import ast
import hashlib
import importlib.util
import json
import os
import re
import shlex
import shutil
import tempfile
import subprocess
import sys
import time

from . import common, jobqueue, vcs

ROOT = common.ROOT
# (the modules' names follow the target, not this PC: RX_TARGET=windows on Linux builds the Windows game)
TARGET_WINDOWS = os.name == "nt" or os.environ.get("RX_TARGET", "").strip().lower() == "windows"
CROSS = TARGET_WINDOWS and os.name != "nt"   # (the Windows game built on Linux: it cannot run here, so it prepares on the PC that plays it)
# (the game's own caches, in its game folder's cache/ (gpu/vk/vk_paths.h cache_path), kept between runs and stamped by the
# translator (gpu/vk/vk_progcache.cpp): a stale one is not used, so a base can start from an older base's. All of them:
# the program cache plain or compressed and its index, its modules' ids (vk_programs.cpp), the pipeline notes, predicted
# (with their keys) and built (vk_draw.cpp), the compute pipelines and their variants, the state lists, the prepare's
# record, and the driver's pipeline cache with its stamp (pcache_stamp.h). tests/test_dev_packs.py fails on a cache the
# renderer names that is not here)
CACHE_DIR = "cache"
CACHES = ("longshot_programs.bin", "longshot_programs.idx", "longshot_programs.zst.bin", "longshot_programs.zst.idx",
          "longshot_module_ids.bin", "longshot_pipelines.bin", "longshot_pipelines.bin.predicted", "longshot_pipelines.bin.predicted_keys",
          "longshot_pipelines.bin.built",
          "longshot_computes.bin", "longshot_compute_variants.bin", "longshot_pipeline_states.bin",
          "longshot_pipeline_states_v1.bin", "longshot_prepared.bin", "longshot_vk_pipelines.bin", "longshot_vk_pipelines.stamp",
          # (the size of the cache's one translator's pipelines, which bounds what another translator carries: pcache_stamp.h)
          "longshot_vk_pipelines.base",
          # (the textures a draw has sampled, for RX_VK_TEX_AHEAD: gpu/vk/texture_ahead.h)
          "longshot_textures_drawn.bin",
          # (AMD FSR 4's unrolled layers' own pipeline cache: gpu/vk/vk_fsr4.cpp own_cache)
          "longshot_fsr4_pipelines.bin")
PLAY_FILES = re.compile(r"longshot_vk_pipelines_play_[0-9]+\.bin")   # (what play adds to the pipeline cache: gpu/vk/vk_draw.cpp play_path)
# (what the game writes into its folder at run time, never a build's output: the caches (and the button prompts it makes) in
# cache/, the logs in logs/ (host/game_layout.c)
RUNTIME_DIRS = (CACHE_DIR + "/", "logs/")
BUSY = "dev_packs.busy"   # (in a base's folder while a job builds, warms or copies from it)
SEED = "longshot_shaders.bin"   # (the shader list the prepare goes through: a new one is prepared again)
# (module, its map, its link list in sa/build, the object that holds the version: nx2/build.py gives host/version.c and
# gpu/vk/vk_version.cpp nx2/version.py's describe(), a fingerprint of every source, so those two objects differ in every build
# of another tree; STAMPED_SOURCES are their sources, in the same order)
MODULES = ((("longshot.exe", "longshot.map", "objs.rsp", "host_version.o"),
            ("longshot_vk.dll", "longshot_vk.map", "vk_objs.rsp", "vk_vk_version.o")) if TARGET_WINDOWS else
           (("longshot", None, "objs.rsp", "host_version.o"), ("longshot_vk.so", None, "vk_objs.rsp", "vk_vk_version.o")))
GAME, RENDERER = MODULES[0][0], MODULES[1][0]
STAMPED_SOURCES = ("host/version.c", "gpu/vk/vk_version.cpp")
# (host/ files the renderer's build compiles itself: nx2/build.py's vk_jobs. A change to one, or to any header, needs the renderer)
RENDERER_HOST = ("host/posix/", "host/pgo_rt.c", "host/mingw_msvc_shims.c")
# (nx2/build.py's shader stamp is made of the files the checkout's own nx2/translator_files.py names: translator_changed)
TRANSLATOR_FILES = "nx2/translator_files.py"
RENDERER_ONLY = ("gpu/vk/", "third_party/imgui/", "third_party/fidelityfx", "third_party/xess/")
INCLUDE = re.compile(r'#\s*include\s+"([^"]+)"')
NO_MODULE = ("docs/", "tests/", ".github/", ".forgejo/", ".claude/")   # (.forgejo: Longshot's workflows and templates)
# (what else no build reads: CI's settings, the website, the tasks, nx2/'s checks (nx2/test_*.py) and the developer tools that
# neither the build nor the environment dev.py gives it runs or opens. tests/test_dev_packs.py test_not_built checks that no file
# of the build names one)
NOT_BUILT = (".circleci/", ".chunk/", "website/", "mise.toml", "mise.lock", "cliff.toml", ".pre-commit-config.yaml", ".gitignore",
             "scripts/profiling/", "scripts/tours/", "scripts/screenshots/", "scripts/coverage/", "scripts/devtools/home.py",
             "scripts/devtools/procs.py", "scripts/perf.py", "scripts/shaders.py", "scripts/flash.py", "scripts/longshot_ctl.py",
             "scripts/longshot_bridge.py", "scripts/longshot_saves.py", "scripts/memtour.py", "scripts/pipeline_tour.py",
             "scripts/setup.py", "scripts/builder_package.py", "scripts/validate_spirv.py",
             "scripts/check_guide_screenshots.py", "scripts/check_pr_description.py", "scripts/check_third_party.py",
             "scripts/check_windows_files.py", "scripts/run-vk.ps1", "scripts/fetch_commit.sh", "scripts/release_keys.json")
HEADERS = (".h", ".hpp", ".hh", ".inc", ".inl")
SOURCES = (".c", ".cc", ".cpp") + HEADERS
# (what the game writes beside itself while it runs, never a build's output: settings, logs and reports, and the markers of a
# skipped preparation and of a slow load (vk_main.cpp, vk_draw.cpp); not seeded either)
WRITTEN_IN_PLAY = ("longshot_options.ini", "longshot_log.txt", "longshot_log.1.txt", "longshot_driver_faults.txt",
                   "longshot_prepared.bin.warming", "longshot_pipelines.bin.predicting", "longshot_pipelines.bin.running",
                   "longshot_pipelines.bin.built.warming", "longshot_prepare_skipped", "longshot_load_start.txt",
                   "longshot_load_start.txt.waited", "longshot_pipeline_report.txt", "longshot_frames.csv",
                   "longshot_damaged_files.txt")


def translator_changed(tree, changed):
    """Whether the shader stamp of the checkout tree's build differs from its base's, given the files changed since (paths
    relative to the checkout, / separated): a stamped file, or what says which files are stamped. A checkout too old to
    name them prepares again"""
    path = os.path.join(tree, *TRANSLATOR_FILES.split("/"))
    if not os.path.isfile(path):
        return True
    spec = importlib.util.spec_from_file_location("translator_files_for_packs", path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return any(m.stamped(p) or p == TRANSLATOR_FILES for p in changed)


PROGRAM_CACHE = "gpu/vk/vk_progcache.cpp"
CACHE_FORMAT = re.compile(r"constexpr\s+uint32_t\s+kProgramCacheFormat\s*=\s*(\d+)")


def cache_format_changed(tree, commit, changed):
    """Whether the checkout's program cache format (kProgramCacheFormat, which the game's program_cache_stamp holds beside the
    translator's stamp) differs from the base commit's: the caches a runtime copies from the base are then dropped at its first
    start, so it prepares again as a translator change does"""
    if PROGRAM_CACHE not in changed:
        return False
    try:
        old = vcs.show(tree, commit, PROGRAM_CACHE)
    except (subprocess.CalledProcessError, OSError):
        old = ""
    try:
        with open(os.path.join(tree, *PROGRAM_CACHE.split("/")), encoding="utf-8") as f:
            new = f.read()
    except OSError:
        new = ""
    def fmt(text):
        m = CACHE_FORMAT.search(text)
        return m.group(1) if m else None
    return fmt(old) != fmt(new)


def load_make_game(root=ROOT):
    spec = importlib.util.spec_from_file_location("make_game_for_packs", os.path.join(root, "nx2", "make_game.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def renderer_read_by_game(root):
    """The renderer's files the game is built from as well: what host/ includes from gpu/, what nx2/build.py adds to the game's
    dependencies from gpu/vk, and what those include in turn. A change there needs both modules"""
    todo = set()
    for top, _, fs in os.walk(os.path.join(root, "host")):
        for f in fs:
            if f.endswith((".c", ".h", ".cpp")):
                with open(os.path.join(top, f), encoding="utf-8", errors="replace") as src:
                    todo.update(os.path.normpath(os.path.join(top, inc)) for inc in INCLUDE.findall(src.read()) if "gpu/" in inc)
    with open(os.path.join(root, "nx2", "build.py"), encoding="utf-8") as src:
        todo.update(os.path.join(root, "gpu", "vk", f) for f in re.findall(r'host_deps \+= open\(os\.path\.join\(GPU, "vk", "([^"]+)"\)', src.read()))
    found = set()
    while todo:
        p = todo.pop()
        rel = os.path.relpath(p, root).replace(os.sep, "/")
        if rel in found or not os.path.isfile(p):
            continue
        found.add(rel)
        with open(p, encoding="utf-8", errors="replace") as src:
            todo.update(os.path.normpath(os.path.join(os.path.dirname(p), inc)) for inc in INCLUDE.findall(src.read()))
    return found


def built_paths(paths):
    """The paths a build reads: not docs, checks, CI's files or the developer tools (NO_MODULE, NOT_BUILT)"""
    return [p for p in paths if not (p.startswith(NO_MODULE + NOT_BUILT) or ("/" not in p and p.endswith(".md"))
                                     or re.fullmatch(r"nx2/test_\w+\.py", p))]


def route(paths, root=ROOT):
    """How a change is built: None (nothing a module or the game folder is made from), "renderer" (the renderer alone) or "game"
    (make_game.py, both modules and the game folder; renderer_untouched says when the renderer can stay the base's)"""
    paths = built_paths(paths)
    if not paths:
        return None
    if all(p.startswith(RENDERER_ONLY) for p in paths) and not set(paths) & renderer_read_by_game(root):
        return "renderer"
    return "game"


def head(tree):
    """The commit a checkout is on (vcs.commit: jj's @, or @- when @ is empty)"""
    return vcs.commit(tree)[0]


def changed_files(tree, commit):
    """The files a checkout changed since a commit: in its commits, not committed, and new ones not ignored"""
    return vcs.changed_files(tree, commit)


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 22), b""):
            h.update(b)
    return h.hexdigest()


def same_file(a, b):
    try:
        return os.path.samefile(a, b) or (os.path.getsize(a) == os.path.getsize(b) and sha(a) == sha(b))
    except OSError:
        return False


def link_list(build, rsp):
    """The objects a module was linked from, by name (nx2/build.py's response file)"""
    with open(os.path.join(build, rsp), encoding="utf-8") as f:
        return {os.path.basename(line.strip().strip('"')): line.strip().strip('"') for line in f if line.strip()}


def module_changed(base_build, build, rsp, stamped, stamped_may_differ):
    """Whether a module differs from the base's beyond the version it names: another set of objects, an object that differs, or
    the object that holds the version when it may differ for more than that (stamped_may_differ: its source or a header
    changed). Its bytes cannot say it: the version (a fingerprint of all the sources) moves the rest of that object's code. True
    when the base kept no link list to compare with"""
    try:
        mine, theirs = link_list(build, rsp), link_list(base_build, rsp)
    except OSError:
        return True
    if set(mine) != set(theirs):
        return True
    return any(not same_file(mine[o], theirs[o]) for o in mine if o != stamped or stamped_may_differ)


def stamped_may_differ(paths, source):
    """Whether the object holding the version may differ for more than the version: its own source changed (it includes no
    header, so nothing else reaches it; a flag does through link_inputs_changed)"""
    return source in paths


def renderer_untouched(paths, lift_files, environ):
    """Whether a game pack's renderer would be the base's, so the pack builds the game alone (RX_GAME_ONLY): every file the
    change reaches the build with is a host/ C source the renderer's build does not compile (RENDERER_HOST), or a file of the
    lift that is not a header (the renderer is built without the game: RX_VK_ONLY), and no switch of the pack reaches the
    renderer (RX_VK_PGO; LONGSHOT_TRACY's zones). Anything else (a header, gpu/, a build script) builds both. The caller
    rules out a change link_inputs_changed names: build.py's own scripts can be the lift's too"""
    if environ.get("RX_VK_PGO") or environ.get("LONGSHOT_TRACY", "0") not in ("", "0"):
        return False
    return all((p.startswith("host/") and p.endswith(".c") and not p.startswith(RENDERER_HOST))
               or (p in lift_files and not p.endswith(HEADERS)) for p in built_paths(paths))


def build_scripts(root):
    """nx2/build.py and every Python module it imports (as make_game.lift_sources finds the lift's: a64-kit's folders, then nx2/),
    and nx2/make_game.py, which sets build.py's environment (the hot order, the profile)"""
    kit = os.path.join(root, "third_party", "a64-kit")
    path = [os.path.join(kit, d) for d in ("rt", "hos", "tools", "lift")] + [os.path.join(root, "nx2")]
    todo, seen = [os.path.join(root, "nx2", "build.py")], set()
    while todo:
        f = todo.pop()
        if f in seen:
            continue
        seen.add(f)
        with open(f, "rb") as src:
            tree = ast.parse(src.read(), f)
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                names = [n.name for n in node.names]
            elif isinstance(node, ast.ImportFrom) and node.module and not node.level:
                names = [node.module]
            else:
                continue
            for name in names:
                for d in path:
                    m = os.path.join(d, name.split(".")[0] + ".py")
                    if os.path.isfile(m):
                        todo.append(m)
                        break
    return {os.path.relpath(f, root).replace(os.sep, "/") for f in seen} | {"nx2/make_game.py"}


def link_inputs_changed(paths, lift_files, build_files):
    """A change that reaches the modules besides their objects, which the link lists do not show: the build's own scripts
    (build_files: flags, link options, the Windows manifest, tools such as a64-kit's toolchain.py even when the lift reads them
    too), a data file under nx2/ that is not the translator's, or a file under host/, gpu/ or third_party/ that is not C or C++
    (libraries, resources, shader sources). The pack then keeps both modules. Other nx2/ Python (the lift's, make_game.py's)
    shows in the objects or in the game folder's files"""
    def reaches(p):
        if p in build_files:
            return True
        if p.startswith("nx2/"):
            return not p.endswith(".py") and p not in lift_files
        return p.startswith(("host/", "gpu/", "third_party/")) and not p.endswith(SOURCES) and p not in lift_files
    return any(reaches(p) for p in paths)


def build_outputs(game):
    """The files a build made in a new game folder, outside romfs: the base's, recorded when it is built, are the only files a
    pack may find gone (a played folder also holds saves, settings and other files no build makes)"""
    skip = {m for mod in MODULES for m in mod[:2] if m} | set(CACHES) | set(WRITTEN_IN_PLAY)
    out = []
    for top, dirs, fs in os.walk(game):
        dirs[:] = sorted(x for x in dirs if not (top == game and x == "romfs"))
        for f in sorted(fs):
            rel = os.path.relpath(os.path.join(top, f), game).replace(os.sep, "/")
            if rel not in skip and not rel.endswith(".log") and not runtime_file(rel):   # (play caches too: a seed brings them)
                out.append(rel)
    return out


def generated_changes(base_game, game, base_outputs=None):
    """The files of a built game folder that differ from the base's, besides the modules, their maps and the caches: what the
    build generates (the shader seed, the shipped pipeline list, button prompts, files it would write into romfs; romfs files the
    same as the base's are its hard links, so only new ones are read), and the base's files outside romfs that this build did not
    make, of those its own build made (base_outputs; none when the base did not record them)"""
    skip = {m for mod in MODULES for m in mod[:2] if m} | set(CACHES)
    changed = []
    for top, dirs, fs in os.walk(game):
        dirs.sort()
        for f in sorted(fs):
            p = os.path.join(top, f)
            rel = os.path.relpath(p, game).replace(os.sep, "/")
            if rel in skip or runtime_file(rel):
                continue
            q = os.path.join(base_game, rel)
            if not (os.path.isfile(q) and same_file(p, q)):
                changed.append(rel)
    # (never a cache, even in an outputs list a base recorded before the caches' family was complete)
    gone = [rel for rel in base_outputs or ()
            if not is_cache(rel) and not runtime_file(rel) and rel not in WRITTEN_IN_PLAY
            and not os.path.isfile(os.path.join(game, rel))]
    return changed, gone


def kept_outputs(mg, work, romfs, game):
    """The files of a pack's game folder clear_output keeps, because the next build makes them again or checks them itself:
    the two modules and image.bin, which every build stages and make_game.py copies again unless its note
    (INSTALLED_NOTE, kept with them) says the folder's copy has the new one's bytes, and the shader list when make_game.py
    would keep it too (shader_list_current: made from the same game files and tools). A 460 MB exe, image.bin and a 500 MB list
    are then not copied again for every pack. mg: the packed checkout's make_game module"""
    keep = {GAME, RENDERER, "image.bin"}
    if getattr(mg, "INSTALLED_NOTE", None):
        keep.add(mg.INSTALLED_NOTE)
    if hasattr(mg, "shader_list_current") and mg.shader_list_current(work, romfs, os.path.join(game, SEED)):
        keep.add(SEED)
    return keep


def clear_output(game, keep=()):
    """A game folder that will hold only the next build's output: everything but romfs (hard links, rebuilt for nothing otherwise)
    and the top-level files named in keep (kept_outputs) goes, so a file an earlier pack's build made and this one does not is
    not taken for this change's"""
    for name in os.listdir(game) if os.path.isdir(game) else ():
        p = os.path.join(game, name)
        if name == "romfs" or (name in keep and os.path.isfile(p) and not os.path.islink(p)):
            continue
        if os.path.isdir(p) and not os.path.islink(p):
            shutil.rmtree(p)
        else:
            os.remove(p)


def object_store():
    """(nx2/object_store.py) The object store every base and pack on this PC shares, unless LONGSHOT_OBJ_STORE names another (or
    is empty or 0: none): a new base compiles only what no earlier one did. In the development folder, beside the bases; on
    another drive than the bases its objects are copied instead of linked, so set LONGSHOT_OBJ_STORE to a folder on theirs"""
    return common.dev_path("stores/objects")


def lift_store():
    """(nx2/lift_store.py) The translations every base and pack on this PC share, unless LONGSHOT_LIFT_STORE names another (or is
    empty or 0: none): a new base or a first pack translates the game only when the translator, the hot list or the game
    changed. Copies are clones where the file system has them (btrfs, XFS), full copies elsewhere"""
    return common.dev_path("stores/lifts")


def store_env():
    """The stores' variables, as a build takes them unless they are set"""
    return {"LONGSHOT_OBJ_STORE": object_store(), "LONGSHOT_LIFT_STORE": lift_store()}


def run(cmd, cwd, env=None):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=cwd, check=True, env=dict(store_env(), **dict(os.environ, **(env or {}))))


def queue_slots(environ):
    """How many jobs of this one's kind the job queue runs at once (LONGSHOT_JOB_MODE: a pack holds one of its build slots,
    `dev.py vk` one of its light ones); 4 outside the queue"""
    mode = environ.get("LONGSHOT_JOB_MODE")
    if mode not in ("build", "light"):
        return 4
    return max(1, jobqueue.slots_now(jobqueue.settings(environ), roomy=False)[mode])


def renderer_build_cmd(environ, cpus, python=sys.executable, slots=4):
    """The build of the renderer alone, with its compilers at once: nx2/build.py run by itself compiles one file at a time
    (make_game.py passes it --jobs), and a change to a header every renderer file includes compiles all of them. LONGSHOT_JOBS
    when it is set, as the game's build takes it (0 or empty: automatic); else the CPU threads shared among the slots of its
    kind the job queue runs at once (queue_slots). Outside the queue (LONGSHOT_JOB_MODE unset, as on the Test Farm) with
    LONGSHOT_JOBS unset: no --jobs, build.py's own default. build.py still starts each compiler only when the free memory
    holds it. The count is in no object key: the objects are the base's"""
    asked = (environ.get("LONGSHOT_JOBS") or "").strip()
    if not asked and not environ.get("LONGSHOT_JOB_MODE"):
        return [python, "-I", "nx2/build.py", "--standalone"]
    if asked and not asked.isdigit():
        raise SystemExit("LONGSHOT_JOBS must be a whole number: use 1 or more, or 0 for automatic.")
    jobs = int(asked or 0) or max(1, cpus // max(1, slots))
    return [python, "-I", "nx2/build.py", "--standalone", "--jobs", str(jobs)]


def renderer_profile(base_work, environ):
    """The renderer's play profile for a pack of the renderer alone: the one make_game.py gave the base's build, by the same
    path, so the base's objects are found again, since nx2/build.py run by itself uses none and the pack's renderer would then
    differ from the base's by more than the change. That is <base work>/longshot_vk.profdata when the base's checkout had its
    own, else the committed one make_game.py unpacked there (longshot_vk.shipped.profdata). Nothing when the base had none,
    or when RX_VK_PGO is set (a counting pack, or another profile)"""
    if environ.get("RX_VK_PGO"):
        return {}
    for name in ("longshot_vk.profdata", "longshot_vk.shipped.profdata"):
        prof = os.path.join(base_work, name)
        if os.path.isfile(prof):
            return {"RX_VK_PGO": prof}
    return {}


def renderer_env(work, base_work, environ):
    """The variables of a build of the renderer alone in work, with base_work's play profile (renderer_profile)"""
    return dict({"OOT_WORK": work, "RX_RELEASE": "1", "RX_VK_ONLY": "1"}, **renderer_profile(base_work, environ))


def pack_route(how, environ):
    """What a pack builds: route()'s answer for the changed files, except that a profile switch set for this pack makes it build
    even when no file changed. RX_VK_PGO (gen: a renderer that counts, for recording its profile; or another profile) builds
    the renderer; RX_PGO (the game's hot pieces) and LONGSHOT_TRACY (its client is in the game's executable) the game"""
    if environ.get("RX_PGO") or environ.get("LONGSHOT_TRACY", "0") not in ("", "0"):
        return "game"
    if environ.get("RX_VK_PGO") and how is None:
        return "renderer"
    return how


PREPARE_FAILED = {3: "no usable Vulkan here", 4: "the GPU driver stopped responding"}   # (make_game.py prepare_shaders' codes)


def prepare(game, b, required=True):
    """The game's own preparation in a game folder, as make_game.py runs it (the game's environment, without the build's
    switches); nothing on a Windows game built on Linux, which prepares at its first start. required:
    a failure stops (a runtime); otherwise (a base) it leaves the work to the first start, as make_game.py's own preparation
    does. Returns why the shaders were not prepared now, or None"""
    if CROSS:
        why = "a Windows game built on Linux"
        print("the shaders were not prepared now (%s): the game prepares them at its first start" % why, flush=True)
        return why
    mg = load_make_game()
    # (make_game.py's Compile shaders level, as its own preparation passes it: the folder's choice, else Most)
    cmd = [os.path.join(game, GAME), "--prepare-shaders", "--compile-shaders", mg.folder_compile_level(game) or "most"]
    if b.get("romfs_in_place") and not os.path.isdir(os.path.join(game, "romfs")):   # (a --romfs-in-place base, its runtimes)
        cmd += ["--romfs", b["romfs"]]
    print("+ " + " ".join([GAME] + cmd[1:]), flush=True)
    try:
        code = subprocess.run(cmd, cwd=game, check=required, env=mg.game_env()).returncode
    except OSError as e:   # (the game could not start: a missing DLL, launch denied)
        if required:
            raise
        code, why = None, str(e)
    if code:
        why = PREPARE_FAILED.get(code, "the game stopped with exit status %d" % code)
    if code != 0:
        print("the shaders were not prepared now (%s): the game prepares them at its first start" % why, flush=True)
        return why
    return None


ROMFS_IN_PLACE_ENV = "LONGSHOT_ROMFS_IN_PLACE"   # (make_game.py --romfs-in-place)


def pack_env(environ):
    """A pack's make_game.py environment: no preparation, and the romfs read where it is (no copy or links in its game
    folder, so nothing of it is compared with the base) unless LONGSHOT_PACK_ROMFS=copy (a change that writes into the
    romfs, which the in-place build may only read)"""
    env = {"LONGSHOT_NO_PREPARE": "1"}
    if environ.get("LONGSHOT_PACK_ROMFS", "") != "copy":
        env[ROMFS_IN_PLACE_ENV] = "1"
    return env


def runtime_romfs(rt):
    """The romfs a runtime's game is started with (--romfs), or None when the runtime holds its own"""
    made = read_runtime(rt) or {}
    romfs = made.get("romfs")
    return romfs if isinstance(romfs, str) and not os.path.isdir(os.path.join(rt, "romfs")) else None


def make_game(tree, b, work, game, env):
    run([sys.executable, "-I", "nx2/make_game.py", "--romfs", b["romfs"], "--exefs", b["exefs"], "--work", work, "--out", game,
         "--language", b.get("language", "en-US")], tree, dict(env, LONGSHOT_KEEP_BUILD_FILES="1"))


def read_base(d):
    with open(os.path.join(d, "base.json"), encoding="utf-8") as f:
        return json.load(f)


def write_base(d, b):
    if not all(os.path.isfile(os.path.join(b["game"], m[0])) for m in MODULES):   # (make_game.py's launcher layout: game/bin/)
        sys.exit("%s has no %s and %s beside each other: a base is a plain game folder (not a launcher's, with bin/)" %
                 (b["game"], GAME, RENDERER))
    b["modules"] = {m[0]: sha(os.path.join(b["game"], m[0])) for m in MODULES}
    with open(os.path.join(d, "base.json"), "w", encoding="utf-8") as f:
        json.dump(b, f, indent=1)


def pack_folders(d):
    """Where a base's packs are built: one work folder for the game and one for the renderer, the same for every pack. A translation
    is kept in the work folder it was made in (make_game.lift_key; LONGSHOT_LIFT_STORE shares it with other folders): a game pack
    after the first on a base skips the lift while the translator, the HLE list and the hot list are the base's. One pack builds at
    a time"""
    return {"game_work": os.path.join(d, "game-work"), "game_out": os.path.join(d, "game-out"),
            "renderer_work": os.path.join(d, "renderer-work")}


def overlaps(a, b):
    """One folder is the other or holds it, through links and junctions too (folders on two drives never do)"""
    a, b = (os.path.normcase(os.path.realpath(x)) for x in (a, b))
    try:
        return os.path.commonpath([a, b]) in (a, b)
    except ValueError:
        return False


def check_folders(d, b):
    """A base whose work or game folder is, holds or lies in one of its packs' folders would be rebuilt or deleted by its own
    packs, and compared with itself"""
    for what in ("work", "game"):
        for f in list(pack_folders(d).values()) + [os.path.join(d, "packs")]:
            if overlaps(b[what], f):
                sys.exit("the base's %s folder %s overlaps %s, where its packs are built: give the base other folders" %
                         (what, b[what], f))


def link_or_copy(src, dst):
    """A hard link, or a copy where the two folders are on different drives (as make_game.py falls back)"""
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


FICLONE = 0x40049409   # (Linux's ioctl: dst shares src's blocks until either is written, as nx2/lift_store.py _clone)


def clone(src, dst):
    """A copy of src at dst with its times and mode (shutil.copy2), made a copy-on-write clone where the file system has them
    (btrfs, XFS): a runtime's 460 MB game and the base's caches are copied in no time, and the game writing its copy never
    touches the base's. A plain copy elsewhere. Returns dst, as shutil's copy functions do"""
    if os.name != "nt" and not os.path.islink(src):
        import fcntl
        try:
            with open(src, "rb") as s, open(dst, "wb") as d:
                fcntl.ioctl(d.fileno(), FICLONE, s.fileno())
            shutil.copystat(src, dst)
            return dst
        except OSError:
            pass
    return shutil.copy2(src, dst)


def link_objects(base_work, work):
    """A new work folder with the base's compiled objects (hard links: the object cache's keys hold no folder path) and the note of
    the compiler's headers they were made with, so only what a change touches compiles"""
    if os.path.isdir(work):
        return
    src = os.path.join(base_work, "objcache")
    for top, _, fs in os.walk(src):
        dst = os.path.join(work, "objcache", os.path.relpath(top, src))
        os.makedirs(dst, exist_ok=True)
        for f in fs:
            link_or_copy(os.path.join(top, f), os.path.join(dst, f))
    os.makedirs(work, exist_ok=True)
    if os.path.isfile(os.path.join(base_work, "system_headers.txt")):
        shutil.copy2(os.path.join(base_work, "system_headers.txt"), os.path.join(work, "system_headers.txt"))


def cmd_base(a):
    tree, d = os.path.abspath(a.tree), os.path.abspath(a.dir)
    work, game = os.path.join(d, "work"), os.path.join(d, "game")
    if os.path.isdir(game) and os.listdir(game):   # (its files are recorded as what the build made: a played folder holds saves)
        sys.exit("%s is not empty: a base is built into a new game folder" % game)
    dirty = [p for p in changed_files(tree, head(tree)) if route([p], tree)]
    if dirty:
        sys.exit("%s has uncommitted or untracked sources (%s): a base is a commit" % (tree, ", ".join(sorted(dirty)[:5])))
    os.makedirs(game, exist_ok=True)
    if a.seed:   # (every cache file, the play files too: recent pipelines can live only in them, vk_draw.cpp play_path)
        src_cache, dst_cache = os.path.join(a.seed, CACHE_DIR), os.path.join(game, CACHE_DIR)
        for c in sorted(os.listdir(src_cache)) if os.path.isdir(src_cache) else []:
            if is_cache(c) and os.path.isfile(os.path.join(src_cache, c)) and not os.path.exists(os.path.join(dst_cache, c)):
                os.makedirs(dst_cache, exist_ok=True)
                shutil.copy2(os.path.join(src_cache, c), os.path.join(dst_cache, c))
    b = {"commit": head(tree), "work": work, "game": game, "romfs": os.path.abspath(a.romfs),
         "exefs": os.path.abspath(a.exefs), "seeded_from": a.seed}
    t = time.time()
    # (make_game.py builds without preparing; the outputs are recorded then, before the preparation, so nothing it writes beside
    # the game (caches, markers, files the renderer adds later) is ever taken for a build's output)
    make_game(tree, b, work, game, dict({"LONGSHOT_NO_PREPARE": "1"}, **({ROMFS_IN_PLACE_ENV: "1"} if a.romfs_in_place else {})))
    if a.romfs_in_place and not os.path.isdir(os.path.join(game, "romfs")):   # (a tree without --romfs-in-place links it)
        b["romfs_in_place"] = True
    b["outputs"] = build_outputs(game)
    if os.environ.get("LONGSHOT_NO_PREPARE", "0") != "0":   # (as make_game.py reads it: set and not 0)
        b["not_prepared"] = "LONGSHOT_NO_PREPARE is set"
        print("the shaders were not prepared now (LONGSHOT_NO_PREPARE is set): the game prepares them at its first start", flush=True)
    else:   # (a failure keeps the compiled base, its reason in base.json)
        b["not_prepared"] = prepare(game, b, required=False)
    b["build_s"] = round(time.time() - t)
    for m in MODULES:   # (the maps beside the modules, for profilers)
        if m[1]:
            shutil.copy2(os.path.join(work, "sa", "build", m[1]), os.path.join(game, m[1]))
    write_base(d, b)
    print("base %s in %s: %d min" % (b["commit"][:8], d, b["build_s"] // 60))


def cmd_pack(a):
    d, tree = os.path.abspath(a.dir), os.path.abspath(a.tree)
    b = read_base(d)
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", a.name):   # (a plain name: packs/<name> is deleted and made again)
        sys.exit("a pack's name is letters, digits, '.', '_' and '-': %r" % a.name)
    check_folders(d, b)
    if not vcs.is_ancestor(tree, b["commit"]):
        sys.exit("%s is not built on the base %s: rebase it on the base, or make a new base" % (tree, b["commit"][:8]))
    changed = changed_files(tree, b["commit"])
    how = pack_route(route(changed, tree), os.environ)
    pack = os.path.join(d, "packs", a.name)
    if os.path.isdir(pack):   # (what an earlier build of this pack brought goes: this one says again what differs)
        shutil.rmtree(pack)
    os.makedirs(pack)
    print("%d files changed since the base: %s" % (len(changed), {None: "nothing to build", "renderer": "the renderer alone",
                                                                  "game": "the game (both modules)"}[how]), flush=True)
    t, w, files, extra, gone = time.time(), pack_folders(d), {}, [], []
    mg = load_make_game(tree)
    lift_files = {os.path.relpath(f, tree).replace(os.sep, "/") for f in mg.lift_sources()}
    whole = link_inputs_changed(changed, lift_files, build_scripts(tree))
    game_only = how == "game" and not whole and renderer_untouched(changed, lift_files, os.environ)
    if game_only:
        print("the renderer is the base's: no file it is built from changed (RX_GAME_ONLY)", flush=True)
    if how == "game":
        link_objects(b["work"], w["game_work"])
        clear_output(w["game_out"], kept_outputs(mg, w["game_work"], b["romfs"], w["game_out"]))
        make_game(tree, b, w["game_work"], w["game_out"], dict(pack_env(os.environ), **({"RX_GAME_ONLY": "1"} if game_only else {})))
        built, kinds = os.path.join(w["game_work"], "sa", "build"), MODULES[:1] if game_only else MODULES
    elif how == "renderer":
        link_objects(b["work"], w["renderer_work"])
        run(renderer_build_cmd(os.environ, os.cpu_count() or 1, slots=queue_slots(os.environ)), tree,
            dict(renderer_env(w["renderer_work"], b["work"], os.environ), LONGSHOT_KEEP_BUILD_FILES="1"))
        built, kinds = os.path.join(w["renderer_work"], "sa", "build"), MODULES[1:]
    else:
        built, kinds = None, ()
    for mod, mp, rsp, stamped in kinds:
        source = STAMPED_SOURCES[[m[0] for m in MODULES].index(mod)]
        if whole or module_changed(os.path.join(b["work"], "sa", "build"), built, rsp, stamped,
                                   stamped_may_differ(changed, source)):
            for f in (mod, mp):
                if f:
                    clone(os.path.join(built, f), os.path.join(pack, f))
            files[mod] = sha(os.path.join(pack, mod))
    if how == "game":
        extra, gone = generated_changes(b["game"], w["game_out"], b.get("outputs"))
        for rel in extra:
            dst = os.path.join(pack, "files", rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            clone(os.path.join(w["game_out"], rel), dst)
    prepare = translator_changed(tree, changed) or cache_format_changed(tree, b["commit"], changed) or SEED in extra
    with open(os.path.join(pack, "pack.json"), "w", encoding="utf-8") as f:
        json.dump({"base": b["commit"], "tree": tree, "head": head(tree), "changed": sorted(changed),
                   "route": how, "game_only": game_only, "files": files, "generated": extra, "deleted": gone, "prepare": prepare,
                   "build_s": round(time.time() - t)}, f, indent=1)
    print("pack %s: %s%s%s%s, in %d s" % (a.name, ", ".join(files) or "no module differs from the base's",
                                          "; %d generated files" % len(extra) if extra else "",
                                          "; %d generated files gone" % len(gone) if gone else "", "; prepares" if prepare else "",
                                          time.time() - t))


def in_runtime(rt, rel):
    """A path a pack names (a module, a generated or deleted file) is relative and names a file below the runtime: pack.json is
    a plain file, and one that named an absolute path or climbed out with .. would have rt write or remove a file anywhere. Judged
    by its text alone, before the runtime exists: no .. anywhere (so no name comparison is needed, which a case-sensitive
    folder could fool); the links the runtime itself holds are checked where each file is written (resolves_inside)"""
    # (rooted counts too: "/x" is not isabs on Windows since Python 3.13, and join() would then put it at the drive's root)
    if not rel or os.path.isabs(rel) or os.path.splitdrive(rel)[0] or rel[0] in "/\\":
        return False
    parts = rel.replace("\\", "/").split("/")
    # (no ':' either: on NTFS romfs/foo:s names a stream of foo, and foo is a hard link shared with the base; the files a pack
    # names never hold one)
    return ".." not in parts and ":" not in rel and os.path.normpath(rel) not in (".", "")


def resolves_inside(rt, path):
    """path (which may not exist yet) lies inside the runtime once links are followed: its nearest existing folder, resolved,
    is the runtime or below it, compared by file identity rather than by name. A link in the base's romfs is cloned into the
    runtime, so a path that reads as inside can still lead out of it"""
    p = os.path.realpath(path)
    while not os.path.exists(p):
        up = os.path.dirname(p)
        if up == p:
            return False
        p = up
    root = os.path.realpath(rt)
    while True:
        if os.path.samefile(p, root):
            return True
        up = os.path.dirname(p)
        if up == p:
            return False
        p = up


RT_LAYOUT = 1   # (dev_packs.json's "layout": a runtime of another, or none, is made again, its play state kept: refresh_runtime)
# (what a runtime holds that no build made, kept when it is refreshed or made again: the game's caches and logs, the saves, the
# settings and what else the game writes beside itself; dev_packs.json is written anew)
PLAY_STATE = (CACHE_DIR, "logs", "user", "longshot_env.txt") + WRITTEN_IN_PLAY


def runtime_files(b, over):
    """What a runtime of the base b with the packs' files over (rel -> source, or None: removed) holds outside its play state:
    rel -> (source, linked). romfs is hard-linked to the base's (linked), everything else is cloned (the game may write it)"""
    want, game = {}, b["game"]
    for top, dirs, fs in os.walk(game):
        rel_top = os.path.relpath(top, game).replace(os.sep, "/")
        rel_top = "" if rel_top == "." else rel_top + "/"
        if not rel_top:
            dirs[:] = [x for x in dirs if x not in PLAY_STATE]
        for f in fs:
            rel = rel_top + f
            if rel not in PLAY_STATE and rel != "dev_packs.json":
                want[rel] = (os.path.join(top, f), rel.startswith("romfs/"))
    for rel, src in over.items():
        if src:
            want[rel] = (src, False)
        else:
            want.pop(rel, None)
    return want


def in_place(dst, src, linked):
    """dst is already what the runtime would put there from src: the same file (a hard link), or a clone of it with its size
    and time (clone keeps them; a file the game or a hand rewrote has another time)"""
    try:
        if linked:
            return os.path.samefile(dst, src)
        if os.path.islink(dst):
            return False
        d, s = os.stat(dst), os.stat(src)
        return d.st_size == s.st_size and d.st_mtime_ns == s.st_mtime_ns and not os.path.samefile(dst, src)
    except OSError:
        return False


def read_runtime(rt):
    """A runtime's dev_packs.json, or None (not a runtime this made, or unreadable)"""
    try:
        with open(os.path.join(rt, "dev_packs.json"), encoding="utf-8") as f:
            made = json.load(f)
    except (OSError, ValueError):
        return None
    return made if isinstance(made, dict) else None


def refresh_runtime(rt, want, made, guard):
    """rt made to hold want (runtime_files) in place: a file already there as it would be is kept, one that differs replaced,
    one the last runtime wrote (its dev_packs.json "files") and this one does not want removed; the play state stays. A runtime
    of another layout (or none) has everything but its play state removed first, and is filled anew. guard(rel): stops when a
    path leads out of the runtime. Returns how many files were written"""
    old = made.get("files") if made and made.get("layout") == RT_LAYOUT else None
    if old is None or not isinstance(old, list):
        for name in os.listdir(rt):
            if name not in PLAY_STATE:
                p = os.path.join(rt, name)
                if os.path.isdir(p) and not os.path.islink(p):
                    shutil.rmtree(p)
                else:
                    os.remove(p)
        old = []
    for rel in old:
        if isinstance(rel, str) and rel not in want and in_runtime(rt, rel) and rel.split("/")[0] not in PLAY_STATE:
            dst = os.path.join(rt, rel)
            guard(rel)
            if os.path.lexists(dst) and resolves_inside(rt, os.path.dirname(dst)):
                os.remove(dst)
    written = 0
    for rel, (src, linked) in sorted(want.items()):
        dst = os.path.join(rt, rel)
        guard(rel)
        if os.path.lexists(dst) and in_place(dst, src, linked):
            continue
        if os.path.lexists(dst):
            os.remove(dst)   # (a romfs hard link, or a link: replaced, never written through to the base or the link's target)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        (link_or_copy if linked else clone)(src, dst)
        written += 1
    return written


def seed_play_state(game, rt):
    """The base's play state (its caches, a warm-up's; its settings) copied into the runtime where the runtime has none of its
    own yet: what the runtime grew in play is never replaced"""
    for name in PLAY_STATE:
        src = os.path.join(game, name)
        files = [src] if os.path.isfile(src) else [os.path.join(top, f) for top, _, fs in os.walk(src) for f in fs]
        for s in files:
            t = os.path.join(rt, os.path.relpath(s, game))
            if os.path.isfile(s) and not os.path.lexists(t):
                os.makedirs(os.path.dirname(t), exist_ok=True)
                clone(s, t)


def cmd_rt(a):
    d, rt = os.path.abspath(a.dir), os.path.abspath(a.runtime)
    b = read_base(d)
    made = read_runtime(rt) if os.path.exists(rt) else None
    if os.path.exists(rt) and made is None:
        sys.exit("%s exists and is not a runtime (no dev_packs.json): delete it, or name another folder" % rt)
    for f in [b["game"], b["work"], os.path.join(d, "packs")] + list(pack_folders(d).values()):
        if overlaps(rt, f):
            sys.exit("%s overlaps %s: a runtime goes in a folder of its own" % (rt, f))
    over, needs_prepare = {}, False   # (a file in the runtime -> the pack and the path it comes from, or None: the pack removes it)
    for p in a.packs:
        pdir = os.path.join(d, "packs", p)
        with open(os.path.join(pdir, "pack.json"), encoding="utf-8") as f:
            meta = json.load(f)
        if meta["base"] != b["commit"]:
            sys.exit("pack %s was built on %s, not the base %s: build it again" % (p, meta["base"][:8], b["commit"][:8]))
        mine = {m: os.path.join(pdir, m) for m in meta["files"]}
        mine.update({m[1]: os.path.join(pdir, m[1]) for m in MODULES
                     if m[0] in meta["files"] and m[1] and os.path.isfile(os.path.join(pdir, m[1]))})
        mine.update({rel: os.path.join(pdir, "files", rel) for rel in meta["generated"]})
        mine.update({rel: None for rel in meta.get("deleted", ())})
        if not set(meta["files"]) <= {m[0] for m in MODULES}:
            sys.exit("pack %s names modules that are not %s: %s" % (p, " or ".join(m[0] for m in MODULES), sorted(meta["files"])))
        bad = sorted(rel for rel in mine if not in_runtime(rt, rel))
        if bad:
            sys.exit("pack %s names paths outside the runtime: %s" % (p, ", ".join(map(repr, bad[:5]))))
        for rel, src in mine.items():
            if rel in over:
                sys.exit("packs %s and %s both bring %s: merge their branches and build one pack" % (over[rel][0], p, rel))
            over[rel] = (p, src)
        needs_prepare |= meta["prepare"]
    os.makedirs(rt, exist_ok=True)

    def guard(rel):
        if rel in over and not resolves_inside(rt, os.path.dirname(os.path.join(rt, rel))):
            sys.exit("pack %s: %s leads outside the runtime through a link in it; nothing of the pack was written past this "
                     "file: delete %s" % (over[rel][0], rel, rt))

    want = runtime_files(b, {rel: src for rel, (_, src) in over.items()})
    written = refresh_runtime(rt, want, made, guard)
    seed_play_state(b["game"], rt)
    if needs_prepare:
        prepare(rt, b)
    in_place_romfs = b["romfs"] if b.get("romfs_in_place") and not os.path.isdir(os.path.join(b["game"], "romfs")) else None
    with open(os.path.join(rt, "dev_packs.json"), "w", encoding="utf-8") as f:
        json.dump(dict({"base": b["commit"], "packs": a.packs, "modules": {m[0]: sha(os.path.join(rt, m[0])) for m in MODULES},
                        "layout": RT_LAYOUT, "files": sorted(want)}, **({"romfs": in_place_romfs} if in_place_romfs else {})),
                  f, indent=1)
    if in_place_romfs:
        print("%s has no romfs (the base reads it where it is): start the game with --romfs %s" % (rt, in_place_romfs), flush=True)
    print("%s: the base %s%s (%s)" % (rt, b["commit"][:8], "".join(", %s from %s" % (m, over[m][0]) for m in (GAME, RENDERER) if m in over),
                                      "made, %d files" % written if made is None else "refreshed in place, %d files replaced" % written))


def is_cache(name):
    return name in CACHES or bool(PLAY_FILES.fullmatch(name))


def runtime_file(rel):
    """What the game made in its folder, not the build: anything in cache/ or logs/"""
    return rel.startswith(RUNTIME_DIRS)


def cmd_warm(a):
    """A runtime with no pack, played once by each --run command ({runtime} in it is the runtime's folder; split as a POSIX
    shell would), then the caches play made or grew copied into the base's game folder, and the runtime deleted: every
    later runtime starts warm for those scenes. Only cache files are copied, through a temporary file moved into place"""
    d = os.path.abspath(a.dir)
    b = read_base(d)
    rt = os.path.join(d, "warm-runtime")
    if os.path.exists(rt):   # (made beforehand by `rt <dir> <dir>/warm-runtime`, so the copy can run under a lock of its own)
        try:
            with open(os.path.join(rt, "dev_packs.json"), encoding="utf-8") as f:
                made = json.load(f)
        except (OSError, ValueError):
            made = {}
        if made.get("base") != b["commit"] or made.get("packs"):
            sys.exit("%s exists and is not this base's runtime with no pack (an earlier warm-up stopped?): look at it, delete "
                     "it and run again" % rt)
        print("warming the runtime already made in %s" % rt, flush=True)
    else:
        cmd_rt(argparse.Namespace(dir=d, runtime=rt, packs=[]))
    for cmd in a.run:
        argv = [x.replace("{runtime}", rt) for x in shlex.split(cmd)]
        print("+ " + " ".join(argv), flush=True)
        subprocess.run(argv, check=True)
    copied = []
    rt_cache, base_cache = os.path.join(rt, CACHE_DIR), os.path.join(b["game"], CACHE_DIR)
    os.makedirs(base_cache, exist_ok=True)
    for name in sorted(os.listdir(rt_cache)) if os.path.isdir(rt_cache) else []:
        src, dst = os.path.join(rt_cache, name), os.path.join(base_cache, name)
        # (a cache the game wrote: a plain file in the runtime, never a link, whose bytes go into a new temporary file of the
        # base's own and then over the old one, so neither a link in the runtime nor one left in the base is followed)
        if os.path.islink(src) or not os.path.isfile(src) or not is_cache(name) or os.path.islink(dst):
            continue
        if os.path.isfile(dst) and same_file(src, dst):
            continue
        fd, tmp = tempfile.mkstemp(prefix=name + ".", suffix=".tmp", dir=base_cache)
        with os.fdopen(fd, "wb") as out, open(src, "rb") as f:
            shutil.copyfileobj(f, out, 1 << 22)
        shutil.copystat(src, tmp)
        os.replace(tmp, dst)
        copied.append(name)
    b["warmed"] = {"at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "runs": a.run, "files": copied}
    write_base(d, b)
    shutil.rmtree(rt)
    print("the base's caches warmed by %d runs: %s" % (len(a.run), ", ".join(copied) or "nothing grew"))


class Busy:
    """dev_packs.busy in a base's folder while a job builds, warms or copies from it: a second job on the same base is refused
    (game packs share one work folder; a warm-up writes the base's caches). A marker left by a stopped job is deleted by hand"""
    def __init__(self, d, what):
        self.path, self.what = os.path.join(d, BUSY), what

    def __enter__(self):
        common.ensure_dir(os.path.dirname(self.path))
        try:
            fd = os.open(self.path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        except FileExistsError:
            try:
                with open(self.path, encoding="utf-8") as f:
                    held = f.read().strip()
            except OSError:
                held = "?"
            sys.exit("the base is busy (%s): wait for that job, or delete %s if it is no longer running" % (held, self.path))
        self.token = "%d %s %s" % (os.getpid(), self.what, time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(self.token)
        return self

    def __exit__(self, *exc):
        try:
            with open(self.path, encoding="utf-8") as f:
                mine = f.read().strip() == self.token
            if mine:
                os.remove(self.path)
        except OSError:
            pass


def add_commands(sub):
    """`dev.py packs`' subcommands"""
    ap = sub.add_parser("packs", help="a base folder you name and its packs, without the queue (the Test Farm's)",
                        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ps = ap.add_subparsers(dest="what", required=True)
    s = ps.add_parser("base", help="build a checkout's commit as a base")
    s.add_argument("dir")
    s.add_argument("--romfs", required=True)
    s.add_argument("--exefs", required=True)
    s.add_argument("--tree", default=ROOT)
    s.add_argument("--seed", help="an earlier game folder whose caches the base starts from")
    s.add_argument("--romfs-in-place", action="store_true",
                   help="no romfs in the base's game folder: read where it is, the game started with --romfs")
    s = ps.add_parser("pack", help="a change built into a pack over the base")
    s.add_argument("dir")
    s.add_argument("name")
    s.add_argument("--tree", default=ROOT)
    s = ps.add_parser("rt", help="a game folder to run: the base with packs over it")
    s.add_argument("dir")
    s.add_argument("runtime")
    s.add_argument("packs", nargs="*")
    s = ps.add_parser("warm", help="play a runtime with no pack and keep the caches it grew in the base")
    s.add_argument("dir")
    s.add_argument("--run", action="append", required=True, help="a command that plays the runtime once; {runtime} is its folder")
    ap.set_defaults(func=command)


def command(a):
    with Busy(os.path.abspath(a.dir), a.what):
        {"base": cmd_base, "pack": cmd_pack, "rt": cmd_rt, "warm": cmd_warm}[a.what](a)
