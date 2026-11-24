"""The job queue's rules (`dev.py queue`, scripts/devtools/jobqueue.py): what may run beside what, the order, and the
environment a job gets.

- A timed run and a build never run at the same time, whichever came first; a job never starts ahead of a waiting one it
  would hold up (a build behind a queued timed run waits; a shared game run behind a queued exclusive one waits).
- A timed run that uses no GPU lets shared game runs go beside it; timed runs of one group go one at a time.
- Priorities and aging (another version's tickets may carry one): a low ticket that waited an hour is at normal's level,
  never above high.
- A job's environment never gains an OOT_, RX_ or A64_ variable (make_game.py lift_key hashes them all: a variable naming
  the job would give every build its own lift key, so no build took another's translation).
- The RAM file system check takes the longest mount holding a path.
- The queue's tickets are where every version of it looks (scripts/job_queue.py before dev.py): in
  $XDG_RUNTIME_DIR/longshot-queue as <pid>.json under .lock (Windows: %LOCALAPPDATA%\\Longshot\\queue\\tickets, which
  `dev.py migrate` leaves in place); the history in the development folder's queue/, job_queue.json read from its old
  place, ~/.config/longshot, until it is moved.
- Two exclusive jobs run one after the other, two shared ones together (a real queue in a temporary folder).
- Windows: a cancelled job's command and its children end, so do those of a runner killed outright (the job object), and a
  command that ends by itself leaves what it started running.
- Linux: a cancelled job's processes end too, its command's and those that left its tree (a game whose supervisor a
  SIGTERM ended, one in a session of its own), never another job's; a command that ends by itself leaves what it started
  running.
- Windows protects a player as Linux does: a longshot.exe with no LONGSHOT_JOB is someone playing (a timed job waits,
  status names it), one under a job or preparing shaders is not, one whose memory can't be read counts as nobody's;
  a build outside the queue is a python running nx2/build.py or make_game.py, backslashes or not. Checked on a made-up
  process table on any OS (the Windows code, its process reads stood in for), and on Windows with real processes.
- Windows: procs.proc_start sends no Ctrl+C, with common.WINDOWS patched off too, as Environment's test does (os.kill(pid,
  0) there is a Ctrl+C to the whole console: it ended run_all.py and the shell around it at once, without a word).

python -I -B tests/test_job_queue.py
"""
import contextlib
import json
import io
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(ROOT, "scripts", "dev.py")
sys.path.insert(0, os.path.join(ROOT, "scripts"))
from devtools import common, procs  # noqa: E402
from devtools import jobqueue as q  # noqa: E402

SLOTS = {"shared": 3, "build": 2, "light": 4}


def T(pid, name, mode, state="waiting", t=0, prio="normal", **k):
    gpu, cpu = q.MODES[mode]
    return dict(pid=pid, name=name, mode=mode, gpu=gpu, cpu=cpu, state=state, t=t, prio=prio, **k)


class Rules(unittest.TestCase):
    def test_timing_and_builds_never_together(self):
        ts = [T(1, "b0", "build", "active"), T(2, "fps", "timing", t=1)]
        self.assertIn("builds", q.blocker(ts[1], ts, 10, SLOTS))
        ts = [T(1, "fps", "timing", "active"), T(2, "b1", "build", t=1)]
        self.assertIn("timed run", q.blocker(ts[1], ts, 10, SLOTS))
        self.assertIsNone(q.blocker(ts[1], ts, 10, SLOTS, quiet=False))   # (LONGSHOT_QUEUE_QUIET=0)

    def test_timing_and_light_jobs_never_together(self):
        # (a test suite run as light beside a timed run took the PC into swap: the run's p99 went from 13 ms to 25)
        ts = [T(1, "checks", "light", "active"), T(2, "fps", "timing", t=1)]
        self.assertIn("checks", q.blocker(ts[1], ts, 10, SLOTS))
        ts = [T(1, "fps", "timing", "active"), T(2, "checks", "light", t=1)]
        self.assertIn("timed run", q.blocker(ts[1], ts, 10, SLOTS))
        self.assertIsNone(q.blocker(ts[1], ts, 10, SLOTS, quiet=False))
        ts = [T(1, "build-time", "timing-cpu", "active"), T(2, "checks", "light", t=1)]
        self.assertIn("timed run", q.blocker(ts[1], ts, 10, SLOTS))

    def test_no_jumping_ahead(self):
        ts = [T(1, "fps", "timing", t=1), T(2, "b1", "build", t=2)]
        self.assertIn("queued ahead", q.blocker(ts[1], ts, 10, SLOTS))
        ts = [T(1, "sh0", "shared", "active"), T(2, "excl", "gpu", t=1), T(3, "sh1", "shared", t=2)]
        self.assertIn("GPU", q.blocker(ts[1], ts, 10, SLOTS))
        self.assertIn("queued ahead", q.blocker(ts[2], ts, 10, SLOTS))
        ts[1]["ready"] = False   # (a ticket waiting while you play: it keeps its place but holds nobody up)
        self.assertIsNone(q.blocker(ts[2], ts, 10, SLOTS))

    def test_slots(self):
        ts = [T(i, "b%d" % i, "build", "active") for i in (1, 2)] + [T(3, "b3", "build", t=1)]
        self.assertIn("build slots", q.blocker(ts[2], ts, 10, SLOTS))
        self.assertIsNone(q.blocker(ts[2], ts, 10, dict(SLOTS, build=3)))
        ts = [T(i, "s%d" % i, "shared", "active") for i in (1, 2, 3)] + [T(4, "s4", "shared", t=1)]
        self.assertIn("shared GPU slots", q.blocker(ts[3], ts, 10, SLOTS))
        ts = [T(1, "g", "gpu", "active"), T(2, "checks", "light", t=1), T(3, "b", "build", t=2)]
        self.assertIsNone(q.blocker(ts[1], ts, 10, SLOTS))
        self.assertIsNone(q.blocker(ts[2], ts, 10, SLOTS))

    def test_timing_without_gpu_and_groups(self):
        ts = [T(1, "build-time", "timing-cpu", "active"), T(2, "shots", "shared", t=1), T(3, "fps", "timing", t=2)]
        self.assertIsNone(q.blocker(ts[1], ts, 10, SLOTS))
        self.assertIsNotNone(q.blocker(ts[2], ts, 10, SLOTS))   # (the GPU the shot run claimed, and the measuring one)
        ts = [T(1, "area-x-1", "timing", "active"), T(2, "area-x-2", "timing", t=1), T(3, "other-1", "timing", t=2)]
        self.assertIn("one timed run per group", q.blocker(ts[1], ts, 10, SLOTS))
        self.assertNotIn("per group", q.blocker(ts[2], ts, 10, SLOTS))   # (another group's: waits for the GPU only)
        self.assertEqual(q.group(T(9, "area-x-3", "timing")), "area-x")
        self.assertEqual(q.group(T(9, "fps-kokiri", "timing", group="mine")), "mine")

    def test_priorities_and_aging(self):
        low_hour = q.rank(T(1, "x", "build", t=0, prio="low"), 3700)
        self.assertEqual(low_hour[0], 0)
        self.assertLess(q.rank(T(2, "y", "build", t=3000, prio="high"), 3700), low_hour)
        self.assertEqual(q.rank(T(3, "z", "build", t=0, prio="normal"), 99999)[0], 0)


class NoiseOfARun(unittest.TestCase):
    QUIET = {"cpu": 3.0, "memory": 0.0, "io": 0.2, "load": 9.0, "strays": 0, "beside": [], "other_cpus": 6.0}

    def samples(self, n=5, **busy):
        return [dict(self.QUIET, t=float(i), swap_pages=1000, **busy) for i in range(n)]

    def test_quiet_pc(self):
        rec = q.noise_summary(self.samples())
        self.assertEqual(rec["swap_mb"], 0)
        self.assertEqual(rec["other_cpus_busy"], 6)
        self.assertEqual(q.noise_reasons(rec), [])

    def test_each_kind_of_noise(self):
        swapping = self.samples()
        for i, x in enumerate(swapping):
            x["swap_pages"] = 1000 + i * 20000   # (80,000 pages in 4 s: 78 MB/s)
        trickle = self.samples()
        for i, x in enumerate(trickle):
            x["swap_pages"] = 1000 + i * 300   # (an idle PC's few MB/s: no noise)
        self.assertEqual(q.noise_reasons(q.noise_summary(trickle)), [])
        # (swapping alone is no noise: the game's own size swaps the rest of the PC out while it stalls on nothing)
        self.assertEqual(q.noise_summary(swapping)["swap_mb_s"], 78.0)
        self.assertEqual(q.noise_reasons(q.noise_summary(swapping)), [])
        cases = [(self.samples(memory=3.6), "memory stalls"), (self.samples(cpu=40.0), "CPU stalls"),
                 (self.samples(io=30.0), "disk stalls"), (self.samples(other_cpus=85.0), "other CPUs 85% busy"),
                 (self.samples(strays=1), "builds outside"), (self.samples(beside=["gb-3-tests (light)"]), "gb-3-tests")]
        for samples, said in cases:
            reasons = q.noise_reasons(q.noise_summary(samples))
            self.assertEqual(len(reasons), 1, reasons)
            self.assertIn(said, reasons[0])

    def test_the_runs_own_stalls(self):
        # (a job's cgroup: its own PSI, read from <cgroup>/<kind>.pressure)
        with tempfile.TemporaryDirectory() as d:
            for kind, full in (("memory", 0.05), ("io", 0.23), ("cpu", 0.06)):
                with open(os.path.join(d, kind + ".pressure"), "w") as f:
                    f.write("some avg10=0.50 avg60=0.0 avg300=0.0 total=1\nfull avg10=%.2f avg60=0.0 avg300=0.0 total=1\n" % full)
            self.assertEqual(q.pressure("memory", "full", d), 0.05)
            self.assertEqual(q.pressure("cpu", "some", d), 0.5)
            n = q.Noise(None, None, d)
            n.sample()
            self.assertTrue(n.samples[0]["own"])
            self.assertEqual(n.samples[0]["io"], 0.23)

    def test_samples_written_for_the_caller(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "noise.jsonl")
            n = q.Noise(None, path)
            n.sample([42], ["b (build)"])
            n.sample()
            with open(path, encoding="utf-8") as f:
                rows = [json.loads(line) for line in f]
            self.assertEqual([r["strays"] for r in rows], [1, 0])
            rec = n.record()
            self.assertEqual(rec["builds_outside"], 1)
            self.assertEqual(rec["beside"], ["b (build)"])


class Environment(unittest.TestCase):
    def test_no_lift_key_variables(self):
        with tempfile.TemporaryDirectory() as d:
            s = dict(q.DEFAULTS, LONGSHOT_QUEUE_STATE=d)
            base = {"PATH": "/usr/bin", "TMPDIR": d}
            for mode in q.MODES:
                env = q.job_env(base, "a-job", mode, s, 42)
                added = {k for k in env if k not in base and k.startswith(q.LIFT_KEY_PREFIXES)}
                self.assertEqual(added, set(), mode)
                self.assertEqual(env["LONGSHOT_JOB_NAME"], "a-job")
            self.assertIn("__GL_SHADER_DISK_CACHE_PATH", q.job_env(base, "g", "shared", s, 42))
            self.assertNotIn("__GL_SHADER_DISK_CACHE_PATH", q.job_env(base, "b", "build", s, 42))

    def test_run_named_on_screen(self):
        # (a game run says what it tests under the version: desc, else what the run set, else the job's name; a build never)
        with tempfile.TemporaryDirectory() as d:
            s = dict(q.DEFAULTS, LONGSHOT_QUEUE_STATE=d)
            base = {"PATH": "/usr/bin", "TMPDIR": d}
            self.assertEqual(q.job_env(base, "ab-main", "timing", s, 42)["LONGSHOT_BUILD_NAME"], "ab-main")
            self.assertEqual(q.job_env(base, "ab-main", "timing", s, 42, "main, Kokiri Forest")["LONGSHOT_BUILD_NAME"],
                             "main, Kokiri Forest")
            own = dict(base, LONGSHOT_BUILD_NAME="mine")
            self.assertEqual(q.job_env(own, "g", "shared", s, 42)["LONGSHOT_BUILD_NAME"], "mine")
            self.assertNotIn("LONGSHOT_BUILD_NAME", q.job_env(base, "b", "build", s, 42))

    def test_ram_file_system(self):
        table = [("/", "btrfs"), ("/tmp", "tmpfs"), ("/tmp/disk", "ext4")]
        self.assertTrue(common.on_ram("/tmp/x", table))
        self.assertFalse(common.on_ram("/tmp/disk/x", table))
        self.assertFalse(common.on_ram("/tmpx", table))

    def test_places_every_version_shares(self):
        with tempfile.TemporaryDirectory() as d:
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith(("LONGSHOT_QUEUE_", "XDG_")) and k != "LONGSHOT_DEV_HOME"}
            env.update(XDG_RUNTIME_DIR=os.path.join(d, "run"), XDG_CACHE_HOME=os.path.join(d, "cache"),
                       XDG_CONFIG_HOME=os.path.join(d, "config"), XDG_DATA_HOME=os.path.join(d, "data"),
                       LOCALAPPDATA=os.path.join(d, "local"), APPDATA=os.path.join(d, "roaming"))
            with mock.patch.dict(os.environ, env, clear=True), mock.patch.object(common, "WINDOWS", False):
                os.makedirs(os.path.join(d, "config", "longshot"))
                with open(os.path.join(d, "config", "longshot", "job_queue.json"), "w") as f:
                    json.dump({"LONGSHOT_QUEUE_BUILD_SLOTS": 3, "other": 1}, f)
                s = q.settings()
                self.assertEqual(s["LONGSHOT_QUEUE_BUILD_SLOTS"], "3")
                self.assertEqual(q.queue_dir(s), os.path.join(d, "run", "longshot-queue"))
                self.assertEqual(q.state_dir(s), os.path.join(d, "data", "longshot-dev", "queue"))
                os.makedirs(os.path.join(d, "cache", "longshot-queue"))   # (not migrated yet: read where it is)
                self.assertEqual(q.state_dir(s), os.path.join(d, "cache", "longshot-queue"))
                t = T(os.getpid(), "old-client", "build", "active", start=procs.proc_start(os.getpid()), ns=procs.pidns())
                q.write_ticket(q.queue_dir(s), t)
                self.assertTrue(os.path.isfile(os.path.join(q.queue_dir(s), "%d.json" % os.getpid())))
                self.assertEqual([x["name"] for x in q.read_tickets(q.queue_dir(s))], ["old-client"])
            with mock.patch.dict(os.environ, env, clear=True), mock.patch.object(common, "WINDOWS", True):
                s = q.settings()
                self.assertEqual(q.state_dir(s), os.path.join(d, "local", "longshot-dev", "queue"))
                self.assertEqual(q.queue_dir(s), os.path.join(d, "local", "Longshot", "queue", "tickets"))
                os.makedirs(os.path.join(d, "local", "longshot-dev", "queue"))   # (migrated: the tickets stay)
                self.assertEqual(q.queue_dir(s), os.path.join(d, "local", "Longshot", "queue", "tickets"))

    def test_cpu_sets(self):
        s = dict(q.DEFAULTS)
        self.assertEqual(q.cpu_list("0-2,5"), {0, 1, 2, 5})
        self.assertEqual(q.cpu_text({0, 1, 2, 5}), "0-2,5")
        self.assertEqual(q.cpu_sets(s, domains=[frozenset({0})]), (None, None))   # (one L3 domain: no pinning)


@unittest.skipIf(os.name == "nt", "the shell commands below")
class RealQueue(unittest.TestCase):
    def run_two(self, mode):
        with tempfile.TemporaryDirectory() as d:
            env = dict(os.environ, LONGSHOT_QUEUE_DIR=os.path.join(d, "q"), LONGSHOT_QUEUE_STATE=os.path.join(d, "s"),
                       LONGSHOT_QUEUE_SCOPE="0", LONGSHOT_QUEUE_MIN_FREE_GB="0", LONGSHOT_QUEUE_MAX_PRESSURE="100",
                       LONGSHOT_QUEUE_MAX_SWAP_GB="100000")
            for k in ("LONGSHOT_JOB",):
                env.pop(k, None)
            marks = os.path.join(d, "marks")
            cmd = lambda n: [sys.executable, "-I", SCRIPT, "queue", "run", n, "--mode", mode, "--",
                             sys.executable, "-c", "import time; f=open(%r, 'a'); f.write('%s+ %%f\\n' %% time.time()); "
                             "f.flush(); time.sleep(1.5); f.write('%s- %%f\\n' %% time.time())" % (marks, n, n)]
            a = subprocess.Popen(cmd("a"), env=env, stderr=subprocess.DEVNULL)
            time.sleep(0.4)
            b = subprocess.Popen(cmd("b"), env=env, stderr=subprocess.DEVNULL)
            self.assertEqual((a.wait(30), b.wait(30)), (0, 0))
            with open(marks) as f:
                t = {line.split()[0]: float(line.split()[1]) for line in f}
            return t["b+"] < t["a-"]   # (b started while a still ran)

    def test_exclusive_one_at_a_time_shared_together(self):
        self.assertFalse(self.run_two("gpu"))
        self.assertTrue(self.run_two("shared"))


class WindowsPlayers(unittest.TestCase):
    """The Windows code over a made-up process table: Toolhelp's (pid: parent, program) and each process's command line and
    environment as its PEB holds them. Main returned "nobody" on Windows whatever ran"""
    PROCS = {   # (pid: (parent, program, command line, environment block; None: its memory can't be read))
        4: (0, "System", None, None),
        40: (4, "longshot.exe", '"C:\\Games\\Longshot\\longshot.exe"', "=C:=C:\\Games\0PATH=C:\\x\0"),
        41: (4, "Longshot.exe", "longshot.exe --x", "LONGSHOT_JOB=fps\0PATH=C:\\x\0"),
        42: (4, "longshot.exe", "longshot.exe --prepare-shaders", "PATH=C:\\x\0"),
        43: (4, "longshot.exe", None, None),
        50: (4, "python.exe", 'python.exe -I C:\\src\\nx2\\build.py --jobs 4', "PATH=C:\\x\0"),
        51: (4, "python.exe", "python.exe -I nx2/make_game.py", "LONGSHOT_JOB=b1\0"),
        52: (4, "python.exe", "python.exe -c pass", "PATH=C:\\x\0"),
        53: (60, "python.exe", "python.exe nx2/build.py", "PATH=C:\\x\0"),
        60: (4, "cmd.exe", "cmd /c x", "PATH=C:\\x\0"),
        61: (60, "notepad.exe", "notepad", "PATH=C:\\x\0"),
        62: (61, "conhost.exe", "conhost", "PATH=C:\\x\0"),   # (61 is a pid used again: started before its "parent")
    }
    BORN = {60: 10, 61: 15, 53: 20, 62: 1}

    def setUp(self):
        self.read = []
        self.PROCS = dict(self.PROCS)

        def params(pid):
            self.read.append(pid)
            p = self.PROCS.get(pid)
            return None if p is None or p[2] is None else (p[2], p[3] + "\0")
        split = lambda cmd: [a.strip('"') for a in cmd.split()]   # (CommandLineToArgvW enough for these)
        self.patches = [mock.patch.object(common, "WINDOWS", True),
                        mock.patch.object(procs, "_win_snapshot", lambda: {k: v[:2] for k, v in self.PROCS.items()}),
                        mock.patch.object(procs, "_win_params", params),
                        mock.patch.object(procs, "_win_split", split),
                        mock.patch.object(procs, "_win_start", lambda pid: self.BORN.get(pid, 0) if pid in self.PROCS else None),
                        mock.patch.dict(os.environ, {"LONGSHOT_QUEUE_PLAYING_FILE": ""})]
        for m in self.patches:
            m.start()
        self.s = dict(q.DEFAULTS)

    def tearDown(self):
        for m in reversed(self.patches):
            m.stop()

    def test_games(self):
        games = {g["pid"]: g["job"] for g in q.game_procs(self.s)}
        self.assertEqual(games, {40: "", 41: "fps", 42: "prepare", 43: None})
        self.assertFalse({4, 50, 60, 61} & set(self.read), "only the games' and pythons' memory is read")
        self.assertEqual(procs.env_of(40), {"PATH": "C:\\x"}, "the block's =C: entries are no variables")

    def test_playing(self):
        with mock.patch.object(q.time, "sleep"):
            self.assertTrue(q.user_playing(self.s), "a longshot.exe of nobody's job: someone is playing")
            del self.PROCS[40]
            self.assertFalse(q.user_playing(self.s), "a job's game, shader preparation, an unreadable one: nobody")

    def test_status(self):
        out = io.StringIO()
        with mock.patch.object(q, "read_tickets", lambda *a, **k: []), \
                contextlib.redirect_stdout(out):
            q.status(self.s)
        self.assertIn("playing: longshot.exe (pid 40)", out.getvalue())
        self.assertIn("game: Longshot.exe pid 41 under job fps", out.getvalue())
        self.assertIn("a build outside the queue: pid 50", out.getvalue())

    def test_builds_outside(self):
        self.assertEqual(q.heavy_procs(self.s), [50, 53])
        self.assertEqual(q.heavy_procs(self.s, procs.tree(60)), [50], "a job's tree is its own")

    def test_tree(self):
        self.assertEqual(sorted(procs.tree(60)), [53, 60, 61], "61's 62 is older than 61: a pid used again")


@unittest.skipUnless(os.name == "nt", "Windows' processes")
class WindowsPlayersReal(unittest.TestCase):
    """Real processes: a copy of ping.exe named longshot.exe, with and without LONGSHOT_JOB, read through Toolhelp and PEB"""
    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.exe = os.path.join(self.d, "longshot.exe")
        shutil.copy(os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "PING.EXE"), self.exe)
        self.procs = []

    def tearDown(self):
        for p in self.procs:
            p.kill()
            p.wait()
        shutil.rmtree(self.d, ignore_errors=True)

    def start(self, job=None, args=("-n", "60", "127.0.0.1")):
        env = {k: v for k, v in os.environ.items() if k != "LONGSHOT_JOB"}
        if job:
            env["LONGSHOT_JOB"] = job
        p = subprocess.Popen([self.exe, *args], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.procs.append(p)
        return p.pid

    def test_a_game_and_a_jobs_game(self):
        mine, jobs = self.start(), self.start(job="probe")
        time.sleep(0.5)
        s = dict(q.DEFAULTS)
        games = {g["pid"]: g for g in q.game_procs(s)}
        self.assertEqual((games[mine]["job"], games[jobs]["job"]), ("", "probe"))
        self.assertEqual(procs.argv_of(jobs)[1:], ["-n", "60", "127.0.0.1"])
        with mock.patch.dict(os.environ, {"LONGSHOT_QUEUE_PLAYING_FILE": ""}):
            self.assertTrue(q.user_playing(s))
        self.assertIn(jobs, procs.tree(os.getpid()), "the test's own children are in its tree")

    def test_a_build_outside_the_queue(self):
        script = os.path.join(self.d, "nx2", "build.py")
        os.makedirs(os.path.dirname(script))
        with open(script, "w") as f:
            f.write("import time\ntime.sleep(60)\n")
        p = subprocess.Popen([sys.executable, "-I", script])
        self.procs.append(p)
        time.sleep(0.5)
        self.assertIn(p.pid, q.heavy_procs(dict(q.DEFAULTS)))
        self.assertNotIn(p.pid, q.heavy_procs(dict(q.DEFAULTS), procs.tree(os.getpid())))


@unittest.skipUnless(os.name == "nt", "Windows' job object")
class WindowsTree(unittest.TestCase):
    """On Windows a job's command and what it starts end with the job (a real queue in a temporary folder): cancel ends the
    command's own children too, so does a runner killed outright (its job object), and a command that ends by itself leaves
    what it started running. Before the job object a cancelled job's children ran on: builds and games beside the next job"""
    GRAND = ("import os, sys, time; open(sys.argv[1] + '.tmp', 'w').write(str(os.getpid())); "
             "os.replace(sys.argv[1] + '.tmp', sys.argv[1]); time.sleep(120)")

    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.env = dict(os.environ, LONGSHOT_QUEUE_DIR=os.path.join(self.d, "q"), LONGSHOT_QUEUE_STATE=os.path.join(self.d, "s"),
                        LONGSHOT_QUEUE_SCOPE="0", LONGSHOT_QUEUE_MIN_FREE_GB="0", LONGSHOT_QUEUE_MAX_PRESSURE="100",
                        LONGSHOT_QUEUE_MAX_SWAP_GB="100000")
        for k in ("LONGSHOT_JOB",):
            self.env.pop(k, None)
        self.pidfile = os.path.join(self.d, "grandchild")
        self.grand = None

    def tearDown(self):
        if self.grand and procs._win_start(self.grand) is not None:
            subprocess.run(["taskkill", "/F", "/PID", str(self.grand)], capture_output=True)
        subprocess.run(["cmd", "/c", "rmdir", "/s", "/q", self.d], capture_output=True)

    def start(self, waits):
        """A runner whose command starts a grandchild, then waits for it (waits) or ends at once; the grandchild's pid"""
        inner = "import subprocess, sys; p = subprocess.Popen([sys.executable, '-c', %r, %r])%s" % (
            self.GRAND, self.pidfile, "; p.wait()" if waits else "")
        runner = subprocess.Popen([sys.executable, "-I", SCRIPT, "queue", "run", "tree", "--mode", "light", "--",
                                   sys.executable, "-c", inner], env=self.env, stderr=subprocess.DEVNULL)
        for _ in range(200):
            if os.path.exists(self.pidfile):
                break
            time.sleep(0.1)
        with open(self.pidfile) as f:
            self.grand = int(f.read())
        return runner

    def gone(self, pid):
        for _ in range(50):
            if procs._win_start(pid) is None:
                return True
            time.sleep(0.1)
        return False

    def test_cancel_ends_the_commands_children(self):
        runner = self.start(waits=True)
        subprocess.run([sys.executable, "-I", SCRIPT, "queue", "cancel", "tree"], env=self.env, capture_output=True, check=True)
        runner.wait(20)
        self.assertTrue(self.gone(self.grand), "the cancelled job's grandchild still runs")

    def test_a_runner_killed_outright_ends_them(self):
        runner = self.start(waits=True)
        runner.kill()   # (TerminateProcess: no finally runs, as cancel's os.kill did; only the job object is left to act)
        runner.wait(20)
        self.assertTrue(self.gone(self.grand), "a killed runner's grandchild still runs")

    def test_a_command_that_ends_leaves_what_it_started(self):
        runner = self.start(waits=False)
        self.assertEqual(runner.wait(30), 0)
        time.sleep(1)
        self.assertIsNotNone(procs._win_start(self.grand), "the job's end took what its command left running")


@unittest.skipIf(os.name == "nt", "Linux's processes (Windows: WindowsTree)")
class PosixTree(unittest.TestCase):
    """On Linux a cancelled job's processes end too, wherever they went (a real queue in a temporary folder). The command
    stands in for longshot_bridge.py's supervisor: a python that a SIGTERM ends before it stops the game it started. Main
    stopped the runner and its command only, so the game ran on as an orphan, its job gone from the queue, holding the GPU
    and memory the next job waited for. A game in a session of its own ends as well; a process of another job, and what a
    command that ended by itself left running, go on"""
    GRAND = ("import os, sys, time; open(sys.argv[1] + '.tmp', 'w').write(str(os.getpid())); "
             "os.replace(sys.argv[1] + '.tmp', sys.argv[1]); time.sleep(120)")

    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.env = dict(os.environ, LONGSHOT_QUEUE_DIR=os.path.join(self.d, "q"), LONGSHOT_QUEUE_STATE=os.path.join(self.d, "s"),
                        LONGSHOT_QUEUE_SCOPE="0", LONGSHOT_QUEUE_MIN_FREE_GB="0", LONGSHOT_QUEUE_MAX_PRESSURE="100",
                        LONGSHOT_QUEUE_MAX_SWAP_GB="100000", LONGSHOT_QUEUE_MIN_DISK_GB="0")
        for k in [k for k in self.env if k == "LONGSHOT_JOB" or k.startswith("LONGSHOT_JOB_")]:
            del self.env[k]
        self.pidfile = os.path.join(self.d, "grandchild")
        self.kids = []   # ((pid, start) to end after the test, whatever it found)

    def tearDown(self):
        for pid, born in self.kids:
            if procs.proc_start(pid) == born:
                try:
                    os.kill(pid, 9)
                except OSError:
                    pass
        shutil.rmtree(self.d, ignore_errors=True)

    def start(self, waits=True, detach=False):
        """A runner whose command starts a game (a grandchild, in a new session when detach), then waits for it (waits) or
        ends at once; the runner, and the grandchild's (pid, start)"""
        inner = "import subprocess, sys; p = subprocess.Popen([sys.executable, '-c', %r, %r], start_new_session=%r)%s" % (
            self.GRAND, self.pidfile, detach, "; p.wait()" if waits else "")
        runner = subprocess.Popen([sys.executable, "-I", SCRIPT, "queue", "run", "tree", "--mode", "light", "--",
                                   sys.executable, "-c", inner], env=self.env, stderr=subprocess.DEVNULL)
        for _ in range(200):
            if os.path.exists(self.pidfile):
                break
            time.sleep(0.1)
        with open(self.pidfile) as f:
            grand = int(f.read())
        self.kids.append((grand, procs.proc_start(grand)))
        return runner, self.kids[-1]

    @staticmethod
    def running(pid, born):
        """The process is there and not a zombie (one that ended and waits for its parent)"""
        try:
            with open("/proc/%d/stat" % pid) as f:
                state = f.read().rsplit(")", 1)[1].split()[0]
        except (OSError, IndexError):
            return False
        return state not in ("Z", "X") and procs.proc_start(pid) == born

    def gone(self, grand):
        for _ in range(150):
            if not self.running(*grand):
                return True
            time.sleep(0.1)
        return False

    def cancel(self, runner):
        subprocess.run([sys.executable, "-I", SCRIPT, "queue", "cancel", "tree"], env=self.env, capture_output=True, check=True)
        runner.wait(30)

    def test_cancel_ends_the_game_its_supervisor_started(self):
        runner, grand = self.start()
        self.cancel(runner)
        self.assertTrue(self.gone(grand), "the cancelled job's game still runs")

    def test_cancel_ends_a_game_in_a_session_of_its_own(self):
        runner, grand = self.start(detach=True)
        self.cancel(runner)
        self.assertTrue(self.gone(grand), "the cancelled job's game, in a session of its own, still runs")

    def test_cancel_leaves_another_jobs_process(self):
        other = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(120)"],
                                 env=dict(self.env, LONGSHOT_JOB="1", LONGSHOT_JOB_NAME="tree"))
        self.kids.append((other.pid, procs.proc_start(other.pid)))
        runner, grand = self.start()
        self.cancel(runner)
        self.assertTrue(self.gone(grand), "the cancelled job's game still runs")
        self.assertIsNone(other.poll(), "cancel ended another job's process")
        other.kill()
        other.wait()

    def test_a_command_that_ends_leaves_what_it_started(self):
        runner, grand = self.start(waits=False)
        self.assertEqual(runner.wait(30), 0)
        time.sleep(1)
        self.assertTrue(self.running(*grand), "the job's end took what its command left running")


@unittest.skipUnless(os.name == "nt", "Windows' consoles")
class WindowsConsole(unittest.TestCase):
    """proc_start, with common.WINDOWS off as Environment's test patches it, in a python started the way run_all.py starts a
    check (the same console), under a python with a hidden console of its own that stands in for run_all.py and its shell:
    that one says whether a Ctrl+C reached it"""
    CALLER = ("import os, sys; sys.path.insert(0, sys.argv[1]); from devtools import common, procs; common.WINDOWS = False; "
              "procs.proc_start(os.getpid())")
    BYSTANDER = ("import signal, subprocess, sys, time; got = []; signal.signal(signal.SIGINT, lambda *_: got.append(1)); "
                 "subprocess.run([sys.executable, '-I', '-c', sys.argv[3], sys.argv[1]]); time.sleep(1); "
                 "open(sys.argv[2], 'w').write('ctrl-c' if got else 'quiet')")

    def test_proc_start_sends_no_ctrl_c(self):
        with tempfile.TemporaryDirectory() as d:
            said = os.path.join(d, "said")
            hidden = subprocess.STARTUPINFO()
            hidden.dwFlags |= subprocess.STARTF_USESHOWWINDOW   # (wShowWindow 0: SW_HIDE)
            subprocess.run([sys.executable, "-I", "-c", self.BYSTANDER, os.path.join(ROOT, "scripts"), said, self.CALLER],
                           creationflags=subprocess.CREATE_NEW_CONSOLE, startupinfo=hidden, timeout=60, check=True)
            with open(said) as f:
                self.assertEqual(f.read(), "quiet", "proc_start sent a Ctrl+C to every process of its console")


if __name__ == "__main__":
    unittest.main()
