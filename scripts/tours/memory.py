"""Memory from /proc (Linux): a process's and the machine's, read the same way for the pipeline tour's memory guard and
memtour's samples; and the memory guard itself."""
import os
import shutil
import subprocess
import time


def proc_kb(text, keys=None):
    """{key: kB} of a /proc file's "Key:   N kB" lines (keys: only those)"""
    kb = {}
    for line in text.splitlines():
        k, _, v = line.partition(":")
        if (keys is None or k in keys) and v.strip():
            kb[k] = int(v.split()[0])
    return kb


def proc_mb(path, keys):
    """{key: MB} of those lines of a /proc file, or None when it can't be read (the process went away)"""
    try:
        with open(path) as f:
            return {k: v // 1024 for k, v in proc_kb(f.read(), keys).items()}
    except (OSError, ValueError, IndexError):
        return None

# ---- memory ----
# One game holds 4 to 14 GB (the guest memory is shared memory, touched as the game goes, and not given back; the program cache's
# file pages come on top and can be dropped). This PC-wide guard keeps a run from taking the machine down: before a game starts
# there must be MIN_FREE_GB available and at most MAX_SWAP_GB of swap in use (else it waits, WAIT_S at most, then gives up); while
# it runs, the game's own memory (anonymous + shared: what cannot be dropped) and the machine's are read once a second, and
# - past SOFT_MB after a stop: the run ends there and the tour goes on in a fresh game (one game's memory only grows),
# - past HARD_MB, under FLOOR_GB available or past MAX_SWAP_GB + 4 of swap: the game is stopped at once, its list kept, and the
#   tour waits for memory and goes on from the stop it was at.
# On Linux with systemd the game also runs in a scope of its own with MemoryMax (SCOPE_GB) and little swap: should all of that
# fail, the kernel ends the game, not the desktop or the terminal it was started from.
MEM = {"min_free_gb": 8.0, "max_swap_gb": 16.0, "floor_gb": 3.0, "soft_mb": 9000, "hard_mb": 12000, "wait_s": 1800, "scope_gb": 16}


def meminfo(text=None):
    """/proc/meminfo: (GB available, GB of swap in use); (None, None) where there is none"""
    if text is None:
        try:
            with open("/proc/meminfo") as f:
                text = f.read()
        except OSError:
            return None, None
    kb = proc_kb(text)
    if "MemAvailable" not in kb:
        return None, None
    return kb["MemAvailable"] / 1048576, (kb.get("SwapTotal", 0) - kb.get("SwapFree", 0)) / 1048576


def held_mb(status_text):
    """A process's memory that cannot be dropped, MB (/proc/<pid>/status: RssAnon + RssShmem)"""
    kb = proc_kb(status_text, ("RssAnon", "RssShmem"))
    return (kb.get("RssAnon", 0) + kb.get("RssShmem", 0)) // 1024


def start_problem(mem, info):
    """Why a game may not start now (None: it may)"""
    free, swap = info
    if free is None:
        return None
    if free < mem["min_free_gb"]:
        return "%.1f GB available, a game wants %.0f" % (free, mem["min_free_gb"])
    if swap > mem["max_swap_gb"]:
        return "%.1f GB of swap in use, at most %.0f" % (swap, mem["max_swap_gb"])
    return None


def run_problem(mem, info, held):
    """Why a running game must stop at once (None: it may go on)"""
    free, swap = info
    if held is not None and held > mem["hard_mb"]:
        return "the game holds %d MB, at most %d" % (held, mem["hard_mb"])
    if free is not None and free < mem["floor_gb"]:
        return "%.1f GB available, under %.0f" % (free, mem["floor_gb"])
    if swap is not None and swap > mem["max_swap_gb"] + 4:
        return "%.1f GB of swap in use, over %.0f" % (swap, mem["max_swap_gb"] + 4)
    return None


def wait_memory(mem, read=meminfo, sleep=time.sleep, clock=time.monotonic, say=print):
    """Until a game may start: None, or why it still may not after mem["wait_s"]"""
    end, said = clock() + mem["wait_s"], None
    while True:
        why = start_problem(mem, read())
        if why is None:
            return None
        if clock() >= end:
            return why
        if why != said:
            say("pipeline tour: waits for memory: %s" % why, flush=True)
            said = why
        sleep(10)


def scope_prefix(mem, run=subprocess.run):
    """systemd-run's scope for the game (MemoryMax, a little swap), or [] where there is none"""
    if not mem.get("scope_gb") or not shutil.which("systemd-run") or os.environ.get("OOT_NO_SCOPE"):
        return []
    pre = ["systemd-run", "--user", "--scope", "--quiet", "--collect", "-p", "MemoryMax=%dG" % mem["scope_gb"],
           "-p", "MemorySwapMax=2G", "--"]
    try:
        if run(pre + ["true"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=20).returncode != 0:
            return []
    except (OSError, subprocess.TimeoutExpired):
        return []
    return pre


class MemoryStop(Exception):
    """The game stopped for memory (not a failure of the route: the tour goes on from `at` in a fresh game)"""
    def __init__(self, why, soft=False):
        Exception.__init__(self, why)
        self.soft = soft
