#include "render/rhi/rhi.h"

#include "core/assert.h"

#include <array>

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

const char* resource_state_name(ResourceState state) {
    switch (state) {
    case ResourceState::Undefined:
        return "undefined";
    case ResourceState::General:
        return "general";
    case ResourceState::RenderTarget:
        return "render_target";
    case ResourceState::DepthWrite:
        return "depth_write";
    case ResourceState::DepthRead:
        return "depth_read";
    case ResourceState::ShaderRead:
        return "shader_read";
    case ResourceState::ShaderWrite:
        return "shader_write";
    case ResourceState::CopySrc:
        return "copy_src";
    case ResourceState::CopyDst:
        return "copy_dst";
    case ResourceState::Present:
        return "present";
    case ResourceState::VertexBuffer:
        return "vertex_buffer";
    case ResourceState::IndexBuffer:
        return "index_buffer";
    case ResourceState::UniformBuffer:
        return "uniform_buffer";
    case ResourceState::IndirectArgument:
        return "indirect_argument";
    case ResourceState::HostRead:
        return "host_read";
    case ResourceState::Count:
        break;
    }
    return "unknown";
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
