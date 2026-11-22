// What gpu/vk/vk_menu.cpp uses of the Insert menu's controller module (gpu/vk/menu/controller.cpp, #1633): what plays the
// controller used last, as the host reports it (HOS_OPT_VIBRATION_STATUS), which Test vibration's cell and help show.
#pragma once

#include <atomic>

namespace mkwvk {
extern std::atomic<int> vibration_status_cache;   // (the host's report: its kind times 16, plus what it says)
} // namespace mkwvk
