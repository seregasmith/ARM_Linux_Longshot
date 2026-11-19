"""A capture's late pages are completed when the replay takes it (gpu/vk/late_replay.h, gpu/vk/vk_memory.cpp).

A capture's page that a submission not done yet writes back is late: its copy lacks the GPU's words, and the replay
waits for the write-back or fixes the copy before a mirror sync uses it. capture_make finds the late pages on the
game's thread from the write-backs recorded so far. With the replay on a thread of its own the game captures the next
submissions before the replay has recorded the ones before: a page the CPU wrote and one of those submissions writes
back was not late, and the mirror sync copied the capture's older words over the GPU's (RX_VK_MEM_CHECK's
sync_over_gpu: the capture made with the GPU four submissions behind). The take is on the replay's thread, after every
earlier submission is recorded, and completes the list there.

The fixture compiles late_replay.h alone and plays that order: a capture whose list was made before the write-back was
recorded has the page added at its take; a page written back by a submission done at the capture is not late; a page
whose newest write the GPU made in place is not; a page already late stays, with the newest write-back to wait for; an
added page loses its staging slot; the list stays in the pages' order; what comes back is the newest write-back.

Then the source: memory_take_capture completes the list before the take (and before the check hears of it), and
late_fix_build does not copy a page found late at the replay again from guest memory.

python -I -B tests/test_late_replay.py [--cxx g++]
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from srccut import block   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r'''#include "late_replay.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
// (the write-backs recorded: a page's newest, and its newest one made in place; vk_memory.cpp's wb_pages and wb_direct)
static std::map<uint32_t, uint64_t> wb, direct;
static uint64_t wb_of(uint32_t page) { return wb.count(page) ? wb[page] : 0; }
static uint64_t direct_of(uint32_t page) { return direct.count(page) ? direct[page] : 0; }
// (capture_make's rule, on the game's thread: late by the write-backs recorded when the capture is made)
static void at_capture(const std::vector<uint32_t>& pages, uint64_t done, std::vector<uint32_t>& late, std::vector<uint64_t>& pv) {
    late.clear(), pv.clear();
    for (size_t i = 0; i < pages.size(); i++)
        if (wb_of(pages[i]) > done) late.push_back((uint32_t)i), pv.push_back(wb_of(pages[i]));
}
int main() {
    const std::vector<uint32_t> pages{10, 11, 12, 13, 14, 15};
    std::vector<uint32_t> late, added;
    std::vector<uint64_t> pv;
    std::vector<int32_t> sidx, none;

    // the frames before: page 11 written back by submission 96293, page 13 by 96295, both done when the capture is made
    wb = {{11, 96293}, {13, 96295}};
    direct.clear();
    const uint64_t done = 96296;
    at_capture(pages, done, late, pv);
    CHECK(late.empty());                                 // (nothing late that the game's thread could know of)
    // ... the replay then records 96297 to 96300: 96300 writes page 11 back from its mirror, 96299 page 14 in place,
    // and 96298 page 15 from a mirror with 96300 writing it in place after
    wb[11] = 96300, wb[14] = 96299, direct[14] = 96299, wb[15] = 96300, direct[15] = 96300;
    sidx = {0, 1, -1, 2, 3, 4};                          // (staging slots: page 12 had none)
    uint64_t newest = late_replay::complete(pages, done, late, pv, added, sidx, wb_of, direct_of);
    CHECK(late == std::vector<uint32_t>{1} && pv == std::vector<uint64_t>{96300});    // (page 11, and only it)
    CHECK(added == std::vector<uint32_t>{1} && newest == 96300);
    CHECK(sidx == (std::vector<int32_t>{0, -1, -1, 2, 3, 4}));                        // (read from the buffer, as a late page)

    // a mirror's write-back after an older write in place: late (the mirror is synced over)
    direct[15] = 96297;
    at_capture(pages, done, late, pv);                   // (as if made after 96300 was recorded: both known there too)
    CHECK(late == (std::vector<uint32_t>{1, 4, 5}));     // (capture_make's own rule takes the page written in place as well)
    late = {4}, pv = {96299};                            // (a list with one page found at the capture: it stays)
    wb[14] = 96301;                                      // (and a newer write-back of it recorded since)
    newest = late_replay::complete(pages, done, late, pv, added, none, wb_of, direct_of);
    CHECK(late == (std::vector<uint32_t>{1, 4, 5}));     // (in the pages' order)
    CHECK(pv == (std::vector<uint64_t>{96300, 96301, 96300}));
    CHECK(added == (std::vector<uint32_t>{1, 5}) && newest == 96301);
    CHECK(none.empty());

    // done again with nothing new recorded: the same list, nothing added
    newest = late_replay::complete(pages, done, late, pv, added, none, wb_of, direct_of);
    CHECK(late == (std::vector<uint32_t>{1, 4, 5}) && added.empty() && newest == 96301);

    // a capture made with everything done: nothing late, nothing to wait for
    late.clear(), pv.clear();
    newest = late_replay::complete(pages, 96301, late, pv, added, none, wb_of, direct_of);
    CHECK(late.empty() && pv.empty() && added.empty() && newest == 0);

    // a list that came with no write-back values (RX_VK_CAPTURE_WAIT's other modes): each gets the newest there is
    late = {3}, pv.clear();
    newest = late_replay::complete(pages, 96290, late, pv, added, none, wb_of, direct_of);
    CHECK(late == (std::vector<uint32_t>{1, 3, 4, 5}) && pv == (std::vector<uint64_t>{96300, 96295, 96301, 96300}));
    CHECK(added == (std::vector<uint32_t>{1, 4, 5}) && newest == 96301);
    std::printf("PASS: a capture's late pages are completed at its take from the write-backs recorded by then\n");
    return 0;
}
'''


def need(where, text, needles):
    for needle in needles:
        if needle not in text:
            raise SystemExit("FAIL: %s no longer has: %s" % (where, needle))


def check_source():
    memory = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    done = block(memory, "void capture_late_at_replay(Capture& c) {")
    need("vk_memory.cpp: capture_late_at_replay()", done,
         ["if (!late_at_replay_on() || !mirror_on() || c.pages.empty() || !c.map) return;", "std::lock_guard lk(wb_pages_mu);",
          "late_replay::complete(c.pages, c.late_done, c.late, c.late_pv, c.late_new, c.sidx,",
          "[](uint32_t pg) { return wb_pages.get(pg); },", "return in_place ? 0 : wb_direct.get(pg); });",
          "if (newest > c.late_value) c.late_value = newest;"])
    take = block(memory, "bool memory_take_capture(uint64_t value, uint64_t completed) {")
    need("vk_memory.cpp: memory_take_capture()", take, ["if (c) capture_late_at_replay(*c);"])
    if not take.index("captures.pop_front();") < take.index("capture_late_at_replay(*c);") < take.index("take_capture(std::move(c)"):
        raise SystemExit("FAIL: vk_memory.cpp: memory_take_capture() does not complete the late pages between the queue and the take")
    # (the pages found there keep their CPU words as submitted: not copied again from guest memory, fixed from late_done on)
    build = block(memory, "bool late_fix_build(Capture& c) {")
    need("vk_memory.cpp: late_fix_build()", build,
         ["c.late_done + 1 >= sb->second.since", "Dp = c.late_done, merged[i] = 1;", "if (copied[i] && !merged[i])",
          "(uint32_t)((end - a) / 4), (uint32_t)Dp});"])
    need("vk_memory.cpp: capture_make()", block(memory, "std::unique_ptr<Capture> capture_make() {"), ["c->late_new.clear();"])
    need("vk_memory.cpp: late_at_replay_on()", block(memory, "bool late_at_replay_on() {"), ['getenv("RX_VK_LATE_AT_REPLAY")'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="late-replay-") as tmp:
        tmp = Path(tmp)
        (tmp / "fixture.cpp").write_text(FIXTURE, encoding="utf-8")
        exe = tmp / ("fixture" + (".exe" if os.name == "nt" else ""))
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu" / "vk"), str(tmp / "fixture.cpp"),
                        "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
