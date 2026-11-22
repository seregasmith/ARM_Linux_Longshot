"""The pipeline state lists, as nx2/pipeline_states.py reads them (whatever their format): counted, merged, compared, and
each step of a run valued by what it alone added (pipeline_tour.py merge, compare, value)."""
import importlib.util
import json
import os
import re
import subprocess
import sys

from tours import ROOT

def state_tool():
    spec = importlib.util.spec_from_file_location("pipeline_states", os.path.join(ROOT, "nx2", "pipeline_states.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def list_counts(tool, path):
    """(pipelines, programs, records) of a list, as nx2/pipeline_states.py reads and counts them (whatever its format)"""
    if not os.path.exists(path) or os.path.getsize(path) == 0:
        return 0, 0, set()
    try:
        recs = tool.read(path)
    except SystemExit:
        return 0, 0, set()
    m = re.search(r"of (\d+) programs", tool.describe(recs))
    return len(recs), int(m.group(1)) if m else 0, set(recs)


def merge(args):
    """The lists merged by nx2/pipeline_states.py (its format, whatever it is), and how much each added"""
    tool = state_tool()
    seen = set()
    for path in args.lists:
        n, progs, recs = list_counts(tool, path)
        print("%s: %d pipelines, %d programs, %d not in the lists before it" % (path, n, progs, len(recs - seen)))
        seen |= recs
    ran = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "nx2", "pipeline_states.py"), "--out", args.out] + args.lists)
    return ran.returncode


def list_diff(tool, new, old):
    """Two lists' records against each other (version 1 ones as version 2 where the other has those): {new, old: how many each
    has, lost: those in old not in new, gained: those in new not in old}"""
    (n, o), _ = tool.as_v2([tool.read(new), tool.read(old)])
    n, o = set(n), set(o)
    return {"new": len(n), "old": len(o), "lost": o - n, "gained": n - o}


def compare(args):
    """How many pipelines each of two lists has that the other lacks: a new tour's against an old one's"""
    tool = state_tool()
    d = list_diff(tool, args.new, args.old)
    print("new %s: %d pipelines; old %s: %d pipelines" % (args.new, d["new"], args.old, d["old"]))
    for what, recs in (("in the old, not in the new (lost)", d["lost"]), ("in the new, not in the old (gained)", d["gained"])):
        print("%s: %s" % (what, tool.describe(sorted(recs)) if recs else "none"))
    return 0


# ---- what each step is worth ----
# Steps the route needs whatever they add: without them no later step happens
NEEDED = ("new game:", "every item", "age switched")
NEEDED_END = (" / arrive", ", hearts not protected")   # (a stop's arrival with --sub; the Game Over's way there)


def run_steps(tool, run_dir, sub=False):
    """A run's steps: [(label, seconds, {records})], the list sliced at each step's count (the game only appends to it). sub:
    a step with marks (arrive, look, items) split at them, "<step> / <part>", the rest "<step> / settle" (seconds: the step's)"""
    with open(os.path.join(run_dir, "report.json"), encoding="utf-8") as f:
        rows = json.load(f)["stops"]
    path = os.path.join(run_dir, "states.bin")
    recs = tool.read(path) if os.path.exists(path) and os.path.getsize(path) else []
    (recs,), _ = tool.as_v2([recs], force=True)   # (version 1 runs compared with version 2 lists; those that do not convert left out)
    out, prev = [], 0
    for r in rows:
        n = min(r["pipelines"], len(recs))
        if sub and r.get("marks"):
            at = prev
            for what, m in r["marks"] + [("settle", n)]:
                m = max(at, min(m, n))
                out.append(("%s / %s" % (r["stop"], what), r.get("took", 0), set(recs[at:m])))
                at = m
        else:
            out.append((r["stop"], r.get("took", 0), set(recs[prev:n])))
        prev = max(prev, n)
    return out


def worth(runs, against=(), floor=10):
    """Each step's worth over runs: [{step, runs, seconds, met, beyond, only, kept}] in route order. met: the pipelines it met
    (over all its runs), beyond: those not in the `against` records (the players' lists), only: of those, the ones no other kept step
    met. Steps whose `only` is under `floor` are cut one at a time, the least worth a second first, the others then counted again
    (two steps that meet the same pipelines: one is kept); the steps the route needs (NEEDED) are kept whatever they add"""
    against = set(against)
    met, secs, order = {}, {}, []
    for steps in runs:
        for label, took, recs in steps:
            if label not in met:
                met[label], secs[label] = set(), []
                order.append(label)
            met[label] |= recs
            secs[label].append(took)
    beyond = {k: v - against for k, v in met.items()}
    kept = set(order)
    holders = {}   # (a pipeline: how many kept steps met it)
    for k in order:
        for r in beyond[k]:
            holders[r] = holders.get(r, 0) + 1
    only = lambda k: sum(1 for r in beyond[k] if holders[r] == (1 if k in kept else 0))
    median = lambda k: sorted(secs[k])[len(secs[k]) // 2]
    while True:
        weak = [(only(k) / max(1.0, median(k)), -median(k), k) for k in kept if not k.startswith(NEEDED) and not k.endswith(NEEDED_END)
                and only(k) < floor]
        if not weak:
            break
        k = min(weak)[2]   # (the least a second; of equals, the slowest)
        kept.discard(k)
        for r in beyond[k]:
            holders[r] -= 1
    return [{"step": k, "runs": len(secs[k]), "seconds": median(k), "met": len(met[k]), "beyond": len(beyond[k]), "only": only(k),
             "kept": k in kept} for k in order]


def value(args):
    """The steps of runs, each with what it added that the players' lists (the shipped list's contrib/ sources) and the other
    steps do not have, and whether it is worth keeping"""
    tool = state_tool()
    against = []
    if not args.no_contrib:
        for src in tool.sources(tool.sources_dir()):
            if os.path.basename(src) != tool.TOUR:
                against += tool.read(src)
    for path in args.against:
        against += tool.read(path)
    (against,), _ = tool.as_v2([against], force=True)
    runs = [run_steps(tool, d, args.sub) for d in args.runs]
    rows = worth(runs, against, args.floor)
    sources = (["players' lists"] if not args.no_contrib else []) + list(args.against)
    print("%d runs; against %d pipelines (%s)" % (len(runs), len(set(against)), ", ".join(sources) or "none"))
    print("%6s %6s %6s %6s %4s  %-5s %s" % ("met", "beyond", "only", "secs", "runs", "", "step"))
    for r in rows:
        print("%6d %6d %6d %6.0f %4d  %-5s %s" % (r["met"], r["beyond"], r["only"], r["seconds"], r["runs"],
                                                 "keep" if r["kept"] else "CUT", r["step"]))
    cut = [r for r in rows if not r["kept"]]
    print("cut %d of %d steps: %.0f s of %.0f (median seconds a run)" % (len(cut), len(rows), sum(r["seconds"] for r in cut),
                                                                        sum(r["seconds"] for r in rows)))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(rows, f, indent=1)
    return 0
