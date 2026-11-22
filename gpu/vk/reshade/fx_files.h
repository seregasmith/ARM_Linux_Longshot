#pragma once

#include <filesystem>
#include <span>
#include <string>

namespace ls::reshade {

// Canonicalize before opening any asset, including includes produced by a preprocessor macro.
std::filesystem::path fx_asset(const std::filesystem::path& root, const std::filesystem::path& name);
std::filesystem::path fx_shader(const std::filesystem::path& root, const std::filesystem::path& name,
                                bool& ambiguous);
std::filesystem::path resolve_fx_include(const std::filesystem::path& source, const std::filesystem::path& name,
                                        std::span<const std::filesystem::path> includes);
bool read_fx_file(const std::filesystem::path& path, std::string& data, size_t limit = 2 * 1024 * 1024);

class FxFileScope {
public:
    explicit FxFileScope(std::span<const std::filesystem::path> roots);
    ~FxFileScope();
    FxFileScope(const FxFileScope&) = delete;
    FxFileScope& operator=(const FxFileScope&) = delete;

private:
    std::span<const std::filesystem::path> previous_;
};

}  // namespace ls::reshade
