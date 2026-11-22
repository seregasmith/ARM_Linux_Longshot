"""The performance statistics (docs/dev/reference/profiling.md, Performance statistics): the game's summary (gpu/vk/perf_report.h, compiled with
tests/test_perf_report.cpp: buckets, hitches, pacing, the JSON, a frame's cost), the wiring (on only with the tick, a
frame added at each present, the events where they happen), what the relay does with what the game sends, and the
launcher's side (nx2/perf_report.py):

  - the sizes: each biggest-case summary the C++ test writes is re-encoded the way the sender does (Python's json.dumps)
    and is no bigger than its MAX_BODY, and the sender itself sends every one of them (none refused and deleted);
  - the relay's rules (a short statement of Longshot-api's telemetry/perf, its public source linked at relay_cell): for
    each exact place and view the C++ test makes, the sent sample is one the relay takes (|yaw| <= pi, |pitch| <= pi/2:
    one sample past that and it refuses the whole summary) and it files it under the cell and view it files the exact
    place under, so the game's rounding costs the relay's totals nothing;
  - the host's pose code is run by tests/test_perf_pose.py, not read here;
  - the tick: send=1 in longshot_perf.ini, off by default; off deletes what is waiting;
  - with the tick on, the waiting summaries are sent oldest first, the game's own version kept (filled in when missing)
    and a release only for the folder's build; each one the relay took or refused is deleted; one it couldn't take now
    (not reached, 429, 5xx) stops the sending and stays; with the tick off nothing is sent;
  - at most 48 wait: the oldest past that go unsent;
  - each one the relay took is kept as sent in perf_sent/ (the 48 sent last), for a tester to read; off deletes those too,
    and none is kept once the tick is off; a copy that can't be kept doesn't stop the sending;
  - the relay's address is perf_reports in nx2/addresses.ini (none in the code);
  - the summary's map before the first one: "Preparing shaders", then "Title and intro".

python -I -B tests/test_perf_report.py [--cxx g++]
"""
import argparse
import http.client
import http.server
import json
import math
import os
import re
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import addresses  # noqa: E402
import perf_report as pr  # noqa: E402

failures = []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        failures.append(what)


class Relay:
    """A stand-in perf relay over HTTP/1.1: answers each POST with the next code of `codes` (then 201) and the headers of
    `headers`, keeps the bodies, counts its connections; `drop_after_first` closes the first connection after its answer
    without saying so (a kept connection the relay let go)"""
    def __init__(self, codes=(), headers=None):
        self.codes, self.bodies, self.headers, self.connections, self.drop_after_first = list(codes), [], headers or {}, 0, False
        relay = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def setup(self):
                super().setup()
                relay.connections += 1

            def do_POST(self):
                relay.bodies.append(json.loads(self.rfile.read(int(self.headers["Content-Length"]))))
                code = relay.codes.pop(0) if relay.codes else 201
                self.send_response(code)
                for k, v in relay.headers.items():
                    self.send_header(k, v)
                self.send_header("Content-Length", "0")
                self.end_headers()
                if relay.drop_after_first:
                    relay.drop_after_first = False
                    self.close_connection = True

            def log_message(self, *a):
                pass
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.url = "http://127.0.0.1:%d/perf" % self.server.server_port

    def close(self):
        self.server.shutdown()
        self.server.server_close()


class Env:
    """Switches set for a with block, as they were after it"""
    def __init__(self, **values):
        self.values, self.before = values, {}

    def __enter__(self):
        for k, v in self.values.items():
            self.before[k] = os.environ.get(k)
            os.environ[k] = v
        return self

    def __exit__(self, *a):
        for k, v in self.before.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v


def game_folder(tmp, name, summaries=3, tick=True, version="v3.1.0", source="abcd1234", log_source="abcd1234", wrote=""):
    f = Path(tmp, name)
    (f / pr.PENDING).mkdir(parents=True)
    for i in range(summaries):
        (f / pr.PENDING / ("17000000%02d-%d.json" % (i, i))).write_text(json.dumps({"v": 1, "version": wrote, "n": i}))
    (f / pr.VERSION_FILE).write_text("version=%s\nsource=%s\n" % (version, source))
    (f / "logs").mkdir()
    (f / "logs" / "longshot_log.txt").write_text("[hos host] longshot %s (source %s)\n" % (version, log_source))
    if tick:
        pr.set_sending(str(f), True)
    return f


def launcher_side(tmp):
    f = game_folder(tmp, "plain", tick=False)
    check(not pr.sending(str(f)), "the tick is off by default")
    pr.set_sending(str(f), True)
    check(pr.sending(str(f)) and (f / pr.CHOICE).read_text() == "send=1\n", "on: send=1 in longshot_perf.ini")

    # (the heartbeat: a sender whose send outlives one LEASE — a relay that trickles — keeps its lock: the holder's
    #  lease is renewed while it sends, so a second sender is refused mid-send with the lease still in force)
    beat = Relay()   # (one held-open request; the reply only goes when we say)
    try:
        import socket, time
        hb = game_folder(tmp, "a send longer than one lease", summaries=1)
        srv = socket.socket(); srv.bind(("127.0.0.1", 0)); srv.listen(1)
        port = srv.getsockname()[1]
        def serve():
            conn, _ = srv.accept()
            conn.recv(65536)
            # (held ~2 s: six beats at the 0.3 s patched below — more than a whole wall second apart, so at least
            #  one beat renews the lease end across a second boundary, whatever its phase; the heartbeat must beat
            #  while we hold the reply)
            time.sleep(2.0)
            conn.sendall(b"HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n"); conn.close()
        t = threading.Thread(target=serve, daemon=True); t.start()
        lk = hb / pr.PENDING / pr.LOCK
        real_beat, pr.BEAT = pr.BEAT, 0.3
        try:
            send_thread = threading.Thread(target=lambda: pr.send_waiting(str(hb), url="http://127.0.0.1:%d/perf" % port, timeout=8))
            send_thread.start()
            for _ in range(200):   # (wait for the sender to take its lock)
                if lk.exists():
                    break
                time.sleep(0.02)
            beaten = [lk.stat().st_mtime] if lk.exists() else []
            for _ in range(160):
                try:
                    m = lk.stat().st_mtime   # (each beat rewrites the body: the lock's mtime walks with the beats;
                except OSError:              #  watching it is phase-free — a lease end second can straddle a tick)
                    break
                if m > beaten[-1]:
                    beaten.append(m)
                    if len(beaten) >= 3:
                        break   # (three beats at 0.3 s apart: the heartbeat beats while the send is in flight)
                time.sleep(0.05)
            refused = pr.take_send_lock(str(hb)) if send_thread.is_alive() else True
            if refused:
                pr.release_send_lock(str(hb))
            send_thread.join(15)
        finally:
            pr.BEAT = real_beat
            srv.close()
        check(len(beaten) >= 3 and not refused and send_thread.is_alive() is False,
              "a send outliving one lease: the heartbeat beats while it runs (lock mtime moved %d times) and a "
              "second sender is refused mid-send" % (len(beaten) - 1))
    finally:
        beat.close()

    relay = Relay()
    try:
        got = pr.send_waiting(str(f), url=relay.url)
        check(got == {"sent": 3, "refused": 0, "left": 0}, "on: every summary sent (%s)" % got)
        check([b["n"] for b in relay.bodies] == [0, 1, 2], "oldest first")
        check(all(b["version"] == "v3.1.0" and b["release"] is True for b in relay.bodies),
              "the version and the release flag filled in")
        check(not list((f / pr.PENDING).glob("*.json")), "each one sent is deleted")
        kept = sorted((f / pr.SENT).glob("*.json"))
        check([json.loads(p.read_text())["n"] for p in kept] == [0, 1, 2]
              and json.loads(kept[0].read_text())["version"] == "v3.1.0",
              "a copy of each one sent is kept in perf_sent/, as sent")

        f2 = game_folder(tmp, "edited", log_source="ffff0000")
        relay.bodies.clear()
        pr.send_waiting(str(f2), url=relay.url)
        check(relay.bodies and all(b["release"] is False for b in relay.bodies), "built from other sources: not a release")

        f7 = game_folder(tmp, "updated since", summaries=2, wrote="v3.0.0")
        relay.bodies.clear()
        pr.send_waiting(str(f7), url=relay.url)
        check(relay.bodies and all(b["version"] == "v3.0.0" and b["release"] is False for b in relay.bodies),
              "a summary of the build before an update keeps its version, and is not this release's")

        f9 = game_folder(tmp, "same build", summaries=1, wrote="v3.1.0")
        relay.bodies.clear()
        pr.send_waiting(str(f9), url=relay.url)
        check(relay.bodies and relay.bodies[0]["release"] is True, "the folder's own build: the release flag")

        f3 = game_folder(tmp, "busy", summaries=4)
        relay.codes[:] = [201, 400, 429]
        relay.bodies.clear()
        got = pr.send_waiting(str(f3), url=relay.url)
        check(got == {"sent": 1, "refused": 1, "left": 2}, "a 429 stops the sending (%s)" % got)
        check(len(list((f3 / pr.PENDING).glob("*.json"))) == 2, "what the relay couldn't take now stays")

        f4 = game_folder(tmp, "off", tick=False)
        relay.bodies.clear()
        got = pr.send_waiting(str(f4), url=relay.url)
        check(not relay.bodies and got["sent"] == 0, "the tick off: nothing sent")

        f5 = game_folder(tmp, "turned off")
        (f5 / pr.PENDING / "1700000099-9.tmp").write_text("{")
        pr.send_waiting(str(f5), url=relay.url)
        (f5 / pr.PENDING / "1700000098-8.json").write_text("{}")
        pr.set_sending(str(f5), False)
        check(not (f5 / pr.CHOICE).exists() and not list((f5 / pr.PENDING).iterdir())
              and not list((f5 / pr.SENT).iterdir()),
              "turning the tick off deletes the setting, what is waiting and the copies of what was sent")

        f6 = game_folder(tmp, "many", summaries=pr.MAX_PENDING + 5)
        relay.bodies.clear()
        with Env(LONGSHOT_PERF_MAX_SENDS="48"):
            pr.send_waiting(str(f6), url=relay.url)
        check(len(relay.bodies) == pr.MAX_PENDING and relay.bodies[0]["n"] == 5, "at most 48: the oldest go unsent")
        check(len(list((f6 / pr.SENT).glob("*.json"))) == pr.MAX_PENDING, "perf_sent/ keeps the newest 48")

        f10 = game_folder(tmp, "clock was wrong", summaries=1)
        (f10 / pr.SENT).mkdir()
        for n in range(pr.MAX_PENDING):   # (copies named by a clock in 2030, written long ago)
            old = f10 / pr.SENT / ("1900000000-%d.json" % n)
            old.write_text("{}")
            os.utime(old, (1000 + n, 1000 + n))
        pr.send_waiting(str(f10), url=relay.url)
        kept = {p.name for p in (f10 / pr.SENT).glob("*.json")}
        check(len(kept) == pr.MAX_PENDING and "1700000000-0.json" in kept and "1900000000-0.json" not in kept,
              "perf_sent/ keeps the ones sent last, whatever their names' time")

        f11 = game_folder(tmp, "copy fails", summaries=2)
        (f11 / pr.SENT).write_text("not a folder")
        got = pr.send_waiting(str(f11), url=relay.url)
        check(got == {"sent": 2, "refused": 0, "left": 0} and not list((f11 / pr.PENDING).glob("*.json")),
              "a copy that can't be kept: the summaries still count as sent, and leave perf_pending/ (%s)" % got)

        f12 = game_folder(tmp, "unticked while sending", summaries=1)
        pr.set_sending(str(f12), False)
        pr.keep_sent(str(f12), "1700000000-0.json", b"{}")
        check(not (f12 / pr.SENT).exists(), "unticked during a send: no copy kept after the player said no")
    finally:
        relay.close()

    f8 = game_folder(tmp, "bad bytes", summaries=1)
    (f8 / pr.PENDING / "1700000000-0.json").write_bytes(b'{"v": 1, "version": "x", "n": 0, "os": "Linu\xc3"}')
    relay2 = Relay()
    try:
        got = pr.send_waiting(str(f8), url=relay2.url)
    finally:
        relay2.close()
    check(got["sent"] == 1, "a summary with a broken character is still sent (%s)" % got)

    f9 = game_folder(tmp, "detached", summaries=1)
    stub = Path(tmp, "stub.py")
    stub.write_text("import sys, pathlib\npathlib.Path(sys.argv[-2], 'started.txt').write_text(' '.join(sys.argv[1:]))\n")
    p = pr.start_sending(str(f9), python=sys.executable, python_args=[str(stub)])
    p.wait(20)
    started = (f9 / "started.txt").read_text() if (f9 / "started.txt").exists() else ""
    check(started.endswith(" %s --send" % f9) and "perf_report.py" in started,
          "start_sending runs nx2/perf_report.py <folder> --send in a process of its own")
    check(pr.start_sending(str(game_folder(tmp, "nothing waits", summaries=0))) is None and
          pr.start_sending(str(game_folder(tmp, "off again", tick=False))) is None,
          "nothing started when nothing waits or the tick is off")

    f10 = game_folder(tmp, "ticked from the launcher", tick=False)
    script = str(ROOT / "nx2" / "perf_report.py")
    on = subprocess.run([sys.executable, "-I", script, str(f10), "--tick", "on"], capture_output=True, text=True)
    check(on.returncode == 0 and pr.sending(str(f10)), "--tick on: the launcher's tick, send=1 (%s)" % on.stderr.strip())
    off = subprocess.run([sys.executable, "-I", script, str(f10), "--tick", "off"], capture_output=True, text=True)
    check(off.returncode == 0 and not pr.sending(str(f10)) and not list((f10 / pr.PENDING).glob("*.json")),
          "--tick off: no tick, and what waited deleted")
    bad = subprocess.run([sys.executable, "-I", script, str(f10), "--tick", "maybe"], capture_output=True, text=True)
    check(bad.returncode != 0 and not pr.sending(str(f10)), "--tick takes on or off only")

    f7 = game_folder(tmp, "unreachable")
    got = pr.send_waiting(str(f7), url="http://127.0.0.1:9/perf", timeout=2)
    check(got["left"] == 3 and len(list((f7 / pr.PENDING).glob("*.json"))) == 3, "not reached: everything stays")

    check(pr.ENDPOINT.startswith("https://") and pr.ENDPOINT.endswith("/perf"), "the relay's address from addresses.ini")
    check(not re.search(r"[\"']https?://", (ROOT / "nx2" / "perf_report.py").read_text(encoding="utf-8")),
          "no address in nx2/perf_report.py's code")
    ini = Path(tmp, "addresses.ini")
    ini.write_bytes(b"\xef\xbb\xbf" + Path(addresses.ADDRESSES).read_bytes())
    check(addresses.address("perf_reports", str(ini)) == pr.ENDPOINT, "addresses.ini with a BOM")
    ini.write_text("[addresses]\nperf_reports = a\nperf_reports = b\n")
    check(addresses.address("perf_reports", str(ini)) == "", "a broken addresses.ini turns the use off, never raises")


def sender_switches(tmp):
    """The polite sending and its switches: one connection, LONGSHOT_PERF_MAX_SENDS, the busy relay left alone
    (LONGSHOT_PERF_BACKOFF), the copies (keep_sent, LONGSHOT_PERF_KEEP_SENT), LONGSHOT_PERF_URL"""
    relay = Relay()
    try:
        f = game_folder(tmp, "one connection", summaries=5)
        got = pr.send_waiting(str(f), url=relay.url)
        check(got["sent"] == 5 and relay.connections == 1, "every summary over one connection (%d)" % relay.connections)

        f = game_folder(tmp, "dropped", summaries=3)
        relay.connections, relay.drop_after_first = 0, True
        relay.bodies.clear()
        got = pr.send_waiting(str(f), url=relay.url)
        check(got == {"sent": 3, "refused": 0, "left": 0} and len(relay.bodies) == 3 and relay.connections == 2,
              "a kept connection the relay let go: the next summary on a new one, none sent twice (%s)" % got)

        # (the same as Windows says it, on any OS: a kept connection's next request raises WinError 10053)
        class Aborts(http.client.HTTPConnection):
            def request(self, *a, **k):
                if getattr(self, "used", False):
                    raise ConnectionAbortedError(10053, "the connection was aborted by this machine")
                self.used = True
                return super().request(*a, **k)
        f = game_folder(tmp, "aborted", summaries=3)
        relay.connections = 0
        relay.bodies.clear()
        plain, http.client.HTTPConnection = http.client.HTTPConnection, Aborts
        try:
            got = pr.send_waiting(str(f), url=relay.url)
        finally:
            http.client.HTTPConnection = plain
        check(got == {"sent": 3, "refused": 0, "left": 0} and [b["n"] for b in relay.bodies] == [0, 1, 2]
              and relay.connections == 3,
              "a kept connection aborted (ConnectionAbortedError): the next summary on a new one, none sent twice (%s)" % got)

        f = game_folder(tmp, "a start's share", summaries=20)
        relay.bodies.clear()
        got = pr.send_waiting(str(f), url=relay.url)
        check(pr.MAX_SENDS == 5 and got == {"sent": 5, "refused": 0, "left": 15} and
              [b["n"] for b in relay.bodies] == list(range(5)),
              "at most 5 a start by default (pinned as a literal: the ingress route takes 6 a minute with a burst of "
              "6 per address — raise one number, raise the other; docs/dev/reference/profiling.md says the same), "
              "oldest first; the rest wait (%s)" % got)
        doc = (ROOT / "docs" / "dev" / "reference" / "profiling.md").read_text(encoding="utf-8")
        check("at most 5" in doc and "6 a minute" in doc and "burst of 6" in doc,
              "the docs name the same two numbers (5 a start, 6 a minute burst 6): they live together")
        src_line = next(l for l in (ROOT / "nx2" / "perf_report.py").read_text(encoding="utf-8").splitlines() if l.startswith("MAX_SENDS ="))
        check("6 a minute" in src_line and "burst of 6" in src_line,
              "the constant's own comment names the ingress pairing (%s)" % src_line.strip())
        with Env(LONGSHOT_PERF_MAX_SENDS="3"):
            got = pr.send_waiting(str(f), url=relay.url)
        check(got == {"sent": 3, "refused": 0, "left": 20 - pr.MAX_SENDS - 3}, "LONGSHOT_PERF_MAX_SENDS=3 (%s)" % got)
        with Env(LONGSHOT_PERF_MAX_SENDS="1000"):
            check(pr.env_number("LONGSHOT_PERF_MAX_SENDS", pr.MAX_SENDS, 1, 48) == 48, "a switch past its range: clamped")
        with Env(LONGSHOT_PERF_MAX_SENDS="lots"):
            check(pr.env_number("LONGSHOT_PERF_MAX_SENDS", pr.MAX_SENDS, 1, 48) == pr.MAX_SENDS,
                  "a switch that isn't a number: the default")
        with Env(LONGSHOT_PERF_TIMEOUT="0"):
            check(pr.env_number("LONGSHOT_PERF_TIMEOUT", 10, 2, 60) == 2, "LONGSHOT_PERF_TIMEOUT at least 2 s")
    finally:
        relay.close()

    busy = Relay(codes=[201, 429], headers={"Retry-After": "120"})
    try:
        f = game_folder(tmp, "busy relay", summaries=3)
        got = pr.send_waiting(str(f), url=busy.url)
        at, wait = pr.busy_until(str(f))
        check(got == {"sent": 1, "refused": 0, "left": 2} and wait == 120 and 100 < at - pr.time.time() <= 120,
              "429 with Retry-After: 120: not asked again for 2 minutes (%s, %s)" % (got, wait))
        busy.bodies.clear()
        got = pr.send_waiting(str(f), url=busy.url)
        check(got["left"] == 2 and not busy.bodies, "while the relay asked to be left alone: nothing sent, nothing asked")
        with Env(LONGSHOT_PERF_BACKOFF="off"):
            got = pr.send_waiting(str(f), url=busy.url)
        check(got["sent"] == 2 and not (f / pr.PENDING / pr.NEXT).exists(),
              "LONGSHOT_PERF_BACKOFF=off: sent anyway; all of it taken, the wait is gone (%s)" % got)
    finally:
        busy.close()

    busy = Relay(codes=[503, 503], headers={})
    try:
        f = game_folder(tmp, "busy again", summaries=1)
        pr.send_waiting(str(f), url=busy.url)
        first = pr.busy_until(str(f))[1]
        (f / pr.PENDING / pr.NEXT).write_text("0 %d\n" % first)   # (the hour went by)
        pr.send_waiting(str(f), url=busy.url)
        second = pr.busy_until(str(f))[1]
        (f / pr.PENDING / pr.NEXT).write_text("0 80000\n")
        busy.codes[:] = [503]
        pr.send_waiting(str(f), url=busy.url)
        check((first, second, pr.busy_until(str(f))[1]) == (3600, 7200, 86400),
              "busy without Retry-After: an hour, then doubled, at most a day (%s, %s)" % (first, second))
        check(pr.wait_of("Wed, 21 Oct 2099 07:28:00 GMT") > 86400 and pr.wait_of("soon") is None,
              "Retry-After as an HTTP date; one that can't be read is none")
    finally:
        busy.close()

    statuses = Relay(codes=[404, 404])
    try:
        f = game_folder(tmp, "a route missing during a deploy", summaries=2)
        got = pr.send_waiting(str(f), url=statuses.url)
        check(got == {"sent": 0, "refused": 0, "left": 2} and
              len(list((f / pr.PENDING).glob("*.json"))) == 2 and not (f / pr.PENDING / pr.NEXT).exists(),
              "404 (the route missing during a deploy): both summaries stay, and no busy-backoff note either (%s)" % got)
    finally:
        statuses.close()

    never = Relay(codes=[400, 413, 415, 422])
    try:
        f = game_folder(tmp, "will never be taken", summaries=4)
        got = pr.send_waiting(str(f), url=never.url)
        check(got == {"sent": 0, "refused": 4, "left": 0} and not list((f / pr.PENDING).glob("*.json")),
              "400, 413, 415, 422: never to be taken: the summaries go (%s)" % got)
    finally:
        never.close()

    check(all(pr.answer(c) == ("later", None) for c in (301, 302, 307, 308, 404, 408, 425)),
          "3xx, 404, 408, 425: 'later' each (kept for the next start, none kept out of it)")
    check(pr.answer(400)[0] == "refused" and pr.answer(413)[0] == "refused" and pr.answer(415)[0] == "refused" and
          pr.answer(422)[0] == "refused" and pr.answer(204)[0] == "sent" and pr.answer(500)[0] == "busy" and
          pr.answer(429, "5") == ("busy", 5),
          "only 400/413/415/422 refused; 2xx sent; 429 and 5xx busy, Retry-After read")

    lock = Relay()
    try:
        f = game_folder(tmp, "another sender holds it", summaries=3)
        (f / pr.PENDING / pr.LOCK).write_text("%d %d\n" % (os.getppid(), int(pr.time.time() + pr.LEASE)))   # (a
        #  sender that IS running (this suite's parent): its lease runs, its pid checks alive)
        got = pr.send_waiting(str(f), url=lock.url)
        check(got == {"sent": 0, "refused": 0, "left": 3} and not lock.bodies and
              len(list((f / pr.PENDING).glob("*.json"))) == 3 and (f / pr.PENDING / pr.LOCK).exists(),
              "another sender holds .sending: nothing sent, nothing posted, its lock stays its own (%s)" % got)
        (f / pr.PENDING / pr.LOCK).write_text("999999999 %d\n" % int(pr.time.time() - 10))   # (its lease ran out: dead)
        got = pr.send_waiting(str(f), url=lock.url)
        check(got == {"sent": 3, "refused": 0, "left": 0} and not (f / pr.PENDING / pr.LOCK).exists(),
              "a sender whose lease ran out un-renewed: the lock is taken over, the send runs, the lock is gone after (%s)" % got)
        # (release is by OWNER, not by pathname: a holder that finished after someone else took its expired lock over
        #  must not delete the new lock — the review's repro had a third sender then post the same summaries twice)
        f = game_folder(tmp, "release by owner", summaries=1)
        lk = f / pr.PENDING / pr.LOCK
        lk.write_text("999999999 %d\n" % int(pr.time.time() - 1))   # (expired: this process takes it over)
        check(pr.take_send_lock(str(f)), "an expired lock from a sender that is gone: taken over")
        lk.write_text("%d %d\n" % (os.getppid(), int(pr.time.time()) + pr.LEASE))   # (a takeover happened meanwhile:
        pr.release_send_lock(str(f))                                                #  the lock is another's now)
        check(lk.exists() and lk.read_text().split()[0] == str(os.getppid()),
              "release only removes the lock whose body names the releaser: another holder's lock stays")
        lk.write_text("%d %d\n" % (os.getpid(), int(pr.time.time()) + pr.LEASE))    # (ours again by the body)
        pr.release_send_lock(str(f))
        check(not lk.exists(), "the holder's own release clears it")
        # (a live holder is refused even when its lease expired, unless its pid is certainly gone; a dead pid is
        #  taken over even when its lease still runs — the wall a backwards clock jump cannot move (issue #1265 review))
        # (the install of a taken-over lock must be atomic: four senders racing over one folder post every summary
        #  exactly once between them — a remove-then-create takeover lets two claim one expired lock and both send)
        import multiprocessing as _mp
        ctx = _mp.get_context("fork") if hasattr(_mp, "get_context") else None
        if ctx is not None:   # (posix only: fork is what makes four senders real here)
            f = game_folder(tmp, "four senders at once", summaries=4)
            lock.bodies.clear()   # (the shared relay: only this race's POSTs count)
            q = ctx.Queue()
            def _send(folder, url):
                q.put(pr.send_waiting(str(folder), url=url, timeout=2))
            kids = [ctx.Process(target=_send, args=(str(f), lock.url)) for _ in range(4)]
            [k.start() for k in kids]
            [k.join(20) for k in kids]
            results = [q.get(timeout=2) for _ in kids]
            posted = sorted(b["n"] for b in lock.bodies)
            check(posted == [0, 1, 2, 3] and not pr.waiting(str(f)) and not (f / pr.PENDING / pr.LOCK).exists(),
                  "four senders race one folder: every summary posted exactly once, no lock left (%s, POSTs=%s)" % (results, posted))
        # (the takeover install is atomic by construction — link, not remove-then-rename — pinned with a rendezvous
        #  inside the claim: taker A moves the expired lock aside and pauses; B (this process) makes its lock; A's
        #  install must then FAIL against B's (O_EXCL create), not overwrite it (issue #1265, review))
        if ctx is not None:
            f = game_folder(tmp, "the claim rendezvous", summaries=1)
            lk = f / pr.PENDING / pr.LOCK
            lk.write_text("999999999 %d\n" % (int(pr.time.time()) - 1))   # (expired: both takers eye it)
            ev_claimed, ev_b_done = ctx.Event(), ctx.Event()
            def taker_a(qq):
                real_rename = pr.os.rename
                def slow_rename(a, b, *rest):
                    r = real_rename(a, b, *rest)
                    if str(a).endswith(pr.LOCK):   # (the claim rename: hold A between claim and install)
                        ev_claimed.set()
                        ev_b_done.wait(10)
                    return r
                pr.os.rename = slow_rename
                qq.put(pr.take_send_lock(str(f)))
            qq = ctx.Queue()
            kid = ctx.Process(target=taker_a, args=(qq,))
            kid.start()
            assert ev_claimed.wait(10), "taker A never claimed"
            b_ok = pr.take_send_lock(str(f))   # (B: the expired lock is aside; a fresh lock is made here)
            ev_b_done.set()
            a_ok = qq.get(timeout=10)
            kid.join(10)
            body_pid = int(lk.read_text().split()[0]) if lk.exists() else None
            check(b_ok and not a_ok and body_pid == os.getpid(),
                  "taker A's install fails against B's fresh lock (O_EXCL create, not open-anything): A=%s B=%s lock owner=%s" % (a_ok, b_ok, body_pid))
            pr.release_send_lock(str(f))
        f = game_folder(tmp, "pid is the wall", summaries=1)
        lk = f / pr.PENDING / pr.LOCK
        lk.write_text("%d %d\n" % (os.getppid(), int(pr.time.time()) - 1))   # (lease expired, holder ALIVE (parent))
        check(not pr.take_send_lock(str(f)), "a holder whose pid is running: its lock holds past its lease (a slow "
                                             "sender mid-heartbeat is not taken over) (%s)")
        lk.write_text("999999999 %d\n" % int(pr.time.time() + 10 * pr.LEASE))   # (dead pid, lease still running)
        check(pr.take_send_lock(str(f)), "a holder whose pid is certainly gone: taken over mid-lease — no waiting "
                                         "for the wall clock to catch up after a clock jump backwards")
        pr.release_send_lock(str(f))
        # (round three, the wall: a live holder's lock is NEVER claimable, however far past its lease. A review
        #  reproduced four windows on 81923547f15ca684808bbb445f6e4f that all required a live holder's lock to read
        #  stale — a taker mid-claim, an empty mid-create body, a parked beat, a check-then-unlink release. With the
        #  wall none of them has anything to claim (issue #1265, review round three).)
        f = game_folder(tmp, "the wall over a wedged sender", summaries=1)
        lk = f / pr.PENDING / pr.LOCK
        import os as _os
        lk.write_text("%d %d\n" % (os.getppid(), int(pr.time.time()) - pr.STALL - 600))   # (alive, lease long gone:
        check(not pr.take_send_lock(str(f)),                                              #  a send wedged on a stuck FS)
              "a live holder wedged long past lease+STALL: holds while its mtime walks with its beats — a clock "
              "jump forward cannot evict a working sender (issue #1265, review round three)")
        _os.utime(str(lk), (pr.time.time() - 2 * pr.BEAT - 5, pr.time.time() - 2 * pr.BEAT - 5))   # (same live pid,
        check(pr.take_send_lock(str(f)),                                                   #  but silent past 2 beats:
              "a live-pid holder that has gone quiet past 2*BEAT is claimable — no permanent wedge, and a pid "     #  dead or recycled)
              "recycled by an unrelated process cannot shut out sending forever (%s)")
        lk.write_text("")   # (an empty body mid-create: a sender may be writing the pid this instant; not claimable
        (f / pr.PENDING / pr.LOCK).touch()                                    #  until it ages past LEGACY)
        _os.utime(str(lk), (pr.time.time() - 1, pr.time.time() - 1))          # (a fresh empty body: holds)
        check(not pr.take_send_lock(str(f)), "an empty lock body younger than LEGACY: a mid-create sender holds; "
                                             "claiming through it double-posted in the round-three review")
        lk.write_text("")
        _os.utime(str(lk), (pr.time.time() - pr.LEGACY - 5, pr.time.time() - pr.LEGACY - 5))
        check(pr.take_send_lock(str(f)), "an unreadable body older than LEGACY: a corpse, cleared and claimed")
        pr.release_send_lock(str(f))
        # (round three, the pre-claim TOCTOU the review reproduced: A judges the expired lock stale and pauses before
        #  its claim rename; B takes over cleanly in that breath; A's rename then moves B's FRESH lock away. Shipped
        #  code inspects what the claim moved: it names a live sender, so A slides it back into the free path and
        #  retries — B keeps its lock and A is refused. Rendezvous-pinned, not raced (issue #1265, review round three).)
        if ctx is not None:
            f = game_folder(tmp, "the claim inspects its corpse", summaries=1)
            lk = f / pr.PENDING / pr.LOCK
            lk.write_text("999999999 %d\n" % (int(pr.time.time()) - 1))   # (expired: A eyes it)
            ev_paused, ev_b_done = ctx.Event(), ctx.Event()
            def taker_a(qq):
                real_judge = pr._lock_holds
                def judge(path, *rest):
                    r = real_judge(path, *rest)
                    if str(path).endswith(pr.LOCK) and not r:   # (A just judged it claimable: pause THERE,
                        ev_paused.set()                          #  before the claim rename touches anything
                        ev_b_done.wait(10)
                    return r
                pr._lock_holds = judge
                qq.put(pr.take_send_lock(str(f)))
            qq = ctx.Queue()
            kid = ctx.Process(target=taker_a, args=(qq,))
            kid.start()
            assert ev_paused.wait(10), "taker A never judged-and-paused"
            b_ok = pr.take_send_lock(str(f))   # (B claims the expired lock and installs its own, cleanly)
            ev_b_done.set()
            a_ok = qq.get(timeout=10)
            kid.join(10)
            body_pid = int(lk.read_text().split()[0]) if lk.exists() else None
            check(b_ok and not a_ok and body_pid == os.getpid(),
                  "A judged stale, B took over mid-judgement, A's claim moved B's live lock and put it back: "
                  "A=%s B=%s lock owner=%s (issue #1265, review round three)" % (a_ok, b_ok, body_pid))
            pr.release_send_lock(str(f))
        # (a .sending left as a DIRECTORY — an older or broken form — must not wedge sending forever)
        f = game_folder(tmp, "lock as a directory", summaries=1)
        (f / pr.PENDING / pr.LOCK).mkdir()
        got = pr.send_waiting(str(f), url=lock.url)
        check(got["sent"] == 1, "a .sending left as a directory: cleared, the send runs (%s)" % got)
        # (a lock whose body couldn't be written (a full disk) must be removed, not handed out unreadable and not
        #  left to block the retry: the sender itself goes ahead, and the next start finds no lock at all)
        import errno as _e
        from unittest import mock
        f = game_folder(tmp, "a full disk at the lock", summaries=1)
        with mock.patch.object(pr.os, "write", side_effect=OSError(_e.ENOSPC, "disk full")):
            got = pr.send_waiting(str(f), url=lock.url)
        check(not str(got).startswith("EXC") and not (f / pr.PENDING / pr.LOCK).exists(),
              "ENOSPC writing the lock body: no crash, no unreadable lock left behind (%s)" % got)
        f = game_folder(tmp, "the retry after a full disk", summaries=1)
        (f / pr.PENDING / pr.LOCK).write_text("999999999 %d\n" % (int(pr.time.time()) - 1))   # (a half-made lock its
        got = pr.send_waiting(str(f), url=lock.url)                                           #  dead sender left)
        check(got["sent"] == 1 and not (f / pr.PENDING / pr.LOCK).exists(),
              "the send right after: no wedged lock, the dead sender's half-lock is taken and gone (%s)" % got)
    finally:
        lock.close()

    # (a redirect is the relay pointing elsewhere, not taking the summary: urllib must NOT follow it into a GET whose
    #  200 would read as accepted (issue #1265, review). The proxied path is what a player's system proxy hits.)
    import http.server as _hs
    seen = []
    class Redirector(_hs.BaseHTTPRequestHandler):
        def do_POST(self):
            seen.append("POST"); self.rfile.read(int(self.headers["Content-Length"]))
            self.send_response(302); self.send_header("Location", "/elsewhere"); self.send_header("Content-Length", "0")
            self.end_headers()
        def do_GET(self):
            seen.append("GET"); self.send_response(200); self.send_header("Content-Length", "0"); self.end_headers()
        def log_message(self, *a):
            pass
    srv = _hs.ThreadingHTTPServer(("127.0.0.1", 0), Redirector)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        url = "http://127.0.0.1:%d/perf" % srv.server_address[1]
        f = game_folder(tmp, "a proxy that redirects", summaries=1)
        with Env(http_proxy=url, HTTP_PROXY=url, no_proxy="", NO_PROXY=""):
            got = pr.send_waiting(str(f), url=url)
        check(got["left"] == 1 and got["sent"] == 0 and seen == ["POST"],
              "a 302 (through the system's proxy) is the answer: the summary is kept, nothing follows to a GET (%s %s)" % (got, seen))
    finally:
        srv.shutdown(); srv.server_close()

    # (source pins for the review round-two shape: the loop notices an untick mid-send, the takeover installs by
    #  link (atomic), the release checks the body's owner, and the proxy answer never follows a redirect)
    import inspect as _inspect
    _send_these = _inspect.getsource(pr.send_these)
    assert "if not sending(folder):" in _send_these, \
        "send_these notices set_sending(False) mid-loop and re-sweeps (issue #1265, review)"
    _take = _inspect.getsource(pr.take_send_lock)
    assert "os.link(" not in _take and "O_CREAT | os.O_EXCL" in _take and "os.remove(path)" not in _take.split("claim it")[0].split("for _ in")[1], \
        "a takeover claims by rename and installs by O_EXCL create (atomic everywhere: link() has no hard links on " \
        "vfat/exfat, a Deck's microSD game folder), never remove-then-rename (issue #1265, review round three)"
    assert "_lock_holds(mine)" in _take, \
        "a taker inspects what its claim moved before discarding it: a lock renamed away that names a LIVE sender " \
        "goes back to its name (the round-three pre-claim TOCTOU; issue #1265)"
    _holds = _inspect.getsource(pr._lock_holds)
    assert "_pid_gone(pid)" in _holds and "return True" in _holds, \
        "the wall: a lock whose body names a running pid never goes stale (closes the claim/release windows " \
        "a round-three review reproduced on 81926bf5; issue #1265)"
    _swait = _inspect.getsource(pr.send_waiting)
    assert _swait.index("beat.join(") < _swait.index("release_send_lock"), \
        "the heartbeat thread is joined BEFORE the lock is released: a beat cannot land after hand-off " \
        "(issue #1265, review round three)"
    _rel = _inspect.getsource(pr.release_send_lock)
    assert "_lock_read(path)[0] != os.getpid()" in _rel, \
        "release removes only the lock whose body names the releaser (issue #1265, review)"
    _post = _inspect.getsource(pr.post)
    assert "build_opener(_NoRedirect)" in _post and "urllib.request.urlopen" not in _post, \
        "post() judges the 3xx itself; urllib never follows it into a GET that reads as accepted (issue #1265, review)"

    relay = Relay()
    try:
        f = game_folder(tmp, "no copies", summaries=2)
        (f / pr.SENT).mkdir()
        (f / pr.SENT / "1600000000-0.json").write_text("{}")
        script = str(ROOT / "nx2" / "perf_report.py")
        off = subprocess.run([sys.executable, "-I", script, str(f), "--tick", "on", "--keep-sent", "off"],
                             capture_output=True, text=True)
        check(off.returncode == 0 and (f / pr.CHOICE).read_text() == "send=1\nkeep_sent=0\n"
              and pr.sending(str(f)) and not pr.keeping_sent(str(f)) and not list((f / pr.SENT).glob("*.json")),
              "--keep-sent off: keep_sent=0, the tick still on, and perf_sent/ emptied (%s)" % off.stderr.strip())
        got = pr.send_waiting(str(f), url=relay.url)
        check(got["sent"] == 2 and not list((f / pr.SENT).glob("*.json")), "copies off: sent, and no copy kept")
        bad = subprocess.run([sys.executable, "-I", script, str(f), "--tick", "on", "--keep-sent", "maybe"],
                             capture_output=True, text=True)
        check(bad.returncode != 0, "--keep-sent takes on or off only")
        with Env(LONGSHOT_PERF_KEEP_SENT="on"):
            check(pr.keeping_sent(str(f)), "LONGSHOT_PERF_KEEP_SENT=on wins over keep_sent=0")
        f2 = game_folder(tmp, "copies by default", summaries=1)
        with Env(LONGSHOT_PERF_KEEP_SENT="off"):
            got = pr.send_waiting(str(f2), url=relay.url)
        check(pr.keeping_sent(str(f2)) and got["sent"] == 1 and not list((f2 / pr.SENT).glob("*.json")),
              "copies on by default; LONGSHOT_PERF_KEEP_SENT=off keeps none")
    finally:
        relay.close()

    with Env(LONGSHOT_PERF_URL="http://example.com/perf"):
        check(pr.endpoint() == pr.ENDPOINT, "LONGSHOT_PERF_URL over plain http to another PC: not taken")
    with Env(LONGSHOT_PERF_URL="http://127.0.0.1:8080/perf"):
        check(pr.endpoint() == "http://127.0.0.1:8080/perf", "LONGSHOT_PERF_URL to this PC: a local relay")
    with Env(LONGSHOT_PERF_URL="https://relay.example/telemetry/perf"):
        check(pr.endpoint() == "https://relay.example/telemetry/perf", "LONGSHOT_PERF_URL over https")


def wiring():
    vk = ROOT / "gpu" / "vk"
    fps = (vk / "vk_fps.cpp").read_text(encoding="utf-8")
    check("perf_report_init()" in fps and "perf_frame(" in fps, "vk_fps.cpp starts the summary and adds each frame")
    check("perf_measure" in (vk / "vk_fps.h").read_text(encoding="utf-8"), "the Breakdown is measured while the summary is on")
    cpp = (vk / "perf_report.cpp").read_text(encoding="utf-8")
    check((vk / "perf_curl.cpp").exists() and (vk / "perf_curl.h").exists(),
          "curl's posting lives in perf_curl.cpp/h")
    check((vk / "perf_os.h").exists() and (vk / "perf_os_posix.cpp").exists() and (vk / "perf_os_win.cpp").exists(),
          "external-tool spawning is a generic OS interface with one file per platform")
    check("CreateProcessW" not in cpp and "posix_spawn" not in cpp and "find_curl" not in cpp,
          "perf_report.cpp keeps the policy, not the platform's curl and spawn code")
    check('"longshot_perf.ini"' in cpp and '"send="' in cpp, "the game reads the tick from longshot_perf.ini")
    check('pr::summary_period(getenv("RX_VK_PERF_INTERVAL"))' in cpp and "since_summary >= period" in cpp,
          "the game's interval is RX_VK_PERF_INTERVAL")
    check('"perf_pending"' in cpp and "rename(" in cpp, "summaries are written whole into perf_pending")
    for f, use in (("vk_replay.cpp", "perf_report_final(perf_report::kDeviceLost)"), ("vk_replay.cpp", "kVramOverBudget"),
                   ("vk_replay.cpp", "kMovieUpscalerDropped"), ("vk_dlss.cpp", "kDlssRecreated")):
        check(use in (vk / f).read_text(encoding="utf-8"), "%s reports %s" % (f, use))
    main_cpp = (vk / "vk_main.cpp").read_text(encoding="utf-8")
    close = main_cpp[main_cpp.index("void mkwvk_close_asked(void) {"):]
    check("perf_report_final(-1)" in close[:close.index("\n}\n")],
          "the window's close writes the last summary (ExitProcess is _exit on Linux: no atexit)")
    api = (vk / "vk_api.h").read_text(encoding="utf-8")
    check("perf_info" in api, "the host gives the renderer the map and the movie totals (perf_info)")
    check("hos_perf_info" in (ROOT / "host" / "rx_backend.c").read_text(encoding="utf-8"), "rx_backend.c fills perf_info")
    check("call_once(once, [] {\n        perf_report_final(perf_report::kDeviceLost);" in
          (vk / "vk_replay.cpp").read_text(encoding="utf-8"), "a lost device's summary is written once")
    check("if (!ticked(folder))" in cpp[cpp.index("void summarize("):], "unticked while the game runs: it stops")
    check("constexpr int kMaxMaps = 16;" in (vk / "perf_report.h").read_text(encoding="utf-8") and
          "maps.size() >= (size_t)pr::kMaxMaps" in cpp and "if (where_now()) summarize(false), since_summary = 0, where_now();" in cpp,
          "a summary holds 16 maps at most, as the relay takes them: a 17th ends it early")
    check("shaders_preparing() ? pr::kPreparingShaders : pr::kTitleAndIntro" in cpp and
          "bool shaders_preparing()" in main_cpp, "before a map: Preparing shaders, then Title and intro (not 'not known')")
    header = (vk / "perf_report.h").read_text(encoding="utf-8")
    m = re.search(r"constexpr size_t kMaxBody = (\d+) \* (\d+);", header)
    check(m is not None and int(m.group(1)) * int(m.group(2)) == pr.MAX_BODY,
          "the game fits its summaries to the sender's MAX_BODY (a bigger one is refused and deleted)")
    frame_fn = cpp[cpp.index("void perf_frame("):]
    frame_fn = frame_fn[:frame_fn.index("\n}\n")]
    check("since_sample" not in cpp and "if (!current || since_map >= kMapEvery) {" in frame_fn and
          "constexpr double kMapEvery = 1;" in cpp, "one clock for the map and the one-second sample")
    summarize = cpp[cpp.index("void summarize("):]
    check("if (now) finish_sample();" in summarize[:summarize.index("\n}\n")],
          "a summary mid-second keeps the second being sampled: the frames after it are sampled too")
    host = (ROOT / "host" / "hos_main.c").read_text(encoding="utf-8")
    pose_fn = host[host.index("static void oot_perf_pose("):]
    pose_fn = pose_fn[:pose_fn.index("\n}\n")]
    check("if (now - last < PERF_POSE_EVERY_NS) return;" in pose_fn and
          pose_fn.index("PERF_POSE_EVERY_NS") < pose_fn.index("oot_gameplay_view(c)"),
          "Link's pose is read a few times a second, not at every update")
    check("recomp_load64(c, view) != 0x84c18a88ULL" in pose_fn and "oot_event_playing(c)" in pose_fn and
          "isfinite" in pose_fn, "the pose is read only from the gameplay camera's view while no event plays, and a stray value is no pose")
    info_fn = host[host.index("const char *hos_perf_info("):]
    check("now - perf_pose_ns < PERF_POSE_STALE_NS" in info_fn[:info_fn.index("\n}\n")],
          "a pose Link's update no longer sets (the title, a load) is no pose")
    title = host[host.index("if (!loaded && logged[0]) {"):]
    check("perf_pose_clear();" in title[:title.index("\n    }\n")], "back at the title: no pose from the last map")


def relay_round(v):
    """Go's math.Round: halves away from zero"""
    return math.copysign(math.floor(abs(v) + 0.5), v)


def relay_cell(s):
    """The cell the relay files a sample's place under (Longshot-api telemetry/perf/totals.go addPosition at eadbca4,
    https://git.magicbean.dev/MagicBeanDev/DataServicesAPI/src/commit/eadbca4c0bb47a9e128f4c314cf2a9c9916fec15/telemetry/perf/totals.go#L205):
    each axis's nearest multiple of 100, math.Round(v / 100) * 100"""
    return tuple(relay_round(s[k] / 100) * 100 for k in ("x", "y", "z"))


def relay_view(s):
    """The view bins it files the angles under (the same function): the yaw wrapped round and floored to 45 degrees from
    -180, the pitch floored to 30 degrees from -90, both in degrees"""
    yaw = math.fmod(s["yaw"] * 180 / math.pi + 180, 360)
    return (math.floor(yaw / 45) * 45 - 180, math.floor((s["pitch"] * 180 / math.pi + 90) / 30) * 30 - 90)


def relay_takes(s):
    """Whether the relay takes the sample (telemetry/perf/summary.go, the sample's coordinates at L471): a coordinate out of
    its range, or not a number, and it refuses the whole summary with a 400, which the sender then deletes"""
    ranges = {"x": 1e9, "y": 1e9, "z": 1e9, "yaw": math.pi, "pitch": math.pi / 2}
    return all(isinstance(s.get(k), (int, float)) and not isinstance(s.get(k), bool) and math.isfinite(s[k]) and abs(s[k]) <= top
               for k, top in ranges.items())


def relay_rules(written):
    """The exact places and views the C++ test made, and the sample the game makes of each, against the relay's rules"""
    rows = [line.rstrip("\n").split("\t") for line in open(written / "pairs.tsv", encoding="utf-8")]
    check(len(rows) > 20000, "the C++ test wrote its places and views (%d)" % len(rows))
    refused, other_cell, other_view, cells, straight_up = [], [], [], set(), 0
    for x, y, z, yaw, pitch, sample_text in rows:
        exact = dict(zip("xyz", map(float, (x, y, z))), yaw=float(yaw), pitch=float(pitch))
        sample = json.loads(sample_text)
        if not relay_takes(exact):
            check(False, "the C++ test's own exact place is out of the relay's range: %s" % exact)
        if not relay_takes(sample):
            refused.append((exact, sample))
            continue
        cells.add(relay_cell(sample))
        if relay_cell(sample) != relay_cell(exact):   # (-0.0 == 0.0: the relay's own label for an exact -40 is "x=-0")
            other_cell.append((exact, sample, relay_cell(exact), relay_cell(sample)))
        want = relay_view(exact)
        if exact["pitch"] >= math.pi / 2:   # (the relay's 90-degree pitch bin holds only exactly straight up: the game puts it in the top bin)
            want, straight_up = (want[0], 60), straight_up + 1
        if relay_view(sample) != want:
            other_view.append((exact, sample, want, relay_view(sample)))
    check(not refused, "the relay takes every sample the game sends (it refuses the whole summary for one out of range; %d of %d "
                       "are, the first: %s)" % (len(refused), len(rows), refused[:1]))
    check(not other_cell, "the relay files each sample under the cell it files the exact place under (%d of %d differ, the first: %s)"
          % (len(other_cell), len(rows), other_cell[:1]))
    check(not other_view, "the relay files each sample under the view bins it files the exact angles under (%d of %d differ, "
                          "the first: %s)" % (len(other_view), len(rows), other_view[:1]))
    check(straight_up >= 1 and (0.0, 0.0, 0.0) in cells, "the cases include straight up, and the cell at the origin, x=0, y=0, z=0, is sent "
                                                          "(the relay's own rounding of a half-way sample sent it away)")


def sizes(tmp, written):
    """Each size case re-encoded the way the sender does, and sent by the sender itself to a stand-in relay"""
    cases = sorted(written.glob("size-*.json"))
    check(len(cases) == 8, "the C++ test wrote its size cases (%d)" % len(cases))
    for f in cases:
        body = json.dumps(json.loads(f.read_bytes()), separators=(",", ":")).encode()
        check(len(body) <= pr.MAX_BODY, "%s re-encoded as the sender does: %d B, at most %d" % (f.name, len(body), pr.MAX_BODY))
    folder = game_folder(tmp, "sizes", summaries=0)
    for i, f in enumerate(cases):
        (folder / pr.PENDING / ("17100000%02d-%d.json" % (i, i))).write_bytes(f.read_bytes())
    relay = Relay()
    try:
        # (the start's share has its own pin above: raise it here so this checks body sizes, not the cap)
        with Env(LONGSHOT_PERF_MAX_SENDS="48"):
            got = pr.send_waiting(str(folder), url=relay.url)
    finally:
        relay.close()
    check(got == {"sent": len(cases), "refused": 0, "left": 0}, "the sender sends every size case, none refused and deleted (%s)" % got)
    # (what each keeps: every map's totals, and samples spread over the period: the longest names, repeated in each sample and
    # escaped to six bytes a character, leave room for the fewest)
    check([len(b["maps"]) for b in relay.bodies] == [1 if "one-map" in f.name else 16 for f in cases],
          "each keeps every map's totals (%s)" % [len(b["maps"]) for b in relay.bodies])
    check(all(len(b["samples"]) >= 50 for b in relay.bodies), "and at least 50 of its one-second samples (%s)" %
          [len(b["samples"]) for b in relay.bodies])


def unit(cxx, tmp, written):
    exe = Path(tmp, "perf_report_test")
    subprocess.run([cxx, "-std=c++20", "-O2", "-D_CRT_SECURE_NO_WARNINGS", "-include", str(ROOT / "gpu" / "vk" / "compat" / "longshot_env.h"),
                    "-I", str(ROOT / "gpu" / "vk"), str(ROOT / "tests" / "test_perf_report.cpp"), "-o", str(exe)],
                   check=True)
    out = subprocess.run([str(exe), str(written)], capture_output=True, text=True)
    print(out.stdout.strip())
    check(out.returncode == 0, "tests/test_perf_report.cpp" + (": " + out.stderr.strip() if out.stderr else ""))
    m = re.search(r"add_frame: ([\d.]+) ns", out.stdout)
    check(m is not None and float(m.group(1)) < 200, "a frame costs well under a microsecond to add")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        written = Path(tmp, "written")
        written.mkdir()
        unit(a.cxx, tmp, written)
        relay_rules(written)
        sizes(tmp, written)
        wiring()
        launcher_side(tmp)
        sender_switches(tmp)
    print("%d failed" % len(failures) if failures else "all passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
