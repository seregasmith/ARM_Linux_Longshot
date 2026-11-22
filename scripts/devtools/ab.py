"""`dev.py ab`: measure a switch or this checkout's change against main in the game, in one command
(docs/dev/developing.md, *Measure a change*; docs/dev/standards.md#measurements). Python's standard library only.

    dev.py ab --env NAME=VALUE [...]            a switch, set against unset, on one runtime (no build)
    dev.py ab --a NAME=VALUE --b NAME=VALUE     a switch, two values
    dev.py ab                                   this checkout's committed change (its pack) against its base (main)
       [--scene NAME | --save FOLDER [--slot NN] | --route ROUTE [--sync N]] [--seconds 60] [--runs 3] [--warmup 10]
       [--options FILE] [--dynamic]
       [--set NAME=VALUE] [--framegen] [--vsync] [--headless] [--runtime DIR] [--separate] [--allow KEY] [--dirty]
       [--threads] [--dry-run] [--scenes]

What it does:
  - builds (a change): `dev.py base` (only when missing), `dev.py pack` (the pack layer builds only what changed), and the
    two runtimes (rt-<checkout>, rt-base), each refreshed in place only when it no longer matches its base and pack;
  - each game run is one job of the PC's queue in timing mode (the control bridge's supervisor started through
    `dev.py queue run --mode timing`), started once, from a save in a user folder of the run's own, the title skipped by
    the game's Skip title screen row, the scene reached with the Debug tab's teleport and age switch, then a warm-up
    until no frame waits for a pipeline or a shader, then --seconds measured from the frame trace (RX_VK_FRAME_TRACE);
  - --route ROUTE (assets/routes/, scripts/devtools/routes.py): the route's repository save instead of a scene, and after
    the warm-up the route played over the control bridge (its `play`, put back at every --sync-th update, 1 by default):
    the measured stretch is the route, the same moving input on both sides; a run whose replay diverged (Link past 1 m
    from the route) or missed one of its loads is refused;
  - runs interleaved A, B, A, B, every one from a copy of one pinned options file (cap off: HOS_FPS_MAX=1000; V-Sync
    and frame generation off unless asked; the Render resolution at 67 % where the file leaves it at Auto, the game's
    dynamic resolution, unless --dynamic), and refused when the runs differ in what ran (the menu's settings, the
    switches set, the renderer, the window) in anything but the compared thing (a compared switch's own menu row, as
    gpu/vk/switches.def names its options-file key, counts as the compared thing; --allow KEY names another);
  - a switch whose reader takes HOS_AB (host/hos_ab.c: a hos_ab_value("NAME", ...) call in host/ or gpu/) and whose
    values are whole numbers is flipped within each run instead (scripts/perf.py ab's reader), unless --separate;
  - --threads is temporarily unavailable until CPU samples can be matched safely to the measured frames;
  - writes plan.json, results.json and measurements.md (Verdict, Setup, the table, the Spread line, the log lines) to
    the development folder's runs/ab/<time>-<what>/, and prints where the job's time went (each run's phases are in
    results.json), then the section. Exit status: 0, 1 a run failed, 3 the sides differed.
"""
import argparse
import csv
import json
import os
import re
import shutil
import statistics
import sys
import time

from . import build, common, packs, routes, vcs

WHO = "dev.py ab"
# (the scenes: a repository save (assets/saves) to start from, then the Debug tab's teleport to a destination of
# gpu/vk/vk_menu.cpp's teleport_names (None: stay where the save loads), Link's age, the hour (None: the save's))
SCENES = {
    "kokiri-house": ("kokiri-start", None, "child", None, "Link's house, as the repository save loads (indoors)"),
    "kokiri-forest": ("kokiri-start", "Kokiri Forest", "child", None, "Kokiri Forest, child"),
    "hyrule-field": ("kokiri-start", "Hyrule Field", "child", None, "Hyrule Field, child (open, far view)"),
    "hyrule-field-adult": ("kokiri-start", "Hyrule Field", "adult", None, "Hyrule Field, adult"),
    "hyrule-field-night": ("kokiri-start", "Hyrule Field", "child", 22, "Hyrule Field at 22:00, child"),
    "market": ("kokiri-start", "Market", "child", None, "Market, child (crowded)"),
    "kakariko": ("kokiri-start", "Kakariko Village", "child", None, "Kakariko Village, child"),
    "lake-hylia": ("kokiri-start", "Lake Hylia", "adult", None, "Lake Hylia, adult (water)"),
    "deku-tree": ("kokiri-start", "Inside the Deku Tree", "child", None, "Inside the Deku Tree, child (a dungeon)"),
    "fire-temple": ("kokiri-start", "Fire Temple", "adult", None, "Fire Temple, adult (heat haze)"),
}
DEFAULT_SCENE = "hyrule-field"
# (what every run sets alike: the cap off, the frame trace the numbers come from, sound at volume 0, no Discord thread,
# the window behind the one in front)
COMMON_ENV = {"HOS_FPS_MAX": "1000", "RX_VK_FRAME_TRACE": "1", "HOS_AUDIO_MUTE": "1", "HOS_DISCORD": "0",
              "HOS_WINDOW_NOACTIVATE": "1"}
# (the pinned options file's rows set for every run: the title skipped (host/oot_autoload.c), V-Sync Off, frame generation Off)
SKIP_TITLE, VSYNC_OFF, FRAMEGEN_OFF = ("skip_title_screen", 1), ("vsync", 2), ("frame_generation", 0)
# (and the Render resolution at 67 %, DLSS's and FSR's Quality share (gpu/vk/upscaler_rows.h kPercent[3]), where the options
# file leaves it at Auto: the game's dynamic resolution sizes the 3D to the GPU's load, so a change that saves GPU time comes
# out as a larger picture, not as frames, and each run settles at a size of its own. One switch, Hyrule Field at 4K on an
# RTX 3090, came out +6.0 % and then -6.5 % at Auto, every run of one side past every run of the other both times; at 67 %
# it was +1.1 %, each side's three runs within 1 %. --dynamic keeps Auto)
FIXED_RESOLUTION = ("resolution", 3)
# (a frame is still warming up while one of these frame-trace columns is over its limit: pipelines waited for or made,
# shaders translated)
COLD = {"pipe_waits": 0, "pipelines_ms": 1.0, "translate_ms": 1.0}
QUIET_S, WARM_MAX_S = 5, 120
AB_PERIOD_S = 3
NAME = re.compile(r"[A-Z][A-Z0-9_]*")
WIRED = re.compile(r'hos_ab_value\("([A-Z0-9_]+)"')
INT = re.compile(r"-?\d+")
AB_LINE = re.compile(r"\bab: ")   # (host/hos_ab.c's lines)
SLOW_MS = 50.0   # (a frame this long or longer is a hitch a player sees)
SWITCHES_LINE = re.compile(r"vk: switches set(?: \(\d+\))?: (.*)$")
# (the settings a Setup line names, by their options-file keys)
SETUP_KEYS = ("quality_preset", "upscaler", "resolution", "output_res", "frame_generation", "vsync")
RUN_ONLY = re.compile(r"LONGSHOT_BUILD_NAME$|LONGSHOT_JOB")   # (set by the queue and the build for each run: never compared)
# (a switch's menu row, as its switches.def row names its options-file key: "the options file's fullscreen=",
# "(frame_rate_cap in the options file)")
MENU_KEY = re.compile(r"options file's (\w+)=|\((\w+) in the options file\)")
TRACE = os.path.join("logs", "longshot_frames.csv")   # (a runtime's frame trace: gpu/vk/vk_paths.h log_path)


def die(text):
    common.die(WHO, text)


def say(text):
    common.say(WHO, text)


# ---- the plan ----

def assignments(items, what):
    """["NAME=VALUE", ...] as {NAME: VALUE}; a bad one stops"""
    out = {}
    for kv in items or ():
        k, eq, v = kv.partition("=")
        if not eq or not NAME.fullmatch(k):
            die("%s %r: NAME=VALUE, NAME in capitals" % (what, kv))
        out[k] = v
    return out


def differing(a, b):
    """The names set differently on the two sides (unset counts as a value)"""
    return sorted(k for k in set(a) | set(b) if a.get(k) != b.get(k))


def wired_switches(root=common.ROOT):
    """The switches HOS_AB can flip within a run: those whose reader calls hos_ab_value("NAME", ...) (host/hos_ab.c)"""
    names = set()
    for top in ("host", "gpu"):
        for d, _, files in os.walk(os.path.join(root, top)):
            for f in files:
                if f.endswith((".c", ".cpp", ".h")):
                    with open(os.path.join(d, f), encoding="utf-8", errors="replace") as fh:
                        names.update(WIRED.findall(fh.read()))
    return names


def in_run(a, b, wired):
    """(True, name) when HOS_AB can measure the two sides within one run, else (False, why)"""
    names = differing(a, b)
    if len(names) != 1:
        return False, "%d switches differ (HOS_AB flips one)" % len(names)
    n = names[0]
    if n not in wired:
        return False, "%s is read once, not through hos_ab_value: separate runs" % n
    if n not in b or not INT.fullmatch(b[n]) or (n in a and not INT.fullmatch(a[n])):
        return False, "HOS_AB takes whole numbers: separate runs"
    return True, n


def order(runs, how):
    """The runs in the order they go: interleaved (A, 1), (B, 1), (A, 2) ..., or (AB, k) within one run"""
    if how == "in-run":
        return [("AB", k) for k in range(1, runs + 1)]
    return [(side, k) for k in range(1, runs + 1) for side in ("A", "B")]


def scene_of(name, save=None, slot=None):
    """The scene as a dict: save (a repository save's name) or folder (--save), destination, age, hour, about"""
    if name not in SCENES:
        die("no scene %r: %s (--scenes lists them)" % (name, ", ".join(SCENES)))
    repo, to, age, hour, about = SCENES[name]
    s = {"name": name, "save": None if save else repo, "folder": os.path.abspath(save) if save else None, "slot": slot,
         "to": to, "age": age, "hour": hour, "about": about}
    if save:
        s["about"] += " (from the save %s)" % os.path.basename(s["folder"].rstrip("/\\"))
    return s


def destination(scene, actions):
    """(index, map) of the scene's teleport in the Debug tab's list (tours.route.actions: read from the source); None
    when the scene stays where the save loads"""
    if not scene["to"]:
        return None
    labels = [label for label, _ in actions["teleports"]]
    if scene["to"] not in labels:
        die("the Debug tab has no destination %r (gpu/vk/vk_menu.cpp teleport_names): fix SCENES" % scene["to"])
    i = labels.index(scene["to"])
    banc = actions["bancs"][i][0 if scene["age"] == "child" else 1]
    if not banc:
        die("%s has no map for a%s %s" % (scene["to"], "n" if scene["age"] == "adult" else "", scene["age"]))
    return i, banc


def short(commit):
    return commit[:8] + (" (uncommitted files)" if commit.endswith("+uncommitted") else "")


def menu_rows(names, root=common.ROOT):
    """The options-file keys of the menu rows the switches names override, as gpu/vk/switches.def says: a run with the
    switch set shows that row changed, which is the compared thing, not a difference"""
    keys = set()
    with open(os.path.join(root, "gpu", "vk", "switches.def"), encoding="utf-8") as f:
        for line in f:
            m = re.match(r"SW\(([A-Z0-9_]+),", line)
            if m and m.group(1) in names:
                keys.update(k for found in MENU_KEY.findall(line) for k in found if k)
    return keys


def make_plan(a_env, b_env, change, scene, seconds, runs, warmup, common_env, how, why, hashes, runtimes, folder, allow=()):
    """Everything a run needs, as JSON. hashes and runtimes: {A: ..., B: ...}; allow: settings or switches the runs may
    differ in"""
    if change:
        labels = {"A": "main @ %s" % short(hashes["A"]), "B": "this PR @ %s" % short(hashes["B"])}
    else:
        labels = {s: ", ".join("%s=%s" % kv for kv in sorted(e.items())) or "%s unset" % "/".join(differing(a_env, b_env))
                  for s, e in (("A", a_env), ("B", b_env))}
    plan = {"what": "change" if change else "switch", "how": how, "why": why, "compared": differing(a_env, b_env),
            "sides": {s: {"label": labels[s], "env": e, "runtime": runtimes[s], "commit": hashes.get(s)}
                      for s, e in (("A", a_env), ("B", b_env))},
            "common_env": common_env, "scene": scene, "seconds": seconds, "runs": runs, "warmup": warmup,
            "order": order(runs, how), "folder": folder,
            "allow": sorted(set(allow) | menu_rows(differing(a_env, b_env)))}
    if how == "in-run":
        n = plan["compared"][0]
        plan["sides"]["AB"] = {"label": "%s flipped in the run" % n, "runtime": runtimes["A"], "commit": hashes.get("A"),
                               "env": dict(a_env, HOS_AB="%s=%s,%d,0" % (n, b_env[n], AB_PERIOD_S))}
    return plan


def plan_text(plan):
    lines = ["%s A/B, %s%s" % (plan["what"], plan["how"], " (%s)" % plan["why"] if plan["why"] else ""),
             "scene: %s: %s; %s" % (plan["scene"]["name"], plan["scene"]["about"], "teleport %d to %s" % tuple(
                 plan["scene"]["dest"]) if plan["scene"].get("dest") else "where the save loads")]
    for s in ("A", "B", "AB"):
        if s in plan["sides"]:
            side = plan["sides"][s]
            lines.append("%-2s %s: %s, env %s" % (s, side["label"], side["runtime"], side["env"] or "{}"))
    lines.append("every run: %s, %d s measured after a warm-up of at least %d s (until no frame waits for a pipeline)"
                 % (" ".join("%s=%s" % kv for kv in sorted(plan["common_env"].items())), plan["seconds"], plan["warmup"]))
    lines.append("order (each a timing job of the queue): " + ", ".join("%s%d" % r for r in plan["order"]))
    lines.append("folder: " + plan["folder"])
    return "\n".join(lines)


# ---- the frame trace ----

def read_trace(path):
    """The frame trace's whole rows, as dicts (a line the game is still writing left out)"""
    try:
        with open(path, newline="", encoding="utf-8", errors="replace") as f:
            return [r for r in csv.DictReader(f) if r.get("ab_side") not in (None, "") and r.get("at_ns")]
    except OSError:
        return []


def num(row, k):
    try:
        return float(row.get(k) or 0)
    except ValueError:
        return 0.0


def cold(row):
    return any(num(row, k) > limit for k, limit in COLD.items())


def settled(rows, quiet_s=QUIET_S):
    """The last quiet_s seconds of the trace hold no cold frame (and the trace is that long)"""
    if not rows:
        return False
    end = num(rows[-1], "at_ns")
    if end - num(rows[0], "at_ns") < quiet_s * 1e9:
        return False
    return not any(cold(r) for r in rows if num(r, "at_ns") >= end - quiet_s * 1e9)


def window(rows, start_ns, seconds):
    """The rows of the measured stretch: after start_ns, up to seconds later"""
    return [r for r in rows if start_ns < num(r, "at_ns") <= start_ns + seconds * 1e9]


def frame_stats(ms):
    """fps (1000 / the mean frame time), mean, median and p99 frame time, the 1 % low (1000 / p99), and how many frames
    took SLOW_MS or longer"""
    from profiling import ab as abtrace
    s = sorted(ms)
    mean = sum(s) / len(s)
    p99 = abtrace.percentile(s, 99)
    return {"frames": len(s), "fps": 1000.0 / mean, "mean_ms": mean, "median_ms": abtrace.percentile(s, 50),
            "p99_ms": p99, "low1_fps": 1000.0 / p99 if p99 else 0.0, "n50": sum(1 for x in s if x >= SLOW_MS)}


def in_run_stats(rows, name):
    """perf.py ab's summary (scripts/profiling/ab.py) of the measured rows of a HOS_AB run"""
    from profiling import ab as abtrace
    frames = [(num(r, "ms"), int(num(r, "ab_side"))) for r in rows]
    left_out = [sum(1 for _, v in frames if v in abtrace.TRANSITION),
                sum(1 for _, v in frames if v not in abtrace.STEADY + abtrace.TRANSITION)]
    periods, dropped = abtrace.trim(abtrace.periods_of(frames))
    return abtrace.summary(name, periods, dropped, left_out, 2000, 1), periods, dropped, left_out


# ---- what ran ----

def log_facts(text):
    """From a game log: the version and GPU (profiling/folder.py build_of), the switches set, the HOS_AB lines"""
    from profiling import folder
    lines = text.splitlines()
    version, gpu = folder.build_of(lines[:50])
    switches, line = {}, None
    for l in lines:
        m = SWITCHES_LINE.search(l)
        if m:
            line = l.strip()
            for part in [] if m.group(1) == "none" else m.group(1).split(", "):
                k, _, v = part.partition("=")
                switches[k.strip()] = v.rsplit(" (", 1)[0]
            break
    return {"version": version, "gpu": gpu, "switches": switches, "switches_line": line,
            "ab_lines": [l.strip() for l in lines if AB_LINE.search(l)]}


def what_ran(status, options, facts, skip=()):
    """What a run is compared on: the renderer, the window, every menu row's text, the switches set (less skip)"""
    return {"renderer": status.get("renderer"), "window": status.get("window"),
            "options": {r["key"]: r.get("text", r.get("value")) for r in options if r.get("key") not in skip},
            "switches": {k: v for k, v in facts["switches"].items() if k not in skip and not RUN_ONLY.match(k)}}


def differences(ran, version_counts=False):
    """[text] of what differs between the runs ({(side, k): what_ran(...) plus version}), each against the first; [] when
    they agree"""
    keys = sorted(ran)
    out = []
    for key in keys[1:]:
        r, ref = ran[key], ran[keys[0]]
        pairs = [(part, r.get(part), ref.get(part)) for part in ("renderer", "window") + (("version",) if version_counts else ())]
        pairs += [("%s %s" % (part, k), r[part].get(k), ref[part].get(k))
                  for part in ("options", "switches") for k in sorted(set(r[part]) | set(ref[part]))]
        out += ["%s: %s%d has %r, %s%d %r" % (what, key[0], key[1], mine, keys[0][0], keys[0][1], theirs)
                for what, mine, theirs in pairs if mine != theirs]
    return out


# ---- the verdict and the write-up ----

def verdict(a_fps, b_fps):
    """("gain" | "loss" | "within spread" | "too few runs", text) of B's runs against A's, by fps: a gain or a loss only
    when the runs do not overlap, and never from one run a side, which has no spread to compare with"""
    ma, mb = statistics.median(a_fps), statistics.median(b_fps)
    pct = 100 * (mb / ma - 1)
    if len(a_fps) < 2 or len(b_fps) < 2:
        return "too few runs", ("one run a side cannot tell a difference from noise: %+.1f%% fps (%.1f against %.1f); "
                                "--runs 2 or more" % (pct, mb, ma))
    if min(b_fps) > max(a_fps):
        return "gain", "faster: %+.1f%% fps (median %.1f against %.1f), every B run above every A run" % (pct, mb, ma)
    if max(b_fps) < min(a_fps):
        return "loss", "slower: %+.1f%% fps (median %.1f against %.1f), every B run below every A run" % (pct, mb, ma)
    return "within spread", ("no difference shown: %+.1f%% fps (median %.1f against %.1f), within the runs' spread "
                             "(A %.1f to %.1f, B %.1f to %.1f)" % (pct, mb, ma, min(a_fps), max(a_fps), min(b_fps), max(b_fps)))


def spread_line(results):
    """The observed fps ranges, not a prediction of detection probability."""
    ok = [r for r in results["runs"] if not r.get("error") and r.get("stats")]
    fps = {s: [r["stats"]["fps"] for r in ok if r["side"] == s] for s in ("A", "B")}
    if min(len(fps["A"]), len(fps["B"])) < 2:
        return "**Spread:** fewer than 2 runs on a side; variability cannot be estimated."
    return ("**Spread:** observed fps ranges: A %.1f to %.1f; B %.1f to %.1f. "
            "These ranges do not predict what another run will show." %
            (min(fps["A"]), max(fps["A"]), min(fps["B"]), max(fps["B"])))


def verdict_in_run(pooled):
    """The verdict of a HOS_AB summary: its 95 % interval of side 1's mean frame time less side 0's"""
    d = pooled.get("diff")
    if not d or "ci_pct" not in d:
        return "within spread", "no difference shown: too few periods for an interval"
    lo, hi = d["ci_pct"]
    text = "%+.2f%% fps, the mean frame time %+.3f ms (95%% interval %+.2f%% to %+.2f%%)" % (d["fps_pct"], d["mean_ms"], lo, hi)
    if hi < 0:
        return "gain", "faster: " + text
    if lo > 0:
        return "loss", "slower: " + text
    return "within spread", "no difference shown: " + text + ", the interval holds 0"


def cells(values, fmt):
    return " / ".join(fmt % v for v in values)


def table(plan, results):
    """The Measurements table: a row a number, a column a side, Δ of the medians"""
    a, b = plan["sides"]["A"], plan["sides"]["B"]
    if plan["what"] == "change":
        head = "| | Before: %s | After: %s | Δ |" % (a["label"], b["label"])
    else:   # (one build both sides: the switch named before the colon, the build after it, for check_pr_description.py)
        built = "this PR @ %s" % version_hash(results)
        head = "| | Before, %s: %s | After, %s: %s | Δ |" % (a["label"], built, b["label"], built)
    head += "\n|---|---|---|---|"
    rows = [head]
    if plan["how"] == "in-run":
        s0, s1 = results["pooled"]["sides"]["0"], results["pooled"]["sides"]["1"]
        d = results["pooled"].get("diff", {})
        for label, k, fmt in (("fps", "fps", "%.1f"), ("mean frame time", "mean_ms", "%.2f ms"),
                              ("median frame time", "median_ms", "%.2f ms"), ("p99 frame time", "p99_ms", "%.2f ms")):
            if k == "fps" and d.get("fps_pct") is not None:
                delta = "%+.2f%%" % d["fps_pct"]
            elif s0.get(k) is not None and s1.get(k) is not None:
                delta = "%+.2f ms" % (s1[k] - s0[k])
            else:
                delta = ""
            rows.append("| %s | %s | %s | %s |" % (label, fmt % s0.get(k, 0), fmt % s1.get(k, 0), delta))
        periods = [p for r in results.get("runs", []) for p in r.get("periods", [])]
        rates = []
        for side in (0, 1):
            ms = [v for p in periods if p["side"] == side for v in p["ms"]]
            rates.append(60000 * sum(v >= SLOW_MS for v in ms) / sum(ms) if ms and sum(ms) else None)
        if all(v is not None for v in rates):
            rows.append("| frames of 50 ms or more per minute | %.2f | %.2f | %+.2f |" %
                        (rates[0], rates[1], rates[1] - rates[0]))
        else:
            rows.append("| frames of 50 ms or more per minute | unavailable | unavailable | |")
        return "\n".join(rows)
    by = {s: [r["stats"] for r in results["runs"] if r["side"] == s and r.get("stats")] for s in ("A", "B")}
    for label, k, fmt, kind in (("fps (each run)", "fps", "%.1f", "pct"), ("1% low fps (1000 / p99)", "low1_fps", "%.1f", "pct"),
                                ("median frame time", "median_ms", "%.2f ms", "ms"), ("p99 frame time", "p99_ms", "%.2f ms", "ms"),
                                ("frames of 50 ms or more (median)", "n50", "%g", "count")):
        va, vb = [s[k] for s in by["A"]], [s[k] for s in by["B"]]
        ma, mb = statistics.median(va), statistics.median(vb)
        if kind == "pct":
            delta = "%+.1f%%" % (100 * (mb / ma - 1))
        else:
            delta = ("%+.2f ms" if kind == "ms" else "%+g") % (mb - ma)
        if k == "fps":
            rows.append("| %s | %s (median %.1f) | %s (median %.1f) | %s |" % (label, cells(va, fmt), ma, cells(vb, fmt), mb, delta))
        else:
            rows.append("| %s | %s | %s | %s |" % (label, fmt % ma, fmt % mb, delta))
    return "\n".join(rows)


def window_text(runs, ran):
    """The effective display modes recorded for the runs."""
    modes = []
    for r in runs:
        w = (r.get("ran") or {}).get("window")
        mode = "headless" if r.get("headless") else ("window %sx%s" % (w.get("width"), w.get("height")) if w else "window size unavailable")
        if mode not in modes:
            modes.append(mode)
    if modes:
        return ", ".join(modes)
    w = ran.get("window")
    return "window %sx%s" % (w.get("width"), w.get("height")) if w else "headless"


def setup_line(plan, results, system=None):
    ran = results.get("ran") or {}
    opts = ran.get("options", {})
    settings = ", ".join("%s %s" % (k.replace("_", " "), opts[k]) for k in SETUP_KEYS if k in opts)
    return ("**Setup:** %s, %s; %s (%s); %s; uncapped (HOS_FPS_MAX=1000)%s; the frame trace, %d s each after a warm-up "
            "until no frame waited for a pipeline, %s; `dev.py ab`" % (
                system or results.get("system", "?"), results.get("gpu") or "?", plan["scene"]["about"],
                window_text(results.get("runs", []), ran),
                settings or "the options file's settings", ", frame generation off" if "frame_generation" not in opts else "",
                plan["seconds"], "%d runs, the switch flipped every %d s within each (HOS_AB)" % (plan["runs"], AB_PERIOD_S)
                if plan["how"] == "in-run" else "%d interleaved pairs, the median" % plan["runs"]))


def version_hash(results):
    """The commit of the build a switch A/B ran: the -g<hash> of the game's version, else the version"""
    version = (results.get("ran") or {}).get("version") or "?"
    m = re.search(r"-g([0-9a-f]{7,40})\b", version)
    return m.group(1)[:8] if m else version


def measurements(plan, results):
    """The pull request's Measurements section (docs/dev/standards.md#measurements); scripts/check_pr_description.py
    measurement_problems passes it"""
    out = ["**Verdict:** %s." % results["verdict"][1], "", setup_line(plan, results), ""]
    if plan["what"] == "switch":
        out += ["**Baseline:** both columns are one build, %s (not main): the switch set against unset on it." %
                ((results.get("ran") or {}).get("version") or "?"), ""]
    out += [table(plan, results), ""]
    if plan["how"] != "in-run":
        out += [spread_line(results), ""]
    out.append("```")
    out += [r["line"] for r in results["runs"]]
    out += sorted({l for r in results["runs"] for l in r.get("log_lines", [])})
    out.append("```")
    return "\n".join(out) + "\n"


def run_line(r):
    if r.get("error"):
        return "%s%d: FAILED: %s" % (r["side"], r["k"], r["error"])
    if r.get("ab"):
        d = r["ab"].get("diff") or {}
        return "%s%d: %d frames, side 0 %.1f fps, side 1 %.1f fps, %+.2f%%%s" % (
            r["side"], r["k"], sum(s.get("frames", 0) for s in r["ab"]["sides"].values()),
            r["ab"]["sides"]["0"].get("fps", 0), r["ab"]["sides"]["1"].get("fps", 0), d.get("fps_pct", 0),
            "" if r["warm"] else " (the warm-up never settled)")
    s = r["stats"]
    return "%s%d: %d frames in %d s, %.1f fps, median %.2f ms, p99 %.2f ms, 1%% low %.1f fps, %d cold frames%s" % (
        r["side"], r["k"], s["frames"], r["seconds"], s["fps"], s["median_ms"], s["p99_ms"], s["low1_fps"], r["cold"],
        "" if r["warm"] else " (the warm-up never settled)")


# ---- runtimes and commits ----

def runtime_current(d, rt, pack):
    """The runtime rt is what `dev.py packs rt d rt [pack]` would make now: the same base, the same pack, the same modules
    and generated files (pack.json); a stale one is made again"""
    try:
        with open(os.path.join(rt, "dev_packs.json"), encoding="utf-8") as f:
            made = json.load(f)
        b = packs.read_base(d)
        meta = {"files": {}, "generated": [], "deleted": []}
        if pack:
            with open(os.path.join(d, "packs", pack, "pack.json"), encoding="utf-8") as f:
                meta = json.load(f)
    except (OSError, ValueError):
        return False
    if made.get("base") != b["commit"] or made.get("packs") != ([pack] if pack else []):
        return False
    for m in packs.MODULES:   # (the pack's module, else the base's: base.json's hash of it)
        want = meta["files"].get(m[0]) or b.get("modules", {}).get(m[0])
        if not want or made.get("modules", {}).get(m[0]) != want:
            return False
    return all(os.path.isfile(os.path.join(rt, rel)) and packs.same_file(os.path.join(d, "packs", pack, "files", rel),
                                                                         os.path.join(rt, rel))
               for rel in meta["generated"]) and not any(os.path.lexists(os.path.join(rt, rel)) for rel in meta["deleted"])


def fresh_runtime(empty):
    """build-linux/rt-<checkout> (or rt-base), refreshed in place when stale (build.cmd_rt, `packs rt`): its caches stay, the
    pipelines and programs a run built, stamped, and the renderer leaves out a stale one"""
    d, pack = build.base_for(), None if empty else build.pack_name()
    rt = os.path.join(build.own_dir(), "rt-" + (pack or "base"))
    if runtime_current(d, rt, pack):
        say("%s is current: kept, warm" % rt)
        return rt
    build.cmd_rt(argparse.Namespace(empty=empty))
    return rt


def checkout_commit(dirty_ok):
    """This checkout's commit, as the After column names it; uncommitted files stop it (a pack counts them, so the hash
    would not name what ran) unless dirty_ok"""
    commit, _ = vcs.commit(common.ROOT)
    dirty = not vcs.clean(common.ROOT)
    if dirty and not dirty_ok:
        die("uncommitted changes: commit them first, so the After column's hash names what ran (--dirty: measure anyway)")
    return commit + ("+uncommitted" if dirty else "")


def default_runtime():
    for name in ("rt-" + build.pack_name(), "rt-base"):
        rt = os.path.join(build.own_dir(), name)
        if os.path.isdir(rt):
            return rt
    return os.path.join(build.own_dir(), "rt-" + build.pack_name())


# ---- a run ----

def dynamic_resolution(path):
    """Whether the options file at path (None: no file) leaves the Render resolution at Auto, the game's dynamic resolution:
    no resolution= line, or 0"""
    if not path:
        return True
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            key, eq, value = line.strip().partition("=")
            if eq and key.strip() == FIXED_RESOLUTION[0]:
                return value.strip() in ("", "0")
    return True


def pin_options(src, path, framegen, vsync, dynamic=False):
    """The options file every run starts from a copy of: src's (or none: every row at its default) with the title skipped,
    V-Sync and frame generation off unless asked, and the Render resolution fixed where it was Auto unless dynamic is asked"""
    from pipeline_tour import set_ini
    if src:
        shutil.copyfile(src, path)
    fixed = [] if dynamic or not dynamic_resolution(src) else [FIXED_RESOLUTION]
    set_ini(path, [SKIP_TITLE] + ([] if vsync else [VSYNC_OFF]) + ([] if framegen else [FRAMEGEN_OFF]) + fixed)


def new_clock():
    return {"_at": time.monotonic()}


def lap(clock, phase):
    """Adds the time since the last lap to phase, in seconds; lap(clock, None) ends the clock and returns the phases"""
    now = time.monotonic()
    if phase is None:
        clock.pop("_at", None)
        return clock
    clock[phase] = round(clock.get(phase, 0.0) + now - clock["_at"], 1)
    clock["_at"] = now
    return clock


TIMING_ORDER = ("start", "title and load", "teleport", "warm-up", "measured", "stop", "copy and analysis", "cleanup")


def timing_summary(runs, job_s, before_runs_s=0, batch_s=0):
    """Where the job's time went: its length and each phase's median over the runs (a failed one's too)"""
    phases = {}
    for r in runs:
        for k, v in (r.get("timing") or {}).items():
            phases.setdefault(k, []).append(v)
    return {"job_s": round(job_s, 1), "before_runs_s": round(before_runs_s, 1), "batch_s": round(batch_s, 1), "runs": len(runs),
            "median_s": {k: statistics.median(phases[k]) for k in TIMING_ORDER if k in phases}}


def timing_text(t):
    """The time through the batch and summary, including preparation, and the runs' phase medians."""
    return "the job took %.1f min through analysis (%d runs; preparation %.1f min, run batch %.1f min); a run's median: %s" % (
        t["job_s"] / 60, t["runs"], t["before_runs_s"] / 60, t["batch_s"] / 60,
        ", ".join("%s %.0f s" % kv for kv in t["median_s"].items()))


def run_one(plan, side, k, pinned):
    """One game run: queued in timing mode, started once, driven to the scene, warmed up, measured, stopped"""
    import longshot_bridge
    from tours import drive, route
    s, scene = plan["sides"][side], plan["scene"]
    d = os.path.join(plan["folder"], "%s%d" % (side, k))
    os.makedirs(d)
    trace = os.path.join(s["runtime"], TRACE)
    if os.path.exists(trace):
        os.remove(trace)
    opts, no_env, live = os.path.join(d, "options.ini"), os.path.join(d, "empty-env.txt"), os.path.join(d, "live.txt")
    shutil.copyfile(pinned, opts)
    open(no_env, "w").close()
    env = dict(plan["common_env"])
    env.update(s["env"])
    env.update(HOS_OPTIONS_FILE=opts, HOS_ENV_FILE=no_env)
    if scene["hour"] is not None:   # (the Debug tab's clock: a live input line, with the Debug tab's tools on)
        open(live, "w").close()
        env.update(HOS_INPUT_LIVE=live, HOS_OOT_CHEATS="1")
    desc = "%s: %s · %s" % (side, s["label"], scene["name"])
    wrapper = [sys.executable, "-I", os.path.join(common.SCRIPTS, "dev.py"), "queue", "run", "ab-%s%d" % (side, k),
               "--mode", "timing", "--desc", desc, "--"]
    r = {"side": side, "k": k, "folder": d, "seconds": plan["seconds"], "warm": False,
         "headless": env.get("HOS_HEADLESS", "").strip(" \t\r\n").lower() not in ("", "0", "false", "off", "no")}
    session = None
    clock = new_clock()   # (where the run's time goes: results.json, and the line after the runs)
    phase = "start"
    try:
        say("%s%d: queued (timing); %s" % (side, k, desc))
        session = longshot_bridge.start_session(
            s["runtime"], romfs=packs.runtime_romfs(s["runtime"]),   # (a --romfs-in-place base's runtime has none)
            run=os.path.join(d, "game"), user=os.path.join(d, "user"), save=scene["folder"], slot=scene["slot"],
            repo_save=scene["save"], env=env, ready="present", timeout=1800, wrapper=wrapper)
        lap(clock, "start")   # (the queue's wait, the boot, to the first frame)
        phase = "title and load"
        g = drive.Game(session.connect(30), d, "", live=live)
        end = time.monotonic() + 300   # (the title skipped by the game: its presses start 20 s after the first controller read)
        while True:
            st = g.status()
            if st["map"] and st["fresh"] and st["event"] is False and st["loading"] is False:
                break
            if time.monotonic() > end:
                raise drive.Failed("not in play 300 s after the first frame (is Skip title screen on? last state %s)" % st)
            time.sleep(1)
        lap(clock, "title and load")
        phase = "teleport"
        if scene["age"] and st["age"] != scene["age"] and not scene.get("route"):
            drive.switch_age(g, scene["age"])
        if scene["dest"]:
            drive.teleport(g, scene["dest"][0], scene["dest"][1], scene["age"])
        if scene["hour"] is not None:
            g.cheat(route.TIME_ACTION + scene["hour"])
        status = g.must("status")
        options = g.must("option_get", timeout=30).get("options") or []
        lap(clock, "teleport")
        phase = "warm-up"
        t0 = time.monotonic()
        while time.monotonic() - t0 < WARM_MAX_S:
            time.sleep(1)
            if time.monotonic() - t0 >= plan["warmup"] and settled(read_trace(trace)):
                r["warm"] = True
                break
        lap(clock, "warm-up")
        phase = "measured"
        rows = read_trace(trace)
        if not rows:
            raise drive.Failed("no frame trace at %s (RX_VK_FRAME_TRACE)" % trace)
        start = num(rows[-1], "at_ns")
        if scene.get("route"):   # (the route played from where the save loaded Link: measured while it plays)
            g.must("play", timeout=30, path=scene["route"]["path"], sync=scene["route"]["sync"])
            rows = read_trace(trace)
            start = num(rows[-1], "at_ns") if rows else start
            end = time.monotonic() + plan["seconds"] + 120
            while (g.must("status").get("recorder") or {}).get("mode") not in ("off", None):
                if time.monotonic() > end:
                    raise drive.Failed("the route did not end within %d s of its length" % 120)
                time.sleep(0.5)
        else:
            time.sleep(plan["seconds"])
        time.sleep(3)   # (the trace is flushed every 60 frames)
        lap(clock, "measured")
        phase = "stop"
        if session.info()["state"] != "running":
            raise drive.Failed("the game ended while measured")
        info = session.stop(timeout=30)
        lap(clock, "stop")
        phase = "copy and analysis"
        if info.get("exit_code"):
            raise drive.Failed("the game exited %s" % info["exit_code"])
        shutil.copyfile(trace, os.path.join(d, "frames.csv"))
        rows = window(read_trace(trace), start, plan["seconds"])
        if not rows or num(rows[-1], "at_ns") < start + 0.9 * plan["seconds"] * 1e9:
            raise drive.Failed("the frame trace covers less than 90 %% of the %d s measured" % plan["seconds"])
        with open(info["log"], encoding="utf-8", errors="replace") as f:
            text = f.read()
        facts = log_facts(text)
        if scene.get("route"):   # (a route's replay that went another way measured another run: refused)
            gone = re.search(r"input: the replay diverged at frame (\d+): ([^\n]*)", text)
            if gone:
                raise drive.Failed("the route's replay diverged at frame %s: %s" % gone.groups())
            met = re.search(r"input: (\d+) of (\d+) anchors met", text)
            if met and met.group(1) != met.group(2):
                raise drive.Failed("the route's replay met %s of its %s loads" % met.groups())
        skip = set(plan["compared"]) | set(plan["allow"]) | {"HOS_AB"}
        r.update(ran=dict(what_ran(status, options, facts, skip), version=facts["version"]), gpu=facts["gpu"],
                 cold=sum(1 for x in rows if cold(x)), log_lines=[l for l in [facts["switches_line"]] + facts["ab_lines"] if l])
        if side == "AB":
            r["ab"], r["periods"], r["dropped"], r["left_out"] = in_run_stats(rows, "%s%d" % (side, k))
            if any("was never read during the A/B" in l for l in facts["ab_lines"]):
                raise drive.Failed("%s was never read during the A/B: both sides ran the same" % plan["compared"][0])
        else:
            r["stats"] = frame_stats([num(x, "ms") for x in rows])
        lap(clock, phase)
        phase = None
    except (drive.Failed, longshot_bridge.Failed, OSError) as e:
        r["error"] = str(e)
        if phase:
            lap(clock, phase)
        if session is not None:
            try:
                session.stop(timeout=15, terminate=True)
            except longshot_bridge.Failed:
                pass
        lap(clock, "cleanup")
    r["timing"] = lap(clock, None)
    r["line"] = run_line(r)
    say(r["line"])
    return r


def summarise(plan, runs):
    """results.json: the runs, what ran, the verdict; None for the verdict when a run failed or the sides differed"""
    import platform
    res = {"runs": runs, "system": "%s %s" % (platform.system(), platform.release()), "problems": []}
    ok = [r for r in runs if not r.get("error")]
    res["problems"] += [r["line"] for r in runs if r.get("error")]
    if ok:
        res["gpu"], res["ran"] = ok[0].get("gpu"), ok[0]["ran"]
        res["problems"] += differences({(r["side"], r["k"]): r["ran"] for r in ok}, version_counts=plan["what"] == "switch")
    if res["problems"]:
        res["verdict"] = None
        return res
    if plan["how"] == "in-run":
        from profiling import ab as abtrace
        periods = [p for r in ok for p in r["periods"]]
        res["pooled"] = abtrace.summary("all runs", periods, sum(r["dropped"] for r in ok),
                                        [sum(r["left_out"][i] for r in ok) for i in (0, 1)], 2000, 1)
        res["verdict"] = verdict_in_run(res["pooled"])
    else:
        res["verdict"] = verdict([r["stats"]["fps"] for r in ok if r["side"] == "A"],
                                 [r["stats"]["fps"] for r in ok if r["side"] == "B"])
    return res


# ---- the command ----

def cmd_ab(a):
    called = time.monotonic()
    if a.scenes:
        for n, (save, to, age, hour, about) in SCENES.items():
            print("%-20s %s%s" % (n, about, "  (default)" if n == DEFAULT_SCENE else ""))
        return 0
    if a.env and (a.a or a.b):
        die("--env, or --a and --b, not both")
    b_env = assignments(a.env or a.b, "--env" if a.env else "--b")
    a_env = assignments(a.a, "--a")
    both = assignments(a.set, "--set")
    change = not (a_env or b_env)
    if not change and not differing(a_env, b_env):
        die("the two sides set the same switches")
    clash = set(both) & (set(a_env) | set(b_env))
    if clash:
        die("--set names a compared switch: %s" % ", ".join(sorted(clash)))
    if a.runs < 1 or a.seconds < 10:
        die("--runs 1 or more, --seconds 10 or more")
    if a.route:
        if a.save:
            die("--route or --save, not both: a route starts from its own repository save")
        r = routes.read_route(routes.route_file(a.route))
        if r["seconds"] < 10:
            die("the route %s is %.1f s: 10 s or more to measure" % (r["name"], r["seconds"]))
        scene = {"name": "route-" + r["name"], "save": r["save"], "folder": None, "slot": None, "to": None, "age": None,
                 "hour": None, "about": "the route %s (%.0f s from %s, %s)" % (r["name"], r["seconds"], r["save"], r["map"]),
                 "route": {"path": r["path"], "sync": a.sync, "frames": r["frames"]}}
        a.seconds = int(r["seconds"])
    else:
        scene = scene_of(a.scene, a.save, a.slot)
    for p in (a.options, scene["folder"]):
        if p and not os.path.exists(p):
            die("no such file or folder: %s" % p)
    from tours import route
    scene["dest"] = None if scene.get("route") else destination(scene, route.actions())
    common_env = dict(COMMON_ENV, **both)
    if a.headless:
        common_env["HOS_HEADLESS"] = "1"
    how, why = "separate", ""
    if not change:
        ok, why = in_run(a_env, b_env, wired_switches())
        how = "in-run" if ok and not a.separate else "separate"
        why = "" if ok and not a.separate else why if not ok else "--separate"
    if a.threads:
        if "HOS_THREAD_TIMES" in set(both) | set(a_env) | set(b_env):
            die("--threads cannot be combined with HOS_THREAD_TIMES overrides")
        if how == "in-run":
            die("--threads cannot sample an in-run comparison; use --separate (thread comparisons are currently unavailable)")
        die("--threads is currently unavailable: CPU samples cannot be safely matched to measured frames")
    label = "change" if change else "-".join(differing(a_env, b_env)).lower()
    folder = os.path.join(common.dev_path("runs"), "ab", time.strftime("%Y%m%d-%H%M%S-") + label + "-" + scene["name"])
    if change:
        if a.dry_run:
            hashes, rts = {"A": "<base>", "B": "<this checkout>"}, {
                "A": os.path.join(build.own_dir(), "rt-base"), "B": os.path.join(build.own_dir(), "rt-" + build.pack_name())}
            say("would build: dev.py base (when missing), dev.py pack, and each runtime when stale")
        else:
            hashes = {"B": checkout_commit(a.dirty)}
            build.cmd_base(argparse.Namespace(rev=None, romfs_in_place=False))
            build.cmd_pack(argparse.Namespace())
            rts = {"B": fresh_runtime(False), "A": fresh_runtime(True)}
            hashes["A"] = packs.read_base(build.base_for())["commit"]
    else:
        rt = os.path.abspath(a.runtime) if a.runtime else default_runtime()
        if not os.path.isdir(rt):
            (say if a.dry_run else die)("no runtime at %s: dev.py rt makes one, or name one with --runtime" % rt)
        rts, hashes = {"A": rt, "B": rt}, {}
    plan = make_plan(a_env, b_env, change, scene, a.seconds, a.runs, a.warmup, common_env, how, why, hashes, rts, folder,
                     a.allow or ())
    print(plan_text(plan), flush=True)
    if not a.dynamic and dynamic_resolution(a.options):
        say("the Render resolution at 67 % in every run: the options file leaves it at Auto, the game's dynamic resolution, "
            "whose 3D size follows the GPU's load (--dynamic keeps Auto)")
    if a.dry_run:
        return 0
    os.makedirs(folder)
    with open(os.path.join(folder, "plan.json"), "w", encoding="utf-8") as f:
        json.dump(plan, f, indent=1)
    leaked = sorted(k for k in os.environ if k.startswith(("HOS_", "RX_", "A64_", "OOT_")))
    if leaked:   # (after the builds, which name these themselves: a switch in this shell would reach both sides unsaid)
        say("left out of the runs (set in this shell): %s; --set passes one to both sides" % ", ".join(leaked))
        for k in leaked:
            del os.environ[k]
    pinned = os.path.join(folder, "options.ini")
    pin_options(a.options, pinned, a.framegen, a.vsync, a.dynamic)
    began = time.monotonic()
    runs = [run_one(plan, side, k, pinned) for side, k in plan["order"]]
    batch_end = time.monotonic()
    res = summarise(plan, runs)
    res["timing"] = timing_summary(runs, time.monotonic() - called, began - called, batch_end - began)
    say(timing_text(res["timing"]))
    text = measurements(plan, res) if res["verdict"] is not None else None
    for r in runs:   # (the analysis' own lists: results.json keeps the numbers)
        r.pop("periods", None)
    with open(os.path.join(folder, "results.json"), "w", encoding="utf-8") as f:
        json.dump(res, f, indent=1)
    if res["verdict"] is None:
        say("no write-up: %s" % ("; ".join(res["problems"][:12])))
        rows = sorted({p.split(":")[0].split(" ", 1)[1] for p in res["problems"] if p.startswith("options ")})
        if rows:
            say("the runs differ in menu rows (%s): --allow <options-file key> each, if that is the compared thing"
                % ", ".join(rows))
        say("the runs: %s" % folder)
        return 1 if any(r.get("error") for r in runs) else 3
    with open(os.path.join(folder, "measurements.md"), "w", encoding="utf-8") as f:
        f.write(text)
    print("\n" + text, flush=True)
    say("written to %s" % os.path.join(folder, "measurements.md"))
    return 0


def add_commands(sub):
    """`dev.py ab`"""
    p = sub.add_parser("ab", help="measure a switch or this checkout's change against main in the game, through the queue",
                       description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter, allow_abbrev=False)
    p.add_argument("--env", action="append", metavar="NAME=VALUE", help="a switch: set (B) against unset (A)")
    p.add_argument("--a", action="append", metavar="NAME=VALUE", help="side A's switches")
    p.add_argument("--b", action="append", metavar="NAME=VALUE", help="side B's switches")
    p.add_argument("--set", action="append", metavar="NAME=VALUE", help="a switch for both sides")
    p.add_argument("--scene", default=DEFAULT_SCENE, help="where to measure (--scenes lists them; default %(default)s)")
    p.add_argument("--scenes", action="store_true", help="list the scenes")
    p.add_argument("--save", metavar="FOLDER", help="start from this save folder instead of the scene's repository save")
    p.add_argument("--slot", help="with --save: that slot alone")
    p.add_argument("--route", metavar="ROUTE", help="measure while a route (assets/routes/ or a file) plays, from its save")
    p.add_argument("--sync", type=int, default=1, help="with --route: put Link and the camera back at every N-th update "
                                                       "(0 never; default %(default)s)")
    p.add_argument("--seconds", type=int, default=60, help="measured a run (default %(default)s)")
    p.add_argument("--runs", type=int, default=3, help="pairs of runs, or HOS_AB runs (default %(default)s)")
    p.add_argument("--warmup", type=int, default=10, help="seconds of warm-up at least, then until no frame waits for a "
                                                            "pipeline (at most %d)" % WARM_MAX_S)
    p.add_argument("--options", metavar="FILE", help="the options file to pin (default: none, every row at its default)")
    p.add_argument("--dynamic", action="store_true", help="keep the Render resolution at Auto, the game's dynamic resolution "
                                                          "(default: 67%% where the options file leaves it at Auto, so the "
                                                          "3D is the same size in every run)")
    p.add_argument("--framegen", action="store_true", help="keep the options file's frame generation")
    p.add_argument("--vsync", action="store_true", help="keep the options file's V-Sync")
    p.add_argument("--headless", action="store_true", help="no window (HOS_HEADLESS=1): not what players see")
    p.add_argument("--runtime", metavar="DIR", help="a switch's runtime (default rt-<checkout>, else rt-base)")
    p.add_argument("--separate", action="store_true", help="separate runs even when HOS_AB could flip the switch")
    p.add_argument("--allow", action="append", metavar="KEY", help="a menu row (its options-file key) or switch the runs "
                                                                    "may differ in (a row the change adds)")
    p.add_argument("--dirty", action="store_true", help="measure a change with uncommitted files")
    p.add_argument("--threads", action="store_true", help="thread CPU comparison (currently unavailable)")
    p.add_argument("--dry-run", action="store_true", help="print the plan, run nothing")
    p.set_defaults(func=cmd_ab)
