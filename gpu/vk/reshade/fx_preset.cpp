#include "fx_preset.h"
#include "fx_files.h"

#include <algorithm>
#include <ranges>

namespace ls::reshade {

namespace {
std::string_view trim(std::string_view text) {
    constexpr std::string_view whitespace = " \r\t";
    const auto first = text.find_first_not_of(whitespace);
    if (first == text.npos) return {};
    return text.substr(first, text.find_last_not_of(whitespace) - first + 1);
}

std::vector<std::string> split_list(std::string_view text) {
    // ReShade INI escapes a literal comma by doubling it.
    std::vector<std::string> list;
    std::string part;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != ',') {
            part += text[i];
        } else if (i + 1 < text.size() && text[i + 1] == ',') {
            part += ',';
            ++i;
        } else {
            list.emplace_back(trim(part));
            part.clear();
        }
    }
    if (!part.empty()) list.emplace_back(trim(part));
    return list;
}

FxTechnique technique(std::string_view text) {
    const auto at = text.rfind('@');
    if (at == text.npos) return {std::string(text), {}};
    return {std::string(text.substr(0, at)), std::string(text.substr(at + 1))};
}

void add_definitions(FxDefinitions& out, std::string_view text) {
    for (const auto& definition : split_list(text)) {
        const auto equals = definition.find('=');
        const auto key = trim(std::string_view(definition).substr(0, equals));
        if (key.empty()) continue;
        const auto value = (equals == definition.npos || equals + 1 == definition.size()) ? "1" : definition.substr(equals + 1);
        auto previous = std::ranges::find(out, key, &FxDefinitions::value_type::first);
        if (previous != out.end()) previous->second = value;
        else out.emplace_back(key, value);
    }
}
}  // namespace

std::optional<FxPreset> parse_preset(std::string_view text, std::string& diagnostics) {
    diagnostics.clear();
    if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
    std::map<std::string, FxPresetSection, std::less<>> sections;
    std::string name;
    for (const auto line_chars : std::views::split(text, '\n')) {
        const auto line = trim(std::string_view(line_chars.begin(), line_chars.end()));
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.starts_with('[') && line.ends_with(']')) {
            name = trim(line.substr(1, line.size() - 2));
            continue;
        }
        const auto equals = line.find('=');
        if (equals != line.npos) sections[name][std::string(trim(line.substr(0, equals)))] = trim(line.substr(equals + 1));
    }

    auto root = sections.extract("");
    if (root.empty() || !root.mapped().contains("Techniques")) {
        diagnostics = "The preset is missing Techniques";
        return std::nullopt;
    }

    FxPreset preset;
    const auto& color_space = root.mapped()["LongshotColorSpace"];
    if (color_space == "LinearHDR") preset.linear_hdr = true;
    else if (!color_space.empty() && color_space != "SDR") {
        diagnostics = "Unsupported LongshotColorSpace (use SDR or LinearHDR)";
        return std::nullopt;
    }
    add_definitions(preset.definitions, root.mapped()["PreprocessorDefinitions"]);
    std::vector<FxTechnique> enabled;
    for (const auto& id : split_list(root.mapped()["Techniques"])) {
        auto item = technique(id);
        if (item.name.empty() || (id.find('@') != id.npos && item.shader.empty())) {
            diagnostics = "Invalid ReShade technique identifier: " + id;
            return std::nullopt;
        }
        enabled.push_back(std::move(item));
    }
    for (const auto& id : split_list(root.mapped()["TechniqueSorting"])) {
        const auto order = technique(id);
        auto it = std::ranges::find(enabled, order);
        if (it == enabled.end() && order.shader.empty() && std::ranges::count(enabled, order.name, &FxTechnique::name) == 1)
            it = std::ranges::find(enabled, order.name, &FxTechnique::name);
        if (it != enabled.end()) {
            preset.techniques.push_back(std::move(*it));
            enabled.erase(it);
        }
    }
    for (auto& item : enabled) preset.techniques.push_back(std::move(item));
    preset.shader_options = std::move(sections);
    return preset;
}

std::optional<FxPreset> load_preset(const std::filesystem::path& root, const std::filesystem::path& name,
                                    std::string& diagnostics) {
    const FxFileScope scope(std::span(&root, 1));
    const auto file = fx_asset(root, name);
    std::string text;
    if (file.empty() || !read_fx_file(file, text)) {
        diagnostics = "Could not read the ReShade preset";
        return std::nullopt;
    }
    return parse_preset(text, diagnostics);
}

FxDefinitions preset_definitions(const FxPreset& preset, std::string_view shader) {
    FxDefinitions result = preset.definitions;
    if (auto section = preset.shader_options.find(shader); section != preset.shader_options.end()) {
        if (auto item = section->second.find("PreprocessorDefinitions"); item != section->second.end())
            add_definitions(result, item->second);
    }
    return result;
}

}  // namespace ls::reshade
