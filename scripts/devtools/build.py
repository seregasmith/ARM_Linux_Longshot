"""This checkout's development builds on a PC that several agents share: one shared base per commit of main, this checkout
as a pack over it, a runtime to play, every step through the job queue (jobqueue.py) in the build environment a player's
build has. Linux and Windows; Python's standard library only. docs/dev/developing.md, *Test a change against a base*.

    dev.py base [REV] [--romfs-in-place]
                                the shared base of REV (default: where this checkout left main), built once by whoever
                                asks first (`dev.py packs base`, run by a checkout of that commit); --romfs-in-place:
                                its game folder has no romfs, and its runtimes' game is started with --romfs
    dev.py pack                 this checkout as a pack over the nearest base in its history (`dev.py packs pack`)
    dev.py rt [--empty]         build-linux/rt-<checkout>/, a game folder to run: the base with that pack (--empty: none,
                                build-linux/rt-base/, the other half of an A/B)
    dev.py game [--quick | --timing] [--fsr]
                                a whole game folder, build-linux/game/ (--quick: -O1 hot pieces, never measured;
                                --timing: a measured build)
    dev.py vk                   the renderer alone, its exports checked
    dev.py env                  the build environment as shell lines, for builds of your own
    dev.py bases                the shared bases, their packs and when each was last used

(build-windows\\ instead of build-linux/ on Windows; both git-ignored.) Nothing goes on a RAM file system (tmpfs): a build
there fills the memory, so this refuses one.

The environment:
  - Python: LONGSHOT_DEV_PYTHON, else build-linux/deps/python of this checkout or of one beside it (scripts/linux/deps.sh
    makes it); on Windows the build tools' own. It must have the build's packages (PY_PACKAGES).
  - the build tools a player's build uses: tools.lock's lines for this system, installed once per tools.lock into the
    development folder's builder/ (Linux by builder/linux/linux_tools.py, Windows by scripts/tools.py install, with the
    variables the Windows builder sets and its prebuilt SDL DLLs)
  - DLSS: when LONGSHOT_NGX_SDK is unset, the development folder's sdk/ngx-<linux|windows> (scripts/fetch.py dlss)
  - the development folder's config/dev_build.json: "env" (variables for every build), "python", "romfs", "exefs",
    "fsr" (always --fsr). The game's unpacked files: LONGSHOT_DEV_ROMFS and LONGSHOT_DEV_EXEFS, else the newest game
    folder in ~/Games (Windows also ~/Games/Longshot) with romfs/ and .longshot_exefs/
  - make_game.py's lift key hashes every A64_, OOT_ and RX_ variable but the compile-only ones: one set by hand gives the
    build its own translation, so this names any it sees. The queue's own variables are dropped.
"""
import contextlib
import hashlib
import io
import os
import re
import shlex
import shutil
import subprocess
import time

from . import common, jobqueue, packs, vcs

ROOT = common.ROOT
WHO = "dev.py"
COMPILE_ONLY_ENV = ("RX_HOT_OPT", "RX_PGO", "RX_VK_PGO", "RX_VK_PGO_FILE", "RX_A64_RT_O2", "RX_GAME_ONLY")   # (make_game.py's: left out of the lift key)
PY_PACKAGES = ("capstone", "unicorn", "zstandard", "numpy")
HOT_QUICK = {"RX_HOT_OPT": "O1"}   # (docs/dev/developing.md, Build quickly: compile-only, never in the lift key)
# (the Windows builder's prebuilt third_party files that nothing in the repository builds: tools.lock's third-party)
WINDOWS_PREBUILT = ("sdl2/SDL2.dll", "sdl2/SDL3.dll")
# (the variables the Windows builder sets to its tools: builder/LongshotBuilder/Runner.cs sets RX_CPM, and nx2/build.py run
# alone needs LONGSHOT_MINGW. No EMSDK: llvm-mingw has every compiler and LLVM tool, tests/test_windows_toolchain.py)
WINDOWS_TOOL_ENV = (("llvm-mingw", "LONGSHOT_MINGW"), ("cpm", "RX_CPM"))
# (what host/rx_backend.c asks the renderer's DLL for: without mkwvk_init the game draws nothing)
RENDERER_EXPORTS = ("mkwvk_init",)
CONFIG = "dev_build.json"   # (in the development folder's config/, beside the job queue's job_queue.json)


def die(text):
    common.die(WHO, text)


def say(text):
    common.say(WHO, text)


def system():
    """tools.lock's name for this system"""
    return "windows-x64" if common.WINDOWS else "linux-x64"


def own_dir():
    """This checkout's own build files (git-ignored)"""
    return os.path.join(ROOT, "build-windows" if common.WINDOWS else "build-linux")


def renderer_name():
    return "longshot_vk.dll" if common.WINDOWS else "longshot_vk.so"


def config():
    return common.read_config(CONFIG)


def not_on_ram(path, what):
    if common.on_ram(path if os.path.exists(path) else os.path.dirname(os.path.abspath(path))):
        die("%s %s is on a RAM file system (tmpfs): a build or a game folder there fills the memory. Put it on a disk" % (what, path))


def touch(d):
    try:
        os.utime(d)   # (`dev.py bases` shows a base's time as its last use)
    except OSError:
        pass


# ---- the checkout ----

def jj_repo():
    return vcs.is_jj(ROOT)


def out(cmd, cwd=ROOT):
    return common.output(WHO, cmd, cwd)


def resolve(rev):
    """A revision (jj or git) as its full commit id"""
    try:
        return vcs.resolve(ROOT, rev)
    except subprocess.CalledProcessError as e:
        die("no commit %s: %s" % (rev, (e.stderr or e.stdout or "").strip()[-400:]))


def fork_point():
    """Where this checkout left main: the newest commit of main in its history"""
    if jj_repo():
        return resolve("heads(::@ & ::trunk())")
    for main in ("forge/main", "origin/main", "upstream/main", "main"):
        r = subprocess.run(["git", "merge-base", "HEAD", main], cwd=ROOT, capture_output=True, text=True)
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip()
    die("no main to start from (fetch it, or name a revision)")


def is_ancestor(commit):
    return vcs.is_ancestor(ROOT, commit)


def distance(commit):
    return vcs.distance(ROOT, commit)


# ---- the environment ----

def python_candidates(py_dir=None):
    """Where a Python with the build's packages may be: on Windows the build tools' own (py_dir, the one a player's build
    runs), on Linux build-linux/deps/python of this checkout or of one beside it"""
    if common.WINDOWS:
        return [os.path.join(py_dir, "python.exe")] if py_dir else []
    rel = os.path.join("build-linux", "deps", "python", "bin", "python3")
    parent = os.path.dirname(ROOT)
    try:
        beside = [os.path.join(parent, n, rel) for n in sorted(os.listdir(parent))]
    except OSError:
        beside = []
    return [os.path.join(ROOT, rel)] + beside


def find_python(c, py_dir=None):
    """A Python with the build's packages: the one named (LONGSHOT_DEV_PYTHON, dev_build.json's "python"), else the first
    candidate that has them"""
    named = os.environ.get("LONGSHOT_DEV_PYTHON") or c.get("python")
    tried = set()
    for p in ([named] if named else []) + python_candidates(py_dir):
        if not os.path.isfile(p) or p in tried:
            continue
        tried.add(p)
        if subprocess.run([p, "-I", "-c", "import " + ", ".join(PY_PACKAGES)], capture_output=True).returncode == 0:
            return p
        if p == named:
            die("%s lacks the build's packages (%s)" % (p, ", ".join(PY_PACKAGES)))
    if common.WINDOWS:
        die("no Python with the build's packages (%s): the build tools' own (%s) lacks them or is not there; set "
            "LONGSHOT_DEV_PYTHON to one that has them" % (", ".join(PY_PACKAGES), (python_candidates(py_dir) or ["?"])[0]))
    die("no Python with the build's packages (%s): run scripts/linux/deps.sh once, or set LONGSHOT_DEV_PYTHON" %
        ", ".join(PY_PACKAGES))


def tools_lock_key():
    with open(os.path.join(ROOT, "tools.lock"), encoding="utf-8") as f:
        lines = [l for l in f if l.startswith("tool ") and " %s " % system() in l]
    return hashlib.sha1("".join(lines).encode()).hexdigest()[:12]


def builder_dir():
    # (one install per tools.lock: two checkouts on other tools.lock never unpack over each other's tools mid-build)
    return os.path.join(common.dev_path("builder"), tools_lock_key())


def windows_tool_dirs():
    """(Windows) {tool: its folder}: tools.lock's windows-x64 lines at their dest under the builder folder, as the Windows
    builder's setup unpacks them (LongshotBuilder/tools/<tool>)"""
    import tools   # (scripts/tools.py)
    b = builder_dir()
    return {t.name: os.path.join(b, *t.dest.split("/"))
            for t in tools.of_system(tools.load(os.path.join(ROOT, "tools.lock")), system())}


def windows_toolchain():
    """(Windows) tools.lock's windows-x64 tools installed once (scripts/tools.py install: downloaded on first use, checked by
    size and SHA-256), as ({variable: path}, {tool: folder})"""
    import tools
    b = builder_dir()
    lock = [t for t in tools.load(os.path.join(ROOT, "tools.lock")) if t.system == system()]
    said = io.StringIO()
    with common.Lock(b + ".lock"):   # (two checkouts at once: the second waits, then finds them there)
        try:
            with contextlib.redirect_stdout(said):
                tools.install(b, lock, cache=os.path.join(common.dev_path("builder"), "downloads"))
        except SystemExit as e:
            die("the build tools could not be installed into %s (%s)" % (b, e))
        finally:   # (on stderr: `env` prints the environment alone)
            for line in said.getvalue().splitlines():
                if not line.endswith("there already"):
                    common.say(WHO, "build tools: " + line, err=True)
    dirs = windows_tool_dirs()
    env = {}
    for name, var in WINDOWS_TOOL_ENV:
        if not os.path.isdir(dirs.get(name, "")):
            die("tools.lock has no %s for Windows: its tool folder %s is not there" % (name, dirs.get(name)))
        env[var] = dirs[name]
    return env, dirs


def windows_prebuilt(tree):
    """(Windows) SDL2.dll and SDL3.dll (the game's controllers), which nothing here builds, copied from the build tools into
    the tree's third_party when it lacks them or has others. git ignores them (*.dll), so a pack or a base sees no change"""
    src = windows_tool_dirs().get("third-party")
    for rel in WINDOWS_PREBUILT:
        a, b = os.path.join(src or "", *rel.split("/")), os.path.join(tree, "third_party", *rel.split("/"))
        if not src or not os.path.isfile(a):
            say("the build tools have no third_party/%s: the game folder goes without it" % rel)
            continue
        if os.path.isfile(b) and os.path.getsize(b) == os.path.getsize(a):
            continue
        os.makedirs(os.path.dirname(b), exist_ok=True)
        shutil.copyfile(a, b + ".new")
        os.replace(b + ".new", b)


def toolchain(py):
    """(Linux) {variable: path} of the build tools, installed once"""
    r = subprocess.run([py, "-I", os.path.join(ROOT, "builder", "linux", "linux_tools.py"), "env", "--install"], cwd=ROOT,
                       env=dict(os.environ, LONGSHOT_BUILDER_HOME=builder_dir()), stdout=subprocess.PIPE, text=True)
    if r.returncode:
        die("the build tools could not be installed (above)")
    env = {}
    for line in r.stdout.splitlines():
        parts = shlex.split(line)
        if len(parts) == 2 and parts[0] == "export" and "=" in parts[1]:
            k, v = parts[1].split("=", 1)
            env[k] = v
    return env


def game_roots():
    """The folders whose game folders may hold the unpacked game: ~/Games; on Windows also ~/Games/Longshot, the Longshot
    Launcher's builds folder (LauncherSettings.DefaultBuildsFolder)"""
    games = os.path.expanduser(os.path.join("~", "Games"))
    return [games, os.path.join(games, "Longshot")] if common.WINDOWS else [games]


def game_files(c):
    """(romfs, exefs) of the game, unpacked on this PC"""
    romfs = os.environ.get("LONGSHOT_DEV_ROMFS") or c.get("romfs")
    exefs = os.environ.get("LONGSHOT_DEV_EXEFS") or c.get("exefs")
    if romfs and exefs:
        return os.path.expanduser(romfs), os.path.expanduser(exefs)
    found = []
    for games in game_roots():
        for n in os.listdir(games) if os.path.isdir(games) else ():
            g = os.path.join(games, n)
            if os.path.isdir(os.path.join(g, "romfs")) and os.path.isdir(os.path.join(g, ".longshot_exefs")):
                found.append((os.path.getmtime(os.path.join(g, ".longshot_exefs")), g))
    if not found:
        die("no unpacked game here: set LONGSHOT_DEV_ROMFS and LONGSHOT_DEV_EXEFS (a built game folder's romfs/ and "
            ".longshot_exefs/), or \"romfs\" and \"exefs\" in %s" % common.config_file(CONFIG))
    g = max(found)[1]
    return os.path.join(g, "romfs"), os.path.join(g, ".longshot_exefs")


def ngx_sdk():
    """(folder, the library a build links) of the development folder's NGX SDK, where scripts/fetch.py dlss puts it"""
    if common.WINDOWS:
        return common.dev_path("sdk/ngx-windows"), "nvsdk_ngx_s.lib"
    return common.dev_path("sdk/ngx-linux"), "libnvsdk_ngx.a"


def environment(quiet=False):
    """(python, env): the build's environment"""
    c = config()
    if common.WINDOWS:
        tc, dirs = windows_toolchain()
        py = find_python(c, dirs.get("python"))
    else:
        py = find_python(c)
        tc = toolchain(py)
    env = jobqueue.without_queue_vars(os.environ)
    env.update(tc)
    ngx, ngx_lib = ngx_sdk()
    if "LONGSHOT_NGX_SDK" not in env and os.path.isfile(os.path.join(ngx, "lib", ngx_lib)):
        env["LONGSHOT_NGX_SDK"] = ngx
    elif "LONGSHOT_NGX_SDK" not in env and not quiet:
        common.say(WHO, "no NVIDIA DLSS in this build: python -I scripts/fetch.py dlss puts its SDK in %s" % ngx, err=True)
    for k, v in (c.get("env") or {}).items():
        if k not in os.environ:
            env[k] = str(v)
    for k, v in packs.store_env().items():
        env.setdefault(k, v)
        if env[k] not in ("", "0"):
            not_on_ram(env[k], k)
    env.setdefault("LONGSHOT_LIFT_STORE_KEEP", "6")   # (3 is one per checkout of main; agents build several branches at once)
    keyed = sorted(k for k in env if k.startswith(jobqueue.LIFT_KEY_PREFIXES) and k not in COMPILE_ONLY_ENV and k != "RX_CPM")
    if keyed and not quiet:
        say("these enter make_game.py's lift key, so this build takes no other build's translation unless it had them "
            "too: %s" % ", ".join(keyed))
    return py, env


def shell_line(k, v):
    """An assignment for this system's shell: POSIX sh's export, PowerShell's $env: on Windows"""
    if common.WINDOWS:
        return "$env:%s = '%s'" % (k, v.replace("'", "''"))
    return "export %s=%s" % (k, shlex.quote(v))


# ---- the shared bases ----

def short(commit):
    return commit[:12]


def bases():
    """[(folder, its base.json)] of the shared bases"""
    d = common.dev_path("bases")
    found = []
    for n in sorted(os.listdir(d)) if os.path.isdir(d) else ():
        try:
            found.append((os.path.join(d, n), packs.read_base(os.path.join(d, n))))
        except (OSError, ValueError):
            continue
    return found


def base_dir(commit):
    return os.path.join(common.dev_path("bases"), short(commit))


def base_for():
    """The nearest base in this checkout's history"""
    mine = [(distance(b["commit"]), p) for p, b in bases() if is_ancestor(b["commit"])]
    if not mine:
        die("no base in this checkout's history: dev.py base (it builds the one where this checkout left main)")
    return min(mine)[1]


def base_lock(d):
    """One job at a time on a shared base: <base>.lock, waited for (`dev.py packs` refuses a second job on a base, by its
    dev_packs.busy; this waits for the first instead)"""
    d = d.rstrip("/" + os.sep)
    return common.Lock(d + ".lock", waiting="%s: another job uses the base %s: waiting for it" % (WHO, os.path.basename(d)))


def packs_cmd(py, tree, *args):
    """`dev.py packs` of a checkout, run by the build's Python"""
    return [py, "-I", os.path.join(tree, "scripts", "dev.py"), "packs"] + list(args)


def queued(name, mode, cmd, env, cwd=ROOT):
    """cmd through the job queue (directly when this runs inside a job already); a failure stops"""
    rc = jobqueue.run(name, mode, cmd, environ=env, cwd=cwd)
    if rc:
        die("%s failed (exit status %d)" % (name, rc))


def on_base(d, name, mode, cmd, env):
    """A `dev.py packs` step on the shared base d through the queue, holding the base's lock"""
    with base_lock(d):
        touch(d)
        queued(name, mode, cmd, env)


def pack_name():
    """This checkout's pack: its folder's name"""
    return re.sub(r"[^A-Za-z0-9._-]+", "-", os.path.basename(ROOT))


# ---- commands ----

def cmd_base(a):
    commit = resolve(a.rev) if a.rev else fork_point()
    d = base_dir(commit)
    not_on_ram(d, "the base")
    if os.path.isfile(os.path.join(d, "base.json")):
        say("the base of %s is there: %s" % (short(commit), d))
        touch(d)
        return 0
    py, env = environment()
    romfs, exefs = game_files(config())
    with base_lock(d):
        if os.path.isfile(os.path.join(d, "base.json")):   # (built by whoever held the lock before)
            say("the base of %s is there: %s" % (short(commit), d))
            return 0
        # (a checkout of the commit alone, built by its own `dev.py packs base`: a base has nothing uncommitted)
        src, ws = d + ".src", "base-" + short(commit)
        shutil.rmtree(src, ignore_errors=True)
        if jj_repo():
            subprocess.run(["jj", "workspace", "forget", ws], cwd=ROOT, capture_output=True)
            out(["jj", "workspace", "add", "--name", ws, "-r", commit, src])
        else:
            out(["git", "worktree", "add", "--detach", src, commit])
        try:
            if common.WINDOWS:
                windows_prebuilt(src)
            # (the shader caches of the newest base: the preparation then redoes only what changed)
            seed = [g for g in (os.path.join(p, "game") for p, _ in sorted(bases(), key=lambda x: -os.path.getmtime(x[0])))
                    if os.path.isdir(g)][:1]
            cmd = packs_cmd(py, src, "base", d, "--romfs", romfs, "--exefs", exefs, "--tree", src) + \
                (["--seed", seed[0]] if seed else []) + (["--romfs-in-place"] if a.romfs_in_place else [])
            say("building the base of %s in %s%s" % (short(commit), d, " (shader caches from %s)" % seed[0] if seed else ""))
            queued("base-" + short(commit)[:8], "build", cmd, env, cwd=src)
        finally:
            if jj_repo():
                subprocess.run(["jj", "workspace", "forget", ws], cwd=ROOT, capture_output=True)
                shutil.rmtree(src, ignore_errors=True)
                subprocess.run(["git", "worktree", "prune"], cwd=ROOT, capture_output=True)
            else:
                subprocess.run(["git", "worktree", "remove", "--force", src], cwd=ROOT, capture_output=True)
    say("base of %s: %s" % (short(commit), d))
    return 0


def cmd_pack(a):
    d, name = base_for(), pack_name()
    py, env = environment()
    if common.WINDOWS:
        windows_prebuilt(ROOT)
    on_base(d, "pack-" + name, "build", packs_cmd(py, ROOT, "pack", d, name, "--tree", ROOT), env)
    say("pack %s on the base %s" % (name, d))
    return 0


def cmd_rt(a):
    d = base_for()
    pack = [] if a.empty else [pack_name()]
    rt = os.path.join(own_dir(), "rt-" + (pack[0] if pack else "base"))
    not_on_ram(rt, "the runtime")
    py, env = environment(quiet=True)
    # (shared: rt may prepare the shaders again, which takes the GPU, when a pack changed the translator or the shader seed)
    on_base(d, "rt-" + os.path.basename(rt), "shared", packs_cmd(py, ROOT, "rt", d, rt, *pack), env)
    say("runtime: %s" % rt)
    return 0


def cmd_game(a):
    out_dir, work = os.path.join(own_dir(), "game"), os.path.join(own_dir(), "dev-work")
    not_on_ram(out_dir, "the game folder")
    not_on_ram(work, "the work folder")
    py, env = environment()
    romfs, exefs = game_files(config())
    if common.WINDOWS:
        windows_prebuilt(ROOT)
    env["LONGSHOT_KEEP_BUILD_FILES"] = "1"   # (the next build here compiles only what changed)
    env.setdefault("LONGSHOT_NO_PREPARE", "1")   # (its first start prepares the shaders; LONGSHOT_NO_PREPARE=0 prepares now)
    if a.quick:
        env.update(HOT_QUICK)
    cmd = [py, "-I", "nx2/make_game.py", "--romfs", romfs, "--exefs", exefs, "--work", work, "--out", out_dir,
           "--language", "en-US"]
    if a.fsr or config().get("fsr"):   # (make_game.py decides FSR 4 by --fsr or the folder's mark, not the environment)
        cmd.append("--fsr")
    queued("build-" + pack_name(), "timing-cpu" if a.timing else "build", cmd, env)
    say("game folder: %s" % out_dir)
    return 0


def cmd_vk(a):
    work = os.path.join(own_dir(), "vk-work")
    not_on_ram(work, "the work folder")
    py, env = environment()
    # (built as a renderer pack builds it, with the play profile `dev.py game` gave this checkout's renderer)
    env.update(packs.renderer_env(work, os.path.join(own_dir(), "dev-work"), env))
    so = os.path.join(work, "sa", "build", renderer_name())
    if os.path.exists(so):
        os.remove(so)
    queued("vk-" + pack_name(), "light", packs.renderer_build_cmd(dict(env, LONGSHOT_JOB_MODE="light"), os.cpu_count() or 1, py), env)
    if not os.path.isfile(so):
        die("no %s after the build" % so)
    if common.WINDOWS:
        check_dll_exports(so, env)
    else:
        # (a renderer function left in an anonymous namespace is an undefined symbol: the game finds no renderer, opens no window)
        nm = subprocess.run("nm -D --undefined-only '%s' | c++filt | grep mkwvk" % so, shell=True, capture_output=True, text=True)
        bad = [l for l in nm.stdout.splitlines() if not l.lstrip().startswith("w ")]
        if bad:
            die("longshot_vk.so has undefined mkwvk symbols:\n" + "\n".join(bad))
    say("renderer: %s (symbols checked)" % so)
    return 0


def dll_exports(text):
    """The names in llvm-objdump -p's export table (its lines "  <ordinal> 0x<RVA> <name>" after "Export Table:")"""
    names, inside = set(), False
    for line in text.splitlines():
        if line.startswith("Export Table:"):
            inside = True
        elif inside and line.strip() and not line.startswith((" ", "\t")):
            inside = False
        elif inside:
            w = line.split()
            if len(w) >= 3 and w[0].isdigit() and w[1].lower().startswith("0x"):
                names.add(w[-1])
    return names


def check_dll_exports(dll, env):
    """(Windows) the DLL exports the renderer's entry points. An undefined symbol fails the link there, so nm's check has
    nothing to find; an entry point without __declspec(dllexport) is a DLL the game finds no renderer in"""
    mingw = env.get("LONGSHOT_MINGW", "")
    objdump = os.path.join(mingw, "bin", "llvm-objdump.exe") if mingw else ""
    objdump = objdump if os.path.isfile(objdump) else shutil.which("llvm-objdump")
    if not objdump:
        die("no llvm-objdump to check %s's exports with (LONGSHOT_MINGW's bin)" % dll)
    r = subprocess.run([objdump, "-p", dll], capture_output=True, text=True, encoding="utf-8", errors="replace")
    missing = [n for n in RENDERER_EXPORTS if n not in dll_exports(r.stdout)]
    if r.returncode or missing:
        die("%s does not export %s: the game would find no renderer%s" % (
            os.path.basename(dll), ", ".join(missing or RENDERER_EXPORTS),
            " (%s)" % r.stderr.strip()[-300:] if r.returncode else ""))


def cmd_env(a):
    py, env = environment()
    shown = {k: env[k] for k in sorted(env) if os.environ.get(k) != env[k]}
    shown["PY"] = py
    for k, v in shown.items():
        print(shell_line(k, v))
    return 0


def cmd_bases(a):
    now = time.time()
    found = bases()
    if not found:
        print("no bases in %s" % common.dev_path("bases"))
    for p, b in found:
        print("%s  %s  used %s ago%s%s" % (short(b["commit"]), p, common.ago(now - os.path.getmtime(p)),
                                         "  (not prepared: %s)" % b["not_prepared"] if b.get("not_prepared") else "",
                                         "  in this checkout's history" if is_ancestor(b["commit"]) else ""))
        kept = os.path.join(p, "packs")
        for k in sorted(os.listdir(kept)) if os.path.isdir(kept) else ():
            print("    pack %s" % k)
    return 0


def add_commands(sub):
    """dev.py's build commands: base, pack, rt, game, vk, env, bases"""
    def add(name, func, text):
        p = sub.add_parser(name, help=text, description=text)
        p.set_defaults(func=func)
        return p
    s = add("base", cmd_base, "the shared base of REV, built when no one has")
    s.add_argument("rev", nargs="?", help="a revision of main (default: where this checkout left main)")
    s.add_argument("--romfs-in-place", action="store_true",
                   help="no romfs in the base's game folder: its runtimes' game is started with --romfs")
    add("pack", cmd_pack, "this checkout as a pack over the nearest base in its history")
    add("rt", cmd_rt, "a game folder to run: the base with this checkout's pack").add_argument(
        "--empty", action="store_true", help="the base alone, the other half of an A/B")
    s = add("game", cmd_game, "a whole game folder from this checkout (make_game.py)")
    speed = s.add_mutually_exclusive_group()
    speed.add_argument("--quick", action="store_true", help="-O1 hot pieces: faster to build, slower to run; never measure it")
    speed.add_argument("--timing", action="store_true", help="a measured build (the queue's timing-cpu mode)")
    s.add_argument("--fsr", action="store_true", help="with AMD FSR 4 (\"fsr\": true in dev_build.json: always)")
    add("vk", cmd_vk, "the renderer alone, its exports checked")
    add("env", cmd_env, "the build environment, as shell lines")
    add("bases", cmd_bases, "the shared bases, their packs and when each was last used")
