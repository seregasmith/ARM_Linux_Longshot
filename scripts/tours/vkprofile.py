"""The renderer's play profile recorded on the tour's route (pipeline_tour.py run/tour --vk-profile) and merged into the
profile the build uses (pipeline_tour.py merge-profile)."""
import json
import os
import shutil
import struct
import subprocess
import sys
import time

import longshot_bridge
from tours import ROOT
from tours.drive import Failed

# ---- the renderer's play profile (--vk-profile) ----
# An RX_VK_PGO=gen renderer (nx2/build.py) counts its branches, and its copy of host/pgo_rt.c, reading RX_VK_PGO_FILE=<file>,
# looks every 500 ms for <file>.reset (the counters cleared) and <file>.dump (the counters written to <file>), deleting each once
# done. A run with --vk-profile clears them once the new game has given control of Link (the loading, the prepare and the
# title left out) and writes them at its end, failed or not, as <dir>/<run name>.profraw with <run name>.json (where it was
# recorded); merge-profile merges them with llvm-profdata (docs/dev/developing.md, "Record the renderer's play profile")
PGO_ENV = "RX_VK_PGO_FILE"
PGO_MARK = b"RX_VK_PGO_FILE\x00"   # (the counting renderer's getenv: only its copy of pgo_rt.c names it, -DPGO_ENV)
PGO_RAW_MAGIC = struct.pack("<Q", 0xff6c70726f667281)   # (LLVM's raw profile, host/pgo_rt.c pgo_write: \xfflprofr\x81)
PGO_WAIT_S = 10           # (the writer looks every 500 ms; a written file comes within a second)
RENDERERS = ("longshot_vk.so", "longshot_vk.dll")   # (beside the game's executable: host/rx_backend.c load_vulkan)
VK_PROFILE = os.path.join("assets", "profiles", "longshot_vk.profdata.xz")
VK_PROFILE_WORK = os.path.join("work", "longshot_vk.profdata")   # (what nx2/make_game.py builds with, before the committed one)


def drop(path):
    """path deleted if it is there (the renderer's writer may delete it at the same time)"""
    try:
        os.remove(path)
    except FileNotFoundError:
        pass


def counting_renderer(exe_dir):
    """The renderer beside the game's executable is an RX_VK_PGO=gen one (it reads RX_VK_PGO_FILE): its path, else None"""
    for name in RENDERERS:
        path = os.path.join(exe_dir, name)
        if not os.path.isfile(path):
            continue
        tail = b""
        with open(path, "rb") as f:
            while True:
                chunk = f.read(1 << 20)
                if not chunk:
                    break
                if PGO_MARK in tail + chunk:
                    return path
                tail = chunk[-len(PGO_MARK):]
    return None


class Profile:
    """The renderer's counters in one game: file is RX_VK_PGO_FILE's, out the --vk-profile folder, name the run's"""
    def __init__(self, file, out, name, wait_s=PGO_WAIT_S, sleep=time.sleep, clock=time.monotonic):
        self.file, self.out, self.name, self.wait_s, self.sleep, self.clock = file, out, name, wait_s, sleep, clock
        self.was_reset = False
        for stale in (file, file + ".reset", file + ".dump"):   # (an earlier run's: not this one's counters)
            drop(stale)

    def _until(self, done, wait_s):
        end = self.clock() + wait_s
        while not done():
            if self.clock() > end:
                return False
            self.sleep(0.1)
        return True

    def reset(self):
        """The counters cleared (from here on only play counts): Failed when the renderer does not take the request, as one
        that does not count never does"""
        open(self.file + ".reset", "w").close()
        if not self._until(lambda: not os.path.exists(self.file + ".reset"), self.wait_s):
            drop(self.file + ".reset")
            raise Failed("--vk-profile: the renderer did not take %s.reset in %g s: not an RX_VK_PGO=gen renderer" % (
                self.file, self.wait_s))
        self.was_reset = True

    def dump(self, wait_s=None):
        """The counters written and moved to <out>/<name>.profraw: its path, None when the renderer wrote nothing"""
        drop(self.file)
        open(self.file + ".dump", "w").close()
        written = lambda: not os.path.exists(self.file + ".dump") and os.path.isfile(self.file) and os.path.getsize(self.file) > 0
        if not self._until(written, self.wait_s if wait_s is None else wait_s):
            drop(self.file + ".dump")
            return None
        os.makedirs(self.out, exist_ok=True)
        to = os.path.join(self.out, self.name + ".profraw")
        shutil.move(self.file, to)
        return to


def profile_end(profile, alive, hard=False):
    """A run's end with --vk-profile: the counters written if the game is there and they were cleared after control.
    ({file, reset, why}, the run's error or None): a renderer that writes nothing fails the run, never an empty profile"""
    if profile is None:
        return None, None
    if not profile.was_reset:   # (no control of Link yet: the counters hold the loading and the title, not play)
        return {"file": None, "reset": False, "why": "the counters were never cleared (no control of Link): not kept"}, None
    if not alive:
        return {"file": None, "reset": True, "why": "the game ended before its counters were written"}, None
    to = profile.dump(3 if hard else None)   # (a hard memory stop: a few seconds at most)
    if to is None:
        why = "--vk-profile: the renderer wrote no counters to %s in %g s: not an RX_VK_PGO=gen renderer" % (
            profile.file, 3 if hard else profile.wait_s)
        return {"file": None, "reset": True, "why": why}, why
    return {"file": to, "reset": True, "why": None}, None


def refuse_uncounted(game):
    """--vk-profile with a renderer that does not count: refused before anything is touched"""
    exe = longshot_bridge.game_exe(game)
    if not counting_renderer(os.path.dirname(exe)):
        raise SystemExit("pipeline_tour: --vk-profile: the renderer beside %s does not count (no %s in %s): make a counting "
                         "runtime (RX_VK_PGO=gen python -I scripts/dev.py packs pack <base> vk-count, then python -I "
                         "scripts/dev.py packs rt <base> <runtime> vk-count) and tour that" % (exe, PGO_ENV, " or ".join(RENDERERS)))


def raw_problem(path):
    """Why a --vk-profile file is not one llvm-profdata should merge (None: it is): LLVM's raw format, with counted functions"""
    with open(path, "rb") as f:
        head = f.read(32)
    if head[:8] != PGO_RAW_MAGIC:
        return "not an LLVM raw profile (host/pgo_rt.c's)"
    if len(head) < 32 or struct.unpack_from("<Q", head, 24)[0] == 0:
        return "no counted functions in it (a renderer built without RX_VK_PGO=gen counting)"
    return None


def profdata_tool():
    """The toolchain's llvm-profdata (the compiler that builds the renderer: third_party/a64-kit/tools/toolchain.py)"""
    sys.path.insert(0, os.path.join(ROOT, "third_party", "a64-kit", "tools"))
    import toolchain
    return toolchain.need(toolchain.find("llvm-profdata"))


def profile_note(d, raws, tool, run=subprocess.run, today=None):
    """The block for assets/profiles/README.md: when, where (GPU and driver, the build), the route's parts, the compiler"""
    games = []
    for raw in raws:
        try:
            with open(raw[:-len(".profraw")] + ".json", encoding="utf-8") as f:
                games.append(json.load(f))
        except (OSError, ValueError):
            games.append({})
    parts = []
    for m in games:
        if m.get("part") and m["part"] not in parts:
            parts.append(m["part"])
    gpus = sorted({m["renderer"] for m in games if m.get("renderer")})
    builds = sorted({m["version"] for m in games if m.get("version")})
    minutes = sum(m.get("seconds") or 0 for m in games) / 60
    failed = [m["name"] for m in games if m.get("name") and not m.get("completed")]
    try:
        version = run([tool, "--version"], capture_output=True, text=True).stdout
    except OSError:
        version = ""
    llvm = next((line.strip() for line in version.splitlines() if "LLVM version" in line), "llvm-profdata (version unknown)")
    cpu = ""
    try:
        with open("/proc/cpuinfo", encoding="utf-8", errors="replace") as f:
            cpu = next((line.split(":", 1)[1].strip() for line in f if line.startswith("model name")), "")
    except OSError:
        pass
    import platform
    return "\n".join([
        "- Recorded: %s, the pipeline tour (`scripts/pipeline_tour.py tour --vk-profile`), parts %s from a new save: %d games, "
        "%.0f minutes, an `RX_VK_PGO=gen` renderer on build %s; each game's counters cleared once it had control of Link and "
        "written at its end%s" % (today or time.strftime("%Y-%m-%d", time.gmtime()), ", ".join(parts) or "unknown", len(raws),
                                   minutes, ", ".join(builds) or "unknown",
                                   "" if not failed else " (not completed, their counters kept: %s)" % ", ".join(failed)),
        "- GPU and driver: %s, %s%s" % ("; ".join(gpus) or "unknown", platform.system() or "unknown", "; " + cpu if cpu else ""),
        "- Compiler: %s (the llvm-profdata that merged it)" % llvm]) + "\n"


def merge_profile(args, root=ROOT, run=subprocess.run):
    """The --vk-profile folder's counters merged by llvm-profdata into the renderer's play profile (work/longshot_vk.profdata,
    which nx2/make_game.py builds with); --publish also writes the committed one, assets/profiles/longshot_vk.profdata.xz (xz
    -9e), and the note for assets/profiles/README.md. Refused when there is nothing counted to merge"""
    d = os.path.abspath(args.dir)
    raws = sorted(os.path.join(d, n) for n in os.listdir(d) if n.endswith(".profraw")) if os.path.isdir(d) else []
    if not raws:
        raise SystemExit("pipeline_tour: merge-profile: no .profraw in %s (a run or tour with --vk-profile %s writes them)" % (d, d))
    for raw in raws:
        why = raw_problem(raw)
        if why:
            raise SystemExit("pipeline_tour: merge-profile: %s: %s" % (raw, why))
    tool = args.llvm_profdata or profdata_tool()
    out = os.path.abspath(args.out) if args.out else os.path.join(root, VK_PROFILE_WORK)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    if os.path.exists(out):
        os.remove(out)
    cmd = [tool, "merge", "-o", out] + raws
    print(" ".join(cmd), flush=True)
    if run(cmd).returncode or not os.path.isfile(out) or not os.path.getsize(out):
        raise SystemExit("pipeline_tour: merge-profile: llvm-profdata wrote no profile (%s)" % out)
    print("merge-profile: %d games' counters in %s (%d bytes)" % (len(raws), out, os.path.getsize(out)))
    if args.publish:
        import lzma
        with open(out, "rb") as f:
            data = f.read()
        shipped = os.path.join(root, VK_PROFILE)
        with open(shipped, "wb") as f:   # (as xz -9e: the preset 9, extreme)
            f.write(lzma.compress(data, format=lzma.FORMAT_XZ, preset=9 | lzma.PRESET_EXTREME))
        note = profile_note(d, raws, tool, run)
        with open(os.path.join(d, "README-note.md"), "w", encoding="utf-8") as f:
            f.write(note)
        print("merge-profile: %s written (%d bytes). Its note, for assets/profiles/README.md in place of the Recorded, GPU and "
              "driver, and Compiler lines (also in %s):\n%s" % (shipped, os.path.getsize(shipped),
                                                                 os.path.join(d, "README-note.md"), note), end="")
    return 0
