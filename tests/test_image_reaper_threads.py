"""The image reaper (gpu/vk/vk_vma.cpp destroy_image_later, RX_VK_REAPER) run on threads against stub VMA and Vulkan calls, with
the textures' compaction passes (image_compact_pass, image_compact_pass_end) beside it. No GPU or game is involved.

Cut out of gpu/vk/vk_vma.cpp unmodified: the compaction's section and the reaper's. The stubs count every destroy, the bytes
create_image's count still holds (destroy_image takes them off), and any free made while a pass is open. Checked:

- overlap: 3000 images handed over from one thread while another opens and ends passes and asks the queue back (reap_now):
  no free inside a pass, every image, view, buffer and memory object destroyed once, the counts back to 0;
- hand-over: with each free taking 2 ms, 40 hand-overs take a fraction of the 80 ms the frees take;
- cap: with the frees slow, what the queue holds stays under its cap (an 8th of a 4 GB allowance, 512 MB); the rest is
  destroyed where it was handed over;
- a pass open: images handed over wait (the queue then is over its cap, as main's dead list waited), reap_now gives nothing,
  and they go once the pass ends;
- reap_now: near the allowance the queue is freed on the asking thread while the reaper is busy, until what goes back to the
  driver (dedicated allocations, in the ledger asked) covers what was asked; shares of blocks give nothing back, so all of
  them go and it says 0; an entry is taken for its image's or its buffer's ledger (a buffer alone, mixed ledgers);
- RX_VK_REAPER=0: destroyed at once, on the thread that handed them over.

python -I -B tests/test_image_reaper_threads.py [--cxx g++] [--vma <a vk_vma.cpp>]

--vma reads another vk_vma.cpp (the negative control: one whose reaper frees inside a pass, or keeps no count, fails).
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#define VK_NULL_HANDLE nullptr
enum VkResult { VK_SUCCESS = 0, VK_INCOMPLETE = 5 };
struct Obj { int id; bool ram = false, dedicated = false; };   // (an allocation's: in system memory; a memory object of its own)
typedef Obj* VkDevice;
typedef Obj* VkImage;
typedef Obj* VkImageView;
typedef Obj* VkBuffer;
typedef Obj* VkDeviceMemory;
typedef Obj* VmaAllocation;
typedef Obj* VmaPool;
typedef Obj* VmaAllocator;
typedef Obj* VmaDefragmentationContext;
struct VmaDefragmentationMove { VmaAllocation srcAllocation; VmaAllocation dstTmpAllocation; int operation; };
#define VMA_DEFRAGMENTATION_MOVE_OPERATION_IGNORE 2
struct VmaDefragmentationPassMoveInfo { uint32_t moveCount; VmaDefragmentationMove* pMoves; };
struct VmaDefragmentationInfo { int flags; VmaPool pool; uint64_t maxBytesPerPass; uint32_t maxAllocationsPerPass; };
struct VmaDefragmentationStats { uint64_t bytesMoved, bytesFreed; uint32_t allocationsMoved, deviceMemoryBlocksFreed; };
struct ImageMove { VmaAllocation src; };
#define SetThreadDescription(t, name) ((void)0)
#define GetCurrentThread() 0

static Obj the_device{1}, the_pool{2}, the_ctx{3}, the_vma{4};
static VmaAllocator vma = &the_vma;
static VmaPool texture_pool = &the_pool;
static struct { VkDevice device = &the_device; } dev;
constexpr uint64_t kMB = 1ull << 20;

// ---- the stubs' counts ----
static std::mutex stub_mu;
static std::multiset<const Obj*> destroyed;     // (every image, view, buffer and memory object destroyed: once each)
static std::atomic<bool> vma_in_pass{false};    // (between vmaBeginDefragmentationPass's VK_INCOMPLETE and its end)
static std::atomic<int> frees_running{0}, overlaps{0};
static std::atomic<int64_t> image_bytes_held{0}, buffer_bytes_held{0};
static std::atomic<int> free_us{0};
static std::atomic<int> freed_on_main{0}, freed_elsewhere{0};
static std::thread::id main_id;
static std::atomic<bool> gate_closed{false};    // (the reaper's thread waits in its free while it is set)
static int reaper_switch = 1;
static std::string log_text;

#define logf fixture_logf   // (not <cmath>'s logf)
static void logf(const char* f, ...) {
    char line[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(line, sizeof line, f, ap);
    va_end(ap);
    std::lock_guard lk(stub_mu);
    log_text += line, log_text += "\n";
}
static bool env_on_or(const char*, int) { return reaper_switch != 0; }
static bool vram_usage(uint64_t* used, uint64_t* allowed) { return *used = 0, *allowed = 4096 * kMB, true; }
static uint64_t allocation_size(VmaAllocation a) { return a ? kMB : 0; }
static bool allocation_in_vram(VmaAllocation a) { return a && !a->ram; }
static bool allocation_dedicated(VmaAllocation a) { return a && a->dedicated; }
static void destroyed_one(const Obj* o) {
    if (vma_in_pass.load()) overlaps++;
    std::lock_guard lk(stub_mu);
    destroyed.insert(o);
}
static void spin_us(int us) {   // (sleep_for is a millisecond and more on Windows)
    const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
    while (std::chrono::steady_clock::now() < end) {}
}
static void a_free() {   // (what a free costs, and who made it)
    if (std::this_thread::get_id() == main_id) freed_on_main++;
    else {
        freed_elsewhere++;
        while (gate_closed.load()) std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    if (free_us.load()) spin_us(free_us.load());
}
static void destroy_image(VkImage i, VmaAllocation a) {
    frees_running++;
    a_free();
    destroyed_one(i), destroyed_one(a);
    image_bytes_held -= (int64_t)allocation_size(a);
    frees_running--;
}
static void destroy_buffer(VkBuffer b, VmaAllocation a) {
    destroyed_one(b), destroyed_one(a);
    buffer_bytes_held -= (int64_t)allocation_size(a);
}
static void vkDestroyImageView(VkDevice, VkImageView v, const void*) { destroyed_one(v); }
static void vkFreeMemory(VkDevice, VkDeviceMemory m, const void*) { destroyed_one(m); }

static int passes_left = 0;
static VmaDefragmentationMove the_move{};
static VkResult vmaBeginDefragmentation(VmaAllocator, const VmaDefragmentationInfo*, VmaDefragmentationContext* c) {
    *c = &the_ctx;
    return VK_SUCCESS;
}
static VkResult vmaBeginDefragmentationPass(VmaAllocator, VmaDefragmentationContext, VmaDefragmentationPassMoveInfo* p) {
    if (frees_running.load()) overlaps++;   // (a free of the reaper's still running as the pass begins)
    if (passes_left-- <= 0) return VK_SUCCESS;
    vma_in_pass = true;
    p->moveCount = 1, p->pMoves = &the_move;
    return VK_INCOMPLETE;
}
static VkResult vmaEndDefragmentationPass(VmaAllocator, VmaDefragmentationContext, VmaDefragmentationPassMoveInfo*) {
    if (frees_running.load()) overlaps++;
    vma_in_pass = false;
    return passes_left > 0 ? VK_INCOMPLETE : VK_SUCCESS;
}
static void vmaEndDefragmentation(VmaAllocator, VmaDefragmentationContext, VmaDefragmentationStats* s) { *s = {}; }
static VkResult vmaBindImageMemory(VmaAllocator, VmaAllocation, VkImage) { return VK_SUCCESS; }

/* COMPACTION */

/* REAPER */

// (the process ends at once, as the game's does on Linux: host/posix/win32_posix.c ExitProcess is _exit. A return from main
// runs the static destructors under the detached reaper, which waits on reap_cv: glibc's destructor of it blocks for good,
// after a failed check as after a passed one, and the result is lost with the process killed)
[[noreturn]] static void finish(int code) { std::fflush(stdout); std::_Exit(code); }
#define CHECK(c, what) do { if (!(c)) { std::printf("FAIL: %s\n", what); finish(1); } } while (0)

// (images made for a case: an image, its allocation, 2 views, every 3rd a buffer and its allocation, every 5th a memory object)
struct Made {   // (a case's own: the record of what was destroyed starts again with it, as its objects may take a past case's addresses)
    Made() { std::lock_guard lk(stub_mu); destroyed.clear(); }
    std::deque<Obj> objs;
    size_t n = 0, views = 0, buffers = 0, memory = 0;
    Obj* obj() { objs.push_back({(int)objs.size()}); return &objs.back(); }
};
static void hand_over(Made& m) {
    VkImage image = m.obj();
    VmaAllocation alloc = m.obj();
    std::vector<VkImageView> views{m.obj(), m.obj()};
    VkBuffer buffer = nullptr;
    VmaAllocation buffer_alloc = nullptr;
    std::vector<VkDeviceMemory> memory;
    if (m.n % 3 == 0) buffer = m.obj(), buffer_alloc = m.obj(), m.buffers++, buffer_bytes_held += (int64_t)kMB;
    if (m.n % 5 == 0) memory.push_back(m.obj()), m.memory++;
    image_bytes_held += (int64_t)kMB;
    m.n++, m.views += 2;
    destroy_image_later(image, alloc, std::move(views), buffer, buffer_alloc, std::move(memory));
}
struct Entry { bool image = false, image_ram = false, dedicated = false, buffer = false, buffer_ram = false, buffer_dedicated = false; };
static void hand(Made& m, Entry e) {   // (one entry as asked, nothing else with it)
    VkImage image = nullptr;
    VmaAllocation alloc = nullptr, buffer_alloc = nullptr;
    VkBuffer buffer = nullptr;
    if (e.image) image = m.obj(), alloc = m.obj(), alloc->ram = e.image_ram, alloc->dedicated = e.dedicated, image_bytes_held += (int64_t)kMB;
    if (e.buffer) {
        buffer = m.obj(), buffer_alloc = m.obj(), buffer_alloc->ram = e.buffer_ram, buffer_alloc->dedicated = e.buffer_dedicated;
        buffer_bytes_held += (int64_t)kMB;
    }
    destroy_image_later(image, alloc, {}, buffer, buffer_alloc, {});
}
static bool drained(int seconds = 20);
static bool all_once(const Made& m);
static bool plug(Made& m) {   // (the reaper held in a free of its own, so what is handed over next stays queued)
    free_us = 0, freed_on_main = 0, freed_elsewhere = 0;
    gate_closed = true;
    hand(m, {.image = true});
    for (int i = 0; i < 2000 && freed_elsewhere.load() == 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return freed_elsewhere.load() == 1;
}
static bool unplug(const Made& m) {
    gate_closed = false;
    return drained() && all_once(m) && image_bytes_held == 0 && buffer_bytes_held == 0;
}
static bool drained(int seconds) {
    for (int i = 0; i < seconds * 1000 && images_to_reap(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return !images_to_reap();
}
static bool all_once(const Made& m) {
    std::lock_guard lk(stub_mu);
    for (const Obj& o : m.objs)
        if (destroyed.count(&o) != 1) return false;
    return true;
}
static void held(uint64_t* images, uint64_t* buffers) {
    uint64_t i[2], b[2];
    reaper_bytes(i, b);
    *images = i[0] + i[1], *buffers = b[0] + b[1];
}
static bool pass_open() {
    passes_left = 1;
    std::vector<ImageMove> moves;
    return image_compact_begin(64 * kMB, 12) && image_compact_pass(moves, nullptr, nullptr);
}
static void pass_end() { image_compact_pass_end(nullptr, nullptr); }

int main(int argc, char** argv) {
    main_id = std::this_thread::get_id();
    if (argc > 1 && !std::strcmp(argv[1], "off")) {   // (RX_VK_REAPER=0)
        reaper_switch = 0;
        Made m;
        for (int i = 0; i < 20; i++) hand_over(m);
        CHECK(!images_to_reap() && all_once(m), "with RX_VK_REAPER=0 the images were not destroyed at once");
        CHECK(freed_on_main == 20 && !freed_elsewhere, "with RX_VK_REAPER=0 an image was destroyed on another thread");
        CHECK(log_text.find("images destroyed on the render thread, as before (RX_VK_REAPER=0)") != std::string::npos,
              "RX_VK_REAPER=0 is not logged");
        CHECK(log_text.find("thread of their own") == std::string::npos, "with RX_VK_REAPER=0 the reaper started");
        CHECK(image_bytes_held == 0 && buffer_bytes_held == 0, "with RX_VK_REAPER=0 the counts did not go back to 0");
        std::printf("image reaper, off: ok\n");
        return 0;
    }
    {   // (overlap: passes, reap_now and 3000 hand-overs at once; each free 20 us)
        Made m;
        free_us = 20;
        std::atomic<bool> done{false};
        std::thread producer([&] {
            for (int i = 0; i < 3000; i++) {
                hand_over(m);
                if (i % 50 == 0) spin_us(300);
            }
            done = true;
        });
        int passes = 0;
        while (!done.load()) {
            passes_left = 3;
            std::vector<ImageMove> moves;
            if (image_compact_begin(64 * kMB, 12))
                while (image_compact_pass(moves, nullptr, nullptr)) {
                    passes++;
                    spin_us(200);
                    image_compact_pass_end(nullptr, nullptr);
                }
            reap_now(4 * kMB, true);
        }
        producer.join();
        CHECK(passes > 10, "the case opened too few passes to say anything");
        CHECK(drained(), "the queue did not drain");
        CHECK(overlaps == 0, "a free was made while a compaction pass was open");
        CHECK(all_once(m), "an image, view, buffer or memory object was not destroyed exactly once");
        uint64_t i, b;
        held(&i, &b);
        CHECK(i == 0 && b == 0, "reaper_bytes is not 0 once the queue drained");
        CHECK(image_bytes_held == 0 && buffer_bytes_held == 0, "an image's or a buffer's bytes stayed counted");
        CHECK(log_text.find("images are destroyed on a thread of their own") != std::string::npos, "the reaper's start is not logged");
    }
    {   // (hand-over: 40 images, each free 2 ms: the hand-overs do not wait for the frees)
        Made m;
        free_us = 2000;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 40; i++) hand_over(m);
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ms < 40, "40 hand-overs waited for the frees (80 ms of them)");
        CHECK(drained() && all_once(m), "the hand-over case did not drain");
    }
    {   // (cap: 700 images of 1 MB at once, the frees slow: the queue holds at most its cap, 512 MB; the rest go here)
        Made m;
        free_us = 500;
        freed_on_main = 0;
        uint64_t most = 0;
        for (int i = 0; i < 700; i++) {
            hand_over(m);
            uint64_t img, buf;
            held(&img, &buf);
            most = std::max(most, img + buf);
        }
        CHECK(most <= 512 * kMB + 2 * kMB, "the queue held more than its cap");
        CHECK(freed_on_main > 0, "past its cap nothing was destroyed where it was handed over");
        CHECK(drained() && all_once(m), "the cap case did not drain");
        CHECK(image_bytes_held == 0 && buffer_bytes_held == 0, "the cap case left bytes counted");
    }
    {   // (a pass open: what is handed over waits, past the cap too; reap_now gives nothing; all of it goes once the pass ends)
        Made m;
        free_us = 0;
        freed_on_main = 0;
        CHECK(pass_open(), "the case's pass did not open");
        for (int i = 0; i < 200; i++) hand_over(m);
        CHECK(images_to_reap() == 200, "an image was destroyed, or left the queue, while a pass was open");
        CHECK(!reap_now(64 * kMB, true), "reap_now freed something while a pass was open");
        CHECK(overlaps == 0, "a free was made while the case's pass was open");
        pass_end();
        CHECK(drained() && all_once(m), "the images handed over in a pass did not go after it");
        CHECK(overlaps == 0, "a free overlapped the case's pass");
    }
    {   // (reap_now, dedicated images in VRAM: the reaper held in its first free; 6 queued; 3 MB asked back on this thread)
        Made m;
        CHECK(plug(m), "the reaper did not take the case's first image");
        for (int i = 0; i < 6; i++) hand(m, {.image = true, .dedicated = true});
        CHECK(reap_now(3 * kMB, true) == 3 * kMB, "reap_now did not give back what was asked, from dedicated images");
        CHECK(freed_on_main == 3, "reap_now freed more or fewer than it needed, or not on the asking thread");
        CHECK(images_to_reap() == 4, "the rest of the queue went with reap_now");
        CHECK(!reap_now(64 * kMB, false) && freed_on_main == 3, "reap_now freed VRAM images when system memory was asked");
        CHECK(unplug(m), "the dedicated reap_now case did not drain");
    }
    {   // (reap_now, shares of blocks: they give the driver nothing back, so all of them go and it says 0)
        Made m;
        CHECK(plug(m), "the reaper did not take the case's first image");
        for (int i = 0; i < 4; i++) hand(m, {.image = true});
        CHECK(reap_now(10 * kMB, true) == 0, "reap_now counted shares of blocks as given back to the driver");
        CHECK(freed_on_main == 4 && images_to_reap() == 1, "reap_now left shares of blocks in the queue under pressure");
        CHECK(unplug(m), "the shares' reap_now case did not drain");
    }
    {   // (reap_now by either allocation's ledger: a system-memory image with a dedicated VRAM buffer, a dedicated VRAM buffer
        // alone, and a dedicated system-memory image alone)
        Made m;
        CHECK(plug(m), "the reaper did not take the case's first image");
        hand(m, {.image = true, .image_ram = true, .buffer = true, .buffer_dedicated = true});
        hand(m, {.buffer = true, .buffer_dedicated = true});
        hand(m, {.image = true, .image_ram = true, .dedicated = true});
        CHECK(reap_now(100 * kMB, true) == 2 * kMB, "reap_now did not give back the VRAM buffers (mixed, alone), or counted RAM");
        CHECK(images_to_reap() == 2, "a VRAM request took the system-memory image, or left a VRAM buffer queued");
        CHECK(reap_now(100 * kMB, false) == kMB && images_to_reap() == 1, "reap_now did not give back the system-memory image");
        CHECK(unplug(m), "the ledgers' reap_now case did not drain");
    }
    std::printf("image reaper, on threads: ok\n");
    finish(0);
}
"""

START_COMPACTION = "// ---- the textures' pool compacted"
END_COMPACTION = "\n// (mem_account.h) what every image made here takes"
START_REAPER = "// ---- (longshot) Images destroyed on a thread of their own"
END_REAPER = "\nuint64_t allocation_size(VmaAllocation allocation) {"


def cut(text, start, end):
    i = text.index(start)
    return text[i:text.index(end, i)]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--vma", type=Path, default=ROOT / "gpu/vk/vk_vma.cpp")
    a = p.parse_args()
    source = a.vma.read_text(encoding="utf-8")
    fixture = (FIXTURE.replace("/* COMPACTION */", cut(source, START_COMPACTION, END_COMPACTION))
               .replace("/* REAPER */", cut(source, START_REAPER, END_REAPER)))
    with tempfile.TemporaryDirectory(prefix="longshot-image-reaper-") as temp:
        cpp, exe = Path(temp) / "fixture.cpp", Path(temp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                        "-Wno-unused-function", "-Wno-unused-variable", "-pthread", str(cpp), "-o", str(exe)], check=True)
        for mode in ([], ["off"]):
            r = subprocess.run([str(exe)] + mode, timeout=120)
            if r.returncode:
                sys.exit(r.returncode)


if __name__ == "__main__":
    main()
