"""The pipeline tour: plays the game unattended, from a new save through every debug Teleport destination as child and as
adult, with the items, their effects and the menus, so that its graphics pipelines are met and written to the pipeline state list
(gpu/vk/vk_draw.cpp: longshot_pipeline_states.bin). The parts' lists become the tour's source of the list a release ships,
assets/pipeline_states/tour.bin.xz and tour.md (nx2/pipeline_states.py tour): the first start prepares those pipelines before the
title. The game is driven through the control bridge (host/control.c, docs/dev/reference/control-bridge.md).

    python -I scripts/pipeline_tour.py tour --game <game folder> --out <dir> [--name play] [--parts ...] [--publish] [--env ...] [--ini ...]
                                            [--vk-profile <dir>] [--settle-quiet FRAMES] [--settle-max FRAMES] [--compare-to <list>]
    python -I scripts/pipeline_tour.py run  --game <game folder> --out <dir> [--name play] [--part <part>] [--first <stop>] [--stops N]
                                            [--skip-opening] [--env NAME=VALUE ...] [--ini key=value ...] [--window WxH] [--dry-run]
                                            [--vk-profile <dir>] [--settle-quiet FRAMES] [--settle-max FRAMES]
    python -I scripts/pipeline_tour.py merge-profile <vk-profile dir> [--out <file>] [--publish] [--llvm-profdata <path>]
    python -I scripts/pipeline_tour.py merge --out <file> <list> ...
    python -I scripts/pipeline_tour.py compare <new list> <old list>
    python -I scripts/pipeline_tour.py value <run dir> ... [--sub] [--floor 10]

run: one game. The game folder is used as it is, so give it a copy (cp -a --reflink=always): its cache/longshot_pipelines.bin (the
pipeline notes), cache/longshot_pipeline_states.bin (and _v1) and longshot_pipeline_states_shipped.bin are moved aside for the run
(a precompiled pipeline is not noted again, and only what this run meets counts) and put back after it. The run starts with no save
at all (an empty data folder of its own, <out>/<name>/data): the title, New Game, the opening, then control of Link. Then every item, and each destination: the
teleport, its loading, the camera all around, then on once no new pipeline came for --settle-quiet frames (60: a second of game
time; --settle-max, 600, at most). The sword, the shield and an item from the pause menu are used once an age (ITEMS_ONCE) and at
the few stops with passes of their own an item's draws could meet (ITEMS_PASSES: water, the Lens of Truth, heat haze), not at
every stop: a pipeline is a program and its fixed draw state, and a scene's light and fog are inputs to the same pipelines.

The game is driven through the control bridge (HOS_CONTROL, a socket in the run's folder): presses timed in the game's frames
(sequence), the debug teleport by destination and the age switch with the game's own answer (refused, cancelled, sent; then the map
it arrives in), and the game's state (the map, an event playing, a map loading, Link's age: status). The one thing the bridge does
not do, the Debug tab's inventory, goes through the live input file (HOS_INPUT_LIVE, "!cheat <action>"). No controller is opened
(HOS_NO_SDL) and nothing reaches the desktop. As fast as the game goes: no window (HOS_HEADLESS), the smallest output (640x360, the
game's own upscaler at 50%: a pipeline's key has formats and state, not sizes), frame pacing off, no frame generation, the cap
Uncapped with every frame a 60th of a second of game time (HOS_FPS_MAX=1000, HOS_FPS_STEP=0), text at once, and the opening skipped
by the game itself (HOS_SKIP_OPENING) in every part but the one that watches it. Every wait on the game counts its frames (the
bridge's wait present), so a faster game waits less; only the inventory's live lines are spaced in wall time as well (the game
reads that file every 30 ms). Each step has a limit: a step that does not happen fails the run, it does not wander. --env and
--ini run it under other settings (RX_VK_FORCE_MESH_FALLBACK=1, dlss=0, hdr=1, HOS_HANDHELD=1); the options file is restored.
--window shows it in a window instead.

tour: the parts one after another, one game at a time, in a copy of the game folder (reflinked, else romfs/ hard-linked and the
rest copied), in games of --chunk stops (a
fresh game each), at nice 19 on --cpus, each under the memory guard (MEM: it waits for memory before a game, and ends a game that
holds too much or leaves the machine too little, the next game going on from there; on Linux the game runs in a systemd scope
with a MemoryMax); merged (--compare-to <list>: what the merged list lacks of an older one); --publish writes the repository's
tour list from them (nx2/pipeline_states.py tour, with the GPU, build, settings and route the runs logged).

--vk-profile <dir> (run and tour): the renderer's play profile recorded on the tour's route (docs/dev/developing.md, "Record
the renderer's play profile"). The game folder's renderer must be an RX_VK_PGO=gen one (a counting pack: RX_VK_PGO=gen python -I
scripts/dev.py packs pack <base> vk-count): it is refused otherwise. Each game runs with RX_VK_PGO_FILE set; once it has control
of Link the counters are cleared (<file>.reset: the loading, the prepare and the title left out), and at its end, failed or not,
they are written (<file>.dump) and moved to <dir>/<run name>.profraw, with <run name>.json saying where they were recorded. A
renderer that takes neither request fails the run rather than leaving nothing. The pipeline lists are made as without it (the
counting renderer is slower, its pipelines the same). merge-profile then merges <dir>/*.profraw with the toolchain's
llvm-profdata into work/longshot_vk.profdata (what nx2/make_game.py builds with); --publish also writes
assets/profiles/longshot_vk.profdata.xz (xz -9e) and the note for assets/profiles/README.md (<dir>/README-note.md).

compare: two lists' pipelines against each other (either may be a tour's merged <out>/<name>.bin, a run's states.bin or a
tour.bin.xz): how many each has that the other lacks. A faster tour that loses nothing has none in the old list that the new lacks.

value: each step's pipelines over runs that the players' lists (the shipped list's contrib/ sources) and the other steps do not
have, its seconds, and which steps add too little to keep.

Each run writes <out>/<name>/: states.bin (its list), report.txt and report.json (the pipelines after each step, the new ones each
added, its frames and wall time), log.txt (the game's log) and steps.txt (what was sent, at which frame).

Linux (the memory guard reads /proc; the bridge is a Unix socket). Standard library only. The actions' numbers come from
host/hos_main.c, gpu/vk/oot_cheat_items.h and gpu/vk/vk_menu.cpp; the bridge's client is scripts/longshot_bridge.py;
the parts are in scripts/tours/ (route, drive, memory, lists, vkprofile).
"""
import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
from tours.drive import Failed, Game, play, start_game, stop_game  # noqa: E402
from tours.lists import compare, list_counts, merge, state_tool, value  # noqa: E402
from tours.memory import MEM, MemoryStop, scope_prefix, wait_memory  # noqa: E402
from tours.route import PARTS, SETTLE_MAX, SETTLE_QUIET, actions, item_stops, items_unlock, plan_stops, route, stop_labels  # noqa: E402
from tours.vkprofile import PGO_ENV, Profile, merge_profile, profile_end, refuse_uncounted  # noqa: E402

# (in the game folder: the pipeline notes and the lists play adds to are in cache/, the shipped list beside the game
# (gpu/vk/vk_draw.cpp state_list_path, pipe_notes_path; host/game_layout.c), and a build from before left its loose beside the
# game, which the game's start moves into cache/. longshot_computes.bin: the compute programs earlier play dispatched, which the
# game writes into the list when it opens it (vk_draw.cpp state_open_locked): aside too, or every run's list starts with them)
STATE_FILES = ("cache/longshot_pipelines.bin", "cache/longshot_pipeline_states.bin", "cache/longshot_pipeline_states_v1.bin",
               "cache/longshot_computes.bin", "longshot_pipeline_states_shipped.bin", "longshot_pipelines.bin",
               "longshot_pipeline_states.bin", "longshot_pipeline_states_v1.bin", "longshot_computes.bin")
LIST = "cache/longshot_pipeline_states.bin"
OPTIONS = "longshot_options.ini"

# no window, no sound, no controller, no pacing, the cap Uncapped with a 60th of a second of game time a frame, the smallest
# output, the bridge's input alone, text at once, no frame generation
BASE_ENV = {
    "HOS_HEADLESS": "1",
    "HOS_NO_SDL": "1",
    "HOS_AUDIO": "0",
    "HOS_INPUT_EXCLUSIVE": "1",
    "HOS_INPUT_SCRIPT_BACKGROUND": "1",
    "HOS_OOT_CHEATS": "1",            # (the Debug tab's tools: the inventory, through the live input)
    "HOS_OOT_PROTECT_HEARTS": "1",
    "HOS_NO_PACING": "1",
    "HOS_REALTIME_STEP": "0",         # (Game speed: Per frame)
    "HOS_FPS_MAX": "1000",            # (Uncapped's)
    "HOS_FPS_STEP": "0",              # (every frame a 60th of a second of game time: host/nn_vi.c)
    "HOS_FPS_LOG": "1",
    "HOS_OUTPUT_RES": "640x360",
    "HOS_TEXT_SPEED": "3",
    "HOS_DISCORD": "0",
    "RX_VK_FRAMEGEN": "0",
    "HOS_RT_POOL_MB": "320",     # (render target pool: 640x360 needs far less than the default 512 MB)
    "RX_VK_SHOT_EVERY": "600",   # (with RX_VK_SHOTS, set per run: a small picture every 600 frames, to see where a run got stuck)
    "RX_VK_SHOT_W": "320",
    "RX_VK_SHOT_H": "180",
}

# the options for the run (its own copy of longshot_options.ini, restored after it): the cheapest of each that does not take passes
# or formats away. The output 640x360 (as HOS_OUTPUT_RES), the upscaler the game's own (dlss=0), render scale 50% (resolution=4),
# no frame generation, sharpening or HDR, the cap Uncapped (frame_rate_cap=6), Game speed Per frame, no V-Sync, text at once. Set
# in the file before the game starts: several rows take effect only at a start
BASE_INI = {"output_res": "640x360", "dlss": "0", "upscaling": "0", "resolution": "4", "frame_generation": "0", "sharpness": "0",
            "hdr": "0", "frame_rate_cap": "6", "game_speed": "0", "vsync": "0", "text_speed": "3", "fps_counter": "0",
            "frame_pacing": "0", "discord_status": "0"}


def aside_name(rel):
    """A STATE_FILES path's name in the folder it is put aside in, and in the run's folder: cache/'s by their own names, a loose
    one beside the game that cache/ has too as loose_<name>"""
    if rel.startswith("cache/"):
        return rel[len("cache/"):]
    return "loose_" + rel if "cache/" + rel in STATE_FILES else rel


# ---- a run ----

def set_ini(path, pairs):
    """The options file with these keys set: the original's text, to put back (None: there was none, restore_ini removes it)"""
    old = open(path, encoding="utf-8").read() if os.path.exists(path) else None
    lines = (old or "").splitlines()
    for key, value in pairs:
        for k, line in enumerate(lines):
            if line.split("=", 1)[0] == key:
                lines[k] = "%s=%s" % (key, value)
                break
        else:
            lines.append("%s=%s" % (key, value))
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    return old


def restore_ini(path, old):
    """The options file as set_ini found it: its text, or no file (a folder never played keeps none of the run's options)"""
    if old is None:
        if os.path.exists(path):
            os.remove(path)
        return
    with open(path, "w", encoding="utf-8") as f:
        f.write(old)


def set_aside(game, aside):
    """The folder's pipeline notes and lists (STATE_FILES) moved into aside (in the game folder: a rename), so only what the run
    meets counts"""
    os.makedirs(aside)
    for name in STATE_FILES:
        if os.path.exists(os.path.join(game, name)):
            os.replace(os.path.join(game, name), os.path.join(aside, aside_name(name)))


def put_back(game, aside, run_dir):
    """The run's notes and lists moved into its folder (shutil.move: --out may be on another drive), then the folder's own put
    back from aside, each file on its own: one that cannot move is named and the rest still go. aside is removed once empty.
    Returns the problems ([] when all went back)"""
    problems = []
    for name in STATE_FILES:
        mine, kept = os.path.join(game, name), os.path.join(aside, aside_name(name))
        try:
            if os.path.exists(mine):
                shutil.move(mine, os.path.join(run_dir, aside_name(name)))
            if os.path.exists(kept):
                os.replace(kept, mine)
        except OSError as e:
            problems.append("%s: %s" % (name, e))
    try:
        os.rmdir(aside)
    except OSError as e:
        problems.append("%s left: %s (its files go back to %s by their names)" % (aside, e, game))
    return problems


def write_report(run_dir, rows, meta):
    total = rows[-1]["pipelines"] if rows else 0
    out = ["pipeline tour %s%s: %d pipelines, %d programs, %.0f s, %s frames%s" % (
        meta["name"], "" if meta.get("completed", True) else " (NOT COMPLETED: %s)" % (meta.get("error") or
                                                                                    "memory, %d stops left" % len(meta.get("left") or [])), total,
        rows[-1]["programs"] if rows else 0, meta["seconds"], meta.get("frames", "?"),
        ", %.0f frames a second" % meta["fps"] if meta.get("fps") else "")]
    if meta.get("peak_mb") or meta.get("memory_stop"):
        out.append("memory: the game held %s MB at most%s" % (meta.get("peak_mb") or "?", "; stopped: %s; left: %s" % (
            meta["memory_stop"], ", ".join(meta.get("left") or []) or "none") if meta.get("memory_stop") else ""))
    if "vk_profile" in meta:
        vk = meta["vk_profile"] or {}
        out.append("renderer profile: %s" % (vk.get("file") or "none (%s)" % (vk.get("why") or "the game did not start")))
    if meta.get("settle"):
        out.append("settle: on after %d frames with no new pipeline, %d at most" % tuple(meta["settle"]))
    out.append("settings: %s" % " ".join("%s=%s" % kv for kv in sorted(meta["env"].items())))
    if meta.get("ini"):
        out.append("options: %s" % " ".join(meta["ini"]))
    out.append("%7s %7s %7s %7s %6s %6s %6s %6s  %s" % ("at s", "took s", "frames", "total", "new", "progs", "rss MB", "shm MB", "step"))
    for r in rows:
        mem = r.get("memory") or {}
        out.append("%7.0f %7.1f %7d %7d %6d %6d %6s %6s  %s" % (r["seconds"], r.get("took", 0), r.get("frames", 0), r["pipelines"],
                                                             r["new"], r["programs"], mem.get("rss", "-"), mem.get("shmem", "-"),
                                                             r["stop"]))
    text = "\n".join(out) + "\n"
    with open(os.path.join(run_dir, "report.txt"), "w", encoding="utf-8") as f:
        f.write(text)
    with open(os.path.join(run_dir, "report.json"), "w", encoding="utf-8") as f:
        json.dump({"meta": meta, "stops": rows}, f, indent=1)
    return text


def run_env(args):
    """The run's switches: BASE_ENV, a window instead of none, hearts unprotected for the Game Over, the opening skipped by the
    game unless this run watches it, --env over them all (NAME=- drops one)"""
    env = dict(BASE_ENV)
    if args.window:   # (a window: no headless run, the window's size; the output stays 640x360, scaled into the window)
        env.pop("HOS_HEADLESS", None)
        env["HOS_WINDOW_SIZE"] = args.window
        env["HOS_WINDOW_NOACTIVATE"] = "1"
    if args.part == "gameover":
        env["HOS_OOT_PROTECT_HEARTS"] = "0"
    env["HOS_SKIP_OPENING"] = "1" if args.skip_opening else "0"
    for kv in args.env:
        k, _, v = kv.partition("=")
        if v == "-":
            env.pop(k, None)
        else:
            env[k] = v
    return env


def run(args):
    """One game of the route (main sends --dry-run to dry_route). The game folder is changed only inside the try whose finally
    puts it back: the notes and lists aside, the options file; what can fail before that (--cpus, the memory wait) fails first"""
    a = actions()
    steps = route(a, args.part, args.stops, args.first)
    run_dir = os.path.join(os.path.abspath(args.out), args.name)
    os.makedirs(run_dir, exist_ok=True)
    for old in ("states.bin", "report.json", "report.txt"):   # (an earlier tour's, with the same --out and --name: not this run's)
        if os.path.exists(os.path.join(run_dir, old)):
            os.remove(os.path.join(run_dir, old))
    env = run_env(args)
    game = os.path.abspath(args.game)
    vk_out = os.path.abspath(args.vk_profile) if getattr(args, "vk_profile", None) else None
    if vk_out:
        refuse_uncounted(game)
        env[PGO_ENV] = os.path.join(run_dir, "vk.profraw")   # (the renderer's counters: written here, then moved to <vk_out>/<name>.profraw)
    if not any(kv.startswith("RX_VK_SHOTS=") for kv in args.env):   # (--env RX_VK_SHOTS=-: none)
        env["RX_VK_SHOTS"] = os.path.join(run_dir, "shots")
    if env.get("RX_VK_SHOTS"):
        os.makedirs(env["RX_VK_SHOTS"], exist_ok=True)
    cpus = cpu_list(args.cpus) if args.cpus else None
    if hasattr(os, "sched_setaffinity"):   # (the game inherits them)
        if cpus:
            try:
                os.sched_setaffinity(0, cpus)
            except OSError as e:
                raise SystemExit("pipeline_tour: --cpus %s: %s" % (args.cpus, e))
        os.nice(max(0, args.nice - os.nice(0)))
    tool = state_tool()
    mem = mem_settings(args)
    prefix = []
    if mem:
        why = wait_memory(mem)
        if why:   # (nothing touched yet: the tour may try again later)
            print("pipeline tour %s: not started, memory: %s" % (args.name, why), flush=True)
            with open(os.path.join(run_dir, "report.json"), "w", encoding="utf-8") as f:
                json.dump({"meta": {"name": args.name, "part": args.part, "completed": False, "error": "memory: " + why,
                                    "memory_wait": why, "left": stop_labels(a, steps)}, "stops": []}, f, indent=1)
            return 3
        prefix = scope_prefix(mem)
    aside = os.path.join(game, ".pipeline_tour_aside")
    if os.path.exists(aside):
        raise SystemExit("pipeline_tour: %s exists (an earlier run did not end): put its files back first" % aside)
    data = os.path.join(run_dir, "data")
    ini = dict(BASE_INI)
    ini.update(kv.split("=", 1) for kv in args.ini)
    rows, seen = [], set()
    proc = bridge = g = profile_meta = None
    touched, old_ini = False, None
    error, memory_stop, left, hard, interrupted = None, None, [], False, False
    try:
        touched = True
        set_aside(game, aside)
        old_ini = set_ini(os.path.join(game, OPTIONS), sorted(ini.items()))
        shutil.rmtree(data, ignore_errors=True)
        os.makedirs(data)   # (no save: the run makes its own)
        proc, bridge, live = start_game(game, run_dir, data, env, prefix)
        g = Game(bridge, run_dir, os.path.join(game, LIST), live, proc, mem)
        if vk_out:
            g.profile = Profile(env[PGO_ENV], vk_out, args.name)
        g.at = 0
        mark = [0.0, 0]

        def count(label):
            nonlocal seen
            n, progs, recs = list_counts(tool, g.list_path)
            rows.append({"stop": label, "seconds": round(g.now(), 1), "took": round(g.now() - mark[0], 1), "frames": g.frame - mark[1],
                         "pipelines": n, "programs": progs, "new": len(recs - seen), "memory": g.memory(), "marks": g.marks})
            g.marks = []
            seen |= recs
            g.note("%s: %d pipelines (+%d), %d programs" % (label, n, rows[-1]["new"], progs))
            mark[0], mark[1] = g.now(), g.frame

        play(g, a, steps, rows, count, args)
    except MemoryStop as e:
        memory_stop, hard = str(e), not e.soft
        left = stop_labels(a, steps[g.at:])   # (the stop it was at, again: a hard stop cut it short)
        g.note("MEMORY: %s; left: %s" % (memory_stop, left))
        print("pipeline tour %s: memory: %s" % (args.name, memory_stop), flush=True)
        if not left:
            error = "memory: " + memory_stop
    except Failed as e:
        error = str(e)
        if g:
            g.note("FAILED: " + error)
    except KeyboardInterrupt:   # (Ctrl+C, or SIGTERM: stopped. The game is ended and the folder put back, and the report written)
        error, interrupted = "stopped (Ctrl+C or SIGTERM)", True
        if g:
            g.note("STOPPED")
    finally:
        hold_stops()
        if g is not None and g.profile is not None:   # (failed or not: what the game counted since control)
            try:
                profile_meta, why = profile_end(g.profile, proc is not None and proc.poll() is None, hard)
            except OSError as e:
                profile_meta, why = {"file": None, "reset": g.profile.was_reset, "why": str(e)}, "--vk-profile: %s" % e
            if why and error is None:
                error = why
            g.note("vk profile: %s" % profile_meta)
        stop_game(proc, bridge if not hard else None, 3 if hard else 15)
        if bridge is not None:
            bridge.close()
        if touched:
            if os.path.exists(os.path.join(game, LIST)):
                shutil.copyfile(os.path.join(game, LIST), os.path.join(run_dir, "states.bin"))
            problems = put_back(game, aside, run_dir)
            try:
                restore_ini(os.path.join(game, OPTIONS), old_ini)
            except OSError as e:
                problems.append("%s: %s" % (OPTIONS, e))
            for p in problems:
                print("pipeline tour %s: NOT PUT BACK: %s" % (args.name, p), flush=True)
            if problems and error is None:
                error = "the game folder was not all put back: " + "; ".join(problems)
    seconds = round(g.now(), 1) if g else 0
    frames = g.frame if g else 0
    last = (g.last or {}) if g else {}
    meta = {"name": args.name, "part": args.part, "seconds": seconds, "frames": frames, "env": env, "ini": args.ini,
            "completed": error is None and not left, "error": error, "fps": frames / seconds if seconds else None,
            "memory_stop": memory_stop, "memory_soft": memory_stop is not None and not hard, "left": left,
            "peak_mb": (g.peak or None) if g else None, "version": last.get("version"), "renderer": last.get("renderer"),
            "settle": [getattr(args, "settle_quiet", SETTLE_QUIET), getattr(args, "settle_max", SETTLE_MAX)]}
    if vk_out:
        meta["vk_profile"] = profile_meta
        if profile_meta and profile_meta.get("file"):   # (where the counters were recorded, for merge-profile's note)
            with open(os.path.join(vk_out, args.name + ".json"), "w", encoding="utf-8") as f:
                json.dump({k: meta[k] for k in ("name", "part", "seconds", "frames", "completed", "error", "version", "renderer")},
                          f, indent=1)
    print(write_report(run_dir, rows, meta), end="")
    if interrupted:
        return 130
    return 1 if error is not None else 4 if left else 0


def mem_settings(args):
    """The memory guard's limits (MEM, --mem key=value over them), None with --no-mem-guard"""
    if getattr(args, "no_mem_guard", False):
        return None
    mem = dict(MEM)
    for kv in getattr(args, "mem", []) or []:
        k, _, v = kv.partition("=")
        if k not in mem:
            raise SystemExit("pipeline_tour: --mem %s: one of %s" % (kv, ", ".join(sorted(MEM))))
        mem[k] = type(MEM[k])(float(v))
    return mem


def cpu_list(text):
    """"0-3,12-15" -> {0, 1, 2, 3, 12, 13, 14, 15}; anything else refused (before the run touches the game folder)"""
    out = set()
    for piece in text.split(","):
        lo, _, hi = piece.strip().partition("-")
        try:
            first, last = int(lo), int(hi or lo)
        except ValueError:
            raise SystemExit("pipeline_tour: --cpus %s: CPU numbers and ranges, as 0-3,12-15" % text)
        if first < 0 or last < first:
            raise SystemExit("pipeline_tour: --cpus %s: %s is not a range" % (text, piece))
        out.update(range(first, last + 1))
    return out


def chunks(a, part, size, limit=None):
    """A part's stops in runs of `size` (a fresh game each: one game's memory grows by about 1 GB an area and is not given back):
    [(first stop's label or None, stops)]. The opening part, or size 0: one run"""
    plan = plan_stops(a, part, limit)
    if not plan:
        return [(None, None)]
    if not size:
        size = len(plan)
    return [("%s (%s)" % (a["teleports"][plan[k][1]][0], plan[k][0]), min(size, len(plan) - k)) for k in range(0, len(plan), size)]


def part_runs(a, part, size, start, say=print, limit=None):
    """A part's games, one after another: its chunks of `size` stops, each a fresh game; a game the memory guard ended (start's
    code 4, or 3: not started) goes on from the stops it left in another fresh game, and a stop the guard ended twice in a row is
    left out. start(first, stops, k) runs game k and returns (code, its report's meta). Returns [(k, code)]; it ends early when
    a game could not start for memory or failed"""
    out, k = [], 0
    for first, stops in chunks(a, part, size, limit):
        stuck = None
        while True:
            code, meta = start(first, stops, k)
            out.append((k, code))
            k += 1
            left = meta.get("left") or []
            if code == 3:
                say("tour: part %s ends: no memory to start a game (%s)" % (part, meta.get("memory_wait")), flush=True)
                return out
            if code != 4 or not left:
                break
            if left[0] == stuck:   # (the same stop ended by the guard twice: left out)
                say("tour: %s left out: the game ran out of memory there twice" % left[0], flush=True)
                left = left[1:]
                if not left:
                    break
            stuck = left[0] if meta.get("memory_stop") and not meta.get("memory_soft") else None
            first, stops = left[0], len(left)
        if code not in (0, 4):
            return out
    return out


def run_command(args, part, name, first, stops, skip_opening):
    """The `run` a tour starts for one game of a part"""
    cmd = [sys.executable, "-I", os.path.abspath(__file__), "run", "--game", args.copy, "--out", args.out_dir, "--name", name,
           "--part", part, "--nice", str(args.nice), "--settle-quiet", str(args.settle_quiet), "--settle-max",
           str(args.settle_max)] + sum((["--env", e] for e in args.env), []) + \
        sum((["--ini", i] for i in args.ini), []) + sum((["--mem", m] for m in args.mem), [])
    if args.no_mem_guard:
        cmd.append("--no-mem-guard")
    if first:
        cmd += ["--first", first, "--stops", str(stops)]
    if skip_opening:
        cmd.append("--skip-opening")
    if args.cpus:
        cmd += ["--cpus", args.cpus]
    if args.window:
        cmd += ["--window", args.window]
    if getattr(args, "vk_profile", None):
        cmd += ["--vk-profile", os.path.abspath(args.vk_profile)]
    return cmd


def run_child(cmd, grace=120, popen=subprocess.Popen):
    """A `run`, waited for; its code. Stopped (Ctrl+C, SIGTERM) while it runs: it is sent SIGTERM (Ctrl+C reached it already when
    it came from the terminal) and given `grace` seconds to end its game, dump the profile and put the folder back, before it is
    killed; then the stop goes on. (subprocess.run would kill it a quarter of a second in, its game left running)"""
    child = popen(cmd)
    try:
        return child.wait()
    except KeyboardInterrupt:
        end = time.monotonic() + grace
        if TERMINATED:   # (Ctrl+C reached the run itself: a second signal would only be held)
            try:
                child.send_signal(signal.SIGTERM)
            except OSError:
                pass
        while True:
            try:
                child.wait(max(0.1, end - time.monotonic()))
                break
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
                break
            except KeyboardInterrupt:   # (pressed again: still waited for, the folder is being put back)
                print("tour: waiting for the run to put the game folder back (%.0f s at most)" % max(0, end - time.monotonic()),
                      flush=True)
        raise


COPY_SPARE = 2 << 30   # (free space kept beside a linked copy's real files: the runs' caches grow in it)


def tree_bytes(top, skip=()):
    """The bytes of the files under top, the entries named in skip left out (links not followed)"""
    total = 0
    for name in os.listdir(top):
        if name in skip:
            continue
        p = os.path.join(top, name)
        if os.path.isdir(p) and not os.path.islink(p):
            for d, _, fs in os.walk(p):
                total += sum(os.lstat(os.path.join(d, f)).st_size for f in fs)
        else:
            total += os.lstat(p).st_size
    return total


def romfs_stamp(romfs):
    """Each file under romfs/: its size, change of content time and inode, to tell after a tour that none was written"""
    out = {}
    for d, _, fs in os.walk(romfs):
        for f in fs:
            s = os.lstat(os.path.join(d, f))
            out[os.path.relpath(os.path.join(d, f), romfs)] = (s.st_size, s.st_mtime_ns, s.st_ino)
    return out


def copy_game(game, copy):
    """The tour's copy of the game folder. Reflinked where the drive can (btrfs, XFS): None. Elsewhere (ext4) romfs/ is
    hard-linked, as `scripts/dev.py packs`' runtimes have it (the game mounts it read-only, host/nn_fs.c), and the rest is
    copied: cache/ is written in place (the program cache appends, vk_progcache.cpp), the options file is rewritten, and bin/
    is where the game finds its folder (/proc/self/exe), so never a link out of the copy. Returns romfs/'s stamp then, which
    romfs_changed checks after the tour. Refused when romfs/ is not a folder on the copy's drive, or the drive lacks room."""
    if not subprocess.run(["cp", "-a", "--reflink=always", game, copy], stderr=subprocess.DEVNULL).returncode:
        return None
    shutil.rmtree(copy, ignore_errors=True)
    romfs, parent = os.path.join(game, "romfs"), os.path.dirname(copy)
    if os.path.islink(romfs) or not os.path.isdir(romfs) or os.stat(romfs).st_dev != os.stat(parent).st_dev:
        raise SystemExit("pipeline_tour: %s cannot be reflinked here, and its romfs/ is not a folder on the drive of %s to "
                         "hard-link: put the game folder on one drive, or on btrfs or XFS" % (game, parent))
    need, free = tree_bytes(game, skip=("romfs",)), shutil.disk_usage(parent).free
    if need + COPY_SPARE > free:
        raise SystemExit("pipeline_tour: copying %s besides its romfs/ needs %.1f GB and %.1f GB more for the runs; %.1f GB "
                         "free" % (game, need / 2 ** 30, COPY_SPARE / 2 ** 30, free / 2 ** 30))
    print("tour: no reflinks on this drive: %s gets romfs/ hard-linked and %.1f GB copied" % (copy, need / 2 ** 30), flush=True)
    os.mkdir(copy)
    rc = subprocess.run(["cp", "-al", romfs, copy]).returncode
    for name in sorted(os.listdir(game)):
        if name != "romfs" and not rc:
            rc = subprocess.run(["cp", "-a", "--reflink=auto", os.path.join(game, name), copy]).returncode
    if rc:
        shutil.rmtree(copy, ignore_errors=True)
        raise SystemExit("pipeline_tour: the copy of %s failed" % game)
    shutil.copystat(game, copy)
    return romfs_stamp(os.path.join(copy, "romfs"))


def romfs_changed(copy, seal):
    """Whether a hard-linked romfs/ was written during the tour (it is the game folder's own as well): the files that differ"""
    if seal is None or not os.path.isdir(os.path.join(copy, "romfs")):
        return []
    now = romfs_stamp(os.path.join(copy, "romfs"))
    changed = sorted(k for k in set(seal) | set(now) if seal.get(k) != now.get(k))
    if changed:
        print("tour: romfs/ changed during the tour, in the game folder too (it is hard-linked): %s" % ", ".join(changed[:20]),
              flush=True)
    return changed


def tour(args):
    """The parts one after another (one game at a time: one game can hold 14 GB), each in a copy of the game folder (copy_game)
    and in games of --chunk stops, under the memory guard; then merged"""
    a = actions()
    parts = args.parts.split(",")
    for part in parts:
        if part not in PARTS:
            raise SystemExit("pipeline_tour: no part %r (%s)" % (part, ", ".join(PARTS)))
    if getattr(args, "vk_profile", None):   # (refused before the copy, as each run would refuse it)
        args.vk_profile = os.path.abspath(args.vk_profile)
        refuse_uncounted(os.path.abspath(args.game))
        os.makedirs(args.vk_profile, exist_ok=True)
        had = [n for n in os.listdir(args.vk_profile) if n.endswith(".profraw")]
        if had:
            print("tour: %s holds %d .profraw files already (merge-profile takes every one): %s" % (
                args.vk_profile, len(had), ", ".join(sorted(had))), flush=True)
    args.out_dir = out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    t0 = time.monotonic()
    args.copy = copy = os.path.abspath(args.game).rstrip("/") + "-tour"
    if os.path.exists(copy):   # (an earlier tour's, kept or cut short: maybe of another build, maybe with its files still aside)
        raise SystemExit("pipeline_tour: %s exists (an earlier tour's copy): remove it, and the tour copies the game folder again"
                         % copy)
    seal = copy_game(os.path.abspath(args.game), copy)
    results = []

    def start_for(part):
        def start(first, stops, k):
            name = "%s-%s" % (args.name, part) + ("-%d" % k if first or k else "")
            # (the opening is watched whole once: by its own part, else by the first part's first game)
            skip = part != "opening" and ("opening" in parts or part != parts[0] or k > 0)
            print("tour: %s" % name, flush=True)
            code = run_child(run_command(args, part, name, first, stops, skip))
            results.append((part, os.path.join(out, name), code))
            try:
                with open(os.path.join(out, name, "report.json"), encoding="utf-8") as f:
                    meta = json.load(f)["meta"]
            except (OSError, ValueError, KeyError):
                meta = {}
            return code, meta
        return start

    try:
        for part in parts:
            runs = part_runs(a, part, args.chunk, start_for(part), limit=args.stops)
            if runs and runs[-1][1] == 3:
                break   # (no memory: the tour ends here, with what it has merged)
    finally:
        changed = romfs_changed(copy, seal)
        if not args.keep:
            shutil.rmtree(copy, ignore_errors=True)
    rc = 0 if all(code in (0, 4) for _, _, code in results) and not changed else 1
    order = sorted(results, key=lambda r: (parts.index(r[0]), r[1]))
    have = [os.path.join(d, "states.bin") for _, d, _ in order if os.path.exists(os.path.join(d, "states.bin"))]
    merged = os.path.join(out, "%s.bin" % args.name)
    if have:
        merge(argparse.Namespace(out=merged, lists=have))
        if getattr(args, "compare_to", None):   # (a faster or changed tour against an older one's list: what it lost)
            compare(argparse.Namespace(new=merged, old=args.compare_to))
    if args.publish and have and (not rc or args.publish_partial):
        rc = publish(have, [d for _, d, _ in order], parts)
    elif args.publish:
        print("tour %s: not published (a run failed or made no list: --publish-partial publishes anyway)" % args.name)
    print("tour %s: %.0f s in all, parts %s, %d runs%s" % (args.name, time.monotonic() - t0, ",".join(parts), len(results),
                                                         "" if not rc else " (a run failed)"))
    if getattr(args, "vk_profile", None):
        raws = sorted(n for n in os.listdir(args.vk_profile) if n.endswith(".profraw")) if os.path.isdir(args.vk_profile) else []
        print("tour %s: the renderer's counters of %d games in %s; merged by: python -I %s merge-profile %s [--publish]" % (
            args.name, len(raws), args.vk_profile, os.path.relpath(os.path.abspath(__file__), ROOT), args.vk_profile))
    return rc


NOT_SETTINGS = (PGO_ENV, "RX_VK_SHOTS")   # (paths of the run's, in the committed tour.md otherwise)


def published_settings(env):
    """The run's switches tour.md names: the output, the frame rate and the renderer's, without the ones that are paths of the run"""
    return {k: v for k, v in env.items() if k.startswith(("HOS_OUTPUT", "HOS_FPS", "RX_VK")) and k not in NOT_SETTINGS
            and not os.path.isabs(v)}


def publish(lists, run_dirs, parts, sources=None):
    """nx2/pipeline_states.py tour: the repository's tour list replaced by these, its note from what the runs' bridges said
    (the build, the renderer: GPU and driver) and their settings"""
    gpu = build = ""
    settings, ini, minutes = {}, None, 0.0
    for d in run_dirs:
        try:
            with open(os.path.join(d, "report.json"), encoding="utf-8") as f:
                meta = json.load(f)["meta"]
        except (OSError, ValueError, KeyError):
            continue
        gpu = gpu or meta.get("renderer") or ""
        build = build or meta.get("version") or ""
        if not gpu:   # (a run from before its renderer answered: the log's device line)
            try:
                log = open(os.path.join(d, "log.txt"), encoding="utf-8", errors="replace").read()
            except OSError:
                log = ""
            m = re.search(r"vk: (.+?), Vulkan [\d.]+, driver (\S+); mesh shaders", log)
            gpu = (m and "%s (driver %s)" % (m.group(1), m.group(2))) or ""
        settings = settings or published_settings(meta.get("env", {}))
        if ini is None:   # (the options file as the runs had it: BASE_INI with their --ini over it)
            ini = dict(BASE_INI)
            ini.update(kv.split("=", 1) for kv in meta.get("ini") or [])
        minutes += meta.get("seconds", 0) / 60
    ini = " ".join("%s=%s" % kv for kv in sorted((ini or BASE_INI).items()))
    cmd = [sys.executable, "-I", os.path.join(ROOT, "nx2", "pipeline_states.py"), "tour"] + lists + [
        "--gpu", gpu or "unknown", "--build", build or "unknown",
        "--settings", ini + " " + " ".join("%s=%s" % kv for kv in sorted(settings.items())),
        "--route", "scripts/pipeline_tour.py, parts %s, from a new save (%.0f minutes of runs)" % (", ".join(parts), minutes)]
    if sources:
        cmd += ["--dir", sources]
    return subprocess.run(cmd).returncode


def dry_route(args):
    """--dry-run: the route's steps, without the game"""
    a = actions()
    picks = item_stops(a)
    for step in route(a, args.part, args.stops, args.first):
        if step[0] == "visit":
            item = ", sword, shield, item %d" % (picks.index(step[1:3]) + 1) if step[1:3] in picks else ""
            print("visit %s (%s): teleport %d%s" % (a["teleports"][step[2]][0], step[1], step[2], item))
        elif step[0] == "items":
            print("items: %d Debug tab actions" % len(items_unlock(a)))
        else:
            print(" ".join(str(s) for s in step[:2]))
    return 0


TERMINATED = []   # (SIGTERM came: a tour passes it on to its run, which Ctrl+C at the terminal reaches by itself)


def stopped(signum, frame):
    TERMINATED.append(signum)
    raise KeyboardInterrupt   # (SIGTERM as SIGINT: the run's finally ends the game and puts the game folder's files back)


def hold_stops():
    """No more Ctrl+C or SIGTERM from here: a run putting the game folder back is not cut short by a second one (a tour that
    waited too long kills it instead)"""
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            signal.signal(sig, signal.SIG_IGN)
        except ValueError:   # (not the main thread: the tests')
            pass


def settle_options(p):
    p.add_argument("--settle-quiet", type=int, default=SETTLE_QUIET, metavar="FRAMES",
                   help="a stop is left after this many frames with no new pipeline (default %d)" % SETTLE_QUIET)
    p.add_argument("--settle-max", type=int, default=SETTLE_MAX, metavar="FRAMES",
                   help="and after this many at most (default %d)" % SETTLE_MAX)


def parser():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="one game")
    r.add_argument("--game", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--name", default="play")
    r.add_argument("--part", choices=PARTS, default="all")
    r.add_argument("--cpus", help="the CPUs the game may use (Linux), e.g. 0-3,12-15")
    r.add_argument("--nice", type=int, default=5, help="the game's niceness (default 5: the desktop stays responsive)")
    r.add_argument("--stops", type=int, help="only the first N stops")
    r.add_argument("--first", metavar="STOP", help="start the route at this stop (its name, or \"<name> (<age>)\"): after a failure")
    r.add_argument("--env", action="append", default=[], help="NAME=VALUE over the run's switches (NAME=- drops one)")
    r.add_argument("--ini", action="append", default=[], help="key=value in longshot_options.ini for the run")
    r.add_argument("--window", metavar="WxH", help="a window instead of the headless run")
    r.add_argument("--skip-opening", action="store_true", help="the game skips the new-game opening (HOS_SKIP_OPENING)")
    r.add_argument("--dry-run", action="store_true", help="the route's steps, without the game")
    r.add_argument("--mem", action="append", default=[], metavar="KEY=VALUE",
                   help="the memory guard's limits over its defaults (%s)" % ", ".join("%s=%s" % kv for kv in sorted(MEM.items())))
    r.add_argument("--no-mem-guard", action="store_true", help="no memory guard (no waiting, no limits, no scope)")
    r.add_argument("--vk-profile", metavar="DIR", help="an RX_VK_PGO=gen renderer's counters, from control of Link to the "
                   "run's end, to DIR/<name>.profraw (merge-profile merges them)")
    settle_options(r)
    t = sub.add_parser("tour", help="the parts, one game at a time, merged")
    t.add_argument("--game", required=True)
    t.add_argument("--out", required=True)
    t.add_argument("--name", default="play")
    t.add_argument("--parts", default="opening,child-overworld,dungeons,adult-overworld")
    t.add_argument("--cpus", help="the CPUs the games may use (Linux), e.g. 6-11,18-23: off the desktop's")
    t.add_argument("--nice", type=int, default=19, help="the games' niceness (default 19)")
    t.add_argument("--mem", action="append", default=[], metavar="KEY=VALUE", help="the memory guard's limits (run --help)")
    t.add_argument("--no-mem-guard", action="store_true")
    t.add_argument("--env", action="append", default=[])
    t.add_argument("--ini", action="append", default=[])
    t.add_argument("--keep", action="store_true", help="keep the parts' game folder copy")
    t.add_argument("--stops", type=int, help="only each part's first N stops (a short test)")
    t.add_argument("--chunk", type=int, default=8, help="stops a game, then a fresh one (one game grows ~1 GB an area); 0: no limit")
    t.add_argument("--publish-partial", action="store_true", help="--publish even when a run failed")
    t.add_argument("--window", metavar="WxH", help="the games in a window")
    t.add_argument("--publish", action="store_true",
                   help="write the repository's tour list (assets/pipeline_states/tour.bin.xz and tour.md) from the parts' lists")
    t.add_argument("--vk-profile", metavar="DIR", help="each game's RX_VK_PGO=gen renderer counters to DIR/<game>.profraw "
                   "(run --vk-profile; merge-profile merges them)")
    settle_options(t)
    t.add_argument("--compare-to", metavar="LIST", help="after the merge: how many pipelines LIST (an older tour's merged list, "
                   "or tour.bin.xz) has that this tour's lacks, and the other way round (compare)")
    mp = sub.add_parser("merge-profile", help="the --vk-profile counters merged into the renderer's play profile")
    mp.add_argument("dir", help="the --vk-profile folder (its *.profraw)")
    mp.add_argument("--out", help="the merged profile (default work/longshot_vk.profdata, which nx2/make_game.py builds with)")
    mp.add_argument("--publish", action="store_true", help="also write assets/profiles/longshot_vk.profdata.xz (xz -9e) and "
                    "the note for assets/profiles/README.md")
    mp.add_argument("--llvm-profdata", metavar="PATH", help="this llvm-profdata, not the toolchain's (the compiler's that "
                    "built the counting renderer)")
    m = sub.add_parser("merge", help="lists merged, and what each added")
    m.add_argument("--out", required=True)
    m.add_argument("lists", nargs="+")
    c = sub.add_parser("compare", help="two lists: how many pipelines each has that the other lacks")
    c.add_argument("new", help="the new list (a tour's <out>/<name>.bin, a run's states.bin, a .bin.xz)")
    c.add_argument("old", help="the list it should lose nothing of")
    v = sub.add_parser("value", help="each step's new pipelines and seconds over runs, and the steps worth cutting")
    v.add_argument("runs", nargs="+", help="run folders (<out>/<name>: report.json and states.bin)")
    v.add_argument("--against", action="append", default=[], help="a list whose pipelines count as had already")
    v.add_argument("--no-contrib", action="store_true", help="not against the players' lists in assets/pipeline_states/contrib")
    v.add_argument("--floor", type=int, default=10, help="a step that alone adds fewer pipelines than this is cut (default 10)")
    v.add_argument("--sub", action="store_true", help="the stops' parts apart (arrive, look, items, settle), where runs marked them")
    v.add_argument("--json")
    return p


def main(argv=None):
    signal.signal(signal.SIGTERM, stopped)
    a = parser().parse_args(argv)
    if a.cmd == "run" and a.dry_run:
        return dry_route(a)
    return {"run": run, "tour": tour, "merge": merge, "compare": compare, "value": value,
            "merge-profile": merge_profile}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
