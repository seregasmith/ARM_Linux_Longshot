"""`dev.py update`: a game folder you play kept on the newest commit of main (or another branch), built here as soon as it
lands, without waiting for Nightly; or the folder's commit with only the pull requests you pick of those merged since;
and open pull requests merged on top of either, to try them before they land.
Linux and Windows; Python's standard library only (tkinter for the window). docs/dev/developing.md, *Keep a game on main*.

    dev.py update [--game DIR] [--remote R] [--branch B]   build the branch's newest commit into the game folder, when it
                                                           moved since the folder's last update (--force: even if not)
    dev.py update --pick 1874,1880                         the folder's commit with only those merged pull requests
    dev.py update --with 1913,1925                         open pull requests merged on top (with --pick too)
    dev.py update --check                                  the pull requests merged since the folder's build
    dev.py update --rollback                               the folder back as it was before its last update
    dev.py update --logs                                   the folder's logs, crash records and settings in one zip
    dev.py update --gui                                    all of it in a window

--game, --remote and --branch are remembered (dev_build.json's "update"); the first --game names the folder.

An update:
  1. fetches the branch into this checkout's repository and puts a worktree of its own (the development folder's
     update/tree, detached) on the newest commit (--pick: on the folder's commit, the picked pull requests' commits
     cherry-picked onto it in their order; one that needs a pull request left out stops it, named): your checkouts and
     branches are not touched; --with then merges each open pull request's head (refs/pull/N/head, which Forgejo, Gitea
     and GitHub keep) onto that, in number order; one that conflicts stops it, named with the files;
  2. backs up the folder's build files (the game, the renderer, image.bin, longshot_env.txt, longshot_options.ini, the
     texts...) into update/backups/, the last three kept; saves and mods are not touched by a build;
  3. builds that tree into the folder with its own dev.py when it has this command (so its own tools.lock and builder),
     through the job queue, in the player's build environment, sharing the development folder's stores: when the
     translation did not change, only what changed compiles. A folder that keeps its game files (romfs/,
     .longshot_exefs/) is built again from them (make_game.py --update); another from the game files dev.py finds
     (dev_build.json's "romfs" and "exefs");
  4. on failure, or when stopped, puts the backup back (a stopped update is put back by the next `dev.py update` command);
     on success writes the commit into the folder (.longshot_dev_update; the picks into .longshot_dev_update_picks, the
     open pull requests into .longshot_dev_update_open) and what came in into update/history.md. An update without
     --with leaves out the open pull requests the folder had.
The open pull requests are read from the remote's forge (its URL: a Forgejo or Gitea server's /api/v1, or GitHub's API),
without signing in.
It refuses while the game in that folder is running.

--logs zips logs/ (the log, its older copies, the frame log), bug-reports/ but bug-reports/local-only/ (minidumps: game and
private data, never shared), longshot_env.txt, longshot_options.ini, the driver cache notes, the last update's build log
and history, and a summary of the PC and the build, into update/logs/. Logs can name your user folder: read before sharing.
"""
import argparse
import datetime
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
import urllib.request
import zipfile

from . import build, common, jobqueue, procs

ROOT = common.ROOT
WHO = "dev.py update"
MARK = ".longshot_dev_update"   # (in the game folder: the commit of the branch it was built from)
PICKS = ".longshot_dev_update_picks"   # (and, after --pick, the commits picked onto it: a JSON list)
OPEN = ".longshot_dev_update_open"   # (and, after --with, the open pull requests merged on top: [{pr, sha, title}])
KEEP = 3
GAME_EXE = "longshot.exe" if common.WINDOWS else "longshot"
BACKED_UP = (GAME_EXE, "longshot_vk.dll" if common.WINDOWS else "longshot_vk.so", "image.bin", "longshot_env.txt",
             "longshot_options.ini", "language.txt", "exefs_patches.txt", "longshot_pipeline_states_shipped.bin", MARK,
             PICKS, OPEN)
BACKED_UP_DIRS = ("texts",)
PR_NUMBER = re.compile(r"\(#(\d+)\)\s*$")
PICKER = ["-c", "user.name=dev.py update", "-c", "user.email=dev-update@localhost"]   # (the picks' committer: local only)
# (what --logs takes from the game folder: (path, a folder's files at most this many newest, a file at most this many MB))
LOG_PARTS = (("logs", 12, 64), ("bug-reports", 40, 8), ("longshot_env.txt", 1, 1), ("longshot_options.ini", 1, 1),
             ("language.txt", 1, 1), ("exefs_patches.txt", 1, 1), ("cache/longshot_driver_env.txt", 1, 1), (MARK, 1, 1),
             (PICKS, 1, 1), (OPEN, 1, 1))
NEVER_SHARED = ("local-only",)   # (bug-reports/local-only: HOS_CRASH_MINIDUMP's minidumps hold game and private data)


def say(text):
    common.say(WHO, text)


def die(text):
    common.die(WHO, text)


def part(rel):
    return common.dev_path("update/" + rel)


# ---- settings ----
def settings(a=None):
    """{game, remote, branch}: the options given (remembered in dev_build.json's "update"), else the remembered ones"""
    c = build.config().get("update") or {}
    s = {"game": c.get("game", ""), "remote": c.get("remote", "origin"), "branch": c.get("branch", "main")}
    given = {k: getattr(a, k) for k in s if a is not None and getattr(a, k, None)}
    if given.get("game"):
        given["game"] = os.path.abspath(os.path.expanduser(given["game"]))
    if given and any(s[k] != v for k, v in given.items()):
        s.update(given)
        remember(s)
    return s


def remember(s):
    path = common.config_file(build.CONFIG)
    c = common.read_config(build.CONFIG)
    c["update"] = dict(s)
    common.ensure_dir(os.path.dirname(path))
    with open(path + ".tmp", "w", encoding="utf-8") as f:
        json.dump(c, f, indent=2)
    os.replace(path + ".tmp", path)


# ---- the branch ----
def git(*args, cwd=None):
    return common.output(WHO, ["git", "-C", cwd or ROOT] + list(args))


def fetch(s):
    git("fetch", "-q", s["remote"], s["branch"])
    return git("rev-parse", "FETCH_HEAD")


def installed(game):
    """(the branch's commit the folder was built from, or ""; the commits picked onto it)"""
    try:
        with open(os.path.join(game, MARK), encoding="utf-8") as f:
            old = f.read().strip()
    except OSError:
        return "", []
    try:
        with open(os.path.join(game, PICKS), encoding="utf-8") as f:
            picks = json.load(f)
    except (OSError, ValueError):
        picks = []
    return old, picks if isinstance(picks, list) else []


def installed_open(game):
    """The open pull requests merged into the folder's build (--with): [{pr, sha, title}]"""
    try:
        with open(os.path.join(game, OPEN), encoding="utf-8") as f:
            got = json.load(f)
    except (OSError, ValueError):
        return []
    return got if isinstance(got, list) else []


def known(commit):
    return bool(commit) and subprocess.run(["git", "-C", ROOT, "cat-file", "-e", commit + "^{commit}"],
                                           capture_output=True).returncode == 0


def merged(old, new, picks=()):
    """The branch's commits from old to new, oldest first, as {sha, short, subject, pr, picked}; the last 15 when old is not
    known here. picked: in the folder's build already (--pick)"""
    rng = ["%s..%s" % (old, new)] if known(old) else ["-15", new]
    out = git("log", "--first-parent", "--reverse", "--format=%H%x00%h%x00%s", *rng)
    rows = []
    for line in out.splitlines():
        sha, short, subject = (line.split("\0") + ["", ""])[:3]
        m = PR_NUMBER.search(subject)
        rows.append({"sha": sha, "short": short, "subject": subject, "pr": int(m.group(1)) if m else None,
                     "picked": sha in picks})
    return rows


def chosen(rows, picks):
    """The rows --pick names (pull request numbers, #numbers or commits, comma-separated), oldest first"""
    want = [p.strip().lstrip("#") for p in picks.split(",") if p.strip()]
    found = []
    for w in want:
        hit = [r for r in rows if (w.isdigit() and r["pr"] == int(w)) or (len(w) >= 7 and r["sha"].startswith(w))]
        if not hit:
            die("%s is not one of the pull requests merged since the folder's build (`dev.py update --check` lists them)" % w)
        found += [r for r in hit if r not in found]
    return [r for r in rows if r in found]


def tree_at(commit):
    """update/tree, a worktree of this repository detached at commit"""
    t = part("tree")
    if not os.path.isdir(t):
        common.ensure_dir(os.path.dirname(t))
        git("worktree", "add", "-q", "--detach", t, commit)
        return t
    if subprocess.run(["git", "-C", t, "rev-parse", "-q", "--verify", "CHERRY_PICK_HEAD"], capture_output=True).returncode == 0:
        subprocess.run(["git", "-C", t, "cherry-pick", "--abort"], capture_output=True)   # (a pick an update left half done)
    if git("status", "--porcelain", cwd=t):
        die("%s has changes of its own: it is only for dev.py update. Delete it (git worktree remove --force %s)" % (t, t))
    git("checkout", "-q", "--detach", commit, cwd=t)
    return t


def pick_onto(tree, rows, skipped):
    """rows' commits cherry-picked onto the tree in their order; one that does not apply stops it, with what it touches"""
    for r in rows:
        p = subprocess.run(["git"] + PICKER + ["-C", tree, "cherry-pick", "--allow-empty", r["sha"]], capture_output=True,
                           text=True, encoding="utf-8", errors="replace")
        if p.returncode:
            files = subprocess.run(["git", "-C", tree, "diff", "--name-only", "--diff-filter=U"], capture_output=True,
                                   text=True).stdout.split()
            subprocess.run(["git", "-C", tree, "cherry-pick", "--abort"], capture_output=True)
            before = [s for s in skipped if s["sha"] in set(git("log", "--format=%H", "-n", "400", r["sha"]).split())]
            touching = [s for s in before if set(files) & set(git("show", "--name-only", "--format=", s["sha"]).split())]
            need = ", ".join(label(s) for s in (touching or before)[-3:]) or "an earlier commit"
            die("%s does not apply without %s, which you left out (it conflicts in %s): pick that too, or update to "
                "everything" % (label(r), need, ", ".join(files[:4]) or "its files"))
    return git("rev-parse", "HEAD", cwd=tree)


def forge_api(s):
    """The API of the remote's repository: a Forgejo or Gitea server's /api/v1/repos/<owner>/<repo>, GitHub's; None"""
    url = git("remote", "get-url", s["remote"])
    m = re.match(r"(?:https?://(?:[^@/]+@)?([^/]+)/|[^@/]+@([^:/]+):|ssh://(?:[^@/]+@)?([^/:]+)(?::\d+)?/)(.+?)(?:\.git)?/?$",
                 url)
    if not m:
        return None
    host, repo = m.group(1) or m.group(2) or m.group(3), m.group(4)
    return "https://api.github.com/repos/" + repo if host == "github.com" else "https://%s/api/v1/repos/%s" % (host, repo)


def open_prs(s):
    """(the open pull requests into the branch, newest first, as {pr, title, author, sha, draft, mergeable}; why the list
    is short or empty, or "")"""
    api = forge_api(s)
    if not api:
        return [], "the remote's URL names no forge"
    rows = []
    try:
        for page in range(1, 11):
            req = urllib.request.Request("%s/pulls?state=open&limit=50&per_page=50&page=%d" % (api, page),
                                         headers={"Accept": "application/json", "User-Agent": "longshot-dev-update/1"})
            with urllib.request.urlopen(req, timeout=30) as r:
                got = json.loads(r.read())
            for p in got:
                if (p.get("base") or {}).get("ref") not in (None, s["branch"]):
                    continue
                title = p.get("title") or ""
                rows.append({"pr": p["number"], "title": title, "author": (p.get("user") or {}).get("login", ""),
                             "sha": (p.get("head") or {}).get("sha", ""), "mergeable": p.get("mergeable"),
                             "draft": bool(p.get("draft")) or title.upper().startswith("WIP")})
            if len(got) < 50:
                break
    except (OSError, ValueError) as e:
        return sorted(rows, key=lambda r: -r["pr"]), "the forge's open pull requests could not be read (%s)" % e
    return sorted(rows, key=lambda r: -r["pr"]), ""


def numbers(text):
    """"1913,#1925 1930": [1913, 1925, 1930]"""
    found = re.findall(r"\d+", text or "")
    if text and not found:
        die("--with takes pull request numbers (1913,1925)")
    return sorted({int(n) for n in found})


def merge_open(tree, s, prs, titles):
    """Each open pull request's head merged onto the tree in number order: [{pr, sha, title}]; one that does not merge
    stops it, with the files it conflicts in"""
    done = []
    for n in prs:
        f = subprocess.run(["git", "-C", tree, "fetch", "-q", s["remote"], "refs/pull/%d/head" % n], capture_output=True,
                           text=True, encoding="utf-8", errors="replace")
        if f.returncode:
            die("#%d has no head on %s (not a pull request there, or gone): %s" % (n, s["remote"], f.stderr.strip()[-200:]))
        sha = git("rev-parse", "FETCH_HEAD", cwd=tree)
        m = subprocess.run(["git"] + PICKER + ["-C", tree, "merge", "--no-ff", "--no-edit", "-m",
                                               "dev.py update: open pull request #%d" % n, sha],
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
        if m.returncode:
            files = subprocess.run(["git", "-C", tree, "diff", "--name-only", "--diff-filter=U"], capture_output=True,
                                   text=True).stdout.split()
            subprocess.run(["git", "-C", tree, "merge", "--abort"], capture_output=True)
            die("#%d does not merge onto your build (it conflicts in %s%s): leave it out, or try it with main's newest%s" % (
                n, ", ".join(files[:4]) or "its files",
                " with " + ", ".join("#%d" % d["pr"] for d in done) if done else "",
                " or without the picks" if s.get("picked") else ""))
        done.append({"pr": n, "sha": sha, "title": titles.get(n, "")})
    return done


def label(r):
    return "#%d" % r["pr"] if r["pr"] else r["short"]


# ---- the game folder ----
def running_in(game):
    """A game started from the folder is running (any longshot whose path can't be read counts too)"""
    want = os.path.normcase(os.path.abspath(game))
    for pid, exe in procs.exes().items():
        if exe.lower() != GAME_EXE:
            continue
        argv = procs.argv_of(pid)
        if not argv or os.path.normcase(os.path.dirname(os.path.abspath(argv[0]))) == want:
            return True
    return False


def backups():
    d = part("backups")
    return sorted((os.path.join(d, n) for n in os.listdir(d) if os.path.isdir(os.path.join(d, n))), reverse=True) \
        if os.path.isdir(d) else []


def back_up(game, tag):
    stamp = time.strftime("%Y%m%d-%H%M%S")
    d, n = os.path.join(part("backups"), stamp + "-" + tag), 1
    while os.path.exists(d):   # (two in one second)
        n += 1
        d = os.path.join(part("backups"), "%s-%s-%d" % (stamp, tag, n))
    os.makedirs(d)
    for f in BACKED_UP:
        if os.path.isfile(os.path.join(game, f)):
            shutil.copy2(os.path.join(game, f), d)
    for sub in BACKED_UP_DIRS:
        if os.path.isdir(os.path.join(game, sub)):
            shutil.copytree(os.path.join(game, sub), os.path.join(d, sub))
    with open(os.path.join(d, "game.txt"), "w", encoding="utf-8") as f:
        f.write(game)
    for old in backups()[KEEP:]:
        shutil.rmtree(old, ignore_errors=True)
    return d


def put_back(d):
    with open(os.path.join(d, "game.txt"), encoding="utf-8") as f:
        game = f.read().strip()
    for f in BACKED_UP:
        src, dst = os.path.join(d, f), os.path.join(game, f)
        if os.path.isfile(src):
            shutil.copy2(src, dst)
        elif f in (MARK, PICKS, OPEN) and os.path.isfile(dst):   # (the folder had none then: it has none again)
            os.remove(dst)
    for sub in BACKED_UP_DIRS:
        if os.path.isdir(os.path.join(d, sub)):
            shutil.rmtree(os.path.join(game, sub), ignore_errors=True)
            shutil.copytree(os.path.join(d, sub), os.path.join(game, sub))
    return game


def pending_file():
    return part("pending.json")


def recover():
    """An update that never finished (stopped, or its PC went down): its backup put back, once its process is gone"""
    try:
        with open(pending_file(), encoding="utf-8") as f:
            p = json.load(f)
    except (OSError, ValueError):
        return
    start = procs.proc_start(p.get("pid", 0))
    if start is not None and start == p.get("start"):
        die("an update is running (process %s): wait for it, or stop it first" % p.get("pid"))
    if os.path.isdir(p.get("backup", "")):
        say("the update to %s did not finish: the game folder is put back as it was" % p.get("commit", "?")[:8])
        put_back(p["backup"])
    os.remove(pending_file())


def history(game, old, new, rows, minutes, picked=False, opened=()):
    path = part("history.md")
    first = not os.path.exists(path)
    with open(path, "a", encoding="utf-8") as f:
        if first:
            f.write("# dev.py update\n")
        f.write("\n## %s: %s, %s%s (%.1f min%s)\n" % (datetime.datetime.now().strftime("%Y-%m-%d %H:%M"), game, new[:8],
                                                      " with picks" if picked else "", minutes,
                                                      ", from " + old[:8] if old else ""))
        f.writelines("- %s %s\n" % (r["short"], r["subject"]) for r in rows)
        f.writelines("- open #%d %s (%s)\n" % (o["pr"], o.get("title", ""), o["sha"][:8]) for o in opened)


# ---- logs ----
def gather_logs(game, out_dir=None):
    """The zip --logs makes (module notes): its path"""
    out_dir = out_dir or part("logs")
    common.ensure_dir(out_dir)
    zpath = os.path.join(out_dir, "longshot-logs-%s.zip" % time.strftime("%Y%m%d-%H%M%S"))
    taken = []
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for rel, newest, max_mb in LOG_PARTS:
            p = os.path.join(game, *rel.split("/"))
            files = []
            if os.path.isfile(p):
                files = [p]
            elif os.path.isdir(p):
                for d, subs, names in os.walk(p):
                    subs[:] = [s for s in subs if s not in NEVER_SHARED and not s.startswith("profile-")]
                    files += [os.path.join(d, n) for n in names]
                files = sorted(files, key=os.path.getmtime, reverse=True)[:newest]
            for f in files:
                if os.path.getsize(f) <= max_mb << 20:
                    arc = os.path.join("game", os.path.relpath(f, game))
                    z.write(f, arc)
                    taken.append(arc)
        hist = part("history.md")
        if os.path.isfile(hist):
            z.write(hist, "update/history.md")
        last = sorted((os.path.join(part(""), n) for n in os.listdir(part("")) if n.startswith("build-") and n.endswith(".log")),
                      key=os.path.getmtime, reverse=True)[:1] if os.path.isdir(part("")) else []
        for f in last:
            z.write(f, "update/" + os.path.basename(f))
        z.writestr("summary.txt", summary(game, taken))
    return zpath


def summary(game, taken):
    old, picks = installed(game)
    lines = ["made by dev.py update --logs, %s" % time.strftime("%Y-%m-%d %H:%M:%S %z"),
             "system: %s %s (%s), Python %s" % (platform.system(), platform.release(), platform.machine(),
                                                platform.python_version()),
             "game folder built from: %s%s%s" % (old or "unknown", " + %d picked commits" % len(picks) if picks else "",
                                                 "".join(" + open #%d (%s)" % (o["pr"], o["sha"][:8])
                                                         for o in installed_open(game)))]
    if shutil.which("nvidia-smi"):
        r = subprocess.run(["nvidia-smi", "--query-gpu=name,driver_version,memory.total", "--format=csv,noheader"],
                           capture_output=True, text=True)
        lines.append("GPU: " + r.stdout.strip().replace("\n", "; "))
    lines.append("files:")
    lines += ["  " + t for t in taken]
    return "\n".join(lines) + "\n"


def open_folder(path):
    if common.WINDOWS:
        os.startfile(path)
    else:
        subprocess.Popen(["xdg-open", path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


# ---- the build ----
def build_cmd(tree, game, log):
    """(python, env, make_game.py's command) for the folder"""
    py, env = build.environment()
    env["LONGSHOT_KEEP_BUILD_FILES"] = "1"
    if os.path.isdir(os.path.join(game, "romfs")) and os.path.isdir(os.path.join(game, ".longshot_exefs")):
        args = ["--update", game]
    else:
        romfs, exefs = build.game_files(build.config())
        args = ["--romfs", romfs, "--exefs", exefs, "--out", game, "--language", "en-US"]
    if common.WINDOWS:
        build.windows_prebuilt(tree)
    cmd = [py, "-I", "nx2/make_game.py"] + args + ["--work", part("work"), "--log", log]
    if build.config().get("fsr"):
        cmd.append("--fsr")
    return py, env, cmd


def run_build(tree, game, commit):
    """Builds tree into game: with the tree's own dev.py when it has this command, else here. The build's exit status"""
    if os.path.isfile(os.path.join(tree, "scripts", "devtools", "update.py")) and \
            os.path.normcase(os.path.realpath(tree)) != os.path.normcase(os.path.realpath(ROOT)):
        return subprocess.call([sys.executable, "-I", os.path.join(tree, "scripts", "dev.py"), "update", "--build-here",
                                "--game", game, "--commit", commit])
    return build_here(tree, game, commit)


def build_here(tree, game, commit):
    py, env, cmd = build_cmd(tree, game, part("build-%s.log" % commit[:8]))
    say("building %s into %s (the log: %s)" % (commit[:8], game, part("build-%s.log" % commit[:8])))
    return jobqueue.run("update-" + commit[:8], "build", cmd, environ=env, cwd=tree)


# ---- the commands ----
def cmd_update(a):
    if a.gui:
        return gui()
    if a.build_here:   # (run_build's step, in the tree being built)
        return build_here(ROOT, os.path.abspath(a.game), a.commit)
    recover()
    s = settings(a)
    if not s["game"]:
        die("which game folder? Name it once with --game DIR")
    game = s["game"]
    if a.logs:
        z = gather_logs(game)
        say("the logs are in %s (logs can name your user folder: read them before sharing)" % z)
        if a.open:
            open_folder(os.path.dirname(z))
        return 0
    if a.rollback:
        b = backups()
        if not b:
            die("there is no backup to put back")
        if running_in(game):
            die("the game in %s is running: close it first" % game)
        put_back(b[0])
        say("the game folder is back as it was in %s" % os.path.basename(b[0]))
        return 0
    new = fetch(s)
    old, picks = installed(game)
    opened = installed_open(game)
    rows = merged(old, new, picks)
    where = "%s/%s" % (s["remote"], s["branch"])
    if a.list_json:
        prs, note = open_prs(s)
        inside = {o["pr"]: o["sha"] for o in opened}
        for o in prs:
            o["in_build"] = o["pr"] in inside
            o["moved"] = o["in_build"] and inside[o["pr"]] != o["sha"]
        print(json.dumps({"game": game, "branch": where, "old": old, "new": new, "known": known(old), "picks": picks,
                          "commits": rows, "open": prs, "open_note": note,
                          "open_in_build": [o["pr"] for o in opened]}), flush=True)
        return 0
    if old == new:
        say("%s is on %s's newest commit, %s" % (game, where, new[:8]))
    elif known(old):
        say("%s is at %s, the game folder at %s%s: %d new commits" % (where, new[:8], old[:8],
                                                             " (+%d picked)" % len(picks) if picks else "", len(rows)))
    else:
        say("%s is at %s; the game folder's commit is not known here (the last 15):" % (where, new[:8]))
    for r in rows if old != new else []:
        print("  %s %s%s" % (r["short"], r["subject"], "   [in your build]" if r["picked"] else ""), flush=True)
    if opened:
        say("open pull requests merged into the folder's build: %s" % ", ".join("#%d" % o["pr"] for o in opened))
    if a.check:
        return 0
    with_ = numbers(a.with_)
    take, base = rows, new
    if a.pick:
        if not known(old):
            die("picking needs the folder's commit, which isn't known here: update it to everything once first")
        take = chosen(rows, a.pick)
        if [r["sha"] for r in take] == [r["sha"] for r in rows]:
            take, a.pick = rows, None   # (every one: the branch's newest commit itself)
        else:
            base = old
    elif old == new and not picks and not opened and not with_ and not a.force:
        return 0
    if opened and not with_:
        say("the open pull requests in the folder's build (%s) are left out: --with keeps them" %
            ", ".join("#%d" % o["pr"] for o in opened))
    if running_in(game):
        die("the game in %s is running: close it first" % game)
    tree = tree_at(base)
    built = base
    if a.pick:
        say("picking %s onto %s" % (", ".join(label(r) for r in take), old[:8]))
        built = pick_onto(tree, take, [r for r in rows if r not in take])
    merged_open = []
    if with_:
        say("merging the open pull requests %s on top" % ", ".join("#%d" % n for n in with_))
        titles = {o["pr"]: o["title"] for o in open_prs(s)[0]}
        merged_open = merge_open(tree, dict(s, picked=bool(a.pick)), with_, titles)
        built = git("rev-parse", "HEAD", cwd=tree)
    common.ensure_dir(game)
    bk = back_up(game, old[:8] if old else "first")
    say("backed up the game folder's build files to %s" % bk)
    with open(pending_file(), "w", encoding="utf-8") as f:
        json.dump({"pid": os.getpid(), "start": procs.proc_start(os.getpid()), "backup": bk, "commit": built}, f)
    t0 = time.time()
    try:
        rc = run_build(tree, game, built)
    except BaseException:   # (Ctrl+C, or anything else: the folder as it was)
        put_back(bk)
        os.remove(pending_file())
        raise
    minutes = (time.time() - t0) / 60
    if rc:
        put_back(bk)
        os.remove(pending_file())
        die("the build failed (exit status %d, after %.1f min): the game folder is back as it was" % (rc, minutes))
    with open(os.path.join(game, MARK), "w", encoding="utf-8") as f:
        f.write(base)
    for name, value in ((PICKS, [r["sha"] for r in take] if a.pick else None), (OPEN, merged_open or None)):
        path = os.path.join(game, name)
        if value:
            with open(path, "w", encoding="utf-8") as f:
                json.dump(value, f)
        elif os.path.exists(path):
            os.remove(path)
    os.remove(pending_file())
    history(game, old, base, take, minutes, picked=bool(a.pick), opened=merged_open)
    say("done in %.1f min: %s is on %s %s%s%s" % (
        minutes, game, where, base[:8], " + %s" % ", ".join(label(r) for r in take) if a.pick else "",
        " + open %s" % ", ".join("#%d" % o["pr"] for o in merged_open) if merged_open else ""))
    return 0


# ---- the window ----
KINDS = (("feat", "#1a7f37"), ("fix", "#c2410c"), ("perf", "#1d4ed8"), ("revert", "#b91c1c"))   # (the PR list's colours)
DIM = "#6b7280"


def pr_colour(subject):
    for kind, colour in KINDS:
        if re.match(kind + r"(\(|:|!)", subject):
            return colour
    return DIM if re.match(r"(docs|test|ci|build|chore|style|refactor)(\(|:|!)", subject) else "#111827"


def log_tag(line):
    """The build log's colour for a line"""
    low = line.lower()
    if line.startswith("@@stage") or line.startswith("> "):
        return "stage"
    if "error" in low or "failed" in low or low.startswith("fail") or "fatal" in low or "traceback" in low:
        return "error"
    if "warn" in low or "[warn]" in low:
        return "warn"
    if "done in" in low or line.startswith("@@done") or "is on " in low and "newest" in low:
        return "ok"
    if line.startswith("dev.py") or line.startswith(WHO):
        return "info"
    if line.startswith("  ") and re.match(r"\s+[0-9a-f]{7,} ", line):
        return "commit"
    return ""


def matches(row, query):
    """The window's search: a number (with or without #) matches the pull requests whose number holds it; other text the
    title, the author or the commit"""
    q = query.strip().lstrip("#").strip()
    if not q:
        return True
    if q.isdigit():
        return row.get("pr") is not None and q in str(row["pr"])
    text = " ".join(str(row.get(k) or "") for k in ("subject", "title", "author", "short"))
    return q.lower() in text.lower()


def gui():
    """The window: the pull requests merged since the folder's build (ticked to pick; all: the newest commit) and the open
    ones (ticked to merge on top), both searchable by number, beside the build's log in colour. Each action runs
    `dev.py update ...`; Stop ends it, and the folder is put back by the check that follows"""
    import queue
    import threading
    import tkinter as tk
    import tkinter.font as tkfont
    from tkinter import filedialog, messagebox, scrolledtext, ttk

    s = settings()
    root = tk.Tk()
    root.title("dev.py update")
    root.geometry("1180x660")
    root.minsize(800, 440)
    q = queue.Queue()
    st = {"proc": None, "t0": None, "stage": "Ready", "merged": [], "open": [], "after": None, "note": ""}

    top = ttk.Frame(root, padding=(8, 8, 8, 4))
    top.pack(fill="x")
    game, remote, branch = tk.StringVar(value=s["game"]), tk.StringVar(value=s["remote"]), tk.StringVar(value=s["branch"])
    ttk.Label(top, text="Game folder").grid(row=0, column=0, sticky="w")
    ttk.Entry(top, textvariable=game).grid(row=0, column=1, columnspan=4, sticky="we", padx=4)

    def browse():
        p = filedialog.askdirectory(parent=root, initialdir=game.get() or os.path.expanduser("~"))
        if p:
            game.set(os.path.normpath(p))
    ttk.Button(top, text="Browse", command=browse).grid(row=0, column=5)
    ttk.Label(top, text="Remote").grid(row=1, column=0, sticky="w", pady=(4, 0))
    ttk.Entry(top, textvariable=remote, width=14).grid(row=1, column=1, sticky="w", padx=4, pady=(4, 0))
    ttk.Label(top, text="Branch").grid(row=1, column=2, sticky="e", pady=(4, 0))
    ttk.Entry(top, textvariable=branch, width=18).grid(row=1, column=3, sticky="w", padx=4, pady=(4, 0))
    top.columnconfigure(4, weight=1)

    row = ttk.Frame(root, padding=(8, 0))
    row.pack(fill="x")
    force = tk.BooleanVar(value=False)
    btn = {}
    for key, text in (("check", "Check"), ("update", "Update"), ("rollback", "Roll back"), ("stop", "Stop"),
                      ("logs", "Gather logs")):
        btn[key] = ttk.Button(row, text=text)
        btn[key].pack(side="left", padx=(0, 6))
    ttk.Checkbutton(row, text="Build even if up to date", variable=force).pack(side="left", padx=(8, 0))
    bar = ttk.Frame(root, padding=(8, 6))
    bar.pack(fill="x")
    prog = ttk.Progressbar(bar, mode="determinate")
    prog.pack(side="left", fill="x", expand=True)
    stage = tk.StringVar(value="Ready")
    ttk.Label(bar, textvariable=stage, width=50).pack(side="left", padx=(8, 0))

    panes = ttk.PanedWindow(root, orient="horizontal")
    panes.pack(fill="both", expand=True, padx=8, pady=(0, 8))
    left = ttk.LabelFrame(panes, text="Pull requests", padding=4)
    right = ttk.LabelFrame(panes, text="Build log", padding=4)
    panes.add(left, weight=2)
    panes.add(right, weight=3)
    search = tk.StringVar()
    srow = ttk.Frame(left)
    srow.grid(row=0, column=0, columnspan=4, sticky="we", pady=(0, 4))
    ttk.Label(srow, text="Search (number or text)").pack(side="left")
    sbox = ttk.Entry(srow, textvariable=search)
    sbox.pack(side="left", fill="x", expand=True, padx=(6, 0))
    tree = ttk.Treeview(left, columns=("on", "pr", "title"), show="headings", selectmode="browse")
    for col, text, width, stretch in (("on", "", 30, False), ("pr", "PR", 64, False), ("title", "Title", 420, True)):
        tree.heading(col, text=text)
        tree.column(col, width=width, stretch=stretch, anchor="center" if col != "title" else "w")
    ysb = ttk.Scrollbar(left, orient="vertical", command=tree.yview)
    tree.configure(yscrollcommand=ysb.set)
    tree.grid(row=1, column=0, columnspan=3, sticky="nsew")
    ysb.grid(row=1, column=3, sticky="ns")
    left.rowconfigure(1, weight=1)
    left.columnconfigure(2, weight=1)
    count = tk.StringVar(value="Press Check to list them")
    ttk.Button(left, text="All merged", command=lambda: tick_merged(True)).grid(row=2, column=0, pady=(4, 0), sticky="w")
    ttk.Button(left, text="No open", command=lambda: clear_open()).grid(row=2, column=1, pady=(4, 0), sticky="w", padx=4)
    ttk.Label(left, textvariable=count).grid(row=2, column=2, sticky="e", pady=(4, 0))
    out = scrolledtext.ScrolledText(right, wrap="word", height=16,
                                    font=("Consolas", 9) if common.WINDOWS else ("monospace", 9))
    out.pack(fill="both", expand=True)
    heavy = tkfont.Font(root, font=out.cget("font"))
    heavy.configure(weight="bold")
    for tag, colour, bold in (("error", "#b91c1c", True), ("warn", "#b45309", False), ("ok", "#15803d", True),
                              ("info", "#1d4ed8", False), ("stage", "#7c3aed", True), ("commit", DIM, False)):
        out.tag_configure(tag, foreground=colour, **({"font": heavy} if bold else {}))
    out.configure(state="disabled")
    theavy = tkfont.nametofont("TkDefaultFont").copy()
    theavy.configure(weight="bold")
    tree.tag_configure("head", font=theavy, background="#e5e7eb")
    tree.tag_configure("dim", foreground="#9ca3af")

    def write(t, tag=None):
        out.configure(state="normal")
        out.insert("end", t + "\n", (tag or log_tag(t),))
        out.see("end")
        out.configure(state="disabled")

    def show_count():
        m, o = st["merged"], st["open"]
        mon, oon = sum(1 for r in m if r["on"]), sum(1 for r in o if r["on"])
        parts = []
        if m:
            parts.append("all %d merged" % len(m) if mon == len(m) else "%d of %d merged" % (mon, len(m)))
        if o:
            parts.append("%d open on top" % oon)
        count.set(" · ".join(parts) or "nothing new")

    def render(*_):
        tree.delete(*tree.get_children())
        q_ = search.get()
        shown = [(i, r) for i, r in enumerate(st["merged"]) if matches(r, q_)]
        tree.insert("", "end", iid="hm", values=("", "", "Merged since your build (%d%s)" % (
            len(st["merged"]), ", %d shown" % len(shown) if q_.strip() else "")), tags=("head",))
        for i, r in shown:
            colour = pr_colour(r["subject"])
            tree.tag_configure(colour, foreground=colour)
            title = PR_NUMBER.sub("", r["subject"]).strip() + ("   (in your build)" if r.get("picked") else "")
            tree.insert("", "end", iid="m%d" % i, values=("☑" if r["on"] else "☐", "#%d" % r["pr"] if r["pr"] else r["short"],
                                                          title), tags=(colour,))
        shown = [(i, r) for i, r in enumerate(st["open"]) if matches(r, q_)]
        tree.insert("", "end", iid="ho", values=("", "", "Open, not merged yet (%d%s)%s" % (
            len(st["open"]), ", %d shown" % len(shown) if q_.strip() else "",
            ": " + st["note"] if st["note"] else "")), tags=("head",))
        for i, r in shown:
            extra = "   (in your build%s)" % (", updated since" if r.get("moved") else "") if r.get("in_build") else ""
            colour = pr_colour(r["title"].split(":", 1)[-1].strip() if r["title"].upper().startswith("WIP") else r["title"])
            tree.tag_configure(colour, foreground=colour)
            tree.insert("", "end", iid="o%d" % i, values=("☑" if r["on"] else "☐", "#%d" % r["pr"],
                                                          "%s  · %s%s" % (r["title"], r["author"], extra)),
                        tags=("dim",) if r.get("draft") else (colour,))
        show_count()

    def toggle(iid):
        if not iid or iid in ("hm", "ho"):
            return
        r = (st["merged"] if iid[0] == "m" else st["open"])[int(iid[1:])]
        r["on"] = not r["on"]
        tree.set(iid, "on", "☑" if r["on"] else "☐")
        show_count()
    tree.bind("<Button-1>", lambda e: toggle(tree.identify_row(e.y)))
    tree.bind("<space>", lambda e: toggle(tree.focus()))
    search.trace_add("write", render)

    def tick_merged(on):
        for r in st["merged"]:
            r["on"] = on
        render()

    def clear_open():
        for r in st["open"]:
            r["on"] = False
        render()

    def busy(on):
        for k, b in btn.items():
            b.configure(state="normal" if (k == "stop") == on else "disabled")
        if on:
            prog.configure(mode="indeterminate")
            prog.start(12)
        else:
            prog.stop()
            prog.configure(mode="determinate", value=0)   # (empty while idle)
        st["t0"] = time.time() if on else None

    def base_cmd():
        return [sys.executable, "-I", os.path.join(ROOT, "scripts", "dev.py"), "update", "--game", game.get().strip(),
                "--remote", remote.get().strip() or "origin", "--branch", branch.get().strip() or "main"]

    def run(extra, after=None, quiet=False):
        if not game.get().strip():
            messagebox.showinfo("dev.py update", "Choose the game folder first.", parent=root)
            return
        busy(True)
        st["stage"], st["after"] = "Working", after
        if not quiet:
            write("> dev.py update " + " ".join(extra), "stage")

        def work():
            kw = {"creationflags": 0x08000000} if common.WINDOWS else {"start_new_session": True}   # (no console window)
            p = subprocess.Popen(base_cmd() + list(extra), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, text=True, encoding="utf-8", errors="replace",
                                 env=dict(os.environ, PYTHONIOENCODING="utf-8"), **kw)
            st["proc"] = p
            lines = []
            for line in p.stdout:
                line = line.rstrip("\n")
                lines.append(line)
                if not quiet:
                    q.put(("line", line))
            q.put(("end", p.wait(), lines))
        threading.Thread(target=work, daemon=True).start()

    def check():
        def listed(rc, lines):
            try:
                d = json.loads(next(l for l in reversed(lines) if l.startswith("{")))
            except (StopIteration, ValueError):
                for l in lines:
                    write(l)
                return
            for l in lines:
                if not l.startswith("{"):
                    write(l)
            st["merged"] = [dict(r, on=True) for r in (d["commits"] if d["old"] != d["new"] else [])]
            st["open"] = [dict(r, on=bool(r.get("in_build"))) for r in d.get("open", [])]
            st["note"] = d.get("open_note", "")
            render()
            if d["old"] == d["new"]:
                write("%s is on %s's newest commit, %s" % (d["game"], d["branch"], d["new"][:8]), "ok")
            elif d["known"]:
                write("%s is at %s, your build at %s: %d merged since%s" % (
                    d["branch"], d["new"][:8], d["old"][:8], len(d["commits"]),
                    " (%d picked into it already)" % len(d["picks"]) if d["picks"] else ""), "info")
            else:
                write("%s is at %s; your build's commit isn't known here, so the last 15 are listed and only all of them "
                      "can be built" % (d["branch"], d["new"][:8]), "warn")
            if d.get("open_in_build"):
                write("open pull requests in your build: %s" % ", ".join("#%d" % n for n in d["open_in_build"]), "info")
            write("%d open pull requests listed%s" % (len(st["open"]), ": " + st["note"] if st["note"] else ""),
                  "warn" if st["note"] else "info")
        write("> check", "stage")
        run(["--list-json"], after=listed, quiet=True)

    def update():
        m, o = st["merged"], st["open"]
        mon, oon = [r for r in m if r["on"]], [r for r in o if r["on"]]
        if m and not mon:
            messagebox.showinfo("dev.py update", "Tick at least one merged pull request, or All merged.", parent=root)
            return
        extra = ["--force"] if force.get() else []
        if m and len(mon) < len(m):
            extra = ["--pick", ",".join(str(r["pr"]) if r["pr"] else r["sha"][:12] for r in mon)]
        if oon:
            extra += ["--with", ",".join(str(r["pr"]) for r in oon)]
        run(extra, after=lambda rc, lines: check())

    def logs():
        run(["--logs", "--open"])

    def stop():
        p = st["proc"]
        if p and p.poll() is None and messagebox.askyesno("Stop", "Stop it? The game folder is put back as it was.",
                                                          parent=root):
            if common.WINDOWS:
                procs.win_kill_tree(p.pid)
            else:
                import signal
                os.killpg(p.pid, signal.SIGTERM)
            write("stopped: the next check puts the game folder back", "warn")

    def pump():
        try:
            while True:
                m = q.get_nowait()
                if m[0] == "line":
                    line = m[1]
                    if line.startswith("@@stage "):
                        st["stage"] = line.split(" ", 2)[-1]
                        write("> " + st["stage"], "stage")
                    elif line.startswith("@@") or not line.strip():
                        continue
                    else:
                        write(line)
                    continue
                st["proc"] = None
                busy(False)
                stage.set("Ready")
                after, st["after"] = st["after"], None
                if after:
                    after(m[1], m[2])
                elif m[1] and os.path.exists(pending_file()):   # (stopped: the check puts the folder back)
                    check()
        except queue.Empty:
            pass
        if st["t0"]:
            mins, sec = divmod(int(time.time() - st["t0"]), 60)
            stage.set("%s (%d:%02d)" % (st["stage"], mins, sec))
        root.after(200, pump)

    btn["check"].configure(command=check)
    btn["update"].configure(command=update)
    btn["rollback"].configure(command=lambda: messagebox.askyesno(
        "Roll back", "Put the game folder back as it was before its last update?", parent=root) and run(["--rollback"]))
    btn["stop"].configure(command=stop)
    btn["logs"].configure(command=logs)
    root.bind("<Control-f>", lambda e: (sbox.focus_set(), sbox.select_range(0, "end")))
    busy(False)
    pump()
    render()
    if game.get():
        root.after(300, check)
    root.mainloop()
    return 0


def add_commands(sub):
    """`dev.py update`"""
    p = sub.add_parser("update", help="keep a game folder you play on main's newest commit, pick its PRs, add open ones",
                       description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--game", help="the game folder (remembered)")
    p.add_argument("--remote", help="the remote to follow (remembered; origin)")
    p.add_argument("--branch", help="its branch (remembered; main)")
    what = p.add_mutually_exclusive_group()
    what.add_argument("--check", action="store_true", help="list what was merged since the folder's build; build nothing")
    what.add_argument("--pick", metavar="PRS", help="the folder's commit with only these merged pull requests (1874,1880)")
    what.add_argument("--rollback", action="store_true", help="put the folder back as it was before its last update")
    what.add_argument("--logs", action="store_true", help="the folder's logs, crash records and settings in one zip")
    what.add_argument("--gui", action="store_true", help="all of it in a window")
    what.add_argument("--list-json", action="store_true", help=argparse.SUPPRESS)   # (the window's list)
    what.add_argument("--build-here", action="store_true", help=argparse.SUPPRESS)   # (run_build's step in the newer tree)
    p.add_argument("--with", dest="with_", metavar="PRS",
                   help="open pull requests merged on top of the build (1913,1925); with --pick too")
    p.add_argument("--force", action="store_true", help="build even if the folder is on the newest commit")
    p.add_argument("--open", action="store_true", help="with --logs: open the folder the zip is in")
    p.add_argument("--commit", help=argparse.SUPPRESS)
    p.set_defaults(func=cmd_update)
