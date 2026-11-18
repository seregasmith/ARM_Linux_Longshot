"""The first start after a preparation leaves the warm out where the caches already answer the notes (gpu/vk/vk_draw.cpp
notes_answered_by_caches and precompile_run, gpu/vk/warm_check.h).

The preparation's own process leaves no marker, so the first start of the game warmed: it built every noted pipeline again
into the pipeline cache before the game. On Mesa's drivers the preparation's cache file already answers them, and the warm
only made the cache object hold every shader built through it for the session: 36,449 pipelines on a 15 GB AYN Odin 3
(Turnip), the save 30 s, and the system went down right after it. This cuts notes_answered_by_caches and the start of precompile_run out
of vk_draw.cpp and runs them on stand-ins for the caches (tests/warm_check_test.cpp):
 - every note and compute program of the sample answered (256 and 64, spread over them): no warm, the marker written, the
   start loads at create calls, every pipeline asked let go, and nothing asked in a way that compiles;
 - one of them not answered, a sample that could mostly not be asked, no notes, no pipelineCreationCacheControl: the warm
   runs as before;
 - RX_VK_WARM_CHECK=0: warmed as before, nothing asked; a --prepare-shaders run asks nothing and leaves no marker; a start
   with the marker asks nothing.
python -I -B tests/test_warm_check.py [--cxx g++]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

SCENARIOS = {
    "answered": {}, "few": {}, "cold_note": {}, "cold_compute": {}, "cold_last": {}, "other_variant": {}, "no_notes": {},
    "switch_off": {"RX_VK_WARM_CHECK": "0"}, "prepare_only": {"LONGSHOT_PREPARE_ONLY": "1"}, "marker": {},
    "no_cache_control": {},
}


def function(source, signature):
    start = source.index(signature)
    first = source.index("{", start)
    depth, at = 1, first + 1
    while depth:
        depth += {"{": 1, "}": -1}.get(source[at], 0)
        at += 1
    return source[start:at]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    args = parser.parse_args()
    cxx = shutil.which(args.cxx)
    if not cxx:
        raise SystemExit("C++ compiler not found: " + args.cxx)
    draw = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
    sig = "bool notes_answered_by_caches() {"
    answered = function(draw, sig) if sig in draw else ""   # (none on main: precompile_run then warms whatever the caches hold)
    run_sig = "void precompile_run(void (*progress)(size_t done, size_t total)) {"
    start = draw.index(run_sig)
    head = draw[start:draw.index("    const auto t0 = std::chrono::steady_clock::now();", start)]
    if "precompile_warm(progress);" not in head:
        raise SystemExit("FAIL: precompile_run's start no longer warms the notes: this test cuts the wrong part")
    problems = []
    with tempfile.TemporaryDirectory(prefix="longshot-warm-check-") as folder:
        tmp = Path(folder)
        (tmp / "notes_answered.inc").write_text(answered, encoding="utf-8")
        (tmp / "precompile_run_start.inc").write_text(head, encoding="utf-8")
        exe = tmp / "warm-check"
        r = subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-I", str(tmp),
                            str(ROOT / "tests" / "warm_check_test.cpp"), "-o", str(exe)],
                           capture_output=True, text=True, timeout=180)
        if r.returncode:
            raise SystemExit("FAIL: the cut does not compile with the stand-ins\n" + r.stdout + r.stderr)
        for name, extra in SCENARIOS.items():
            env = {k: v for k, v in os.environ.items() if k not in ("RX_VK_WARM_CHECK", "LONGSHOT_PREPARE_ONLY")}
            env.update(extra)
            out = subprocess.run([str(exe), name], cwd=tmp, env=env, capture_output=True, text=True, timeout=60)
            if out.returncode:
                problems.append("%s:\n    %s" % (name, "\n    ".join(
                    line for line in out.stdout.splitlines() if line.startswith(("FAIL", "unknown")))))
    if problems:
        raise SystemExit("FAIL:\n  " + "\n  ".join(problems))
    print("PASS: the warm is left out only where the caches answer the whole sample, which is asked without a compile")


if __name__ == "__main__":
    main()
