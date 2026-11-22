"""A run folder's files: the profiling mode's (HOS_PROFILE=1, host/profiling.c: logs/profile-<date>-<time>/) and a sampled
run's for `perf.py cpu` (the same files). Reading them, the build the log names, and the run's clock (seconds from its
first present), as the report, the trace and the CPU page read them."""
import csv, json, os, re, shutil

from profiling import samples, symbols

GPU_LINE = re.compile(r"vk: ([^;]+?, Vulkan \d+\.\d+, driver \w+); mesh shaders")   # (the device the renderer chose)
VCORE_WAIT = re.compile(r"(\d+) vcore_wait ([\d.]+) ms from (\d+): (.*)")   # (events.txt, host/hos_thread.c vc_long_wait)


def read_csv(path):
    """the rows of a CSV file as dicts; [] when there is no file"""
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8", errors="replace") as f:
        return list(csv.DictReader(f))


def read_json(path, default=None):
    """a JSON file's value; default when there is no file"""
    if not os.path.exists(path):
        return default
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def read_text(path):
    """a text file's text; "" when there is no file"""
    if not os.path.exists(path):
        return ""
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def num(row, k, default=0.0):
    """a CSV cell as a number; default when empty, missing or cut short"""
    try:
        return float(row.get(k) or default)
    except ValueError:
        return default


def build_of(lines):
    """(version, GPU) a game log's lines name; None for each it does not"""
    ver = next((l.split("longshot ", 1)[1].strip() for l in lines if "longshot v" in l), None)
    gpu = next((m.group(1) for l in lines for m in [GPU_LINE.search(l)] if m), None)
    return ver, gpu


def default_maps(d):
    """the build's symbols for a profile folder d: the game folder it is in (<d>/../..) when that holds the binaries or maps,
    else RX_MAPS"""
    game = os.path.dirname(os.path.dirname(os.path.abspath(d)))
    if any(os.path.exists(os.path.join(game, m)) for m in ("longshot", "longshot_vk.so", "longshot.map", "longshot_vk.map")):
        return game
    return os.environ.get("RX_MAPS")


def names_for(d, maps=None):
    """symbols.Names for the folder d: maps when given, else default_maps(d); without either, no symbols"""
    folder = maps or default_maps(d)
    return symbols.Names(folder) if folder else symbols.Names(maps={})


class Run:
    """a profile folder's presents, frame trace, cpu.csv (cpu_rows: t_ns, process_cpu_ns, rss_mb, available_mb) and long
    waits for a virtual core (vcore_waits: begin_ns, end_ns, ms, text), and its clock: origin, the first present (else the
    first frame, else the samples' t0), and the window [lo, hi] read (from_s / to_s seconds from the origin). A run that
    ended abruptly cuts its last line: num() takes what is there, and a row without its time is left out"""

    def __init__(self, d, from_s=None, to_s=None):
        self.d = d
        self.sample_path = os.path.join(d, "all.txt")
        self.t0_ns = samples.t0(self.sample_path) if os.path.exists(self.sample_path) else None
        self.presents = read_csv(os.path.join(d, "presents.csv"))
        self.frames = read_csv(os.path.join(d, "frames.csv"))
        self.cpu_rows = [(int(num(r, "t_ns")), int(num(r, "process_cpu_ns")), num(r, "rss_mb"), num(r, "available_mb"))
                         for r in read_csv(os.path.join(d, "cpu.csv"))
                         if num(r, "t_ns") > 0 and r.get("available_mb") not in (None, "")]
        self.vcore_waits = []
        for line in read_text(os.path.join(d, "events.txt")).splitlines():
            m = VCORE_WAIT.match(line)
            if m:
                self.vcore_waits.append((int(m.group(3)), int(m.group(1)), float(m.group(2)), m.group(4)))
        first_present = [int(num(r, "return_ns")) for r in self.presents if num(r, "return_ns") > 0][:1]
        first_frame = [int(num(r, "at_ns")) for r in self.frames if num(r, "at_ns")][:1]
        starts = first_present + first_frame + ([self.t0_ns] if self.t0_ns else [])
        if not starts:
            raise SystemExit("%s holds no presents, frames or samples: not a profile folder (HOS_PROFILE=1)" % d)
        self.origin = starts[0] if first_present or first_frame else min(starts)
        self.lo = self.origin + int((from_s or 0) * 1e9)
        self.hi = self.origin + int(to_s * 1e9) if to_s is not None else None

    def inside(self, ns):
        return ns >= self.lo and (self.hi is None or ns <= self.hi)

    def sec(self, ns):
        return (ns - self.origin) / 1e9

    def env(self):
        """the run's switches (process.json)"""
        return read_json(os.path.join(self.d, "process.json"), {}).get("env", {})

    def log(self):
        """the run's game.log as lines; copied first from the game's live log when the folder has none and the live log is
        this run's (it names this folder, and no later profile folder exists)"""
        path = os.path.join(self.d, "game.log")
        if not os.path.exists(path):
            game = os.path.dirname(os.path.dirname(self.d))
            live, pj = os.path.join(game, "logs", "longshot_log.txt"), os.path.join(self.d, "process.json")
            if os.path.exists(live) and os.path.exists(pj) and os.path.getmtime(live) >= os.path.getmtime(pj):
                later = [p for p in os.listdir(os.path.dirname(self.d)) if p.startswith("profile-") and p > os.path.basename(self.d)]
                if not later and os.path.basename(self.d) in read_text(live):   # (its prof: line names the folder)
                    shutil.copyfile(live, path)
        return read_text(path).splitlines()
