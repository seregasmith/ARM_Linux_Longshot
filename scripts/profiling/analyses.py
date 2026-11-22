"""The quick reads of a sample file, each printed to the terminal (scripts/perf.py stacks, threads, renderer, hotlist).

stacks    one thread's samples (HOS_SAMPLE=<thread> + HOS_SAMPLE_FILE, host/mkw_glue.c): a line of module+offset frames a
          sample, leaf first. The functions by inclusive share (in the stack at all) and self share (the leaf), and the leaf
          modules.
threads   an all-thread file (HOS_SAMPLE_ALL; a profile folder's all.txt; HOS_SLOW_FRAMES' files): every 2 ms each guest and
          renderer thread, "<ms> <thread>|<frames>". A thread is blocked at an instant when its leaf is outside Longshot and
          the innermost frame of Longshot's under it is a wait (symbols.WAIT_FRAMES: a condition variable, a sleep, a fence,
          the present); running otherwise, libc's copies and the driver's own work included. Prints each thread's running
          share; for the instants when no game thread runs (the frame waits on something outside the game's CPU work): what
          each serial thread waits in and what the renderer's threads do; for the instants when the main thread is blocked:
          how many game threads run and the functions they are in.
renderer  an all-thread file: for the renderer's threads (named mkw_vk...), the share of samples running
          (leaf in Longshot or the GPU driver), and the functions by inclusive and self share of the running samples.
hotlist   an all-thread file: for each sample of a game thread running in longshot.exe / longshot, the innermost lifted
          function (f_...) is charged. Writes the functions that make up the first <share> of those samples, hottest first,
          one a line, for the lift's A64_HOT_FUNCS (assets/profiles/hot_funcs.txt; their FP ops call the helpers' inline
          fast paths; with A64_HOT_CHUNK=1 they are compiled together in code_hot.c).
"""
import collections, itertools, os, stat, sys
from contextlib import ExitStack
from tempfile import TemporaryFile

from profiling import samples, symbols

MAIN, PRESENT = "sead::MainThread", "Presentation_Thread"


# ---- stacks

def profile_lines(path, skip):
    """the file's lines after the first <skip> fraction of them (a negative one: the last), read as a stream"""
    with open(path, encoding="utf-8", errors="replace") as f, ExitStack() as files:
        start = 0
        if skip:
            # Windows can report a pipe as seekable, even though it cannot be replayed.
            if stat.S_ISREG(os.fstat(f.fileno()).st_mode) and f.seekable():
                count = sum(1 for _ in f)
            else:
                # A pipe can only be read once. Save its lines on disk to keep memory bounded.
                spool = files.enter_context(TemporaryFile(mode="w+t", encoding="utf-8", newline="\n"))
                count = 0
                for count, line in enumerate(f, 1):
                    spool.write(line)
                f = spool
            start = int(count * skip)
            if start < 0:
                start = max(0, count + start)
            else:
                start = min(start, count)
            f.seek(0)
        yield from itertools.islice(f, start, None)


def stacks(path, names, top=40, skip=0.0, only=None):
    incl, self_, leafmod = collections.Counter(), collections.Counter(), collections.Counter()
    n = 0
    for line in profile_lines(path, skip):
        frames = line.strip().split(";")
        if not frames or not frames[0]:
            continue
        named = [names.frame(f) for f in frames]
        if only and not any(only in f for f in named):
            continue
        n += 1
        self_[named[0]] += 1
        leafmod[frames[0].rpartition("+")[0]] += 1
        for f in set(named):
            incl[f] += 1
    print("%d samples" % n)
    print("-- leaf modules")
    for k, v in leafmod.most_common(10):
        print("%6.1f%%  %s" % (100.0 * v / n, k))
    print("-- inclusive")
    for k, v in incl.most_common(top):
        print("%6.1f%%  %s" % (100.0 * v / n, k))
    print("-- self")
    for k, v in self_.most_common(top):
        print("%6.1f%%  %s" % (100.0 * v / n, k))


# ---- threads

def threads(path, names, top=25):
    ticks = collections.defaultdict(dict)   # ms -> thread -> (running, frames named)
    for ms, thread, stack in samples.records(path):
        if not ms.isdigit():
            continue
        running, _, _, named = names.stack(stack)
        ticks[int(ms)][thread] = (running, named)
    n = len(ticks)
    print("%d instants" % n)
    running = collections.Counter()
    seen = collections.Counter()
    for t in ticks.values():
        for th, (r, _) in t.items():
            seen[th] += 1
            running[th] += r
    print("-- running share by thread")
    for th, c in sorted(running.items(), key=lambda kv: -kv[1])[:30]:
        print("  %5.1f%%  %s" % (100.0 * c / seen[th], th))

    def is_game(th):
        return not th.startswith("mkw_vk")

    idle = [t for t in ticks.values() if not any(r for th, (r, _) in t.items() if is_game(th))]
    print("-- no game thread running: %.1f%% of instants" % (100.0 * len(idle) / max(1, n)))
    waits = collections.Counter()
    vk = collections.Counter()
    for t in idle:
        for th, (r, frames) in t.items():
            if th in (MAIN, PRESENT) or th.startswith("BaseProcExecutor0"):
                waits["%s: %s" % (th, symbols.wait_key(frames))] += 1
            if not is_game(th):
                vk["%s %s: %s" % (th, "runs" if r else "waits", frames[0] if not r else
                                  next((f for f in frames if f.startswith("longshot_vk:")), frames[0]))] += 1
    for k, c in waits.most_common(top):
        print("  %5.1f%%  %s" % (100.0 * c / max(1, len(idle)), k))
    print("  (renderer threads then)")
    for k, c in vk.most_common(top):
        print("  %5.1f%%  %s" % (100.0 * c / max(1, len(idle)), k))

    mw = [t for t in ticks.values() if MAIN in t and not t[MAIN][0]]
    print("-- main thread blocked: %.1f%% of instants" % (100.0 * len(mw) / max(1, n)))
    hist = collections.Counter(sum(1 for th, (r, _) in t.items() if r and is_game(th)) for t in mw)
    print("  game threads running then: " + ", ".join("%d: %.0f%%" % (k, 100.0 * v / len(mw)) for k, v in sorted(hist.items())))
    mwhere = collections.Counter(symbols.wait_key(t[MAIN][1]) for t in mw)
    print("  main waits in:")
    for k, c in mwhere.most_common(8):
        print("    %5.1f%%  %s" % (100.0 * c / len(mw), k))
    fns, leaves = collections.Counter(), collections.Counter()
    for t in mw:
        for th, (r, frames) in t.items():
            if r and is_game(th):
                fns[next((f for f in frames if ":f_" in f), frames[0])] += 1
                leaves[frames[0]] += 1
    print("  game functions running then (innermost f_, share of those instants):")
    for k, c in fns.most_common(top):
        print("    %5.1f%%  %s" % (100.0 * c / len(mw), k))
    print("  leaves running then:")
    for k, c in leaves.most_common(top):
        print("    %5.1f%%  %s" % (100.0 * c / len(mw), k))


# ---- renderer

# (the GPU drivers' user-mode modules: work there is the thread running. Windows: the file names; Linux: the file's name
# has the version after .so, so a prefix)
WINDOWS_DRIVERS = ("nvoglv64.dll", "amdvlk64.dll", "amdxc64.dll", "amdxx64.dll", "igvk64.dll", "igxelpicd64.dll")
LINUX_DRIVERS = ("libnvidia-glcore.so", "libvulkan_radeon.so", "libvulkan_intel.so")


def renderer(path, names, top=40):
    seen, running = collections.Counter(), collections.Counter()
    incl, self_ = collections.Counter(), collections.Counter()
    nrun = 0
    for _, th, st in samples.records(path):
        if not th.startswith("mkw_vk"):
            continue
        frames = st.split(";")
        seen[th] += 1
        leafmod = frames[0].rpartition("+")[0]
        if leafmod not in symbols.RUN_MODS + WINDOWS_DRIVERS and not leafmod.startswith(LINUX_DRIVERS):
            continue
        running[th] += 1
        nrun += 1
        named = [names.frame(f) for f in frames]
        self_[named[0]] += 1
        for n in set(named):
            incl[n] += 1
    print("-- running share by thread")
    for th in sorted(seen, key=lambda t: -running[t]):
        print("  %5.1f%%  %s (%d samples)" % (100.0 * running[th] / seen[th], th, seen[th]))
    print("-- inclusive (share of %d running samples)" % nrun)
    for k, c in incl.most_common(top):
        print("  %5.1f%%  %s" % (100.0 * c / max(1, nrun), k[:90]))
    print("-- self")
    for k, c in self_.most_common(top):
        print("  %5.1f%%  %s" % (100.0 * c / max(1, nrun), k[:90]))


# ---- hotlist

# (the game's Zstd decoder: the job loop's f_80115bf0 -> f_80115c80 tree. It runs while areas load and stream, which a play
# profile seldom samples, so it fell to the cold -O1 chunks. At the hot flags a whole decode takes 16% less time and the
# code is no bigger; the functions match Unicorn on every register and written page either way. Listed whatever the profile says)
ZSTD_DECODER = ("f_80115c80 f_80109790 f_80113880 f_80116490 f_801099a0 f_8010b160 f_8010ffc0 f_801102e0 f_80110860 "
                "f_801117a0 f_80112880 f_80113080 f_801156e0 f_80115880 f_8010ad60 f_834ab610 f_834abda0 f_834ab4a0 "
                "f_834ac0b0 f_834ac110 f_834ac2a0 f_8572f040").split()
HOT_CAP = 1500   # (the most functions a list holds; tests/test_shipped_hot_list.py checks the committed one against it)


def hotlist(path, maps, out_path, share=0.85, cap=HOT_CAP):
    game = next((m for m in symbols.GAME_MODS if m in maps), None)
    if not game:
        sys.exit("no longshot.map (Windows) or longshot binary (Linux) in the maps folder: give --maps (or RX_MAPS)")
    c = collections.Counter()
    total = 0
    for _, th, st in samples.records(path):
        if th.startswith("mkw_vk"):
            continue
        frames = st.split(";")
        if not frames[0].startswith(game + "+"):
            continue
        for f in frames:
            mod, _, off = f.rpartition("+")
            if mod != game:
                continue
            n = symbols.nearest(maps[game], off) or ""
            if n.startswith("f_"):
                c[n] += 1
                total += 1
                break
    out, acc = [], 0
    for n, k in c.most_common():
        if acc >= share * total or len(out) >= cap:
            break
        out.append(n)
        acc += k
    out += [n for n in ZSTD_DECODER if n not in out]
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(n + "\n" for n in out))
    print("%d samples in %d functions; %d functions cover %.0f%%" % (total, len(c), len(out), 100.0 * acc / max(1, total)))
