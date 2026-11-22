// In SDR, replace the final transfer blit with one draw into the same swapchain image.
// Off still uses the transfer blit. No colour image or readback is allocated here.
#include "vk_colour_vision.h"
#include "vk_internal.h"
#include "vk_draw.h"
#include "vk_builtin_spv.h"
#include "shaders/clear_vert_spv.h"
#include "shaders/colour_vision_frag_spv.h"
#include <unordered_map>

#include "vk_work.h"  // preserve the present command order under the recorder (the last include: its macros)

namespace mkwvk {
namespace {

struct ColourArgs {
    float uv[4];
    float rect[4];
    int mode, source_linear, target_srgb;
};
static_assert(sizeof(ColourArgs) == 44);

VkPipelineLayout layout;
VkSampler sampler;
// Window presents serialize this state. A swapchain resize reuses its format's pipeline.
std::unordered_map<VkFormat, VkPipeline> pipelines;

bool init() {
    if (layout) return true;
    const VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    di.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    di.bindingCount = 1;
    di.pBindings = &b;
    VkDescriptorSetLayout set = VK_NULL_HANDLE;
    VkResult result = vkCreateDescriptorSetLayout(dev.device, &di, nullptr, &set);
    if (result == VK_ERROR_DEVICE_LOST) device_lost();
    if (result != VK_SUCCESS) return false;
    const VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ColourArgs)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = li.pushConstantRangeCount = 1;
    li.pSetLayouts = &set;
    li.pPushConstantRanges = &range;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    result = vkCreatePipelineLayout(dev.device, &li, nullptr, &pipeline_layout);
    VkSampler next_sampler = VK_NULL_HANDLE;
    if (result == VK_SUCCESS) {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        result = vkCreateSampler(dev.device, &si, nullptr, &next_sampler);
    }
    if (result == VK_ERROR_DEVICE_LOST) device_lost();
    if (result != VK_SUCCESS) {
        if (pipeline_layout) vkDestroyPipelineLayout(dev.device, pipeline_layout, nullptr);
        vkDestroyDescriptorSetLayout(dev.device, set, nullptr);
        return false;
    }
    vkDestroyDescriptorSetLayout(dev.device, set, nullptr);  // the pipeline layout owns the layout definition
    layout = pipeline_layout;
    sampler = next_sampler;
    return true;
}

VkPipeline pipeline(VkFormat format) {
    // Each format is tried once, including a guarded driver compilation failure.
    auto [it, first_try] = pipelines.try_emplace(format, VK_NULL_HANDLE);
    if (!first_try) return it->second;
    IntelDriverGate gate;
    VkPipelineShaderStageCreateInfo st[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                         {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[0].pName = st[1].pName = "main";
    const auto vert = BUILTIN_SPV(CLEAR_VERT_SPV);
    const auto frag = BUILTIN_SPV(COLOUR_VISION_FRAG_SPV);
    auto make_module = [](auto spv, VkShaderModule* out) {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = spv.size;
        mi.pCode = spv.code;
        const VkResult result = extern_guarded_shader_module(&mi, out);
        if (result == VK_ERROR_DEVICE_LOST) device_lost();
        return result == VK_SUCCESS;
    };
    if (!make_module(vert, &st[0].module)) return VK_NULL_HANDLE;
    if (!make_module(frag, &st[1].module)) {
        vkDestroyShaderModule(dev.device, st[0].module, nullptr);
        return VK_NULL_HANDLE;
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    const VkPipelineColorBlendAttachmentState cb{VK_FALSE, {}, {}, {}, {}, {}, {}, 0xf};
    VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    bs.attachmentCount = 1;
    bs.pAttachments = &cb;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &ri;
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pColorBlendState = &bs;
    ci.pDynamicState = &dy;
    ci.layout = layout;
    VkPipeline made = VK_NULL_HANDLE;
    const auto result = extern_guarded_graphics(VK_NULL_HANDLE, &ci, &made);
    for (const auto& stage : st) vkDestroyShaderModule(dev.device, stage.module, nullptr);
    if (result == VK_ERROR_DEVICE_LOST) device_lost();
    if (result != VK_SUCCESS) {
        if (made) vkDestroyPipeline(dev.device, made, nullptr);
        return VK_NULL_HANDLE;
    }
    return it->second = made;
}

void attachment_layout(VkCommandBuffer cmd, VkImage image, bool begin) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.oldLayout = begin ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = begin ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcStageMask = begin ? VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.dstStageMask = begin ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = begin ? VK_ACCESS_2_TRANSFER_WRITE_BIT : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = begin ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_MEMORY_READ_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

} // namespace

bool colour_vision_sdr(VkCommandBuffer cmd, VkImageView src, uint32_t sw, uint32_t sh, const int crop[4],
                       VkImage dst, VkImageView dst_view, PresentRect rect, int mode, bool source_srgb) {
    const PresentSourceRect source = present_source_rect(sw, sh, crop);
    if (!src || !dst_view || !source.w || !source.h || !rect.w || !rect.h ||
        !dev.push_descriptor || !vkCmdPushDescriptorSetKHR || !init()) return false;
    const VkFormat fmt = dev.swap_view_format;
    const VkPipeline pipe = pipeline(fmt);
    if (!pipe) return false;
    const bool srgb = fmt == VK_FORMAT_R8G8B8A8_SRGB || fmt == VK_FORMAT_B8G8R8A8_SRGB || fmt == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
    const ColourArgs args{{source.x / float(sw), source.y / float(sh),
                           (source.x + source.w) / float(sw), (source.y + source.h) / float(sh)},
                          {float(rect.x), float(rect.y), 1.0f / rect.w, 1.0f / rect.h},
                          mode, source_srgb, srgb};
    const VkRect2D area{{int32_t(rect.x), int32_t(rect.y)}, {rect.w, rect.h}};
    const VkViewport viewport{float(rect.x), float(rect.y), float(rect.w), float(rect.h), 0.0f, 1.0f};
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = dst_view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = area;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    attachment_layout(cmd, dst, true);
    vkCmdBeginRendering(cmd, &rendering);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    const VkDescriptorImageInfo img{sampler, src, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &img;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &w);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof args, &args);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &area);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    attachment_layout(cmd, dst, false);
    return true;
}

} // namespace mkwvk
