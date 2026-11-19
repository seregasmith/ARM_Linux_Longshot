// Frame interpolation (vk_framegen.cpp): made frames shown between the game's (in a window; RX_VK_FRAMEGEN=0: off).
#pragma once

#include "vk_internal.h"

namespace mkwvk {

bool framegen_on();
/// (RX_VK_FG_LATENCY_LOG) Before the swapchain is destroyed (retire_swapchain): the log's waits for its presents end first
void fg_latency_swapchain_gone();
/// (longshot) Frame generation's setting as it runs: the Insert menu's (opts.framegen: 0 Off, 1 Auto, 2 to 8 a fixed
/// multiplier), a fixed one at most NVIDIA DLSS Frame Generation's runtime's most made frames + 1 while DLSS-G is to make the
/// frames (vk_dlss.h dlss_fg_most: 6x on an RTX 50's runtime, 2x on an RTX 40's; that most is 0 otherwise: no cap). Past it
/// NGX was not asked for the sets, and the renderer's own or AMD FSR 3.1 made every frame with DLSS FG chosen. A saved value
/// past it is kept (another engine runs it), and is shown and run as that top. Off and Auto are as they are
int framegen_capped(int framegen, int dlss_most);
int framegen_now();
/// Records the frame's processing into the present's recording (src: the window texture's view, sw x sh, the crop
/// x, y, w, h; native_pq: the encoding of that captured present); false when interpolation cannot run
/// (the present goes to the window as without it)
bool framegen_record(VkCommandBuffer cmd, VkImageView src, uint32_t sw, uint32_t sh, const int crop[4], bool native_pq);
/// (longshot) Who made the made frames of the sets shown lately (the last 64, about a second at 60): what the Insert menu's
/// Frame generation by row and the FPS counter name. None: none made (Off, nothing to blend from, none for half a second).
/// DlssPaused: DLSS FG chosen, and the sets were the game's frames alone, with no Upscaler picture to make frames from (the
/// game's own menus, movies and loading screens: vk_dlss.cpp dlss_fg_holds)
enum class FgMaker { None, BuiltIn, Dlss, Fsr3, DlssPaused };
FgMaker framegen_maker();
/// The first frame the last framegen_record made (null when it made none), w x h, GENERAL; with RX_VK_SHOT_MADE=all, each
/// call the next of its made frames in turn, its place in the set in index
VkImage framegen_made_image(uint32_t* w, uint32_t* h, int* index = nullptr);
/// (longshot) The gameplay upscale's picture (its output, at level), recorded right after the upscale wrote it, before the
/// game draws its HUD into it: copied while AMD FSR 3.1's frame interpolation is asked for, so that framegen_record can mark
/// the HUD for it (what the game drew over the picture since). Anything else (null, another format or shape) is left alone
struct Texture;
void framegen_hud_before(VkCommandBuffer cmd, Texture* picture, uint32_t level);
/// After that recording's submit (value): the display thread shows its frames
void framegen_queue(uint64_t value);
/// The window's present of an image (GENERAL, w x h; crop as above or null), vk_replay.cpp: the display thread's
void present_image(VkImage src, uint32_t w, uint32_t h, const int crop[4], VkImageView view = VK_NULL_HANDLE);
/// (longshot) Present ahead (vk_framegen.cpp display_main, #1702): while present_defer_into is set on a thread, present_image
/// submits the frame's copy into its swapchain image and stops there, with what present_commit needs to show it later. The
/// copy is then on the queue before the game's next frame, so its present at its time does not wait for that frame's rendering
struct PendingPresent {
    bool used = false;
    uint32_t index = 0;
    VkFence done = VK_NULL_HANDLE;           // (the copy's fence)
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    uint64_t reflex_frame = ~0ull;           // (NVIDIA Reflex: the frame it shows, vk_reflex.h reflex_present_frame)
    uint64_t antilag_frame = ~0ull;          // (AMD Anti-Lag 2: the frame it shows, vk_antilag.h antilag_present_frame)
    uint64_t swapchain_generation = 0;
};
extern thread_local PendingPresent* present_defer_into;
bool present_commit(const PendingPresent& p);   // (false: not shown, nothing deferred or a swapchain made again since)
/// The Insert menu's output effects at dw x dh: window scaling/sharpness/sides, then the optional SDR CRT.
/// Returns null when Off or unavailable, leaving the existing direct-copy path. CRT runs after frame generation.
/// pq: src is Native HDR10, scaled in RGB10A2; the SDR CRT is bypassed, never applied to PQ codes.
VkImage framegen_scale(VkCommandBuffer cmd, VkImageView src, VkImage image, uint32_t sw, uint32_t sh, const int crop[4], uint32_t dw, uint32_t dh,
                       bool pq = false);
VkImageView framegen_scaled_view();   // final scale/CRT view for the existing HDR expansion
/// (longshot) The HDR10 picture of src's crop at dw x dh (the Insert menu's HDR), GENERAL; null: none (the SDR picture then;
/// Native's scaled picture, else black: present_image).
/// pq: src is the game's own HDR10 picture (Native), its PQ codes as they are at 100% contrast, else graded and written again;
/// else the SDR picture, expanded
VkImage framegen_hdr(VkCommandBuffer cmd, VkImageView src, uint32_t sw, uint32_t sh, const int crop[4], uint32_t dw, uint32_t dh,
                     bool pq = false);
/// Each present: ultrawide's menus (the Insert menu's setting to the game's side, the sides blurred or black while a menu shows)
void wide_sides_tick();
/// Whether ultrawide's menu sides can show at all: Ultrawide with a margin and the Insert menu's setting on
bool wide_sides_possible();

} // namespace mkwvk
