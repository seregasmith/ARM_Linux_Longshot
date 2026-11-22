#pragma once

#include "vk_internal.h"

#include <atomic>
#include <string>
#include <vector>

namespace mkwvk {

struct Texture;

namespace reshade {

// Tested only at the gameplay scene boundary. No state is allocated with Off.
extern std::atomic<bool> enabled;
extern std::atomic<bool> needs_tick;
extern std::atomic<bool> depth_requested;

void select(int preset);
void preview_bypass(bool bypass);
void preview_select(int preset, std::string user_file);
void preview_end(int saved_preset);
void select_user(std::string name);
std::vector<std::string> user_presets();
std::string user_selection();
std::string status();
void tick();
bool record(VkCommandBuffer cmd, Texture* picture, uint32_t level, bool native_hdr);

}  // namespace reshade
}  // namespace mkwvk
