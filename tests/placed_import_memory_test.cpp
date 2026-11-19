// The imports of the guest's pools placed in the GPU's own memory (gpu/vk/vk_memory.cpp placed_*, the arm64 port), run against
// a stand-in driver that counts memory as Turnip (Mesa's Adreno driver) does: every memory object at its buffer object's whole
// size, an import of a dma-buf of a buffer object it already holds counted once more, and a memory object refused once the
// count would pass its heap (VK_ERROR_OUT_OF_DEVICE_MEMORY). Memfds are its buffer objects, mapped shared over the pool as a
// dma-buf is, and the window is made as host/hos_main.c makes it. tests/test_placed_import_memory.py pastes the code it cuts
// out of vk_memory.cpp where ACTUAL is. Linux; no game, no GPU.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "placed_memory.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fixture {

int failures = 0;
void expect(bool ok, const char* what, ...) {
    if (ok) return;
    va_list ap;
    va_start(ap, what);
    std::printf("FAIL: ");
    std::vprintf(what, ap);
    std::printf("\n");
    va_end(ap);
    failures++;
}

template <class T> T handle(uintptr_t n) { return reinterpret_cast<T>(n); }
uintptr_t next_handle = 0x1000;
constexpr uint64_t MB = 1ull << 20;

// ---- the driver: buffer objects (memfds), memory objects over them, buffers bound to those ----
struct Bo {
    int fd;
    uint64_t size;
    ino_t ino;
    int objects;   // (the memory objects over it: it goes with the last)
};
std::vector<Bo*> bos;                     // (live)
std::map<VkDeviceMemory, Bo*> memories;   // (live memory objects)
struct Buf {
    uint64_t size;
    VkDeviceMemory memory;
};
std::map<VkBuffer, Buf> buffers;   // (live buffers: memory VK_NULL_HANDLE until bound)
uint64_t heap_size = UINT64_MAX, heap_used = 0;

Bo* bo_of(int fd) {
    struct stat st {};
    if (fstat(fd, &st) != 0) return nullptr;
    for (Bo* b : bos)
        if (b->ino == st.st_ino) return b;
    return nullptr;
}

VkResult vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo* ai, const VkAllocationCallbacks*, VkDeviceMemory* out) {
    const VkImportMemoryFdInfoKHR* im = nullptr;
    for (auto* s = static_cast<const VkBaseInStructure*>(ai->pNext); s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR) im = reinterpret_cast<const VkImportMemoryFdInfoKHR*>(s);
    Bo* bo = nullptr;
    bool made = false;
    if (im) {
        bo = bo_of(im->fd);
        expect(bo != nullptr, "an import of a dma-buf this driver did not make");
        if (!bo) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    } else {
        const int fd = (int)syscall(SYS_memfd_create, "bo", 1u /* MFD_CLOEXEC */);
        struct stat st {};
        if (fd < 0 || ftruncate(fd, (off_t)ai->allocationSize) != 0 || fstat(fd, &st) != 0) {
            if (fd >= 0) close(fd);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        bo = new Bo{fd, ai->allocationSize, st.st_ino, 0};
        made = true;
    }
    // (as Turnip: each memory object counted at its buffer object's whole size, an import of one it holds already too, and
    // refused once the count would pass the heap)
    if (heap_used + bo->size > heap_size) {
        if (made) close(bo->fd), delete bo;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    if (made) bos.push_back(bo);
    if (im) close(im->fd);   // (an import that succeeds owns its fd)
    heap_used += bo->size;
    bo->objects++;
    *out = handle<VkDeviceMemory>(next_handle++);
    memories[*out] = bo;
    return VK_SUCCESS;
}
void vkFreeMemory(VkDevice, VkDeviceMemory m, const VkAllocationCallbacks*) {
    if (!m) return;
    const auto it = memories.find(m);
    expect(it != memories.end(), "a memory object freed that is not live (freed twice)");
    if (it == memories.end()) return;
    for (const auto& [b, buf] : buffers)
        if (buf.memory == m) expect(false, "a memory object freed under a live buffer bound to it (an import the GPU may still read)");
    Bo* const bo = it->second;
    memories.erase(it);
    heap_used -= bo->size;
    if (--bo->objects == 0) {
        close(bo->fd);
        bos.erase(std::find(bos.begin(), bos.end(), bo));
        delete bo;
    }
}
VkResult vkGetMemoryFdKHR(VkDevice, const VkMemoryGetFdInfoKHR* gi, int* fd) {
    const auto it = memories.find(gi->memory);
    expect(it != memories.end(), "a memory object exported that is not live");
    if (it == memories.end()) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    *fd = dup(it->second->fd);   // (a new dma-buf fd of the same buffer object)
    return *fd >= 0 ? VK_SUCCESS : VK_ERROR_TOO_MANY_OBJECTS;
}
VkResult vkCreateBuffer(VkDevice, const VkBufferCreateInfo* ci, const VkAllocationCallbacks*, VkBuffer* out) {
    const auto* eb = static_cast<const VkExternalMemoryBufferCreateInfo*>(ci->pNext);
    // (VUID-vkBindBufferMemory-memory-02726 and -02985: a buffer bound to exportable or imported dma-buf memory says so)
    expect(eb && eb->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO &&
               (eb->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT),
           "an import's buffer made without the dma-buf handle type");
    *out = handle<VkBuffer>(next_handle++);
    buffers[*out] = {ci->size, VK_NULL_HANDLE};
    return VK_SUCCESS;
}
void vkGetBufferMemoryRequirements(VkDevice, VkBuffer b, VkMemoryRequirements* req) {
    req->size = (buffers.at(b).size + 4095) & ~4095ull;
    req->alignment = 4096;
    req->memoryTypeBits = 1;
}
VkResult vkBindBufferMemory(VkDevice, VkBuffer b, VkDeviceMemory m, VkDeviceSize offset) {
    const auto it = memories.find(m);
    expect(it != memories.end(), "a buffer bound to a memory object that is not live");
    if (it == memories.end()) return VK_ERROR_UNKNOWN;
    Buf& buf = buffers.at(b);
    expect(offset + buf.size <= it->second->size, "a buffer bound past its memory's end");
    buf.memory = m;
    return VK_SUCCESS;
}
VkDeviceAddress vkGetBufferDeviceAddress(VkDevice, const VkBufferDeviceAddressInfo* ai) {
    expect(buffers.count(ai->buffer) && buffers.at(ai->buffer).memory, "the address of a buffer not bound");
    return 0x100000000ull;
}
void vkDestroyBuffer(VkDevice, VkBuffer b, const VkAllocationCallbacks*) {
    expect(buffers.erase(b) == 1, "a buffer destroyed that is not live");
}

// ---- what vk_memory.cpp's code around them is ----
void logf(const char*, ...) {}
struct Dev {
    VkDevice device = VK_NULL_HANDLE;
} dev;
uint8_t* window = nullptr;
uint64_t window_size = 256 * MB;
struct Host {
    uint8_t* guest_base() { return window; }
    uint64_t guest_size() { return window_size; }
} host_value;
Host* host = &host_value;
uint64_t max_import_size = UINT64_MAX;
std::atomic<uint64_t> pools_gen{1};
constexpr VkBufferUsageFlags kUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
std::atomic<bool> placed_on{true};
struct Pool {
    uint64_t lo = 0, hi = 0;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::vector<VkDeviceMemory> pieces;
    uint64_t placement = 0;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    uint64_t retired_value = 0;
};
void release_texel_buffer_views(VkBuffer) {}
using VmaAllocation = void*;
void destroy_buffer(VkBuffer b, VmaAllocation) { vkDestroyBuffer(dev.device, b, nullptr); }
struct DeadMirror {
    VkBuffer buffer;
    VmaAllocation alloc;
    uint64_t value;
};
std::deque<DeadMirror> dead_mirrors;
/* DEAD IMPORT */
std::deque<DeadImport> dead_imports;
uint64_t mem_completed = 0;
uint64_t st_retired_imports_freed = 0, st_retired_import_bytes_freed = 0, dead_import_bytes = 0;
std::deque<std::map<uint64_t, Pool>::node_type> evicted_pools;   // (idle imports given back: imports_give_back frees them)
uint64_t evicted_import_bytes = 0;

/* ACTUAL */

// ---- the cases ----
uint64_t counted_mb() { return heap_used / MB; }
void place(uint64_t lo, uint64_t hi) {   // (memory_pool_created's)
    std::lock_guard lk(placed_mu);
    placed_back(lo, hi);
}
void unplace(uint64_t lo, uint64_t hi) {   // (memory_pool_finalized's)
    std::lock_guard lk(placed_mu);
    placed_unback(lo, hi);
}
// (retired, and handed to reap_mirrors as retired_imports_give_back hands it: freed once the GPU is past `value`)
void retire(Pool& p, uint64_t value) {
    dead_imports.push_back({p.buffer, p.memory, value, p.lo, p.hi - p.lo, std::move(p.pieces)});
    dead_import_bytes += p.hi - p.lo;
    p = Pool{};
}
void reap(uint64_t completed) {
    mem_completed = completed;
    reap_mirrors();
}
void nothing_left(const char* after) {
    expect(placements.empty() && memories.empty() && bos.empty() && buffers.empty() && heap_used == 0 && dead_imports.empty(),
           "%s: %zu placements, %zu memory objects, %zu buffer objects, %zu buffers and %llu MB counted left", after,
           placements.size(), memories.size(), bos.size(), buffers.size(), (unsigned long long)counted_mb());
    placed_gone.clear();   // (as placed_retire_gone takes them)
    placed_gone_any = false;
}

// A placed pool imported, twice (the first import retired and waiting for the GPU while the second is made): the driver counts
// the placement's memory once. Before, each import was the placement's memory imported again, counted at the placement's size
// once more: 3.5 GB of imports over 5.1 GB of placements in Kokiri Forest took Turnip's count to its 11 GB heap
void counted_once() {
    const uint64_t lo = 16 * MB, hi = lo + 32 * MB;
    place(lo, hi);
    expect(counted_mb() == 32, "a 32 MB pool placed: %llu MB counted", (unsigned long long)counted_mb());
    Pool a, b;
    expect(placed_import(lo, hi, a), "the placed pool not imported");
    expect(counted_mb() == 32, "an import of a placed pool counted by the driver once more: %llu MB counted for 32 MB placed",
           (unsigned long long)counted_mb());
    retire(a, 1);
    expect(placed_import(lo, hi, b), "the placed pool not imported again while its first import waits for the GPU");
    expect(counted_mb() == 32, "two imports of one placement: %llu MB counted for 32 MB placed", (unsigned long long)counted_mb());
    reap(1);
    retire(b, 2);
    reap(2);
    unplace(lo, hi);
    nothing_left("a placement and its two imports given back");
}

// Where the count meets the heap, as in Kokiri Forest ("no memory of the device's to place 576 KB of guest memory in
// (VkResult -2)", images not made): a 32 MB placement and its import, then a 16 MB pool, under a 64 MB heap. 48 MB is held
void at_the_heap() {
    heap_size = 64 * MB;
    const uint64_t lo = 64 * MB, mid = lo + 32 * MB, hi = mid + 16 * MB;
    place(lo, mid);
    Pool a;
    expect(placed_import(lo, mid, a), "a 32 MB placement's import refused under a 64 MB heap");
    place(mid, hi);
    expect(placements.size() == 2, "a 16 MB pool refused a placement with 48 MB placed under a 64 MB heap: %llu MB counted",
           (unsigned long long)counted_mb());
    retire(a, 3);
    reap(3);
    unplace(lo, mid);
    unplace(mid, hi);
    nothing_left("two placements and an import given back under a full heap");
    heap_size = UINT64_MAX;
}

// The game finalizes a pool whose imports the GPU may still read (one retired and waiting for its submission, one live): its
// contents go back to ordinary memory, and the placement's memory stays the imports' until the last of them is freed
void outlives_its_pool() {
    const uint64_t lo = 128 * MB, hi = lo + 8 * MB;
    place(lo, hi);
    window[lo + 5] = 0x5A;   // (the game's write, into the placement)
    Pool a, b;
    expect(placed_import(lo, hi, a) && placed_import(lo, hi, b), "the placed pool not imported twice");
    retire(a, 10);
    unplace(lo, hi);
    expect(window[lo + 5] == 0x5A, "the pool's contents not back in ordinary memory");
    expect(placements.empty(), "the placement not given back");
    expect(bos.size() == 1, "the placement's buffer object gone while two imports' buffers are bound to it");
    reap(10);
    expect(bos.size() == 1, "the placement's buffer object gone with its first import, the second still bound to it");
    retire(b, 11);
    reap(11);
    nothing_left("the last import of a pool given back freed");
}

// A pool across a placement's end: one placement over both, and the old one's import holds the old memory until it is freed
void taken_into_a_larger_one() {
    const uint64_t lo = 160 * MB, mid = lo + 2 * MB, end_a = lo + 4 * MB, hi = lo + 8 * MB;
    place(lo, end_a);
    Pool a, c;
    expect(placed_import(lo, end_a, a), "the first pool not imported");
    window[lo + 7] = 0x77;
    place(mid, hi);
    expect(placements.size() == 1 && placements.begin()->first == lo && placements.begin()->second.hi == hi,
           "no one placement over both pools");
    expect(window[lo + 7] == 0x77, "the first pool's contents not in the placement over both");
    expect(bos.size() == 2, "the old placement's buffer object gone while its import's buffer is bound to it");
    expect(placed_gone.size() == 1, "the old placement's imports not left to retire");
    placed_gone.clear();   // (placed_retire_gone retires them: here, the one import)
    placed_gone_any = false;
    retire(a, 20);
    reap(20);
    expect(bos.size() == 1 && counted_mb() == 8, "the old placement's memory not freed with its last import: %llu MB counted",
           (unsigned long long)counted_mb());
    expect(placed_import(lo, hi, c), "the placement over both not imported");
    expect(counted_mb() == 8, "an import of the placement over both counted once more: %llu MB counted for 8 MB placed",
           (unsigned long long)counted_mb());
    retire(c, 21);
    reap(21);
    unplace(lo, end_a);   // (two pools over it: the first finalized leaves it placed)
    expect(placements.size() == 1, "a placement two pools share given back when the first of them is finalized");
    unplace(mid, hi);
    nothing_left("a placement over two pools given back");
}

// An idle import given back (imports_give_back) while its pool stays placed: its buffer and its hold go, and the placement's
// memory stays the placement's until the pool is given back
void given_back_while_placed() {
    const uint64_t lo = 192 * MB, hi = lo + 4 * MB;
    place(lo, hi);
    std::map<uint64_t, Pool> table;
    Pool a;
    expect(placed_import(lo, hi, a), "the placed pool not imported");
    a.retired_value = 30;
    table.emplace(lo, std::move(a));
    evicted_pools.push_back(table.extract(lo));
    evicted_import_bytes += hi - lo;
    mem_completed = 30;
    imports_give_back();
    expect(evicted_pools.empty() && buffers.empty(), "an idle import not given back once the GPU is past it");
    expect(bos.size() == 1 && counted_mb() == 4, "the placement's memory gone with its idle import, the pool still placed");
    unplace(lo, hi);
    nothing_left("an idle import given back, then its pool");
}

} // namespace fixture

int main() {
    using namespace fixture;
    // (the guest window where /dev/udmabuf is not: private, anonymous, no swap reserved, no huge pages, host/hos_main.c)
    window = (uint8_t*)mmap(nullptr, window_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (window == MAP_FAILED) return std::printf("FAIL: no window\n"), 1;
    madvise(window, window_size, MADV_NOHUGEPAGE);
    placed_type = 0;   // (placed_setup's choice: the one type the stand-in has)
    counted_once();
    at_the_heap();
    outlives_its_pool();
    taken_into_a_larger_one();
    given_back_while_placed();
    munmap(window, window_size);
    if (failures) return 1;
    std::printf("placed imports: ok\n");
    return 0;
}
