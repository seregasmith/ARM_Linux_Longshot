#pragma once

#include "vk_present_rect.h"
#include "vk_loader.h"

namespace mkwvk {

bool colour_vision_sdr(VkCommandBuffer cmd, VkImageView src, uint32_t sw, uint32_t sh, const int crop[4],
                       VkImage dst, VkImageView dst_view, PresentRect rect, int mode, bool source_srgb);

} // namespace mkwvk
