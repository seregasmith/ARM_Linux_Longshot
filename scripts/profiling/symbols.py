"""Naming the samplers' frames (module+offset, leaf first) from a build's symbols, and telling a waiting sample from a running one.

A Windows build's symbols are its linker maps (longshot.map, longshot_vk.map; lld-link's format), a Linux build's are the
binaries' own (longshot, longshot_vk.so; binutils' nm). A folder of either is the "maps" every subcommand of
scripts/perf.py takes (--maps; by default RX_MAPS, else work/sa/build).
"""
import bisect, glob, os, re, shutil, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.path.join(ROOT, "work", "sa", "build")

# The modules whose frames are Longshot's own code (the game and the runtime, the renderer): Windows, then Linux
GAME_MODS = ("longshot.exe", "longshot")
VK_MODS = ("longshot_vk.dll", "longshot_vk.so")
RUN_MODS = GAME_MODS + VK_MODS
OURS = ("longshot:", "longshot_vk:", "longshot.exe:", "longshot_vk.dll:")   # (a frame of ours, named)
# A sample whose leaf is outside Longshot is a wait only when a wait is on its stack: libc's memcpy and the driver's own
# vkCmd* work are CPU time of the thread. These names (as the samples are named: Longshot's frames, leaf first) wait.
WAIT_FRAMES = ("SleepConditionVariableSRW", "WaitForSingleObject", "hos_cv_sleep", "condition_variable::wait",
               "hos_sleep_ns", "sleep_ns", "hos_thread_wait", "vc_acquire", "present_out", "vkWaitForFences",
               "vkWaitSemaphores", "wait_value", "vk_timeline_wait", "recording_wait", ":Sleep", ":SleepEx",
               "WaitForMultipleObjects", "WaitOnAddress",
               # (host/hos_thread.c's sleep_away, which hos_sleep_ns and the guest's sleeps call; libc++'s waits, below
               # std::condition_variable's; the renderer's wait for a GPU fence value, vk_replay.cpp)
               "sleep_away", "__libcpp_condvar_wait", "__libcpp_condvar_timedwait", "mkwvk::(anonymous namespace)::wait")

MAP_LINE = re.compile(r"^\s*([0-9a-f]{4}):[0-9a-f]+\s+(\S+)\s+([0-9a-f]{16})\s+(?:f\s+)?(?:i\s+)?(\S*)", re.I)


def read_map(path):
    """(the map's preferred load address or None, [(address, symbol, object file, section)] sorted) from an lld/MSVC
    linker map; the object file without its folder"""
    base, rows = None, []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if base is None and "Preferred load address is" in line:
                base = int(line.split()[-1], 16)
            m = MAP_LINE.match(line)
            if m:
                rows.append((int(m.group(3), 16), m.group(2), re.sub(r"^.*[\\/]", "", m.group(4)), m.group(1)))
    rows.sort()
    return base, rows


def load_map(path, base):
    """(offsets from base, names demangled) of a linker map's code (section 0001)"""
    syms = [(a - base, s) for a, s, _, sec in read_map(path)[1] if sec == "0001"]
    return [a for a, _ in syms], demangle_all([n for _, n in syms])


def demangler():
    """A C++ demangler for the Itanium names of a Windows build's maps (llvm-mingw's, as on Linux): llvm-cxxfilt or c++filt
    on the PATH, else LONGSHOT_MINGW's, else the llvm-mingw of a build tools folder on this PC (scripts/dev.py's, the
    Longshot Launcher's). None: there is none"""
    for name in ("llvm-cxxfilt", "c++filt"):
        p = shutil.which(name)
        if p:
            return p
    dirs = [os.environ.get("LONGSHOT_MINGW", "")]
    local = os.environ.get("LOCALAPPDATA")
    if local:
        dirs += sorted(glob.glob(os.path.join(local, "Longshot", "dev", "builder", "*", "LongshotBuilder", "tools",
                                              "llvm-mingw")), key=os.path.getmtime, reverse=True)
        dirs += sorted(glob.glob(os.path.join(local, "LongshotLauncher", "toolchains", "*", "LongshotBuilder", "tools",
                                              "llvm-mingw")), key=os.path.getmtime, reverse=True)
    for d in dirs:
        for exe in ("llvm-cxxfilt.exe", "llvm-cxxfilt"):
            p = os.path.join(d, "bin", exe) if d else ""
            if p and os.path.isfile(p):
                return p
    return None


def demangle_all(names):
    """names with their Itanium C++ names (_Z...) demangled, as Linux's nm -C gives them; any other name (C, MSVC's, a lifted
    import's imp__ZN...) as it is. Without a demangler (demangler()), all as they are"""
    idx = [i for i, n in enumerate(names) if n.startswith("_Z")]
    tool = demangler() if idx else None
    if not tool:
        return names
    try:
        r = subprocess.run([tool], input="\n".join(names[i] for i in idx) + "\n", capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=120)
    except (OSError, subprocess.SubprocessError):
        return names
    out = r.stdout.splitlines()
    if r.returncode or len(out) != len(idx):
        return names
    names = list(names)
    for i, n in zip(idx, out):
        names[i] = n
    return names


def bare(name):
    """a demangled name without its parameter list (and const, noexcept after it): "ns::f(int) const" -> "ns::f"; one with
    none as it is ("(anonymous namespace)" inside a name stays)"""
    n = re.sub(r"(\s+(const|volatile|noexcept|&&|&))+$", "", name)
    if not n.endswith(")"):
        return name
    depth = 0
    for i in range(len(n) - 1, -1, -1):
        depth += {")": 1, "(": -1}.get(n[i], 0)
        if depth == 0:
            return n[:i] if i else name
    return name


def waiting(leaf_module, names):
    """whether a sample (its leaf's module, its frames named) waits: outside Longshot, under a wait"""
    if leaf_module in RUN_MODS:
        return False
    for n in names:
        if n.startswith(OURS):
            return bare(n).endswith(WAIT_FRAMES)   # (a C++ name as nm -C gives it: its parameters left out)
    return True   # (no frame of Longshot's at all: a thread blocked outside it)


def load_elf(path):
    """An ELF's function symbols by their offset in the file's address space, which is the offset from the load address
    that a sample of a position-independent executable or a shared object gives (binutils' nm, demangled)"""
    out = subprocess.run(["nm", "--defined-only", "-C", path], capture_output=True, text=True, encoding="utf-8",
                         errors="replace").stdout
    syms = []
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[1] in ("t", "T", "w", "W") and len(parts[0]) >= 8:
            # (the whole demangled name, parameters too: overloads and anonymous namespaces' functions stay apart)
            syms.append((int(parts[0], 16), parts[2]))
    syms.sort()
    return [a for a, _ in syms], [n for _, n in syms]


def load_symbols(folder=None):
    """{module: (addresses, names)} for the modules found in folder (when None: RX_MAPS, else the build's): the linker maps
    of the Windows build, the binaries of the Linux one"""
    folder = folder or os.environ.get("RX_MAPS", BUILD)
    maps = {}
    for mod in ("longshot_vk.dll", "longshot.exe"):
        mp = os.path.join(folder, mod[:-4] + ".map")
        if os.path.exists(mp):
            maps[mod] = load_map(mp, 0x180000000 if mod.endswith(".dll") else 0x140000000)
    for mod in ("longshot_vk.so", "longshot"):
        b = os.path.join(folder, mod)
        if os.path.isfile(b):
            try:
                maps[mod] = load_elf(b)
            except OSError:   # (no nm: frames stay module+offset)
                pass
    return maps


def nearest(table, offset):
    """the name in table, (addresses sorted, names), of the symbol a frame's offset (hex text) falls in; None when the
    offset is below the first symbol or not hex"""
    addrs, names = table
    try:
        i = bisect.bisect_right(addrs, int(offset, 16)) - 1
    except ValueError:
        return None
    return names[i] if i >= 0 else None


def module_name(mod):
    """longshot_vk / longshot, whichever system the sample came from"""
    return mod.split(".")[0]


def demangle(n):
    # ?name@ns@@... -> ns::name (enough to read)
    m = re.match(r"\?([^@]+)@([^@]*)@", n)
    if m and not n.startswith("??"):
        return "%s::%s" % (m.group(2), m.group(1)) if m.group(2) else m.group(1)
    m = re.match(r"\?\?\$?([^@]+)@", n)
    return m.group(1) if m else n


class Names:
    """module+offset frames named from a build's symbols, cached by frame and by stack: "longshot_vk:<function>"; a frame of
    ours with no symbols as module+offset; any other module's frame as its module"""

    def __init__(self, folder=None, maps=None):
        self.maps = maps if maps is not None else load_symbols(folder)
        self.frame_cache, self.stack_cache = {}, {}

    def frame(self, fr):
        r = self.frame_cache.get(fr)
        if r is None:
            mod, _, off = fr.rpartition("+")
            r = mod or fr
            if mod in self.maps:
                name = nearest(self.maps[mod], off)
                if name is not None:
                    r = module_name(mod) + ":" + demangle(name)
            elif mod in RUN_MODS:
                r = fr   # (no symbols: the offset is all there is)
            self.frame_cache[fr] = r
        return r

    def frames(self, stack):
        """a stack's frames ("a;b;c", leaf first) named, in order"""
        return [self.frame(x) for x in stack.split(";") if x]

    def stack(self, st):
        """(running, leaf, owner: the innermost frame of Longshot's, named frames)"""
        r = self.stack_cache.get(st)
        if r is None:
            frames = [x for x in st.split(";") if x]
            named = [self.frame(x) for x in frames]
            leafmod = frames[0].rpartition("+")[0] if frames else ""
            running = bool(frames) and not waiting(leafmod, named)
            owner = next((n for x, n in zip(frames, named) if x.rpartition("+")[0] in RUN_MODS), named[0] if named else "?")
            r = self.stack_cache[st] = (running, named[0] if named else "?", owner, tuple(named))
        return r


def wait_key(named):
    """where a blocked thread waits: the import it called (imp_<sym>, or an HLE body not inlined, hle_<sym>) under the
    innermost lifted function, else the innermost frame of ours"""
    imp = next((n for n in named if ":imp_" in n or ":hle_" in n), None)
    fn = next((n for n in named if ":f_" in n), None)
    head = imp.replace(":hle_", ":imp_", 1).split(":imp_")[1][:48] + " <- " if imp else ""
    ours = next((n for n in named if n.startswith(OURS)), None)
    return head + (fn or ours or (named[0] if named else "?"))
