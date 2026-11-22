#include "../gpu/vk/grass_distance.h"
#include <cassert>
#include <cstdint>
#include <initializer_list>

int main() {
    constexpr uint64_t radius = 0x383a566958350efeull;
    constexpr uint64_t colour = 0x43ec86f35fd937b9ull;
    constexpr uint64_t second = 0x8a8304edb7bdf4bfull;
    constexpr uint64_t field = 0xd5a9475f2790532eull;
    for (uint64_t key : {radius, colour, second, field, uint64_t{1}}) {
        float data[100];
        for (float& value : data) value = 12.0f;
        int changes = 0;
        mkwvk::grass_patch(key, 3, data, sizeof data, 4.0f, [&](uint16_t, float, float) { ++changes; });
        for (unsigned i = 0; i < 100; ++i) {
            const float expected = key == radius && i == 75 ? 48.0f :
                (key == colour || key == second) && (i == 46 || i == 47) ? 3.0f : 12.0f;
            assert(data[i] == expected);
        }
        assert(changes == (key == radius ? 1 : (key == colour || key == second) ? 2 : 0));
    }
    float data[100] = {};
    data[46] = -4.0f; data[47] = 8.0f; data[75] = 8.0f;
    auto changed = [](uint16_t, float, float) { assert(false); };
    mkwvk::grass_patch(colour, 3, data, 47 * sizeof(float), 2.0f, changed);
    mkwvk::grass_patch(radius, 4, data, sizeof data, 2.0f, changed);
    assert(data[46] == -4.0f && data[47] == 8.0f && data[75] == 8.0f);
}
