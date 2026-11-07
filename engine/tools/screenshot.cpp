#include "tools/screenshot.h"

#include "core/enum_flags.h"
#include "core/profile.h"

#include <cstring>
#include <limits>

namespace {

void* tools_malloc(size_t size) {
    return de::core::tagged_malloc(de::core::MemoryTag::Tools, size);
}

void* tools_realloc(void* pointer, size_t size) {
    return de::core::tagged_realloc(de::core::MemoryTag::Tools, pointer, size);
}

void tools_free(void* pointer) {
    de::core::tagged_free(de::core::MemoryTag::Tools, pointer);
}

} // namespace

// stb's own names and configuration. Static: the tests, which link this, have an stb_image_write of their own.
// NOLINTBEGIN(readability-identifier-naming, bugprone-macro-parentheses)
#define STBIW_MALLOC(size) tools_malloc(size)
#define STBIW_REALLOC(pointer, size) tools_realloc(pointer, size)
#define STBIW_FREE(pointer) tools_free(pointer)
#define STBI_WRITE_NO_STDIO // files go through the VFS
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
// NOLINTEND(readability-identifier-naming, bugprone-macro-parentheses)

#include <stb_image_write.h>

namespace de::tools {

namespace {

constexpr size_t kBytesPerTexel = 4;
constexpr size_t kPngChannels = 3;

bool is_bgra(rhi::Format format) {
    return format == rhi::Format::BGRA8Unorm || format == rhi::Format::BGRA8Srgb;
}

bool is_rgba(rhi::Format format) {
    return format == rhi::Format::RGBA8Unorm || format == rhi::Format::RGBA8Srgb;
}

// Where stb's encoder hands over what it made: all of it, in one go.
void keep_png(void* context, void* data, int size) {
    auto& png = *static_cast<core::Array<std::byte>*>(context);
    png.resize(static_cast<size_t>(size));
    std::memcpy(png.data(), data, static_cast<size_t>(size));
}

} // namespace

core::Result<core::Array<std::byte>> encode_screenshot_png(std::span<const std::byte> texels, uint32_t width,
                                                           uint32_t height, rhi::Format format,
                                                           core::Allocator& allocator) {
    if (!is_bgra(format) && !is_rgba(format)) {
        return core::fail(core::ErrorCode::Unsupported, "a screenshot takes 8-bit RGBA or BGRA");
    }
    const uint64_t texel_count = uint64_t{width} * height;
    constexpr auto kMostRowBytes = static_cast<uint64_t>(std::numeric_limits<int>::max());
    if (texel_count == 0 || uint64_t{width} * kPngChannels > kMostRowBytes || uint64_t{height} > kMostRowBytes) {
        return core::fail(core::ErrorCode::InvalidArgument, "a screenshot's size is out of range");
    }
    if (texels.size() != texel_count * kBytesPerTexel) {
        return core::fail(core::ErrorCode::InvalidArgument, "a screenshot's texels don't fit its size");
    }
    core::Array<std::byte> rgb(core::heap_allocator(core::MemoryTag::Tools));
    rgb.resize(static_cast<size_t>(texel_count * kPngChannels));
    const size_t red = is_bgra(format) ? 2 : 0;
    const size_t blue = is_bgra(format) ? 0 : 2;
    for (size_t texel = 0; texel < texel_count; ++texel) {
        const std::byte* const in = texels.data() + (texel * kBytesPerTexel);
        std::byte* const out = rgb.data() + (texel * kPngChannels);
        out[0] = in[red];
        out[1] = in[1];
        out[2] = in[blue];
    }
    core::Array<std::byte> png(allocator);
    const int written =
        stbi_write_png_to_func(&keep_png, &png, static_cast<int>(width), static_cast<int>(height),
                               static_cast<int>(kPngChannels), rgb.data(), static_cast<int>(width * kPngChannels));
    if (written == 0 || png.empty()) {
        return core::fail(core::ErrorCode::OutOfMemory, "the screenshot's PNG couldn't be made");
    }
    return png;
}

core::Result<void> Screenshot::add_pass(rhi::Device& device, render::RenderGraph& graph, render::GraphTexture target,
                                        rhi::TextureHandle texture) {
    DE_PROFILE_ZONE("screenshot");
    const rhi::TextureDesc* const desc = device.texture_desc(texture);
    if (desc == nullptr || !core::has_any(desc->usage, rhi::TextureUsage::CopySrc)) {
        return core::fail(core::ErrorCode::Unsupported, "a screenshot's texture has no CopySrc usage");
    }
    if (!is_bgra(desc->format) && !is_rgba(desc->format)) {
        return core::fail(core::ErrorCode::Unsupported, "a screenshot takes 8-bit RGBA or BGRA");
    }
    const uint64_t size = uint64_t{desc->width} * desc->height * kBytesPerTexel;
    if (readback_.is_valid() && (desc->width != width_ || desc->height != height_)) {
        device.destroy(readback_);
        readback_ = {};
    }
    if (!readback_.is_valid()) {
        const core::Result<rhi::BufferHandle> made = device.create_buffer({.size = size,
                                                                           .usage = rhi::BufferUsage::CopyDst,
                                                                           .memory = rhi::MemoryLocation::Readback,
                                                                           .debug_name = "screenshot readback"});
        if (!made) {
            return core::fail(made.error());
        }
        readback_ = *made;
        readback_used_ = false;
    }
    width_ = desc->width;
    height_ = desc->height;
    format_ = desc->format;
    const render::GraphBuffer readback =
        graph.import_buffer({.buffer = readback_,
                             .name = "screenshot readback",
                             .state = readback_used_ ? rhi::ResourceState::HostRead : rhi::ResourceState::Undefined,
                             .final_state = rhi::ResourceState::HostRead});
    readback_used_ = true;
    graph.add_pass(
        "screenshot",
        [&](render::PassBuilder& pass) {
            pass.use(target, rhi::ResourceState::CopySrc);
            pass.use(readback, rhi::ResourceState::CopyDst);
        },
        [target, readback](render::PassContext& context) {
            context.list.copy_texture_to_buffer(context.texture(target), context.buffer(readback));
        });
    picture_pending_ = true;
    return {};
}

core::Result<core::Array<std::byte>> Screenshot::take_png(rhi::Device& device, core::Allocator& allocator) {
    if (!picture_pending_) {
        return core::fail(core::ErrorCode::InvalidArgument, "no screenshot was recorded");
    }
    picture_pending_ = false;
    device.wait_idle();
    device.invalidate_mapped(readback_);
    return encode_screenshot_png(device.mapped(readback_), width_, height_, format_, allocator);
}

void Screenshot::release(rhi::Device& device) {
    if (readback_.is_valid()) {
        device.destroy(readback_);
    }
    readback_ = {};
    readback_used_ = false;
    picture_pending_ = false;
}

} // namespace de::tools
