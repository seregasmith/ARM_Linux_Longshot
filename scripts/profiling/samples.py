"""The all-thread sampler's file (HOS_SAMPLE_ALL, host/posix/prof.c and host/mkw_glue.c sample_all_thread), read the same
way in either of its formats; every reader of it in scripts/perf.py goes through here:

    plain   "<ms> <thread>|<frame>;<frame>;..." a sample, innermost frame first, each module+offset
    packed  (HOS_SAMPLE_PACKED, the profiling mode's: host/profiling.c) each distinct stack once, "s <id> <frames>", before
            the first sample of it, "<ms> <thread>|=<id>"

lines(path) gives every line as the plain format has it ("# t0_ns", "# at" and other notes as they are); records(path) the
samples with frames, (ms, thread, stack) as text; timed(path) the samples on the host's clock.
"""


def lines(path):
    stacks = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("s "):
                _, sid, stack = line.rstrip("\n").split(" ", 2)
                stacks[sid] = stack
                continue
            head, bar, stack = line.partition("|")
            if bar and stack.startswith("="):
                yield "%s|%s\n" % (head, stacks.get(stack[1:].rstrip("\n"), ""))
            else:
                yield line


def origin(line):
    """the host's clock (ns) at ms 0 from a "# t0_ns <ns>" line; None for any other line"""
    if line.startswith("# t0_ns "):
        try:
            return int(line.split()[2])
        except (IndexError, ValueError):
            pass
    return None


def parse(line):
    """(ms, thread, stack) of a sample line, as text (the stack may be empty); None for a line that is no sample"""
    head, bar, stack = line.rstrip("\n").partition("|")
    if not bar:
        return None
    ms, _, thread = head.partition(" ")
    return ms, thread, stack


def records(path):
    """(ms, thread, stack) of each sample with frames, plain or packed"""
    for line in lines(path):
        r = parse(line)
        if r and r[2]:
            yield r


def timed(path):
    """(ns, thread, stack) a sample, its time on the host's clock (hos_now_ns): each "# t0_ns" line starts the ms after it
    again (the sampler writes one each time it opens the file: a pause and a resume of HOS_SAMPLE_ALL's <file>.go). Samples
    before the first "# t0_ns" are left out"""
    t0 = None
    for line in lines(path):
        if line.startswith("# t0_ns "):
            o = origin(line)
            t0 = t0 if o is None else o
            continue
        r = parse(line)
        if r and t0 is not None and r[0].isdigit():
            yield t0 + int(r[0]) * 1000000, r[1], r[2]


def t0(path):
    """the first "# t0_ns" of the file (the host's clock at its ms 0), None when it has none"""
    return next((origin(line) for line in lines(path) if line.startswith("# t0_ns ")), None)
