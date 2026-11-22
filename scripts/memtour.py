#!/usr/bin/env python3
"""A memory tour: start a game folder through the control bridge, load a save, teleport through some areas while
sampling the game's memory and the system's, and write what it saw. For a report like "the game outgrows 16 GB of RAM
and the swap fills" (or for a change that should move it): one command, and numbers that two PCs, or two builds, can
compare.

    python -I scripts/memtour.py GAME_DIR (--save SAVE_FOLDER [--slot NN] | --repo-save NAME) [--out DIR]
        [--dests 13,1,2,3,0,7,9,11 | all] [--dwell 40] [--laps 1] [--settle 20] [--interval 2]
        [--env NAME=VALUE ...] [--exe FILE] [--wrapper '["admission", "command"]']

It uses the control bridge's client, `scripts/longshot_bridge.py` (docs/dev/reference/control-bridge.md): a run with
the test profile, as `longshot_ctl.py launch --profile test` (headless, a copy of the save, the game
folder's own options), A pressed until the save is loaded, the OoT debug teleport to each destination (`longshot_ctl.py
teleport` lists them; `all` takes every one) with a walk and camera swings for DWELL seconds in each, and a clean quit.
A first start prepares shaders before the first frame: give it `--launch-timeout`, or run the game once first.

Written to OUT (default `runs/memtour-<time>` in the development folder, `dev.py paths runs`; a new folder):
    mem.csv      one row every INTERVAL seconds: the game's RssAnon, RssShmem, swap (anonymous and shared), the sum
                 `game_total` (resident + swapped, file pages left out: they can be dropped), MemAvailable, swap in use;
                 then the renderer's newest `vk: budget:` line of the run's log (every 600 presents, gpu/vk/mem_account.h):
                 the VRAM its pools hold, the driver's allowance and count and what no pool reports, the RAM they hold,
                 the process's private memory, and a column for each pool (MB; empty until the first line, and
                 `vram_unaccounted` empty where the driver gives no count)
    marks.csv    the tour's events with wall times (in_play, each teleport, arrival, dwell)
    summary.md   the peaks and each area's peak, end value and lowest MemAvailable
    run/         the control run's folder (manifest, log.txt, shots/)

Linux only (it reads /proc). The numbers are observations of one run: a change is shown by runs in turn, on the same
PC, the same scene and the same settings, not by one run each way. Standard library only."""
import argparse
import csv
import json
import os
import re
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import longshot_bridge  # noqa: E402
from tours.memory import proc_mb  # noqa: E402

DEFAULT_DESTS = "13,1,2,3,0,7,9,11"
# (the renderer's pools by the ledger they can be in, as gpu/vk/vk_texture.cpp and vk_memory.cpp register them:
# tests/test_mem_account.py checks that every registered name is here)
BUDGET_VRAM = ["textures", "sparse textures", "target pool", "dead images", "dead buffers", "reaper", "other images",
               "blocks free", "mirrors", "shadows", "staging", "captures", "capture spares", "imports", "retired imports", "other buffers"]
BUDGET_RAM = ["imports", "retired imports", "retired records", "captures", "capture spares", "capture staging spares",
              "capture table", "staging", "textures in RAM", "target pool", "dead images", "dead buffers", "reaper",
              "other images", "other buffers"]
BUDGET_COLUMNS = (["vram_sum", "vram_allowed", "vram_driver", "vram_unaccounted"]
                  + ["vram_" + n.replace(" ", "_") for n in BUDGET_VRAM] + ["vram_others", "ram_sum", "ram_private"]
                  + ["ram_" + n.replace(" ", "_") for n in BUDGET_RAM] + ["ram_others"])
COLUMNS = ["t", "wall", "pid", "rss", "anon", "shmem", "file", "vmswap", "swap_all", "shmem_swapped", "game_total",
           "memavail", "swapused", "sys_shmem"] + BUDGET_COLUMNS
# ("unaccounted n/a": the driver gives no count of its own, so there is nothing to take the sum from: an empty cell)
BUDGET_LINE = re.compile(r"vk: budget: VRAM (\d+) of (\d+) MB \((\d+) MB by the driver, (?:(-?\d+) MB unaccounted|unaccounted "
                         r"n/a)\): (.*); RAM (\d+) MB \(the process's private memory (\d+) MB\): (.*)$")


def sample(pid, proc="/proc"):
    """One row of memory numbers (MB) for process `pid`, or None. game_total = RssAnon + RssShmem + smaps Swap: what the
    game holds in RAM and in swap, shared memory included (VmSwap counts only the anonymous swap entries)."""
    st = proc_mb(f"{proc}/{pid}/status", ("VmRSS", "RssAnon", "RssShmem", "RssFile", "VmSwap"))
    sm = proc_mb(f"{proc}/{pid}/smaps_rollup", ("Swap",))
    mi = proc_mb(f"{proc}/meminfo", ("MemAvailable", "SwapTotal", "SwapFree", "Shmem"))
    if not st or sm is None or not mi or "RssAnon" not in st or "MemAvailable" not in mi:
        return None
    swap_all, vmswap = sm.get("Swap", 0), st.get("VmSwap", 0)
    return dict(pid=pid, rss=st["VmRSS"], anon=st["RssAnon"], shmem=st["RssShmem"], file=st.get("RssFile", 0),
                vmswap=vmswap, swap_all=swap_all, shmem_swapped=max(swap_all - vmswap, 0),
                game_total=st["RssAnon"] + st["RssShmem"] + swap_all, memavail=mi["MemAvailable"],
                swapused=mi.get("SwapTotal", 0) - mi.get("SwapFree", 0), sys_shmem=mi.get("Shmem", 0))


def parse_budget(line):
    """{column: MB} for a log line with the renderer's `vk: budget:` in it (every BUDGET_COLUMNS key), or None. A pool
    the line leaves out holds nothing: 0. A pool this script has no column for is added to its ledger's others."""
    m = BUDGET_LINE.search(line.rstrip("\r\n"))
    if not m:
        return None
    out = dict.fromkeys(BUDGET_COLUMNS, 0)
    out.update(vram_sum=int(m[1]), vram_allowed=int(m[2]), vram_driver=int(m[3]),
               vram_unaccounted="" if m[4] is None else int(m[4]), ram_sum=int(m[6]), ram_private=int(m[7]))
    for ledger, known, pools in (("vram_", BUDGET_VRAM, m[5]), ("ram_", BUDGET_RAM, m[8])):
        for item in pools.split(", "):
            name, _, mb = item.rpartition(" ")
            if not name or not mb.isdigit():   # ("none", or a list the log cut short)
                continue
            out[ledger + (name.replace(" ", "_") if name in known else "others")] += int(mb)
    return out


class BudgetLog:
    """The newest `vk: budget:` line of a log that is still being written: newest() reads what was added since the
    last call and returns parse_budget's columns for the last such line so far ({} before the first, or with no log)"""

    def __init__(self, path):
        self.path, self.at, self.rest, self.last = path, 0, b"", {}

    def newest(self):
        if not self.path:
            return self.last
        try:
            with open(self.path, "rb") as f:
                f.seek(self.at)
                data = self.rest + f.read()
                self.at = f.tell()
        except OSError:
            return self.last
        lines = data.split(b"\n")
        self.rest = lines.pop()   # (a line still being written: read whole at the next call)
        for line in reversed(lines):   # (the pools' line, not the arbiter's "vk: budget: gave back ..." ones)
            row = parse_budget(line.decode("utf-8", "replace")) if b"vk: budget: VRAM" in line else None
            if row:
                self.last = row
                break
        return self.last


class Sampler(threading.Thread):
    """Writes a row to `path` every `interval` seconds until stopped or the process is gone. `log`: the game's log,
    for the budget columns (none: they stay empty)"""

    def __init__(self, pid, path, interval=2.0, proc="/proc", log=None):
        super().__init__(daemon=True)
        self.pid, self.path, self.interval, self.proc = pid, path, interval, proc
        self.budget = BudgetLog(log)
        self.stop_event = threading.Event()

    def run(self):
        t0 = time.time()
        with open(self.path, "w", buffering=1, newline="") as f:
            w = csv.writer(f)
            w.writerow(COLUMNS)
            while not self.stop_event.is_set():
                now = time.time()
                row = sample(self.pid, self.proc)
                if row is None and not os.path.isdir(f"{self.proc}/{self.pid}"):
                    break
                if row:
                    row.update(t=round(now - t0, 1), wall=round(now, 1))
                    row.update(self.budget.newest())
                    w.writerow([row.get(c, "") for c in COLUMNS])
                self.stop_event.wait(max(0.0, self.interval - (time.time() - now)))

    def stop(self):
        self.stop_event.set()
        self.join(10)


class Marks:
    """Events with wall times, to marks.csv as they happen and in memory for the summary"""

    def __init__(self, path, echo=True):
        self.rows, self.echo = [], echo
        self.f = open(path, "w", buffering=1, newline="")
        self.w = csv.writer(self.f)
        self.w.writerow(["wall", "event", "detail"])

    def __call__(self, event, detail=""):
        wall = time.time()
        self.rows.append((wall, event, str(detail)))
        self.w.writerow([f"{wall:.1f}", event, detail])
        if self.echo:
            print(f'{time.strftime("%H:%M:%S")} {event} {detail}', flush=True)

    def close(self):
        self.f.close()


WALK = [("L", 0, 1, 150), ("R", 0.6, 0, 60), ("L", 0.7, 0.7, 120), ("R", -0.6, 0, 60), ("L", -0.7, 0.7, 120),
        ("L", 0, -1, 90), ("R", 0.8, 0, 90)]   # (stick, x, y, frames): forward with turns and camera swings


class Tour:
    """The tour itself, against a control bridge `b` (longshot_bridge.Bridge or anything with its request()) and the
    client module `ctl` (for Failed and check)"""

    def __init__(self, ctl, b, mark, clock=time.time, sleep=time.sleep):
        self.ctl, self.b, self.mark, self.clock, self.sleep = ctl, b, mark, clock, sleep

    def request(self, cmd, **args):
        return self.ctl.check(self.b.request(cmd, **args))

    def to_play(self, timeout=600):
        """Press A through the title and the file select until the save is loaded and Link can be steered"""
        self.request("wait", event="frame", frame=1, timeout=900)
        end = self.clock() + timeout
        while True:
            if self.ctl.ready_status(self.request("status"), "play"):
                return
            if self.clock() > end:
                raise self.ctl.Failed("the save was not loaded after %d s" % timeout, 3)
            self.request("press", buttons="A", frames=8, timeout=30)
            self.request("wait", event="present", frames=60, timeout=60)

    def walk(self, seconds):
        end, i = self.clock() + seconds, 0
        while self.clock() < end:
            s, x, y, frames = WALK[i % len(WALK)]
            i += 1
            try:
                self.request("stick", stick=s, x=x, y=y, frames=frames, timeout=30)
            except self.ctl.Failed as e:
                self.mark("input_error", e)
                self.sleep(1)

    def destinations(self, spec):
        if spec != "all":
            return [int(x) for x in spec.split(",") if x.strip()]
        return [d["index"] for d in self.request("teleport")["destinations"]]

    def teleport(self, lap, d):
        """Send the teleport (the game refuses while it is busy: retried), wait for the map and for play again"""
        sent = None
        for _ in range(6):
            try:
                r = self.request("teleport", to=d, timeout=15)
            except self.ctl.Failed as e:
                self.mark("teleport_error", f"{d} {e}")
                if "event is playing" in str(e):
                    # a text box (the owl, a first visit's words) waits for A, not for time
                    try:
                        self.request("press", buttons="A", frames=8, timeout=30)
                    except self.ctl.Failed as pe:
                        self.mark("input_error", pe)
                self.sleep(3)
                continue
            if r.get("sent"):
                sent = r
                break
            self.mark("teleport_not_sent", f'{d} {r.get("refused") or r.get("error") or r}')
            self.sleep(4)
        if not sent:
            self.mark("teleport_failed", d)
            return False
        self.mark("teleport", f'{lap}:{d}:{sent.get("map")}')
        try:
            w = self.request("wait", timeout=90, event="map", map=sent.get("map"))
            self.mark("arrived", f'{lap}:{d}:{w.get("map")}')
            self.request("wait", timeout=90, event="in_play")
        except self.ctl.Failed as e:
            self.mark("arrive_error", f"{d} {e}")
        return True

    def run(self, dests, dwell, laps, settle):
        self.to_play()
        self.mark("in_play")
        self.walk(settle)
        for lap in range(laps):
            for d in dests:
                if self.teleport(lap, d):
                    self.mark("dwell", f"{lap}:{d}")
                    self.walk(dwell)
        self.mark("tour_done")


def read_rows(path):
    with open(path, newline="") as f:
        # (an empty cell: a budget column before the renderer's first line, or with no log)
        return [{k: float(v) if v != "" else float("nan") for k, v in r.items()} for r in csv.DictReader(f)]


def areas(marks):
    """[(label, start wall, end wall)] from the marks: the save's own area from in_play, then each teleport's dwell"""
    marks = [(float(w), e, d) for w, e, d in marks]
    out, cur = [], None
    for wall, event, detail in marks:
        if event in ("in_play", "teleport", "tour_done"):
            if cur:
                out.append((cur[0], cur[1], wall))
            cur = None if event == "tour_done" else (
                "start" if event == "in_play" else "teleport " + detail.replace(":", " "), wall)
    return out


def summarize(rows, marks, header=""):
    """Markdown: the run's peaks and each area's numbers. `rows` are mem.csv's rows, `marks` (wall, event, detail)"""
    if not rows:
        return header + "\nno memory samples\n"
    col = lambda k, rs=rows: [r[k] for r in rs]
    last = rows[-1]
    out = [header.rstrip(), "", "| | MB |", "|---|---|",
           f"| peak game total (resident + swapped) | {max(col('game_total')):,.0f} |",
           f"| game total at the end | {last['game_total']:,.0f} |",
           f"| peak private memory (RssAnon + its swap) | {max(r['anon'] + r['vmswap'] for r in rows):,.0f} |",
           f"| peak shared memory (resident + swapped) | {max(r['shmem'] + r['shmem_swapped'] for r in rows):,.0f} |",
           f"| lowest MemAvailable | {min(col('memavail')):,.0f} |",
           f"| system swap in use, peak | {max(col('swapused')):,.0f} |",
           f"| samples (below 500 MB available) | {len(rows)} ({sum(r['memavail'] < 500 for r in rows)}) |", ""]
    areas_ = areas(marks)
    if areas_:
        out += ["| area | peak total | end total | end private | end shared | lowest MemAvailable | swap peak |",
                "|---|---|---|---|---|---|---|"]
        for label, a, b in areas_:
            rs = [r for r in rows if a <= r["wall"] <= b]
            if not rs:
                continue
            e = rs[-1]
            out.append(f"| {label} | {max(col('game_total', rs)):,.0f} | {e['game_total']:,.0f} | "
                       f"{e['anon'] + e['vmswap']:,.0f} | {e['shmem'] + e['shmem_swapped']:,.0f} | "
                       f"{min(col('memavail', rs)):,.0f} | {max(col('swapused', rs)):,.0f} |")
    return "\n".join(out) + "\n"


def parse(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game_dir")
    where = ap.add_mutually_exclusive_group(required=True)
    where.add_argument("--save", help="a save folder: a copy is the run's save, the source is only read")
    where.add_argument("--repo-save", help="a repository save (assets/saves/NAME, longshot_ctl.py saves --repo) instead")
    ap.add_argument("--slot")
    ap.add_argument("--out", help="a new or empty folder (default runs/memtour-<time> in the development folder)")
    ap.add_argument("--dests", default=DEFAULT_DESTS, help="teleport indices, comma separated, or all")
    ap.add_argument("--dwell", type=float, default=40, help="seconds in each area")
    ap.add_argument("--laps", type=int, default=1)
    ap.add_argument("--settle", type=float, default=20, help="seconds in the save's own area before the first teleport")
    ap.add_argument("--interval", type=float, default=2.0, help="seconds between memory samples")
    ap.add_argument("--env", action="append", default=[], metavar="NAME=VALUE")
    ap.add_argument("--exe")
    ap.add_argument("--wrapper", help="a JSON array: an admission command the game starts behind")
    ap.add_argument("--launch-timeout", type=float, default=900)
    a = ap.parse_args(argv)
    if a.dwell < 0 or a.laps < 1 or a.settle < 0 or a.interval <= 0:
        ap.error("--dwell and --settle are not negative, --laps is at least 1, --interval is above 0")
    return a


def main(argv=None):
    a = parse(argv)
    if not sys.platform.startswith("linux"):
        print("memtour: memory is sampled from /proc: Linux only", file=sys.stderr)
        return 2
    env = {}
    for kv in a.env:
        k, sep, v = kv.partition("=")
        if not sep or not k:
            print("memtour: --env NAME=VALUE", file=sys.stderr)
            return 2
        env[k] = v
    out = Path(a.out or longshot_bridge.dev_home() / "runs" / ("memtour-" + time.strftime("%Y%m%d-%H%M%S"))).absolute()
    if out.is_symlink() or (out.exists() and (not out.is_dir() or any(out.iterdir()))):
        print(f"memtour: {out} is not a new or empty folder", file=sys.stderr)
        return 2
    out.mkdir(parents=True, exist_ok=True)
    session = sampler = bridge = None
    mark = Marks(out / "marks.csv")
    code = 0
    try:
        session = longshot_bridge.start_session(a.game_dir, run=str(out / "run"), exe=a.exe, save=a.save, slot=a.slot,
                                    repo_save=a.repo_save, profile="test", env=env, timeout=a.launch_timeout,
                                    wrapper=json.loads(a.wrapper) if a.wrapper else None)
        info = session.info()
        mark("started", f'pid {info["pid"]}')
        sampler = Sampler(info["pid"], out / "mem.csv", a.interval, log=info.get("log"))
        sampler.start()
        bridge = longshot_bridge.Bridge(info["socket"], 10)
        tour = Tour(longshot_bridge, bridge, mark)
        tour.run(tour.destinations(a.dests), a.dwell, a.laps, a.settle)
    except longshot_bridge.Failed as e:
        mark("error", e)
        print(f"memtour: {e}", file=sys.stderr)
        code = e.code if isinstance(e.code, int) and e.code else 1
    except KeyboardInterrupt:
        mark("interrupted")
        code = 130
    finally:
        if session:
            try:
                session.stop(30, terminate=True)
            except Exception as e:   # (the game is gone or going: the samples are what matter)
                print(f"memtour: stop: {e}", file=sys.stderr)
        if sampler:
            sampler.stop()
        mark.close()
        if bridge:
            bridge.close()
    header = f"# Memory tour of {a.game_dir}\n\nsave {a.save or 'repo ' + a.repo_save}, dests {a.dests}, dwell {a.dwell} s, laps {a.laps}, " \
             f"settle {a.settle} s, env {env or '-'}, {time.strftime('%Y-%m-%d %H:%M')}"
    if (out / "mem.csv").is_file():
        text = summarize(read_rows(out / "mem.csv"), mark.rows, header)
        (out / "summary.md").write_text(text)
        print(text)
    return code


if __name__ == "__main__":
    sys.exit(main())
