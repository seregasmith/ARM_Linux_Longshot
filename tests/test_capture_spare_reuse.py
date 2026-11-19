"""Exercise capture-buffer reuse through cap_buf_get. Run python -I -B tests/test_capture_spare_reuse.py."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from srccut import block


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default="c++")
    args = parser.parse_args()
    source = (ROOT / "gpu/vk/vk_memory.cpp").read_text()
    harness = r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <vector>
using VkBuffer = unsigned;
using VmaAllocation = void*;
constexpr uint64_t MB = 1ull << 20;
std::mutex cap_free_mu;
struct FreeBuf { VkBuffer buffer; VmaAllocation alloc; void* map; uint64_t size; };
std::vector<FreeBuf> cap_free;
uint64_t cap_free_bytes;
std::vector<uint64_t> warm;
static void cap_prewarm_class(uint64_t size) { warm.push_back(size); }
static unsigned driver_allocations;
static bool refuse;
static uint64_t shortfall, budget = UINT64_MAX;
static uint64_t linux_pressure_shortfall() { return shortfall; }
static uint64_t cap_spare_max() { return budget; }
static bool env_on(const char*) { return false; }
static unsigned cap_usage() { return 1; }
static bool create_buffer_cached(uint64_t, unsigned, VkBuffer& b, VmaAllocation& a, void** m, bool) {
    ++driver_allocations;
    if (refuse) return false;
    b = 2; a = *m = &warm; return true;
}
""" + block(source, "uint64_t cap_buf_size(") + "\n" + block(source, "bool cap_buf_get(") + r"""
int main() {
    VkBuffer b = 0;
    VmaAllocation a = nullptr;
    void* m = nullptr;
    uint64_t size = 0;
    cap_free = {{1, &warm, &warm, 128*MB}};
    cap_free_bytes = 128*MB;
    assert(cap_buf_get(4*MB, b, a, m, size) && m);
    assert(size == 4*MB && cap_free_bytes == 128*MB);
    const unsigned before = driver_allocations;
    warm.clear();
    assert(cap_buf_get(110*MB, b, a, m, size) && b == 1 && size == 128*MB);
    assert(cap_free_bytes == 0 && driver_allocations == before);
    assert(std::count(warm.begin(), warm.end(), 128*MB) == 1);
    // If memory is tight, the exact same oversized spare remains usable.
    cap_free = {{3, &warm, &warm, 64*MB}};
    cap_free_bytes = 64*MB;
    refuse = true;
    assert(cap_buf_get(3*MB, b, a, m, size) && b == 3 && size == 64*MB);
    assert(cap_free.empty() && cap_free_bytes == 0);
    assert(!cap_buf_get(3*MB, b, a, m, size));
    refuse = false;
    for (int i = 0; i < 2; ++i) {
        shortfall = i == 0 ? MB : 0;
        budget = i == 0 ? UINT64_MAX : 32*MB;
        cap_free = {{4, &warm, &warm, 32*MB}};
        cap_free_bytes = 32*MB;
        const unsigned count = driver_allocations;
        assert(cap_buf_get(2*MB, b, a, m, size) && b == 4 && cap_free.empty());
        assert(driver_allocations == count);
    }

    // A tight spare budget should leave room for other common sizes alongside a 17-28 MiB capture.
    budget = 32*MB;
    shortfall = 0;
    for (uint64_t need : {16*MB+1, 20*MB, 20*MB+1, 24*MB, 24*MB+1, 28*MB, 28*MB+1, 32*MB}) {
        assert(cap_buf_get(need, b, a, m, size));
        assert(size == ((need+4*MB-1)/(4*MB))*(4*MB));
    }
    assert(cap_buf_size(32*MB+1) == 64*MB);
    assert(cap_buf_size(16*MB) == 16*MB);
    budget = UINT64_MAX;
    assert(cap_buf_get(16*MB+1, b, a, m, size) && size == 32*MB);

}
"""
    # Hold a real prewarm worker inside its allocator while a retirement or pressure
    # sample arrives. The release path and worker accounting are the production ones.
    worker = r"""
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>
using VkBuffer = unsigned;
using VmaAllocation = void*;
constexpr unsigned VK_NULL_HANDLE = 0;
constexpr uint64_t MB = 1ull << 20;
struct FreeBuf { VkBuffer buffer; VmaAllocation alloc; void* map; uint64_t size; };
std::vector<FreeBuf> cap_free;
std::mutex cap_free_mu, gate;
std::condition_variable cap_workers_done, gate_cv;
std::atomic<bool> cap_shutting_down{false};
std::atomic<uint64_t> shortfall{0};
uint64_t cap_reserved_bytes, cap_free_bytes;
int cap_prewarm_busy;
bool cap_prewarm_class_busy[4], entered, finish;
std::atomic<unsigned> destroyed{0};
static uint64_t cap_spare_max() { return 64*MB; }
static uint64_t linux_pressure_shortfall() { return shortfall.load(); }
static void destroy_buffer(VkBuffer, VmaAllocation) { ++destroyed; }
static bool env_on(const char*) { return false; }
static unsigned cap_usage() { return 1; }
static void logf(const char*, ...) {}
static bool create_buffer_cached(uint64_t, unsigned, VkBuffer& b, VmaAllocation& a, void** m, bool) {
    std::unique_lock lk(gate);
    entered = true;
    gate_cv.notify_all();
    gate_cv.wait(lk, [] { return finish; });
    b = 2; a = *m = &gate; return true;
}
""" + "\n".join(block(source, marker) for marker in (
        "void cap_buf_free(", "void cap_exit_note(", "int cap_prewarm_index(",
        "void cap_spare_make(", "void cap_prewarm_class(")) + r"""
int main() {
    {
        std::lock_guard lk(cap_free_mu);
        cap_prewarm_class(64*MB);
        cap_prewarm_class(32*MB);
        assert(cap_prewarm_busy == 1 && cap_reserved_bytes == 64*MB);
    }
    {
        std::unique_lock lk(gate);
        gate_cv.wait(lk, [] { return entered; });
    }
    cap_buf_free(1, &gate, &gate, 8*MB);
    assert(destroyed == 1);
    shortfall = MB;
    {
        std::lock_guard lk(gate);
        finish = true;
        gate_cv.notify_all();
    }
    {
        std::unique_lock lk(cap_free_mu);
        while (cap_prewarm_busy) cap_workers_done.wait_for(lk, std::chrono::milliseconds(10));
        assert(destroyed == 2 && cap_reserved_bytes == 0 && cap_free.empty());
        cap_prewarm_class(32*MB);
        assert(cap_prewarm_busy == 0);
    }
}
"""
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder)
        for program in (harness, worker):
            (path / "test.cpp").write_text(program)
            exe = path / "test.exe"
            subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                            str(path / "test.cpp"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=15)


if __name__ == "__main__":
    main()
