#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace mkwvk::colour_vision {

constexpr bool enabled(int mode) { return mode >= 1 && mode <= 3; }

// CPU equivalent of colour_vision.glsl for the Insert menu's sRGB vertex palette.
inline std::array<float, 3> correct_srgb(std::array<float, 3> rgb, int mode) {
    if (!enabled(mode)) return rgb;
    for (auto& c : rgb)
        c = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    const float r = rgb[0], g = rgb[1], b = rgb[2];
    constexpr float wr = 0.2126f, wg = 0.7152f, wb = 0.0722f;
    constexpr float yellow = -wb / (1.0f - wb);
    const float error = mode == 1 ? 0.847714f * (r - g) + 0.204868f * (b - g)
                      : mode == 2 ? 0.632678f * (r - g) + 0.227968f * (b - g)
                                  : -0.004733f * (r - g) + 0.696100f * (b - g);
    const auto axis = mode == 3 ? std::array{1.0f, -wr / wg, 0.0f} : std::array{yellow, yellow, 1.0f};
    const float y = wr * r + wg * g + wb * b;
    for (size_t i = 0; i < rgb.size(); ++i) rgb[i] += 0.75f * error * axis[i] - y;
    const auto [low, high] = std::minmax_element(rgb.begin(), rgb.end());
    const float scale = std::min({1.0f, y / std::max(-*low, 1e-9f), (1.0f - y) / std::max(*high, 1e-9f)});
    for (auto& c : rgb) {
        c = std::clamp(y + std::max(scale, 0.0f) * c, 0.0f, 1.0f);
        c = c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    }
    return rgb;
}

} // namespace mkwvk::colour_vision
