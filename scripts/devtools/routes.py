"""`dev.py route`: routes, input recordings that can be committed (assets/routes/), for tests and `dev.py ab --route`
(docs/dev/reference/input.md, *Routes*). Python's standard library only.

    dev.py route record --save NAME --name ROUTE [--runtime DIR]   play it once: recorded from the save's load, to
                                                                    assets/routes/ROUTE.jsonl when the game is closed
    dev.py route check [FILE ...]                                  every route (or these) holds only what a route may
    dev.py route list                                              the routes, their save, map and length

A route is recorded by the game itself (host/input_movie.c, HOS_INPUT_ROUTE=1) from a repository save's load: the
controller, motion, the mouse camera's turns, the Debug tools' actions and rows, the loads (anchors), Link's positions
and the camera's angles, as numbers. Nothing in it is read from the game's files or memory words: no guest words (the
camera's state block, its inputs), no addresses (the menu mouse's screens and buttons), no texts but the format's. That
is what `check` holds it to, as tests/test_routes.py does in CI, so a route can be committed like a play profile.
"""
import argparse
import json
import os
import re
import shutil
import sys
import tempfile
import time

from . import common

WHO = "dev.py route"
ROUTES = os.path.join(common.ROOT, "assets", "routes")
SAVES = os.path.join(common.ROOT, "assets", "saves")
FORMAT = "ls-input-recording/1"
# (what a route's lines may hold, field by field: host/input_movie.h's lines with HOS_INPUT_ROUTE; nothing else)
HEADER = {"format", "fps", "start", "source", "from", "build", "fixed_time", "step", "camera_before", "link_before",
          "clock", "map", "age", "profile", "save"}
# (the header's texts, each to what the game writes there: no room for anything else)
HEADER_TEXT = {"format": re.compile(r"^ls-input-recording/1$"), "source": re.compile(r"^longshot$"),
               "from": re.compile(r"^(play|boot)$"), "build": re.compile(r"^[A-Za-z0-9 ._+()/-]{0,80}$"),   # (nx2/version.py describe's)
               "fixed_time": re.compile(r"^[0-9]{0,12}$"), "step": re.compile(r"^(per frame|wall clock)$"),
               "map": re.compile(r"^[0-9A-Za-z_-]{0,48}$"), "age": re.compile(r"^(child|adult|)$"),
               "profile": re.compile(r"^(route|full)$"), "save": re.compile(r"^[a-z0-9-]*$")}
LINES = {
    "pad": {"f", "cmd", "frames", "bits", "buttons", "lx", "ly", "rx", "ry", "fl", "fc", "fh"},
    "cam": {"f", "cmd", "n", "i", "native", "free", "held", "dyaw", "dpitch"},
    "gyro": {"f", "cmd", "c", "i", "live", "top", "upto"},
    "motion": {"f", "cmd", "n", "dt", "v"},
    "frames": {"f", "cmd", "count", "reads", "read"},
    "view": {"f", "cmd", "n", "i", "yaw", "pitch"},
    "mark": {"f", "cmd", "n", "i", "pos"},
    "action": {"f", "cmd", "a"},
    "option": {"f", "cmd", "id", "v"},
    "anchor": {"f", "cmd", "n", "gap"},
}
TEXT = {"bits", "buttons", "fl", "fc"}   # (the only strings a line may hold besides cmd: hex words and button names)
HEX = re.compile(r"^0x[0-9a-f]{1,16}$")
BUTTONS = re.compile(r"^([A-Z]{1,5}|b[0-9]{1,2})(\+([A-Z]{1,5}|b[0-9]{1,2}))*$|^$")
SAVE_NAME = re.compile(r"^[a-z0-9-]+$")


def die(text):
    raise SystemExit("%s: %s" % (WHO, text))


def number(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool) and v == v and v not in (float("inf"), float("-inf"))


def strict_json(text):
    """A line as JSON, refused (ValueError) when a key is there twice (the first one's value would ride along unseen) or
    a number is NaN or an infinity"""
    def pairs(items):
        keys = [k for k, _ in items]
        if len(keys) != len(set(keys)):
            raise ValueError("a key twice")
        return dict(items)

    def constant(name):
        raise ValueError("%s is no number" % name)
    return json.loads(text, object_pairs_hook=pairs, parse_constant=constant)


def check_lines(lines, saves=SAVES):
    """The problems of a route's lines (a list of strings, as the file holds them): [] when it holds only what a route
    may. saves: the repository saves' folder its header's save must name"""
    problems = []
    if not lines:
        return ["empty"]
    try:
        head = strict_json(lines[0])
    except ValueError as e:
        return ["line 1: not JSON as a route's: %s" % e]
    if not isinstance(head, dict) or head.get("format") != FORMAT:
        return ["line 1: not an %s header" % FORMAT]
    extra = set(head) - HEADER
    if extra:
        problems.append("the header: fields a route does not hold: %s" % ", ".join(sorted(extra)))
    if head.get("profile") != "route":
        problems.append("the header: profile %r, not route (record it with HOS_INPUT_ROUTE=1: dev.py route record)" %
                        head.get("profile"))
    save = head.get("save")
    if not isinstance(save, str) or not SAVE_NAME.match(save) or not os.path.isdir(os.path.join(saves, save)):
        problems.append("the header: save %r is not one of assets/saves/" % (save,))
    if head.get("from") != "play":
        problems.append("the header: from %r: a route starts at Link's first update after the save's load" % head.get("from"))
    for k, v in head.items():
        if k in HEADER_TEXT:
            if not isinstance(v, str) or not HEADER_TEXT[k].match(v):
                problems.append("the header: %s is not what the game writes there" % k)
        elif k in HEADER and not number(v):
            problems.append("the header: %s is not a number" % k)
    for n, text in enumerate(lines[1:], 2):
        if not text.strip():
            continue
        try:
            e = strict_json(text)
        except ValueError as err:
            problems.append("line %d: not JSON as a route's: %s" % (n, err))
            continue
        cmd = e.get("cmd") if isinstance(e, dict) else None
        if cmd not in LINES:
            problems.append("line %d: %r is no line a route holds" % (n, cmd))
            continue
        extra = set(e) - LINES[cmd]
        if extra:
            problems.append("line %d (%s): fields a route does not hold: %s" % (n, cmd, ", ".join(sorted(extra))))
        for k, v in e.items():
            if k == "cmd":
                continue
            if k in TEXT:
                ok = isinstance(v, str) and (BUTTONS.match(v) if k == "buttons" else HEX.match(v))
            elif k in ("v", "pos"):
                ok = isinstance(v, list) and all(number(x) for x in v) if cmd in ("motion", "mark") else number(v)
            else:
                ok = number(v)
            if not ok:
                problems.append("line %d (%s): %s is not a number%s" % (n, cmd, k, " or a hex word" if k in TEXT else ""))
        if len(problems) > 40:
            problems.append("and more")
            break
    return problems


def read_route(path):
    """{path, name, save, map, frames, seconds, header} of a route file, or die with its problems"""
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    problems = check_lines(lines)
    if problems:
        die("%s is not a route: %s" % (path, "; ".join(problems[:6])))
    head = json.loads(lines[0])
    frames = 0
    for text in lines[1:]:
        if text.strip():
            e = json.loads(text)
            if e["cmd"] == "pad":
                frames = max(frames, e["f"] + e["frames"])
    name = os.path.splitext(os.path.basename(path))[0]
    return {"path": os.path.abspath(path), "name": name, "save": head["save"], "map": head.get("map", ""),
            "frames": frames, "seconds": frames / 60.0, "header": head}


def route_file(name):
    """A route's file: a path, or the name of one in assets/routes/"""
    if os.path.isfile(name):
        return name
    path = os.path.join(ROUTES, name if name.endswith(".jsonl") else name + ".jsonl")
    if not os.path.isfile(path):
        have = sorted(f[:-6] for f in os.listdir(ROUTES) if f.endswith(".jsonl")) if os.path.isdir(ROUTES) else []
        die("no route %r (assets/routes/: %s)" % (name, ", ".join(have) or "none yet"))
    return path


def cmd_check(a):
    files = a.files or ([os.path.join(ROUTES, f) for f in sorted(os.listdir(ROUTES)) if f.endswith(".jsonl")]
                        if os.path.isdir(ROUTES) else [])
    bad = 0
    for path in files:
        with open(path, encoding="utf-8") as f:
            problems = check_lines(f.read().splitlines())
        print("%s: %s" % (os.path.relpath(path, common.ROOT), "ok" if not problems else "; ".join(problems[:8])))
        bad += bool(problems)
    if not files:
        print("no routes in assets/routes/")
    return 1 if bad else 0


def cmd_list(a):
    if not os.path.isdir(ROUTES):
        print("no routes in assets/routes/")
        return 0
    for f in sorted(os.listdir(ROUTES)):
        if f.endswith(".jsonl"):
            r = read_route(os.path.join(ROUTES, f))
            print("%-24s %5.1f s  from %-16s %s" % (r["name"], r["seconds"], r["save"], r["map"]))
    return 0


def cmd_record(a):
    """The game started from the repository save through the queue (shared), recording a route from its load until it is
    closed; the file checked and put in assets/routes/"""
    import longshot_bridge
    from devtools import ab, packs
    if not SAVE_NAME.match(a.save) or not os.path.isdir(os.path.join(SAVES, a.save)):
        die("no repository save %r in assets/saves/" % a.save)
    if not re.match(r"^[a-z0-9-]+$", a.name):
        die("a route's name: a-z, 0-9 and -")
    out = os.path.join(ROUTES, a.name + ".jsonl")
    if os.path.exists(out) and not a.replace:
        die("%s exists: --replace to record it again" % os.path.relpath(out, common.ROOT))
    rt = os.path.abspath(a.runtime) if a.runtime else ab.default_runtime()
    if not os.path.isdir(rt):
        die("no runtime at %s: dev.py rt makes one, or name one with --runtime" % rt)
    d = tempfile.mkdtemp(prefix="route-", dir=common.dev_path("runs"))
    rec = os.path.join(d, a.name + ".jsonl")
    opts = os.path.join(d, "options.ini")
    ab.pin_options(None, opts, False, True)
    env = {"HOS_INPUT_RECORD": rec, "HOS_INPUT_ROUTE": "1", "HOS_INPUT_SAVE": a.save, "HOS_REALTIME_STEP": "0",
           "HOS_FIXED_TIME": "1767225600", "HOS_OPTIONS_FILE": opts, "HOS_FPS_MAX": "60"}
    wrapper = [sys.executable, "-I", os.path.join(common.SCRIPTS, "dev.py"), "queue", "run", "route-" + a.name, "--mode",
               "shared", "--desc", "recording the route %s" % a.name, "--"]
    print("%s: the game starts from %s; play the route, then close the game (its window, or the menu's Quit)" % (WHO, a.save),
          flush=True)
    session = longshot_bridge.start_session(rt, romfs=packs.runtime_romfs(rt), run=os.path.join(d, "game"),
                                            user=os.path.join(d, "user"), repo_save=a.save, env=env, ready="present",
                                            timeout=1800, wrapper=wrapper)
    while session.info()["state"] == "running":
        time.sleep(1)
    if not os.path.isfile(rec):
        die("the game wrote no recording (its log: %s)" % session.info().get("log"))
    r = read_route(rec)
    os.makedirs(ROUTES, exist_ok=True)
    shutil.copyfile(rec, out)
    print("%s: %s, %.1f s from %s (%s)" % (WHO, os.path.relpath(out, common.ROOT), r["seconds"], r["save"], r["map"]))
    return 0


def add_commands(sub):
    """`dev.py route`"""
    p = sub.add_parser("route", help="routes (assets/routes/): record one, check them, list them", description=__doc__,
                       formatter_class=argparse.RawDescriptionHelpFormatter, allow_abbrev=False)
    s = p.add_subparsers(dest="route_cmd", required=True)
    r = s.add_parser("record", help="play a route once from a repository save; it goes to assets/routes/")
    r.add_argument("--save", required=True, help="the repository save (assets/saves/) it starts from")
    r.add_argument("--name", required=True, help="its name: assets/routes/<name>.jsonl")
    r.add_argument("--runtime", metavar="DIR", help="the runtime to play (default rt-<checkout>, else rt-base)")
    r.add_argument("--replace", action="store_true", help="record over a route of that name")
    r.set_defaults(func=cmd_record)
    c = s.add_parser("check", help="every route (or these files) holds only what a route may")
    c.add_argument("files", nargs="*")
    c.set_defaults(func=cmd_check)
    s.add_parser("list", help="the routes").set_defaults(func=cmd_list)
