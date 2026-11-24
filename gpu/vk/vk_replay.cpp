// The NVN replay on Vulkan: the game's recorded command buffers (host/nvn_native.c, records in rx_nvn.h) at each
// submit, recorded into a Vulkan command buffer and submitted with the next value of a timeline semaphore (the NVN
// fences); and the window presents.
//
// Each recording has two command buffers: the replay's, and one submitted right before it that texture uploads and
// new images' layout changes go to (vk_texture.cpp), so no pass has to end for them. The stream's memory writes
// happen on the GPU timeline: guest memory is shared with the GPU, so a CPU write at replay time would reach draws
// recorded before it. Uniform updates (UPDATE_UB) are written at the end of their recording, and the draws after
// one bind a copy of the range with it in (ub_overlay), so a pass need not end for them either.
//
// Without a window (HOS_HEADLESS) presents render into an image of their own; RX_VK_SHOTS=<dir> writes every
// RX_VK_SHOT_EVERY-th one (default 300) there as a PNG.
#include <algorithm>
#include <sys/stat.h>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <exception>
#include <thread>
#include <string>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "vk_temporal.h"
#include "vk_fsr3_inputs.h"   // (the camera jitter while AMD FSR 3.1 upscales)
#include "status_line.h"
#include "vk_reflex.h"
#ifdef TRACY_ENABLE
#include "../../host/hos_tracy.h"   // (LONGSHOT_TRACY: the profiler's zones; nothing in any other build)
#endif
#include "vk_antilag.h"
#include "vk_draw.h"
#include "mesh_conv.h"   // (RX_VK_MESH_CONV=2: the indirect mesh draws after a barrier of the game's, scanned there)
#include "vk_dump.h"
#include "vk_paths.h"   // (log_path: the frame trace in the game folder's logs/)
#include "vk_gprof.h"
#include "vk_framegen.h"
#include "vk_dlss.h"   // (longshot: dlss_fg_idle)
#include "vk_fps.h"
#include "perf_report.h"
#include "vk_menu.h"
#include "reshade/fx_runtime.h"
#include "vk_jobs.h"
#include "vk_shader.h"
#include "vk_state.h"
#include "vk_texture.h"
#include "vk_xfer.h"
#include "vk_present_rect.h"
#include "vk_present_trace.h"
#include "colour_vision.h"
#include "vk_colour_vision.h"

#include "vk_internal.h"
#ifndef LONGSHOT_ENV_H   // (env_on: the switches' one rule)
#include "longshot_env.h"
#endif
#include "vk_programs.h"
#include "readback_late.h"
#include "cxx_escape.h"   // (the replay's threads: their exceptions said, a start that fails at the commit limit stops as out of memory)
#include "hold_gpu.h"   // (longshot: sync_watch's rule for queues stopped past hold_ms)
#include "frame_overlap.h"   // (longshot: when the game's frame wait is answered, and where the presents are made)
#include "submit_retry.h"    // (longshot, #1879: a submit refused for want of memory made again)
#include "shader_recompiler/backend/spirv/mma_layout.h"   // (graphics_table_loads, mesh_fallback_pages: replay_start)
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)

namespace mkwvk {
extern std::recursive_mutex menu_mu;
extern std::atomic<bool> movie_networks_slow;   // (vk_draw.cpp: the movie networks left out)
int64_t presents_now;   // (the presents so far, for SlowWatch)
/// (longshot) The last present confirmed *shown* by VK_KHR_present_wait, in microseconds from vkQueuePresentKHR to the
/// frame reaching the screen: written by the presenter thread, taken by the frame trace. -1 when no present was waited
/// for (no extension, or not FIFO), so a row says whether it has a presentation clock at all
std::atomic<int64_t> shown_wait_us{-1};
/// (longshot) The host clock at which the last present was confirmed *shown*, for the frame trace's single
/// presentation clock: 0 when the driver gave no confirmation. Same gate as shown_wait_us
std::atomic<int64_t> shown_at_ns{0};
/// (longshot) Running total and count of present-to-shown confirmations, drained by perf_report.cpp into the next
/// summary so the relay can show the presentation clock in the field, not only in the local frame-trace CSV
std::atomic<int64_t> shown_sum_us{0}, shown_count{0};
void take_shown_samples(double& sum_ms, int64_t& count) {
    const int64_t total = shown_sum_us.exchange(0);
    count = shown_count.exchange(0);
    sum_ms = total / 1000.0;
}
void hang_watch_start();   // (EvilOOT, vk_hangdump.cpp: RX_VK_HANG_DUMP)
void gpu_hang_watch_start();   // (below: a GPU that stops finishing work, reported and ended)
struct CondNote { int64_t present; uint64_t addr, x, y; int mode; bool on; };   // (the last render conditions, for a dark frame)
CondNote cond_ring[1 << 15];
uint32_t cond_ring_at = 0;
namespace {

// ---- submissions: a timeline semaphore; value n = the n-th submit done ----
VkSemaphore timeline = VK_NULL_HANDLE;
std::atomic<uint64_t> timeline_sent{0};   // (the highest value a submission already in the queue signals: gpu_hang_watch_start)
void note_timeline_sent(uint64_t v) {
    uint64_t was = timeline_sent.load(std::memory_order_relaxed);
    while (v > was && !timeline_sent.compare_exchange_weak(was, v, std::memory_order_relaxed)) {}
}
// ---- (longshot, #1879) a submit the driver refused for want of memory: memory given back and the submit made again
// (submit_retry.h). The replay's vkQueueSubmit2 calls go through submit_or_retry, each under dev.present_mu ----
bool submit_retry_on() {
    static const bool on = [] {
        const bool v = env_on_or("RX_VK_SUBMIT_RETRY", 1);   // (longshot_env.h: unset is on)
        if (!v) logf("vk: a submit refused for want of memory is dropped, as before (RX_VK_SUBMIT_RETRY=0)");
        return v;
    }();
    return on;
}
/// (a test: RX_VK_TEST_SUBMIT_REFUSE=<n> answers the first try of every n-th submit with VK_ERROR_OUT_OF_DEVICE_MEMORY without
/// making it, as AMD's Windows driver answered a Legion Go's, so any card plays the refusal and what follows it. Unset: never)
bool test_submit_refused() {
    static const uint64_t every = getenv("RX_VK_TEST_SUBMIT_REFUSE") ? (uint64_t)std::max(0, atoi(getenv("RX_VK_TEST_SUBMIT_REFUSE"))) : 0;
    static uint64_t submits = 0;   // (dev.present_mu)
    return every && ++submits % every == 0;
}
/// What a refused submit does before it is made again (dev.present_mu held, so the queue is this thread's): it waits for the
/// GPU's work before it, so nothing in flight holds memory resident, and tells the next present (texture_memory_refused) to
/// free the idle staging arenas and the reaper's queue. Not here: under the queue's lock nothing may be destroyed (a destroy's
/// rec::boundary can wait for the recorder, whose part's submit waits for this lock), and the arenas are the replay thread's
void submit_give_back(int tried, VkResult res) {
    const auto t0 = std::chrono::steady_clock::now();
    vkQueueWaitIdle(dev.queue);
    texture_memory_refused();
    static std::atomic<int> told{0};
    if (told.fetch_add(1, std::memory_order_relaxed) < 16)
        logf("vk: a submit refused for want of memory (%d): the GPU's work before it waited for (%.1f ms), the idle staging and "
             "the reaper's queue freed at the next present; made again (try %d of %d)", (int)res,
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), tried + 1, submit_retry::kTries);
}
/// vkQueueSubmit2 of one submit (dev.present_mu held), made again after submit_give_back while the driver refuses it for want
/// of memory (submit_retry.h)
VkResult submit_or_retry(const VkSubmitInfo2& si, VkFence fence) {
    int tries = 0;
    VkResult last = VK_SUCCESS;
    const VkResult res = submit_retry::submit(
        [&] {
            const bool refuse = !tries++ && test_submit_refused();
            return last = refuse ? VK_ERROR_OUT_OF_DEVICE_MEMORY : vkQueueSubmit2(dev.queue, 1, &si, fence);
        },
        [&](int tried) { submit_give_back(tried, last); }, [] { return submit_retry_on(); });
    if (tries > 1) {
        static std::atomic<int> told{0};
        if (told.fetch_add(1, std::memory_order_relaxed) < 16)
            logf("vk: the refused submit %s on try %d", res == VK_SUCCESS ? "was taken" : "was refused again, dropped", tries);
    }
    return res;
}
// ---- (longshot) recordings by number: a second timeline, signalled at the end of every recording with its number, and the
// recording each submission was finished in. The timeline above is signalled only as far as no older submission is stopped
// at a wait, and every NVN queue's submissions share its numbers: a fence taken on one queue waited for the other queues'
// stopped work too, and a wait for it forced that work past the waits it stopped at (OoT's presentation work run ahead of
// the scene work it waits for: a frame of the fog, haze and sun shafts of the frame before). NVN's fences are per queue: a
// wait for one is a wait for that queue's work, here for the recordings it was finished in. RX_VK_PER_QUEUE_FENCES=0: as
// before ----
VkSemaphore rec_timeline = VK_NULL_HANDLE;
uint64_t rec_seq = 0;                       // (the recordings submitted so far; replay_mu)
// (RX_VK_RECORDER_PARTS) the highest number on the recordings' timeline whose submit is made, or held with RX_VK_BATCH: a part's
// submit is the recorder's, after the render thread has put its recording where the waiters find it (map_recording)
std::atomic<uint64_t> rec_seq_sent{0};
void note_seq_sent(uint64_t seq) {
    uint64_t was = rec_seq_sent.load(std::memory_order_relaxed);
    while (seq > was && !rec_seq_sent.compare_exchange_weak(was, seq, std::memory_order_release, std::memory_order_relaxed)) {}
}
/// (RX_VK_RECORDER_PARTS) Until recording seq's submit is made: before a wait on the recordings' timeline for it, which with
/// RX_VK_BATCH must find it in the list of held submits that the wait's flush sends; at most timeout_ns, which it leaves at what
/// is left of it, and whether it is. At once without the hand-off by part
bool wait_seq_submitted(uint64_t seq, uint64_t& timeout_ns) {
    return rec::wait_submitted([seq] { return rec_seq_sent.load(std::memory_order_acquire) >= seq; }, timeout_ns);
}
std::vector<uint64_t> rec_open_values;      // (the submissions finished in the recording being made; replay_mu)
std::mutex rec_map_mu;
std::map<uint64_t, uint64_t> value_rec;     // submission value (or a recording's own) -> the recording that finished it
// (longshot) A submission is recorded in parts (maybe_split), and each part writes back, at its own end, the pages the GPU
// wrote in it (memory_end_submission). A CPU read of GPU results needs the part that writes its pages back, not the
// submission's last one: part (Recording::part) -> its number on the recordings' timeline, once it is submitted
uint64_t rec_part_next = 0;                 // (the recordings begun so far; replay_mu)
std::map<uint64_t, uint64_t> part_rec;      // (rec_map_mu)
std::condition_variable rec_map_cv;         // (a part has been submitted: wait_recorded_part)
std::unordered_map<uint64_t, uint64_t> q_last;   // queue -> its last submission's value (sync_order_mu)
std::map<uint64_t, uint64_t> value_queue;   // a submission's value -> its queue (sync_order_mu)
std::atomic<uint64_t> pushed{0};
std::atomic<uint64_t> q_values{0};   // the fence values handed out (a submit or a present takes one each)
::mkwvk::rec::DrainingMutex replay_mu;   // one replay at a time (the game submits from several threads; RX_VK_RECORDER=defer: what
                                         // a thread recorded is drained before another may take it)
// (RX_VK_RECORDER=thread: the recorder thread named, and above normal priority as the render thread)
// RX_VK_RECORDER_PIN (experiment): next = the physical core below the reserved one (the replay thread's: hos_reserved_cpus),
// sibling = the reserved core itself (shares it with the replay thread), or a hex mask; unset: not pinned
const bool rec_thread_hook = (::mkwvk::rec::g_thread_start_hook = [] {
    SetThreadDescription(GetCurrentThread(), L"mkw_vk recorder");
#ifdef TRACY_ENABLE
    HT_THREAD_NAME("mkw_vk recorder");
#endif
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    const char* pin = getenv("RX_VK_RECORDER_PIN");
    if (!pin || !*pin) return;
    const uint64_t reserved = host && host->reserved_cpus ? host->reserved_cpus() : 0;
    uint64_t m = 0;
    if (!strcmp(pin, "next")) m = reserved ? reserved >> __builtin_popcountll(reserved) : 0;
    else if (!strcmp(pin, "sibling")) m = reserved;
    else m = strtoull(pin, nullptr, 16);
    const bool ok = m && SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)m) != 0;
    ::mkwvk::logf("vk: RX_VK_RECORDER_PIN=%s: the recorder %s %#llx (reserved %#llx)", pin, ok ? "pinned to" : "NOT pinned, mask",
                  (unsigned long long)m, (unsigned long long)reserved);
}, true);
// (RX_VK_RECORDER_PARTS: the queue locks tell the recorder when they are taken and let go, vk_internal.h QueueMutex)
const bool rec_queue_hook = (::mkwvk::queue_lock_hook = ::mkwvk::rec::queue_lock_event, true);
// (RX_VK_RECORDER_IDLE=wait: the recorder's idle waits on its ring counter. Windows: from kernelbase.dll, without linking
// Synchronization.lib; Linux: host/posix's, a futex)
const bool rec_wait_hooks = [] {
#ifdef _WIN32
    using WaitFn = BOOL(WINAPI*)(volatile VOID*, PVOID, SIZE_T, DWORD);
    using WakeFn = VOID(WINAPI*)(PVOID);
    static WaitFn wait = nullptr;
    static WakeFn wake = nullptr;
    if (const HMODULE k = GetModuleHandleA("kernelbase.dll")) {
        wait = (WaitFn)(void*)GetProcAddress(k, "WaitOnAddress");
        wake = (WakeFn)(void*)GetProcAddress(k, "WakeByAddressSingle");
    }
    if (!wait || !wake) return false;
    ::mkwvk::rec::g_wait_on_address = [](volatile void* a, void* c, size_t n, uint32_t ms) { return wait(a, c, n, ms) != 0; };
    ::mkwvk::rec::g_wake_by_address = [](void* a) { wake(a); };
#else
    ::mkwvk::rec::g_wait_on_address = [](volatile void* a, void* c, size_t n, uint32_t ms) { return WaitOnAddress(a, c, n, ms) != 0; };
    ::mkwvk::rec::g_wake_by_address = [](void* a) { WakeByAddressSingle(a); };
#endif
    return true;
}();
// (RX_VK_BATCH: see queue_flush_locked)
VkSemaphore batch_tl = VK_NULL_HANDLE;
uint64_t batch_next = 0, batch_flushed = 0;
struct { std::vector<VkCommandBufferSubmitInfo> cbs; uint64_t signal = 0; bool any_signal = false; uint64_t rec = 0; uint64_t reflex = 0; } pend;   // (rec: the recordings' timeline value the held ones reach; reflex: the game frame of the first, vk_reflex.h)
int64_t st_flushes, st_batched_cbs;
enum { FR_OTHER, FR_POLL, FR_WAIT, FR_SLOT, FR_PRESENT, FR_CAP, FR_VALUE, FR_COUNT };
int64_t st_reason[FR_COUNT];
int flush_reason = FR_OTHER;
std::chrono::steady_clock::time_point pend_since;
// (longshot) whether any submit is held (pend.cbs not empty; written under dev.present_mu only), for the flushes that come
// from the game's threads to look at without the mutex: a present holds dev.present_mu for as long as it takes (4 ms a
// frame in Hyrule Field), and a flush with nothing to send waited for it. The host's copy bookkeeping asks how far the
// replay is under its own lock (rx_nvn_replayed -> queue_flush_poll), so one job thread waiting there held up every other
// one recording a command buffer: 0.5-1.1 threads blocked at any instant
std::atomic<bool> pend_held{false};

// (longshot, RX_VK_GPU_TIMELINE=1: diag) The frame's timeline, CPU and GPU, into longshot_gpu_timeline.csv: each recording's
// submit (the CPU's steady clock, ns) and its GPU start and end (the GPU's timestamps in ns, on the GPU's own clock: the
// reader finds the offset, as no submission starts on the GPU before its submit), each job the game hands the render thread
// (handed over, begun, done), each wait of the game's for a fence, and each present. For finding where the GPU sits idle
// in a frame and where the game waits for it (docs/dev/reference/architecture.md, The frame pipeline)
bool gtl_on() {
    static const bool on = [] { const char* e = getenv("RX_VK_GPU_TIMELINE"); return e && *e && *e != '0'; }();
    return on;
}
int64_t gtl_now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int64_t gtl_ns(std::chrono::steady_clock::time_point t) { return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count(); }
std::mutex gtl_mu;
FILE* gtl_file() {
    static FILE* f = [] {
        const char* dir = getenv("HOS_PROFILE_DIR");
        FILE* g = fopen(dir && *dir ? (std::string(dir) + "/gpu_timeline.csv").c_str() : log_path("longshot_gpu_timeline.csv").c_str(), "w");
        if (g) fputs("kind,value,a_ns,b_ns,c_ns,extra\n", g);
        return g;
    }();
    return f;
}
void gtl_line(char kind, uint64_t value, int64_t a, int64_t b, int64_t c, int64_t extra) {
    if (!gtl_on()) return;
    std::lock_guard lk(gtl_mu);
    if (FILE* f = gtl_file()) fprintf(f, "%c,%llu,%lld,%lld,%lld,%lld\n", kind, (unsigned long long)value, (long long)a, (long long)b,
                                      (long long)c, (long long)extra);
}
int64_t gtl_submit_ns[64];   // (each recording slot's submit, for its GPU line)
struct Recording {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPool upload_pool = VK_NULL_HANDLE;   // (the upload command buffer's own pool)
    VkCommandBuffer cmd = VK_NULL_HANDLE, upload = VK_NULL_HANDLE;
    uint64_t value = 0;   // the timeline value its submit signals (reusable once reached)
    int slot = 0;         // its index: the draw path keeps a descriptor pool and stream per recording
    // (its slot comes round again once its own submit is done: a fence of its own. The timeline is signalled as far
    // as no older submission is unfinished (submit_job), and a slot waiting on its value could wait on the replay)
    VkFence fence = VK_NULL_HANDLE;
    bool submitted = false;
    uint64_t batch = 0;   // (RX_VK_BATCH: the batch it went out in, waited for on batch_tl; 0: its own fence)
    uint64_t part = 0;    // (its number among the recordings begun: the pages it writes back are noted with it, part_rec)
};
// (24: the event waits cut a frame's command buffers into pieces, each with recordings of its own, and with 16 the
// replay waited ~1.5 ms a frame for a slot the GPU still had)
constexpr int kRecordings = 48;   // (test: 24 before; with RX_VK_BATCH the ring came round to held submits once a frame)
static_assert(kRecordings <= 64, "gtl_submit_ns holds each slot");
Recording recordings[kRecordings];
uint64_t rec_end_pos[kRecordings];   // (RX_VK_RECORDER_OWN: the recorder ring's position after each slot's last end, vk_recorder.h)
uint64_t rec_ticket[kRecordings];    // (RX_VK_RECORDER_PARTS: each slot's last part handed to the recorder, rec::post_part's number)
int recording_index = 0;

/// (a recording slot's last submit done: before it is used again)
void recording_wait(Recording& r) {
#ifdef TRACY_ENABLE
    HT_WAIT(ht_w, "recording_wait");
#endif
    // (RX_VK_RECORDER_PARTS: its end and submit made by the recorder, which set r.submitted and r.batch; the waits below are on
    // that submit's fence or batch, and need no more of the recorder: Settled)
    if (!rec::wait_part(rec_ticket[r.slot]))   // (unbounded, and never under a queue lock: met, or the replay cannot go on)
        rec::recorder_fatal("a recording slot reused before its part was submitted");
    rec::Settled settled;
    if (!r.submitted) return;
    FpsRtWait rt_wait(fps_stats::kRtGpu);   // (the FPS counter's render lane)
    if (r.batch) {
        {
            std::lock_guard lk(dev.present_mu);
            if (batch_flushed < r.batch) flush_reason = FR_SLOT, queue_flush_locked();
        }
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = 1;
        wi.pSemaphores = &batch_tl;
        wi.pValues = &r.batch;
        vkWaitSemaphores(dev.device, &wi, UINT64_MAX);
        r.batch = 0;
        r.submitted = false;
        return;
    }
    vkWaitForFences(dev.device, 1, &r.fence, VK_TRUE, UINT64_MAX);
    vkResetFences(dev.device, 1, &r.fence);
    r.submitted = false;
}

uint64_t completed() {
    uint64_t v = 0;
    vkGetSemaphoreCounterValue(dev.device, timeline, &v);
    return v;
}
// ---- (test) RX_VK_BATCH=1: the replay's submits held and sent as one vkQueueSubmit2 ----
// On amdgpu every command submission pays for all imported host memory (userptr) again; the replay made ~6 a frame.
// Held submits go out at the next point that needs the GPU's results or the queue's order: a wait, a readback, a
// present, a sparse bind, another submit (queue_flush_locked under dev.present_mu).
// (longshot) on by default with Mesa's RADV, where it was measured (a Radeon 8060S: ~10 -> 30-50 fps, the replay's queue
// submit ~70 -> ~6 ms a frame); off elsewhere. RX_VK_BATCH=0 / 1: off / on on any driver
/// (longshot) The GPU has done every batch sent so far: the batch being held goes out at once. A held batch would wait for the
/// next wait or present while the GPU sat idle, and with serial replay the game waits for it mid-frame (take_capture's late
/// pages), so the GPU would start cold just as it was waited for. ~10 flushes a frame of ~3.7 command buffers each: batching
/// still groups the submits while the GPU is busy
bool batch_gpu_idle() {
    uint64_t done = 0;
    return vkGetSemaphoreCounterValue(dev.device, batch_tl, &done) == VK_SUCCESS && done >= batch_flushed;
}
bool batch_on() {
    static const bool on = [] {
        // (device_policy.h batch_submits: RADV, and (k10) not on GCN 1-4, where an RX 580 ran Kokiri Forest at 24 fps without
        // and 20 with; then the switch)
        const bool v = dev.policy.batch_submits;
        logf("vk: batched submits %s (RX_VK_BATCH%s)", v ? "on" : "off",
             !dev.policy.set(Sw::batch_submits) && dev.facts.radv() ? dev.facts.radv_gcn() ? ", off on GCN" : ", on with RADV" : "");
        return v;
    }();
    return on && batch_tl != VK_NULL_HANDLE;
}
} // namespace
void queue_flush_locked() {   // (dev.present_mu held)
    if (pend.cbs.empty()) return;
    VkSemaphoreSubmitInfo sig[3]{{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO},
                                 {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}};
    uint32_t n = 0;
    if (pend.any_signal) {
        sig[n].semaphore = timeline, sig[n].value = pend.signal, sig[n].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        n++;
    }
    if (pend.rec) {   // (the recordings' timeline too: wait_recorded waits on it, and held submits carried no signal of it)
        sig[n].semaphore = rec_timeline, sig[n].value = pend.rec, sig[n].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        n++;
    }
    sig[n].semaphore = batch_tl, sig[n].value = batch_next, sig[n].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    n++;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = (uint32_t)pend.cbs.size();
    si.pCommandBufferInfos = pend.cbs.data();
    si.signalSemaphoreInfoCount = n;
    si.pSignalSemaphoreInfos = sig;
    PROF(PROF_QSUBMIT);
    si.pNext = reflex_submit_id(nullptr, pend.reflex);   // (NVIDIA Reflex: the game frame of the batch's first command buffer)
    VkSemaphoreSubmitInfo sparse_wait;   // (longshot: after the sparse mirrors' binds, RX_VK_MIRROR_SPARSE)
    if (memory_sparse_wait(&sparse_wait)) si.waitSemaphoreInfoCount = 1, si.pWaitSemaphoreInfos = &sparse_wait;
    const VkResult res = submit_or_retry(si, VK_NULL_HANDLE);
    static int failures;
    if (res != VK_SUCCESS && failures++ < 8) logf("vk: batched submit %llu failed (%d)", (unsigned long long)batch_next, (int)res);
    if (res == VK_ERROR_DEVICE_LOST) device_lost();
    if (res == VK_SUCCESS && pend.any_signal) note_timeline_sent(pend.signal);
    batch_flushed = batch_next;
    st_flushes++, st_batched_cbs += (int64_t)pend.cbs.size();
    st_reason[flush_reason]++;
    flush_reason = FR_OTHER;
    pend.cbs.clear(), pend.any_signal = false, pend.signal = 0, pend.rec = 0, pend.reflex = 0;
    pend_held.store(false, std::memory_order_release);
}
void queue_flush() {
#ifdef TRACY_ENABLE
    HT_SCOPE("queue_flush");
#endif
    std::lock_guard lk(dev.present_mu);
    queue_flush_locked();
}
void device_wait_idle_locked() {   // (dev.present_mu held)
    queue_flush_locked();
    std::lock_guard pq(dev.present_queue_mu);
    std::lock_guard pr(dev.presenter_queue_mu);   // (the presenter thread's queue too: every queue is idle-waited)
    vkDeviceWaitIdle(dev.device);
}
namespace {
/// (RX_VK_RECORDER_PARTS: the parts handed over submitted first, at most settle_ns, before the look at what is held: a part
/// still in the ring when a waiter flushed was held after it, and the wait for it never came back with RX_VK_BATCH. Unmet (a
/// part's submit behind a present holding the queue's lock), what is held is sent if the lock is free, and the part the waiter
/// needs sends what is held once submitted (part_submit). The nanoseconds it waited for the parts: the caller's driver wait gets
/// what is left of its time)
uint64_t queue_flush_for(int why, uint64_t settle_ns = UINT64_MAX) {
    const auto t0 = std::chrono::steady_clock::now();
    const bool met = rec::settle_parts(rec::B_QUEUE_LOCK, settle_ns);
    const uint64_t spent = met && settle_ns == 0 ? 0 : (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                          std::chrono::steady_clock::now() - t0).count();
    if (!pend_held.load(std::memory_order_acquire)) return spent;   // (nothing held: nothing to send, the mutex not taken)
    rec::Settled settled;   // (the parts waited for above: the lock waits for none)
    std::unique_lock lk(dev.present_mu, std::defer_lock);
    if (met) lk.lock();
    else if (!lk.try_lock()) return spent;
    if (pend.cbs.empty()) return spent;
    flush_reason = why;
    queue_flush_locked();
    return spent;
}
/// (what is left of a wait's time once spent of it is gone)
uint64_t time_left(uint64_t timeout_ns, uint64_t spent) { return timeout_ns == UINT64_MAX ? timeout_ns : spent < timeout_ns ? timeout_ns - spent : 0; }
// (RX_VK_BATCH_POLL_US=<us>: a progress poll sends held submits only once the oldest has waited that long; 0: always.
// Default 2000: the game polls many times a frame, and each poll that flushed was a whole command submission, the cost
// batching is there to avoid. RX 7800 XT, RADV, Kokiri: 525 -> 67 poll flushes per 120 presents, 38 -> 42.5 fps; issues
// 413, 528)
void queue_flush_poll() {
    static const int64_t us = getenv("RX_VK_BATCH_POLL_US") ? atoll(getenv("RX_VK_BATCH_POLL_US")) : 2000;
    if (!pend_held.load(std::memory_order_acquire)) return;   // (nothing held: nothing to send, the mutex not taken)
    rec::Settled settled;   // (RX_VK_RECORDER_PARTS: a poll sends what is held, and waits for no part the recorder has yet to hold)
    std::lock_guard lk(dev.present_mu);
    if (pend.cbs.empty()) return;
    if (us > 0 && std::chrono::steady_clock::now() - pend_since < std::chrono::microseconds(us)) return;
    flush_reason = FR_POLL;
    queue_flush_locked();
}
} // namespace
namespace {
void wait_value(uint64_t v) {
    queue_flush_for(FR_VALUE);
    rec::Settled settled;   // (RX_VK_RECORDER_PARTS: the flush waited for the parts)
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline;
    wi.pValues = &v;
    vkWaitSemaphores(dev.device, &wi, UINT64_MAX);
}
bool wait_recorded(uint64_t f, uint64_t timeout_ns, int* ok);
/// (longshot) What the calling thread's last wait on the recordings' timeline came back with (wait_rec_seq): one that came back
/// VK_ERROR_DEVICE_LOST is said there, and not again by vk_timeline_wait as a capture's wait that timed out
thread_local VkResult wait_rec_result = VK_SUCCESS;
/// (longshot) A timeline wait that came back VK_ERROR_DEVICE_LOST, said as a lost device (8 times at most). Run 21 (macOS,
/// MoltenVK 1.4.2) logged one as a capture's wait that "timed out (done 0)": the driver marks the device lost, signals every
/// timeline waited on, returns VK_ERROR_DEVICE_LOST from the wait and leaves the counter's value unwritten, so completed() read
/// 0, and the log showed a timeout where the GPU was gone. Only the line: the next submit or the GPU hang watch ends the game,
/// as before
void wait_lost(const char* what, uint64_t v) {
    static std::atomic<int> logged{0};
    if (logged++ >= 8) return;
    logf("vk: %s %llu came back VK_ERROR_DEVICE_LOST: the GPU device is lost", what, (unsigned long long)v);
}
} // namespace
uint64_t vk_timeline_completed() { return timeline ? completed() : 0; }
void vk_timeline_wait(uint64_t v) {   // (at most 250 ms: a submission held back behind the caller's thread goes on without it)
    if (!timeline || completed() >= v) return;
    // (a burst recording whose write-back stamped wb_pages with burst_value does not advance `timeline` while an older queue
    // submission is stopped at an event wait; wait on `rec_timeline` where that recording actually signalled completion)
    int ok = 0;
    if (wait_recorded(v, 250000000ull, &ok)) {
        const bool lost = wait_rec_result == VK_ERROR_DEVICE_LOST;   // (said by wait_rec_seq)
        if (lost) return;
        if (!ok) {
            static std::atomic<int> logged{0};
            if (logged++ < 8)
                logf("vk: a capture's wait for write-back %llu timed out (done %llu)", (unsigned long long)v,
                     (unsigned long long)completed());
        }
        return;
    }
    // (RX_VK_RECORDER_PARTS: the parts waited for within the wait's 250 ms, the driver's wait given what is left)
    const uint64_t spent = queue_flush_for(FR_VALUE, 250000000ull);
    rec::Settled settled;
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline;
    wi.pValues = &v;
    const VkResult res = vkWaitSemaphores(dev.device, &wi, spent < 250000000ull ? 250000000ull - spent : 0);
    if (res == VK_TIMEOUT) {
        static std::atomic<int> logged{0};
        if (logged++ < 8) logf("vk: a capture's wait for write-back %llu timed out (done %llu)", (unsigned long long)v, (unsigned long long)completed());
    }
    const bool lost = res == VK_ERROR_DEVICE_LOST;
    if (lost) wait_lost("a capture's wait for write-back", v);
}
namespace {

int64_t st_wait_ns;   // (time the replay waited for a recording slot: the GPU behind)

// GPU time: a timestamp at the start and the end of each recording's command buffer, read when its slot comes
// round again (the work done by then)
VkQueryPool ts_pool = VK_NULL_HANDLE;
bool ts_pending[kRecordings];
int64_t st_gpu_ns, st_gpu_max_ns;
// (frame overlap, below with the render thread: what each recording did, and the GPU's time between recordings)
void fo_recording_begin(const Recording& r);
void fo_recording_end(const Recording& r);
void fo_collect(const Recording& r, const uint64_t* t);
// (longshot) an A/B within the run (HOS_AB=RX_VK_MESH_CONV=<v>): the GPU time of the recordings made with each value, and the
// frames presented with each, so that the log can give each side's GPU time a frame (the frame trace has no GPU time column).
// A recording's value: -1 outside the A/B (its warm-up, none set)
int rec_mc_mode[kRecordings];
int64_t st_gpu_mc_ns[3], st_mc_frames[3];
bool ab_running() {
    return host->version >= 34 && host->ab_frame && host->now_ns && host->ab_frame(0, host->now_ns()) >= 0;
}
void gpu_time_collect(Recording& r) {
    if (!ts_pool || !ts_pending[r.slot]) return fo_collect(r, nullptr);
    ts_pending[r.slot] = false;
    uint64_t t[2] = {0, 0};
    if (vkGetQueryPoolResults(dev.device, ts_pool, 2 * r.slot, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return fo_collect(r, nullptr);
    if (t[1] <= t[0]) return fo_collect(r, nullptr);
    const int64_t ns = (int64_t)((t[1] - t[0]) * (double)dev.props.limits.timestampPeriod);
    st_gpu_ns += ns;
    if (gtl_on())
        gtl_line('R', r.value, gtl_submit_ns[r.slot], (int64_t)(t[0] * (double)dev.props.limits.timestampPeriod),
                 (int64_t)(t[1] * (double)dev.props.limits.timestampPeriod), r.slot);
    st_gpu_max_ns = std::max(st_gpu_max_ns, ns);
    if (rec_mc_mode[r.slot] >= 0 && rec_mc_mode[r.slot] < 3) st_gpu_mc_ns[rec_mc_mode[r.slot]] += ns;
    fo_collect(r, t);
}
}   // namespace

const char* const call_kind_names[kCallKinds] = {"BindPipeline", "PushDescriptorSetKHR", "PushConstants", "BindDescriptorSets", "BindVertexBuffers2", "BindIndexBuffer", "SetViewportWithCount", "SetScissorWithCount", "SetViewport", "SetScissor", "SetBlendConstants", "SetStencilReference", "SetStencilCompareMask", "SetStencilWriteMask", "SetDepthBias", "SetDepthBiasEnable", "SetLineWidth", "SetDepthBoundsTestEnable", "SetCullMode", "SetFrontFace", "SetPrimitiveTopology", "SetPrimitiveRestartEnable", "SetDepthTestEnable", "SetDepthWriteEnable", "SetDepthCompareOp", "SetStencilTestEnable", "SetStencilOp", "SetRasterizerDiscardEnable", "SetDepthClampEnableEXT", "SetPolygonModeEXT", "SetColorBlendEnableEXT", "SetColorBlendEquationEXT", "SetColorWriteMaskEXT", "SetAlphaToCoverageEnableEXT", "SetCheckpointNV"};
std::atomic<uint64_t> call_counts[kCallKinds];
bool call_count_on() {
    static const bool on = [] { const char* e = getenv("RX_VK_CALL_COUNT"); return e && *e && *e != '0'; }();
    return on;
}
/// (the FPS counter's Breakdown, vk_fps.cpp) the GPU's time in the timed recordings so far: read when their slots come round
int64_t gpu_time_ns() { return st_gpu_ns; }
/// (and the replay's waits for a recording slot so far: the GPU behind)
int64_t slot_wait_ns() { return st_wait_ns; }
namespace {
void gpu_time_begin(Recording& r) {
    fo_recording_begin(r);   // (at every recording's start: begin_recording)
    if (!ts_pool) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2 * kRecordings;
        if (vkCreateQueryPool(dev.device, &qi, nullptr, &ts_pool) != VK_SUCCESS) ts_pool = VK_NULL_HANDLE;
        if (!ts_pool) return;
    }
    vkCmdResetQueryPool(r.cmd, ts_pool, 2 * r.slot, 2);
    vkCmdWriteTimestamp2(r.cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, ts_pool, 2 * r.slot);
}
void gpu_time_end(Recording& r) {
    fo_recording_end(r);   // (at every recording's end, right before its submit: submit_recording)
    rec_mc_mode[r.slot] = ab_running() ? mesh_conv_mode() : -1;   // (the value draw_begin_recording read for it, on this thread)
    if (!ts_pool) return;
    vkCmdWriteTimestamp2(r.cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, ts_pool, 2 * r.slot + 1);
    ts_pending[r.slot] = true;
    if (gtl_on()) gtl_submit_ns[r.slot] = gtl_now();   // (the submit follows at once: submit_recording)
}

// (longshot) Real occlusion counts (on unless RX_VK_OCC_QUERIES=0; from Hookshot's draft by jordanella). Ocarina of Time
// reads its z-prepass samples-passed reports back on the CPU a frame or two later and leaves out of its draw lists the
// objects whose count was 0 (RX_VK_OCC_QUERIES's row in switches.def); with every report reading 4096 nothing was ever left out. Each draw from
// a samples-passed reset to its report gets an occlusion query, numbered from one counter for all the game's queues
// (occ_next_all; each command buffer's replay works on its state's copy, NvnState::occ_next). The report is still written at replay as before ("visible"); once its queries have all
// ended on the GPU, the replay thread writes their sum over it, unless the slot has been reported into again since.
// The rule: a count that is not known for sure stays "visible". A wrong 0 hides an object the player can see; a late or
// unknown count only costs culling. So a report stays "visible" when its run took more than kOccRun draws, had a draw
// that writes colour, was split by another queue's queries, when one of its draws did not really draw (OccGuard in
// vk_draw.cpp: its query is never begun), or when its slot was reported into again first.
constexpr uint64_t kOccPool = 1 << 16;   // (the queries' ring)
constexpr uint64_t kOccRun = 256;        // (the most draws one report counts)
constexpr uint64_t kOccBuffer = 8192;    // (the most queries one command buffer takes)
constexpr uint64_t kOccVisible = 4096;   // (what a report reads while its count is not known)
VkQueryPool occ_pool = VK_NULL_HANDLE;
struct OccReport {
    uint64_t addr, ts;       // (the report's slot and the timestamp written with it; addr 0: a run with no count, freed)
    uint64_t first, n;       // (its queries)
    uint64_t value, part;    // (the submission they go out in (rec_cur_value at the report), and the recording count then)
    bool color = false;      // (a draw of the run writes colour)
    uint64_t seq = 0;        // (the slot's report sequence then: occ_slots)
};
std::deque<OccReport> occ_pending;   // (in report order; the replay thread's)
// (queries read, waiting to be reset until the GPU has finished the submission they went out in: its timeline value. The
// availability bit says the query has ended, not that its command buffer has: a reset then ran into submitted work, in
// the validation layer's view (VUID-vkResetQueryPool-firstQuery-02741, then -vkCmdBeginQuery-None-00807 at the next use))
struct OccFree {
    uint64_t first, n, value;
};
std::deque<OccFree> occ_freeing;
uint64_t occ_next_all = 0;           // (the next query's number, for every queue)
// (each report slot's latest report: a count is written only over the report it belongs to. The timestamp word alone
// was not enough: two reports into one slot within a tick (~0.5 us at RX_VK_TS_SCALE 0.1) look the same)
// Kept in a direct-mapped table (no allocation, one cache line a lookup): a slot that shares an entry with another evicts
// it, and the evicted slot's waiting count is then not written (its report stays "visible": the safe side)
struct OccSlot {
    uint64_t addr = 0, seq = 0;
};
constexpr size_t kOccSlots = 1 << 14;
std::vector<OccSlot> occ_slots(kOccSlots);
uint64_t occ_seq = 0;
OccSlot& occ_slot(uint64_t addr) { return occ_slots[(size_t)((addr >> 4) * 0x9E3779B97F4A7C15ull >> 50)]; }
struct {
    uint64_t real, zero, unknown, stale, lag, calls, polls, ns, sum, small, creal, czero, split, ahead, walks, walk_ns, copies, copied, fell_back;
} occ_st;

/// (longshot) RX_VK_OCC_COPY: the queries' results copied by the GPU at the end of the recording that ran them
/// (vkCmdCopyQueryPoolResults, after the recording's end barrier) into a buffer in host memory, which occ_collect reads as plain
/// memory. Read from the pool (vkGetQueryPoolResults), each query cost ~3.4 us in the driver on an Arc B580 (101.9154; the time
/// grew with the queries read, not with the calls): 7.4 ms of the render thread's 28.8 ms a frame in Castle Town's market,
/// 34.8 fps there against 45.8 with the copy. A query the copy found not done (never begun: its draw did not draw), or one
/// whose recording ended without the copy, is read from the pool once its submission is done, as before. A count reads
/// kOccUnread until its copy has written it, whichever of its two words the GPU writes first. The copies cost GPU time (~2 ms
/// a frame in the market on the B580, ~1,400 queries), so they are on by default only where they were measured: Intel's
/// Windows driver on a discrete GPU (Arc). An iGPU, mostly held back by the GPU, or another driver whose pool reads cost
/// less, may lose by them (RX_VK_OCC_COPY=1: on there too). Off without coherent host memory. RX_VK_OCC_COPY=0: read from
/// the pool
bool occ_copy_wanted() {
    return dev.policy.occ_copy;   // (device_policy.h: Intel's Windows driver on a discrete GPU, then the switch)
}
VkBuffer occ_host_buf = VK_NULL_HANDLE;
VmaAllocation occ_host_alloc = nullptr;
volatile uint64_t* occ_host = nullptr;   // ((count, availability) for each query of the ring, at its place in the pool)
constexpr uint64_t kOccUnread = ~0ull;   // (a count no copy has written since the query's reset)
uint64_t occ_part_lo = UINT64_MAX, occ_part_hi = 0;   // (the queries the recording being made took: occ_copy_part)
void occ_host_clear(uint64_t at, uint64_t k) {
    for (uint64_t i = at; i < at + k; i++) {
        occ_host[2 * i] = kOccUnread;
        occ_host[2 * i + 1] = 0;
    }
}

bool occ_queries_on() {
    static const bool on = [] {
        const char* e = getenv("RX_VK_OCC_QUERIES");
        if (e && atoi(e) == 0) return false;
        if (getenv("RX_VK_OCC_FIXED")) {   // (a fixed value asked for: that value, as before, whatever RX_VK_OCC_QUERIES says)
            if (e) logf("vk: RX_VK_OCC_QUERIES: off, RX_VK_OCC_FIXED is set");
            return false;
        }
        // (the replay on the submitting thread, the default, writes "visible" at the submit, or at the stop for work held at
        // an event wait, occ_visible_ahead)
        // (RX_VK_ASYNC=1, the render thread: on too. A report reads "visible" from its replay until its queries are done, as
        // here; the game, a frame ahead at most, reads a count that much older, never an older report's: occ_collect)
        if (!dev.host_query_reset) {
            logf("vk: RX_VK_OCC_QUERIES: this device cannot reset queries from the CPU (hostQueryReset): off");
            return false;
        }
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_OCCLUSION;
        qi.queryCount = (uint32_t)kOccPool;
        if (vkCreateQueryPool(dev.device, &qi, nullptr, &occ_pool) != VK_SUCCESS) {
            logf("vk: RX_VK_OCC_QUERIES: the query pool could not be made: off");
            occ_pool = VK_NULL_HANDLE;
            return false;
        }
        vkResetQueryPool(dev.device, occ_pool, 0, (uint32_t)kOccPool);
        if (occ_copy_wanted()) {
            void* m = nullptr;
            if (create_buffer_cached(kOccPool * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, occ_host_buf, occ_host_alloc, &m, true) && m &&
                host_coherent(occ_host_alloc)) {
                occ_host = (volatile uint64_t*)m;
                occ_host_clear(0, kOccPool);
            } else {
                if (occ_host_buf) destroy_buffer(occ_host_buf, occ_host_alloc);
                occ_host_buf = VK_NULL_HANDLE, occ_host_alloc = nullptr;
            }
        }
        logf("vk: occlusion queries on (RX_VK_OCC_QUERIES): the game's samples-passed reports get the GPU's counts%s; %s",
             dev.occlusion_precise ? "" : " (not precise: a count says only none or some)",
             occ_host ? "copied into host memory at each recording's end (RX_VK_OCC_COPY)"
                      : occ_copy_wanted() ? "read from the pool (RX_VK_OCC_COPY: no coherent host memory for the copies)"
                                          : "read from the pool");
        return true;
    }();
    return on;
}
/// The ring's queries [first, first + n) back to unused (each one done on the GPU, or never begun)
void occ_reset(uint64_t first, uint64_t n) {
    while (n) {
        const uint64_t at = first % kOccPool, k = std::min(n, kOccPool - at);
        vkResetQueryPool(dev.device, occ_pool, (uint32_t)at, (uint32_t)k);
        if (occ_host) occ_host_clear(at, k);   // (the GPU is done with them, and with their copies: the same submissions)
        first += k, n -= k;
    }
}
/// Room in the ring for another command buffer's queries. oldest_open: the first query of the oldest run still open in any
/// queue's state (taken, not in occ_pending yet; UINT64_MAX: none)
bool occ_room(uint64_t oldest_open) {
    uint64_t tail = std::min(oldest_open, occ_next_all);
    for (const OccReport& o : occ_pending) tail = std::min(tail, o.first);   // (two queues' reports can come out of order)
    for (const OccFree& f : occ_freeing) tail = std::min(tail, f.first);
    return occ_queries_on() && occ_next_all - tail + kOccBuffer <= kOccPool;
}
/// A command buffer's replay begins on state s: its numbering from the counter every queue shares. When another queue's
/// command buffers took queries since s's last one, a run s has open is split: what it took is freed and its report stays
/// "visible". (Each queue had its own count before: two queues handed out the same queries.) value: the submission it goes
/// out in. Then the limit, from the ring's room (oldest_open: the other states' oldest open run, occ_room)
void occ_begin_buffer(NvnState& s, uint64_t value, uint64_t oldest_open) {
    if (s.occ_next != occ_next_all) {
        if (s.occ_on) {
            if (s.occ_next > s.occ_first)
                occ_pending.push_back({0, 0, s.occ_first, s.occ_next - s.occ_first, value, rec_part_next});
            if (s.occ_next > s.occ_first || !s.occ_overflow) occ_st.split++;
            s.occ_overflow = true;
            s.occ_first = occ_next_all;
        }
        s.occ_next = occ_next_all;
    }
    if (s.occ_on) oldest_open = std::min(oldest_open, s.occ_first);
    s.occ_limit = occ_room(oldest_open) ? s.occ_next + kOccBuffer : 0;
}
/// ... and ends: the queries it took are the counter's
void occ_end_buffer(const NvnState& s) { occ_next_all = s.occ_next; }
/// A queue's state made as a copy of another's: a run the other has open is not this queue's
void occ_state_copied(NvnState& s) {
    s.occ_on = false, s.occ_overflow = false, s.occ_color = false;
    s.occ_first = s.occ_next;
}
/// A samples-passed reset (NOP_RESET_COUNTER): a run begins; one still open had no report: its queries are freed
void occ_counter_reset(NvnState& s, uint64_t value) {
    if (s.occ_on && s.occ_next > s.occ_first)
        occ_pending.push_back({0, 0, s.occ_first, s.occ_next - s.occ_first, value, rec_part_next});
    s.occ_on = s.occ_next < s.occ_limit;
    s.occ_first = s.occ_next;
    s.occ_overflow = false;
    s.occ_color = false;
}
/// The run a report counts, closed (NOP_REPORT_COUNTER)
struct OccRun {
    uint64_t first = 0, n = 0;
    bool counted = false, color = false;
};
OccRun occ_close(NvnState& s, bool samples_passed) {
    OccRun r;
    if (!samples_passed || !s.occ_on) return r;
    r.first = s.occ_first, r.n = s.occ_next - s.occ_first;
    r.counted = !s.occ_overflow, r.color = s.occ_color;
    s.occ_on = false, s.occ_overflow = false;
    return r;
}
/// A slot's report sequence moved on: a count still waiting for an older report there is not written
uint64_t occ_slot_next(uint64_t addr) {
    OccSlot& e = occ_slot(addr);
    e.addr = addr;
    return e.seq = ++occ_seq;
}
/// A report written by the replay (any counter: the slot's sequence moves on), and the count of its run to come, at
/// value (rec_cur_value: the submission its queries go out in. Not the last begin_recording's value: a present's and
/// frame generation's recordings begin with their own)
void occ_reported(const OccRun& run, uint64_t addr, uint64_t ts, bool flag_written, uint64_t value) {
    if (!occ_pool) return;
    const uint64_t seq = occ_slot_next(addr);
    if (run.n)
        occ_pending.push_back({run.counted && !flag_written ? addr : 0, ts, run.first, run.n, value, rec_part_next, run.color, seq});
}
/// A samples-passed report the replay has not reached yet (its queue stopped at an event wait): "visible" now, so the game
/// never reads an older count there in the meantime; an older count still waiting for the slot is not written
void occ_slot_visible(uint64_t addr) {
    if (!occ_pool) return;
    occ_slot_next(addr);
    uint64_t now = 0;
    host->gpu_read(addr, &now, 8);
    if (now == kOccVisible) return;
    // (no memory_cpu_owned: the slot was made the CPU's when it was last reported, and will be again when this report is
    // replayed; ~1,500 more words a frame made every write-back's cut dearer)
    guest_cpu_write(addr, &kOccVisible, 8);
    occ_st.ahead++;
}
/// The reports whose queries are done, written into the game's slots (the replay thread, as a recording begins). The
/// queries of every waiting report are asked for at once, as one run of the ring (two calls where it wraps), without a wait;
/// the reports from the oldest on whose queries have all ended (or whose submission the GPU has finished: a query never begun
/// is unknown then) are written, and their queries reset with one call. (Hookshot's draft asked and reset each report's
/// queries on their own, ~1,500 of each a frame in the Market; waiting for a whole submission instead wrote the counts 3-13
/// recordings late, about two frames, often after the game had read the slot.)
void occ_collect() {
    if (!occ_pool) return;
    if (!occ_freeing.empty()) {   // (the queries read before, whose submissions the GPU has finished: reset)
        const uint64_t done = completed();
        while (!occ_freeing.empty() && done >= occ_freeing.front().value) {
            occ_reset(occ_freeing.front().first, occ_freeing.front().n);
            occ_freeing.pop_front();
        }
    }
    if (occ_pending.empty()) return;
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t done = completed();
    // (the waiting reports' queries follow on from each other: each run starts where the last one ended; a gap (two
    // queues' runs reported out of order) ends the batch there and the rest wait for the next call)
    const uint64_t first = occ_pending.front().first;
    uint64_t end = first;
    size_t span = 0;
    for (const OccReport& o : occ_pending) {
        if (o.first != end) break;
        end += o.n, span++;
    }
    static std::vector<uint64_t> got;   // (count, availability) for each query of [first, end)
    got.assign(2 * (end - first), 0);
    const auto pool_read = [&](uint64_t from, uint64_t to) {   // (the queries [first + from, first + to), from the pool)
        for (uint64_t i = from; i < to;) {
            const uint64_t at = (first + i) % kOccPool, k = std::min(to - i, kOccPool - at);
            vkGetQueryPoolResults(dev.device, occ_pool, (uint32_t)at, (uint32_t)k, k * 16, got.data() + 2 * i, 16,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);   // (VK_NOT_READY: some not done)
            i += k;
        }
    };
    if (occ_host) {   // (RX_VK_OCC_COPY: their copies. One is there once its availability word is set and its count written)
        for (uint64_t i = 0; i < end - first; i++) {
            const uint64_t at = (first + i) % kOccPool;
            const uint64_t a = occ_host[2 * at + 1], c = occ_host[2 * at];
            got[2 * i] = c, got[2 * i + 1] = a && c != kOccUnread;
        }
    } else {
        pool_read(0, end - first);
    }
    size_t ready = 0;
    for (size_t r = 0; r < span; r++) {
        const OccReport& o = occ_pending[r];
        const uint64_t from = o.first - first, to = from + o.n;
        bool all = true;
        for (uint64_t i = from; i < to && all; i++) all = got[2 * i + 1] != 0;
        if (!all && occ_host && o.value && done >= o.value) {   // (its submission done: what its copy did not have, from the pool)
            std::fill(got.begin() + 2 * from, got.begin() + 2 * to, 0);
            pool_read(from, to);
            occ_st.fell_back += o.n;
        }
        if (!all && !(o.value && done >= o.value)) break;   // (still on the GPU: it and those after it wait)
        ready++;
    }
    occ_st.polls++;
    if (!ready) {
        const OccReport& o = occ_pending.front();
        static int logged;
        if (rec_part_next - o.part > 200 && logged++ < 4)
            logf("vk: occlusion queries: a report has waited %llu recordings (value %llu, done %llu, %zu waiting)",
                 (unsigned long long)(rec_part_next - o.part), (unsigned long long)o.value, (unsigned long long)done,
                 occ_pending.size());
        occ_st.ns += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        return;
    }
    end = occ_pending[ready - 1].first + occ_pending[ready - 1].n;
    uint64_t latest = 0;   // (the last submission these queries went out in)
    for (size_t r = 0; r < ready; r++) {
        const OccReport& o = occ_pending.front();
        latest = std::max(latest, o.value);
        bool all = true;
        uint64_t sum = 0;
        for (uint64_t i = o.first - first; i < o.first - first + o.n; i++) {
            if (!got[2 * i + 1]) all = false;   // (its draw did not draw: its query never begun, unknown)
            else sum += got[2 * i];
        }
        if (o.addr && all) {
            uint64_t ts = 0;
            host->gpu_read(o.addr + 8, &ts, 8);
            const OccSlot& e = occ_slot(o.addr);
            static const bool dry = env_on("RX_VK_OCC_DRY");   // (a test: the queries made and read, the reports left at 4096)
            if (ts == o.ts && e.addr == o.addr && e.seq == o.seq) {
                if (!dry) guest_cpu_write(o.addr, &sum, 8);
                (sum ? occ_st.real : occ_st.zero)++;
                occ_st.sum += sum, occ_st.small += sum && sum < 64;
                if (o.color) (sum ? occ_st.creal : occ_st.czero)++;
                occ_st.lag += rec_part_next - o.part;
            } else {
                occ_st.stale++;   // (reported again before this one was done: the newer one stands)
            }
        } else if (o.addr) {
            occ_st.unknown++;
        }
        occ_pending.pop_front();
    }
    // (reset once the GPU has finished their submissions: at once when it has, else at a later call. In order of value:
    // a later batch waits behind an earlier one)
    if (done >= latest && occ_freeing.empty()) occ_reset(first, end - first);
    else occ_freeing.push_back({first, end - first, std::max(latest, occ_freeing.empty() ? 0 : occ_freeing.back().value)});
    occ_st.calls++;
    occ_st.ns += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    static const bool log_on = env_on("RX_VK_OCC_LOG");
    const uint64_t n = occ_st.real + occ_st.zero + occ_st.unknown + occ_st.stale;
    if (log_on && n >= 5000) {
        const uint64_t w = occ_st.real + occ_st.zero;
        uint64_t nd[3] = {0, 0, 0};   // (vk_draw.cpp: queries left unbegun, by why)
        occ_draw_counts(nd);
        logf("vk: occlusion queries: %llu reports written (%llu of them 0, %.1f%%), %llu left visible (a draw did not draw), "
             "%llu reported again first; written %.1f recordings after the report; %zu waiting; %llu reads (%llu wrote), %.1f us each; "
             "the others: %.0f samples on average, %llu under 64; runs with colour draws %llu (%llu of them 0); "
             "queries not begun: %llu draws returned before drawing, %llu with vertex input not all bound, %llu task batches; "
             "%llu runs split by another queue's queries; %llu slots made visible ahead of a stopped queue (%llu walks, %.1f us each); "
             "%llu copies, %.1f queries each, %llu queries read from the pool after their copy (RX_VK_OCC_COPY)",
             (unsigned long long)w, (unsigned long long)occ_st.zero, w ? 100.0 * occ_st.zero / w : 0.0,
             (unsigned long long)occ_st.unknown, (unsigned long long)occ_st.stale, w ? (double)occ_st.lag / w : 0.0,
             occ_pending.size(), (unsigned long long)occ_st.polls, (unsigned long long)occ_st.calls,
             occ_st.polls ? occ_st.ns / 1e3 / occ_st.polls : 0.0, occ_st.real ? (double)occ_st.sum / occ_st.real : 0.0,
             (unsigned long long)occ_st.small, (unsigned long long)(occ_st.creal + occ_st.czero), (unsigned long long)occ_st.czero,
             (unsigned long long)nd[0], (unsigned long long)nd[1], (unsigned long long)nd[2], (unsigned long long)occ_st.split,
             (unsigned long long)occ_st.ahead, (unsigned long long)occ_st.walks,
             occ_st.walks ? occ_st.walk_ns / 1e3 / occ_st.walks : 0.0, (unsigned long long)occ_st.copies,
             occ_st.copies ? (double)occ_st.copied / occ_st.copies : 0.0, (unsigned long long)occ_st.fell_back);
        occ_st = {};
    }
}

/// (RX_VK_OCC_COPY) The queries the recording took, copied into the host buffer after its last draws (the recording's end
/// barrier is before this: a query not done by then is copied as not available), then made the host's. Every recording's end
/// forgets them (submit_recording): the queries of one that ended without this are read from the pool
void occ_copy_part(VkCommandBuffer cmd) {
    if (occ_host && occ_part_hi > occ_part_lo) {
        for (uint64_t q = occ_part_lo; q < occ_part_hi;) {
            const uint64_t at = q % kOccPool, k = std::min(occ_part_hi - q, kOccPool - at);
            vkCmdCopyQueryPoolResults(cmd, occ_pool, (uint32_t)at, (uint32_t)k, occ_host_buf, at * 16, 16,
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            q += k;
        }
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1, di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &di);
        occ_st.copies++, occ_st.copied += occ_part_hi - occ_part_lo;
    }
    occ_part_lo = UINT64_MAX, occ_part_hi = 0;
}

/// RX_VK_OCC_QUERIES=2: the runs with colour draws counted too (occ_take)
bool occ_all() {
    static const bool on = getenv("RX_VK_OCC_QUERIES") && atoi(getenv("RX_VK_OCC_QUERIES")) == 2;
    return on;
}
/// A draw's query while a run counts
void occ_take(NvnState& s, DrawArgs& da) {
    if (!s.occ_on || !s.render_enabled || s.raster_discard) return;
    // (only runs of depth-only draws are counted: the game's bounding boxes. A run with a draw that writes colour stays
    // "visible": a query's end waits for its draw to finish, and around the scene's real draws that took ~0.9 ms of GPU time a
    // frame in the Market (RTX 3090) for a few more objects left out. RX_VK_OCC_QUERIES=2: every run counted)
    const bool color = s.chmask.mask != 0;
    s.occ_color |= color;
    if (s.occ_overflow) return;   // (its count unknown already: no more queries for it)
    if (s.occ_next - s.occ_first >= kOccRun || s.occ_next >= s.occ_limit || (color && !occ_all())) {
        s.occ_overflow = true;
        return;
    }
    const uint64_t q = s.occ_next++;
    da.occ_pool = occ_pool, da.occ_query = (uint32_t)(q % kOccPool);
    if (occ_host) occ_part_lo = std::min(occ_part_lo, q), occ_part_hi = std::max(occ_part_hi, q + 1);
}

/// The next recording slot, begun, for the submission that will signal value
Recording& begin_recording(uint64_t value) {
    occ_collect();
    Recording& r = recordings[recording_index];
    recording_index = (recording_index + 1) % kRecordings;
    if (r.value) {
        PROF(PROF_SLOTWAIT);
        const auto t0 = std::chrono::steady_clock::now();
        recording_wait(r);
        st_wait_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        gpu_time_collect(r);
        gprof_collect(r.slot);
    }
    rec::slot_begin(rec_end_pos[r.slot]);   // (RX_VK_RECORDER_OWN: the slot's own resets below and in draw_begin_recording)
    {
        rec::OwnSlot own;
        vkResetCommandPool(dev.device, r.pool, 0);
        vkResetCommandPool(dev.device, r.upload_pool, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(r.cmd, &bi);
        vkBeginCommandBuffer(r.upload, &bi);
    }
    gpu_time_begin(r);
    gprof_begin_recording(r.slot, r.cmd, r.upload);
    r.value = value;
    r.part = ++rec_part_next;
    memory_recording_part(r.part);
    const uint64_t done = completed();
    memory_recording_value(value, done);
    // Loads can submit work for many seconds between presents.
    if ((r.part & 31) == 0) texture_complete_writes(done);
    texture_begin_recording(r.upload, value);
    draw_begin_recording(r.slot, r.cmd, value);
    rec::slot_done();
    return r;
}
/// The game reads what the GPU wrote into its memory once the timeline reaches the submission; waiting on the host makes
/// it visible to the device only (a fence's or a semaphore's access scope is device access), so each recording ends with
/// a memory dependency into the host's reads
void host_read_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1, d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}
/// (RX_VK_RECORDER_PARTS) A part's end and submit as submit_recording decides them, for the recorder thread to make
struct PartSubmit {
    Recording* r;
    uint64_t value;    // (the timeline value it signals, when signal_value)
    uint64_t seq;      // (its number on the recordings' timeline; 0: none)
    uint64_t reflex;   // (NVIDIA Reflex: the game frame its work belongs to, reflex_frame() as it was recorded; 0: none)
    bool uploads, signal_value;
};
void post_part_submit(const PartSubmit& s);   // (below)
void submit_recording(Recording& r, bool signal_value = true, uint64_t signal_at = 0) {
#ifdef TRACY_ENABLE
    HT_SCOPE("submit_recording");
#endif
    occ_part_lo = UINT64_MAX, occ_part_hi = 0;   // (RX_VK_OCC_COPY: copied only if occ_copy_part did, else read from the pool)
    draw_flush_recording(r.slot);
    const bool uploads = texture_uploads_recorded();
    gprof_submitted(r.slot, uploads);
    gpu_time_end(r);
    host_read_barrier(r.cmd);
    if (!rec::parts_wanted()) {   // (RX_VK_RECORDER_PARTS: the recorder ends it instead, part_submit)
        vkEndCommandBuffer(r.upload);
        vkEndCommandBuffer(r.cmd);
        rec_end_pos[r.slot] = rec::produced();   // (the ends waited for the recorder: it is past this)
    }
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = timeline;
    signal.value = signal_at ? signal_at : r.value;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cb[2]{{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}};
    uint32_t n = 0;
    if (uploads) cb[n++].commandBuffer = r.upload;
    cb[n++].commandBuffer = r.cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = n;
    si.pCommandBufferInfos = cb;
    VkSemaphoreSubmitInfo signals[2] = {signal, {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}};
    uint32_t nsig = signal_value ? 1 : 0;
    uint64_t seq = 0;
    // (longshot) Where this recording is to be found by the threads that wait for it (value_rec, part_rec). A held submit
    // is put there only once it is in the list of held ones, under dev.present_mu: a thread that finds a recording and
    // waits for it flushes first (wait_rec_seq), and one found before it was in the list had nothing to flush and waited
    // for a signal still held, until something else sent the batch (also before the flushes looked at pend_held: found
    // by mooki000 there)
    const auto map_recording = [&] {
        std::lock_guard mk(rec_map_mu);
        // (a submission is recorded in parts (maybe_split), each part's recording with the
        // submission's value: the value's recording is its last part's, the latest seq. emplace kept the first part's,
        // and a wait for the submission's fence (wait, the readback wait) would come back once its first part was done:
        // the game would write the next frame's uniforms, vertices and its exposure meter over memory the later parts still
        // read (with no VRAM mirror, an iGPU: guest memory read in place): flashes and garbage)
        const auto finished_in = [&](uint64_t v) {
            auto it = value_rec.find(v);
            if (it == value_rec.end()) value_rec.emplace(v, seq);
            else if (it->second < seq) it->second = seq;
        };
        for (uint64_t v : rec_open_values) finished_in(v);
        if (r.value) finished_in(r.value);
        rec_open_values.clear();
        while (value_rec.size() > 8192) value_rec.erase(value_rec.begin());
        if (r.part) part_rec.emplace_hint(part_rec.end(), r.part, seq);
        while (part_rec.size() > 8192) part_rec.erase(part_rec.begin());
        rec_map_cv.notify_all();
    };
    if (rec_timeline) {   // (and its number on the recordings' timeline; every caller holds replay_mu: in order)
        seq = ++rec_seq;
        signals[nsig].semaphore = rec_timeline;
        signals[nsig].value = seq;
        signals[nsig].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        nsig++;
        if (!batch_on()) map_recording();
    }
    si.signalSemaphoreInfoCount = nsig;
    si.pSignalSemaphoreInfos = nsig ? signals : nullptr;
    // (RX_VK_RECORDER_PARTS: its two ends and its submit go to the recorder thread after the part's packets, and the replay goes on
    // into the next part at once. It is found by the waiters here, held or not: a waiter waits for its submit first, wait_rec_seq)
    // doc: docs/dev/reference/threads.md#the-hand-off-by-part
    if (rec::parts_wanted()) {
        if (seq && batch_on()) map_recording();
        return post_part_submit({&r, signal.value, seq, reflex_frame(), uploads, signal_value});
    }
    std::lock_guard lk(dev.present_mu);   // (one queue: submits and presents take turns)
    if (batch_on()) {
        // (NVIDIA Reflex: the batch is tagged with the game frame its first command buffer belongs to, read here. A batch that
        // spans two game frames counts as the first's; one begun before the frame's sleep returned (reflex_frame() 0) goes
        // untagged. Batching is on by default only with RADV, so on NVIDIA this matters only with RX_VK_BATCH=1)
        if (pend.cbs.empty()) batch_next++, pend_since = std::chrono::steady_clock::now(), pend.reflex = reflex_frame();
        for (uint32_t i = 0; i < n; i++) pend.cbs.push_back(cb[i]);
        pend_held.store(true, std::memory_order_release);
        if (seq) map_recording();   // (held, and said so: now it may be found and waited for)
        if (signal_value) pend.signal = std::max(pend.signal, signal.value), pend.any_signal = true;
        if (seq) pend.rec = std::max(pend.rec, seq);
        r.batch = batch_next;
        r.submitted = true;
        // (RX_VK_BATCH_MAX=<command buffers>: held at most this many, default 32)
        static const size_t cap = getenv("RX_VK_BATCH_MAX") ? (size_t)std::max(1, atoi(getenv("RX_VK_BATCH_MAX"))) : 32;
        if (pend.cbs.size() >= cap) flush_reason = FR_CAP, queue_flush_locked();
        if (!pend.cbs.empty() && batch_gpu_idle()) flush_reason = FR_OTHER, queue_flush_locked();   // (the GPU idle: batch_gpu_idle)
        return;
    }
    PROF(PROF_QSUBMIT);
    si.pNext = reflex_submit_id(nullptr);   // (NVIDIA Reflex: the frame being made)
    VkSemaphoreSubmitInfo sparse_wait;   // (longshot: after the sparse mirrors' binds, RX_VK_MIRROR_SPARSE)
    if (memory_sparse_wait(&sparse_wait)) si.waitSemaphoreInfoCount = 1, si.pWaitSemaphoreInfos = &sparse_wait;
    const VkResult res = submit_or_retry(si, r.fence);
    r.submitted = res == VK_SUCCESS;
    static int failures;
    if (res != VK_SUCCESS && failures++ < 8) logf("vk: submit %llu failed (%d)", (unsigned long long)r.value, (int)res);
    if (res == VK_ERROR_DEVICE_LOST) device_lost();
    if (res == VK_SUCCESS && signal_value) note_timeline_sent(signal.value);
}
/// (RX_VK_RECORDER_PARTS) A part's end and submit: on the recorder thread, in the ring's order after the part's packets
/// (post_part), or on the replay's own thread when the recorder did not take it (its ends then wait for the ring, as before). Its
/// two command buffers ended, then submitted with what submit_recording chose (the timeline value, the recording's number, the
/// Reflex frame), or held with RX_VK_BATCH under the queue's lock. On the recorder its ends and submit wait for nothing
/// (boundary()), and the queue's lock says the recorder waits for it (QueueMutex)
void part_submit(const PartSubmit& s) {
    Recording& r = *s.r;
    if (vkEndCommandBuffer(r.upload) == VK_ERROR_DEVICE_LOST) device_lost();
    if (vkEndCommandBuffer(r.cmd) == VK_ERROR_DEVICE_LOST) device_lost();
    if (gtl_on()) gtl_submit_ns[r.slot] = gtl_now();   // (RX_VK_GPU_TIMELINE: the submit's own time, here, not the part's end)
    VkSemaphoreSubmitInfo signals[2] = {{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}};
    uint32_t nsig = 0;
    if (s.signal_value) {
        signals[nsig].semaphore = timeline, signals[nsig].value = s.value, signals[nsig].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        nsig++;
    }
    if (s.seq) {
        signals[nsig].semaphore = rec_timeline, signals[nsig].value = s.seq, signals[nsig].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        nsig++;
    }
    VkCommandBufferSubmitInfo cb[2]{{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}};
    uint32_t n = 0;
    if (s.uploads) cb[n++].commandBuffer = r.upload;
    cb[n++].commandBuffer = r.cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = n;
    si.pCommandBufferInfos = cb;
    si.signalSemaphoreInfoCount = nsig;
    si.pSignalSemaphoreInfos = nsig ? signals : nullptr;
    {
        std::lock_guard lk(dev.present_mu);   // (one queue: submits and presents take turns)
        if (batch_on()) {   // (as submit_recording holds one; it was put where the waiters find it before it was handed over)
            if (pend.cbs.empty()) batch_next++, pend_since = std::chrono::steady_clock::now(), pend.reflex = s.reflex;
            for (uint32_t i = 0; i < n; i++) pend.cbs.push_back(cb[i]);
            pend_held.store(true, std::memory_order_release);
            if (s.signal_value) pend.signal = std::max(pend.signal, s.value), pend.any_signal = true;
            if (s.seq) pend.rec = std::max(pend.rec, s.seq);
            r.batch = batch_next;
            r.submitted = true;
            static const size_t cap = getenv("RX_VK_BATCH_MAX") ? (size_t)std::max(1, atoi(getenv("RX_VK_BATCH_MAX"))) : 32;
            if (pend.cbs.size() >= cap) flush_reason = FR_CAP, queue_flush_locked();
            if (!pend.cbs.empty() && batch_gpu_idle()) flush_reason = FR_OTHER, queue_flush_locked();   // (the GPU idle: batch_gpu_idle)
            // (a wait that gave up on the parts before this one was held: its flush may have found nothing, and it waits for this)
            if (!pend.cbs.empty() && rec::take_flush_owed()) flush_reason = FR_WAIT, queue_flush_locked();
        } else {   // (not PROF_QSUBMIT: the replay's phases are the render thread's time, and this is the recorder's)
            si.pNext = reflex_submit_id(nullptr, s.reflex);   // (NVIDIA Reflex: the frame it was recorded in)
            VkSemaphoreSubmitInfo sparse_wait;   // (longshot: after the sparse mirrors' binds, RX_VK_MIRROR_SPARSE)
            if (memory_sparse_wait(&sparse_wait)) si.waitSemaphoreInfoCount = 1, si.pWaitSemaphoreInfos = &sparse_wait;
            const VkResult res = submit_or_retry(si, r.fence);
            r.submitted = res == VK_SUCCESS;
            static std::atomic<int> failures{0};
            if (res != VK_SUCCESS && failures++ < 8) logf("vk: submit %llu failed (%d)", (unsigned long long)r.value, (int)res);
            if (res == VK_ERROR_DEVICE_LOST) device_lost();   // (the recorder's too: the report, then the process ends)
            if (res == VK_SUCCESS && s.signal_value) note_timeline_sent(s.value);
        }
    }
    if (s.seq) note_seq_sent(s.seq);
}
/// (RX_VK_RECORDER_PARTS) The part's end and submit to the recorder thread (rec::post_part), its number kept for the slot's reuse;
/// not taken (no ring, the queue's lock held here): made here at once
[[noreturn]] void replay_fatal(const char* why);   // (below)
void post_part_submit(const PartSubmit& s) {
    // (never under a queue lock: part_submit takes dev.present_mu, and a part handed over from under it could not be submitted)
    if (rec::t_queue_held) replay_fatal("a part's submit asked for under the queue's lock (RX_VK_RECORDER_PARTS)");
    Recording& r = *s.r;
    rec_ticket[r.slot] = rec::post_part([s] { part_submit(s); });
    if (!rec_ticket[r.slot]) part_submit(s), rec::part_made_here();
    rec_end_pos[r.slot] = rec::produced();   // (the ring's position after its end and submit: R is past it once they are made)
}

// ---- NVN state (the draw path's, vk_state.h) ----
NvnState state;
// (each of the game's queues has one of its own, as the console's channels do: their command buffers are replayed in
// turns at their event waits (submit_job), and one's draws went on with what another's had set. A queue seen first
// starts from the state the replay was at, as all of them did before. The queue-less replays (RX_VK_ASYNC=0, the
// bench) keep the one above.)
std::unordered_map<uint64_t, NvnState> qstates;
NvnState* cur_state = &state;
/// (longshot) The first query of the oldest occlusion run still open in a queue's state other than except (UINT64_MAX: none)
uint64_t occ_open_oldest(const NvnState* except) {
    uint64_t m = UINT64_MAX;
    if (&state != except && state.occ_on) m = state.occ_first;
    for (const auto& [q, t] : qstates)
        if (&t != except && t.occ_on) m = std::min(m, t.occ_first);
    return m;
}
/// The state the replay goes on with: its queue's. (The draw path's caches made afresh: they are keyed on the state's
/// counters, which the queues' states share no order of.)
void use_queue_state(uint64_t queue) {
    NvnState* s = &state;
    if (queue) {
        auto it = qstates.find(queue);
        if (it == qstates.end()) {
            it = qstates.emplace(queue, *cur_state).first;
            occ_state_copied(it->second);   // (longshot: the other queue's open occlusion run is not this one's)
        }
        s = &it->second;
    }
    if (s == cur_state) return;
    cur_state = s;
    static uint64_t gen_base = 0;
    gen_base += 1ull << 32;
    s->bind_gen = gen_base, s->rt_serial = gen_base;
    for (int k = 0; k < kNvnStages; k++) s->tex_gen[k] = gen_base, s->ssbo_gen[k] = gen_base;
}

/// payload's copy of a payload shorter than its type, rare: a function of its own, so that the compiler can't merge it with the
/// copy at the type's size into one call of memcpy with a chosen size (clang 23 does, at -O2)
[[gnu::noinline, gnu::cold]] void payload_short(void* v, const uint8_t* a, uint32_t bytes) { std::memcpy(v, a, bytes); }

template <class T>
T payload(const uint8_t* a, uint32_t bytes) {
    // (a payload at least as long as T, nearly every one, is copied at T's size: a copy the compiler makes inline. A variable size
    // was a call of the C library's memcpy for every op replayed, a few percent of the Steam Deck's render thread, where it shows
    // as __memmove_avx_unaligned_erms)
    T v{};
    if (bytes >= sizeof v) [[likely]]
        std::memcpy(&v, a, sizeof v);
    else
        payload_short(&v, a, bytes);
    return v;
}

int64_t st_barriers, st_ub_updates, st_ub_overlays, st_submits;
std::map<int32_t, int64_t> st_barrier_flags;   // (the game's nvnCommandBufferBarrier bits: how many of each combination, the frame log)
std::mutex st_barrier_flags_mu;
// ---- events in replay order: the values the stream's signals and the CPU's set, for the GPU's waits on them. The
// game's three queues order their work with events (a command buffer waits for another queue's to reach a stage);
// replayed on one queue in the order they were submitted, a waiter submitted before its signaller ran first ----
std::unordered_map<uint64_t, uint32_t> ev_values;
// (longshot) The CPU's signals, while a replay holds replay_mu: the main thread stopped for it in nvnEventSignal, ~1.7 ms
// a frame of the critical path. They wait here for the replay's next look at the events (ev_cpu_apply).
std::mutex ev_cpu_mu;
std::vector<std::pair<uint64_t, uint32_t>> ev_cpu_pending;
std::atomic<bool> ev_cpu_any{false};
// (longshot) how many signals the CPU has made, and how many of them a look at the stopped queues has been taken since
// (schedule, whole): equal, no queue is stopped at a wait the CPU has met and nobody has looked at (sync_settle_met)
std::atomic<uint64_t> ev_cpu_signals{0}, ev_cpu_settled{0};
// (and whether a stopped queue waits on an event neither the stream nor nvnEventSignal has ever set: its value is read
// from guest memory (event_value_in), where a store of the CPU's or a write-back can meet the wait with no signal counted;
// update_blocked)
std::atomic<bool> ev_wait_in_memory{false};
// (a look's count, kept until the burst it was taken in has finished its recording: burst_end. sync_order_mu)
uint64_t ev_looked = 0;
bool ev_looked_any = false;
std::unordered_set<uint64_t> cpu_events;   // (events the CPU has signalled, nvnEventSignal; ev_cpu_mu)
void ev_cpu_apply() {
    if (!ev_cpu_any.load(std::memory_order_acquire)) return;
    std::lock_guard lk(ev_cpu_mu);
    for (const auto& e : ev_cpu_pending) ev_values[e.first] = e.second, cpu_events.insert(e.first);
    ev_cpu_pending.clear();
    ev_cpu_any.store(false, std::memory_order_release);
}
int64_t st_waits_met, st_waits_unmet;
bool event_wait_met(uint32_t have, uint32_t mode, uint32_t want) {
    return mode == 0 ? have == want : (int32_t)(have - want) >= 0;   // (NVN: EQUAL, GEQUAL_WRAP)
}
using EvMap = std::unordered_map<uint64_t, uint32_t>;
uint32_t event_value_in(const EvMap& ev, uint64_t gpu) {   // (one the stream never set: as the CPU has it)
    auto it = ev.find(gpu);
    if (it != ev.end()) return it->second;
    uint32_t v = 0;
    host->gpu_read(gpu, &v, 4);
    return v;
}
thread_local bool suspend_ok = false;               // (a serial replay that stops at a wait not met: submit_job)
thread_local const uint8_t* suspend_at = nullptr;   // (the wait it stopped at)
// ---- (longshot) a wait for an event the CPU signals that is met already when the replay reaches it ----
// The game writes part of a frame's data after it has submitted the frame's work, then signals the event that work waits
// for (OoT: the scene buffer's light grid, among others). The console's GPU reads memory after its wait. Here a queue that
// stopped at the wait gets a capture when it goes on (resume_queue). One that found the wait met would go on with its submit's
// capture, and when the signal came after that capture was begun, the frame would be drawn with that data as it was before
// the game wrote it: the lit scene and Link dark for a frame. Such a wait stops there too, once, and schedule()
// lets it on at once through resume_queue and its capture. A signal made before the newest capture was begun needs
// none: what the game wrote up to it is in that capture. On the submitting thread only: the render thread's capture is
// taken at the signal itself (event_signal_queued).
std::unordered_map<uint64_t, uint64_t> ev_cpu_captures;   // (an event: memory_captures_begun at the CPU's last signal of it; ev_cpu_mu)
thread_local const uint8_t* met_resumed_at = nullptr;     // (the wait a resume starts at: not stopped at again)
int64_t st_met_stops;
bool async_on();
bool met_wait_capture_on() {
    static const bool v = !async_on();
    return v;
}
/// Whether the CPU's last signal of this event came after the newest capture was begun: what the game wrote before that
/// signal may be in no capture yet
bool cpu_signal_uncaptured(uint64_t gpu) {
    const uint64_t begun = memory_captures_begun();   // (first: a signal made meanwhile counts as after it)
    std::lock_guard ek(ev_cpu_mu);
    const auto it = ev_cpu_captures.find(gpu);
    return it != ev_cpu_captures.end() && it->second >= begun;
}
uint64_t ts_anchor_ns = 0;   // (the last present's time: counter timestamps are scaled from it)

/// (longshot) The binding of up to n bytes of guest memory from gpu that one import holds, n cut to its length: all of it when an
/// import can hold it; else the part in the game pool holding gpu (a range across two pools has no import: pool_lookup), or,
/// outside every pool, none for the bytes up to the next one. None too when the range's import failed (logged there)
BufferRef gpu_piece_bind(uint64_t gpu, uint64_t& n) {
    if (const BufferRef b = gpu_buffer_bind(gpu, n, true)) return b;
    bool in_pool = false;
    const uint64_t k = gpu_pool_piece(gpu, n, in_pool);
    if (k == n) return {};   // (in one pool, whose import failed, or in none)
    n = k;
    return in_pool ? gpu_buffer_bind(gpu, n, true) : BufferRef{};
}

/// (longshot) k bytes into b on the GPU timeline, after a barrier: an update when they are 4-byte aligned (vkCmdUpdateBuffer's
/// rule, up to 64 KB), else put in the recording's stream and copied into place, a copy having no alignment rule. false: no
/// room in the stream, and nothing recorded
bool gpu_put(VkCommandBuffer cmd, const BufferRef& b, const uint8_t* d, uint64_t k) {
    const bool update = !(b.offset & 3) && !(k & 3) && k <= 65536;
    const BufferRef s = update ? BufferRef{} : stream_copy(d, k);
    if (!update && !s) return false;
    full_barrier(cmd);   // overlapping inline writes must reach memory in stream order
    if (update) {
        vkCmdUpdateBuffer(cmd, b.buffer, b.offset, k, d);
    } else {
        const VkBufferCopy r{s.offset, b.offset, k};
        vkCmdCopyBuffer(cmd, s.buffer, b.buffer, 1, &r);
    }
    return true;
}

/// A GPU-timeline write of guest memory (the stream's own writes: they reach memory in command order). (longshot) Every part an
/// import holds goes on the GPU, an unaligned one too, one import at a time; only bytes no import holds are written on the CPU,
/// at record time, which is exact outside every pool: no import there, so the GPU neither reads nor writes them. An unaligned
/// write, or one across two game pools, went on the CPU whole: out of command order, ahead of the draws recorded before it
void gpu_write(VkCommandBuffer cmd, uint64_t gpu, const void* data, uint64_t n) {
    const uint8_t* d = (const uint8_t*)data;
    while (n) {
        uint64_t k = std::min<uint64_t>(n, 65536);
        const BufferRef b = gpu_piece_bind(gpu, k);
        if (!b || !gpu_put(cmd, b, d, k)) guest_cpu_write(gpu, d, k);   // (no import, a failed one, or a full stream: on the CPU)
        gpu += k, d += k, n -= k;
    }
}

/// (longshot) n bytes of guest memory from gpu cleared to value on the GPU timeline (NOP_CLEAR_BUF, between its barriers): each
/// part an import holds as a fill of whole words, the up to 3 bytes on either side copied from the recording's stream (a fill
/// takes aligned words only), stamped as the GPU's. The rest on the CPU (an unaligned clear, or one across two game pools, went
/// there whole, out of command order), unless in_stream: the replay cleared those bytes on the CPU in stream order already, and
/// the GPU's parts are not stamped. True when every byte went on the GPU
bool gpu_fill(VkCommandBuffer cmd, uint64_t gpu, uint64_t n, uint32_t value, bool in_stream) {
    uint8_t pattern[8];   // (byte i of the clear is the value's byte i % 4: pattern[i % 4] on)
    std::memcpy(pattern, &value, 4), std::memcpy(pattern + 4, &value, 4);
    bool all = true;
    for (uint64_t done = 0; done < n;) {
        const uint64_t at = gpu + done;
        uint64_t k = n - done;
        const BufferRef b = gpu_piece_bind(at, k);
        const uint64_t head = std::min<uint64_t>(k, (0 - b.offset) & 3), mid = (k - head) & ~3ull, tail = k - head - mid;
        uint8_t ends[6];   // (the head's bytes, then the tail's)
        std::memcpy(ends, pattern + (done & 3), head);
        std::memcpy(ends + head, pattern + ((done + head + mid) & 3), tail);
        const BufferRef s = b && head + tail ? stream_copy(ends, head + tail) : BufferRef{};
        const bool put = b && (s || !(head + tail));
        if (put) {
            uint32_t w;
            std::memcpy(&w, pattern + ((done + head) & 3), 4);
            if (mid) vkCmdFillBuffer(cmd, b.buffer, b.offset + head, mid, w);
            VkBufferCopy r[2];
            uint32_t nr = 0;
            if (head) r[nr++] = {s.offset, b.offset, head};
            if (tail) r[nr++] = {s.offset + head, b.offset + head + mid, tail};
            if (nr) vkCmdCopyBuffer(cmd, s.buffer, b.buffer, nr, r);
            const uint64_t guest = host->gpu_guest(at);
            if (guest && !in_stream) memory_stamp_gpu(guest, k);
        } else if (!in_stream) {   // (no import, a failed one, or a full stream: on the CPU)
            uint32_t buf[1025];
            for (auto& x : buf) x = value;
            for (uint64_t off = 0; off < k;) {
                const uint64_t m = std::min<uint64_t>(4096, k - off);
                if (!guest_cpu_write(at + off, (const uint8_t*)buf + (done & 3), m)) break;
                off += m;
            }
        }
        all = all && put;
        done += k;
    }
    return all;
}

// ---- uniform updates (UPDATE_UB): written at the end of their recording; ub_overlay applies them to copies ----
struct UbUpdate {
    uint64_t value;   // the submission that writes it
    uint64_t dst;
    std::vector<uint8_t> bytes;
};
std::deque<UbUpdate> ub_updates;   // in stream order, oldest first
std::atomic<uint64_t> ub_list_epoch{1};   // (bumped when that deque gains or loses an update)
thread_local uint64_t ub_gen = 0;   // (each recording thread's: its draws' overlays change with what it passed)

thread_local size_t ub_limit = SIZE_MAX;   // (the uniform updates a thread's reads see: all of them; 0 on a background thread, ub_thread_off)
size_t ub_first_of_recording = 0;  // (the current recording's first entry)
uint64_t rec_value_now = 0;

void ub_prune(uint64_t done) {
    if (ub_updates.empty() || ub_updates.front().value > done) return;
    while (!ub_updates.empty() && ub_updates.front().value <= done) {
        ub_updates.pop_front();
        if (ub_first_of_recording) ub_first_of_recording--;
    }
    ub_list_epoch.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

uint64_t ub_generation() {
    return ub_gen;
}
void ub_thread_off() {
    ub_limit = 0;
}
/// The open gap around [addr, addr+bytes) that no update covers. False when one does. [lo, hi) is that gap.
bool ub_gap(std::deque<UbUpdate>::const_iterator first, std::deque<UbUpdate>::const_iterator last,
            uint64_t addr, uint64_t bytes, uint64_t* lo, uint64_t* hi) {
    if (!bytes || addr + bytes < addr) return false;
    uint64_t left = 0, right = ~uint64_t{0};
    const uint64_t stop = addr + bytes;
    for (auto it = first; it != last; ++it) {
        const uint64_t start = it->dst, end = start + it->bytes.size();
        if (start < stop && addr < end) return false;
        if (end <= addr && end > left) left = end;
        if (start >= stop && start < right) right = start;
    }
    *lo = left;
    *hi = right;
    return true;
}
struct UbGapCache {
    uint64_t epoch = 0, lo = 0, hi = 0;
    size_t first = 0, lim = 0;
    bool on = false;
};
thread_local UbGapCache ub_gap_cache;
bool ub_gap_holds(uint64_t addr, uint64_t n, size_t first, size_t lim) {
    return n && ub_gap_cache.on && ub_gap_cache.epoch == ub_list_epoch.load(std::memory_order_relaxed) &&
           ub_gap_cache.first == first && ub_gap_cache.lim == lim && addr >= ub_gap_cache.lo &&
           addr + n >= addr && addr + n <= ub_gap_cache.hi;
}
bool ub_try_gap(size_t first, size_t lim, uint64_t addr, uint64_t n) {
    if (first > lim) return false;
    const uint64_t epoch = ub_list_epoch.load(std::memory_order_relaxed);
    // (a background thread's range is empty: it reads nothing of the deque, not even its start, while the replay
    // thread changes it)
    using It = std::deque<UbUpdate>::const_iterator;
    const It from = first == lim ? It{} : ub_updates.cbegin() + first, to = first == lim ? It{} : from + (lim - first);
    uint64_t lo = 0, hi = 0;
    if (!ub_gap(from, to, addr, n, &lo, &hi)) return false;
    if (ub_list_epoch.load(std::memory_order_relaxed) != epoch) return false;   // (the list changed: do not keep the gap)
    ub_gap_cache = {epoch, lo, hi, first, lim, true};
    return true;
}
// (a draw reads the same uniform words as the draw before it. The gap is what says later updates missed the word:
// it is kept while that gap covers it and its page is unstamped)
struct UbWord {
    uint64_t addr = 0, rec = 0;
    uint64_t page = 0;   // (its page's generation when noted: memory_page_stamp)
    uint32_t v = 0;
    bool on = false;
};
thread_local UbWord ub_words[128];
uint32_t ub_word_index(uint64_t addr) {
    // Words 512 bytes or a page apart share the low seven word bits. Add higher bits to separate those strides
    // while keeping runs of up to 64 words collision-free. Hits still compare the full address in ub_word_hit.
    const uint64_t word = addr >> 2;
    return (uint32_t)((word + (word >> 7) + (word >> 14)) & 127);
}
bool ub_word_hit(uint64_t addr, size_t lim, uint32_t* out) {
    UbWord& w = ub_words[ub_word_index(addr)];
    if (!w.on || w.addr != addr || w.rec != rec_value_now || w.page != memory_page_stamp(addr)) return false;
    if (!ub_gap_holds(addr, 4, 0, lim)) return false;
    *out = w.v;
    return true;
}
// (a range an update does overlap: the stream copy stays while the gap's update list is unchanged and the copy is
// still in the recording's stream. A range in the gap has no copy)
struct UbCopy {
    const uint8_t* base = nullptr;
    uint64_t addr = 0, n = 0, at = 0, rec = 0, view = 0, epoch = 0;
    size_t lim = 0, first = 0;
    bool on = false;
};
thread_local UbCopy ub_copies[8];
bool ub_copy_hit(uint64_t addr, uint64_t n, const uint8_t* base, size_t first, size_t lim, uint64_t* at) {
    UbCopy& c = ub_copies[((addr >> 4) ^ n) & 7];
    if (!c.on || c.base != base || c.addr != addr || c.n != n || c.first != first || c.lim != lim ||
        c.rec != rec_value_now || c.epoch != ub_list_epoch.load(std::memory_order_relaxed) ||
        c.view != memory_capture_view())
        return false;
    if (!stream_holds(base, c.at, n)) return false;
    *at = c.at;
    return true;
}
void ub_copy_keep(uint64_t addr, uint64_t n, const uint8_t* base, size_t first, size_t lim, uint64_t at,
                  uint64_t view, uint64_t epoch) {
    if (view != memory_capture_view()) return;
    if (epoch != ub_list_epoch.load(std::memory_order_relaxed)) return;
    UbCopy& c = ub_copies[((addr >> 4) ^ n) & 7];
    c = {};
    c.on = true, c.base = base, c.addr = addr, c.n = n, c.at = at, c.first = first, c.lim = lim;
    c.rec = rec_value_now, c.view = view, c.epoch = epoch;
}
uint64_t ub_overlay(uint64_t addr, uint64_t n, uint8_t* base, uint64_t (*alloc)(uint64_t n, uint64_t align), uint64_t align) {
    // (a background thread: none, not even the list's size)
    const size_t lim = ub_limit ? std::min(ub_limit, ub_updates.size()) : 0;
    static const bool cur_only = env_on("RX_VK_UB_CUR_ONLY");   // (A/B: this submission's updates only)
    const size_t first = cur_only ? std::min(ub_first_of_recording, lim) : 0;
    if (ub_gap_holds(addr, n, first, lim)) return UINT64_MAX;
    const uint64_t view = memory_capture_view();
    uint64_t kept = 0;
    if (ub_copy_hit(addr, n, base, first, lim, &kept)) return kept;
    if (ub_try_gap(first, lim, addr, n)) return UINT64_MAX;
    const uint64_t epoch = ub_list_epoch.load(std::memory_order_relaxed);
    bool any = false;
    for (size_t i = first; i < lim; i++) {
        const UbUpdate& u = ub_updates[i];
        if (u.dst < addr + n && addr < u.dst + u.bytes.size()) {
            any = true;
            break;
        }
    }
    if (!any) return UINT64_MAX;
    const uint64_t at = alloc(n, align);
    if (at == UINT64_MAX) return UINT64_MAX;
    uint8_t* p = base + at;
    if (!memory_read_as_submitted(addr, p, n)) std::memset(p, 0, n);
    for (size_t i = first; i < lim; i++) {
        const UbUpdate& u = ub_updates[i];
        const uint64_t lo = std::max(addr, u.dst), hi = std::min(addr + n, u.dst + u.bytes.size());
        if (lo < hi) std::memcpy(p + (lo - addr), u.bytes.data() + (lo - u.dst), hi - lo);
    }
    st_ub_overlays++;
    ub_copy_keep(addr, n, base, first, lim, at, view, epoch);
    return at;
}

/// Guest memory as the stream's place sees it: as it was at the submission (the capture), with the uniform updates
/// the stream made before this place over it. For what the replay reads on the CPU for
/// the GPU's sake (handles in uniform and storage buffers): the game's memory now may be its next frames'.
bool ub_read(uint64_t addr, void* dst, uint64_t n) {
    const size_t lim = ub_limit ? std::min(ub_limit, ub_updates.size()) : 0;
    if (n == 4) {
        uint32_t v = 0;
        if (ub_word_hit(addr, lim, &v)) {
            std::memcpy(dst, &v, 4);
            return true;
        }
    }
    const uint64_t page = n == 4 ? memory_page_stamp(addr) : 0;
    if (!memory_read_as_submitted(addr, dst, n)) return false;
    // (most words are not inside an inline update: the scan that finds that is kept for the gap, and the next words
    // in the gap do not walk the list. A word kept from that gap is the capture's own bytes)
    if (ub_gap_holds(addr, n, 0, lim) || ub_try_gap(0, lim, addr, n)) {
        if (n == 4 && page == memory_page_stamp(addr)) {
            UbWord& w = ub_words[ub_word_index(addr)];
            uint32_t v = 0;
            std::memcpy(&v, dst, 4);
            w = {};
            w.on = true, w.addr = addr, w.v = v, w.rec = rec_value_now, w.page = page;
        }
        return true;
    }
    for (size_t i = 0; i < lim; i++) {
        const UbUpdate& u = ub_updates[i];
        const uint64_t lo = std::max(addr, u.dst), hi = std::min(addr + n, u.dst + u.bytes.size());
        if (lo < hi) std::memcpy((uint8_t*)dst + (lo - addr), u.bytes.data() + (lo - u.dst), hi - lo);
    }
    return true;
}

namespace {

/// (longshot) Uniform updates on their way into one import's buffer as ONE copy from the recording's stream: its regions, and
/// the destination's bytes they hold (sorted, none overlapping another: a copy's regions may not overlap in the destination,
/// and two updates of the same bytes have to land in stream order)
struct UbBatch {
    VkBuffer src = VK_NULL_HANDLE, dst = VK_NULL_HANDLE;
    std::vector<VkBufferCopy> regions;
    std::vector<std::pair<uint64_t, uint64_t>> held;   // ([lo, hi) of dst, by lo)
};
/// Whether k bytes at `at` of dst, staged at `from` of src, can join the batch: the same two buffers (another import may hold
/// the same guest memory: its writes stay behind a barrier), and no byte an earlier region of the batch writes. Joined when true
bool ub_batch_add(UbBatch& b, VkBuffer src, VkBuffer dst, uint64_t from, uint64_t at, uint64_t k) {
    if (!b.regions.empty() && (b.src != src || b.dst != dst)) return false;
    const auto next = std::lower_bound(b.held.begin(), b.held.end(), std::pair<uint64_t, uint64_t>{at, 0});
    if (next != b.held.end() && next->first < at + k) return false;
    if (next != b.held.begin() && std::prev(next)->second > at) return false;
    b.held.insert(next, {at, at + k});
    b.src = src, b.dst = dst;
    b.regions.push_back({from, at, k});
    return true;
}
/// The batch recorded: one barrier, then its regions as one copy (what came before it in the stream has landed)
void ub_batch_emit(VkCommandBuffer cmd, UbBatch& b) {
    if (b.regions.empty()) return;
    full_barrier(cmd);   // overlapping inline writes must reach memory in stream order
    vkCmdCopyBuffer(cmd, b.src, b.dst, (uint32_t)b.regions.size(), b.regions.data());
    b.regions.clear(), b.held.clear();
}

/// The recording's uniform updates into guest memory (after its draws: the ones that came after an update read
/// their overlay copies). (longshot) Batched: each update's bytes go into the recording's stream, and the updates that
/// write different bytes of one import land as one copy of many regions behind one barrier. A batch ends, and the next
/// starts behind a barrier of its own, where an update writes bytes an earlier one of the batch wrote, or goes to another
/// buffer: stream order, as when each update had its own barrier and vkCmdUpdateBuffer (80 a frame: 0.33 ms of GPU time).
/// One with no room in the stream is written as before (gpu_put), after the batch before it; bytes no import holds on the CPU
void flush_ub_updates(VkCommandBuffer cmd) {
    if (ub_first_of_recording >= ub_updates.size()) return;
    memory_writer_note(kRbWriterUb, 0);   // (RX_VK_LOG_RB_WRITERS: what these write is the uniform updates')
    GprofSpan gp(cmd, "ubupd (the stream's uniform updates -> guest memory, with its barriers)");
    gp.items = (int)(ub_updates.size() - ub_first_of_recording);
    UbBatch batch;
    for (size_t i = ub_first_of_recording; i < ub_updates.size(); i++) {
        uint64_t gpu = ub_updates[i].dst, n = ub_updates[i].bytes.size();
        const uint8_t* d = ub_updates[i].bytes.data();
        gp.bytes += n;
        while (n) {
            uint64_t k = std::min<uint64_t>(n, 65536);
            const BufferRef b = gpu_piece_bind(gpu, k);
            const BufferRef s = b ? stream_copy(d, k) : BufferRef{};
            if (s) {
                if (!ub_batch_add(batch, s.buffer, b.buffer, s.offset, b.offset, k)) {
                    ub_batch_emit(cmd, batch);
                    ub_batch_add(batch, s.buffer, b.buffer, s.offset, b.offset, k);
                }
            } else if (b) {   // (a full stream: after what the batch holds, as an update where it can be one)
                ub_batch_emit(cmd, batch);
                if (!gpu_put(cmd, b, d, k)) guest_cpu_write(gpu, d, k);
            } else {
                guest_cpu_write(gpu, d, k);   // (no import, or a failed one: on the CPU, as gpu_write)
            }
            gpu += k, d += k, n -= k;
        }
    }
    ub_batch_emit(cmd, batch);
    full_barrier(cmd);
}

/// (mesh shading: the stages its resources are bound to, beyond NVN's six)
void log_stage_bind(uint32_t op, int stage) {
    static int logged[64];
    if (stage >= 0 && stage < 64 && logged[stage]++ < 3) logf("vk: record %u binds to stage %d", op, stage);
}

void maybe_split(VkCommandBuffer& cmd);
// (longshot) A part's end that waits for the next plain draw (maybe_split, split_part): see part_ends_before_draw
void split_part(VkCommandBuffer& cmd);
bool part_end_wanted = false;
void early_split(VkCommandBuffer& cmd);   // (RX_VK_READBACK_EARLY: a part's end after a readback's writer)
bool early_split_pending = false;   // (RX_VK_READBACK_EARLY: a writer of a waited-for range is in the part being recorded)
extern int64_t part_work;
extern uint64_t rec_cur_value;   // (the submission being recorded: the occlusion queries' value)

/// The handles already stored, so a rebind of the same ones does not rebuild descriptors
bool handles_unchanged(const uint64_t* have, const uint64_t* next, int n) {
    if (n < 0) return false;
    for (int i = 0; i < n; i++)
        if (have[i] != next[i]) return false;
    return true;
}
/// A uniform or storage binding already stored
bool buf_unchanged(int32_t stage, int32_t index, uint64_t addr, uint64_t size, int32_t nstage, int32_t nindex,
                   uint64_t naddr, uint64_t nsize) {
    return stage == nstage && index == nindex && addr == naddr && size == nsize;
}

// (longshot) RX_VK_MESH_CONV=2: the scans after the game's barriers, by why each ended (mesh_conv.h Stop), and the ops that ended
// them (kStopOp), for the log's line every 120 frames
std::atomic<int64_t> st_mc_stops[mesh_conv::kStops];
std::atomic<int64_t> st_mc_stop_ops[64];
thread_local const uint8_t* mesh_rescan_at = nullptr;   // (the op the last scan ended at, with mesh draws after it: replay())
/// Right after a barrier of the game's (the pass ended, a full barrier recorded), or right after the op that ended the scan
/// before with indirect mesh draws after it (a counter's report, an event's wait, a dispatch, a copy: replay() once the op is
/// recorded): the indirect mesh draws from next up to the next barrier, the first op that may write buffer memory or the
/// stream's end, converted here at once (vk_draw.cpp mesh_conv_batch: after a full barrier, which leaves out itself after the
/// game's)
void mesh_conv_at_barrier(VkCommandBuffer cmd, const uint8_t* next, const uint8_t* end, bool after_op = false) {
    mesh_rescan_at = nullptr;
    if (!mesh_conv_batching()) return mesh_conv_batch(cmd, nullptr, 0);
    // (records no GPU write to buffer memory in stream order: state, binds, draws (one whose program writes memory drops the
    // batch as it is drawn: vk_draw.cpp), attachment clears (images), uniform updates (kept in the replay's list, written to
    // memory at the recording's end), counters' resets, render conditions (read on the CPU))
    static constexpr uint32_t pass_ops[] = {NOP_BEGIN, NOP_JUMP, NOP_CALL, NOP_UPDATE_UB, NOP_SET_RT, NOP_CLEAR_COLOR,
                                            NOP_CLEAR_DS, NOP_VIEWPORTS, NOP_SCISSOR, NOP_DEPTH_RANGE, NOP_BIND_PROGRAM,
                                            NOP_BIND_STATE, NOP_BIND_VATTR, NOP_BIND_VSTREAM, NOP_BIND_VBUF, NOP_BIND_UBO,
                                            NOP_BIND_SSBO, NOP_BIND_TEX, NOP_BIND_SAMP, NOP_STENCIL, NOP_POLY_OFFSET, NOP_DRAW,
                                            NOP_DRAW_IDX, NOP_SET_TEX_POOL, NOP_SET_SAMP_POOL, NOP_COPY_STRIDE, NOP_BLEND_COLOR,
                                            NOP_BIND_IMG, NOP_RESET_COUNTER, NOP_DEPTH_CLAMP, NOP_RENDER_ENABLE,
                                            NOP_RASTER_DISCARD, NOP_RENDER_COND, NOP_PRIMITIVE_RESTART, NOP_BIND_BUFS,
                                            NOP_DRAW_INDIRECT, NOP_PUSH_CONST};
    // (each op's kind in a table made from the list at compile time: the scan looks an op up, it does not walk the list)
    static_assert(NOP_COUNT_ <= mesh_conv::kOpTable, "mesh_conv.h kOpTable: an op past it would be Stop");
    static constexpr mesh_conv::Ops ops =
        mesh_conv::make_ops(NOP_END, NOP_BARRIER, NOP_DRAW_MESH, pass_ops, sizeof pass_ops / sizeof pass_ops[0]);
    thread_local std::vector<mesh_conv::Item> items;
    thread_local uint32_t cut_by = 0;   // (the op the scan before this one ended at, for the log)
    uint32_t stop_op = 0;
    const uint8_t* at = nullptr;
    // (at most 1 MB of slots and 256 draws a batch: a quarter of the ring, so the batch never comes round onto itself)
    const mesh_conv::Stop why = mesh_conv::scan(ops, next, end, items, 1ull << 20, 256, &stop_op, &at);
    st_mc_stops[why].fetch_add(1, std::memory_order_relaxed);
    if (after_op && !items.empty()) st_mc_stop_ops[cut_by & 63].fetch_add((int64_t)items.size(), std::memory_order_relaxed);
    // (an op that may write memory ended it: scanned again once the op is recorded, from the op after it. Each op of the
    // stretch is walked once, however many such ops it has: the occlusion pass's counter reports, ~200 a frame)
    if (why == mesh_conv::kStopOp) mesh_rescan_at = at, cut_by = stop_op;
    mesh_conv_batch(cmd, items.data(), items.size());
}

/// A draw or a dispatch: what a part is a count of (maybe_split)
bool op_is_work(uint32_t op) {
    return op == NOP_DRAW || op == NOP_DRAW_IDX || op == NOP_DRAW_INDIRECT || op == NOP_DRAW_MESH || op == NOP_DISPATCH ||
           op == NOP_DISPATCH_INDIRECT;
}

// A timestamp report writes two guest words. Check the mapped storage too: two virtual pools
// can name the same pages. A fragmented or unknown mapping stays on the ordinary clear path.
bool depth_copy_report_disjoint(uint64_t addr, const nvn_p_copy_tt& copy) {
    if (!copy.src || !copy.dst || !copy.src->L.size || !copy.dst->L.size) return false;
    auto mapped = [](uint64_t gpu, uint64_t size) -> uintptr_t {
        uint64_t available = 0;
        auto ptr = host->gpu_ptr(gpu, &available);
        return available >= size ? reinterpret_cast<uintptr_t>(ptr) : 0;
    };
    auto separate = [](uintptr_t a, uint64_t na, uintptr_t b, uint64_t nb) {
        return a < b ? b - a >= na : a - b >= nb;
    };
    constexpr uint64_t bytes = 2 * sizeof(uint64_t);
    const uintptr_t report = mapped(addr, bytes);
    const uintptr_t src = mapped(copy.src->gpu, copy.src->L.size);
    const uintptr_t dst = mapped(copy.dst->gpu, copy.dst->L.size);
    return report && src && dst && separate(report, bytes, src, copy.src->L.size) &&
           separate(report, bytes, dst, copy.dst->L.size) && separate(src, copy.src->L.size, dst, copy.dst->L.size);
}

void replay(VkCommandBuffer& cmd, NvnState& s, const uint8_t* p, const uint8_t* end = nullptr) {
    const bool trace_packets = pass_trace_on();
    Texture* cleared = nullptr;
    float clear_value = 0;
    // (RX_VK_LOG_RB_WRITERS, RX_VK_READBACK_EARLY: each op said, as the writer of what it writes)
    static const bool rb_writers = rb_writers_on(), early = readback_early_on(), note_ops = rb_writers || early;
    for (;;) {
        if (p == end) return;
        nvn_op_hdr h;
        std::memcpy(&h, p, sizeof h);
        if (note_ops) memory_writer_note(h.op, 0);
        bool split_after = false;   // (RX_VK_READBACK_EARLY: an update of uniforms the CPU reads back: its part ends after it)
        const uint8_t* a = p + 8;
        const uint32_t avail = h.size - 8;
        // (the part's end maybe_split asked for: here, right before a draw with a vertex stage that will be recorded, so that
        // the next part begins with it and not with a mesh draw or a dispatch)
        if (part_end_wanted && (h.op == NOP_DRAW || h.op == NOP_DRAW_IDX || h.op == NOP_DRAW_INDIRECT) &&
            s.render_enabled && !s.raster_discard)
            split_part(cmd);
        if (trace_packets) trace_depth_clear_packet(h.op, a, avail);
        switch (h.op) {
        case NOP_END: return;
        case NOP_BEGIN: case NOP_JUMP: case NOP_CALL: break;
        // ---- memory the stream writes: on the GPU timeline ----
        case NOP_SIGNAL_EVENT: {
            const auto e = payload<nvn_p_signal>(a, avail);
            {   // (in stream order)
                ev_cpu_apply();
                auto it = ev_values.find(e.gpu);
                uint32_t old = it != ev_values.end() ? it->second : 0;
                if (it == ev_values.end()) host->gpu_read(e.gpu, &old, 4);
                ev_values[e.gpu] = e.mode == NVN_EVENT_SIGNAL_MODE_ADD ? old + e.value : e.value;
            }
            uint32_t v = e.value;
            if (e.mode == NVN_EVENT_SIGNAL_MODE_ADD) {   // (the add on the CPU's view: events are few and in order)
                uint32_t old = 0;
                host->gpu_read(e.gpu, &old, 4);
                v = old + e.value;
            }
            end_pass(cmd, "event signal");
            GprofSpan gp(cmd, "event signal (barrier, write, barrier)");
            full_barrier(cmd);
            gpu_write(cmd, e.gpu, &v, 4);
            break;
        }
        case NOP_WAIT_EVENT: {   // (a wait for an event another queue's work signals: submit_job orders them)
            const auto w = payload<nvn_p_wait_event>(a, avail);
            ev_cpu_apply();
            const uint32_t have = event_value_in(ev_values, w.gpu);
            if (event_wait_met(have, w.mode, w.value)) {
                // (longshot) met by a signal of the CPU's that no capture follows: stopped here once, for resume_queue's
                if (suspend_ok && p != met_resumed_at && met_wait_capture_on() && cpu_signal_uncaptured(w.gpu)) {
                    st_met_stops++;
                    suspend_at = p;
                    return;
                }
                st_waits_met++;
            } else if (suspend_ok) {   // (on once that work is replayed)
                suspend_at = p;
                return;
            } else {
                st_waits_unmet++;
                static int logged;
                if (logged++ < 20 || st_waits_unmet % 5000 == 0)
                    logf("vk: a wait not met in replay order: event %#llx wants %s %u, has %u (submit %llu; %lld met, %lld "
                         "not)", (unsigned long long)w.gpu, w.mode == 0 ? "==" : ">=", w.value, have,
                         (unsigned long long)st_submits, (long long)st_waits_met, (long long)st_waits_unmet);
            }
            break;
        }
        case NOP_REPORT_COUNTER: {
            const auto c = payload<nvn_p_counter>(a, avail);
            // (longshot) the run of occlusion queries this report counts (RX_VK_OCC_QUERIES), closed
            const OccRun occ_run = occ_close(s, (uint32_t)c.type == NVN_COUNTER_TYPE_SAMPLES_PASSED);
            const bool flag_written = (c.type & NVN_COUNTER_WRITTEN) != 0;   // (a clear flag: nvn_native.c wrote it as it was recorded)
            const bool samples = (uint32_t)c.type == NVN_COUNTER_TYPE_SAMPLES_PASSED;
            // (timestamps on the guest tick clock in counter ticks (19.2 MHz, nvnDeviceGetTimestampInNanoseconds
            // converts), the replay's time; occlusion counts grow with every draw, as in the WebGPU renderer)
            // doc: docs/dev/reference/architecture.md#rules-from-mkw (rule 4: GPU timestamps drive dynamic resolution)
            // (the GPU's time as the game measures it: from the frame's present on, at RX_VK_TS_SCALE (default 0.1)
            // times the real clock. The reports are written as the replay records them, so the real clock gave the
            // replay's time and its lag, not the GPU's: MKW's dynamic resolution moved every frame (each new size a
            // new set of ~20 render targets, re-registered textures, 20-25 ms of replay a frame) and kept low. The
            // GPU takes 6-7 ms a frame at the resolution it then holds. RX_VK_TS_SCALE=1: the real clock;
            // RX_VK_TS_GLOBAL=1: scaled from the start, not from each present.)
            static const double ts_scale = getenv("RX_VK_TS_SCALE") ? atof(getenv("RX_VK_TS_SCALE")) : 0.1;
            static const bool ts_global = env_on("RX_VK_TS_GLOBAL");
            static uint64_t ts_base = 0;
            uint64_t ns = host->ticks() * 32;
            if (ts_scale != 1.0) {
                const uint64_t base = ts_global ? (ts_base ? ts_base : (ts_base = ns)) : std::min(ns, ts_anchor_ns ? ts_anchor_ns : ns);
                ns = base + (uint64_t)((double)(ns - base) * ts_scale);
            }
            // (longshot) Every samples-passed report reads 4096: everything visible. The count of 16 a draw since the reset went
            // up and down from frame to frame, and OoT's sun visibility followed it: for one frame in ~70 the sunlight left the
            // ground and the haze brightened over the whole picture (one-frame flicker, most of it at load-in; 30-52 per 2,700
            // frames in Kokiri, 4-6 with this, RX 7900 GRE). RX_VK_OCC_FIXED=<n>: that value; -1: the per-draw count as before
            static const int64_t occ_fixed = getenv("RX_VK_OCC_FIXED") ? atoll(getenv("RX_VK_OCC_FIXED")) : 4096;
            const uint64_t rec[2] = {samples ? (occ_fixed >= 0 ? (uint64_t)occ_fixed : s.samples_passed) : 0, ns * 12 / 625};
            if (dump_on())
                logf("vk: dump-op %03d REPORT_COUNTER type %d at %#llx: %llu", dump_seq(), (int)c.type,
                     (unsigned long long)c.addr, (unsigned long long)rec[0]);
            // (written now, in stream order, not on the GPU timeline: SetRenderEnableConditional reads these words at
            // replay time, and MKW gates clears with them as TOTK does (the CPU writes 1 to ask for a clear, the report
            // after the clear resets it). A GPU-timeline write left the 1 in place for the stream's later conditions on
            // the same words, depending on how far the GPU was: extra clears of the HDR target, black frames at random;
            // and its late write could undo the game's next request. No pass ends for it either.)
            if (flag_written) guest_cpu_write(c.addr + 8, rec + 1, 8);
            else guest_cpu_write(c.addr, rec, sizeof rec);
            memory_cpu_owned(host->gpu_guest(c.addr), sizeof rec);   // (vk_memory.cpp: kept out of the mirrors' write-backs)
            // (longshot) the GPU's count over it once its queries are done (occ_collect); a run whose count is not known is only
            // freed. The slot's report sequence moves on with every report
            occ_reported(occ_run, c.addr, rec[1], flag_written, rec_cur_value);
            break;
        }
        case NOP_RESET_COUNTER: {
            const auto c = payload<nvn_p_counter>(a, avail);
            if (c.type == NVN_COUNTER_TYPE_SAMPLES_PASSED) {
                s.samples_passed = 0;
                // (longshot) a run of occlusion queries begins (RX_VK_OCC_QUERIES); one still open had no report: its queries are freed
                occ_counter_reset(s, rec_cur_value);
            }
            break;
        }
        case NOP_COPY_B2B: {
            const auto c = payload<nvn_p_copy_bb>(a, avail);
            BufferRef src = gpu_buffer_bind(c.src, c.size, false), dst = gpu_buffer_bind(c.dst, c.size, true);
            {   // (RX_VK_WATCH_RANGE=<lo>:<hi>: buffer copies into it or out of it, 400 lines)
                static const std::pair<uint64_t, uint64_t> range = [] {
                    unsigned long long lo = 0, hi = 0;
                    if (const char* e = getenv("RX_VK_WATCH_RANGE")) sscanf(e, "%llx:%llx", &lo, &hi);
                    return std::pair<uint64_t, uint64_t>{lo, hi};
                }();
                static int logged;
                const bool hit = range.second && ((c.dst < range.second && c.dst + c.size > range.first) ||
                                                  (c.src < range.second && c.src + c.size > range.first));
                if (hit && logged++ < 400)
                    logf("vk: watch range buffer copy %#llx -> %#llx (%#llx bytes): source %s, destination %s", (unsigned long long)c.src,
                         (unsigned long long)c.dst, (unsigned long long)c.size, src ? "bound" : "NOT bound",
                         dst ? "bound" : "NOT bound (on the CPU)");
            }
            end_pass(cmd, "buffer copy");
            GprofSpan gp(cmd, "copy buffer->buffer (with its barriers)");
            gp.bytes = c.size;
            full_barrier(cmd);
            if (src && dst) {
                const VkBufferCopy r{src.offset, dst.offset, c.size};
                vkCmdCopyBuffer(cmd, src.buffer, dst.buffer, 1, &r);
                const uint64_t guest = host->gpu_guest(c.dst);
                if (guest) memory_stamp_gpu(guest, c.size);
            } else {   // (outside the pools: on the CPU)
                for (uint64_t done = 0; done < c.size;) {
                    uint8_t buf[65536];
                    const uint64_t k = std::min<uint64_t>(sizeof buf, c.size - done);
                    if (!host->gpu_read(c.src + done, buf, k) || !guest_cpu_write(c.dst + done, buf, k)) break;
                    done += k;
                }
            }
            full_barrier(cmd);
            break;
        }
        case NOP_CLEAR_BUF: {   // (longshot) nvnCommandBufferClearBuffer: on the GPU in order, as a fill (gpu_fill)
            const auto c0 = payload<nvn_p_clear_buf>(a, avail);
            // (the small ones are the counters the reports and conditions use, which happen here on the CPU in stream order. Left
            // to the GPU, a clear earlier in the stream reached guest memory after a later report and wiped it, as far as the GPU
            // was behind: lights and whole rooms switched off by their conditions for a frame. RX_VK_CLEAR_CPU_MAX=0: on the GPU)
            static const uint64_t cpu_max = getenv("RX_VK_CLEAR_CPU_MAX") ? (uint64_t)atoll(getenv("RX_VK_CLEAR_CPU_MAX")) : 256;
            const bool in_stream = c0.size && c0.size <= cpu_max;
            if (in_stream) {
                uint32_t words[64];
                for (auto& w : words) w = c0.value;
                guest_cpu_write(c0.dst, words, std::min<uint64_t>(c0.size, sizeof words));   // (longshot: not past words)
            }
            const auto c = c0;
            if (!c.size) break;
            end_pass(cmd, "buffer clear");
            GprofSpan gp(cmd, "clear buffer (with its barriers)");
            gp.bytes = c.size;
            full_barrier(cmd);
            const bool on_gpu = gpu_fill(cmd, c.dst, c.size, c.value, in_stream);
            full_barrier(cmd);
            {   // (EvilOOT: RX_VK_WATCH_RANGE, the clears into it too)
                unsigned long long lo = 0, hi = 0;
                static const char* e = getenv("RX_VK_WATCH_RANGE");
                if (e) sscanf(e, "%llx:%llx", &lo, &hi);
                static int logged;
                if (hi && c.dst < hi && c.dst + c.size > lo && logged++ < 400)
                    logf("vk: watch range buffer clear %#llx (%#llx bytes) to %#x: %s", (unsigned long long)c.dst,
                         (unsigned long long)c.size, (unsigned)c.value, on_gpu ? "on the GPU" : "NOT all on the GPU");
            }
            break;
        }
        case NOP_COPY_B2T: copy_b2t(cmd, s, payload<nvn_p_copy_bt>(a, avail), true); break;
        case NOP_COPY_T2B: copy_b2t(cmd, s, payload<nvn_p_copy_bt>(a, avail), false); break;
        case NOP_COPY_T2T: copy_t2t(cmd, s, payload<nvn_p_copy_tt>(a, avail), cleared, clear_value); cleared = nullptr; break;
        case NOP_CLEAR_TEX: clear_tex(cmd, s, payload<nvn_p_clear_tex>(a, avail)); break;
        case NOP_UPDATE_UB: {
            const auto u = payload<nvn_p_update_ub>(a, avail);
            if (dump_on() && u.size && u.size <= avail - sizeof u) {   // (in a dump: each update, its first words)
                float f[12] = {};
                std::memcpy(f, a + sizeof u, std::min<size_t>(sizeof f, u.size));
                logf("vk: dump-op %03d UPDATE_UB %#llx +%#x: %.5g %.5g %.5g %.5g | %.5g %.5g %.5g %.5g | %.5g %.5g %.5g %.5g", dump_seq(),
                     (unsigned long long)u.dst, (unsigned)u.size, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11]);
            }
            if (u.size && u.size <= avail - sizeof u) {
                UbUpdate up{rec_value_now, u.dst, std::vector<uint8_t>(a + sizeof u, a + sizeof u + u.size)};
                ub_updates.push_back(std::move(up));
                ub_list_epoch.fetch_add(1, std::memory_order_relaxed);
                ub_gen++;
                st_ub_updates++;
                if (early && memory_rb_waited_range(host->gpu_guest(u.dst), u.size)) split_after = true;
            }
            break;
        }
        // ---- state ----
        case NOP_SET_RT: s.rt = payload<nvn_p_set_rt>(a, avail), s.rt_serial++; break;
        case NOP_VIEWPORTS: {
            const auto v = payload<nvn_p_viewports>(a, avail);
            for (int i = 0; i < v.n && v.first + i < 16; i++) std::memcpy(s.viewport[v.first + i], v.v[i], 16);
            break;
        }
        case NOP_SCISSOR: {
            const auto c = payload<nvn_p_scissor>(a, avail);
            s.scissor[0] = c.x, s.scissor[1] = c.y, s.scissor[2] = c.w, s.scissor[3] = c.h;
            break;
        }
        case NOP_DEPTH_RANGE: {
            const auto d = payload<nvn_p_depth_range>(a, avail);
            s.depth_near = d.n, s.depth_far = d.f;
            break;
        }
        case NOP_BIND_PROGRAM: {
            const auto b = payload<nvn_p_bind_program>(a, avail);
            // (the same program bound again: its descriptors already stand)
            const bool same = b.prog && s.has_prog ? std::memcmp(&s.prog, b.prog, sizeof s.prog) == 0 : !b.prog && !s.has_prog;
            if (!same) s.bind_gen++;
            if (b.prog) s.prog = *b.prog;
            s.has_prog = b.prog != nullptr && b.prog->nstage > 0;
            break;
        }
        case NOP_BIND_STATE: {
            const auto b = payload<nvn_p_bind_state>(a, avail);
            switch (b.kind) {
            case 0: {
                nvn_blend_st st;
                std::memcpy(&st, b.bytes, sizeof st);
                s.blend[st.target & 7] = st;
                break;
            }
            case 1: std::memcpy(&s.color, b.bytes, sizeof s.color); break;
            case 2: std::memcpy(&s.chmask, b.bytes, sizeof s.chmask); break;
            case 3: std::memcpy(&s.ms, b.bytes, sizeof s.ms); break;
            case 4: std::memcpy(&s.poly, b.bytes, sizeof s.poly); break;
            case 5: std::memcpy(&s.ds, b.bytes, sizeof s.ds); break;
            default: break;
            }
            break;
        }
        case NOP_BIND_VATTR: {
            const auto v = payload<nvn_p_bind_vattr>(a, avail);
            std::memset(s.vattr, 0, sizeof s.vattr);
            for (int i = 0; i < v.n && i < 16; i++) s.vattr[i] = v.st[i];
            s.nvattr = std::min(v.n, 16);
            break;
        }
        case NOP_BIND_VSTREAM: {
            const auto v = payload<nvn_p_bind_vstream>(a, avail);
            for (int i = 0; i < v.n && i < 16; i++) s.vstream[i] = v.st[i];
            break;
        }
        case NOP_BIND_VBUF: {
            const auto v = payload<nvn_p_bind_vbuf>(a, avail);
            for (int i = 0; i < v.n && v.first + i < 16; i++) s.vbuf[v.first + i] = v.r[i];
            break;
        }
        case NOP_BIND_BUFS: {
            auto b = payload<nvn_p_bind_bufs>(a, avail);
            // (deferred binds: the ranges as the game's array held them at the submit, which the host copied into
            // the recording then (nvn_native.c resolve_deferred). Read again here, at the replay, the array was
            // often the next frame's already (MKW refills it as soon as the submit returns): the terrain patches
            // took other patches' placements (holes in the far terrain, the minimap's triangles).)
            {   // (EvilOOT, RX_VK_LOG_DEFERRED=1: compute's deferred binds, as at the submit and as the array holds them now)
                static const bool on = env_on("RX_VK_LOG_DEFERRED");
                static std::atomic<int> told{0};
                if (on && b.src && b.stage == 5 && told < 400) {
                    NVNbufferRange now[16]{};
                    const int n = std::clamp(b.n, 0, 16);
                    host->gpu_read(b.src, now, (uint64_t)n * sizeof(NVNbufferRange));
                    for (int i = 0; i < n && told < 400; i++, told++)
                        logf("vk: deferred %s stage 5 slot %d (array %#llx): submit %#llx+%#llx, now %#llx+%#llx%s", b.ssbo ? "ssbo" : "ubo",
                             b.first + i, (unsigned long long)b.src, (unsigned long long)b.r[i].address, (unsigned long long)b.r[i].size,
                             (unsigned long long)now[i].address, (unsigned long long)now[i].size,
                             now[i].address != b.r[i].address || now[i].size != b.r[i].size ? "  DIFFERS" : "");
                }
            }
            for (int i = 0; i < b.n && i < 16; i++) {
                const int index = b.first + i;
                if (b.stage < 0 || b.stage >= kNvnStages || index < 0 || index >= 16) continue;
                {
                    static const bool on = env_on("RX_VK_LOG_MESH_BINDS");
                    static std::atomic<int> told{0};
                    if (on && b.stage >= 6 && b.r[i].size != 0x100 && told++ < 300)
                        logf("vk: bind (many) %s stage %d index %d addr %#llx size %#llx", b.ssbo ? "ssbo" : "ubo", b.stage, index,
                             (unsigned long long)b.r[i].address, (unsigned long long)b.r[i].size);
                }
                nvn_p_bind_buf& u = (b.ssbo ? s.ssbo : s.ubo)[b.stage][index];
                if (!buf_unchanged(u.stage, u.index, u.addr, u.size, b.stage, index, b.r[i].address, b.r[i].size)) {
                    s.bind_gen++;
                    if (b.ssbo) s.ssbo_gen[b.stage] = s.bind_gen;
                }
                u.stage = b.stage, u.index = index, u.addr = b.r[i].address, u.size = b.r[i].size;
            }
            break;
        }
        case NOP_BIND_UBO: case NOP_BIND_SSBO: {
            const auto b = payload<nvn_p_bind_buf>(a, avail);
            if (b.stage >= kNvnStages) log_stage_bind(h.op, b.stage);
            {   // (longshot) RX_VK_LOG_MESH_BINDS=1: what the game binds to the task and mesh stages, 80 lines
                static const bool on = env_on("RX_VK_LOG_MESH_BINDS");
                static std::atomic<int> told{0};
                if (on && b.stage >= 6 && b.size != 0x100 && told++ < 300)
                    logf("vk: bind %s stage %d index %d addr %#llx size %#llx", h.op == NOP_BIND_UBO ? "ubo" : "ssbo", b.stage, b.index,
                         (unsigned long long)b.addr, (unsigned long long)b.size);
            }
            if (b.stage >= 0 && b.stage < kNvnStages && b.index >= 0 && b.index < 16) {
                nvn_p_bind_buf& u = (h.op == NOP_BIND_UBO ? s.ubo : s.ssbo)[b.stage][b.index];
                if (!buf_unchanged(u.stage, u.index, u.addr, u.size, b.stage, b.index, b.addr, b.size)) {
                    s.bind_gen++;
                    if (h.op == NOP_BIND_SSBO) s.ssbo_gen[b.stage] = s.bind_gen;
                }
                u = b;
            }
            break;
        }
        case NOP_BIND_TEX: case NOP_BIND_SAMP: case NOP_BIND_IMG: {
            nvn_p_bind_handles b;
            std::memset(&b, 0, sizeof b);
            std::memcpy(&b, a, std::min<size_t>(sizeof b, h.size - 8));
            // (deferred: the handles as of the submit, as the ranges above)
            if (b.stage >= kNvnStages) log_stage_bind(h.op, b.stage);
            if (b.stage < 0 || b.stage >= kNvnStages || b.first < 0) break;
            uint64_t* to = h.op == NOP_BIND_TEX ? s.tex[b.stage] : h.op == NOP_BIND_SAMP ? s.samp[b.stage] : s.img[b.stage];
            const int lim = h.op == NOP_BIND_IMG ? 32 : 128;
            int n = 0;
            while (n < b.n && b.first + n < lim) n++;
            if (!handles_unchanged(to + b.first, b.h, n)) {   // (the same handles: the draw keeps its descriptors)
                s.bind_gen++;
                s.tex_gen[b.stage] = s.bind_gen;
            }
            for (int k = 0; k < n; k++) to[b.first + k] = b.h[k];
            break;
        }
        case NOP_STENCIL: {
            const auto st = payload<nvn_p_stencil>(a, avail);
            for (int f = 0; f < 2; f++) {
                if (!(st.face & (1 << f))) continue;
                if (st.which == 0) s.stencil_value_mask[f] = (uint32_t)st.value_mask;
                else if (st.which == 1) s.stencil_mask[f] = (uint32_t)st.mask;
                else s.stencil_ref[f] = (uint32_t)st.ref;
            }
            break;
        }
        case NOP_POLY_OFFSET: {
            const auto o = payload<nvn_p_poly_offset>(a, avail);
            s.poly_offset[0] = o.factor, s.poly_offset[1] = o.units, s.poly_offset[2] = o.clamp;
            break;
        }
        case NOP_BLEND_COLOR: std::memcpy(s.blend_color, payload<nvn_p_blend_color>(a, avail).rgba, 16); break;
        case NOP_PRIMITIVE_RESTART: {
            const auto r = payload<nvn_p_restart>(a, avail);
            s.restart = r.enable != 0, s.restart_index = (uint32_t)r.index;
            break;
        }
        case NOP_DEPTH_CLAMP: s.depth_clamp = payload<nvn_p_flag>(a, avail).value != 0; break;
        case NOP_RASTER_DISCARD: s.raster_discard = payload<nvn_p_flag>(a, avail).value != 0; break;
        case NOP_RENDER_ENABLE:
            s.render_enabled = payload<nvn_p_flag>(a, avail).value != 0;
            if (dump_on()) logf("vk: dump-op %03d RENDER_ENABLE %d", dump_seq(), (int)s.render_enabled);
            break;
        case NOP_RENDER_COND: {   // (read on the CPU for now; VK_EXT_conditional_rendering later)
            const auto r = payload<nvn_p_render_cond>(a, avail);
            uint64_t x = r.x, y = r.y;
            if (!r.snapped) {
                host->gpu_read(r.addr, &x, 8);
                host->gpu_read(r.addr + 16, &y, 8);
            }
            s.render_enabled = r.mode == 0 ? x == y : x != y;
            {   // (RX_VK_COND_ALWAYS=1 (test): every condition holds. The frame log's dark frames dump the last ones)
                static const bool always = env_on("RX_VK_COND_ALWAYS");
                const uint32_t at = cond_ring_at++ % (1 << 15);
                cond_ring[at] = {presents_now, r.addr, x, y, (int)r.mode, s.render_enabled};
                if (always) s.render_enabled = true;
            }
            if (dump_on())
                logf("vk: dump-op %03d RENDER_COND mode %d at %#llx: %llu vs %llu: %s", dump_seq(), r.mode,
                     (unsigned long long)r.addr, (unsigned long long)x, (unsigned long long)y,
                     s.render_enabled ? "on" : "off");
            break;
        }
        case NOP_SET_TEX_POOL: case NOP_SET_SAMP_POOL:
            s.bind_gen++;
            for (int k = 0; k < kNvnStages; k++) s.tex_gen[k] = s.bind_gen;
            break;
        case NOP_COPY_STRIDE: s.copy_stride = payload<nvn_p_copy_stride>(a, avail).stride; break;
        case NOP_PUSH_CONST: {   // (longshot) compute push constants, kept in the state (vk_bank.cpp serves them from c0)
            const auto u = payload<nvn_p_push_const>(a, avail);
            if (u.size && u.size <= avail - sizeof u && (uint64_t)u.offset + u.size <= sizeof s.cs_push) {
                const uint8_t* src = a + sizeof u;
                const bool same = (uint64_t)u.offset + u.size <= s.cs_push_n && std::memcmp(s.cs_push + u.offset, src, u.size) == 0;
                std::memcpy(s.cs_push + u.offset, src, u.size);
                s.cs_push_n = std::max<uint32_t>(s.cs_push_n, u.offset + u.size);
                if (!same) s.bind_gen++;
            }
            break;
        }
        case NOP_BARRIER: {
            const int32_t bits = payload<int32_t>(a, avail);
            {
                std::lock_guard lk(st_barrier_flags_mu);
                st_barrier_flags[bits]++;
            }
            end_pass(cmd, "guest barrier", static_cast<uint32_t>(bits));
            full_barrier(cmd, "barrier (the game's nvnCommandBufferBarrier)");
            st_barriers++;
            mesh_conv_at_barrier(cmd, p + h.size, end);   // (RX_VK_MESH_CONV=2)
            break;
        }
        // ---- work ----
        case NOP_CLEAR_COLOR:
            if (s.render_enabled) {
                const auto c = payload<nvn_p_clear_color>(a, avail);
                clear_color_target(cmd, s, c.index, c.rgba, (uint32_t)c.mask);
            }
            break;
        case NOP_CLEAR_DS:
            if (s.render_enabled) {
                const auto c = payload<nvn_p_clear_ds>(a, avail);
                // Inspect complete packets in this replay only. The regular loop still executes the report.
                const uint8_t* next = p + h.size;
                nvn_op_hdr hdr{};
                const bool has_header = !end || end - next >= (ptrdiff_t)sizeof hdr;
                if (has_header) std::memcpy(&hdr, next, sizeof hdr);
                const char* reason = nullptr;
                if (avail < sizeof c) reason = "short clear packet";
                else if (!c.depth_mask || c.stencil_mask) reason = "clear mask";
                else if (!has_header) reason = "replay boundary";
                const int first_op = has_header ? (int)hdr.op : -1;
                const bool report = !reason && hdr.op == NOP_REPORT_COUNTER;
                nvn_p_counter counter{};
                if (report) {
                    if (hdr.size != sizeof hdr + sizeof counter ||
                        (end && end - next < (ptrdiff_t)(hdr.size + sizeof hdr))) {
                        reason = "short report or replay boundary";
                    } else {
                        counter = payload<nvn_p_counter>(next + sizeof hdr, sizeof counter);
                        next += hdr.size;
                        std::memcpy(&hdr, next, sizeof hdr);
                        if (counter.type != NVN_COUNTER_TYPE_TIMESTAMP && counter.type != NVN_COUNTER_TYPE_TIMESTAMP_TOP)
                            reason = "counter is not an ordinary timestamp";
                        if (pass_trace_on())
                            trace_line("depth clear: counter type %#llx at %#llx, following guest op %u",
                                       (unsigned long long)counter.type, (unsigned long long)counter.addr, hdr.op);
                    }
                }
                // The clear ran with rendering enabled. Setting it to one again changes no state.
                // Only inspect this packet here; replay still executes both it and the timestamp in order.
                if (!reason && report && hdr.op == NOP_RENDER_ENABLE) {
                    if (hdr.size != sizeof hdr + sizeof(nvn_p_flag) ||
                        (end && end - next < (ptrdiff_t)(hdr.size + sizeof hdr))) {
                        reason = "short render-enable packet or replay boundary";
                    } else if (payload<nvn_p_flag>(next + sizeof hdr, sizeof(nvn_p_flag)).value != 1) {
                        reason = "render-enable value is not one";
                    } else {
                        next += hdr.size;
                        std::memcpy(&hdr, next, sizeof hdr);
                    }
                }
                if (!reason && hdr.op != NOP_COPY_T2T) reason = "next eligible guest command is not a texture copy";
                else if (!reason && (hdr.size < sizeof hdr + sizeof(nvn_p_copy_tt) ||
                         (end && end - next < (ptrdiff_t)(sizeof hdr + sizeof(nvn_p_copy_tt)))))
                    reason = "short copy packet";
                nvn_p_copy_tt copy{};
                if (!reason) {
                    copy = payload<nvn_p_copy_tt>(next + sizeof hdr, hdr.size - sizeof hdr);
                    if (report && !depth_copy_report_disjoint(counter.addr, copy))
                        reason = "timestamp or texture storage overlaps or is not contiguous";
                }
                if (pass_trace_on())
                    trace_line("depth clear candidate %#llx: next guest op %d, depth mask %d stencil mask %#x, %s",
                               (unsigned long long)(s.rt.depth ? s.rt.depth->gpu : 0), first_op,
                               c.depth_mask, (unsigned)c.stencil_mask, reason ? reason : report ? "checking copy after timestamp" : "checking adjacent copy");
                if (!reason) {
                    cleared = depth_clear_before_copy(s, copy);
                    clear_value = c.depth;
                }
                if (!cleared) clear_depth_stencil(cmd, s, c.depth, c.depth_mask != 0, (uint32_t)c.stencil, (uint32_t)c.stencil_mask);
            }
            break;
        case NOP_DRAW: {
            const auto d = payload<nvn_p_draw>(a, avail);
            DrawArgs da;
            occ_take(s, da);   // (longshot: RX_VK_OCC_QUERIES)
            if (s.render_enabled && !s.raster_discard) {
                da.prim = d.prim, da.first = d.first, da.count = d.count;
                da.base_instance = d.base_instance, da.instances = d.instances;
                draw(cmd, s, da);
            }
            s.samples_passed += 16;
            break;
        }
        case NOP_DRAW_IDX: {
            const auto d = payload<nvn_p_draw_idx>(a, avail);
            DrawArgs da;
            occ_take(s, da);   // (longshot: RX_VK_OCC_QUERIES)
            if (s.render_enabled && !s.raster_discard) {
                da.prim = d.prim, da.indexed = true, da.itype = d.itype, da.count = d.count;
                da.base_vertex = d.base_vertex, da.base_instance = d.base_instance, da.instances = d.instances;
                da.ibuf = d.ibuf;
                draw(cmd, s, da);
            }
            s.samples_passed += 16;
            break;
        }
        case NOP_DRAW_INDIRECT: {
            const auto d = payload<nvn_p_draw_indirect>(a, avail);
            DrawArgs da;
            occ_take(s, da);   // (longshot: RX_VK_OCC_QUERIES)
            if (s.render_enabled && !s.raster_discard) {
                da.prim = d.prim, da.indexed = d.indexed != 0, da.itype = d.itype;
                da.indirect = d.indirect, da.count_addr = d.count_addr, da.max = d.max, da.stride = d.stride;
                da.ibuf = d.ibuf;
                draw(cmd, s, da);
            }
            s.samples_passed += 16;
            break;
        }
        case NOP_DRAW_MESH: {
            const auto d = payload<nvn_p_draw_mesh>(a, avail);
            static int logged_mesh;
            if (logged_mesh++ < 8)
                logf("vk: mesh draw kind %d: %d %d %d, indirect %#llx count %#llx max %d stride %d", d.kind, d.x, d.y,
                     d.z, (unsigned long long)d.indirect, (unsigned long long)d.count_addr, d.max, d.stride);
            DrawArgs da;
            occ_take(s, da);   // (longshot: RX_VK_OCC_QUERIES)
            if (s.render_enabled && !s.raster_discard) {
                da.mesh = true;
                // (DrawMeshTasks(first, count): the count of task workgroups, as Eden draws NVN2's mesh draws; a
                // first task other than 0 has no Vulkan counterpart)
                da.groups[0] = d.y, da.groups[1] = 1, da.groups[2] = 1;
                if (d.kind == 0) da.first_task = d.x;   // (longshot: the task programs add it, vk_draw.cpp's mesh_args)
                da.indirect = d.indirect, da.count_addr = d.count_addr, da.max = d.max, da.stride = d.stride;
                da.op = p;   // (RX_VK_MESH_CONV=2: mesh_conv_at_barrier's scan named it by this)
                draw(cmd, s, da);
            }
            s.samples_passed += 16;
            break;
        }
        case NOP_DISPATCH: {
            const auto d = payload<nvn_p_dispatch>(a, avail);
            if (s.render_enabled) dispatch(cmd, s, (uint32_t)d.x, (uint32_t)d.y, (uint32_t)d.z, 0);
            break;
        }
        case NOP_DISPATCH_INDIRECT: {
            const auto d = payload<nvn_p_dispatch_indirect>(a, avail);
            if (s.render_enabled) dispatch(cmd, s, 1, 1, 1, d.addr);
            break;
        }
        default: break;
        }
        if (op_is_work(h.op)) {
            if (rb_writers) memory_count_work();
            maybe_split(cmd);
        }
        // (RX_VK_READBACK_EARLY: this op wrote a page the CPU reads back and has waited for: its part ends here, so the read
        // waits for the GPU's work up to its writer, not for the draws a part runs on to. At least early_gap draws into
        // a part: writers in a row (four dispatches within ten draws) end one part, not four, each end a submit and barriers)
        if (early) {
            static const int64_t early_gap = std::max<int64_t>(0, env_int("RX_VK_READBACK_EARLY_GAP", 16));
            if (split_after || memory_split_wanted()) early_split_pending = true;
            if (cleared && early_split_pending && part_work >= early_gap) {
                if (pass_trace_on()) trace_line("depth clear kept: early recording split");
                clear_depth_stencil(cmd, s, clear_value, true, 0, 0);
                cleared = nullptr;
            }
            if (early_split_pending && part_work >= early_gap) early_split(cmd);
        }
        // (RX_VK_MESH_CONV=2: the op that ended the last scan, with indirect mesh draws after it before the next barrier, is
        // recorded, and the part's end if any: those draws converted here at once, after a full barrier of their own)
        if (p == mesh_rescan_at) mesh_conv_at_barrier(cmd, p + h.size, end, true);
        p += h.size;
    }
}

// ---- chunks: a long submission goes to the GPU in parts (maybe_split: where the game has ended its render pass, from
// RX_VK_PART_MIN draws), so the GPU starts on its first part while the rest is recorded. The game waits for its fences within
// the frame: with the whole submission recorded before the GPU saw any of it, its wait was the replay's time plus
// the GPU's. Only the last part signals the submission's timeline value; the parts' slots free with it.
// 240 a part: each part end costs a command buffer, a queue submit and its barriers, and the first part still reaches
// the GPU within a millisecond or two of the recording's start. Kokiri Forest 63.0 -> 67.9 fps against 120 a part, the
// Market 68.6 -> 70.8, Hyrule Field the same (RTX 3090, 2560x1440, six interleaved sessions each); 480 was no better,
// 30 and sending the submission whole were worse (whole: 46 fps, the replay's time and the GPU's added up). ----
Recording* rec_cur = nullptr;
uint64_t rec_cur_value = 0;
int rec_parts = 0;
int64_t part_work = 0;
// (RX_VK_READBACK_EARLY) the value the recording being made writes back with (rec_cur_value while rec_cur is set, 0 between
// recordings), for the other threads: a read whose writer's submission is still being recorded has not seen all of its
// writers yet (early_recorded_wait). Set where a recording opens (rec_open) and cleared where it is finished (rec_close)
std::atomic<uint64_t> rec_open_tag{0};
bool rec_tags_on() { return readback_early_on() || rb_writers_on(); }   // (their only readers: unset, nothing is kept)
void rec_open(uint64_t tag) {
    if (rec_tags_on()) rec_open_tag.store(tag, std::memory_order_release);
}
void rec_close();   // (below, with the recordings' timeline it wakes the waiters of)
// (a slot is used again once its own submit is done (recording_wait): a submission's parts may outnumber the slots,
// which a wait on the timeline value it signals at its end kept them from)
constexpr int kMaxParts = 32;

/// (longshot) Whether a part ends only right before a plain draw (one with a vertex stage), so that no cut leaves a part
/// beginning with a mesh draw or a dispatch (a submission's own first recording begins as the game began it). On an Intel Arc B580 (Windows, driver 32.0.101.9154, native mesh shaders) a command buffer
/// whose first draws are mesh draws drew every plain draw after them as exploded triangles: white shards over the scene
/// in Kokiri Forest while moving, in about 2% of the frames (whenever the 120th draw or dispatch fell among the depth
/// pre-pass's mesh draws or on the culling dispatch before them). A cut forced after that pass's first mesh draw showed it
/// in 333 of 335 frames; with only the new part's leading mesh draws left out, 0 of 324. Not a race (the GPU waited for
/// after each part: 317 of 319), not a state the replay keeps (all of a draw's state sent again before every draw: 319 of
/// 328), nothing the validation layer names, and the uniform data the draws were given was the submission's own. With
/// this the forced cut shows none (0 of 324), and over six 30 s walks alternated with it off, 0 of 1,101 frames against
/// 40 of 1,121, at the same frame rate (a run's median 52 and 48 fps with it off, 55 and 50 with it on).
/// A run of mesh draws or dispatches with no plain draw in it is no longer cut: the part after it is shorter by what the
/// run went over. A plain draw that draw() then leaves out (no targets, a program not ready) still begins the part.
/// On by default on Intel, where it was measured; RX_VK_SPLIT_BEFORE_DRAW=1 turns it on elsewhere, =0 off (a part ends
/// where maybe_split says, whatever follows)
bool part_ends_before_draw() {
    return dev.policy.split_before_draw;   // (device_policy.h: Intel, then the switch)
}

/// (longshot) Where a submission's part ends: where the game has already ended the render pass, once the part is worth a
/// cut. A part ends to give the GPU what is recorded so far while the rest is replayed: no parts ran 30-42% slower in every
/// area (RTX 3090, Ryzen 9 7900X, Linux), a part every 960 draws up to 30% slower. But a part that ends inside a render pass
/// ends the pass early: its targets are written out and read in again by the next part, on top of the part's two full
/// barriers, its submit and the next recording's start. A count alone cuts wherever it falls (a part every 120 draws was 5-7%
/// slower moving than every 240, every 30 35-45%), and cutting whenever the GPU ran out of work fed itself (small parts
/// done at once, the GPU out again: 18% slower). The game itself ends its passes ~150 times a frame (at its barriers,
/// dispatches and copies), and a cut there breaks no pass. So a part ends at the first such place once it is kPartMin (120)
/// draws or dispatches long, and wherever it is at kPartMax (512), for the rare run with none. In the Market this was faster
/// than a part every 240 in all of 9 rounds paired (median standing +4%, walking +15%, turning +2.5%; walking's 99th
/// percentile 26.8 ms against 36.7, frames over 50 ms 20 against 44), and no part ended inside a pass but at the ceiling.
/// RX_VK_PART_MIN, RX_VK_PART_MAX, RX_VK_PART_FIRST
}   // namespace
bool draw_pass_open();   // (vk_draw.cpp)
namespace {
struct {
    int64_t at_pass_end = 0, ceiling = 0;
} part_st;
void maybe_split(VkCommandBuffer& cmd) {
    static const int64_t kPartMin = std::max<int64_t>(1, env_int("RX_VK_PART_MIN", 120)),
                         kPartMax = std::max<int64_t>(kPartMin, env_int("RX_VK_PART_MAX", 512));
    // (a submission's first part is shorter: the GPU has nothing of this submission until it ends, and on an RX 9070 XT it sat
    // idle ~4.9 ms a frame before a frame's first recording with a draw, against 3 ms of work. Ended at the first pass end
    // past RX_VK_PART_FIRST draws, so it costs no pass; 0: as long as any other. An RX 580 was even with it, 19 Nov)
    static const int64_t kPartFirst = std::clamp<int64_t>(env_int("RX_VK_PART_FIRST", 32), 0, kPartMin);
    const int64_t need = rec_parts == 0 && kPartFirst ? kPartFirst : kPartMin;
    if (++part_work < need || !rec_cur || rec_parts + 1 >= kMaxParts || dump_on()) return;
    const bool ceiling = part_work >= kPartMax;
    if (!ceiling && draw_pass_open()) return;   // (the next place the game ends its pass)
    (ceiling ? part_st.ceiling : part_st.at_pass_end)++;
    if ((part_st.ceiling + part_st.at_pass_end) % 20000 == 0)
        logf("vk: a submission's parts ended: %lld where the game had ended its render pass, %lld at %lld draws",
             (long long)part_st.at_pass_end, (long long)part_st.ceiling, (long long)kPartMax);
    if (part_ends_before_draw()) {   // (at the next plain draw: replay(). The submission's end comes first when none follows)
        part_end_wanted = true;
        return;
    }
    split_part(cmd);
}

std::atomic<int64_t> st_early_splits{0};   // (RX_VK_READBACK_EARLY: parts ended after a readback's writer)
bool early_end_asked = false;   // (early_split asked for the part's end: counted in st_early_splits once it is made)

/// The recording's part ends here and the submission goes on in a new one (maybe_split says when)
void split_part(VkCommandBuffer& cmd) {
#ifdef TRACY_ENABLE
    HT_SCOPE("split_part");
#endif
    part_end_wanted = false;
    // (asked for in another command buffer of the submission: what maybe_split refuses is refused here too, and it asks
    // again at the next draw it would cut after)
    if (!rec_cur || rec_parts + 1 >= kMaxParts || dump_on()) return;
    end_pass(cmd, early_end_asked ? "readback split" : "recording split");
    // (a part's end does not write the stream's uniform updates recorded so far into guest memory, nor start a new window
    // of them: the submission's last part writes them all, as an unsplit submission does (finish_recording). A uniform binding is not what changes: one overlapping an update gets an overlay copy built
    // from the whole list (ub_overlay), the same bytes either way. What changes is everything else that reads guest memory
    // after a part's end: a later part's storage, vertex, index or texture reads of an updated range, and, on a GPU whose
    // buffer pools have no VRAM mirror, the game's own CPU reads, since the GPU then writes the memory the game sees partway
    // through its frame. Written at each part's end, on Intel without mirrors: white blocks and flashes at Kokiri, and
    // a whole-screen white flash every 8 frames on the Arc 140V (https://github.com/NotRexed/Hookshot/issues/999).
    // Measured on an Intel iGPU only (no mirrors), so the default there; with mirrors (Arc discrete) the write goes to the
    // mirror, and it was not tested there: left as before. RX_VK_SPLIT_UBFLUSH=1: written at each part's end, as before,
    // =0: at the last part's on any GPU)
    // (device_policy.h split_ub_flush: not on Intel without mirrors, the mirrors as the run has them; then the switch)
    const bool part_flush = dev.policy.split_ub_flush;
    if (part_flush) flush_ub_updates(cmd);
    memory_end_submission(cmd, rec_cur->part);
    full_barrier(cmd, "barrier (recording end)");
    occ_copy_part(cmd);
    submit_recording(*rec_cur, false);
    if (part_flush) ub_first_of_recording = ub_updates.size();
    Recording& r = begin_recording(rec_cur_value);
    memory_begin_submission();   // (not memory_gpu_writes_begin: the submission's GPU writes so far stay noted, vk_memory.cpp)
    full_barrier(r.cmd, "barrier (recording start)");
    rec_cur = &r;
    cmd = r.cmd;
    part_work = 0;
    rec_parts++;
    if (early_end_asked) st_early_splits++;
    early_split_pending = early_end_asked = false;   // (the writer's part has ended, by its count or after it)
    if (readback_early_on()) (void)memory_split_wanted();
}

/// (RX_VK_READBACK_EARLY) The part ends after a writer of a range a read has waited for (replay(), at least
/// RX_VK_READBACK_EARLY_GAP draws into the part), so that the read waits for the GPU's work up to its writer. Where a part
/// ends only right before a plain draw (part_ends_before_draw: Intel by default, RX_VK_SPLIT_BEFORE_DRAW), this end waits
/// for that draw too, as maybe_split's does: the writer's part runs on over the mesh draws and dispatches after it, so a
/// read waits for a little more than its writer, never less, and no part begins with a mesh draw or a dispatch. With no
/// plain draw left in the submission its end comes first, and the read waits for the whole submission, as without the switch
void early_split(VkCommandBuffer& cmd) {
#ifdef TRACY_ENABLE
    HT_SCOPE("early_split");
#endif
    early_end_asked = true;
    if (part_ends_before_draw()) {   // (at the next plain draw: replay())
        part_end_wanted = true;
        return;
    }
    split_part(cmd);
}

/// The replay cannot go on: logged, the host's queued log written out (as hos_fatal does: a device lost's report is many
/// lines, and the queue went with the process), written to stderr and the process ended (exit code 3)
[[noreturn]] void replay_fatal(const char* why) {
    // The host's crash/freeze records read this prefix (host/hos_report_prefixes.h); keep it stable.
    logf("vk: FATAL: %s", why);
    if (host && host->log_flush) host->log_flush();
    std::fprintf(stderr, "vk: FATAL: %s\n", why);
    std::fflush(stderr);
    std::_Exit(3);
}
/// A command buffer of the submission from h (its start, or where it stopped at a wait), recorded on this thread.
/// may_stop: up to a wait not met, which it returns (null: the command buffer done)
const uint8_t* replay_handle(VkCommandBuffer& cmd, const uint8_t* h, bool may_stop = false) {
    NvnState& state = *cur_state;
    suspend_at = nullptr;
    // (longshot) the occlusion queries this command buffer may take (RX_VK_OCC_QUERIES), numbered from the counter all the
    // queues share; the counter goes on from where the replay here left this state, however this returns
    occ_begin_buffer(state, rec_cur_value, occ_open_oldest(&state));
    struct OccEnd {
        const NvnState& s;
        ~OccEnd() { occ_end_buffer(s); }
    } occ_end{state};
    suspend_ok = may_stop;
    replay(cmd, state, h);
    suspend_ok = false;
    return suspend_at;
}

// ---- the rx_* functions ----

// ---- a benchmark (RX_VK_BENCH=<present>[,<times>], default 50 times): the frame before that present replayed again and
// again, the game held (its next present waits) and the recordings kept (the replayed value the host frees them by
// is held at the frame's start); the replay's CPU time each time, logged. The same frame every time: what a change to
// the replay costs or saves, without the title scenes' differences. ----
std::vector<std::vector<uint64_t>> frame_submits;   // (this frame's submits' handles)
uint64_t frame_first_value = 0;
std::atomic<uint64_t> replayed_hold{0};             // (0: none)

void bench_replay(const std::vector<uint64_t>& handles) {
    const uint64_t value = pushed.load();   // (reached already: no slot waits on it)
    rec_value_now = value;
    ub_first_of_recording = ub_updates.size();
    Recording& first = begin_recording(value);
    memory_begin_submission();
    memory_gpu_writes_begin();   // (RX_VK_UB_SNAPSHOT's guard: a submission's start, not a part's: split_part)
    full_barrier(first.cmd);
    rec_cur = &first, rec_cur_value = value, rec_parts = 0, part_work = 0, part_end_wanted = false;
    rec_open(value);
    VkCommandBuffer cmd = first.cmd;
    for (uint64_t h : handles)
        if (h) {
            occ_begin_buffer(*cur_state, value, occ_open_oldest(cur_state));   // (longshot: as replay_handle)
            replay(cmd, *cur_state, reinterpret_cast<const uint8_t*>(h));
            occ_end_buffer(*cur_state);
        }
    Recording& r = *rec_cur;
    rec_cur = nullptr;
    end_pass(r.cmd, "benchmark recording end");
    flush_ub_updates(r.cmd);
    memory_end_submission(r.cmd, r.part);
    full_barrier(r.cmd);
    submit_recording(r, false);
    rec_close();
}

void bench_frame(int64_t presents) {
    static int64_t at = -2;
    static int times = 50;
    if (at == -2) {
        at = -1;
        if (const char* e = getenv("RX_VK_BENCH")) {
            at = atoll(e);
            if (const char* c = strchr(e, ',')) times = std::max(1, atoi(c + 1));
        }
    }
    if (at < 0 || presents + 1 != at || frame_submits.empty()) return;
    replayed_hold = std::max<uint64_t>(1, frame_first_value - 1);
    int64_t draws0 = 0, sk = 0, di = 0, pl = 0;
    draw_stats(&draws0, &sk, &di, &pl);
    std::vector<double> ms;
    int64_t prof0[PROF_COUNT];
    prof_full++;   // (every phase timed, not one draw in eight: the A/B differences are small)
    std::memcpy(prof0, prof_ns, sizeof prof0);
    const size_t ub_before = ub_updates.size();
    queue_flush();
    for (int i = 0; i < times; i++) {
        vkDeviceWaitIdle(dev.device);
        ub_updates.resize(ub_before);   // (the last time's uniform updates are written: as if pruned)
        const auto t0 = std::chrono::steady_clock::now();
        for (const auto& h : frame_submits) bench_replay(h);
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    vkDeviceWaitIdle(dev.device);
    prof_full--;
    int64_t draws1 = 0;
    draw_stats(&draws1, &sk, &di, &pl);
    replayed_hold = 0;
    std::vector<double> sorted = ms;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0;
    for (double v : ms) sum += v;
    logf("vk: bench: the frame before present %lld (%zu submits, %lld draws) replayed %d times: median %.2f ms, min %.2f, "
         "mean %.2f, max %.2f", (long long)at, frame_submits.size(), (long long)((draws1 - draws0) / times), times,
         sorted[sorted.size() / 2], sorted.front(), sum / times, sorted.back());
    static const char* names[PROF_COUNT] = {"submit", "present", "draw", "descriptors", "texture lookups", "uploads",
                                            "pipelines", "programs", "commands", "dispatch", "memory sync",
                                            "queue submit", "slot wait"};
    char line[600];
    int n = snprintf(line, sizeof line, "vk: bench: a replay's time (ms):");
    for (int i = 2; i < PROF_COUNT; i++)
        n += snprintf(line + n, sizeof line - n, " %s %.2f%s", names[i], (prof_ns[i] - prof0[i]) / (double)times / 1e6,
                      i + 1 < PROF_COUNT ? "," : "");
    logf("%s", line);
}

/// A slow submit (over 15 ms): what took its time, logged (60 of them). The stalls in it are exact (PROF, PROF_EXACT); the
/// per-draw phases' cheap part is sampled, one draw in eight (PROF_HOT): an estimate, exact with RX_VK_PROF_ALL=1
struct SlowWatch {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    int64_t prof0[PROF_COUNT];
    int64_t ms0[4];   // (longshot: memory sync's parts and the lock wait, at the start)
    const char* what;
    explicit SlowWatch(const char* w) : what(w) {
        std::memcpy(prof0, prof_ns, sizeof prof0);
        for (int i = 0; i < 3; i++) ms0[i] = memsync_part_ns[i].load(std::memory_order_relaxed);
        ms0[3] = memsync_lock_wait_ns.load(std::memory_order_relaxed);
    }
    ~SlowWatch() {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        static int logged;
        if (ms < 15.0 || logged >= 80 || presents_now < 600) return;   // (after the first load)
        logged++;
        static const char* names[PROF_COUNT] = {"submit", "present", "draw", "descriptors", "texture lookups",
                                                "uploads", "pipelines", "programs", "commands", "dispatch",
                                                "memory sync", "queue submit", "slot wait"};
        char line[600];
        int k = snprintf(line, sizeof line, "vk: slow %s: %.1f ms:", what, ms);
        for (int i = 2; i < PROF_COUNT; i++) {
            const double d = (prof_ns[i] - prof0[i]) / 1e6;
            if (d >= 1.0) k += snprintf(line + k, sizeof line - k, " %s %.1f", names[i], d);
        }
        {   // (longshot: which part of "memory sync" it was: the pools' refresh [and its lock waits], the capture, the written pages collected.
            // Counted across threads, so a part another thread spent meanwhile shows too)
            const double a = (memsync_part_ns[0].load(std::memory_order_relaxed) - ms0[0]) / 1e6,
                         b = (memsync_part_ns[1].load(std::memory_order_relaxed) - ms0[1]) / 1e6,
                         c = (memsync_part_ns[2].load(std::memory_order_relaxed) - ms0[2]) / 1e6,
                         w = (memsync_lock_wait_ns.load(std::memory_order_relaxed) - ms0[3]) / 1e6;
            if (a + b + c >= 1.0 || w >= 1.0)
                k += snprintf(line + k, sizeof line - k, " [memory sync: maps %.1f (lock waits %.1f), capture %.1f, collect %.1f]", a, w, b, c);
        }
        logf("%s", line);
    }
};

/// The memory work before a submission's recording: the pools mapped, its capture taken, the other writes collected
/// (replay_mu held). In the order of the submits: one behind its queue's stopped one (submit_job) has it done as it comes.
void submit_memory_work(uint64_t value) {
#ifdef TRACY_ENABLE
    HT_SCOPE("submit_memory_work");
#endif
    PROF(PROF_MEMSYNC);
    const auto m0 = std::chrono::steady_clock::now();
    memory_sync();
    const auto m1a = std::chrono::steady_clock::now();
    memsync_part_ns[0] += std::chrono::duration_cast<std::chrono::nanoseconds>(m1a - m0).count();
    // (the capture takes only the mirrored pools' pages, the collection the rest: skipping it whenever a capture was
    // taken, which with the replay thread is at every submit, left every other CPU write uncollected. Textures never
    // saw them: MK World's font cache kept its first glyphs (the menus lost letters), CPU-streamed terrain went stale)
    const auto m1 = std::chrono::steady_clock::now();
    memory_take_capture(value, completed());
    const auto m2 = std::chrono::steady_clock::now();
    memory_collect_written();
    memory_check_submit(value);   // (RX_VK_MEM_CHECK: nothing unless it is set)
    const auto m3 = std::chrono::steady_clock::now();
    memsync_part_ns[1] += std::chrono::duration_cast<std::chrono::nanoseconds>(m2 - m1).count();
    memsync_part_ns[2] += std::chrono::duration_cast<std::chrono::nanoseconds>(m3 - m2).count();
    {   // (every 600 submits: what the memory work before a submit's recording cost)
        static double a, b, c;
        static int64_t k;
        a += std::chrono::duration<double, std::milli>(m1 - m0).count();
        b += std::chrono::duration<double, std::milli>(m2 - m1).count();
        c += std::chrono::duration<double, std::milli>(m3 - m2).count();
        if (++k % 600 == 0) {
            logf("vk: a submit's memory work (ms): maps %.2f, capture taken %.2f, writes collected %.2f", a / 600, b / 600, c / 600);
            a = b = c = 0;
        }
    }
}

// ---- the game's queues ordered by their events ----
// MK World's two worker queues pass stages back and forth with events: a command buffer waits (nvnCommandBufferWaitEvent)
// for the other queue's to reach a stage, several times a frame, and the two are submitted one after the other.
// Replayed one after the other on the one Vulkan queue, the first went past waits for work the second had not done
// yet: what that work writes was read before it was written (55% of the waits). Now a submission is replayed as it
// comes as far as its waits allow, and what it recorded goes to the GPU; at a wait not met it stops, and its queue's
// later submits wait behind it (their memory work done in turn). Once another queue's work has signalled the event
// it goes on from there, as on the console, where it would have waited on the GPU. A present, a released texture or
// program, a wait from another thread for the replay, or RX_VK_HOLD_MS (default 3) with one stopped, and the stopped
// ones go on as their waits allow, the oldest past its wait whenever all of them wait. The timeline is signalled as
// far as no older submission is unfinished. (Holding a whole submission until the other queue's came, as this did
// first, had the registrations made meanwhile reach its draws before it: the menus' scenes in rectangles of fog.)
struct LaterSubmit {
    std::vector<uint64_t> handles;
    uint64_t value = 0;
};
struct QueueRun {   // (a queue stopped at a wait)
    bool waiting = false;
    std::vector<uint64_t> handles;   // (the submission it stopped in)
    int cb = 0;                      // (its command buffer, and the wait there)
    const uint8_t* p = nullptr;
    uint64_t value = 0;
    std::deque<LaterSubmit> later;   // (its queue's submits since)
    uint64_t ahead_upto = 0;         // (longshot: its submissions up to this value walked by occ_visible_ahead already)
    bool blocked() const { return waiting || !later.empty(); }
    uint64_t oldest() const { return waiting ? value : later.front().value; }
};
std::unordered_map<uint64_t, QueueRun> qruns;
std::atomic<int> blocked_n{0};                      // (queues stopped, for the other threads)
std::chrono::steady_clock::time_point blocked_since;
uint64_t done_max = 0, signaled = 0;                // (the newest value recorded whole; the timeline's last signal)
int64_t st_stops, st_resumed, st_forced_waits, st_forced[4], st_resume_captures;   // (made to go by: presents, releases, waits, time)
double st_stopped_max_ms;
int hold_ms() {
    // (50: with the replay caught up, the worker queue's submission came up to 16 ms before the one that lets it on;
    // at 3 ms it went past its waits ~80 times a race. The game never waits for it before that submit)
    static const int v = getenv("RX_VK_HOLD_MS") ? std::max(0, atoi(getenv("RX_VK_HOLD_MS"))) : 50;
    return v;
}
const uint8_t* op_next(const uint8_t* p) {
    nvn_op_hdr h;
    std::memcpy(&h, p, sizeof h);
    return p + h.size;
}
/// (longshot) Work the replay holds back (its queue stopped at an event wait): handles[cb] from op p (null: its start) and the
/// command buffers after it. Its samples-passed reports read "visible" until it is replayed: the slot may hold an older count,
/// and the game may read it before the replay gets there (occ_slot_visible)
void occ_visible_ahead(const uint64_t* handles, int n, int cb, const uint8_t* p) {
    if (!occ_pool) return;
    const auto t0 = std::chrono::steady_clock::now();
    for (; cb < n; cb++, p = nullptr) {
        if (!handles[cb]) continue;
        for (const uint8_t* o = p ? p : reinterpret_cast<const uint8_t*>(handles[cb]);; o = op_next(o)) {
            nvn_op_hdr h;
            std::memcpy(&h, o, sizeof h);
            if (h.op == NOP_END) break;
            if (h.op != NOP_REPORT_COUNTER) continue;
            const auto c = payload<nvn_p_counter>(o + 8, h.size - 8);
            if (c.type == NVN_COUNTER_TYPE_SAMPLES_PASSED) occ_slot_visible(c.addr);   // (not a flag: NVN_COUNTER_WRITTEN)
        }
    }
    occ_st.walks++;
    occ_st.walk_ns += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
}
nvn_p_wait_event wait_at(const uint8_t* p) {   // (p: a wait op)
    nvn_op_hdr h;
    std::memcpy(&h, p, sizeof h);
    return payload<nvn_p_wait_event>(p + 8, h.size - 8);
}
bool wait_met_at(const uint8_t* p) {
    const nvn_p_wait_event w = wait_at(p);
    ev_cpu_apply();
    return event_wait_met(event_value_in(ev_values, w.gpu), w.mode, w.value);
}
uint64_t oldest_unfinished() {
    uint64_t m = UINT64_MAX;
    for (const auto& [queue, q] : qruns)
        if (q.blocked()) m = std::min(m, q.oldest());
    return m;
}
// ---- Releases while queues are stopped: kept until the stopped work has gone on, instead of making it
// go at once (drain 1). Made to go early, a queue's work runs before the work it waits for, and a pass reads what is not
// there yet: the ground fog thinner for a frame, the light dimmed. The stopped work runs as before, at
// its wait, a present or hold_ms; the release follows it. ----
void do_texture_released(const nvn_tex* t);
void do_program_released(const nvn_prog* p);
std::mutex held_mu;   // (its own: sync_order_mu is held while whole submissions replay, and every release waited for them)
std::vector<nvn_tex> held_tex_releases;    // (held_mu)
std::vector<nvn_prog> held_prog_releases;  // (held_mu)
std::atomic<int64_t> st_held_releases{0};
/// Whether a stopped queue waits on an event whose value is guest memory's (never set by the stream or nvnEventSignal)
bool waits_on_memory_event() {
    for (const auto& [queue, q] : qruns)
        if (q.waiting && !ev_values.count(wait_at(q.p).gpu)) return true;
    return false;
}
void update_blocked() {
    int n = 0;
    for (const auto& [queue, q] : qruns) n += q.blocked();
    ev_wait_in_memory.store(n && waits_on_memory_event(), std::memory_order_release);
    if (n && !blocked_n.load()) blocked_since = std::chrono::steady_clock::now();
    blocked_n = n;
    if (!n) {   // (the stopped work has gone on: its releases now)
        std::vector<nvn_tex> ts;
        std::vector<nvn_prog> ps;
        {
            std::lock_guard hl(held_mu);
            ts.swap(held_tex_releases);
            ps.swap(held_prog_releases);
        }
        for (const nvn_tex& t : ts) do_texture_released(&t);
        for (const nvn_prog& p : ps) do_program_released(&p);
    }
}
void log_order_stats() {
    logf("vk: queues ordered by their events: %lld submissions stopped at a wait, %lld went on once it was reached, %lld "
         "waits gone past; made to go by presents %lld, releases %lld, waits for the replay %lld, %d ms %lld (stopped "
         "%.2f ms at most); waits met %lld, not met %lld; captures at a CPU event's wait %lld, %lld of them at a wait met "
         "on arrival", (long long)st_stops, (long long)st_resumed,
         (long long)st_forced_waits, (long long)st_forced[0], (long long)st_forced[1], (long long)st_forced[2], hold_ms(),
         (long long)st_forced[3], st_stopped_max_ms, (long long)st_waits_met, (long long)st_waits_unmet,
         (long long)st_resume_captures, (long long)st_met_stops);
}

// ---- a burst: the pieces replayed one after the other with no memory work between them (a submit's job: its own
// piece and those of the stopped queues it lets on, then theirs again: MK World's two queues pass five or six stages
// back and forth a frame) go into one recording. Each piece had its own, and its memory sync, write-backs and submit:
// ~8 a frame. The burst's recording is tagged with the newest value among them (what is used in it is kept until
// that one is done; the timeline is signalled at its end as far as allowed, as for any). ----
bool burst_open = false;
uint64_t burst_value = 0;
void burst_begin(uint64_t value) {
    burst_open = !dump_on();
    burst_value = value;
    for (const auto& [queue, q] : qruns) {
        if (q.waiting) burst_value = std::max(burst_value, q.value);
        if (!q.later.empty()) burst_value = std::max(burst_value, q.later.back().value);
    }
}
void finish_recording(uint64_t value);
void rb_span_close();   // (RX_VK_LOG_RB_WRITERS: below, with the readback's)
void burst_end() {
    if (!burst_open) return;
    burst_open = false;
    {
        std::lock_guard lk(replay_mu);
        if (rec_cur) rb_span_close(), finish_recording(burst_value);
    }
    // (the looks taken in this burst count only now: what they let on is recorded, its write-backs are in the page table
    // and its recording is submitted. Said at the look's end, a readback that came before this point found the counts
    // equal, took no look, and read the page table before the burst's recording had noted its pages: found by mooki000)
    if (ev_looked_any) ev_cpu_settled.store(ev_looked, std::memory_order_release), ev_looked_any = false;
}

void rb_span_begin(uint64_t value, uint64_t part);   // (RX_VK_LOG_RB_WRITERS: below, with the readback's)
void rb_span_end(uint64_t value, uint64_t part);
/// The recording being made: ended and submitted, the timeline signalled as far as no older submission is unfinished
void finish_recording(uint64_t value) {
#ifdef TRACY_ENABLE
    HT_SCOPE("finish_recording");
#endif
    (void)value;
    Recording& r = *rec_cur;
    rec_cur = nullptr;
    end_pass(r.cmd, "recording end");
    flush_ub_updates(r.cmd);
    memory_end_submission(r.cmd, r.part);
    full_barrier(r.cmd, "barrier (recording end)");
    occ_copy_part(r.cmd);
    const uint64_t oldest = oldest_unfinished();
    const uint64_t at = oldest == UINT64_MAX ? done_max : std::min(done_max, oldest - 1);
    const bool signal = at > signaled;
    if (signal) signaled = at;
    submit_recording(r, signal, at);
    rec_close();   // (its last part is on the recordings' timeline now: an early read's writers are all known)
}

// ---- (longshot) the synchronous replay on the core kept for the replay ----
// With RX_VK_ASYNC off (the default) a submission is replayed on the thread that submits it, or on whichever thread goes on
// with a queue stopped at an event wait, and the core the host keeps free of the game's threads for the replay thread
// (host->reserved_cpus: hos_reserved_cpus, the last core of the fastest kind) had nothing on it. Now a thread runs there
// while it replays and takes a submission's capture, and goes back to its own processors after: every thread that replays,
// none by name; with RX_VK_ASYNC=1 the render thread is there already. Ryzen 9 3900X, Kokiri Forest: see the PR.
// (Only preferring the core, as the thread's ideal processor with every processor allowed, gained
// nothing there: the pin is what helps)
bool async_on();
// (On Linux pthread_create inherits the calling thread's affinity, so win32_posix.c's thread_self and thread_entry
// apply process_affinity when a new thread registers via CreateThread or GetCurrentThread in SetThreadDescription,
// keeping helper threads made on first use inside a replay off the reserved core, as Windows does)
uint64_t replay_pin_mask() {
    static const uint64_t m = [] {
        const uint64_t r = host->reserved_cpus ? host->reserved_cpus() : 0;
        if (r) logf("vk: replays run on logical processors %#llx while they replay", (unsigned long long)r);
        return r;
    }();
    return m;
}
thread_local int replay_pin_depth = 0;
thread_local DWORD_PTR replay_pin_prev = 0;
struct ReplayPin {
    bool counted = false;
    ReplayPin() {
        if (async_on()) return;
        counted = true;
        if (replay_pin_depth++) return;   // (the outermost one: a submit's replay inside its capture's)
        if (const uint64_t m = replay_pin_mask()) {
            replay_pin_prev = SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)m);
            static std::atomic<bool> told{false};   // (refused: a mask outside the process's. Said once, not every replay)
            if (!replay_pin_prev && !told.exchange(true, std::memory_order_relaxed))
                logf("vk: a replay was NOT kept to logical processors %#llx (error %lu): replays run where their threads may",
                     (unsigned long long)m, (unsigned long)GetLastError());
        }
    }
    ~ReplayPin() {
        if (!counted || --replay_pin_depth) return;
        if (replay_pin_prev) SetThreadAffinityMask(GetCurrentThread(), replay_pin_prev), replay_pin_prev = 0;
    }
    ReplayPin(const ReplayPin&) = delete;
    ReplayPin& operator=(const ReplayPin&) = delete;
};

/// A submission's command buffers from command buffer cb (op p; null: its start) on, into recordings of its own,
/// submitted: true when it finished; false when it stopped at a wait not met (may_stop), its place kept in qruns.
/// memory_work: its memory work first (else done as it came)
bool run_submit(const uint64_t* handles, int n, uint64_t value, uint64_t queue, bool memory_work, int cb = 0,
                const uint8_t* p = nullptr, bool may_stop = false) {
    std::lock_guard lk(replay_mu);
    ReplayPin pin;
    SlowWatch slow("submit");
    PROF(PROF_SUBMIT);
    if (readback_early_on()) (void)memory_split_wanted(), early_split_pending = early_end_asked = false;   // (the last op's: its part has ended since)
    if (memory_work) {
        if (frame_submits.empty()) frame_first_value = value;
        frame_submits.emplace_back(handles, handles + std::max(n, 0));
        submit_memory_work(value);
    }
    if (burst_open && rec_cur) {   // (the burst's recording goes on: what the piece before did, done first)
        end_pass(rec_cur->cmd, "next submit in burst");
        full_barrier(rec_cur->cmd, "barrier (a burst's next submit start)");
        use_queue_state(queue);
    } else {
        const uint64_t tag = burst_open ? std::max(burst_value, value) : value;
        use_queue_state(queue);
        ub_prune(completed());
        rec_value_now = tag;
        ub_first_of_recording = ub_updates.size();
        Recording& first = begin_recording(tag);
        memory_begin_submission();
        memory_gpu_writes_begin();   // (RX_VK_UB_SNAPSHOT's guard: a submission's start, not a part's: split_part)
        full_barrier(first.cmd, "barrier (recording start)");
        rec_cur = &first, rec_cur_value = tag, rec_parts = 0, part_work = 0, part_end_wanted = false;
        rec_open(tag);
        if (rb_writers_on()) rb_span_begin(tag, first.part);
        texture_ahead_run();   // (longshot, RX_VK_TEX_AHEAD: into this recording's uploads, while the frames are light)
        temporal_prewarm(first.cmd);   // (longshot, RX_VK_DLSS_PREWARM: DLSS's feature, in a light frame before the first 3D one)
    }
    VkCommandBuffer cmd = rec_cur->cmd;
    const uint8_t* stop = nullptr;
    for (; cb < n; cb++, p = nullptr) {
        if (!handles[cb]) continue;
        stop = replay_handle(cmd, p ? p : reinterpret_cast<const uint8_t*>(handles[cb]), may_stop);
        if (stop) break;
    }
    if (stop) {
        QueueRun& q = qruns[queue];
        q.waiting = true;
        q.handles.assign(handles, handles + n);
        q.cb = cb, q.p = stop, q.value = value;
        if (value > q.ahead_upto) {   // (longshot: its reports "visible" until they are replayed; once a submission: a later
            occ_visible_ahead(handles, n, cb, stop);   // stop in it holds back less of it)
            q.ahead_upto = value;
        }
        if (st_stops++ < 12) {
            const nvn_p_wait_event w = wait_at(stop);
            logf("vk: submission %llu (queue %#llx) stopped at command buffer %d's wait for event %#llx %s %u (has %u)",
                 (unsigned long long)value, (unsigned long long)queue, cb, (unsigned long long)w.gpu,
                 w.mode == 0 ? "==" : ">=", w.value, event_value_in(ev_values, w.gpu));
        }
        if (st_stops % 6000 == 0) log_order_stats();
    } else {
        done_max = std::max(done_max, value);
        rec_open_values.push_back(value);
    }
    if (!burst_open) rb_span_close(), finish_recording(value);   // (a burst's: at its end)
    if (stop) return false;
    if (pushed.load() < value) pushed = value;
    st_submits++;
    if (!burst_open && dump_pending()) wait_value(value), dump_write();
    return true;
}
void do_submit(const uint64_t* handles, int n) {   // (RX_VK_ASYNC=0: on the submitting thread, in its order)
    run_submit(handles, n, ++q_values, 0, true);
}

/// A stopped queue on (forced: past the wait it stopped at), then its later submits, as far as their waits allow
void resume_queue(uint64_t queue, bool forced) {
    QueueRun& q = qruns[queue];
    for (;;) {
        if (q.waiting) {
            // (longshot) a wait for an event the CPU signals, met: the game wrote what this work reads after it submitted it,
            // up to the signal (OoT's presentation queue waits for its frame's data this way, 1 frame in 12 here). The
            // console's GPU reads memory after its wait; the capture taken at the submit had the frame before's: the fog and
            // exposure of the frame before, for a frame. A capture now, for what follows the wait.
            bool cpu_event = false;
            if (!forced && !async_on()) {   // (the render thread: taken at the signal, event_signal_queued)
                std::lock_guard ek(ev_cpu_mu);
                cpu_event = cpu_events.count(wait_at(q.p).gpu) != 0;
            }
            if (cpu_event) {
                std::lock_guard lk(replay_mu);
                if (rec_cur) rb_span_close(), finish_recording(burst_open ? burst_value : q.value);   // (its mirrors synced again after)
                memory_capture_now(q.value, completed());
                st_resume_captures++;
            }
            q.waiting = false;
            std::vector<uint64_t> handles = std::move(q.handles);
            q.handles.clear();
            const uint8_t* from = forced ? op_next(q.p) : q.p;
            (forced ? st_forced_waits : st_resumed)++;
            met_resumed_at = forced ? nullptr : from;   // (its wait: met, and its capture taken or not wanted)
            const bool went = run_submit(handles.data(), (int)handles.size(), q.value, queue, false, q.cb, from, true);
            met_resumed_at = nullptr;
            if (!went) return;
            continue;
        }
        if (q.later.empty()) return;
        LaterSubmit s = std::move(q.later.front());
        q.later.pop_front();
        if (!run_submit(s.handles.data(), (int)s.handles.size(), s.value, queue, false, 0, nullptr, true)) return;
    }
}
/// (longshot, the render thread) Whether a queue is stopped at a wait for an event the CPU signals (nvnEventSignal has set
/// it before) that is not met: its frame's data is not all written yet (present held, replay_thread_main)
bool cpu_wait_unmet() {
    for (const auto& [queue, q] : qruns) {
        if (!q.waiting) continue;
        {
            std::lock_guard ek(ev_cpu_mu);
            if (!cpu_events.count(wait_at(q.p).gpu)) continue;
        }
        if (!wait_met_at(q.p)) return true;
    }
    return false;
}
/// The stopped queues whose events are reached go on, until none can
void schedule() {
    // (the CPU's signals made before this look: counted before the first wait is tested, so one made meanwhile, which a
    // test may or may not have seen, is still to be looked at; said once every queue those let on is recorded)
    const uint64_t signals = ev_cpu_signals.load(std::memory_order_acquire);
    for (bool moved = true; moved;) {
        moved = false;
        for (auto& [queue, q] : qruns)
            if (q.waiting && wait_met_at(q.p)) {
                resume_queue(queue, false);
                moved = true;
            }
    }
    // (in a burst the recording of what this look let on is finished at the burst's end, and the count is said there)
    if (burst_open) ev_looked = signals, ev_looked_any = true;
    else ev_cpu_settled.store(signals, std::memory_order_release);
}
/// Every stopped queue on now (why: an index of st_forced): in turn as their waits allow; when all of them wait, the
/// oldest past its wait
void drain(int why) {
    if (oldest_unfinished() == UINT64_MAX) return;
    st_forced[why]++;
    st_stopped_max_ms = std::max(st_stopped_max_ms,
                                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - blocked_since).count());
    burst_begin(0);
    for (;;) {
        schedule();
        uint64_t best = UINT64_MAX, bq = 0;
        for (const auto& [queue, q] : qruns)
            if (q.blocked() && q.oldest() < best) best = q.oldest(), bq = queue;
        if (best == UINT64_MAX) break;
        resume_queue(bq, qruns[bq].waiting);
    }
    burst_end();
    update_blocked();
}

// ---- (longshot) the same on the submitting thread, the default. do_submit replayed each queue's submission as it came and went
// past every wait: 2 of 3 were not met. A lit pass waiting for another queue's work that had not been submitted yet read what
// that work makes before it was made, and for a frame lit surfaces came out black (a race with the game's threads: 3-9% of
// frames with frame generation off, less the less they overlapped). The stopped work goes on from whichever thread's submit or
// signal lets it, or a present, a wait for it, or hold_ms (sync_watch: nothing else may come while the game polls). ----
bool async_on();
/// RX_VK_PER_QUEUE_FENCES=0: a fence waits for every queue's work up to it, stopped work forced on, as before
bool per_queue_fences_off() {
    static const bool v = getenv("RX_VK_PER_QUEUE_FENCES") && atoi(getenv("RX_VK_PER_QUEUE_FENCES")) == 0;
    return v;
}
std::mutex sync_order_mu;   // (before replay_mu, always; all of the order's state: qruns, burst_*)
bool sync_waits_on() {
    return !async_on() && !dump_on();
}
void sync_drain(int why) {
#ifdef TRACY_ENABLE
    HT_SCOPE("sync_drain");
#endif
    if (!blocked_n.load()) return;
    std::lock_guard lk(sync_order_mu);
    drain(why);
    update_blocked();
}
/// (longshot) The stopped queues whose waits are met go on; none is forced past its wait
void sync_settle() {
#ifdef TRACY_ENABLE
    HT_SCOPE("sync_settle");
#endif
    if (!blocked_n.load()) return;
    std::lock_guard lk(sync_order_mu);
    burst_begin(0);
    schedule();
    burst_end();
    update_blocked();
}
/// (longshot) The same for a CPU read of GPU results (invalidate): only when the CPU has signalled an event since the
/// stopped queues were last looked at, which is when one of them can be stopped at a wait that is met with nobody to let it
/// on (the hole the readback's settle closes: its work not recorded, its write-back unknown). Every other met wait was met by
/// a signal in the stream, replayed under sync_order_mu by a thread that looks at the queues before it lets the mutex go.
/// Without this test the readback took the mutex whenever anything was stopped, and waited for whoever held it: the
/// presenting thread, for the whole replay of its submission and for its present (Hyrule Field: a job thread blocked there
/// at 24 % of all instants, 3.9 ms a frame, for a look that let nothing on). An event the stream and nvnEventSignal never
/// set is the exception: its value is read from guest memory, where a plain store can meet a wait; while a queue is stopped
/// at such a wait the readback looks every time, as before (ev_wait_in_memory). True when it looked.
bool sync_settle_met() {
    if (!blocked_n.load()) return false;
    if (!ev_wait_in_memory.load(std::memory_order_acquire) &&
        ev_cpu_settled.load(std::memory_order_acquire) == ev_cpu_signals.load(std::memory_order_acquire))
        return false;
    sync_settle();
    return true;
}
std::atomic<int> gpu_waiters{0};   // (the game's threads in a wait for the GPU on recorded work of their own queue: GpuWaiter, at wait())
// (how often the stopped queues were held on past hold_ms for such a wait, and how often they were then made to go at the bound)
std::atomic<int64_t> st_hold_gpu{0}, st_hold_gpu_forced{0};
void sync_watch() {
    SetThreadDescription(GetCurrentThread(), L"mkw_vk order");
    for (;;) {
        Sleep(1);
        if (!blocked_n.load()) continue;
        std::unique_lock lk(sync_order_mu, std::try_to_lock);
        if (!lk) continue;
        burst_begin(0);   // (a signal from the CPU the thread that made it could not act on)
        schedule();
        burst_end();
        update_blocked();
        // (longshot) held on while a thread of the game's waits for the GPU on older work, and for hold_ms after: hold_gpu.h
        static const double most_ms = getenv("RX_VK_HOLD_GPU_MS") ? std::max(atof(getenv("RX_VK_HOLD_GPU_MS")), (double)hold_ms()) : 1000.0;
        static bool holding = false;
        static std::chrono::steady_clock::time_point gpu_wait_seen{};
        const auto now = std::chrono::steady_clock::now();
        const bool gpu_wait = gpu_waiters.load(std::memory_order_acquire) > 0;
        if (gpu_wait) gpu_wait_seen = now;
        if (!blocked_n.load()) {
            holding = false;
            continue;
        }
        const auto ms_since = [&](std::chrono::steady_clock::time_point t) { return std::chrono::duration<double, std::milli>(now - t).count(); };
        switch (hold_gpu::choose(gpu_wait, ms_since(blocked_since), ms_since(std::max(blocked_since, gpu_wait_seen)), hold_ms(),
                                 most_ms)) {
        case hold_gpu::Choice::Wait: break;
        case hold_gpu::Choice::Hold:
            if (!holding) {
                holding = true;
                const int64_t n = ++st_hold_gpu;
                if (n <= 8 || n % 50 == 0)
                    logf("vk: stopped queues held past %d ms while a thread waits for the GPU: %lld times so far (%lld of them "
                         "made to go at %.0f ms)", hold_ms(), (long long)n, (long long)st_hold_gpu_forced.load(), most_ms);
            }
            break;
        case hold_gpu::Choice::Force:
            if (holding) st_hold_gpu_forced++;
            holding = false;
            drain(3);
            update_blocked();
            break;
        }
    }
}

// doc: docs/dev/reference/architecture.md#rules-from-mkw (rule 1: cross-queue event waits are honoured)
/// A submit on the replay thread: replayed as far as its waits allow, or (its queue stopped) after its queue's
void submit_job(std::vector<uint64_t>& handles, uint64_t value, uint64_t queue) {
#ifdef TRACY_ENABLE
    HT_SCOPE("submit_job");
#endif
    const bool may_stop = !dump_on();
    if (!may_stop) drain(2);
    auto it = qruns.find(queue);
    if (it != qruns.end() && it->second.blocked()) {   // (its memory work now, in the order of the submits)
        std::lock_guard lk(replay_mu);
        if (frame_submits.empty()) frame_first_value = value;
        frame_submits.emplace_back(handles);
        submit_memory_work(value);
        occ_visible_ahead(handles.data(), (int)handles.size(), 0, nullptr);   // (longshot: as run_submit's stop)
        it->second.ahead_upto = std::max(it->second.ahead_upto, value);
        it->second.later.push_back({std::move(handles), value});
        return;
    }
    if (may_stop) burst_begin(value);   // (its piece and what it lets on: one recording)
    run_submit(handles.data(), (int)handles.size(), value, queue, true, 0, nullptr, may_stop);
    schedule();   // (what it signalled may let the stopped ones on)
    burst_end();
}
uint64_t get_replayed() {
    queue_flush_poll();
    const uint64_t hold = replayed_hold.load();
    return hold ? std::min(hold, completed()) : completed();
}
/// (longshot) Waits for the recording submission value f (or the recording of value f) was finished in, on the recordings'
/// timeline: true when that recording is known (the result in *ok), false when it is not (f not finished yet)
static void wait_rec_seq(uint64_t seq, uint64_t timeout_ns, int* ok);
bool wait_recorded(uint64_t f, uint64_t timeout_ns, int* ok) {
#ifdef TRACY_ENABLE
    HT_WAIT(ht_w, "wait_recorded");
#endif
    if (!rec_timeline) return false;
    uint64_t seq = 0;
    {
        std::lock_guard mk(rec_map_mu);
        auto it = value_rec.find(f);
        if (it == value_rec.end()) return false;
        seq = it->second;
    }
    wait_rec_seq(seq, timeout_ns, ok);
    return true;
}
/// (longshot) The same for one part of a submission (Recording::part, as memory_end_submission noted it with the pages that
/// part writes back): true when that part is submitted (the result in *ok), false when it is not known (older than the map
/// keeps, or never submitted on the recordings' timeline). A part's pages are noted at its end, just before its submit, on
/// the thread that replays it: a read from another thread in between waits for that submit here (250 ms at most, then
/// false: the caller waits as it did before parts were known)
bool wait_recorded_part(uint64_t part, uint64_t timeout_ns, int* ok) {
#ifdef TRACY_ENABLE
    HT_WAIT(ht_w, "wait_recorded_part");
#endif
    if (!rec_timeline || per_queue_fences_off() || !part) return false;
    uint64_t seq = 0;
    {
        std::unique_lock mk(rec_map_mu);
        const auto known = [&] {
            auto it = part_rec.find(part);
            if (it == part_rec.end()) return false;
            seq = it->second;
            return true;
        };
        if (!known()) {
            if (!part_rec.empty() && part_rec.rbegin()->first > part) return false;   // (a newer part is in: this one will not come)
            if (!rec_map_cv.wait_for(mk, std::chrono::milliseconds(250), known)) return false;
        }
    }
    wait_rec_seq(seq, timeout_ns, ok);
    return true;
}
/// (RX_VK_READBACK_EARLY) The recording being made is finished: no more of its writers will be noted (rec_open_tag)
void rb_release_close(uint64_t value);   // (RX_VK_LOG_RB_WRITERS: below, with the readback's)
void rec_close() {
    if (!rec_tags_on()) return;
    if (rb_writers_on()) rb_release_close(rec_open_tag.load(std::memory_order_relaxed));
    rec_open_tag.store(0, std::memory_order_release);
    { std::lock_guard mk(rec_map_mu); }   // (a waiter in early_recorded_wait looks under rec_map_mu: it is asleep or sees 0)
    rec_map_cv.notify_all();
}
/// (RX_VK_READBACK_EARLY) Waits, at most 250 ms, until the submission whose value a read's pages were last written back with
/// (whole) is no longer being recorded; true when it is not. A read let go at its writer's part while the rest of that
/// submission was still being recorded took the newest writer recorded so far, not the range's last: on the Steam Deck in
/// Kokiri Forest, the Market, Kakariko and Hyrule Field, 269 of 745 reads of the culling camera block (+0x3980) were let go at
/// dispatch 7984161db3e053b5's part (or another of the first writers), and dispatch fd958faab99f20a2, recorded after the read,
/// wrote ~1170 of its bytes later in the same submission. Once the recording is finished every write of that submission is
/// noted (memory_in_place_writer_part), the uniform updates of its last part's flush too. *ms: the time waited.
bool early_recorded_wait(uint64_t whole, double* ms) {
#ifdef TRACY_ENABLE
    HT_WAIT(ht_w, "early_recorded_wait");
#endif
    *ms = 0;
    if (rec_open_tag.load(std::memory_order_acquire) != whole) return true;
    const auto t0 = std::chrono::steady_clock::now();
    std::unique_lock mk(rec_map_mu);
    const bool done = rec_map_cv.wait_for(mk, std::chrono::milliseconds(250),
                                          [whole] { return rec_open_tag.load(std::memory_order_acquire) != whole; });
    *ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return done;
}
static void wait_rec_seq(uint64_t seq, uint64_t timeout_ns, int* ok) {
    // (longshot) reached already: nothing to wait for, and nothing to flush for it. The flush below took dev.present_mu,
    // which a present holds for as long as it takes: a readback whose recording was long done waited there for the
    // presenting thread's present, 4-5 ms (Hyrule Field: a job thread at 9 % of all instants; queue_flush_for now looks
    // at pend_held first)
    uint64_t reached = 0;
    if (vkGetSemaphoreCounterValue(dev.device, rec_timeline, &reached) == VK_SUCCESS && reached >= seq) {
        *ok = 1;
        wait_rec_result = VK_SUCCESS;
        return;
    }
    // (RX_VK_RECORDER_PARTS: its submit made by the recorder first, so that a batched one is in the list the flush sends; the
    // flush and the wait then need nothing more of the recorder: Settled)
    if (!wait_seq_submitted(seq, timeout_ns)) {   // (not submitted within the wait's time: as a wait that timed out)
        *ok = 0;
        wait_rec_result = VK_TIMEOUT;
        return;
    }
    rec::Settled settled;
    // (a batched submit holds its recordings' signal until the batch goes out: the readback wait in invalidate came here
    // without a flush, and the job that waited held up the main thread, so nothing flushed it: the load hang on RADV)
    queue_flush_for(FR_WAIT);
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &rec_timeline;
    wi.pValues = &seq;
    VkResult res;
    {
        FpsWait fw(fps_gpu_wait_kind());   // (the FPS counter's Breakdown; in the game's fence wait, its kind)
        res = vkWaitSemaphores(dev.device, &wi, timeout_ns);
    }
    *ok = res == VK_SUCCESS;
    wait_rec_result = res;
    const bool lost = res == VK_ERROR_DEVICE_LOST;   // (said as a lost device, not by vk_timeline_wait as a wait that timed out)
    if (lost) wait_lost("a wait for recording", seq);
}
uint64_t fence();
/// (longshot) A fence of one queue (nvnQueueFenceSync): the value of that queue's last submission
uint64_t fence_q(uint64_t queue) {
    if (async_on() || !sync_waits_on() || per_queue_fences_off() || !queue) return fence();
    std::lock_guard lk(sync_order_mu);
    auto it = q_last.find(queue);
    return it != q_last.end() ? it->second : fence();
}
// (longshot, RX_VK_LOG_READBACK_RANGES: gf_note_value) this thread's last wait for the GPU
thread_local int64_t gf_waited_ns = 0;
int64_t gf_now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void gate_need_value(uint64_t f);   // (below, with the render thread)
// (longshot) A thread of the game's waiting, with a timeout that is not a poll's, for the GPU to finish recorded work of a
// queue none of whose work up to it is stopped (wait's part wait): counted in gpu_waiters while it waits, for sync_watch's
// hold (hold_gpu.h)
struct GpuWaiter {
    const bool counted;
    explicit GpuWaiter(bool count) : counted(count) {
        if (counted) gpu_waiters.fetch_add(1, std::memory_order_acq_rel);
    }
    ~GpuWaiter() {
        if (counted) gpu_waiters.fetch_sub(1, std::memory_order_acq_rel);
    }
};
int wait_inner(uint64_t f, uint64_t timeout_ns);
// (RX_VK_GPU_TIMELINE: W, a wait of the game's for a fence that had not passed: begun, done, the fence's timeline value)
int wait(uint64_t f, uint64_t timeout_ns) {
    if (!gtl_on() || !timeout_ns || completed() >= f) return wait_inner(f, timeout_ns);
    const int64_t t0 = gtl_now();
    const int r = wait_inner(f, timeout_ns);
    gtl_line('W', f, t0, gtl_now(), (int64_t)(std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xffff), 0);
    return r;
}
int wait_inner(uint64_t f, uint64_t timeout_ns) {
    gf_waited_ns = gf_now();   // (RX_VK_LOG_READBACK_RANGES: this thread waited for the GPU, or asked whether it was done)
    if (completed() >= f) return 1;
    // (the FPS counter's Breakdown: this wait's kind, by whether its work was recorded when it began: the renderer behind, or
    // the GPU)
    bool recorded = true;
    if (timeout_ns && fps_breakdown_on()) {
        std::lock_guard mk(rec_map_mu);
        recorded = value_rec.count(f) != 0;
    }
    FpsFenceWait fence_kind(recorded);
    if (timeout_ns) gate_need_value(f);   // (the render thread's whole frames: this one's submit goes on without its present)
    FpsRender fr;   // (the FPS counter's Breakdown: the replay of stopped work in here is the renderer's, the waits the GPU's)
    // (RX_VK_RECORDER_PARTS: the parts waited for within the wait's time, the rest of it left to the waits below; a poll: not)
    timeout_ns = time_left(timeout_ns, queue_flush_for(FR_WAIT, timeout_ns));
    // (longshot) a fence of one queue whose work up to it is recorded whole (none of it stopped): that work, not the other
    // queues' stopped work the timeline above waits for (which a wait for it forced past its waits after RX_VK_WAIT_HOLD_US)
    if (!async_on() && sync_waits_on() && !per_queue_fences_off()) {
        bool mine = false;
        {
            std::lock_guard lk(sync_order_mu);
            auto it = value_queue.find(f);
            if (it != value_queue.end()) {
                auto q = qruns.find(it->second);
                mine = q == qruns.end() || !q->second.blocked() || q->second.oldest() > f;
            }
        }
        int ok = 0;
        if (mine) {
            GpuWaiter waiting(timeout_ns > 1000000);   // (a wait for the GPU, not a poll: sync_watch holds the stopped queues meanwhile)
            if (wait_recorded(f, timeout_ns, &ok)) return ok;
        }
    }
    // (a wait that needs stopped work: another queue's submit usually lets that on in a moment. Forced at once, as the main thread
    // waits ~15 times a second, 69000 waits went past in five minutes, and each one that mattered was a black frame.
    // RX_VK_WAIT_HOLD_US: how long the waiter gives it. A wait for older work than the stopped ones needs nothing of them)
    if (timeout_ns && !async_on() && blocked_n.load()) {
        static const int64_t hold_ns = (getenv("RX_VK_WAIT_HOLD_US") ? atoll(getenv("RX_VK_WAIT_HOLD_US")) : 3000) * 1000;
        const auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            queue_flush_for(FR_WAIT, 0);
            if (completed() >= f) return 1;
            bool needs;
            {
                std::lock_guard lk(sync_order_mu);
                needs = oldest_unfinished() <= f;
            }
            if (!needs) break;
            const int64_t waited = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
            if ((uint64_t)waited >= timeout_ns) break;   // (sync_watch's, past hold_ms)
            if (waited >= hold_ns) {
                sync_drain(2);
                break;
            }
            std::this_thread::yield();
        }
    }
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline;
    wi.pValues = &f;
    timeout_ns = time_left(timeout_ns, queue_flush_for(FR_WAIT, timeout_ns));
    rec::Settled settled;   // (RX_VK_RECORDER_PARTS: the flush waited for the parts, as long as the wait may; the rest to the driver)
    VkResult res;
    {
        FpsWait fw(fps_gpu_wait_kind());
        res = vkWaitSemaphores(dev.device, &wi, timeout_ns);
    }
    static int logged;
    if (res != VK_SUCCESS && timeout_ns > 1000000000ull && logged++ < 8)
        logf("vk: wait for fence %llu (done %llu, pushed %llu): %d", (unsigned long long)f,
             (unsigned long long)completed(), (unsigned long long)pushed.load(), (int)res);
    if (res == VK_ERROR_DEVICE_LOST) device_lost();
    return res == VK_SUCCESS;
}
void wait_queue_empty();
bool replay_behind();
void finish() {
    gf_waited_ns = gf_now();
    FpsWait fw(fps_stats::kGpuWait);   // (the FPS counter's Breakdown)
    wait_queue_empty();
    wait_value(pushed.load());
}
void wait_writebacks_reading(const void*, uint64_t) {}   // (nothing to write back: the GPU writes guest memory)
/// (host: the CPU is about to read [gpu, gpu + size), nvnMemoryPoolInvalidateMappedRange / nvnBufferInvalidateMappedRange)
/// RX_VK_LOG_INVALIDATE=1: the ranges, 400 lines, and the textures the GPU wrote there (their texels are not in memory)
// ---- live textures' storage, by 4 KB page (as the host made and finalized them, ahead of the replay) ----
// For invalidate: the CPU reads a range back; texels the GPU wrote into a texture there are not in memory until a
// write-back, and a texture the replay has not listed yet (it runs behind) needs it to catch up first. Ranges no live
// texture's storage touches hold buffers only, which the submissions write back themselves: no catch-up for them.
// (Counts per page: storage shared by several textures; two levels, chunks of 1M pages made when first touched.)
constexpr uint64_t kTexPageChunk = 1u << 20;
std::unique_ptr<std::atomic<uint16_t>[]> tex_pages[(1ull << 39 >> 12) / kTexPageChunk];
std::mutex tex_pages_mu;
// (longshot, the render thread) and by byte: a readback next to a texture's storage in its page, not in it, needs no catch-up
// (the Market: 2.4 such readbacks a frame, each held 3.3-11 ms for the jobs handed over). tex_live_mu
std::mutex tex_live_mu;
std::map<uint64_t, std::pair<uint64_t, int>> tex_live;   // (storage start -> its largest size, how many live textures start there)
uint64_t tex_live_max = 0;
void tex_live_add(const nvn_tex* t, int d) {
    std::lock_guard lk(tex_live_mu);
    if (d > 0) {
        auto& e = tex_live[t->gpu];
        e.first = std::max<uint64_t>(e.first, t->L.size), e.second++;
        tex_live_max = std::max<uint64_t>(tex_live_max, t->L.size);
    } else if (auto it = tex_live.find(t->gpu); it != tex_live.end() && --it->second.second <= 0) {
        tex_live.erase(it);
    }
}
bool tex_live_any(uint64_t gpu, uint64_t size) {
    std::lock_guard lk(tex_live_mu);
    for (auto it = tex_live.lower_bound(gpu > tex_live_max ? gpu - tex_live_max : 0); it != tex_live.end() && it->first < gpu + size; ++it)
        if (it->first + it->second.first > gpu) return true;
    return false;
}
void tex_pages_add(const nvn_tex* t, int d) {
    if (!t || !t->gpu || t->gpu >= (1ull << 39) || !t->L.size) return;   // (the guest window: virtual pools are not read)
    if (async_on()) tex_live_add(t, d);   // (the render thread's: invalidate)
    const uint64_t p0 = t->gpu >> 12, p1 = std::min((t->gpu + t->L.size - 1) >> 12, uint64_t((1ull << 39 >> 12) - 1));
    for (uint64_t p = p0; p <= p1; p++) {
        auto& c = tex_pages[p / kTexPageChunk];
        if (!c) {
            std::lock_guard lk(tex_pages_mu);
            if (!c) {
                auto n = std::make_unique<std::atomic<uint16_t>[]>(kTexPageChunk);
                for (uint64_t i = 0; i < kTexPageChunk; i++) n[i].store(0, std::memory_order_relaxed);
                c = std::move(n);
            }
        }
        std::atomic<uint16_t>& v = c[p % kTexPageChunk];
        if (d > 0) v.fetch_add(1, std::memory_order_relaxed);
        else if (v.load(std::memory_order_relaxed)) v.fetch_sub(1, std::memory_order_relaxed);
    }
}
bool tex_pages_any(uint64_t gpu, uint64_t size) {
    if (gpu >= (1ull << 39)) return true;   // (not tracked: as before)
    const uint64_t p0 = gpu >> 12, p1 = std::min((gpu + size - 1) >> 12, uint64_t((1ull << 39 >> 12) - 1));
    for (uint64_t p = p0; p <= p1; p++) {
        const auto& c = tex_pages[p / kTexPageChunk];
        if (!c) { p = (p / kTexPageChunk + 1) * kTexPageChunk - 1; continue; }
        if (c[p % kTexPageChunk].load(std::memory_order_relaxed)) return true;
    }
    return false;
}
void texture_created_of(const nvn_tex* t) {
    tex_pages_add(t, +1);
    texture_ahead_made(t);   // (longshot, RX_VK_TEX_AHEAD: it may be made and uploaded before its first draw)
}
uint64_t fence();
// ---- (longshot, diagnostics) RX_VK_LOG_RB_WRITERS=1 (with RX_VK_LOG_READBACK_WAITS=1 for a line a wait) ----
// What wrote the pages a readback waits for (memory_rb_writers: the op, the program, the part, the draw), where that is in its
// submission, and how long the read would have waited for the part its last writer was recorded in rather than for its whole
// submission: the readback waits for that part first, timed, then as before (the same wait in two steps). Every 3000
// readbacks, by the size read: the waits, the part's share of them, and the writers
struct SubSpan { uint64_t first_part = 0, last_part = 0, w0 = 0, w1 = 0; };
std::mutex rb_span_mu;
std::map<uint64_t, SubSpan> rb_spans;   // (submission value -> its first and last parts, its draws' and dispatches' count then)
void rb_span_begin(uint64_t value, uint64_t part) {
    std::lock_guard lk(rb_span_mu);
    SubSpan& s = rb_spans[value];
    if (!s.first_part) s.first_part = part, s.w0 = memory_work_count();
    while (rb_spans.size() > 4096) rb_spans.erase(rb_spans.begin());
}
void rb_span_end(uint64_t value, uint64_t part) {
    std::lock_guard lk(rb_span_mu);
    SubSpan& s = rb_spans[value];
    if (!s.first_part) s.first_part = part, s.w0 = memory_work_count();
    s.last_part = std::max(s.last_part, part), s.w1 = memory_work_count();
}
/// The recording about to be finished closes its submission's span: on every path that finishes one (run_submit's, a burst's
/// end, a resumed queue's capture). Closed only outside a burst before, a burst's span kept no last part or total work: the
/// log took the last writer's count for the total and printed a last part of 0
void rb_span_close() {
    if (rb_writers_on() && rec_cur) rb_span_end(rec_cur->value, rec_cur->part);
}
const char* rb_what_name(uint32_t what) {
    switch (what) {
    case kRbWriterUb: return "uniform-updates";
    case kRbWriterWb: return "write-back";
    case NOP_SIGNAL_EVENT: return "event";
    case NOP_COPY_B2B: return "copy";
    case NOP_CLEAR_BUF: return "clear";
    case NOP_COPY_T2B: return "texture->buffer";
    case NOP_DRAW: case NOP_DRAW_IDX: case NOP_DRAW_INDIRECT: case NOP_DRAW_MESH: return "draw";
    case NOP_DISPATCH: case NOP_DISPATCH_INDIRECT: return "dispatch";
    default: return "other";
    }
}
struct RbDiag {
    bool on = false;
    RbWriter first, last;
    int pages = 0;
    SubSpan span;
    double part_ms = -1;   // (when the last writer's part was done, into the wait; -1: not waited for apart)
    // (the bytes read, up to 1 MB: before the wait, once the last writer's part was done, after the whole wait. What changed
    // after the part says whether waiting for the part alone would have given the CPU the same bytes)
    bool snap = false;
    int64_t changed_before_part = 0, changed_after_part = 0;
};
thread_local std::vector<uint8_t> rb_snap[3];
uint64_t rb_diag_early_part(uint64_t early, uint64_t part, bool allowed) {
    return allowed && early ? std::max(early, part) : 0;
}
void rb_diag_wait(uint64_t gpu, uint64_t size, RbDiag& d, uint64_t whole, uint64_t part, bool early_allowed) {
    d.on = memory_rb_writers(host->gpu_guest(gpu), size, d.first, d.last, &d.pages);
    if (!d.on) return;
    // with RX_VK_READBACK_EARLY on, the part the early wait lets the read go at (its own writers' record, which holds
    // the writes over 1 MiB and the bare and wide bindings the diagnostics' record has not), or no check at all where the early
    // wait would take the whole submission: "bytes changed after the part" is then what an early read would have missed. (Any
    // CPU write to the range in the wait is counted too: the game's other threads run)
    if (readback_early_on() && whole) {
        double rec_ms = 0;   // (as invalidate: its writer's submission recorded to its end first, then its last writer's part)
        const bool recorded = !early_allowed || early_recorded_wait(whole, &rec_ms);
        const uint64_t early = recorded ? memory_in_place_writer_part(host->gpu_guest(gpu), size, whole) : 0;
        d.last.part = rb_diag_early_part(early, part, early_allowed);   // (exactly invalidate's target, or 0 for the whole)
    }
    if (size <= (1u << 20)) {
        rb_snap[0].resize(size);
        d.snap = host->gpu_read(gpu, rb_snap[0].data(), size);
    }
    {
        std::lock_guard lk(rb_span_mu);
        if (auto it = rb_spans.find(d.last.value); it != rb_spans.end()) d.span = it->second;
    }
    const auto t0 = std::chrono::steady_clock::now();
    int ok = 0;
    if (d.last.part && wait_recorded_part(d.last.part, UINT64_MAX, &ok))
        d.part_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (d.snap && d.part_ms >= 0) {
        rb_snap[1].resize(size);
        d.snap = host->gpu_read(gpu, rb_snap[1].data(), size);
    } else {
        d.snap = false;   // (no part waited for apart: nothing to tell)
    }
}
int64_t rb_bytes_differ(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    int64_t n = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); i++) n += a[i] != b[i];
    return n;
}
struct RbAgg {
    int64_t n = 0, parted = 0;
    double ms = 0, part_ms = 0, whole_ms = 0;   // (part_ms, whole_ms: the waits whose part was waited for apart)
    int64_t snaps = 0, changed = 0, changed_after = 0, after_waits = 0;   // (bytes changed in the wait, after the part; waits with any after)
    std::map<std::pair<uint32_t, uint64_t>, int> writers;
};
std::mutex rb_agg_mu;
std::map<uint64_t, RbAgg> rb_agg;   // (by the size read)
void rb_diag_done(uint64_t gpu, uint64_t size, RbDiag& d, double ms, bool log_line) {
    if (!d.on) return;
    if (d.snap) {
        rb_snap[2].resize(size);
        if ((d.snap = host->gpu_read(gpu, rb_snap[2].data(), size))) {
            d.changed_before_part = rb_bytes_differ(rb_snap[0], rb_snap[1]);
            d.changed_after_part = rb_bytes_differ(rb_snap[1], rb_snap[2]);
        }
    }
    {
        std::lock_guard lk(rb_agg_mu);
        RbAgg& a = rb_agg[size];
        a.n++, a.ms += ms;
        if (d.part_ms >= 0) a.parted++, a.part_ms += d.part_ms, a.whole_ms += ms;
        a.writers[{d.last.what, d.last.key}]++;
        if (d.snap) {
            a.snaps++, a.changed += d.changed_before_part + d.changed_after_part, a.changed_after += d.changed_after_part;
            a.after_waits += d.changed_after_part != 0;
        }
    }
    if (!log_line) return;
    const uint64_t w0 = d.span.w0, w1 = std::max(d.span.w1, d.last.work);
    logf("vk: readback writers %#llx +%#llx: last %s %016llx (part %llu, draw %llu of %llu in submission %llu), first %s %016llx "
         "(part %llu, draw %llu), %d pages; the submission's parts %llu..%llu; its last writer's part done %.2f ms into the wait "
         "of %.2f; bytes changed %lld by then, %lld after%s", (unsigned long long)gpu, (unsigned long long)size, rb_what_name(d.last.what), (unsigned long long)d.last.key,
         (unsigned long long)d.last.part, (unsigned long long)(d.last.work >= w0 ? d.last.work - w0 : 0),
         (unsigned long long)(w1 - w0), (unsigned long long)d.last.value, rb_what_name(d.first.what),
         (unsigned long long)d.first.key, (unsigned long long)d.first.part,
         (unsigned long long)(d.first.work >= w0 ? d.first.work - w0 : 0), d.pages, (unsigned long long)d.span.first_part,
         (unsigned long long)d.span.last_part, d.part_ms, ms, (long long)d.changed_before_part, (long long)d.changed_after_part,
         d.snap ? "" : " (not read)");
}
void rb_diag_report() {
    std::lock_guard lk(rb_agg_mu);
    for (auto& [size, a] : rb_agg) {
        if (a.n < 3) continue;
        std::vector<std::pair<int, std::pair<uint32_t, uint64_t>>> top;
        for (const auto& [k, c] : a.writers) top.push_back({c, k});
        std::sort(top.rbegin(), top.rend());
        std::string who;
        for (size_t i = 0; i < top.size() && i < 4; i++) {
            char one[96];
            snprintf(one, sizeof one, "%s%s %016llx x%d", i ? ", " : "", rb_what_name(top[i].second.first),
                     (unsigned long long)top[i].second.second, top[i].first);
            who += one;
        }
        logf("vk: readback writers, size %#llx: %lld waits, %.2f ms each; %lld with the last writer's part known: done %.2f ms into "
             "their %.2f ms (%.2f ms of each wait after it); bytes the wait changed: %.1f each, %.1f of them after the part (%lld of "
             "%lld waits); last writers: %s", (unsigned long long)size, (long long)a.n, a.ms / a.n,
             (long long)a.parted, a.parted ? a.part_ms / a.parted : 0.0, a.parted ? a.whole_ms / a.parted : 0.0,
             a.parted ? (a.whole_ms - a.part_ms) / a.parted : 0.0, a.snaps ? (double)a.changed / a.snaps : 0.0,
             a.snaps ? (double)a.changed_after / a.snaps : 0.0, (long long)a.after_waits, (long long)a.snaps, who.c_str());
    }
    rb_agg.clear();
}
// ---- (diagnostics) RX_VK_LOG_RB_WRITERS: what a read was let go with, at the game's own timing ----
// Each read whose range's newest writer's submission was not done keeps that writer's draw count and part as the diagnostics'
// record had them (memory_rb_writers), and whether that submission was still being recorded; once its recording is finished
// (rec_close, after its last part's uniform updates) the record says whether a writer of the range recorded after the read
// followed in the same submission: bytes the read was let go without, the game's to wait for. And whether the same thread read
// the range again for that submission, as a poll does. Every 3000 readbacks, by the size read. No wait and no copy here: with
// RX_VK_LOG_RB_WRITERS=2 this is all it does, and the replay's and the game's timing are their own (=1 adds the timed waits
// and the snapshots above). Not seen: the writes the record does not hold (bindings over 1 MiB, bare and wide bindings)
struct RbRelease {
    uint64_t gpu = 0, size = 0, value = 0, part = 0, work = 0;
    uint32_t tid = 0;
    bool open = false, early = false, closed = false, later = false, again = false, seen = false;
};
std::mutex rb_rel_mu;
std::vector<RbRelease> rb_rels;   // (rb_rel_mu)
bool rb_probe_only() {
    static const bool v = getenv("RX_VK_LOG_RB_WRITERS") && atoi(getenv("RX_VK_LOG_RB_WRITERS")) == 2;
    return v;
}
uint32_t rb_tid() {
    static std::atomic<uint32_t> next{0};
    thread_local const uint32_t id = ++next;
    return id;
}
/// A read of [gpu, gpu + size) whose newest write-back is value, let go now; pending: that submission was not done
void rb_release_note(uint64_t gpu, uint64_t size, uint64_t value, bool pending, bool early) {
    if (!value) return;
    const uint32_t tid = rb_tid();
    RbWriter first, last;
    int pages = 0;
    const bool known = pending && memory_rb_writers(host->gpu_guest(gpu), size, first, last, &pages) && last.value == value;
    const bool open = known && rec_open_tag.load(std::memory_order_acquire) == value;
    std::lock_guard lk(rb_rel_mu);
    for (RbRelease& r : rb_rels)
        if (r.gpu == gpu && r.size == size && r.value == value && r.tid == tid) r.again = true;
    if (!known) return;
    if (rb_rels.size() >= 4096) rb_rels.erase(rb_rels.begin());   // (bounded: a diagnostic's memory)
    RbRelease r;
    r.gpu = gpu, r.size = size, r.value = value, r.part = last.part, r.work = last.work, r.tid = tid, r.open = open, r.early = early;
    r.closed = !open;   // (its recording finished already: every writer of the range in it was in the record at the read)
    rb_rels.push_back(r);
}
/// The recording of value finished (rec_close): the reads let go before it, against the range's last writer in it
void rb_release_close(uint64_t value) {
    std::lock_guard lk(rb_rel_mu);
    for (RbRelease& r : rb_rels) {
        if (r.closed || r.value != value) continue;
        r.closed = true;
        RbWriter first, last;
        int pages = 0;
        if (memory_rb_writers(host->gpu_guest(r.gpu), r.size, first, last, &pages) && last.value == value)
            r.later = last.part > r.part || last.work > r.work;
    }
}
void rb_release_report() {
    struct Agg { int64_t n = 0, open = 0, early = 0, later = 0, later_early = 0, later_last = 0; };
    std::map<uint64_t, Agg> agg;
    {
        std::lock_guard lk(rb_rel_mu);
        std::erase_if(rb_rels, [&](RbRelease& r) {
            if (!r.closed) return false;
            if (!r.seen) return !(r.seen = true);   // (one more interval: a read again of it comes after its recording too)
            Agg& a = agg[r.size];
            a.n++, a.open += r.open, a.early += r.early, a.later += r.later, a.later_early += r.later && r.early;
            a.later_last += r.later && !r.again;
            return true;
        });
    }
    for (const auto& [size, a] : agg)
        if (a.n >= 3)
            logf("vk: readback releases, size %#llx: %lld let go before their writer's submission was done (%lld by the early wait), "
                 "%lld of them while it was still recorded; %lld before a writer of the range recorded later in it (%lld by the early "
                 "wait), %lld of those the thread's last read of the range for it", (unsigned long long)size, (long long)a.n,
                 (long long)a.early, (long long)a.open, (long long)a.later, (long long)a.later_early, (long long)a.later_last);
}
/// (longshot) invalidate's wait for a write-back's value, at most RX_VK_READBACK_WAIT_MS (default 250; 0: no limit, as before).
/// On the Steam Deck (RADV, no buffer mirrors, queues replayed in submit order) the game's main thread and one of its jobs
/// waited here, right after Kokiri Forest's save loaded, for a value the timeline never reached while they waited (it had been
/// submitted; the GPU stayed one value short), and the game stopped for good: 2 of 8 starts, 4 of 4 with
/// RX_VK_CAPTURE_WAIT=0. Past the limit the CPU reads what memory holds, as the capture's wait does (vk_timeline_wait), and
/// the first times are logged with how far the timeline and the submits had got: 6-8 a start, all in the save's load, and no
/// start stopped (0 of 4). True when reached
bool readback_wait_value(uint64_t v) {
    static const int64_t limit_ms = getenv("RX_VK_READBACK_WAIT_MS") ? atoll(getenv("RX_VK_READBACK_WAIT_MS")) : 250;
    if (limit_ms <= 0) {
        wait_value(v);
        return true;
    }
    // (RX_VK_RECORDER_PARTS: the parts waited for within the limit, the driver's wait given what is left of it)
    const uint64_t limit_ns = (uint64_t)limit_ms * 1000000ull, spent = queue_flush_for(FR_VALUE, limit_ns);
    rec::Settled settled;
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline;
    wi.pValues = &v;
    if (vkWaitSemaphores(dev.device, &wi, spent < limit_ns ? limit_ns - spent : 0) != VK_TIMEOUT) return true;
    static std::atomic<int> logged{0};
    if (logged++ < 8)
        logf("vk: a readback's wait for write-back %llu timed out after %lld ms (done %llu, submitted %llu, handed out %llu)",
             (unsigned long long)v, (long long)limit_ms, (unsigned long long)completed(), (unsigned long long)pushed.load(),
             (unsigned long long)q_values.load());
    return false;
}
// The diagnostic's second snapshot must be after the whole submission even if the normal path succeeded at an early part.
// A failed wait makes the comparison invalid; it must not be reported as an unchanged range.
void rb_diag_finish(RbDiag& d, uint64_t value) {
    if (!d.snap) return;
    // value_rec can still name an earlier part while the rest is being recorded. The submission timeline advances only
    // when the whole value is finished; a timeout leaves no valid second snapshot.
    d.snap = readback_wait_value(value);
}
/// (longshot) RX_VK_READBACK_LATE (readback_late.h): whether a readback of that size, written by the newest submission or
/// not, is read without its wait. Off unless the switch is set
bool readback_late_wanted(uint64_t size, bool newest) {
    static const readback_late::Rules rules = [] {
        const char* e = getenv("RX_VK_READBACK_LATE");
        readback_late::Rules r = readback_late::parse(e);
        if (!e || !*e) {   // (unset: the culling camera block on every PC, and on a handheld the newest submission's too)
            r.sizes = readback_late::default_sizes();
            r.newest = dev.policy.readback_late_newest;   // (device_policy.h)
        }
        if (r.any())
            logf("vk: readbacks read without their wait: %s%s (newest %d, %d sizes)", e && *e ? "RX_VK_READBACK_LATE=" : "",
                 e && *e ? e : r.newest ? "the culling camera block (0x3980) and the newest submission's (a handheld) by default"
                                        : "the culling camera block (0x3980) by default", (int)r.newest, (int)r.sizes.size());
        return r;
    }();
    return readback_late::wanted(rules, size, newest);
}
/// (longshot) RX_VK_LOG_READBACK_RANGES=1: the readbacks by range, in the log every 3000 calls: how many waited and for how
/// long, how many had the newest submission as their writer, how many were read without their wait (RX_VK_READBACK_LATE).
/// How the ranges worth a rule are found on a given PC
struct RbRange {
    uint64_t gpu, size;
    int64_t calls, waits, ns, newest, late;
    int64_t age = 0, aged = 0, at_ns = 0, sub = 0, after_wait = 0;   // (longshot: see gf_note_value)
};
std::mutex rb_ranges_mu;
std::vector<RbRange> rb_ranges;
// (longshot, RX_VK_LOG_READBACK_RANGES) How old what a readback gets is, in the game's frames: each submission's value with
// the game's frame (its presents handed over so far) and its place among that frame's submissions, kept for the last 4096
// values; a read's age is the frames handed over since the newest write-back of its range (0: written by work submitted
// since the last present). And when in the game's frame the read comes (ms after its last present's hand-over), and whether
// the reading thread waited for the GPU (nvnSyncWait, nvnQueueFinish) in the 2 ms before it
struct GfValue {
    uint32_t frame = 0, index = 0;
};
GfValue gf_values[4096];
std::atomic<uint32_t> gf_frame{0}, gf_index{0};
std::atomic<int64_t> gf_present_ns{0};
void gf_note_value(uint64_t value) { gf_values[value % 4096] = {gf_frame.load(), gf_index.fetch_add(1)}; }
void gf_note_present() { gf_frame++, gf_index = 0, gf_present_ns = gf_now(); }
void rb_range_age(uint64_t gpu, uint64_t size, uint64_t value) {
    const GfValue e = gf_values[value % 4096];
    const int64_t now = gf_now();
    std::lock_guard lk(rb_ranges_mu);
    for (auto& r : rb_ranges)
        if (r.gpu == gpu && r.size == size) {
            r.age += (int64_t)gf_frame.load() - e.frame, r.aged++, r.at_ns += now - gf_present_ns.load(), r.sub += e.index;
            r.after_wait += gf_waited_ns && now - gf_waited_ns < 2000000;
            return;
        }
}
void rb_range_note(uint64_t gpu, uint64_t size, bool waited, int64_t ns, bool newest, bool late) {
    std::lock_guard lk(rb_ranges_mu);
    for (auto& r : rb_ranges)
        if (r.gpu == gpu && r.size == size) {
            r.calls++;
            r.waits += waited;
            r.ns += ns;
            r.newest += newest;
            r.late += late;
            return;
        }
    if (rb_ranges.size() < 256) rb_ranges.push_back({gpu, size, 1, waited, ns, newest, late});
}
void rb_ranges_log() {
    static std::atomic<int> logged{0};
    std::vector<RbRange> top;
    {
        std::lock_guard lk(rb_ranges_mu);
        top = rb_ranges;
        rb_ranges.clear();
    }
    if (logged++ >= 400) return;
    static const bool by_size = getenv("RX_VK_LOG_READBACK_RANGES") && atoi(getenv("RX_VK_LOG_READBACK_RANGES")) == 2;   // (the largest first)
    std::sort(top.begin(), top.end(), [](const RbRange& a, const RbRange& b) {
        if (by_size && a.size != b.size) return a.size > b.size;
        return a.ns + a.late * 5000000 > b.ns + b.late * 5000000;
    });
    int64_t late = 0;
    for (const auto& r : top) late += r.late;
    std::string s;
    for (size_t i = 0; i < top.size() && i < 8; i++) {
        char b[360];
        const double n = top[i].aged ? (double)top[i].aged : 1.0;
        snprintf(b, sizeof b, " | %#llx +%#llx: %lld calls, %lld waited %.1f ms, %lld newest, %lld late; got work %.2f frames old "
                 "(its frame's submit %.1f), read %.1f ms into the frame, %lld after a GPU wait of its thread",
                 (unsigned long long)top[i].gpu, (unsigned long long)top[i].size, (long long)top[i].calls,
                 (long long)top[i].waits, top[i].ns / 1e6, (long long)top[i].newest, (long long)top[i].late, top[i].age / n,
                 top[i].sub / n, top[i].at_ns / 1e6 / n, (long long)top[i].after_wait);
        s += b;
    }
    logf("vk: readback ranges at present %lld: %d ranges, %lld read late%s", (long long)presents_now, (int)top.size(),
         (long long)late, s.c_str());
}
void readback_fell_back(int what = 0);   // (the render thread's log)
// ---- (longshot) RX_VK_LOG_EARLY_CHECK: what a read RX_VK_READBACK_EARLY let go would have missed against main's wait ----
// sheep's probe (Bunny Hood #2: the reader held at the early part, then for the whole submission) saw the 0x3980 camera block change
// after the early boundary in 269 of 745 compares, and could not tell the game's own writes from the GPU's; and the whole submission
// is more than main waits for. Here, for one read let go early in RX_VK_LOG_EARLY_CHECK (1: every one; 4: one in four):
//  - s1: the range as the early wait returns it;
//  - sm: the range right after the wait main makes for the same read (invalidate's usual choice for pages written in place: the
//    newest part of its submission submitted so far, wait_recorded, else the whole submission), made by this read now (the sampled
//    reads come back as main's would: a check run is not a measurement). s1 -> sm is what the early wait let the game miss;
//  - s2: the range once the read's submission is done on the main timeline, copied by a thread of its own after it waits (later,
//    informative only: a copy after a host wait is not at the GPU's boundary). sm -> s2 is the parts recorded after the read, which
//    main misses too.
// The game's own writes: the soft write watch (only CPU stores, and the host's marked writes, mark a page; the GPU's writes in place
// never do). A page the collections report from the read until the second full collection begun after a copy (memory_collect_written:
// a mark published late) is the CPU's in that interval. A change on a page the CPU wrote is ambiguous (the GPU may have written the
// same page): what proves a missed GPU write is a change on a page the CPU did not write, and the clean intervals (no CPU page) are
// counted. A host write into guest memory that is not marked (some nvn getters) would show as the GPU's. The GPU writes the replay
// records after the read in a later part of the read's submission are counted by kind
struct EcDiff {
    int64_t words_gpu = 0, words_cpu = 0;   // (changed 4-byte words on pages the CPU did not write, and on pages it did)
    int64_t first_gpu = -1;                 // (the offset of the first such word on a page the CPU did not write)
};
/// a -> b over [guest, guest + size): changed words, by whether the CPU wrote a page they are on (bit k of cpu: the range's k-th page)
EcDiff ec_compare(const uint8_t* a, const uint8_t* b, uint64_t guest, uint64_t size, uint64_t cpu) {
    EcDiff d;
    const auto touched = [&](uint64_t off) {
        const uint64_t k = ((guest + off) >> 12) - (guest >> 12);
        return k < 64 && ((cpu >> k) & 1);
    };
    for (uint64_t off = 0; off < size; off += 4) {
        const uint64_t n = std::min<uint64_t>(4, size - off);
        if (!std::memcmp(a + off, b + off, n)) continue;
        if (touched(off) || touched(off + n - 1)) {   // (a word across a page's end: either page)
            d.words_cpu++;
        } else {
            if (d.first_gpu < 0) d.first_gpu = (int64_t)off;
            d.words_gpu++;
        }
    }
    return d;
}
/// the pages of [guest, guest + size) among the collected ones (in any order), as bits by the range's page
uint64_t ec_pages_hit(uint64_t guest, uint64_t size, const uint32_t* pages, int64_t n) {
    const uint64_t p0 = guest >> 12, p1 = (guest + size - 1) >> 12;
    uint64_t m = 0;
    for (int64_t i = 0; i < n; i++)
        if (pages[i] >= p0 && pages[i] <= p1 && pages[i] - p0 < 64) m |= 1ull << (pages[i] - p0);
    return m;
}
int early_check_every() {   // (RX_VK_LOG_EARLY_CHECK=<n>: one read in n let go early is checked; 0, unset or RX_VK_READBACK_EARLY off: none)
    static const int n = [] {
        const char* e = getenv("RX_VK_LOG_EARLY_CHECK");
        return readback_early_on() && e ? std::max(0, atoi(e)) : 0;
    }();
    return n;
}
bool early_check_on() { return early_check_every() > 0; }
struct EarlyCheck {
    uint64_t gpu = 0, guest = 0, size = 0, value = 0, part = 0;
    std::vector<uint8_t> s1, sm, s2;
    uint64_t cpu1 = 0, cpu2 = 0;      // (pages the CPU wrote: s1..sm, sm..s2)
    uint32_t gen_m = 0, gen_2 = 0;    // (the full collections begun when sm, s2 had been copied)
    bool closed1 = false, closed2 = false, valid1 = false, valid2 = false;
    int stage = 0;                    // (0: sm being taken by the read, 1: s2 to take, 2: the collections to close the intervals)
    uint64_t wb_2 = 0;                // (the range's newest write-back noted when s2 was taken: a newer submission's?)
    int gpu_noted = 0, kinds = 0;     // (the replay's own GPU writes after the read, later parts: bit 0 binding, 1 over 1 MiB, 2 bare or wide)
};
std::mutex ec_mu;
std::condition_variable ec_cv;
std::deque<std::unique_ptr<EarlyCheck>> ec_pending;   // (ec_mu; in the order of the reads)
std::atomic<uint32_t> ec_full_begun{0};
struct EcStats {
    int64_t n = 0, lost1 = 0, same1 = 0, gpu1 = 0, amb1 = 0, clean1 = 0, clean_gpu1 = 0;
    int64_t lost2 = 0, same2 = 0, gpu2 = 0, amb2 = 0, newer2 = 0, noted = 0;
    int64_t kinds[3] = {};
};
EcStats ec_st;                               // (ec_mu)
std::map<uint64_t, EcStats> ec_by_size;      // (ec_mu)
std::atomic<int64_t> ec_skipped{0};
bool ec_wait(VkSemaphore s, uint64_t v) {
    if (!s || !v) return false;
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &s;
    wi.pValues = &v;
    return vkWaitSemaphores(dev.device, &wi, 3000000000ull) == VK_SUCCESS;
}
void ec_finish(EarlyCheck& e) {   // (ec_mu held) one check's counts
    EcStats* ss[2] = {&ec_st, &ec_by_size[e.size]};
    const EcDiff d1 = e.valid1 ? ec_compare(e.s1.data(), e.sm.data(), e.guest, e.size, e.cpu1) : EcDiff{};
    const EcDiff d2 = e.valid2 ? ec_compare(e.sm.data(), e.s2.data(), e.guest, e.size, e.cpu2) : EcDiff{};
    const bool newer = e.wb_2 > e.value;   // (a later submission was recorded writing the range before s2: its changes may be in it)
    for (EcStats* s : ss) {
        s->n++;
        if (!e.valid1) s->lost1++;
        else if (d1.words_gpu) s->gpu1++;
        else if (d1.words_cpu) s->amb1++;
        else s->same1++;
        if (e.valid1 && !e.cpu1) s->clean1++, s->clean_gpu1 += d1.words_gpu != 0;
        if (!e.valid2) s->lost2++;
        else if (newer && (d2.words_gpu || d2.words_cpu)) s->newer2++;
        else if (d2.words_gpu) s->gpu2++;
        else if (d2.words_cpu) s->amb2++;
        else s->same2++;
        s->noted += e.gpu_noted != 0;
        for (int k = 0; k < 3; k++) s->kinds[k] += (e.kinds >> k) & 1;
    }
    static int told1, told2;
    if (d1.words_gpu && told1++ < 16)
        logf("vk: early-read check: %#llx +%#llx (submission %llu, let go at part %llu): %lld words changed by main's release on pages "
             "the CPU did not write, the first at +%#llx (%lld on pages it wrote); GPU writes recorded after the read: %d (kinds %#x)",
             (unsigned long long)e.gpu, (unsigned long long)e.size, (unsigned long long)e.value, (unsigned long long)e.part,
             (long long)d1.words_gpu, (unsigned long long)d1.first_gpu, (long long)d1.words_cpu, e.gpu_noted, e.kinds);
    if (d2.words_gpu && !newer && told2++ < 16)
        logf("vk: early-read check: %#llx +%#llx (submission %llu, let go at part %llu): %lld words changed after main's release, before "
             "the submission's end, on pages the CPU did not write, the first at +%#llx; GPU writes recorded after the read: %d (kinds %#x)",
             (unsigned long long)e.gpu, (unsigned long long)e.size, (unsigned long long)e.value, (unsigned long long)e.part,
             (long long)d2.words_gpu, (unsigned long long)d2.first_gpu, e.gpu_noted, e.kinds);
    if (ec_st.n >= 100) {
        std::string sizes;
        for (const auto& [sz, s] : ec_by_size) {
            char b[200];
            snprintf(b, sizeof b, " | %#llx: %lld, missed %lld (clean %lld, missed of them %lld), ambiguous %lld, after main's %lld",
                     (unsigned long long)sz, (long long)s.n, (long long)s.gpu1, (long long)s.clean1, (long long)s.clean_gpu1,
                     (long long)s.amb1, (long long)s.gpu2);
            sizes += b;
        }
        logf("vk: early-read check (RX_VK_LOG_EARLY_CHECK=%d), %lld reads (%lld skipped): early -> main's release: %lld unchanged, %lld "
             "changed on a page the CPU did not write (a GPU write main waits for and the early wait missed: should be 0), %lld changed "
             "only on pages the CPU wrote (ambiguous), %lld lost; %lld with no CPU page written (clean), %lld of them missed; main's "
             "release -> the submission's end: %lld unchanged, %lld GPU (parts recorded after the read), %lld ambiguous, %lld a newer "
             "submission's, %lld lost; GPU writes the replay recorded after the read in a later part: %lld reads (bindings %lld, over "
             "1 MiB %lld, bare or wide %lld)%s",
             early_check_every(), (long long)ec_st.n, (long long)ec_skipped.exchange(0), (long long)ec_st.same1, (long long)ec_st.gpu1,
             (long long)ec_st.amb1, (long long)ec_st.lost1, (long long)ec_st.clean1, (long long)ec_st.clean_gpu1, (long long)ec_st.same2,
             (long long)ec_st.gpu2, (long long)ec_st.amb2, (long long)ec_st.newer2, (long long)ec_st.lost2, (long long)ec_st.noted,
             (long long)ec_st.kinds[0], (long long)ec_st.kinds[1], (long long)ec_st.kinds[2], sizes.c_str());
        ec_st = EcStats{};
        ec_by_size.clear();
    }
}
void ec_reap() {   // (ec_mu held) the checks done, in the order of their reads
    while (!ec_pending.empty() && ec_pending.front()->stage == 2 && ec_pending.front()->closed1 && ec_pending.front()->closed2) {
        ec_finish(*ec_pending.front());
        ec_pending.pop_front();
    }
}
void early_check_thread() {   // (s2: once each check's submission is done on the main timeline)
    for (;;) {
        EarlyCheck* e = nullptr;
        {
            std::unique_lock lk(ec_mu);
            ec_cv.wait(lk, [] {
                for (auto& x : ec_pending) if (x->stage == 1) return true;
                return false;
            });
            for (auto& x : ec_pending)
                if (x->stage == 1) { e = x.get(); break; }
        }
        // (the entry stays in ec_pending, and only reaches stage 2, which may be reaped, below: its address holds)
        const bool ok = ec_wait(timeline, e->value);
        std::vector<uint8_t> snap(e->size);
        const bool read = ok && host->gpu_read(e->gpu, snap.data(), e->size);
        const uint64_t wb = read ? memory_writeback_value(e->guest, e->size, nullptr) : 0;
        std::lock_guard lk(ec_mu);
        e->s2 = std::move(snap), e->valid2 = read, e->wb_2 = wb, e->stage = 2;
        e->gen_2 = ec_full_begun.load();   // (after the copy: a collection begun after this saw any CPU write before it)
        e->cpu2 |= e->cpu1;   // (pages collected while sm was copied are in cpu1 only)
        if (!read) e->closed2 = true;
        ec_reap();
    }
}
/// (invalidate) one read in RX_VK_LOG_EARLY_CHECK let go at its writer's part: what it returns, then main's wait for it and what that
/// returns. v: the range's newest write-back; checked only when it is the in-place writer's (no mirror's part in it), up to 64 KiB
void early_check_read(uint64_t gpu, uint64_t size, uint64_t v, uint64_t whole, uint64_t part, bool unforced) {
    static std::atomic<int64_t> reads{0};
    if (reads.fetch_add(1, std::memory_order_relaxed) % early_check_every()) return;
    if (size > 0x10000 || v != whole) {
        ec_skipped++;
        return;
    }
    static std::once_flag started;
    std::call_once(started, [] { std::thread(early_check_thread).detach(); });
    EarlyCheck* e = nullptr;
    bool ok1 = false;
    {
        std::lock_guard lk(ec_mu);
        if (ec_pending.size() >= 32) {
            ec_skipped++;
            return;
        }
        auto x = std::make_unique<EarlyCheck>();
        x->gpu = gpu, x->guest = host->gpu_guest(gpu), x->size = size, x->value = whole, x->part = part;
        // (copied under ec_mu and listed before it is let go: a collection that finds a CPU write made after the copy reports it after
        // it scanned, so after this, and finds the check)
        x->s1.resize(size);
        ok1 = host->gpu_read(gpu, x->s1.data(), size);
        e = x.get();
        ec_pending.push_back(std::move(x));
    }
    // (main's wait for this read: invalidate's usual choice for a page written in place whose submission is pending)
    int ok = 0;
    if (!(unforced && wait_recorded(v, UINT64_MAX, &ok))) {
        if (unforced && replay_behind()) wait_queue_empty();
        readback_wait_value(v);
    }
    std::vector<uint8_t> sm(size);
    const bool okm = host->gpu_read(gpu, sm.data(), size);
    std::lock_guard lk(ec_mu);
    e->sm = std::move(sm), e->valid1 = ok1 && okm, e->stage = 1;
    e->gen_m = ec_full_begun.load();   // (after the copy)
    ec_cv.notify_one();
}
// (longshot) the invalidates that kept their virtual core (invalidate_quick) and the ones that gave it up, counted on each
// thread and added here every 256 calls (the kept ones come per shape per view: no shared line written at each), on the
// readbacks line's cadence below
std::atomic<int64_t> inv_kept{0}, inv_gave{0};
void inv_count(bool kept) {
    thread_local uint32_t k = 0, g = 0;
    (kept ? k : g)++;
    if (k + g < 256) return;
    inv_kept.fetch_add(k, std::memory_order_relaxed), inv_gave.fetch_add(g, std::memory_order_relaxed);
    k = g = 0;
}
void invalidate(uint64_t gpu, uint64_t size) {
    static const bool log_inv = env_on("RX_VK_LOG_INVALIDATE");
    static std::atomic<int> logged{0};
    if (log_inv && logged++ < 400) {
        std::string hits;
        {
            std::lock_guard lk(replay_mu);
            texture_describe_written(gpu, size, hits);
        }
        logf("vk: invalidate %#llx +%#llx:%s", (unsigned long long)gpu, (unsigned long long)size,
             hits.empty() ? " no texture the GPU wrote" : hits.c_str());
    }
    // (longshot) a range the CPU reads: a large binding over it keeps its write-back (wb_learn.h, RX_VK_WB_MAX_KB)
    memory_note_cpu_read(host->gpu_guest(gpu), size);
    {   // (EvilBoyce, diagnostics) RX_VK_LOG_READBACK=<present>: from that present on, 300 invalidates with the fence handed
        // out, the GPU's completed value, whether the replay is behind, and the first words the CPU is about to read
        static const int64_t from = getenv("RX_VK_LOG_READBACK") ? atoll(getenv("RX_VK_LOG_READBACK")) : -1;
        static std::atomic<int> n{0};
        if (from >= 0 && presents_now >= from && n++ < 300) {
            uint32_t w[8]{};
            host->gpu_read(gpu, w, sizeof w);
            logf("vk: readback %lld %#llx +%#llx: fence %llu done %llu behind %d | %08x %08x %08x %08x %08x %08x %08x %08x",
                 (long long)presents_now, (unsigned long long)gpu, (unsigned long long)size, (unsigned long long)fence(),
                 (unsigned long long)completed(), (int)replay_behind(), w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
        }
    }
    // (EvilBoyce) GPU results in buffers: the CPU reads them once the submission that writes them back is done. The game
    // reads them when its own pacing says the GPU has finished that frame, which on the console's coherent memory means the
    // values are there; here they reach guest memory only with the write-back at the end of the submission that the replay
    // may not have recorded or the GPU not finished yet. OoT reads its exposure meter (a sum the GPU adds to each frame and
    // the CPU reads and sets to 0) this way: it read 0 one frame, then the old sum came back over the CPU's 0 and the next
    // frame added to it (2x): the exposure stepped 0.2 EV up and down several times a second (the white flashes).
    if (size) {
        bool ever = false;
        if ((memory_writeback_value(host->gpu_guest(gpu), size, &ever), ever)) {
            memory_note_readback(host->gpu_guest(gpu), size);   // (the CPU may write it back: memory_rb_refresh)
            static std::atomic<int64_t> calls{0}, waited{0}, ns{0};
            const auto w0 = std::chrono::steady_clock::now();
            bool w = false;
            const uint64_t f0 = fence(), c0 = completed();
            const bool behind0 = replay_behind();
            static const bool no_catchup = env_on("RX_VK_READBACK_NO_CATCHUP");   // (A/B: recorded work only)
            // (longshot) not on the submitting thread's replay (the default): there what is handed over is recorded at once, and
            // the work not recorded yet is stopped at an event wait. Catching up forced it past that wait, ahead of the work it
            // waits for (OoT's presentation work before the scene's: the fog of the frame before, for a frame, at 1 in 20 of the
            // exposure readbacks). The write-back is waited for on the recordings' timeline instead, which a stopped queue
            // does not hold back. The stopped work whose waits are met goes on first, never forced (sync_settle): it may be what
            // writes this range, and its write-back is known only once it is recorded. Without it the CPU read the range
            // before that work, with nothing to wait for (once d6c5ddcc's catch-up no longer forced anything, it was left
            // out here too). RX_VK_PER_QUEUE_FENCES=0 waits for every queue's work instead: the game's readbacks 19-87 ms
            // each on an RX 5500M, against ~1 ms here
            // (longshot) RX_VK_ASYNC=1, the render thread: no catch-up either, and nothing forced. The game is a frame ahead
            // of the replay by design there: what it reads is what the GPU has written so far, as on the console, whose GPU
            // runs behind the CPU too. What must hold is that no write-back lands on the range after the CPU has read it and
            // written its answer (the exposure sum the CPU reads and sets to 0): the write-backs of the work recorded so far
            // are waited for below, by part; the work still queued takes the range from guest memory when the GPU gets to it
            // (memory_rb_refresh), after the CPU's answer. Catching up instead (the older replay thread, removed) held the
            // game's job threads for the whole replay of the frame, 7.5-11 ms a readback in the Market, and forced half of
            // the stopped submissions past their waits (the flashes)
            // (not unforced: the submitting thread's replay with a dump being written (dump_on) or RX_VK_PER_QUEUE_FENCES=0, as before)
            const bool behind = async_on();
            const bool unforced = behind || (sync_waits_on() && !per_queue_fences_off());
            static std::atomic<int64_t> settled{0}, held_back{0}, in_place{0};
            // That is the console's answer for a read the game makes without waiting for the GPU: the latest result that has
            // landed. Where the game does wait first (a fence, nvnSyncWait, nvnQueueFinish), that wait is the exact one and
            // the read after it finds its write-back recorded. RX_VK_READBACK_EXACT (a test, off): 1, every range is read
            // after everything handed over before the read is replayed as far as the game has let it go (wait_queue_empty
            // there: nothing forced, and work stopped behind a signal the CPU has not made yet is not waited for); 2, only
            // the ranges over 16 KiB (tables the CPU acts on, not meters: Hyrule Field's 0x8b040 and 0x9b040 bytes)
            static const int exact_env = getenv("RX_VK_READBACK_EXACT") ? atoi(getenv("RX_VK_READBACK_EXACT")) : 0;
            const bool exact = behind && (exact_env == 1 || (exact_env == 2 && size > 0x4000));
            static std::atomic<int64_t> st_early_waits{0}, st_early_ns{0}, st_early_whole{0};   // (RX_VK_READBACK_EARLY)
            static std::atomic<int64_t> st_early_rec{0}, st_early_rec_ns{0}, st_early_rec_late{0};   // (and the waits for the writer's recording first)
            if (behind0 && !no_catchup) {   // (work handed over may write it back too)
                if (!unforced) wait_queue_empty();
                else if (behind) {   // (the render thread: neither, see above; but an exact range)
                    if (exact) wait_queue_empty(), readback_fell_back(3);
                }
                else if (sync_settle_met()) settled++;
                w = !behind || exact;
            }
            const auto w1 = std::chrono::steady_clock::now();
            // (longshot) what is waited for is the part of the submission that writes the range back, when its pages come
            // from a mirror (memory_end_submission, at each part's end), not the submission's last part: the range is in
            // guest memory once that part's copy is done, and the later parts read their mirrors and the submit's capture,
            // not guest memory. A value's recording is its last part's, so waiting for it would wait for the GPU's whole
            // submission: one job a frame for ~5 ms, the game's other job threads polling meanwhile. A page written in place (no
            // mirror: `whole`) keeps the wait for all of its submission, whose later parts read and write guest memory
            // itself.
            uint64_t part = 0, whole = 0;
            const uint64_t v = memory_writeback_value(host->gpu_guest(gpu), size, nullptr, &part, &whole);
            static const bool log_each = env_on("RX_VK_LOG_READBACK_WAITS");
            RbDiag rbd;   // (RX_VK_LOG_RB_WRITERS: its writers, and its last writer's part waited for first, timed: below)
            uint64_t l_part = 0, l_last = 0, l_sent = 0, l_done = 0;   // (the log's: recordings, on their timeline)
            if (log_each && v > completed() && rec_timeline) {
                std::lock_guard mk(rec_map_mu);
                if (auto it = part_rec.find(part); it != part_rec.end()) l_part = it->second;
                if (auto it = value_rec.find(v); it != value_rec.end()) l_last = it->second;
                if (!part_rec.empty()) l_sent = part_rec.rbegin()->second;
                vkGetSemaphoreCounterValue(dev.device, rec_timeline, &l_done);
            }
            const bool pending = v > completed(), newest = pending && v >= f0;
            const bool late = pending && readback_late_wanted(size, newest);   // (RX_VK_READBACK_LATE: read without its wait)
            if (rb_writers_on() && pending && !late && !rb_probe_only())
                rb_diag_wait(gpu, size, rbd, whole, part, unforced && whole > completed());
            // (longshot) RX_VK_READBACK_EARLY (on where no pool has a mirror): pages written in place (the Steam Deck's every page) wait for
            // up to the part their last writer was recorded in, not their whole submission: otherwise the game's job polls ~18 ms a
            // frame for its camera block's read while the GPU finishes the rest of the submission after the writer. The parts after
            // it may still read guest memory in place, which the game, let go, may write: as on the console, whose GPU reaches the
            // writer before the rest of its command buffer too, but the replay's GPU runs later than the console's (reads must not
            // be let go before their writer: value_rec). Pages whose reads waited end their writer's part right after it
            // (memory_split_wanted). Not known (0: written before it was on, or its part not mapped): the whole submission. A
            // range RX_VK_READBACK_LATE reads late is not waited for at all, here either
            bool early_done = false;
            // (only a read with a page written in place pending: with VRAM mirrors (a dGPU) a page is written back by part already,
            // and nothing is noted, split or rebuilt for it: on an RTX 5090 the switch cost ~2 fps doing that and waited early 0 times.
            // A sparse mirror's block with no memory yet is written in place for a submission: those pages take the early wait)
            if (pending && !late && readback_early_on() && unforced && whole > completed()) {
                memory_rb_waited(host->gpu_guest(gpu), size);
                // (its writer's submission recorded to its end first: the part found below is then its last writer's, not the
                // newest recorded so far. Past 250 ms, the whole submission as before)
                double rec_ms = 0;
                const bool recorded = early_recorded_wait(whole, &rec_ms);
                if (rec_ms > 0) {
                    st_early_rec.fetch_add(1, std::memory_order_relaxed);
                    st_early_rec_ns.fetch_add((int64_t)(rec_ms * 1e6), std::memory_order_relaxed);
                }
                if (!recorded) st_early_rec_late.fetch_add(1, std::memory_order_relaxed);
                const uint64_t writer = recorded ? memory_in_place_writer_part(host->gpu_guest(gpu), size, whole) : 0;
                const uint64_t target = std::max(writer, part);   // (a mirror's part as well, when pages of both kinds are read)
                int ok = 0;
                const auto e0 = std::chrono::steady_clock::now();
                if (writer && wait_recorded_part(target, UINT64_MAX, &ok)) {
                    early_done = true, w = true;
                    if (early_check_on()) early_check_read(gpu, size, v, whole, target, unforced);   // (RX_VK_LOG_EARLY_CHECK)
                    st_early_waits.fetch_add(1, std::memory_order_relaxed);
                    st_early_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - e0).count(),
                                          std::memory_order_relaxed);
                } else {
                    st_early_whole.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (early_done) {
            } else if (pending && !late) {
                int ok = 0;
                // (and not when a part after it reads guest memory in place, on a card whose pools are not all mirrored
                // or whose mirrors take pages no capture holds: the game, let go after the first part, would write what
                // those parts still read. Then the last part, as before)
                const bool in_place_after = memory_in_place_part() > part;
                if (part && in_place_after) held_back++;
                const bool parted = unforced && part && whole <= completed() && !in_place_after &&
                                    wait_recorded_part(part, UINT64_MAX, &ok);
                if (!parted && !part) in_place++;   // (pages written in place (no mirror: part 0): the whole submission waited for)
                if (!parted && !(unforced && wait_recorded(v, UINT64_MAX, &ok))) {
                    if (unforced && replay_behind()) wait_queue_empty();   // (not recorded whole: as before)
                    if (behind) readback_fell_back();
                    readback_wait_value(v);
                }
                w = true;
            }
            if (w) {
                rb_diag_finish(rbd, v);
                waited++;
                const int64_t d = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - w0).count();
                ns += d;
                static std::atomic<int> logged{0};
                const bool line = log_each && presents_now > 4000 && logged++ < 300;
                if (line)
                    logf("vk: readback wait %lld %#llx +%#llx: %.2f ms (catch-up %.2f) value %llu, fence %llu, done %llu, behind %d; "
                         "recordings: its part's %llu, its submission's last %llu (in place: value %llu), %llu submitted, %llu done",
                         (long long)presents_now, (unsigned long long)gpu, (unsigned long long)size, d / 1e6,
                         std::chrono::duration_cast<std::chrono::nanoseconds>(w1 - w0).count() / 1e6, (unsigned long long)v,
                         (unsigned long long)f0, (unsigned long long)c0, (int)behind0, (unsigned long long)l_part,
                         (unsigned long long)l_last, (unsigned long long)whole, (unsigned long long)l_sent, (unsigned long long)l_done);
                if (presents_now > 4000) rb_diag_done(gpu, size, rbd, d / 1e6, line);
            }
            if (rb_writers_on() && presents_now > 4000) rb_release_note(gpu, size, v, pending, early_done);
            static const bool log_ranges = env_on("RX_VK_LOG_READBACK_RANGES");
            if (log_ranges) {
                rb_range_note(gpu, size, w,
                              w ? std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - w0).count() : 0,
                              newest, late);
                // (what the CPU is about to read: the range's newest write-back, waited for above unless read late)
                rb_range_age(gpu, size, late ? std::min<uint64_t>(v, completed()) : v);
            }
            if (++calls % 3000 == 0) {
                if (log_ranges) rb_ranges_log();
                const int64_t n = waited.exchange(0);
                logf("vk: readbacks of GPU results: %lld of 3000 waited for their write-back, %.2f ms each; %lld looked at the "
                     "stopped queues first, %lld waited for their submission's last part (guest memory read in place after theirs), "
                     "%lld waited for their whole submission (their pages written in place, no mirror)",
                     (long long)n, n ? ns.exchange(0) / (double)n / 1e6 : 0.0, (long long)settled.exchange(0),
                     (long long)held_back.exchange(0), (long long)in_place.exchange(0));
                if (rb_writers_on()) rb_diag_report(), rb_release_report();
                if (readback_early_on()) {
                    const int64_t e = st_early_waits.exchange(0), r = st_early_rec.exchange(0);
                    logf("vk: readbacks waited for their writer's part (RX_VK_READBACK_EARLY): %lld, %.2f ms each; %lld for their whole "
                         "submission (writer not known); %lld parts ended after a readback's writer; %lld waited for their writer's "
                         "submission to be recorded first, %.2f ms each (%lld of them past 250 ms: their whole submission)", (long long)e,
                         e ? st_early_ns.exchange(0) / (double)e / 1e6 : 0.0, (long long)st_early_whole.exchange(0),
                         (long long)st_early_splits.exchange(0), (long long)r, r ? st_early_rec_ns.exchange(0) / (double)r / 1e6 : 0.0,
                         (long long)st_early_rec_late.exchange(0));
                }
                logf("vk: invalidates since the last readbacks line: %lld kept their virtual core (nothing there to wait for), %lld "
                     "gave it up (counted by 256 a thread; RX_VK_INVALIDATE_QUICK=0: none kept)",
                     (long long)inv_kept.exchange(0), (long long)inv_gave.exchange(0));
            }
        }
    }
    // (textures the GPU wrote there: their texels into memory before the CPU reads it, as vmap_changing does. The game
    // waited for the work that wrote them; the replay is caught up first)
    {   // (RX_VK_READBACK_OFF_FILE=<path> (A/B): while that file exists, not)
        static const char* off_file = getenv("RX_VK_READBACK_OFF_FILE");
        if (off_file && GetFileAttributesA(off_file) != INVALID_FILE_ATTRIBUTES) return;
    }
    // (the replay caught up first only for ranges a texture the GPU wrote may be in: MKW invalidates a few buffer
    // ranges every frame (GPU results it reads back, written back with their submission; one of them a ring of
    // slots at a new address each time), and the catch-up for each held its main thread until the replay had
    // recorded everything queued: ~10% of the main thread's time. A 1 MB region caught up for kProbe times without a
    // texture found holds buffers only from then on; one a texture was ever found in keeps the catch-up.)
    // (decided by the live textures' storage, not by what the replay has listed: a region guessed to hold buffers
    // only, after probes that found no texture, left MKW's Grand Prix waiting at its map for a texture's texels)
    if (!size) return;
    // (longshot) and on the submitting thread's replay, the catch-up lets on what may go without forcing anything past its wait
    // (once the exposure readback above no longer did, this one forced OoT's presentation work on, every frame or two)
    // (RX_VK_ASYNC=1, the render thread: wait_queue_empty there waits for what was handed over before this call, nothing
    // forced)
    const bool unforced = !async_on() && sync_waits_on() && !per_queue_fences_off();
    if (!texture_gpu_written_in(gpu, size)) {
        if (!replay_behind()) return;
        if (!tex_pages_any(gpu, size)) return;   // (no texture's storage there: buffers only)
        if (async_on()) {
            if (!tex_live_any(gpu, size)) return readback_fell_back(1);
            readback_fell_back(2);
            static std::atomic<int> told{0};
            if (told++ < 24) logf("vk: a readback in a live texture's storage: %#llx +%#llx", (unsigned long long)gpu, (unsigned long long)size);
        }
        unforced ? sync_settle() : wait_queue_empty();
        if (!texture_gpu_written_in(gpu, size)) return;
    }
    {   // (what the catch-ups for a texture cost the game's thread, every 300 calls)
        static std::atomic<int64_t> calls{0}, ns{0};
        const auto w0 = std::chrono::steady_clock::now();
        unforced ? sync_settle() : wait_queue_empty();
        ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - w0).count();
        if (++calls % 300 == 0)
            logf("vk: invalidate %#llx +%#llx: textures there; the catch-ups waited %.2f ms each (300 calls)",
                 (unsigned long long)gpu, (unsigned long long)size, ns.exchange(0) / 300.0 / 1e6);
    }
    std::lock_guard lk(replay_mu);
    texture_flush_to_guest(gpu, size);
}
/// (longshot) invalidate with the game's virtual core kept (host/rx_backend.c rx_nvn_invalidate, nvn_invalidate_quick): 1 when
/// invalidate would find nothing in [gpu, gpu + size) to wait for or to write back, done here as it would be (the CPU's read
/// noted for the write-backs left out, memory_note_cpu_read: the one thing it does then); 0 when it may find something, and
/// the host calls it with the core given up, as before. Read without a lock, from marks set and never taken off
/// (page_marks.h): no page there ever written back (memory_writeback_ever: invalidate's `ever`, none of its waits), no texture
/// the GPU wrote ever listed there (texture_gpu_written_ever: texture_gpu_written_in false, no texels to write back) and no
/// live texture's storage (tex_pages_any, as invalidate reads it: no catch-up, behind or not). OoT's model calc invalidates
/// each shape's 64-byte occlusion-query slot, per view, from the stage slot jobs: the counts are written there on the CPU
/// (REPORT_COUNTER and occ_collect, guest_cpu_write), never written back, and each call gave up the core, let another thread
/// on it and queued to get one back, for nothing to wait for. On the console the call is a cache operation and the thread
/// runs on. It takes no lock but memory_note_cpu_read's (wb_learn_mu, a leaf, only with RX_VK_WB_MAX_KB) and never waits,
/// so holding the core here cannot keep another guest thread from what it waits for. Not with invalidate's diagnostics on
/// (RX_VK_LOG_INVALIDATE, RX_VK_LOG_READBACK); ranges past 512 GB (virtual pools, not followed) always go the slow way.
/// RX_VK_INVALIDATE_QUICK=0: never, as before
int invalidate_quick(uint64_t gpu, uint64_t size) {
    static const bool on = [] {
        const char* q = getenv("RX_VK_INVALIDATE_QUICK");
        const char* rb = getenv("RX_VK_LOG_READBACK");
        const bool v = (!q || atoi(q) != 0) && !env_on("RX_VK_LOG_INVALIDATE") && !(rb && atoll(rb) >= 0);
        logf("vk: invalidates with nothing to wait for keep their virtual core: %s",
             v ? "on" : "off (RX_VK_INVALIDATE_QUICK=0 or invalidate's logs)");
        return v;
    }();
    constexpr uint64_t kFollowed = 1ull << 39;   // (tex_pages' and the marks' window: GPU addresses below 512 GB)
    const bool kept = on && (!size || (gpu < kFollowed && size <= kFollowed - gpu && !tex_pages_any(gpu, size) &&
                                       !texture_gpu_written_ever(gpu, size) &&
                                       !memory_writeback_ever(host->gpu_guest(gpu), size)));
    if (kept) memory_note_cpu_read(host->gpu_guest(gpu), size);
    inv_count(kept);
    return kept;
}
std::atomic<uint64_t> output_preview_retired_serial;

void do_texture_released(const nvn_tex* t) {
    std::lock_guard lk(replay_mu);
    if (t) {
        mkwvk::texture_released(t->serial);   // (vk_bank.cpp: the registrations of it)
        mkwvk::texture_released(t);           // (vk_texture.cpp: its image may take CPU writes again)
        if (t->output_preview == 3) output_preview_retired_serial.store(t->serial, std::memory_order_release);
    }
}

// ---- presents ----
struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence done = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkSemaphore retired_acquire = VK_NULL_HANDLE;
};
Frame frames[kPresentSlotsMax];   // (present_image uses present_slots_for of them)
uint32_t frame_index = 0;
int64_t presents = 0;

// offscreen (no window): the picture, and the readback of a screenshot
VkImage off_image = VK_NULL_HANDLE;
VmaAllocation off_alloc = nullptr;
// (two readbacks, by the present's parity: a shot is read at a later present, after that one's own copy was submitted,
// and with one buffer the CPU read it while the GPU wrote the next frame over it: torn shots, and flashes counted)
VkBuffer shot_bufs[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
VmaAllocation shot_allocs[2] = {nullptr, nullptr};
void* shot_maps[2] = {nullptr, nullptr};
// (RX_VK_SHOT_W / RX_VK_SHOT_H: another size, e.g. the game's output size for its own pixels)
uint32_t shot_dimension(const char* text, uint32_t fallback) {
    if (!text) return fallback;
    if (!*text) return 0;
    uint32_t value = 0;
    for (; *text; text++) {
        if (*text < '0' || *text > '9') return 0;
        const uint32_t digit = (uint32_t)(*text - '0');
        if (value > (UINT32_MAX - digit) / 10) return 0;
        value = value * 10 + digit;
    }
    return value;
}
const uint32_t kShotW = shot_dimension(getenv("RX_VK_SHOT_W"), 1280),
               kShotH = shot_dimension(getenv("RX_VK_SHOT_H"), 720);
VkImage shot_image = VK_NULL_HANDLE;
VkImageView shot_view = VK_NULL_HANDLE;   // (longshot: RX_VK_SHOT_MENU's menu draws through it)
VkBuffer made_buf = VK_NULL_HANDLE;   // (RX_VK_SHOT_MADE: a made frame's readback)
VmaAllocation made_buf_alloc = nullptr;
void* made_map = nullptr;
VkImage made_image = VK_NULL_HANDLE;
VmaAllocation made_image_alloc = nullptr;
VmaAllocation shot_image_alloc = nullptr;

bool shot_dimensions_ready() {
    static int state = 0;
    if (state) return state > 0;
    state = -1;
    const uint32_t limit = dev.props.limits.maxImageDimension2D;
    const uint64_t pixels = (uint64_t)kShotW * kShotH, row = (uint64_t)kShotW * 3 + 1;
    bool valid = false;
    if (kShotW && kShotH && kShotW <= limit && kShotH <= limit && pixels <= SIZE_MAX / 4) {
        const uint64_t raw = row * kShotH;
        // write_png emits one IDAT: zlib header, stored blocks (5 bytes per65535), and Adler32.
        // PNG chunks are limited to2^31-1 bytes. This is an encoder limit, not a memory budget.
        constexpr uint64_t max_chunk = 0x7fffffffu;
        if (raw <= max_chunk) {
            const uint64_t encoded = 2 + raw + 5 * ((raw + 65534) / 65535) + 4;
            valid = encoded <= max_chunk && encoded <= SIZE_MAX - 4;
        }
    }
    if (!valid) {
        logf("vk: screenshots disabled: RX_VK_SHOT_W/H must be positive decimal dimensions within the device limit "
             "(%u) and the PNG/readback size limits (got %ux%u)", limit, kShotW, kShotH);
        return false;
    }
    state = 1;
    return true;
}

bool made_shot_init() {
    if (made_image) return true;
    VkImage next_image = VK_NULL_HANDLE;
    VmaAllocation next_alloc = nullptr, next_buf_alloc = nullptr;
    VkBuffer next_buf = VK_NULL_HANDLE;
    void* next_map = nullptr;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {kShotW, kShotH, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!create_image(ci, next_image, next_alloc)) return false;
    if (!create_buffer((uint64_t)kShotW * kShotH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                       next_buf, next_buf_alloc, &next_map)) {
        destroy_image(next_image, next_alloc);
        return false;
    }
    made_image = next_image, made_image_alloc = next_alloc;
    made_buf = next_buf, made_buf_alloc = next_buf_alloc, made_map = next_map;
    return true;
}

bool offscreen_init() {
    if (!shot_dimensions_ready()) return false;
    if (off_image) return true;
    VkImage next_off = VK_NULL_HANDLE, next_shot = VK_NULL_HANDLE;
    VkImageView next_view = VK_NULL_HANDLE;
    VmaAllocation next_off_alloc = nullptr, next_shot_alloc = nullptr, next_allocs[2]{};
    VkBuffer next_bufs[2]{};
    void* next_maps[2]{};
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {dev.swap_extent.width, dev.swap_extent.height, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    bool ok = create_image(ci, next_off, next_off_alloc);
    ci.extent = {kShotW, kShotH, 1};
    ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;   // (longshot: RX_VK_SHOT_MENU draws the menu over it, vk_menu.cpp)
    ok = ok && create_image(ci, next_shot, next_shot_alloc);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = next_shot;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ci.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    ok = ok && vkCreateImageView(dev.device, &vi, nullptr, &next_view) == VK_SUCCESS;
    for (int i = 0; i < 2; i++)
        ok = ok && create_buffer((uint64_t)kShotW * kShotH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                                 next_bufs[i], next_allocs[i], &next_maps[i]);
    if (!ok) {
        for (int i = 0; i < 2; i++)
            if (next_bufs[i]) destroy_buffer(next_bufs[i], next_allocs[i]);
        if (next_view) vkDestroyImageView(dev.device, next_view, nullptr);
        if (next_shot) destroy_image(next_shot, next_shot_alloc);
        if (next_off) destroy_image(next_off, next_off_alloc);
        return false;
    }
    // Publish the ready marker only after both images, the menu view and both mapped readbacks exist.
    off_image = next_off, off_alloc = next_off_alloc;
    shot_image = next_shot, shot_image_alloc = next_shot_alloc;
    shot_view = next_view;
    for (int i = 0; i < 2; i++)
        shot_bufs[i] = next_bufs[i], shot_allocs[i] = next_allocs[i], shot_maps[i] = next_maps[i];
    return true;
}

void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

// A completed capture copy must also be made available in the host memory domain.
void capture_host_read_barrier(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize bytes) {
    VkBufferMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = buffer;
    b.size = bytes;
    VkDependencyInfo d{};
    d.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    d.bufferMemoryBarrierCount = 1;
    d.pBufferMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

/// The crop of src (x, y, w, h; w 0: all of it) scaled into a rectangle of dst.
/// An empty source clears the whole destination image; current callers give that fallback a full-image rectangle.
void blit_crop_to_rect(VkCommandBuffer cmd, VkImage src, uint32_t sw, uint32_t sh, const int crop[4], VkImage dst,
                       VkImageLayout dst_layout, PresentRect rect) {
    const PresentSourceRect source = present_source_rect(sw, sh, crop);
    if (!rect.w || !rect.h) return;
    if (!source.w || !source.h) {
        const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, dst, dst_layout, &black, 1, &range);
        return;
    }
    VkImageBlit b{};
    b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[0] = {(int)source.x, (int)source.y, 0};
    b.srcOffsets[1] = {(int)(source.x + source.w), (int)(source.y + source.h), 1};
    b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.dstOffsets[0] = {(int)rect.x, (int)rect.y, 0};
    b.dstOffsets[1] = {(int)(rect.x + rect.w), (int)(rect.y + rect.h), 1};
    vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_GENERAL, dst, dst_layout, 1, &b, VK_FILTER_LINEAR);
}

/// The crop of src scaled to the whole destination (captures keep their existing dimensions).
void blit_crop(VkCommandBuffer cmd, VkImage src, uint32_t sw, uint32_t sh, const int crop[4], VkImage dst,
               VkImageLayout dst_layout, uint32_t dw, uint32_t dh) {
    blit_crop_to_rect(cmd, src, sw, sh, crop, dst, dst_layout, {0, 0, dw, dh});
}

// ---- PNG (stored deflate blocks: no compression library needed) ----
uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> out{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t k = i;
            for (int j = 0; j < 8; j++) k = k & 1 ? 0xEDB88320u ^ (k >> 1) : k >> 1;
            out[i] = k;
        }
        return out;
    }();
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}
} // namespace
/// Whether the storage of a texture the game made reaches [gpu, gpu + size) (tex_pages_any: true where it is not
/// tracked), for vk_memory.cpp's late pages left to their reads (RX_VK_LATE_DEFER)
bool texture_storage_any(uint64_t gpu, uint64_t size) { return tex_pages_any(gpu, size); }
/// Whether the replay runs on the render thread (async_on: RX_VK_ASYNC, or the Threaded rendering row), for
/// vk_memory.cpp's RX_VK_READBACK_EARLY, which is left off with it
bool replay_render_thread_on() { return async_on(); }

// (vk_internal.h) One part of a submission done on the GPU: the late pages' fix reads a page of the capture whose fix an earlier
// part of this submission recorded (vk_memory.cpp, late_fix_cpu_read), after that part's fix has run
int vk_part_wait(uint64_t part, uint64_t timeout_ns) {
#ifdef TRACY_ENABLE
    HT_WAIT(ht_w, "vk_part_wait");
#endif
    int ok = 0;
    if (!wait_recorded_part(part, timeout_ns, &ok)) return -1;
    return ok ? 1 : 0;
}

// Called while replaying, including workers joined before the next signal is issued.
// The current logical value may already have completed before a stopped queue resumes.
// A new signal instead follows every recording using the images being retired, including
// batched work and the unsignalled post-present interpolation recording. Zero retains forever.
uint64_t vk_retirement_value() {
    return timeline && signaled != UINT64_MAX ? signaled + 1 : 0;
}

/// A submission or a wait came back VK_ERROR_DEVICE_LOST. Nothing sent from here on runs: a replay that kept going would
/// complete no submission again, let go of no capture or staging arena, and fill system memory. So the report once (a thread
/// that sees the loss too waits for it, instead of ending the process before it is written), then the process ends as
/// replay_fatal does
[[noreturn]] void device_lost(const char* why) {
    static std::once_flag once;
    std::call_once(once, [] {
        perf_report_final(perf_report::kDeviceLost);   // (the performance summary's last, once, before the process ends)
        checkpoints_report();
    });
    // (the presents on a queue of their own: once an RTX 4090 on NVIDIA's 621.14 had a black screen with them, Hookshot issue
    // 1161: the way back named where a player reads it. A local array: device_lost can run on several threads at once, and
    // replay_fatal uses the text before it ends the process)
    char lost[400];
    std::snprintf(lost, sizeof lost, "%s%s",
                  why ? why
                      : "the GPU device was lost (VK_ERROR_DEVICE_LOST): the driver stopped or reset the GPU, and nothing sent "
                        "to it runs any more (on Linux, dmesg or journalctl -k around this time names the reset)",
                  dev.present_queue_supported ? ". The presents were on a queue of their own: RX_VK_PRESENT_QUEUE=0 in "
                                                "longshot_env.txt turns that off; please report your GPU and driver" : "");
    replay_fatal(lost);
}

/// (longshot) A GPU hang the driver never reports as a lost device. RTX 4070 Ti, NVIDIA 595.99.02, Linux, Hyrule Castle: the
/// kernel's Xid 109 (CTX SWITCH TIMEOUT) ended the game's GPU context, the timeline stopped 4 short of what was sent, and
/// from then on every wait came back VK_TIMEOUT or never came back, none VK_ERROR_DEVICE_LOST: device_lost() did not run,
/// the game froze, and the log had no report of what the GPU was running. A thread reads the timeline once a second from the first present: a lost device
/// it sees goes to device_lost(), and work sent to the queue that makes no progress at all for RX_VK_GPU_HANG_SEC
/// seconds (default 30) is taken as a hang and goes there too. A window the system stops presenting can hold the queue
/// that long as well (the present waits for its image): RX_VK_GPU_HANG_SEC=0 logs the stall and lets the game wait.
void gpu_hang_watch_start() {
    static std::atomic<bool> started{false};
    if (!timeline || started.exchange(true)) return;
    const char* e = getenv("RX_VK_GPU_HANG_SEC");
    const int limit = e ? std::max(0, atoi(e)) : 30;
    // (atoi reads a typo as 0, which turns the exit off: the value it settled on is logged)
    if (limit) logf("vk: GPU hang watch: work that finishes nothing for %d s ends the game (RX_VK_GPU_HANG_SEC)", limit);
    else logf("vk: GPU hang watch: a stall is only logged (RX_VK_GPU_HANG_SEC=%s)", e);
    std::thread([limit] {
        SetThreadDescription(GetCurrentThread(), L"mkw_vk gpu hang watch");
        uint64_t last = 0;
        int still = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            uint64_t done = 0;
            if (vkGetSemaphoreCounterValue(dev.device, timeline, &done) == VK_ERROR_DEVICE_LOST) device_lost();
            const uint64_t sent = timeline_sent.load(std::memory_order_relaxed);
            if (done >= sent || done != last) {
                last = done, still = 0;
                continue;
            }
            still++;
            if (still == 10 || still % 30 == 0)
                logf("vk: the GPU has finished nothing for %d s: the timeline at %llu, %llu sent (RX_VK_GPU_HANG_SEC=%d)",
                     still, (unsigned long long)done, (unsigned long long)sent, limit);
            if (limit && still >= limit) {
                static char why[256];
                std::snprintf(why, sizeof why, "the GPU hung: no submission finished in %d s, and the driver never reported the "
                              "device lost (on Linux, dmesg or journalctl -k around this time names the fault, an Xid on NVIDIA)", still);
                logf("vk: GPU hang: the timeline at %llu, %llu sent", (unsigned long long)done, (unsigned long long)sent);
                device_lost(why);
            }
        }
    }).detach();
}

bool write_png(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h) {
    auto failed = [path](const char* phase, unsigned error = 0) {
        static std::atomic<unsigned> reported{0};
        if (reported.fetch_add(1, std::memory_order_relaxed) < 8)
            logf("vk: PNG %s failed (error %u): %s", phase, error, path ? path : "(null)");
        return false;
    };
    if (!path || !*path || !rgba || !w || !h || w > 0x7fffffffu || h > 0x7fffffffu)
        return failed("dimensions/path");
    const uint64_t pixels = (uint64_t)w * h;
    if (pixels > SIZE_MAX / 4) return failed("size");
    const uint64_t raw_size = ((uint64_t)w * 3 + 1) * h;
    constexpr uint64_t max_chunk = 0x7fffffffu;
    if (raw_size > max_chunk || 6 + raw_size + 5 * ((raw_size + 65534) / 65535) > max_chunk)
        return failed("size");
    try {
        std::vector<uint8_t> raw;
        raw.reserve((size_t)(w * 3 + 1) * h);
        for (uint32_t y = 0; y < h; y++) {
            raw.push_back(0);
            for (uint32_t x = 0; x < w; x++) {
                const uint8_t* q = rgba + ((size_t)y * w + x) * 4;
                raw.insert(raw.end(), q, q + 3);
            }
        }
        std::vector<uint8_t> z = {0x78, 0x01};
        uint32_t a = 1, b = 0;
        for (uint8_t c : raw) a = (a + c) % 65521, b = (b + a) % 65521;
        for (size_t at = 0; at < raw.size();) {
            const size_t n = std::min<size_t>(65535, raw.size() - at);
            z.push_back(at + n == raw.size() ? 1 : 0);
            z.push_back((uint8_t)n), z.push_back((uint8_t)(n >> 8));
            z.push_back((uint8_t)~n), z.push_back((uint8_t)(~n >> 8));
            z.insert(z.end(), raw.begin() + (long)at, raw.begin() + (long)(at + n));
            at += n;
        }
        const uint32_t adler = b << 16 | a;
        for (int i = 3; i >= 0; i--) z.push_back((uint8_t)(adler >> (i * 8)));

        // Encode before opening a file; only a completed private temporary replaces the destination.
        static std::atomic<uint64_t> next{0};
        std::string temporary;
        FILE* f = nullptr;
        for (int attempt = 0; attempt < 128; attempt++) {
            temporary = std::string(path) + ".part." + std::to_string(next.fetch_add(1, std::memory_order_relaxed));
            f = fopen(temporary.c_str(), "wbx");
            if (f || errno != EEXIST) break;
        }
        if (!f) return failed("open", errno);
        bool ok = true;
        unsigned error = 0;
        const char* phase = "write";
        auto put = [&](const void* bytes, size_t n) {
            if (ok && n && fwrite(bytes, 1, n, f) != n) {
                error = errno ? errno : EIO;
                ok = false;
            }
        };
        auto be32 = [&](uint32_t v) {
            const uint8_t q[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
            put(q, sizeof q);
        };
        auto chunk = [&](const char* type, const uint8_t* d, size_t n) {
            be32((uint32_t)n);
            put(type, 4);
            put(d, n);
            be32(crc32(d, n, crc32((const uint8_t*)type, 4)) ^ 0xFFFFFFFFu);
        };
        static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
        put(sig, sizeof sig);
        const uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                                 (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0};
        chunk("IHDR", ihdr, sizeof ihdr);
        chunk("IDAT", z.data(), z.size());
        chunk("IEND", nullptr, 0);
        if (fclose(f) != 0) {
            if (ok) phase = "close", error = errno ? errno : EIO;
            ok = false;
        }
        if (ok) {
            phase = "publish";
#ifdef _WIN32
            ok = MoveFileExA(temporary.c_str(), path, MOVEFILE_REPLACE_EXISTING) != 0;
            if (!ok) error = GetLastError();
#else
            ok = rename(temporary.c_str(), path) == 0;
            if (!ok) error = errno;
#endif
        }
        if (!ok) {
            if (remove(temporary.c_str()) != 0) failed("temporary cleanup", errno);
            return failed(phase, error);
        }
        return true;
    } catch (const std::exception&) {
        return failed("encoding allocation");
    }
}

// (do_present: the game's picture is already HDR10, Native HDR taken. Read by present_image, and by vk_dlss.cpp, whose
// feature is made for HDR colour while it is)
std::atomic<bool> present_native_pq{false};

namespace {

int64_t fps_n = 0;
std::chrono::steady_clock::time_point fps_t = std::chrono::steady_clock::now();

/// RX_VK_SLOW_LOG=<ms>: a frame that long after the last: its replay's times, for what the stutters are
void log_slow_frame() {
    static const int limit = getenv("RX_VK_SLOW_LOG") ? atoi(getenv("RX_VK_SLOW_LOG")) : 0;
    if (!limit) return;
    static auto last_t = std::chrono::steady_clock::now();
    static int64_t last[PROF_COUNT];
    static int64_t last_pl, last_tr, last_d, last_up;
    static int logged;
    const auto now = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - last_t).count();
    last_t = now;
    int64_t d = 0, sk = 0, di = 0, pl = 0, tr = 0, fl = 0, up = 0, astc = 0, upb = 0;
    draw_stats(&d, &sk, &di, &pl);
    shader_stats(&tr, &fl);
    texture_stats(&up, &astc, &upb);
    if (ms >= limit && d - last_d >= 500 && logged < 800) {
        logged++;
        const auto dp = [](int i) { return (prof_ns[i] - last[i]) / 1e6; };
        logf("vk: slow frame at present %lld: %.1f ms; submit %.1f, draw %.1f, descriptors %.1f, pipelines %.1f, programs %.1f, "
             "commands %.1f, memory sync %.1f, queue submit %.1f, slot wait %.1f; %lld draws, %lld new pipelines, %lld new programs, "
             "%lld uploads", (long long)presents, ms, dp(PROF_SUBMIT), dp(PROF_DRAW), dp(PROF_DESC), dp(PROF_PIPE), dp(PROF_PROGRAM),
             dp(PROF_CMDS), dp(PROF_MEMSYNC), dp(PROF_QSUBMIT), dp(PROF_SLOTWAIT), (long long)(d - last_d), (long long)(pl - last_pl),
             (long long)(tr - last_tr), (long long)(up - last_up));
    }
    for (int i = 0; i < PROF_COUNT; i++) last[i] = prof_ns[i];
    last_pl = pl, last_tr = tr, last_d = d, last_up = up;
}

uint64_t made_ring[32];
int64_t made_ring_present = 0;
void log_frame_stats() {
    log_slow_frame();
    made_ring[presents % 32] = texture_made();
    made_ring_present = presents;
    if (presents % 600 == 0) programs_census_log("play");
    if (++fps_n == 120) {
        const auto now = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(now - fps_t).count();
        int64_t d = 0, sk = 0, di = 0, pl = 0, tr = 0, fl = 0, passes = 0, mesh = 0, up = 0, astc = 0, upb = 0;
        int64_t st_clears = 0, masked = 0;
        clear_stats(&st_clears, &masked);
        double pms = 0;
        draw_stats(&d, &sk, &di, &pl);
        draw_stats2(&passes, &mesh, &pms);
        shader_stats(&tr, &fl);
        texture_stats(&up, &astc, &upb);
        int64_t pskip = 0, pwaits = 0;
        double pwait_ms = 0;
        draw_stats3(&pskip, &pwaits, &pwait_ms);
        static int64_t l_pskip, l_pwaits;
        static double l_pwait_ms;
        static int64_t l_reused, l_saves;
        int64_t links = 0, lib_hits = 0, lib_cold = 0, lib_built = 0;
        double link_ms = 0, lib_build_ms = 0;
        static int64_t l_links;
        static double l_link_ms;
        char libs[256] = "";   // (the stage libraries' part of the line, with RX_VK_GPL only: other logs keep the line they had)
        if (draw_stats4(&links, &link_ms, &lib_hits, &lib_cold, &lib_built, &lib_build_ms))
            snprintf(libs, sizeof libs, "; %.2f library links tried (%.2f ms), %lld libraries the driver had and %lld cold in all; "
                     "%lld of the cold built by workers (%.0f ms)", (links - l_links) / 120.0, (link_ms - l_link_ms) / 120.0,
                     (long long)lib_hits, (long long)lib_cold, (long long)lib_built, lib_build_ms);
        logf("vk: a frame: %.0f draws kept their neighbour's descriptors (%.0f of them through other slots' registrations)",
             (draw_desc_reused() - l_reused) / 120.0, (draw_slotgen_saves() - l_saves) / 120.0);
        l_reused = draw_desc_reused(), l_saves = draw_slotgen_saves();
        programs_frames_note();   // (the load default's safety net: vk_programs.h)
        logf("vk: a frame: %.1f draws left out (not ready), %.2f waits for shaders or pipelines (%.1f ms); "
             "translation %.0f ms in all, %d jobs queued%s",
             (pskip - l_pskip) / 120.0, (pwaits - l_pwaits) / 120.0, (pwait_ms - l_pwait_ms) / 120.0,
             shader_translate_ms(), jobs_pending(), libs);
        l_pskip = pskip, l_pwaits = pwaits, l_pwait_ms = pwait_ms, l_links = links, l_link_ms = link_ms;
        {
            static const char* names[PROF_COUNT] = {"submit", "present", "draw", "descriptors", "texture lookups",
                                                    "uploads", "pipelines", "programs", "commands", "dispatch",
                                                    "memory sync", "queue submit", "slot wait"};
            static int64_t last[PROF_COUNT];
            char line[600];
            int n = snprintf(line, sizeof line, "vk: a frame's replay time (ms):");
            for (int i = 0; i < PROF_COUNT; i++) {
                n += snprintf(line + n, sizeof line - n, " %s %.2f%s", names[i], (prof_ns[i] - last[i]) / 120.0 / 1e6,
                              i + 1 < PROF_COUNT ? "," : "");
                last[i] = prof_ns[i];
            }
            logf("%s", line);
        }
        int64_t mirrors = 0, sync_b = 0, sync_c = 0, wb_b = 0;
        uint64_t mirror_mb = 0;
        memory_stats(&mirrors, &mirror_mb, &sync_b, &sync_c, &wb_b);
        static int64_t l_sync_b, l_sync_c, l_wb_b;
        logf("vk: a frame: mirrors %lld (%llu MB): %.2f MB synced in %.1f copies, %.2f MB written back",
             (long long)mirrors, (unsigned long long)mirror_mb, (sync_b - l_sync_b) / 120.0 / 1048576.0,
             (sync_c - l_sync_c) / 120.0, (wb_b - l_wb_b) / 120.0 / 1048576.0);
        l_sync_b = sync_b, l_sync_c = sync_c, l_wb_b = wb_b;
        static int64_t l_d, l_sk, l_di, l_pass, l_sub, l_up, l_upb, l_bar, l_ubu, l_ubo, l_wait;
        logf("vk: %lld presents, %.1f fps; a frame: %.0f draws (%.0f skipped), %.0f dispatches, %.0f passes, %.0f "
             "barriers, %.0f submits, %.1f uploads (%.1f MB), %.0f ub updates (%.0f overlays), GPU-bound wait %.1f ms (recording slots only); "
             "%lld pipelines (%.0f ms), %lld programs (%lld failed), %zu textures (%llu MB)",
             (long long)presents, 120.0 / secs, (d - l_d) / 120.0, (sk - l_sk) / 120.0, (di - l_di) / 120.0,
             (passes - l_pass) / 120.0, (st_barriers - l_bar) / 120.0, (st_submits - l_sub) / 120.0,
             (up - l_up) / 120.0, (upb - l_upb) / 120.0 / 1048576.0, (st_ub_updates - l_ubu) / 120.0,
             (st_ub_overlays - l_ubo) / 120.0, (st_wait_ns - l_wait) / 120.0 / 1e6, (long long)pl, pms,
             (long long)tr, (long long)fl, texture_count(), (unsigned long long)(texture_bytes() >> 20));
        {
            const auto waits = gpu_wait_stats::counters.take();
            const double scale = 1.0 / (120.0 * 1e6);
            logf("vk: host time in GPU resource waits: %.2f ms a frame (fences %.2f, timelines %.2f, idle %.2f, queries %.2f; "
                 "sum across threads, not frame time; excludes pacing and driver submit/present blocking)",
                 (waits[gpu_wait_stats::kFence] + waits[gpu_wait_stats::kTimeline] + waits[gpu_wait_stats::kIdle] +
                  waits[gpu_wait_stats::kQuery]) * scale,
                 waits[gpu_wait_stats::kFence] * scale, waits[gpu_wait_stats::kTimeline] * scale,
                 waits[gpu_wait_stats::kIdle] * scale, waits[gpu_wait_stats::kQuery] * scale);
        }
        draw_skip_report(120.0);
        {   // (longshot: the full barriers recorded and the ones left out as the second of two in a row, RX_VK_BARRIER_DEDUPE_OFF=1)
            static int64_t l_made, l_left;
            int64_t made = 0, left = 0;
            barrier_stats(&made, &left);
            static int64_t l_relaxed;
            const int64_t relaxed = pass_barriers_relaxed();
            logf("vk: a frame: %.1f full barriers recorded, %.1f left out (nothing recorded since the one before), %.1f pass starts "
                 "ordered in their fragment stages only (draw passes after draw passes)", (made - l_made) / 120.0, (left - l_left) / 120.0,
                 (relaxed - l_relaxed) / 120.0);
            l_made = made, l_left = left, l_relaxed = relaxed;
            std::string fl;   // (NVN's barrier bits: ORDER_PRIMITIVES 1, ORDER_FRAGMENTS 2, ORDER_FRAGMENTS_TILED 4, ORDER_INDIRECT_DATA 8, INVALIDATE_TEXTURE 0x10, _SHADER 0x20, _TEXTURE_DESCRIPTOR 0x40, _ZCULL 0x80)
            std::lock_guard lk(st_barrier_flags_mu);
            for (const auto& [bits, n] : st_barrier_flags) {
                char b[48];
                snprintf(b, sizeof b, " %#x: %.1f;", (unsigned)bits, n / 120.0);
                fl += b;
            }
            logf("vk: a frame: the game's barriers by their NVN flags:%s", fl.c_str());
            st_barrier_flags.clear();
        }
        {   // (longshot, RX_VK_MESH_CONV: the indirect mesh draws' arguments, vk_draw.cpp mesh_indirect_convert and mesh_conv_batch)
            int64_t mc[7];
            mesh_conv_stats(mc);
            int64_t stops[mesh_conv::kStops];
            for (int i = 0; i < mesh_conv::kStops; i++) stops[i] = st_mc_stops[i].exchange(0, std::memory_order_relaxed);
            std::string cut;
            for (int o = 0; o < 64; o++)
                if (const int64_t n = st_mc_stop_ops[o].exchange(0, std::memory_order_relaxed)) {
                    char b[32];
                    snprintf(b, sizeof b, " %d: %.1f;", o, n / 120.0);
                    cut += b;
                }
            logf("vk: a frame: indirect mesh draws' arguments (RX_VK_MESH_CONV=%d): %.1f converted one by one, %.1f from %.1f batches "
                 "at the game's barriers (%.1f converted in them; %.1f dropped after a write, %.1f the ring came round), %.1f for the "
                 "compute fallback (no mesh shaders in use: one by one); scans ended at a barrier %.1f, an op %.1f, the end %.1f, full %.1f; the draws "
                 "batched by a scan right after an op that ended the one before, by op:%s",
                 mesh_conv_mode(), mc[0] / 120.0, mc[1] / 120.0, mc[2] / 120.0, mc[3] / 120.0, mc[4] / 120.0, mc[5] / 120.0,
                 mc[6] / 120.0, stops[mesh_conv::kStopBarrier] / 120.0, stops[mesh_conv::kStopOp] / 120.0,
                 stops[mesh_conv::kStopEnd] / 120.0, stops[mesh_conv::kStopFull] / 120.0, cut.empty() ? " none" : cut.c_str());
            if (st_mc_frames[0] + st_mc_frames[1] + st_mc_frames[2]) {
                char b[3][64];
                for (int m = 0; m < 3; m++)
                    snprintf(b[m], sizeof b[m], "%d: %.3f ms a frame (%lld frames)", m,
                             st_mc_frames[m] ? st_gpu_mc_ns[m] / 1e6 / st_mc_frames[m] : 0.0, (long long)st_mc_frames[m]);
                logf("vk: GPU time by RX_VK_MESH_CONV in the A/B so far: %s; %s; %s", b[0], b[1], b[2]);
            }
        }
        static int64_t l_gpu;
        static uint64_t l_made;
        if (call_count_on()) {   // (RX_VK_CALL_COUNT: the replay's calls by kind, a frame and a draw)
            static uint64_t l_c[kCallKinds], l_draws;
            int64_t dr = 0, x;
            draw_stats(&dr, &x, &x, &x);
            const double draws = std::max<double>(1, (double)((uint64_t)dr - l_draws));
            std::string line;
            char b[96];
            for (int i = 0; i < kCallKinds; i++) {
                const uint64_t c = call_counts[i].load(std::memory_order_relaxed);
                if (c - l_c[i]) snprintf(b, sizeof b, " %s %.1f (%.2f a draw);", call_kind_names[i], (c - l_c[i]) / 120.0, (c - l_c[i]) / draws), line += b;
                l_c[i] = c;
            }
            l_draws = (uint64_t)dr;
            logf("vk: the replay's calls a frame (RX_VK_CALL_COUNT), %.1f draws:%s", draws / 120.0, line.c_str());
        }
        {   // (and by kind: what each brings the GPU, a whole clear for a target, a decode for ASTC)
            static uint64_t l_n[4], l_b[4];
            uint64_t n[4], b[4];
            texture_made_kinds(n, b);
            logf("vk: GPU time %.1f ms a frame (the longest submission %.1f ms); %.1f textures made a frame: colour targets "
                 "%.1f (%.1f MB), depth %.1f (%.1f MB), ASTC or BC %.1f (%.1f MB), other %.1f (%.1f MB)",
                 (st_gpu_ns - l_gpu) / 120.0 / 1e6, st_gpu_max_ns / 1e6, (texture_made() - l_made) / 120.0,
                 (n[0] - l_n[0]) / 120.0, (b[0] - l_b[0]) / 120.0 / 1048576.0, (n[1] - l_n[1]) / 120.0,
                 (b[1] - l_b[1]) / 120.0 / 1048576.0, (n[2] - l_n[2]) / 120.0, (b[2] - l_b[2]) / 120.0 / 1048576.0,
                 (n[3] - l_n[3]) / 120.0, (b[3] - l_b[3]) / 120.0 / 1048576.0);
            for (int i = 0; i < 4; i++) l_n[i] = n[i], l_b[i] = b[i];
        }
        {   // (the movie networks too slow on this card: left out from now on, vk_draw.cpp movie_networks_slow)
            static const bool keep_all = getenv("RX_VK_NETWORK_KEEP") && !strcmp(getenv("RX_VK_NETWORK_KEEP"), "all");
            const double gpu_ms = (st_gpu_ns - l_gpu) / 120.0 / 1e6, draws = (d - l_d) / 120.0;
            menu_auto_tune(gpu_ms, draws, 120.0 / secs);   // (the Quality preset Auto (this PC): measured in play)
            if (!keep_all && gpu_ms > 40.0 && draws < 40.0 && !movie_networks_slow.exchange(true)) {
                perf_event(perf_report::kMovieUpscalerDropped);
                logf("vk: %.0f ms of GPU time a frame for %.0f draws (a movie): its upscaler networks are left out from now on "
                     "(a linear upscale; RX_VK_NETWORK_KEEP=all keeps them)", gpu_ms, draws);
            }
        }
        l_made = texture_made();
        {
            static int64_t l_ub, l_un;
            int64_t ub = 0, un = 0;
            texture_unswizzle_stats(&ub, &un);
            logf("vk: a frame: %.2f MB unswizzled on the CPU in %.2f ms", (ub - l_ub) / 120.0 / 1048576.0,
                 (un - l_un) / 120.0 / 1e6);
            l_ub = ub, l_un = un;
        }
        l_gpu = st_gpu_ns, st_gpu_max_ns = 0;
        gprof_report(120);
        l_d = d, l_sk = sk, l_di = di, l_pass = passes, l_sub = st_submits, l_up = up, l_upb = upb, l_bar = st_barriers;
        l_ubu = st_ub_updates, l_ubo = st_ub_overlays, l_wait = st_wait_ns;
        fps_t = now, fps_n = 0;
    }
}

/// (longshot) Once a game frame: every 600 a status line (the video memory, DLSS's details while it draws), and every 60
/// a look at the driver's allowance, warned of once when the use passes it (again only after it was back under)
void status_tick() {
    namespace sl = status_line;
    {   // (longshot, RX_VK_TEX_AHEAD: a light frame, one of few draws, lets the next one prepare textures)
        static int64_t l_draws = 0;
        int64_t draws = 0, sk = 0, di = 0, pl = 0;
        draw_stats(&draws, &sk, &di, &pl);
        texture_ahead_frame(draws - l_draws);
        temporal_note_frame(draws - l_draws);   // (and RX_VK_DLSS_PREWARM's)
        l_draws = draws;
    }
    static uint64_t frames = 0;
    // (the slowest frame since the last line, present to present: a stall shows here, to be matched with what the log says
    // around it, such as DLSS's feature made again)
    static int64_t last_ns = 0, slowest_ns = 0;
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (last_ns) slowest_ns = std::max(slowest_ns, now_ns - last_ns);
    // (RX_VK_FRAME_TRACE=1: every frame a line of longshot_frames.csv beside the game, present to present and what the frame did,
    // so that each stutter can be matched to its cause: waits for shaders or pipelines, translation, uploads, the memory sync,
    // pipeline compiles, the GPU behind. RX_VK_FRAME_LOG has the frame times alone)
    static FILE* trace = [] {
        if (!getenv("RX_VK_FRAME_TRACE") || !atoi(getenv("RX_VK_FRAME_TRACE"))) return (FILE*)nullptr;
        // (the game folder's logs/: vk_paths.h; with HOS_PROFILE=1, frames.csv in that run's profile folder, which the host
        // chose: host/profiling.c)
        const char* dir = getenv("HOS_PROFILE_DIR");
        FILE* f = fopen(dir && *dir ? (std::string(dir) + "/frames.csv").c_str() : log_path("longshot_frames.csv").c_str(), "w");
        if (f) fprintf(f, "frame,ms,pipe_waits,pipe_wait_ms,left_out,translate_ms,uploads,upload_mb,textures,memsync_ms,pipelines_ms,"
                          "uploads_ms,programs_ms,slot_wait_ms,gpu_behind_ms,draw_ms,desc_ms,cmds_ms,tex_lookup_ms,textures_gone,"
                          "late_wait_ms,readback_wait_ms,tex_frame_ms,game_ms,render_ms,gpu_wait_ms,pace_ms,wait_render_ms,"
                          "wait_gpu_ms,wait_main_ms,main_cpu_ms,main_blocked_ms,main_vcore_ms,shown_ms,shown_at_ns,at_ns,ab_side,"
                          "create_ms,reuse_ms,astc_stage_ms,astc_record_ms,astc_view_ms,astc_flush_ms,astc_flushes,create_n,"
                          "pool_reused,pool_new,pool_put,pool_idle,pool_destroyed,pool_holds,dead_waiting,compact_pass,pool_resized\n");
        return f;
    }();
#ifdef TRACY_ENABLE
    if (last_ns) HT_PLOT("frame ms (renderer)", (now_ns - last_ns) / 1e6);
#endif
    if (trace && last_ns) {
        static int64_t l_ps, l_pw, l_up, l_upb, l_wait, l_prof[PROF_COUNT];
        static double l_pwms, l_tr;
        int64_t ps = 0, pw = 0, up = 0, astc = 0, upb = 0;
        double pwms = 0;
        draw_stats3(&ps, &pw, &pwms);
        texture_stats(&up, &astc, &upb);
        const double tr = shader_translate_ms();
        const auto dp = [](int i) { return (prof_ns[i] - l_prof[i]) / 1e6; };
        static uint64_t l_gone;
        const uint64_t gone = texture_destroy_generation();
        // (longshot: the waits no other column has: the capture's for the GPU (inside memsync_ms), a read-back's for the GPU, and
        // texture_frame as a whole (the present's eviction and sweeps; the read-back's wait is inside it when an eviction asked))
        static int64_t l_late, l_rb, l_tf;
        const int64_t late = memory_late_wait_ns(), rb = texture_readback_wait_ns(), tf = texture_frame_ns();
        // (longshot) the game's frame as the FPS counter's Breakdown splits it (vk_fps.cpp: measured while the trace is on; the
        // presenting thread's frame, which with the render thread on is the game's present before this one), and the game's main
        // thread's own time over the trace's frame (MkwVkHost main_thread_times): on the CPU, blocked in waits and sleeps, and
        // waiting for a virtual core. Its time running but off the CPU (the system's other work) is the frame less the three
        const fps_stats::Frame ff = fps_last_frame();
        // (one clock at the frame's arrival: -1 when the driver gave no present-wait confirmation, so the row says so)
        const int64_t shown_us = shown_wait_us.exchange(-1);
        const double shown_ms = shown_us < 0 ? -1 : shown_us / 1000.0;
        const int64_t shown_at = shown_at_ns.exchange(0);   // (the same confirmation, on the host clock)
        static int64_t l_mt[3];
        int64_t mt[3] = {0, 0, 0};
        if (host->main_thread_times) host->main_thread_times(mt);
        // (at_ns: this present on the host's clock, hos_now_ns: the clock of the profile's samples, presents.csv and events.txt,
        // so a frame is found in them. ab_side (longshot): the frame's side in an A/B within the run, HOS_AB (host/hos_ab.c):
        // -1 none, 0 or 1, 2 or 3 in a transition; scripts/perf.py ab)
        static int64_t l_at;
        const int64_t at = host->now_ns ? host->now_ns() : 0;
        const int ab_side = host->version >= 34 && host->ab_frame ? host->ab_frame(l_at, at) : -1;
        static int64_t l_sp[8];   // (TEST, local: the texture steps' times, vk_texture.cpp step_ns)
        int64_t sp[8];
        texture_step_times(sp);
        static int64_t l_pc[9];   // (TEST, local: the render target pool's counts, vk_texture.cpp)
        int64_t pc[9];
        {
            std::lock_guard lk(replay_mu);
            texture_pool_counts(pc);
        }
        l_at = at;
        fprintf(trace, "%llu,%.2f,%lld,%.2f,%lld,%.1f,%lld,%.2f,%zu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%llu,%.2f,%.2f,%.2f,"
                       "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%lld,%lld,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld\n",
                (unsigned long long)frames, (now_ns - last_ns) / 1e6, (long long)(pw - l_pw), pwms - l_pwms, (long long)(ps - l_ps),
                tr - l_tr, (long long)(up - l_up), (upb - l_upb) / 1048576.0, texture_count(), dp(PROF_MEMSYNC), dp(PROF_PIPE),
                dp(PROF_UPLOAD), dp(PROF_PROGRAM), dp(PROF_SLOTWAIT), (st_wait_ns - l_wait) / 1e6, dp(PROF_DRAW), dp(PROF_DESC),
                dp(PROF_CMDS), dp(PROF_TEXGET), (unsigned long long)(gone - l_gone), (late - l_late) / 1e6, (rb - l_rb) / 1e6,
                (tf - l_tf) / 1e6, ff.part[fps_stats::kGame], ff.part[fps_stats::kRender], ff.part[fps_stats::kGpuWait],
                ff.part[fps_stats::kPace], ff.part[fps_stats::kWaitRender], ff.part[fps_stats::kWaitGpu], ff.part[fps_stats::kWaitMain],
                (mt[2] - l_mt[2]) / 1e6, (mt[1] - l_mt[1]) / 1e6, (mt[0] - l_mt[0]) / 1e6, shown_ms, shown_at, (long long)at, ab_side,
                (sp[0] - l_sp[0]) / 1e6, (sp[1] - l_sp[1]) / 1e6, (sp[2] - l_sp[2]) / 1e6, (sp[3] - l_sp[3]) / 1e6, (sp[4] - l_sp[4]) / 1e6,
                (sp[5] - l_sp[5]) / 1e6, (long long)(sp[6] - l_sp[6]), (long long)(sp[7] - l_sp[7]),
                (long long)(pc[0] - l_pc[0]), (long long)(pc[1] - l_pc[1]), (long long)(pc[2] - l_pc[2]), (long long)(pc[3] - l_pc[3]),
                (long long)(pc[4] - l_pc[4]), (long long)pc[5], (long long)pc[6], (long long)pc[7], (long long)(pc[8] - l_pc[8]));
        memcpy(l_sp, sp, sizeof l_sp);
        memcpy(l_pc, pc, sizeof l_pc);
#ifdef TRACY_ENABLE
        // (LONGSHOT_TRACY: the trace's main values as plots, beside the zones)
        HT_PLOT("game_ms", ff.part[fps_stats::kGame]);
        HT_PLOT("render_ms", ff.part[fps_stats::kRender]);
        HT_PLOT("wait_gpu_ms", ff.part[fps_stats::kWaitGpu]);
        HT_PLOT("wait_render_ms", ff.part[fps_stats::kWaitRender]);
        HT_PLOT("wait_main_ms", ff.part[fps_stats::kWaitMain]);
        HT_PLOT("memsync_ms", dp(PROF_MEMSYNC));
#endif
        l_gone = gone, l_late = late, l_rb = rb, l_tf = tf;
        std::memcpy(l_mt, mt, sizeof l_mt);
        if (frames % 60 == 0) fflush(trace);
        l_ps = ps, l_pw = pw, l_pwms = pwms, l_up = up, l_upb = upb, l_tr = tr, l_wait = st_wait_ns;
        std::memcpy(l_prof, prof_ns, sizeof l_prof);
    }
    last_ns = now_ns;
    if (++frames % sl::kCheckEvery) return;
    uint64_t used = 0, allowed = 0;
    const bool known = vram_usage_now(&used, &allowed);
    static bool told_unknown = false;
    if (!known && !told_unknown) {
        told_unknown = true;
        logf("vk: status: the driver gives no video-memory budget (VK_EXT_memory_budget): only the game's own allocations are known");
    }
    char dlss[320] = "";
    const bool line = frames % sl::kEvery == 0;
    const int mode = line ? temporal_status(dlss, sizeof dlss) : temporal_mode();   // (temporal_status: since the last line, so only for one)
    static bool over = false;
    if (known && sl::crossed(over, used, allowed)) {
        if (over) perf_event(perf_report::kVramOverBudget);
        char w[200];
        sl::over_text(w, sizeof w, used, allowed, mode);
        logf("%s", w);
    }
    if (!line) return;
    char v[128];
    sl::vram_text(v, sizeof v, used, allowed, known);
    logf("vk: status: %d frames, the slowest %.1f ms; %s%s", sl::kEvery, slowest_ns / 1e6, v, dlss);
    slowest_ns = 0;
}

// (RX_VK_FRAME_LOG=<file>: one CSV line a present: frame, seconds since the first, frame time in ms. Frame pacing on the
// Steam Deck, where MangoHud's log stays empty under gamescope. Taken once the present is handed to the swapchain
// (present_out), or without a window (HOS_HEADLESS) once the picture's offscreen copy is submitted: a headless run wrote none)
void frame_log_present() {
    static FILE* fl = getenv("RX_VK_FRAME_LOG") ? std::fopen(getenv("RX_VK_FRAME_LOG"), "w") : nullptr;
    static std::chrono::steady_clock::time_point first, prev;
    static uint64_t frames;
    if (!fl) return;
    const auto t = std::chrono::steady_clock::now();
    if (!frames) first = prev = t, std::fputs("frame,time_s,frametime_ms\n", fl);
    std::fprintf(fl, "%llu,%.4f,%.3f\n", (unsigned long long)frames, std::chrono::duration<double>(t - first).count(),
                 std::chrono::duration<double, std::milli>(t - prev).count());
    prev = t;
    if (++frames % 60 == 0) std::fflush(fl);
}

// ---- (longshot) the control bridge's shots (host/control.c, docs/dev/reference/control-bridge.md): a PNG of the first present after a given one,
// asked for from another thread. Read back as RX_VK_SHOTS' are (at a later present, once the GPU reached it, and written on a
// thread of its own), with an image and a buffer of their own: the two kinds never share a readback. Never waited for: a
// present looks at the readback once its value is completed() and moves on otherwise ----
struct ShotAsk {
    std::string path;
    int64_t after = -1;
    void (*done)(void* ctx, int64_t present, int ok) = nullptr;
    void* ctx = nullptr;
};
std::mutex shot_ask_mu;
std::deque<ShotAsk> shot_asks;   // (shot_ask_mu)
struct { bool on; uint64_t value; int64_t present; ShotAsk ask; } shot_asked{};   // (replay_mu: the one being read back)
VkImage ask_image = VK_NULL_HANDLE;
VmaAllocation ask_image_alloc = nullptr, ask_buf_alloc = nullptr;
VkBuffer ask_buf = VK_NULL_HANDLE;
void* ask_map = nullptr;

int shot_ask(const char* path, int64_t after, void (*done)(void*, int64_t, int), void* ctx) {
    if (!path || !*path || !done) return 0;
    std::lock_guard lk(shot_ask_mu);
    if (shot_asks.size() >= 16) return 0;
    shot_asks.push_back({path, after, done, ctx});
    return 1;
}
bool shot_ask_init() {
    if (ask_image) return true;
    if (!shot_dimensions_ready()) return false;   // (RX_VK_SHOT_W/H out of bounds: no shots of either kind)
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation image_alloc = nullptr, buf_alloc = nullptr;
    VkBuffer buf = VK_NULL_HANDLE;
    void* map = nullptr;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {kShotW, kShotH, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!create_image(ci, image, image_alloc)) return false;
    if (!create_buffer((uint64_t)kShotW * kShotH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, buf, buf_alloc, &map)) {
        destroy_image(image, image_alloc);
        return false;
    }
    ask_image = image, ask_image_alloc = image_alloc, ask_buf = buf, ask_buf_alloc = buf_alloc, ask_map = map;
    return true;
}
/// (do_present, replay_mu) the copy of this present for the first shot asked for before it, unless one is being read back
void shot_ask_record(VkCommandBuffer cmd, VkImage src, uint32_t sw, uint32_t sh, const int crop[4], uint64_t value) {
    if (shot_asked.on || !src) return;
    ShotAsk a;
    {
        std::lock_guard lk(shot_ask_mu);
        if (shot_asks.empty() || shot_asks.front().after >= presents) return;
        a = std::move(shot_asks.front());
        shot_asks.pop_front();
    }
    if (!shot_ask_init()) {
        a.done(a.ctx, presents, 0);
        return;
    }
    image_barrier(cmd, ask_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    blit_crop(cmd, src, sw, sh, crop, ask_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kShotW, kShotH);
    image_barrier(cmd, ask_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const VkBufferImageCopy c{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {kShotW, kShotH, 1}};
    vkCmdCopyImageToBuffer(cmd, ask_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ask_buf, 1, &c);
    capture_host_read_barrier(cmd, ask_buf, (VkDeviceSize)kShotW * kShotH * 4);
    shot_asked.on = true, shot_asked.value = value, shot_asked.present = presents, shot_asked.ask = std::move(a);
}
/// (do_present, replay_mu) the shot being read back, once the GPU is past it: never waited for
void shot_ask_take() {
    if (!shot_asked.on || completed() < shot_asked.value) return;
    invalidate_mapped(ask_buf_alloc, (uint64_t)kShotW * kShotH * 4);   // (non-coherent host memory: what the GPU wrote)
    auto px = std::make_shared<std::vector<uint8_t>>((const uint8_t*)ask_map, (const uint8_t*)ask_map + (size_t)kShotW * kShotH * 4);
    ShotAsk a = std::move(shot_asked.ask);
    const int64_t at = shot_asked.present;
    shot_asked.on = false;
    std::thread([px, a, at] {
        // (through a file of its own and a rename: whoever is told the path never reads half a picture)
        const std::string part = a.path + ".part";
        bool ok = write_png(part.c_str(), px->data(), kShotW, kShotH);
        if (ok) {
            std::remove(a.path.c_str());
            ok = std::rename(part.c_str(), a.path.c_str()) == 0;
        }
        if (!ok) std::remove(part.c_str());
        logf("vk: present %lld %s %s (the control bridge)", (long long)at, ok ? "written to" : "could not be written to",
             a.path.c_str());
        a.done(a.ctx, at, ok ? 1 : 0);
    }).detach();
}
void frame_info(int64_t* replayed, int64_t* draws, int* width, int* height) {
    int64_t v[4] = {0, 0, 0, 0};
    draw_stats(&v[0], &v[1], &v[2], &v[3]);   // (counters the replay's thread writes: read as they are, a sample)
    if (replayed) *replayed = __atomic_load_n(&presents_now, __ATOMIC_RELAXED);
    if (draws) *draws = v[0];
    if (width) *width = (int)dev.swap_extent.width;
    if (height) *height = (int)dev.swap_extent.height;
}

uint64_t present_value = 0;   // (the present job's value, handed out as it came; 0: the next one)
struct PendingShot {
    bool on = false;
    uint64_t value = 0;
    int64_t presents = 0;
    std::shared_ptr<std::vector<uint8_t>> pixels;
};
std::shared_ptr<std::vector<uint8_t>> shot_pixels(uint64_t at_value, int64_t at_present) {
    wait_value(at_value);
    // The completed transfer may target non-coherent host memory.
    invalidate_mapped(shot_allocs[at_present & 1], (uint64_t)kShotW * kShotH * 4);
    const uint8_t* const sm = (const uint8_t*)shot_maps[at_present & 1];
    return std::make_shared<std::vector<uint8_t>>(sm, sm + (size_t)kShotW * kShotH * 4);
}
// The detached trial owns images that are still outside the texture cache and have no GPU submissions.
using TrialClock = std::chrono::steady_clock;
enum class OutputPath { Unavailable, FifoShown };
struct OutputTrialSource {
    uint64_t generation{};
    uint64_t serial{};
    VkImage image{};
    bool rendered_at_target{};
    uint64_t native_generation{};
};
struct OutputTrialFrame {
    uint64_t generation{};
    uint64_t swapchain_generation{};
    OutputPath path{};
    uint64_t serial{};
    bool matches_backing{};
};
struct OutputProbeRequest {
    nvn_tex targets[3]{};
    uint64_t generation{};
    TrialClock::time_point deadline{};
    char mode{};
    bool from_menu{};
};
struct OutputTrial {
    OutputProbeRequest request;
    uint64_t swapchain_generation{};
    std::unique_ptr<OutputTrialBacking> backing;
    bool preparing{};
    bool active{};
    bool reported_original{};
    bool reported_drop{};
    bool shown{};
    bool kept{};
};
static OutputTrial output_trial;
static std::mutex output_trial_mu;
static std::atomic<uint64_t> output_trial_generation;
static std::atomic<uint64_t> output_kept_generation;
static std::atomic<bool> output_preview_path;
static uint64_t output_preview_swapchain;
static std::atomic<bool> output_preview_rebuilt;
static std::atomic<uint64_t> output_preview_rebuilt_size;
static uint64_t output_trial_sequence;
static TrialClock::time_point output_preview_queued_until;
static bool output_preview_activated;
bool present_thread_on();
static bool output_trial_watchdog(uint64_t generation);

static int request_output_probe_backing(const nvn_tex* targets, char mode, bool from_menu = false) {
    if (!targets) return 0;
    std::lock_guard lk(output_trial_mu);
    if (output_trial.request.generation ||
        ((from_menu || mode == 'k') && TrialClock::now() >= output_preview_queued_until)) return 0;
    auto& request = output_trial.request;
    for (unsigned i = 0; i < 3; ++i) {
        request.targets[i] = targets[i];
        request.targets[i].pool = nullptr;
    }
    request.mode = mode;
    request.from_menu = from_menu || mode == 'k';
    request.deadline = TrialClock::now() + std::chrono::milliseconds(mode == 'x' ? 0 : 2000);
    request.generation = ++output_trial_sequence;
    output_trial_generation.store(request.generation, std::memory_order_release);
    try {
        std::thread([generation = request.generation] {
            do {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            } while (output_trial_watchdog(generation));
        }).detach();
    } catch (...) {
        output_trial = {};
        output_trial_generation.store(0, std::memory_order_release);
        return 0;
    }
    return 1;
}

static std::unique_ptr<OutputTrialBacking> clear_output_trial();

static int output_trial_phase(uint64_t serial, int cancel) {
    std::unique_ptr<OutputTrialBacking> released;
    std::lock_guard lk(output_trial_mu);
    const auto& trial = output_trial;
    if (!trial.request.generation || trial.request.targets[0].serial != serial) return OUTPUT_TRIAL_EXPIRED;
    if (cancel) {
        if (trial.kept && trial.request.from_menu) output_preview_rebuilt.store(true, std::memory_order_release);
        released = clear_output_trial();
        return OUTPUT_TRIAL_EXPIRED;
    }
    if (TrialClock::now() >= trial.request.deadline) return OUTPUT_TRIAL_EXPIRED;
    if (!trial.backing) return OUTPUT_TRIAL_PREPARING;
    return trial.shown ? OUTPUT_TRIAL_SHOWN : trial.active ? OUTPUT_TRIAL_ACTIVE : OUTPUT_TRIAL_READY;
}

// A replacement native game texture supplies the running size. Generic and generated presents cannot publish it.
static void observe_output_preview_native(const nvn_tex* t, uint64_t generation) {
    if (!t || generation || !output_preview_rebuilt.load(std::memory_order_acquire)) return;
    if (output_preview_rebuilt.exchange(false, std::memory_order_acq_rel))
        output_preview_rebuilt_size.store((uint64_t)t->d.width << 32 | t->d.height, std::memory_order_release);
}

static int output_trial_publish(uint64_t serial, void (*apply)(void*, uint64_t), void* state) {
    std::lock_guard lk(output_trial_mu);
    auto& trial = output_trial;
    if (!trial.backing || trial.active || trial.request.targets[0].serial != serial || TrialClock::now() >= trial.request.deadline)
        return 0;
    trial.active = true;
    if (trial.request.from_menu) output_preview_activated = true;
    trial.request.deadline = TrialClock::now() + std::chrono::seconds(2);
    // The guest callback only publishes already validated fields while it owns the empty presentation queue.
    apply(state, trial.request.generation);
    return 1;
}

static bool output_trial_skip_present(const nvn_tex* t, uint64_t generation) {
    if (!generation) return false;
    std::lock_guard lk(output_trial_mu);
    auto& trial = output_trial;
    if (trial.request.generation != generation || !trial.active || TrialClock::now() >= trial.request.deadline) return true;
    if (trial.request.mode != 'n') return false;
    if (!t) return true;
    for (const auto& target : trial.request.targets) {
        if (target.serial != t->serial || target.gpu != t->gpu) continue;
        if (!trial.reported_drop) {
            trial.reported_drop = true;
            logf("output trial: generation %llu suppressing native present for no-frame test", (unsigned long long)generation);
        }
        return true;
    }
    return false;
}

// Called with the trial lock held. Give the detached backing to the caller to destroy outside the lock.
static std::unique_ptr<OutputTrialBacking> clear_output_trial() {
    auto released = std::move(output_trial.backing);
    output_trial = {};
    output_trial_generation.store(0, std::memory_order_release);
    output_kept_generation.store(0, std::memory_order_release);
    return released;
}

static bool output_trial_watchdog(uint64_t generation) {
    std::unique_ptr<OutputTrialBacking> released;
    OutputProbeRequest expired;
    {
        std::lock_guard lk(output_trial_mu);
        if (output_trial.request.generation != generation || output_trial.kept) return false;
        if (TrialClock::now() < output_trial.request.deadline) return true;
        expired = output_trial.request;
        released = clear_output_trial();
    }
    const bool retained = bool(released);
    released.reset();
    logf("output trial: generation %llu serial %llu expired; retained backing %s",
         (unsigned long long)expired.generation, (unsigned long long)expired.targets[0].serial,
         retained ? "released" : "absent");
    return false;
}

static OutputTrialFrame make_output_trial_frame(OutputTrialSource source, uint64_t swapchain, OutputPath path) {
    if (!source.generation) return {};
    OutputProbeRequest request;
    std::unique_ptr<OutputTrialBacking> released;
    {
        std::lock_guard lk(output_trial_mu);
        if (output_trial.request.generation != source.generation || TrialClock::now() >= output_trial.request.deadline)
            return {};
        if (output_trial.kept) return {};
        if (path != OutputPath::FifoShown ||
            (output_trial.preparing && output_trial.swapchain_generation != swapchain)) {
            logf("output trial: generation %llu canceled; no matching FIFO shown path",
                 (unsigned long long)source.generation);
            released = clear_output_trial();
            return {};
        } else if (output_trial.backing) {
            return {source.generation, swapchain, path, source.serial,
                    source.rendered_at_target && output_trial.active && source.native_generation == source.generation &&
                    output_trial.backing->matches(source.serial, source.image)};
        } else {
            if (output_trial.preparing) return {};
            output_trial.preparing = true;
            output_trial.swapchain_generation = swapchain;
            request = output_trial.request;
        }
    }
    unsigned made = 0;
    uint64_t bytes = 0;
    auto backing = output_probe_backing(request.targets, request.mode == 'g', made, bytes);
    std::lock_guard lk(output_trial_mu);
    if (output_trial.request.generation != request.generation || TrialClock::now() >= request.deadline) return {};
    if (!backing) {
        clear_output_trial();
        logf("output trial: generation %llu serial %llu refused; images %u; bytes %llu",
             (unsigned long long)request.generation, (unsigned long long)request.targets[0].serial, made, (unsigned long long)bytes);
        return {};
    }
    logf("output trial: generation %llu serial %llu retained; images %u; bytes %llu; FIFO chain %llu",
         (unsigned long long)request.generation, (unsigned long long)request.targets[0].serial, made,
         (unsigned long long)bytes, (unsigned long long)swapchain);
    output_trial.backing = std::move(backing);
    return {source.generation, swapchain, path, source.serial, false};
}

static bool acknowledge_output_trial_frame(OutputTrialFrame frame) {
    if (!frame.generation || frame.path != OutputPath::FifoShown) return false;
    std::lock_guard lk(output_trial_mu);
    if (frame.generation != output_trial.request.generation || frame.swapchain_generation != output_trial.swapchain_generation ||
        !output_trial.backing || output_trial.shown || TrialClock::now() >= output_trial.request.deadline) return false;
    if (!frame.matches_backing || !output_trial.active) {
        if (!output_trial.reported_original) {
            output_trial.reported_original = true;
            logf("output trial: generation %llu FIFO shown rejected source serial %llu",
                 (unsigned long long)frame.generation, (unsigned long long)frame.serial);
        }
        return false;
    }
    output_trial.shown = true;
    output_trial.request.deadline = TrialClock::now() + std::chrono::seconds(10);
    logf("output trial: generation %llu FIFO shown accepted source serial %llu",
         (unsigned long long)frame.generation, (unsigned long long)frame.serial);
    return true;
}

void present_image_with_trial(VkImage src, uint32_t sw, uint32_t sh, const int crop[4], VkImageView view, bool source_srgb, bool allow_scale, OutputTrialSource trial);

void do_present(const nvn_tex* t, const int crop[4], uint64_t native_generation = 0) {
    OutputTrialSource trial;
    trial.native_generation = native_generation;
    trial.generation = output_trial_generation.load(std::memory_order_acquire);
#ifdef TRACY_ENABLE
    HT_SCOPE("do_present");
#endif
    static PendingShot pending;
    // (longshot) NVIDIA Reflex (vk_reflex.cpp): this frame's simulation ended, its submission starts; on the way out (after the
    // present) the driver's sleep until the next frame should begin. AMD Anti-Lag 2 (vk_antilag.cpp) waits there as well: the
    // next frame's input stage
    temporal_idle();   // (DLSS, FSR: their video memory let go once not used for a while)
    if (reshade::needs_tick.load(std::memory_order_relaxed)) reshade::tick();
    status_tick();   // (the status line, and the video-memory warning)
    reflex_present_begins();
    struct ReflexSleep { ~ReflexSleep() { FpsWait fw(fps_stats::kPace); reflex_sleep(); antilag_input(); } } reflex_sleep_after;
    {   // (longshot, RX_VK_PASS_TRACE_AT=<frames>: the passes of the frames around these frame numbers, for finding what a frame is made of)
        static std::atomic<int64_t> seen{0};
        static const int64_t at = getenv("RX_VK_PASS_TRACE_AT") ? atoll(getenv("RX_VK_PASS_TRACE_AT")) : 0;
        if (at && ++seen % at == 0) pass_traces_log(texture_frame_no() - 1, texture_frame_no());
    }
    static const char* shots = getenv("RX_VK_SHOTS");
    static const int shot_every = getenv("RX_VK_SHOT_EVERY") ? std::max(1, atoi(getenv("RX_VK_SHOT_EVERY"))) : 300;
    VkImage src = VK_NULL_HANDLE;
    VkImageView direct_view = VK_NULL_HANDLE;
    uint32_t sw = 0, sh = 0;
    bool source_srgb = false;
    bool direct_scale = false;
    bool shot = false, interpolated = false;
    VkImage made_src = VK_NULL_HANDLE;
    int made_index = 0;
    VkImage headless_fx = VK_NULL_HANDLE;   // CRT output at the capture pixel grid, reused by both screenshot routes
    {
        std::lock_guard lk(replay_mu);
        bench_frame(presents);
        SlowWatch slow("present");
        frame_submits.clear();
        PROF(PROF_PRESENT);
        const uint64_t value = present_value ? present_value : ++q_values;
        present_value = 0;
        rec_value_now = value;
        ub_first_of_recording = ub_updates.size();
        Recording& r = begin_recording(value);
        memory_begin_submission();
        memory_gpu_writes_begin();   // (RX_VK_UB_SNAPSHOT's guard: a submission's start, not a part's: split_part)
        // A held present may resume after its generation's texture release. Finish its bookkeeping
        // without recreating an image from that old descriptor at the reusable reservation.
        const bool dropped_output = output_trial_skip_present(t, trial.native_generation);
        Texture* tex = t && !dropped_output ? texture_for_present(t) : nullptr;
        static int logged_p;
        if (logged_p++ < 4)
            logf("vk: present texture %#llx %ux%u format %#x: %s", (unsigned long long)(t ? t->gpu : 0),
                 t ? t->d.width : 0, t ? t->d.height : 0, t ? t->d.format : 0,
                 tex ? (tex->gpu_written ? "rendered" : "never rendered") : "no image");
        if (tex) src = tex->image, sw = tex->width, sh = tex->height;
        if (t && !dropped_output) temporal_note_present(t->d.width, t->d.height);   // (RX_VK_DLSS_PREWARM: the size its feature is made for)
        if (trial.generation && t) {
            trial.serial = t->serial;
            trial.image = src;
            trial.rendered_at_target = tex && tex->gpu_written && sw == 1024 && sh == 576 &&
                                       crop && crop[0] == 0 && crop[1] == 0 && crop[2] == 1024 && crop[3] == 576;
        }
        // (the Insert menu's HDR set to Native: the game, told its display is HDR10, presents PQ codes in RGB10A2 (vk
        // A2B10G10R10); that picture goes to the HDR10 swapchain as PQ, not through the SDR-to-HDR expansion. Told by the format
        // the game presents, the one its view is made in (direct_view below), not by the cached image's: images are shared by
        // format class (vk_texture.cpp format_class), and were RGBA8 and RGB10A2 to share one the window's RGB10A2 picture
        // would be a view of an RGBA8 image and take the SDR expansion, a flat, grey picture. The image's format is not asked as well:
        // an image first made as RGB10A2 under an RGBA8 picture would take an SDR picture for PQ)
        const VkFormat shown_format = t ? vk_format(t->d.format).format : VK_FORMAT_UNDEFINED;
        present_native_pq.store(tex && dev.hdr && opts.hdr == 2 && shown_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                std::memory_order_relaxed);
        {   // (the presented picture's format, the game's and its image's, and the HDR path it takes, said when any of them
            // changes: what an HDR report needs)
            static int64_t said = -1;
            const int64_t now =
                tex ? ((int64_t)tex->format * 4096 + (int64_t)shown_format) * 8 + (dev.hdr ? 4 : 0) + opts.hdr : -2;
            if (now != said) {
                said = now;
                logf("vk: present: %ux%u, format %d (its image's %d); the Insert menu's HDR %d, HDR10 swapchain %s: %s",
                     tex ? tex->width : 0, tex ? tex->height : 0, (int)shown_format, tex ? (int)tex->format : 0, opts.hdr,
                     dev.hdr ? "yes" : "no",
                     !dev.hdr ? "SDR" : present_native_pq.load(std::memory_order_relaxed) ? "the game's PQ picture (Native)"
                                                                                        : "the SDR picture expanded to HDR10");
            }
        }
        direct_scale = tex && !dev.offscreen &&
                       (opts.upscale || opts.sharpness || opts.crt_filter || dev.hdr || wide_sides_possible());
        if (tex && !dev.offscreen && (direct_scale || colour_vision::enabled(menu_colour_vision_mode()))) {
            // Keep the sampled view with the same cached image as the direct present, before advancing its age.
            VkFormat sampled_format = VK_FORMAT_UNDEFINED;
            direct_view = texture_view(tex, t, nullptr, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Sample, &sampled_format);
            source_srgb = sampled_format == VK_FORMAT_R8G8B8A8_SRGB || sampled_format == VK_FORMAT_B8G8R8A8_SRGB ||
                          sampled_format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
            tex->last_frame = texture_frame_no();
            tex->last_value = std::max(tex->last_value, value);
        }
        // (frame interpolation: made frames and this one shown by the display thread, vk_framegen.cpp. Recorded
        // after the submit that signals the present's value, in a submit of its own: the game waits for that value
        // before it submits its next frame, and the interpolation's GPU work (~1 ms) held it back. The display
        // thread's present follows it on the one queue. RX_VK_SHOT_MADE shots need it in this recording.)
        static const bool fg_inline = env_on("RX_VK_SHOT_MADE");
        const bool preview = trial.native_generation &&
                             trial.native_generation != output_kept_generation.load(std::memory_order_acquire);
        const bool fg_after = !preview && framegen_on() && tex && !fg_inline;
        interpolated = !preview && framegen_on() && tex && fg_inline &&
                       framegen_record(r.cmd, texture_view(tex, t, nullptr, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Sample), sw,
                                       sh, crop, present_native_pq.load(std::memory_order_relaxed));
        texture_frame(completed());
        full_barrier(r.cmd);
        presents++;
        presents_now = presents;
        rec::note_frame();   // (RX_VK_RECORDER_PARTS: the recorder's stats line, a frame)
        new_refuse_test(presents);
        if (ab_running()) st_mc_frames[std::clamp(mesh_conv_mode(), 0, 2)]++;   // (RX_VK_MESH_CONV's A/B: the frame's side)
        wide_sides_tick();
        if (presents == 1) hang_watch_start(), gpu_hang_watch_start();
        {   // (EvilBoyce, diagnostics) RX_VK_WATCH_CPU=<present>:<gpu address>[,...] (hex addresses): from that present on, the
            // CPU's writes to those 4 KB pages logged with the writer's stack (host page watch: 12 reports)
            static bool armed = false;
            static const char* e = getenv("RX_VK_WATCH_CPU");
            if (e && !armed && host->watch_guest_writes && presents >= atoll(e)) {
                armed = true;
                for (const char* q = strchr(e, ':'); q && *q;) {
                    char* end;
                    const uint64_t a = strtoull(q + 1, &end, 16);
                    if (end == q + 1) break;
                    if (const uint64_t g = host->gpu_guest(a)) host->watch_guest_writes(g & ~4095ull, 4096);
                    q = *end ? end : nullptr;
                }
            }
        }
        ts_anchor_ns = host->ticks() * 32;
        dump_present(presents);
        if (src && (dev.offscreen || shots) && offscreen_init()) {
            if (dev.offscreen) {
                image_barrier(r.cmd, off_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                // No swapchain exists here. Exercise the same output effects on every frame, at the
                // requested capture size, not only when a screenshot is due. Off keeps the original blit.
                if (opts.crt_filter && tex)
                    headless_fx = framegen_scale(r.cmd, texture_view(tex, t, nullptr, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Sample),
                                                 src, sw, sh, crop, kShotW, kShotH,
                                                 present_native_pq.load(std::memory_order_relaxed));
                if (headless_fx)
                    blit_crop(r.cmd, headless_fx, kShotW, kShotH, nullptr, off_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              dev.swap_extent.width, dev.swap_extent.height);
                else
                    blit_crop(r.cmd, src, sw, sh, crop, off_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              dev.swap_extent.width, dev.swap_extent.height);
            }
            // (RX_VK_SHOT_FROM / _TO: every present in that range too)
            static const int64_t shot_from = getenv("RX_VK_SHOT_FROM") ? atoll(getenv("RX_VK_SHOT_FROM")) : -1;
            static const int64_t shot_to = getenv("RX_VK_SHOT_TO") ? atoll(getenv("RX_VK_SHOT_TO")) : -1;
            shot = shots && (presents % shot_every == 0 || (presents >= shot_from && presents <= shot_to));
            // (RX_VK_SHOT_GO=<file>: every present while that file is there too, a test's run of frames)
            static const char* shot_go = getenv("RX_VK_SHOT_GO");
            if (shots && shot_go && !shot) {
                struct stat sb;
                shot = stat(shot_go, &sb) == 0;
            }
            // (RX_VK_SHOT_MENU=1, a debug aid: without a window the Insert menu is neither ticked nor drawn; this ticks it
            // at each present and draws it into the shots, for pictures of it from a headless run)
            static const bool shot_menu = env_on("RX_VK_SHOT_MENU");
            if (shot_menu && dev.offscreen) menu_tick();
            // (RX_VK_SHOT_MADE=1: the frame interpolation made before it too, written beside it as vk_N_made.png)
            static const bool shot_made = env_on("RX_VK_SHOT_MADE");
            uint32_t mw = 0, mh = 0;
            made_src = shot && shot_made && interpolated ? framegen_made_image(&mw, &mh, &made_index) : VK_NULL_HANDLE;
            if (made_src && !made_shot_init()) made_src = VK_NULL_HANDLE;
            if (made_src) {
                image_barrier(r.cmd, made_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                blit_crop(r.cmd, made_src, mw, mh, nullptr, made_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kShotW, kShotH);
                image_barrier(r.cmd, made_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                const VkBufferImageCopy c{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {kShotW, kShotH, 1}};
                vkCmdCopyImageToBuffer(r.cmd, made_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, made_buf, 1, &c);
                capture_host_read_barrier(r.cmd, made_buf, (VkDeviceSize)kShotW * kShotH * 4);
            }
            if (shot) {
                // A pending shot may be two (or more) presents old. Own its pixels before this slot is written again.
                if (pending.on && (pending.presents & 1) == (presents & 1) && !pending.pixels)
                    pending.pixels = shot_pixels(pending.value, pending.presents);
                image_barrier(r.cmd, shot_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                // (RX_VK_SHOT_FX=1: through the window's scale pass too, ultrawide's menu sides as the window shows them. For headless
                // runs: in a window it resizes the pass's image to the shot's size and the present back again, two device waits a
                // shot, and it is not under present_image_mu, which frame generation's display thread takes for the same pass)
                static const bool shot_fx = env_on("RX_VK_SHOT_FX");
                // (pq as the window's present has it: Native HDR's picture through the 10-bit pass, not a format switch each shot)
                const VkImage fx_img = headless_fx ? headless_fx : shot_fx && tex ? framegen_scale(r.cmd, texture_view(tex, t, nullptr, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Sample),
                                                                       src, sw, sh, crop, kShotW, kShotH,
                                                                       present_native_pq.load(std::memory_order_relaxed))
                                                        : VK_NULL_HANDLE;
                if (fx_img) full_barrier(r.cmd), blit_crop(r.cmd, fx_img, kShotW, kShotH, nullptr, shot_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kShotW, kShotH);
                else blit_crop(r.cmd, src, sw, sh, crop, shot_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kShotW, kShotH);
                if (shot_menu && dev.offscreen) menu_record_image(r.cmd, shot_image, shot_view, kShotW, kShotH);   // (headless: the menu over the shot)
                image_barrier(r.cmd, shot_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                const VkBufferImageCopy c{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {kShotW, kShotH, 1}};
                vkCmdCopyImageToBuffer(r.cmd, shot_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot_bufs[presents & 1], 1, &c);
                capture_host_read_barrier(r.cmd, shot_bufs[presents & 1], (VkDeviceSize)kShotW * kShotH * 4);
            }
        }
        shot_ask_record(r.cmd, headless_fx ? headless_fx : src, headless_fx ? kShotW : sw, headless_fx ? kShotH : sh,
                        headless_fx ? nullptr : crop, value);   // the bridge captures the same CRT output, without Longshot overlays
        memory_end_submission(r.cmd, r.part);
        full_barrier(r.cmd);
        submit_recording(r);   // (nothing is stopped at a present: drain)
        if (pushed.load() < value) pushed = value;
        done_max = std::max(done_max, value), signaled = std::max(signaled, value);
        shot_ask_take();   // (an earlier present's, when the GPU is past it)
        if (fg_after) {
            // (its slot is free again once the next submit's value is reached: that one comes after it)
            Recording& g = begin_recording(value + 1);
            interpolated = framegen_record(g.cmd, texture_view(tex, t, nullptr, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Sample),
                                           sw, sh, crop, present_native_pq.load(std::memory_order_relaxed));
            submit_recording(g, false);
        }
        // (frame generation at Off or Auto, where DLSS-G is not asked: its feature, if made, let go, vk_dlss.cpp)
        if (swapchain_framegen() < 2) dlss_fg_idle();
        if (interpolated) {   // (NVIDIA Reflex: the game frame's rendering is submitted; the display thread presents it later)
            reflex_game_submitted();
            framegen_queue(value);
        }
        else menu_note_frame(true);   // (the FPS counter's, as frame generation's thread counts what it shows)
        // (and the built-in interpolation's and FSR 3.1's images, off for a while: after the queue, so a set this present recorded
        // is the display thread's to show and free before any drain waits for it; and counted as on, whatever the row says now)
        framegen_idle(interpolated);
        {   // (RX_VK_WAIT_GPU=<frames>: the replay no further ahead of the GPU than that)
            static uint64_t values[8];
            static const int ahead = [] {
                const char* e = getenv("RX_VK_WAIT_GPU");
                const long count = e ? std::strtol(e, nullptr, 10) : 0;
                constexpr long capacity = sizeof(values) / sizeof(values[0]);
                if (count > capacity) logf("vk: RX_VK_WAIT_GPU=%ld exceeds the ring capacity; using %ld", count, capacity);
                return (int)std::clamp<long>(count, 0, capacity);
            }();
            if (ahead > 0) {
                const uint64_t old = values[presents % ahead];
                values[presents % ahead] = value;
                if (old) wait_value(old);
            }
        }
        if (presents % 300 == 0) pipeline_cache_save(false);
        {   // (RX_VK_DUMP_KEY: F9 dumps the next frame, 8 times at most)
            static const bool dump_key = env_on("RX_VK_DUMP_KEY");
            static bool was_down;
            static int key_left = 8;
            const bool down = dump_key && (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
            if (down && !was_down && key_left > 0) {
                key_left--;
                static const int n = getenv("RX_VK_DUMP_KEY_N") ? std::max(1, atoi(getenv("RX_VK_DUMP_KEY_N"))) : 1;
                logf("vk: F9 at present %lld: the next %d frame(s) are dumped", (long long)presents, n);
                dump_request(presents + 1, n);
            }
            was_down = down;
        }
        {   // (RX_VK_DUMP_FILE=<path>: that file appearing dumps the next frame, and is deleted: a test that steers the
            // game live (HOS_INPUT_LIVE) dumps where it stands)
            // (RX_VK_DUMP_MOD=<m>,<r>[,<n>]: the file arms the next n frames (default 2) whose present is r mod m
            // instead: a glitch that comes back every m frames, dumped in its own frame (the flash detector names it))
            static const char* dump_file = getenv("RX_VK_DUMP_FILE");
            static int mod_m = -1, mod_r = 0, mod_n = 2, mod_left = 0;
            if (mod_m < 0) {
                mod_m = 0;
                if (const char* e = getenv("RX_VK_DUMP_MOD")) sscanf(e, "%d,%d,%d", &mod_m, &mod_r, &mod_n);
            }
            if (dump_file && presents % 10 == 0 && GetFileAttributesA(dump_file) != INVALID_FILE_ATTRIBUTES) {
                DeleteFileA(dump_file);
                if (mod_m > 0) {
                    mod_left = std::max(1, mod_n);
                    logf("vk: dump file at present %lld: the next %d frames at %d mod %d are dumped", (long long)presents,
                         mod_left, mod_r, mod_m);
                } else {
                    logf("vk: dump file at present %lld: the next frame is dumped", (long long)presents);
                    dump_request(presents + 1, 1);
                }
            }
            if (mod_left > 0 && (presents + 2) % mod_m == mod_r) {   // (armed at the present before the frame's own)
                mod_left--;
                dump_request(presents + 1, 1);
            }
        }
        // (a shot is read once the GPU reached its frame, at a later present, and written on a thread of its own:
        // waiting for the GPU and encoding the PNG there took the replay ~570 ms a shot, and frequent shots
        // halved the frame rate test runs measured. RX_VK_SHOT_MADE shots are taken at once, as before.)
        auto take = [&](uint64_t at_value, int64_t at_present) {
            auto px = pending.on && pending.presents == at_present && pending.pixels
                          ? std::move(pending.pixels) : shot_pixels(at_value, at_present);
            // (RX_VK_DUMP_BLACK=<n>: a shot with 12-90% pure black pixels in a lit picture, or a green cast over it,
            // dumps the next frame, n times at most: the glitches some scenes show now and then)
            static int black_left = getenv("RX_VK_DUMP_BLACK") ? atoi(getenv("RX_VK_DUMP_BLACK")) : 0;
            if (black_left > 0) {
                const uint8_t* q0 = px->data();
                uint64_t black = 0, white = 0, sum = 0, ch[3] = {};
                const uint64_t n = (uint64_t)kShotW * kShotH;
                for (uint64_t i = 0; i < n; i++) {
                    const uint8_t* q = q0 + i * 4;
                    black += q[0] < 3 && q[1] < 3 && q[2] < 3;
                    white += q[0] > 235 && q[1] > 235 && q[2] > 235;
                    sum += q[0] + q[1] + q[2];
                    ch[0] += q[0], ch[1] += q[1], ch[2] += q[2];
                }
                // (longshot: a picture blown out to white, the HUD on it: written as white_<present>.png, 40 of them)
                static int white_left = 40;
                if (at_present > 600 && white * 100 > n * 40 && white_left > 0) {
                    white_left--;
                    logf("vk: present %lld: %.1f%% white pixels, mean %llu %llu %llu", (long long)at_present, 100.0 * white / n,
                         (unsigned long long)(ch[0] / n), (unsigned long long)(ch[1] / n), (unsigned long long)(ch[2] / n));
                    auto w0 = px;
                    const std::string name = std::string(shots) + "/white_" + std::to_string(at_present) + ".png";
                    std::thread([w0, name] { write_png(name.c_str(), w0->data(), kShotW, kShotH); }).detach();
                }
                const bool green = ch[1] * 10 > ch[0] * 12 && ch[1] * 10 > ch[2] * 14 && ch[1] / n > 120;
                if (at_present > 600 && ((black * 8 > n && black * 100 < n * 97 && sum / (n * 3) > 4) || green)) {
                    black_left--;
                    logf("vk: present %lld: %.1f%% black pixels, mean %llu %llu %llu: the next frame is dumped",
                         (long long)at_present, 100.0 * black / n, (unsigned long long)(ch[0] / n),
                         (unsigned long long)(ch[1] / n), (unsigned long long)(ch[2] / n));
                    dump_request(presents + 1);
                }
            }
            // (RX_VK_FLASH=1, with RX_VK_SHOT_EVERY=1: a frame that differs from both its neighbours in a part of the
            // picture where they agree is a one-frame flash: the three are written as flash_<present>_{a,b,c}.png and
            // the next frame is dumped (a few times). The other shots are written every 300th present only.)
            static const bool flash = getenv("RX_VK_FLASH") && atoi(getenv("RX_VK_FLASH")) != 0;
            if (flash) {
                constexpr uint32_t W = 64, H = 36;
                auto small = std::make_shared<std::vector<uint8_t>>(W * H * 3);
                for (uint32_t y = 0; y < H; y++) {
                    const uint32_t y0 = (uint64_t)y * kShotH / H;
                    const uint32_t y1 = std::max(y0 + 1, (uint32_t)((uint64_t)(y + 1) * kShotH / H));
                    for (uint32_t x = 0; x < W; x++) {
                        const uint32_t x0 = (uint64_t)x * kShotW / W;
                        const uint32_t x1 = std::max(x0 + 1, (uint32_t)((uint64_t)(x + 1) * kShotW / W));
                        for (int ch = 0; ch < 3; ch++) {
                            uint64_t sum = 0, samples = 0;
                            // Partition the full image and count the samples actually visited. A partial
                            // stride must not brighten the cell or wrap its mean back through uint8.
                            for (uint32_t yy = y0; yy < y1; yy += 4)
                                for (uint32_t xx = x0; xx < x1; xx += 4) {
                                    sum += (*px)[((size_t)yy * kShotW + xx) * 4 + ch];
                                    samples++;
                                }
                            (*small)[(y * W + x) * 3 + ch] = (uint8_t)(sum / samples);
                        }
                    }
                }
                static std::shared_ptr<std::vector<uint8_t>> sa, sb, pa, pb;
                static int64_t present_b = -1;
                static int flashes_left = 400, dumps_left = getenv("RX_VK_FLASH_DUMPS") ? atoi(getenv("RX_VK_FLASH_DUMPS")) : 6;
                if (sa && sb && present_b == at_present - 1) {
                    // (RX_VK_FLASH_DIFF, RX_VK_FLASH_CELLS: a subtler flash; logged with how many cells went up, and where)
                    static const int fl_diff = getenv("RX_VK_FLASH_DIFF") ? atoi(getenv("RX_VK_FLASH_DIFF")) : 48;
                    static const int fl_cells = getenv("RX_VK_FLASH_CELLS") ? atoi(getenv("RX_VK_FLASH_CELLS")) : 40;
                    int hits = 0, up = 0, minx = W, maxx = -1, miny = H, maxy = -1, peak = 0;
                    uint64_t tab = 0, tac = 0;   // (camera motion: a and c differ as much as either from b; a flash: a ~ c)
                    for (uint32_t i = 0; i < W * H * 3; i++)
                        tab += std::abs((*sa)[i] - (*sb)[i]), tac += std::abs((*sa)[i] - (*small)[i]);
                    for (uint32_t i = 0; i < W * H; i++) {
                        int dab = 0, dbc = 0, dac = 0;
                        for (int ch = 0; ch < 3; ch++) {
                            dab = std::max(dab, std::abs((*sa)[i * 3 + ch] - (*sb)[i * 3 + ch]));
                            dbc = std::max(dbc, std::abs((*sb)[i * 3 + ch] - (*small)[i * 3 + ch]));
                            dac = std::max(dac, std::abs((*sa)[i * 3 + ch] - (*small)[i * 3 + ch]));
                        }
                        if (dab > fl_diff && dbc > fl_diff && dac < fl_diff / 3) {
                            hits++;
                            up += (*sb)[i * 3 + 1] > (*sa)[i * 3 + 1];
                            peak = std::max(peak, dab);
                            const int cx = (int)(i % W), cy = (int)(i / W);
                            minx = std::min(minx, cx), maxx = std::max(maxx, cx);
                            miny = std::min(miny, cy), maxy = std::max(maxy, cy);
                        }
                    }
                    // (RX_VK_FLASH_REGION=<level>: also while the picture moves, when the cells' test cannot hold: a 16x9 block of
                    // the picture whose mean brightness jumps by the level and comes back the next frame)
                    static const int fl_region = getenv("RX_VK_FLASH_REGION") ? atoi(getenv("RX_VK_FLASH_REGION")) : 0;
                    int region_hits = 0, region_peak = 0, region_at = -1;
                    if (fl_region)
                        for (uint32_t ry = 0; ry < 9; ry++)
                            for (uint32_t rx = 0; rx < 16; rx++) {
                                int m[3] = {0, 0, 0};
                                for (uint32_t y = ry * 4; y < ry * 4 + 4; y++)
                                    for (uint32_t x = rx * 4; x < rx * 4 + 4; x++) {
                                        const size_t o = (y * W + x) * 3 + 1;
                                        m[0] += (*sa)[o], m[1] += (*sb)[o], m[2] += (*small)[o];
                                    }
                                const int ab = (m[1] - m[0]) / 16, cb = (m[1] - m[2]) / 16, ac = std::abs(m[0] - m[2]) / 16;
                                if (((ab > fl_region && cb > fl_region) || (ab < -fl_region && cb < -fl_region)) &&
                                    ac * 2 < std::min(std::abs(ab), std::abs(cb))) {
                                    region_hits++;
                                    if (std::abs(ab) > std::abs(region_peak)) region_peak = ab, region_at = (int)(ry * 16 + rx);
                                }
                            }
                    if (region_hits && flashes_left > 0)
                        logf("vk: region flash at present %lld: %d regions, peak %+d at region %d,%d", (long long)present_b, region_hits,
                             region_peak, region_at % 16, region_at / 16);
                    if (((hits >= fl_cells && tac * 2 < tab) || region_hits) && flashes_left > 0) {
                        flashes_left--;
                        logf("vk: flash at present %lld: %d of %u cells differ from both neighbours, %d brighter, peak %d, "
                             "cells %d,%d-%d,%d (now present %lld, texture frame %llu)", (long long)present_b, hits, W * H, up,
                             peak, minx, miny, maxx, maxy, (long long)presents, (unsigned long long)texture_frame_no());
                        const std::string base = std::string(shots) + "/flash_" + std::to_string(present_b);
                        auto a0 = pa, b0 = pb, c0 = px;
                        std::thread([a0, b0, c0, base] {
                            write_png((base + "_a.png").c_str(), a0->data(), kShotW, kShotH);
                            write_png((base + "_b.png").c_str(), b0->data(), kShotW, kShotH);
                            write_png((base + "_c.png").c_str(), c0->data(), kShotW, kShotH);
                        }).detach();
                        if (dumps_left > 0) dumps_left--, dump_request(presents + 1);
                        static int records_left = 12;   // (RX_VK_FLASH_RECORD: the draws around it)
                        const uint64_t tf = (uint64_t)(present_b + (int64_t)texture_frame_no() - presents);
                        probe_log(tf);
                        if (records_left > 0) records_left--, draw_records_log(tf - 1, tf + 1), pass_traces_log(tf - 1, tf + 1);
                    }
                }
                sa = sb, pa = pb, sb = small, pb = px, present_b = at_present;
                if (at_present % 300) return;
            }
            std::string path = std::string(shots) + "/vk_" + std::to_string(1000000 + at_present).substr(1) + ".png";
            std::thread([px, path, at_present] {
                if (write_png(path.c_str(), px->data(), kShotW, kShotH))
                    logf("vk: present %lld written to %s", (long long)at_present, path.c_str());
            }).detach();
        };
        if (pending.on && (shot || completed() >= pending.value)) take(pending.value, pending.presents), pending.on = false;
        if (shot && made_src && made_map) {
            take(value, presents);
            invalidate_mapped(made_buf_alloc, (uint64_t)kShotW * kShotH * 4);
            char path[1024];
            if (made_index < 0) snprintf(path, sizeof path, "%s/vk_%06lld_made.png", shots, (long long)presents);
            else snprintf(path, sizeof path, "%s/vk_%06lld_made%d.png", shots, (long long)presents, made_index);
            write_png(path, (const uint8_t*)made_map, kShotW, kShotH);
        } else if (shot) {
            pending = {true, value, presents, {}};
        }
        log_frame_stats();
    }
    if (output_trial_skip_present(t, trial.native_generation)) return;
    if (src) observe_output_preview_native(t, native_generation);
    if (dev.offscreen) frame_log_present();   // (no window: the copy into the offscreen image above is its present)
    const bool pending_preview = trial.generation &&
                                 trial.generation != output_kept_generation.load(std::memory_order_acquire);
    if (dev.offscreen || interpolated || (pending_preview && framegen_on())) {
        if (pending_preview) make_output_trial_frame(trial, 0, OutputPath::Unavailable);
        if (dev.offscreen || interpolated) return;
        trial = {};
    }
    present_image_with_trial(src, sw, sh, crop, direct_view, source_srgb, direct_scale, trial);
}

} // namespace

bool output_preview_request(uint32_t width, uint32_t height) {
    std::lock_guard settings(menu_mu);
    if (opts.hdr || opts.reflex || opts.dlss || framegen_on()) return false;
    if (host->version < 41 || !host->output_preview_request || !output_preview_path.load(std::memory_order_acquire)) return false;
    std::lock_guard lk(output_trial_mu);
    if (output_trial.request.generation || TrialClock::now() < output_preview_queued_until) return false;
    if (!host->output_preview_request(width, height)) return false;
    output_preview_activated = false;
    output_preview_queued_until = TrialClock::now() + std::chrono::seconds(2);
    return true;
}

// 0: staging unavailable; -1: waiting; -2: activated or shown and ended; positive: seconds left to decide.
int output_preview_seconds() {
    std::lock_guard lk(output_trial_mu);
    const auto now = TrialClock::now();
    const auto& trial = output_trial;
    if (trial.request.generation && trial.request.from_menu) {
        if (trial.kept || now >= trial.request.deadline) return output_preview_activated ? -2 : 0;
        return trial.shown ? (int)std::chrono::ceil<std::chrono::seconds>(trial.request.deadline - now).count() : -1;
    }
    return output_preview_activated ? -2 : now < output_preview_queued_until ? -1 : 0;
}

bool output_preview_answer(bool keep) {
    std::unique_ptr<OutputTrialBacking> released;
    std::lock_guard lk(output_trial_mu);
    auto& trial = output_trial;
    if (!keep) {
        output_preview_queued_until = {};
        output_preview_activated = false;
        if (trial.request.generation && trial.request.from_menu) released = clear_output_trial();
        return true;
    }
    if (!trial.shown || !trial.request.from_menu || trial.swapchain_generation != output_preview_swapchain ||
        TrialClock::now() >= trial.request.deadline) return false;
    trial.kept = true;
    output_kept_generation.store(trial.request.generation, std::memory_order_release);
    trial.request.deadline = TrialClock::time_point::max();
    logf("output trial: generation %llu kept", (unsigned long long)trial.request.generation);
    return true;
}

// The presentation thread applies the running size during its normal menu tick.
uint64_t output_preview_take_rebuilt_size() {
    if (!output_preview_rebuilt_size.load(std::memory_order_acquire)) return 0;
    return output_preview_rebuilt_size.exchange(0, std::memory_order_acq_rel);
}

void take_output_trial_image(const nvn_tex* t, const VkImageCreateInfo& shape, VkImage& image, VmaAllocation& allocation) {
    if (!t->output_preview || !output_trial_generation.load(std::memory_order_acquire)) return;
    std::lock_guard lk(output_trial_mu);
    if (!output_trial.active || !output_trial.backing || TrialClock::now() >= output_trial.request.deadline) return;
    for (auto& owned : output_trial.backing->images) {
        // A guest draw can reuse a recorded descriptor from the prior preview. Its exclusive GPU address
        // and exact image shape still identify the backing. Only this preview's native serial can acknowledge it.
        if (!owned.allocation || owned.gpu != t->gpu) continue;
        const auto& preflight = owned.shape;
        if (shape.flags != preflight.flags || shape.imageType != preflight.imageType || shape.format != preflight.format ||
            shape.extent.width != preflight.extent.width || shape.extent.height != preflight.extent.height ||
            shape.extent.depth != preflight.extent.depth || shape.mipLevels != preflight.mipLevels ||
            shape.arrayLayers != preflight.arrayLayers || shape.samples != preflight.samples || shape.tiling != preflight.tiling ||
            shape.usage != preflight.usage || shape.initialLayout != preflight.initialLayout ||
            shape.sharingMode != preflight.sharingMode || shape.pNext || preflight.pNext) return;
        image = owned.image;
        allocation = owned.allocation;
        owned.allocation = {};
        vkDestroyImageView(dev.device, owned.view, nullptr);
        owned.view = {};
        logf("output trial: generation %llu adopted retained image serial %llu",
             (unsigned long long)output_trial.request.generation, (unsigned long long)owned.serial);
        return;
    }
}

int shot_ask_next(const char* path, void (*done)(void* ctx, int64_t present, int ok), void* ctx) { return shot_ask(path, -1, done, ctx); }

// (longshot) RX_VK_FRAMELOG=<file>: every frame shown, the game's and the made ones, as they go to the screen, 320x180, while
// <file>.go exists (scripts/flash.py framelog finds the flashes). Taken at the last step and written on a thread of its own, so
// that the game keeps its speed: a shot of every frame at 1280x720 held it at 30 fps, where the flashes may not show.
// A record: u64 time (ns), u32 sequence, u32 kind (0 the game's, 1 made), 320x180 RGBA.
void exposure_log_dump(const char* why);   // (vk_draw.cpp)
void probe_dump_all(const char* why);
namespace {
constexpr uint32_t kLogW = 320, kLogH = 180;
const char* framelog_path() {
    static const char* p = getenv("RX_VK_FRAMELOG");
    return p;
}
struct LogSlot {
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    void* map = nullptr;
    bool used = false;
    int64_t at = 0;
    uint32_t seq = 0, kind = 0;
};
LogSlot log_slots[kPresentSlotsMax];   // (by present_image's frame slot)
VkImage log_image = VK_NULL_HANDLE;
VmaAllocation log_image_alloc = nullptr;
std::mutex log_mu;
std::condition_variable log_cv;
std::deque<std::vector<uint8_t>> log_queue;
std::atomic<int> log_state{0};   // 0: not tried; 1: complete sink; -1: disabled after a failure
void framelog_resources_drop() {
    for (LogSlot& l : log_slots) {
        if (l.buf) destroy_buffer(l.buf, l.alloc);
        l = LogSlot{};
    }
    if (log_image) destroy_image(log_image, log_image_alloc);
    log_image = VK_NULL_HANDLE;
    log_image_alloc = nullptr;
}
void framelog_write(FILE* f) {
    SetThreadDescription(GetCurrentThread(), L"mkw_vk frame log");
    for (;;) {
        std::vector<uint8_t> rec;
        {
            std::unique_lock lk(log_mu);
            log_cv.wait(lk, [] { return !log_queue.empty(); });
            rec = std::move(log_queue.front());
            log_queue.pop_front();
        }
        const size_t written = fwrite(rec.data(), 1, rec.size(), f);
        if (written != rec.size() || fflush(f) != 0) {
            {
                std::lock_guard lk(log_mu);
                log_state.store(-1, std::memory_order_release);
                log_queue.clear();   // (no worker remains to consume these; don't retain the queued CPU pictures)
            }
            logf("vk: frame log stopped: %s failed (wrote %zu of %zu bytes)", written == rec.size() ? "flush" : "write",
                 written, rec.size());
            fclose(f);
            return;
        }
    }
}
void framelog_enqueue(std::vector<uint8_t> rec) {
    std::lock_guard lk(log_mu);
    if (log_state.load(std::memory_order_acquire) > 0 && log_queue.size() < 512) {
        log_queue.push_back(std::move(rec));
        log_cv.notify_one();
    }
}
bool framelog_ready() {
    const int state = log_state.load(std::memory_order_acquire);
    if (state) return state > 0;
    log_state.store(-1, std::memory_order_release);
    if (!framelog_path()) return false;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {kLogW, kLogH, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!create_image(ci, log_image, log_image_alloc)) {
        framelog_resources_drop();
        logf("vk: frame log disabled: the readback image could not be made");
        return false;
    }
    for (LogSlot& l : log_slots) {
        if (!create_buffer_cached((uint64_t)kLogW * kLogH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, l.buf, l.alloc, &l.map) || !l.map) {
            framelog_resources_drop();
            logf("vk: frame log disabled: the complete mapped readback set could not be made");
            return false;
        }
    }
    FILE* f = fopen(framelog_path(), "wb");
    if (!f) {
        framelog_resources_drop();
        logf("vk: frame log disabled: cannot open %s", framelog_path());
        return false;
    }
    // Other presenters are serialized by present_image_mu. Publish before the worker can report a failure.
    log_state.store(1, std::memory_order_release);
    try {
        std::thread([f] { framelog_write(f); }).detach();
    } catch (const std::exception& e) {
        log_state.store(-1, std::memory_order_release);
        fclose(f);
        framelog_resources_drop();
        logf("vk: frame log disabled: writer thread could not start (%s)", e.what());
        return false;
    }
    logf("vk: the frame log writes every frame shown to %s", framelog_path());
    return true;
}
bool framelog_on() {
    static bool on = false;
    static int64_t next = 0;
    if (!framelog_path()) return false;
    const int64_t now = prof_now();
    if (now >= next) {
        next = now + 250000000;
        const std::string go = std::string(framelog_path()) + ".go";
        on = GetFileAttributesA(go.c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    return on && framelog_ready();
}
void framelog_collect(int slot) {
    LogSlot& l = log_slots[slot];
    if (!l.used) return;
    l.used = false;
    // present_image waited for this slot's fence; expose its GPU writes before the CPU copy.
    invalidate_mapped(l.alloc, (uint64_t)kLogW * kLogH * 4);
    std::vector<uint8_t> rec(16 + (size_t)kLogW * kLogH * 4);
    std::memcpy(rec.data(), &l.at, 8);
    std::memcpy(rec.data() + 8, &l.seq, 4);
    std::memcpy(rec.data() + 12, &l.kind, 4);
    std::memcpy(rec.data() + 16, l.map, (size_t)kLogW * kLogH * 4);
    {   // (a picture blown out to white: what the exposure was)
        const uint8_t* q = rec.data() + 16;
        uint64_t sum = 0;
        const size_t n = (size_t)kLogW * kLogH;
        for (size_t i = 0; i < n; i += 7) sum += q[i * 4 + 1];
        const uint64_t mean = sum / (n / 7 + 1);
        static uint64_t last_mean;
        if (mean > 225 && last_mean < 215 && l.kind == 0) {
            logf("vk: white frame: frame log sequence %u, present %lld, brightness %llu after %llu", l.seq, (long long)presents,
                 (unsigned long long)mean, (unsigned long long)last_mean);
            char line[700];
            int n2 = snprintf(line, sizeof line, "vk: white detail: textures made by present (now %lld):", (long long)made_ring_present);
            for (int k = 30; k >= 0 && n2 < 660; k--) {
                const int64_t at = made_ring_present - k;
                const uint64_t a = made_ring[(at - 1) & 31], b = made_ring[at & 31];
                if (b > a) n2 += snprintf(line + n2, sizeof line - n2, " -%d:%llu", k, (unsigned long long)(b - a));
            }
            logf("%s", line);
            exposure_log_dump("white frame");
            probe_dump_all("white frame");
        }
        if (l.kind == 0 && mean * 2 < last_mean && last_mean > 40) {   // (a picture gone dark for a frame, the HUD on it)
            static int dark_left = 6;
            if (dark_left > 0) {
                dark_left--;
                logf("vk: dark frame: frame log sequence %u, present %lld, brightness %llu after %llu", l.seq, (long long)presents,
                     (unsigned long long)mean, (unsigned long long)last_mean);
                char name[32];
                snprintf(name, sizeof name, "cond_%d.txt", dark_left);
                if (FILE* o = fopen(name, "w")) {
                    for (uint32_t i = 0; i < (1 << 15); i++) {
                        const CondNote& c = cond_ring[(cond_ring_at + i) % (1 << 15)];
                        if (c.addr)
                            fprintf(o, "%lld %#llx %d %llu %llu %d%c", (long long)c.present, (unsigned long long)c.addr, c.mode,
                                    (unsigned long long)c.x, (unsigned long long)c.y, c.on ? 1 : 0, 10);
                    }
                    fclose(o);
                }
                exposure_log_dump("dark frame");
                probe_dump_all("dark frame");
                pass_traces_log(texture_frame_no() - 7, texture_frame_no());
            }
        }
        if (l.kind == 0) last_mean = mean;
    }
    static const bool drop = env_on("RX_VK_FRAMELOG_DROP");
    if (drop) return;
    framelog_enqueue(std::move(rec));
}
} // namespace
std::atomic<uint32_t> present_kind{0};   // (vk_framegen.cpp: 1 while a made frame goes out)

// (longshot) One thread in here at a time; the queue's own lock (dev.present_mu, the replay's submits take it too) is held only
// for the submit and the present. Held over vkAcquireNextImageKHR and the fence, it stopped the replay's submits for as long as
// the swapchain had no image free, twice a frame with frame interpolation.
std::mutex present_image_mu;
static bool present_out(uint32_t index, std::unique_lock<decltype(dev.present_mu)>& qk, OutputTrialSource trial = {}, bool preview_path = false);   // (present_image's end, below)
thread_local PendingPresent* present_defer_into = nullptr;
static VkFence present_copy_done = VK_NULL_HANDLE;   // (present_image's copy, while it presents: the presenter thread may take it)
static bool present_acquire_ahead = false;   // (present_image's present, while it presents: the presenter asks for the next image after it)
namespace {
static bool acquire_ahead_wanted();   // (acquire ahead: every driver, RX_VK_ACQUIRE_AHEAD; after present_image)
static void acquire_ahead(VkSwapchainKHR sc, uint64_t generation);
bool present_thread_on();
void present_hand_off(uint32_t index, VkFence done, uint64_t reflex_id, OutputTrialFrame trial);
void present_finish(VkResult res, bool on_presenter);
std::atomic<bool> presenter_surface_lost{false};   // (the presenter thread's present: VK_ERROR_SURFACE_LOST_KHR, for present_image)
}  // namespace
// doc: docs/dev/reference/architecture.md#frame-generations-presents (rule 4: a frame slot per frame of both sets)
/// (longshot) The frame slots present_image uses while frame generation runs, for the sets a swapchain is made for
/// (vk_main.cpp create_swapchain: framegen is swapchain_framegen's multiplier, 0 none; two_sets while RX_VK_FG_TIMELINE is
/// on, in every present mode, but for a set too big for the chain under FIFO). Each present waits for the copy last made in its slot, so the slots are
/// also how many presents can be on the GPU at once: 3, and a slot per frame of two sets where the next set is taken while
/// one is shown (2 x the multiplier, at most kPresentSlotsMax). A set presented ahead (vk_framegen.cpp display_main) submits
/// its copies at once: with fewer slots than its frames its 4th copy would wait in its own first copy's slot, and with a slot
/// per frame of one set the next set's copy k would wait in the slot of the shown set's copy k, each until the GPU had done
/// that copy and so the game's work queued before it, holding the frames of the set still being shown.
/// One set's (RX_VK_FG_TIMELINE=0; under FIFO a set too big for the chain) keeps 3
int present_slots_for(int framegen, bool two_sets) {
    return two_sets && framegen >= 2 ? std::min(2 * framegen, kPresentSlotsMax) : 3;
}

// (longshot) The swapchain's image is waited for on the CPU, never on the replay's queue. A copy into it waiting on the
// GPU for the acquire's semaphore, on the queue every submission of the game's work goes to, lets whatever holds the image
// hold the replay. AMD's driver on Windows hands an image back from vkAcquireNextImageKHR at once and leaves the wait to the
// semaphore; with the game's fullscreen window behind another one (a browser, OBS) the system gives the image back late or
// never, the queue finishes nothing, and the GPU hang watch ends the game. So the acquire signals
// acquire_fence; a copy goes to the queue only once the image is free; an image not free in kImageFreeNs is held (frames are
// not shown meanwhile, the game goes on) and looked at again, without waiting, at each present until it is
static VkFence acquire_fence = VK_NULL_HANDLE;
static VkSemaphore acquire_semaphore = VK_NULL_HANDLE;
struct RetiredAcquire { VkFence fence; VkSemaphore semaphore; };
static std::vector<RetiredAcquire> retired_acquires;
struct HeldImage {
    bool held = false;
    uint32_t index = 0;
    uint64_t generation = 0;   // (dev.swapchain_generation it was acquired from)
    // (acquire ahead: acquired on the presenter thread right after the present before, acquire_ahead, not at a present that
    // found it busy. Its present waits for it up to kImageFreeNs, as for an image acquired then; only then is it held as above)
    bool ahead = false;
};
static HeldImage held_image;
/// Whether the next image is asked for right after each present: device_policy.h's acquire_ahead (every driver), with
/// RX_VK_ACQUIRE_AHEAD over it (0 off, anything else on; read once by device_policy_env). Which way it went
/// is logged at the first present
static bool acquire_ahead_on() {
    static const bool on = [] {
        const bool v = dev.policy.acquire_ahead;
        if (dev.policy.set(Sw::acquire_ahead))
            logf("vk: acquire ahead: %s, set by RX_VK_ACQUIRE_AHEAD",
                 v ? "on: the next image asked for right after each present" : "off: each image asked for at its present (#1458's order)");
        else if (v)
            logf("vk: acquire ahead: on by default: the next image asked for right after each present "
                 "(RX_VK_ACQUIRE_AHEAD=0: off)");
        else
            logf("vk: acquire ahead: off by default with this driver: each image asked for at its present, #1458's order "
                 "(RX_VK_ACQUIRE_AHEAD=1: on)");
        return v;
    }();
    return on;
}
/// The image-wait counter below: only with RX_VK_ACQUIRE_AHEAD set to on, for A/B runs (a player's log gets no line every
/// 600 presents, and the default costs nothing)
static bool acquire_ahead_counted() {
    static const bool v = acquire_ahead_on() && dev.policy.set(Sw::acquire_ahead);
    return v;
}
// (RX_VK_ACQUIRE_AHEAD set to on only: what the presents waited for the window's image, the acquire and its fence, said every
// 600 presents; and what became of the images asked for a present ahead. present_image_mu)
struct ImageWaits {
    int64_t ns = 0;
    uint32_t presents = 0, ahead = 0, ahead_free = 0, ahead_waited = 0, ahead_dropped = 0, ahead_not_given = 0;
};
static ImageWaits image_waits;
static void image_waits_note(int64_t ns) {
    image_waits.ns += ns;
    if (++image_waits.presents < 600) return;
    const ImageWaits& w = image_waits;
    if (w.ahead || w.ahead_not_given || w.ahead_dropped)
        logf("vk: the window's image (600 presents): waited for %.2f ms a present (its acquire and its fence); asked for a "
             "present ahead (RX_VK_ACQUIRE_AHEAD=1) %u times: %u free at their present, %u waited for there, %u let go with "
             "their swapchain; %u times none given at once", w.ns / 1e6 / w.presents, w.ahead, w.ahead_free, w.ahead_waited,
             w.ahead_dropped, w.ahead_not_given);
    else
        logf("vk: the window's image (600 presents): waited for %.2f ms a present (its acquire and its fence)", w.ns / 1e6 / w.presents);
    image_waits = {};
}
// (250 ms: past a refresh of a 24 Hz display for each image FIFO may hold queued, at frame generation's most; far under the GPU
// hang watch's 30 s)
constexpr uint64_t kImageFreeNs = 250000000ull;
// (longshot, #1482) An image held this long is taken as never coming back, and the swapchain is made again: then twice as
// long, and at most 4 s, between the remakes that brought none back. With frame generation on an RX 9070 XT (Windows, full
// screen) an image held after Alt+Tab was not given back once the game was in front again either, until F11 made the
// swapchain again: the game and its sound went on, its picture stayed as it was. The system lets an image go when another
// is shown, and while one is held nothing else is presented, so nothing ever let it go. A new swapchain's images are all
// free; while the window stays hidden its first image is held again, and the remakes are kept far apart
constexpr int64_t kHeldRemakeNs = 1000000000ll;
/// Whether an image held for held_ns, after `remakes` swapchains made again for holds that brought no image back, is let go
/// with its swapchain
static bool held_remake_due(int64_t held_ns, int remakes) {
    return held_ns >= kHeldRemakeNs << (remakes < 2 ? remakes : 2);
}
/// Whether the acquired image is free (acquire_fence signaled, and reset for the next acquire), waiting up to timeout_ns
static bool image_free(uint64_t timeout_ns) {
    if (!acquire_fence) return false;   // (none could be made again: held_image_drop)
    VkResult r;
    {
        const gpu_wait_stats::Exclude pacing;   // (the display's pace, not the GPU's work: not counted as a GPU wait)
        r = vkWaitForFences(dev.device, 1, &acquire_fence, VK_TRUE, timeout_ns);
    }
    if (r == VK_ERROR_DEVICE_LOST) device_lost();
    if (r != VK_SUCCESS) return false;   // (VK_TIMEOUT: not yet)
    if (vkResetFences(dev.device, 1, &acquire_fence) == VK_ERROR_DEVICE_LOST) device_lost();
    return true;
}
// (fences held_image_drop let go unsignaled: the presentation engine may still signal them, so they are neither reset nor
// destroyed then. acquire_fence_ready uses one again once it is signaled, before it makes a new one; at most 8 are kept, past
// that one is left as it is. present_image's thread, or the presenter's while it owns the swapchain: acquire_ahead)
static std::vector<VkFence> fences_let_go;
/// A held image of a swapchain made again since: let go. Its fence may never be signaled now; then another takes its place
/// (acquire_fence_ready: one let go before and signaled since, else a new one), and this one is kept in fences_let_go. With
/// none, no acquire until there is one
static void held_image_drop() {
    if (held_image.ahead) image_waits.ahead_dropped++;
    held_image.held = false, held_image.ahead = false;
    if (acquire_semaphore) {
        retired_acquires.push_back({acquire_fence, acquire_semaphore});
        acquire_fence = VK_NULL_HANDLE;
        acquire_semaphore = VK_NULL_HANDLE;
        return;
    }
    if (image_free(0)) return;
    if (fences_let_go.size() < 8) fences_let_go.push_back(acquire_fence);
    acquire_fence = VK_NULL_HANDLE;
}
/// Whether there is an unsignaled acquire fence to acquire with: when held_image_drop let the last one go, one it let go before
/// and the engine has signaled since (reset here), else a new one
static bool acquire_fence_ready() {
#ifdef _WIN32
    // Streamline's NVIDIA acquire hook forwards a signal semaphore even for a fence-only acquire.
    if (dev.props.vendorID == 0x10de && !acquire_semaphore) {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vkCreateSemaphore(dev.device, &si, nullptr, &acquire_semaphore) != VK_SUCCESS) return false;
    }
#endif
    if (acquire_fence) return true;
    for (auto& retired : retired_acquires) {
        if (!retired.fence) continue;
        if (vkWaitForFences(dev.device, 1, &retired.fence, VK_TRUE, 0) != VK_SUCCESS) continue;
        if (vkResetFences(dev.device, 1, &retired.fence) != VK_SUCCESS) continue;
        acquire_fence = retired.fence;
        retired.fence = VK_NULL_HANDLE;   // (the fence signaled; the semaphore still needs its own wait)
        return true;
    }
    for (auto it = fences_let_go.begin(); it != fences_let_go.end(); ++it) {
        VkFence was = *it;
        VkResult r;
        {
            const gpu_wait_stats::Exclude pacing;   // (a look, no wait: not counted as a GPU wait)
            r = vkWaitForFences(dev.device, 1, &was, VK_TRUE, 0);
        }
        if (r != VK_SUCCESS) continue;   // (not signaled yet; a lost device: the next wait that meets it says so)
        if (vkResetFences(dev.device, 1, &was) != VK_SUCCESS) continue;
        fences_let_go.erase(it);
        acquire_fence = was;
        return true;
    }
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence made = VK_NULL_HANDLE;
    if (vkCreateFence(dev.device, &fi, nullptr, &made) != VK_SUCCESS) return false;
    acquire_fence = made;
    return true;
}

// A retired swapchain may never release its held image. Only consume the old signal after the CPU fence is ready,
// and destroy the semaphore once the consuming copy's fence says its wait has completed.
static VkSemaphore take_retired_acquire() {
    if (retired_acquires.empty()) return VK_NULL_HANDLE;
    // Probe at most one old acquire per copy, including when the window system never signals some of them.
    static size_t next = 0;
    next %= retired_acquires.size();
    const auto it = retired_acquires.begin() + next;
    if (it->fence) {
        const VkResult ready = vkWaitForFences(dev.device, 1, &it->fence, VK_TRUE, 0);
        if (ready == VK_ERROR_DEVICE_LOST) device_lost();
        if (ready != VK_SUCCESS) { ++next; return VK_NULL_HANDLE; }
        if (fences_let_go.size() < 8) fences_let_go.push_back(it->fence);
        else vkDestroyFence(dev.device, it->fence, nullptr);
    }
    const VkSemaphore semaphore = it->semaphore;
    retired_acquires.erase(it);
    return semaphore;
}

void present_image(VkImage src, uint32_t sw, uint32_t sh, const int crop[4], VkImageView view, bool source_srgb, bool allow_scale) {
    present_image_with_trial(src, sw, sh, crop, view, source_srgb, allow_scale, {});
}

namespace {
void present_image_with_trial(VkImage src, uint32_t sw, uint32_t sh, const int crop[4], VkImageView view, bool source_srgb, bool allow_scale, OutputTrialSource trial) {
#ifdef TRACY_ENABLE
    HT_SCOPE("present_image");
#endif
    std::lock_guard lk(present_image_mu);
    if (dev.closing.load(std::memory_order_acquire)) return;   // (the close retired the swapchain on purpose: making one again would take the window from under the compositor's final frames)
    static int hdr_was = -1;   // (the Insert menu's HDR as the swapchain was made for it; -1: not yet looked at)
    const bool hdr_changed = hdr_was >= 0 && hdr_was != opts.hdr;   // (turned on or off: the swapchain's format)
    if (hdr_was < 0) hdr_was = opts.hdr;
    static int vsync_was = -1;   // (the V-Sync row as the swapchain was made for it: its present mode)
    // (and frame generation's multiplier, swapchain_framegen: a spare image a frame; and swapchain_dlssg: FIFO under V-Sync
    // Auto with DLSS Frame Generation making the frames. It turns to no once, when NGX's answer at the first present is a no
    // or fewer frames than the multiplier asks; and past 4x the multiplier itself: under FIFO a set too big for the chain's
    // spare images takes one set's images, create_swapchain. The multiplier as it runs, framegen_now: DLSS FG's top with a
    // saved value past it)
    const int framegen = framegen_now();
    const int vsync_now = menu_vsync() * 16 + swapchain_framegen() * 2 + (swapchain_dlssg() ? 1 : 0) +
                          (framegen > 4 ? framegen * 256 : 0);
    const bool vsync_changed = vsync_was >= 0 && vsync_was != vsync_now;
    if (vsync_was < 0) vsync_was = vsync_now;
    if (presenter_surface_lost.exchange(false, std::memory_order_acquire)) {   // (the presenter's present found the surface lost)
        output_preview_path.store(false, std::memory_order_release);
        present_thread_drain();   // (before the queue's lock: the presenter takes it)
        std::lock_guard qk(dev.present_mu);
        invalidate_lost_surface(VK_ERROR_SURFACE_LOST_KHR);
        return;
    }
    const uint64_t want = dev.swap_resize.exchange(0, std::memory_order_acquire);
    if (want || !dev.swapchain || reflex_swapchain_stale() || hdr_changed || vsync_changed) {   // (resized, gone, Reflex, HDR or V-Sync changed)
        present_thread_drain();   // (before the queue's lock: the presenter takes it)
        std::lock_guard qk(dev.present_mu);
        const uint32_t width = want ? (uint32_t)(want >> 32) : dev.swap_extent.width;
        const uint32_t height = want ? (uint32_t)want : dev.swap_extent.height;
        if (!create_swapchain(width, height)) {
            output_preview_path.store(false, std::memory_order_release);
            // Retry without replacing a newer resize published while recreation was in progress.
            uint64_t empty = 0;
            dev.swap_resize.compare_exchange_strong(empty, ((uint64_t)width << 32) | height,
                                                   std::memory_order_release, std::memory_order_relaxed);
            return;   // (the old swapchain may already be retired; do not acquire or reset a frame fence)
        }
        hdr_was = opts.hdr, vsync_was = vsync_now;
    }
    // Resize or surface recovery can replace the chain after the display thread chose present-ahead.
    // A deferred pair needs two spare images before either one can be presented.
    if (present_defer_into && dev.swap_spare.load(std::memory_order_relaxed) < 2) return;
    if (src) {   // (the Window scaling filter's row: whether the window is the picture's size. Only the game's frames and the
                 // made ones: the start-up screen (no src) is drawn at the window's own size, which made the row read "Not
                 // needed" while the game's output was another size; until the first frame the size is not known: needed)
        const bool cropped = crop && crop[2] > 0 && crop[3] > 0;
        menu_note_present_sizes(cropped ? (uint32_t)crop[2] : sw, cropped ? (uint32_t)crop[3] : sh, dev.swap_extent.width,
                                dev.swap_extent.height);
    }
    // Ordered render jobs retire the preview after the copy has been submitted. On the game thread
    // retirement and the window copy could be handled by different threads.
    bool preview_path;
    {
        std::lock_guard lk(menu_mu);
        preview_path = async_on() && dev.swapchain && dev.present_wait && dev.swap_mode == VK_PRESENT_MODE_FIFO_KHR &&
                       !dev.hdr && !opts.hdr && !opts.reflex && !opts.dlss && !framegen_on() && !present_defer_into;
        output_preview_path.store(preview_path, std::memory_order_release);
        {
            std::unique_ptr<OutputTrialBacking> released;
            std::lock_guard gate(output_trial_mu);
            output_preview_swapchain = dev.swapchain_generation;
            if (output_trial.request.generation && !output_trial.kept &&
                (!preview_path || ((output_trial.preparing || output_trial.backing) &&
                                   dev.swapchain_generation != output_trial.swapchain_generation)))
                released = clear_output_trial();
        }
        menu_tick();
    }
    if (trial.native_generation) {
        std::unique_ptr<OutputTrialBacking> released;
        std::lock_guard gate(output_trial_mu);
        if (output_trial.request.generation != trial.native_generation || TrialClock::now() >= output_trial.request.deadline) return;
        if (!output_trial.kept && (!preview_path || dev.swapchain_generation != output_trial.swapchain_generation)) {
            released = clear_output_trial();
            return;
        }
        // Cancellation stops new acquisitions. Once acquired, finish the copy and release the WSI image
        // on the admitted path, even if the lease expires during the copy. Its shown receipt will be ignored.
        // The next original frame drains the handed-off present before it acquires its own swapchain image.
    }
    // (the slots in turn: the swapchain's count while frame generation runs (create_swapchain: present_slots_for), else 3; a
    // slot past them waits with its fence signalled. Where the count went down, the frame log's pictures in the slots past it
    // are dropped (RX_VK_FRAMELOG, at most kPresentSlotsMax - 3 of them): read only once the count came back up, they went
    // in late and out of order)
    const uint32_t slots = framegen_on() ? std::clamp<uint32_t>(dev.swap_slots.load(std::memory_order_relaxed), 3u,
                                                                 (uint32_t)kPresentSlotsMax) : 3u;
    static uint32_t slots_was = 3;
    for (uint32_t k = slots; k < slots_was; k++) log_slots[k].used = false;
    slots_was = slots;
    const uint32_t slot = frame_index % slots;
    Frame& f = frames[slot];
    const int log_slot = (int)slot;
    frame_index = (slot + 1) % slots;
    // (the presenter thread, RX_VK_PRESENT_THREAD: the present handed over before this one is made first. The swapchain is one
    // thread's at a time: vkAcquireNextImageKHR below ran beside the presenter's vkQueuePresentKHR and vkWaitForPresentKHR of
    // the frame before, which waited for it only at this frame's hand-over, after the acquire. One present out at most, as
    // before; with the presenter off or idle, nothing)
    present_thread_drain();
    const int64_t fence_t0 = fps_breakdown_on() ? fps_clock() : 0;   // (the FPS counter's Breakdown: this frame slot's last copy)
    const VkResult copy_ready = vkWaitForFences(dev.device, 1, &f.done, VK_TRUE, UINT64_MAX);
    if (copy_ready == VK_ERROR_DEVICE_LOST) device_lost();
    if (copy_ready != VK_SUCCESS) return;
    if (f.retired_acquire) {
        vkDestroySemaphore(dev.device, f.retired_acquire, nullptr);
        f.retired_acquire = VK_NULL_HANDLE;
    }
    framelog_collect(log_slot);
    if (output_trial_skip_present(nullptr, trial.native_generation)) return;
    uint32_t index = 0;
    const int64_t acquire_t0 = fence_t0 ? fps_clock() : 0;   // (then the swapchain's image: the display's pace)
    if (fence_t0) fps_wait(fps_stats::kGpuWait, acquire_t0 - fence_t0);
#ifdef _WIN32
    const uint64_t acquire_timeout = UINT64_MAX;
#else
    // (linux: a Wayland compositor gives no image back while the window cannot be seen: its screen asleep, another workspace. A wait
    // without end there held the presentation thread, and with it the game, until the screen came back. 100 ms, then the window is
    // taken as not shown: frames are not waited for and not shown, and the game goes on at its speed, as on Windows, until an image
    // comes back)
    static bool unseen = false;
    const uint64_t acquire_timeout = unseen ? 0 : 100000000ull;
#endif
    if (held_image.held && held_image.generation != dev.swapchain_generation) held_image_drop();   // (its swapchain is gone)
    const bool was_held = held_image.held;
    const bool was_ahead = was_held && held_image.ahead;   // (acquire ahead: asked for on the presenter right after the present before)
    if (!was_held && !acquire_fence_ready()) return;   // (no fence to acquire with: the frame is not shown)
    if (was_held) index = held_image.index;   // (acquired at an earlier present, not free then: no other acquire meanwhile)
    VkResult res = VK_SUCCESS;
    const bool image_count = acquire_ahead_counted();   // (image_waits: RX_VK_ACQUIRE_AHEAD set only, the default costs nothing)
    const int64_t image_t0 = image_count ? fps_clock() : 0;
    if (!was_held) res = vkAcquireNextImageKHR(dev.device, dev.swapchain, acquire_timeout, acquire_semaphore, acquire_fence, &index);
    if (res == VK_ERROR_DEVICE_LOST) device_lost();
    if (acquire_t0) fps_wait(fps_stats::kPace, fps_clock() - acquire_t0);
    if (res == VK_ERROR_SURFACE_LOST_KHR) {
        present_thread_drain();   // (before the queue's lock: the presenter takes it)
        std::lock_guard qk(dev.present_mu);
        invalidate_lost_surface(res);
        return;
    }
#ifndef _WIN32
    if (res == VK_TIMEOUT || res == VK_NOT_READY) {
        if (image_count) image_waits_note(fps_clock() - image_t0);   // (the 100 ms waited, though no frame is shown)
        if (!unseen) logf("vk: the window is not shown (no image back in 100 ms): frames are not shown until it is");
        unseen = true;
        return;
    }
    if (unseen && (res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR)) {
        unseen = false;
        logf("vk: the window is shown again");
    }
#endif
    if (res == VK_ERROR_OUT_OF_DATE_KHR) {
        present_thread_drain();
        std::lock_guard qk(dev.present_mu);
        create_swapchain(dev.swap_extent.width, dev.swap_extent.height);
        return;
    }
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) return;
    {   // (#1376: the image free before any work for it goes to the queue; a held one is looked at without waiting, and one
        // asked for a present ahead is waited for as one acquired now, once it is not free at once)
        const int64_t free_t0 = fence_t0 ? fps_clock() : 0;
        bool ready;
        if (was_ahead) {
            ready = image_free(0);
            if (ready) image_waits.ahead_free++;
            else image_waits.ahead_waited++, ready = image_free(kImageFreeNs);
        } else {
            ready = image_free(was_held ? 0 : kImageFreeNs);
        }
        if (free_t0) fps_wait(fps_stats::kPace, fps_clock() - free_t0);
        if (image_count) image_waits_note(fps_clock() - image_t0);
        static int said;     // (the first 16 holds are said, and their ends)
        static bool told;    // (this hold was said)
        static int64_t held_since;   // (#1482: when this hold began, fps_clock)
        static int remakes, said_remakes;   // (swapchains made again for holds since an image last came back; the first 16 said)
        if (!ready) {
            const int64_t now = fps_clock();
            if (!was_held || was_ahead) {
                held_since = now;
                told = said < 16;
                if (told) said++, logf("vk: the window's image was not given back in 250 ms (the window hidden?): frames are not "
                                       "shown until it is");
            }
            held_image = {true, index, dev.swapchain_generation, false};
            if (held_remake_due(now - held_since, remakes)) {   // (#1482: never given back; a new swapchain, as F11 makes)
                if (said_remakes < 16)
                    said_remakes++, logf("vk: the window's image was not given back in %.0f s: the swapchain made again",
                                         (now - held_since) / 1e9);
                if (remakes < 1000) remakes++;
                present_thread_drain();   // (before the queue's lock: the presenter takes it)
                std::lock_guard qk(dev.present_mu);
                create_swapchain(dev.swap_extent.width, dev.swap_extent.height);   // (the held image let go at the next present)
                held_since = now;   // (not made, a window with no size: tried again only after the next wait, not at each present)
            }
            return;
        }
        held_image.held = false, held_image.ahead = false;
        remakes = 0;
        if (was_held && told) told = false, logf("vk: the window's image was given back: frames are shown again");
    }
    if (vkResetFences(dev.device, 1, &f.done) == VK_ERROR_DEVICE_LOST) device_lost();
    if (vkResetCommandPool(dev.device, f.pool, 0) == VK_ERROR_DEVICE_LOST) device_lost();
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(f.cmd, &bi) == VK_ERROR_DEVICE_LOST) device_lost();
    VkImage dst = dev.swap_images[index];
    full_barrier(f.cmd);   // (after the game's work for the frame: earlier submits on this queue)
    image_barrier(f.cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const PresentAspect aspect{src && host && host->aspect_ratio ? host->aspect_ratio() : 0.0f};
    const PresentRect rect = present_rect(sw, sh, crop, dev.swap_extent.width, dev.swap_extent.height, aspect);
    [[maybe_unused]] uint32_t trace_route = 0; // direct; 1 scaled, 2 clear, 3 HDR conversion
    const bool has_bars = rect.x || rect.y || rect.w != dev.swap_extent.width || rect.h != dev.swap_extent.height;
    if (has_bars) {
        const VkClearColorValue black{0.0f, 0.0f, 0.0f, 1.0f};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(f.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        // The following blit overlaps the clear in the content rectangle.
        image_barrier(f.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    }
    // (Native HDR: scaled in 10 bits (framegen_scale's pq), and read by the HDR pass in every case, through the view in the
    // format the game presents (do_present): at 100% contrast it writes the game's PQ codes as they are. Never blitted from its
    // image: a blit converts from the image's own format, not the picture's)
    const bool native_pq = present_native_pq.load(std::memory_order_relaxed);
    const VkImage scaled = allow_scale && src && view ? framegen_scale(f.cmd, view, src, sw, sh, crop, rect.w, rect.h, native_pq) : VK_NULL_HANDLE;
    // (longshot) the Insert menu's HDR: the picture (scaled, or the game's own with its crop) to HDR10, copied in as the SDR one is
    const int colour = menu_colour_vision_mode();
    const bool correct_sdr = src && !dev.hdr && colour_vision::enabled(colour);
    const VkImage hdr_image = dev.hdr && src && view
        ? (scaled ? framegen_hdr(f.cmd, framegen_scaled_view(), rect.w, rect.h, nullptr, rect.w, rect.h, native_pq, colour)
                  : framegen_hdr(f.cmd, view, sw, sh, crop, rect.w, rect.h, native_pq, colour))
        : VK_NULL_HANDLE;
    const bool corrected = correct_sdr && index < dev.swap_views.size() && colour_vision_sdr(
        f.cmd, scaled ? framegen_scaled_view() : view, scaled ? rect.w : sw, scaled ? rect.h : sh,
        scaled ? nullptr : crop, dst, dev.swap_views[index], rect, colour, !scaled && source_srgb);
    if (corrected) {
        trace_route = 4;
    } else if (hdr_image) {
        trace_route = 3;
        full_barrier(f.cmd);
        blit_crop_to_rect(f.cmd, hdr_image, rect.w, rect.h, nullptr, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, rect);
    } else if (scaled) {
        trace_route = 1;
        blit_crop_to_rect(f.cmd, scaled, rect.w, rect.h, nullptr, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, rect);
    } else if (src && !native_pq) {
        blit_crop_to_rect(f.cmd, src, sw, sh, crop, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, rect);
    } else {
        if (src) {   // (Native, unscaled, and the HDR pass not made: black, said once, rather than the picture converted)
            static bool told;
            if (!told) told = true, logf("vk: HDR: Native's picture could not go through the HDR pass: shown black");
        }
        trace_route = 2;
        const VkClearColorValue black{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(f.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    }
    if (src && framelog_on()) {
        static uint32_t seq;
        LogSlot& l = log_slots[log_slot];
        image_barrier(f.cmd, log_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        blit_crop(f.cmd, src, sw, sh, crop, log_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kLogW, kLogH);
        image_barrier(f.cmd, log_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        const VkBufferImageCopy c{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {kLogW, kLogH, 1}};
        vkCmdCopyImageToBuffer(f.cmd, log_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, l.buf, 1, &c);
        capture_host_read_barrier(f.cmd, l.buf, (VkDeviceSize)kLogW * kLogH * 4);
        l.used = true, l.at = host->now_ns(), l.seq = seq++, l.kind = present_kind.load();
    }
    // Geometry trace: after choosing the actual presentation route, without frame-log pixel readback.
    static const bool trace_geometry = getenv("RX_VK_PRESENT_LOG") && atoi(getenv("RX_VK_PRESENT_LOG")) != 0;
    if (trace_geometry) {
        static PresentGeometryTrace trace;
        PresentGeometrySample sample;
        const PresentContentSize content = present_content_size(sw, sh, crop);
        sample.source = {sw, sh};
        sample.content = {content.w, content.h};
        sample.output = {dev.swap_extent.width, dev.swap_extent.height};
        sample.destination = {rect.x, rect.y, rect.w, rect.h};
        sample.crop_supplied = crop != nullptr;
        if (crop) std::copy_n(crop, 4, sample.crop.begin());
        sample.sampled_view = view != VK_NULL_HANDLE;
        sample.host_aspect(src && host && host->aspect_ratio, aspect.ratio);
        sample.kind = src ? std::min(present_kind.load(), 1u) : 2;
        sample.route = trace_route;
        sample.filter = opts.upscale;
        sample.sharpness = opts.sharpness;
        sample.framegen = opts.framegen;
        const PresentTraceDecision decision = trace.note(sample);
        if (decision == PresentTraceDecision::Emit) {
            const char* kinds[] = {"game", "made", "startup"};
            const char* routes[] = {"direct", "scaled", "clear", "hdr", "colour vision"};
            const char* aspects[] = {"unavailable", "invalid", "valid"};
            logf("vk: present geometry: kind=%s source=%ux%u crop=%s:%d,%d %dx%d content=%ux%u output=%ux%u "
                 "destination=%u,%u %ux%u host-aspect=%s:%.9g route=%s view=%d filter-request=%d sharpness-request=%d framegen-request=%d",
                 kinds[sample.kind], sw, sh, crop ? "supplied" : "none", sample.crop[0], sample.crop[1], sample.crop[2], sample.crop[3],
                 content.w, content.h, sample.output[0], sample.output[1], rect.x, rect.y, rect.w, rect.h,
                 aspects[sample.aspect_state], sample.aspect, routes[sample.route], int(sample.sampled_view), sample.filter,
                 sample.sharpness, sample.framegen);
        } else if (decision == PresentTraceDecision::Limit) {
            logf("vk: present geometry: limit of %u changes reached; further geometry messages suppressed", PresentGeometryTrace::kLimit);
        }
    }
    const bool applied_colour = corrected || (hdr_image && colour_vision::enabled(colour));
    static int last_colour = 0;
    if (src && applied_colour && colour != last_colour)
        logf("vk: colour vision mode %d applied in %s", colour, dev.hdr ? "HDR" : "SDR");
    last_colour = applied_colour ? colour : 0;
    if (src && colour_vision::enabled(colour) && !applied_colour) {
        static bool warned;
        if (!warned) warned = true, logf("vk: colour vision correction unavailable for this presentation route");
    }
    menu_record(f.cmd, index, dev.swap_extent.width, dev.swap_extent.height, applied_colour ? colour : 0);   // (longshot) Dear ImGui's overlays, over all of it
    menu_rumble_tick();   // (longshot: the Rumble strength row's own rumble, vk_menu.cpp)
    image_barrier(f.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    // Allocate the trial backing before submitting this copy, so allocation cannot hold up its presenter hand-off.
    if (trial.generation && preview_path && present_thread_on())
        make_output_trial_frame(trial, dev.swapchain_generation, OutputPath::FifoShown);
    if (vkEndCommandBuffer(f.cmd) == VK_ERROR_DEVICE_LOST) device_lost();
    // The CPU fence already proved the image is free. Consume the optional binary signal on this copy.
    // The previous copy on this slot completed at f.done, so its semaphore can go back to the next acquire.
    std::swap(f.acquired, acquire_semaphore);
    VkSemaphoreSubmitInfo acquire_waits[2]{};
    uint32_t acquire_wait_count = 0;
    f.retired_acquire = take_retired_acquire();
    for (VkSemaphore semaphore : {f.acquired, f.retired_acquire}) {
        if (!semaphore) continue;
        auto& wait = acquire_waits[acquire_wait_count++];
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait.semaphore = semaphore;
        wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = dev.swap_rendered[index];
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cb.commandBuffer = f.cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cb;
    si.waitSemaphoreInfoCount = acquire_wait_count;
    si.pWaitSemaphoreInfos = acquire_waits;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &signal;
    std::unique_lock qk(dev.present_mu);
    if (!pend.cbs.empty()) flush_reason = FR_PRESENT;
    queue_flush_locked();
    static int64_t presents_counted, st_flushes0, st_cbs0, r0[FR_COUNT];
    if (st_flushes && ++presents_counted % 120 == 0) {
        logf("vk: batched submits: %lld flushes, %.1f command buffers each, over 120 presents; by reason: poll %lld, wait %lld, "
             "slot %lld, present %lld, cap %lld, value %lld, other %lld", (long long)(st_flushes - st_flushes0),
             (double)(st_batched_cbs - st_cbs0) / std::max<int64_t>(1, st_flushes - st_flushes0),
             (long long)(st_reason[FR_POLL] - r0[FR_POLL]), (long long)(st_reason[FR_WAIT] - r0[FR_WAIT]),
             (long long)(st_reason[FR_SLOT] - r0[FR_SLOT]), (long long)(st_reason[FR_PRESENT] - r0[FR_PRESENT]),
             (long long)(st_reason[FR_CAP] - r0[FR_CAP]), (long long)(st_reason[FR_VALUE] - r0[FR_VALUE]),
             (long long)(st_reason[FR_OTHER] - r0[FR_OTHER]));
        st_flushes0 = st_flushes, st_cbs0 = st_batched_cbs;
        std::memcpy(r0, st_reason, sizeof r0);
    }
    reflex_submit_ends();
    // (NVIDIA Reflex: the frame this present shows; with frame generation the set's game frame, none for a made frame)
    si.pNext = reflex_submit_id(nullptr, reflex_present_frame);
    res = submit_or_retry(si, f.done);
    if (res == VK_ERROR_DEVICE_LOST) device_lost();
    if (present_defer_into) {   // (present ahead: the copy is queued; present_commit shows it at its time)
        present_defer_into->used = true;
        present_defer_into->index = index;
        present_defer_into->done = f.done;
        present_defer_into->swapchain = dev.swapchain;
        present_defer_into->reflex_frame = reflex_present_frame;   // (NVIDIA Reflex: the frame it shows, at its present)
        present_defer_into->antilag_frame = antilag_present_frame;   // (AMD Anti-Lag 2: the same)
        present_defer_into->swapchain_generation = dev.swapchain_generation;
        return;
    }
    // (acquire ahead: the presenter thread asks for the next present's image right after it makes this one, a game frame's)
    present_acquire_ahead = src && acquire_ahead_wanted();
    present_copy_done = f.done;
    present_out(index, qk, trial, preview_path);
    present_copy_done = VK_NULL_HANDLE;
    present_acquire_ahead = false;
}

// (longshot) Acquire ahead: on by default with every driver (device_policy.h acquire_ahead); RX_VK_ACQUIRE_AHEAD=0 turns it
// off (each image asked for at its own present, #1458's order), for A/B runs and as the off switch. Since #1376 the
// window's image is waited for on the CPU, at its present, on every GPU: in its fence
// (image_free) where the driver hands the image back from vkAcquireNextImageKHR at once (AMD's on Windows), in the acquire
// itself where it waits there (NVIDIA's, Mesa's). Asked for right after the present before, the image has the next frame's
// work to come back in, whichever part holds the wait. Measured on AMD's Windows driver: in a window the system gives the
// image back at its next composition, and with the presenter thread (on every PC) the render thread still waited 2.3-2.7 ms a
// frame for it (RX 9070 XT, driver 26.11.2, a 1280x720 window on a 144 Hz desktop; full screen 0.4). The rules, the same on
// every GPU and system where it is on:
// - Asked for on the presenter thread, right after its own vkQueuePresentKHR of a game frame came back VK_SUCCESS, on the
//   swapchain that present went to (presenter_loop). The swapchain is one thread's at a time: the presenter's from the
//   hand-over until it says it is done (presenter.busy), and every other user of the swapchain and of the image's state below
//   (held_image, acquire_fence, fences_let_go, image_waits) waits for that first (present_thread_drain: present_image before
//   its acquire, create_swapchain, the lost surface's paths, frame generation's present_commit). The acquire is made with
//   presenter.mu held, and a drain that gave up after its 5 s clears presenter.ahead under it, so an image is never asked
//   for ahead after the render thread went on without the presenter. The swapchain's lock (presenter_queue_mu) is taken for
//   the call, as for the presenter's present; the queue's lock (dev.present_mu) is not: the replay's submits go on meanwhile.
// - Never waited for: a timeout of 0, so a driver that waits in the acquire never holds the presenter (nor, through it, the
//   next hand-over). None given at once (VK_NOT_READY, VK_TIMEOUT), or out of date, or the surface lost: nothing is out, and
//   the next present acquires as when it is off (and meets the same result there; on Linux its 100 ms timeout still tells a
//   window that is not shown).
// - At most one image asked for ahead, none while one is held, and only with an unsignaled fence that is not in use
//   (acquire_fence_ready). Right after frame generation is turned off its last presents ahead may still hold images of
//   their own; the image asked for ahead is then one more out, as the next present's own acquire would be when it is off.
// - Its present waits for it up to kImageFreeNs as for an image acquired then, and only then holds it (#1376's path). A
//   swapchain made again meanwhile lets it go (held_image_drop: its fence reused if signaled, else kept in fences_let_go
//   until it is).
// - Not while frame generation runs (its presents ahead hold images of their own, and are made by its display thread), under
//   FIFO (V-Sync On, or a driver's only mode: the acquire is what paces there, and the presenter waits there for the present
//   to be shown), or for the start-up screen's presents: each image asked for at its present, as before.
static bool acquire_ahead_wanted() {
    if (!acquire_ahead_on()) return false;
    const int why = framegen_on() ? 1
                    : dev.swap_mode != VK_PRESENT_MODE_IMMEDIATE_KHR && dev.swap_mode != VK_PRESENT_MODE_MAILBOX_KHR ? 2 : 0;
    static int told = -1;
    if (why != told) {
        told = why;
        static const char* const whys[] = {"", "frame generation runs", "the swapchain is FIFO (V-Sync On, or the window's only mode)"};
        if (why) logf("vk: acquire ahead: not now, %s: each image asked for at its present", whys[why]);
        else logf("vk: acquire ahead: each next image asked for on the presenter thread right after the present");
    }
    return !why;
}
/// (the presenter thread, presenter.mu held, the swapchain its own (presenter.busy); right after its present of a game frame
/// came back VK_SUCCESS on sc, the swapchain of generation) The next present's image, asked for now. A lost device ends the
/// process in device_lost, so the locks held there are no matter
static void acquire_ahead(VkSwapchainKHR sc, uint64_t generation) {
    if (held_image.held || generation != dev.swapchain_generation || !acquire_fence_ready()) return;
    uint32_t index = 0;
    VkResult res;
    {   // (the swapchain's lock, as the presenter's present takes it: RX_VK_FG_LATENCY_LOG's waits on the chain never beside it)
        std::lock_guard pq(dev.presenter_queue_mu);
        res = vkAcquireNextImageKHR(dev.device, sc, 0, acquire_semaphore, acquire_fence, &index);
        if (res == VK_ERROR_DEVICE_LOST) device_lost();
    }
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {   // (no image: the fence untouched, the next present acquires)
        image_waits.ahead_not_given++;
        return;
    }
    held_image = {true, index, generation, true};
    image_waits.ahead++;
}
} // namespace

bool present_commit(const PendingPresent& p) {
    if (!p.used) return false;
    rec::Settled settled;   // (RX_VK_RECORDER_PARTS: its copy was submitted after the parts before it; the present needs none)
    std::lock_guard lk(present_image_mu);
    std::unique_lock qk(dev.present_mu);
    if (p.swapchain != dev.swapchain || p.swapchain_generation != dev.swapchain_generation) return false;   // (made again since the copy)
    const uint64_t was = reflex_present_frame, al_was = antilag_present_frame;
    reflex_present_frame = p.reflex_frame;   // (NVIDIA Reflex: the frame this present shows, as at its copy)
    antilag_present_frame = p.antilag_frame;   // (AMD Anti-Lag 2: the same)
    const bool shown = present_out(p.index, qk);
    reflex_present_frame = was, antilag_present_frame = al_was;
    return shown;
}

// The present of a swapchain image whose copy is submitted, as present_image ended before; qk holds dev.present_mu
static bool present_out(uint32_t index, std::unique_lock<decltype(dev.present_mu)>& qk, OutputTrialSource trial, bool preview_path) {
#ifdef TRACY_ENABLE
    HT_SCOPE("present_out");
#endif
    VkResult res;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.pNext = reflex_present_id(nullptr);   // (NVIDIA Reflex: the frame's present id, and its present's markers)
    const bool hand_off = present_copy_done && present_thread_on();
    OutputTrialFrame frame;
    if (trial.generation) {
        const bool fifo_shown = preview_path && hand_off && !pi.pNext && dev.present_wait &&
                                dev.swap_mode == VK_PRESENT_MODE_FIFO_KHR && dev.swapchain && !dev.hdr;
        frame = make_output_trial_frame(trial, dev.swapchain_generation, fifo_shown ? OutputPath::FifoShown : OutputPath::Unavailable);
    }
    if (hand_off) {
        const uint64_t reflex_id = pi.pNext ? static_cast<const VkPresentIdKHR*>(pi.pNext)->pPresentIds[0] : 0;
        if (reflex_id) reflex_present_handed_off();
        antilag_present();   // (AMD Anti-Lag 2: the frame's present stage)
        qk.unlock();
        present_hand_off(index, present_copy_done, reflex_id, frame);
        return true;   // (taken as shown: only present_commit's presents, never handed over, read it)
    }
    if (present_thread_on()) {   // (one handed over before, made first: the presents in order; the queue's lock let go meanwhile)
        qk.unlock();
        present_thread_drain();
        qk.lock();
    }
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &dev.swap_rendered[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &dev.swapchain;
    pi.pImageIndices = &index;
    antilag_present();   // (AMD Anti-Lag 2: the frame's present stage)
    // (longshot) vkQueuePresentKHR blocked a quarter of the display thread's time with frame interpolation at 4x (a 14700K and an
    // RTX 5070 Ti), and with the queue's lock held there the game's main thread waited ~9% of its time and the presentation thread
    // ~15% to submit. With a queue of their own (dev.present_queue, a compute family's: vk_main.cpp create_device) the presents go
    // out with the submits' lock let go: the blit above signals swap_rendered on the replay's queue (submitted already, as a binary
    // semaphore's wait needs). (Waiting for the blit's fence before the present, with the lock let go, was worse.)
    // (the FPS counter's Breakdown: the present's wait is vsync's, the pacing's; unless the frame's copy was not done when it was
    // called and was when it returned. Then the present waited for the GPU: NVIDIA's driver on Linux waits there for the copy, after
    // all of the frame's work, ~2 ms a frame on an RTX 3090. Two reads of its fence, while the Breakdown shows; nothing waits)
    const VkFence copy = fps_breakdown_on() ? present_copy_done : VK_NULL_HANDLE;
    // (a wait of no time: the fence's state)
    const auto copy_done = [&] { return (::vkx::vkWaitForFences)(dev.device, 1, &copy, VK_TRUE, 0) == VK_SUCCESS; };
    const bool copy_was_done = copy && copy_done();
    const int64_t present_t0 = fps_breakdown_on() ? fps_clock() : 0;
    if (dev.present_queue_supported) {
        qk.unlock();
        std::lock_guard pq(dev.present_queue_mu);
        res = vkQueuePresentKHR(dev.present_queue, &pi);
    } else {
        res = vkQueuePresentKHR(dev.queue, &pi);
    }
    if (present_t0) {
        const bool waited_gpu = copy && !copy_was_done && copy_done();
        fps_wait(waited_gpu ? fps_stats::kGpuWait : fps_stats::kPace, fps_clock() - present_t0);
    }
    if (res == VK_ERROR_DEVICE_LOST) device_lost();
    reflex_presented();
    if (res == VK_ERROR_SURFACE_LOST_KHR) {
        if (!qk.owns_lock()) qk.lock();
        invalidate_lost_surface(res);
        return false;
    }
    present_finish(res, false);
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
        if (!qk.owns_lock()) qk.lock();
        create_swapchain(dev.swap_extent.width, dev.swap_extent.height);
    }
    return res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR;
}
namespace {
/// What follows a present, on the thread that made it. The presenter thread asks the next present_image for a new swapchain (its
/// swap_resize) rather than making one itself
void present_finish(VkResult res, bool on_presenter) {
#ifdef TRACY_ENABLE
    HT_SCOPE("present_finish");
#endif
    if (res < 0 && res != VK_ERROR_OUT_OF_DATE_KHR) {   // (a present that failed: said, a few times)
        static int logged;
        if (logged++ < 8) logf("vk: present failed (VkResult %d%s)", (int)res, dev.present_queue_supported ? ", on the presents' own queue" : "");
    }
    frame_log_present();
    if (on_presenter && res == VK_ERROR_SURFACE_LOST_KHR) presenter_surface_lost.store(true, std::memory_order_release);
    if (on_presenter && (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR)) {   // (the size it has: made again as it is)
        uint64_t empty = 0;
        dev.swap_resize.compare_exchange_strong(empty, (uint64_t)dev.swap_extent.width << 32 | dev.swap_extent.height,
                                                std::memory_order_release, std::memory_order_relaxed);
    }
}
}  // namespace

// (longshot) The presenter thread, on every PC (present_thread_on). With NVIDIA's driver on Linux, vkQueuePresentKHR waits on the
// CPU until the frame's copy into the window's image is done on the GPU, after all of the frame's work: 2.4-4.5 ms a frame of the
// thread that presents, on the frame's critical path. present_image hands the present to a thread of its own, which waits for the
// copy's fence, then presents on the presents' own queue (the replay's, under dev.present_mu, or the Present queue row's) with
// that queue's lock held for the call alone, and the game's thread goes on at once.
// - One present out at most: the next frame's acquire waits until it is made (present_image: the swapchain is one thread's at a
//   time); under FIFO (V-Sync On), with VK_KHR_present_wait, until it is shown. No frame is buffered past what the game's own
//   present gave: the game is never further ahead than before.
// - The presents in order; a swapchain is made again or dropped only with the presenter idle (present_thread_drain, before the
//   queue's lock).
// - Only the presents made right after their copy (present_image; not frame interpolation's, made by present_commit, whose display
//   thread is already its own). With NVIDIA Reflex its present id goes with the present: the presenter presents with it and marks
//   the present's end.
// - No wait without end: the copy's fence and the present wait in steps, a device lost ends them, and a drain gives up after 5 s.
// - Acquire ahead (on by default: acquire_ahead_wanted): after a game frame's present that came back
//   VK_SUCCESS, the next present's image is asked for here, with a timeout of 0 and presenter.mu held, before the presenter
//   says it is done; present_image takes it after its drain.
namespace {
struct Presenter {
    std::mutex mu;
    std::condition_variable cv;
    bool busy = false;               // (a present handed over and not yet made)
    uint32_t index = 0;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFence done = VK_NULL_HANDLE;   // (the copy's fence)
    uint64_t reflex_id = 0;          // (NVIDIA Reflex's present id for it, 0 none: present_out)
    uint64_t generation = 0;         // (dev.swapchain_generation of swapchain)
    bool ahead = false;              // (acquire ahead after it: present_image's present_acquire_ahead; a drain that gave up clears it)
    OutputTrialFrame trial;
    int64_t copy_done_at = 0;        // (fps_clock() when the presenter's wait for it ended: the FPS counter's Breakdown)
    bool started = false;
};
Presenter presenter;
/// The copy's fence, in 100 ms steps: false after 4 s (a GPU that stopped: the present is not made) or a device lost. Less
/// than present_thread_drain's 5 s: a drain that gave up at the moment the presenter did went on to remake the swapchain while
/// the presenter still held it
bool presenter_wait_copy(VkFence done) {
#ifdef TRACY_ENABLE
    HT_WAIT(ht_w, "presenter_wait_copy");
#endif
    for (int step = 0; step < 40; step++) {
        const VkResult r = vkWaitForFences(dev.device, 1, &done, VK_TRUE, 100000000ull);
        if (r == VK_SUCCESS) return true;
        if (r == VK_ERROR_DEVICE_LOST) device_lost();
        if (r != VK_TIMEOUT) return false;
    }
    static int told;
    if (told++ < 4) logf("vk: presenter thread: a frame's copy not done in 4 s: its present not made");
    return false;
}
void presenter_loop() {
    SetThreadDescription(GetCurrentThread(), L"mkw_vk presenter");
    // (RX_VK_RECORDER_PARTS: its fence is the copy's, submitted before the hand-off, and its present needs no part: it never waits
    // for the parts handed to the recorder, at its waits or the queue's lock)
    rec::Settled settled;
#ifdef TRACY_ENABLE
    HT_THREAD_NAME("mkw_vk presenter");
#endif
    static const auto wait_for_present = dev.present_wait
        ? (PFN_vkWaitForPresentKHR)vkGetDeviceProcAddr(dev.device, "vkWaitForPresentKHR") : nullptr;
    static const bool frame_trace_on = getenv("RX_VK_FRAME_TRACE") && atoi(getenv("RX_VK_FRAME_TRACE"));
    VkSwapchainKHR ids_of = VK_NULL_HANDLE;   // (the swapchain the present ids count on: they start again on a new one)
    uint64_t next_id = 0;
    std::unique_lock lk(presenter.mu);
    for (;;) {
        presenter.cv.wait(lk, [] { return presenter.busy; });
        const uint32_t index = presenter.index;
        const VkSwapchainKHR sc = presenter.swapchain;
        const VkFence done = presenter.done;
        const uint64_t reflex_id = presenter.reflex_id;
        const uint64_t generation = presenter.generation;
        const OutputTrialFrame trial = presenter.trial;
        lk.unlock();
        const auto t0 = std::chrono::steady_clock::now();
        const bool copied = !done || presenter_wait_copy(done);
        const auto t1 = std::chrono::steady_clock::now();
        if (fps_breakdown_on()) {
            const int64_t at = fps_clock();
            std::lock_guard g(presenter.mu);
            presenter.copy_done_at = at;
        }
        VkResult res = VK_NOT_READY;
        double shown_ms = -1;
        if (copied) {
            if (sc != ids_of) ids_of = sc, next_id = 0;
            // (under FIFO the present is waited for until shown: the next frame is not handed over before, as the game's own
            // present held it at the swapchain before. Mailbox and immediate: as before, the present's return)
            const bool wait_shown = wait_for_present && dev.swap_mode == VK_PRESENT_MODE_FIFO_KHR;
            // (NVIDIA Reflex's id when it gave one that goes up from the last: its markers name it; else the next of our own)
            const bool reflex = reflex_id > next_id;
            const uint64_t id = reflex ? reflex_id : next_id + 1;
            next_id = id;
            VkPresentIdKHR pid{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
            pid.swapchainCount = 1;
            pid.pPresentIds = &id;
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.pNext = dev.present_wait || reflex ? &pid : nullptr;   // (ids on every present of the swapchain, so they go up)
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &dev.swap_rendered[index];
            pi.swapchainCount = 1;
            pi.pSwapchains = &sc;
            pi.pImageIndices = &index;
            {   // (on the queue the presents use (the replay's, or the Present queue row's), its lock held for the call only:
                // the copy is done by now, so the call does not wait for the GPU (NVIDIA's driver on Linux would, ~4.5 ms, with
                // the replay's submits held behind it). Lock order: the queue's, then presenter_queue_mu (the swapchain's)
                const bool own = dev.present_queue && dev.present_queue_supported;
                std::unique_lock q(own ? dev.present_queue_mu : dev.present_mu);
                std::lock_guard pq(dev.presenter_queue_mu);
                res = vkQueuePresentKHR(own ? dev.present_queue : dev.queue, &pi);
            }
            if (reflex) reflex_present_end(id);
            if (res == VK_ERROR_DEVICE_LOST) device_lost();
            if (wait_shown && (res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR)) {
                const auto ts = std::chrono::steady_clock::now();
                VkResult w = VK_TIMEOUT;
                for (int step = 0; step < 5 && w == VK_TIMEOUT; step++) w = wait_for_present(dev.device, sc, id, 20000000ull);
                if (w == VK_ERROR_DEVICE_LOST) device_lost();
                if (w == VK_SUCCESS) {
                    if (res == VK_SUCCESS) acknowledge_output_trial_frame(trial);
                    shown_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ts).count();
                    const int64_t wait_us = (int64_t)(shown_ms * 1000);
                    // (no bookkeeping at all when neither consumer is on: the perf tick or the frame trace)
                    if (perf_measure.load(std::memory_order_relaxed) || frame_trace_on) {
                        shown_wait_us.store(wait_us);                      // (the frame trace's presentation clock)
                        shown_at_ns.store(host->now_ns ? host->now_ns() : fps_clock());
                        shown_sum_us.fetch_add(wait_us), shown_count.fetch_add(1);   // (the perf summary's)
                    }
                }
            }
        }
        {   // (what it waited for, every 600 presents)
            static double fence_ms, present_ms, shown_sum;
            static int n, shown_n;
            const auto t2 = std::chrono::steady_clock::now();
            fence_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
            present_ms += std::chrono::duration<double, std::milli>(t2 - t1).count() - std::max(shown_ms, 0.0);
            if (shown_ms >= 0) shown_sum += shown_ms, shown_n++;
            if (++n % 600 == 0) {
                if (shown_n)
                    logf("vk: presenter thread: the copy's fence %.2f ms, the present %.2f ms, shown %.2f ms after it (%d of 600)",
                         fence_ms / 600, present_ms / 600, shown_sum / shown_n, shown_n);
                else
                    logf("vk: presenter thread: the copy's fence %.2f ms, the present %.2f ms a present", fence_ms / 600, present_ms / 600);
                fence_ms = present_ms = shown_sum = 0, shown_n = 0;
            }
        }
        if (copied) present_finish(res, true);
        lk.lock();
        // (acquire ahead: the next present's image asked for now, while the swapchain is still this thread's and presenter.mu is
        // held (a drain that gave up meanwhile cleared presenter.ahead). Only after a present made as it is: out of date or
        // suboptimal asks the next present_image for a new swapchain, which would let the image go again)
        if (presenter.ahead && res == VK_SUCCESS) acquire_ahead(sc, generation);
        presenter.ahead = false;
        presenter.busy = false;
        presenter.cv.notify_all();
    }
}
bool present_thread_on() {
    // (every PC: each present made on the presenter thread once its copy is done, on the presents' own queue. It frees the
    // render thread from the present call wherever that call waits (NVIDIA's driver on Linux: ~4.5 ms for the copy) and costs
    // a thread's hand-off elsewhere. A second queue of its own would time-share the GPU with the replay: presents 2-4 ms
    // later; presenting on the presents' own queue after the copy's fence keeps that queue's lock for the call only)
    static bool told = false;
    if (!told) {
        told = true;
        logf("vk: presenter thread: each present made on a thread of its own once its copy is done");
    }
    return true;
}
/// Waits, with presenter.mu held in lk, until the presenter has made the present it was handed (limit_ms: at most that long; false
/// then). For the FPS counter's Breakdown, the wait is the GPU's until the presenter's wait for the copy ended, and the pacing's after it
/// (the present itself, vsync)
bool presenter_wait_idle(std::unique_lock<std::mutex>& lk, int limit_ms = 0) {
    if (!presenter.busy) return true;
    const int64_t t0 = fps_breakdown_on() ? fps_clock() : 0;
    const auto idle = [] { return !presenter.busy; };
    if (limit_ms) {
        if (!presenter.cv.wait_for(lk, std::chrono::milliseconds(limit_ms), idle)) return false;
    } else {
        presenter.cv.wait(lk, idle);
    }
    if (!t0) return true;
    const int64_t t1 = fps_clock();
    const int64_t gpu = presenter.copy_done_at ? std::clamp<int64_t>(presenter.copy_done_at - t0, 0, t1 - t0) : 0;
    fps_wait(fps_stats::kGpuWait, gpu);
    fps_wait(fps_stats::kPace, t1 - t0 - gpu);
    return true;
}
/// The present of image index, whose copy signals done, to the presenter thread (started at the first), once the one before is made
void present_hand_off(uint32_t index, VkFence done, uint64_t reflex_id, OutputTrialFrame trial) {
#ifdef TRACY_ENABLE
    HT_SCOPE("present_hand_off");
#endif
    std::unique_lock lk(presenter.mu);
    if (!presenter.started) {
        presenter.started = true;
        ::mkwvk::cxx_escape::spawn_needed("presenter", presenter_loop);
    }
    presenter_wait_idle(lk);   // (the frame before's present made: one out at most; the presenter's own waits are bounded)
    presenter.index = index, presenter.swapchain = dev.swapchain, presenter.done = done, presenter.busy = true;
    presenter.reflex_id = reflex_id;
    presenter.generation = dev.swapchain_generation, presenter.ahead = present_acquire_ahead;
    presenter.trial = trial;
    presenter.copy_done_at = 0;
    presenter.cv.notify_all();
}
}  // namespace
/// Waits until the presenter thread has made the present it was handed (none: at once). Before a swapchain is made again or destroyed,
/// and never with dev.present_mu held (a resize must not wait on a present that waits for the window). Gives up after 5 s, said.
/// The return is whether the presenter is idle: the close only destroys the swapchain when it is (a worker still mid-present
/// would touch a chain being destroyed; vk_main.cpp's close leaves the chain to the process exit otherwise)
bool present_thread_drain() {
#ifdef TRACY_ENABLE
    HT_WAIT(ht_w, "present_thread_drain");
#endif
    std::unique_lock lk(presenter.mu);
    if (presenter_wait_idle(lk, 5000)) return true;
    presenter.ahead = false;   // (acquire ahead: none after a present the caller goes on without)
    logf("vk: presenter thread: its present not made in 5 s; going on without it");
    return false;
}

/// (the one rule, so every path that would retire the chain agrees) may this swapchain be let go
/// and made again now? The presenter thread copies the image index and the chain at the hand-off
/// (presenter_loop), works away from the presenter's mutex for as long as its fence and its
/// vkWaitForPresent run, and then reads dev.swap_rendered[index] — while retire clears that vector
/// and destroys its semaphores and the chain under it. A present still in the making therefore
/// keeps its chain: the caller waits for the next frame (a wedge of the GPU shows up as the frame
/// not being shown, which is what a wedged GPU already means, not as the presenter presenting a
/// destroyed chain). The close asks the same question the same way (vk_main.cpp)
bool swapchain_made_ready() {
    if (present_thread_drain()) return true;
    logf("vk: a present of this swapchain is still in the making on the presenter thread: the chain waits for it");
    return false;
}


namespace {

void set_texture_pool_size(int) {}
void read_texels(const nvn_tex* t, unsigned level, unsigned layer0, const NVNcopyRegion* r, uint8_t* dst, uint64_t row,
                 uint64_t img) {
    finish();   // (what the submits before it wrote)
    std::lock_guard lk(replay_mu);
    {   // (RX_VK_LOG_READ_TEXELS=1: the CPU reading textures back, 200 lines: are they ones the GPU wrote?)
        static const bool log_reads = env_on("RX_VK_LOG_READ_TEXELS");
        static int logged;
        if (log_reads && t && logged < 200) {
            logged++;
            Texture* tex = texture_get(t, false);
            logf("vk: ReadTexels %#llx %ux%ux%u NVN %#x level %u layer %u region (%d,%d,%d %dx%dx%d) row %llu: %s", (unsigned long long)t->gpu,
                 t->d.width, t->d.height, t->d.depth, t->d.format, level, layer0, r ? r->xoffset : 0, r ? r->yoffset : 0,
                 r ? r->zoffset : 0, r ? r->width : 0, r ? r->height : 0, r ? r->depth : 0, (unsigned long long)row,
                 !tex ? "no image" : tex->gpu_written ? (tex->released ? "rendered, finalized" : "RENDERED (guest memory is stale)")
                                                     : "uploaded");
        }
    }
    texture_read_texels(t, level, layer0, r, dst, row, img, false);
}
/// (host: virtual pages [gpu, gpu + size) are about to be unmapped or remapped) what the GPU wrote into textures there
/// goes into their memory first, as on the console, where it is there all along (vk_texture.cpp, texture_flush_to_guest)
bool replay_behind();
void vmap_changing(uint64_t gpu, uint64_t size) {
    if (!size) return;
    static const bool log_calls = env_on("RX_VK_LOG_VMAP_CHANGING");   // (each call, 300 lines)
    static std::atomic<int> logged{0};
    // (the list of GPU-written textures is complete only once the replay has caught up: MK World unmaps a tile's pool
    // a frame or two after it submitted the drawing, which the replay may not have recorded yet)
    const bool listed = texture_gpu_written_in(gpu, size);
    bool behind = false, listed_after = listed;
    if (!listed) {
        behind = replay_behind();
        if (behind) {
            wait_queue_empty();
            listed_after = texture_gpu_written_in(gpu, size);
        }
    }
    if (log_calls && logged++ < 300)
        logf("vk: vmap changing %#llx..%#llx: listed %d, replay behind %d, listed after %d", (unsigned long long)gpu,
             (unsigned long long)(gpu + size), (int)listed, (int)behind, (int)listed_after);
    if (!listed_after) return;
    wait_queue_empty();   // (the work the game recorded before: submitted)
    std::lock_guard lk(replay_mu);
    const int n = texture_flush_to_guest(gpu, size);
    if (log_calls && logged < 300) logf("vk: vmap changing %#llx: %d written back", (unsigned long long)gpu, n);
}
void write_texels(const nvn_tex* t, unsigned level, unsigned layer0, const NVNcopyRegion* r, const uint8_t* src,
                  uint64_t row, uint64_t img) {
    finish();   // (after the submits before it read the texture)
    std::lock_guard lk(replay_mu);
    texture_read_texels(t, level, layer0, r, const_cast<uint8_t*>(src), row, img, true);
}
void stats(int64_t* draws, int64_t* skipped, int64_t* pipelines) {
    int64_t v[4] = {0, 0, 0, 0};
    draw_stats(&v[0], &v[1], &v[2], &v[3]);
    if (draws) *draws = v[0];
    if (skipped) *skipped = v[1];
    if (pipelines) *pipelines = v[3];
}
void register_texture_locked(int id, const nvn_tex* t, const nvn_view* v) { register_texture(id, t, v); }
void do_program_released(const nvn_prog* p) {
    std::lock_guard lk(replay_mu);   // (the replay may be translating it)
    program_released(p);
}

// ---- the render thread (RX_VK_ASYNC=1; unset or 0, the default: none, the replay on the submitting thread) ----
// The game's submits and presents are queued and replayed by a thread of the back end's own, so the game's thread
// (MKW's Presentation Thread, 70% busy replaying before) goes on with its frame while the last one is recorded. As on
// the console, where the GPU reads the commands and the memory they name after the submit: the host resolves the
// deferred bindings at the submit and keeps the recordings until rx_nvn_replayed() passes them (nvn_native.c), and
// a fence value is handed out at the submit, signalled when the replay's submission completes. At most one present
// waits (RX_VK_PRESENTS_AHEAD): the game is held at that when the replay falls behind. Finalized textures and programs
// are queued too, in order with the work that may still use them.
enum JobKind { JOB_SUBMIT, JOB_PRESENT, JOB_TEXTURE_RELEASED, JOB_PROGRAM_RELEASED, JOB_REGISTER_TEXTURE, JOB_REGISTER_SAMPLER,
               JOB_EVENT_SIGNAL };
struct Job {
    JobKind kind;
    std::vector<uint64_t> handles;
    nvn_tex tex;
    bool has_tex = false;
    int id = 0;                 // (a registration's slot, its view and sampler)
    nvn_view view{};
    bool has_view = false;
    nvn_samp_par samp{};
    uint64_t ev_gpu = 0;        // (a CPU signal: the event and its value)
    uint32_t ev_value = 0;
    uint64_t value = 0;         // (a submit's or present's timeline value: its fence, handed out as it came)
    uint64_t queue = 0;         // (a submit's queue)
    int crop[4] = {0, 0, 0, 0};
    bool has_crop = false;
    uint64_t output_generation = 0;
    nvn_prog prog;
    std::chrono::steady_clock::time_point queued;
    uint64_t cap_seq = 0;       // (RX_VK_MEM_CHECK: the number of the capture made with it, for its take to match; 0: none)
};
std::mutex q_mu;
std::condition_variable q_cv, q_space, q_idle;
std::deque<Job> q_jobs;
int q_presents = 0;
bool q_busy = false;
uint64_t q_handed = 0, q_done = 0;   // (jobs handed over and done: q_mu)
// (longshot, the render thread) Whole frames, a test: RX_VK_FRAME_GATE=1. The render thread then starts a frame's jobs
// when the frame's present has been handed over, not before, and the present is handed over after the main thread's
// signal for the frame (present(): the signal the frame's work waits for): the game has then written the whole frame.
// They were made because replaying a frame's first submits while the game was still writing the rest of that frame (the
// render thread caught up with the game: 0.3-2 % of the presents in the Market) drew the lit scene black for one frame
// at 5-15 % of those moments. Measured in the Market with the flash detector and the meter log (which makes the moments
// frequent): 15 and 15 flashes a session without them and with the gate alone (the present still came before the
// signal), 0 with both. Those moments were also where a submit's capture missed the pages the replay's collection was
// marking again (collect_mu, vk_memory.cpp: a third of the captures with jobs replayed as they come, and Kokiri
// Forest's ground mist gone for a frame in 16 to 89 of 3,600 frames). With that window closed, jobs replayed as they
// come (the default) drew no such frame on an Arc B580 in 23,400 frames (Kokiri Forest and the Market standing, four
// sessions), and a frame no longer waits for its present before its replay starts: 86.95 -> 92.4 fps in Kokiri Forest,
// 53.6 -> 58.0 in the Market (medians of four sessions each, interleaved with four of whole frames). With whole frames,
// a frame behind, the replay still runs beside the game for the whole frame (the larger of the two, not their sum), and
// a frame is shown no later than the replay on the submitting thread shows it. What a game's thread waits for goes on
// at once (gate_upto: a queue finish, a texture read back, a fence of this frame), and so does a job that has waited
// RX_VK_FRAME_GATE_MS (1000) with no present behind it (a load that submits without presenting; at 100 a game frame
// stalled by swapping let its first jobs on, and two such frames flashed in 19,763 presents).
uint64_t gate_upto = 0;   // (jobs up to this hand-over number go on without their frame's present: q_mu)
std::deque<std::pair<uint64_t, uint64_t>> gate_values;   // (the last submits' and presents' fence values and hand-over numbers: q_mu)
struct {
    std::atomic<int64_t> timeouts, demands, fences;
} gate_st;
bool gate_on() {
    static const bool on = async_on() && env_on("RX_VK_FRAME_GATE");
    return on;
}
std::chrono::milliseconds gate_ms() {
    static const int v = getenv("RX_VK_FRAME_GATE_MS") ? std::max(0, atoi(getenv("RX_VK_FRAME_GATE_MS"))) : 1000;
    return std::chrono::milliseconds(v);
}
/// (q_mu) Whether the oldest job handed over may be replayed now
bool gate_ready() {
    if (q_jobs.empty()) return false;
    if (!gate_on() || q_presents > 0 || q_done < gate_upto) return true;
    if (std::chrono::steady_clock::now() - q_jobs.front().queued < gate_ms()) return false;
    gate_upto = q_handed, gate_st.timeouts++;   // (what is queued now goes on: none of it waits again)
    return true;
}
/// (a game's thread, about to wait for fence value f) The submit that value was handed out for goes on without its present
void gate_need_value(uint64_t f) {
    if (!gate_on()) return;
    std::lock_guard lk(q_mu);
    for (const auto& [value, number] : gate_values) {
        if (value < f) continue;
        // (the frame before's, the usual one, has its present handed over: nothing to let on)
        if (number > gate_upto && number > q_done && !q_presents) gate_upto = number, gate_st.fences++, q_cv.notify_all();
        break;
    }
}
// (the render thread's log, every 600 presents: what its jobs took, and what the game's threads waited for it)
struct {
    std::atomic<int64_t> job_ns[3], room_ns, room_n, catchup_ns, catchup_n, depth, rb_fallback, held, held_ns, held_forced,
        sig_caps, walk_ns, tex_page_only, tex_exact, rb_exact, lag_ns, lag_max;
} bh_st;
void bh_shown(const std::chrono::steady_clock::time_point& handed) {   // (a present made: how long after the game handed it over)
    const int64_t d = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - handed).count();
    bh_st.lag_ns += d;
    if (d > bh_st.lag_max.load()) bh_st.lag_max = d;
}
int64_t bh_now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void fo_log(double frames);   // (frame overlap, below: the GPU between recordings and what comes after a frame's last reads)
void bh_log() {
    static int64_t last = bh_now();
    const int64_t now = bh_now();
    const double frames = 600.0, wall = (now - last) / 1e6 / frames;
    last = now;
    const double sub = bh_st.job_ns[0].exchange(0) / 1e6 / frames, pres = bh_st.job_ns[1].exchange(0) / 1e6 / frames,
                 other = bh_st.job_ns[2].exchange(0) / 1e6 / frames;
    const int64_t rn = bh_st.room_n.exchange(0), cn = bh_st.catchup_n.exchange(0);
    if (gate_on())
        logf("vk: the render thread's whole frames (600 presents): %lld jobs went on before their present after %d ms, %lld times a "
             "game's thread waited for the work handed over, %lld times for a fence of the frame in the making",
             (long long)gate_st.timeouts.exchange(0), (int)gate_ms().count(), (long long)gate_st.demands.exchange(0),
             (long long)gate_st.fences.exchange(0));
    logf("vk: the render thread (600 presents, %.2f ms a frame): its jobs %.2f ms a frame (submits %.2f, presents %.2f, others "
         "%.2f), idle %.2f; the game waited for room at %lld presents, %.2f ms each (%.2f a frame; %.2f jobs queued at a "
         "present); %lld catch-ups (a finish, a texture read back, a write-back not recorded yet: %lld), %.2f ms each; "
         "%lld presents held for the CPU's signal, %.2f ms each (%lld let go by RX_VK_HOLD_MS); %lld captures at a CPU signal; "
         "the submits' waits read in %.3f ms a frame; readbacks: %lld next to a texture's storage (no catch-up), %lld in one, "
         "%lld read exactly (after the work handed over: among the catch-ups); a present made %.2f ms after its hand-over (%.2f at most)",
         wall, sub + pres + other, sub, pres, other, wall - sub - pres - other, (long long)rn,
         rn ? bh_st.room_ns.load() / 1e6 / rn : 0.0, bh_st.room_ns.exchange(0) / 1e6 / frames,
         bh_st.depth.exchange(0) / frames, (long long)cn, (long long)bh_st.rb_fallback.exchange(0),
         cn ? bh_st.catchup_ns.exchange(0) / 1e6 / cn : 0.0, (long long)bh_st.held.load(),
         bh_st.held.load() ? bh_st.held_ns.exchange(0) / 1e6 / bh_st.held.load() : 0.0, (long long)bh_st.held_forced.exchange(0),
         (long long)bh_st.sig_caps.exchange(0), bh_st.walk_ns.exchange(0) / 1e6 / frames, (long long)bh_st.tex_page_only.exchange(0),
         (long long)bh_st.tex_exact.exchange(0), (long long)bh_st.rb_exact.exchange(0), bh_st.lag_ns.exchange(0) / 1e6 / frames,
         bh_st.lag_max.exchange(0) / 1e6);
    bh_st.held = 0;
    fo_log(frames);
}
int presents_ahead() {   // (RX_VK_PRESENTS_AHEAD=<n>: how many presents the game may hand over ahead of the replay; 1)
    static const int n = std::clamp(getenv("RX_VK_PRESENTS_AHEAD") ? atoi(getenv("RX_VK_PRESENTS_AHEAD")) : 1, 1, 8);
    return n;
}

// ---- (longshot) Frame overlap: what each recording does to the game's memory, and the GPU's time between recordings.
// frame_overlap.h has the rules; docs/dev/reference/architecture.md, The frame pipeline the frame's sequence. ----
// The game waits once a frame, with no timeout, for the fence of the frame before's present (OoT's main thread, at
// 0x8042cde0), and makes the CPU signal its next frame's work waits for only after it: the render thread gets a frame once
// the GPU has finished the one before. Counted always, with the render thread: per recording, whether it reads the game's
// memory at the GPU's time (a dispatch, a draw that follows pointers, a binding in place) and whether its part's end writes
// any back; per frame, how many recordings and how much GPU time come after the last of each kind; and the GPU's time
// between recordings, idle and in upload command buffers, by where in the frame (bh_log's second line).
/// Whether recordings are counted: the render thread, its recordings made in order
bool fo_tracking() {
    static const bool on = async_on();
    return on && rec_timeline != VK_NULL_HANDLE;
}
struct FoSlot {
    uint64_t seq = 0;   // (its number on the recordings' timeline)
    int column = 0;     // (frame_overlap::gap_column: where in its frame it comes)
};
FoSlot fo_slots[kRecordings];   // (replay_mu, as the rest of fo: recordings are begun, ended and collected under it)
VkQueryPool fo_up_pool = VK_NULL_HANDLE;   // (a timestamp at the start of each recording's upload command buffer, by slot)
struct {
    uint64_t last_read = 0, last_touch = 0;   // (the newest recording that read the game's memory, and that read or wrote it)
    int64_t follows = 0, follows0 = 0, wb0 = 0;   // (reading draws and dispatches so far; at the last recording's end, with the bytes written back)
    uint64_t last_end = 0;                    // (the GPU's clock at the end of the recording collected last; 0: not known)
    int64_t work0 = 0;                        // (draws and dispatches so far, at the last recording's end)
    int frame_working = 0;                    // (this frame's recordings so far that had one)
    frame_overlap::Frames reads, touches;     // (the frames recorded whole, by their last recording of each kind: for the tails' GPU time)
} fo;
std::atomic<uint64_t> fo_present{0};      // (the value of the present being made: its own recording ends its frame)
struct {
    std::atomic<int64_t> read_tail, touch_tail, read_tail_ns, touch_tail_ns, idle_ns[frame_overlap::kGapColumns], uploads_ns[frame_overlap::kGapColumns];
} fo_st;
// ---- (longshot) Frame ahead: counters only. What the parts recorded in the signal's job took that came with the signal
// (frame_overlap.h PartInputs; docs/dev/reference/architecture.md, The frame pipeline has what is and is not counted, and what the numbers decide).
struct {
    frame_overlap::AheadTally tally;   // (replay_mu)
    int64_t cap0 = 0, live0 = 0;       // (memory_sync_sources at the last part's end, or at the signal's job's start)
} fa;
std::atomic<bool> fa_in_signal{false};   // (the render thread is in a signal's job that took a capture)
struct {
    std::atomic<int64_t> signals, pages, pools, regs, releases, other_signals, frames_any;
} fa_st;
struct FaWindow {   // (the render thread's: the jobs since the frame's last submit)
    int regs = 0, releases = 0, signals = 0;
} fa_window;
/// (the render thread, at a signal's job that brings a capture, before the capture is taken)
void fa_signal_begin() {
    if (!fo_tracking()) return;
    int64_t pages = 0, pools = 0;
    memory_capture_peek(&pages, &pools);
    fa_st.signals++, fa_st.pages += pages, fa_st.pools += pools;
    fa_st.regs += fa_window.regs, fa_st.releases += fa_window.releases, fa_st.other_signals += fa_window.signals;
    fa_st.frames_any += fa_window.regs || fa_window.releases || fa_window.signals;
    fa_window = {};
    std::lock_guard lk(replay_mu);
    memory_sync_sources(&fa.cap0, &fa.live0);
    fa_in_signal.store(true, std::memory_order_release);
}
void fo_recording_begin(const Recording& r) {
    if (!fo_tracking()) return;
    fo_slots[r.slot] = {};
    if (!fo_up_pool) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = kRecordings;
        if (vkCreateQueryPool(dev.device, &qi, nullptr, &fo_up_pool) != VK_SUCCESS) fo_up_pool = VK_NULL_HANDLE;
        if (!fo_up_pool) return;
    }
    // (in the upload command buffer itself: one with no uploads is not submitted, and its slot's query keeps an older use's
    // time, which fo_collect tells by its place)
    vkCmdResetQueryPool(r.upload, fo_up_pool, (uint32_t)r.slot, 1);
    vkCmdWriteTimestamp2(r.upload, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, fo_up_pool, (uint32_t)r.slot);
}
void fo_recording_end(const Recording& r) {
    if (!fo_tracking()) return;
    const uint64_t seq = rec_seq + 1;   // (the number submit_recording gives it next: its callers hold replay_mu)
    int64_t mirrors = 0, sync_bytes = 0, sync_copies = 0, wb = 0;
    uint64_t mirror_mb = 0;
    memory_stats(&mirrors, &mirror_mb, &sync_bytes, &sync_copies, &wb);
    const frame_overlap::RecordingFacts f{fo.follows - fo.follows0, wb - fo.wb0, memory_in_place_part(), r.part};
    fo.follows0 = fo.follows, fo.wb0 = wb;
    if (frame_overlap::reads_game(f)) fo.last_read = seq;
    if (frame_overlap::touches_game(f)) fo.last_touch = seq;
    int64_t draws = 0, skipped = 0, dispatches = 0, pipes = 0;
    draw_stats(&draws, &skipped, &dispatches, &pipes);
    // (the six submits before the signal's job record no draw: they and the first part after them are the frame's lead)
    fo_slots[r.slot] = {seq, frame_overlap::gap_column(fo.frame_working)};
    if (draws + dispatches != fo.work0) {
        fo.frame_working++;
        if (fa_in_signal.load(std::memory_order_acquire)) {   // (frame ahead: a part of the signal's job, and what it took)
            int64_t cap = 0, live = 0;
            memory_sync_sources(&cap, &live);
            fa.tally.part({cap - fa.cap0, live - fa.live0});
            fa.cap0 = cap, fa.live0 = live;
        }
    }
    fo.work0 = draws + dispatches;
    const uint64_t present = fo_present.load(std::memory_order_acquire);
    if (!present || r.value != present) return;
    // (the present's own recording: its frame is recorded whole)
    fo_present.store(0, std::memory_order_release);
    fo.frame_working = 0;
    fa.tally.frame_end();
    fo.reads.note(present, fo.last_read, seq), fo.touches.note(present, fo.last_touch, seq);
    fo_st.read_tail += (int64_t)(seq - std::min(fo.last_read, seq)), fo_st.touch_tail += (int64_t)(seq - std::min(fo.last_touch, seq));
}
/// A recording's GPU timestamps as its slot comes round (t: its start and end; null: none), in the order the recordings were made
void fo_collect(const Recording& r, const uint64_t* t) {
    if (!fo_tracking()) return;
    if (!t) {
        fo.last_end = 0;
        return;
    }
    const double period = (double)dev.props.limits.timestampPeriod;
    const FoSlot& s = fo_slots[r.slot];
    uint64_t up = 0;
    if (!fo_up_pool || vkGetQueryPoolResults(dev.device, fo_up_pool, (uint32_t)r.slot, 1, sizeof up, &up, 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        up = 0;
    const frame_overlap::Gap g = frame_overlap::gap(fo.last_end, up, t[0], period);
    fo_st.idle_ns[s.column] += g.idle_ns, fo_st.uploads_ns[s.column] += g.uploads_ns;
    fo.last_end = t[1];
    const int64_t ns = (int64_t)((double)(t[1] - t[0]) * period);
    if (fo.reads.after_release(s.seq)) fo_st.read_tail_ns += ns;
    if (fo.touches.after_release(s.seq)) fo_st.touch_tail_ns += ns;
}
void fo_log(double frames) {
    if (!fo_tracking()) return;
    constexpr int n = frame_overlap::kGapColumns;
    double idle[n], up[n], idle_after = 0, up_after = 0;
    for (int i = 0; i < n; i++) {
        idle[i] = fo_st.idle_ns[i].exchange(0) / 1e6 / frames, up[i] = fo_st.uploads_ns[i].exchange(0) / 1e6 / frames;
        if (i) idle_after += idle[i], up_after += up[i];
    }
    logf("vk: frame overlap (600 presents): the GPU between recordings, ms a frame, idle + in the next one's uploads: up to a "
         "frame's first recording with a draw %.2f + %.2f (the window's copy is in the idle), after it %.2f + %.2f; after a "
         "frame's last recording that reads or writes the game's memory come %.2f recordings (%.2f ms of GPU time), after its "
         "last that reads it %.2f (%.2f ms)",
         idle[0], up[0], idle_after, up_after, fo_st.touch_tail.exchange(0) / frames, fo_st.touch_tail_ns.exchange(0) / 1e6 / frames,
         fo_st.read_tail.exchange(0) / frames, fo_st.read_tail_ns.exchange(0) / 1e6 / frames);
    {   // (frame ahead: counters only)
        frame_overlap::AheadTally t;
        {
            std::lock_guard lk(replay_mu);
            t = fa.tally;
            const int index = t.index;
            const bool three = t.three;
            fa.tally = {};
            fa.tally.index = index, fa.tally.three = three;   // (a frame under way keeps its place)
        }
        const double f = (double)std::max<int64_t>(1, t.frames);
        logf("vk: frame ahead (600 presents): %.2f parts a frame recorded in the signal's job, %.1f%% of them took nothing that "
             "came with the signal; of the others, %.2f a frame took pages of the signal's capture into a mirror (%.1f pages a "
             "frame) and %.2f pages read as guest memory had them (%.1f pages a frame); a frame's first part took nothing in "
             "%lld of %lld frames, its first three in %lld",
             t.parts / f, frame_overlap::final_share(t), t.by_signal / f, t.signal_pages / f, t.by_live / f, t.live_pages / f,
             (long long)t.first_final, (long long)t.frames, (long long)t.first_three_final);
        const int64_t sig = fa_st.signals.exchange(0);
        const double g = (double)std::max<int64_t>(1, sig);
        logf("vk: frame ahead, what came with the signal: %lld signals with a capture, each of %.1f pages in %.1f pools; between a "
             "frame's last submit and that signal %.2f texture and sampler registrations, %.2f releases and %.2f other CPU "
             "signals a signal (%lld of them with any)",
             (long long)sig, fa_st.pages.exchange(0) / g, fa_st.pools.exchange(0) / g, fa_st.regs.exchange(0) / g,
             fa_st.releases.exchange(0) / g, fa_st.other_signals.exchange(0) / g, (long long)fa_st.frames_any.exchange(0));
    }
    // (and where in the frame: before each of its recordings with a draw, the second to the twelfth, then all the later ones)
    char line[512];
    int at = 0;
    for (int i = 1; i < n && at < (int)sizeof line - 24; i++)
        at += snprintf(line + at, sizeof line - at, "%s%.2f+%.2f", i > 1 ? ", " : "", idle[i], up[i]);
    logf("vk: frame overlap, by place: idle + uploads before a frame's 2nd to %dth recording with a draw, then all later ones: %s",
         n - 1, line);
}

/// (longshot) Whether this run has the render thread, from the one place async_on reads: RX_VK_ASYNC when it is set (the
/// switch, for testing, over the menu: 0 the submitting thread, any other value the render thread), else the Insert menu's
/// Threaded rendering as this run resolved it (vk_menu.cpp menu_render_thread: On, or Auto from 4 cores). Read once: a
/// change of the row applies at the next start
int render_thread_mode() {
    static const int mode = [] {
        const char* e = getenv("RX_VK_ASYNC");
        return e ? atoi(e) : menu_render_thread() ? 1 : 0;
    }();
    return mode;
}
/// (longshot) RX_VK_ASYNC=1 (any value but 0): the render thread. Off by default: the replay on the submitting thread
/// (TotK Recomp's reason for it: OoT rewrites double-buffered data right after a submit with no fence, which lands
/// inside a replay running behind).
/// The replay on a thread of its own, with the game let run a frame ahead of it, so a frame costs the larger of the
/// game's work and the replay's, not their sum. The older replay thread (RX_VK_ASYNC=1 before, the render thread =2;
/// removed) could not: the game waited for the replay three times a frame anyway. Its presenting thread polls its
/// frame's event, which the GPU signals at the frame's end, before it presents (20 of a 37 ms frame asleep there,
/// profiled; host/nvn_native.c answers the poll here); its job threads' readbacks caught the replay up (invalidate:
/// 7.5-11 ms each, and the stopped queues forced past their waits, 29789 of 60000: whole-screen white or black frames,
/// 28 per 1000 gameplay frames against 1.3 on the submitting thread); and the occlusion queries were off (2006 draws a
/// frame for 1709). Here:
///  - a submit: the capture and the hand-over, on the game's thread;
///  - a present: handed over; the game waits there while the present before it is not replayed yet (RX_VK_PRESENTS_AHEAD,
///    1): one frame ahead at most;
///  - a readback waits for the write-backs of the work recorded so far, never for the queue (invalidate);
///  - nothing forces a stopped queue but its present and RX_VK_HOLD_MS: a catch-up (a queue finish, a texture read back)
///    waits for what was handed over before it; a release waits for the stopped work, as on the submitting thread;
///  - the occlusion queries stay on (occ_queries_on).
bool async_on() {
    static const bool on = render_thread_mode() != 0;   // (the switch, or the Threaded rendering row: render_thread_mode)
    return on;
}
// (the recorder's half of the Threaded rendering row: with RX_VK_RECORDER unset, vk_recorder.h's mode_from_env asks the row
// through this, set before anything runs; the checks that build vk_recorder.h alone leave it null, the recorder off. The
// recorder thread's half (menu_recorder_thread: On, Auto from 4 cores with mirrored pools, or Auto alone with 6 or more cores
// when only buffer-pool mirrors are absent), not the render thread's: each reader keeps its own half so the rule alone
// decides)
[[maybe_unused]] const bool rec_setting_hooked = (rec::g_setting_thread = menu_recorder_thread, true);

void readback_fell_back(int what) {
    (what == 0 ? bh_st.rb_fallback : what == 1 ? bh_st.tex_page_only : what == 2 ? bh_st.tex_exact : bh_st.rb_exact)++;
}
#ifdef _WIN32
/// (longshot) The logical processors of the cores of the fastest kind in the calling thread's processor group: the highest
/// EfficiencyClass, a hybrid CPU's performance cores; every core on a CPU of one kind (replay_thread_main)
uint64_t fastest_kind_cpus() {
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (!len) return 0;
    std::vector<uint8_t> buf(len);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)buf.data(), &len)) return 0;
    GROUP_AFFINITY ga{};
    const WORD group = GetThreadGroupAffinity(GetCurrentThread(), &ga) ? ga.Group : 0;
    uint64_t mask = 0;
    int best = -1;
    for (DWORD at = 0; at + sizeof(DWORD) * 2 <= len;) {
        const auto* e = (const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)(buf.data() + at);
        if (!e->Size) break;
        if (e->Relationship == RelationProcessorCore && e->Processor.GroupCount && e->Processor.GroupMask[0].Group == group) {
            const int cls = e->Processor.EfficiencyClass;
            const uint64_t m = (uint64_t)e->Processor.GroupMask[0].Mask;
            if (cls > best) best = cls, mask = m;
            else if (cls == best) mask |= m;
        }
        at += e->Size;
    }
    return mask;
}
#endif
void replay_thread_main() {
    SetThreadDescription(GetCurrentThread(), L"mkw_vk replay");
#ifdef TRACY_ENABLE
    HT_THREAD_NAME("mkw_vk replay");
#endif
    fps_render_thread();   // (the FPS counter's render lane is this thread's)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    // (on the core the host keeps free of the game's threads, hos_reserved_cpus: its last physical one)
    // (longshot) Windows: its ideal processor there, and free to go to the other cores of the fastest kind when another
    // program's thread takes it: the game's threads are kept off the core, other programs' are not. Kept to the core alone, a
    // thread at priority 15 (Intel's PresentMon service, which the Intel Graphics Software overlay runs) took its one usable
    // logical processor for 30-40 ms at a time (Windows had parked the other) and the replay waited, ready to run, until it was
    // done: 14-37 frames over 40 ms in each 40 s run in Castle Town's market on an i5-13600K with an Arc B580, none with the
    // thread free to move. (Elsewhere than on Windows: the reserved core alone)
    if (host->reserved_cpus) {
        if (const uint64_t m = host->reserved_cpus()) {
            uint64_t use = m;
#ifdef _WIN32
            {
                DWORD_PTR proc = 0, sys = 0;
                if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys)) proc = 0;
                use |= fastest_kind_cpus() & (uint64_t)proc;
            }
#endif
            // (longshot) Windows refuses a mask with a processor outside the process's, and the thread stays where it was:
            // the log says so, not the placement it did not get (hos_reserved_cpus keeps to the process's cores; a
            // HOS_RESERVE_CORE_MASK outside them still gets here)
            if (!SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)use)) {
                logf("vk: the replay thread was NOT kept to logical processors %#llx (error %lu): it runs where the process "
                     "may, beside the game's threads", (unsigned long long)use, (unsigned long)GetLastError());
            } else {
#ifdef _WIN32
                if (use != m) SetThreadIdealProcessor(GetCurrentThread(), (DWORD)(63 - __builtin_clzll(m)));
#endif
                logf("vk: the replay thread runs on logical processors %#llx, kept from the game's threads%s", (unsigned long long)m,
                     use != m ? " (its ideal processor there; free to go to the other fastest cores when another program takes it)" : "");
                if (use != m) logf("vk: the replay thread may run on logical processors %#llx", (unsigned long long)use);
            }
        }
    }
    // (longshot, the render thread) A present that comes while a queue is stopped at a wait for an event the CPU signals is
    // held until that signal's job, RX_VK_HOLD_MS at most. OoT submits its presentation queue's work before the frame's data
    // is all written, behind such a wait, and signals when it is; its presenting thread then polls the frame's event, which
    // the GPU signals at that work's end, and presents. With the poll answered at once (host/nvn_native.c) the present is
    // handed over before the main thread's signal about every other frame: forced on by it (drain 0), the frame was
    // replayed from the submit's capture, without what the game wrote since, and every one of its waits was gone past
    // (44259 at 3380 presents, measured). The jobs between the present and the signal are replayed meanwhile: none is a
    // submit of the next frame, which the main thread starts after its signal
    Job held_present;
    bool present_held = false;
    int64_t present_held_at = 0;
    auto held_present_run = [&](bool forced) {
#ifdef TRACY_ENABLE
        HT_SCOPE("render job: held present");
#endif
        bh_st.held_ns += bh_now() - present_held_at, bh_st.held_forced += forced;
        present_held = false;
        const int64_t t0 = bh_now();
        drain(0);
        present_value = held_present.value;
        fo_present.store(held_present.value, std::memory_order_release);   // (frame overlap: its recording ends the frame)
        do_present(held_present.has_tex ? &held_present.tex : nullptr, held_present.has_crop ? held_present.crop : nullptr,
                   held_present.output_generation);
        bh_shown(held_present.queued);
        update_blocked();
        bh_st.job_ns[1] += bh_now() - t0;
        if (presents % 3600 == 0) log_order_stats();
        if (presents % 600 == 0) bh_log();
        std::lock_guard lk(q_mu);
        q_presents--, q_space.notify_all();
    };
    for (;;) {
        Job j;
        bool have = false;
        {
            std::unique_lock lk(q_mu);
            FpsRtWait rt_idle(fps_stats::kRtIdle);   // (the FPS counter's render lane: waiting for a job)
            // (gate_ready: the oldest job, once its frame's present is handed over or a game's thread waits for it)
            if (present_held)   // (the signal's job, or on without it)
                q_cv.wait_for(lk, std::chrono::milliseconds(2), [] { return gate_ready(); });
            else if (blocked_n.load())   // (a queue stopped at a wait: the next job, or on without it after a while)
                q_cv.wait_until(lk, blocked_since + std::chrono::milliseconds(hold_ms()), [] { return gate_ready(); });
            else
                while (!gate_ready()) {
                    if (q_jobs.empty()) q_cv.wait(lk);
                    else q_cv.wait_until(lk, q_jobs.front().queued + gate_ms());
                }
            if (gate_ready()) j = std::move(q_jobs.front()), q_jobs.pop_front(), have = true;
            q_busy = true;
        }
        // (stopped ones: on now when stopped too long. No thread's wait for the replay forces them: wait_queue_empty)
        if (blocked_n.load() && std::chrono::steady_clock::now() >= blocked_since + std::chrono::milliseconds(hold_ms())) drain(3);
        if (!have) {
            if (present_held) {
                const bool late = bh_now() - present_held_at >= (int64_t)hold_ms() * 1000000;
                if (late || !cpu_wait_unmet()) held_present_run(late);
            }
            update_blocked();
            std::lock_guard lk(q_mu);
            q_busy = false;
            continue;   // (no job done: nothing for wait_queue_empty's waiters, which wait for q_done)
        }
        // (RX_VK_TIMELINE: one frame's jobs every 240 presents: when the game handed each over, how long it waited
        // for the replay thread, how long it took, its draws; the present's time is 0)
        static const bool timeline = getenv("RX_VK_TIMELINE") != nullptr;
        static const bool timeline_late = timeline && atoi(getenv("RX_VK_TIMELINE")) >= 2;
        const auto t_start = std::chrono::steady_clock::now();
        int64_t d0 = 0, x;
        if (timeline) draw_stats(&d0, &x, &x, &x);
        bool deferred = false;
        // (frame ahead's counters: what is handed over between a frame's last submit and its signal)
#ifdef TRACY_ENABLE
        HT_ZONE(ht_job, "render job");   // (one job of the render thread, from the game's hand-over queue)
        HT_ZONE_TEXTF(ht_job, "%s, value %llu, waited %.3f ms", j.kind == JOB_SUBMIT ? "submit" : j.kind == JOB_PRESENT ? "present" :
                      j.kind == JOB_EVENT_SIGNAL ? "event signal" : "other", (unsigned long long)j.value,
                      std::chrono::duration<double, std::milli>(t_start - j.queued).count());
#endif
        if (j.kind == JOB_SUBMIT) fa_window = {};
        else if (j.kind == JOB_REGISTER_TEXTURE || j.kind == JOB_REGISTER_SAMPLER) fa_window.regs++;
        else if (j.kind == JOB_TEXTURE_RELEASED || j.kind == JOB_PROGRAM_RELEASED) fa_window.releases++;
        switch (j.kind) {
        case JOB_SUBMIT:
            memory_check_expect_capture(j.cap_seq);   // (RX_VK_MEM_CHECK: its take is of the capture its submit made)
            submit_job(j.handles, j.value, j.queue);
            break;
        case JOB_PRESENT:   // (the frame's work whole before it is shown)
            if (present_held) held_present_run(true);   // (two presents ahead: the one before it first)
            if (cpu_wait_unmet()) {
                held_present = std::move(j), present_held = deferred = true, present_held_at = bh_now();
                bh_st.held++;
                break;
            }
            drain(0);
            present_value = j.value;
            fo_present.store(j.value, std::memory_order_release);   // (frame overlap: its recording ends the frame)
            do_present(j.has_tex ? &j.tex : nullptr, j.has_crop ? j.crop : nullptr, j.output_generation);
            bh_shown(j.queued);
            break;
        case JOB_TEXTURE_RELEASED:   // (after the stopped work that may use it)
            if (blocked_n.load()) {   // (kept until it has gone on, not forced: update_blocked)
                std::lock_guard hl(held_mu);
                held_tex_releases.push_back(j.tex);
                st_held_releases++;
                break;
            }
            drain(1);   // (nothing stopped)
            do_texture_released(&j.tex);
            break;
        case JOB_PROGRAM_RELEASED:
            if (blocked_n.load()) {
                std::lock_guard hl(held_mu);
                held_prog_releases.push_back(j.prog);
                st_held_releases++;
                break;
            }
            drain(1);
            do_program_released(&j.prog);
            break;
        case JOB_REGISTER_TEXTURE: register_texture_copy(j.id, j.tex, j.has_view ? &j.view : nullptr); break;
        case JOB_REGISTER_SAMPLER: register_sampler_copy(j.id, j.samp); break;
        case JOB_EVENT_SIGNAL:   // (the CPU set an event: the stopped ones may go on)
            {
                std::lock_guard ek(ev_cpu_mu);
                cpu_events.insert(j.ev_gpu);
            }
            if (j.id) fa_signal_begin();   // (frame ahead's counters: what this capture brings, before it is taken)
            else fa_window.signals++;
            if (j.id) {   // (its capture: what the game wrote up to the signal, for the work that waited for it)
#ifdef TRACY_ENABLE
                HT_SCOPE("signal job: capture");
#endif
                std::lock_guard lk(replay_mu);
                memory_check_expect_capture(j.cap_seq);   // (RX_VK_MEM_CHECK)
                memory_take_capture(j.queue, completed());
                st_resume_captures++;
            }
            ev_values[j.ev_gpu] = j.ev_value;
            burst_begin(0);
            schedule();
            burst_end();
            fa_in_signal.store(false, std::memory_order_release);
            break;
        }
        update_blocked();
        if (j.kind == JOB_PRESENT && !deferred && presents % 3600 == 0) log_order_stats();
        bh_st.job_ns[j.kind == JOB_SUBMIT ? 0 : j.kind == JOB_PRESENT ? 1 : 2] +=
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t_start).count();
        if (j.kind == JOB_PRESENT && !deferred && presents % 600 == 0) bh_log();
        if (present_held && !deferred && !cpu_wait_unmet()) held_present_run(false);
        // (RX_VK_GPU_TIMELINE: the job, handed over, begun and done; S a submit (its value, its command buffers), P a present)
        if (!deferred && (j.kind == JOB_SUBMIT || j.kind == JOB_PRESENT))
            gtl_line(j.kind == JOB_SUBMIT ? 'S' : 'P', j.value, gtl_ns(j.queued), gtl_ns(t_start), gtl_now(), (int64_t)j.handles.size());
        if (timeline && !deferred && (j.kind == JOB_SUBMIT || j.kind == JOB_PRESENT)) {
            struct T { char k; double q, s, e; int64_t d; size_t h; };
            static std::vector<T> frame;
            const auto t_end = std::chrono::steady_clock::now();
            int64_t d1 = 0;
            draw_stats(&d1, &x, &x, &x);
            auto ms = [](auto a) { return std::chrono::duration<double, std::milli>(a.time_since_epoch()).count(); };
            frame.push_back({j.kind == JOB_SUBMIT ? 's' : 'p', ms(j.queued), ms(t_start), ms(t_end), d1 - d0, j.handles.size()});
            if (j.kind == JOB_PRESENT) {
                // (RX_VK_TIMELINE=2: also every frame handed over 20 ms or more after the last one, 200 of them)
                static double last_q = 0;
                static int late_logged = 0;
                const double q_now = frame.back().q;
                const bool late = timeline_late && last_q && q_now - last_q >= 20.0 && late_logged < 200;
                if (late) late_logged++;
                last_q = q_now;
                if (presents % 240 == 0 || late) {
                    const double base = frame.back().q;
                    char line[2048];
                    int n = snprintf(line, sizeof line, "vk: timeline%s (ms from the present's hand-over):",
                                     late ? " of a late frame" : "");
                    for (const T& t : frame)
                        if (n < 1900)
                            n += snprintf(line + n, sizeof line - n, " %c q%+.1f s%+.1f e%+.1f %lldd %zuh;", t.k, t.q - base,
                                          t.s - base, t.e - base, (long long)t.d, t.h);
                    logf("%s", line);
                }
                frame.clear();
            }
        }
        std::lock_guard lk(q_mu);
        if (j.kind == JOB_PRESENT && !deferred) q_presents--, q_space.notify_all();
        q_busy = false;
        q_done++;
        q_idle.notify_all();   // (wait_queue_empty: a job done)
    }
}

void enqueue(Job&& j) {
#ifdef TRACY_ENABLE
    HT_SCOPE("enqueue (game thread)");
#endif
    j.queued = std::chrono::steady_clock::now();
    std::unique_lock lk(q_mu);
    static bool started;
    if (!started) {
        started = true;
        ::mkwvk::cxx_escape::spawn_needed("replay", replay_thread_main);
        logf("vk: the replay runs on a thread of its own, the render thread (RX_VK_ASYNC=0: on the submitting thread)");
    }
    if (j.kind == JOB_PRESENT) {   // (the game waits here while the present before it is not replayed: a frame ahead at most)
        bh_st.depth += (int64_t)q_jobs.size() + (q_busy ? 1 : 0);
        if (q_presents >= presents_ahead()) {
            FpsWait room_wait(fps_stats::kWaitRender);   // (the FPS counter's Breakdown: the render thread a frame behind)
            const int64_t t0 = bh_now();
            q_space.wait(lk, [] { return q_presents < presents_ahead(); });
            bh_st.room_ns += bh_now() - t0, bh_st.room_n++;
        }
        q_presents++;
    }
    if (j.kind == JOB_SUBMIT || j.kind == JOB_PRESENT) j.value = ++q_values;
    if (j.kind == JOB_SUBMIT) gf_note_value(j.value);
    q_handed++;
    if ((j.kind == JOB_SUBMIT || j.kind == JOB_PRESENT) && gate_on()) {   // (a fence of it, waited for before its present: gate_need_value)
        gate_values.emplace_back(j.value, q_handed);
        if (gate_values.size() > 64) gate_values.pop_front();
    }
    q_jobs.push_back(std::move(j));
    q_cv.notify_one();
}

void wait_queue_empty() {
    if (!async_on()) {
        // A catch-up (a queue finish, a CPU read back of what the GPU wrote, a pool remapped) used to force every queue
        // stopped at an event wait past it. The work those waits are for is mostly not submitted yet (the waiting thread is the one
        // that submits it), so the stopped submission went on without it and its lit passes read what that work makes before it
        // was made: the scene black, or grey, or blown out for a frame, every other frame for seconds at a time in Kokiri Forest
        // once the game ran at 60 fps (at 4-8 fps the work had always come first). Measured over 70 s of Kokiri gameplay: 10000-
        // 40000 waits gone past and 250-1100 flashing frames before, ~300 and ~65 now, 60 fps either way. A catch-up now lets on the
        // waits that are met; what stays stopped goes on with its own queue's work, a present, or sync_watch's hold_ms as before.
        if (!blocked_n.load()) return;
        std::lock_guard lk(sync_order_mu);
        burst_begin(0);
        schedule();
        burst_end();
        update_blocked();
        return;
    }
    // (the render thread: what was handed over before this call, replayed as far as its waits allow: nothing forced)
    std::unique_lock lk(q_mu);
    const uint64_t upto = q_handed;
    if (q_done >= upto) return;
    if (gate_on() && upto > gate_upto) gate_upto = upto, gate_st.demands++, q_cv.notify_all();   // (on without their present)
    const int64_t t0 = bh_now();
    q_idle.wait(lk, [upto] { return q_done >= upto; });
    bh_st.catchup_ns += bh_now() - t0, bh_st.catchup_n++;
}
/// (work handed to the replay thread and not yet recorded)
bool replay_behind() {
    if (!async_on()) return blocked_n.load() > 0;
    std::lock_guard lk(q_mu);
    return !q_jobs.empty() || q_busy || blocked_n.load() > 0;
}

std::mutex submit_order_mu;   // (captures and submits queued in the same order)
// ---- (longshot, the render thread) the capture at a CPU signal ----
// A queue stopped at a wait for an event the CPU signals reads what the game wrote between its submit and the signal
// (resume_queue: the fog and exposure of the frame before otherwise). On the submitting thread that capture is taken when the
// queue goes on; behind the game that is too late, the game has gone on writing. Here it is taken on the game's thread,
// at the nvnEventSignal that meets a wait of a submission handed over, and the render thread takes it at the signal's job.
// The waits are read from the command buffers at their submit: the last 256 (~14 a frame). submit_order_mu
struct BhWait {
    uint64_t gpu = 0;
    uint32_t mode = 0, value = 0;
    bool open = false;
};
BhWait bh_waits[256];
size_t bh_waits_n = 0;
std::unordered_map<uint64_t, uint32_t> bh_cpu_value;   // (the events the CPU has signalled, and their last value)
void bh_note_waits(const uint64_t* handles, int n) {
    const int64_t t0 = bh_now();
    for (int cb = 0; cb < n; cb++) {
        if (!handles[cb]) continue;
        for (const uint8_t* o = reinterpret_cast<const uint8_t*>(handles[cb]);; o = op_next(o)) {
            nvn_op_hdr h;
            std::memcpy(&h, o, sizeof h);
            if (h.op == NOP_END) break;
            if (h.op != NOP_WAIT_EVENT) continue;
            const auto w = payload<nvn_p_wait_event>(o + 8, h.size - 8);
            const auto it = bh_cpu_value.find(w.gpu);
            if (it != bh_cpu_value.end() && event_wait_met(it->second, w.mode, w.value)) continue;   // (met already: its submit's capture)
            bh_waits[bh_waits_n++ % 256] = {w.gpu, w.mode, w.value, true};
        }
    }
    bh_st.walk_ns += bh_now() - t0;
}
bool bh_signal_meets(uint64_t gpu, uint32_t value) {
    bh_cpu_value[gpu] = value;
    bool any = false;
    for (BhWait& w : bh_waits)
        if (w.open && w.gpu == gpu && event_wait_met(value, w.mode, w.value)) w.open = false, any = true;
    return any;
}
/// (longshot, the render thread) Whether a submission handed over waits for an event the CPU signals that it has not
/// signalled yet: the frame's data is not all written. submit_order_mu
std::condition_variable bh_signal_cv;
bool bh_cpu_wait_open() {
    for (const BhWait& w : bh_waits) {
        if (!w.open) continue;
        const auto it = bh_cpu_value.find(w.gpu);
        if (it != bh_cpu_value.end() && !event_wait_met(it->second, w.mode, w.value)) return true;
    }
    return false;
}
void submit_q(const uint64_t* handles, int n, uint64_t queue) {
#ifdef TRACY_ENABLE
    HT_SCOPE("rx submit_q (game thread)");
#endif
    FpsRender fr;   // (the FPS counter's Breakdown: the replay, on this thread)
    if (!async_on()) {
        std::lock_guard order(sync_order_mu);
        ReplayPin pin;   // (the capture and the replay on the replay's core)
        // (the replay here is on the submitting thread, but the game's other threads go on writing their next frames: a pool
        // synced from guest memory later in the frame read those. The capture is taken here too)
        // (EvilOOT) on by default: without it every mirror sync read guest memory as it was at the replay, the game's later
        // frames. OoT's GPU culling alternates two list sets: every other frame its setup read the next frame's records,
        // counted into the other set, and the foliage, grass, characters and particles of that frame were never drawn.
        // With it the syncs take the submits' captures, as the console's GPU sees its memory.
        memory_capture_submit();
        memory_check_expect_capture(memory_check_capture_seq());   // (RX_VK_MEM_CHECK: taken below, on this thread)
        // (RX_VK_SUBMIT_DELAY_US=<us> (a test): a console's GPU starts later than this replay does. Does what a submission reads
        // arrive just after it, from the game's other threads?)
        static const int delay_us = getenv("RX_VK_SUBMIT_DELAY_US") ? atoi(getenv("RX_VK_SUBMIT_DELAY_US")) : 0;
        if (delay_us) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(delay_us);
            while (std::chrono::steady_clock::now() < until) std::this_thread::yield();
        }
        if (sync_waits_on()) {
            static std::once_flag watch;
            std::call_once(watch, [] { ::mkwvk::cxx_escape::spawn_needed("order", sync_watch); });
            std::vector<uint64_t> h(handles, handles + std::max(n, 0));
            const uint64_t value = ++q_values;
            gf_note_value(value);
            if (queue) {
                q_last[queue] = value;
                value_queue[value] = queue;
                while (value_queue.size() > 8192) value_queue.erase(value_queue.begin());
            }
            submit_job(h, value, queue);
            update_blocked();
            return;
        }
        return do_submit(handles, n);
    }
    std::lock_guard lk(submit_order_mu);
    memory_capture_submit();   // (the pages as they are now, on the game's thread: vk_memory.cpp)
    bh_note_waits(handles, n);
    Job j{JOB_SUBMIT};
    j.cap_seq = memory_check_capture_seq();   // (RX_VK_MEM_CHECK: 0 unless it is set)
    j.handles.assign(handles, handles + std::max(n, 0));
    j.queue = queue;
    enqueue(std::move(j));
}
void submit(const uint64_t* handles, int n) {
    submit_q(handles, n, 0);
}
void present_tagged(const nvn_tex* t, const int crop[4], uint64_t generation) {
    fps_presented();   // (the FPS counter: the game's frame ends here, on its presenting thread)
#ifdef TRACY_ENABLE
    HT_SCOPE("rx present (game thread)");
#endif
    gf_note_present();
    programs_presented();   // (the programs' life: whether the game is in a load)
    FpsRender fr;
    if (!async_on()) {
        std::lock_guard order(sync_order_mu);
        ReplayPin pin;   // (the frame's last replays and the present on the replay's core)
        if (sync_waits_on()) {
            drain(0);   // (the frame's work whole before it is shown)
            update_blocked();
            present_value = ++q_values;
            return do_present(t, crop, generation);
        }
        return do_present(t, crop, generation);
    }
    {   // (the render thread) The present is handed over after the CPU's signal its frame's work waits for: the main
        // thread makes that signal when it has written the frame's data, and the presenting thread, its poll answered
        // at once, came before it at most presents. With whole frames (gate_on) the frame is then whole at its present,
        // which is where the render thread starts it (gate_ready); without them it stays as it was with them, which is
        // what jobs replayed as they come were measured with. At most RX_VK_FRAME_GATE_MS. RX_VK_PRESENT_AFTER_SIGNAL=0
        // (a test): handed over at once
        static const bool after = env_on_or("RX_VK_PRESENT_AFTER_SIGNAL", true);
        // (as long as the gate gives a job: a frame the game takes longer over is waited for too, not started half written)
        static const auto after_ms = std::max(gate_ms(), std::chrono::milliseconds(hold_ms()));
        if (after) {
            static std::atomic<int64_t> n{0}, waited{0}, ns{0}, late{0};
            std::unique_lock lk(submit_order_mu);
            if (bh_cpu_wait_open()) {
                FpsWait main_wait(fps_stats::kWaitMain);   // (the FPS counter's Breakdown: the game's main thread, not the renderer)
                const int64_t t0 = bh_now();
                if (!bh_signal_cv.wait_for(lk, after_ms, [] { return !bh_cpu_wait_open(); })) late++;
                ns += bh_now() - t0, waited++;
            }
            if (++n % 600 == 0) {
                const int64_t w = waited.exchange(0);
                logf("vk: presents after the CPU's signal (600): %lld waited for it, %.2f ms each, %lld let go after RX_VK_FRAME_GATE_MS",
                     (long long)w, w ? ns.exchange(0) / 1e6 / w : 0.0, (long long)late.exchange(0));
            }
        }
    }
    Job j{JOB_PRESENT};
    j.output_generation = generation;
    if (t) j.tex = *t, j.has_tex = true;
    if (crop) std::memcpy(j.crop, crop, sizeof j.crop), j.has_crop = true;
    enqueue(std::move(j));
}
void texture_released_of(const nvn_tex* t) {
    if (!t) return;
    tex_pages_add(t, -1);
    texture_ahead_gone(t);
    if (!async_on()) {
        // (after the stopped work that may use it, when it goes on. The lock is held_mu, only while queues are
        // stopped: sync_order_mu, taken on every release, would make the thread finalizing textures (OptimizeChangeBank) wait
        // for whole submissions being replayed under it. A release just after the stopped work went on waits
        // for the next update_blocked: the next submit or present)
        if (sync_waits_on() && blocked_n.load()) {
            {
                std::lock_guard hl(held_mu);
                held_tex_releases.push_back(*t);
            }
            if (++st_held_releases % 2000 == 1) logf("vk: releases kept for stopped work: %lld", (long long)st_held_releases.load());
            return;
        }
        sync_drain(1);   // (after the stopped work that may use it)
        return do_texture_released(t);
    }
    Job j{JOB_TEXTURE_RELEASED};
    j.tex = *t, j.has_tex = true;
    enqueue(std::move(j));
}
void program_released_locked(const nvn_prog* p) {
    if (!p) return;
    programs_freed(p);   // (the program's life: the game's call, now; the replay's part follows in order below)
    if (!async_on()) {
        if (sync_waits_on() && blocked_n.load()) {   // (as texture releases, on held_mu)
            {
                std::lock_guard hl(held_mu);
                held_prog_releases.push_back(*p);
            }
            st_held_releases++;
            return;
        }
        sync_drain(1);
        return do_program_released(p);
    }
    Job j{JOB_PROGRAM_RELEASED};
    j.prog = *p;
    enqueue(std::move(j));
}
// ---- texture and sampler registrations: in order with the submits ----
// A registration took effect at once, on the game's thread, while the replay recorded submits up to two presents
// older: the game, which registers the slots of its next frames as it builds them (sets of render targets at each
// dynamic-resolution step), had the frame being recorded sample the next ones' textures, of another size: its
// picture went to pieces below or above a line (edges clamped into stripes, the sky's or fog's colour, the shadows
// dark) for a frame, with the size changes (every few frames when the resolution swung). Now they are jobs, applied
// where the game made them among its submits, as finalized textures already were.
// doc: docs/dev/reference/architecture.md#rules-from-mkw (rule 3: registrations are queued in order with the submits)
void register_texture_queued(int id, const nvn_tex* t, const nvn_view* v) {
    if (!async_on()) return register_texture_locked(id, t, v);
    if (id < 0 || id >= (1 << 20) || !t) return;
    t = (const nvn_tex*)descriptor_ok(t, sizeof(nvn_tex));
    if (!t) return;
    v = (const nvn_view*)descriptor_ok(v, sizeof(nvn_view));
    Job j{JOB_REGISTER_TEXTURE};
    j.id = id, j.tex = *t;
    if (v) j.view = *v, j.has_view = true;
    enqueue(std::move(j));
}
void event_signal_queued(uint64_t gpu, uint32_t value) {
#ifdef TRACY_ENABLE
    HT_SCOPE("rx event_signal (game thread)");
#endif
    if (!async_on()) {
        std::lock_guard lk(ev_cpu_mu);
        ev_cpu_pending.emplace_back(gpu, value);
        ev_cpu_captures[gpu] = memory_captures_begun();   // (after the game's writes for it: cpu_signal_uncaptured)
        ev_cpu_any.store(true, std::memory_order_release);
        ev_cpu_signals.fetch_add(1, std::memory_order_release);   // (after it is in the list: schedule)
        return;   // (sync_watch or the next submit lets the stopped ones on: not this thread, ~1.7 ms a frame of the critical path)
    }
    Job j{JOB_EVENT_SIGNAL};
    j.ev_gpu = gpu, j.ev_value = value;
    std::lock_guard lk(submit_order_mu);
    if (bh_signal_meets(gpu, value)) {
        memory_capture_submit();
        j.cap_seq = memory_check_capture_seq();   // (RX_VK_MEM_CHECK)
        j.id = 1, j.queue = q_values.load();   // (its capture, and the newest value handed out: the take's)
        bh_st.sig_caps++;
    }
    enqueue(std::move(j));
    bh_signal_cv.notify_all();
}
void register_sampler_queued(int id, const nvn_samp_par* p) {
    if (!async_on()) return register_sampler(id, p);
    if (id < 0 || id >= (1 << 12) || !p) return;
    p = (const nvn_samp_par*)descriptor_ok(p, sizeof(nvn_samp_par));
    if (!p) return;
    Job j{JOB_REGISTER_SAMPLER};
    j.id = id, j.samp = *p;
    enqueue(std::move(j));
}
uint64_t fence() { return async_on() || sync_waits_on() ? q_values.load() : pushed.load(); }
uint64_t get_pushed() { return fence(); }


} // namespace

/// (frame overlap, vk_internal.h; vk_draw.cpp push_constants) A draw or a dispatch is recorded now; follows: a dispatch, or a
/// draw whose program follows pointers into the game's memory: its recording reads that memory at the GPU's time
void frame_overlap_op(bool follows) {
    if (fo_tracking()) fo.follows += follows;
}

/// (RX_VK_LOG_EARLY_CHECK, vk_internal.h) a GPU write the replay records now, in place: counted for the pending checks of reads of its
/// submission let go at an earlier part (kind: 0 a binding, a copy or clear; 1 over 1 MiB; 2 a bare or wide storage range)
void early_check_gpu_write(uint64_t lo, uint64_t hi, uint64_t value, uint64_t part, int kind) {
    if (!early_check_on()) return;
    std::lock_guard lk(ec_mu);
    for (auto& e : ec_pending)
        if (e->value == value && part > e->part && lo < e->guest + e->size && e->guest < hi) e->gpu_noted++, e->kinds |= 1 << kind;
}
/// (RX_VK_LOG_EARLY_CHECK, vk_internal.h) pages the soft write watch reported (the CPU's writes); full: memory_collect_written's,
/// over every range, begun as collection gen
void early_check_cpu_pages(const uint32_t* pages, int64_t n, bool full, uint32_t gen) {
    if (!early_check_on()) return;
    std::lock_guard lk(ec_mu);
    for (auto& e : ec_pending) {
        if (e->stage == 2 && e->closed1 && e->closed2) continue;
        const uint64_t m = n > 0 ? ec_pages_hit(e->guest, e->size, pages, n) : 0;
        if (!e->closed1) e->cpu1 |= m;
        if (e->stage >= 1 && !e->closed2) e->cpu2 |= m;
        // (the second full collection begun after a copy: a mark the game's store published late is in one of them)
        if (full && e->stage >= 1 && gen > e->gen_m + 1) e->closed1 = true;
        if (full && e->stage == 2 && gen > e->gen_2 + 1) e->closed2 = true;
    }
    ec_reap();
}
uint32_t early_check_collect_begin() { return early_check_on() ? ++ec_full_begun : 0; }

bool replay_flush_upload(VkCommandBuffer cur) {
    if (!cur) return false;
    Recording* r = nullptr;
    if (rec_cur && rec_cur->upload == cur && !rec_cur->submitted) {
        r = rec_cur;
    } else {
        for (Recording& slot : recordings) {
            if (slot.upload == cur && !slot.submitted) {
                r = &slot;
                break;
            }
        }
    }
    if (!r) return false;
    full_barrier(cur, "barrier (upload chunk)");
    const VkResult end_res = vkEndCommandBuffer(cur);
    if (end_res != VK_SUCCESS) {
        static int failures;
        if (failures++ < 8) logf("vk: upload chunk end failed (%d)", (int)end_res);
        if (end_res == VK_ERROR_DEVICE_LOST) device_lost();
    } else {
        VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cbi.commandBuffer = cur;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &cbi;
        VkResult sub_res = VK_SUCCESS;
        {
            std::lock_guard lk(dev.present_mu);
            queue_flush_locked();
            PROF(PROF_QSUBMIT);
            si.pNext = reflex_submit_id(nullptr);
            VkSemaphoreSubmitInfo sparse_wait;
            if (memory_sparse_wait(&sparse_wait)) si.waitSemaphoreInfoCount = 1, si.pWaitSemaphoreInfos = &sparse_wait;
            sub_res = submit_or_retry(si, r->fence);
        }
        if (sub_res != VK_SUCCESS) {
            static int failures;
            if (failures++ < 8) logf("vk: upload chunk submit failed (%d)", (int)sub_res);
            if (sub_res == VK_ERROR_DEVICE_LOST) device_lost();
        } else {
            FpsRtWait rt_wait(fps_stats::kRtGpu);
            const VkResult w = vkWaitForFences(dev.device, 1, &r->fence, VK_TRUE, UINT64_MAX);
            if (w != VK_SUCCESS) {
                static int failures;
                if (failures++ < 8) logf("vk: upload chunk wait failed (%d)", (int)w);
                if (w == VK_ERROR_DEVICE_LOST) device_lost();
                std::lock_guard lk(dev.present_mu);
                vkQueueWaitIdle(dev.queue);
            }
            if (vkResetFences(dev.device, 1, &r->fence) == VK_ERROR_DEVICE_LOST) device_lost();
        }
    }
    if (vkResetCommandPool(dev.device, r->upload_pool, 0) == VK_ERROR_DEVICE_LOST) device_lost();
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(r->upload, &bi) == VK_ERROR_DEVICE_LOST) device_lost();
    return true;
}

bool replay_start() {
    menu_init();
    if (opts.reshade) reshade::select(opts.reshade);
    if (!memory_init() || !vma_init(instance_proc())) return false;
    {   // (longshot) the guest page table made now, before a shader is translated: the stages that read global memory through it
        // alone (mma_layout.h graphics_table_loads, mesh_fallback_pages) would read address 0 without it, so without one they
        // search the storage buffers instead
        using namespace Shader::Backend::SPIRV;
        if ((graphics_table_loads || mesh_fallback_pages) && !memory_guest_pages()) {
            graphics_table_loads = 0, mesh_fallback_pages = false;
            logf("vk: no guest page table: global loads search the storage buffers (RX_VK_GP_TABLE and RX_VK_MESH_PAGES not taken)");
        }
    }
    VkSemaphoreTypeCreateInfo ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo tsi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    tsi.pNext = &ti;
    if (vkCreateSemaphore(dev.device, &tsi, nullptr, &timeline) != VK_SUCCESS) return false;
    if (vkCreateSemaphore(dev.device, &tsi, nullptr, &batch_tl) != VK_SUCCESS) batch_tl = VK_NULL_HANDLE;
    if (vkCreateSemaphore(dev.device, &tsi, nullptr, &rec_timeline) != VK_SUCCESS) rec_timeline = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = dev.queue_family;
    pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    int slot = 0;
    for (Recording& r : recordings) {
        r.slot = slot++;
        VkFenceCreateInfo rfi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(dev.device, &rfi, nullptr, &r.fence) != VK_SUCCESS) return false;
        if (vkCreateCommandPool(dev.device, &pi, nullptr, &r.pool) != VK_SUCCESS) return false;
        if (vkCreateCommandPool(dev.device, &pi, nullptr, &r.upload_pool) != VK_SUCCESS) return false;
        ai.commandPool = r.pool;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(dev.device, &ai, &r.cmd);
        ai.commandPool = r.upload_pool;
        vkAllocateCommandBuffers(dev.device, &ai, &r.upload);
    }
    ai.commandBufferCount = 1;
    for (Frame& f : frames) {
        if (vkCreateCommandPool(dev.device, &pi, nullptr, &f.pool) != VK_SUCCESS) return false;
        ai.commandPool = f.pool;
        vkAllocateCommandBuffers(dev.device, &ai, &f.cmd);
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(dev.device, &fi, nullptr, &f.done);
    }
    {   // (the swapchain image's own fence: present_image waits for it on the CPU, image_free)
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(dev.device, &fi, nullptr, &acquire_fence) != VK_SUCCESS) return false;
    }
    logf("vk: replay ready (%s; push descriptors %s (%u), extended dynamic state 3 %s, custom border colours %s)",
         dev.offscreen ? "offscreen" : "window", dev.push_descriptor ? "yes" : "no", dev.max_push_descriptors,
         dev.eds3_blend ? "yes" : "no", dev.custom_border_color ? "yes" : "no");
    return true;
}

/// (longshot) The host's camera jitter (nx2/oot_jitter.py, host/rx_backend.c): the scene's size while a temporal upscaler
/// draws, NVIDIA DLSS or AMD FSR 4 (vk_temporal.cpp) or AMD FSR 3.1 (vk_fsr3_inputs.cpp), whichever it is (one at a time: the
/// Upscaler row); 0, no jitter, otherwise
void jitter_sample(MkwVkJitter* out) {
    *out = temporal_jitter_sample();
    if (!out->width) *out = fsr3_inputs::jitter_sample();
}

int jitter_option(int id) {
    const int axis = id - HOS_OPT_DLSS_JITTER_W;
    const uint32_t d = temporal_jitter_size(axis);
    return (int)(d ? d : fsr3_inputs::jitter_size(axis));
}

void fill_replay_table(MkwVkRenderer& t) {
    t.stats = stats;
    t.nvn_submit = submit;
    t.nvn_submit_q = submit_q;
    t.nvn_fence_q = fence_q;
    t.nvn_fence = fence;
    t.nvn_pushed = get_pushed;
    t.nvn_replayed = get_replayed;
    t.nvn_wait = wait;
    t.wait_writebacks_reading = wait_writebacks_reading;
    t.nvn_finish = finish;
    t.nvn_present = [](const nvn_tex* tex, const int crop[4]) { present_tagged(tex, crop, 0); };
    t.nvn_present_trial = present_tagged;
    t.nvn_register_texture = register_texture_queued;
    t.nvn_event_signal = event_signal_queued;
    t.option = [](int id) -> int {   // (3: the fixed path's scale in 1/10000, vk_menu.cpp; DLAA: full scale, vk_dlss.cpp)
        if (id == 11) return opts.stick_dead_zone;
        if (id == HOS_OPT_WALK_MODE) return opts.walk_mode;
        if (id == HOS_OPT_WALK_SPEED) return opts.walk_speed;
        if (id == HOS_OPT_CHEAT_HEARTS) return menu_heart_protection();
        if (id == HOS_OPT_CHEAT_FLIGHT) return menu_player_flight();
        if (id == HOS_OPT_CHEAT_ACTION) return menu_take_cheat_action();
        if (id == HOS_OPT_CHEAT_CAMERA) return menu_free_camera();
        if (id == HOS_OPT_CHEAT_SPEED) return menu_link_speed();
        if (id == HOS_OPT_HDR_NATIVE) return dev.hdr && opts.hdr == 2;
        if (id == HOS_OPT_HDR_TONEMAP) return scene_tonemap_on();
        if (id == HOS_OPT_HDR_PEAK) return std::clamp(opts.hdr_peak, 400, 10000);
        if (id == HOS_OPT_HDR_WHITE) return std::clamp(opts.hdr_white, 80, 500);
        if (id == HOS_OPT_DISCORD) return opts.discord != 0;
        if (id == HOS_OPT_VOLUME) return opts.volume;
        if (id == HOS_OPT_AUDIO_OUTPUT) return opts.audio_output;
        if (id == HOS_OPT_MUTE_BACKGROUND) return opts.mute_background;
        if (id == HOS_OPT_DISABLE_CAMERA_CENTERING) return opts.disable_camera_centering;
        if (id == HOS_OPT_SPRINT) return opts.sprint;
        if (id == HOS_OPT_SPRINT_TOGGLE)
            return host && host->version >= 40 && host->binding ? host->binding(HOS_BIND_SPRINT_TOGGLE, 0, 0) : 0;
        if (id == HOS_OPT_TEXT_SIZE) return opts.text_size;
        if (id == HOS_OPT_CAMERA_SPEED) return opts.camera_speed;
        if (id == HOS_OPT_PROMPTS) return opts.prompts;
        if (id == HOS_OPT_FOV) return opts.fov;
        if (id == HOS_OPT_TRANSITION_AUTOSAVE) return menu_transition_autosave();
        if (id == HOS_OPT_SHADOW_RES) return menu_shadow_res();
        if (id == HOS_OPT_DAMAGE_TAKEN) return menu_damage_taken();
        if (id == HOS_OPT_SKIP_OPENING) return opts.skip_opening;
        if (id == HOS_OPT_FPS_CAP) return opts.fps_cap;
        if (id == HOS_OPT_HOLD_TOGGLE) return opts.hold_toggle;
        if (id == HOS_OPT_MOUSE_INVERT_X) return opts.mouse_invert_x;
        if (id >= HOS_OPT_HUD_LAYOUT && id <= HOS_OPT_HUD_MINIMAP) return menu_hud_option(id);
        if (id == HOS_OPT_WIDE_MENUS) return opts.wide_menus;
        if (id == HOS_OPT_SKIP_TITLE) return opts.skip_title != 0;
        if (id == HOS_OPT_PAD_SPEAKER_VOLUME) return opts.pad_speaker_volume;
        if (id == HOS_OPT_PAD_SPEAKER_SYNC) return opts.pad_speaker_sync;
        if (id == HOS_OPT_PAD_SPEAKER_HEADPHONES) return opts.pad_speaker_headphones;
        if (id == HOS_OPT_PAD_SPEAKER_HEADPHONE_VOLUME) return opts.pad_speaker_headphone_volume;
        if (id >= HOS_OPT_PAD_SPEAKER_CLASS && id < HOS_OPT_PAD_SPEAKER_CLASS + 9)   // (HOS_OPT_PAD_SPEAKER_OF: 141 to 149, then 150 to 152)
            return opts.pad_speaker[id - HOS_OPT_PAD_SPEAKER_CLASS];
        if (id >= HOS_OPT_PAD_SPEAKER_CLASS2 && id < HOS_OPT_PAD_SPEAKER_CLASS2 + HOS_PAD_SPEAKER_CLASS_OPTIONS - 9)
            return opts.pad_speaker[9 + id - HOS_OPT_PAD_SPEAKER_CLASS2];
        if (id == HOS_OPT_RENDER_THREAD) return async_on();   // (host/nvn_native.c: the present poll answered at once)
        if (id == HOS_OPT_NATIVE_ZSTD) return native_zstd_default.load(std::memory_order_acquire);   // (host/hos_zstd.c)
        /* (longshot) Vibration (host/vibration.h), and what plays the controller used last as the host wrote it back */
        if (id == HOS_OPT_VIBRATION) return opts.vibration;
        if (id == HOS_OPT_DS_HD_HAPTICS) return opts.ds_hd_haptics;
        if (id == HOS_OPT_VIBRATION_STATUS) return menu_vibration_status_option();
        return id == 0 ? opts.realtime : id == 1 ? menu_is_open() : id == 2 ? resolution_auto() && !temporal_full_scale() : id == 3 ? (temporal_full_scale() ? 10000 : resolution_scale()) : id == 4 ? opts.pad_background : id == 5 ? opts.text_speed : id == 6 ? opts.mouse_native :
               id == 7 ? opts.mouse_sens : id == 8 ? opts.mouse_invert : id == 9 ? opts.rumble : id == 16 ? opts.invert_cam_x : id == 17 ? opts.invert_cam_y : id == HOS_OPT_GYRO ? opts.gyro : id == HOS_OPT_GYRO_SENS_X ? opts.gyro_sens_x : id == HOS_OPT_GYRO_SENS_Y ? opts.gyro_sens_y : id == HOS_OPT_GYRO_INV_X ? opts.gyro_inv_x : id == HOS_OPT_GYRO_INV_Y ? opts.gyro_inv_y : id == HOS_OPT_GYRO_DEADZONE ? opts.gyro_deadzone :
               id == HOS_OPT_DLSS_JITTER_W || id == HOS_OPT_DLSS_JITTER_H || id == HOS_OPT_DLSS_JITTER_PHASES ? jitter_option(id) : 0;
    };
    t.option_set = menu_option_set;
    t.jitter_sample = jitter_sample;
    t.frame_info = frame_info;
    t.shot = shot_ask;
    t.setting = menu_setting;
    t.setting_set = menu_setting_set;
    t.nvn_register_sampler = register_sampler_queued;
    t.nvn_set_texture_pool_size = set_texture_pool_size;
    t.nvn_texture_released = texture_released_of;
    t.nvn_program_released = program_released_locked;
    t.nvn_program_created = programs_created;
    programs_life_on(), programs_load_at_create();
    t.nvn_read_texels = read_texels;
    t.nvn_write_texels = write_texels;
    t.nvn_invalidate = invalidate;
    t.nvn_invalidate_quick = invalidate_quick;
    t.nvn_pool_created = memory_pool_created;
    t.nvn_pool_finalized = memory_pool_finalized;
    t.guest_imported = memory_guest_imported;
    t.nvn_vmap_changing = vmap_changing;
    t.nvn_texture_created = texture_created_of;
    t.output_preview_stage = [](const nvn_tex* targets) {
        const char* test = getenv("RX_VK_OUTPUT_PREVIEW_TEST");
        const char mode = !test ? 'k' : !strcmp(test, "no-frame") ? 'n' : !strcmp(test, "allocation") ? 'g' : 'k';
        return request_output_probe_backing(targets, mode, true);
    };
    t.output_preview_retired = [](uint64_t last_serial) -> int {
        return output_preview_retired_serial.load(std::memory_order_acquire) == last_serial;
    };
    t.output_trial_phase = output_trial_phase;
    t.output_trial_publish = output_trial_publish;
}

} // namespace mkwvk
