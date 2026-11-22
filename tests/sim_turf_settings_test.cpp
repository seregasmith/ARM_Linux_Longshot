// SPDX-License-Identifier: GPL-3.0-or-later
// The per-area simulated-turf settings over a sparse stand-in for the guest window, without Vulkan or the game.
#define NOMINMAX
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "vk_api.h"

namespace mkwvk {
static uint8_t* memory;
static int64_t clock_ns;
static std::filesystem::path fixture;
static std::string test_area;

static uint8_t* mock_base() { return memory; }
static uint64_t mock_size() { return 0x85000000ull; }
static int64_t mock_now() { return clock_ns; }
static const char* mock_perf(char* out, int size, int64_t[5], float[5], int* has_pose) {
    std::snprintf(out, size, "%s", test_area.c_str());
    *has_pose = 0;
    return "fixture";
}
static MkwVkHost mock;
const MkwVkHost* host = &mock;
std::string options_path() {
    const auto path = (fixture / "longshot_options.ini").u8string();
    return {reinterpret_cast<const char*>(path.data()), path.size()};
}
void logf(const char*, ...) {}
void menu_redraw() {}
} // namespace mkwvk

#include "menu/sim_turf_settings.h"

namespace {
using namespace mkwvk;
using namespace mkwvk::sim_turf_settings;

template<class T> void put(uint64_t at, T value) { std::memcpy(memory + at, &value, sizeof value); }
template<class T> T get(uint64_t at) {
    T value{};
    assert(read(at, value));
    return value;
}

void scene_data(uint64_t address, float start, float density, int blades, float far_power) {
    put(address, vtable);
    put(address + 8, address + 1);
    put(address + 0x7c, start);
    put(address + 0x68, density);
    put(address + 0x70, blades);
    put(address + 0x6c, far_power);
    for (const auto& field : fields) put(address + field.flag, uint8_t{1});
}

void load(uint64_t address, const char* area) {
    put<uint64_t>(0x14150, address);
    test_area = area;
    for (int i = 0; i < 5; ++i) {
        clock_ns += 250'000'000;
        tick(clock_ns);
    }
    assert(ready);
}

std::string config_text() {
    std::ifstream in(config_path());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
} // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    using namespace mkwvk;
    using namespace mkwvk::sim_turf_settings;
    fixture = std::filesystem::path(argv[1]) / std::filesystem::path(u8"sim-turf-測試");
    assert(std::filesystem::create_directories(fixture));
    assert(!std::filesystem::exists(config_path()));

    mock.guest_base = mock_base;
    mock.guest_size = mock_size;
    mock.now_ns = mock_now;
    mock.perf_info = mock_perf;
#ifdef _WIN32
    memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, mock_size(), MEM_RESERVE, PAGE_READWRITE));
    assert(memory);
    assert(VirtualAlloc(memory + 0x10000, 0x10000, MEM_COMMIT, PAGE_READWRITE));
    assert(VirtualAlloc(memory + 0x84e42000, 4096, MEM_COMMIT, PAGE_READWRITE));
#else
    void* reserved = mmap(nullptr, mock_size(), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(reserved != MAP_FAILED);
    memory = static_cast<uint8_t*>(reserved);
    assert(mprotect(memory + 0x10000, 0x10000, PROT_READ | PROT_WRITE) == 0);
    assert(mprotect(memory + 0x84e42000, 4096, PROT_READ | PROT_WRITE) == 0);
#endif
    put<uint64_t>(0x84e42540, 0x10000);
    put<uint64_t>(0x100f0, 0x11000);
    put<uint64_t>(0x13a90, 0x14000);

    constexpr uint64_t field = 0x18000, market = 0x19000, parent = 0x1a000;
    scene_data(field, 20.0f, 100.0f, 24, 0.8f);
    scene_data(market, 8.0f, 200.0f, 2, 0.0f);
    scene_data(parent, 8.0f, 200.0f, 2, 2.0f);
    put(market + 0xb1, uint8_t{0});
    put<uint64_t>(market + 0x10, 1);
    put<uint64_t>(market + 0x18, 0x1b000);
    put<uint64_t>(0x1b000, parent);
    put<uint32_t>(market + 0x20, 1);
    put<uint32_t>(0x1b000 + 0xc, 1);

    load(field, "HyruleField_Adult (adult)");
    assert((values == std::array<int, 4>{200, 100, 24, 80}));

    // The explicit low-end mode writes both zero controls live and saves them for only this area.
    values[0] = 0;
    area_changed(0);
    values[2] = 0;
    area_changed(2);
    flush_config();
    assert(get<float>(field + 0x7c) == 0.0f && get<int>(field + 0x70) == 0);
    assert(!config_dirty && config_text().find("HyruleField_Adult") != std::string::npos);

    // Global multipliers temporarily replace the area edit, including inherited values, then restore it.
    global = 1;
    mode_changed();
    multipliers[1] = 20;
    multiplier_changed();
    assert(get<int>(field + 0x70) == 24 && get<float>(field + 0x68) == 50.0f);
    load(market, "Market_Adult (adult)");
    assert(get<float>(market + 0x68) == 100.0f && values[3] == 200);
    multipliers[3] = 20;
    multiplier_changed();
    assert(get<float>(market + 0x6c) == 4.0f);
    global = 0;
    mode_changed();
    assert(get<float>(market + 0x68) == 200.0f && get<uint8_t>(market + 0xb1) == 0);

    // A failed atomic reset keeps both the saved edit and the live values; a successful one restores the install.
    values[2] = 4;
    area_changed(2);
    values[3] = 300;
    area_changed(3);
    flush_config();
    assert(can_reset_area() && get<int>(market + 0x70) == 4 && get<float>(market + 0x6c) == 3.0f);
    const auto before_reset = config_text();
    auto blocked_temp = config_path();
#ifdef _WIN32
    blocked_temp += L".tmp." + std::to_wstring(GetCurrentProcessId());
#else
    blocked_temp += ".tmp." + std::to_string((long long)getpid());
#endif
    assert(std::filesystem::create_directory(blocked_temp));
    reset_area();
    assert(save_failed && can_reset_area() && config_text() == before_reset);
    assert(get<int>(market + 0x70) == 4 && get<float>(market + 0x6c) == 3.0f);
    assert(std::filesystem::remove(blocked_temp));
    reset_area();
    assert(!save_failed && !can_reset_area());
    assert(get<int>(market + 0x70) == 2 && get<uint8_t>(market + 0xb1) == 0);

    // Returning to the first area reapplies its own zero-turf edit, not Market's values.
    load(field, "HyruleField_Adult (adult)");
    assert(get<float>(field + 0x7c) == 0.0f && get<int>(field + 0x70) == 0);
    load(market, "Market_Adult (adult)");
    assert(get<int>(market + 0x70) == 2 && !can_reset_area());
#ifdef _WIN32
    assert(VirtualFree(memory, 0, MEM_RELEASE));
#else
    assert(munmap(memory, mock_size()) == 0);
#endif
    std::puts("PASS: per-area simulated-turf settings, zero-turf mode, inheritance, persistence and reset rollback");
}
