"""RX_VK_RECORDER_PARTS, the hand-off by part (gpu/vk/vk_recorder.h post_part, boundary_parts, queue_lock_event; vk_internal.h
QueueMutex; docs/dev/reference/threads.md, The hand-off by part), against a fake Vulkan: tests/recorder_parts_test.cpp built with the
real vk_work.h, vk_recorder.h and vk_internal.h's QueueMutex.

  - handoff: two parts posted while the recorder is held in the first's packets; the producer waits neither at a part's end
    nor at replay_mu's release; the driver sees each part's calls, then its end and submit, made on the recorder thread, in
    order; a wait for the second part comes back only once it is submitted
  - settle: a fence wait on a thread without replay_mu reaches the driver only after the part handed over before it is submitted
  - queue: a thread about to take the queue's lock waits for the parts handed over; one holding it while the recorder waits for
    it at a part's submit does not wait for the recorder, and the part goes on once the lock is let go
  - slot: a slot whose part is submitted resets its pool with another packet in flight (OwnSlot, RX_VK_RECORDER_OWN unset)
  - timeout: a wait for a part with a deadline comes back unmet in time, and owes the next part's submit a flush of what is held
    (vk_replay.cpp part_submit: a waiter's flush that came before its part was held, RX_VK_BATCH), once; a poll owes none
  - target: the flush owed is sent by the part the waiter waited for, not by the first part submitted after it gave up

Then the controls: each with one piece of the hand-off broken must fail its case (the watchdog or a check).

    python -I -B tests/test_recorder_parts.py --headers <Vulkan-Headers/include> [--cxx clang++]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_recorder_sync as recsync  # noqa: E402  (its test_loader() and the paths)
from srccut import between  # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = recsync.ROOT
VK = recsync.VK
CASES = ("handoff", "settle", "queue", "slot", "timeout", "target")


def queue_mutex(internal):
    """vk_internal.h's queue_lock_hook and QueueMutex"""
    return between(internal, "inline void (*queue_lock_hook)(int phase, bool settles) = nullptr;", "\nstruct Device {") + "\n"


def build(cxx, headers, out, recorder_text, name):
    d = out / name
    d.mkdir()
    (d / "vk_loader.h").write_text(recsync.test_loader(), encoding="utf-8")
    work = (VK / "vk_work.h").read_text(encoding="utf-8")
    (d / "vk_work.h").write_text(work, encoding="utf-8")
    (d / "vk_recorder.h").write_text(recorder_text, encoding="utf-8")
    for h in ("gpu_wait_stats.h", "vk_new.h"):
        shutil.copyfile(VK / h, d / h)
    (d / "queue_mutex.inc").write_text(queue_mutex((VK / "vk_internal.h").read_text(encoding="utf-8")), encoding="utf-8")
    exe = d / "recorder_parts_test.exe"
    cmd = [cxx, "-std=c++20", "-include", recsync.ENV_ON_H, "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
           "-Wno-missing-field-initializers", "-DVK_NO_PROTOTYPES", "-D_CRT_SECURE_NO_WARNINGS",
           "-I" + str(d), "-I" + str(headers), str(ROOT / "tests" / "recorder_parts_test.cpp"), "-o", str(exe)]
    if os.name != "nt":
        cmd.append("-pthread")
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        raise SystemExit("%s: the build failed:\n%s" % (name, r.stdout))
    return exe


def run(exe, case):
    env = {k: v for k, v in os.environ.items() if not k.startswith("RX_VK_RECORDER")}
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    try:
        r = subprocess.run([str(exe), case], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env, timeout=60,
                           creationflags=flags)
        return r.returncode, r.stdout
    except subprocess.TimeoutExpired as e:
        out = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        return None, out + "\nTIMEOUT"


def broken(text, old, new, what):
    if text.count(old) != 1:
        raise SystemExit("vk_recorder.h: the control %r's anchor must appear once: %r" % (what, old))
    return text.replace(old, new)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--headers", type=Path, required=True)
    args = parser.parse_args()
    cxx = shutil.which(args.cxx)
    if not cxx:
        raise SystemExit("C++ compiler not found: " + args.cxx)
    recorder = (VK / "vk_recorder.h").read_text(encoding="utf-8")
    # (each control breaks one piece of the hand-off; the case it names must fail)
    controls = (
        ("the producer drains at a part's end (the part's packet run on the producer after a wait)", "handoff",
         broken(recorder, "    if (!parts_wanted() || t_queue_held || t_recorder) return 0;",
                "    if (Recorder* g = t_prod) g->wait_idle(B_vkEndCommandBuffer);\n    if (!parts_wanted() || t_queue_held || t_recorder) return 0;",
                "drain")),
        ("a fence wait made without the parts submitted", "settle",
         broken(recorder, "        if (!t_settled) g_rec.settle_parts(k, kSettleNs);\n", "", "settle")),
        ("the queue's lock taken without the parts submitted", "queue",
         broken(recorder, "    if (settles && !t_recorder && !t_queue_held && !t_settled && parts_wanted()) g_rec.settle_parts(B_QUEUE_LOCK);\n", "",
                "queue settle")),
        ("a wait under the queue's lock not given up (the deadlock)", "queue",
         broken(recorder, "        return t_present_held || chain_waiters.load(std::memory_order_seq_cst) > 0;   // (the recorder waits for this thread)",
                "        return false;", "escape")),
        ("a wait for the parts that ends unmet owes no flush", "timeout",
         broken(recorder, "                while (want > was && !flush_owed_upto.compare_exchange_weak(was, want, std::memory_order_acq_rel)) {}\n",
                "                (void)want, (void)was;\n", "owed")),
        ("a poll owes a flush", "timeout",
         broken(recorder, "            if (timeout_ns) {\n                const uint64_t want", "            {\n                const uint64_t want", "poll")),
        ("the flush owed sent by the first part after it, not the waiter's", "target",
         broken(recorder, "    if (cur < upto) return false;\n", "    (void)cur;\n", "target")),
        ("a slot's reset waits for the recorder (slot_begin without the hand-off by part)", "slot",
         broken(recorder, "if ((!g_own && !g_parts.load(std::memory_order_relaxed)) ||", "if (!g_own ||", "slot")),
    )
    bad = []
    with tempfile.TemporaryDirectory(prefix="recorder-parts-") as tmp:
        out = Path(tmp)
        exe = build(cxx, args.headers, out, recorder, "real")
        for case in CASES:
            code, text = run(exe, case)
            print("---- %s\n%s" % (case, text.rstrip()))
            if code != 0 or ("PASS " + case) not in text:
                bad.append("%s failed" % case)
        for i, (what, case, text) in enumerate(controls):
            code, out_text = run(build(cxx, args.headers, out, text, "control%d" % i), case)
            if code == 0:
                bad.append("the control with %s passed %s" % (what, case))
            else:
                print("PASS control: %s fails %s" % (what, case))
    if bad:
        raise SystemExit("FAIL:\n  " + "\n  ".join(bad))
    print("PASS: the recorder takes whole parts and submits them; what needs a part submitted waits for that, and the queue's "
          "lock neither runs ahead of the parts nor deadlocks with the recorder")


if __name__ == "__main__":
    main()
