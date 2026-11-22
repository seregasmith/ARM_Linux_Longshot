#pragma once

#include "effect_module.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ls::reshade {

struct FxCompileOptions {
    unsigned width, height;
    unsigned reshade_version;
    unsigned color_space, color_format, color_bit_depth;  // ReShade buffer macro values
};

struct FxShader {
    std::string name;
    reshadefx::shader_type stage;
    std::vector<uint32_t> spirv;
};

struct FxEffect {
    reshadefx::effect_module module;
    std::vector<FxShader> shaders;
};

using FxDefinitions = std::vector<std::pair<std::string, std::string>>;

// The caller supplies the shader roots. Preprocessing confines source and includes to these roots.
std::optional<FxEffect> compile_fx(const std::filesystem::path& source,
                                  std::span<const std::filesystem::path> include_paths,
                                  const FxCompileOptions& options, std::string& diagnostics,
                                  const FxDefinitions& definitions = {});
std::optional<FxEffect> compile_builtin_fx(std::string source, const FxCompileOptions& options,
                                          std::string& diagnostics, const std::filesystem::path& name);

}  // namespace ls::reshade
