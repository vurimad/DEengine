#include "core/log.h"
#include "render/rhi/vulkan/vk_device.h"

#include <array>

namespace de::rhi {

namespace {

// Indexed by Format.
constexpr std::array<VkFormat, kFormatCount> kVkFormats = {{
    VK_FORMAT_UNDEFINED,
    VK_FORMAT_R8_UNORM,
    VK_FORMAT_R8G8_UNORM,
    VK_FORMAT_R8G8B8A8_UNORM,
    VK_FORMAT_R8G8B8A8_SRGB,
    VK_FORMAT_B8G8R8A8_UNORM,
    VK_FORMAT_B8G8R8A8_SRGB,
    VK_FORMAT_R16_UNORM,
    VK_FORMAT_R16G16_UNORM,
    VK_FORMAT_R16G16B16A16_UNORM,
    VK_FORMAT_R16_SFLOAT,
    VK_FORMAT_R16G16_SFLOAT,
    VK_FORMAT_R16G16B16A16_SFLOAT,
    VK_FORMAT_R32_UINT,
    VK_FORMAT_R32G32_UINT,
    VK_FORMAT_R32G32B32A32_UINT,
    VK_FORMAT_R32_SFLOAT,
    VK_FORMAT_R32G32_SFLOAT,
    VK_FORMAT_R32G32B32A32_SFLOAT,
    VK_FORMAT_A2B10G10R10_UNORM_PACK32, // R in the low bits, like DXGI's R10G10B10A2
    VK_FORMAT_B10G11R11_UFLOAT_PACK32,  // R in the low bits, like DXGI's R11G11B10
    VK_FORMAT_D16_UNORM,
    VK_FORMAT_D32_SFLOAT,
    VK_FORMAT_D24_UNORM_S8_UINT,
    VK_FORMAT_D32_SFLOAT_S8_UINT,
    VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
    VK_FORMAT_BC1_RGBA_SRGB_BLOCK,
    VK_FORMAT_BC3_UNORM_BLOCK,
    VK_FORMAT_BC3_SRGB_BLOCK,
    VK_FORMAT_BC4_UNORM_BLOCK,
    VK_FORMAT_BC5_UNORM_BLOCK,
    VK_FORMAT_BC6H_UFLOAT_BLOCK,
    VK_FORMAT_BC7_UNORM_BLOCK,
    VK_FORMAT_BC7_SRGB_BLOCK,
}};
static_assert(kVkFormats.back() == VK_FORMAT_BC7_SRGB_BLOCK, "kVkFormats is out of step with Format");

constexpr VkPipelineStageFlags2 kDepthStages =
    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

// CommandList::barriers asserts a shader state names a stage. Should one slip through a ship
// build anyway, every stage is used: over-synchronizing is slow, under-synchronizing is a bug.
VkPipelineStageFlags2 vk_shader_stages(ShaderStages stages) {
    if (stages == ShaderStages::None) {
        stages = ShaderStages::All;
    }
    VkPipelineStageFlags2 flags = VK_PIPELINE_STAGE_2_NONE;
    if (core::has_any(stages, ShaderStages::Vertex)) {
        flags |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    }
    if (core::has_any(stages, ShaderStages::Fragment)) {
        flags |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    }
    if (core::has_any(stages, ShaderStages::Compute)) {
        flags |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    }
    return flags;
}

} // namespace

VkFormat to_vk_format(Format format) {
    const auto index = static_cast<size_t>(format);
    DE_ASSERT(index < kFormatCount);
    return index < kFormatCount ? kVkFormats[index] : VK_FORMAT_UNDEFINED;
}

Format from_vk_format(VkFormat format) {
    for (size_t i = 1; i < kFormatCount; ++i) {
        if (kVkFormats[i] == format) {
            return static_cast<Format>(i);
        }
    }
    return Format::Unknown;
}

VkImageAspectFlags format_aspects(Format format) {
    const FormatInfo& info = format_info(format);
    VkImageAspectFlags aspects = 0;
    if (info.depth) {
        aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    if (info.stencil) {
        aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    return aspects != 0 ? aspects : static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT);
}

// The stages and accesses a state covers, plus the image layout it needs. A barrier from A to B
// waits for A's stages and accesses and blocks B's. Combined states add up their bits.
VkStateInfo vk_state_info(ResourceState state, ShaderStages shader_stages) {
    VkStateInfo info;
    const auto add = [&](ResourceState bit, VkPipelineStageFlags2 stages, VkAccessFlags2 access, VkImageLayout layout) {
        if (core::has_any(state, bit)) {
            info.stages |= stages;
            info.access |= access;
            info.layout = layout; // combinations share a layout (is_valid_state)
        }
    };
    const bool shader_state =
        core::has_any(state, ResourceState::ShaderRead | ResourceState::ShaderWrite | ResourceState::UniformBuffer);
    const VkPipelineStageFlags2 shaders = shader_state ? vk_shader_stages(shader_stages) : VK_PIPELINE_STAGE_2_NONE;

    add(ResourceState::RenderTarget, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL);
    add(ResourceState::DepthWrite, kDepthStages,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL);
    add(ResourceState::DepthRead, kDepthStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
        VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL);
    add(ResourceState::ShaderRead, shaders, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL);
    add(ResourceState::ShaderWrite, shaders, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_IMAGE_LAYOUT_GENERAL);
    add(ResourceState::UniformBuffer, shaders, VK_ACCESS_2_UNIFORM_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    add(ResourceState::VertexBuffer, VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
        VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    add(ResourceState::IndexBuffer, VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED);
    add(ResourceState::IndirectArgument, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED);
    add(ResourceState::CopySrc, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    add(ResourceState::CopyDst, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    // Presentation is ordered by the frame's semaphore signal; the stage anchors the dependency
    // chain with the next acquire, whose semaphore wait sits at the color output stage.
    add(ResourceState::Present, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    add(ResourceState::HostRead, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    add(ResourceState::General, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL);
    return info; // Undefined: no stages, no access, UNDEFINED layout
}

VkImageViewType vk_view_type(const TextureDesc& desc, bool storage) {
    if (desc.dimension == TextureDimension::Texture3D) {
        return VK_IMAGE_VIEW_TYPE_3D;
    }
    if (desc.dimension == TextureDimension::Cube && !storage) {
        return desc.layer_count > 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
    }
    return desc.layer_count > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
}

const char* vk_result_name(VkResult result) {
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_NOT_READY:
        return "VK_NOT_READY";
    case VK_TIMEOUT:
        return "VK_TIMEOUT";
    case VK_INCOMPLETE:
        return "VK_INCOMPLETE";
    case VK_SUBOPTIMAL_KHR:
        return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED:
        return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:
        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:
        return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
        return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS:
        return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_SURFACE_LOST_KHR:
        return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
        return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR:
        return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
        return "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT";
    default:
        return "VkResult (unnamed)";
    }
}

void vk_check(VkResult result, const char* call, const char* file, int line) {
    if (result != VK_SUCCESS) {
        core::log_fatal("vulkan", file, line, "%s failed: %s (%d)", call, vk_result_name(result),
                        static_cast<int>(result));
    }
}

} // namespace de::rhi
