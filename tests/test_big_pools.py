# SPDX-License-Identifier: GPL-3.0-or-later
"""The big placed pools measured (RX_VK_LOG_BIG_POOLS: gpu/vk/big_pools.h, gpu/vk/vk_memory.cpp big_*).

On the AYN Odin 3 (Turnip, no host pointers, no udmabuf) the guest's pools are placed in the device's own memory, and each
placement seems to cost its whole size in RAM: 5.5 GB of them in Kokiri Forest, 2.4 GB of it one pool placed at boot with
8 KB copied in. With the switch on, the log says for each placement of 64 MB or more what MemAvailable did as it was made,
what the GPU reaches of it and which parts hold data, to tell whether such a pool can be placed in part, or later. Checked
here without a GPU or game:
- big_pools.h: a pool's stretches (1 MB, larger past 4 GB), the marks of what the GPU reached (clipped to the pool, no
  wrap-around), the 64 KB blocks holding data (read in parts at the pool's offsets), the line's map and the pages mapped in;
- vk_memory.cpp notes the GPU's reach where a pool's buffer is handed out (gpu_buffer_bind, gpu_buffer), takes MemAvailable
  around a big placement and follows it (placed_back), lets it go where a placement goes (placed_back, placed_unback),
  starts the scanner from placed_setup only with the switch, and reads the placements under placed_mu.

python -I -B tests/test_big_pools.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def need(where, text, needles):
    for needle in needles:
        if needle not in text:
            raise SystemExit(where + " no longer has: " + needle)


def check_source():
    mem = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    need("vk_memory.cpp", mem, ['#include "big_pools.h"', "void big_note(uint64_t, uint64_t, int) {}"])
    if mem.index("void big_note(uint64_t guest, uint64_t size, int kind)") > mem.index("struct Placement {"):
        raise SystemExit("vk_memory.cpp: the big placements' code is in the placements' block, which tests/test_placed_import_memory.py cuts out with stand-ins for it")
    need("vk_memory.cpp: gpu_buffer_bind()", block(mem, "BufferRef gpu_buffer_bind(uint64_t gpu, uint64_t size, bool written)"),
         ["big_note(guest, size, written ? 1 : 0);"])
    need("vk_memory.cpp: gpu_buffer()", block(mem, "BufferRef gpu_buffer(uint64_t gpu, uint64_t size)"),
         ["big_note(guest, size, 2);"])
    back = block(mem, "void placed_back(uint64_t lo, uint64_t hi)")
    need("vk_memory.cpp: placed_back()", back,
         ["const bool big = n >= kBigMin && big_active.load(std::memory_order_relaxed);",
          "if (big) avail_mb[1] = big_avail_mb();",
          'big_drop(a, o->second.serial, "taken into a larger placement");',
          "if (big) avail_mb[2] = big_avail_mb(), big_take(plan.range.lo, plan.range.hi, placed_serial, avail_mb);"])
    if back.index("avail_mb[1] = big_avail_mb()") > back.index("placed::copy_in("):
        raise SystemExit("vk_memory.cpp: placed_back() takes MemAvailable once allocated after the copy, not before it")
    need("vk_memory.cpp: placed_unback()", block(mem, "void placed_unback(uint64_t lo, uint64_t hi)"),
         ['big_drop(it->first, it->second.serial, "given back to ordinary memory");'])
    need("vk_memory.cpp: placed_setup()", block(mem, "bool placed_setup(bool forced)"), ["big_start();"])
    need("vk_memory.cpp: big_start()", block(mem, "void big_start() {"),
         ['if (!env_on("RX_VK_LOG_BIG_POOLS")) return;', "std::atexit(big_stop);", ").detach();"])
    scan = block(mem, "void big_scan() {")
    need("vk_memory.cpp: big_scan()", scan,
         ["const auto it = placements.find(sn.lo);",
          "gone = big_quit.load() || it == placements.end() || it->second.serial != sn.serial;",
          "big_pools::resident_bytes(host->guest_base() + sn.lo, n, placed_page);"])
    reads = scan.index("blocks += big_pools::data_blocks(")
    if scan.rfind("std::lock_guard lk(placed_mu);", 0, reads) < scan.rfind("for (uint64_t off = 0;", 0, reads):
        raise SystemExit("vk_memory.cpp: big_scan() reads a placement outside placed_mu")
    need("vk_memory.cpp: big_stop()", block(mem, "void big_stop() {"), ["std::lock_guard lk(placed_mu);", "big_quit = true;"])
    need("switches.def", (ROOT / "gpu/vk/switches.def").read_text(encoding="utf-8"), ["SW(RX_VK_LOG_BIG_POOLS, diag, bool,"])
    need("docs/development.md", (ROOT / "docs/development.md").read_text(encoding="utf-8"), ["| `RX_VK_LOG_BIG_POOLS=1` |"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="big-pools-") as tmp:
        exe = Path(tmp) / "big-pools"
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests/big_pools_test.cpp"),
                        "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=60)


if __name__ == "__main__":
    main()
