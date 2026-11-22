"""The CPU report: where the game's CPU time goes, by category, thread and function, from all-thread sampled runs.

    python -I scripts/perf.py cpu <run folder>... --maps <folder with longshot.map, longshot_vk.map> --out report.md
        [--csv <folder>] [--functions <more descriptions.json>] [--readobj <llvm-readobj>] [--top 40] [--cover 0.9]

A run folder holds the sampler's file (HOS_SAMPLE_ALL: `all.txt`, or `*_all.txt`), and for "ms a frame" `load.json` (the
measured window: fps and cores; scripts/perf.py report writes it into a profile folder) and `presents.csv` (frame times).
Without `load.json`, shares are still given as % of a core; "ms a frame" needs the run's fps (`--fps`).

Each line of the sampler's file is one thread at one instant: "<ms> <thread>|<frame>;<frame>;...", leaf first, each frame
module+offset. A sample is on the CPU unless its leaf is a waiting system call (cpu_categories.BLOCKING_SYSCALLS); naming a
system DLL's frames needs its exports, which --readobj (llvm-readobj) reads on Windows (without it, system leaves are counted
as on the CPU and the report says so).

Two views of each on-CPU sample:
  self   the category of the leaf frame: who executes the instructions;
  owner  the leaf, or for a leaf in the system, the C runtime or the driver, the first frame of ours above it: who asked for the
         work. A sleep's CPU inside ntdll under the host's WaitLightEvent is os/sync in this view.
Inclusive time per function counts each function once a sample when it is anywhere in the stack.

Categories and function descriptions are data (profiling/cpu_categories.py, profiling/cpu_functions.json), keyed on module,
object file, symbol and guest address, so the report regenerates from a new capture of a new build with no edits; symbols no
rule matches are listed as unclassified, hot functions with no description as undocumented. profiling/cpu_candidates.json lists
work that could be removed or done more cheaply, which the report prices from the capture.

The module, in the order the data flows: naming frames (Names, the guest map), counting a run's samples (analyse), reading a
run folder (Sampled, measured, machine, log_stats; the HLE counters, hle_*), and the page (Page, one method a section).
"""
import bisect, collections, csv, glob, json, math, os, re, statistics, subprocess, sys

from profiling import cpu_categories as cats, samples, symbols
from profiling.folder import build_of, read_csv, read_json, read_text

HERE = os.path.dirname(os.path.abspath(__file__))
BASES = {".exe": 0x140000000, ".dll": 0x180000000}
LIFTED = "lifted game code"   # (all lifted functions as one row: per-function game rows are game-derived and stay local)
OURS = ("game/", "runtime/", "os/", "render/", "tooling/", "unclassified")
OUR_MODULES = symbols.RUN_MODS
SERIAL = ("sead::MainThread", "Presentation_Thread")


# ---- naming frames: module+offset -> (key, object, module, category, symbol)

def load_map(path):
    """(addresses relative to the image, [(symbol, object)]) from an lld/MSVC linker map, every section"""
    base, rows = symbols.read_map(path)
    if base is None:
        base = BASES.get(os.path.splitext(path)[1].replace(".map", ""), 0x140000000)
    # (host/hos.h's HLE: a body not inlined into its imp_<sym> entry is hle_<sym>; it is the same function)
    return [a - base for a, _, _, _ in rows], [("imp_" + s[4:] if s.startswith("hle_") else s, o) for _, s, o, _ in rows]


def base_name(sym):
    """The key a function keeps across builds: lifted f_<guest address> and C names as they are; MSVC-mangled C++ as
    namespace::name, without the anonymous namespace's hash and the signature, which change between builds"""
    if "<lambda" in sym:
        m = re.search(r"\?\?([A-Za-z_]\w*)@((?:\w+@)*)@", sym)
        if m:
            scopes = [p for p in m.group(2).split("@") if p and not p.startswith("?A0x")]
            return "::".join(reversed(scopes)) + "::" + m.group(1) + "::lambda"
    if sym.startswith(("??0", "??1")):
        m = re.match(r"^\?\?[01](\w+)@((?:[\w?]+@)*)@", sym)
        if m:
            scopes = [p for p in m.group(2).split("@") if p and not p.startswith("?A0x")]
            return "::".join(list(reversed(scopes)) + [m.group(1), ("~" if sym[2] == "1" else "") + m.group(1)])
    if sym.startswith("?"):
        m = re.match(r"^\?\??\$?([A-Za-z_]\w*)@((?:[\w?]+@)*)@", sym)
        if m:
            scopes = [p for p in m.group(2).split("@") if p and not p.startswith("?A0x") and not p.startswith("?")]
            return "::".join(list(reversed(scopes)) + [m.group(1)])
    return sym


def load_exports(readobj, dll):
    """A system DLL's exports by RVA (llvm-readobj --coff-exports); ([], []) when they cannot be read"""
    path = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", dll)
    if not readobj or not os.path.exists(path):
        return [], []
    try:
        out = subprocess.run([readobj, "--coff-exports", path], capture_output=True, text=True, timeout=120).stdout
    except (OSError, subprocess.TimeoutExpired):
        return [], []
    rows = sorted((int(rva, 16), name) for name, rva in re.findall(r"Name: (\S+)\s+RVA: 0x([0-9A-Fa-f]+)", out))
    return [r[0] for r in rows], [(r[1], dll) for r in rows]


def is_ours(cat):
    return cat.startswith(OURS)


class Names:
    """frame "module+offset" -> (key, object, module, category, symbol); key is what the tables list"""

    def __init__(self, maps_dir, readobj=None, game_detail=False):
        self.game_detail = game_detail   # (False: every lifted function is one row, LIFTED; no guest address is printed)
        self.syms = {}
        for mod, mp in (("longshot.exe", "longshot.map"), ("longshot_vk.dll", "longshot_vk.map")):
            p = os.path.join(maps_dir, mp)
            if os.path.exists(p):
                self.syms[mod] = load_map(p)
        self.readobj = readobj
        self.maps_dir = maps_dir
        self.cache = {}
        self.rules = [tuple(re.compile(x) if x else None for x in r[:3]) + (r[3],) for r in cats.RULES]
        self.block = re.compile(cats.BLOCKING_SYSCALLS)
        self.system = re.compile(cats.SYSTEM_MODULES)
        self.system_named = False

    def category(self, mod, obj, sym):
        for rm, ro, rs, cat in self.rules:
            if (rm is None or rm.search(mod)) and (ro is None or ro.search(obj)) and (rs is None or rs.search(sym)):
                return cat
        return "unclassified" if mod in OUR_MODULES else "external"

    def of(self, frame):
        r = self.cache.get(frame)
        if r is None:
            mod, _, off = frame.rpartition("+")
            if mod in ("longshot", "longshot_vk.so"):   # (a Linux capture: its ELF maps are not read, so do not mislabel it)
                raise SystemExit("a Linux capture (%s): `perf.py cpu` reads Windows captures with lld-link maps only" % mod)
            if mod not in self.syms and self.system.search(mod):
                local = os.path.join(self.maps_dir, mod + ".map")
                self.syms[mod] = load_map(local) if os.path.exists(local) else load_exports(self.readobj, mod)
                self.system_named |= bool(self.syms[mod][0])
            sym, obj = (symbols.nearest(self.syms[mod], off) if mod in self.syms else None) or (mod or frame, mod)
            cat = self.category(mod, obj, sym)
            if mod in OUR_MODULES and not self.game_detail and cat.startswith("game"):
                key = LIFTED
            elif mod in OUR_MODULES:
                key = base_name(sym)
            elif sym != mod:   # (a system DLL's nearest export: may be the wrong name for an internal function)
                key = "%s (%s)" % (sym, mod)
            else:   # (a module without names: the driver)
                key = mod
            r = self.cache[frame] = (key, obj, mod, cat, sym)
        return r

    def blocked(self, leaf):
        return bool(self.block.search(leaf[4])) and bool(self.system.search(leaf[2]))


GUEST = {"starts": [], "rows": []}
# (how a guest range of the local guest map was labelled -> how far the report trusts it)
TRUST = {"module": "reliable", "paths": "reliable", "paths+words": "reliable", "stacks": "reliable", "test": "reliable",
         "names": "reliable", "read": "reliable", "behaviour": "guess", "words": "guess", "calls": "guess", "layout": "unlabelled"}


def load_guest(rows):
    """rows of (start, end, module, subsystem, how[, functions]), sorted by start for bisect"""
    rows = sorted(rows, key=lambda r: r[0])
    GUEST["rows"] = rows
    GUEST["starts"] = [r[0] for r in rows]


def guest_row(sym):
    """the guest map's row of a lifted function f_<guest address>: None when not lifted or no map, () when unmapped"""
    m = re.match(r"^f_([0-9a-fA-F]{8,16})$", sym)
    if not m or not GUEST["rows"]:
        return None
    a = int(m.group(1), 16)
    i = bisect.bisect_right(GUEST["starts"], a) - 1
    if i >= 0 and a < GUEST["rows"][i][1]:
        return GUEST["rows"][i]
    return ()


def guest_subsystem(sym):
    """game/lifted -> game/<module>/<subsystem> from the guest map (f_<guest address>); None when not lifted or no map"""
    r = guest_row(sym)
    if r is None:
        return None
    if not r:
        return "game/unmapped"
    return "game/%s/%s" % (r[2], "unlabelled" if TRUST.get(r[4]) == "unlabelled" else r[3])


def guest_trust(sym):
    r = guest_row(sym)
    return None if r is None else (TRUST.get(r[4], "guess") if r else "unlabelled")


def nn_name(sym):
    """imp__ZN2nn2os13WaitLightEventEP... -> nn::os::WaitLightEvent (the HLE entry the game called); other names unchanged"""
    m = re.match(r"^(?:imp_)?_ZN", sym)
    if not m:
        return sym.replace("n_nvn", "nvn", 1) if sym.startswith("n_nvn") else sym
    rest, parts = sym[m.end():], []
    while rest and rest[0].isdigit():   # (length-prefixed names until the nested name's E)
        n = re.match(r"^(\d+)", rest)
        k, d = int(n.group(1)), len(n.group(1))
        parts.append(rest[d:d + k])
        rest = rest[d + k:]
    return "::".join(parts) if parts else sym


def disp(sym):
    """a function as the tables print it: the HLE entries Longshot implements by their SDK name (nn::os::LockMutex)"""
    return nn_name(sym) if sym.startswith(("imp__ZN", "_ZN")) else sym


def calls_key(sym):
    """the counter's name for a frame key: imp__ZN... -> _ZN..., n_nvnX -> nvnX"""
    if sym.startswith("imp_"):
        return sym[4:]
    if sym.startswith("n_nvn"):
        return sym[2:]
    return sym


def wait_key(named):
    """a blocked sample: the HLE entry the game called (nn::..., nvn...) and the game function that called it"""
    entry = next((n[0] for n in named if n[0].startswith(("imp__ZN", "_ZN", "n_nvn"))), None)
    lifted = next((n[0] for n in named if n[3].startswith("game")), None)
    if entry:
        return nn_name(entry) + (" from " + lifted if lifted else "")
    # (an HLE entry that ends in a tail call leaves no frame of its own: the host function that waits, and the game's caller)
    owner = next((n for n in named if is_ours(n[3])), named[0])
    return owner[0] + (" from " + lifted if lifted and owner[0] != lifted else "")


# ---- counting a run's samples

def analyse(path, names, window=None, cands=()):
    """One run's samples (window: only those from start_s to end_s): (counters of samples by view and key, instants)"""
    instants = set()
    c = collections.defaultdict(collections.Counter)
    fcache = {}
    for ms_text, thread, frames in samples.records(path):   # (plain or packed)
        try:
            ms = float(ms_text)
        except ValueError:
            continue
        if not thread or window and not (window[0] <= ms / 1000.0 <= window[1]):
            continue
        if thread.startswith(("ModuleSystemWorker", "BaseProcExecutor", "NavMeshCAStepDepWorker")):
            thread = re.sub(r"\d+$", "N", thread)
        if not names.game_detail:   # (an unnamed thread is logged by its guest entry address: not published)
            thread = re.sub(r"^Thread_[0-9a-fA-F]+$", "unnamed thread", thread)
        instants.add(ms)
        named = [names.of(fr) for fr in frames.split(";") if fr]
        if not named:
            continue
        leaf = named[0]
        c["thread_all"][thread] += 1
        if names.blocked(leaf):
            c["blocked_in"][(thread, wait_key(named))] += 1
            continue
        c["thread_cpu"][thread] += 1
        selfcat = guest_subsystem(leaf[0]) or leaf[3]
        owner = next((n for n in named if is_ours(n[3])), leaf)
        ownercat = guest_subsystem(owner[0]) or owner[3]
        c["self_cat"][selfcat] += 1
        c["owner_cat"][ownercat] += 1
        c["self_fn"][(leaf[0], selfcat)] += 1
        c["owner_fn"][(owner[0], ownercat)] += 1
        c["thread_owner"][(thread, ownercat.split("/")[0])] += 1
        trust = guest_trust(owner[0])
        if trust:
            c["game_trust"][trust] += 1
        seen = set()
        for n in named:
            if n[0] not in seen:
                seen.add(n[0])
                c["incl_fn"][(n[0], guest_subsystem(n[0]) or n[3])] += 1
        if cands:
            hit = set()
            for n in named:
                ids = fcache.get(n[0])
                if ids is None:
                    ids = fcache[n[0]] = frozenset(cid for cid, rx in cands if rx.search(n[0]))
                hit |= ids
            for cid in hit:
                c["cand"][cid] += 1
                if thread in SERIAL:
                    c["cand_path"][cid] += 1
        if ownercat == "unclassified":
            c["unclassified"][(owner[2], owner[1], owner[0])] += 1
    return c, len(instants)


# ---- reading a run folder

def run_files(folder):
    """the run's sample file: all.txt, else the first *_all.txt; None when it has none"""
    if os.path.exists(os.path.join(folder, "all.txt")):
        return os.path.join(folder, "all.txt")
    found = sorted(glob.glob(os.path.join(folder, "*_all.txt")))
    return found[0] if found else None


def window_of(folder):
    """(fps, cores, the frame times in ms sorted) over load.json's measured window; (None, None, []) without load.json"""
    load = read_json(os.path.join(folder, "load.json"))
    if load is None:
        return None, None, []
    w, rows = load.get("window_present_s"), read_csv(os.path.join(folder, "presents.csv"))
    fps, times = None, []
    if w and rows:
        t0 = int(rows[0]["return_ns"]) - int(rows[0]["interval_ns"])
        inside = [r for r in rows if w[0] <= (int(r["return_ns"]) - t0) / 1e9 <= w[1]]
        fps = len(inside) / (w[1] - w[0]) if w[1] > w[0] else None
        times = sorted(int(r["interval_ns"]) / 1e6 for r in inside)
    return fps, load.get("window_cores"), times


def log_stats(folder):
    """The renderer's own per-frame lines in game.log, medians: GPU time, the replay's submit time, draws"""
    text = read_text(os.path.join(folder, "game.log"))

    def grab(pat):
        xs = [float(x) for x in re.findall(pat, text)]
        # (the last lines: a run stops at the end of its measured window, so they are play; the title screen before the load
        # prints far more lines at an unlocked frame rate, so "the second half" would be the menu)
        return statistics.median(xs[-4:]) if xs else None
    return dict(gpu_ms=grab(r"vk: GPU time ([0-9.]+) ms a frame"),
                submit_ms=grab(r"a frame's replay time \(ms\): submit ([0-9.]+)"),
                draws=grab(r"presents, [0-9.]+ fps; a frame: ([0-9.]+) draws"))


class Sampled:
    """one sampled run: its counters (analyse) over its instants, its fps and cores (load.json), its log's medians"""

    def __init__(self, folder, path, names, cands, fps=None):
        self.name = os.path.basename(os.path.normpath(folder))
        window_fps, self.cores, _ = window_of(folder)
        self.fps = window_fps or fps
        self.c, self.inst = analyse(path, names, cands=cands)
        self.log = log_stats(folder)

    def share(self, view, key):
        """% of one core"""
        return 100.0 * self.c[view][key] / self.inst if self.inst else 0.0

    def ms(self, pct):
        """ms a frame of a share"""
        return pct * 10.0 / self.fps if self.fps else None

    def busy(self, thread):
        """% of the thread's samples on the CPU"""
        return 100.0 * self.c["thread_cpu"][thread] / max(1, self.c["thread_all"][thread])

    def oncpu(self):
        """% of a core on the CPU, all threads"""
        return 100.0 * sum(self.c["thread_cpu"].values()) / self.inst if self.inst else 0.0


SPEED = re.compile(r"vi: game clock [0-9.]+: [0-9.]+ a second of wall time \(speed ([0-9.]+)\)")


def measured(folder):
    """An unsampled run's numbers: fps and cores from load.json/presents.csv, the game clock from game.log, frame times"""
    fps, cores, iv = window_of(folder)
    out = dict(name=os.path.basename(os.path.normpath(folder)), fps=fps, cores=cores)
    out["cpu_ms"] = cores * 1000.0 / fps if fps and cores else None
    sp = [float(x) for x in SPEED.findall(read_text(os.path.join(folder, "game.log")))]
    out["clock"] = statistics.median(sp[-4:]) if sp else None
    out["valid"] = out["clock"] is not None and abs(out["clock"] - 1.0) <= 0.01 + 1e-9 and fps is not None
    if iv:
        out.update(p99=iv[min(len(iv) - 1, int(0.99 * len(iv)))], worst=iv[-1], over50=sum(x > 50 for x in iv),
                   over100=sum(x > 100 for x in iv), over250=sum(x > 250 for x in iv))
    return out


def machine(folder):
    """version, GPU and the run's switches, from game.log and process.json"""
    info = {}
    log = read_text(os.path.join(folder, "game.log")).splitlines()
    ver, gpu = build_of(log)
    info.update({k: v for k, v in (("version", ver), ("gpu", gpu)) if v})
    process = read_json(os.path.join(folder, "process.json"))
    if process is not None:
        env = process.get("env", {})
        keep = ("HOS_FPS_MAX", "HOS_FPS_LOG", "HOS_WINDOW_SIZE", "HOS_ASPECT", "RX_VK_ASYNC", "RX_VK_PIPE_SKIP", "HOS_INPUT_FRAME_CLOCK")
        info["switches"] = " ".join("%s=%s" % (k, env[k]) for k in keep if k in env)
        rendering = {k: env[k] for k in env if k.startswith(("RX_VK_UPSCALE", "RX_VK_FRAMEGEN", "RX_VK_DLSS", "RX_VK_FSR",
                                                              "RX_VK_PACE", "HOS_DYNRES", "HOS_OUTPUT_RES", "HOS_HANDHELD"))}
        if rendering:
            info["switches"] += " " + " ".join("%s=%s" % kv for kv in sorted(rendering.items()))
    ini = read_text(os.path.join(folder, "longshot_options.ini"))   # (a runner may copy the game folder's options into the run)
    if ini:
        opts = dict(l.strip().split("=", 1) for l in ini.splitlines() if "=" in l)
        info["menu"] = " ".join("%s=%s" % (k, opts[k]) for k in ("quality_preset", "frame_generation", "vsync", "dlss",
                                                                     "resolution", "upscaling", "output_res") if k in opts)
    found = []   # (the effective settings, as the run itself logged them)
    for line in log:
        for pat in (r"oe: output resolution .*", r"vk: swapchain .*", r"(vk|menu): .*(upscaler|frame generation|DLSS|FSR [34]).*(on|off|chosen|used|runs).*",
                    r"vk: dynamic resolution .*"):
            m = re.search(pat, line)
            if m and not any(f_.split(":")[0] == m.group(0).split(":")[0] and f_[:20] == m.group(0)[:20] for f_ in found):
                found.append(m.group(0).strip())
        if len(found) >= 5:
            break
    if found:
        info["options"] = "; ".join(found)
    return info


# ---- the HLE call counters (HOS_HLE_STATS=1): --calls

def hle_blocks(folder):
    """Complete counter snapshots; legacy records have no clock boundaries and cannot be joined to frames."""
    lines = read_text(os.path.join(folder, "game.log")).splitlines()
    head = re.compile(r"hle: (\d+) calls of (\d+) functions in ([0-9.]+) s")
    heads = [(i, head.search(line)) for i, line in enumerate(lines) if head.search(line)]
    out = []
    for index, (i, match) in enumerate(heads):
        total, count, secs = int(match[1]), int(match[2]), float(match[3])
        if not math.isfinite(secs) or secs <= 0:
            continue
        end = heads[index + 1][0] if index + 1 < len(heads) else len(lines)
        rows = [entry for line in lines[i + 1:end] if "hle: " in line
                for entry in re.findall(r"(\d+) ([^;]+);", line.split("hle: ", 1)[1])]
        if len(rows) != count or sum(int(n) for n, _ in rows) != total:
            continue   # (a process may end while the rows of its last report are being printed)
        calls = collections.Counter()
        for n, name in rows:
            calls[name.strip()] += int(n)
        bounds = re.search(r"begin_ns=(\d+) end_ns=(\d+)\s*$", lines[i])
        span = tuple(map(int, bounds.groups())) if bounds else None
        if span and (span[1] <= span[0] or abs((span[1] - span[0]) / 1e9 - secs) > 1e-9):
            continue
        out.append(dict(calls=calls, seconds=secs, span=span))
    return out


def hle_calls(folder, blocks=4):
    """Raw rates from complete blocks; frame/call estimates additionally require hle_measurement's clock checks."""
    use = hle_blocks(folder)[-blocks:]
    if not use:
        return {}, None
    calls = collections.Counter()
    secs = sum(b["seconds"] for b in use)
    for b in use:
        calls.update(b["calls"])
    return {k: v / secs for k, v in calls.items()}, secs


def hle_measurement(folder, blocks=4):
    """Counter and native-frame totals over the same clock span, wholly inside the measured play window."""
    load = json.load(open(os.path.join(folder, "load.json"), encoding="utf-8"))
    if not isinstance(load, dict):
        raise ValueError("load.json must be an object")
    if "counter_window" in load and not isinstance(load["counter_window"], dict):
        raise ValueError("counter_window must be an object")
    w = load.get("window_present_s")
    play = load.get("counter_window", {}).get("analysis_window_ns")
    if load.get("failed"):
        raise ValueError("the measured run failed")
    with open(os.path.join(folder, "presents.csv"), encoding="utf-8") as f:
        rows = [(int(r["return_ns"]) - int(r["interval_ns"]), int(r["return_ns"])) for r in csv.DictReader(f)]
    if not rows or rows[0][1] < rows[0][0] or any(e <= b for b, e in rows[1:]):
        raise ValueError("invalid native frame intervals")
    if play is not None:
        if len(play) != 2 or not all(isinstance(x, int) for x in play) or play[1] <= play[0]:
            raise ValueError("invalid measured clock boundaries")
    else:
        if not w or len(w) != 2 or not all(math.isfinite(x) for x in w) or w[1] <= w[0]:
            raise ValueError("no valid measured play window")
        play = (rows[0][0] + round(w[0] * 1e9), rows[0][0] + round(w[1] * 1e9))
    if rows[0][0] == rows[0][1]:
        rows = rows[1:]   # (the native logger's first return has no preceding interval)
    if not rows or any(a[1] >= b[1] for a, b in zip(rows, rows[1:])):
        raise ValueError("invalid native frame order")
    use = [b for b in hle_blocks(folder) if b["span"] and play[0] <= b["span"][0] < b["span"][1] <= play[1]][-blocks:]
    if not use or any(a["span"][1] != b["span"][0] for a, b in zip(use, use[1:])):
        raise ValueError("no contiguous complete timed counter blocks inside the play window")
    span = use[0]["span"][0], use[-1]["span"][1]
    if rows[0][0] > span[0] or rows[-1][1] < span[1]:
        raise ValueError("native frames do not cover the counter span")
    if any(max(min(a[1], b[0]), span[0]) < min(max(a[1], b[0]), span[1]) for a, b in zip(rows, rows[1:])):
        raise ValueError("a gap or overlap in native frames crosses the counter span")
    frames = sum(span[0] < e <= span[1] for _, e in rows)
    if not frames:
        raise ValueError("no native frame returns in the counter span")
    calls = collections.Counter()
    for b in use:
        calls.update(b["calls"])
    return dict(calls=calls, seconds=(span[1] - span[0]) / 1e9, span=span, frames=frames)


def hle_sampled_costs(folder, names, measured):
    """Estimated sampled occupancy per call, using this counter run's stacks over its counter span."""
    path = run_files(folder)
    if not path:
        return {}
    origins, times = [], []
    for line in samples.lines(path):
        if line.startswith("# t0_ns "):
            origins.append(samples.origin(line))
            continue
        r = samples.parse(line)
        if r and r[1] and any(fr.strip() for fr in r[2].split(";")):
            try:
                times.append(float(r[0]))
            except ValueError:
                pass
    if len(origins) != 1 or origins[0] is None or not times or not all(math.isfinite(t) for t in times):
        return {}
    origin = origins[0]
    begin, end = measured["span"]
    if origin + min(times) * 1e6 > begin or origin + max(times) * 1e6 < end:
        return {}
    c, instants = analyse(path, names, window=((begin - origin) / 1e9, (end - origin) / 1e9))
    if not instants:
        return {}
    by_key = collections.Counter()
    for (sym, _), n in c["owner_fn"].items():
        by_key[calls_key(sym)] += n
    return {key: n / instants * measured["seconds"] * 1e6 / measured["calls"][key]
            for key, n in by_key.items() if measured["calls"].get(key)}


# ---- the page

MEASURE = re.compile(r"\d\s*%|%\s*of|\d[\d,.]*\s+(million\s+|thousand\s+)?(\w+\s+)?(a|per)\s+(frame|second)\b|"
                     r"\bon-CPU\b|\bof (the |all )?(on-CPU )?samples\b|\bsamples (of|are|under|in)\b|"
                     r"\b(a third|half|a quarter|two thirds|three quarters|most) of (all|its|the)\b|\bthe (largest|hottest|most sampled)\b|"
                     r"\b(one|two|three|four|five|\d+) (of|in) (every )?(two|three|four|five|six|ten|\d+) "
                     r"(waits|calls|lookups|samples|frames|times|runs|misses|hits)\b|\b(dozens|hundreds|thousands|millions) of (times|calls)\b|"
                     r"\b(a|one|two|several|a few) (dozen|hundred|thousand|million)\b[\w ]{0,40}?\b(a|per) (frame|second)\b|"
                     r"\b(one|two|three|four|\d+) or (two|three|four|five|\d+) [\w-]+( [\w-]+)? (a|per) \w+|\bwas measured\b", re.I)
SENTENCES = re.compile(r"(?<=[.;])\s+(?=[A-Z(])")


def unmeasured(text, behaviour=False):
    """text without its sentences that state a measurement, which would go stale with the next build or scene (every number
    on the page comes from the capture); behaviour: the first sentence says what the function does, so its measured
    parentheses are taken out rather than dropping it"""
    out = []
    for i, x in enumerate(SENTENCES.split(text or "")):
        if MEASURE.search(x) and behaviour and i == 0:
            x = re.sub(r"\s*\([^()]*\)", lambda m: "" if MEASURE.search(m.group(0)) else m.group(0), x)
        if x and not MEASURE.search(x):
            out.append(x)
    return " ".join(out)


def bottleneck_text(fps, gpu_ms, busy, serial):
    """the limit, in words, from the numbers: the GPU, a saturated thread, or the chain of waits"""
    frame = 1000.0 / fps if fps else None
    if frame and gpu_ms and gpu_ms > 0.8 * frame:
        return ("the GPU takes %s of the %s ms a frame, close to all of it: the GPU is the likely limit here, and CPU "
                "savings would mostly free CPU rather than raise the frame rate." % (fmt(gpu_ms, 1), fmt(frame, 1)))
    serial_names = {t for t, _b, _w in serial}
    hot = sorted(((t, b) for t, b in busy.items() if t in serial_names and b is not None and b >= 90.0), key=lambda kv: (-kv[1], kv[0]))
    if hot:
        return ("`%s`, one of the two serial threads, is on the CPU %s %% of the time: it is saturated, so its own work is "
                "the likely limit." % (hot[0][0], fmt(hot[0][1])))
    top_t = min(busy, key=lambda k: (-(busy[k] or 0), k))   # (the busiest thread, ties by name)
    parts = ["the GPU takes %s of the %s ms a frame and neither serial thread is saturated (the busiest thread overall: "
             "`%s`, %s %%), so the frame is most likely paced by the chain of hand-offs between threads" % (
                 fmt(gpu_ms, 1), fmt(frame, 1), top_t, fmt(busy[top_t]))]
    for t, run_pct, waits in serial:
        if waits:
            parts.append("`%s` runs %s %% of the time and waits most in `%s` (%s %%)" % (t, fmt(run_pct), waits[0][0], fmt(waits[0][1])))
    return ("; ".join(parts) + ". So the frame's path runs through the serial threads' own work and the work they wait "
            "for; a saving there can raise the frame rate, while a saving elsewhere frees CPU. Sampling shows where the "
            "threads wait, not which awaited job finishes last in each frame, so it cannot say which saving raises the "
            "frame rate most; a per-frame trace of the job system would settle that.")


def med(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None


def fmt(x, nd=1):
    return "-" if x is None else ("%.*f" % (nd, x))


class Page:
    """the report's markdown, a method a section in the page's order; glance collects the At a glance lines' numbers"""

    def __init__(self, runs, names, a):
        self.runs, self.names, self.a = runs, names, a
        self.out, self.glance = [], {}
        self.w = self.out.append

    def med(self, f):
        """the median over the runs of f(run)"""
        return med([f(r) for r in self.runs])

    def keys(self, view):
        """the view's keys, largest median share first"""
        return sorted({k for r in self.runs for k in r.c[view]}, key=lambda k: (-self.med(lambda r: r.share(view, k)), str(k)))

    def table(self, view, keys):
        """(key, median share, min, max, median ms a frame) of each key"""
        rows = []
        for k in keys:
            pcts = [r.share(view, k) for r in self.runs]
            rows.append((k, med(pcts), min(pcts), max(pcts), med([r.ms(x) for r, x in zip(self.runs, pcts)])))
        return rows

    def waits(self, thread=None):
        """where threads wait, % of the thread's samples (the mean of the runs): {(thread, wait): %}"""
        out = collections.Counter()
        for r in self.runs:
            for (t, f), n in r.c["blocked_in"].items():
                if thread is None or t == thread:
                    out[(t, f)] += 100.0 * n / (r.c["thread_all"][t] or 1) / len(self.runs)
        return out

    def render(self, docs, cdata, calls):
        w, runs = self.w, self.runs
        w("# CPU profile\n")
        w("Generated by `scripts/perf.py cpu` from %d run(s): %s. Shares are %% of one CPU core over the sampled instants, the "
          "median of the runs (min-max in brackets); ms a frame = share x 10 / fps.\n" % (len(runs), ", ".join(r.name for r in runs)))
        glance_at = len(self.out)
        if not self.names.system_named:
            w("System DLL frames were not named (no `--readobj`): blocked waits inside them are counted as on-CPU `system/other`.\n")
        if self.a.measured:
            self.measured_runs()
        self.sampled_runs()
        self.categories()
        self.limits()
        self.trust()
        self.threads()
        self.functions(docs, calls)
        self.blocked()
        if cdata["candidates"]:
            self.candidates(cdata)
        self.unclassified()
        g = self.at_a_glance()
        if g:
            self.out[glance_at:glance_at] = ["## At a glance\n"] + g + [""]
        return "\n".join(self.out) + "\n"

    def measured_runs(self):
        w = self.w
        ms_runs = [measured(f) for f in self.a.measured]
        info = machine(self.a.measured[0])
        w("## Measured without the sampler\n")
        w("The frame rate and CPU numbers of the scene. Sampling costs some frame rate, so these come from runs without it; "
          "the shares below come from the sampled runs.\n")
        if info.get("version") or info.get("gpu"):
            w("- Longshot %s; GPU %s" % (info.get("version", "?"), info.get("gpu", "?")))
        if info.get("switches"):
            w("- Switches: `%s`" % info["switches"])
        if info.get("options"):
            w("- Graphics settings, as the run logged them: %s" % info["options"])
        if info.get("menu"):
            w("- Insert-menu options: %s" % info["menu"])
        w("")
        w("| run | fps | CPU ms a frame | cores | game clock | p99 ms | worst ms | frames >50 / >100 / >250 ms | counted |")
        w("|---|---|---|---|---|---|---|---|---|")
        for m in ms_runs:
            w("| %s | %s | %s | %s | %s | %s | %s | %s / %s / %s | %s |" % (
                m["name"], fmt(m["fps"]), fmt(m["cpu_ms"]), fmt(m["cores"], 2), fmt(m["clock"], 3), fmt(m.get("p99")),
                fmt(m.get("worst")), m.get("over50", "-"), m.get("over100", "-"), m.get("over250", "-"),
                "yes" if m["valid"] else "no (clock)"))
        ok = [m for m in ms_runs if m["valid"]]
        fps, cpu_ms, cores = med([m["fps"] for m in ok]), med([m["cpu_ms"] for m in ok]), med([m["cores"] for m in ok])
        over = [sum(m.get(k, 0) for m in ok) for k in ("over50", "over100", "over250")]
        self.glance.update(fps=fps, cpu_ms=cpu_ms, cores=cores, n=len(ok), over100=over[1])
        w("| **median (%d counted)** | **%s** | **%s** | **%s** | **%s** | **%s** | max **%s** | %d / %d / %d in all | |" % (
            len(ok), fmt(fps), fmt(cpu_ms), fmt(cores, 2), fmt(med([m["clock"] for m in ok]), 3),
            fmt(med([m.get("p99") for m in ok])), fmt(max([m.get("worst") or 0 for m in ok]) if ok else None), *over))
        w("")

    def sampled_runs(self):
        w = self.w
        w("## Runs\n")
        w("Sampled shares are occupancy: a thread counts as on the CPU when its user-mode leaf is not a waiting system call, "
          "which includes a thread that is ready but preempted, and leaves out time in the kernel. The measured process cores "
          "(from the operating system's CPU time) are the reference; the two columns below show how far the sampled total is "
          "from it.\n")
        w("| run | fps | cores (measured) | cores (sampled, on-CPU) | instants | on-CPU samples | blocked samples |")
        w("|---|---|---|---|---|---|---|")
        for r in self.runs:
            oncpu = sum(r.c["thread_cpu"].values())
            blocked = sum(r.c["thread_all"].values()) - oncpu
            w("| %s | %s | %s | %s | %d | %d | %d |" % (r.name, fmt(r.fps), fmt(r.cores, 2),
                                                    fmt(oncpu / r.inst if r.inst else None, 2), r.inst, oncpu, blocked))
        w("")

    def categories(self):
        w, runs = self.w, self.runs
        for view, title in (("owner_cat", "By category: who asked for the work (owner view)"), ("self_cat", "By category: who executes it (self view)")):
            keys = self.keys(view)
            tops_run = []
            for r in runs:
                t_ = collections.Counter()
                for k, n in r.c[view].items():
                    t_[k.split("/")[0]] += n
                tops_run.append({k: 100.0 * v / r.inst for k, v in t_.items()} if r.inst else {})
            tops = {k: med([t.get(k, 0.0) for t in tops_run]) for k in {k for t in tops_run for k in t}}
            w("## %s\n" % title)
            w("| category | % of a core | ms a frame |")
            w("|---|---|---|")
            if view == "owner_cat":
                self.glance["tops"] = sorted(tops.items(), key=lambda kv: (-kv[1], kv[0]))
                subs = {k: self.med(lambda r: r.share(view, k)) for k in keys}
                self.glance["subs"] = sorted(((k, v) for k, v in subs.items() if "/" in k), key=lambda kv: (-kv[1], kv[0]))
            for top, pct in sorted(tops.items(), key=lambda kv: (-kv[1], kv[0])):
                w("| **%s** | **%s** | **%s** |" % (top, fmt(pct), fmt(med([r.ms(t.get(top, 0.0)) for r, t in zip(runs, tops_run)]), 2)))
                for k, m_, lo, hi, msf in self.table(view, [k for k in keys if k.split("/")[0] == top]):
                    if k != top:
                        w("| &nbsp;&nbsp;%s | %s (%s-%s) | %s |" % (k, fmt(m_), fmt(lo), fmt(hi), fmt(msf, 2)))
            w("")

    def limits(self):
        w, runs = self.w, self.runs
        w("## What limits the frame\n")
        w("Three things can set the frame time: the GPU, when its time a frame is close to the frame time; one of the two serial "
          "threads below, when it is on the CPU nearly all the time; or the chain of waits between threads. Occupancy shows "
          "which threads are busy and where the serial threads wait; it does not by itself prove which dependency is the "
          "limit, so the reading below says how far the numbers go.\n")
        fps_m = self.med(lambda r: r.fps)
        gpu = self.med(lambda r: r.log.get("gpu_ms"))
        w("| | value |")
        w("|---|---|")
        w("| frame time of the sampled runs (1000 / their fps) | %s ms |" % fmt(1000.0 / fps_m if fps_m else None, 2))
        w("| GPU time a frame (renderer's log) | %s ms |" % fmt(gpu, 2))
        w("| replay's submit time a frame (renderer's log) | %s ms |" % fmt(self.med(lambda r: r.log.get("submit_ms")), 2))
        w("| draws a frame | %s |" % fmt(self.med(lambda r: r.log.get("draws")), 0))
        w("")
        w("| thread | busy (on CPU / sampled) | on CPU, % of a core |")
        w("|---|---|---|")
        busy = {t: self.med(lambda r: r.busy(t)) for t in {k for r in runs for k in r.c["thread_all"]}}
        for t in sorted(busy, key=lambda k: (-busy[k], k))[:12]:
            w("| %s | %s %% | %s |" % (t, fmt(busy[t]), fmt(self.med(lambda r: r.share("thread_cpu", t)))))
        w("")
        w("### The frame's two serial threads: running, and what they wait in\n")
        w("The game's main thread (`sead::MainThread`) builds each frame's commands; the game's `Presentation_Thread` submits "
          "them, and with the default `RX_VK_ASYNC=0` the renderer replays each submission on Vulkan on that same thread. When "
          "neither is saturated, the frame is paced by what each waits for: the other thread, the game's job workers (when the "
          "main thread waits for their jobs, their work is on the frame's path too), or the GPU and the display.\n")
        w("| thread | running | waits in (top 6, % of the thread's samples) |")
        w("|---|---|---|")
        serial = []
        for t in SERIAL:
            if not any(r.c["thread_all"][t] for r in runs):
                continue
            waits = collections.Counter({f: p for (_, f), p in self.waits(t).items()})
            serial.append((t, busy[t], waits.most_common(2)))
            w("| %s | %s %% | %s |" % (t, fmt(busy[t]), "; ".join("`%s` %s %%" % (f, fmt(p)) for f, p in waits.most_common(6))))
        self.glance["serial"] = serial
        w("")
        w("**Reading:** " + bottleneck_text(fps_m, gpu, busy, serial) + "\n")

    def trust(self):
        w, runs = self.w, self.runs
        if not any(r.c["game_trust"] for r in runs):
            return
        w("## How far the game's subsystem labels can be trusted\n")
        w("Share of lifted game code's on-CPU samples by how its guest range was labelled: reliable (the module table, source "
          "paths in the code, its place in every stack), guess (what it calls, words it logs, behaviour) or unlabelled.\n")
        w("| label | % of game samples |")
        w("|---|---|")
        for k in ("reliable", "guess", "unlabelled"):
            w("| %s | %s |" % (k, fmt(self.med(lambda r: 100.0 * r.c["game_trust"][k] / max(1, sum(r.c["game_trust"].values()))))))
        w("")

    def threads(self):
        w = self.w
        tops = ("game", "runtime", "os", "render")
        w("## Threads\n")
        w("| thread | on CPU, % of a core | ms a frame | of which game | runtime | os | render | system/driver/crt |")
        w("|---|---|---|---|---|---|---|---|")
        for t in self.keys("thread_cpu")[:30]:
            parts = [self.med(lambda r: r.share("thread_owner", (t, top))) for top in tops]
            other = self.med(lambda r: r.share("thread_cpu", t) - sum(r.share("thread_owner", (t, top)) for top in tops))
            w("| %s | %s | %s | %s |" % (t, fmt(self.med(lambda r: r.share("thread_cpu", t))),
                                         fmt(self.med(lambda r: r.ms(r.share("thread_cpu", t))), 2),
                                         " | ".join(fmt(x) for x in parts + [other])))
        w("")

    def by_sym(self, view):
        """% of a core of each function, whatever its category, a run"""
        out = []
        for r in self.runs:
            d = collections.Counter()
            for (sym, _cat), n in r.c[view].items():
                d[sym] += n
            out.append({k: 100.0 * v / r.inst for k, v in d.items()} if r.inst else {})
        return out

    def functions(self, docs, calls):
        """one table, the owner view, listed until --cover of the on-CPU time is covered, with each function's self and
        inclusive share beside its owner share; the first --top rows shown, the rest folded"""
        w, runs, a = self.w, self.runs, self.a
        cps, cfps, call_costs = {}, None, {}
        if calls:
            cps = {key: n / calls["seconds"] for key, n in calls["calls"].items()}
            cfps, call_costs = calls["frames"] / calls["seconds"], calls["costs"]
            w("Call counts come from `%s` (`HOS_HLE_STATS=1`), %.3f s inside its play window: **calls a frame** uses native "
              "frame returns over that same counter span. **Estimated occupancy us/call** uses that run's timestamped "
              "owner-stack samples over the same span; measured process CPU remains separate.\n" %
              (os.path.basename(os.path.normpath(a.calls)), calls["seconds"]))
            if not call_costs:
                w("Per-call estimates unavailable: no timestamped counter-run stacks cover this span.\n")
        self_s, incl_s = self.by_sym("self_fn"), self.by_sym("incl_fn")
        w("## Functions: what each costs\n")
        w("Every function in the order of its **owner** share (the work it asked for: its own instructions, and the system, "
          "C runtime and driver time below it); **self** is the time in its own instructions, **inclusive** the time with it "
          "anywhere in the stack (callees included). Listed until %d %% of the on-CPU time is covered; the first %d rows "
          "are shown, the rest are folded below them.\n" % (int(a.cover * 100), a.top))
        head = ("| function | category | owner: %% of a core | ms a frame | self | inclusive |%s what it does |"
                % (" calls a frame | estimated occupancy us/call |" if cps else ""))
        rule = "|---|---|---|---|---|---|%s---|" % ("---|---|" if cps else "")
        rows, undocumented = [], []
        run_oncpu = [r.oncpu() for r in runs]
        run_cov = [0.0] * len(runs)

        def covered():
            return med([c_ / o_ if o_ else 1.0 for c_, o_ in zip(run_cov, run_oncpu)])
        for k, m_, lo, hi, msf in self.table("owner_fn", self.keys("owner_fn")):
            if covered() >= a.cover:
                break
            sym, cat = k
            d = docs.get(sym, {})
            text = (unmeasured(d.get("does", ""), behaviour=True) + " " + unmeasured(d.get("note", ""))).strip()
            if text and d.get("source") == "generated":
                text = "(generated) " + text
            run_cov = [c_ + r.share("owner_fn", k) for c_, r in zip(run_cov, runs)]
            if not d.get("does"):
                undocumented.append(sym)
            extra = ""
            if cps:
                cpf = cps.get(calls_key(sym), 0) / cfps if cfps else None
                extra = " %s | %s |" % (fmt(cpf, 0) if cpf else "-", fmt(call_costs.get(calls_key(sym)), 2))
            rows.append("| `%s` | %s | %s (%s-%s) | %s | %s | %s |%s %s |" % (
                disp(sym), cat, fmt(m_), fmt(lo), fmt(hi), fmt(msf, 2), fmt(med([x.get(sym, 0.0) for x in self_s])),
                fmt(med([x.get(sym, 0.0) for x in incl_s])), extra, text.replace("|", "/")))
        w(head)
        w(rule)
        for line in rows[:a.top]:
            w(line)
        w("")
        if len(rows) > a.top:
            w("<details><summary>The other %d functions up to %d %% of the on-CPU time</summary>\n" % (len(rows) - a.top, int(a.cover * 100)))
            w(head)
            w(rule)
            for line in rows[a.top:]:
                w(line)
            w("\n</details>\n")
        w("These %d functions cover %s %% of the on-CPU time (the median of the runs; %s of %s %% of a core).\n" % (
            len(rows), fmt(100.0 * covered()), fmt(med(run_cov)), fmt(med([x for x, r in zip(run_oncpu, runs) if r.inst]) or 1.0)))
        if a.csv and undocumented:
            os.makedirs(a.csv, exist_ok=True)
            open(os.path.join(a.csv, "undocumented.txt"), "w", encoding="utf-8", newline="\n").write("\n".join(undocumented) + "\n")
        if undocumented:
            w("Not yet described (add them to `scripts/profiling/cpu_functions.json`): %d, the hottest: %s\n" % (
                len(undocumented), ", ".join("`%s`" % x for x in undocumented[:20])))

    def blocked(self):
        w = self.w
        w("## Blocked: where threads wait (not CPU time)\n")
        w("| thread | waiting in | % of the thread's samples |")
        w("|---|---|---|")
        for (t, f), pct in self.waits().most_common(self.a.top):
            w("| %s | `%s` | %s |" % (t, f, fmt(pct)))
        w("")

    def candidates(self, cdata):
        w, runs = self.w, self.runs
        w("## What could be removed, merged or done more cheaply\n")
        w("Each row is priced from these runs: **cost** is the on-CPU time of every sample with the named work anywhere in "
          "its stack; **on the serial threads** is the part of it on the main thread and the Presentation Thread, where a "
          "saving is most likely to shorten the frame (work on the job workers can shorten it too when the main thread is "
          "waiting for that work, as the waits table shows; otherwise a saving frees CPU, which helps smaller CPUs); **gain** is the cost times "
          "the share the fix is expected to remove, always an estimate; **supporting measurement** names an earlier "
          "measurement behind the estimate, where there is one. Ranked by the gain's middle.\n")
        rows = []
        for x in cdata["candidates"]:
            pct = self.med(lambda r: r.share("cand", x["id"])) or 0.0
            path = self.med(lambda r: r.share("cand_path", x["id"])) or 0.0
            msf = self.med(lambda r: r.ms(r.share("cand", x["id"]))) or 0.0
            lo, hi = x["gain"]
            rows.append((msf * (lo + hi) / 2, x, pct, path, msf, lo, hi))
        rows.sort(key=lambda t: -t[0])
        self.glance["cands"] = rows
        w("| # | what | cost: % of a core (ms a frame) | on the serial threads, % of a core | fix | gain (estimate), ms a frame | risk | supporting measurement |")
        w("|---|---|---|---|---|---|---|---|")
        for i, (_, x, pct, path, msf, lo, hi) in enumerate(rows, 1):
            what = re.sub(r"\s*\([^()]*\)", lambda m: "" if MEASURE.search(m.group(0)) else m.group(0), unmeasured(x["what"]))
            w("| %d | **%s**%s | %s (%s) | %s | %s | %s to %s | %s | %s |" % (
                i, x["title"], (": " + what) if what else "", fmt(pct), fmt(msf, 2), fmt(path), x["fix"], fmt(msf * lo, 2), fmt(msf * hi, 2),
                x["risk"], x["evidence"]))
        w("")
        if cdata.get("game_own"):
            w("Large costs that are the game's own work (not Longshot's to remove; for scale):\n")
            w("| what | % of a core | ms a frame | note |")
            w("|---|---|---|---|")
            for x in cdata["game_own"]:
                pcts = [sum(v for k, v in r.c["owner_cat"].items() if k.startswith(x["match"])) * 100.0 / r.inst for r in runs]
                w("| %s | %s | %s | %s |" % (x["title"], fmt(med(pcts)), fmt(med([r.ms(p) for r, p in zip(runs, pcts)]), 2), x["note"]))
            w("")

    def unclassified(self):
        w = self.w
        unc = collections.Counter()
        for r in self.runs:
            unc.update(r.c["unclassified"])
        if unc:
            w("## Unclassified (add a rule to `scripts/profiling/cpu_categories.py`)\n")
            w("| module | object | symbol | samples |")
            w("|---|---|---|---|")
            for (mod, obj, sym), n in unc.most_common(30):
                w("| %s | %s | `%s` | %d |" % (mod, obj, sym, n))
            w("")

    def at_a_glance(self):
        gl, g = self.glance, []
        if gl.get("fps"):
            g.append("- **Speed:** %s fps and %s ms of CPU a frame (%s cores), median of %d runs without the sampler; %d frames over "
                     "100 ms in all of them." % (fmt(gl["fps"]), fmt(gl["cpu_ms"]), fmt(gl["cores"], 2), gl["n"], gl["over100"]))
        if gl.get("tops"):
            g.append("- **Where the CPU goes:** " + ", ".join("%s %s %%" % (k, fmt(v)) for k, v in gl["tops"][:5]) +
                     " of a core; the largest parts: " + ", ".join("%s %s %%" % (k, fmt(v)) for k, v in gl["subs"][:5]) + ".")
        for t, busy, waits in gl.get("serial", []):
            g.append("- **%s:** running %s %% of the time; it waits most in %s." % (
                t, fmt(busy), " and ".join("`%s` (%s %%)" % (f, fmt(p)) for f, p in waits) or "-"))
        if gl.get("cands"):
            path = sorted(gl["cands"], key=lambda r: -(r[3] * (r[5] + r[6]) / 2))[:3]
            g.append("- **Largest estimated savings:** " + "; ".join("%s (%s to %s ms a frame)" % (r[1]["title"], fmt(r[4] * r[5], 2),
                                                                                               fmt(r[4] * r[6], 2)) for r in gl["cands"][:3]) + ".")
            g.append("- **On the serial threads** (the main and Presentation threads, where a saving is most likely to shorten the frame): " +
                     "; ".join("%s (%s %% of a core there)" % (r[1]["title"], fmt(r[3])) for r in path) + ".")
        return g

    def write_csv(self, folder):
        """each view as CSV: a row a key, a column a run's % of a core"""
        os.makedirs(folder, exist_ok=True)
        for view in ("owner_cat", "self_cat", "owner_fn", "self_fn", "incl_fn", "thread_cpu"):
            with open(os.path.join(folder, view + ".csv"), "w", encoding="utf-8", newline="") as f:
                wr = csv.writer(f)
                wr.writerow(["key"] + ["%s_pct_core" % r.name for r in self.runs])
                for k in self.keys(view):
                    wr.writerow([k if isinstance(k, str) else "|".join(map(str, k))] + ["%.3f" % r.share(view, k) for r in self.runs])


# ---- the subcommand

def read_data(extra_functions, extra_candidates):
    """(function descriptions, candidates) from the package's files and the given ones: a generated description gives way to
    a written one"""
    docs = {}
    for fp in [os.path.join(HERE, "cpu_functions.json")] + extra_functions:
        for k, v in read_json(fp, {}).items():
            docs.setdefault(k, v) if v.get("source") == "generated" else docs.__setitem__(k, v)
    cdata = {"candidates": [], "game_own": []}
    for fp in [os.path.join(HERE, "cpu_candidates.json")] + extra_candidates:
        for k, v in read_json(fp, {}).items():
            cdata.setdefault(k, []).extend(v)
    return docs, cdata


def read_calls(folder, names):
    """--calls: the counter run's calls and frames over its counter span, and the sampled cost of a call"""
    try:
        counted = hle_measurement(folder)
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise SystemExit("--calls %s: %s (use complete timestamped HLE counters and native presents)" % (folder, error))
    return dict(counted, costs=hle_sampled_costs(folder, names, counted))


def run(a):
    """write the report; a: the cpu subcommand's arguments (scripts/perf.py)"""
    names = Names(a.maps, a.readobj, game_detail=bool(a.guest_map))
    rows = []
    if a.guest_map:
        ns = {}
        exec(compile(open(a.guest_map, encoding="utf-8").read(), a.guest_map, "exec"), ns)   # (a local data file of literals)
        rows = list(ns["GUEST_MAP"])
    load_guest(rows)
    docs, cdata = read_data(a.functions, a.candidates)
    cands = [(x["id"], re.compile(x["match"])) for x in cdata["candidates"]]
    runs = []
    for folder in a.runs:
        path = run_files(folder)
        if not path:
            print("no sampler file in %s" % folder, file=sys.stderr)
            continue
        runs.append(Sampled(folder, path, names, cands, a.fps))
    if not runs:
        raise SystemExit("no runs")
    if not any(m in names.syms and names.syms[m][0] for m in OUR_MODULES):
        raise SystemExit("no linker map of longshot.exe or longshot_vk.dll in --maps: `perf.py cpu` reads Windows captures "
                         "with lld-link maps only")
    calls = read_calls(a.calls, names) if a.calls else None
    page = Page(runs, names, a)
    text = page.render(docs, cdata, calls)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)) or ".", exist_ok=True)
    open(a.out, "w", encoding="utf-8", newline="\n").write(text)
    if a.csv:
        page.write_csv(a.csv)
    print("wrote %s (%d runs)" % (a.out, len(runs)))
