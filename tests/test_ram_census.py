# SPDX-License-Identifier: GPL-3.0-or-later
"""Where the RAM goes (RX_VK_LOG_RAM: gpu/vk/ram_census.h, gpu/vk/vk_memory.cpp ram_census_*).

On the AYN Odin 3 (15 GB, zram swap) Kokiri Forest left 340-540 MB available with 6 GB in swap, and the renderer's own lines
leave out the game's memory as against the rest of the process, what is in swap, what zram's compressed pages take, and the
other programs. With the switch on, three lines every 30 s say it. Checked here without a GPU or game:
- ram_census.h reads the kernel's text as it is written: /proc/meminfo, zram's mm_stat, a process's status (a kernel thread
  left out), /proc/self/smaps split at the guest window (the device's memory placed there and the driver's device files
  apart), and the DRM driver's memory in fdinfo (a client opened twice counted once; the engine's time and clock left out);
  the other programs grouped by name, the most first;
- vk_memory.cpp starts the thread from memory_init only with the switch, at the lowest priority; reads smaps (tens of seconds
  on the Odin 3) only with RX_VK_LOG_RAM=2, at the guest window's own bounds; takes placed_mu only for the placements' two
  numbers; and every 2 s reads the driver's own count alone, with the pipelines made.

python -I -B tests/test_ram_census.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def body(src, head):
    """The function starting with head, to its closing brace at the start of a line"""
    start = src.index(head)
    return src[start:src.index("\n}\n", start)]


def check_source():
    mem = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    for needle in ('#include "ram_census.h"', "void ram_census_log() {", "void ram_census_start() {"):
        if needle not in mem:
            raise SystemExit("vk_memory.cpp no longer has: " + needle)
    init = body(mem, "bool memory_init() {")
    if "ram_census_start();" not in init:
        raise SystemExit("memory_init no longer starts the RAM census")
    start = body(mem, "void ram_census_start() {")
    for needle in ('if (!env_on("RX_VK_LOG_RAM")) return;', "setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);", ").detach();"):
        if needle not in start:
            raise SystemExit("ram_census_start no longer has: " + needle)
    log = body(mem, "void ram_census_log() {")
    for needle in ("const uint64_t lo = (uint64_t)(uintptr_t)host->guest_base(), hi = lo + host->guest_size();",
                   'ram_census::maps(ram_slurp("/proc/self/smaps").c_str(), lo, hi)',
                   "std::lock_guard lk(placed_mu);\n        placed = placed_bytes, later = placed_later.bytes();"):
        if needle not in log:
            raise SystemExit("ram_census_log no longer has: " + needle)
    if log.count("placed_mu") != 1:
        raise SystemExit("ram_census_log takes placed_mu for more than the placements' two numbers")
    # (smaps walks every mapping: 20 to 36 s a read on the Odin 3, only with RX_VK_LOG_RAM=2)
    if "const ram_census::Maps m = split ? ram_census::maps(" not in log or "const bool split = ram_mode == 2;" not in log:
        raise SystemExit("ram_census_log reads smaps without RX_VK_LOG_RAM=2")
    drv = body(mem, "void ram_driver_log() {")
    for needle in ("const ram_census::Drm d = ram_drm();", "draw_stats(&draws, &skipped, &dispatches, &pipes);",
                   "const uint64_t outside = d.total - std::min(d.total, heap_used);"):
        if needle not in drv:
            raise SystemExit("ram_driver_log no longer has: " + needle)
    if "smaps" in drv or "placed_mu" in drv:
        raise SystemExit("ram_driver_log reads more than the driver's own count (every 2 s)")
    if "ram_driver_log();" not in start or "if (t % kRamEvery == 0) ram_census_log();" not in start:
        raise SystemExit("ram_census_start no longer logs the driver every tick and the census every kRamEvery seconds")
    if 'SW(RX_VK_LOG_RAM, diag, enum,' not in (ROOT / "gpu/vk/switches.def").read_text(encoding="utf-8"):
        raise SystemExit("switches.def has no RX_VK_LOG_RAM row")
    if "| `RX_VK_LOG_RAM=1` or `=2` |" not in (ROOT / "docs/dev/reference/profiling.md").read_text(encoding="utf-8"):
        raise SystemExit("docs/dev/reference/profiling.md has no RX_VK_LOG_RAM row")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="ram-census-") as tmp:
        exe = Path(tmp) / "ram-census"
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests/ram_census_test.cpp"),
                        "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=60)


if __name__ == "__main__":
    main()
