// The first start after a preparation leaves the warm out where the caches already answer the notes (gpu/vk/vk_draw.cpp
// notes_answered_by_caches and precompile_run's start, cut out by tests/test_warm_check.py into the .inc files included
// below, and run here on stand-ins for the caches, the programs and the marker). No game, no GPU.
//   warm_check_test <scenario>   (one a process: the switch is read once, as in the game)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#if __has_include("../gpu/vk/warm_check.h")
#include "../gpu/vk/warm_check.h"
#else   // (before the check: the sample's sizes alone, so that the run shows the warm made whatever the caches hold)
namespace warm_check { constexpr size_t kNotes = 256, kComputes = 64; }
#endif

// ---- stand-ins (the renderer's, as far as the cut functions use them) ----
typedef struct VkPipeline_T* VkPipeline;
#define VK_NULL_HANDLE nullptr
struct Dev {
    struct { bool pipelineCreationCacheControl = true; } vk13;
    void* device = nullptr;
} dev;
struct Stage { void* module = (void*)1; };
struct Program {
    uint64_t key = 0, variant = 0;
    bool compute = false, failed = false;
    std::vector<Stage> stages{Stage{}};
};
struct PipeNote { uint64_t prog = 0, variant = 0; };
struct NoteRef { uint64_t key = 0, prog = 0, variant = 0; int64_t at = 0; };

static std::vector<NoteRef> refs_;
static std::vector<uint64_t> computes_;
static std::map<uint64_t, Program> kept_;          // (the program cache: each key's kept variant)
static std::map<uint64_t, bool> cold_;             // (what the caches cannot answer without a compile, by program)
static int asked = 0, made = 0, destroyed = 0, warmed = 0, at_create = 0, rest = 0;
static std::string notes_path_ = "notes.bin";

static bool env_on(const char* name) { const char* v = getenv(name); return v && *v && strcmp(v, "0") != 0; }
static void logf(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    printf("\n");
}
static bool prepare_stopped() { return false; }
static void pcache_init() {}
static void ensure_layout(Program&) {}
static bool taau_key(uint64_t) { return false; }
struct StartNoteRefs {};
static StartNoteRefs start_note_refs_kept;
static std::vector<NoteRef> start_note_refs(bool) { return refs_; }
static std::vector<uint64_t> program_cache_compute_keys() { return computes_; }
static std::string pipe_notes_path() { return notes_path_; }
static bool pipe_note_at(FILE* f, int64_t at, PipeNote* out) {
    if (!f || at < 0 || (size_t)at >= refs_.size()) return false;
    out->prog = refs_[(size_t)at].prog, out->variant = refs_[(size_t)at].variant;
    return true;
}
static void program_cache_borrow(uint64_t key, const std::function<void(Program&)>& fn) {
    auto it = kept_.find(key);
    if (it == kept_.end()) return;
    Program p = it->second;
    fn(p);
}
static VkPipeline handle() { return (VkPipeline)(uintptr_t)(0x1000 + 8 * ++made); }
// (asked as a load at a create call asks: never a compile; a pipeline the caches lack is "cold")
static VkPipeline pipeline_from_caches(const Program& p, const PipeNote&, bool* cold) {
    asked++;
    *cold = cold_[p.key];
    return *cold ? VK_NULL_HANDLE : handle();
}
static VkPipeline compute_pipeline_from_caches(const Program& p) {
    asked++;
    return cold_[p.key] ? VK_NULL_HANDLE : handle();
}
static void vkDestroyPipeline(void*, VkPipeline, const void*) { destroyed++; }
// (the marker, precompile_built_path's file, in the folder the test runs in)
static std::string precompile_built_path() { return "notes.bin.built"; }
static bool precompile_covers_notes() {
    FILE* f = fopen(precompile_built_path().c_str(), "rb");
    if (f) fclose(f);
    return f != nullptr;
}
static void precompile_built_write(size_t, uint64_t) {
    if (env_on("LONGSHOT_PREPARE_ONLY")) return;
    if (FILE* f = fopen(precompile_built_path().c_str(), "wb")) fclose(f);
}
// (the compute programs the warm builds: every kept one here, compute_warm_plan's "all")
struct ComputePlan {
    std::set<uint64_t> keys;
    bool all = true, none = false;
    uint64_t coverage() const { return 1; }
};
static const ComputePlan& compute_warm_plan() { static const ComputePlan plan; return plan; }
// (the start's own steps around the pass: the pipeline cache opened, the texture pipelines, the bar's count)
struct ParLock { ParLock() {} ~ParLock() {} };   // (a lock: not here)
static void texture_pipelines_warm() {}
static bool pcache_file_rejected = false;
static size_t program_cache_fallback_steps() { return 0; }
namespace start_steps {
static void begin(void (*)(size_t, size_t), bool, size_t) {}
static void warm(size_t, size_t) {}
static void create(size_t, size_t) {}
static void end() {}
}
static bool programs_load_at_create() { return true; }
static bool pcache_drop_stale() { return false; }
static bool notes_lost_by_driver() { return false; }
static void precompile_warm(void (*)(size_t, size_t)) {
    warmed++;
    precompile_built_write(refs_.size(), 1);   // (a warm that got to its end, in a process that plays: warm_bounded.h leaves_marker)
}
static void precompile_at_create(void (*)(size_t, size_t)) { at_create++; }

#include "notes_answered.inc"
#include "precompile_run_start.inc"
    rest++;   // (the rest of precompile_run: the start that keeps everything ready)
}

// ---- the scenarios ----
static int failures = 0;
static void expect(bool ok, const char* what) {
    if (!ok) printf("FAIL: %s (asked %d, made %d, destroyed %d, warmed %d, at create %d)\n", what, asked, made, destroyed, warmed,
                    at_create), failures++;
}
static void world(size_t notes, size_t computes) {
    if (FILE* f = fopen(notes_path_.c_str(), "wb")) fclose(f);
    for (size_t i = 0; i < notes; i++) {
        const uint64_t prog = 100 + i % 97;   // (several notes a program, as in the game)
        refs_.push_back(NoteRef{0x9000 + i, prog, 1, (int64_t)i});
        kept_[prog] = Program{prog, 1, false, false, {Stage{}}};
    }
    for (size_t i = 0; i < computes; i++) {
        const uint64_t key = 5000 + i;
        computes_.push_back(key);
        kept_[key] = Program{key, 0, true, false, {Stage{}}};
    }
}

int main(int argc, char** argv) {
    const std::string s = argc > 1 ? argv[1] : "";
    remove(precompile_built_path().c_str());
    if (s == "answered") {   // (the preparation's cache file answers every note and compute program: no warm)
        world(3000, 400);
        precompile_run(nullptr);
        expect(warmed == 0, "the warm ran though the caches answer every note and compute program asked");
        expect(at_create == 1 && precompile_covers_notes(), "the marker is not written, or the start does not load at create calls");
        expect(asked == (int)(warm_check::kNotes + warm_check::kComputes), "the sample is not 256 notes and 64 compute programs");
        expect(destroyed == made, "a pipeline asked for the sample is not let go");
    } else if (s == "few") {   // (fewer notes and computes than the sample: each asked once)
        world(10, 3);
        precompile_run(nullptr);
        expect(warmed == 0 && at_create == 1 && asked == 13, "a small set of notes is not asked whole, or not taken as answered");
    } else if (s == "cold_note" || s == "cold_compute" || s == "cold_last") {   // (one the caches lack: the warm runs)
        world(3000, 400);
        if (s == "cold_note") cold_[100 + 1234 % 97] = true;
        if (s == "cold_compute") cold_[5000 + 200] = true;   // (the 33rd of the 64 asked)
        if (s == "cold_last") cold_[100 + (3000 * 255 / 256) % 97] = true;
        precompile_run(nullptr);
        expect(warmed == 1, "the warm was left out though the caches lack one asked");
        expect(at_create == 1, "the start does not load at create calls after the warm");
        expect(destroyed == made, "a pipeline asked for the sample is not let go");
    } else if (s == "other_variant") {   // (most notes' programs kept in another variant: too little asked to say)
        world(3000, 0);
        for (auto& kv : kept_) kv.second.variant = 2;
        kept_[100].variant = 1;
        precompile_run(nullptr);
        expect(warmed == 1, "the warm was left out on a sample that could hardly be asked");
    } else if (s == "no_notes") {
        world(0, 50);
        precompile_run(nullptr);
        expect(warmed == 1, "with no notes the check answers for them");
    } else if (s == "switch_off") {   // (RX_VK_WARM_CHECK=0: warmed as before, nothing asked)
        world(3000, 400);
        precompile_run(nullptr);
        expect(warmed == 1 && asked == 0, "RX_VK_WARM_CHECK=0 does not warm as before");
    } else if (s == "prepare_only") {   // (the preparation's own process: warms, asks nothing, leaves no marker, goes no further)
        world(3000, 400);
        precompile_run(nullptr);
        expect(warmed == 1 && asked == 0 && !precompile_covers_notes() && at_create == 0 && rest == 0,
               "a --prepare-shaders run asks the caches, leaves the marker or goes on after its warm");
    } else if (s == "marker") {   // (the marker there: nothing asked, no warm)
        world(3000, 400);
        precompile_built_write(0, 1);
        precompile_run(nullptr);
        expect(warmed == 0 && asked == 0 && at_create == 1, "a start with the marker asks the caches or warms");
    } else if (s == "no_cache_control") {   // (no asking without a compile on this driver: warmed as before)
        world(3000, 400);
        dev.vk13.pipelineCreationCacheControl = false;
        precompile_run(nullptr);
        expect(warmed == 1 && asked == 0, "without pipelineCreationCacheControl the caches are asked");
    } else {
        printf("unknown scenario %s\n", s.c_str());
        return 2;
    }
    remove(precompile_built_path().c_str());
    remove(notes_path_.c_str());
    if (failures) return 1;
    printf("ok %s\n", s.c_str());
    return 0;
}
