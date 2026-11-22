// The Audio module's (gpu/vk/menu/audio.cpp, #1633) that the rest of the menu uses (vk_menu.cpp): the options file's lines for
// the chosen devices (save, the load), the devices listed again when the menu opens, and the microphone test's meter.
#pragma once

#include <string>

namespace mkwvk {
// (the Output device row: its value, and the device it stands for, by its endpoint ID and the name it had)
extern int output_pick;
extern std::string output_want, output_saved_name;
bool can_outputs();
bool output_forced();   // (HOS_AUDIO_DEVICE chooses the device for this run: the row is not changed)
std::string output_id_key(const char* text);   // (an options file's endpoint ID, as the host takes it, or none)
std::string output_name_key(const char* text);   // (an options file's device name, on one line: also the monitor's)
void refresh_outputs();   // (the host's playback devices listed again, the row on the chosen one)

// (the Microphone row's chosen microphone, by its name as the options file keeps it)
extern std::string mic_want;
std::string mic_key(const char* name);
void refresh_mics();   // (the host's microphones listed again, the row on mic_want's)
void mic_tell_host();

// (the microphone test: on while the player has it on (Test microphone), the levels the host measured and its state)
extern bool mic_testing;
extern int mic_levels[2], mic_test_state;
void mic_test_tick(bool focused);
} // namespace mkwvk
