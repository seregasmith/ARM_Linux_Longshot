// The output module's engine (gpu/vk/menu/output.cpp, #1633) that the rest of the menu uses (vk_menu.cpp: the options file,
// the bug report, the pipeline share, the window followed between screens) and the upscaler rows (the 3D's size).
#pragma once

#include "display_modes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mkwvk {
// The output the menu's sizes are shown for: the Output resolution row's (applied at the next start), so the rows below follow a change
// at once; the Handheld profile runs the game's own (1536x864)
struct Out {
    uint32_t w, h;
};

inline constexpr Out kGameOutput{2048, 1152};   // (the game's own reduced size: the Output resolution's choice 0)

// (the screen: the host's main one, the one the game's window is on, its display modes; a screen's own size among them)
bool host_screen_size(uint32_t* w, uint32_t* h);
bool screen_size(uint32_t* w, uint32_t* h);
std::vector<display_modes::Size> screen_modes();
void add_screen_size(std::vector<display_modes::Size>& modes, uint32_t sw, uint32_t sh);
extern Out outputs_screen;   // (the window's screen's size the choices were read for: outputs_follow_screen)

// (the Output resolution's choices: the game's own, the sizes offered, the player's own, the sizes found while running)
const std::vector<Out>& outputs();
int custom_index();
extern Out custom_output;
extern std::vector<Out> output_extras;
int last_choice();
int output_choice(int v);
Out output_at(int v);
int output_choice_of(uint32_t w, uint32_t h);
int parse_output(const char* text);
display_modes::Size size_of(Out o);
std::vector<int> output_ids();
std::vector<display_modes::Size> output_sizes();

// (the Aspect ratio's values: the shapes of the sizes offered, the monitor's own)
const std::vector<display_modes::Shape>& output_shapes();
int output_shape_of(int v);
std::string monitor_shape();
int aspect_monitor_value();
int aspect_last();
std::string aspect_shape(int v);
int aspect_index(const std::string& name);
bool aspect_is_monitors(int v);
std::string host_shape();
bool aspect_follows_host(int v);
int aspect_of_choice(int v, int ultrawide);
int aspect_for_choice(int v);
std::string step_shape(int id);
int step_output_choice(int id, int dir);
extern int aspect_started, output_started;   // (the rows' values at the start: outputs_sync)
int output_for_aspect(int v);

// (the picture: a choice's, and the one shown; the 3D's size at a share of it, in its twelfths)
Out picture_for(int v, bool* native = nullptr);
Out shown_output();
int twelfths(float scale);
void game_size(float scale, uint32_t& w, uint32_t& h);
} // namespace mkwvk
