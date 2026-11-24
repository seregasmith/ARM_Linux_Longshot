"""Two built game folders (a stable's and main's, each from its release AppImage), each run as one game session through
the queue in timing mode that visits several areas: A, B, A, B. Each area is reached by the Debug tab's teleport, warmed up
until no frame waits for a pipeline, then measured from the frame trace. The release-benchmark skill
(.claude/skills/release-benchmark/SKILL.md) says how to build the folders and what to report. Python's standard library
only; dev.py ab's pieces (scripts/devtools/ab.py).

    python -I scripts/release_bench.py --a DIR --b DIR --options FILE [--label-a v3.1.1] [--label-b nightly-786]
        [--areas kokiri-forest,market,hyrule-field] [--seconds 45] [--pairs 2]

Writes results.json and summary.md (each area's median fps, 1% low and p99 a side) to the development folder's
runs/release-bench/<time>/ and prints the summary. A run measured while the PC was busy is marked NOISY: run it again.
"""
import argparse
import json
import os
import shutil
import statistics
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from devtools import ab, common, jobqueue, packs  # noqa: E402


def say(text):
    common.say("release_bench", text)


def run(a, out, pinned, side, k):
    label, rt = (a.label_a, a.a) if side == "A" else (a.label_b, a.b)
    import longshot_bridge
    from tours import drive, route
    d = os.path.join(out, "%s%d" % (side, k))
    os.makedirs(d)
    trace = os.path.join(rt, ab.TRACE)
    if os.path.exists(trace):
        os.remove(trace)
    opts, no_env, noise = os.path.join(d, "options.ini"), os.path.join(d, "empty-env.txt"), os.path.join(d, "noise.jsonl")
    shutil.copyfile(pinned, opts)
    open(no_env, "w").close()
    env = dict(ab.COMMON_ENV, HOS_OPTIONS_FILE=opts, HOS_ENV_FILE=no_env)
    wrapper = [sys.executable, "-I", os.path.join(HERE, "dev.py"), "queue", "run", "release-bench-%s%d" % (side, k),
               "--mode", "timing", "--desc", "%s: %s" % (side, label), "--noise", noise, "--"]
    r = {"side": side, "k": k, "label": label, "areas": {}}
    session = None
    try:
        say("%s%d (%s): queued" % (side, k, label))
        session = longshot_bridge.start_session(
            rt, romfs=packs.runtime_romfs(rt), run=os.path.join(d, "game"), user=os.path.join(d, "user"),
            repo_save=ab.scene_of(a.areas[0])["save"], env=env, ready="present", timeout=3600, wrapper=wrapper)
        g = drive.Game(session.connect(30), d, "")
        end = time.monotonic() + 300
        while True:
            st = g.status()
            if st["map"] and st["fresh"] and st["event"] is False and st["loading"] is False:
                break
            if time.monotonic() > end:
                raise drive.Failed("not in play 300 s after the first frame: %s" % st)
            time.sleep(1)
        for name in a.areas:
            sc = ab.scene_of(name)
            dest = ab.destination(sc, route.actions())
            if sc["age"] and g.status()["age"] != sc["age"]:
                drive.switch_age(g, sc["age"])
            if dest:
                drive.teleport(g, dest[0], dest[1], sc["age"])
            t0, warm = time.monotonic(), False
            while time.monotonic() - t0 < ab.WARM_MAX_S:
                time.sleep(1)
                if time.monotonic() - t0 >= 15 and ab.settled(ab.read_trace(trace)):
                    warm = True
                    break
            start, wall = ab.num(ab.read_trace(trace)[-1], "at_ns"), time.time()
            time.sleep(a.seconds)
            wall_end = time.time()
            time.sleep(3)   # (the trace is flushed every 60 frames)
            rows = ab.window(ab.read_trace(trace), start, a.seconds)
            s = ab.frame_stats([ab.num(x, "ms") for x in rows])
            s.update(warm=warm, cold=sum(1 for x in rows if ab.cold(x)))
            n = ab.measured_noise(noise, wall, wall_end)
            s["noisy"] = jobqueue.noise_reasons(n) if n else []
            r["areas"][name] = s
            say("%s%d %s: %.1f fps, median %.2f ms, p99 %.2f ms, 1%% low %.1f, %d cold%s%s" % (
                side, k, name, s["fps"], s["median_ms"], s["p99_ms"], s["low1_fps"], s["cold"],
                "" if warm else " (never settled)", " (NOISY: %s)" % "; ".join(s["noisy"]) if s["noisy"] else ""))
        info = session.stop(timeout=30)
        with open(info["log"], encoding="utf-8", errors="replace") as f:
            facts = ab.log_facts(f.read())
        r.update(version=facts["version"], gpu=facts["gpu"], switches=facts["switches_line"])
    except (drive.Failed, longshot_bridge.Failed, OSError, IndexError) as e:
        r["error"] = str(e)
        say("%s%d FAILED: %s" % (side, k, e))
        if session is not None:
            try:
                session.stop(timeout=15, terminate=True)
            except longshot_bridge.Failed:
                pass
    return r


def summary(a, results):
    """summary.md: each area's medians a side over its quiet runs, and the runs left out"""
    lines = ["| Area | %s fps | %s fps | Δ | 1%% low (A → B) | p99 ms (A → B) |" % (a.label_a, a.label_b), "|---|---|---|---|---|---|"]
    left = []
    for name in a.areas:
        med = {}
        for side in "AB":
            ok = [r["areas"][name] for r in results if r["side"] == side and name in r.get("areas", {})]
            left += ["%s%d %s: %s" % (side, r["k"], name, "; ".join(r["areas"][name]["noisy"])) for r in results
                     if r["side"] == side and r.get("areas", {}).get(name, {}).get("noisy")]
            ok = [s for s in ok if not s["noisy"]]
            med[side] = {k: statistics.median([s[k] for s in ok]) for k in ("fps", "low1_fps", "p99_ms")} if ok else None
        if med["A"] and med["B"]:
            lines.append("| %s | %.1f | %.1f | %+.1f%% | %.1f → %.1f | %.2f → %.2f |" % (
                name, med["A"]["fps"], med["B"]["fps"], 100 * (med["B"]["fps"] / med["A"]["fps"] - 1),
                med["A"]["low1_fps"], med["B"]["low1_fps"], med["A"]["p99_ms"], med["B"]["p99_ms"]))
        else:
            lines.append("| %s | no quiet run on a side | | | | |" % name)
    if left:
        lines += ["", "Left out (NOISY, run them again): " + ", ".join(left)]
    return "\n".join(lines) + "\n"


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--a", required=True, help="side A's game folder (the stable)")
    p.add_argument("--b", required=True, help="side B's game folder (main)")
    p.add_argument("--label-a", default="A")
    p.add_argument("--label-b", default="B")
    p.add_argument("--options", required=True, help="the options file every run starts from a copy of")
    p.add_argument("--areas", default="kokiri-forest,market,hyrule-field", help="dev.py ab --scenes names, in order")
    p.add_argument("--seconds", type=int, default=45, help="measured in each area")
    p.add_argument("--pairs", type=int, default=2)
    a = p.parse_args()
    a.a, a.b, a.areas = os.path.abspath(a.a), os.path.abspath(a.b), [x for x in a.areas.split(",") if x]
    for n in a.areas:
        ab.scene_of(n)
    out = os.path.join(common.dev_path("runs"), "release-bench", time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(out)
    pinned = os.path.join(out, "options.ini")
    ab.pin_options(a.options, pinned, False, False)
    for k in [k for k in os.environ if k.startswith(("HOS_", "RX_", "A64_", "OOT_"))]:
        del os.environ[k]
    results = []
    for k in range(1, a.pairs + 1):
        for side in "AB":
            results.append(run(a, out, pinned, side, k))
            with open(os.path.join(out, "results.json"), "w", encoding="utf-8") as f:
                json.dump(results, f, indent=1)
    text = summary(a, results)
    with open(os.path.join(out, "summary.md"), "w", encoding="utf-8") as f:
        f.write(text)
    print("\n" + text, flush=True)
    say("written to %s" % out)
    return 1 if any(r.get("error") for r in results) else 0


if __name__ == "__main__":
    sys.exit(main())
