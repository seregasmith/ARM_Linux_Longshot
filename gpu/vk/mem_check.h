// (longshot) RX_VK_MEM_CHECK=1: a check of the rules the memory sync rests on (vk_memory.cpp), off by default.
//
// Guest memory has several copies in the renderer (the submit-time captures, the VRAM mirrors, the write-back shadows) and
// several tables that say which copy of a page is newest. The rules that keep them agreeing are spread over the code as
// comments; this states six of them as counters. vk_memory.cpp and vk_replay.cpp call in at the places where each rule
// could break, behind one test of the switch each; nothing here changes what the renderer does, and nothing aborts. A
// rule broken is counted, its first few cases are logged with the page and where, and one line every 600 submits has the
// totals ("vk: mem check: ...").
//
// The rules, and what breaking each one means:
//   take_open, collect_open  A capture is taken, or the written pages collected, only between recordings. Both stamp
//                            CPU pages with a new generation, and a recording's end sets every pool it synced to "current
//                            as of now": a page stamped while the recording was open would be taken for synced.
//   seq_mismatch             The capture a submission takes is the one its own submit made. They are paired by their
//                            place in a queue alone: one capture made with no take, or the reverse, shifts every later
//                            pair by one and nothing else would say so.
//   sync_over_gpu            A mirror sync of a capture's page, where the GPU wrote the page in a submission that was not
//                            done when the capture was made and the capture did not mark the page late.
//   sync_over_gpu_only       A mirror sync of a page the GPU wrote through a pointer (a bare or wide range): words that
//                            are in the mirror alone and are never written back.
//                            Neither count is a count of losses. Both are told by the page's records first (a
//                            candidate) and then looked at by the page's bytes: the renderer reads the mirror's page
//                            back as it was before the sync and hands in three copies of the page, what the last sync
//                            of it copied (prev), what this one copies (cur), and the mirror before it. "replaced": the
//                            sync put a word over a different one in the mirror while the capture's word equals the
//                            last synced one (cur == prev, mirror != cur). That is one of two things the bytes cannot
//                            tell apart. A loss: the CPU never wrote the word, the GPU did, and the page was copied
//                            whole because the CPU wrote another word of it. Or the CPU's own write of the same value
//                            as before: a counter or a list the GPU fills and the game sets back to zero every frame.
//                            On the console memory is shared and a write of zero is a write whatever was there; here
//                            only the page's write mark says the CPU wrote, and not which words. (A sync that copied
//                            only the words whose values changed was tried: the resets never reached the mirror, the
//                            GPU's counters grew, and its work more than doubled.) One case is certain, and counted
//                            apart: a page synced from live guest memory whose only stamp since its pool's sync is the
//                            GPU's own (gpu_stamp). No write of the CPU's made that page dirty, so the copy is not
//                            there to bring a reset, and a word it replaced is a loss (but for a CPU write still
//                            waiting to be collected, which the live bytes hold already: rare, and not told apart). A
//                            page synced from a capture was written by the CPU, by construction: a capture takes the
//                            pages whose mark was set. "same": no such word. "not judged": by reason.
//   live reads               Design rule 2: nothing the replay reads comes from live guest memory. Each dirty page a
//                            sync copies from guest memory as it is now is counted by why no capture had it.
//   gen                      The generation counter only goes up (64 bits; 0 means "never written"). Its speed is in
//                            the summary; a step backwards is counted.
//
// What it keeps: a record a page (4 KB), only for pages a capture took or the GPU wrote, and counters. Its own mutex,
// taken last and never held over a call out: any thread may call in. Pure: no Vulkan, no renderer headers.
// Tests: tests/test_mem_check.py drives it with made-up events, one sequence per rule.
#pragma once
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mem_check {

enum Kind : int {
    kTakeOpen,          // (a capture taken while a recording is open)
    kCollectOpen,       // (the written pages collected while a recording is open)
    kSeqMismatch,       // (the capture taken is not the one the submission's submit made)
    kSyncOverGpu,       // (a capture's page copied over a newer GPU write, the page not late: words replaced. A loss, or
                        //  the CPU's write of the value it wrote before: see the top)
    kSyncOverGpuOnly,   // (the CPU's page copied over a page the GPU wrote through a pointer: words replaced, the same)
    kGenBack,           // (the generation counter went backwards: it wrapped)
    kKinds
};
inline const char* kind_name(Kind k) {
    static const char* const names[kKinds] = {"take_open", "collect_open", "seq_mismatch", "sync_over_gpu", "sync_over_gpu_only",
                                              "gen_back"};
    return names[k];
}
// (why a dirty page was read from live guest memory at a sync)
enum Live : int {
    kLiveNeverCaptured,   // (no capture ever held the page)
    kLiveWrittenSince,    // (written again since its capture: by the collection, or by the replay itself)
    kLiveRetired,         // (its capture is the newest, but the capture's buffer was let go)
    kLiveNotCopied,       // (its capture stamped it and copied nothing: no buffer, or more than the capture copies)
    kLives
};

// (what last wrote a page on the GPU, as the renderer's call sites say it; 0: not said)
enum Writer : uint8_t { kWriterUnknown, kWriterProgram, kWriters };
inline const char* writer_name(uint8_t w) {
    static const char* const names[kWriters] = {"a copy, a fill or another write on the GPU", "a storage buffer the program writes"};
    return names[w < kWriters ? w : 0];
}
// (a sync's page that may break a rule, as sync_page found it: handed back with the page's bytes for the verdict)
struct Cand {
    int mask = 0;              // (kCandGpu, kCandGpuOnly: which rule; kCandWatched: its bytes are followed)
    uint64_t gpu_value = 0;    // (the newest submission whose GPU work wrote the page)
    uint64_t cap_done = 0;     // (the GPU's progress when the capture was made)
    uint8_t writer = 0;        // (Writer, and its key: a program's)
    uint64_t writer_key = 0;
    bool gpu_stamp = false;    // (synced from live memory, dirty by the GPU's own stamp alone: no CPU write made it so)
};
enum : int { kCandGpu = 1, kCandGpuOnly = 2, kCandWatched = 4 };
// (why a candidate got no verdict)
enum Why : int {
    kWhyFirst,      // (the first sync of the page the check saw: nothing to compare its bytes with)
    kWhyPending,    // (too many read-backs waiting for the GPU at once)
    kWhyFollowed,   // (more pages than the check keeps the last synced bytes of)
    kWhyOther,      // (no bytes to copy from, no read-back buffer, no mirror)
    kWhys
};
// (how a sync_over_gpu candidate came to be one, by the write-backs recorded for its page when it is synced)
enum Path : int {
    kPathNoWriteBack,   // (no write-back of the page since the capture's view of the GPU: a written binding covers it, and
                        //  either the GPU did not write this page or its write-back is left out. Its words, if any, are in
                        //  the mirror alone)
    kPathInPlace,       // (the newest write-back is one the GPU made in guest memory in place)
    kPathMirror,        // (a mirror's write-back since the capture's view: a page the replay should have found late)
    kPaths
};
inline const char* path_name(Path p) {
    static const char* const names[kPaths] = {"no write-back since the capture", "written in place since the capture",
                                              "written back from a mirror since the capture, and not late"};
    return names[p];
}
constexpr size_t kPage = 4096;
/// The words of [lo, hi) of a page that a sync replaced with the value the page's last sync had given them: cur == prev,
/// and the mirror held something else. Either the GPU's words lost, or the CPU's writes of the same value as before:
/// the bytes do not say which. *first: the first one's offset
inline uint32_t replaced_words(const uint8_t* prev, const uint8_t* cur, const uint8_t* mirror, size_t lo, size_t hi, size_t* first) {
    uint32_t n = 0;
    for (size_t at = (lo + 3) & ~size_t(3); at + 4 <= hi && at + 4 <= kPage; at += 4) {
        if (std::memcmp(cur + at, prev + at, 4) != 0 || std::memcmp(mirror + at, cur + at, 4) == 0) continue;
        if (!n++ && first) *first = at;
    }
    return n;
}

class Check {
public:
    using LogFn = void (*)(const char* line);
    static constexpr uint64_t kLogFirst = 8;            // (cases logged of each kind)
    static constexpr uint64_t kSummaryEvery = 600;      // (submits a summary line)
    static constexpr uint64_t kMaxRangePages = 16384;   // (a GPU write of more than 64 MB is not noted page by page)
    static constexpr size_t kMaxTracked = size_t(1) << 21;   // (records kept; past it they are dropped and counted)
    static constexpr size_t kMaxWatched = 65536;             // (pages whose last synced bytes are kept: 256 MB)
    static constexpr size_t kMaxBig = 4096;                  // (GPU writes too large to note page by page, kept as ranges)

    explicit Check(LogFn log = nullptr) : log_(log) {}

    // ---- captures ----
    /// A capture was made and queued (the game's thread, or the replay's for one taken at once): its number, from 1
    uint64_t capture_made() {
        std::lock_guard lk(mu_);
        return ++made_;
    }
    /// A submission takes a capture. seq: the number of the one it got, 0 when the queue was empty. expected_known: the
    /// submission said which one its submit made (expected, 0: none). recording_open: a recording is being made.
    void take(uint64_t seq, bool expected_known, uint64_t expected, bool recording_open, uint64_t value) {
        locked([&] {
            takes_++;
            if (recording_open)
                violation(kTakeOpen, "a capture (#%llu) is taken while a recording is open, for submission %llu",
                          (unsigned long long)seq, (unsigned long long)value);
            if (!expected_known) {
                unpaired_++;
            } else if (seq != expected) {
                violation(kSeqMismatch, "submission %llu took capture #%llu, its own submit made #%llu (0: none)",
                          (unsigned long long)value, (unsigned long long)seq, (unsigned long long)expected);
            }
        });
    }
    /// The pages of the capture being taken: all it stamps (pages), those it holds a copy of (copied, page numbers), the
    /// late ones (late: indices into pages), and the GPU's progress when the capture was made (done)
    void take_pages(const uint32_t* pages, size_t n, const uint32_t* copied, size_t ncopied, const uint32_t* late, size_t nlate,
                    uint64_t done) {
        locked([&] {
            for (size_t i = 0; i < n; i++) {
                PageRec& r = rec(pages[i]);
                r.flags = (r.flags & ~(kCapLate | kCapNotCopied)) | kCaptured | kCapNotCopied;
                r.cap_done = done;
            }
            for (size_t i = 0; i < ncopied; i++) rec(copied[i]).flags &= ~kCapNotCopied;
            for (size_t i = 0; i < nlate; i++)
                if (late[i] < n) rec(pages[late[i]]).flags |= kCapLate;
        });
    }
    /// Pages of a capture found late only at the replay: written back from a mirror by a submission recorded after the
    /// capture was made (they are in take_pages' late list too)
    void late_at_replay(size_t n) {
        std::lock_guard lk(mu_);
        late_at_replay_ += n;
    }
    /// The written pages are collected (once a submission, after its take)
    void collect(bool recording_open, uint64_t value) {
        locked([&] {
            if (recording_open)
                violation(kCollectOpen, "the written pages are collected while a recording is open, for submission %llu",
                          (unsigned long long)value);
        });
    }

    // ---- the GPU's writes ----
    /// The GPU writes guest pages [page0, page1] in the submission with this timeline value (a written binding, a copy)
    /// writer, key: what wrote (Writer; a program's key). A range too large to note page by page is kept as a range
    /// gen: the generation the renderer stamped the pages with for this write (0: not said)
    void gpu_wrote(uint64_t page0, uint64_t page1, uint64_t value, uint8_t writer = kWriterUnknown, uint64_t key = 0, uint64_t gen = 0) {
        locked([&] {
            if (page1 < page0) return;
            if (page1 - page0 >= kMaxRangePages) {
                if (Big* b = big(page0, page1)) {
                    if (value >= b->gpu_value) b->gpu_value = value, b->writer = writer, b->writer_key = key;
                    if (gen > b->gpu_gen) b->gpu_gen = gen;
                }
                return;
            }
            for (uint64_t p = page0; p <= page1; p++) {
                PageRec& r = rec((uint32_t)p);
                if (value >= r.gpu_value) r.gpu_value = value, r.writer = writer, r.writer_key = key;
                if (gen > r.gpu_gen) r.gpu_gen = gen;
            }
        });
    }
    /// The GPU writes [page0, page1] through a pointer: never written back, the words are in the mirror alone
    void gpu_only(uint64_t page0, uint64_t page1) {
        locked([&] {
            if (page1 < page0) return;
            if (page1 - page0 >= kMaxRangePages) {
                if (Big* b = big(page0, page1)) b->gpu_only = true;
                return;
            }
            for (uint64_t p = page0; p <= page1; p++) rec((uint32_t)p).flags |= kGpuOnly;
        });
    }

    // ---- mirror syncs ----
    /// A mirror sync copies the CPU's version of a dirty page into the mirror of the pool at pool_lo: from a capture's
    /// buffer (from_capture), or from guest memory as it is now. What comes back says whether the page is a candidate for
    /// one of the two sync rules (the renderer then hands in its bytes: exchange, and judged or unjudged) and whether its
    /// bytes are followed (exchange alone). Nothing is counted as a break here: the bytes decide
    Cand sync_page(uint64_t page, bool from_capture, uint64_t pool_lo, uint64_t page_gen = 0) {
        (void)pool_lo;
        Cand c;
        std::lock_guard lk(mu_);
        syncs_++;
        if (last_.count((uint32_t)page)) c.mask |= kCandWatched;
        PageRec r;   // (the page's record, with what the large ranges over it say)
        if (const auto it = pages_.find((uint32_t)page); it != pages_.end()) r = it->second;
        for (const Big& b : big_) {
            if (page < b.page0 || page > b.page1) continue;
            if (b.gpu_only) r.flags |= kGpuOnly;
            if (b.gpu_value > r.gpu_value) r.gpu_value = b.gpu_value, r.writer = b.writer, r.writer_key = b.writer_key;
            if (b.gpu_gen > r.gpu_gen) r.gpu_gen = b.gpu_gen;
        }
        c.gpu_value = r.gpu_value, c.cap_done = r.cap_done, c.writer = r.writer, c.writer_key = r.writer_key;
        // (page_gen: the page's generation, the stamp that made it dirty. From live memory with the GPU's own stamp as its
        // newest: no capture took the page and nothing of the CPU's stamped it since, so the CPU did not write it)
        c.gpu_stamp = !from_capture && page_gen && page_gen == r.gpu_gen;
        if (r.flags & kGpuOnly) c.mask |= kCandGpuOnly, cands_[1]++;
        if (from_capture && (r.flags & kCaptured) && !(r.flags & kCapLate) && r.gpu_value > r.cap_done) c.mask |= kCandGpu, cands_[0]++;
        return c;
    }
    /// How a sync_over_gpu candidate came to be one (Path), by the renderer's write-back tables: counted, and the first of
    /// each path logged with what the renderer knows of the page. Not a break: the bytes decide that
    void candidate_path(uint64_t page, const Cand& c, uint64_t pool_lo, Path path, const char* what) {
        locked([&] {
            if (++paths_[path] > kLogFirst) return;
            pending_.push_back(format("vk: mem check: sync_over_gpu candidate (%s) #%llu: page %#llx (pool %#llx), written by %s "
                                      "(%016llx) in submission %llu, the capture made with the GPU at %llu; %s", path_name(path),
                                      (unsigned long long)paths_[path], (unsigned long long)(page << 12), (unsigned long long)pool_lo,
                                      writer_name(c.writer), (unsigned long long)c.writer_key, (unsigned long long)c.gpu_value,
                                      (unsigned long long)c.cap_done, what ? what : ""));
        });
    }
    /// A candidate in a sparse mirror's block with no memory: the GPU cannot have written there, so the sync loses
    /// nothing. Counted with the harmless ones, and apart
    void unbound(const Cand& c) {
        std::lock_guard lk(mu_);
        if (c.mask & kCandGpu) same_[0]++, unbound_[0]++;
        if (c.mask & kCandGpuOnly) same_[1]++, unbound_[1]++;
    }
    /// The bytes this sync copies into the page (cur, 4 KB) are kept, and the ones the last sync of it copied handed out
    /// (prev, 4 KB). 1: there were some; 0: none, the page's first time; -1: none, and cur not kept (more pages than are
    /// followed)
    int exchange(uint64_t page, const uint8_t* cur, uint8_t* prev) {
        std::lock_guard lk(mu_);
        auto it = last_.find((uint32_t)page);
        const bool had = it != last_.end();
        if (!had) {
            if (last_.size() >= kMaxWatched) return -1;
            it = last_.emplace((uint32_t)page, std::vector<uint8_t>(kPage)).first;
        } else {
            std::memcpy(prev, it->second.data(), kPage);
        }
        std::memcpy(it->second.data(), cur, kPage);
        return had ? 1 : 0;
    }
    /// The verdict on a candidate, by its bytes: prev (have_prev: exchange's answer) and cur as exchange gave and took them,
    /// mirror: the mirror's page as it was before the sync, [lo, hi): the part of the page the sync copied. what: what the
    /// renderer knows of the page, for the log
    void judged(uint64_t page, const Cand& c, uint64_t pool_lo, int have_prev, const uint8_t* prev, const uint8_t* cur,
                const uint8_t* mirror, size_t lo, size_t hi, const char* what) {
        locked([&] {
            for (int k = 0; k < 2; k++) {
                if (!(c.mask & (k ? kCandGpuOnly : kCandGpu))) continue;
                if (have_prev <= 0) {
                    unjudged_[k][have_prev < 0 ? kWhyFollowed : kWhyFirst]++;
                    continue;
                }
                size_t first = 0;
                const uint32_t lost = replaced_words(prev, cur, mirror, lo, hi, &first);
                if (!lost) {
                    same_[k]++;
                    continue;
                }
                uint32_t had = 0, got = 0;
                std::memcpy(&had, mirror + first, 4), std::memcpy(&got, cur + first, 4);
                if (c.gpu_stamp) certain_[k]++;
                const char* const which = c.gpu_stamp
                    ? "the page was dirty by the GPU's own stamp alone, no CPU write made it so: a loss"
                    : "a loss if the CPU did not write those words, its own write of the same value as before if it did (a counter "
                      "set back to zero): the bytes cannot tell";
                if (k)
                    violation(kSyncOverGpuOnly, "a sync of page %#llx (pool %#llx) replaced %u words of a page the GPU writes through a "
                              "pointer with the values the page's last sync had given them (first at +%#x: the mirror had %08x, the "
                              "copy has %08x); %s; %s", (unsigned long long)(page << 12), (unsigned long long)pool_lo, lost,
                              (unsigned)first, had, got, which, what ? what : "");
                else
                    violation(kSyncOverGpu, "a sync of page %#llx (pool %#llx) from a capture replaced %u words with the values the "
                              "page's last sync had given them (first at +%#x: the mirror had %08x, the capture has %08x); %s; the "
                              "page written by %s (%016llx) in submission %llu, the capture made with the GPU at %llu and the page "
                              "not late; %s", (unsigned long long)(page << 12), (unsigned long long)pool_lo, lost, (unsigned)first, had,
                              got, which, writer_name(c.writer), (unsigned long long)c.writer_key, (unsigned long long)c.gpu_value,
                              (unsigned long long)c.cap_done, what ? what : "");
            }
        });
    }
    /// A candidate whose bytes could not be had, and why: not judged
    void unjudged(const Cand& c, Why why) {
        std::lock_guard lk(mu_);
        if (c.mask & kCandGpu) unjudged_[0][why]++;
        if (c.mask & kCandGpuOnly) unjudged_[1][why]++;
    }
    /// The sync read the page from live guest memory. page_gen: the page's generation; entry_gen: its capture entry's (0:
    /// none); buffer_kept: that capture's buffer is still there
    void live_read(uint64_t page, uint64_t page_gen, uint64_t entry_gen, bool buffer_kept) {
        locked([&] {
            const auto it = pages_.find((uint32_t)page);
            const bool not_copied = it != pages_.end() && (it->second.flags & kCapNotCopied);
            Live why;
            if (entry_gen && entry_gen == page_gen) why = buffer_kept ? kLiveWrittenSince : kLiveRetired;
            else if (not_copied) why = kLiveNotCopied;
            else if (!entry_gen) why = kLiveNeverCaptured;
            else why = kLiveWrittenSince;
            lives_[why]++;
        });
    }

    // ---- once a submission ----
    /// A submission's memory work is done. gen_now: the generation counter; now_ns: a steady clock. Every kSummaryEvery
    /// calls the summary line is logged
    void submit(uint64_t gen_now, int64_t now_ns) {
        locked([&] {
            if (submits_ && gen_now < last_gen_)
                violation(kGenBack, "the generation counter went from %llu back to %llu: every page now reads as older than its "
                          "pool's sync", (unsigned long long)last_gen_, (unsigned long long)gen_now);
            last_gen_ = gen_now;
            if (!submits_) sum_gen_ = gen_now, sum_ns_ = now_ns;
            if (++submits_ % kSummaryEvery == 0) pending_.push_back(summary_locked(gen_now, now_ns));
        });
    }

    // ---- what was counted (the tests, and the summary) ----
    uint64_t count(Kind k) {
        std::lock_guard lk(mu_);
        return counts_[k];
    }
    uint64_t live(Live l) {
        std::lock_guard lk(mu_);
        return lives_[l];
    }
    uint64_t violations() {
        std::lock_guard lk(mu_);
        uint64_t n = 0;
        for (uint64_t c : counts_) n += c;
        return n;
    }
    /// The candidates of a sync rule (kSyncOverGpu or kSyncOverGpuOnly) so far, those judged harmless and those not judged
    uint64_t candidates(Kind k) {
        std::lock_guard lk(mu_);
        return cands_[k == kSyncOverGpuOnly];
    }
    uint64_t same(Kind k) {
        std::lock_guard lk(mu_);
        return same_[k == kSyncOverGpuOnly];
    }
    /// Of a rule's replaced ones, those on a page no CPU write made dirty (synced live, dirty by the GPU's stamp alone)
    uint64_t certain(Kind k) {
        std::lock_guard lk(mu_);
        return certain_[k == kSyncOverGpuOnly];
    }
    uint64_t not_judged(Kind k) {
        std::lock_guard lk(mu_);
        return unjudged_all(k == kSyncOverGpuOnly);
    }
    uint64_t not_judged(Kind k, Why why) {
        std::lock_guard lk(mu_);
        return unjudged_[k == kSyncOverGpuOnly][why];
    }
    uint64_t unbound_pages(Kind k) {
        std::lock_guard lk(mu_);
        return unbound_[k == kSyncOverGpuOnly];
    }
    uint64_t path_count(Path p) {
        std::lock_guard lk(mu_);
        return paths_[p];
    }
    uint64_t late_at_replay_pages() {
        std::lock_guard lk(mu_);
        return late_at_replay_;
    }
    uint64_t unpaired_takes() {
        std::lock_guard lk(mu_);
        return unpaired_;
    }
    size_t tracked() {
        std::lock_guard lk(mu_);
        return pages_.size();
    }
    /// The summary line as it would be logged now
    std::string summary(uint64_t gen_now, int64_t now_ns) {
        std::lock_guard lk(mu_);
        return summary_locked(gen_now, now_ns);
    }

private:
    static constexpr uint32_t kGpuOnly = 1, kCapLate = 2, kCapNotCopied = 4, kCaptured = 8;
    struct PageRec {
        uint64_t gpu_value = 0;   // (the newest submission whose GPU work wrote the page; 0: none noted)
        uint64_t cap_done = 0;    // (the GPU's progress when the capture that last took the page was made)
        uint64_t writer_key = 0;  // (what made that write: a program's key)
        uint32_t flags = 0;
        uint8_t writer = 0;       // (Writer)
        uint64_t gpu_gen = 0;     // (the generation of the renderer's newest stamp of the page for a GPU write)
    };

    PageRec& rec(uint32_t page) {
        if (pages_.size() >= kMaxTracked && !pages_.count(page)) pages_.clear(), table_resets_++;
        return pages_[page];
    }
    // (mu_ held) one more case of a kind: counted, and its line kept for the log while it is among the first
    void violation(Kind k, const char* fmt, ...) {
        if (++counts_[k] > kLogFirst) return;
        char text[400];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(text, sizeof text, fmt, ap);
        va_end(ap);
        std::string line = std::string("vk: mem check: ") + kind_name(k) + " #" + std::to_string(counts_[k]) + ": " + text;
        if (counts_[k] == kLogFirst) line += " (the last one logged of this kind; the summary line counts on)";
        pending_.push_back(std::move(line));
    }
    std::string summary_locked(uint64_t gen_now, int64_t now_ns) {
        const uint64_t dgen = gen_now >= sum_gen_ ? gen_now - sum_gen_ : 0;
        const double seconds = (double)(now_ns - sum_ns_) / 1e9;
        const double rate = seconds > 0 ? (double)dgen / seconds : 0;   // (generations a second)
        std::string text = format(
            "vk: mem check: %llu submits: take_open %llu, collect_open %llu, seq_mismatch %llu (%llu of %llu takes not paired); "
            "sync_over_gpu: %llu replaced (a loss or the CPU's reset; %llu on a page no CPU write made dirty), %llu same (%llu "
            "unbound), %llu not judged (of %llu candidates; not judged: %s; candidates "
            "by path: %llu no write-back, %llu in place, %llu mirror write-back not late); sync_over_gpu_only: %llu replaced (a loss "
            "or the CPU's reset; %llu on a page no CPU write made dirty), %llu "
            "same (%llu unbound), %llu not judged (of %llu candidates; not judged: %s); %llu pages synced, %llu pages late at the "
            "replay; gen_back %llu; live reads: %llu never captured, %llu written since their capture, %llu capture retired, %llu "
            "not copied by their capture; gen %llu (+%llu, %.0f a second); %llu pages tracked, %llu followed, %llu large ranges "
            "(%llu table resets, %llu ranges too large to note)",
            (unsigned long long)submits_, (unsigned long long)counts_[kTakeOpen], (unsigned long long)counts_[kCollectOpen],
            (unsigned long long)counts_[kSeqMismatch], (unsigned long long)unpaired_, (unsigned long long)takes_,
            (unsigned long long)counts_[kSyncOverGpu], (unsigned long long)certain_[0], (unsigned long long)same_[0],
            (unsigned long long)unbound_[0],
            (unsigned long long)unjudged_all(0), (unsigned long long)cands_[0], whys(0).c_str(),
            (unsigned long long)paths_[kPathNoWriteBack], (unsigned long long)paths_[kPathInPlace], (unsigned long long)paths_[kPathMirror],
            (unsigned long long)counts_[kSyncOverGpuOnly], (unsigned long long)certain_[1], (unsigned long long)same_[1],
            (unsigned long long)unbound_[1],
            (unsigned long long)unjudged_all(1), (unsigned long long)cands_[1], whys(1).c_str(), (unsigned long long)syncs_,
            (unsigned long long)late_at_replay_, (unsigned long long)counts_[kGenBack], (unsigned long long)lives_[kLiveNeverCaptured],
            (unsigned long long)lives_[kLiveWrittenSince], (unsigned long long)lives_[kLiveRetired],
            (unsigned long long)lives_[kLiveNotCopied], (unsigned long long)gen_now, (unsigned long long)dgen, rate,
            (unsigned long long)pages_.size(), (unsigned long long)last_.size(), (unsigned long long)big_.size(),
            (unsigned long long)table_resets_, (unsigned long long)ranges_skipped_);
        sum_gen_ = gen_now, sum_ns_ = now_ns;
        return text;
    }
    uint64_t unjudged_all(int k) const {
        uint64_t n = 0;
        for (uint64_t c : unjudged_[k]) n += c;
        return n;
    }
    std::string whys(int k) const {   // (the not-judged ones of a rule, by reason)
        return format("%llu first sync seen, %llu read-backs pending, %llu past the pages followed, %llu other",
                      (unsigned long long)unjudged_[k][kWhyFirst], (unsigned long long)unjudged_[k][kWhyPending],
                      (unsigned long long)unjudged_[k][kWhyFollowed], (unsigned long long)unjudged_[k][kWhyOther]);
    }
    // (a GPU write too large to note page by page: found by its ends, made while there is room)
    struct Big {
        uint64_t page0 = 0, page1 = 0, gpu_value = 0, writer_key = 0;
        uint8_t writer = 0;
        bool gpu_only = false;
        uint64_t gpu_gen = 0;
    };
    Big* big(uint64_t page0, uint64_t page1) {
        for (Big& b : big_)
            if (b.page0 == page0 && b.page1 == page1) return &b;
        if (big_.size() >= kMaxBig) {
            ranges_skipped_++;
            return nullptr;
        }
        big_.push_back(Big{page0, page1, 0, 0, 0, false});
        return &big_.back();
    }
    // (printf into a string: through a va_list, so a long line is cut at the buffer's end and never past it)
    static std::string format(const char* fmt, ...) {
        char text[1400];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(text, sizeof text, fmt, ap);
        va_end(ap);
        return text;
    }
    // (the body under mu_; the lines it made are logged after the lock is let go: the log takes locks of its own)
    template <class F> void locked(F&& body) {
        std::vector<std::string> lines;
        {
            std::lock_guard lk(mu_);
            body();
            lines.swap(pending_);
        }
        if (log_)
            for (const std::string& line : lines) log_(line.c_str());
    }

    std::mutex mu_;
    LogFn log_;
    std::unordered_map<uint32_t, PageRec> pages_;
    std::unordered_map<uint32_t, std::vector<uint8_t>> last_;   // (a followed page: the bytes its last sync copied)
    std::vector<std::string> pending_;
    uint64_t counts_[kKinds] = {};
    uint64_t lives_[kLives] = {};
    uint64_t made_ = 0, takes_ = 0, unpaired_ = 0, syncs_ = 0, submits_ = 0, table_resets_ = 0, ranges_skipped_ = 0;
    uint64_t cands_[2] = {}, same_[2] = {}, unbound_[2] = {}, late_at_replay_ = 0;   // ([0]: sync_over_gpu, [1]: sync_over_gpu_only)
    uint64_t unjudged_[2][kWhys] = {}, paths_[kPaths] = {};
    uint64_t certain_[2] = {};   // (replaced on a page no CPU write made dirty: gpu_stamp)
    std::vector<Big> big_;
    uint64_t last_gen_ = 0, sum_gen_ = 0;
    int64_t sum_ns_ = 0;
};

} // namespace mem_check
