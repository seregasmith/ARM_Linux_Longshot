# SPDX-License-Identifier: GPL-3.0-or-later
"""Warm-cache save-load pipeline lookup, play-cache init, and load-at-create indexing (gpu/vk/vk_draw.cpp, vk_programs.cpp).

Checks the behaviors behind save-load warm-cache stalls and skipped 3D draws (no game, GPU, or Vulkan loader):
  - Play-cache init: pipeline_cache_play() calls pcache_init() before play_cache_begin(), so play-cache generations and
    from-driver / from-start / compiled counters stay active even when precompile_run() was skipped (RX_VK_PRECOMPILE=0).
  - Create-time indexing: when !precompile_wanted() && programs_load_at_create() && !program_cache_kept_keys().empty(),
    pipeline_cache_play() runs the lightweight index-only path (start_note_refs, notes/state open, computes_plan,
    programs_index) with zero compilation; and programs_frames_note() / load_wait_note() never poison
    longshot_load_start.txt when !precompile_wanted().
  - No cache probe: a pipeline state new to play goes to the background build as on `main`, with no non-compiling ask of
    the caches first (that probe is left to its own change); a noted state the loader yielded to first use is still asked
    of its cache once, as on `main`.

python -I -B tests/test_saveload_warm_cache.py [--cxx g++] [--source-dir .]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from srccut import block   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#define _CRT_SECURE_NO_WARNINGS
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "compat/longshot_env.h"
#include "quick_pipe.h"

#define PROF_EXACT() ((void)0)
#define FINE_N(a, b) ((void)0)
#define FINE_X(a, b) ((void)0)
#define VK_NULL_HANDLE nullptr

using VkPipeline = void*;
using VkPipelineCache = void*;
using VkPrimitiveTopology = int;

template <class K, class V>
using FlatMap = std::unordered_map<K, V>;

struct {
    struct { bool pipelineCreationCacheControl = true; } vk13;
} dev;

struct PipeEntry {
    std::atomic<bool> ready{false};
    std::atomic<VkPipeline> pipeline{nullptr};
    std::atomic<uint8_t> how{0}, quick{0};
};

struct Program {
    uint64_t key = 0;
    uint64_t variant = 0;
    bool fallback = false;
    bool modules_dropped = false;
};

struct NvnState {
    uint32_t vattr[16]{};
    struct { uint32_t divisor = 0; } vstream[16]{};
    uint64_t blend[8]{};
    int nvattr = 0;
    struct { uint32_t blend_en = 0; } color{};
    struct { uint32_t mask = 0; } chmask{};
    struct { uint32_t mode = 0; } poly{};
    bool depth_clamp = false;
    struct { bool a2c = false; } ms{};
};

struct Targets {
    int n = 1;
    uint32_t samples = 1;
    uint32_t color_format[8]{};
    uint32_t depth_format = 0;
};

struct PipeFast {
    bool on = false;
    PipeEntry* pe = nullptr;
    uint64_t prog = 0, variant = 0;
    int topo = 0;
    uint32_t samples = 0, n = 0, depth = 0, nvattr = 0;
    uint32_t blend_en = 0, chmask = 0, poly = 0, clamp = 0, a2c = 0;
    uint32_t formats[8]{}, vattr[16]{}, div[16]{};
    uint64_t blend[8]{};
};

struct NoteRef {
    uint64_t key = 0, prog = 0, variant = 0;
    int64_t at = 0;
    uint32_t vattr[16]{};
    uint8_t color_types[8]{};
};

struct PipeNote {
    uint64_t key = 0;
};

struct ComputePlan {
    std::unordered_set<uint64_t> keys;
    bool none = false;
};

static thread_local PipeFast pipe_fast[2]{};
static thread_local bool ask_start_first = false;
thread_local bool gate_busy = false;
static thread_local bool slow_full_build = false;
static thread_local struct {
    int64_t pipes_new = 0;
    uint8_t made_how = 0;
} use_counters;

static struct {
    std::atomic<int64_t> pipes_taken{0};
    std::atomic<int64_t> load_pipes_cold{0};
    std::atomic<int64_t> load_computes_cold{0};
    std::atomic<int64_t> play_pipes_new{0};
    std::atomic<int64_t> play_from_start{0};
    std::atomic<int64_t> play_from_driver{0};
    std::atomic<int64_t> play_compiled{0};
} census;

static std::unordered_map<uint64_t, std::unique_ptr<PipeEntry>> pipelines;
static std::mutex kept_ready_mu;
static std::unordered_map<uint64_t, VkPipeline> kept_ready;
static std::atomic<size_t> kept_ready_n{0};

std::mutex note_mu, state_mu;
FILE* note_file = nullptr;
FILE* state_file = nullptr;
static std::unordered_set<uint64_t> noted;

static VkPipelineCache pcache = nullptr;
static bool stub_precompile_wanted = false;
static bool stub_load_at_create = true;
static bool stub_gpl = false;
static bool stub_fresh_note = false;
static bool stub_predicted = false;
static bool stub_probe_hits = true;
static bool stub_probe_busy = false;
static uint8_t stub_probe_how = 2;
static int stub_probe_sleep_ms = 0;
static uint64_t stub_frame_no = 1;
static int stub_pcache_init_calls = 0;
static int stub_play_cache_begin_calls = 0;
static bool stub_pcache_ready_at_play_begin = false;
static int stub_programs_play_begin_calls = 0;
static int stub_start_note_refs_calls = 0;
static bool stub_start_note_refs_take = false;
static int stub_notes_open_calls = 0;
static int stub_state_open_calls = 0;
static int stub_programs_index_calls = 0;
static size_t stub_programs_index_notes = 0;
static size_t stub_programs_index_computes = 0;
static std::unordered_set<uint64_t> stub_kept_keys;
static std::vector<NoteRef> stub_note_refs;
static ComputePlan stub_compute_plan;

int stub_build_calls = 0;
static int stub_build_probe_calls = 0;
[[maybe_unused]] static int stub_build_probe_ask_start_first = 0;
static int stub_jobs_pushed = 0;
[[maybe_unused]] static int stub_predicted_use_calls = 0;
[[maybe_unused]] static int stub_predicted_use_how = 0;
int64_t st_link_us = 0, st_links = 0;
static int fake_pipe_token = 1;

static int stub_marker_writes = 0;
static int stub_strike_writes = 0;
static int stub_lacked_writes = 0;

inline void logf(const char*, ...) {}

inline bool precompile_wanted() { return stub_precompile_wanted; }
inline bool programs_load_at_create() { return stub_load_at_create; }
inline std::unordered_set<uint64_t> program_cache_kept_keys() { return stub_kept_keys; }
inline std::vector<NoteRef> start_note_refs(bool take) {
    ++stub_start_note_refs_calls;
    stub_start_note_refs_take = take;
    return stub_note_refs;
}
inline void notes_open_locked(const std::vector<std::unique_ptr<PipeNote>>*) {
    ++stub_notes_open_calls;
    note_file = reinterpret_cast<FILE*>(1);
}
inline void state_open_locked() {
    ++stub_state_open_calls;
    state_file = reinterpret_cast<FILE*>(1);
}
inline const char* precompile_computes_which() { return "listed"; }
inline ComputePlan computes_plan(const char*, bool) { return stub_compute_plan; }
inline void programs_index(std::vector<NoteRef> notes, const std::unordered_set<uint64_t>& computes) {
    ++stub_programs_index_calls;
    stub_programs_index_notes = notes.size();
    stub_programs_index_computes = computes.size();
}
inline void pcache_init() {
    ++stub_pcache_init_calls;
    if (!pcache) pcache = reinterpret_cast<VkPipelineCache>(0x1234);
}
inline void play_cache_begin() {
    ++stub_play_cache_begin_calls;
    stub_pcache_ready_at_play_begin = (pcache != nullptr);
}
inline void programs_play_begin() { ++stub_programs_play_begin_calls; }

#ifdef __linux__
inline bool prepare_left_workers() { return true; }
namespace linux_pressure {
inline bool read_meminfo(uint64_t*, uint64_t*, uint64_t*, uint64_t*, bool*) { return false; }
inline uint64_t avail_reserve(const char*) { return 0; }
inline uint64_t commit_reserve(const char*) { return 0; }
inline uint64_t shortfall(uint64_t, uint64_t, uint64_t, uint64_t, bool) { return 0; }
inline bool should_drop_modules(uint64_t, uint64_t, const char*, const char*) { return false; }
}
inline size_t program_cache_drop_modules() { return 0; }
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#endif

inline int topology_class(VkPrimitiveTopology t) { return t; }
inline bool pipe_same(uint64_t, uint64_t, int, uint32_t, int, uint32_t, const uint32_t[8], int, const uint32_t[16],
                      const uint32_t[16], uint32_t, const uint64_t[8], uint32_t, uint32_t, uint32_t, uint32_t,
                      uint64_t, uint64_t, int, uint32_t, uint32_t, uint32_t, const uint32_t[8], uint32_t,
                      const uint32_t[16], const uint32_t[16], uint32_t, const uint64_t[8], uint32_t, uint32_t,
                      uint32_t, uint32_t) {
    return false;
}
inline uint64_t pipeline_key(const Program& p, const NvnState&, const Targets&, VkPrimitiveTopology topo) {
    return (p.key << 16) ^ (p.variant << 4) ^ (uint64_t)topo;
}
inline bool pipeline_note(uint64_t, const Program&, const NvnState&, const Targets&, VkPrimitiveTopology, const uint8_t*) {
    return stub_fresh_note;
}
inline bool predicted_key(uint64_t) { return stub_predicted; }
inline void predicted_use(int how) { ++stub_predicted_use_calls; stub_predicted_use_how = how; }
inline bool gpl_wanted() { return stub_gpl; }
inline bool program_modules_dropped(const Program& p) { return p.modules_dropped; }
inline bool program_cache_complete_graphics(const Program&) { return true; }
inline VkPipeline gpl_link(const Program&, const NvnState&, const Targets&, VkPrimitiveTopology, uint64_t[2]) { return nullptr; }
inline void gpl_build_cold(const Program&, const NvnState&, const Targets&, VkPrimitiveTopology, int, uint64_t) {}
inline uint64_t texture_frame_no() { return stub_frame_no; }
inline VkPipeline build_graphics(const Program& p, const NvnState&, const Targets&, VkPrimitiveTopology, int = -1, bool* cold = nullptr) {
    ++stub_build_calls;
    if (cold) {
        ++stub_build_probe_calls;
        if (ask_start_first) ++stub_build_probe_ask_start_first;
        if (stub_probe_sleep_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(stub_probe_sleep_ms));
        if (p.modules_dropped) return nullptr;
        if (stub_probe_busy) {
            gate_busy = true;
            *cold = true;
            return nullptr;
        }
        if (!stub_probe_hits) {
            *cold = true;
            return nullptr;
        }
        use_counters.made_how = stub_probe_how;
        return reinterpret_cast<VkPipeline>(static_cast<uintptr_t>(++fake_pipe_token));
    }
    use_counters.made_how = 3;
    return reinterpret_cast<VkPipeline>(static_cast<uintptr_t>(++fake_pipe_token));
}
inline void jobs_push(std::function<void()>, bool = false, std::atomic<bool>* = nullptr, std::function<void()> = {}) {
    ++stub_jobs_pushed;
}

// ---- Production blocks cut from gpu/vk/vk_draw.cpp ----
#include "graphics_pipeline.inc"
#include "pipeline_cache_play.inc"

// ---- Production blocks cut from gpu/vk/vk_programs.cpp ----
namespace load_default {
constexpr double kLongMs = 50.0;
constexpr double kStrikeMs = 500.0;
constexpr double kNowMs = 2000.0;
constexpr long long kNotInCacheMost = 10;
enum class Waits { kFine, kStrike, kMark };
inline bool counts(double ms) { return ms >= kLongMs; }
inline Waits waits_verdict(double long_ms, bool struck_before) {
    if (long_ms > kNowMs) return Waits::kMark;
    if (long_ms > kStrikeMs) return struck_before ? Waits::kMark : Waits::kStrike;
    return Waits::kFine;
}
inline bool caches_lacked(long long n) { return n > kNotInCacheMost; }
enum class Marker { kLacked };
inline bool marker_counts(Marker, uint32_t) { return true; }
inline bool strike_write(const std::string&, double) { ++stub_strike_writes; return true; }
inline bool marker_write(const std::string&, double, double) { ++stub_marker_writes; return true; }
inline bool marker_write_lacked(const std::string&, long long) { ++stub_lacked_writes; return true; }
} // namespace load_default

static std::string load_fallback_path() { return "longshot_load_start.txt"; }
static uint32_t ram_total_mb() { return 16384; }
static std::atomic<bool> struck_last_session{false};
static std::atomic<int64_t> wait_worst_ns{0}, wait_long_ns{0};
static std::atomic<bool> wait_struck{false};
static std::mutex marker_mu;
static bool wait_marked = false, lacked_marked = false;

#include "load_wait_note.inc"
#include "programs_frames_note.inc"

// ---- Production programs_index() cut from gpu/vk/vk_programs.cpp (tested in isolation for one-shot idempotency) ----
namespace prod_index {
using Clock = std::chrono::steady_clock;
struct IndexNote { int64_t at; uint64_t variant, key; };
struct Signature { uint32_t vattr[16]{}; uint8_t color_types[8]{}; };
static std::atomic<bool> index_ready{false};
static std::unordered_map<uint64_t, std::vector<IndexNote>> index_notes;
static std::vector<Signature> index_signatures;
static std::unordered_set<uint64_t> index_computes;
static std::unordered_set<uint64_t> index_kept;
static std::unordered_map<uint64_t, std::vector<uint64_t>> index_compute_variants;
static std::mutex jobs_mu;
static bool workers_started = false;
static inline uint32_t attribute_type_of(uint32_t a) { return a & 0xFFu; }
static inline std::vector<std::pair<uint64_t, uint64_t>> compute_variants_learned() { return {{0x9999, 7}}; }
static inline int workers_cap(bool) { return 0; }
static inline void load_worker(int) {}
struct PipeSource { const char* name() const { return "pipeline caches"; } };
static inline PipeSource pipeline_source() { return {}; }
static inline double ms_between(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}
#include "programs_index.inc"
} // namespace prod_index

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); ++fails; } } while (0)

static void reset_play_state() {
    pcache = nullptr;
    note_file = nullptr;
    state_file = nullptr;
    noted.clear();
    stub_pcache_init_calls = 0;
    stub_play_cache_begin_calls = 0;
    stub_pcache_ready_at_play_begin = false;
    stub_programs_play_begin_calls = 0;
    stub_start_note_refs_calls = 0;
    stub_start_note_refs_take = false;
    stub_notes_open_calls = 0;
    stub_state_open_calls = 0;
    stub_programs_index_calls = 0;
    stub_programs_index_notes = 0;
    stub_programs_index_computes = 0;
}

static void reset_pipe_counters() {
    stub_build_calls = 0;
    stub_build_probe_calls = 0;
    stub_build_probe_ask_start_first = 0;
    stub_jobs_pushed = 0;
    stub_predicted_use_calls = 0;
    stub_predicted_use_how = 0;
    census.play_from_start = 0;
    census.play_from_driver = 0;
    census.load_pipes_cold = 0;
    census.pipes_taken = 0;
    stub_probe_sleep_ms = 0;
    stub_probe_busy = false;
}

static void set_env_var(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value && *value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

int main() {
    // 1. Play-cache init + create-time index: precompile_wanted()==false, warm kept keys -> pcache_init runs before
    //    play_cache_begin, and programs_index runs.
    reset_play_state();
    stub_precompile_wanted = false;
    stub_load_at_create = true;
    stub_kept_keys = {0x1111, 0x2222};
    stub_note_refs = {NoteRef{101, 0x1111, 1, 16, {}, {}}, NoteRef{102, 0x2222, 2, 32, {}, {}}};
    stub_compute_plan = {{0x9999}, false};
    pipeline_cache_play();
    CHECK(stub_pcache_init_calls == 1);
    CHECK(stub_play_cache_begin_calls == 1);
    CHECK(stub_pcache_ready_at_play_begin);
    CHECK(stub_start_note_refs_calls == 1 && stub_start_note_refs_take);
    CHECK(stub_notes_open_calls == 1 && noted.count(101) && noted.count(102));
    CHECK(stub_state_open_calls == 1);
    CHECK(stub_programs_index_calls == 1 && stub_programs_index_notes == 2 && stub_programs_index_computes == 1);
    CHECK(stub_programs_play_begin_calls == 1);

    // 1b. RX_VK_NO_PROGRAM_CACHE=1 -> pcache_init + play_cache_begin still run, indexing & state/notes open are skipped.
    reset_play_state();
    stub_precompile_wanted = false;
    stub_load_at_create = true;
    stub_kept_keys = {0x1111, 0x2222};
    set_env_var("RX_VK_NO_PROGRAM_CACHE", "1");
    pipeline_cache_play();
    set_env_var("RX_VK_NO_PROGRAM_CACHE", nullptr);
    CHECK(stub_pcache_init_calls == 1 && stub_pcache_ready_at_play_begin);
    CHECK(stub_start_note_refs_calls == 0);
    CHECK(stub_notes_open_calls == 0 && stub_state_open_calls == 0);
    CHECK(stub_programs_index_calls == 0);
    CHECK(stub_programs_play_begin_calls == 1);

    // 1c. programs_index() one-shot guard: calling programs_index() twice does not duplicate index_notes,
    //     index_signatures, or index_compute_variants.
    {
        std::vector<NoteRef> n1 = {NoteRef{101, 0x1111, 1, 16, {}, {}}, NoteRef{102, 0x2222, 2, 32, {}, {}}};
        std::unordered_set<uint64_t> c1 = {0x9999};
        prod_index::programs_index(n1, c1);
        CHECK(prod_index::index_ready.load());
        CHECK(prod_index::index_notes[0x1111].size() == 1);
        CHECK(prod_index::index_notes[0x2222].size() == 1);
        CHECK(prod_index::index_signatures.size() == 1);
        CHECK(prod_index::index_compute_variants[0x9999].size() == 1);
        // Second call must be a no-op.
        prod_index::programs_index(n1, c1);
        CHECK(prod_index::index_notes[0x1111].size() == 1);
        CHECK(prod_index::index_notes[0x2222].size() == 1);
        CHECK(prod_index::index_signatures.size() == 1);
        CHECK(prod_index::index_compute_variants[0x9999].size() == 1);
    }

    // 2. Cold cache: empty kept keys -> pcache_init + play_cache_begin still run, programs_index is skipped.
    reset_play_state();
    stub_precompile_wanted = false;
    stub_load_at_create = true;
    stub_kept_keys.clear();
    pipeline_cache_play();
    CHECK(stub_pcache_init_calls == 1 && stub_pcache_ready_at_play_begin);
    CHECK(stub_programs_index_calls == 0);

    // 3. When precompile_wanted()==true: pipeline_cache_play does NOT re-run programs_index.
    reset_play_state();
    stub_precompile_wanted = true;
    stub_load_at_create = true;
    stub_kept_keys = {0x1111};
    pipeline_cache_play();
    CHECK(stub_pcache_init_calls == 1 && stub_pcache_ready_at_play_begin);
    CHECK(stub_programs_index_calls == 0);

    // 4. Safety-net guard: RX_VK_PRECOMPILE=0 (!precompile_wanted) must NOT poison longshot_load_start.txt or strike.
    stub_precompile_wanted = false;
    stub_load_at_create = true;
    census.load_pipes_cold = 500;
    census.load_computes_cold = 50;
    wait_worst_ns = 0; wait_long_ns = 0; wait_marked = false; lacked_marked = false; wait_struck = false;
    stub_marker_writes = 0; stub_strike_writes = 0; stub_lacked_writes = 0;
    programs_frames_note();
    load_wait_note(3000LL * 1000000LL);
    CHECK(stub_lacked_writes == 0);
    CHECK(stub_marker_writes == 0 && stub_strike_writes == 0);

    // ... while precompile_wanted()==true still triggers the safety net normally.
    stub_precompile_wanted = true;
    programs_frames_note();
    CHECK(stub_lacked_writes == 1);
    wait_marked = false; wait_struck = false; wait_long_ns = 0;
    load_wait_note(3000LL * 1000000LL);
    CHECK(stub_marker_writes == 1);

    // 5. A pipeline state new to play: no ask of the caches before the background build (no probe), the entry not ready.
    NvnState s{};
    Targets at{};
    uint8_t types[8]{};
    reset_pipe_counters();
    dev.vk13.pipelineCreationCacheControl = true;
    stub_probe_hits = true;
    stub_fresh_note = false;
    stub_predicted = false;
    Program p1{0x10, 1, false, false};
    PipeEntry* e1 = graphics_pipeline(p1, s, at, 3, types);
    CHECK(e1 && !e1->ready.load() && e1->pipeline.load() == nullptr);
    CHECK(stub_build_probe_calls == 0 && stub_jobs_pushed == 1);
    CHECK(census.play_from_start.load() == 0 && census.play_from_driver.load() == 0);

    // 6. A noted state the loader yielded to first use (main's own path): asked of its cache once; cold, it goes to the
    //    background build.
    reset_pipe_counters();
    stub_probe_hits = false;
    Program p2{0x16, 1, false, false};
    const uint64_t k2 = pipeline_key(p2, s, at, 3);
    kept_ready[k2] = nullptr;
    kept_ready_n.store(kept_ready.size(), std::memory_order_release);
    PipeEntry* e2 = graphics_pipeline(p2, s, at, 3, types);
    CHECK(e2 && !e2->ready.load());
    CHECK(stub_build_probe_calls == 1 && census.load_pipes_cold.load() == 1 && stub_jobs_pushed == 1);

    if (fails) {
        std::fprintf(stderr, "FAIL: %d checks failed\n", fails);
        return 1;
    }
    std::printf("PASS: play-cache init, load-at-create index & marker guard, no cache probe\n");
    return 0;
}
"""


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++") or "g++")
    p.add_argument("--source-dir", type=Path, default=ROOT, help="repo root to cut gpu/vk sources from")
    args = p.parse_args()

    src_root = args.source_dir
    draw = (src_root / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    progs = (src_root / "gpu/vk/vk_programs.cpp").read_text(encoding="utf-8")

    # Quick source invariant: quick_pipeline() stays untouched so test_quick_pipelines.py's required-only contract holds.
    qp = block(draw, "bool quick_pipeline(PipeEntry& e,")
    if "if (budget_ms <= 0 || program_modules_dropped(p)) return false;" not in qp:
        raise SystemExit("FAIL: quick_pipeline() required-only budget guard was altered")

    gp_inc = block(draw, "PipeEntry* graphics_pipeline(")
    pcp_inc = block(draw, "void pipeline_cache_play() {")
    lwn_inc = block(progs, "void load_wait_note(int64_t ns) {")
    pfn_inc = block(progs, "void programs_frames_note() {")
    pi_inc = block(progs, "void programs_index(")

    with tempfile.TemporaryDirectory(prefix="saveload-warm-cache-") as tmp:
        tmp = Path(tmp)
        (tmp / "graphics_pipeline.inc").write_text(gp_inc + "\n", encoding="utf-8")
        (tmp / "pipeline_cache_play.inc").write_text(pcp_inc + "\n", encoding="utf-8")
        (tmp / "load_wait_note.inc").write_text(lwn_inc + "\n", encoding="utf-8")
        (tmp / "programs_frames_note.inc").write_text(pfn_inc + "\n", encoding="utf-8")
        (tmp / "programs_index.inc").write_text(pi_inc + "\n", encoding="utf-8")
        cpp = tmp / "fixture.cpp"
        exe = tmp / ("fixture.exe" if os.name == "nt" else "fixture")
        cpp.write_text(FIXTURE, encoding="utf-8")
        cmd = [args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-pthread",
               "-I", str(tmp), "-I", str(src_root / "gpu/vk"), str(cpp), "-o", str(exe)]
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        if res.returncode != 0:
            sys.stderr.write(res.stdout + res.stderr)
            return 1
        env = {k: v for k, v in os.environ.items()
               if k not in ("RX_VK_SYNC_PIPELINES", "LONGSHOT_PREPARE_ONLY", "RX_VK_NO_PROGRAM_CACHE", "RX_VK_NO_STATE_LIST")}
        if os.name == "nt":
            cxx_dir = str(Path(args.cxx).resolve().parent)
            if Path(cxx_dir).is_dir():
                env["PATH"] = cxx_dir + os.pathsep + env.get("PATH", "")
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60, env=env)
        sys.stdout.write(run.stdout)
        if run.returncode != 0:
            sys.stderr.write(run.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
