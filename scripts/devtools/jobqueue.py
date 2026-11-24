"""The PC's queue of builds, game runs and measurements (`dev.py queue`). Several agents and people share one PC, its GPU,
CPUs and memory, and a timed run is only worth its numbers when nothing else runs beside it (docs/dev/developing.md,
*Share a PC: the job queue*). Python's standard library only.

    dev.py queue run NAME [--mode MODE] -- CMD...   CMD when the queue lets it; exits with CMD's status
    dev.py queue status                             who plays, what runs, what waits and why, memory and disk
    dev.py queue history                            the last 20 jobs: waited, ran, status, a timed run's noise
    dev.py queue cancel ID|NAME                     stop a job and every process it started (Windows: by its job object)

MODE (MODES below):
    build       a build or anything heavy on CPU and memory: LONGSHOT_QUEUE_BUILD_SLOTS (2) at once, never beside a timed run
    light       small game-free work (a copy, one check): LONGSHOT_QUEUE_LIGHT_SLOTS (4) at once, never beside a timed run.
                The test suites, compile-and-run checks and renderer builds are builds
    shared      the game, for anything that is not a number: LONGSHOT_QUEUE_GPU_SHARED (3) at once
    gpu         the game alone on the GPU, not a measurement (the default)
    timing      a measurement: alone on the GPU, on its own CPUs, no build beside it, no swap, not while you play
    timing-cpu  a measurement without the GPU (a build's time): as timing, but shared and gpu runs may go beside it
    profile     a profile recorded in the game: as timing

Order: high before normal before low priority (a ticket moves up a level per 30 min of waiting; tickets of other versions
of the queue may carry one), first come first served within a level, and no job starts ahead of a waiting one it would
hold up. Timed runs of one group (the name's first word) go one at a time. Inside a job (LONGSHOT_JOB set) `run` runs CMD
directly.

A job gets LONGSHOT_JOB, LONGSHOT_JOB_NAME and LONGSHOT_JOB_MODE, never an OOT_, RX_ or A64_ variable (make_game.py's
lift key hashes those: a variable naming the job would give every build its own translation); TMPDIR moved off a RAM file
system; for the GPU modes its own NVIDIA shader cache folder (LONGSHOT_QUEUE_NV_CACHE); on Linux its own systemd scope (the
OOM killer stops that job, not your terminal) and CPU affinity while a timed run measures. Every job also waits for memory
(LONGSHOT_QUEUE_MIN_FREE_GB, _MAX_PRESSURE, _MAX_SWAP_GB), and a timed run for builds outside the queue. While a timed run
measures, the queue samples every 2 s what its processes stalled on (Noise: CPU, memory and disk, its job's cgroup's PSI),
the swapping, the other CPUs' use, builds outside the queue, jobs beside it); `--noise FILE` writes the samples as JSON lines, and the
history keeps their summary and says to run it again when it was noisy (NOISY).

Settings (DEFAULTS): environment variables, or the same names in job_queue.json in the development folder's config/
(`dev.py paths`); the environment wins.

The on-disk protocol is shared with every version of this queue on the PC: the tickets (<queue>/<runner pid>.json,
fields as `me` in run()), the lock they are written under (<queue>/.lock), the history (<state>/history.jsonl) and
job_queue.json. Change their format or places only for all of them at once (`dev.py migrate` moves the history and
job_queue.json into the development folder and leaves a link at their old places, where older versions look).
"""
import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time

from . import common, procs

PRIO = {"high": 0, "normal": 1, "low": 2}
AGING = 1800   # (seconds of waiting that move a ticket up one level)
POLL = 1.0
# (mode: (GPU use, CPU use). GPU: exclusive / shared / None. CPU: quiet (a timed run's own CPUs, nothing heavy beside it) /
# heavy (a build slot) / light (a light slot) / None (counted in no slot))
MODES = {
    "build": (None, "heavy"),
    "light": (None, "light"),
    "shared": ("shared", None),
    "gpu": ("exclusive", None),
    "timing": ("exclusive", "quiet"),
    "timing-cpu": (None, "quiet"),
    "profile": ("exclusive", "quiet"),
}
MEASURING = ("timing", "timing-cpu", "profile")
# (a timed run is noisy, and to be run again, past any of these over what it measured: PSI cpu "some" and memory and io
# "full" avg10 at their worst (%), the run's own (its job's cgroup: what its processes waited for) where it has one, else
# the PC's; the other CPUs' mean use (%). The swap rate is kept but judges nothing: a game holding 18 GB it can't swap
# (MemorySwapMax=0) pushes the rest of a 32 GB PC into swap at 100 MB/s while its own stalls stay at 0.05 %)
NOISY = {"cpu_pressure_max": 20.0, "memory_stall_max": 1.0, "io_stall_max": 10.0, "other_cpus_busy": 50.0}
# (what an LONGSHOT_JOB variable must never be: the lift key hashes these, make_game.py lift_key)
LIFT_KEY_PREFIXES = ("A64_", "OOT_", "RX_")
DEFAULTS = {
    "LONGSHOT_QUEUE_GPU_SHARED": "3",
    "LONGSHOT_QUEUE_BUILD_SLOTS": "2",
    # (a third build only while LONGSHOT_QUEUE_ROOMY_GB is free and memory pressure is low: three builds that each prepared
    # shaders once took swap to 37 GB)
    "LONGSHOT_QUEUE_BUILD_SLOTS_MAX": "2",
    "LONGSHOT_QUEUE_ROOMY_GB": "14",
    "LONGSHOT_QUEUE_LIGHT_SLOTS": "4",
    "LONGSHOT_QUEUE_MIN_FREE_GB": "6",
    "LONGSHOT_QUEUE_MAX_PRESSURE": "10",
    "LONGSHOT_QUEUE_MAX_SWAP_GB": "32",
    "LONGSHOT_QUEUE_MIN_DISK_GB": "40",
    "LONGSHOT_QUEUE_QUIET": "1",
    "LONGSHOT_QUEUE_SCOPE": "1",
    "LONGSHOT_QUEUE_NO_SWAP": "1",
    "LONGSHOT_QUEUE_GAMES": "longshot,longshot.exe,hookshot,lichii",
    "LONGSHOT_QUEUE_HEAVY": "make_game.py,nx2/build.py",
    "LONGSHOT_QUEUE_TIMING_CPUS": "",
    "LONGSHOT_QUEUE_OTHER_CPUS": "",
    "LONGSHOT_QUEUE_DIR": "",
    "LONGSHOT_QUEUE_STATE": "",
    "LONGSHOT_QUEUE_NV_CACHE": "",
}


# ---- settings ----

def settings(environ=None):
    environ = os.environ if environ is None else environ
    s = dict(DEFAULTS)
    s.update({k: str(v) for k, v in common.read_config("job_queue.json").items() if k in DEFAULTS})
    s.update({k: environ[k] for k in DEFAULTS if environ.get(k, "") != ""})
    return s


def number(s, name):
    try:
        return float(s[name])
    except (KeyError, ValueError):
        return float(DEFAULTS[name])


def state_dir(s):
    return s["LONGSHOT_QUEUE_STATE"] or common.dev_path("queue")


def queue_dir(s):
    """The tickets: $XDG_RUNTIME_DIR/longshot-queue; without one (Windows) tickets/ in the queue's old state folder, which
    `dev.py migrate` leaves in place, so that every version of the queue on the PC keeps seeing every ticket"""
    if s["LONGSHOT_QUEUE_DIR"]:
        return s["LONGSHOT_QUEUE_DIR"]
    run = None if common.WINDOWS else os.environ.get("XDG_RUNTIME_DIR")
    if run:
        return os.path.join(run, "longshot-queue")
    return os.path.join(s["LONGSHOT_QUEUE_STATE"] or common.old_places()["queue"], "tickets")


def cpu_list(text):
    out = set()
    for part in (text or "").split(","):
        part = part.strip()
        if not part:
            continue
        a, _, b = part.partition("-")
        out.update(range(int(a), int(b or a) + 1))
    return out


def cpu_text(cpus):
    cpus, out, i = sorted(cpus), [], 0
    while i < len(cpus):
        j = i
        while j + 1 < len(cpus) and cpus[j + 1] == cpus[j] + 1:
            j += 1
        out.append(str(cpus[i]) if i == j else "%d-%d" % (cpus[i], cpus[j]))
        i = j + 1
    return ",".join(out)


def l3_domains(sysfs="/sys/devices/system/cpu"):
    """The CPUs grouped by the L3 cache they share (a Ryzen's core complexes), first CPU first"""
    groups = set()
    try:
        names = os.listdir(sysfs)
    except OSError:
        return []
    for n in names:
        if not re.match(r"cpu\d+$", n):
            continue
        try:
            with open(os.path.join(sysfs, n, "cache", "index3", "shared_cpu_list")) as f:
                groups.add(frozenset(cpu_list(f.read().strip())))
        except (OSError, ValueError):
            continue
    return sorted(groups, key=min)


def cpu_sets(s, domains=None):
    """(the timed run's CPUs, everyone else's while it runs), or (None, None): no pinning"""
    usable = set(range(os.cpu_count() or 1))
    if hasattr(os, "sched_getaffinity"):
        usable = set(os.sched_getaffinity(0)) | usable
    timing = cpu_list(s["LONGSHOT_QUEUE_TIMING_CPUS"]) & usable
    other = cpu_list(s["LONGSHOT_QUEUE_OTHER_CPUS"]) & usable
    if not timing:
        domains = l3_domains() if domains is None else domains
        if len(domains) < 2:
            return None, None
        timing = set(domains[0]) & usable
    if not other:
        other = usable - timing
    if not timing or not other:
        return None, None
    return timing, other


# ---- the processes ----

def alive(t):
    """True, False, or None: can't tell from here (another pid namespace: a sandboxed caller)"""
    if t.get("ns") and t["ns"] != procs.pidns():
        return None
    for pid, st in ((t.get("pid"), t.get("start")), (t.get("child"), t.get("child_start"))):
        if pid:
            now = procs.proc_start(pid)
            if now is not None and (st is None or now == st):
                return True
    return False


def game_procs(s):
    """The games running: [{pid, name, job}], job the LONGSHOT_JOB it runs under, "" for nobody's
    (yours), None when its environment can't be read"""
    names = {n.strip().lower() for n in s["LONGSHOT_QUEUE_GAMES"].split(",") if n.strip()}
    out = []
    for pid, exe in sorted(procs.exes().items()):
        if common.WINDOWS and exe.lower() not in names:   # (Windows: the program's name; reading every process's memory costs)
            continue
        argv = procs.argv_of(pid)
        a0 = os.path.basename(argv[0].replace("\\", "/")) if argv else ""
        comm = procs.comm_of(pid)
        hit = next((n for n in (exe, a0, comm) if n.lower() in names), None)
        if not hit:
            continue
        env = procs.env_of(pid)
        job = None if env is None else (env.get("LONGSHOT_JOB") or "")
        if any(a == "--prepare-shaders" for a in argv):   # (a build's shader preparation: never someone playing)
            job = job or "prepare"
        out.append({"pid": pid, "name": hit, "job": job})
    return out


def user_playing(s):
    fake = os.environ.get("LONGSHOT_QUEUE_PLAYING_FILE")   # (tests: you "play" while this file is there)
    if fake:
        return os.path.exists(fake)
    if not any(g["job"] == "" for g in game_procs(s)):
        return False
    time.sleep(1)   # (looked at twice: a job's game between its exec and its first readable environ looks like yours)
    return any(g["job"] == "" for g in game_procs(s))


def heavy_procs(s, skip=()):
    """Builds running outside the queue: a python running one of LONGSHOT_QUEUE_HEAVY's scripts, not in a job's tree"""
    pats = [p.strip() for p in s["LONGSHOT_QUEUE_HEAVY"].split(",") if p.strip()]
    skip = set(skip)
    out = []
    for pid, exe in sorted(procs.exes().items()):
        if pid in skip or common.WINDOWS and "python" not in exe.lower():   # (Windows: the program's name first, as game_procs)
            continue
        argv = procs.argv_of(pid)
        if argv and "python" in os.path.basename(argv[0].replace("\\", "/")).lower() and \
                any(a.replace("\\", "/").endswith(p) for a in argv[1:4] for p in pats):
            env = procs.env_of(pid) or {}
            if not env.get("LONGSHOT_JOB"):
                out.append(pid)
    return out


def job_procs(t):
    """{pid: start} of a job's processes but its runner (Linux): its command's tree, and every process that carries its
    LONGSHOT_JOB and LONGSHOT_JOB_NAME wherever it went. A game whose parent ended (longshot_bridge.py's supervisor, which a
    SIGTERM ends before it stops its game) or that left in a session of its own is in no tree any more, but keeps the
    environment it started with. A process of another job carries another runner's pid; this process is never in it"""
    found = {}
    child, me = t.get("child"), os.getpid()
    if child and procs.running(child, t.get("child_start", procs.proc_start(child))):
        for p in procs.tree(child):
            found[p] = procs.proc_start(p)
    job, name = str(t["pid"]), t.get("name")
    for pid in procs.exes():
        if pid in found or pid in (me, t["pid"]):
            continue
        env = procs.env_of(pid)
        if env and env.get("LONGSHOT_JOB") == job and env.get("LONGSHOT_JOB_NAME", name) == name:
            found[pid] = procs.proc_start(pid)
    return {p: st for p, st in found.items() if st is not None and p not in (me, t["pid"])}


def _signal(found, sig):
    for pid, st in found.items():
        if procs.running(pid, st):   # (the process that was found: never a pid used again)
            try:
                os.kill(pid, sig)
            except OSError:
                pass


def _scope(t, sig):
    """sig to every process left in the job's systemd scope (run() puts the command in one): one that dropped its
    LONGSHOT_JOB too"""
    if common.WINDOWS or not shutil.which("systemctl"):
        return
    try:
        subprocess.run(["systemctl", "--user", "kill", "--signal=%s" % sig,
                        "longshot-job-%s-%d.scope" % (safe(t.get("name") or ""), t["pid"])],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        pass


def end_job(t, found=None, grace=10.0, scope=True):
    """Linux: SIGTERM to every process of job t but its runner (job_procs, and found: those seen before the runner was
    stopped; with scope, its systemd scope's too), SIGKILL to what still runs after grace seconds. Returns how many it
    found"""
    found = dict(found or {})
    found.update(job_procs(t))
    _signal(found, signal.SIGTERM)
    if scope:
        _scope(t, "SIGTERM")
    deadline = time.monotonic() + grace
    while time.monotonic() < deadline:
        more = {p: st for p, st in job_procs(t).items() if found.get(p) != st}   # (started since: a crash watcher)
        _signal(more, signal.SIGTERM)
        found.update(more)
        if not any(procs.running(p, st) for p, st in found.items()):
            break
        time.sleep(0.2)
    _signal(found, signal.SIGKILL)
    if scope:
        _scope(t, "SIGKILL")
    return len(found)


# ---- memory and disk ----

def meminfo():
    try:
        with open("/proc/meminfo") as f:
            return {l.split(":")[0]: int(l.split()[1]) * 1024 for l in f if len(l.split()) >= 2}
    except (OSError, ValueError):
        return {}


def pressure(kind="memory", line="full", cgroup=None):
    """PSI avg10 of kind (memory, cpu, io) and line (some, full): the whole PC's, or a cgroup's (a folder of /sys/fs/cgroup)"""
    try:
        with open(os.path.join(cgroup, "%s.pressure" % kind) if cgroup else "/proc/pressure/%s" % kind) as f:
            for l in f:
                if l.startswith(line + " "):
                    return float(dict(kv.split("=") for kv in l.split()[1:])["avg10"])
    except (OSError, ValueError, KeyError):
        pass
    return None


def memory_short(s):
    """Why a job should wait for memory, or None"""
    info = meminfo()
    if not info:
        return None
    free = info.get("MemAvailable", 0) / common.GB
    swap = (info.get("SwapTotal", 0) - info.get("SwapFree", 0)) / common.GB
    if free < number(s, "LONGSHOT_QUEUE_MIN_FREE_GB"):
        return "memory: %.1f GB free (waits for %.0f)" % (free, number(s, "LONGSHOT_QUEUE_MIN_FREE_GB"))
    p = pressure()
    if p is not None and p > number(s, "LONGSHOT_QUEUE_MAX_PRESSURE"):
        return "memory: thrashing, %.0f%% of the last 10 s stalled on it (waits under %.0f%%)" % (
            p, number(s, "LONGSHOT_QUEUE_MAX_PRESSURE"))
    # (swap alone is no test of a shortage: what idle programs left there never drains, so it is only a ceiling)
    if swap > number(s, "LONGSHOT_QUEUE_MAX_SWAP_GB"):
        return "memory: %.1f GB of swap in use (waits under %.0f)" % (swap, number(s, "LONGSHOT_QUEUE_MAX_SWAP_GB"))
    return None


def memory_roomy(s):
    info = meminfo()
    p = pressure()
    return bool(info) and info.get("MemAvailable", 0) / common.GB >= number(s, "LONGSHOT_QUEUE_ROOMY_GB") and (p is None or p < 3)


# ---- the queue ----

def write_ticket(qdir, t):
    os.makedirs(qdir, exist_ok=True)
    tmp = os.path.join(qdir, ".%d.tmp" % t["pid"])
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(t, f)
    os.replace(tmp, os.path.join(qdir, "%d.json" % t["pid"]))


def read_tickets(qdir, clean=True):
    out = []
    try:
        names = os.listdir(qdir)
    except OSError:
        return out
    for n in names:
        if not n.endswith(".json"):
            continue
        p = os.path.join(qdir, n)
        try:
            with open(p, encoding="utf-8") as f:
                t = json.load(f)
        except (OSError, ValueError):
            continue
        if alive(t) is False:   # (left by a killed runner)
            if clean:
                try:
                    os.remove(p)
                except OSError:
                    pass
            continue
        out.append(t)
    return out


def rank(t, now):
    p = PRIO.get(t.get("prio"), 1)
    if p == 0:
        return (-1, t["t"], t["pid"])
    return (max(0, p - int((now - t["t"]) // AGING)), t["t"], t["pid"])


def group(t):
    """Whose timed runs these are: the ticket's group (another version of the queue may set one), else the name's first
    word (area-<what> keeps two)"""
    if t.get("group"):
        return t["group"]
    parts = [p for p in re.split(r"[-:_ ./]", t["name"].lower()) if p]
    if not parts:
        return t["name"]
    return "-".join(parts[:2]) if parts[0] == "area" and len(parts) > 1 else parts[0]


def deferred(t, ts):
    """Another timed run of t's group runs, or waits since before t: t keeps its place but lets other groups' go first"""
    if t["mode"] not in MEASURING:
        return None
    for o in ts:
        if o["pid"] == t["pid"] or o["mode"] not in MEASURING or group(o) != group(t):
            continue
        if o["state"] == "active" or (o["state"] == "waiting" and o.get("ready", True) and (o["t"], o["pid"]) < (t["t"], t["pid"])):
            return o
    return None


class Use:
    """What the running jobs (and the waiting ones ahead, which keep their claim) hold"""

    def __init__(self):
        self.gpu_excl = []
        self.gpu_shared = []
        self.quiet = []
        self.heavy = []
        self.light = []

    def add(self, t):
        if t.get("gpu") == "exclusive":
            self.gpu_excl.append(t)
        elif t.get("gpu") == "shared":
            self.gpu_shared.append(t)
        {"quiet": self.quiet, "heavy": self.heavy, "light": self.light}.get(t.get("cpu"), []).append(t)


def conflict(t, use, slots, quiet=True):
    """Why t can't start beside what use holds, or None"""
    names = lambda ts: ", ".join("%s (%s)" % (o["name"], o["mode"]) for o in ts)
    if t.get("gpu") == "exclusive" and (use.gpu_excl or use.gpu_shared):
        return "the GPU: " + names(use.gpu_excl + use.gpu_shared)
    if t.get("gpu") == "shared":
        if use.gpu_excl:
            return "the GPU: " + names(use.gpu_excl)
        if len(use.gpu_shared) >= slots["shared"]:
            return "the %d shared GPU slots: %s" % (slots["shared"], names(use.gpu_shared))
    cpu = t.get("cpu")
    if cpu == "quiet":
        if use.quiet:
            return "a timed run: " + names(use.quiet)
        if quiet and (use.heavy or use.light):
            return "builds and checks (a timed run goes alone): " + names(use.heavy + use.light)
    elif cpu == "heavy":
        if quiet and use.quiet:
            return "a timed run (no build beside it): " + names(use.quiet)
        if len(use.heavy) >= slots["build"]:
            return "the %d build slots: %s" % (slots["build"], names(use.heavy))
    elif cpu == "light":
        if quiet and use.quiet:   # (a light job's memory and disk reach a timed run's frames through its own CPUs)
            return "a timed run (no check beside it): " + names(use.quiet)
        if len(use.light) >= slots["light"]:
            return "the %d light slots: %s" % (slots["light"], names(use.light))
    return None


def blocker(me, ts, now, slots, quiet=True):
    """Why the ticket `me` can't start now, or None. Pure: the tickets, the time and the slots only. The running jobs hold what
    they use; each waiting ticket ahead of me (by rank) that is ready keeps a claim on what it needs, so a job behind it that
    would hold it up waits too (no starvation: a stream of shared runs never keeps a gpu run waiting for ever)"""
    s = deferred(me, ts)
    if s:
        return "one timed run per group at a time: %s first" % s["name"]
    use = Use()
    for t in ts:
        if t["state"] == "active" and t["pid"] != me["pid"]:
            use.add(t)
    waiting = sorted((t for t in ts if t["state"] == "waiting" and (t.get("ready", True) or t["pid"] == me["pid"])),
                     key=lambda t: rank(t, now))
    for t in waiting:
        if t["pid"] == me["pid"]:
            why = conflict(me, use, slots, quiet)
            return None if why is None else "waits for " + why
        if not deferred(t, ts):
            t = dict(t, name=t["name"] + ", queued ahead")
            use.add(t)
    return None


def slots_now(s, roomy=None):
    build = int(number(s, "LONGSHOT_QUEUE_BUILD_SLOTS"))
    top = int(number(s, "LONGSHOT_QUEUE_BUILD_SLOTS_MAX"))
    if top > build and (memory_roomy(s) if roomy is None else roomy):
        build = top
    return {"shared": int(number(s, "LONGSHOT_QUEUE_GPU_SHARED")), "build": build,
            "light": int(number(s, "LONGSHOT_QUEUE_LIGHT_SLOTS"))}


# ---- a job ----

def safe(name):
    return "".join(c if c.isalnum() or c in "-_" else "-" for c in name)[:48] or "job"


def job_env(environ, name, mode, s, me_pid, desc=None):
    """The environment a job runs with (the module's notes): never a new OOT_, RX_ or A64_ variable. A game run (a mode
    with a GPU share) is named on screen: LONGSHOT_BUILD_NAME, under the version in the menu and on the start-up screen,
    is desc, else what the run already set, else the job's name"""
    env = without_queue_vars(environ)
    env.update(LONGSHOT_JOB=str(me_pid), LONGSHOT_JOB_NAME=name, LONGSHOT_JOB_MODE=mode)
    if MODES[mode][0]:
        if desc:
            env["LONGSHOT_BUILD_NAME"] = desc
        else:
            env.setdefault("LONGSHOT_BUILD_NAME", name)
    state = state_dir(s)
    tmp = env.get("TMPDIR") or "/tmp"
    if not common.WINDOWS and common.on_ram(tmp):
        root = os.path.join(state, "tmp")
        try:
            os.makedirs(root, exist_ok=True)
            for old in os.listdir(root):
                p = os.path.join(root, old)
                if os.path.isdir(p) and time.time() - os.path.getmtime(p) > 86400:
                    shutil.rmtree(p, ignore_errors=True)
            d = os.path.join(root, "%s-%d" % (safe(name), me_pid))
            os.makedirs(d, exist_ok=True)
            env.update(TMPDIR=d, TMP=d, TEMP=d)
        except OSError:
            pass
    if MODES[mode][0] and not env.get("__GL_SHADER_DISK_CACHE_PATH"):
        nv = s["LONGSHOT_QUEUE_NV_CACHE"] or os.path.join(state, "nvcache")
        try:
            os.makedirs(nv, exist_ok=True)   # (NVIDIA keeps no cache at all in a folder that isn't there)
            env["__GL_SHADER_DISK_CACHE_PATH"] = nv
            env.setdefault("__GL_SHADER_DISK_CACHE_SIZE", str(10 << 30))
            env.setdefault("__GL_SHADER_DISK_CACHE_SKIP_CLEANUP", "1")
        except OSError:
            pass
    return env


def scope_ok():
    if common.WINDOWS or not shutil.which("systemd-run"):
        return False
    try:
        return subprocess.run(["systemd-run", "--user", "--scope", "--quiet", "--collect", "--", "true"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def history_append(s, rec):
    p = os.path.join(state_dir(s), "history.jsonl")
    try:
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "a", encoding="utf-8") as f:
            f.write(json.dumps(rec) + "\n")
    except OSError:
        pass


def swap_pages():
    """Pages swapped in and out since boot (/proc/vmstat), or None"""
    try:
        with open("/proc/vmstat") as f:
            v = dict(l.split() for l in f if l.startswith(("pswpin ", "pswpout ")))
        return int(v["pswpin"]) + int(v["pswpout"])
    except (OSError, ValueError, KeyError):
        return None


def cgroup_of(pid):
    """The cgroup v2 folder of process pid (its job's systemd scope), or None"""
    try:
        with open("/proc/%d/cgroup" % pid) as f:
            for l in f:
                if l.startswith("0::"):
                    d = "/sys/fs/cgroup" + l[3:].strip()
                    return d if os.path.isfile(os.path.join(d, "memory.pressure")) else None
    except (OSError, ValueError):
        pass
    return None


def cpu_times(cpus):
    """(busy, total) jiffies of the CPUs cpus (/proc/stat), or None"""
    try:
        busy = total = 0
        with open("/proc/stat") as f:
            for l in f:
                head, _, rest = l.partition(" ")
                if head.startswith("cpu") and head[3:].isdigit() and int(head[3:]) in cpus:
                    v = [int(x) for x in rest.split()]
                    idle = v[3] + (v[4] if len(v) > 4 else 0)   # (idle and iowait)
                    total += sum(v[:8])
                    busy += sum(v[:8]) - idle
        return (busy, total) if total else None
    except (OSError, ValueError):
        return None


class Noise:
    """For a timed run: how busy the rest of the PC was while it measured, a sample every 2 s (PSI cpu "some", memory and
    io "full" avg10, the load, the swap counter, the other CPUs' use, builds outside the queue moved off its CPUs, jobs of
    the queue beside it). The history keeps their summary (noise_summary) so a noisy measurement shows; path: the samples
    as JSON lines too, for a caller that judges only its own measured stretch (dev.py ab). cgroup: the job's own, whose
    stalls are sampled instead of the PC's"""

    def __init__(self, other_cpus=None, path=None, cgroup=None):
        self.other_cpus, self.path, self.cgroup = other_cpus, path, cgroup
        self.samples = []
        self.strays = set()
        self.last = cpu_times(other_cpus) if other_cpus else None

    def sample(self, strays=(), beside=()):
        g = self.cgroup if self.cgroup and os.path.isdir(self.cgroup) else None
        x = {"t": round(time.time(), 2), "cpu": pressure("cpu", "some", g), "memory": pressure("memory", "full", g),
             "io": pressure("io", "full", g), "own": bool(g), "swap_pages": swap_pages(), "strays": len(strays),
             "beside": sorted(beside)}
        try:
            x["load"] = os.getloadavg()[0]
        except (OSError, AttributeError):
            pass
        if self.other_cpus:
            now = cpu_times(self.other_cpus)
            if now and self.last and now[1] > self.last[1]:
                x["other_cpus"] = round(100.0 * (now[0] - self.last[0]) / (now[1] - self.last[1]), 1)
            self.last = now
        self.strays.update(strays)
        self.samples.append(x)
        if self.path:
            try:
                with open(self.path, "a", encoding="utf-8") as f:
                    f.write(json.dumps(x) + "\n")
            except OSError:
                pass

    def record(self):
        rec = noise_summary(self.samples)
        rec["builds_outside"] = len(self.strays)
        return rec


def noise_summary(samples):
    """Noise's samples (those of a measured stretch, or a whole run) as the history's noise record"""
    top = lambda k: round(max([x.get(k) or 0 for x in samples] or [0]), 1)
    swap = [(x["t"], x["swap_pages"]) for x in samples if x.get("swap_pages") is not None]
    busy = [x["other_cpus"] for x in samples if x.get("other_cpus") is not None]
    rec = {"cpu_pressure_max": top("cpu"), "memory_stall_max": top("memory"), "io_stall_max": top("io"), "load_max": top("load"),
           "swap_mb": round((swap[-1][1] - swap[0][1]) * 4096 / 2 ** 20) if len(swap) > 1 else 0,
           "builds_outside": int(top("strays")), "beside": sorted({n for x in samples for n in x.get("beside", ())})}
    if len(swap) > 1 and swap[-1][0] > swap[0][0]:
        rec["swap_mb_s"] = round(rec["swap_mb"] / (swap[-1][0] - swap[0][0]), 1)
    if busy:
        rec["other_cpus_busy"] = round(sum(busy) / len(busy))
    return rec


def noise_reasons(rec):
    """[text]: why the PC was too busy for a measurement with this noise record; [] when it was quiet"""
    out = []
    if rec.get("builds_outside"):
        out.append("%d builds outside the queue" % rec["builds_outside"])
    if rec.get("beside"):
        out.append("beside: %s" % ", ".join(rec["beside"]))
    for k, what in (("cpu_pressure_max", "CPU stalls %.0f%%"), ("memory_stall_max", "memory stalls %.1f%%"),
                    ("io_stall_max", "disk stalls %.0f%%"), ("other_cpus_busy", "the other CPUs %.0f%% busy")):
        if (rec.get(k) or 0) > NOISY[k]:
            out.append(what % rec[k])
    return out


def say(text):
    common.say("queue", text, err=True)


def inside_job(environ):
    """The job a process runs under, or None"""
    if environ.get("LONGSHOT_JOB"):
        return environ.get("LONGSHOT_JOB_NAME") or environ.get("LONGSHOT_JOB")
    return None


def without_queue_vars(environ):
    """environ less what a queue sets for its job: LONGSHOT_JOB, LONGSHOT_JOB_* (LONGSHOT_JOBS, the compilers at once, is the
    build's and stays)"""
    return {k: v for k, v in environ.items() if k != "LONGSHOT_JOB" and not k.startswith("LONGSHOT_JOB_")}


def run(name, mode, cmd, environ=None, s=None, cwd=None, desc=None, noise_file=None):
    """Queue, wait, run cmd (in cwd, with environ); returns its exit status. Inside a job already (this process's environment
    or environ says so) it runs cmd directly: that job holds what this one needs"""
    environ = dict(os.environ if environ is None else environ)
    s = settings(environ) if s is None else s
    cwd = cwd or os.getcwd()
    inside = inside_job(os.environ) or inside_job(environ)
    if inside:
        say("inside job %s: running %s directly" % (inside, name))
        if desc and MODES[mode][0]:
            environ["LONGSHOT_BUILD_NAME"] = desc
        return subprocess.call(cmd, cwd=cwd, env=environ)
    qdir = queue_dir(s)
    try:
        os.makedirs(qdir, exist_ok=True)
        probe = os.path.join(qdir, ".probe.%d" % os.getpid())
        open(probe, "w").close()
        os.remove(probe)
    except OSError as e:
        sys.exit("dev.py queue: can't write the queue at %s (%s). In a sandbox, run it outside: the queue is the PC's" % (qdir, e))
    gpu, cpu = MODES[mode]
    quiet = s["LONGSHOT_QUEUE_QUIET"] != "0"
    # (prio, idle and group are read by every version of the queue; an older one may set them on its own tickets)
    me = {"name": name, "pid": os.getpid(), "start": procs.proc_start(os.getpid()), "ns": procs.pidns(), "mode": mode, "gpu": gpu,
          "cpu": cpu, "prio": "normal", "t": time.time(), "state": "waiting", "ready": True, "idle": False, "group": "",
          "cmd": " ".join(cmd), "cwd": cwd}
    lock = os.path.join(qdir, ".lock")
    said, last_idle, child, job, scoped = None, 0.0, None, None, False
    noise = Noise(cpu_sets(s)[1], noise_file)
    if cpu == "heavy":
        free = common.disk_free_gb(cwd)
        if free is not None and free < number(s, "LONGSHOT_QUEUE_MIN_DISK_GB"):
            say("%.0f GB free on this drive: clean up old workspaces, game folders and caches (Tooling's "
                "scripts/workspaces.py clean)" % free)
    try:
        with common.Lock(lock):
            write_ticket(qdir, me)
        while True:
            if mode in MEASURING and time.time() - last_idle >= (10 if me["ready"] else 2):
                last_idle = time.time()
                ready = not user_playing(s)
                if ready != me["ready"]:
                    me["ready"] = ready
                    with common.Lock(lock):
                        write_ticket(qdir, me)
                if not ready and said != "playing":
                    say("%s waits: you are playing (it keeps its place)" % name)
                    said = "playing"
            if me["ready"]:
                with common.Lock(lock):
                    ts = read_tickets(qdir)
                    for t in ts:
                        if t["pid"] == me["pid"]:
                            t.update(me)
                    why = blocker(me, ts, time.time(), slots_now(s), quiet)
                    if why is None and cpu == "quiet" and quiet:
                        own = set()
                        for t in ts:
                            if t["state"] == "active" and t.get("child"):
                                own.update(procs.tree(t["child"]))
                        strays = heavy_procs(s, own)
                        if strays:
                            why = "waits for a build outside the queue (pid %d: %s)" % (strays[0], " ".join(procs.argv_of(strays[0])[:3]))
                    if why is None:
                        why = memory_short(s)
                    if why is None:
                        me.update(state="active", since=time.time(), waited=time.time() - me["t"])
                        write_ticket(qdir, me)
                        break
                if why != said:
                    say("%s %s" % (name, why))
                    said = why
            time.sleep(POLL)
        if said:
            say("%s starts (%s, waited %.0f s)" % (name, mode, me["waited"]))
        env = job_env(environ, name, mode, s, me["pid"], desc)
        timing_cpus, other_cpus = cpu_sets(s)
        pre = None
        if cpu == "quiet" and timing_cpus and hasattr(os, "sched_setaffinity"):
            pre = lambda: os.sched_setaffinity(0, timing_cpus)
        run_cmd = list(cmd)
        if s["LONGSHOT_QUEUE_SCOPE"] != "0" and scope_ok():
            props = ["-p", "MemorySwapMax=0"] if cpu == "quiet" and s["LONGSHOT_QUEUE_NO_SWAP"] != "0" else []
            run_cmd = ["systemd-run", "--user", "--scope", "--quiet", "--collect",
                       "--unit=longshot-job-%s-%d" % (safe(name), me["pid"])] + props + ["--"] + run_cmd
            scoped = True
        stop = threading.Event()
        if common.WINDOWS:
            child = subprocess.Popen(run_cmd, cwd=cwd, env=env, creationflags=procs.CREATE_SUSPENDED)
            job = procs.win_job_start(child)
        else:
            child = subprocess.Popen(run_cmd, cwd=cwd, env=env, preexec_fn=pre)
        me.update(child=child.pid, child_start=procs.proc_start(child.pid))
        if cpu == "quiet" and not common.WINDOWS:
            noise.cgroup = cgroup_of(child.pid)
        with common.Lock(lock):
            write_ticket(qdir, me)
        if (timing_cpus or cpu == "quiet") and not common.WINDOWS:
            threading.Thread(target=_keep_apart, args=(s, qdir, me, child.pid, cpu, timing_cpus, other_cpus, noise, stop),
                             daemon=True).start()
        try:
            rc = child.wait()
        finally:
            stop.set()
        end = time.time()
        rec = {"name": name, "mode": mode, "cmd": me["cmd"][:300], "cwd": me["cwd"], "queued": me["t"], "started": me["since"],
               "ended": end, "waited": round(me["waited"], 1), "ran": round(end - me["since"], 1), "status": rc}
        if cpu == "quiet":
            rec["noise"] = noise.record()
            rec["noisy"] = noise_reasons(rec["noise"])
            if rec["noisy"]:
                say("%s: the PC was busy while it measured (%s): run it again" % (name, "; ".join(rec["noisy"])))
        history_append(s, rec)
        return rc
    finally:
        ended = child is None or child.poll() is not None
        if not ended:
            if common.WINDOWS:   # (the rest of its tree: the job object, at win_job_release)
                child.terminate()
            else:   # (everything the job started: a game whose supervisor a SIGTERM ends would run on)
                end_job(me, scope=scoped)
            try:
                child.wait(10)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        if job is not None:
            procs.win_job_release(job, keep_running=ended)
        try:
            os.remove(os.path.join(qdir, "%d.json" % me["pid"]))
        except OSError:
            pass


def _keep_apart(s, qdir, me, child, cpu, timing_cpus, other_cpus, noise, stop):
    """Every 2 s while the job runs. A timed run: builds started outside the queue move off its CPUs, and the noise is
    sampled. Any other job: onto the other CPUs while a timed run measures, all of them back after it"""
    pinned = False
    while not stop.wait(2):
        if cpu == "quiet":
            own, beside = set(), set()
            for t in read_tickets(qdir, clean=False):
                if t.get("child"):
                    own.update(procs.tree(t["child"]))
                # (what may go beside: GPU runs beside a timed run without the GPU (timing-cpu), nothing else)
                if t["state"] == "active" and t["pid"] != me["pid"] and (t.get("cpu") or me.get("gpu") and t.get("gpu")):
                    beside.add("%s (%s)" % (t["name"], t["mode"]))
            strays = heavy_procs(s, own)
            for p in strays if other_cpus else ():
                procs.pin(p, other_cpus)
            noise.sample(strays, beside)
            continue
        timing = any(t.get("cpu") == "quiet" and t["state"] == "active" and t["pid"] != me["pid"]
                     for t in read_tickets(qdir, clean=False))
        if timing and not pinned:
            procs.pin(child, other_cpus)
            pinned = True
        elif not timing and pinned:
            procs.pin(child, timing_cpus | other_cpus)
            pinned = False


# ---- what runs ----

def status(s):
    now = time.time()
    qdir = queue_dir(s)
    can_clean = os.access(qdir, os.W_OK)
    ts = read_tickets(qdir, clean=can_clean)
    slots = slots_now(s)
    quiet = s["LONGSHOT_QUEUE_QUIET"] != "0"
    games = game_procs(s)
    info = meminfo()
    timing_cpus, other_cpus = cpu_sets(s)
    running = sorted((t for t in ts if t["state"] == "active"), key=lambda t: t.get("since", t["t"]))
    waiting = sorted((t for t in ts if t["state"] == "waiting"), key=lambda t: rank(t, now))
    rows = []
    for t in waiting:
        why = "you are playing (keeps its place)" if not t.get("ready", True) else blocker(t, ts, now, slots, quiet)
        rows.append(dict(t, why=why or "starting (or waiting for memory)"))
    heavy = heavy_procs(s, [p for t in running if t.get("child") for p in procs.tree(t["child"])])
    yours = [g for g in games if g["job"] == ""]
    print("playing: %s" % (", ".join("%s (pid %d)" % (g["name"], g["pid"]) for g in yours) if yours else "nobody"))
    for g in games:
        if g["job"]:
            print("  game: %s pid %d under job %s" % (g["name"], g["pid"], g["job"]))
    if info:
        swap = (info.get("SwapTotal", 0) - info.get("SwapFree", 0)) / common.GB
        p = pressure()
        print("memory: %.1f GB available, %.1f GB swap in use%s" % (info.get("MemAvailable", 0) / common.GB, swap,
                                                                   "" if p is None else ", pressure %.0f%%" % p))
    free = common.disk_free_gb(os.getcwd())
    if free is not None:
        print("disk here: %.0f GB free" % free)
    print("CPUs: %s" % ("timed runs %s, the rest %s while one measures" % (cpu_text(timing_cpus), cpu_text(other_cpus))
                        if timing_cpus else "no pinning (one L3 domain; LONGSHOT_QUEUE_TIMING_CPUS sets it)"))
    print("slots: %d build, %d light, %d shared GPU" % (slots["build"], slots["light"], slots["shared"]))
    print("running:" if running else "running: nothing")
    for t in running:
        print("  %-24s %-10s for %-7s waited %-7s %s" % (t["name"], t["mode"], common.ago(now - t.get("since", t["t"])),
                                                          common.ago(t.get("waited", 0)), t.get("cmd", "")[:70]))
    for p in heavy:
        print("  (a build outside the queue: pid %d %s)" % (p, " ".join(procs.argv_of(p)[:3])[:80]))
    if rows:
        print("queue:")
    for i, t in enumerate(rows, 1):
        print("  %d. %-22s %-10s %-6s waiting %-7s %s" % (i, t["name"], t["mode"], t.get("prio", "normal"), common.ago(now - t["t"]), t["why"]))
    return 0


def history(s, n=20):
    p = os.path.join(state_dir(s), "history.jsonl")
    try:
        with open(p, encoding="utf-8") as f:
            recs = [json.loads(l) for l in f if l.strip()][-n:]
    except (OSError, ValueError):
        recs = []
    for r in recs:
        noise = r.get("noise")
        print("%s  %-24s %-10s waited %-7s ran %-7s status %-4s%s%s" % (
            time.strftime("%m-%d %H:%M", time.localtime(r["started"])), r["name"], r["mode"], common.ago(r["waited"]), common.ago(r["ran"]),
            r["status"], "  noise %s" % json.dumps(noise) if noise else "",
            "  NOISY, run it again: %s" % "; ".join(r["noisy"]) if r.get("noisy") else ""))
    return 0


def cancel(s, what):
    hits = [t for t in read_tickets(queue_dir(s)) if str(t["pid"]) == what or t["name"] == what]
    if not hits:
        sys.exit("dev.py queue: no job %s in the queue" % what)
    for t in hits:
        if t.get("ns") and t["ns"] != procs.pidns():   # (its pids name other processes here)
            say("could not stop %s: it runs in another pid namespace (a sandbox's): cancel it from there or outside" % t["name"])
            continue
        try:
            if common.WINDOWS and procs.win_kill_tree(t["pid"]):   # (the runner and its command's whole tree, for a runner without a job object too)
                say("stopped %s (pid %d) and every process it started" % (t["name"], t["pid"]))
                continue
            # (Linux: the job's processes are found before its runner stops, then ended here too, whatever the runner's
            # version: one that ends only its command leaves a game whose supervisor a SIGTERM ends running)
            found = {} if common.WINDOWS else job_procs(t)
            os.kill(t["pid"], signal.SIGTERM)
            if common.WINDOWS:
                say("stopped %s (pid %d)" % (t["name"], t["pid"]))
                continue
            n = end_job(t, found, scope=s["LONGSHOT_QUEUE_SCOPE"] != "0")
            say("stopped %s (pid %d) and %d process%s of its job" % (t["name"], t["pid"], n, "" if n == 1 else "es"))
        except OSError as e:
            say("could not stop %s: %s" % (t["name"], e))
    return 0


def add_commands(sub):
    """`dev.py queue`'s subcommands"""
    p = sub.add_parser("queue", help="the PC's job queue: run a command when it may, and see what runs",
                       description=__doc__,
                       formatter_class=argparse.RawDescriptionHelpFormatter)
    qs = p.add_subparsers(dest="what", required=True)
    r = qs.add_parser("run", help="CMD (after --) when the queue lets it")
    r.add_argument("name")
    r.add_argument("--mode", choices=sorted(MODES), default="gpu")
    r.add_argument("--desc", help="what a game run tests, in a few words: shown under the version in the menu (F1) and on the "
                   "start-up screen (LONGSHOT_BUILD_NAME; default: the job's name)")
    r.add_argument("--noise", metavar="FILE", help="a timed run: append the queue's samples of how busy the PC was, every 2 s, "
                   "to FILE as JSON lines (dev.py ab judges its measured stretch by them)")
    r.set_defaults(func=cmd_run, takes_command=True)
    qs.add_parser("status", help="who plays, what runs, what waits and why").set_defaults(func=lambda a: status(settings()))
    qs.add_parser("history", help="the last 20 jobs").set_defaults(func=lambda a: history(settings()))
    c = qs.add_parser("cancel", help="stop a job")
    c.add_argument("job", help="its id (the runner's pid) or name")
    c.set_defaults(func=lambda a: cancel(settings(), a.job))


def cmd_run(a):
    if not a.command:
        sys.exit("dev.py queue run: needs a command after --")
    if not common.WINDOWS:   # (a kill runs the clean-up: the command stopped, the ticket gone)
        signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
        signal.signal(signal.SIGHUP, lambda *_: sys.exit(129))
    return run(a.name, a.mode, a.command, desc=a.desc, noise_file=a.noise)
