#include "fx_builtin.h"
#include "fx_compile.h"
#include "fx_preset.h"
#include "fx_texture.h"

#include <algorithm>
#include <iostream>

using reshadefx::shader_type;
static constexpr ls::reshade::FxCompileOptions options{1920, 1080, 60000, 1, 28, 8};

static int probe(const std::filesystem::path& path) {
    if (path == "--builtins") {
        using namespace ls::reshade;
        std::string error;
        using namespace mkwvk::reshade;
        if (clarity_presets.size() != 20 || builtin_preset(0).shader || builtin_preset(user_preset).shader ||
            builtin_preset(last_builtin_preset + 1).shader) return 1;
        if (supports_color_space(true, false, false) || !supports_color_space(true, true, false) ||
            supports_color_space(false, true, false) || !supports_color_space(false, false, false)) return 1;
        for (int i = 1; i <= last_builtin_preset; ++i) {
            if (i == user_preset) continue;
            const auto included = builtin_preset(i);
            const auto preset = parse_preset(included.ini, error);
            if (!preset || preset->techniques.size() != 1) return 1;
            const auto& tech = preset->techniques[0];
            if (!supports_color_space(true, preset->linear_hdr, true)) return 1;
            const auto validate = [&](const FxCompileOptions& target) {
                const auto fx = compile_builtin_fx(included.source(), target, error, tech.shader);
                if (!fx) {
                    std::cerr << error;
                    return false;
                }
                const auto technique = std::ranges::find(fx->module.techniques, tech.name, &reshadefx::technique::name);
                if (technique == fx->module.techniques.end() || technique->passes.size() != 1 || fx->shaders.size() < 2) return false;
                const auto section = preset->shader_options.find(tech.shader);
                if (section == preset->shader_options.end()) return fx->module.uniforms.empty();
                for (const auto& [name, value] : section->second) {
                    const auto uniform = std::ranges::find(fx->module.uniforms, name, &reshadefx::uniform::name);
                    if (uniform == fx->module.uniforms.end() || uniform->type.rows != 1 + std::ranges::count(value, ',')) return false;
                }
                return true;
            };
            if (!validate({1920, 1080, 60000, 2, 10, 16}) || (!preset->linear_hdr && !validate(options))) return 1;
        }
        return 0;
    }
    if (path.extension() == ".png") {
        std::string diagnostics;
        const auto pixels = ls::reshade::load_png(path.parent_path(), path.filename(), diagnostics);
        if (!pixels) {
            std::cerr << diagnostics;
            return 1;
        }
        std::cout << pixels->width << 'x' << pixels->height;
        for (auto i : pixels->rgba) std::cout << ' ' << unsigned(i);
        std::cout << '\n';
        return 0;
    }
    if (path.extension() == ".ini") {
        std::string diagnostics;
        const auto preset = ls::reshade::load_preset(path.parent_path(), path.filename(), diagnostics);
        if (!preset) {
            std::cerr << diagnostics << '\n';
            return 1;
        }
        for (const auto& t : preset->techniques) std::cout << t.name << '@' << t.shader << '\n';
        for (const auto& [key, value] : ls::reshade::preset_definitions(*preset, "one.fx"))
            std::cout << key << '=' << value << '\n';
        for (const auto& [key, value] : preset->shader_options.at("one.fx"))
            if (key != "PreprocessorDefinitions") std::cout << key << '=' << value << '\n';
        return 0;
    }
    const std::filesystem::path includes[] = {path.parent_path()};
    std::string diagnostics;
    const auto effect = ls::reshade::compile_fx(path, includes, options, diagnostics);
    std::cerr << diagnostics;
    if (!effect) return 1;

    const auto has_shader = [&](const std::string& name, shader_type stage) {
        return std::ranges::any_of(effect->shaders, [&](const auto& shader) {
            return shader.name == name && shader.stage == stage && shader.spirv.size() > 5 &&
                   shader.spirv.front() == 0x07230203;
        });
    };

    for (const auto& technique : effect->module.techniques) {
        std::cout << "technique " << technique.name << ' ' << technique.passes.size() << '\n';
        for (const auto& pass : technique.passes) {
            const bool valid = pass.cs_entry_point.empty()
                ? has_shader(pass.vs_entry_point, shader_type::vertex) && has_shader(pass.ps_entry_point, shader_type::pixel)
                : has_shader(pass.cs_entry_point, shader_type::compute);
            if (!valid) {
                std::cerr << "Missing SPIR-V entry point for " << technique.name << '\n';
                return 1;
            }
            std::cout << "bindings " << pass.texture_bindings.size() << ' ' << pass.sampler_bindings.size() << '\n';
        }
    }
    for (const auto& uniform : effect->module.uniforms) std::cout << "uniform " << uniform.name << '\n';
    for (const auto& texture : effect->module.textures) std::cout << "texture " << texture.name << '\n';
    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    if (argc != 2) {
        std::cerr << "Usage: fx_probe <effect.fx>\n";
        return 2;
    }
    return probe(argv[1]);
}
