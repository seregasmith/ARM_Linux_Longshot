#include "fx_pipeline_cache.h"
#include "vk_paths.h"

#include <cstring>
#include <fstream>
#include <vector>

namespace mkwvk::reshade {
namespace {

constexpr size_t file_limit = 16 * 1024 * 1024;

bool matches_device(const std::vector<char>& bytes) {
    VkPipelineCacheHeaderVersionOne header{};
    if (bytes.size() < sizeof(header)) return false;
    std::memcpy(&header, bytes.data(), sizeof(header));
    return header.headerSize >= sizeof(header) && header.headerSize <= bytes.size() &&
           header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && header.vendorID == dev.props.vendorID &&
           header.deviceID == dev.props.deviceID && std::memcmp(header.pipelineCacheUUID, dev.props.pipelineCacheUUID, VK_UUID_SIZE) == 0;
}

}  // namespace

PipelineCache::PipelineCache() {
    const auto root = game_dir_path();
    if (!root.empty()) path_ = root / "cache" / "reshade_vulkan.bin";
    std::vector<char> bytes;
    if (!path_.empty()) {
        std::ifstream file(path_, std::ios::binary | std::ios::ate);
        const auto size = file.tellg();
        if (size > 0 && static_cast<std::streamoff>(size) <= file_limit) {
            bytes.resize(static_cast<size_t>(size));
            if (!file.seekg(0).read(bytes.data(), bytes.size()) || !matches_device(bytes)) bytes.clear();
        }
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = bytes.size();
    ci.pInitialData = bytes.data();
    if (vkCreatePipelineCache(dev.device, &ci, nullptr, &cache_) != VK_SUCCESS && !bytes.empty()) {
        ci.initialDataSize = 0;
        ci.pInitialData = nullptr;
        (void)vkCreatePipelineCache(dev.device, &ci, nullptr, &cache_);
    }
}

PipelineCache::~PipelineCache() { if (cache_) vkDestroyPipelineCache(dev.device, cache_, nullptr); }

void PipelineCache::save() const {
    if (!cache_ || path_.empty()) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(dev.device, cache_, &size, nullptr) != VK_SUCCESS || !size || size > file_limit) return;
    std::vector<char> bytes(size);
    if (vkGetPipelineCacheData(dev.device, cache_, &size, bytes.data()) != VK_SUCCESS) return;
    std::error_code error;
    std::filesystem::create_directories(path_.parent_path(), error);
    if (error) return;
    auto temporary = path_;
    temporary += "." + std::to_string(GetCurrentProcessId()) + ".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), size);
    out.close();
    if (out) {
        std::filesystem::rename(temporary, path_, error);
        if (!error) return;
    }
    std::filesystem::remove(temporary, error);
}

}  // namespace mkwvk::reshade
