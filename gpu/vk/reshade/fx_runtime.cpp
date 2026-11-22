#include "fx_runtime.h"

#include "fx_builtin.h"
#include "fx_files.h"
#include "fx_pipeline_cache.h"
#include "fx_preset.h"
#include "fx_texture.h"
#include "vk_draw.h"
#include "vk_gprof.h"
#include "vk_paths.h"
#include "vk_temporal.h"
#include "vk_texture.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>

#include "vk_work.h"  // Order the effect with the game's recorder and barrier deduplication.

namespace mkwvk::reshade {

std::atomic<bool> enabled{false};
std::atomic<bool> needs_tick{false};
std::atomic<bool> depth_requested{false};
static std::atomic<bool> preview_bypassed{false};

namespace {

namespace fs = std::filesystem;
using namespace ls::reshade;
constexpr VkFormat sdr_format = VK_FORMAT_R8G8B8A8_SRGB;
constexpr VkFormat code_format = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat hdr_format = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkShaderStageFlags graphics = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
constexpr VkImageSubresourceLayers color_layers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};

struct Scene {
    uint32_t width, height;
    VkFormat format;
    bool hdr;
    bool operator==(const Scene&) const = default;
};

struct Image {
    VkImage image{};
    VmaAllocation allocation{};
    VkImageView code{}, srgb{};
    VkFormat code_view_format = code_format;
    uint32_t width = 0, height = 0;

    ~Image() {
        if (code) vkDestroyImageView(dev.device, code, nullptr);
        if (srgb) vkDestroyImageView(dev.device, srgb, nullptr);
        if (image) destroy_image(image, allocation);
    }
    Image() = default;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    bool make(uint32_t width, uint32_t height, VkImageUsageFlags usage, bool fp16 = false) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        if (!fp16) ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = fp16 ? hdr_format : sdr_format;
        ci.extent = {width, height, 1};
        this->width = width;
        this->height = height;
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage;
        if (!create_image(ci, image, allocation)) return false;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        code_view_format = vi.format = fp16 ? hdr_format : code_format;
        if (vkCreateImageView(dev.device, &vi, nullptr, &code) != VK_SUCCESS) return false;
        if (fp16) return true;
        vi.format = sdr_format;
        return vkCreateImageView(dev.device, &vi, nullptr, &srgb) == VK_SUCCESS;
    }
};

struct Buffer {
    VkBuffer buffer{};
    VmaAllocation allocation{};
    ~Buffer() { if (buffer) destroy_buffer(buffer, allocation); }
    Buffer() = default;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    bool make(std::span<const uint8_t> bytes, VkBufferUsageFlags usage) {
        void* map = nullptr;
        if (!create_buffer_cached(bytes.size(), usage, buffer, allocation, &map) || !map) return false;
        std::memcpy(map, bytes.data(), bytes.size());
        return flush_mapped_checked(allocation, bytes.size());
    }
};

struct Binding {
    uint32_t binding;
    VkSampler sampler;
    bool srgb;
    bool depth;
    const Image* image;  // null for this pass's COLOR input
};

struct Pass {
    std::vector<Binding> bindings;
    VkDescriptorSetLayout samples{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkDescriptorSet uniforms{};
    bool srgb_write = false;
    bool copy_scene = false;
    VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_CLEAR;
    Image* target = nullptr;  // null for the ping-pong scene buffer

    ~Pass() {
        if (pipeline) vkDestroyPipeline(dev.device, pipeline, nullptr);
        if (layout) vkDestroyPipelineLayout(dev.device, layout, nullptr);
        if (samples) vkDestroyDescriptorSetLayout(dev.device, samples, nullptr);
        for (const auto& b : bindings) vkDestroySampler(dev.device, b.sampler, nullptr);
    }
};

struct Resource {
    Image image;
    std::unique_ptr<Buffer> upload;
    bool ready = false;  // a file or a target written by an earlier pass of this effect
};

struct Program {
    Scene scene{};
    Image scene_ping[2];
    bool initialized = false;
    bool depth = false;
    uint64_t uploaded_at = 0;
    size_t resource_bytes = 0;
    VkDescriptorSetLayout uniforms{};
    VkDescriptorPool pool{};
    std::vector<std::unique_ptr<Buffer>> buffers;
    std::vector<std::unique_ptr<Resource>> resources;
    std::vector<std::unique_ptr<Pass>> passes;

    ~Program() {
        passes.clear();
        if (pool) vkDestroyDescriptorPool(dev.device, pool, nullptr);
        if (uniforms) vkDestroyDescriptorSetLayout(dev.device, uniforms, nullptr);
    }
};

struct Request {
    int preset = 0;
    std::string name;
    uint64_t revision = 0;
};

struct Prepared {
    uint64_t revision;
    Scene scene;
    std::unique_ptr<Program> program;
    std::string message;
};

struct State {
    std::mutex mutex;
    Request request;
    std::string message;
    std::future<Prepared> work;
    std::optional<std::pair<uint64_t, Scene>> attempted;
    std::unique_ptr<Program> active;
    uint64_t active_revision = 0;
    std::optional<uint64_t> preview_start;
    bool show_requested_only = false;
    std::vector<std::pair<uint64_t, std::unique_ptr<Program>>> retired;
};

State& state() {
    // As for the renderer's other long-lived Vulkan state, the process owns the device and this state.
    static auto* const instance = new State;
    return *instance;
}

fs::path user_root() {
    const auto options = options_path();
    const auto root = options.empty() ? game_dir_path() : fs::u8path(options).parent_path();
    return root / "reshade";
}

std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::string read_selection() {
    const auto root = user_root();
    const auto path = fx_asset(root, "selected.txt");
    if (path.empty()) return {};
    const FxFileScope scope(std::span(&root, 1));
    std::string name;
    if (!read_fx_file(path, name) || name.size() > 255 || name.find_first_of("/\\\r\n:") != name.npos) return {};
    return name;
}

bool blend_or_mask(const reshadefx::pass& pass) {
    return pass.blend_enable[0] || pass.render_target_write_mask[0] != 0xf;
}

bool graphics_pass(const reshadefx::pass& pass, const Scene& scene) {
    const bool scene_target = pass.render_target_names[0].empty();
    return pass.cs_entry_point.empty() && !pass.vs_entry_point.empty() && !pass.ps_entry_point.empty() &&
           pass.num_vertices == 3 && pass.topology == reshadefx::primitive_topology::triangle_list &&
           pass.storage_bindings.empty() && !pass.stencil_enable &&
           std::all_of(std::begin(pass.render_target_names) + 1, std::end(pass.render_target_names),
                       [](const auto& name) { return name.empty(); }) &&
           (!scene_target || ((!pass.viewport_width || pass.viewport_width == scene.width) &&
                              (!pass.viewport_height || pass.viewport_height == scene.height)));
}

VkBlendFactor blend_factor(reshadefx::blend_factor factor) {
    constexpr VkBlendFactor factors[] = {
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE,
        VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
    };
    const auto index = static_cast<unsigned>(factor);
    return index < std::size(factors) ? factors[index] : VK_BLEND_FACTOR_MAX_ENUM;
}

VkBlendOp blend_op(reshadefx::blend_op op) {
    constexpr VkBlendOp ops[] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT,
                                 VK_BLEND_OP_MIN, VK_BLEND_OP_MAX};
    const auto index = static_cast<unsigned>(op) - 1;
    return index < std::size(ops) ? ops[index] : VK_BLEND_OP_MAX_ENUM;
}

template<class T>
bool number(std::string_view value, uint8_t* to) {
    T x{};
    while (value.starts_with(' ') || value.starts_with('\t')) value.remove_prefix(1);
    while (value.ends_with(' ') || value.ends_with('\t')) value.remove_suffix(1);
    if (value.empty()) return false;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), x);
    if (error != std::errc{} || end != value.data() + value.size()) return false;
    if constexpr (std::is_same_v<T, float>) if (!std::isfinite(x)) return false;
    std::memcpy(to, &x, sizeof(x));
    return true;
}

bool uniform_bytes(const reshadefx::uniform& u, const FxPresetSection* values, std::span<uint8_t> bytes) {
    const auto count = u.type.rows;
    if (!u.type.is_numeric() || u.type.is_array() || u.type.is_matrix() || count < 1 || count > 4 ||
        u.offset > bytes.size() || count * 4 > bytes.size() - u.offset ||
        std::ranges::any_of(u.annotations, [](const auto& a) { return a.name == "source"; })) return false;
    auto* dst = bytes.data() + u.offset;
    if (u.has_initializer_value) std::memcpy(dst, u.initializer_value.as_uint, count * 4);
    if (!values) return true;
    const auto preset = values->find(u.name);
    if (preset == values->end()) return true;
    std::string_view value = preset->second;
    for (size_t i = 0; i < count; ++i) {
        const auto comma = value.find(',');
        if ((i + 1 == count) != (comma == value.npos)) return false;
        const auto item = value.substr(0, comma);
        const bool written = u.type.is_floating_point() ? number<float>(item, dst) :
                             u.type.is_signed() ? number<int32_t>(item, dst) : number<uint32_t>(item, dst);
        if (!written) return false;
        dst += 4;
        if (comma != value.npos) value.remove_prefix(comma + 1);
    }
    return true;
}

bool make_uniforms(Program& p, const FxEffect& effect, const FxPresetSection* values, VkDescriptorSet& set,
                   std::string& error) {
    if (!effect.module.total_uniform_size) return true;
    if (effect.module.total_uniform_size > 16384) return false;
    std::vector<uint8_t> bytes(effect.module.total_uniform_size);
    for (const auto& u : effect.module.uniforms) {
        if (!uniform_bytes(u, values, bytes)) {
            error = "Unsupported or invalid uniform " + u.name;
            return false;
        }
    }
    auto buffer = std::make_unique<Buffer>();
    if (!buffer->make(bytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) return false;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = p.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &p.uniforms;
    if (vkAllocateDescriptorSets(dev.device, &ai, &set) != VK_SUCCESS) return false;
    const VkDescriptorBufferInfo info{buffer->buffer, 0, bytes.size()};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(dev.device, 1, &write, 0, nullptr);
    p.buffers.push_back(std::move(buffer));
    return true;
}

VkSamplerAddressMode address(reshadefx::texture_address_mode mode) {
    switch (mode) {
    case reshadefx::texture_address_mode::wrap: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case reshadefx::texture_address_mode::mirror: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case reshadefx::texture_address_mode::border: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

using Resources = std::map<std::string, Resource*, std::less<>>;

Resource* make_resource(Program& p, Resources& resources, const reshadefx::texture& tex,
                        const fs::path& root, std::string& error) {
    if (auto it = resources.find(tex.unique_name); it != resources.end()) return it->second;
    const bool fp16 = tex.format == reshadefx::texture_format::rgba16f;
    if (!tex.semantic.empty() || tex.type != reshadefx::texture_type::texture_2d || tex.levels != 1 || tex.storage_access ||
        (!fp16 && tex.format != reshadefx::texture_format::rgba8) ||
        !tex.width || !tex.height || tex.width > 4096 || tex.height > 4096) {
        error = "Unsupported texture format or dimensions for " + tex.name + " (requires 2D RGBA8 or RGBA16F)";
        return nullptr;
    }
    const auto source = std::ranges::find(tex.annotations, "source", &reshadefx::annotation::name);
    const auto bytes = size_t(tex.width) * tex.height * (fp16 ? 8 : 4);
    if (p.resources.size() >= 24 || p.resource_bytes + bytes > 128 * 1024 * 1024) {
        error = "ReShade resources exceed the preset limit";
        return nullptr;
    }
    auto res = std::make_unique<Resource>();
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    if (source != tex.annotations.end()) {
        if (tex.render_target || fp16) {
            error = "PNG sources require a read-only RGBA8 texture";
            return nullptr;
        }
        const auto pixels = load_png(root, fs::u8path(source->value.string_data), error);
        if (!pixels) return nullptr;
        if (pixels->width != tex.width || pixels->height != tex.height) {
            error = "Texture dimensions do not match " + tex.name;
            return nullptr;
        }
        res->upload = std::make_unique<Buffer>();
        if (!res->upload->make(pixels->rgba, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return nullptr;
        res->ready = true;
        usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    } else if (tex.render_target) {
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    } else {
        error = "Texture " + tex.name + " requires a PNG source or a render pass";
        return nullptr;
    }
    if (fp16) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(dev.physical, hdr_format, &properties);
        constexpr auto required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((properties.optimalTilingFeatures & required) != required) return nullptr;
    }
    if (!res->image.make(tex.width, tex.height, usage, fp16)) return nullptr;
    auto* resource = res.get();
    p.resource_bytes += bytes;
    p.resources.push_back(std::move(res));
    resources.emplace(tex.unique_name, resource);
    return resource;
}

bool make_bindings(Program& program, Pass& out, const FxEffect& effect, const reshadefx::pass& in,
                   Resources& resources, const fs::path& root, std::string& error) {
    if (in.sampler_bindings.size() > dev.max_push_descriptors || in.sampler_bindings.size() > 16) return false;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (const auto& binding : in.sampler_bindings) {
        const auto& s = effect.module.samplers.at(binding.index);
        const auto tex = std::ranges::find(effect.module.textures, s.texture_name, &reshadefx::texture::unique_name);
        if (tex == effect.module.textures.end() || (!tex->semantic.empty() && tex->semantic != "COLOR" && tex->semantic != "DEPTH")) {
            error = "Unsupported texture input " + (tex == effect.module.textures.end() ? s.texture_name : tex->semantic);
            return false;
        }
        const bool depth = tex->semantic == "DEPTH";
        if (depth && s.srgb) {
            error = "Depth is a linear scalar texture";
            return false;
        }
        program.depth |= depth;
        if (program.scene.hdr && tex->semantic == "COLOR" && s.srgb) {
            error = "Linear HDR COLOR cannot be sampled as sRGB";
            return false;
        }
        const Image* image = nullptr;
        if (tex->semantic.empty()) {
            auto* resource = make_resource(program, resources, *tex, root, error);
            if (!resource) return false;
            image = &resource->image;
            if (s.srgb && !image->srgb) {
                error = "RGBA16F texture " + tex->name + " cannot be sampled as sRGB";
                return false;
            }
            if (!resource->ready || image == out.target) {
                error = "Pass reads " + tex->name + " before it has a separate output to sample";
                return false;
            }
        }
        if (s.filter == reshadefx::filter_mode::anisotropic) return false;
        const auto filter = static_cast<unsigned>(s.filter);
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.minFilter = (!depth && (filter & 0x10)) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.magFilter = (!depth && (filter & 0x4)) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.mipmapMode = (!depth && (filter & 1)) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = address(s.address_u);
        si.addressModeV = address(s.address_v);
        si.addressModeW = address(s.address_w);
        VkSampler sampler{};
        if (vkCreateSampler(dev.device, &si, nullptr, &sampler) != VK_SUCCESS) return false;
        out.bindings.push_back({binding.entry_point_binding, sampler, s.srgb, depth, image});
        bindings.push_back({binding.entry_point_binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, graphics, nullptr});
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = static_cast<uint32_t>(bindings.size());
    li.pBindings = bindings.data();
    return vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &out.samples) == VK_SUCCESS;
}

bool make_pipeline(Program& p, Pass& pass, const FxEffect& effect, const reshadefx::pass& in,
                   VkPipelineCache cache, unsigned& cache_hits) {
    const VkDescriptorSetLayout sets[] = {p.uniforms, pass.samples};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 2;
    li.pSetLayouts = sets;
    if (vkCreatePipelineLayout(dev.device, &li, nullptr, &pass.layout) != VK_SUCCESS) return false;

    struct Modules {
        VkShaderModule modules[2]{};
        ~Modules() { for (auto module : modules) if (module) vkDestroyShaderModule(dev.device, module, nullptr); }
    } modules;
    const std::string* names[] = {&in.vs_entry_point, &in.ps_entry_point};
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (int i = 0; i < 2; i++) {
        const auto s = std::ranges::find(effect.shaders, *names[i], &FxShader::name);
        if (s == effect.shaders.end()) return false;
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sm.codeSize = s->spirv.size() * sizeof(uint32_t);
        sm.pCode = s->spirv.data();
        if (extern_guarded_shader_module(&sm, &modules.modules[i]) != VK_SUCCESS) return false;
        stages[i] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        stages[i].module = modules.modules[i];
        stages[i].pName = s->name.c_str();
    }

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = in.render_target_write_mask[0] & 0xf;
    blend.blendEnable = in.blend_enable[0];
    blend.srcColorBlendFactor = blend_factor(in.source_color_blend_factor[0]);
    blend.dstColorBlendFactor = blend_factor(in.dest_color_blend_factor[0]);
    blend.colorBlendOp = blend_op(in.color_blend_op[0]);
    blend.srcAlphaBlendFactor = blend_factor(in.source_alpha_blend_factor[0]);
    blend.dstAlphaBlendFactor = blend_factor(in.dest_alpha_blend_factor[0]);
    blend.alphaBlendOp = blend_op(in.alpha_blend_op[0]);
    if (blend.srcColorBlendFactor == VK_BLEND_FACTOR_MAX_ENUM || blend.dstColorBlendFactor == VK_BLEND_FACTOR_MAX_ENUM ||
        blend.srcAlphaBlendFactor == VK_BLEND_FACTOR_MAX_ENUM || blend.dstAlphaBlendFactor == VK_BLEND_FACTOR_MAX_ENUM ||
        blend.colorBlendOp == VK_BLEND_OP_MAX_ENUM || blend.alphaBlendOp == VK_BLEND_OP_MAX_ENUM) return false;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    const VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dynamics;
    const auto& output = pass.target ? *pass.target : p.scene_ping[0];
    const VkFormat format = pass.srgb_write ? sdr_format : output.code_view_format;
    if (blend.blendEnable) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(dev.physical, format, &properties);
        if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)) return false;
    }
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    VkPipelineCreationFeedback feedback{};
    VkPipelineCreationFeedbackCreateInfo feedback_info{VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO};
    feedback_info.pNext = &ri;
    feedback_info.pPipelineCreationFeedback = &feedback;
    gi.pNext = &feedback_info;
    gi.stageCount = 2;
    gi.pStages = stages;
    gi.pVertexInputState = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &ds;
    gi.layout = pass.layout;
    const bool made = extern_guarded_graphics(cache, &gi, &pass.pipeline) == VK_SUCCESS;
    if (made && (feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT)) ++cache_hits;
    return made;
}

bool make_targets(Program& p, size_t effects) {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(dev.physical, p.scene.format, &properties);
    constexpr auto blit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if ((properties.optimalTilingFeatures & blit) != blit || !dev.push_descriptor) return false;
    vkGetPhysicalDeviceFormatProperties(dev.physical, p.scene.hdr ? hdr_format : sdr_format, &properties);
    constexpr auto color = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((properties.optimalTilingFeatures & (blit | color)) != (blit | color)) return false;
    vkGetPhysicalDeviceFormatProperties(dev.physical, p.scene.hdr ? hdr_format : code_format, &properties);
    if ((properties.optimalTilingFeatures & color) != color) return false;
    for (auto& image : p.scene_ping) {
        if (!image.make(p.scene.width, p.scene.height, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        p.scene.hdr)) return false;
    }
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, graphics, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &p.uniforms) != VK_SUCCESS) return false;
    const VkDescriptorPoolSize count{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, static_cast<uint32_t>(effects)};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = count.descriptorCount;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &count;
    return vkCreateDescriptorPool(dev.device, &pi, nullptr, &p.pool) == VK_SUCCESS;
}

Prepared prepare(Request request, Scene scene) {
    helper_thread_start();
    const auto start = std::chrono::steady_clock::now();
    Prepared result{request.revision, scene, {}, {}};
    try {
        const auto included = builtin_preset(request.preset);
        const bool builtin = included.shader != nullptr;
        const auto root = builtin ? fs::path{} : user_root();
        const auto shaders = root / "shaders";
        const auto preset = builtin ? parse_preset(included.ini, result.message) :
                                       load_preset(root / "presets", fs::u8path(request.name), result.message);
        if (!preset) return result;
        if (!supports_color_space(scene.hdr, preset->linear_hdr, builtin)) {
            result.message = scene.hdr ? "Use Expanded HDR for imported SDR presets." :
                                         "This preset requires active Native HDR";
            return result;
        }
        if (preset->techniques.empty() || preset->techniques.size() > 32) {
            result.message = "Select a preset with 1 to 32 enabled techniques";
            return result;
        }
        auto program = std::make_unique<Program>();
        program->scene = scene;
        if (!make_targets(*program, preset->techniques.size())) {
            result.message = "Cannot make the ReShade targets on this device";
            return result;
        }
        const FxCompileOptions options{scene.width, scene.height, 60000, scene.hdr ? 2u : 1u,
                                       scene.hdr ? 10u : 28u, scene.hdr ? 16u : 8u};
        struct Compiled { FxEffect effect; VkDescriptorSet uniforms; Resources resources; };
        std::map<std::string, Compiled, std::less<>> compiled;
        const PipelineCache cache;
        unsigned cache_hits = 0;
        for (auto id : preset->techniques) {
            const bool unnamed_file = id.shader.empty();
            if (unnamed_file) id.shader = id.name + ".fx";
            auto it = compiled.find(id.shader);
            if (it == compiled.end()) {
                bool ambiguous = false;
                const auto path = builtin ? fs::path{} : fx_shader(shaders, fs::u8path(id.shader), ambiguous);
                if (!builtin && path.empty()) {
                    if (ambiguous) result.message = "More than one " + id.shader + "; give the shader's relative path";
                    else if (unnamed_file) result.message = "Name the shader explicitly as " + id.name + "@shader.fx";
                    else result.message = "Missing shader " + id.shader + " in reshade/shaders";
                    return result;
                }
                const auto filename = builtin ? id.shader : utf8(path.filename());
                auto effect = builtin ? compile_builtin_fx(included.source(), options, result.message, fs::u8path(id.shader)) :
                                       compile_fx(path, std::span(&shaders, 1), options, result.message,
                                                  preset_definitions(*preset, filename));
                if (!effect) return result;
                VkDescriptorSet uniforms{};
                const auto settings = preset->shader_options.find(filename);
                result.message = "Cannot prepare uniforms for " + id.shader;
                if (!make_uniforms(*program, *effect, settings == preset->shader_options.end() ? nullptr : &settings->second,
                                   uniforms, result.message)) return result;
                it = compiled.emplace(id.shader, Compiled{std::move(*effect), uniforms, {}}).first;
            }
            auto& info = it->second;
            const auto tech = std::ranges::find(info.effect.module.techniques, id.name, &reshadefx::technique::name);
            if (tech == info.effect.module.techniques.end() || tech->passes.empty()) {
                result.message = "Missing technique " + id.name + " in " + id.shader;
                return result;
            }
            for (const auto& source : tech->passes) {
                if (!graphics_pass(source, scene) || program->passes.size() >= 64) {
                    result.message = "Unsupported pass in " + id.name + " (requires a fullscreen graphics pass)";
                    return result;
                }
                if (scene.hdr && source.render_target_names[0].empty() && source.srgb_write_enable) {
                    result.message = "Linear HDR output cannot use SRGBWriteEnable";
                    return result;
                }
                auto pass = std::make_unique<Pass>();
                pass->srgb_write = source.srgb_write_enable;
                pass->uniforms = info.uniforms;
                result.message = "Cannot create Vulkan pipeline for " + id.name;
                Resource* target = nullptr;
                if (!source.render_target_names[0].empty()) {
                    const auto tex = std::ranges::find(info.effect.module.textures, source.render_target_names[0],
                                                       &reshadefx::texture::unique_name);
                    if (tex == info.effect.module.textures.end()) return result;
                    target = make_resource(*program, info.resources, *tex, root / "textures", result.message);
                    if (!target || target->upload || (source.viewport_width && source.viewport_width != target->image.width) ||
                        (source.viewport_height && source.viewport_height != target->image.height)) return result;
                    pass->target = &target->image;
                    if (pass->srgb_write && !target->image.srgb) {
                        result.message = "RGBA16F target " + tex->name + " cannot use SRGBWriteEnable";
                        return result;
                    }
                }
                if (!source.clear_render_targets && blend_or_mask(source)) {
                    pass->copy_scene = !target;
                    if (!target || target->ready) pass->load = VK_ATTACHMENT_LOAD_OP_LOAD;
                }
                if (!make_bindings(*program, *pass, info.effect, source, info.resources, root / "textures", result.message) ||
                    !make_pipeline(*program, *pass, info.effect, source, cache.handle(), cache_hits)) return result;
                if (target) target->ready = true;
                program->passes.push_back(std::move(pass));
            }
        }
        if (std::ranges::none_of(program->passes, [](const auto& pass) { return pass->target == nullptr; })) {
            result.message = "The preset does not write to the gameplay scene";
            return result;
        }
        cache.save();
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start);
        logf("vk: ReShade: prepared in %.1f ms, Vulkan pipeline cache %u/%zu hits", elapsed.count(),
             cache_hits, program->passes.size());
        result.program = std::move(program);
        result.message = scene.hdr ? "Ready for linear HDR gameplay" : "Ready for SDR gameplay";
    } catch (const std::exception& e) {
        result.message = e.what();
    }
    return result;
}

void retire(State& s) {
    if (s.active) s.retired.emplace_back(vk_retirement_value(), std::move(s.active));
}

void blit(VkCommandBuffer cmd, VkImage from, VkImage to, const Scene& scene) {
    const VkOffset3D end{static_cast<int32_t>(scene.width), static_cast<int32_t>(scene.height), 1};
    const VkImageBlit region{color_layers, {{0, 0, 0}, end}, color_layers, {{0, 0, 0}, end}};
    vkCmdBlitImage(cmd, from, VK_IMAGE_LAYOUT_GENERAL, to, VK_IMAGE_LAYOUT_GENERAL, 1, &region, VK_FILTER_NEAREST);
}

void render(VkCommandBuffer cmd, Program& p, VkImage image, VkImageView depth) {
    GprofSpan profile(cmd, "ReShade FX");
    if (!p.initialized) {
        std::vector<VkImageMemoryBarrier2> barriers;
        auto transition = [&](VkImage handle) {
            VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = handle;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            barriers.push_back(barrier);
        };
        for (const auto& ping : p.scene_ping) transition(ping.image);
        for (const auto& resource : p.resources) transition(resource->image.image);
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
        dependency.pImageMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(cmd, &dependency);
        for (const auto& resource : p.resources) {
            if (!resource->upload) continue;
            VkBufferImageCopy copy{};
            copy.imageSubresource = color_layers;
            copy.imageExtent = {resource->image.width, resource->image.height, 1};
            vkCmdCopyBufferToImage(cmd, resource->upload->buffer, resource->image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
            p.uploaded_at = vk_retirement_value();
        }
        if (p.uploaded_at) needs_tick.store(true, std::memory_order_release);
        p.initialized = true;
        logf("vk: ReShade: recording %zu pass(es) at %ux%u before the HUD (%s%s)", p.passes.size(),
             p.scene.width, p.scene.height, p.scene.hdr ? "linear HDR" : "SDR", p.depth ? ", gameplay depth" : "");
    }
    full_barrier(cmd, "ReShade input");
    blit(cmd, image, p.scene_ping[0].image, p.scene);
    unsigned read = 0;
    for (const auto& pass : p.passes) {
        full_barrier(cmd, "ReShade pass");
        const auto& input = p.scene_ping[read];
        const auto& output = pass->target ? *pass->target : p.scene_ping[read ^ 1];
        const auto width = output.width, height = output.height;
        if (pass->copy_scene) {
            const VkImageCopy copy{color_layers, {}, color_layers, {}, {width, height, 1}};
            vkCmdCopyImage(cmd, input.image, VK_IMAGE_LAYOUT_GENERAL, output.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
            full_barrier(cmd, "ReShade blend destination");
        }
        VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView = pass->srgb_write ? output.srgb : output.code;
        attachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        attachment.loadOp = pass->load;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea.extent = {width, height};
        ri.layerCount = ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &attachment;
        vkCmdBeginRendering(cmd, &ri);
        const VkViewport viewport{0.0f, float(height), float(width), -float(height), 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, {width, height}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass->pipeline);
        if (pass->uniforms) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass->layout, 0, 1,
                                                   &pass->uniforms, 0, nullptr);
        std::array<VkDescriptorImageInfo, 16> images{};
        std::array<VkWriteDescriptorSet, 16> writes{};
        for (size_t i = 0; i < pass->bindings.size(); ++i) {
            const auto& b = pass->bindings[i];
            const auto& texture = b.image ? *b.image : input;
            images[i] = {b.sampler, b.depth ? depth : b.srgb ? texture.srgb : texture.code, VK_IMAGE_LAYOUT_GENERAL};
            auto& w = writes[i];
            w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstBinding = b.binding;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &images[i];
        }
        if (!pass->bindings.empty()) vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass->layout, 1,
                                                              static_cast<uint32_t>(pass->bindings.size()), writes.data());
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        if (!pass->target) read ^= 1;
    }
    full_barrier(cmd, "ReShade output");
    blit(cmd, p.scene_ping[read].image, image, p.scene);
    full_barrier(cmd, "ReShade to game HUD");
}

}  // namespace

void preview_bypass(bool bypass) { preview_bypassed.store(bypass, std::memory_order_relaxed); }

static void request_preset(State& s, int preset, std::string name, bool reload, bool exact) {
    s.show_requested_only = exact;
    if (!reload && s.request.preset == preset && s.request.name == name) return;
    s.request = {preset, std::move(name), s.request.revision + 1};
    s.message = preset ? "Waiting for the gameplay scene" : "Off";
    enabled.store(preset != 0, std::memory_order_release);
    if (!preset) depth_requested.store(false, std::memory_order_release);
    needs_tick.store(true, std::memory_order_release);
}

void select(int preset) {
    if (preset < 0 || preset > last_builtin_preset) preset = 0;
    if (!preset && !enabled.load(std::memory_order_acquire)) return;
    auto name = preset == user_preset ? read_selection() : std::string{};
    auto& s = state();
    std::lock_guard lock(s.mutex);
    request_preset(s, preset, std::move(name), true, false);
}

void preview_select(int preset, std::string user_file) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.preview_start) s.preview_start = s.request.revision;
    request_preset(s, preset, std::move(user_file), false, true);
}

void preview_end(int saved_preset) {
    auto name = saved_preset == user_preset ? read_selection() : std::string{};
    auto& s = state();
    std::lock_guard lock(s.mutex);
    const bool changed = s.preview_start && *s.preview_start != s.request.revision;
    s.preview_start.reset();
    request_preset(s, saved_preset, std::move(name), false, changed);
    preview_bypass(false);
}

void select_user(std::string name) {
    const auto root = user_root();
    if (fx_asset(root / "presets", fs::u8path(name)).empty()) return;
    const auto file = root / "selected.txt";
    const auto temporary = root / "selected.tmp";
    std::error_code error;
    if (fs::exists(fs::symlink_status(temporary, error))) fs::remove(temporary, error);
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out << name;
    out.close();
    if (out) fs::rename(temporary, file, error);
}

std::vector<std::string> user_presets() {
    std::vector<std::string> names;
    const auto dir = user_root() / "presets";
    std::error_code error;
    fs::create_directories(dir, error);
    fs::create_directories(dir.parent_path() / "shaders", error);
    fs::create_directories(dir.parent_path() / "textures", error);
    for (fs::directory_iterator it(dir, error), end; !error && it != end; it.increment(error)) {
        const auto filename = it->path().filename();
        if (filename.extension() == ".ini" && !fx_asset(dir, filename).empty()) names.push_back(utf8(filename));
    }
    std::ranges::sort(names);
    return names;
}

std::string user_selection() { return read_selection(); }

std::string status() {
    if (!enabled.load(std::memory_order_acquire)) return {};
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return s.message;
}

void tick() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.request.preset) retire(s);
    if (s.work.valid() && s.work.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto done = s.work.get();
        if (s.request.preset && done.revision == s.request.revision) {
            s.message = done.message;
            if (done.program) {
                retire(s);
                s.active = std::move(done.program);
                s.active_revision = done.revision;
                depth_requested.store(s.active->depth, std::memory_order_release);
            }
            logf("vk: ReShade: %s", s.message.c_str());
        }
    }
    if (!s.retired.empty() || (s.active && s.active->uploaded_at)) {
        const auto completed = vk_timeline_completed();
        std::erase_if(s.retired, [completed](const auto& retired) { return retired.first && retired.first <= completed; });
        if (s.active && s.active->uploaded_at && s.active->uploaded_at <= completed) {
            for (const auto& resource : s.active->resources) resource->upload.reset();
            s.active->uploaded_at = 0;
        }
    }
    needs_tick.store(s.work.valid() || !s.retired.empty() || (s.active && (s.active->uploaded_at || !s.request.preset)),
                     std::memory_order_release);
}

bool record(VkCommandBuffer cmd, Texture* picture, uint32_t level, bool native_hdr) {
    constexpr auto transfer = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!picture || !picture->image || level || picture->layers != 1 || picture->type != VK_IMAGE_TYPE_2D ||
        picture->samples != VK_SAMPLE_COUNT_1_BIT || (picture->usage & transfer) != transfer ||
        (picture->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 && !(native_hdr && picture->format == hdr_format))) return false;
    const Scene scene{picture->width, picture->height, picture->format, native_hdr};
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.request.preset) return false;
    const auto key = std::pair{s.request.revision, scene};
    if (!s.work.valid() && (!s.attempted || *s.attempted != key)) {
        s.attempted = key;
        try {
            s.work = std::async(std::launch::async, prepare, s.request, scene);
            s.message = "Preparing ReShade preset";
            needs_tick.store(true, std::memory_order_release);
        } catch (const std::exception& e) {
            s.message = e.what();
        }
    }
    if (!s.active || s.active->scene != scene || preview_bypassed.load(std::memory_order_relaxed) ||
        (s.show_requested_only && s.active_revision != s.request.revision)) return false;
    const auto depth = s.active->depth ? temporal_reshade_depth(scene.width, scene.height) : VK_NULL_HANDLE;
    if (s.active->depth && !depth) return false;
    render(cmd, *s.active, picture->image, depth);
    return true;
}

}  // namespace mkwvk::reshade
