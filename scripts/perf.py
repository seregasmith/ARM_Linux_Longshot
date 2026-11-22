"""Where a run's time goes, from the host's profiling files (python -I scripts/perf.py <subcommand> ...).

A profile folder (HOS_PROFILE=1, host/profiling.c: logs/profile-<date>-<time>/):
  report    the folder read into one markdown page, report.md: frame times, what limits the frame, the hottest code by
            thread, each stutter, memory; with trace.json.gz (Perfetto) and speedscope.json beside it

A sample file (HOS_SAMPLE_ALL=<file>, plain or packed; a profile folder's all.txt; HOS_SLOW_FRAMES' files):
  threads   each thread's running share; what the threads wait in when no game thread runs, and when the main thread waits
  renderer  the renderer's threads (mkw_vk...): running share, functions by inclusive and self share
  hotlist   the hottest lifted functions, for the lift's hot list (assets/profiles/hot_funcs.txt, A64_HOT_FUNCS)
  cpu       sampled runs' CPU time by category, thread and function, as a publishable page (Windows captures)

Other recordings:
  stacks    one thread's stacks (HOS_SAMPLE=<thread> + HOS_SAMPLE_FILE=<file>): functions by inclusive and self share
  chain     a HOS_CHAIN recording: each frame's critical path, walked back from its present through waits and signals
  ab        frame traces of a run with HOS_AB (an A/B within one run): each side's frame times and their difference

--maps <folder>: the build's symbols, to name the frames: on Linux the game folder (its longshot and longshot_vk.so), on
Windows a folder with longshot.map and longshot_vk.map of the same build. By default RX_MAPS, else work/sa/build (report:
the game folder the profile folder is in). The profiling/ package beside this file holds the readers; `<subcommand>
--help` says more of each. The pages and files name lifted functions by their guest address: keep them on this PC (cpu
without --guest-map makes the view that may be published).
"""
import argparse, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from profiling import ab, analyses, chain, cpu, report, symbols  # noqa: E402


def parser():
    raw = argparse.RawDescriptionHelpFormatter
    p = argparse.ArgumentParser(prog="perf.py", description=__doc__, formatter_class=raw)
    sub = p.add_subparsers(dest="command", required=True, metavar="<subcommand>")

    def add(name, module, text, maps=True):
        s = sub.add_parser(name, help=text, description=module.__doc__, formatter_class=raw)
        if maps:
            s.add_argument("--maps", help="the build's symbols (default RX_MAPS)")
        return s

    s = add("report", report, "a profile folder into report.md, trace.json.gz and speedscope.json")
    s.add_argument("folder")
    s.add_argument("--out", help="the page (default report.md in the folder)")
    s.add_argument("--stutter-ms", type=float, default=50.0)
    s.add_argument("--stutters", type=int, default=15, help="how many of the slowest frames to show one by one")
    s.add_argument("--top", type=int, default=20)
    s.add_argument("--from-s", type=float, help="from this many seconds after the first present")
    s.add_argument("--to-s", type=float, help="to this many seconds after the first present")
    s.add_argument("--no-trace", action="store_true", help="no trace.json.gz and speedscope.json")

    s = add("threads", analyses, "threads running and waiting, from a sample file")
    s.add_argument("samples")
    s.add_argument("--top", type=int, default=25)

    s = add("renderer", analyses, "the renderer's threads, from a sample file")
    s.add_argument("samples")
    s.add_argument("--top", type=int, default=40)

    s = add("hotlist", analyses, "the hot lifted functions, from a sample file")
    s.add_argument("samples")
    s.add_argument("out", help="the list to write")
    s.add_argument("--share", type=float, default=0.85, help="the share of the game's samples the list covers (0.85)")
    s.add_argument("--max", type=int, default=analyses.HOT_CAP, help="the most functions listed (%d)" % analyses.HOT_CAP)

    s = add("stacks", analyses, "one thread's stacks (HOS_SAMPLE_FILE)")
    s.add_argument("stacks")
    s.add_argument("--top", type=int, default=40)
    s.add_argument("--skip", type=float, default=0.0, help="leave out the first fraction of the lines (negative: keep the last)")
    s.add_argument("--only", help="only the samples with a frame whose name holds this, e.g. bench_replay")

    s = add("chain", chain, "a HOS_CHAIN recording's critical path")
    s.add_argument("recording")
    s.add_argument("--samples", help="the same run's HOS_SAMPLE_ALL file: the functions running on the path")
    s.add_argument("--top", type=int, default=20)

    s = add("ab", ab, "an A/B within one run (HOS_AB), from its frame traces", maps=False)
    s.add_argument("frames", nargs="+", help="frame traces (logs/longshot_frames.csv) of runs with HOS_AB")
    s.add_argument("--boot", type=int, default=2000, help="bootstrap resamples (default 2000)")
    s.add_argument("--seed", type=int, default=1, help="the bootstrap's seed, for the same interval each time (default 1)")
    s.add_argument("--json", action="store_true", help="the numbers as JSON")

    s = add("cpu", cpu, "sampled runs' CPU time by category, thread and function", maps=False)
    s.add_argument("runs", nargs="+")
    s.add_argument("--maps", required=True, help="a folder with the build's longshot.map and longshot_vk.map")
    s.add_argument("--out", required=True, help="the page to write")
    s.add_argument("--csv", help="a folder for each view as CSV, and the undocumented functions")
    s.add_argument("--functions", action="append", default=[], help="more description files (JSON, same keys), over "
                   "profiling/cpu_functions.json: e.g. a local file of generated lines for lifted functions")
    s.add_argument("--readobj", help="llvm-readobj, to name system DLLs' frames")
    s.add_argument("--guest-map", help="LOCAL ONLY: a Python file with GUEST_MAP rows (start, end, module, subsystem, how, "
                   "functions) made from the game binary. With it lifted code is listed per function and per subsystem; "
                   "without it (what may be published) all lifted code is one row")
    s.add_argument("--candidates", action="append", default=[], help="LOCAL ONLY: more candidate files (same format), "
                   "e.g. ones that match lifted functions or game subsystems")
    s.add_argument("--fps", type=float, help="the runs' fps when they have no load.json")
    s.add_argument("--measured", nargs="*", default=[], help="unsampled runs of the same scene: fps, CPU and frame times "
                   "come from these (sampling costs some fps); a run whose game clock is off 1.000 by more than 0.01 is "
                   "listed but left out of the medians")
    s.add_argument("--calls", help="a run made with HOS_HLE_STATS=1: calls a frame and the cost of a call for the HLE entries")
    s.add_argument("--top", type=int, default=40)
    s.add_argument("--cover", type=float, default=0.9, help="list functions until this share of on-CPU time is covered")
    return p


def main(argv=None):
    a = parser().parse_args(argv)
    if a.command == "report":
        report.run(a)
    elif a.command == "threads":
        analyses.threads(a.samples, symbols.Names(a.maps), a.top)
    elif a.command == "renderer":
        analyses.renderer(a.samples, symbols.Names(a.maps), a.top)
    elif a.command == "hotlist":
        analyses.hotlist(a.samples, symbols.load_symbols(a.maps), a.out, a.share, a.max)
    elif a.command == "stacks":
        analyses.stacks(a.stacks, symbols.Names(a.maps), a.top, a.skip, a.only)
    elif a.command == "chain":
        chain.run(a.recording, a.samples, a.top, a.maps)
    elif a.command == "ab":
        ab.run(a.frames, a.boot, a.seed, a.json)
    elif a.command == "cpu":
        cpu.run(a)


if __name__ == "__main__":
    main()
