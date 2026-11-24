"""The processes of this PC, read the same way on Linux (/proc) and Windows (a Toolhelp snapshot and each process's PEB),
and a command run in a Windows job object that ends with its runner. The job queue's: who plays, builds outside the queue,
a job's process tree. Python's standard library only."""
import functools
import os
import subprocess
import sys

from . import common

CREATE_SUSPENDED = 0x00000004


@functools.lru_cache(maxsize=None)
def _win():
    """(Windows) ctypes, wintypes, kernel32, ntdll, shell32 with the types of every call made here, and the structures"""
    import ctypes
    import types
    from ctypes import wintypes as w
    H, P = w.HANDLE, ctypes.POINTER

    class Entry(ctypes.Structure):   # (PROCESSENTRY32W)
        _fields_ = [("dwSize", w.DWORD), ("cntUsage", w.DWORD), ("th32ProcessID", w.DWORD),
                    ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", w.DWORD), ("cntThreads", w.DWORD),
                    ("th32ParentProcessID", w.DWORD), ("pcPriClassBase", ctypes.c_long), ("dwFlags", w.DWORD),
                    ("szExeFile", ctypes.c_wchar * 260)]

    class Basic(ctypes.Structure):   # (JOBOBJECT_BASIC_LIMIT_INFORMATION)
        _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64), ("PerJobUserTimeLimit", ctypes.c_int64),
                    ("LimitFlags", w.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                    ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", w.DWORD),
                    ("Affinity", ctypes.c_size_t), ("PriorityClass", w.DWORD), ("SchedulingClass", w.DWORD)]

    class Io(ctypes.Structure):
        _fields_ = [(n, ctypes.c_uint64) for n in ("Read", "Write", "Other", "ReadBytes", "WriteBytes", "OtherBytes")]

    class Extended(ctypes.Structure):   # (JOBOBJECT_EXTENDED_LIMIT_INFORMATION)
        _fields_ = [("Basic", Basic), ("Io", Io), ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                    ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]

    k = ctypes.WinDLL("kernel32", use_last_error=True)
    nt, sh = ctypes.WinDLL("ntdll"), ctypes.WinDLL("shell32")
    for f, res, args in (
            (k.OpenProcess, H, [w.DWORD, w.BOOL, w.DWORD]),
            (k.CloseHandle, w.BOOL, [H]),
            (k.GetExitCodeProcess, w.BOOL, [H, P(w.DWORD)]),
            (k.GetProcessTimes, w.BOOL, [H] + [P(w.FILETIME)] * 4),
            (k.CreateJobObjectW, H, [ctypes.c_void_p, ctypes.c_wchar_p]),
            (k.AssignProcessToJobObject, w.BOOL, [H, H]),
            (k.SetInformationJobObject, w.BOOL, [H, ctypes.c_int, ctypes.c_void_p, w.DWORD]),
            (k.CreateToolhelp32Snapshot, H, [w.DWORD, w.DWORD]),
            (k.Process32FirstW, w.BOOL, [H, P(Entry)]),
            (k.Process32NextW, w.BOOL, [H, P(Entry)]),
            (k.ReadProcessMemory, w.BOOL, [H, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, P(ctypes.c_size_t)]),
            (k.IsWow64Process, w.BOOL, [H, P(w.BOOL)]),
            (k.LocalFree, ctypes.c_void_p, [ctypes.c_void_p]),
            (nt.NtResumeProcess, ctypes.c_long, [H]),
            (nt.NtQueryInformationProcess, ctypes.c_long, [H, ctypes.c_int, ctypes.c_void_p, w.ULONG, P(w.ULONG)]),
            (sh.CommandLineToArgvW, P(w.LPWSTR), [w.LPCWSTR, P(ctypes.c_int)])):
        f.restype, f.argtypes = res, args
    return types.SimpleNamespace(ctypes=ctypes, w=w, k=k, nt=nt, sh=sh, Entry=Entry, Extended=Extended)


def proc_start(pid):
    """The process's start time (clock ticks since boot on Linux): a pid used again is not the same process"""
    if common.WINDOWS:
        return _win_start(pid)
    try:
        with open("/proc/%d/stat" % pid) as f:
            stat = f.read()
        return int(stat[stat.rindex(")") + 2:].split()[19])
    except (OSError, ValueError, IndexError):
        if sys.platform == "linux":
            return None
    if os.name == "nt":   # (common.WINDOWS patched off: os.kill(pid, 0) here is a Ctrl+C to every process of the console)
        return _win_start(pid)
    try:   # (no /proc: alive or not is all there is to say)
        os.kill(pid, 0)
    except PermissionError:
        pass
    except OSError:
        return None
    return 0


def running(pid, start):
    """The process pid, started at start (proc_start), runs: not ended, not a zombie waiting for its parent, not a pid used
    again"""
    now = proc_start(pid)
    if now is None or now != start:
        return False
    if common.WINDOWS:
        return True
    try:
        with open("/proc/%d/stat" % pid) as f:
            return f.read().rsplit(")", 1)[1].split()[0] not in ("Z", "X")
    except (OSError, IndexError):
        return sys.platform != "linux"   # (no /proc: running as far as can be told)


def _win_start(pid):
    W = _win()
    h = W.k.OpenProcess(0x1000, False, pid)   # (PROCESS_QUERY_LIMITED_INFORMATION)
    if not h:
        return None
    try:
        code = W.w.DWORD()
        if not W.k.GetExitCodeProcess(h, W.ctypes.byref(code)) or code.value != 259:   # (STILL_ACTIVE)
            return None
        times = [W.w.FILETIME() for _ in range(4)]
        if not W.k.GetProcessTimes(h, *[W.ctypes.byref(t) for t in times]):
            return 0
        return (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime
    finally:
        W.k.CloseHandle(h)


# (Windows) A job's command and everything it starts, in a job object of the runner's. Windows has no signal a runner can
# clean up on: cancel's os.kill is TerminateProcess, and a runner that ends that way (or crashes, or loses its console) runs
# no finally, so its command's processes would go on as orphans beside the next job. With JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
# the runner's handle is the job's only one, and its end, however it comes, kills them all. A command that ends by itself
# lets what it started go on, as on Linux (win_job_release)
_KILL_ON_JOB_CLOSE = 0x2000


def _win_job_limits(job, flags):
    W = _win()
    info = W.Extended()
    info.Basic.LimitFlags = flags
    return bool(W.k.SetInformationJobObject(job, 9, W.ctypes.byref(info), W.ctypes.sizeof(info)))   # (JobObjectExtendedLimitInformation)


def win_job_start(child):
    """The suspended child into a new kill-on-close job object, then let go. Returns the job's handle, or None when it could not
    be made (the child runs anyway)"""
    W = _win()
    job = W.k.CreateJobObjectW(None, None)
    if job and not (_win_job_limits(job, _KILL_ON_JOB_CLOSE) and W.k.AssignProcessToJobObject(job, int(child._handle))):
        W.k.CloseHandle(job)
        job = None
    W.nt.NtResumeProcess(int(child._handle))   # (started suspended, so nothing it starts is outside the job)
    return job


def win_job_release(job, keep_running):
    """The runner's handle on the job closed: what is still in it ends, unless keep_running (the command ended by itself: what it
    left running, a game it started, goes on as without the queue)"""
    if keep_running:
        _win_job_limits(job, 0)
    _win().k.CloseHandle(job)


def win_kill_tree(pid):
    """pid and every process under it (taskkill /T): a runner whose job object could not be made, or another version's"""
    return subprocess.run(["taskkill", "/T", "/F", "/PID", str(pid)], capture_output=True, text=True).returncode == 0


def pidns():
    try:
        return os.readlink("/proc/self/ns/pid")
    except (OSError, AttributeError):
        return ""


# (Windows) What Linux reads from /proc: every process with its parent and its program's name (a Toolhelp snapshot), and a
# process's command line and environment, read from its own memory (its PEB's process parameters), as /proc/<pid>/cmdline
# and environ are. Only a 64-bit process another of the user's may read: anything else (elevated, another user's, 32-bit, a
# 32-bit Python) is not readable, as on Linux
def _win_snapshot():
    """{pid: (parent pid, program's file name)}"""
    W = _win()
    snap = W.k.CreateToolhelp32Snapshot(0x2, 0)   # (TH32CS_SNAPPROCESS)
    if not snap or snap == W.w.HANDLE(-1).value:
        return {}
    found = {}
    try:
        e = W.Entry()
        e.dwSize = W.ctypes.sizeof(W.Entry)
        more = W.k.Process32FirstW(snap, W.ctypes.byref(e))
        while more:
            if e.th32ProcessID:
                found[e.th32ProcessID] = (e.th32ParentProcessID, e.szExeFile)
            more = W.k.Process32NextW(snap, W.ctypes.byref(e))
    finally:
        W.k.CloseHandle(snap)
    return found


def _win_params(pid):
    """(command line, environment block) read from the process's PEB, or None"""
    W = _win()
    ctypes, k = W.ctypes, W.k
    if ctypes.sizeof(ctypes.c_void_p) != 8:
        return None
    h = k.OpenProcess(0x1000 | 0x10, False, pid)   # (PROCESS_QUERY_LIMITED_INFORMATION, PROCESS_VM_READ)
    if not h:
        return None

    def read(addr, size):
        buf = ctypes.create_string_buffer(size)
        got = ctypes.c_size_t()
        if not addr or not k.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(got)):
            return None
        return buf.raw[:got.value]

    try:
        wow = W.w.BOOL()
        if not k.IsWow64Process(h, ctypes.byref(wow)) or wow.value:
            return None
        basic = (ctypes.c_void_p * 6)()   # (PROCESS_BASIC_INFORMATION: [1] is the PEB's address)
        if W.nt.NtQueryInformationProcess(h, 0, basic, ctypes.sizeof(basic), None) != 0:
            return None
        raw = read(basic[1] or 0, 0x28)
        if not raw or len(raw) < 0x28:
            return None
        params = int.from_bytes(raw[0x20:0x28], "little")   # (PEB.ProcessParameters)
        raw = read(params, 0x3F8)   # (RTL_USER_PROCESS_PARAMETERS up to EnvironmentSize)
        if not raw or len(raw) < 0x88:
            return None
        cmd_len = int.from_bytes(raw[0x70:0x72], "little")   # (CommandLine: a UNICODE_STRING)
        cmd = read(int.from_bytes(raw[0x78:0x80], "little"), cmd_len) if cmd_len else b""
        env_at = int.from_bytes(raw[0x80:0x88], "little")
        env_size = int.from_bytes(raw[0x3F0:0x3F8], "little") if len(raw) >= 0x3F8 else 0
        env = read(env_at, min(env_size, 1 << 20)) if env_size else None
        if env is None:   # (no size: read until the block's closing empty string)
            env, at = b"", env_at
            while b"\0\0\0\0" not in env and len(env) < 1 << 20:
                part = read(at, 4096)
                if not part:
                    break
                env, at = env + part, at + len(part)
        return (cmd or b"").decode("utf-16-le", "replace"), env.decode("utf-16-le", "replace")
    finally:
        k.CloseHandle(h)


def _win_split(cmd):
    """A command line as the program's argv (CommandLineToArgvW)"""
    if not cmd.strip():
        return []
    W = _win()
    n = W.ctypes.c_int()
    argv = W.sh.CommandLineToArgvW(cmd, W.ctypes.byref(n))
    if not argv:
        return []
    try:
        return [argv[i] for i in range(n.value)]
    finally:
        W.k.LocalFree(argv)


def exes():
    """{pid: the program's file name} of every process ("" when it can't be read): one snapshot on Windows"""
    if common.WINDOWS:
        return {pid: exe for pid, (_, exe) in _win_snapshot().items()}
    found = {}
    try:
        pids = [int(d) for d in os.listdir("/proc") if d.isdigit()]
    except OSError:
        pids = []
    for pid in pids:
        try:
            found[pid] = os.path.basename(os.readlink("/proc/%d/exe" % pid)).replace(" (deleted)", "")
        except OSError:
            found[pid] = ""
    return found


def comm_of(pid):
    """Linux's short name of the process ("" elsewhere)"""
    if common.WINDOWS:
        return ""
    try:
        with open("/proc/%d/comm" % pid) as f:
            return f.read().strip()
    except OSError:
        return ""


def argv_of(pid):
    if common.WINDOWS:
        got = _win_params(pid)
        return _win_split(got[0]) if got else []
    try:
        with open("/proc/%d/cmdline" % pid, "rb") as f:
            return [a.decode("utf-8", "replace") for a in f.read().split(b"\0") if a]
    except OSError:
        return []


def env_of(pid):
    """None: not readable (starting, ending, or not ours)"""
    if common.WINDOWS:
        got = _win_params(pid)
        if not got:
            return None
        text = got[1]
    else:
        try:
            with open("/proc/%d/environ" % pid, "rb") as f:
                text = f.read().decode("utf-8", "replace")
        except OSError:
            return None
    # (Windows' block holds "=C:=C:\\..." entries for the drives' folders: a name never starts with "=")
    env = dict(kv.split("=", 1) for kv in text.split("\0") if "=" in kv and not kv.startswith("="))
    return env or None


def _children(pid):
    """Linux: the pids pid started (its threads' children)"""
    kids = []
    try:
        for t in os.listdir("/proc/%d/task" % pid):
            try:
                with open("/proc/%d/task/%s/children" % (pid, t)) as f:
                    kids.extend(int(c) for c in f.read().split())
            except OSError:
                pass
    except OSError:
        pass
    return kids


def tree(pid):
    """pid and its descendants. Windows: by parent pid, each started after its parent (a pid used again is not its child)"""
    if common.WINDOWS:
        kids = {}
        for p, (parent, _) in _win_snapshot().items():
            kids.setdefault(parent, []).append(p)
    found, todo = [], [pid]
    while todo:
        p = todo.pop()
        found.append(p)
        if not common.WINDOWS:
            todo.extend(_children(p))
            continue
        born = _win_start(p)
        todo.extend(c for c in kids.get(p, []) if c != p and c not in found and born is not None and (_win_start(c) or 0) >= born)
    return found


def pin(root, cpus):
    """Every thread of root's tree onto cpus (children started later inherit it)"""
    if not hasattr(os, "sched_setaffinity"):
        return
    for p in tree(root):
        try:
            for t in os.listdir("/proc/%d/task" % p):
                try:
                    os.sched_setaffinity(int(t), cpus)
                except OSError:
                    pass
        except OSError:
            pass
