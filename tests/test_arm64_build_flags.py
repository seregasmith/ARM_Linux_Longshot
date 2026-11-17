"""The game's compile flags on an ARM64 Linux PC (the arm64 port, docs/arm64-port-plan.md): no x86-64 option in them.
-mcx16 (cmpxchg16b, for the 128-bit compare-exchange CASP and STXP take) stopped the AppImage's build on an ARM64 handheld at
its first compile, "clang: error: unsupported option '-mcx16' for target 'aarch64-unknown-linux-gnu'". The lifted code's flags
(nx2/build.py flags) and the runtime's (main's host_fl) are cut from the source and run for an ARM64 PC, an x86-64 one and a
Windows build: -mcx16 only for the x86-64 ones, where it stays where it was (the object keys hash the flags). Game-free.

    python -I -B tests/test_arm64_build_flags.py
"""
import os
from pathlib import Path
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILD = (ROOT / "nx2" / "build.py").read_text(encoding="utf-8")


class Platform:
    def __init__(self, machine):
        self._machine = machine

    def machine(self):
        return self._machine


class Toolchain:
    def __init__(self, windows):
        self._windows = windows

    def windows(self):
        return self._windows


class Kit:
    KIT = "a64-kit"


def build_flags(machine, windows=False):
    """(the lifted code's flags, the runtime's) as nx2/build.py makes them on a PC of that machine"""
    start = BUILD.index('OPT = "O1"')
    end = BUILD.index("\n\n\n", BUILD.index("def flags(", start))
    names = {"os": os, "platform": Platform(machine), "toolchain": Toolchain(windows), "kit": Kit, "target": lambda: [],
             "CPU": ["-march=<cpu>"], "NX2_RT": "nx2/rt", "WRITE_TRACK": ["-DA64_WRITE_TRACK"], "LINUX": not windows,
             "POSIX": "host/posix", "HOST": "host", "rt64": "rt64", "gen": "gen"}
    exec(BUILD[start:end], names)
    host_start = BUILD.index("    host_fl = ")
    exec(textwrap.dedent(BUILD[host_start:BUILD.index("    host_roots = ", host_start)]), names)
    return names["flags"]("gen", "rt64", "O1", True), names["host_fl"]


class Cx16(unittest.TestCase):
    def test_arm64_linux_has_no_cx16(self):
        for machine in ("aarch64", "arm64"):
            lifted, host = build_flags(machine)
            self.assertNotIn("-mcx16", lifted, machine)
            self.assertNotIn("-mcx16", host, machine)
            self.assertNotIn("-mcx16", " ".join(lifted + host), machine)

    def test_x86_64_keeps_it_where_it_was(self):
        """(the same flags in the same order as before the port: an x86-64 player's objects keep their keys)"""
        for machine, windows in (("x86_64", False), ("AMD64", True), ("aarch64", True)):   # (a Windows build is x86-64)
            lifted, host = build_flags(machine, windows)
            at = lifted.index("-fno-strict-aliasing")
            self.assertEqual(lifted[at:at + 3], ["-fno-strict-aliasing", "-mcx16", "-O1"], (machine, windows))
            self.assertEqual(host[host.index("-g0"):host.index("-g0") + 3], ["-g0", "-mcx16", "-DA64_POLLING"],
                             (machine, windows))


if __name__ == "__main__":
    unittest.main()
