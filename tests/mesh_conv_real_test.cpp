// The real conversion and fallback_record, cut from vk_draw.cpp by test_mesh_conv.py.
// Vulkan, allocation and pipeline setup are stand-ins; copies and indirect dispatches are observed.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "gpu/vk/mesh_conv.h"

using VkCommandBuffer = void*;
using VkBuffer = uint64_t;
using VkDeviceSize = uint64_t;
using VkDeviceAddress = uint64_t;
constexpr VkBuffer VK_NULL_HANDLE = 0;
constexpr int VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 = 1, VK_STRUCTURE_TYPE_DEPENDENCY_INFO = 2;
constexpr int VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT = 1, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT = 2;
constexpr int VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT = 4, VK_ACCESS_2_MEMORY_WRITE_BIT = 1;
constexpr int VK_ACCESS_2_TRANSFER_READ_BIT = 2, VK_ACCESS_2_TRANSFER_WRITE_BIT = 4, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT = 8;
constexpr int VK_PIPELINE_BIND_POINT_COMPUTE = 1, VK_SHADER_STAGE_COMPUTE_BIT = 1;
struct VkMemoryBarrier2 { int type; int srcStageMask = 0, srcAccessMask = 0, dstStageMask = 0, dstAccessMask = 0; };
struct VkDependencyInfo { int type; int memoryBarrierCount = 0; VkMemoryBarrier2* pMemoryBarriers = nullptr; };
struct VkBufferCopy { uint64_t srcOffset, dstOffset, size; };
struct DrawArgs {
    bool mesh = true;
    uint64_t indirect = 1, count_addr = 0;
    int max = 2, stride = 8, groups[3] = {};
    uint32_t first_task = 0;
    const void* op = nullptr;
};
struct BufferRef { VkBuffer buffer = 0; uint64_t offset = 0; explicit operator bool() const { return buffer != 0; } };
struct Program { bool fallback_task = false; uint32_t fallback_task_threads = 1; uint64_t key = 0;
    struct Stage { int vk_stage; }; std::vector<Stage> stages{{VK_SHADER_STAGE_COMPUTE_BIT}};
    std::vector<int> fallback_compute{1}; };
struct FallbackBuffers { VkBuffer records = 3; uint64_t payload_address = 0, records_address = 3, records_bytes = 0;
    bool warp_counters_cleared = false; };
namespace Shader::Backend::SPIRV {
constexpr uint32_t MESH_FALLBACK_CAPACITY = 64, MESH_FALLBACK_DISPATCH_WORD = 5, MESH_FALLBACK_HEADER_WORDS = 8;
constexpr uint64_t MESH_FALLBACK_POOL_OFFSET = 100, MESH_FALLBACK_POOL_BYTES = 100, MESH_FALLBACK_POOL_HEADER_BYTES = 4;
constexpr uint64_t WARP_COUNTER_BYTES = 4;
bool MeshFallbackCounters() { return false; }
uint64_t MeshFallbackRecordsBytes() { return 128; }
}
struct { bool mesh_shader = false; } dev;
constexpr uint64_t kMeshRing = 4ull << 20;
VkBuffer mesh_ring = 2, mesh_conv_buf = 0, fallback_draw_records = 0;
uint64_t mesh_conv_off = 0, fallback_indirect_input = 0, fallback_payload_address = 0, fallback_records_address = 0;
int mesh_mode = 2;
std::atomic<bool> mesh_ring_filled{true};
std::atomic<uint64_t> mesh_ring_pos{0};
mesh_conv::Batch mesh_batch;
enum { kMcOne, kMcBatched, kMcStale, kMcFallback, kMcNoMesh, kMcCount };
std::atomic<int64_t> st_mc[kMcCount];
bool mesh_args_rewrite() { return false; }
uint32_t mesh_count_word() { return 0; }
BufferRef gpu_buffer_bind(uint64_t, uint64_t, bool) { return {1, 16}; }
bool mesh_ring_slot(uint32_t, uint64_t* off, uint64_t* start) { *off = 48; *start = 48; return true; }
void end_pass(VkCommandBuffer, const char*) {}
bool gprof_detail_fallback(uint64_t) { return false; }
void gprof_fallback_stage(VkCommandBuffer, uint64_t, const char*) {}
struct GprofSpan { bool on = false; GprofSpan(VkCommandBuffer, const char*) {} };
#define PROF_EXACT() ((void)0)
void full_barrier(VkCommandBuffer, const char* = nullptr) {}
void vkCmdPipelineBarrier2(VkCommandBuffer, const VkDependencyInfo*) {}
void vkCmdFillBuffer(VkCommandBuffer, VkBuffer, uint64_t, uint64_t, uint32_t) {}
std::vector<VkBufferCopy> copies;
VkBuffer copy_src = 0, copy_dst = 0, dispatch_buf = 0;
uint64_t dispatch_off = 0;
void vkCmdCopyBuffer(VkCommandBuffer, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy* regions) {
    copy_src = src; copy_dst = dst; copies.assign(regions, regions + n);
}
void vkCmdDispatchIndirect(VkCommandBuffer, VkBuffer b, uint64_t off) { dispatch_buf = b; dispatch_off = off; }
void vkCmdDispatch(VkCommandBuffer, uint32_t, uint32_t, uint32_t) {}
void vkCmdBindPipeline(VkCommandBuffer, int, int) {}
struct { Program* prog = nullptr; } descriptors;
auto& dw() { return descriptors; }
FallbackBuffers buffers;
bool fallback_buffers_of(Program&, FallbackBuffers*& f) { f = &buffers; return true; }
bool fallback_pipelines_ready(Program&) { return true; }
uint64_t fallback_buffer_address(VkBuffer b) { return b; }
bool gprof_on() { return false; }
bool bind_descriptors(VkCommandBuffer, int, Program&) { return true; }
void push_constants(VkCommandBuffer, Program&, void*, uint32_t) {}
void fallback_warp_counters(VkCommandBuffer, const FallbackBuffers&) {}
template<class... T> void logf(const char*, T...) {}
enum LogLevel : int { log_error, log_warn, log_info, log_debug, log_trace };   // (vk_internal.h's)
template<class... T> void logf_at(int, const char*, T...) {}

#include "mesh_conv_real.inc"

int main() {
    VkCommandBuffer cmd = reinterpret_cast<VkCommandBuffer>(1);
    DrawArgs d;
    Program p;
    descriptors.prog = &p;
    assert(mesh_indirect_convert(cmd, d));
    assert(copy_src == 1 && copy_dst == mesh_ring && copies.size() == 2);
    assert(copies[0].srcOffset == 16 && copies[0].dstOffset == 48 && copies[0].size == 4);
    assert(copies[1].srcOffset == 24 && copies[1].dstOffset == 60 && copies[1].size == 4);
    assert(mesh_conv_buf == mesh_ring && mesh_conv_off == 48);
    assert(fallback_record(cmd, p, d));
    assert(dispatch_buf == mesh_ring && dispatch_off == 48);
}
