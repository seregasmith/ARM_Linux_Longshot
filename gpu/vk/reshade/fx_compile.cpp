#include "fx_compile.h"

#include "effect_codegen.hpp"
#include "effect_parser.hpp"
#include "effect_preprocessor.hpp"
#include "fx_files.h"

#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <utility>

namespace ls::reshade {
namespace {

struct CachedEffect {
    std::string source;
    FxEffect effect;
    std::string diagnostics;
};

struct EffectCache {
    std::mutex mutex;
    std::list<CachedEffect> entries;
};
constexpr size_t cache_entries = 8;
constexpr size_t max_cached_source = 256 * 1024;

static std::optional<FxEffect> compile(const std::filesystem::path& path, std::optional<std::string> source,
                                       std::span<const std::filesystem::path> include_paths,
                                       const FxCompileOptions& options, std::string& diagnostics,
                                       const FxDefinitions& definitions) {
    diagnostics.clear();
    if (!options.width || !options.height) {
        diagnostics = "ReShade FX requires a nonzero buffer size\n";
        return std::nullopt;
    }

    const FxFileScope file_scope(include_paths);
    reshadefx::preprocessor preprocessor;
    preprocessor.add_macro_definition("__RESHADE__", std::to_string(options.reshade_version));
    preprocessor.add_macro_definition("__RENDERER__", "0x20000");  // Vulkan
    preprocessor.add_macro_definition("__RESHADE_PERMUTATION__", "0");
    preprocessor.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", "0");
    preprocessor.add_macro_definition("BUFFER_WIDTH", std::to_string(options.width));
    preprocessor.add_macro_definition("BUFFER_HEIGHT", std::to_string(options.height));
    preprocessor.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
    preprocessor.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");
    preprocessor.add_macro_definition("BUFFER_COLOR_SPACE", std::to_string(options.color_space));
    preprocessor.add_macro_definition("BUFFER_COLOR_FORMAT", std::to_string(options.color_format));
    preprocessor.add_macro_definition("BUFFER_COLOR_BIT_DEPTH", std::to_string(options.color_bit_depth));
    for (const auto& [name, value] : definitions) {
        if (!preprocessor.add_macro_definition(name, value)) {
            diagnostics = "Cannot redefine ReShade FX macro " + name + '\n';
            return std::nullopt;
        }
    }
    // The game's depth runs from near 0 to far 1. A preset may still supply its own interpretation.
    preprocessor.add_macro_definition("RESHADE_DEPTH_INPUT_IS_REVERSED", "0");
    for (const auto& path : include_paths) preprocessor.add_include_path(path);

    const bool preprocessed = source ? preprocessor.append_string(std::move(*source), path) : preprocessor.append_file(path);
    diagnostics = preprocessor.errors();
    if (!preprocessed) {
        if (diagnostics.empty()) diagnostics = "Could not open the ReShade FX source\n";
        return std::nullopt;
    }
    const auto& expanded = preprocessor.output();
    static EffectCache cache;
    if (expanded.size() <= max_cached_source) {
        std::lock_guard lock(cache.mutex);
        for (auto entry = cache.entries.begin(); entry != cache.entries.end(); ++entry) {
            if (entry->source != expanded) continue;
            diagnostics += entry->diagnostics;
            cache.entries.splice(cache.entries.begin(), cache.entries, entry);
            return cache.entries.front().effect;
        }
    }

    std::unique_ptr<reshadefx::codegen> codegen(reshadefx::create_codegen_spirv(
        /*vulkan_semantics=*/true, /*debug_info=*/false, /*uniforms_to_spec_constants=*/false));
    reshadefx::parser parser;
    const auto compile_diagnostics = diagnostics.size();
    const bool parsed = parser.parse(expanded, codegen.get());
    diagnostics += parser.errors();
    if (!parsed) return std::nullopt;

    FxEffect effect;
    for (const auto& [name, stage] : codegen->module().entry_points) {
        std::string binary, assembly, errors;
        const bool assembled = codegen->assemble_code_for_entry_point(name, binary, assembly, errors);
        diagnostics += errors;
        if (!assembled || binary.empty() || binary.size() % sizeof(uint32_t) != 0) {
            diagnostics += "Could not assemble SPIR-V for " + name + '\n';
            return std::nullopt;
        }
        FxShader shader{name, stage, std::vector<uint32_t>(binary.size() / sizeof(uint32_t))};
        std::memcpy(shader.spirv.data(), binary.data(), binary.size());
        effect.shaders.push_back(std::move(shader));
    }
    effect.module = std::move(codegen->module());
    if (expanded.size() <= max_cached_source) {
        size_t shader_bytes = 0;
        for (const auto& shader : effect.shaders) shader_bytes += shader.spirv.size() * sizeof(uint32_t);
        if (shader_bytes <= max_cached_source) {
            std::lock_guard lock(cache.mutex);
            cache.entries.push_front({expanded, effect, diagnostics.substr(compile_diagnostics)});
            if (cache.entries.size() > cache_entries) cache.entries.pop_back();
        }
    }
    return effect;
}

}  // namespace

std::optional<FxEffect> compile_fx(const std::filesystem::path& source,
                                  std::span<const std::filesystem::path> include_paths,
                                  const FxCompileOptions& options, std::string& diagnostics,
                                  const FxDefinitions& definitions) {
    return compile(source, std::nullopt, include_paths, options, diagnostics, definitions);
}

std::optional<FxEffect> compile_builtin_fx(std::string source, const FxCompileOptions& options,
                                          std::string& diagnostics, const std::filesystem::path& name) {
    return compile(name, std::move(source), {}, options, diagnostics, {});
}

}  // namespace ls::reshade
