#pragma once

#include "fx_compile.h"

#include <map>
#include <string_view>

namespace ls::reshade {

struct FxTechnique {
    std::string name, shader;
    bool operator==(const FxTechnique&) const = default;
};

using FxPresetSection = std::map<std::string, std::string, std::less<>>;
struct FxPreset {
    bool linear_hdr = false;
    std::vector<FxTechnique> techniques;
    FxDefinitions definitions;
    std::map<std::string, FxPresetSection, std::less<>> shader_options;
};

std::optional<FxPreset> parse_preset(std::string_view text, std::string& diagnostics);
std::optional<FxPreset> load_preset(const std::filesystem::path& root, const std::filesystem::path& name,
                                    std::string& diagnostics);
FxDefinitions preset_definitions(const FxPreset& preset, std::string_view shader);

}  // namespace ls::reshade
