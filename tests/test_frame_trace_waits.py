"""The frame trace (RX_VK_FRAME_TRACE, longshot_frames.csv) names the waits it could not show (no game/GPU required):
its header ends with the Breakdown parts, the main thread's time, shown_ms (present-wait's presentation clock) and at_ns,
a line has as many values as the header has names, the columns before keep their order (scripts read them), and the
counters are fed where the time is spent.

python -I -B tests/test_frame_trace_waits.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

BEFORE = ("frame,ms,pipe_waits,pipe_wait_ms,left_out,translate_ms,uploads,upload_mb,textures,memsync_ms,pipelines_ms,"
          "uploads_ms,programs_ms,slot_wait_ms,gpu_behind_ms,draw_ms,desc_ms,cmds_ms,tex_lookup_ms,textures_gone")
NEW = ["late_wait_ms", "readback_wait_ms", "tex_frame_ms",
       # (the Breakdown's parts, and the game's main thread: on the CPU, blocked, waiting for a virtual core)
       "game_ms", "render_ms", "gpu_wait_ms", "pace_ms", "wait_render_ms", "wait_gpu_ms", "wait_main_ms",
       "main_cpu_ms", "main_blocked_ms", "main_vcore_ms",
       # (the present on the host's clock: HOS_PROFILE=1's samples, presents.csv and events.txt share it)
       "shown_ms", "shown_at_ns",
       "at_ns",
       # (longshot: the frame's side in an A/B within the run, HOS_AB, host/hos_ab.c; scripts/perf.py ab)
       "ab_side", "create_ms", "reuse_ms", "astc_stage_ms", "astc_record_ms", "astc_view_ms", "astc_flush_ms",
       "astc_flushes", "create_n", "pool_reused", "pool_new", "pool_put", "pool_idle", "pool_destroyed", "pool_holds",
       "dead_waiting", "compact_pass", "pool_resized"]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    # the header: the string pieces of the fprintf that writes it, joined
    at = replay.index('"frame,ms,pipe_waits')
    header = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', replay[at:replay.index(");", at)])).replace("\\n", "")
    names = header.split(",")
    if header != BEFORE + "," + ",".join(NEW):
        raise SystemExit("vk_replay.cpp: the frame trace's header is not the old columns followed by " + ", ".join(NEW) + ": " + header)
    # a line: one conversion a column
    line_at = replay.index('fprintf(trace, "%llu,')
    # (the format's string pieces, joined: it may run over several lines)
    fmt_src = replay[line_at + len("fprintf(trace, "):]
    fmt_src = fmt_src[:re.match(r'(\s*"(?:[^"\\]|\\.)*")+', fmt_src).end()]
    fmt = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', fmt_src)).replace("\\n", "")
    if len(fmt.split(",")) != len(names):
        raise SystemExit("vk_replay.cpp: a frame trace line has %d values for %d columns" % (len(fmt.split(",")), len(names)))
    for needle in ("memory_late_wait_ns()", "texture_readback_wait_ns()", "texture_frame_ns()", "(late - l_late) / 1e6", "(rb - l_rb) / 1e6",
                   "(tf - l_tf) / 1e6", "l_gone = gone, l_late = late, l_rb = rb, l_tf = tf;",
                   "shown_wait_us.store(", "shown_wait_us.exchange(-1)", "shown_at_ns.store(", "shown_at_ns.exchange(0)",
                   "perf_measure.load(std::memory_order_relaxed) || frame_trace_on"):
        if needle not in replay:
            raise SystemExit("vk_replay.cpp: the frame trace no longer has: " + needle)
    mem = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    take = block(mem, "bool take_capture(std::unique_ptr<Capture> c, uint64_t value, uint64_t completed)")
    # (the counter is fed by the wait itself, after it)
    if not take.index("vk_timeline_wait(wait);") < take.index("st_late_wait_ns.fetch_add("):
        raise SystemExit("vk_memory.cpp: take_capture() does not time its wait for the GPU into st_late_wait_ns")
    tex = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    if "const TraceTimer timed{st_readback_wait_ns};" not in block(tex, "bool readback_submit_wait()"):
        raise SystemExit("vk_texture.cpp: readback_submit_wait() is not timed into st_readback_wait_ns")
    frame = block(tex, "void texture_frame(uint64_t completed)")
    if frame.index("const TraceTimer timed{st_texture_frame_ns};") > frame.index("upload_frame_report();"):
        raise SystemExit("vk_texture.cpp: texture_frame() is not timed from its start")
    for path, needle in (("gpu/vk/vk_internal.h", "int64_t memory_late_wait_ns();"), ("gpu/vk/vk_texture.h", "int64_t texture_readback_wait_ns();"),
                         ("gpu/vk/vk_texture.h", "int64_t texture_frame_ns();")):
        if needle not in (ROOT / path).read_text(encoding="utf-8"):
            raise SystemExit(path + " no longer declares: " + needle)
    print("frame trace waits: ok")


if __name__ == "__main__":
    main()
