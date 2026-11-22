// (longshot) Every Vulkan command that does GPU work, or orders it, counts in work_epoch, the recording thread's own count.
// full_barrier (vk_texture.cpp) leaves out a barrier recorded right after another one on the same command buffer with nothing in
// between: the second orders nothing the first did not. The count is the recording thread's own: work another thread records
// (a background upload) never makes this thread's barrier look needed. Include this header AFTER the file's other includes: it turns the calls below
// into a count and the call (a header included after it that names one of them with a '(' after it would be changed too). The
// waits for the GPU are counted too (gpu_wait_stats.h).
#pragma once

#include <atomic>
#include <cstdint>

#include "vk_loader.h"
#include "vk_recorder.h"   // (RX_VK_RECORDER: each vkCmd* macro below calls its packer, which calls the driver directly when the recorder is off)
#include "gpu_wait_stats.h"

namespace mkwvk {
extern thread_local uint64_t work_epoch;   // (vk_texture.cpp)
/// (vk_texture.cpp pass_start_barrier) work other than draw passes recorded since this thread's last full barrier: a dispatch, a
/// copy, a fill or update, a blit or resolve, a clear of a whole image, a query pool's results copied. Draws, the passes'
/// begins and ends, their attachments' clears and queries are the draw passes' own
extern thread_local bool other_work_since_barrier;
inline void note_work() { work_epoch++; }
inline void note_other_work() { work_epoch++, other_work_since_barrier = true; }
/// (vk_gprof.cpp, RX_VK_PASS_TIMING=3: diag) A command recorded outside every named span of the GPU profile is timed as a
/// span of its own, "glue: <command>", from its recording to the end of the statement: what the profile's spans leave out
/// (the time between them) by what was recorded there. Nothing when the level is lower: one load of a static
bool gprof_glue_on();
struct GprofGlue {
    VkCommandBuffer cmd;
    const char* label;
    bool on = false;
    GprofGlue(VkCommandBuffer c, const char* l);
    ~GprofGlue();
    GprofGlue(const GprofGlue&) = delete;
    GprofGlue& operator=(const GprofGlue&) = delete;
};
} // namespace mkwvk
#define MKWVK_CMD0_(a, ...) a
#define MKWVK_CMD0(...) MKWVK_CMD0_(__VA_ARGS__, 0)
#define MKWVK_GLUE(name, ...) ::mkwvk::GprofGlue(MKWVK_CMD0(__VA_ARGS__), name)

#define vkCmdPipelineBarrier2(...) (::mkwvk::note_work(), MKWVK_GLUE("vkCmdPipelineBarrier2", __VA_ARGS__), ::mkwvk::rec::rx_vkCmdPipelineBarrier2(__VA_ARGS__))
#define vkCmdBeginRendering(...) (::mkwvk::note_work(), MKWVK_GLUE("vkCmdBeginRendering", __VA_ARGS__), ::mkwvk::rec::rx_vkCmdBeginRendering(__VA_ARGS__))
#define vkCmdEndRendering(...) (::mkwvk::note_work(), MKWVK_GLUE("vkCmdEndRendering", __VA_ARGS__), ::mkwvk::rec::rx_vkCmdEndRendering(__VA_ARGS__))
#define vkCmdDraw(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdDraw(__VA_ARGS__))
#define vkCmdDrawIndexed(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdDrawIndexed(__VA_ARGS__))
#define vkCmdDrawIndirect(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdDrawIndirect(__VA_ARGS__))
#define vkCmdDrawIndexedIndirect(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdDrawIndexedIndirect(__VA_ARGS__))
#define vkCmdDrawIndirectCount(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdDrawIndirectCount(__VA_ARGS__))
#define vkCmdDrawIndexedIndirectCount(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdDrawIndexedIndirectCount(__VA_ARGS__))
#define vkCmdDrawMeshTasksEXT(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdDrawMeshTasksEXT(__VA_ARGS__))
#define vkCmdDrawMeshTasksIndirectEXT(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdDrawMeshTasksIndirectEXT(__VA_ARGS__))
#define vkCmdDrawMeshTasksIndirectCountEXT(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdDrawMeshTasksIndirectCountEXT(__VA_ARGS__))
#define vkCmdDispatch(...) (::mkwvk::note_other_work(), MKWVK_GLUE("vkCmdDispatch", __VA_ARGS__), ::mkwvk::rec::rx_vkCmdDispatch(__VA_ARGS__))
#define vkCmdDispatchIndirect(...) (::mkwvk::note_other_work(), MKWVK_GLUE("vkCmdDispatchIndirect", __VA_ARGS__), ::mkwvk::rec::rx_vkCmdDispatchIndirect(__VA_ARGS__))
#define vkCmdCopyBuffer(...) (::mkwvk::note_other_work(), MKWVK_GLUE("vkCmdCopyBuffer", __VA_ARGS__), ::mkwvk::rec::rx_vkCmdCopyBuffer(__VA_ARGS__))
#define vkCmdCopyBufferToImage(...) (::mkwvk::note_other_work(), MKWVK_GLUE("vkCmdCopyBufferToImage", __VA_ARGS__), ::mkwvk::rec::rx_vkCmdCopyBufferToImage(__VA_ARGS__))
#define vkCmdCopyImageToBuffer(...) (::mkwvk::note_other_work(), MKWVK_GLUE("vkCmdCopyImageToBuffer", __VA_ARGS__), ::mkwvk::rec::rx_vkCmdCopyImageToBuffer(__VA_ARGS__))
#define vkCmdCopyImage(...) (::mkwvk::note_other_work(), MKWVK_GLUE("vkCmdCopyImage", __VA_ARGS__), ::mkwvk::rec::rx_vkCmdCopyImage(__VA_ARGS__))
#define vkCmdBlitImage(...) (::mkwvk::note_other_work(), MKWVK_GLUE("vkCmdBlitImage", __VA_ARGS__), ::mkwvk::rec::rx_vkCmdBlitImage(__VA_ARGS__))
#define vkCmdResolveImage(...) (::mkwvk::note_other_work(), MKWVK_GLUE("vkCmdResolveImage", __VA_ARGS__), ::mkwvk::rec::rx_vkCmdResolveImage(__VA_ARGS__))
#define vkCmdClearColorImage(...) (::mkwvk::note_other_work(), MKWVK_GLUE("vkCmdClearColorImage", __VA_ARGS__), ::mkwvk::rec::rx_vkCmdClearColorImage(__VA_ARGS__))
#define vkCmdClearDepthStencilImage(...) (::mkwvk::note_other_work(), MKWVK_GLUE("vkCmdClearDepthStencilImage", __VA_ARGS__), ::mkwvk::rec::rx_vkCmdClearDepthStencilImage(__VA_ARGS__))
#define vkCmdClearAttachments(...) (::mkwvk::note_work(), MKWVK_GLUE("vkCmdClearAttachments", __VA_ARGS__), ::mkwvk::rec::rx_vkCmdClearAttachments(__VA_ARGS__))
#define vkCmdFillBuffer(...) (::mkwvk::note_other_work(), MKWVK_GLUE("vkCmdFillBuffer", __VA_ARGS__), ::mkwvk::rec::rx_vkCmdFillBuffer(__VA_ARGS__))
#define vkCmdUpdateBuffer(...) (::mkwvk::note_other_work(), MKWVK_GLUE("vkCmdUpdateBuffer", __VA_ARGS__), ::mkwvk::rec::rx_vkCmdUpdateBuffer(__VA_ARGS__))
#define vkCmdBeginQuery(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdBeginQuery(__VA_ARGS__))
#define vkCmdEndQuery(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdEndQuery(__VA_ARGS__))
#define vkCmdCopyQueryPoolResults(...) (::mkwvk::note_other_work(), MKWVK_GLUE("vkCmdCopyQueryPoolResults", __VA_ARGS__), ::mkwvk::rec::rx_vkCmdCopyQueryPoolResults(__VA_ARGS__))
#define vkCmdResetQueryPool(...) (MKWVK_GLUE("vkCmdResetQueryPool", __VA_ARGS__), ::mkwvk::rec::rx_vkCmdResetQueryPool(__VA_ARGS__))
#define vkCmdWriteTimestamp2(...) (::mkwvk::rec::rx_vkCmdWriteTimestamp2(__VA_ARGS__))
#define vkCmdWriteBufferMarkerAMD(...) (MKWVK_GLUE("vkCmdWriteBufferMarkerAMD", __VA_ARGS__), ::mkwvk::rec::rx_vkCmdWriteBufferMarkerAMD(__VA_ARGS__))


// (diag, RX_VK_CALL_COUNT=1) The replay's Vulkan calls counted by kind (vk_replay.cpp logs them a frame and a draw): which calls a
// draw still makes once the repeats are skipped, the per-call cost being the driver's (about twice NVIDIA's on AMD's)
namespace mkwvk {
enum CallKind : int { CK_BindPipeline, CK_PushDescriptorSetKHR, CK_PushConstants, CK_BindDescriptorSets, CK_BindVertexBuffers2, CK_BindIndexBuffer, CK_SetViewportWithCount, CK_SetScissorWithCount, CK_SetViewport, CK_SetScissor, CK_SetBlendConstants, CK_SetStencilReference, CK_SetStencilCompareMask, CK_SetStencilWriteMask, CK_SetDepthBias, CK_SetDepthBiasEnable, CK_SetLineWidth, CK_SetDepthBoundsTestEnable, CK_SetCullMode, CK_SetFrontFace, CK_SetPrimitiveTopology, CK_SetPrimitiveRestartEnable, CK_SetDepthTestEnable, CK_SetDepthWriteEnable, CK_SetDepthCompareOp, CK_SetStencilTestEnable, CK_SetStencilOp, CK_SetRasterizerDiscardEnable, CK_SetDepthClampEnableEXT, CK_SetPolygonModeEXT, CK_SetColorBlendEnableEXT, CK_SetColorBlendEquationEXT, CK_SetColorWriteMaskEXT, CK_SetAlphaToCoverageEnableEXT, CK_SetCheckpointNV, kCallKinds };
extern const char* const call_kind_names[kCallKinds];
bool call_count_on();
extern std::atomic<uint64_t> call_counts[kCallKinds];
inline void call_count(int k) {
    if (call_count_on()) call_counts[k].fetch_add(1, std::memory_order_relaxed);
}
} // namespace mkwvk
#define vkCmdBindPipeline(...) (::mkwvk::call_count(::mkwvk::CK_BindPipeline), ::mkwvk::rec::rx_vkCmdBindPipeline(__VA_ARGS__))
#define vkCmdPushDescriptorSetKHR(...) (::mkwvk::call_count(::mkwvk::CK_PushDescriptorSetKHR), ::mkwvk::rec::rx_vkCmdPushDescriptorSetKHR(__VA_ARGS__))
#define vkCmdPushConstants(...) (::mkwvk::call_count(::mkwvk::CK_PushConstants), ::mkwvk::rec::rx_vkCmdPushConstants(__VA_ARGS__))
#define vkCmdBindDescriptorSets(...) (::mkwvk::call_count(::mkwvk::CK_BindDescriptorSets), ::mkwvk::rec::rx_vkCmdBindDescriptorSets(__VA_ARGS__))
#define vkCmdBindVertexBuffers2(...) (::mkwvk::call_count(::mkwvk::CK_BindVertexBuffers2), ::mkwvk::rec::rx_vkCmdBindVertexBuffers2(__VA_ARGS__))
#define vkCmdBindIndexBuffer(...) (::mkwvk::call_count(::mkwvk::CK_BindIndexBuffer), ::mkwvk::rec::rx_vkCmdBindIndexBuffer(__VA_ARGS__))
#define vkCmdSetViewportWithCount(...) (::mkwvk::call_count(::mkwvk::CK_SetViewportWithCount), ::mkwvk::rec::rx_vkCmdSetViewportWithCount(__VA_ARGS__))
#define vkCmdSetScissorWithCount(...) (::mkwvk::call_count(::mkwvk::CK_SetScissorWithCount), ::mkwvk::rec::rx_vkCmdSetScissorWithCount(__VA_ARGS__))
#define vkCmdSetViewport(...) (::mkwvk::call_count(::mkwvk::CK_SetViewport), ::mkwvk::rec::rx_vkCmdSetViewport(__VA_ARGS__))
#define vkCmdSetScissor(...) (::mkwvk::call_count(::mkwvk::CK_SetScissor), ::mkwvk::rec::rx_vkCmdSetScissor(__VA_ARGS__))
#define vkCmdSetBlendConstants(...) (::mkwvk::call_count(::mkwvk::CK_SetBlendConstants), ::mkwvk::rec::rx_vkCmdSetBlendConstants(__VA_ARGS__))
#define vkCmdSetStencilReference(...) (::mkwvk::call_count(::mkwvk::CK_SetStencilReference), ::mkwvk::rec::rx_vkCmdSetStencilReference(__VA_ARGS__))
#define vkCmdSetStencilCompareMask(...) (::mkwvk::call_count(::mkwvk::CK_SetStencilCompareMask), ::mkwvk::rec::rx_vkCmdSetStencilCompareMask(__VA_ARGS__))
#define vkCmdSetStencilWriteMask(...) (::mkwvk::call_count(::mkwvk::CK_SetStencilWriteMask), ::mkwvk::rec::rx_vkCmdSetStencilWriteMask(__VA_ARGS__))
#define vkCmdSetDepthBias(...) (::mkwvk::call_count(::mkwvk::CK_SetDepthBias), ::mkwvk::rec::rx_vkCmdSetDepthBias(__VA_ARGS__))
#define vkCmdSetDepthBiasEnable(...) (::mkwvk::call_count(::mkwvk::CK_SetDepthBiasEnable), ::mkwvk::rec::rx_vkCmdSetDepthBiasEnable(__VA_ARGS__))
#define vkCmdSetLineWidth(...) (::mkwvk::call_count(::mkwvk::CK_SetLineWidth), ::mkwvk::rec::rx_vkCmdSetLineWidth(__VA_ARGS__))
#define vkCmdSetDepthBoundsTestEnable(...) (::mkwvk::call_count(::mkwvk::CK_SetDepthBoundsTestEnable), ::mkwvk::rec::rx_vkCmdSetDepthBoundsTestEnable(__VA_ARGS__))
#define vkCmdSetCullMode(...) (::mkwvk::call_count(::mkwvk::CK_SetCullMode), ::mkwvk::rec::rx_vkCmdSetCullMode(__VA_ARGS__))
#define vkCmdSetFrontFace(...) (::mkwvk::call_count(::mkwvk::CK_SetFrontFace), ::mkwvk::rec::rx_vkCmdSetFrontFace(__VA_ARGS__))
#define vkCmdSetPrimitiveTopology(...) (::mkwvk::call_count(::mkwvk::CK_SetPrimitiveTopology), ::mkwvk::rec::rx_vkCmdSetPrimitiveTopology(__VA_ARGS__))
#define vkCmdSetPrimitiveRestartEnable(...) (::mkwvk::call_count(::mkwvk::CK_SetPrimitiveRestartEnable), ::mkwvk::rec::rx_vkCmdSetPrimitiveRestartEnable(__VA_ARGS__))
#define vkCmdSetDepthTestEnable(...) (::mkwvk::call_count(::mkwvk::CK_SetDepthTestEnable), ::mkwvk::rec::rx_vkCmdSetDepthTestEnable(__VA_ARGS__))
#define vkCmdSetDepthWriteEnable(...) (::mkwvk::call_count(::mkwvk::CK_SetDepthWriteEnable), ::mkwvk::rec::rx_vkCmdSetDepthWriteEnable(__VA_ARGS__))
#define vkCmdSetDepthCompareOp(...) (::mkwvk::call_count(::mkwvk::CK_SetDepthCompareOp), ::mkwvk::rec::rx_vkCmdSetDepthCompareOp(__VA_ARGS__))
#define vkCmdSetStencilTestEnable(...) (::mkwvk::call_count(::mkwvk::CK_SetStencilTestEnable), ::mkwvk::rec::rx_vkCmdSetStencilTestEnable(__VA_ARGS__))
#define vkCmdSetStencilOp(...) (::mkwvk::call_count(::mkwvk::CK_SetStencilOp), ::mkwvk::rec::rx_vkCmdSetStencilOp(__VA_ARGS__))
#define vkCmdSetRasterizerDiscardEnable(...) (::mkwvk::call_count(::mkwvk::CK_SetRasterizerDiscardEnable), ::mkwvk::rec::rx_vkCmdSetRasterizerDiscardEnable(__VA_ARGS__))
#define vkCmdSetDepthClampEnableEXT(...) (::mkwvk::call_count(::mkwvk::CK_SetDepthClampEnableEXT), ::mkwvk::rec::rx_vkCmdSetDepthClampEnableEXT(__VA_ARGS__))
#define vkCmdSetPolygonModeEXT(...) (::mkwvk::call_count(::mkwvk::CK_SetPolygonModeEXT), ::mkwvk::rec::rx_vkCmdSetPolygonModeEXT(__VA_ARGS__))
#define vkCmdSetColorBlendEnableEXT(...) (::mkwvk::call_count(::mkwvk::CK_SetColorBlendEnableEXT), ::mkwvk::rec::rx_vkCmdSetColorBlendEnableEXT(__VA_ARGS__))
#define vkCmdSetColorBlendEquationEXT(...) (::mkwvk::call_count(::mkwvk::CK_SetColorBlendEquationEXT), ::mkwvk::rec::rx_vkCmdSetColorBlendEquationEXT(__VA_ARGS__))
#define vkCmdSetColorWriteMaskEXT(...) (::mkwvk::call_count(::mkwvk::CK_SetColorWriteMaskEXT), ::mkwvk::rec::rx_vkCmdSetColorWriteMaskEXT(__VA_ARGS__))
#define vkCmdSetAlphaToCoverageEnableEXT(...) (::mkwvk::call_count(::mkwvk::CK_SetAlphaToCoverageEnableEXT), ::mkwvk::rec::rx_vkCmdSetAlphaToCoverageEnableEXT(__VA_ARGS__))
#define vkCmdSetCheckpointNV(...) (::mkwvk::call_count(::mkwvk::CK_SetCheckpointNV), MKWVK_GLUE("vkCmdSetCheckpointNV", __VA_ARGS__), ::mkwvk::rec::rx_vkCmdSetCheckpointNV(__VA_ARGS__))

// (RX_VK_RECORDER=defer) the calls that begin, end or reset command buffers, submit, present or wait, read back what the GPU did,
// or let go of what recorded commands use: the thread's held packets reach the driver first (rec::boundary(): a relaxed load and
// a branch unless deferred; RX_VK_RECORDER=thread: the replay lock's holder waits for the recorder thread to run all it put in its ring;
// the kind is for the recorder's stats line). RX_REC_BOUNDARIES in vk_recorder.h lists them
#define vkBeginCommandBuffer(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkBeginCommandBuffer), ::vkx::vkBeginCommandBuffer(__VA_ARGS__))
#define vkEndCommandBuffer(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkEndCommandBuffer), ::vkx::vkEndCommandBuffer(__VA_ARGS__))
#define vkResetCommandPool(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkResetCommandPool), ::vkx::vkResetCommandPool(__VA_ARGS__))
#define vkDestroyCommandPool(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyCommandPool), ::vkx::vkDestroyCommandPool(__VA_ARGS__))
#define vkQueueSubmit2(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkQueueSubmit2), ::vkx::vkQueueSubmit2(__VA_ARGS__))
#define vkQueueWaitIdle(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkQueueWaitIdle), ::mkwvk::gpu_wait_stats::idle(::vkx::vkQueueWaitIdle, __VA_ARGS__))
#define vkQueueBindSparse(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkQueueBindSparse), ::vkx::vkQueueBindSparse(__VA_ARGS__))
#define vkQueuePresentKHR(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkQueuePresentKHR), ::vkx::vkQueuePresentKHR(__VA_ARGS__))
#define vkAcquireNextImageKHR(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkAcquireNextImageKHR), ::vkx::vkAcquireNextImageKHR(__VA_ARGS__))
#define vkDeviceWaitIdle(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDeviceWaitIdle), ::mkwvk::gpu_wait_stats::idle(::vkx::vkDeviceWaitIdle, __VA_ARGS__))
#define vkWaitForFences(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkWaitForFences), ::mkwvk::gpu_wait_stats::fences(::vkx::vkWaitForFences, __VA_ARGS__))
#define vkResetFences(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkResetFences), ::vkx::vkResetFences(__VA_ARGS__))
#define vkWaitSemaphores(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkWaitSemaphores), ::mkwvk::gpu_wait_stats::timeline(::vkx::vkWaitSemaphores, __VA_ARGS__))
#define vkGetSemaphoreCounterValue(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkGetSemaphoreCounterValue), ::vkx::vkGetSemaphoreCounterValue(__VA_ARGS__))
#define vkGetQueryPoolResults(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkGetQueryPoolResults), ::mkwvk::gpu_wait_stats::queries(::vkx::vkGetQueryPoolResults, VK_QUERY_RESULT_WAIT_BIT, __VA_ARGS__))
#define vkResetQueryPool(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkResetQueryPool), ::vkx::vkResetQueryPool(__VA_ARGS__))
#define vkGetQueueCheckpointDataNV(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkGetQueueCheckpointDataNV), ::vkx::vkGetQueueCheckpointDataNV(__VA_ARGS__))
#define vkGetDeviceFaultInfoEXT(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkGetDeviceFaultInfoEXT), ::vkx::vkGetDeviceFaultInfoEXT(__VA_ARGS__))
#define vkResetDescriptorPool(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkResetDescriptorPool), ::vkx::vkResetDescriptorPool(__VA_ARGS__))
#define vkDestroyBuffer(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyBuffer), ::vkx::vkDestroyBuffer(__VA_ARGS__))
#define vkDestroyImage(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyImage), ::vkx::vkDestroyImage(__VA_ARGS__))
#define vkDestroyImageView(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyImageView), ::vkx::vkDestroyImageView(__VA_ARGS__))
#define vkDestroyBufferView(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyBufferView), ::vkx::vkDestroyBufferView(__VA_ARGS__))
#define vkDestroySampler(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroySampler), ::vkx::vkDestroySampler(__VA_ARGS__))
#define vkDestroyPipeline(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyPipeline), ::vkx::vkDestroyPipeline(__VA_ARGS__))
#define vkDestroyPipelineLayout(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyPipelineLayout), ::vkx::vkDestroyPipelineLayout(__VA_ARGS__))
#define vkDestroyDescriptorSetLayout(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyDescriptorSetLayout), ::vkx::vkDestroyDescriptorSetLayout(__VA_ARGS__))
#define vkDestroyDescriptorPool(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyDescriptorPool), ::vkx::vkDestroyDescriptorPool(__VA_ARGS__))
#define vkDestroyQueryPool(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyQueryPool), ::vkx::vkDestroyQueryPool(__VA_ARGS__))
#define vkDestroyShaderModule(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyShaderModule), ::vkx::vkDestroyShaderModule(__VA_ARGS__))
#define vkDestroyPipelineCache(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyPipelineCache), ::vkx::vkDestroyPipelineCache(__VA_ARGS__))
#define vkDestroyFence(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyFence), ::vkx::vkDestroyFence(__VA_ARGS__))
#define vkDestroySemaphore(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroySemaphore), ::vkx::vkDestroySemaphore(__VA_ARGS__))
#define vkDestroySwapchainKHR(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroySwapchainKHR), ::vkx::vkDestroySwapchainKHR(__VA_ARGS__))
#define vkDestroySurfaceKHR(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroySurfaceKHR), ::vkx::vkDestroySurfaceKHR(__VA_ARGS__))
#define vkDestroyDevice(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyDevice), ::vkx::vkDestroyDevice(__VA_ARGS__))
#define vkDestroyInstance(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkDestroyInstance), ::vkx::vkDestroyInstance(__VA_ARGS__))
#define vkFreeMemory(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vkFreeMemory), ::vkx::vkFreeMemory(__VA_ARGS__))
#define vmaDestroyBuffer(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vmaDestroyBuffer), ::vmaDestroyBuffer(__VA_ARGS__))
#define vmaDestroyImage(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vmaDestroyImage), ::vmaDestroyImage(__VA_ARGS__))
#define vmaInvalidateAllocation(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vmaInvalidateAllocation), ::vmaInvalidateAllocation(__VA_ARGS__))
#define vmaFreeMemory(...) (::mkwvk::rec::boundary(::mkwvk::rec::B_vmaFreeMemory), ::vmaFreeMemory(__VA_ARGS__))   // (longshot: a sparse mirror's blocks, vk_vma.cpp)
