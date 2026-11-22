"""The launcher's side of the performance statistics (gpu/vk/perf_report.cpp is the game's): the tick, and sending what
the game left in the game folder's perf_pending/.

  - The launcher's "Send performance statistics" tick is send=1 in the game folder's longshot_perf.ini, off by default.
    The game reads it at its start: off, it measures nothing and writes nothing.
  - On, the game writes a summary (JSON, the relay's version 1) every 5 minutes and at exit into perf_pending/. The
    launcher sends them at its next start, in a process of its own (start_sending), to perf_reports in nx2/addresses.ini (Longshot-api's telemetry),
    oldest first, and deletes each one the relay took or refused; one it couldn't reach stays for next time.
  - The sender fills in version and release (the release flag as the crash report has it: a release's version, built
    from the release's own sources), which the game can't know.
  - Each summary the relay took is also kept, as it was sent, in perf_sent/ (the 48 sent last): what went to the team, for the
    player or a tester to read. The launcher's "Keep a copy of what was sent" (keep_sent=0 in longshot_perf.ini when it is
    off), or LONGSHOT_PERF_KEEP_SENT, turns that off, and empties perf_sent/.
  - Turning the tick off deletes what is waiting, and what perf_sent/ kept: the player said no.
  - Polite by default: the summaries go over one connection, at most LONGSHOT_PERF_MAX_SENDS (5: under the perf route's
    ingress, 6 a minute with a burst of 6) a start, one sender at a time (perf_pending/.sending, a lease its holder renews), and a relay that
    answers busy (429 or 5xx) is left alone until the time it names (Retry-After), else an hour, doubled each time it is
    busy again up to a day (perf_pending/.next). A status that is not the relay saying the summary will never be taken
    (only 400, 413, 415, 422) keeps the summary for the next start. The switches: docs/dev/reference/profiling.md,
    "Performance statistics".

python -I nx2/perf_report.py <game folder>            what would be sent, sends nothing
python -I nx2/perf_report.py <game folder> --send     sends it (what the launchers start: start_sending)
python -I nx2/perf_report.py <game folder> --tick on|off [--keep-sent on|off]
                                                      the launcher's tick (set_sending): off deletes what waits
"""
import email.utils
import http.client
import json
import os
import re
import subprocess
import threading
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from addresses import address  # noqa: E402

ENDPOINT = address("perf_reports")
CHOICE = "longshot_perf.ini"   # (send=1: the launcher's tick; the game reads it, gpu/vk/perf_report.cpp)
PENDING = "perf_pending"
SENT = "perf_sent"   # (a copy of each summary the relay took, as sent: the newest MAX_PENDING)
VERSION_FILE = "longshot_version.txt"
# The game's logs, newest first: in logs/ (host/game_layout.c), else at the top of folders made before it moved there
LOGS = [os.path.join(d, name) for d in ("logs", "") for name in ["longshot_log.txt"] + ["longshot_log.%d.txt" % k for k in range(1, 5)]]
MAX_PENDING = 48   # (the newest kept when the launcher hasn't been back for long: older ones go unsent)
MAX_BODY = 60 * 1024   # (the relay's limit is 64 KiB: a bigger one is refused there anyway)
# A release's version (docs/dev/reference/releases.md), as the crash report has it (host/hos_crash_dialog.h)
RELEASE_VERSION = re.compile(r"^v?\d+\.\d+\.\d+(?:-rc\.\d+)?$|^nightly-\d+-[0-9a-f]{8}$")
FIRST_LINE = re.compile(r"\blongshot (\S+)(?: \(source ([0-9a-f]+)\))?")
NEXT = ".next"   # (in perf_pending/: when the relay may be asked again after it answered busy, and the wait it took)
LOCK = ".sending"   # (in perf_pending/: the send lock while a sender runs. Its body names the holder and its lease;
                    #  the holder's heartbeat renews the lease every BEAT (a live sender is never taken over, and a
                    #  whole send runs minutes at most — its timeouts' worst case — so a lease that ran un-renewed
                    #  means the holder died between taking it and releasing it).)
LEASE = 900        # (a lock's lease in seconds: see LOCK)
BEAT = 30          # (the heartbeat's period: the holder renews its lease this often while it sends)
STALL = 4 * BEAT   # (a lease may run out this far before the lock stops holding: the heartbeat may be mid-beat on a
                   #  stalled file system; the holder pid check is the hard wall a clock cannot move)
LEGACY = 60        # (a body an older sender wrote (no lease): fall back to its mtime, this old means its sender is gone)
BUSY_FIRST, BUSY_MOST = 3600, 86400   # (seconds: a busy relay's first wait without a Retry-After, and the longest)
# The status that means "this summary will never be taken" (bad body, too big, wrong kind, unprocessable): only those
# are deleted. Every other answer (a redirect, 404 while the service is redeployed, 408, 425) keeps it for later.
REFUSED = (400, 413, 415, 422)
MAX_SENDS = 5   # (one start's share, oldest first: the perf route's ingress takes 6 a minute with a burst of 6 per
                # address (the deploy repo's deploy/telemetry/40-ingress.yaml); the two numbers live together, if you
                # raise one, raise the other in the same change)
HEADERS = {"Content-Type": "application/json", "User-Agent": "longshot-perf-report"}


def env_number(name, default, low, high):
    """A whole-number switch, clamped to [low, high] (said on stderr); unset, empty or not a number: the default"""
    raw = os.environ.get(name, "").strip()
    if not raw:
        return default
    try:
        value = int(raw)
    except ValueError:
        print("%s=%s is not a whole number: %d" % (name, raw, default), file=sys.stderr)
        return default
    if not low <= value <= high:
        value = min(max(value, low), high)
        print("%s=%s is outside %d to %d: %d" % (name, raw, low, high, value), file=sys.stderr)
    return value


def env_switch(name):
    """An on/off switch: True, False, or None when unset or unknown (the caller's default)"""
    raw = os.environ.get(name, "").strip().lower()
    if raw in ("1", "on", "yes", "true"):
        return True
    if raw in ("0", "off", "no", "false"):
        return False
    if raw:
        print("%s=%s is neither on nor off: the default" % (name, raw), file=sys.stderr)
    return None


def endpoint():
    """Where the summaries go: LONGSHOT_PERF_URL (https, or http to this PC only, for a local relay), else perf_reports in
    nx2/addresses.ini"""
    raw = os.environ.get("LONGSHOT_PERF_URL", "").strip()
    if not raw:
        return ENDPOINT
    u = urllib.parse.urlsplit(raw)
    if u.scheme == "https" and u.hostname or u.scheme == "http" and u.hostname in ("127.0.0.1", "localhost", "::1"):
        return raw
    print("LONGSHOT_PERF_URL=%s is not https (or http to this PC): %s" % (raw, ENDPOINT or "nowhere"), file=sys.stderr)
    return ENDPOINT


def choices(folder):
    """longshot_perf.ini's lines as {key: value}; {} without the file"""
    try:
        with open(os.path.join(folder, CHOICE), encoding="utf-8", errors="replace") as f:
            return dict(line.strip().split("=", 1) for line in f if "=" in line)
    except OSError:
        return {}


def sending(folder):
    """The tick: on only with send=1 in the game folder"""
    return choices(folder).get("send") == "1"


def keeping_sent(folder):
    """Whether a copy of each summary sent is kept in perf_sent/: LONGSHOT_PERF_KEEP_SENT, else the launcher's choice
    (keep_sent=0 in longshot_perf.ini: off), on by default"""
    env = env_switch("LONGSHOT_PERF_KEEP_SENT")
    return env if env is not None else choices(folder).get("keep_sent") != "0"


def remove_all(folder, sub, ends):
    """Deletes sub's files whose names end with one of ends; nothing raised"""
    try:
        names = [n for n in os.listdir(os.path.join(folder, sub)) if n.endswith(ends)]
    except OSError:
        names = []
    for name in names:
        try:
            os.remove(os.path.join(folder, sub, name))
        except OSError:
            pass


def set_sending(folder, on, keep_sent=True):
    """The tick, saved for the game (it reads it at its start), and whether copies of what was sent are kept (off empties
    perf_sent/). The tick off deletes what is waiting, and the copies in perf_sent/"""
    path = os.path.join(folder, CHOICE)
    if on:
        with open(path + ".tmp", "w", encoding="utf-8") as f:
            f.write("send=1\n" + ("" if keep_sent else "keep_sent=0\n"))
        os.replace(path + ".tmp", path)
        if not keep_sent:
            remove_all(folder, SENT, (".json", ".tmp"))
        return
    try:
        os.remove(path)
    except OSError:
        pass
    remove_all(folder, PENDING, (".json", ".tmp", NEXT))   # (and a file cut short while it was written: its .tmp)
    remove_all(folder, SENT, (".json", ".tmp"))


def order(name):
    """A summary's place, oldest first: its name is <unix time>-<n>.json"""
    m = re.match(r"(\d+)-(\d+)\.json$", name)
    return (int(m.group(1)), int(m.group(2))) if m else (0, 0)


def listed(folder, sub):
    """The summaries in the game folder's sub (perf_pending/ or perf_sent/), unsorted; none without the folder"""
    try:
        return [n for n in os.listdir(os.path.join(folder, sub)) if n.endswith(".json")]
    except OSError:
        return []


def waiting(folder):
    """The summaries waiting, oldest first"""
    return sorted(listed(folder, PENDING), key=order)


def build_facts(folder):
    """(version, release) of the game in that folder: longshot_version.txt's version, and the release flag (a release's
    version and the log's source fingerprint the same as the release's own source= line)"""
    version, expected = "", ""
    try:
        with open(os.path.join(folder, VERSION_FILE), encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("version="):
                    version = line[8:].strip()
                elif line.startswith("source="):
                    expected = line[7:].strip()
    except OSError:
        pass
    source = ""
    for name in LOGS:
        try:
            with open(os.path.join(folder, name), encoding="utf-8", errors="replace") as f:
                m = FIRST_LINE.search(f.readline())
        except OSError:
            continue
        if m:
            version = version or m.group(1)
            source = m.group(2) or ""
            break
    return version, bool(RELEASE_VERSION.match(version) and expected and source == expected)


def answer(code, retry_after=None):
    """(result, seconds to wait) of the relay's status: 'sent' (2xx), 'refused' (only REFUSED's codes: this summary will
    never be taken there, so it goes), 'busy' (429, 5xx: asked again after its wait), 'later' (every other answer: 3xx a
    redirect http.client does not follow, 404 while the service is redeployed or its route is missing, 408, 425: kept
    for the next start; a missing route is not the relay asking to be left alone, so no wait is noted)"""
    if 200 <= code < 300:
        return "sent", None
    if code == 429 or code >= 500:
        return "busy", wait_of(retry_after)
    if code in REFUSED:
        return "refused", None
    return "later", None


def wait_of(retry_after):
    """Retry-After's seconds (a number, or an HTTP date), None without one or when it can't be read"""
    if not retry_after:
        return None
    if retry_after.strip().isdigit():
        return int(retry_after.strip())
    try:
        return max(0, int(email.utils.parsedate_to_datetime(retry_after).timestamp() - time.time()))
    except (TypeError, ValueError, OverflowError):
        return None


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """urllib follows a 302 by turning the POST into a GET of the Location — a redirect's 200 at the far end is not the
    relay taking the summary: the answer to judge is the 3xx itself (issue #1265, review)"""
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def post(body, url, timeout=10):
    """One summary on a connection of its own (urllib, which takes the system's proxy): answer()'s pair, or ('later', None)
    when not reached or cut short. The opener is built per call, so a proxy that started or stopped after this process
    began is seen as it is now; it never follows a redirect (see _NoRedirect)"""
    req = urllib.request.Request(url, data=body, method="POST", headers=HEADERS)
    try:
        with urllib.request.build_opener(_NoRedirect).open(req, timeout=timeout) as r:
            return answer(r.status)
    except urllib.error.HTTPError as e:
        return answer(e.code, e.headers.get("Retry-After"))
    except Exception:   # (not reached, cut short: kept for next time)
        return "later", None


class Relay:
    """The summaries over one kept connection (a handshake for them all, not one each). Through a proxy, urllib's post()
    for each, which knows the system's proxy and http.client doesn't"""

    def __init__(self, url, timeout):
        self.url, self.timeout, self.conn = url, timeout, None
        u = urllib.parse.urlsplit(url)
        self.https, self.host, self.port = u.scheme == "https", u.hostname, u.port
        self.path = (u.path or "/") + ("?" + u.query if u.query else "")
        proxy = urllib.request.getproxies().get(u.scheme)
        self.proxied = bool(proxy) and not urllib.request.proxy_bypass(u.hostname or "")

    def post(self, body):
        if self.proxied:
            return post(body, self.url, self.timeout)
        for again in (False, True):
            reused = self.conn is not None
            if not reused:
                kind = http.client.HTTPSConnection if self.https else http.client.HTTPConnection
                self.conn = kind(self.host, self.port, timeout=self.timeout)
            try:
                self.conn.request("POST", self.path, body, HEADERS)
                r = self.conn.getresponse()
                r.read()   # (all of it: the connection serves the next one)
            except (http.client.RemoteDisconnected, BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                # (a kept connection the relay closed while it waited: the summary never arrived, once more on a new one.
                # Windows says ConnectionAbortedError, WinError 10053, where Linux says RemoteDisconnected)
                self.close()
                if reused and not again:
                    continue
                return "later", None
            except Exception:   # (not reached, cut short: kept for next time)
                self.close()
                return "later", None
            if r.will_close:
                self.close()
            return answer(r.status, r.getheader("Retry-After"))
        return "later", None

    def close(self):
        if self.conn is not None:
            self.conn.close()
            self.conn = None


def busy_until(folder):
    """(when the relay may be asked again, the wait that set it): (0, 0) when it wasn't busy"""
    try:
        with open(os.path.join(folder, PENDING, NEXT), encoding="utf-8") as f:
            at, wait = f.read().split()
            return float(at), int(wait)
    except (OSError, ValueError):
        return 0, 0


def note_busy(folder, retry_after):
    """The relay answered busy: not asked again before its Retry-After (at most a day), else an hour, doubled each time it
    is busy again, up to a day"""
    _, before = busy_until(folder)
    wait = min(retry_after, BUSY_MOST) if retry_after is not None else min(max(BUSY_FIRST, before * 2), BUSY_MOST)
    path = os.path.join(folder, PENDING, NEXT)
    try:
        with open(path + ".tmp", "w", encoding="utf-8") as f:
            f.write("%d %d\n" % (time.time() + wait, wait))
        os.replace(path + ".tmp", path)
    except OSError:
        pass


def _lock_body(pid):
    return ("%d %d\n" % (pid, int(time.time()) + LEASE)).encode()


def _lock_read(path):
    """(the holder's pid, the lease's end second): (None, None) when there is no lock to read. A body an older sender
    wrote (no lease second in it: the pre-lease form) reads as lease 0 — the mtime check below is its stand-in."""
    try:
        with open(path, "rb") as f:
            fields = f.read(64).split()
        pid = int(fields[0])
        lease = int(fields[1]) if len(fields) > 1 else 0
        return pid, lease
    except (OSError, ValueError, IndexError):
        return None, None


def _pid_gone(pid):
    """True only when the pid is certainly not running (its sender died and took its heartbeat with it). Unknown
    (Windows, another user's process, a wild number) reads as NOT gone: the lease is then the only evidence."""
    if os.name != "posix" or pid <= 1:
        return False
    try:
        os.kill(pid, 0)
        return False
    except ProcessLookupError:
        return True
    except PermissionError:
        return False   # (a process by that number, someone else's: treat as running; the lease bounds it anyway)
    except OSError:
        return False


def _lock_holds(path):
    """True while the lock at path stands for a sender that may still act. Two pieces of evidence, neither of them
    the wall clock's age alone: a holder whose pid is CERTAINLY gone (ProcessLookupError) holds nothing, lease or
    no lease; and a holder whose lease ran out is judged by whether it is still working — a live sender's heartbeat
    touches the lock every BEAT, so its mtime walks with its beats and the lock keeps holding, while a dead one (or
    a pid recycled by an unrelated process) goes quiet and is claimable once 2*BEAT pass without a touch. The round-
    three review's four windows on 81926bf5 all needed a live, working holder's lock to read stale; under these two
    rules none can. (issue #1265, reviews)"""
    pid, lease = _lock_read(path)
    if pid is None:
        # (a body we cannot parse. A directory (older or broken form): not a holder. A regular file whose body is
        #  empty or unparseable: a sender may be mid-create right now (the name exists before the pid lands), so it
        #  holds until it ages past LEGACY — a create+write completes in microseconds, an unreadable body that stays
        #  is a corpse the next sender clears. Claiming through a half-made lock was the round-three review's P2.)
        try:
            st = os.stat(path)
            import stat as _stat
            if _stat.S_ISDIR(st.st_mode):
                return False
            return time.time() - st.st_mtime < LEGACY
        except OSError:
            return False
    if _pid_gone(pid):
        return False
    if lease and time.time() < lease + STALL:
        return True   # (a running holder with a lease that runs: never claimable. The windows a round-three review
                      #  reproduced on 81926bf5 all needed a live holder's lock to read stale; none can.)
    # (the lease ran out — or it's a body an older sender wrote. Is the holder still working? Its heartbeat touches
    #  the lock every BEAT: a live sender's mtime walks with its beats, whatever the clock or a wedged file system
    #  does to the lease. A dead holder's mtime goes stale within a beat or two of its death; so does the pid of a
    #  dead holder recycled by an unrelated process — that one is checked by mtime, never by the pid alone, or a
    #  reused pid would shut out sending forever.)
    try:
        age = time.time() - os.stat(path).st_mtime
    except OSError:
        return False
    return age < (2 * BEAT if lease else LEGACY)


def take_send_lock(folder):
    """True when this process may send; False while another sender's lock holds. The lock (perf_pending/.sending)
    names its holder and its lease in the body, and the holder renews the lease every BEAT while it sends (see
    _heartbeat): a live sender is never taken over, however slow the relay, and a killed one's pid is checked, not
    guessed at from the wall clock's age (see _lock_holds: the wall). A takeover claims the expired lock by rename
    and installs the new one with an O_CREAT | O_EXCL create — atomic on every file system, including the vfat/exfat
    SD cards a Deck game folder may live on (hard links, which an earlier draft used, do not exist there) — so two
    racing takers cannot both reclaim one stale lock and post the same summaries twice; and what the claim moved is
    inspected before it is discarded, so a taker can never step on a fresh lock installed in the breath it spent
    judging (issue #1265, reviews). A folder that can't hold a lock at all (read-only, gone): True — nothing can be
    asked twice under that, and losing summaries to a lock it can't make would be worse."""
    path = os.path.join(folder, PENDING, LOCK)
    for _ in range(3):
        try:
            fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY | getattr(os, "O_NOFOLLOW", 0))
        except FileExistsError:
            try:
                os.remove("%s.%d" % (path, os.getpid()))   # (our own half-made body from a send that broke halfway:
            except OSError:                                #  ours to clear; a running sender shares no pid with us)
                pass
            if _lock_holds(path):
                return False   # (a running holder — us included, our heartbeat keeps that lease fresh; not our turn)
            # (expired, dead, or a form with no body (a directory): claim it. rename is atomic: exactly one taker
            #  moves the old lock aside; the new one then goes in with the same O_EXCL create as the ordinary path,
            #  which fails if anyone made a fresh lock meanwhile. (link() was the first try and is dropped on
            #  purpose: vfat and exfat, what a Deck's microSD game folder is formatted as, do not carry hard links,
            #  and a failed install there means sending unlocked — the very double post this lock is for.)
            mine = "%s.%d" % (path, os.getpid())
            try:
                os.rename(path, mine)
            except OSError:
                continue   # (another taker won the claim a breath ago: re-read the fresh state)
            # (what did we just move? A round-three review caught that a taker which judged the lock stale a breath
            #  ago can rename a FRESH lock another taker installed in that breath. The claimed file itself tells us:
            #  if it names a live sender, it was never ours to claim — slide it back, but through the same O_EXCL
            #  create, never a rename: a rename would clobber a lock that appeared at path while we held the corpse
            #  (and a file that was live in our hands is the very lock the wall says nobody may displace). If path
            #  is taken, that occupant stands; we drop our copy and re-read.)
            if _lock_holds(mine):
                restored = False
                try:
                    with open(mine, "rb") as f:
                        body = f.read(64)   # (slide it back with an O_EXCL create of the SAME body — not a rename:
                    fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)   #  a rename could clobber a lock that
                    os.write(fd, body)                                        #  appeared at path while we inspected)
                    os.close(fd)
                    restored = True
                except OSError:
                    pass
                try:
                    os.unlink(mine)   # (ours either way now: restored to path, or path was taken and this was a
                except OSError:       #  mis-claim — either way no stray .sending.<pid> is left behind)
                    pass
                continue   # (re-read the fresh state; the healthy sender we nearly stepped on keeps its turn)
            try:
                os.unlink(mine)
            except IsADirectoryError:
                try:
                    os.rmdir(mine)   # (a .sending left as a directory by an older or broken form: clear it, don't
                except OSError:      #  wedge every later sender forever (issue #1265, review))
                    pass
            except OSError:
                pass
            try:
                fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            except FileExistsError:
                return not _lock_holds(path)   # (someone else's fresh lock: refused; nothing holding there: ours
                                               #  to make, and a claim is ours to finish — try the claim once more)
            except OSError:
                return True   # (the folder broke under us: send unsent, the old behavior)
            try:
                os.write(fd, _lock_body(os.getpid()))
            except OSError:
                try:
                    os.close(fd)
                except OSError:
                    pass
                try:
                    os.remove(path)   # (a lock whose body didn't get written is nobody's; see the ordinary path's arm)
                except OSError:
                    pass
                return True
            os.close(fd)
            return True
        except OSError:
            return True   # (no lock can be made at all: send unsent, the old behavior)
        try:
            os.write(fd, _lock_body(os.getpid()))
        except OSError:
            try:
                os.close(fd)
            except OSError:
                pass
            try:
                os.remove(path)   # (never hand out a lock whose body didn't get written: an unreadable body wedges
            except OSError:       #  taking over, and the sender that couldn't write it may not be back (ENOSPC left
                pass              #  the lock and shut out the immediate retry; issue #1265, review))
            return True
        os.close(fd)
        return True
    return True   # (the claim race kept losing to healthy senders: sending proceeds, the old behavior)


def release_send_lock(folder):
    """remove only OUR lock: a remove by pathname deleted the lock a takeover had handed to the next sender, and a
    third sender then posted the same summaries a second time (issue #1265, review). the body's pid must be ours."""
    path = os.path.join(folder, PENDING, LOCK)
    if _lock_read(path)[0] != os.getpid():
        return   # (gone, replaced, or someone else's: nothing of ours to remove)
    try:
        os.remove(path)
    except OSError:
        pass   # (gone under us: an expired lock only blocks until its lease runs out or its pid checks gone)


def _heartbeat(folder, stop):
    """every BEAT the holder renews its lease (rewrite OUR lock's body, by name: a lock whose body is no longer ours
    was taken over and is not ours to rewrite). A send slower than any single lease (a relay that trickles) keeps its
    lock — the wall clock's age was never liveness (issue #1265, review)."""
    path = os.path.join(folder, PENDING, LOCK)
    while not stop.wait(BEAT):
        if _lock_read(path)[0] != os.getpid():
            return   # (the lock is gone or another's: nothing to renew; the send finishes what it started)
        try:
            mine = "%s.%d" % (path, os.getpid())
            with open(mine, "wb") as f:
                f.write(_lock_body(os.getpid()))
            os.replace(mine, path)   # (ours by the check above; the rename carries a fresh lease AND mtime. The
                                     #  parked-beat window a round-three review measured (a beat landing after a
                                     #  normal release) is closed where the thread lives: send_waiting joins this
                                     #  thread BEFORE its release. If the join's bound expired on a wedged file
                                     #  system and this beat still lands late, the wall in _lock_holds is the back-
                                     #  stop: the landing names OUR pid, so it can only ever block other senders,
                                     #  never admit a second poster — and our own sender is exiting, so the pid
                                     #  check clears it at the next start.)
        except OSError:
            pass


def send_waiting(folder, url=None, timeout=None):
    """Sends what is waiting, when the tick is on: {"sent": n, "refused": n, "left": n}. Stops at the first one the relay
    couldn't take now, and after LONGSHOT_PERF_MAX_SENDS; nothing while the relay asked to be left alone
    (LONGSHOT_PERF_BACKOFF); nothing while another sender holds .sending (two detached senders would post the same
    summaries, and a summary has no id for the service to dedupe on)"""
    url = endpoint() if url is None else url
    timeout = env_number("LONGSHOT_PERF_TIMEOUT", 10, 2, 60) if timeout is None else timeout
    most = env_number("LONGSHOT_PERF_MAX_SENDS", MAX_SENDS, 1, MAX_PENDING)
    backoff = env_switch("LONGSHOT_PERF_BACKOFF") is not False
    counts = {"sent": 0, "refused": 0, "left": 0}
    if not folder or not sending(folder):
        return counts
    if not take_send_lock(folder):   # (another sender's lease runs: everything stays, nothing is asked twice)
        counts["left"] = len(waiting(folder))
        return counts
    stop = threading.Event()
    beat = threading.Thread(target=_heartbeat, args=(folder, stop), daemon=True)
    beat.start()   # (the send outlives one lease on a slow relay: a live sender keeps its lock; a killed holder's
                   #  thread dies with the process and its lease runs out (issue #1265, review))
    try:
        return send_unlocked(folder, url, timeout, most, backoff, counts)
    finally:
        stop.set()
        beat.join(5 * BEAT)   # (before release, not after: a round-three review's beat parked between the ownership
                              #  check and its rename landed AFTER a normal release and stomped the successor's fresh
                              #  lock. Joined first, no beat can exist once the lock is removed. The join is bounded
                              #  (5 beats) because a beat can only still be in flight on a file system that is itself
                              #  wedged — where every other call on this folder fails the same way; past the bound the
                              #  release goes ahead, the same floor as the broken-folder arm.)
        release_send_lock(folder)


def send_unlocked(folder, url, timeout, most, backoff, counts):
    names = waiting(folder)
    for old in names[:-MAX_PENDING]:   # (too many: the oldest go unsent)
        try:
            os.remove(os.path.join(folder, PENDING, old))
        except OSError:
            pass
    names = names[-MAX_PENDING:]
    keep = keeping_sent(folder)
    if not keep:   # (the player keeps none: what an earlier send kept goes too)
        remove_all(folder, SENT, (".json", ".tmp"))
    if not url or backoff and busy_until(folder)[0] > time.time():
        counts["left"] = len(names)
        return counts
    version, release = build_facts(folder)
    relay = Relay(url, timeout)
    try:
        sent_all = send_these(folder, names[:most], relay, version, release, counts, backoff)
    finally:
        relay.close()
    counts["left"] = len(names) - counts["sent"] - counts["refused"]
    if sent_all:
        try:
            os.remove(os.path.join(folder, PENDING, NEXT))
        except OSError:
            pass
    if counts["sent"] and keep:
        prune_sent(folder)
    return counts


def send_these(folder, names, relay, version, release, counts, backoff):
    """send_waiting's loop: True when the relay took or refused each one, False when it stopped (busy: noted, unless
    LONGSHOT_PERF_BACKOFF is off; not reached: tried again at the next start). Mid-send the player can turn the tick off:
    set_sending(False) deletes what waits and the busy note, and anything this loop writes after that (an answer's
    .next, a kept copy) must not be left behind — the sweep runs again when we notice (issue #1265, review)"""
    for name in names:
        if not sending(folder):
            remove_all(folder, PENDING, (".json", ".tmp", NEXT))
            return False
        path = os.path.join(folder, PENDING, name)
        try:
            with open(path, encoding="utf-8", errors="replace") as f:   # (a bad byte: one field's text, not the summary)
                summary = json.load(f)
            if not isinstance(summary, dict):
                raise ValueError
        except (OSError, ValueError):
            result = "refused"   # (unreadable: dropped)
        else:
            # (the game's own version, of the build that wrote it: the folder may have been updated since. A release only
            # when that is the folder's build, whose fingerprint was checked)
            own = summary.get("version") if isinstance(summary.get("version"), str) else ""
            summary["version"] = own or version
            summary["release"] = release and summary["version"] == version
            body = json.dumps(summary, separators=(",", ":")).encode()
            result, wait = ("refused", None) if len(body) > MAX_BODY else relay.post(body)
            if result == "busy":
                if backoff:
                    note_busy(folder, wait)
                if not sending(folder):   # (unticked while the request was in flight: the note the sweep already
                    remove_all(folder, PENDING, (".json", ".tmp", NEXT))   #  deleted came back from this answer)
                return False
        if result == "later":
            return False
        counts[result] += 1
        try:
            os.remove(path)
        except OSError:
            pass
        if result == "sent":   # (after the remove: nothing between the relay's yes and the summary leaving perf_pending/)
            keep_sent(folder, name, body)
    return True


def keep_sent(folder, name, body):
    """A copy of what the relay took, as sent, in perf_sent/. Best effort: the summary is sent. None once the tick is off
    (unticked during the send: the player said no, and set_sending emptied perf_sent/), or copies are off"""
    if not sending(folder) or not keeping_sent(folder):
        return
    sent = os.path.join(folder, SENT)
    tmp = os.path.join(sent, name + ".tmp")
    try:
        os.makedirs(sent, exist_ok=True)
        with open(tmp, "wb") as f:
            f.write(body)
        os.replace(tmp, os.path.join(sent, name))
    except OSError:
        try:
            os.remove(tmp)
        except OSError:
            pass


def prune_sent(folder):
    """perf_sent/ down to the MAX_PENDING copies sent last (by when they were written there, not by their names' time: a
    clock that was wrong once would keep the wrong ones)"""
    def written(name):
        try:
            return os.stat(os.path.join(folder, SENT, name)).st_mtime_ns
        except OSError:
            return 0
    for old in sorted(listed(folder, SENT), key=written)[:-MAX_PENDING]:
        try:
            os.remove(os.path.join(folder, SENT, old))
        except OSError:
            pass


def start_sending(folder, python=None, python_args=()):
    """send_waiting in a process of its own, left running: Play closes the launcher at once, and a summary the relay took
    must still be deleted after (or it would be sent, and counted, again). Nothing when the tick is off or nothing waits"""
    if not folder or not sending(folder) or not waiting(folder):
        return None
    cmd = [python or sys.executable, *python_args, "-I", os.path.abspath(__file__), folder, "--send"]
    kw = {"stdin": subprocess.DEVNULL, "stdout": subprocess.DEVNULL, "stderr": subprocess.DEVNULL, "close_fds": True}
    if os.name == "nt":
        kw["creationflags"] = subprocess.DETACHED_PROCESS | subprocess.CREATE_NO_WINDOW
    else:
        kw["start_new_session"] = True
    try:
        return subprocess.Popen(cmd, **kw)
    except OSError:
        return None


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    folder = sys.argv[1]
    if "--send" in sys.argv[2:]:
        print(send_waiting(folder))
        return
    if sys.argv[2:3] == ["--tick"]:
        rest = sys.argv[3:]
        if rest[:1] not in (["on"], ["off"]) or rest[1:] and (rest[1:2] != ["--keep-sent"] or rest[2:] not in (["on"], ["off"])):
            sys.exit("--tick on|off [--keep-sent on|off]")
        keep = rest[2:] != ["off"]
        set_sending(folder, rest[0] == "on", keep)
        print("tick: %s, keep sent: %s" % (rest[0], "on" if keep else "off"))
        return
    version, release = build_facts(folder)
    kept = len(listed(folder, SENT))
    at, _ = busy_until(folder)
    print("tick: %s, waiting: %d, version %s, release %s, to %s; %d sent ones kept in %s (keep sent: %s)%s" % (
        "on" if sending(folder) else "off", len(waiting(folder)), version or "?", release, endpoint() or "nowhere", kept,
        os.path.join(folder, SENT), "on" if keeping_sent(folder) else "off",
        "; the relay was busy: not asked before %s" % time.strftime("%Y-%m-%d %H:%M", time.localtime(at))
        if at > time.time() else ""))


if __name__ == "__main__":
    main()
