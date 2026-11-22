#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ls::reshade {

struct TexturePixels {
    uint32_t width, height;
    std::vector<uint8_t> rgba;
};

std::optional<TexturePixels> load_png(const std::filesystem::path& root, const std::filesystem::path& name,
                                      std::string& diagnostics);

}  // namespace ls::reshade
