"""Under Linux memory pressure the textures' budget on an integrated GPU keeps what the frames drew in the last seconds
(RX_VK_PRESSURE_KEEP_S, gpu/vk/linux_pressure.h, vk_texture.cpp texture_frame).

On the AYN Odin 3 (2026-11-20, Turnip, 15 GB of RAM and 15 GB of zram) memory was short from the title screen on: the hold
took the title's 47 MB of textures as its baseline and held the budget at its 512 MB floor for the whole session. Kokiri
Forest draws 1.1-1.3 GB of textures every few frames, so every pass over the budget let go of what had not been drawn for
60 frames (3 s at 20 fps) and the game drew it again: 16,177 textures let go, 2-15 made again every frame, freezes of 1.2 to
3.7 s, and the cache at about 1.4 GB all the same.

Cuts texture_frame's hold block out of vk_texture.cpp and runs it over such a session (tests/pressure_keep_test.cpp, its
/proc/meminfo and texture table stood in, the allocations' padding counted as total_bytes counts it): with swap to spare,
the forest's budget is at least what the last 20 s drew, padding and all, and under the 45 s old textures', and the eviction's
minimum age (its need_unused line, cut out too) is the window's presents, so the budget's 7/8 target cannot let go of what
the window drew; the keep holds when the hold enters in the forest, between the reserve and its clearing line, and while the
hold steps back up. With swap nearly gone, hovering at the reserve (once short, short until it clears by 256 MB), with less
room than the keep, with none, with RX_VK_COMMIT_RESERVE_MB=0 (the keep's own emergency stays), with RX_VK_PRESSURE_KEEP_S=0
and on a discrete GPU it is as before; it is never over the normal budget; and the hold lets go when memory is back. No game,
no GPU.

    python -I tests/test_pressure_keep.py [--cxx g++]
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import between   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
# (env_on, the switches' rule, before the code cut out of the sources: gpu/vk/compat/longshot_env.h)
ENV_ON_H = str(ROOT / "gpu" / "vk" / "compat" / "longshot_env.h")
START = "static linux_pressure::PressureHold hold;"
END = "    // (longshot) the 90% rule and the driver's limit let go of their excess"
READ = "linux_pressure::read_meminfo("
NEED = "const uint64_t need_unused ="   # (the eviction's minimum age: the keep window raises it)
CASES = ["forest", "band", "entry", "step-up", "swap-gone", "swap-hover", "swap-no-room", "no-swap", "cap", "recover", "discrete"]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=ROOT)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    src = (a.source / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    hold = between(src, START, END)
    if hold.count(READ) != 1:
        raise SystemExit("vk_texture.cpp: the hold no longer reads /proc/meminfo once (" + READ + ")")
    hold = hold.replace(READ, "fake_meminfo(")   # (what the stand-in /proc/meminfo says)
    need = between(src, NEED, ";") + ";"
    fixture = (ROOT / "tests/pressure_keep_test.cpp").read_text(encoding="utf-8")
    fixture = fixture.replace("/* ACTUAL */", hold).replace("/* NEED_UNUSED */", need)
    env = {k: v for k, v in os.environ.items() if k not in ("RX_VK_PRESSURE_KEEP_S", "RX_VK_COMMIT_RESERVE_MB")}
    failed = []
    with tempfile.TemporaryDirectory(prefix="pressure-keep-") as temp:
        cpp, exe = Path(temp) / "fixture.cpp", Path(temp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-include", ENV_ON_H, "-O1", "-Wall", "-Wextra", "-Werror",
                        "-Wno-missing-field-initializers", "-I" + str(a.source / "gpu/vk"), str(cpp), "-o", str(exe)], check=True)
        runs = [(case, env) for case in CASES] + [("off", dict(env, RX_VK_PRESSURE_KEEP_S="0")),
                                                  ("commit-off", dict(env, RX_VK_COMMIT_RESERVE_MB="0"))]
        for case, case_env in runs:
            r = subprocess.run([str(exe), case], env=case_env, capture_output=True, text=True, timeout=60)
            sys.stdout.write(r.stdout)
            if r.returncode != 0:
                sys.stderr.write(r.stderr)
                failed.append(case)
    if failed:
        raise SystemExit("the hold's budget went wrong in: " + ", ".join(failed))
    print(f"{len(CASES) + 2} sessions under the hold passed")


if __name__ == "__main__":
    main()
