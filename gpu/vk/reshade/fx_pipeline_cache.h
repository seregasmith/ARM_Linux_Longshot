#pragma once

#include "vk_internal.h"

#include <filesystem>

namespace mkwvk::reshade {

class PipelineCache {
public:
    PipelineCache();
    ~PipelineCache();
    PipelineCache(const PipelineCache&) = delete;
    PipelineCache& operator=(const PipelineCache&) = delete;

    VkPipelineCache handle() const { return cache_; }
    void save() const;

private:
    std::filesystem::path path_;
    VkPipelineCache cache_{};
};

}  // namespace mkwvk::reshade
