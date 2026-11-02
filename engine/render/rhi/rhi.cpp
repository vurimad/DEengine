#include "render/rhi/rhi.h"

#include "core/assert.h"

#include <algorithm>
#include <array>
#include <iterator>

namespace de::rhi {

namespace {

constexpr FormatInfo color(const char* name, uint8_t bytes, bool srgb = false) {
    return {.name = name, .block_bytes = bytes, .srgb = srgb};
}

constexpr FormatInfo depth(const char* name, uint8_t bytes, bool stencil) {
    return {.name = name, .block_bytes = bytes, .depth = true, .stencil = stencil};
}

constexpr FormatInfo block_compressed(const char* name, uint8_t bytes, bool srgb = false) {
    return {.name = name, .block_bytes = bytes, .block_width = 4, .block_height = 4, .srgb = srgb};
}

// Indexed by Format; the static_assert below keeps it in step with the enum.
constexpr std::array<FormatInfo, kFormatCount> kFormats = {{
    {},
    color("r8_unorm", 1),
    color("rg8_unorm", 2),
    color("rgba8_unorm", 4),
    color("rgba8_srgb", 4, true),
    color("bgra8_unorm", 4),
    color("bgra8_srgb", 4, true),
    color("r16_unorm", 2),
    color("rg16_unorm", 4),
    color("rgba16_unorm", 8),
    color("r16_float", 2),
    color("rg16_float", 4),
    color("rgba16_float", 8),
    color("r32_uint", 4),
    color("rg32_uint", 8),
    color("rgba32_uint", 16),
    color("r32_float", 4),
    color("rg32_float", 8),
    color("rgba32_float", 16),
    color("rgb10a2_unorm", 4),
    color("rg11b10_float", 4),
    depth("d16_unorm", 2, false),
    depth("d32_float", 4, false),
    depth("d24_unorm_s8_uint", 4, true),
    depth("d32_float_s8_uint", 8, true), // copies move the depth and stencil aspects separately
    block_compressed("bc1_unorm", 8),
    block_compressed("bc1_srgb", 8, true),
    block_compressed("bc3_unorm", 16),
    block_compressed("bc3_srgb", 16, true),
    block_compressed("bc4_unorm", 8),
    block_compressed("bc5_unorm", 16),
    block_compressed("bc6h_ufloat", 16),
    block_compressed("bc7_unorm", 16),
    block_compressed("bc7_srgb", 16, true),
}};
static_assert(kFormats.back().block_width == 4 && kFormats.back().srgb, "kFormats is out of step with Format");

} // namespace

const FormatInfo& format_info(Format format) {
    const auto index = static_cast<size_t>(format);
    DE_ASSERT(index < kFormatCount);
    return kFormats[index < kFormatCount ? index : 0];
}

Format storage_format(Format format) {
    switch (format) {
    case Format::RGBA8Srgb:
        return Format::RGBA8Unorm;
    case Format::BGRA8Srgb:
        return Format::BGRA8Unorm;
    default:
        return format; // block-compressed sRGB formats have no storage at all
    }
}

namespace {

constexpr ResourceState kBufferOnlyStates = ResourceState::UniformBuffer | ResourceState::VertexBuffer |
                                            ResourceState::IndexBuffer | ResourceState::IndirectArgument |
                                            ResourceState::HostRead;
constexpr ResourceState kTextureOnlyStates =
    ResourceState::RenderTarget | ResourceState::DepthWrite | ResourceState::DepthRead | ResourceState::Present;
// The only read states that share a texture layout (read-only optimal).
constexpr ResourceState kSharedLayoutReads = ResourceState::DepthRead | ResourceState::ShaderRead;

// Indexed by bit position in ResourceState.
constexpr const char* kStateNames[] = {
    "render_target",  "depth_write",   "depth_read",   "shader_read",       "shader_write",
    "uniform_buffer", "vertex_buffer", "index_buffer", "indirect_argument", "copy_src",
    "copy_dst",       "present",       "host_read",    "general",
};
static_assert(static_cast<uint32_t>(kAllResourceStates) == (1U << std::size(kStateNames)) - 1,
              "kStateNames is out of step with ResourceState");
static_assert(ResourceState::General == static_cast<ResourceState>(1U << (std::size(kStateNames) - 1)),
              "kStateNames is out of step with ResourceState");

bool single_bit(ResourceState state) {
    const auto bits = static_cast<uint32_t>(state);
    return bits != 0 && (bits & (bits - 1)) == 0;
}

} // namespace

MipExtent mip_extent(const TextureDesc& desc, uint32_t mip) {
    const auto shrink = [mip](uint32_t size) { return mip < 32 ? std::max(size >> mip, 1U) : 1U; };
    return {.width = shrink(desc.width),
            .height = shrink(desc.height),
            .depth = desc.dimension == TextureDimension::Texture3D ? shrink(desc.depth) : 1U};
}

bool is_valid_state(ResourceState state, bool texture) {
    if ((state & ~kAllResourceStates) != ResourceState::Undefined) {
        return false; // unknown bits
    }
    if (state == ResourceState::Undefined) {
        return true;
    }
    if (core::has_any(state, kWriteStates) && !single_bit(state)) {
        return false; // a write state stands alone
    }
    if (texture) {
        if (core::has_any(state, kBufferOnlyStates)) {
            return false;
        }
        return single_bit(state) || (state & ~kSharedLayoutReads) == ResourceState::Undefined;
    }
    return !core::has_any(state, kTextureOnlyStates);
}

core::FixedString<255> resource_state_name(ResourceState state) {
    core::FixedString<255> name;
    if (state == ResourceState::Undefined) {
        name.assign("undefined");
        return name;
    }
    for (size_t bit = 0; bit < std::size(kStateNames); ++bit) {
        if ((static_cast<uint32_t>(state) & (1U << bit)) != 0) {
            // Every name together fits in 255 characters, so the appends always succeed.
            const bool fits = (name.empty() || name.append('|')) && name.append(kStateNames[bit]);
            DE_ASSERT(fits);
            static_cast<void>(fits);
        }
    }
    if ((state & ~kAllResourceStates) != ResourceState::Undefined) {
        const bool fits = name.append(name.empty() ? "unknown" : "|unknown");
        DE_ASSERT(fits);
        static_cast<void>(fits);
    }
    return name;
}

const char* shader_stage_name(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::Vertex:
        return "vertex";
    case ShaderStage::Fragment:
        return "fragment";
    case ShaderStage::Compute:
        return "compute";
    case ShaderStage::Count:
        break;
    }
    return "unknown";
}

const char* gpu_vendor_name(GpuVendor vendor) {
    switch (vendor) {
    case GpuVendor::Unknown:
        return "unknown";
    case GpuVendor::Nvidia:
        return "NVIDIA";
    case GpuVendor::Amd:
        return "AMD";
    case GpuVendor::Intel:
        return "Intel";
    case GpuVendor::Arm:
        return "Arm";
    case GpuVendor::Qualcomm:
        return "Qualcomm";
    case GpuVendor::Apple:
        return "Apple";
    case GpuVendor::Software:
        return "software";
    }
    return "unknown";
}

const char* gpu_type_name(GpuType type) {
    switch (type) {
    case GpuType::Other:
        return "other";
    case GpuType::Discrete:
        return "discrete";
    case GpuType::Integrated:
        return "integrated";
    case GpuType::Virtual:
        return "virtual";
    case GpuType::Cpu:
        return "cpu";
    }
    return "unknown";
}

} // namespace de::rhi
