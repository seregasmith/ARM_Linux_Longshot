#pragma once

#include <cstdint>

namespace mkwvk {
void sim_turf_settings_init();
void sim_turf_settings_tick(int64_t now);
void sim_turf_settings_flush();
}
