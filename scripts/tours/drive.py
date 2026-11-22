"""A game driven along the pipeline tour's route: started with the bridge on and the live input file (start_game), Game
(its bridge, the frames counted, the list's growth), the teleports and the age switch with the game's own answers, a new
game to control of Link, and the route's steps (play)."""
import os
import signal
import struct
import subprocess
import time

import longshot_bridge
from tours.memory import MemoryStop, held_mb, meminfo, proc_mb, run_problem
from tours.route import (ACT, BOOTS_OFF, DIVE, DIVE_AT, GAME_OVER_AT, GAME_OVER_FRAMES, GAME_OVER_PRESS_FROM, LOOK,
                         NIGHT_AT, SETTLE_MAX, SETTLE_QUIET, TIME_ACTION, UNDERWATER_GEAR, UNSTICK, dungeons_from,
                         item_stops, items_unlock, pause_pick, stop_labels)

class Failed(Exception):
    pass


# ---- the game, through the bridge ----

class Game:
    """The game behind its control bridge: the bridge's replies (each carries the frame), what was sent, at which frame.
    bridge: an object with request(cmd, timeout=None, **args) -> reply dict (scripts/longshot_bridge.py Bridge, or a test's).
    live: the live input file, for the Debug tab's inventory (the one thing the bridge does not do). list_path: the game's
    pipeline state list. proc: the game's process (None: not ours, as in a test)"""
    def __init__(self, bridge, run_dir, list_path, live=None, proc=None, mem=None, dry=False, cheat_gap=0.032):
        self.b, self.run_dir, self.list_path, self.live, self.proc = bridge, run_dir, list_path, live, proc
        self.mem, self.dry, self.cheat_gap = mem, dry, cheat_gap
        self.peak = 0           # (the most the game held, MB: anonymous + shared memory)
        self.checked = 0.0
        self.steps = open(os.path.join(run_dir, "steps.txt"), "w", encoding="utf-8")
        self.t0 = time.monotonic()
        self.frame = 0          # (the game's presents so far, as the last reply said)
        self.marks = []         # (the current step's parts: [(what, records so far)])
        self.last = None        # (the last status)
        self.profile = None     # (--vk-profile: the renderer's counters, Profile)

    def now(self):
        return time.monotonic() - self.t0

    def note(self, what):
        self.steps.write("%8.1f s %7d  %s\n" % (self.now(), self.frame, what))
        self.steps.flush()

    def alive(self):
        return self.dry or self.proc is None or self.proc.poll() is None

    def check(self):
        """The game still there, and within the memory guard's limits"""
        if not self.alive():
            raise Failed("the game ended")
        if self.mem and self.proc is not None and time.monotonic() - self.checked >= 1:
            self.checked = time.monotonic()
            why = run_problem(self.mem, meminfo(), self.held())
            if why:
                raise MemoryStop(why)

    def ask(self, cmd, timeout=None, **args):
        """One request; its reply (ok or not), the frame taken from it"""
        self.check()
        try:
            r = self.b.request(cmd, timeout=timeout, **args)
        except longshot_bridge.Failed as e:   # (the game closed the bridge, or did not answer: a crash, mostly in a wait)
            raise Failed("%s: the bridge: %s%s" % (cmd, e, "" if self.alive() else " (the game ended)"))
        if isinstance(r.get("frame"), int):
            self.frame = max(self.frame, r["frame"])
        return r

    def must(self, cmd, timeout=None, **args):
        r = self.ask(cmd, timeout=timeout, **args)
        if not r.get("ok"):
            raise Failed("%s: %s: %s" % (cmd, r.get("error"), r.get("message", "")))
        return r

    def status(self):
        """{map, event, loading, age, state, draws} from the bridge's status (event, loading and age None before Link was
        first seen)"""
        r = self.must("status")
        game = r.get("game") or {}
        self.last = {"map": r.get("map"), "event": game.get("event"), "loading": game.get("loading"), "age": game.get("age"),
                     "state": r.get("state"), "draws": r.get("draws") or 0, "version": r.get("version"),
                     "renderer": r.get("renderer"), "fresh": fresh(r)}
        return self.last

    def frames(self, n, limit_s=300):
        """n frames of the game, in pieces of two seconds at most (the memory guard looks between them)"""
        if self.dry:
            return
        end, until = time.monotonic() + limit_s, self.frame + n
        while self.frame < until:
            left = min(until - self.frame, 120)
            r = self.ask("wait", timeout=max(5, min(60, limit_s)), event="present", frames=left)
            if not r.get("ok") and r.get("error") != "timeout":
                raise Failed("wait: %s: %s" % (r.get("error"), r.get("message", "")))
            if time.monotonic() > end:
                raise Failed("%d frames did not come in %d s" % (n, limit_s))

    def press(self, script, label=None):
        """A press script in HOS_INPUT_SCRIPT's terms (seconds and milliseconds of game time), the reply once it is over"""
        self.note(label or script)
        if self.dry:
            return
        r = self.ask("sequence", script=script, units="hos", exclusive=True)
        if not r.get("ok"):
            raise Failed("sequence %s: %s: %s" % (script, r.get("error"), r.get("message", "")))

    def tap(self, buttons, frames=6):
        """A press not waited for (the title's A)"""
        if not self.dry:
            self.ask("press", buttons=buttons, frames=frames, wait=False)

    def cheat(self, action):
        """A Debug tab action through the live input file: one a live poll, the game keeping the last one a poll read. The game
        reads the file at a frame's input once 30 ms have passed since it last did (host/nn_hid.c live_input): so the next line
        waits those 30 ms (cheat_gap), then three presents, the first two of which can be of frames that read their input before
        the line was there (with the render thread, Threaded rendering, the game is a frame ahead of its presents). A frame
        slowed by a pipeline build is waited for, as a wall-clock pause alone did not"""
        self.note("!cheat %d" % action)
        if self.dry:
            return
        with open(self.live, "a", encoding="utf-8") as f:
            f.write("!cheat %d\n" % action)
        time.sleep(self.cheat_gap)
        self.frames(3)

    def held(self):
        """The game's memory that cannot be dropped, MB (its most so far kept in peak); None where it cannot be read"""
        try:
            with open("/proc/%d/status" % self.proc.pid) as f:
                mb = held_mb(f.read())
        except (OSError, AttributeError, ValueError):
            return None
        self.peak = max(self.peak, mb)
        return mb

    def memory(self):
        """The game's resident memory in MB (Linux): {rss, anon, shmem} (guest memory is shared: shmem), {} elsewhere"""
        names = {"VmRSS": "rss", "RssAnon": "anon", "RssShmem": "shmem"}
        mb = proc_mb("/proc/%d/status" % self.proc.pid, names) if self.proc is not None else None
        return {names[k]: v for k, v in (mb or {}).items()}

    def list_len(self):
        """The records in the list so far (its size: a 12-byte head, then records of the size it gives)"""
        try:
            with open(self.list_path, "rb") as f:
                head = f.read(12)
            return (os.path.getsize(self.list_path) - 12) // max(1, struct.unpack_from("<I", head, 8)[0])
        except (OSError, struct.error):
            return 0

    def mark(self, what):
        """A step's part (arrived, looked, items used...): the list's length now, for `value --sub`"""
        self.marks.append((what, self.list_len()))

    def list_size(self):
        try:
            return os.path.getsize(self.list_path)
        except OSError:
            return 0

    def settle(self, quiet, most, step=15):
        """Frames until the list has had no new pipeline for `quiet` frames, `most` at most"""
        if self.dry:
            return
        start, size, since = self.frame, self.list_size(), self.frame
        while self.frame - start < most:
            self.frames(step)
            now = self.list_size()
            if now != size:
                size, since = now, self.frame
            elif self.frame - since >= quiet:
                return

    def calm(self, want, limit_frames, every=15):
        """Until the game's state is what is wanted (want(state)), no map loading and no event playing: that state"""
        start = self.frame
        while True:
            st = self.status()
            if st["map"] and want(st) and st["loading"] is False and st["event"] is False and st["fresh"]:
                return st
            if self.frame - start > limit_frames:
                raise Failed("not settled after %d frames (last state %s)" % (self.frame - start, st))
            self.frames(every)


STALE_FRAMES = 10   # (a status's game snapshot taken more presents than this before its reply: kept from before a load or a pause)


def fresh(r):
    """A status reply's game snapshot is of now (docs/dev/reference/control-bridge.md: compare game.frame with the reply's frame; a snapshot kept
    through a load says nothing of control)"""
    taken, now = (r.get("game") or {}).get("frame"), r.get("frame")
    return isinstance(taken, int) and isinstance(now, int) and 0 <= now - taken <= STALE_FRAMES


def calm_after(g, want, start, limit_frames):
    """g.calm with what is left of a step's frames"""
    return g.calm(want, max(60, limit_frames - (g.frame - start)))


# what in a refusal's message says that asking again will never do (host/hos_main.c oot_teleport's teleport.why: no map for
# Link's age there, no such destination, a request the game could not build, Link's age not in its data, an input recording
# playing, which takes its own and none of the bridge's: host/input_movie.c); the rest (an event playing, a map loading,
# another teleport in the last 8 s, the event manager not there yet) pass
PERMANENT = ("no such destination", "has no map", "could not be", "not in the game's data", "out of memory",
             "an input recording is playing")
# (the refusal of a teleport within 8 s of the last, counted in wall time (hos_now_ns): waited out, not pressed at)
COOLDOWN = "another teleport is still in progress"
# (a sent request the game's event manager did not take: waited for this long before asking again, not arrive_s's 60 s)
NOT_TAKEN_S = 5


def map_change(g, cmd, want, limit_frames=60 * 60 * 3, arrive_s=60, reload=False, **args):
    """A teleport or the age switch through the bridge, asked again while the game is busy (refused: an event playing, a map
    loading, another teleport in the last 8 s; timeout: Link not being updated; cancelled), with presses that end what holds
    it back (a conversation, a prompt) once it has been busy for a while; done once the game is in the map wanted (want(state)),
    nothing loading and no event playing (the arrival's own scene over). Returns that state. A refusal that names something
    asking again cannot change ends it (Failed), as does a sent request that does not move Link five times. A request the
    game's event manager did not take (accepted false: right after a save loads, or a map that is not left) is waited for
    NOT_TAKEN_S, not arrive_s: Link will not move for it, and asking again sooner gets there sooner"""
    if g.dry:
        g.note("%s %s" % (cmd, args))
        return {"map": "?"}
    start, busy_since, unstick, unmoved = g.frame, None, 0, 0
    while True:
        if g.frame - start > limit_frames:
            raise Failed("%s %s: not done after %d frames (last state %s)" % (cmd, args, g.frame - start, g.last))
        g.note("%s %s" % (cmd, args))
        r = g.ask(cmd, timeout=10, **args)
        if r.get("ok"):
            if cmd == "age" and r.get("sent") is False:   # (Link is that age already)
                return calm_after(g, want, start, limit_frames)
            if r.get("sent"):
                # (reload: a teleport to the map Link is in, whose wait for that map is met at once: the load's end instead)
                wait = {"event": "map", "map": r.get("map")} if cmd == "teleport" and not reload else {"event": "loading_end"}
                w = g.ask("wait", timeout=arrive_s if r.get("accepted", True) else min(arrive_s, NOT_TAKEN_S), **wait)
                if w.get("ok"):
                    return calm_after(g, want, start, limit_frames)
                st = g.status()   # (the wait ran out: arrived anyway, or the game let the request go)
                if want(st):
                    return calm_after(g, want, start, limit_frames)
                unmoved += 1
                g.note("sent, not moved (accepted %s, %s)" % (r.get("accepted"), st))
                if unmoved >= 5:
                    raise Failed("%s %s: sent five times, the map did not change (as in the Haunted Wasteland)" % (cmd, args))
                g.frames(120)
                continue
            raise Failed("%s %s: an answer the tour does not know: %s" % (cmd, args, r))
        error, message = r.get("error"), r.get("message", "")
        if error == "refused" and any(p in message for p in PERMANENT):
            raise Failed("the game refused %s %s: %s" % (cmd, args, message))
        if error not in ("refused", "timeout", "cancelled", "busy"):
            raise Failed("%s %s: %s: %s" % (cmd, args, error, message))
        g.note("%s: %s" % (error, message))
        if error == "refused" and COOLDOWN in message:   # (the 8 s are wall time: nothing to press away, only to wait out)
            g.frames(60)
            continue
        busy_since = g.frame if busy_since is None else busy_since
        if g.frame - busy_since > 240:   # (an event that waits for a button: a conversation the tour started, a question)
            g.press(UNSTICK[unstick % len(UNSTICK)], "unstick")
            unstick += 1
        g.frames(30)


def teleport(g, index, banc, age, limit_frames=60 * 60 * 2):
    """The debug teleport to destination `index`, until Link stands in `banc` at `age`"""
    return map_change(g, "teleport", lambda st: st["map"] == banc and st["age"] == age, limit_frames, to=index)


def switch_age(g, age, limit_frames=60 * 60 * 3):
    return map_change(g, "age", lambda st: st["age"] == age, limit_frames, to=age)


def new_game(g, limit_s=1200, title_draws=15, first_frame_s=1800):
    """From no save: A at the title until New Game starts, then through the opening (A now and then: its text), until the game's
    state says Link is in a map with no event playing and nothing loading, three times in a row a second apart: control.
    Returns the phases' (frame, s)"""
    phases = {}
    if g.dry:
        g.note("new game")
        return phases
    end = time.monotonic() + limit_s
    # (a first start prepares the shaders before its first frame, and the game reads no controller until then)
    first = g.ask("wait", timeout=first_frame_s, event="frame", frame=1)
    if not first.get("ok"):
        raise Failed("new game: no first frame in %d s (%s)" % (first_frame_s, first.get("message", "")))
    next_a, calm = 0, 0
    while calm < 3:
        st = g.status()
        if "title" not in phases and (st["draws"] >= title_draws or st["state"] in ("menu", "play")):
            phases["title"] = (g.frame, round(g.now(), 1))
            g.note("title")
        if "title" in phases:
            if st["map"] and "player" not in phases:
                phases["player"] = (g.frame, round(g.now(), 1))
                g.note("Link is there: %s" % st)
            calm = calm + 1 if st["map"] and st["event"] is False and st["loading"] is False and st["fresh"] else 0
            if calm == 0 and g.frame >= next_a:
                g.tap("A")
                next_a = g.frame + 45
        if time.monotonic() > end:
            raise Failed("new game: %s after %d s (last state %s)" % ("no control" if "title" in phases else "no title", limit_s, st))
        g.frames(60 if calm else 20)
    phases["control"] = (g.frame, round(g.now(), 1))
    g.note("control: %s" % g.last)
    return phases


def start_game(game, run_dir, data, env, prefix=(), timeout=900):
    """The game in its folder with the bridge on (a socket in a 0700 folder of the run's own, docs/dev/reference/control-bridge.md) and the live input
    file: (process, bridge, live file path). Waits until the bridge answers (a first start prepares the shaders first)"""
    sock_dir = os.path.join(run_dir, "ctl")
    os.makedirs(sock_dir, mode=0o700, exist_ok=True)
    os.chmod(sock_dir, 0o700)
    sock = os.path.join(sock_dir, "control.sock")
    if len(sock.encode()) > 107:   # (a Unix socket's path is at most 107 bytes)
        raise SystemExit("pipeline_tour: %s is too long for a socket: give a shorter --out" % sock)
    live = os.path.join(run_dir, "live.txt")
    open(live, "w").close()
    user = os.path.join(data, "user")   # (the saves: the run's own, whatever the folder's user/ is or links to)
    os.makedirs(user, exist_ok=True)
    no_env = os.path.join(run_dir, "empty-env.txt")   # (HOS_ENV_FILE: not the folder's longshot_env.txt, as longshot_bridge's test profile)
    open(no_env, "w").close()
    full = run_environ(os.environ, env)
    full.update({"XDG_DATA_HOME": data, "HOS_INPUT_LIVE": live, "HOS_CONTROL": sock, "HOS_ENV_FILE": no_env})
    exe = longshot_bridge.game_exe(game)
    with open(os.path.join(run_dir, "log.txt"), "wb") as logf:   # (the game has its own copy of the handle)
        proc = subprocess.Popen(list(prefix) + [exe, "--user", user], cwd=game, env=full, stdout=logf, stderr=subprocess.STDOUT,
                                stdin=subprocess.DEVNULL, start_new_session=True)
    end = time.monotonic() + timeout
    try:
        while True:
            if proc.poll() is not None:
                raise Failed("the game ended (%d) before its bridge answered: %s" % (proc.returncode, os.path.join(run_dir, "log.txt")))
            try:
                b = longshot_bridge.Bridge(sock, 5)
                if b.request("status", timeout=5).get("ok"):
                    return proc, b, live
                b.close()
            except longshot_bridge.Failed:
                pass
            if time.monotonic() > end:
                raise Failed("the game's bridge did not answer in %d s" % timeout)
            time.sleep(0.1)   # (no frames to count yet: the bridge is not there)
    except BaseException:   # (no bridge, or Ctrl+C while waiting: the game is not left running in the folder put back under it)
        stop_game(proc, None, 3)
        raise


def run_environ(current, env):
    """The game's environment: this one's without its own Longshot switches (a player's RX_VK_NO_STATE_LIST=1 in the shell would
    change the list unsaid), then the run's (env, which report.json records)"""
    full = {k: v for k, v in current.items() if not k.startswith(("HOS_", "RX_", "A64_", "OOT_"))}
    full.update(env)
    return full


def stop_game(proc, bridge=None, grace=15):
    """The game asked to quit through the bridge (it flushes its log and lists as when its window closes), then ended"""
    if proc is None or proc.poll() is not None:
        return
    if bridge is not None:
        try:
            bridge.request("quit", timeout=5)
        except Exception:   # (the game may be gone, or not answer: it is ended below)
            pass
    try:
        proc.wait(grace)
        return
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(proc.pid, signal.SIGTERM)
        proc.wait(grace)
    except (subprocess.TimeoutExpired, ProcessLookupError):
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        proc.wait()


def play(g, a, steps, rows_out, count, args):
    """The route's steps on game g. count(label) adds a report row. args: settle_quiet and settle_max, a stop's settle (frames)"""
    misses = 0
    dived = False
    quiet, most = getattr(args, "settle_quiet", SETTLE_QUIET), getattr(args, "settle_max", SETTLE_MAX)
    picks = item_stops(a)
    for at, step in enumerate(steps):
        g.at = at
        kind = step[0]
        if g.mem and not g.dry and kind == "visit" and at and stop_labels(a, steps[:at]):
            held = g.held()   # (between stops: a game past the soft limit ends here, a fresh one does the rest)
            if held is not None and held > g.mem["soft_mb"]:
                raise MemoryStop("the game holds %d MB after a stop, a fresh game from here on (over %d)" % (held, g.mem["soft_mb"]),
                                 soft=True)
        if kind == "newgame":
            phases = new_game(g)
            if g.profile is not None and not g.dry:   # (control: the counters from here on are play's)
                g.profile.reset()
                g.note("vk profile: counters cleared")
            count("new game: title, opening, Link's house")
            rows_out[-1]["phases"] = phases
        elif kind == "items":
            for n in items_unlock(a):   # (one a live poll: the game keeps the last action a poll read, host/nn_hid.c)
                g.cheat(n)
            g.frames(30)
            count("every item")
        elif kind == "press":
            g.press(step[1], step[3])
            g.settle(30, 120)
            count(step[3])
        elif kind == "age":
            switch_age(g, "adult")
            count("age switched: adult")
        elif kind == "gameover":
            banc = a["bancs"][step[1]][0]
            teleport(g, step[1], banc, "child")
            count("%s (child), hearts not protected" % GAME_OVER_AT)
            end = g.frame + GAME_OVER_FRAMES
            while g.frame < end and not g.dry:   # (the heat; then A: Continue)
                g.frames(180)
                if g.frame > end - GAME_OVER_FRAMES + GAME_OVER_PRESS_FROM:
                    g.tap("A")
            count("Game Over")
        elif kind == "visit":
            _, age, i, k = step
            name = a["teleports"][i][0]
            banc = a["bancs"][i][0 if age == "child" else 1]
            try:
                where = teleport(g, i, banc, age)
            except Failed as e:   # (one stop missed, said in the report; three in a row end the run)
                misses += 1
                g.note("missed: %s" % e)
                count("%s (%s): MISSED" % (name, age))
                if misses >= 3:
                    raise Failed("three stops in a row missed: %s" % e)
                continue
            misses = 0
            g.note("arrived: %s" % where)
            g.mark("arrive")
            if name in DIVE_AT:
                g.press(DIVE, "dive")
            g.press(LOOK, "look")
            g.mark("look")
            if name in DIVE_AT and age == "adult" and not dived:
                dived = True
                count("%s (%s)" % (name, age))
                g.press(UNDERWATER_GEAR, "iron boots, Zora tunic")
                try:   # (arrived again, in the water: with the boots Link sinks to the bottom)
                    map_change(g, "teleport", lambda st: True, 60 * 60, reload=True, to=i)
                    g.frames(300)
                    g.press(LOOK, "look")
                    g.settle(quiet, most)
                    count("%s (%s) underwater" % (name, age))
                except Failed as e:
                    g.note("underwater missed: %s" % e)
                g.press(BOOTS_OFF, "boots off")
                continue
            if (age, i) in picks:   # (once an age, and where a pass of the stop's own could meet them: ITEMS_ONCE, ITEMS_PASSES)
                k = picks.index((age, i))
                pick, _ = pause_pick(k)
                g.press(pick, "item %d from the pause menu" % (k + 1))
                g.press(ACT, "sword, shield, item")
                g.mark("items")
            g.settle(quiet, most)   # (on once no new pipeline came for `quiet` frames, `most` at most)
            count("%s (%s)" % (name, age))
            if name in NIGHT_AT and i < dungeons_from(a):
                g.cheat(TIME_ACTION + 22)
                g.frames(60)
                g.press(LOOK, "look at night")
                g.settle(quiet, most)
                count("%s (%s) at night" % (name, age))
                g.cheat(TIME_ACTION + 12)
                g.frames(60)
