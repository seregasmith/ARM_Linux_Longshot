"""RX_VK_READBACK_LATE's rules (gpu/vk/readback_late.h; no game or GPU needed), and that vk_replay.cpp's invalidate asks them.

Unset: default_sizes(), the culling camera block's 0x3980 bytes, read without its wait on every PC, and on a handheld
(device_policy.h readback_late_newest) the newest submission's ranges too, every other readback waiting for its write-back;
=0 (or words that are no rule): no rule, every readback waits. `newest` names the ranges whose writer is the newest
submission handed out, a hex size the ranges of exactly that many bytes; either rule is enough. The rules as vk_replay.cpp's
readback_late_wanted reads the switch are run from its source, in a process each (unset, =0, =newest, =c0), on a handheld
and on any other PC. The by-range
log that finds such ranges has a switch of its own, RX_VK_LOG_READBACK_RANGES, so nothing is noted or logged without it.
The wait itself, with and without a rule, is run in tests/test_readback_part.py.

python -I -B tests/test_readback_late.py [--cxx g++]
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <cstdio>
#include "readback_late.h"
static int failures;
static void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}
int main() {
    using namespace readback_late;
    const Rules none = parse(nullptr), empty = parse("");
    check(!none.any() && !empty.any(), "not set, or set to nothing: no rule");
    check(!wanted(none, 0x3980, true) && !wanted(empty, 0x3980, true), "no rule: no readback is read late");
    const Rules newest = parse("newest");
    check(newest.any() && newest.newest && newest.sizes.empty(), "newest alone");
    check(wanted(newest, 64, true) && wanted(newest, 0x3980, true), "newest: a range the newest submission writes, of any size");
    check(!wanted(newest, 64, false) && !wanted(newest, 0x3980, false), "newest: not a range an older submission writes");
    const Rules size = parse("3980");
    check(size.any() && !size.newest && size.sizes.size() == 1 && size.sizes[0] == 0x3980, "a size is read as hex");
    check(wanted(size, 0x3980, false) && wanted(size, 0x3980, true), "a size: the ranges of that size, whoever wrote them");
    check(!wanted(size, 0x3981, true) && !wanted(size, 0x397f, true) && !wanted(size, 3980, true), "a size: no other size");
    const Rules both = parse("newest,3980,c0");
    check(both.newest && both.sizes.size() == 2 && both.sizes[1] == 0xc0, "rules are separated by commas");
    check(wanted(both, 0x40, true) && wanted(both, 0xc0, false) && !wanted(both, 0x40, false), "either rule is enough");
    const Rules zero = parse("0"), junk = parse("off,,x");
    check(!zero.any() && !junk.any(), "0 and words that are no rule name nothing");
    Rules unset;
    unset.sizes = default_sizes();
    check(unset.any() && !unset.newest && unset.sizes.size() == 1 && unset.sizes[0] == 0x3980,
          "the default: the culling camera block's size alone");
    check(wanted(unset, 0x3980, false) && !wanted(unset, 0x3981, true) && !wanted(unset, 0x8b040, true) && !wanted(unset, 64, true),
          "the default: that size read late, whoever wrote it; no other range, not even the newest submission's");
    if (failures) return 1;
    std::printf("PASS: RX_VK_READBACK_LATE's rules\n");
    return 0;
}
"""


# vk_replay.cpp's readback_late_wanted, cut from the source: the switch read as the renderer reads it, the default unset
WANTED = r"""
#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstdlib>
#include "readback_late.h"
static void logf(const char*, ...) {}
// (dev.policy's one field readback_late_wanted reads: device_policy.h sets it on a handheld, tests/test_device_policy.py
// checks that; here TEST_HANDHELD says it)
static struct { struct { bool readback_late_newest; } policy; } dev = {{std::getenv("TEST_HANDHELD") != nullptr}};
@@WANTED@@
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    std::printf("%d\n", readback_late_wanted(std::strtoull(argv[1], nullptr, 16), std::atoi(argv[2]) != 0) ? 1 : 0);
    return 0;
}
"""
# (RX_VK_READBACK_LATE, or None unset; a handheld; the size in hex, newest; read late)
WANTED_CASES = [
    (None, 0, "3980", 0, 1), (None, 0, "3980", 1, 1), (None, 0, "8b040", 1, 0), (None, 0, "40", 1, 0),
    ("", 0, "3980", 0, 1), ("", 0, "40", 1, 0),   # (set to nothing: as unset)
    # (a handheld, unset: the newest submission's ranges too, of any size; an older one's only the culling camera block)
    (None, 1, "3980", 0, 1), (None, 1, "8b040", 1, 1), (None, 1, "40", 1, 1), (None, 1, "40", 0, 0), ("", 1, "40", 1, 1),
    ("0", 0, "3980", 0, 0), ("0", 0, "3980", 1, 0), ("0", 1, "40", 1, 0),   # (no rule: every readback waits)
    ("newest", 0, "3980", 0, 0), ("newest", 0, "40", 1, 1),
    ("c0", 0, "c0", 0, 1), ("c0", 0, "3980", 1, 0), ("c0", 1, "40", 1, 0),   # (a rule set: no default added on a handheld)
]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    inv = replay[replay.index("void invalidate(uint64_t gpu, uint64_t size) {"):]
    marker = "bool readback_late_wanted(uint64_t size, bool newest) {"
    if replay.count(marker) != 1:
        raise SystemExit("vk_replay.cpp: readback_late_wanted not found once")
    start = replay.index(marker)
    wanted = replay[start:replay.index("\n}\n", start) + 3]
    for needle in ('readback_late::parse(e)', 'getenv("RX_VK_READBACK_LATE")', "return readback_late::wanted(rules, size, newest);",
                   "if (!e || !*e) {", "r.sizes = readback_late::default_sizes();", "r.newest = dev.policy.readback_late_newest;"):
        if needle not in wanted:
            raise SystemExit("vk_replay.cpp readback_late_wanted no longer has: " + needle)
    # (a range is read late only when its write-back is still pending and the rule names it; the wait is the other branch)
    if "const bool late = pending && readback_late_wanted(size, newest);" not in inv or "if (pending && !late) {" not in inv:
        raise SystemExit("invalidate no longer asks the late rule before its wait")
    # (the by-range notes take a lock at every readback: only with their switch)
    notes = inv[inv.index('static const bool log_ranges = env_on("RX_VK_LOG_READBACK_RANGES");'):]
    if not notes.startswith('static const bool log_ranges = env_on("RX_VK_LOG_READBACK_RANGES");\n            if (log_ranges) {\n'
                            '                rb_range_note(') or "rb_range_age(" not in notes[:notes.index("\n            }\n")]:
        raise SystemExit("the readbacks are noted by range without RX_VK_LOG_READBACK_RANGES")
    if "if (log_ranges) rb_ranges_log();" not in notes[:1200]:
        raise SystemExit("the by-range log is written without RX_VK_LOG_READBACK_RANGES")
    with tempfile.TemporaryDirectory(prefix="readback-late-") as temp:
        src = Path(temp) / "readback_late.cpp"
        exe = Path(temp) / "readback_late.exe"   # (the name llvm-mingw writes on Windows; any name runs on Linux)
        src.write_text(FIXTURE, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu/vk"), str(src), "-o", str(exe)],
                       check=True)
        r = subprocess.run([str(exe)], text=True, capture_output=True, timeout=20)
        print(r.stdout, end="")
        if r.returncode:
            raise SystemExit(r.returncode)
        src = Path(temp) / "readback_late_wanted.cpp"
        exe = Path(temp) / "readback_late_wanted.exe"
        src.write_text(WANTED.replace("@@WANTED@@", wanted), encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu/vk"), str(src), "-o", str(exe)],
                       check=True)
        base = {k: v for k, v in os.environ.items() if k not in ("RX_VK_READBACK_LATE", "TEST_HANDHELD")}
        for value, handheld, size, newest, want in WANTED_CASES:
            env = dict(base) if value is None else dict(base, RX_VK_READBACK_LATE=value)
            if handheld:
                env["TEST_HANDHELD"] = "1"
            r = subprocess.run([str(exe), size, str(newest)], env=env, text=True, capture_output=True, timeout=20)
            if r.returncode or r.stdout.strip() != str(want):
                raise SystemExit("FAIL: readback_late_wanted(0x%s, newest %d) with RX_VK_READBACK_LATE %s%s: %s, expected %d"
                                 % (size, newest, "unset" if value is None else "=" + value, " on a handheld" if handheld else "",
                                    (r.stdout + r.stderr).strip(), want))
        print("PASS: readback_late_wanted: the culling camera block read late by default, on a handheld the newest submission's "
              "too, =0 none, the rules over it when set")


if __name__ == "__main__":
    main()
