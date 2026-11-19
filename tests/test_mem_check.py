"""RX_VK_MEM_CHECK's rules, driven with made-up events (gpu/vk/mem_check.h; no game, no GPU), and its hooks in the renderer.

mem_check.h counts breaks of the rules the memory sync rests on. The fixture compiles the header alone and plays one
sequence of events per rule, each shown to raise its own counter and no other, after a sequence with no break that
raises none:

- a capture taken, or the written pages collected, while a recording is open;
- a take that gets another capture than the one its submit made: a wrong number, an empty queue, and a capture made
  with no take, after which every later take is off by one;
- a mirror sync of a capture's page the GPU wrote after the capture's view of it, the page not late: a candidate by the
  records (and none when the page is late, the GPU was done before the capture, or the copy is from live memory), and a
  count only by the bytes: a word the copy holds as the page's last sync left it, which the mirror held differently,
  replaced. That is a loss or the CPU's own write of the same value (a counter set back to zero), which the bytes cannot
  tell apart, and the line says so. The same bytes put back, or only words whose values changed, is counted as "same";
  no earlier copy of the page, or no read-back, as not judged;
- the same for a page the GPU wrote through a pointer, and the one certain case counted apart: a page synced from live
  memory that only the GPU's own stamp made dirty;
- replaced_words itself: the part of the page the sync copied, and whole words only;
- why a candidate got no verdict, by reason; a sparse mirror's block with no memory (harmless); how a candidate came to
  be one, by path; a GPU write too large to note page by page, kept as a range;
- the live reads, counted by cause; the pages found late at the replay;
- the generation counter (64 bits) going backwards.

Then the log: the first few cases of a kind are logged and the rest only counted, and every 600th submit logs the
summary line. And the source: each hook in vk_memory.cpp and vk_replay.cpp is there, behind the switch.

python -I -B tests/test_mem_check.py [--cxx g++]
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

FIXTURE = r'''#include "mem_check.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
using namespace mem_check;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
static std::vector<std::string> lines;
static void log_line(const char* line) { lines.push_back(line); }
// (only this kind was counted, n times)
static void only(Check& c, Kind k, uint64_t n) {
    for (int i = 0; i < kKinds; i++) CHECK(c.count((Kind)i) == (i == k ? n : 0));
    CHECK(c.violations() == n);
}
static uint64_t lives(Check& c) {
    uint64_t n = 0;
    for (int i = 0; i < kLives; i++) n += c.live((Live)i);
    return n;
}
// (one page taken by a capture: copied or not, late or not, made with the GPU at `done`)
static void take_page(Check& c, uint32_t page, bool copied, bool late, uint64_t done) {
    const uint32_t zero = 0;
    c.take_pages(&page, 1, &page, copied ? 1 : 0, &zero, late ? 1 : 0, done);
}
// (a page of 1024 words, all `fill`, and one word of it set)
struct Page {
    uint8_t b[kPage];
    explicit Page(uint32_t fill = 0) {
        for (size_t i = 0; i < kPage / 4; i++) std::memcpy(b + i * 4, &fill, 4);
    }
    Page& word(size_t i, uint32_t v) {
        std::memcpy(b + i * 4, &v, 4);
        return *this;
    }
};

int main() {
    {   // nothing broken: submits whose takes match, between recordings, syncs from captures that hold the GPU's words
        Check c(log_line);
        int64_t now = 0;
        for (uint64_t v = 1; v <= 50; v++) {
            const uint64_t seq = c.capture_made();
            CHECK(seq == v);
            c.take(seq, true, seq, false, v);
            take_page(c, 100, true, false, v - 1);       // (made with every earlier submission done on the GPU)
            c.collect(false, v);
            CHECK(c.sync_page(100, true, 0x64000).mask == 0);   // (the sync, from the capture just taken)
            CHECK(c.sync_page(200, true, 0x64000).mask == 0);   // (a page nothing was ever noted for)
            c.gpu_wrote(100, 101, v);                    // (then the recording's GPU work writes the page)
            c.submit(v * 3, now += 1000000);
        }
        c.take(0, true, 0, false, 51);                   // (a submit with captures off: none made, none taken)
        only(c, kKinds, 0);
        CHECK(lives(c) == 0 && c.unpaired_takes() == 0);
        CHECK(c.candidates(kSyncOverGpu) == 0 && c.candidates(kSyncOverGpuOnly) == 0);
        CHECK(lines.empty());
    }
    {   // a capture taken while a recording is open
        Check c;
        c.take(c.capture_made(), true, 1, true, 7);
        only(c, kTakeOpen, 1);
    }
    {   // the written pages collected while a recording is open
        Check c;
        c.collect(false, 7);
        only(c, kKinds, 0);
        c.collect(true, 8);
        only(c, kCollectOpen, 1);
    }
    {   // the take's capture is not its submit's
        Check c;
        const uint64_t a = c.capture_made(), b = c.capture_made();
        c.take(b, true, a, false, 1);                    // (another submit's)
        only(c, kSeqMismatch, 1);
        c.take(0, true, b, false, 2);                    // (the queue was empty)
        only(c, kSeqMismatch, 2);
        c.take(a, true, 0, false, 3);                    // (its submit made none, and it took one)
        only(c, kSeqMismatch, 3);
        c.take(b, false, 0, false, 4);                   // (a take nobody said what to expect of: counted apart)
        only(c, kSeqMismatch, 3);
        CHECK(c.unpaired_takes() == 1);
    }
    {   // a capture made with no take: every later take is one behind, which only the numbers show
        Check c;
        std::deque<uint64_t> queue;
        queue.push_back(c.capture_made());               // (a capture whose submission never takes it)
        for (uint64_t v = 1; v <= 5; v++) {
            const uint64_t mine = c.capture_made();
            queue.push_back(mine);
            const uint64_t got = queue.front();
            queue.pop_front();
            c.take(got, true, mine, false, v);
        }
        only(c, kSeqMismatch, 5);
    }
    {   // replaced_words: a word the copy holds as the last sync left it, that the mirror held differently; only in [lo, hi), whole words
        const Page prev(0x11111111);
        Page cur(0x11111111), mirror(0x11111111);
        size_t first = 99;
        CHECK(replaced_words(prev.b, cur.b, mirror.b, 0, kPage, &first) == 0);
        mirror.word(5, 0x99999999);                      // (the GPU's word)
        CHECK(replaced_words(prev.b, cur.b, mirror.b, 0, kPage, &first) == 1 && first == 20);
        cur.word(5, 0x22222222);                         // (the CPU gave that word another value: its word wins, nothing counted)
        CHECK(replaced_words(prev.b, cur.b, mirror.b, 0, kPage, &first) == 0);
        cur.word(5, 0x11111111);
        mirror.word(900, 0x77777777);
        CHECK(replaced_words(prev.b, cur.b, mirror.b, 0, kPage, &first) == 2 && first == 20);
        CHECK(replaced_words(prev.b, cur.b, mirror.b, 24, kPage, &first) == 1 && first == 3600);   // (the sync began past word 5)
        CHECK(replaced_words(prev.b, cur.b, mirror.b, 24, 3600, &first) == 0);                     // (and ended before word 900)
        CHECK(replaced_words(prev.b, cur.b, mirror.b, 22, 24, &first) == 0);                       // (word 5 only in part)
    }
    {   // a sync of a capture's page over a GPU write newer than the capture: a candidate, a break by its bytes
        Check c;
        c.gpu_wrote(7, 7, 10, kWriterProgram, 0xabc);    // (submission 10 is recorded: a program writes page 7)
        take_page(c, 7, true, false, 8);                 // (the next capture was made with the GPU at 8, the page not late)
        Cand k = c.sync_page(7, true, 0x7000);
        CHECK(k.mask == kCandGpu && k.gpu_value == 10 && k.cap_done == 8 && k.writer == kWriterProgram && k.writer_key == 0xabc);
        only(c, kKinds, 0);                              // (a candidate is not a break)
        const Page first_copy(0x11111111);
        Page got;
        CHECK(!c.exchange(7, first_copy.b, got.b));      // (the page's first sync: nothing to compare it with)
        c.judged(7, k, 0x7000, false, got.b, first_copy.b, first_copy.b, 0, kPage, "");
        only(c, kKinds, 0);
        CHECK(c.not_judged(kSyncOverGpu) == 1 && c.same(kSyncOverGpu) == 0);
        // (the next one: the CPU changed word 0; the GPU had written word 5 in the mirror, and the sync puts the old one back)
        Page cur(0x11111111), mirror(0x11111111);
        cur.word(0, 0x22222222), mirror.word(5, 0x99999999);
        k = c.sync_page(7, true, 0x7000);
        CHECK(k.mask == (kCandGpu | kCandWatched));
        CHECK(c.exchange(7, cur.b, got.b) && !std::memcmp(got.b, first_copy.b, kPage));
        c.judged(7, k, 0x7000, true, got.b, cur.b, mirror.b, 0, kPage, "a made-up page");
        only(c, kSyncOverGpu, 1);
        // (and one that loses nothing: the mirror has what the last sync put there; the CPU changed word 1)
        Page next = cur;
        next.word(1, 0x33333333);
        k = c.sync_page(7, true, 0x7000);
        CHECK(c.exchange(7, next.b, got.b) && !std::memcmp(got.b, cur.b, kPage));
        c.judged(7, k, 0x7000, true, got.b, next.b, cur.b, 0, kPage, "");
        only(c, kSyncOverGpu, 1);
        CHECK(c.same(kSyncOverGpu) == 1 && c.candidates(kSyncOverGpu) == 3);
        c.unjudged(k, kWhyPending);                      // (no read-back of the mirror: too many waiting)
        CHECK(c.not_judged(kSyncOverGpu) == 2);
        CHECK(c.not_judged(kSyncOverGpu, kWhyFirst) == 1 && c.not_judged(kSyncOverGpu, kWhyPending) == 1);
        CHECK(c.not_judged(kSyncOverGpu, kWhyFollowed) == 0 && c.not_judged(kSyncOverGpu, kWhyOther) == 0);
        c.judged(7, k, 0x7000, -1, got.b, next.b, cur.b, 0, kPage, "");   // (a page past the ones followed)
        CHECK(c.not_judged(kSyncOverGpu, kWhyFollowed) == 1);
        c.unbound(k);                                    // (a sparse mirror's block with no memory: nothing to lose)
        CHECK(c.same(kSyncOverGpu) == 2 && c.unbound_pages(kSyncOverGpu) == 1);
        // (how a candidate came to be one is counted by path, and is no break)
        c.candidate_path(7, k, 0x7000, kPathNoWriteBack, "");
        c.candidate_path(7, k, 0x7000, kPathInPlace, "");
        c.candidate_path(7, k, 0x7000, kPathInPlace, "");
        CHECK(c.path_count(kPathNoWriteBack) == 1 && c.path_count(kPathInPlace) == 2 && c.path_count(kPathMirror) == 0);
        // (no candidate: from live memory; the page late in its capture; a capture made with submission 10 done)
        CHECK(!(c.sync_page(7, false, 0x7000).mask & kCandGpu));
        take_page(c, 7, true, true, 8);
        CHECK(!(c.sync_page(7, true, 0x7000).mask & kCandGpu));
        take_page(c, 7, true, false, 10);
        CHECK(!(c.sync_page(7, true, 0x7000).mask & kCandGpu));
        c.gpu_wrote(7, 7, 12);                           // (and written again after that capture)
        CHECK(c.sync_page(7, true, 0x7000).mask & kCandGpu);
        CHECK(c.candidates(kSyncOverGpu) == 4);
        only(c, kSyncOverGpu, 1);
    }
    {   // a sync over a page the GPU wrote through a pointer
        Check c;
        c.gpu_only(3, 4);
        CHECK(c.sync_page(5, false, 0x3000).mask == 0);
        Cand k = c.sync_page(3, false, 0x3000);
        CHECK(k.mask == kCandGpuOnly);
        const Page a(0x44444444);
        Page got, mirror(0x44444444);
        CHECK(!c.exchange(3, a.b, got.b));
        k = c.sync_page(3, false, 0x3000);
        CHECK(k.mask == (kCandGpuOnly | kCandWatched) && c.exchange(3, a.b, got.b));
        c.judged(3, k, 0x3000, true, got.b, a.b, mirror.b, 0, kPage, "");   // (the mirror had the same bytes)
        only(c, kKinds, 0);
        CHECK(c.same(kSyncOverGpuOnly) == 1);
        mirror.word(77, 0x12345678);                     // (the GPU's record, in the mirror alone)
        k = c.sync_page(3, false, 0x3000);
        CHECK(c.exchange(3, a.b, got.b));
        c.judged(3, k, 0x3000, true, got.b, a.b, mirror.b, 0, kPage, "");
        only(c, kSyncOverGpuOnly, 1);
        CHECK(c.candidates(kSyncOverGpuOnly) == 3);
        CHECK(!k.gpu_stamp && c.certain(kSyncOverGpuOnly) == 0);   // (no stamp of the GPU's was said for the page)
        // (the certain case: synced from live memory, dirty by the GPU's own stamp alone. A capture's page, or a page
        // stamped since, was written by the CPU: its replaced words may be the CPU's own)
        c.gpu_wrote(3, 3, 60, kWriterUnknown, 0, 500);   // (the renderer stamped the page with generation 500 for this write)
        CHECK(!c.sync_page(3, true, 0x3000, 500).gpu_stamp && !c.sync_page(3, false, 0x3000, 501).gpu_stamp);
        CHECK(!c.sync_page(3, false, 0x3000).gpu_stamp);
        k = c.sync_page(3, false, 0x3000, 500);
        CHECK(k.gpu_stamp);
        c.judged(3, k, 0x3000, true, got.b, a.b, mirror.b, 0, kPage, "");
        only(c, kSyncOverGpuOnly, 2);
        CHECK(c.certain(kSyncOverGpuOnly) == 1 && c.certain(kSyncOverGpu) == 0);
        CHECK(c.summary(1, 1).find("sync_over_gpu_only: 2 replaced (a loss or the CPU's reset; 1 on a page no CPU write made dirty), 1 "
                                   "same") != std::string::npos);
        // (a write too large to note page by page keeps its stamp too)
        c.gpu_only(100000, 100000 + Check::kMaxRangePages);
        c.gpu_wrote(100000, 100000 + Check::kMaxRangePages, 61, kWriterUnknown, 0, 700);
        CHECK(c.sync_page(100005, false, 0).gpu_stamp == false);
        CHECK(c.sync_page(100005, false, 0, 700).gpu_stamp);
    }
    {   // the live reads, by cause; none of them is a break of the other rules; the pages found late at the replay
        Check c;
        c.live_read(1, 5, 0, false);
        CHECK(c.live(kLiveNeverCaptured) == 1 && lives(c) == 1);
        c.live_read(1, 9, 5, true);
        CHECK(c.live(kLiveWrittenSince) == 1 && lives(c) == 2);
        c.live_read(1, 5, 5, false);
        CHECK(c.live(kLiveRetired) == 1 && lives(c) == 3);
        take_page(c, 2, false, false, 0);                // (stamped by its capture, not copied)
        c.live_read(2, 9, 0, false);
        CHECK(c.live(kLiveNotCopied) == 1 && lives(c) == 4);
        take_page(c, 2, true, false, 0);                 // (copied by a later one, then written again)
        c.live_read(2, 0x100000000cull, 0x100000000bull, true);   // (generations past 32 bits)
        CHECK(c.live(kLiveWrittenSince) == 2 && lives(c) == 5);
        c.late_at_replay(3);
        CHECK(c.late_at_replay_pages() == 3);
        only(c, kKinds, 0);
    }
    {   // the generation counter: past 32 bits is no step back; a step back is counted
        Check c;
        c.submit(4294967290ull, 1);
        c.submit(4294967400ull, 2);
        only(c, kKinds, 0);
        c.submit(17, 3);
        only(c, kGenBack, 1);
    }
    {   // a GPU write too large to note page by page is kept as a range, and its pages are candidates like the others
        Check c;
        c.gpu_wrote(0, Check::kMaxRangePages, 3, kWriterProgram, 0x77);
        CHECK(c.tracked() == 0);
        CHECK(c.sync_page(6, true, 0).mask == 0);        // (not captured: no candidate for a capture's sync)
        take_page(c, 6, true, false, 2);                 // (captured with the GPU at 2: the range's write, in 3, is newer)
        Cand k = c.sync_page(6, true, 0);
        CHECK(k.mask == kCandGpu && k.gpu_value == 3 && k.writer == kWriterProgram && k.writer_key == 0x77);
        take_page(c, 6, true, false, 3);
        CHECK(c.sync_page(6, true, 0).mask == 0);
        c.gpu_only(0, Check::kMaxRangePages);            // (the same range, written through a pointer)
        CHECK(c.sync_page(9000, false, 0).mask == kCandGpuOnly);
        CHECK(c.sync_page(Check::kMaxRangePages + 1, false, 0).mask == 0);   // (past its end)
        CHECK(c.tracked() == 1);
        CHECK(c.summary(1, 1).find("1 large ranges (0 table resets, 0 ranges too large to note)") != std::string::npos);
    }
    {   // the log: the first cases of a kind, then the count alone; the summary line every 600 submits
        lines.clear();
        Check c(log_line);
        for (int i = 0; i < 20; i++) c.collect(true, 100 + i);
        only(c, kCollectOpen, 20);
        CHECK(lines.size() == Check::kLogFirst);
        CHECK(lines[0].rfind("vk: mem check: collect_open #1: ", 0) == 0);
        CHECK(lines[0].find("submission 100") != std::string::npos);
        c.gpu_only(3, 3);
        const Page a(0x44444444);
        Page got, mirror(0x44444444);
        mirror.word(2, 0x55555555);
        c.exchange(3, a.b, got.b);
        const Cand k = c.sync_page(3, false, 0x2000);
        c.exchange(3, a.b, got.b);
        c.judged(3, k, 0x2000, true, got.b, a.b, mirror.b, 0, kPage, "a made-up page");
        CHECK(lines.size() == Check::kLogFirst + 1);
        CHECK(lines.back().find("sync_over_gpu_only #1") != std::string::npos);
        CHECK(lines.back().find("page 0x3000 (pool 0x2000) replaced 1 words") != std::string::npos);
        CHECK(lines.back().find("first at +0x8: the mirror had 55555555, the copy has 44444444") != std::string::npos);
        CHECK(lines.back().find("a made-up page") != std::string::npos);
        CHECK(lines.back().find("a loss if the CPU did not write those words, its own write of the same value") != std::string::npos);
        c.candidate_path(7, k, 0x7000, kPathMirror, "a made-up page");
        CHECK(lines.size() == Check::kLogFirst + 2);
        CHECK(lines.back().rfind("vk: mem check: sync_over_gpu candidate (written back from a mirror since the capture, and not late) #1: "
                                 "page 0x7000 (pool 0x7000)", 0) == 0);
        lines.clear();
        for (uint32_t i = 1; i < Check::kSummaryEvery; i++) c.submit(i * 10, (int64_t)i * 1000000000);
        CHECK(lines.empty());
        c.submit(6000, 600000000000ll);
        CHECK(lines.size() == 1);
        CHECK(lines[0].rfind("vk: mem check: 600 submits: ", 0) == 0);
        CHECK(lines[0].find("collect_open 20,") != std::string::npos);
        CHECK(lines[0].find("sync_over_gpu: 0 replaced (a loss or the CPU's reset; 0 on a page no CPU write made dirty), 0 same (0 unbound), 0 not judged (of 0 candidates; not judged: 0 first sync seen, "
                             "0 read-backs pending, 0 past the pages followed, 0 other; candidates by path: 0 no write-back, 0 in place, "
                             "1 mirror write-back not late)") != std::string::npos);
        CHECK(lines[0].find("sync_over_gpu_only: 1 replaced (a loss or the CPU's reset; 0 on a page no CPU write made dirty), 0 same (0 unbound), "
                             "0 not judged (of 1 candidates;") != std::string::npos);
        CHECK(lines.size() == 1);
        CHECK(lines[0].find("gen 6000 (+5990, 10 a second)") != std::string::npos);
    }
    std::printf("PASS: mem_check.h counts each rule's break, and nothing in a run that breaks none\n");
    return 0;
}
'''


def need(where, text, needles):
    for needle in needles:
        if needle not in text:
            raise SystemExit("FAIL: %s no longer has: %s" % (where, needle))


def check_source():
    """The hooks are where the rules could break, each behind the switch"""
    memory = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    need("vk_memory.cpp", memory, ['static const bool on = env_on("RX_VK_MEM_CHECK");'])
    need("vk_memory.cpp: memory_capture_submit()", block(memory, "void memory_capture_submit() {"),
         ["if (mem_check_on()) mem_check_made_seq = c ? (c->check_seq = mem_checker().capture_made()) : 0;"])
    take = block(memory, "bool memory_take_capture(uint64_t value, uint64_t completed) {")
    need("vk_memory.cpp: memory_take_capture()", take, ["if (mem_check_on()) mem_check_take(c.get(), value);"])
    if not take.index("mem_check_take(") < take.index("take_capture(std::move(c)"):
        raise SystemExit("FAIL: vk_memory.cpp: memory_take_capture() tells the check of the take only after it")
    need("vk_memory.cpp: mem_check_take()", block(memory, "void mem_check_take(const Capture* c, uint64_t value) {"),
         ["mem_check_recording_open()", "mem_checker().take_pages("])
    need("vk_memory.cpp: memory_capture_now()", block(memory, "bool memory_capture_now(uint64_t value, uint64_t completed) {"),
         ["if (c && mem_check_on()) {", "mem_check_take(c.get(), value);"])
    need("vk_memory.cpp: memory_check_submit()", block(memory, "void memory_check_submit(uint64_t value) {"),
         ["if (!mem_check_on()) return;", "mem_checker().collect(mem_check_recording_open(), value);", "mem_checker().submit(gen_now,"])
    need("vk_memory.cpp: memory_stamp_gpu()", block(memory, "void memory_stamp_gpu(uint64_t guest, uint64_t size) {"),
         ["if (mem_check_on()) mem_check_gpu_wrote(guest, size);"])
    need("vk_memory.cpp: mem_check_gpu_wrote()", block(memory, "void mem_check_gpu_wrote(uint64_t guest, uint64_t size) {"),
         ["page_gen(guest >> kPageShift)"])
    need("vk_memory.cpp: memory_note_gpu_write()", block(memory, "void memory_note_gpu_write(uint64_t gpu, uint64_t size) {"),
         ["if (mem_check_on()) mem_check_gpu_only(guest, size);"])
    need("vk_memory.cpp: mirror_sync()", block(memory, "void mirror_sync(Pool& p) {"),
         ["mem_check_sync(p, page, true, (const uint8_t*)b->second.map + (uint64_t)e.slot * 4096, pg);",
          "mem_check_sync(p, page, false, (const uint8_t*)host->guest_base() + (page << kPageShift), pg);",
          "mem_checker().live_read(page, pg, e.gen, e.gen && cap_bufs.count(e.gen));"])
    # (a candidate's mirror is read back ahead of the sync's own copies, and judged only once the GPU is done with it)
    sync = block(memory, "void mem_check_sync(Pool& p, uint64_t page, bool from_capture, const uint8_t* src, uint64_t page_gen) {")
    need("vk_memory.cpp: mem_check_sync()", sync,
         ["mem_checker().sync_page(page, from_capture, p.lo, page_gen);", "mem_checker().exchange(page, pr.cur.data(), pr.prev.data());",
          "vkCmdCopyBuffer(upload_command_buffer(), p.mirror, probe_buf, 1, &c);", "pr.value = mem_rec_value;",
          "if (p.mirror && p.sparse && !sparse_has(p, ga0 - p.lo, gb0 - ga0)) {", "mem_checker().unbound(cand);",
          "mem_checker().candidate_path(page, cand, p.lo, path, what.c_str());"])
    need("vk_memory.cpp: mem_check_probes_resolve()", block(memory, "void mem_check_probes_resolve() {"),
         ["probes.front().value <= mem_completed", "mem_checker().judged("])
    take_fn = block(memory, "bool memory_take_capture(uint64_t value, uint64_t completed) {")
    if not take_fn.index("capture_late_at_replay(*c);") < take_fn.index("mem_check_take(c.get(), value);"):
        raise SystemExit("FAIL: vk_memory.cpp: the check hears of a take before its late pages are complete")
    work = block(replay, "void submit_memory_work(uint64_t value) {")
    need("vk_replay.cpp: submit_memory_work()", work, ["memory_check_submit(value);"])
    if not work.index("memory_collect_written();") < work.index("memory_check_submit(value);"):
        raise SystemExit("FAIL: vk_replay.cpp: submit_memory_work() tells the check before the collection it checks")
    need("vk_replay.cpp: submit_q()", block(replay, "void submit_q(const uint64_t* handles, int n, uint64_t queue) {"),
         ["memory_check_expect_capture(memory_check_capture_seq());", "j.cap_seq = memory_check_capture_seq();"])
    need("vk_replay.cpp: event_signal_queued()", block(replay, "void event_signal_queued(uint64_t gpu, uint32_t value) {"),
         ["j.cap_seq = memory_check_capture_seq();"])
    if replay.count("memory_check_expect_capture(j.cap_seq);") != 2:
        raise SystemExit("FAIL: vk_replay.cpp: a submit's job and a CPU signal's job each say which capture their take expects")
    # (off, a hook is one test: no call into the check but behind the switch, in a function that is itself behind it)
    guarded = ("mem_check_on()", "void mem_check_", "mem_check::Check& mem_checker() {")
    inside = [block(memory, marker) for marker in ("void mem_check_gpu_wrote(uint64_t guest, uint64_t size) {",
                                                    "void mem_check_sync(Pool& p, uint64_t page, bool from_capture, const uint8_t* src, uint64_t page_gen) {",
                                                    "void mem_check_probes_resolve() {", "void capture_late_at_replay(Capture& c) {",
                                                    "void mem_check_gpu_only(uint64_t guest, uint64_t size) {",
                                                    "void mem_check_take(const Capture* c, uint64_t value) {",
                                                    "void memory_check_submit(uint64_t value) {",
                                                    "bool memory_capture_now(uint64_t value, uint64_t completed) {")]
    lines = memory.split("\n")
    for number, line in enumerate(lines, 1):
        if "mem_checker()." not in line or any(g in line for g in guarded) or any(line in part for part in inside):
            continue
        before = "\n".join(lines[max(0, number - 4):number - 1])
        if "if (mem_check_on()) {" not in before:
            raise SystemExit("FAIL: gpu/vk/vk_memory.cpp:%d calls the memory check outside the switch: %s" % (number, line.strip()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="mem-check-") as tmp:
        tmp = Path(tmp)
        (tmp / "fixture.cpp").write_text(FIXTURE, encoding="utf-8")
        exe = tmp / ("fixture" + (".exe" if os.name == "nt" else ""))
        cmd = [args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu" / "vk"), str(tmp / "fixture.cpp"),
               "-o", str(exe)]
        if os.name != "nt":
            cmd.append("-pthread")
        subprocess.run(cmd, check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
