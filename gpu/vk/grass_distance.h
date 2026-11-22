#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mkwvk {
// The short decorative blades' scene-buffer words and their reach powers. Large-scale flowing simulated turf is
// deliberately left to the separate Simulated turf page and its adaptive private lists.
inline int grass_words(uint64_t key, uint16_t* words, int* power) {
    switch (key) {
    case 0x383a566958350efeull: words[0] = 75, power[0] = 1; return 1;
    case 0x43ec86f35fd937b9ull:
    case 0x8a8304edb7bdf4bfull:
        words[0] = 46, power[0] = -1, words[1] = 47, power[1] = -1; return 2;
    default: return 0;
    }
}
inline float grass_scaled(float value, float reach, int power) {
    return value > 0.0f ? value * (power == 1 ? reach : 1.0f / reach) : value;
}
template <class Changed>
inline void grass_patch(uint64_t key, unsigned index, void* data, size_t size, float reach, Changed changed) {
    uint16_t words[2];
    int power[2];
    const int count = index == 3 ? grass_words(key, words, power) : 0;
    for (int i = 0; i < count; ++i) {
        const size_t offset = size_t(words[i]) * 4;
        if (offset + 4 > size) continue;
        float value;
        auto* bytes = static_cast<unsigned char*>(data);
        std::memcpy(&value, bytes + offset, 4);
        if (!(value > 0.0f)) continue;
        const float result = grass_scaled(value, reach, power[i]);
        std::memcpy(bytes + offset, &result, 4);
        changed(words[i], value, result);
    }
}
}
