"""Link's pose for the performance summary's one-second samples (host/hos_main.c), run without a game.

The opt-in performance statistics take Link's place and the gameplay camera's yaw and pitch once a second. The host reads
them from Link's update (oot_perf_pose) and the renderer asks for them with the map (hos_perf_info). The first version read
the angles from fields of the camera's view that stay 0.0 (every sample landed in the first bin), sent them in the wrong
range for the relay (which refuses a whole summary for one sample out of range), kept sampling through a cutscene and
gave a title-screen second a place. This test runs the real code:

  - oot_perf_pose, hos_perf_info, oot_event_playing, oot_gameplay_view and oot_inventory_refresh (the map line, and the
    title) are cut out of host/hos_main.c by their text, and compiled with tests/perf_pose_test.c, which makes the rest of
    the guest up: the camera manager's chain to the gameplay view, its eye and target, the event manager, Link's place,
    the clock, the locks and what the save says (nothing is taken from a dump);
  - yaw and pitch (radians) follow the camera as the control bridge's camera does (tests also run that arithmetic, and this
    test checks it is the one host/oot_state_tap.c has), over a whole turn at three heights, within the relay's range
    for every direction (|yaw| <= pi, |pitch| <= pi/2, straight behind included); straight up has none;
  - an event playing, another kind of camera object, a stray place, an unticked game: no pose; a pose not set for a
    second: none; the map line changing: none until the next update; no map (the title): none, even just after a
    Link update.

python -I -B tests/test_perf_pose.py [--cc gcc] [--source-root OTHER_CHECKOUT]

--source-root reads the production files from another checkout (main's, for the negative run).
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]
RUN_ALL_SKIP = {"windows": "the locks and Interlocked calls are made up for POSIX here: the host's code is the same, checked on Linux"}

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import between, block   # noqa: E402


def cut(source, tap):
    """The host's pieces, in the order the source has them, as one header; and the check the bridge's camera is the one run"""
    state_camera = block(tap, "static int state_camera(double *yaw) {")
    if ("const double x = at[0] - eye[0], z = at[2] - eye[2];" not in state_camera
            or "*yaw = atan2(x, z) * 57.29577951308232;" not in state_camera):
        raise SystemExit("FAIL: the bridge's camera (oot_state_tap.c state_camera) is not the arithmetic tests/perf_pose_test.c runs "
                         "beside the pose: update bridge_camera there")
    parts = [
        between(source, "static SRWLOCK map_lock = SRWLOCK_INIT;", "void hos_movie_stats(int64_t out[5]);"),
        block(source, "const char *hos_perf_info("),
        block(source, "void hos_perf_tracking(int enabled) {"),
        between(source, "#define OOT_EVENTMGR_SLOT", "\n"),
        block(source, "static uint64_t oot_singleton(Ctx *c, uint64_t slot) {"),
        block(source, "static int oot_event_playing(Ctx *c) {"),
        between(source, "#define OOT_ITEM_NAME(label, name, kind) name,", "static int oot_item_count(Ctx *c, const char *name);"),
        block(source, "static void oot_inventory_refresh(Ctx *c, int64_t now) {"),
        between(source, "static uint64_t oot_gameplay_view(Ctx *c) {", "static _Thread_local uint64_t flight_transform_controller;"),
    ]
    return "\n".join(parts) + "\n"


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cc", default="gcc")
    p.add_argument("--source-root", type=Path, default=ROOT)
    a = p.parse_args()
    source = (a.source_root / "host" / "hos_main.c").read_text(encoding="utf-8")
    tap = (a.source_root / "host" / "oot_state_tap.c").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory() as tmp:
        Path(tmp, "perf_pose_cut.h").write_text(cut(source, tap), encoding="utf-8")
        exe = Path(tmp, "perf_pose_test")
        built = subprocess.run([a.cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-function", "-Wno-unused-const-variable",
                                "-I", tmp, "-I", str(a.source_root / "gpu" / "vk"), str(ROOT / "tests" / "perf_pose_test.c"), "-lm", "-o", str(exe)],
                               capture_output=True, text=True)
        if built.returncode:
            print(built.stdout + built.stderr)
            raise SystemExit("FAIL: tests/perf_pose_test.c does not compile with the host's pose code")
        if built.stderr:
            print(built.stderr)
        ran = subprocess.run([str(exe)], capture_output=True, text=True)
    print(ran.stdout.strip())
    if ran.stderr:
        print(ran.stderr.strip())
    if ran.returncode:
        raise SystemExit("FAIL: the pose test failed (the FAIL lines above)")
    print("PASS: Link's pose follows the camera in the relay's range, and is none in a cutscene, at the title and while stale")


if __name__ == "__main__":
    main()
