#include "fx_texture.h"
#include "fx_files.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_PNG
#define STBI_MAX_DIMENSIONS 4096
#include "stb_image.h"

#include <memory>
#include <span>

namespace ls::reshade {

std::optional<TexturePixels> load_png(const std::filesystem::path& root, const std::filesystem::path& name,
                                      std::string& diagnostics) {
    const auto real = fx_asset(root, name);
    if (real.empty()) {
        const auto text = name.u8string();
        diagnostics = "Missing texture " + std::string(reinterpret_cast<const char*>(text.data()), text.size()) +
                      " in reshade/textures";
        return std::nullopt;
    }
    const FxFileScope scope(std::span(&root, 1));
    std::string bytes;
    if (!read_fx_file(real, bytes, 16 * 1024 * 1024)) {
        diagnostics = "Cannot read ReShade PNG (file limit 16 MiB)";
        return std::nullopt;
    }
    int width = 0, height = 0, components = 0;
    const auto input = reinterpret_cast<const stbi_uc*>(bytes.data());
    const auto length = static_cast<int>(bytes.size());
    const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
        stbi_load_from_memory(input, length, &width, &height, &components, 4), stbi_image_free);
    if (!pixels) {
        diagnostics = "ReShade textures must be valid PNG images up to 4096 x 4096";
        return std::nullopt;
    }
    const auto size = static_cast<size_t>(width) * height * 4;
    return TexturePixels{static_cast<uint32_t>(width), static_cast<uint32_t>(height), {pixels.get(), pixels.get() + size}};
}

}  // namespace ls::reshade
