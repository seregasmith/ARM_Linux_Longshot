"""Execute production stream allocation/reset and serial/segment submission against host caches."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block, between   # noqa: E402

# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/longshot_env.h)
ENV_ON_H = str(Path(__file__).resolve().parents[1] / "gpu" / "vk" / "compat" / "longshot_env.h")

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]


def sim_turf_cut(draw):
    """The adaptive simulated turf's per-recording demand readback (#2093), as RecordingRes and draw_begin_recording use it:
    the sample, the readback's size, the recording's ready flag and RX_VK_SIM_TURF_ADAPTIVE (on, as by default)"""
    return "\n".join([block(draw, "struct SimTurfSample {") + ";",
                      "constexpr size_t " + between(draw, "kSimTurfSamplesPerRecording =", ";") + ";",
                      between(draw, "thread_local bool sim_turf_monitor_ready", ";") + ";",
                      block(draw, "bool sim_turf_adaptive() {")])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=ROOT)
    p.add_argument("--headers", type=Path, required=True)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--cache", choices=["all", "coherent", "noncoherent"], default="all")
    p.add_argument("--case", default="all")
    a = p.parse_args()
    draw = (a.source / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    replay = (a.source / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    fixture = (ROOT / "tests/draw_stream_visibility_test.cpp").read_text(encoding="utf-8")
    fixture = fixture.replace("/* RES */", sim_turf_cut(draw) + "\n" + block(draw, "struct RecordingRes {") + ";")
    actual = "\n".join(block(draw, marker) for marker in ["bool stream_insert(", "bool stream_next(", "bool stream_reserve(", "uint64_t stream_alloc("])
    actual += "\n" + block(draw, "BufferRef stream_copy(")
    fixture = fixture.replace("/* STREAM */", actual)
    flush = block(draw, "void draw_flush_recording(") if "void draw_flush_recording(" in draw else ""
    fixture = fixture.replace("/* FLUSH */", flush).replace("/* BEGIN */", block(draw, "void draw_begin_recording("))
    fixture = fixture.replace("/* SUBMIT */", block(replay, "void submit_recording("))
    # (the parallel replay's segment submits went with its workers, #1763: one render thread records every command buffer)
    cases = ["single", "grow", "reuse", "empty", "batch", "allocation-failure", "first-stream-failure"]
    if a.case != "all":
        assert a.case in cases
        cases = [a.case]
    with tempfile.TemporaryDirectory(prefix="draw-stream-visibility-") as temp:
        cpp, exe = Path(temp) / "fixture.cpp", Path(temp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-include", ENV_ON_H, "-include", str(ROOT / "gpu" / "vk" / "flat_map.h"), "-O1", "-pthread", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                        "-I" + str(a.headers), str(cpp), "-o", str(exe)], check=True)
        for cache in (["coherent", "noncoherent"] if a.cache == "all" else [a.cache]):
            for case in cases:
                subprocess.run([str(exe), case, cache], check=True, timeout=20)


if __name__ == "__main__":
    main()
