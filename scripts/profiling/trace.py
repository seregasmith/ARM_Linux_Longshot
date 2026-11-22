"""The profiling mode's folder (HOS_PROFILE=1, host/profiling.c) as files for a profiler's own viewer, written beside report.md
by scripts/perf.py report:

    trace.json.gz     for Perfetto (https://ui.perfetto.dev, "Open trace file"; it reads the file in the browser, nothing
                      is uploaded): the run as a timeline. Each thread's stack over time, as a flame chart built from its
                      samples (a stack that stays the same from one sample to the next is one slice; a blocked thread shows
                      where it waits, under "(waiting)"); a Frames track with each frame (its trace columns as the slice's
                      arguments, a stutter named STUTTER); the long waits for a virtual core; and counters: frame time,
                      CPU cores, the process's memory, the system's available memory
    speedscope.json   for speedscope (https://www.speedscope.app, "Browse"; also local): each thread's samples as a flame
                      graph ("Left Heavy") and a table of the functions' self and total time ("Sandwich"), and one profile of
                      every thread's running samples together. Identical stacks are added up, so its "Time Order" view is not
                      the run's order: Perfetto has that

A long run makes a big trace (roughly 5 to 10 MB a minute, packed): report's --from-s and --to-s keep a part of it.
Like report.md, both name lifted functions by their guest address: keep them on this PC.
"""
import collections, gzip, json, os

from profiling import samples
from profiling.folder import num

GAP_MS = 50   # (a thread missing from the samples longer than this: its open slices end, as nothing is known of it)


def write(run, names):
    """trace.json.gz and speedscope.json in the folder of run (folder.Run: the report's, its window too; names:
    symbols.Names); returns their paths"""
    d, sample_path, presents, frames = run.d, run.sample_path, run.presents, run.frames
    origin, inside = run.origin, run.inside

    def us(ns):
        return round((ns - origin) / 1000.0, 1)

    ev = []
    tids = {}

    def tid(name):
        if name not in tids:
            tids[name] = len(tids) + 10
            ev.append({"ph": "M", "name": "thread_name", "pid": 1, "tid": tids[name], "args": {"name": name}})
            ev.append({"ph": "M", "name": "thread_sort_index", "pid": 1, "tid": tids[name], "args": {"sort_index": tids[name]}})
        return tids[name]

    ev.append({"ph": "M", "name": "process_name", "pid": 1, "args": {"name": "Longshot (%s)" % os.path.basename(d)}})
    tid("Frames")
    tid("Virtual-core waits")

    # ---- frames: a slice each, its trace columns as arguments
    cols = [c for c in (frames[0].keys() if frames else []) if c not in ("frame", "at_ns")]
    for r in frames:
        at = int(num(r, "at_ns"))
        ms = num(r, "ms")
        if not at or not inside(at):
            continue
        args = {c: num(r, c) for c in cols if num(r, c)}
        ev.append({"ph": "X", "pid": 1, "tid": tids["Frames"], "ts": us(at - int(ms * 1e6)), "dur": round(ms * 1000.0, 1),
                   "name": ("STUTTER %.0f ms" if ms >= 50 else "frame %.1f ms") % ms, "cat": "frame", "args": args})
        ev.append({"ph": "C", "pid": 1, "name": "frame ms", "ts": us(at), "args": {"ms": ms}})
    if not frames:
        for r in presents[1:]:
            at, iv = int(num(r, "return_ns")), num(r, "interval_ns") / 1e6
            if at and inside(at) and iv > 0:
                ev.append({"ph": "X", "pid": 1, "tid": tids["Frames"], "ts": us(at - int(iv * 1e6)), "dur": round(iv * 1000.0, 1),
                           "name": ("STUTTER %.0f ms" if iv >= 50 else "frame %.1f ms") % iv, "cat": "frame"})
                ev.append({"ph": "C", "pid": 1, "name": "frame ms", "ts": us(at), "args": {"ms": iv}})

    # ---- the long waits for a virtual core (events.txt)
    for begin, end, ms, text in run.vcore_waits:
        if inside(end):
            who = text.split(" (priority", 1)[0]
            ev.append({"ph": "X", "pid": 1, "tid": tids["Virtual-core waits"], "ts": us(begin), "dur": round(ms * 1000.0, 1),
                       "name": "%s waited %.1f ms" % (who, ms), "cat": "vcore", "args": {"detail": text}})

    # ---- counters: cores, memory
    rows = run.cpu_rows
    for (a, ca, _, _), (b, cb, rss, avail) in zip(rows, rows[1:]):
        if inside(b) and b > a:
            ev.append({"ph": "C", "pid": 1, "name": "CPU cores", "ts": us(b), "args": {"cores": round((cb - ca) / (b - a), 2)}})
            ev.append({"ph": "C", "pid": 1, "name": "memory MB", "ts": us(b), "args": {"process": rss, "system available": avail}})

    # ---- the samples: a flame chart a thread, and speedscope's added-up stacks
    open_stack = {}   # thread -> (frames outermost first, open since ts, last seen ms)
    agg = collections.defaultdict(collections.Counter)   # thread -> stack tuple (outermost first) -> samples
    every = collections.Counter()   # running stacks of every thread
    interval_ms = 2.0   # (the sampler's: the commonest gap between sweeps, below)
    gaps = collections.Counter()

    def close(th, upto, at_us):
        """end the thread's open slices deeper than upto, at at_us"""
        st, since, seen = open_stack[th]
        for depth in range(len(st) - 1, upto - 1, -1):
            ev.append({"ph": "E", "pid": 1, "tid": tids[th], "ts": at_us})
        open_stack[th] = (st[:upto], since, seen)

    last_ms = None
    if os.path.exists(sample_path):
        prev_ms = None
        for ns, th, st in samples.timed(sample_path):   # (each "# t0_ns" applied: a pause and resume keep their times)
            ms = (ns - origin) // 1000000   # (ms from the run's 0 s, for the gaps and a thread's last sample)
            if prev_ms is not None and ms > prev_ms:
                gaps[ms - prev_ms] += 1
            prev_ms = ms
            if not inside(ns):
                continue
            last_ms = ms
            running, leaf, owner, named = names.stack(st)
            stack = tuple(reversed(named))
            if not running:
                stack = stack + ("(waiting)",)
            agg[th][stack] += 1
            if running:
                every[(th,) + stack] += 1
            at = us(ns)
            tid(th)
            if th in open_stack:
                cur, since, seen = open_stack[th]
                if ms - seen > GAP_MS:
                    close(th, 0, us(origin + seen * 1000000 + int(interval_ms * 1e6)))
                    cur = ()
            else:
                cur = ()
                open_stack[th] = ((), at, ms)
            n = 0
            while n < len(cur) and n < len(stack) and cur[n] == stack[n]:
                n += 1
            if n < len(cur):
                close(th, n, at)
            for fr in stack[n:]:
                ev.append({"ph": "B", "pid": 1, "tid": tids[th], "ts": at, "name": fr,
                           "cat": "wait" if fr == "(waiting)" or not running else "cpu"})
            open_stack[th] = (stack, at, ms)
    if gaps:
        interval_ms = float(gaps.most_common(1)[0][0])
    if last_ms is not None:
        for th in list(open_stack):
            close(th, 0, us(origin + last_ms * 1000000 + int(interval_ms * 1e6)))

    trace = os.path.join(d, "trace.json.gz")
    with gzip.open(trace, "wt", encoding="utf-8") as f:
        f.write('{"displayTimeUnit": "ms", "traceEvents": [\n')
        for i, e in enumerate(ev):
            f.write(("," if i else "") + json.dumps(e, separators=(",", ":")) + "\n")
        f.write("]}\n")

    # ---- speedscope: one sampled profile a thread, weights in ms
    frames_index, frame_list = {}, []

    def fi(name):
        if name not in frames_index:
            frames_index[name] = len(frame_list)
            frame_list.append({"name": name})
        return frames_index[name]

    profiles = []

    def profile(name, counter):
        items = [(s, c) for s, c in counter.most_common() if s]
        if not items:
            return
        total = sum(c for _, c in items) * interval_ms
        profiles.append({"type": "sampled", "name": name, "unit": "milliseconds", "startValue": 0, "endValue": total,
                         "samples": [[fi(x) for x in s] for s, _ in items], "weights": [c * interval_ms for _, c in items]})
    profile("every thread, running", every)
    for th in sorted(agg, key=lambda t: -sum(agg[t].values())):
        profile(th, agg[th])
    scope = os.path.join(d, "speedscope.json")
    with open(scope, "w", encoding="utf-8") as f:
        json.dump({"$schema": "https://www.speedscope.app/file-format-schema.json", "name": os.path.basename(d),
                   "activeProfileIndex": 0, "exporter": "scripts/perf.py report", "shared": {"frames": frame_list},
                   "profiles": profiles}, f, separators=(",", ":"))
    return trace, scope
