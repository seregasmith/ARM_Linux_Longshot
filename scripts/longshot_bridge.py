"""The control bridge's client library (host/control.c, docs/dev/reference/control-bridge.md). Standard library only.

    Bridge(socket)            one connection: request(cmd, **args) -> the reply; Failed for no game or a timeout
    start_session(game_dir)   a game started with the bridge on, in a run folder a supervisor owns (Session)
    Session(folder)           a run: info(), connect(), request(), log(), stop(), finish(); list_runs(root)
    copy_save, copy_repo_save a save copied in as a run's saves; game_exe(game_dir) the executable a folder starts

scripts/longshot_ctl.py is its command line; the tours (scripts/pipeline_tour.py, scripts/memtour.py) and the tests import
it. Run as a script, it is a run's supervisor (start_session starts it: python -I -B longshot_bridge.py supervise RUN).
"""
import calendar
import codecs
import contextlib
import functools
import importlib.util
import json
import math
import os
import re
import socket
import stat
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path

WINDOWS = os.name == "nt"
CLIENT_VERSION = "1"
MANIFEST_VERSION = 1
LOG_LIMIT = 1024 * 1024


class Failed(Exception):
    def __init__(self, message, code=1):
        super().__init__(message)
        self.code = code


def session_call(function):
    """The callable session API has the same expected-error contract as the command line"""
    @functools.wraps(function)
    def call(*args, **kwargs):
        try:
            return function(*args, **kwargs)
        except (OSError, ValueError, TypeError, KeyError) as e:
            raise Failed(str(e), 2) from e
    return call


def open_windows_pipe(name, timeout):
    """Open a local pipe, retrying only the Windows busy error until the deadline."""
    import ctypes
    import msvcrt
    from ctypes import wintypes

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    create = kernel.CreateFileW
    create.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                       wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE)
    create.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    deadline = time.monotonic() + timeout
    while True:
        handle = create(name, 0xC0000000, 0, None, 3, 0, None)  # read/write, OPEN_EXISTING
        if handle != ctypes.c_void_p(-1).value:
            try:
                fd = msvcrt.open_osfhandle(handle, os.O_RDWR | os.O_BINARY)
            except BaseException:
                kernel.CloseHandle(handle)
                raise
            try:
                return os.fdopen(fd, "r+b", buffering=0)
            except BaseException:
                with contextlib.suppress(OSError):
                    os.close(fd)
                raise
        error = ctypes.get_last_error()
        if error != 231 or time.monotonic() >= deadline:  # ERROR_PIPE_BUSY only
            raise ctypes.WinError(error)
        time.sleep(min(0.02, max(0, deadline - time.monotonic())))


class Bridge:
    """One connection: requests with ids, replies matched to them, events kept for watch"""

    def __init__(self, path, timeout=10.0, session=None):
        self.path, self.next_id, self.pending = path, 0, b""
        self.session = session
        try:
            if WINDOWS:
                name = path if path.startswith("\\\\.\\pipe\\") else "\\\\.\\pipe\\" + path
                self.sock = None
                self.pipe = open_windows_pipe(name, timeout)
            else:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.settimeout(timeout)
                self.sock.connect(path)
        except OSError as e:
            raise Failed("no game listens on %s (%s)" % (path, e.strerror or e), 2)

    def close(self):
        (self.sock or self.pipe).close()

    def server_pid(self):
        """The pid of the process that serves this connection, as the system says (Windows: the pipe's server,
        GetNamedPipeServerProcessId; Linux: the socket's peer, SO_PEERCRED); None when it can't tell"""
        try:
            if WINDOWS:
                import ctypes
                import msvcrt
                from ctypes import wintypes
                k = ctypes.WinDLL("kernel32", use_last_error=True)
                k.GetNamedPipeServerProcessId.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.ULONG)]
                pid = wintypes.ULONG()
                if k.GetNamedPipeServerProcessId(msvcrt.get_osfhandle(self.pipe.fileno()), ctypes.byref(pid)):
                    return pid.value or None
                return None
            import struct
            cred = self.sock.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i"))
            return struct.unpack("3i", cred)[0] or None
        except (OSError, AttributeError, ValueError):
            return None

    def send(self, obj):
        data = (json.dumps(obj, separators=(",", ":")) + "\n").encode()
        if self.sock:
            self.sock.sendall(data)
        else:
            self.pipe.write(data)

    def line(self, timeout):
        """The next line as an object; None when the game went away"""
        deadline = time.monotonic() + timeout
        while b"\n" not in self.pending:
            left = deadline - time.monotonic()
            if left <= 0:
                raise Failed("no answer from the game in %.0f s" % timeout, 3)
            try:
                if self.sock:
                    self.sock.settimeout(left)
                    chunk = self.sock.recv(65536)
                else:
                    chunk = self._pipe_read(left)
                    if chunk is None:
                        continue
            except socket.timeout:
                raise Failed("no answer from the game in %.0f s" % timeout, 3)
            except OSError:
                chunk = b""
            if not chunk:
                return None
            self.pending += chunk
        text, self.pending = self.pending.split(b"\n", 1)
        return json.loads(text.decode("utf-8", "replace"))

    def _pipe_read(self, left):
        """(Windows) what the pipe holds, after waiting up to `left` seconds for some: None for nothing yet, b"" when the
        bridge's end closed. PeekNamedPipe and then a read of only what is there: a read that blocks would hold the
        handle's writes too (one synchronous handle)"""
        import ctypes
        import msvcrt
        handle = msvcrt.get_osfhandle(self.pipe.fileno())
        avail = ctypes.c_ulong(0)
        end = time.monotonic() + min(left, 0.05)
        while True:
            if not ctypes.windll.kernel32.PeekNamedPipe(ctypes.c_void_p(handle), None, 0, None, ctypes.byref(avail), None):
                return b""
            if avail.value:
                return self.pipe.read(avail.value)
            if time.monotonic() >= end:
                return None
            time.sleep(0.002)

    def request(self, cmd, timeout=None, reply_timeout=None, **args):
        """The reply to one request (events that come meanwhile are dropped). Without a timeout the bridge's own default
        holds (for an input: 30 s plus its frames at 10 fps, so a long one can take hours): the client then waits as
        long as the bridge can (an hour at most), unless the connection ends"""
        session = getattr(self, "session", None)
        if session:
            args = session.prepare(cmd, args)
        if cmd == "sequence" and args.get("units") == "hos" and "script" in args:
            args["steps"] = hos_steps(args.pop("script"))
            args.pop("units")
        self.next_id += 1
        rid = self.next_id
        req = {"id": rid, "cmd": cmd}
        req.update({k: v for k, v in args.items() if v is not None})
        if timeout is not None:
            req["timeout"] = timeout
        self.send(req)
        wait = (timeout if timeout is not None else 3600 if cmd in ("press", "stick", "sequence") else 60) + 10
        limit = wait if reply_timeout is None else reply_timeout
        deadline = time.monotonic() + limit
        while True:   # (one deadline: an event line that comes meanwhile does not start the wait again)
            left = deadline - time.monotonic()
            if left <= 0 and b"\n" not in self.pending:
                raise Failed("no answer from the game in %.0f s" % limit, 3)
            msg = self.line(max(0, left))
            if msg is None:
                if cmd == "quit":
                    return {"id": rid, "ok": True, "quitting": True}
                raise Failed("the game closed the connection", 2)
            if msg.get("id") == rid and "ok" in msg:
                if session and msg.get("ok"):
                    session.record(cmd, msg)
                return msg


def check(reply):
    if not reply.get("ok"):
        code = 3 if reply.get("error") == "timeout" else 1
        raise Failed("%s: %s" % (reply.get("error", "error"), reply.get("message", "")), code)
    return reply


def game_exe(game, exe=None):
    """A builder folder's root executable is the launcher, not the game; a source build has no bin folder"""
    if exe:
        return os.path.abspath(exe)
    name = "longshot.exe" if WINDOWS else "longshot"
    nested = os.path.join(game, "bin", name)
    if os.path.isfile(nested) or os.path.isdir(os.path.join(game, "bin")):
        return nested   # (an incomplete builder folder must not fall back to its launcher)
    return os.path.join(game, name)


def hos_steps(script):
    """HOS seconds/ms to game presents at 60 Hz: nearest frame, halves up; a positive hold is at least one frame"""
    steps = []
    for text in script.split(","):
        if not text.strip():
            continue
        match = re.fullmatch(r"\s*([0-9.+eE-]+):([^/,]+)(?:/([0-9.+eE-]+))?\s*", text)
        if not match:
            raise Failed("sequence: expected seconds:buttons[/milliseconds]", 2)
        try:
            at, held = float(match[1]), float(match[3] or 200)
        except ValueError:
            raise Failed("sequence: invalid seconds or milliseconds", 2)
        if not math.isfinite(at) or not math.isfinite(held) or at < 0 or held <= 0 or at > 3600 or held > 3600000:
            raise Failed("sequence: time outside 0..3600 seconds or hold outside 0..3600000 ms", 2)
        steps.append({"at": math.floor(at * 60 + 0.5), "frames": max(1, math.floor(held * 60 / 1000 + 0.5)),
                      "buttons": match[2].strip()})
    if not 1 <= len(steps) <= 256:
        raise Failed("sequence: 1..256 steps", 2)
    return steps


def relaunch(b, timeout):
    """The game started again (the relaunch command), then its status once its bridge answers on the same socket from the
    process started again: one whose uptime is no longer than the wait (its bridge started after the ask), not the old one
    answering before it lets go of the socket (status gives the uptime to 0.1 s: 0.05 s to spare). A connection the old
    one drops mid-request (Windows: a write to its pipe as it closes, EINVAL) is tried again. b is closed; the status reply
    has after_s, the seconds it took.
    In a session (--run DIR) the session follows the game: the run folder's manifest records the relaunch, and then the
    process that serves the socket again, its pid and birth identity as the system gives them (follow_relaunch), so
    stop, show and the run's supervisor go on with the game started again. On Linux that is the same process (execve);
    on Windows it is a new one"""
    session = getattr(b, "session", None)
    ticket = None
    if session:
        info = session.info()
        ticket = {"id": uuid.uuid4().hex, "state": "asked", "asked_at": utc_now(), "timeout": timeout,
                  "from": {"pid": info["pid"], "process_start": info["process_start"]}}
        session.update(relaunch=ticket)
    try:
        check(b.request("relaunch"))
    except BaseException:
        if session:
            session.update(relaunch=dict(ticket, state="refused"))
        raise
    path = b.path
    b.close()
    t0 = time.monotonic()
    while True:
        try:
            nb = Bridge(path, 2)
            try:
                st = nb.request("status", timeout=5)
                server = nb.server_pid() if session else None
            finally:
                nb.close()
            waited = time.monotonic() - t0
            if st.get("ok") and st.get("uptime", float("inf")) <= waited + 0.05:
                st = dict(st, after_s=round(waited, 1))
                if session:
                    st["session"] = follow_relaunch(session, ticket, server)
                return st
        except (Failed, OSError):
            pass
        if time.monotonic() - t0 > timeout:
            if session:
                session.update(relaunch=dict(ticket, state="failed", ended_at=utc_now()))
            raise Failed("the game did not answer again in %.0f s" % timeout, 3)
        time.sleep(0.2)


def follow_relaunch(session, ticket, pid):
    """The run's manifest moved to the process that answers after a relaunch: pid (the socket's server, from the system)
    and its birth identity. The same process (Linux: execve keeps both) is kept as it is; one the client can't tell, or one
    gone already, leaves the run failed to follow (stop and show then refuse, as for any game that is gone).
    A run that ended meanwhile, or whose supervisor stopped waiting for this ticket (relaunched_game: it is no longer
    asked), is left as it is: the game started again is not written into a run nobody watches"""
    birth = process_start(pid)
    with run_lock(session.folder):
        info = session._read()
        current = info.get("relaunch") or {}
        if info["state"] in ("finished", "failed") or current.get("id") != ticket["id"] or current.get("state") != "asked":
            raise Failed("relaunched, but the run ended before the game answered again: the run does not follow it: %s"
                         % session.folder, 3)
        if birth is None:
            state = "lost"
        elif pid == info["pid"] and birth == info["process_start"]:
            state = "same-process"
        else:
            state = "followed"
            info.update(pid=pid, process_start=birth, relaunches=info.get("relaunches", 0) + 1)
        info["relaunch"] = dict(ticket, state=state, ended_at=utc_now(), to={"pid": pid, "process_start": birth})
        write_json(session.manifest, info)
    if state == "lost":
        raise Failed("relaunched, but the process serving the socket can't be told: the run no longer follows the game: %s"
                     % session.folder, 3)
    return state


def utc_now():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def process_start(pid):
    """A live process's birth identity, not just its reusable pid; None for a dead process (including a Linux zombie)"""
    if not pid:
        return None
    if WINDOWS:
        import ctypes
        from ctypes import wintypes
        k = ctypes.WinDLL("kernel32", use_last_error=True)
        k.OpenProcess.restype = wintypes.HANDLE
        k.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        k.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
        k.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        k.CloseHandle.argtypes = [wintypes.HANDLE]
        h = k.OpenProcess(0x100000 | 0x1000, False, pid)   # (SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION)
        if not h:
            return None
        try:
            times = [wintypes.FILETIME() for _ in range(4)]
            if k.WaitForSingleObject(h, 0) != 258 or not k.GetProcessTimes(h, *[ctypes.byref(t) for t in times]):
                return None
            return {"clock": "windows-filetime", "ticks": (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime}
        finally:
            k.CloseHandle(h)
    try:
        fields = Path("/proc/%d/stat" % pid).read_text().rsplit(")", 1)[1].split()
        if fields[0] in ("Z", "X"):
            return None
        return {"clock": "linux-start-ticks", "ticks": int(fields[19]),
                "boot": Path("/proc/sys/kernel/random/boot_id").read_text().strip()}
    except (OSError, ValueError, IndexError):
        return None


def same_process(pid, birth):
    return birth is not None and process_start(pid) == birth


def owners_alive(info):
    return any(same_process(**info[name]) for name in ("owner", "launcher") if info.get(name))


def wait_owners(session, timeout=15):
    deadline = time.monotonic() + timeout
    while owners_alive(session.info()):
        if time.monotonic() >= deadline:
            raise Failed("game exited, but its owner or wrapper has not exited: %s" % session.folder, 3)
        time.sleep(0.02)


def write_json(path, value):
    tmp = Path(str(path) + "." + uuid.uuid4().hex + ".tmp")
    try:
        with tmp.open("x", encoding="utf-8") as f:
            json.dump(value, f, indent=2)
            f.write("\n")
        os.replace(tmp, path)
    finally:
        tmp.unlink(missing_ok=True)


@contextlib.contextmanager
def run_lock(folder):
    """Only a manifest update holds this lock; never a bridge request or a process wait"""
    with (Path(folder) / "manifest.lock").open("r+b") as f:
        if WINDOWS:
            import msvcrt
            msvcrt.locking(f.fileno(), msvcrt.LK_LOCK, 1)
        else:
            import fcntl
            fcntl.flock(f, fcntl.LOCK_EX)
        try:
            yield
        finally:
            f.seek(0)
            if WINDOWS:
                msvcrt.locking(f.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(f, fcntl.LOCK_UN)


def dev_home():
    """This PC's development folder (scripts/devtools/common.py dev_home, `dev.py paths`), loaded by its path: this client
    imports nothing of the checkout's"""
    spec = importlib.util.spec_from_file_location(
        "devtools_common", os.path.join(os.path.dirname(os.path.abspath(__file__)), "devtools", "common.py"))
    common = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(common)
    return Path(common.dev_home())


def runs_root():
    """Where start_session makes a run folder when it is given neither run nor root: runs/control in the development
    folder (the one place that says so)"""
    return dev_home() / "runs" / "control"


@session_call
def list_runs(root=None):
    root = Path(root) if root else runs_root()
    return [Session(p.parent).info() for p in sorted(root.glob("*/manifest.json"))]


class Session:
    """A durable run created by this client; a surviving supervisor, not an attaching caller, owns its process"""

    @session_call
    def __init__(self, folder):
        self.folder = Path(folder).resolve()
        self.manifest = self.folder / "manifest.json"

    @session_call
    def info(self):
        with run_lock(self.folder):
            return self._read()

    def _read(self):
        with self.manifest.open(encoding="utf-8") as f:
            value = json.load(f)
        if value.get("schema_version") != MANIFEST_VERSION:
            raise Failed("unsupported run manifest version", 2)
        return value

    @session_call
    def update(self, **fields):
        with run_lock(self.folder):
            value = self._read()
            value.update(fields)
            write_json(self.manifest, value)
        return value

    @classmethod
    @session_call
    def attach(cls, folder):
        session = cls(folder)
        session.require_live()
        return session

    @session_call
    def require_live(self):
        info = self.info()
        if not same_process(info["pid"], info["process_start"]):
            raise Failed("run's game is gone or its pid was reused: %s" % self.folder, 2)
        return info

    @session_call
    def connect(self, timeout=10):
        return Bridge(self.require_live()["socket"], timeout, session=self)

    def prepare(self, cmd, args):
        args = dict(args)
        if cmd in ("press", "stick", "sequence") and args.get("exclusive") is None:
            if self.info()["profile"] == "test":
                args["exclusive"] = True
        if cmd == "shot" and not args.get("path"):
            args["path"] = str(self.folder / "shots" / ("shot-" + uuid.uuid4().hex + ".png"))
        return args

    def record(self, cmd, reply):
        if cmd != "shot":
            return
        # (Only metadata from the reply: an untrusted game must not make the client open an arbitrary file)
        artifact = {k: reply.get(k) for k in ("path", "frame", "present", "width", "height")}
        artifact["kind"] = "shot"
        with run_lock(self.folder):
            value = self._read()
            value["artifacts"].append(artifact)
            write_json(self.manifest, value)

    @session_call
    def request(self, cmd, timeout=None, **args):
        b = self.connect()
        try:
            return check(b.request(cmd, timeout=timeout, **args))
        finally:
            b.close()

    @session_call
    def finish(self):
        """Mark an orphaned, dead run; only its supervisor can know an exit code"""
        with run_lock(self.folder):
            info = self._read()
            if same_process(info["pid"], info["process_start"]) or owners_alive(info):
                raise Failed("run still has a live game or owner; use stop", 2)
            if info["state"] not in ("finished", "failed"):
                info.update(state="finished", ended_at=utc_now(), stop_method="observed-exit", save_drain="unknown",
                            process_exited=True)
                write_json(self.manifest, info)
        return info

    @session_call
    def stop(self, timeout=10, terminate=False):
        """Ask the original supervisor to quit and reap its child; never signal a pid read from a manifest"""
        if not math.isfinite(timeout) or timeout <= 0:
            raise Failed("stop timeout must be positive seconds", 2)
        request = None
        with run_lock(self.folder):
            info = self._read()
            if info["state"] not in ("finished", "failed"):
                if not same_process(**info["owner"]):
                    raise Failed("the run's owner is gone; an attached client will not kill its game", 2)
                if not same_process(info["pid"], info["process_start"]):
                    raise Failed("run's game is gone or its pid was reused", 2)
                if info.get("stop_request"):
                    raise Failed("a stop is already being waited for", 2)
                request = {"id": uuid.uuid4().hex, "timeout": timeout, "terminate": terminate,
                           "pid": info["pid"], "process_start": info["process_start"]}
                info["stop_request"] = request
                write_json(self.manifest, info)
        deadline = time.monotonic() + timeout + 15
        while True:
            info = self.info()
            if info["state"] in ("finished", "failed"):
                wait_owners(self, max(0, deadline - time.monotonic()))
                return info
            result = info.get("stop_result", {})
            if request and result.get("id") == request["id"] and not result.get("process_exited"):
                return info
            if not same_process(**info["owner"]):
                raise Failed("run owner ended without an exit receipt; inspect the run and use finish", 2)
            if time.monotonic() >= deadline:
                raise Failed("no stop receipt yet: %s (the run remains owned)" % self.folder, 3)
            time.sleep(0.05)

    @session_call
    def log(self, lines=100, since=None, limit=65536):
        """A byte cursor into this run's append-only UTF-8 log; every read and returned text is bounded"""
        if not 1 <= limit <= LOG_LIMIT or not 0 <= lines <= 10000 or (since is not None and since < 0):
            raise Failed("log: bytes 1..1048576, lines 0..10000, cursor >= 0", 2)
        info = self.info()
        with open(info["log"], "rb") as f:
            size = os.fstat(f.fileno()).st_size
            if since is not None and since > size:
                raise Failed("log cursor is past the end (the file was truncated?)", 2)
            start = max(0, size - limit) if since is None else since
            f.seek(start)
            data = f.read(min(limit, size - start))
        cursor = start + len(data)
        decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
        text = decoder.decode(data, final=cursor == size and info["state"] in ("finished", "failed"))
        cursor -= len(decoder.getstate()[0])   # (at most three trailing bytes: the next read starts before them)
        if since is None:
            if start:
                text = text.partition("\n")[2]   # (a clipped first line, possibly starting inside a character)
            text = "".join(text.splitlines(keepends=True)[-lines:]) if lines else ""
        return {"text": text, "cursor": cursor, "start": start,
                "more": cursor < size, "truncated": start > 0 if since is None else False}

    @session_call
    def expect_log(self, text, since, timeout=10, regex=False, limit=65536):
        if not text or len(text) > 4096 or not math.isfinite(timeout) or timeout <= 0:
            raise Failed("log expect: a pattern of 1..4096 characters and a positive timeout", 2)
        try:
            pattern = re.compile(text) if regex else None
        except re.error as e:
            raise Failed("log regex: %s" % e, 2)
        deadline, cursor, pending = time.monotonic() + timeout, since, ""
        while True:
            part = self.log(since=cursor, limit=limit)
            pending += part["text"]
            if (pattern.search(pending) if pattern else text in pending):
                return dict(part, text=pending[-limit:], matched=True)
            pending = pending[-limit:]
            advanced = part["cursor"] != cursor
            cursor = part["cursor"]
            if time.monotonic() >= deadline:
                raise Failed("log expect timed out after cursor %d: %s" % (since, text), 3)
            if not part["more"] or not advanced:
                time.sleep(0.05)


def ready_status(reply, level):
    if not reply.get("ok"):
        return False
    if level == "bridge":
        return True
    if reply.get("frame", 0) < 1:
        return False
    if level == "present":
        return True
    game = reply.get("game") or {}
    return reply.get("state") == "play" and not reply.get("stalled", True) and game.get("event") is False \
        and game.get("loading") is False


def wait_ready(session, level, timeout, process):
    def check_exit():
        code = process.poll()
        if code is not None:
            raise Failed("game exited %d before ready=%s" % (code, level), 2)

    deadline, last = time.monotonic() + timeout, "no status reply"
    while True:
        check_exit()   # (also before testing the deadline: an already-exited child is not a readiness timeout)
        if (session.folder / "cancel").exists():
            raise Failed("launch cancelled while waiting for readiness", 3)
        left = deadline - time.monotonic()
        if left <= 0:
            check_exit()   # (the child may have exited between the poll and the deadline check)
            raise Failed("ready=%s timed out: %s; see %s" % (level, last, session.info()["log"]), 3)
        b, reply = None, None
        try:
            b = Bridge(session.info()["socket"], min(1, left))
            reply = b.request("status", reply_timeout=max(0, min(1, deadline - time.monotonic())))
            last = "state=%s frame=%s game=%s" % (reply.get("state"), reply.get("frame"), reply.get("game"))
        except Failed as e:
            last = str(e)
        finally:
            if b:
                b.close()
        check_exit()   # (a connection failure or reply can arrive after the child exited)
        if reply is not None and ready_status(reply, level):
            return reply
        time.sleep(min(0.05, max(0, deadline - time.monotonic())))


def stop_child(process, info, timeout, terminate):
    deadline, method, error = time.monotonic() + timeout, "quit", None
    b = None
    try:
        b = Bridge(info["socket"], min(timeout, 1))
        check(b.request("quit", reply_timeout=max(0.001, deadline - time.monotonic())))
    except (Failed, OSError) as e:
        error = str(e)
    finally:
        if b:
            b.close()
    try:
        process.wait(timeout=max(0, deadline - time.monotonic()))
    except subprocess.TimeoutExpired:
        if process.poll() is None:   # (wait can lose the race to an exit at its deadline)
            if not terminate:
                return {"method": "quit-timeout", "process_exited": False, "forced": False,
                        "save_drain": "unsupported", "error": error}
            method = terminate_child(process)
    return {"method": method, "process_exited": True, "forced": method in ("terminate", "kill"), "exit_code": process.returncode,
            "save_drain": "unsupported", "error": error}


def terminate_child(process):
    # (Only a Popen child held by its original owner, never os.kill(manifest["pid"]))
    if process.poll() is not None:
        return "already-exited"
    process.terminate()
    try:
        process.wait(timeout=5)
        return "terminate"
    except subprocess.TimeoutExpired:
        if process.poll() is None:
            process.kill()
            process.wait()
            return "kill"
        return "terminate"


class Adopted:
    """The game a Windows relaunch started (host/hos_relaunch.c: a new process, not this supervisor's child), held by a
    handle this supervisor opened while the process's birth identity was the manifest's, so a pid reused later is never
    waited for or ended. It answers as the Popen child does where supervise, stop_child and terminate_child ask it"""

    def __init__(self, pid, birth):
        import ctypes
        from ctypes import wintypes
        self.k = ctypes.WinDLL("kernel32", use_last_error=True)
        self.k.OpenProcess.restype = wintypes.HANDLE
        self.k.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        self.k.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        self.k.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        self.k.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
        self.k.CloseHandle.argtypes = [wintypes.HANDLE]
        self.pid, self.birth, self.returncode = pid, birth, None
        # (SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION; the identity checked after the handle is
        # held, so the process the handle names is the one that was checked)
        self.h = self.k.OpenProcess(0x100000 | 0x0001 | 0x1000, False, pid)
        if not self.h or process_start(pid) != birth:
            self.close()
            raise Failed("the relaunched game is gone or its pid was reused", 2)

    def close(self):
        if self.h:
            self.k.CloseHandle(self.h)
            self.h = None

    def _wait(self, ms):
        from ctypes import wintypes
        import ctypes
        if self.returncode is not None:
            return True
        if self.k.WaitForSingleObject(self.h, ms) != 0:
            return False
        code = wintypes.DWORD()
        self.returncode = code.value if self.k.GetExitCodeProcess(self.h, ctypes.byref(code)) else 1
        return True

    def poll(self):
        return self.returncode if self._wait(0) else None

    def wait(self, timeout=None):
        if not self._wait(0xFFFFFFFF if timeout is None else max(0, int(timeout * 1000))):
            raise subprocess.TimeoutExpired("relaunched game", timeout)
        return self.returncode

    def terminate(self):
        if self.returncode is None:
            self.k.TerminateProcess(self.h, 1)

    kill = terminate


def relaunched_game(session, process, birth):
    """After the game the supervisor watches ended: the game a relaunch of it started, when the client followed it to a new
    process (follow_relaunch), held as Adopted; else None, and the run ends. A relaunch still being waited for by its
    client is waited for, as long as the client said it would wait and a little more, counted from the ask (asked_at), so
    a ticket left asked by a client that ended mid-wait does not hold up a later exit. Giving up marks the ticket failed
    under the manifest's lock, so a client that follows the game later finds it so and leaves the run alone"""
    since = time.time()   # (for a ticket without asked_at: from the exit)
    while True:
        info = session.info()
        ticket = info.get("relaunch") or {}
        mine = ticket.get("from") == {"pid": process.pid, "process_start": birth}
        if not mine or info.get("stop_request") or ticket.get("state") not in ("asked", "followed"):
            return None
        if ticket["state"] == "followed":
            if not WINDOWS or ticket["to"]["pid"] == process.pid:
                return None
            try:
                return Adopted(info["pid"], info["process_start"])
            except Failed:
                return None
        if time.time() >= relaunch_deadline(ticket, since):
            with run_lock(session.folder):
                info = session._read()
                current = info.get("relaunch") or {}
                if current.get("id") == ticket.get("id") and current.get("state") == "asked":
                    info["relaunch"] = dict(current, state="failed", ended_at=utc_now())
                    write_json(session.manifest, info)
                    return None
            continue   # (the client wrote its ticket meanwhile: read again)
        time.sleep(0.05)


def relaunch_deadline(ticket, since):
    """When a supervisor stops waiting for a relaunch (the system's clock, seconds): the client's --timeout from its ask,
    and 10 s more, for the client's last try (a 2 s open, a 5 s status) and asked_at's whole seconds; from since when the
    ticket has no asked_at"""
    try:
        asked = calendar.timegm(time.strptime(ticket["asked_at"], "%Y-%m-%dT%H:%M:%SZ"))
    except (KeyError, TypeError, ValueError):
        asked = since
    return asked + float(ticket.get("timeout") or 120) + 10


def supervise(folder):
    session, process = Session(folder), None
    info = session.update(owner={"pid": os.getpid(), "birth": process_start(os.getpid())}, state="starting")
    method, error, code = "exit", None, 0
    try:
        if (session.folder / "cancel").exists():
            raise Failed("launch cancelled before admission", 3)
        with open(info["log"], "ab", buffering=0) as out:
            process = subprocess.Popen(info["command"], cwd=info["cwd"], env=dict(os.environ, **info["environment"]),
                                       stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT, close_fds=True)
        birth = process_start(process.pid)
        info = session.update(pid=process.pid, process_start=birth, started_at=utc_now())
        reply = wait_ready(session, info["ready"], info["ready_timeout"], process)
        session.update(state="running", ready_reply=reply)
        while True:
            while process.poll() is None:
                info = session.info()
                request = info.get("stop_request")
                if request:
                    if request["pid"] != process.pid or request["process_start"] != birth:
                        result = {"method": "refused", "process_exited": False, "error": "process identity changed"}
                    else:
                        session.update(state="stopping")
                        result = stop_child(process, info, request["timeout"], request["terminate"])
                    method = result["method"]
                    session.update(stop_result=dict(result, id=request["id"]), stop_request=None,
                                   state="stopping" if result["process_exited"] else "running")
                if (session.folder / "cancel").exists():
                    method = "cancel-" + terminate_child(process)
                time.sleep(0.05)
            followed = relaunched_game(session, process, birth)
            if followed is None:
                break
            process, birth = followed, followed.birth   # (the game started again: watched and stopped as the first one was)
    except BaseException as e:
        error, code = str(e), e.code if isinstance(e, Failed) else 2
        if process is not None:
            method = "startup-" + terminate_child(process)
    finally:
        session.update(state="failed" if error else "finished", error=error, error_code=code, ended_at=utc_now(),
                       exit_code=process.wait() if process is not None else None, stop_method=method,
                       process_exited=True, save_drain="unsupported")
    return code


# (the one account the runtime has, host/nn_applet.c user_uid: its saves are <data>/longshot/save/<this>)
SAVE_USER = "5a3c9e1f0b7d24680000000000000001"


def slot_name(text):
    """03, 3 or slot_03 -> slot_03"""
    t = text[5:] if text.lower().startswith("slot_") else text
    if not t.isdigit() or len(t) > 2:
        raise Failed("--slot: a slot number, 03 or slot_03", 2)
    return "slot_%02d" % int(t)


def plain_tree(path, flag="--save"):
    """Refused (Failed) when path or anything under it is a link or not a plain file or folder: a save from elsewhere could
    hold a link to one of this user's files, which the copy would read and the game then write through"""
    for top, dirs, files in os.walk(path):
        for n in [top] + [os.path.join(top, d) for d in dirs] + [os.path.join(top, f) for f in files]:
            st = os.lstat(n)
            if not (stat.S_ISDIR(st.st_mode) or stat.S_ISREG(st.st_mode)) or \
                    getattr(st, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0):
                raise Failed("%s: %s is a link or not a plain file: refused" % (flag, n), 2)


def plain_save_dirs(root, flag="--save"):
    """The two entries the copier creates must be directories, not links; /home or a data root may itself be a link"""
    for path in (Path(root) / "save", Path(root) / "save" / SAVE_USER):
        try:
            st = path.lstat()
        except FileNotFoundError:
            continue
        if not stat.S_ISDIR(st.st_mode) or getattr(st, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0):
            raise Failed("%s: %s is a link or not a plain folder: refused" % (flag, path), 2)


@session_call
def copy_save(src, data=None, slot=None, user=None):
    """A save folder (account.sav, option.sav, slot_NN/) copied to where the game reads it in the data folder; with slot,
    that slot alone (so Continue loads it). The source is only read, and holds plain files and folders only (no links).
    Returns the destination"""
    src = os.path.abspath(src)
    if os.path.islink(src) or not os.path.isdir(src):
        raise Failed("--save: %s is not a folder" % src, 2)
    plain_tree(src)
    names = os.listdir(src)
    slots = sorted(n for n in names if n.startswith("slot_") and os.path.isdir(os.path.join(src, n)))
    if not slots or not ({"account.sav", "option.sav"} & set(names)):
        raise Failed("--save: %s does not look like a save folder (account.sav or option.sav, and slot_NN folders): "
                     "it is <data>/longshot/save/<user id>" % src, 2)
    if slot is not None:
        slot = slot_name(slot)
        if slot not in slots:
            raise Failed("--save: %s has no %s (it has %s)" % (src, slot, ", ".join(slots)), 2)
    entries = [(os.path.join(src, n), n) for n in names
               if (n.startswith("slot_") and os.path.isdir(os.path.join(src, n)) and (slot is None or n == slot)) or
               os.path.isfile(os.path.join(src, n))]
    return place_save(entries, [src], data, user, "--save")


def place_save(entries, sources, data, user, flag):
    """entries ([(file or folder, its name in the save)]) copied into the data folder's save folder, which must be empty;
    none of sources (the folders they come from) may hold it or be in it. Returns the destination"""
    import shutil
    root = os.path.abspath(user or os.path.join(data, "longshot"))
    dest = os.path.join(root, "save", SAVE_USER)
    plain_save_dirs(root, flag)
    dest_real = os.path.normcase(os.path.realpath(dest))
    for src in sources:
        source_real = os.path.normcase(os.path.realpath(src))
        try:
            overlap = os.path.commonpath([source_real, dest_real]) in (source_real, dest_real)
        except ValueError:   # (different Windows drives cannot overlap)
            overlap = False
        if overlap:
            raise Failed("%s: source and destination must be separate plain folders" % flag, 2)
    if os.path.exists(dest) and os.listdir(dest):
        raise Failed("%s: %s already holds saves: give an empty --data (or none: the run folder's own)" % (flag, dest), 2)
    os.makedirs(dest, exist_ok=True)
    for p, n in entries:
        if os.path.isdir(p):
            shutil.copytree(p, os.path.join(dest, n), symlinks=True)   # (none: plain_tree; never followed)
        else:
            shutil.copy2(p, os.path.join(dest, n), follow_symlinks=False)
    return dest


# Saves in the repository (assets/saves, CONTRIBUTING.md; scripts/longshot_saves.py scrub and check-repo-saves):
# <name>/{README.md, slot_00/{progress.sav, caption.sav}} only: account.sav and option.sav are never shipped (the game
# makes them when it loads a slot on its own)
CHECKOUT = Path(__file__).resolve().parents[1]
REPO_SAVE_NAME = re.compile(r"[a-z0-9][a-z0-9-]{0,47}")
REPO_SAVE_FILES = {"README.md": "file", "slot_00": "dir"}
REPO_SLOT_FILES = {"progress.sav": "file", "caption.sav": "file"}


def repo_saves_dir(repo=None):
    return os.path.join(os.path.abspath(repo) if repo else str(CHECKOUT), "assets", "saves")


def repo_save_names(repo=None):
    root = repo_saves_dir(repo)
    try:
        names = os.listdir(root)
    except OSError:
        return []
    return sorted(n for n in names if REPO_SAVE_NAME.fullmatch(n) and not os.path.islink(os.path.join(root, n)) and
                  os.path.isdir(os.path.join(root, n)))


def only_files(folder, wanted, label):
    """folder holds exactly wanted ({name: 'file' or 'dir'}), as plain files and folders (plain_tree ran first)"""
    have = set(os.listdir(folder))
    extra, missing = sorted(have - set(wanted)), sorted(set(wanted) - have)
    wrong = sorted(n for n in have & set(wanted) if os.path.isdir(os.path.join(folder, n)) != (wanted[n] == "dir"))
    if extra or missing or wrong:
        raise Failed("--repo-save: %s is not a repo save's layout (%s): refused" % (label, "; ".join(
            x for x in ("not expected: " + ", ".join(extra) if extra else "", "missing: " + ", ".join(missing) if missing
                        else "", "not the right kind: " + ", ".join(wrong) if wrong else "") if x)), 2)


@session_call
def copy_repo_save(name, data=None, user=None, repo=None):
    """The repo save `name` (in the checkout's assets/saves, or repo's) copied to where the game reads it, as copy_save
    does: the save's slot_00 alone (the game makes account.sav and option.sav itself). Only the expected files, no links;
    a _profile folder beside the saves (an old layout, with someone's account.sav and option.sav) is refused. Returns the
    destination"""
    root = repo_saves_dir(repo)
    known = repo_save_names(repo)
    if not REPO_SAVE_NAME.fullmatch(name or "") or name not in known:
        raise Failed("--repo-save: no repo save %r in %s (%s)" % (name, root, "it has: " + ", ".join(known) if known else
                                                                   "it has none"), 2)
    save, profile = os.path.join(root, name), os.path.join(root, "_profile")
    if os.path.islink(root) or not os.path.isdir(root):
        raise Failed("--repo-save: %s is not a folder" % root, 2)
    if os.path.lexists(profile):
        raise Failed("--repo-save: %s is not shipped any more (account.sav and option.sav are made by the game): "
                     "refused" % profile, 2)
    plain_tree(save, "--repo-save")
    only_files(save, REPO_SAVE_FILES, save)
    only_files(os.path.join(save, "slot_00"), REPO_SLOT_FILES, os.path.join(save, "slot_00"))
    return place_save([(os.path.join(save, "slot_00"), "slot_00")], [save], data, user, "--repo-save")


@session_call
def start_session(game_dir, *, run=None, root=None, exe=None, image=None, romfs=None, user=None, cwd=None, data=None,
                  save=None, slot=None, profile="default", headless=False, sock=None, env=None, ready="bridge",
                  timeout=600, wrapper=None, repo_save=None, repo=None):
    """Start a durable supervisor, optionally through an admission wrapper (argv, never a shell command)"""
    game = os.path.abspath(game_dir)
    executable = game_exe(game, exe)
    if not os.path.isfile(executable):
        raise Failed("no game at %s" % executable, 2)
    if slot and not save:
        raise Failed("--slot goes with --save", 2)
    if save and repo_save:
        raise Failed("--save or --repo-save, not both", 2)
    if repo and not repo_save:
        raise Failed("--repo goes with --repo-save", 2)
    if profile not in ("default", "test") or ready not in ("bridge", "present", "play"):
        raise Failed("unknown profile or ready level", 2)
    if not math.isfinite(timeout) or timeout <= 0:
        raise Failed("launch timeout must be positive seconds", 2)
    if profile == "test" and (user or data):
        raise Failed("test profile owns its user folder; use --save or --repo-save for a copy, not --user or --data", 2)
    if cwd and not image:
        raise Failed("--cwd needs --image (otherwise the game changes back to its image folder)", 2)
    if WINDOWS and (save or repo_save) and not user and profile != "test":
        raise Failed("%s on Windows needs --user or --profile test" % ("--save" if save else "--repo-save"), 2)
    changes = dict(env or {})
    if any(not k or "=" in k or "\0" in k + v for k, v in changes.items()):
        raise Failed("--env NAME=VALUE", 2)
    if wrapper is not None and (not isinstance(wrapper, list) or not all(isinstance(s, str) and s for s in wrapper)):
        raise Failed("wrapper must be a JSON array of command arguments", 2)
    if run:
        folder = Path(run).absolute()
        if folder.is_symlink() or (folder.exists() and (not folder.is_dir() or any(folder.iterdir()))):
            raise Failed("run folder must be new or empty: %s" % folder, 2)
        folder.mkdir(mode=0o700, parents=True, exist_ok=True)
        folder = folder.resolve()
    else:
        parent = Path(root) if root else runs_root()
        parent.mkdir(parents=True, exist_ok=True)
        folder = Path(tempfile.mkdtemp(prefix="run-", dir=parent)).resolve()
    if not WINDOWS:
        folder.chmod(0o700)
    with (folder / "manifest.lock").open("xb") as lock:
        lock.write(b"\0")
    (folder / "shots").mkdir()
    log = folder / "log.txt"
    log.touch()
    user = os.path.abspath(user) if user else str(folder / "user") if profile == "test" else None
    data = os.path.abspath(data) if data else str(folder / "data") if (save or repo_save) and not user else None
    if save:
        copy_save(save, data, slot, user=user)
    elif repo_save:
        copy_repo_save(repo_save, data, user=user, repo=repo)
    if user:
        Path(user).mkdir(parents=True, exist_ok=True)
    if data:
        changes.setdefault("XDG_DATA_HOME", data)
    if headless:
        changes.setdefault("HOS_HEADLESS", "1")
    socket_dir = None
    if not sock:
        if WINDOWS:
            sock = "longshot-control-" + uuid.uuid4().hex
        else:
            runtime = os.environ.get("XDG_RUNTIME_DIR") or tempfile.gettempdir()
            if len(os.fsencode(runtime)) > 48:
                runtime = "/tmp"
            socket_dir = tempfile.mkdtemp(prefix="hctl-", dir=runtime)
            sock = os.path.join(socket_dir, "control.sock")
    if not WINDOWS:
        sock = os.path.abspath(sock)
    # (Keep the default profile's explicit --env precedence; record the effective endpoint too)
    changes.setdefault("HOS_CONTROL", sock)
    sock = changes["HOS_CONTROL"]
    if profile == "test":
        empty = folder / "empty-env.txt"
        empty.touch()
        changes.update(HOS_HEADLESS="1", HOS_AUDIO="0", HOS_ENV_FILE=str(empty))
    command = [executable]
    if image:
        command.append(os.path.abspath(image))
    for flag, value in (("romfs", romfs), ("user", user)):
        if value:
            command.extend(["--" + flag, os.path.abspath(value)])
    working = os.path.abspath(cwd) if cwd else game
    user_path = user or (os.path.join(working, "user") if WINDOWS or image else
                        os.path.join(changes.get("XDG_DATA_HOME", os.environ.get("XDG_DATA_HOME") or
                                                os.path.expanduser("~/.local/share")), "longshot"))
    session = Session(folder)
    info = {"schema_version": MANIFEST_VERSION, "client_version": CLIENT_VERSION, "run": str(folder),
            "game": game, "executable": executable, "command": command, "cwd": working, "environment": changes,
            "profile": profile, "user": user_path, "save_source": os.path.abspath(save) if save else
            os.path.join(repo_saves_dir(repo), repo_save) if repo_save else None, "repo_save": repo_save, "slot": slot,
            "socket": sock, "socket_dir": socket_dir, "log": str(log), "pid": None, "process_start": None,
            "owner": {"pid": None, "birth": None}, "created_at": utc_now(), "started_at": None, "ended_at": None,
            "state": "queued", "exit_code": None, "stop_method": None, "save_drain": "unsupported",
            "process_exited": False, "artifacts": [], "ready": ready, "ready_timeout": timeout, "wrapper": wrapper or []}
    write_json(session.manifest, info)
    command = (wrapper or []) + [sys.executable, "-I", "-B", str(Path(__file__).resolve()), "supervise", str(folder)]
    # (Windows: the owner gets a console of its own with no window, apart from the caller's, and the game inherits it. With
    # no console at all (DETACHED_PROCESS) Windows opened a new console window for the game, one each launch)
    flags = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_NO_WINDOW} if WINDOWS \
        else {"start_new_session": True}
    try:
        with (folder / "owner.log").open("ab") as out:
            owner = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT,
                                     close_fds=True, **flags)
    except OSError as e:
        session.update(state="failed", error=str(e), error_code=2, ended_at=utc_now(), process_exited=True)
        raise
    session.update(launcher={"pid": owner.pid, "birth": process_start(owner.pid)})
    import threading
    threading.Thread(target=owner.wait, daemon=True).start()   # (reap it if this caller outlives the supervisor)
    try:
        while True:
            info = session.info()
            if info["state"] == "running":
                return session
            if info["state"] == "failed":
                wait_owners(session)
                raise Failed(info["error"], info["error_code"])
            if owner.poll() is not None:
                error = "launch owner exited %s; see %s" % (owner.returncode, folder / "owner.log")
                session.update(state="failed", error=error, error_code=2, process_exited=info["pid"] is None,
                               ended_at=utc_now())
                raise Failed(error, 2)
            time.sleep(0.05)
    except BaseException:
        (folder / "cancel").touch()   # (also checked after a queued admission: no game may start later)
        raise


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "supervise":
        sys.exit("usage: longshot_bridge.py supervise RUN_FOLDER (started by start_session)")
    sys.exit(supervise(sys.argv[2]))
