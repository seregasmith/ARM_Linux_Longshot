"""The performance statistics (docs/performance-statistics.md): the game's summary (gpu/vk/perf_report.h, compiled with
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
        check(got == {"sent": 12, "refused": 0, "left": 8} and [b["n"] for b in relay.bodies] == list(range(12)),
              "at most 12 a start by default, oldest first; the rest wait (%s)" % got)
        with Env(LONGSHOT_PERF_MAX_SENDS="3"):
            got = pr.send_waiting(str(f), url=relay.url)
        check(got == {"sent": 3, "refused": 0, "left": 5}, "LONGSHOT_PERF_MAX_SENDS=3 (%s)" % got)
        with Env(LONGSHOT_PERF_MAX_SENDS="1000"):
            check(pr.env_number("LONGSHOT_PERF_MAX_SENDS", 12, 1, 48) == 48, "a switch past its range: clamped")
        with Env(LONGSHOT_PERF_MAX_SENDS="lots"):
            check(pr.env_number("LONGSHOT_PERF_MAX_SENDS", 12, 1, 48) == 12, "a switch that isn't a number: the default")
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
