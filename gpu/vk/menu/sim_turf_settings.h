#pragma once

// The simulated-turf page's state and guest resource edits. Included once by menu/sim_turf.cpp; the game-free fixture includes
// the same implementation over a small host stand-in.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <string>
#include <unordered_map>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace mkwvk::sim_turf_settings {

struct Field {
    uint64_t offset, flag;
    int scale, minimum, maximum;
    bool integer;
};
constexpr Field fields[] = {
    {0x7c, 0xb0, 10, 0, 649, false},
    {0x68, 0x9c, 1, 1, 500, false},
    {0x70, 0x9d, 1, 0, 128, true},
    {0x6c, 0xb1, 100, 0, 400, false},
};
constexpr size_t count = std::size(fields);
constexpr uint64_t vtable = 0x84b43228;
constexpr int one = 10, max_factor = 100;
constexpr char area_note[] = "Each area has its own preset. Visit an area to tune its simulated turf. Changes save automatically.";

int global = 0;
std::array<int, count> multipliers{one, one, one, one};
std::array<int, count> values{};

struct Preset {
    unsigned changed = 0;
    std::array<int, count> values{};
};
std::map<std::string, Preset> presets;
bool ready = false;
bool config_dirty = false;
bool save_failed = false;
int64_t dirty_at = 0;

template<class T> bool read(uint64_t address, T& out) {
    if (!host || !host->guest_base || !host->guest_size) return false;
    const auto size = host->guest_size();
    if (size < sizeof(T) || address > size - sizeof(T)) return false;
#ifdef _WIN32
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), host->guest_base() + address, &out, sizeof(T), &got) && got == sizeof(T);
#else
    // On Linux the guest window is one readable mapping; returned pages read as zero (vk_memory.cpp).
    std::memcpy(&out, host->guest_base() + address, sizeof(T));
    return true;
#endif
}

template<class T> bool write(uint64_t address, const T& value) {
    if (!host || !host->guest_base || !host->guest_size) return false;
    const auto size = host->guest_size();
    if (size < sizeof(T) || address > size - sizeof(T)) return false;
    auto* target = host->guest_base() + address;
#ifdef _WIN32
    SIZE_T wrote = 0;
    if (!WriteProcessMemory(GetCurrentProcess(), target, &value, sizeof(T), &wrote) || wrote != sizeof(T)) return false;
#else
    std::memcpy(target, &value, sizeof(T));
#endif
    if (host->mark_written) host->mark_written(target, sizeof(T));
    return true;
}

struct Scene {
    uint64_t address = 0, identity = 0;
    bool operator==(const Scene&) const = default;
};

Scene scene() {
    uint64_t manager = 0, native = 0, holder = 0, resource = 0, type = 0, identity = 0;
    if (!read(0x84e42540, manager) || !manager || !read(manager + 0xf0, native) || !native ||
        !read(native + 0x2a90, holder) || !holder || !read(holder + 0x150, resource) || !resource ||
        !read(resource, type) || type != vtable || !read(resource + 8, identity)) return {};
    return {resource, identity};
}

struct Location {
    Scene scene;
    std::string area;
    bool operator==(const Location&) const = default;
};
Location location;

Location observe() {
    const auto current = scene();
    if (!current.address || !host || !host->perf_info) return {};
    char area[100]{};
    int64_t movies[5]{};
    float pose[5]{};
    int has_pose = 0;
    host->perf_info(area, sizeof area, movies, pose, &has_pose);
    return area[0] ? Location{current, area} : Location{};
}

struct Word {
    uint32_t value = 0;
    uint8_t flag = 0;
    bool operator==(const Word&) const = default;
};
struct Resource {
    Scene scene;
    std::array<Word, count> original{}, applied{};
    std::array<double, count> baseline{};
};
std::unordered_map<uint64_t, Resource> resources;

bool read_word(uint64_t address, const Field& field, Word& word) {
    return read(address + field.offset, word.value) && read(address + field.flag, word.flag);
}

uint64_t parent(uint64_t address) {
    // The resource getters resolve the parent through its handle and generation.
    uint64_t handle = 0, link = 0, found = 0, type = 0;
    uint32_t generation = 0, linked_generation = 0;
    if (!read(address + 0x10, handle) || !handle || !read(address + 0x18, link) || !link ||
        !read(address + 0x20, generation) || !read(link + 0xc, linked_generation) || generation != linked_generation ||
        !read(link, found) || !found || !read(found, type) || type != vtable) return 0;
    return found;
}

double original_value(uint64_t address, const Field& field, Word word) {
    for (int depth = 0; depth < 8 && !(word.flag & 1); ++depth) {
        const auto inherited_from = parent(address);
        Word inherited;
        if (!inherited_from || inherited_from == address || !read_word(inherited_from, field, inherited)) break;
        address = inherited_from;
        word = inherited;
    }
    if (field.integer) return static_cast<int32_t>(word.value);
    float value = 0;
    std::memcpy(&value, &word.value, sizeof value);
    return value;
}

bool capture(const Scene& current) {
    std::array<Word, count> observed;
    for (size_t i = 0; i < count; ++i)
        if (!read_word(current.address, fields[i], observed[i])) return false;
    if (!(scene() == current)) return false;

    // A map can reuse a cached resource. Keep the baseline for fields that still contain our writes.
    auto& resource = resources[current.address];
    for (size_t i = 0; i < count; ++i) {
        if (!(resource.scene == current) || !(observed[i] == resource.applied[i])) {
            resource.original[i] = observed[i];
            resource.baseline[i] = original_value(current.address, fields[i], observed[i]);
        }
    }
    resource.scene = current;
    resource.applied = observed;
    return true;
}

std::filesystem::path config_path() {
    const std::string options = options_path();
    if (options.empty()) return {};
    const std::u8string utf8(reinterpret_cast<const char8_t*>(options.data()), options.size());
    return std::filesystem::path(utf8).parent_path() / "sim_turf_settings.ini";
}

void load_config() {
    const auto path = config_path();
    if (path.empty()) return;
    std::ifstream in(path);
    std::string key;
    int version = 0;
    if (!(in >> key >> version) || key != "version" || version != 1) return;
    while (in >> key) {
        if (key == "global") {
            int enabled = 0;
            auto factors = multipliers;
            if (!(in >> enabled >> factors[0] >> factors[1] >> factors[2] >> factors[3])) break;
            global = enabled == 1;
            for (size_t i = 0; i < count; ++i) multipliers[i] = std::clamp(factors[i], 0, max_factor);
        } else if (key == "area") {
            std::string area;
            Preset preset;
            auto& v = preset.values;
            if (!(in >> std::quoted(area) >> preset.changed >> v[0] >> v[1] >> v[2] >> v[3])) break;
            if (area.empty() || area.size() >= 100 || preset.changed >= (1u << count)) continue;
            for (size_t i = 0; i < count; ++i) v[i] = std::clamp(v[i], fields[i].minimum, fields[i].maximum);
            presets[std::move(area)] = preset;
        } else {
            std::getline(in, key);
        }
    }
}

void mark_dirty() {
    config_dirty = true;
    dirty_at = host && host->now_ns ? host->now_ns() : 0;
}

void flush_config() {
    if (!config_dirty) return;
    const auto path = config_path();
    if (path.empty()) {
        save_failed = true;
        return;
    }
    auto temp = path;
#ifdef _WIN32
    temp += L".tmp." + std::to_wstring(GetCurrentProcessId());
#else
    temp += ".tmp." + std::to_string((long long)getpid());
#endif
    std::ofstream out(temp, std::ios::trunc);
    out << "version 1\nglobal " << global;
    for (const int factor : multipliers) out << ' ' << factor;
    out << '\n';
    for (const auto& [area, preset] : presets) {
        out << "area " << std::quoted(area) << ' ' << preset.changed;
        for (const int value : preset.values) out << ' ' << value;
        out << '\n';
    }
    out.close();
    bool moved = !!out;
#ifdef _WIN32
    if (moved) moved = MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    const unsigned error = moved ? 0 : (unsigned)GetLastError();
    if (!moved) DeleteFileW(temp.c_str());
#else
    std::error_code error;
    if (moved) std::filesystem::rename(temp, path, error), moved = !error;
    if (!moved) std::filesystem::remove(temp, error);
#endif
    save_failed = !moved;
    if (moved) {
        config_dirty = false;
        return;
    }
#ifdef _WIN32
    logf("simulated turf: could not save settings (Windows error %u)", error);
#else
    logf("simulated turf: could not save settings");
#endif
}

bool matches() { return ready && observe() == location; }

std::array<double, count> scaled(const std::array<double, count>& base) {
    const auto factor = [](size_t i) { return double(multipliers[i]) / one; };
    // TurfDensity divides the shader's density terms. A large finite divisor is the zero-density approximation.
    const double density = multipliers[1] == 0 ? std::max(base[1], 1'000'000.0) : std::max(1.0, base[1] / factor(1));
    return {std::clamp(base[0] * factor(0), 0.0, 64.9), density,
            std::clamp(std::round(base[2] * factor(2)), 0.0, 128.0),
            std::clamp(base[3] * factor(3), 0.0, 20.0)};
}

const Preset& area_preset() {
    static constexpr Preset defaults;
    const auto saved = presets.find(location.area);
    return saved == presets.end() ? defaults : saved->second;
}

bool can_reset_area() { return ready && !global && area_preset().changed != 0; }

void apply() {
    if (!matches()) return;
    auto& resource = resources.at(location.scene.address);
    const auto& preset = area_preset();
    const auto adjusted = scaled(resource.baseline);
    for (size_t i = 0; i < count; ++i) {
        const auto& field = fields[i];
        auto target = resource.original[i];
        const bool edit = global ? multipliers[i] != one : (preset.changed & (1u << i)) != 0;
        const double value = global ? adjusted[i] : double(preset.values[i]) / field.scale;
        if (edit && std::isfinite(value) && (!global || (std::isfinite(resource.baseline[i]) && (i != 1 || resource.baseline[i] > 0)))) {
            if (field.integer) target.value = static_cast<uint32_t>(value);
            else {
                const float real = static_cast<float>(value);
                std::memcpy(&target.value, &real, sizeof real);
            }
            target.flag |= 1;
        }
        if (target == resource.applied[i] || !(scene() == location.scene)) continue;
        const uint64_t address = resource.scene.address;
        if (!write(address + field.offset, target.value)) continue;
        if (write(address + field.flag, target.flag)) resource.applied[i] = target;
        else if (scene() == location.scene) write(address + field.offset, resource.applied[i].value);
    }
}

void display_values() {
    if (!ready) return;
    const auto& stock = resources.at(location.scene.address).baseline;
    const auto& preset = area_preset();
    for (size_t i = 0; i < count; ++i) {
        const auto& field = fields[i];
        const double base = std::isfinite(stock[i]) ? stock[i] * field.scale : field.minimum;
        values[i] = preset.changed & (1u << i) ? preset.values[i] :
                    int(std::lround(std::clamp(base, double(field.minimum), double(field.maximum))));
    }
}

void reset_area() {
    if (!can_reset_area() || !matches()) return;
    auto previous = presets.extract(location.area);
    mark_dirty();
    flush_config();
    if (save_failed) {
        presets.insert(std::move(previous));
        return;
    }
    display_values();
    apply();
    menu_redraw();
}

bool tick(int64_t now) {
    if (config_dirty && (!dirty_at || now - dirty_at >= 250'000'000)) flush_config();
    static int64_t next_sample = 0, stable_since = 0;
    static Location candidate;
    if (now < next_sample) return false;
    next_sample = now + 250'000'000;
    const auto observed = observe();
    if (!(observed == candidate)) {
        candidate = observed;
        stable_since = now;
        const bool was_ready = ready;
        ready = false;
        if (!observed.scene.address) location = {};
        return was_ready;
    }
    if (!observed.scene.address || now - stable_since < 750'000'000 || ready) return false;
    if (!capture(candidate.scene)) return false;
    if (!(location == candidate)) {
        location = candidate;
        logf("simulated turf: %s (%s)", location.area.c_str(), global ? "global multipliers" :
             area_preset().changed ? "saved area preset" : "installed profile");
    }
    ready = true;
    display_values();
    apply();
    return true;
}

void mode_changed() {
    if (ready && !global) display_values();
    mark_dirty();
    apply();
    menu_redraw();
}

void multiplier_changed() {
    mark_dirty();
    apply();
}

void area_changed(size_t index) {
    if (!matches() || index >= count) return;
    auto& preset = presets[location.area];
    preset.changed |= 1u << index;
    preset.values[index] = values[index];
    mark_dirty();
    apply();
}

std::string decimal(int value, int scale, int decimals, bool multiplier = false) {
    if (!multiplier && !ready) return "Load an area";
    char text[24];
    snprintf(text, sizeof text, "%.*f%s", decimals, double(value) / scale, multiplier ? "x" : "");
    return text;
}

} // namespace mkwvk::sim_turf_settings
