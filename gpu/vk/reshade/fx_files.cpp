#include "fx_files.h"

#include <algorithm>
#include <fstream>

namespace ls::reshade {

namespace fs = std::filesystem;

namespace {
thread_local std::span<const fs::path> asset_roots;

bool asset_name(const fs::path& name) {
    if (name.empty() || name.has_root_path() || name.native().find(':') != fs::path::string_type::npos) return false;
    return std::none_of(name.begin(), name.end(), [](const auto& part) { return part == ".."; });
}

bool within(const fs::path& root, const fs::path& file) {
    auto [root_end, file_end] = std::mismatch(root.begin(), root.end(), file.begin(), file.end());
    return root_end == root.end() && file_end != file.end();
}

fs::path canonical_file(const fs::path& file) {
    std::error_code error;
    const fs::path real = fs::canonical(file, error);
    return !error && fs::is_regular_file(real, error) && !error ? real : fs::path{};
}

fs::path file_in_scope(const fs::path& file) {
    if (asset_roots.empty()) return {};
    const fs::path real = canonical_file(file);
    for (const auto& root : asset_roots) {
        std::error_code error;
        const auto base = fs::canonical(root, error);
        if (!error && !real.empty() && within(base, real)) return real;
    }
    return {};
}
}  // namespace

FxFileScope::FxFileScope(std::span<const fs::path> roots) : previous_(asset_roots) { asset_roots = roots; }
FxFileScope::~FxFileScope() { asset_roots = previous_; }

fs::path fx_asset(const fs::path& root, const fs::path& name) {
    if (!asset_name(name)) return {};
    const FxFileScope scope(std::span(&root, 1));
    return file_in_scope(root / name);
}

fs::path fx_shader(const fs::path& root, const fs::path& name, bool& ambiguous) {
    ambiguous = false;
    if (!asset_name(name) || name.extension() != ".fx") return {};
    if (const auto direct = fx_asset(root, name); !direct.empty() || name.has_parent_path()) return direct;

    fs::path found;
    std::error_code error;
    size_t entries = 0;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end;
         !error && it != end && entries < 4096; it.increment(error), ++entries) {
        if (it.depth() >= 7) it.disable_recursion_pending();
        if (it->path().filename() != name) continue;
        const auto candidate = fx_asset(root, it->path().lexically_relative(root));
        if (candidate.empty() || candidate == found) continue;
        if (!found.empty()) {
            ambiguous = true;
            return {};
        }
        found = candidate;
    }
    return found;
}

fs::path resolve_fx_include(const fs::path& source, const fs::path& name, std::span<const fs::path> includes) {
    if (!asset_name(name)) return {};
    if (auto path = file_in_scope(source.parent_path() / name); !path.empty()) return path;
    for (const auto& dir : includes) {
        if (auto path = file_in_scope(dir / name); !path.empty()) return path;
    }
    return {};
}

bool read_fx_file(const fs::path& path, std::string& data, size_t limit) {
    const auto real = file_in_scope(path);
    if (real.empty()) return false;
    std::ifstream file(real, std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    if (size < 0 || static_cast<std::streamoff>(size) > static_cast<std::streamoff>(limit)) return false;
    data.resize(static_cast<size_t>(size));
    return static_cast<bool>(file.seekg(0).read(data.data(), size));
}

}  // namespace ls::reshade
