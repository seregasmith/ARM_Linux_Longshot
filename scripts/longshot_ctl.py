"""The control bridge's command line (host/control.c, docs/dev/reference/control-bridge.md): start the game with the bridge
on, then steer it and read its state from a shell, a test or a coding agent. The client itself, which tools import, is
scripts/longshot_bridge.py. Standard library only.

    python -I scripts/longshot_ctl.py [--sock S] [--json] COMMAND ...
    --run DIR COMMAND ...                       re-attach using the run's manifest instead of a socket
    runs [ROOT] / --run DIR show / finish       list runs, show one, mark an orphaned dead run finished
    --run DIR stop [--timeout 10] [--terminate] quit, wait for its owner to reap it; force only when requested
    --run DIR log [--lines 100|--since CURSOR] [--expect TEXT [--regex]]  bounded run log reads

    launch GAME_DIR [--data DIR] [--headless]   start the game with the bridge on; prints the socket's path
           [--save FOLDER [--slot NN]]          a copy of a save folder (one slot of it) as the run's saves
           [--repo-save NAME [--repo DIR]]      its slot_00 from the repository (assets/saves/NAME) instead
    status                                      frame, fps, draws, state, window, version, OoT snapshot
    press A+B [--frames 8] [--exclusive]        buttons held for some frames (the reply once they are let go)
    stick L 0 1 [--frames 60]                   a stick at x, y (-1 to 1)
    sequence '0:A/8,60:A/8' [--hos]             steps, <at>:<buttons>[/<frames held>] (--hos: HOS_INPUT_SCRIPT's s and ms)
    release [--all]                             this client's queued inputs let go (and those of clients gone)
    shot [out.png] [--after FRAME]              a PNG of the next present; prints its path
    wait in_play [--timeout 120]                also: present --frames N, frame --frame N, state --state S,
                                                loading_start, loading_end, change, map [--map NAME]
    teleport [INDEX|MAP] [--arrive SECS]        (OoT) the debug teleport: sent or refused; none: the destinations
    age [child|adult] [--arrive SECS]           (OoT) the Debug tab's Switch age (none: the other age), reloading the map
    option get [NAME]  /  option set NAME VALUE the Insert menu's settings
    saves  /  slots [--thumbs DIR]              the save folder the game uses; its slots, newest first
    saves --repo [DIR]                          the saves in the repository (no game): names, README first lines
    watch [--events present,state,log,map,oot] [--every 60] [--log TEXT] [--count N]
          [--flags NAME,0xHASH,...] [--changes]    oot: watched values and changed flags
    mem read ADDR SIZE [--format hex|u8|u16|u32|u64|f32]   read guest memory (HOS_CONTROL_MEM=ro or rw)
    mem write ADDR HEX                          write guest memory (HOS_CONTROL_MEM=rw); aliases: ro->r/read, rw->readwrite
    quit                                        the game exits
    relaunch [--timeout 120]                    the game starts again (the Insert menu's Restart now), with the same
                                                command and environment; back when its bridge answers again
    raw '{"cmd": "status"}'                     any request

The socket defaults to $HOS_CONTROL. Plain lines by default, --json for the replies as they come. Exit status: 0 done,
1 an error reply, 2 no game or bad usage, 3 a timeout.
"""
import argparse
import json
import math
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from longshot_bridge import (Bridge, Failed, Session, check, list_runs, relaunch, repo_save_names,  # noqa: E402
                             repo_saves_dir, start_session)


def plain_text(text):
    """Text from the game for a terminal: its control characters (an escape sequence in a log line) shown as '?'"""
    return "".join("?" if ord(ch) < 0x20 or 0x7f <= ord(ch) < 0xa0 else ch for ch in str(text))


def when(t):
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t)) if t else "-"


def saves_text(r):
    lines = ["%s  (account.sav %s, option.sav %s)" % (plain_text(r["path"]), "yes" if r["account"] else "no",
                                                        "yes" if r["option"] else "no")]
    for sl in r["slots"]:
        lines.append("slot_%s  %s%s" % (plain_text(sl["slot"]), when(sl["mtime"]), "  (newest)" if sl["newest"] else ""))
        for f in sorted(sl["files"], key=lambda f: f["name"]):
            lines.append("    %-14s %9d  %s" % (plain_text(f["name"]), f["size"], when(f["mtime"])))
    return "\n".join(lines)


def slots_text(r):
    rows = ["   slot  last written          progress   caption  scene"]
    for sl in r["slots"]:
        size = {f["name"]: f["size"] for f in sl["files"]}
        rows.append("%s  %-4s  %-19s %9s %9s  %s" % ("*" if sl["newest"] else " ", plain_text(sl["slot"]), when(sl["mtime"]),
                                                   size.get("progress.sav", "-"), size.get("caption.sav", "-"),
                                                   plain_text(sl.get("caption", {}).get("scene", ""))))
    rows.append("(* the newest: the one Continue loads)")
    return "\n".join(rows)


def write_thumbs(r, folder):
    """Each slot's file-select picture, as the JPEG caption.sav holds, read from the save folder (this PC's). What the game
    says is checked first: this client may run outside a sandbox the game is in, so a slot's name is only digits, the
    picture a bounded slice of that slot's caption.sav, and nothing is written outside folder"""
    os.makedirs(folder, exist_ok=True)
    for sl in r["slots"]:
        th = sl.get("caption", {}).get("thumbnail")
        if not th:
            continue
        slot, off, size = sl.get("slot"), th.get("offset"), th.get("size")
        if not (isinstance(slot, str) and re.fullmatch(r"[0-9]{1,3}", slot) and isinstance(off, int) and
                isinstance(size, int) and 0 <= off < (1 << 24) and 0 < size <= (4 << 20)):
            raise Failed("slots --thumbs: the game's reply does not look like a save listing: %r" % (sl,), 1)
        with open(os.path.join(r["path"], "slot_" + slot, "caption.sav"), "rb") as f:
            f.seek(off)
            data = f.read(size)
        if data[:2] == b"\xff\xd8":
            with open(os.path.join(folder, "slot_%s.jpg" % sl["slot"]), "wb") as out:
                out.write(data)


def addr_of(text):
    try:
        return int(text, 0)
    except ValueError:
        raise Failed("ADDR: a number, or 0x...", 2)


def format_bytes(data, fmt):
    import struct
    if fmt in (None, "hex"):
        return data.hex()
    if fmt == "base64":
        import base64
        return base64.b64encode(data).decode()
    sizes = {"u8": ("B", 1), "u16": ("<H", 2), "u32": ("<I", 4), "u64": ("<Q", 8), "f32": ("<f", 4)}
    code, n = sizes[fmt]
    out = []
    for i in range(0, len(data) - n + 1, n):
        out.append(struct.unpack(code, data[i:i + n])[0])
    return " ".join(("%g" if fmt == "f32" else "%d") % v for v in out)


def repo_saves_list(repo=None):
    """[{name, title (its README's first line), path}] of the repo saves"""
    out = []
    for n in repo_save_names(repo):
        path = os.path.join(repo_saves_dir(repo), n)
        title = ""
        try:
            with open(os.path.join(path, "README.md"), encoding="utf-8", errors="replace") as f:
                title = f.readline(512).strip()
        except OSError:
            pass
        out.append({"name": n, "title": title, "path": path})
    return out


def connect(a, timeout=10.0):
    if getattr(a, "run", None):
        if a.sock:
            raise Failed("--run and --sock are alternatives", 2)
        return Session.attach(a.run).connect(timeout)
    path = a.sock or os.environ.get("HOS_CONTROL")
    if not path:
        raise Failed("no socket: give --sock PATH or set HOS_CONTROL (longshot_ctl.py launch GAME_DIR starts a game "
                     "with the bridge on and prints its socket)", 2)
    return Bridge(path, timeout)


def show(a, reply, plain):
    if a.json:
        print(json.dumps(reply))
    else:
        print(plain(reply))


def run(a):
    if a.command == "launch":
        return launch(a)
    if a.command == "runs":
        show(a, list_runs(a.root), lambda rows: "\n".join("%s  %s" % (r["state"], r["run"]) for r in rows))
        return 0
    if a.command in ("show", "finish", "stop", "log"):
        if not a.run:
            raise Failed("%s needs --run DIR" % a.command, 2)
        session = Session(a.run)
        if a.command == "show":
            result = session.info()
        elif a.command == "finish":
            result = session.finish()
        elif a.command == "stop":
            result = session.stop(a.timeout, terminate=a.terminate)
        elif a.expect is not None:
            if a.since is None:
                raise Failed("log --expect needs --since CURSOR", 2)
            result = session.expect_log(a.expect, a.since, a.timeout, regex=a.regex, limit=a.bytes)
        else:
            result = session.log(lines=a.lines, since=a.since, limit=a.bytes)
        show(a, result, lambda r: "\n".join(plain_text(s) for s in r["text"].split("\n")) if "text" in r
             else json.dumps(r, indent=2))
        return 3 if a.command == "stop" and not result.get("process_exited") else 0
    if a.command == "saves" and a.repo is not None:
        rows = repo_saves_list(a.repo or None)
        show(a, rows, lambda rows: "\n".join("%-28s %s" % (r["name"], plain_text(r["title"])) for r in rows) or
             "no saves in %s" % repo_saves_dir(a.repo or None))
        return 0
    if a.command == "mem":
        a.addr = addr_of(a.addr)
        if a.action == "read":
            if a.size is None:
                raise Failed("mem read ADDR SIZE", 2)
            a.size = addr_of(str(a.size))
        else:
            a.hex = a.hex if a.hex is not None else a.size   # write ADDR HEX
            if not a.hex:
                raise Failed("mem write ADDR HEX", 2)
    b = connect(a)
    try:
        if a.command == "status":
            r = check(b.request("status"))
            win = r.get("window") or {}
            g = r.get("game") or {}
            flags = " ".join(n for n, on in (("event", g.get("event")), ("loading", g.get("loading"))) if on)
            show(a, r, lambda r: "frame %d  fps %.1f  state %s  draws %s  window %s  map %s%s  %s" % (
                r["frame"], r["fps"], r["state"], "-" if r.get("draws") is None else "%.0f" % r["draws"],
                "%dx%d" % (win["width"], win["height"]) if win else "-", plain_text(r.get("map") or "-"),
                " (%s%s)" % (plain_text(g.get("age", "?")), ", " + flags if flags else "") if g else "",
                plain_text(r.get("version", ""))) + ("\ngame   " + json.dumps(g, ensure_ascii=True) if g else ""))
        elif a.command in ("press", "stick", "sequence"):
            args = {"exclusive": a.exclusive, "wait": False if a.no_wait else None}
            if a.command == "press":
                args.update(buttons=a.buttons, frames=a.frames, at=a.at)
            elif a.command == "stick":
                args.update(stick=a.which.upper(), x=a.x, y=a.y, frames=a.frames, at=a.at)
            else:
                args.update(script=a.script.strip(), units="hos" if a.hos else None)
            r = check(b.request(a.command, timeout=a.timeout, **args))
            show(a, r, lambda r: "queued %d input(s) at frame %d" % (r["queued"], r["frame"]) if "queued" in r
                 else "frames %d to %d (now %d)" % (r["start"], r["end"], r["frame"]))
        elif a.command == "release":
            show(a, check(b.request("release", all=True if a.all else None)),
                 lambda r: "released %d%s" % (r["released"], " (every client's)" if a.all else ""))
        elif a.command == "shot":
            path = os.path.abspath(a.path) if a.path else None   # (the bridge would take it as the socket folder's)
            r = check(b.request("shot", timeout=a.timeout, path=path, after=a.after))
            show(a, r, lambda r: r["path"])
        elif a.command == "wait":
            r = check(b.request("wait", timeout=a.timeout, event=a.event, frames=a.frames, frame=a.frame, state=a.state,
                                map=a.map))
            show(a, r, lambda r: "%s after %d frames (frame %d)%s" % (r["state"], r["waited"], r["frame"],
                                                                     "  map %s" % plain_text(r["map"]) if "map" in r else ""))
        elif a.command == "teleport":
            to = None if a.to is None else int(a.to) if a.to.isdigit() else a.to
            if to is None and (a.arrive or a.no_wait):
                raise Failed("teleport: --arrive and --no-wait go with a destination", 2)
            r = check(b.request("teleport", to=to, wait=False if a.no_wait else None,
                                timeout=None if to is None else a.timeout))
            if "destinations" in r:
                show(a, r, lambda r: "\n".join("%3d  %-28s %s" % (d["index"], plain_text(d["child"] or "-"),
                                                                  plain_text(d["adult"] or "-")) for d in r["destinations"]))
            elif not r.get("sent"):
                show(a, r, lambda r: "teleport %d asked for (child %s, adult %s): wait map to see it arrive" % (
                    r["teleport"], plain_text(r["child"] or "-"), plain_text(r["adult"] or "-")))
            elif not a.arrive:
                show(a, r, lambda r: "teleport %d sent: to %s%s" % (
                    r["teleport"], plain_text(r.get("map") or "-"),
                    "" if r.get("accepted", True) else " (but the game's event manager did not take it)"))
            else:   # (and until the game is in that map: the arrival)
                w = check(b.request("wait", timeout=a.arrive, event="map", map=r.get("map")))
                show(a, dict(r, arrived=w), lambda r: "teleport %d: in %s after %d frames (frame %d)" % (
                    r["teleport"], plain_text(w["map"]), w["waited"], w["frame"]))
        elif a.command == "age":
            r = check(b.request("age", to=a.to, wait=False if a.no_wait else None, timeout=a.timeout))
            if r.get("sent") is False:
                show(a, r, lambda r: "Link is the %s already" % plain_text(r["age"]))
            elif r.get("sent") is None:
                show(a, r, lambda r: "age switch to the %s asked for: wait loading_end to see it reload" % plain_text(r["age"]))
            elif not a.arrive:
                show(a, r, lambda r: "age switch sent: the %s, reloading %s%s" % (
                    plain_text(r["age"]), plain_text(r.get("map") or "-"),
                    "" if r.get("accepted", True) else " (but the game's event manager did not take it)"))
            else:   # (and until the reload's load has ended: in a dungeon the map has the same name for both ages, so
                    # a wait map would be met before the reload; loading_end counts from the ask, so a load already
                    # started when it is asked is still waited out)
                w = check(b.request("wait", timeout=a.arrive, event="loading_end"))
                show(a, dict(r, arrived=w), lambda r: "age switch: the %s, in %s after %d frames (frame %d)" % (
                    plain_text(r["age"]), plain_text(r.get("map") or "-"), w["waited"], w["frame"]))
        elif a.command == "option":
            if a.action == "get":
                r = check(b.request("option_get", name=a.name))
                rows = [r["option"]] if "option" in r else r["options"]
                show(a, r, lambda r: "\n".join("%-28s %4d  %s%s" % (o["key"], o["value"], plain_text(o["text"]),
                                                                    "  (restart)" if o.get("restart") else "") for o in rows))
            else:
                if a.value is None:
                    raise Failed("option set NAME VALUE", 2)
                v = {"on": 1, "true": 1, "off": 0, "false": 0}.get(a.value.lower())
                if v is None:
                    try:
                        v = int(a.value)
                    except ValueError:
                        raise Failed("VALUE: a number, on or off", 2)
                r = check(b.request("option_set", name=a.name, value=v))
                show(a, r, lambda r: "%s %d -> %d (%s)" % (r["option"]["key"], r["was"], r["option"]["value"], r["option"]["text"]))
        elif a.command == "mem":
            if a.action == "read":
                fmt = a.format if a.format in ("hex", "base64") else "hex"
                r = check(b.request("mem_read", addr=a.addr, size=a.size, format=fmt))
                data = bytes.fromhex(r["data"]) if r["format"] == "hex" else __import__("base64").b64decode(r["data"])
                show(a, r, lambda r: format_bytes(data, a.format))
            else:
                raw = a.hex.lower().replace("0x", "")
                try:
                    bytes.fromhex(raw)
                except ValueError:
                    raise Failed("HEX: an even number of hex digits", 2)
                r = check(b.request("mem_write", addr=a.addr, hex=raw))
                show(a, r, lambda r: "wrote %d bytes at %d" % (r["wrote"], r["addr"]))
        elif a.command == "saves":
            r = check(b.request("saves"))
            show(a, r, lambda r: saves_text(r))
        elif a.command == "slots":
            r = check(b.request("saves"))
            if a.thumbs:
                write_thumbs(r, a.thumbs)
            if a.json:
                print(json.dumps(r["slots"]))
            else:
                print(slots_text(r))
        elif a.command == "quit":
            show(a, check(b.request("quit")), lambda r: "quitting")
        elif a.command == "relaunch":
            st = relaunch(b, a.timeout)
            show(a, st, lambda r: "relaunched: answering again after %.1f s (uptime %.1f s)" % (r["after_s"], r["uptime"]))
        elif a.command == "raw":
            req = json.loads(a.request)
            b.next_id += 1
            req.setdefault("id", b.next_id)
            b.send(req)
            while True:
                msg = b.line(float(req.get("timeout", 60)) + 10)
                if msg is None:
                    raise Failed("the game closed the connection", 2)
                if msg.get("id") == req["id"] and "ok" in msg:
                    print(json.dumps(msg))
                    return 0 if msg["ok"] else 1
        elif a.command == "watch":
            # (--log alone: the log lines only; with --events, those and the log lines)
            events = [e for e in (a.events or ("" if a.log else "present,state")).split(",") if e]
            if a.log and "log" not in events:
                events.append("log")
            check(b.request("subscribe", events=events, every=a.every, log=a.log,
                            flags=a.flags.split(",") if a.flags is not None else None,
                            changes=True if a.changes else None))
            n = 0
            while a.count is None or n < a.count:
                msg = b.line(3600 * 24)
                if msg is None:
                    return 0
                if "event" not in msg:
                    continue
                n += 1
                if a.json:
                    print(json.dumps(msg), flush=True)
                elif msg["event"] == "log":
                    print("log    %s" % plain_text(msg["line"]), flush=True)
                elif msg["event"] == "state":
                    print("state  frame %d: %s -> %s" % (msg["frame"], msg["from"], msg["state"]), flush=True)
                elif msg["event"] == "present":
                    print("frame  %d  fps %.1f  %s" % (msg["frame"], msg["fps"], msg["state"]), flush=True)
                elif msg["event"] == "map":
                    print("map    frame %d: %s -> %s" % (msg["frame"], plain_text(msg["from"] or "-"), plain_text(msg["map"])),
                          flush=True)
                elif msg["event"] == "oot":
                    print("game   " + json.dumps(msg["game"], ensure_ascii=True), flush=True)
                else:
                    print(json.dumps(msg), flush=True)
        return 0
    finally:
        b.close()


def launch(a):
    changes = {}
    for kv in a.env:
        if "=" not in kv:
            raise Failed("--env NAME=VALUE", 2)
        k, v = kv.split("=", 1)
        changes[k] = v
    session = start_session(a.game_dir, run=a.run, root=a.root, exe=a.exe, image=a.image, romfs=a.romfs, user=a.user,
                            cwd=a.cwd, data=a.data, save=a.save, slot=a.slot, profile=a.profile, headless=a.headless,
                            sock=a.sock, env=changes, ready=a.ready, timeout=a.timeout,
                            wrapper=json.loads(a.wrapper) if a.wrapper else None, repo_save=a.repo_save, repo=a.repo)
    info = session.info()
    print(json.dumps(info) if a.json else info["socket"])
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--sock", help="the bridge's socket (default $HOS_CONTROL)")
    p.add_argument("--run", help="an existing run folder to re-attach to")
    p.add_argument("--json", action="store_true", help="the replies as JSON lines")
    sub = p.add_subparsers(dest="command", required=True)

    def finite_float(text):
        value = float(text)
        if not math.isfinite(value):
            raise argparse.ArgumentTypeError("expected a finite number")
        return value

    def inputs(q):
        q.add_argument("--frames", type=int, default=8)
        q.add_argument("--at", type=int, default=0, help="frames from the next controller read")
        q.add_argument("--exclusive", action=argparse.BooleanOptionalAction, default=None,
                       help="the player's own devices left out meanwhile (default for test runs)")
        q.add_argument("--no-wait", action="store_true", help="reply as soon as it is queued")
        q.add_argument("--timeout", type=finite_float)

    q = sub.add_parser("launch", help="start a game folder with the bridge on")
    q.add_argument("game_dir")
    q.add_argument("--exe", help="the game executable, instead of finding it in GAME_DIR")
    q.add_argument("--image", help="an explicit image.bin (the game's positional image argument)")
    q.add_argument("--romfs", help="the game's --romfs folder")
    q.add_argument("--user", help="the game's --user folder, on Linux and Windows")
    q.add_argument("--data", help="XDG_DATA_HOME for the run (its saves)")
    which = q.add_mutually_exclusive_group()
    which.add_argument("--save", help="a save folder (<data>/longshot/save/<user id>) copied in first (Linux); the source is "
                                  "only read")
    which.add_argument("--repo-save", metavar="NAME", help="a save in the repository (assets/saves/NAME): its slot_00 "
                       "alone copied in first, as --save (the game makes account.sav and option.sav)")
    q.add_argument("--repo", metavar="DIR", help="with --repo-save: the checkout whose assets/saves it is in (default this "
                   "script's)")
    q.add_argument("--slot", help="with --save: that slot alone (03 or slot_03), so Continue loads it")
    q.add_argument("--run", default=argparse.SUPPRESS, help="a new or empty run folder")
    q.add_argument("--root", help="parent for unique run folders (default runs/control in the development folder)")
    q.add_argument("--profile", choices=["default", "test"], default="default")
    q.add_argument("--ready", choices=["bridge", "present", "play"], default="bridge")
    q.add_argument("--cwd", help="working/cache folder; needs --image")
    q.add_argument("--wrapper", help="JSON argv prefix around the lifetime supervisor, for admission")
    q.add_argument("--headless", action="store_true")
    q.add_argument("--env", action="append", default=[], metavar="NAME=VALUE")
    q.add_argument("--timeout", type=float, default=600, help="seconds to wait for the bridge (shaders may be prepared first)")
    sub.add_parser("status")
    q = sub.add_parser("press")
    q.add_argument("buttons", help="A, A+B, ZL+LUP, LX=-16000 ...")
    inputs(q)
    q = sub.add_parser("stick")
    q.add_argument("which", choices=["L", "R", "l", "r"])
    q.add_argument("x", type=finite_float)
    q.add_argument("y", type=finite_float)
    inputs(q)
    q = sub.add_parser("sequence")
    q.add_argument("script", help="<at>:<buttons>[/<frames>],...")
    q.add_argument("--hos", action="store_true", help="times in seconds and holds in ms, as HOS_INPUT_SCRIPT")
    q.add_argument("--exclusive", action=argparse.BooleanOptionalAction, default=None)
    q.add_argument("--no-wait", action="store_true")
    q.add_argument("--timeout", type=finite_float)
    q = sub.add_parser("release", help="let go of this client's queued inputs and those of clients gone")
    q.add_argument("--all", action="store_true", help="every client's, also those still connected")
    q = sub.add_parser("shot")
    q.add_argument("path", nargs="?")
    q.add_argument("--after", type=int, help="the first present after this frame")
    q.add_argument("--timeout", type=finite_float)
    q = sub.add_parser("wait")
    q.add_argument("event", choices=["present", "frame", "in_play", "state", "loading_start", "loading_end", "change", "map"])
    q.add_argument("--frames", type=int)
    q.add_argument("--frame", type=int)
    q.add_argument("--state")
    q.add_argument("--map", help="map: until the game is in this map (default: any other map than now)")
    q.add_argument("--timeout", type=finite_float, default=60)
    q = sub.add_parser("option")
    q.add_argument("action", choices=["get", "set"])
    q.add_argument("name", nargs="?")
    q.add_argument("value", nargs="?")
    q = sub.add_parser("watch")
    q.add_argument("--events", help="present,state,log,map,oot (default present,state; with --log alone, log)")
    q.add_argument("--every", type=int, default=60)
    q.add_argument("--log", help="log lines that contain this text (adds log to the events)")
    q.add_argument("--count", type=int, help="stop after this many events")
    q.add_argument("--flags", help="with --events oot: comma-separated flag names or 0x and eight hex digits")
    q.add_argument("--changes", action="store_true", help="with --events oot: changed Bool/Int/Enum/UInt flags")
    q = sub.add_parser("mem")
    q.add_argument("action", choices=["read", "write"])
    q.add_argument("addr", help="a guest address, decimal or 0x...")
    q.add_argument("size", nargs="?", help="read: how many bytes")
    q.add_argument("hex", nargs="?", help="write: the bytes as hex")
    q.add_argument("--format", default="hex", choices=["hex", "base64", "u8", "u16", "u32", "u64", "f32"],
                   help="read: how to print the bytes")
    q = sub.add_parser("teleport", help="(OoT) the debug teleport: a destination's index or map; none lists them")
    q.add_argument("to", nargs="?")
    q.add_argument("--timeout", type=finite_float, default=10, help="seconds for the game to take it (it does in play)")
    q.add_argument("--arrive", type=finite_float, metavar="SECS", help="then wait up to SECS until the game is in the map")
    q.add_argument("--no-wait", action="store_true", help="reply once asked, without whether the game took it")
    q = sub.add_parser("age", help="(OoT) the Debug tab's Switch age: child or adult (none: the other age)")
    q.add_argument("to", nargs="?", choices=["child", "adult"])
    q.add_argument("--timeout", type=finite_float, default=10, help="seconds for the game to take it (it does in play)")
    q.add_argument("--arrive", type=finite_float, metavar="SECS", help="then wait up to SECS until the map has reloaded (its "
                   "load ended)")
    q.add_argument("--no-wait", action="store_true", help="reply once asked, without whether the game took it")
    q = sub.add_parser("saves", help="the save folder the game uses: its files, newest slot first")
    q.add_argument("--repo", nargs="?", const="", metavar="DIR", help="the saves in the repository instead (assets/saves, "
                   "no game needed): each with its README's first line; DIR another checkout")
    q = sub.add_parser("slots", help="the save slots: last written, sizes, scene; * the newest (Continue's)")
    q.add_argument("--thumbs", metavar="DIR", help="each slot's file-select picture written there as slot_NN.jpg")
    sub.add_parser("quit")
    q = sub.add_parser("relaunch", help="the game starts again (the Insert menu's Restart now); back when it answers")
    q.add_argument("--timeout", type=float, default=120, help="seconds for the game started again to answer")
    q = sub.add_parser("raw")
    q.add_argument("request", help="a JSON request object")
    q = sub.add_parser("runs", help="list run manifests under a root")
    q.add_argument("root", nargs="?")
    sub.add_parser("show", help="show --run's manifest")
    sub.add_parser("finish", help="mark an orphaned dead run finished (exit code unknown)")
    q = sub.add_parser("stop", help="ask --run's owner to quit and wait; never kill an externally attached process")
    q.add_argument("--timeout", type=float, default=10)
    q.add_argument("--terminate", action="store_true", help="allow the original owner to terminate, then kill, its child")
    q = sub.add_parser("log", help="read --run's log without a bridge command")
    q.add_argument("--lines", type=int, default=100)
    q.add_argument("--since", type=int, help="byte cursor returned by the previous read")
    q.add_argument("--bytes", type=int, default=65536, help="read/output bound, at most 1048576")
    q.add_argument("--expect", help="wait for text after --since")
    q.add_argument("--regex", action="store_true")
    q.add_argument("--timeout", type=float, default=10)
    a = p.parse_args(argv)
    try:
        return run(a)
    except Failed as e:
        print("longshot_ctl: %s" % e, file=sys.stderr)
        return e.code
    except (BrokenPipeError, ConnectionResetError):
        print("longshot_ctl: the game closed the connection", file=sys.stderr)
        return 2
    except json.JSONDecodeError as e:
        print("longshot_ctl: not JSON: %s" % e, file=sys.stderr)
        return 2
    except OSError as e:
        print("longshot_ctl: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
