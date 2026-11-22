// RX_VK_RECORDER_PARTS, the hand-off by part, against a fake Vulkan (tests/test_recorder_parts.py builds it with the real vk_work.h,
// vk_recorder.h and vk_internal.h's QueueMutex): the recorder thread takes a whole part and makes its end and submit itself, so the
// producer never waits for it at a part's end, and what needs a part submitted waits for that. Deterministic: the recorder is held
// in a gated packet instead of timed, and a watchdog (3 s) ends a wait that should not happen, as a failure.
//
//   recorder_parts_test <case>    exits 0 when the case holds, 1 (with a FAIL line) when it does not
//
//   handoff  two parts posted while the recorder is held in the first's packets: the producer neither waits at the part's
//            end nor at replay_mu's release; the driver sees each part's calls, then its end and submit, on the recorder thread,
//            in order; a wait for the second part returns only once it is submitted
//   settle   a fence wait on another thread reaches the driver only after the part handed over before it is submitted
//   queue    a thread about to take the queue's lock waits for the parts handed over; a thread holding it while the recorder
//            waits for it at a part's submit does not wait for the recorder (no deadlock), and the part goes on once it is let go
//   slot     a slot whose part is submitted resets its pool with another packet still in flight (OwnSlot without
//            RX_VK_RECORDER_OWN)
//   timeout  a wait for a part with a deadline comes back unmet in time and owes the next part's submit one flush of what is held
//            (RX_VK_BATCH: a waiter's flush that came before its part was held); a poll does not wait, and owes none
//   target   that flush is owed by the part the waiter waited for: the parts submitted before it do not send it
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "vk_work.h"

namespace mkwvk {
thread_local uint64_t work_epoch = 0;   // (vk_work.h's counts, as vk_texture.cpp defines them)
thread_local bool other_work_since_barrier = true;
bool gprof_glue_on() { return false; }   // (vk_work.h's profiler glue, off as with RX_VK_PASS_TIMING under 3)
GprofGlue::GprofGlue(VkCommandBuffer c, const char* l) : cmd(c), label(l) {}
GprofGlue::~GprofGlue() {}
const char* const call_kind_names[kCallKinds] = {};   // (vk_work.h's call counts, off as without RX_VK_CALL_COUNT)
std::atomic<uint64_t> call_counts[kCallKinds];
bool call_count_on() { return false; }
void logf(const char* fmt, ...) {
    char b[1024];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    std::printf("LOG: %s\n", b);
}
#include "queue_mutex.inc"   // (vk_internal.h's queue_lock_hook and QueueMutex, cut from it by the Python side)
} // namespace mkwvk
namespace rec = mkwvk::rec;

// ---- the fake driver: every call a no-op, but for those below, which log what reached it, in order, and on which thread
template <class F>
struct Fake;
template <class R, class... A>
struct Fake<R(VKAPI_PTR*)(A...)> {
    static R VKAPI_CALL call(A...) {
        if constexpr (!std::is_void_v<R>) return R{};
    }
};
namespace vkx {
#define RX_FAKE(f) PFN_##f f = Fake<PFN_##f>::call;
RX_REC_CALLS(RX_FAKE, RX_FAKE)
RX_FAKE(vkBeginCommandBuffer) RX_FAKE(vkEndCommandBuffer) RX_FAKE(vkResetCommandPool) RX_FAKE(vkQueueSubmit2) RX_FAKE(vkWaitForFences)
RX_FAKE(vkGetQueryPoolResults) RX_FAKE(vkDestroyBuffer) RX_FAKE(vkDestroyDevice) RX_FAKE(vkGetSemaphoreCounterValue)
RX_FAKE(vkWaitSemaphores) RX_FAKE(vkGetQueueCheckpointDataNV) RX_FAKE(vkGetDeviceFaultInfoEXT) RX_FAKE(vkResetDescriptorPool)
RX_FAKE(vkDestroyDescriptorPool) RX_FAKE(vkResetFences)
#undef RX_FAKE
} // namespace vkx

static std::mutex g_mu;
static std::condition_variable g_cv;
static bool g_gate_open = true;          // (a line width of kGated runs only once the gate is open)
static std::vector<std::string> g_log;   // (what reached the driver, in order)
static std::thread::id g_main_id;
static bool g_off_main = true;           // (every end and submit made off the main thread: the recorder's)
constexpr float kGated = 7.0f;

static void note(const std::string& what) {
    std::lock_guard lk(g_mu);
    g_log.push_back(what);
}
static std::string name(const void* h) { return std::to_string((unsigned)(uintptr_t)h); }
static void VKAPI_CALL fake_line(VkCommandBuffer cb, float w) {
    if (w == kGated) {
        std::unique_lock lk(g_mu);
        g_cv.wait(lk, [] { return g_gate_open; });
    }
    note("line " + name(cb) + " " + std::to_string((int)w));
}
static VkResult VKAPI_CALL fake_end(VkCommandBuffer cb) {
    if (std::this_thread::get_id() == g_main_id) g_off_main = false;
    note("end " + name(cb));
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL fake_submit(VkQueue, uint32_t, const VkSubmitInfo2* si, VkFence) {
    if (std::this_thread::get_id() == g_main_id) g_off_main = false;
    note("submit " + name(si->pCommandBufferInfos[0].commandBuffer));
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL fake_wait_fences(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t) {
    note("fence wait");
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL fake_reset_pool(VkDevice, VkCommandPool p, VkCommandPoolResetFlags) {
    note("reset " + name(p));
    return VK_SUCCESS;
}
static void VKAPI_CALL fake_destroy_buffer(VkDevice, VkBuffer b, const VkAllocationCallbacks*) { note("destroy " + name(b)); }

static void gate(bool open) {
    {
        std::lock_guard lk(g_mu);
        g_gate_open = open;
    }
    g_cv.notify_all();
}
static std::vector<std::string> log_now() {
    std::lock_guard lk(g_mu);
    return g_log;
}
static int at(const std::vector<std::string>& l, const std::string& what) {
    for (size_t i = 0; i < l.size(); i++)
        if (l[i] == what) return (int)i;
    return -1;
}
[[noreturn]] static void pass(const std::string& c) {   // (_Exit: the recorder thread may still wait on this file's mutexes)
    std::printf("PASS %s\n", c.c_str());
    std::fflush(stdout);
    std::_Exit(0);
}
[[noreturn]] static void fail(const std::string& what) {
    std::printf("FAIL: %s\n", what.c_str());
    for (const std::string& l : log_now()) std::printf("  driver: %s\n", l.c_str());
    std::fflush(stdout);
    std::_Exit(1);
}
static void watchdog(const char* what) {   // (3 s, then the case fails: a wait that should not happen)
    std::thread([what] {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        fail(std::string("watchdog: ") + what);
    }).detach();
}
static bool soon(bool (*ok)()) {   // (up to 2 s)
    for (int i = 0; i < 200; i++) {
        if (ok()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return ok();
}

static const VkCommandBuffer kA = (VkCommandBuffer)(uintptr_t)0xA0, kB = (VkCommandBuffer)(uintptr_t)0xB0,
                             kC = (VkCommandBuffer)(uintptr_t)0xC0;
static const VkDevice kDev = (VkDevice)(uintptr_t)0x20;
static const VkQueue kQueue = (VkQueue)(uintptr_t)0x30;
static rec::DrainingMutex replay_mu;
static mkwvk::QueueMutex queue_mu;   // (the replay's queue's: the one a part's submit takes, as dev.present_mu)

static std::atomic<int> g_owed[3] = {-1, -1, -1};   // (target: each part's take_flush_owed(), as part_submit asks it)
/// A part's end and submit, as vk_replay.cpp's part_submit makes them: through vk_work.h's macros, under the queue's lock
struct PartEnd {
    VkCommandBuffer cb;
    int idx;   // (-1, or where it keeps whether its submit owed a flush)
    void operator()() const {
        if (idx >= 0) g_owed[idx] = rec::take_flush_owed() ? 1 : 0;
        vkEndCommandBuffer(cb);
        VkCommandBufferSubmitInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        ci.commandBuffer = cb;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.commandBufferInfoCount = 1, si.pCommandBufferInfos = &ci;
        std::lock_guard lk(queue_mu);
        vkQueueSubmit2(kQueue, 1, &si, VK_NULL_HANDLE);
    }
};
/// (the producer) a part of cb: its line widths, then its end and submit handed over; the part's number
static uint64_t part(VkCommandBuffer cb, std::initializer_list<float> widths, int idx = -1) {
    for (float w : widths) vkCmdSetLineWidth(cb, w);
    const uint64_t t = rec::post_part(PartEnd{cb, idx});
    if (!t) fail("post_part did not take the part");
    return t;
}
static void warm_up() {   // (the recorder started, idle)
    replay_mu.lock();
    vkCmdSetLineWidth(kC, 1.0f);
    rec::t_prod->flush();
    replay_mu.unlock();
    if (!soon([] { return rec::g_rec.R.load() == rec::g_rec.W.load(); })) fail("the recorder did not start");
    std::lock_guard lk(g_mu);
    g_log.clear();
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string c = argv[1];
    g_main_id = std::this_thread::get_id();
    vkx::vkCmdSetLineWidth = fake_line;
    vkx::vkEndCommandBuffer = fake_end;
    vkx::vkQueueSubmit2 = fake_submit;
    vkx::vkWaitForFences = fake_wait_fences;
    vkx::vkResetCommandPool = fake_reset_pool;
    vkx::vkDestroyBuffer = fake_destroy_buffer;
    rec::g_stats_log = false;
    rec::g_parts = true;
    rec::set_mode(rec::THREAD);
    mkwvk::queue_lock_hook = rec::queue_lock_event;
    warm_up();

    if (c == "handoff") {
        watchdog("the producer waited for the recorder at a part's end, or a part was never submitted");
        gate(false);
        replay_mu.lock();
        const uint64_t a = part(kA, {1.0f, kGated});   // (the recorder held in it)
        const uint64_t b = part(kB, {2.0f});           // (made while the first part's packets have not all run)
        replay_mu.unlock();                            // (and no wait here either)
        if (rec::g_rec.prod_n.load()) fail("the producer waited for the recorder");
        if (rec::g_rec.parts_done.load()) fail("a part counted done while its packets were held");
        std::atomic<bool> back{false};
        std::thread t([&] { rec::wait_part(b), back = true; });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (back) fail("the wait for the second part came back before it was submitted");
        gate(true);
        t.join();
        const std::vector<std::string> l = log_now();
        const std::vector<std::string> want = {"line 160 1", "line 160 7", "end 160", "submit 160", "line 176 2", "end 176", "submit 176"};
        if (l != want) fail("the driver did not see each part's calls, then its end and submit, in order");
        if (!g_off_main) fail("a part's end or submit made on the producer, not the recorder");
        if (rec::g_rec.parts_done.load() < b || a + 1 != b) fail("the parts' numbers");
        pass(c);
    }
    if (c == "settle") {
        watchdog("the fence wait never came back");
        gate(false);
        replay_mu.lock();
        part(kA, {kGated});
        replay_mu.unlock();
        std::thread t([] {   // (a thread without replay_mu)
            const VkFence f = (VkFence)(uintptr_t)0xF0;
            vkWaitForFences(kDev, 1, &f, VK_TRUE, 0);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (at(log_now(), "fence wait") >= 0) fail("the fence wait reached the driver before the part's submit");
        gate(true);
        t.join();
        const std::vector<std::string> l = log_now();
        if (!(0 <= at(l, "submit 160") && at(l, "submit 160") < at(l, "fence wait"))) fail("the fence wait came before the part's submit");
        pass(c);
    }
    if (c == "queue") {
        watchdog("a deadlock between the queue's lock and the recorder");
        // (1) the lock's taker waits for the part handed over before it
        gate(false);
        replay_mu.lock();
        part(kA, {kGated});
        replay_mu.unlock();
        std::atomic<bool> locked{false};
        std::thread q([&] {
            std::lock_guard lk(queue_mu);
            note("locked");
            locked = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (locked) fail("the queue's lock was taken before the part handed over was submitted");
        gate(true);
        q.join();
        {
            const std::vector<std::string> l = log_now();
            if (!(0 <= at(l, "submit 160") && at(l, "submit 160") < at(l, "locked"))) fail("the queue's lock taken before the part's submit");
        }
        // (2) a holder of the lock does not wait for a recorder that waits for it
        queue_mu.lock();
        std::thread p([] {   // (the producer, its part's submit to wait for the lock held above)
            replay_mu.lock();
            part(kB, {2.0f});
            vkCmdSetLineWidth(kC, 3.0f);   // (after the part: published only at the next one, or a boundary)
            rec::t_prod->flush();
            replay_mu.unlock();
        });
        p.join();
        if (!soon([] { return rec::g_rec.at_queue.load(); })) fail("the recorder did not come to the queue's lock");
        vkDestroyBuffer(kDev, (VkBuffer)(uintptr_t)0xD0, nullptr);   // (a boundary without replay_mu: waits for what was published)
        if (!rec::g_rec.escapes.load()) fail("the wait under the queue's lock was not given up");
        queue_mu.unlock();
        if (!soon([] { return rec::g_rec.parts_done.load() >= 2; })) fail("the part did not go on once the lock was let go");
        pass(c);
    }
    if (c == "slot") {
        watchdog("a slot's own reset waited for the recorder held in another packet");
        replay_mu.lock();
        const uint64_t a = part(kA, {1.0f});
        const uint64_t end_pos = rec::produced();   // (as post_part_submit keeps it)
        rec::wait_part(a);
        gate(false);
        vkCmdSetLineWidth(kC, kGated);   // (in flight, the recorder held in it)
        rec::t_prod->flush();
        rec::slot_begin(end_pos);
        {
            rec::OwnSlot own;
            vkResetCommandPool(kDev, (VkCommandPool)(uintptr_t)0xE0, 0);
        }
        rec::slot_done();
        if (at(log_now(), "reset 224") < 0) fail("the slot's reset did not reach the driver with a packet in flight");
        gate(true);
        replay_mu.unlock();
        pass(c);
    }
    if (c == "timeout") {
        watchdog("a wait with a deadline did not come back");
        gate(false);
        replay_mu.lock();
        const uint64_t a = part(kA, {kGated});
        replay_mu.unlock();
        (void)rec::take_flush_owed();
        const auto t0 = std::chrono::steady_clock::now();
        if (rec::g_rec.wait_part(a, rec::B_PART, 50000000ull)) fail("a wait for a held part came back met");
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms < 45 || ms > 1000) fail("a 50 ms wait came back after " + std::to_string(ms) + " ms");
        if (!rec::take_flush_owed()) fail("a wait that ended unmet owed no flush to the next part's submit");
        if (rec::take_flush_owed()) fail("the flush owed twice");
        if (rec::settle_parts(rec::B_QUEUE_LOCK, 0)) fail("a poll of the parts came back met while one was held");
        if (rec::flush_owed()) fail("a poll owed a flush (the game polls many times a frame: every part would send the batch)");
        gate(true);
        if (!rec::wait_part(a)) fail("the part not submitted once let go");
        if (rec::take_flush_owed()) fail("a met wait owed a flush");
        pass(c);
    }
    if (c == "target") {
        watchdog("the parts were not submitted");
        gate(false);
        replay_mu.lock();
        part(kA, {kGated}, 0);
        part(kB, {2.0f}, 1);
        const uint64_t c3 = part(kC, {3.0f}, 2);
        replay_mu.unlock();
        (void)rec::take_flush_owed();
        if (rec::g_rec.wait_part(c3, rec::B_PART, 30000000ull)) fail("a wait for a held part came back met");
        gate(true);
        if (!rec::wait_part(c3)) fail("the parts not submitted once let go");
        if (g_owed[0] != 0 || g_owed[1] != 0) fail("a part before the one waited for sent the flush owed");
        if (g_owed[2] != 1) fail("the part waited for did not send the flush owed");
        if (rec::flush_owed()) fail("the flush still owed after the part waited for");
        pass(c);
    }
    fail("no such case: " + c);
}
