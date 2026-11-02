#include "core/function_ref.h"
#include "render/rhi/device.h"
#include "support/gpu.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

using namespace de::rhi;
using de::test::check_clean;
using de::test::make_device;

namespace {

// Records one frame, submits it and waits for the GPU.
void run_frame(Device& device, de::core::FunctionRef<void(CommandList&)> record) {
    device.begin_frame();
    CommandList list = device.begin_command_list();
    record(list);
    device.end_frame();
    device.wait_idle();
}

bool near_byte(uint8_t value, int expected) {
    return std::abs(static_cast<int>(value) - expected) <= 1;
}

} // namespace

TEST_CASE("format info") {
    CHECK(format_info(Format::RGBA8Unorm).block_bytes == 4);
    CHECK(format_info(Format::RGBA16Float).block_bytes == 8);
    CHECK(format_info(Format::BC7Srgb).block_bytes == 16);
    CHECK(format_info(Format::BC7Srgb).block_width == 4);
    CHECK(format_info(Format::BC7Srgb).srgb);
    CHECK(format_info(Format::BC1Unorm).block_bytes == 8);
    CHECK(format_info(Format::D32FloatS8Uint).depth);
    CHECK(format_info(Format::D32FloatS8Uint).stencil);
    CHECK_FALSE(format_info(Format::D32Float).stencil);
    CHECK(std::strcmp(format_info(Format::BGRA8Srgb).name, "bgra8_srgb") == 0);
}

TEST_CASE("a device reports what the GPU can do") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const Capabilities& caps = device->capabilities();
    CHECK(caps.device_name[0] != '\0');
    CHECK((caps.api_version_major > 1 || caps.api_version_minor >= 3));
    CHECK(caps.max_texture_size_2d >= 4096);
    CHECK(caps.max_bindless_textures >= 1000);
    CHECK(
        caps.supports(Format::RGBA8Unorm, FormatSupport::Sampled | FormatSupport::RenderTarget | FormatSupport::Blend));
    CHECK(caps.supports(Format::RGBA16Float, FormatSupport::RenderTarget | FormatSupport::Storage));
    CHECK(caps.supports(Format::D32Float, FormatSupport::DepthStencil | FormatSupport::Sampled));
    CHECK(caps.supports(Format::BC7Unorm, FormatSupport::Sampled));
    CHECK(device->swapchain_format() == Format::Unknown); // headless
    CHECK(device->validation_enabled());
    check_clean(*device);
}

TEST_CASE("only one device exists at a time") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const auto second = Device::create({.validation = false});
    REQUIRE_FALSE(second.ok());
    CHECK(second.error().code == de::core::ErrorCode::AlreadyExists);
}

TEST_CASE("invalid resource descriptions are rejected") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const auto zero =
        device->create_texture({.width = 0, .format = Format::RGBA8Unorm, .usage = TextureUsage::Sampled});
    CHECK(zero.error().code == de::core::ErrorCode::InvalidArgument);
    const auto no_usage = device->create_texture({.width = 4, .height = 4, .format = Format::RGBA8Unorm});
    CHECK(no_usage.error().code == de::core::ErrorCode::InvalidArgument);
    const auto cube = device->create_texture({.width = 8,
                                              .height = 4,
                                              .layer_count = 6,
                                              .format = Format::RGBA8Unorm,
                                              .usage = TextureUsage::Sampled,
                                              .dimension = TextureDimension::Cube});
    CHECK(cube.error().code == de::core::ErrorCode::InvalidArgument);
    const auto mips = device->create_texture(
        {.width = 8, .height = 8, .mip_count = 5, .format = Format::RGBA8Unorm, .usage = TextureUsage::Sampled});
    CHECK(mips.error().code == de::core::ErrorCode::InvalidArgument);
    const auto depth_storage = device->create_texture(
        {.width = 8, .height = 8, .format = Format::D32Float, .usage = TextureUsage::RenderTarget});
    CHECK(depth_storage.error().code == de::core::ErrorCode::Unsupported);
    const auto empty = device->create_buffer({.size = 0, .usage = BufferUsage::Storage});
    CHECK(empty.error().code == de::core::ErrorCode::InvalidArgument);
    CHECK(device->stats().live_textures == 0);
    check_clean(*device);
}

TEST_CASE("every texture shape can be created") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const TextureDesc descs[] = {
        {.width = 64, .height = 32, .mip_count = 7, .format = Format::RGBA8Srgb, .usage = TextureUsage::Sampled},
        {.width = 32,
         .height = 32,
         .layer_count = 4,
         .format = Format::RGBA16Float,
         .usage = TextureUsage::Sampled | TextureUsage::Storage},
        {.width = 16,
         .height = 16,
         .layer_count = 6,
         .format = Format::RGBA16Float,
         .usage = TextureUsage::Sampled,
         .dimension = TextureDimension::Cube},
        {.width = 16,
         .height = 16,
         .mip_count = 5,
         .layer_count = 12,
         .format = Format::RGBA16Float,
         .usage = TextureUsage::Sampled,
         .dimension = TextureDimension::Cube},
        {.width = 32,
         .height = 32,
         .depth = 16,
         .mip_count = 6,
         .format = Format::R16Float,
         .usage = TextureUsage::Sampled | TextureUsage::Storage,
         .dimension = TextureDimension::Texture3D},
        {.width = 128, .height = 128, .format = Format::D32Float, .usage = TextureUsage::DepthStencil},
        {.width = 64,
         .height = 64,
         .mip_count = 7,
         .format = Format::BC7Srgb,
         .usage = TextureUsage::Sampled | TextureUsage::CopyDst},
    };
    for (const TextureDesc& desc : descs) {
        const auto texture = device->create_texture(desc);
        CHECK(texture.ok());
        if (texture.ok()) {
            CHECK(device->texture_desc(*texture)->layer_count == desc.layer_count);
            device->destroy(*texture);
        }
    }
    device->wait_idle();
    CHECK(device->stats().pending_destroys == 0);
    check_clean(*device);
}

TEST_CASE("clear a render target and read it back") {
    auto device = make_device();
    if (!device) {
        return;
    }
    constexpr uint32_t kWidth = 64;
    constexpr uint32_t kHeight = 32;
    constexpr uint64_t kBytes = uint64_t{kWidth} * kHeight * 4;
    const auto target = device->create_texture({.width = kWidth,
                                                .height = kHeight,
                                                .format = Format::RGBA8Unorm,
                                                .usage = TextureUsage::RenderTarget | TextureUsage::CopySrc,
                                                .debug_name = "test target"});
    REQUIRE(target.ok());
    const auto readback = device->create_buffer({.size = kBytes,
                                                 .usage = BufferUsage::CopyDst,
                                                 .memory = MemoryLocation::Readback,
                                                 .debug_name = "test readback"});
    REQUIRE(readback.ok());

    run_frame(*device, [&](CommandList& list) {
        const ScopedDebugLabel label(list, "clear test");
        list.barrier(*target, ResourceState::Undefined, ResourceState::RenderTarget);
        const ColorAttachment color{.texture = *target, .clear = {.r = 0.25f, .g = 0.5f, .b = 0.75f, .a = 1.0f}};
        list.begin_rendering({.colors = std::span(&color, 1)});
        list.end_rendering();
        list.barrier(*target, ResourceState::RenderTarget, ResourceState::CopySrc);
        list.copy_texture_to_buffer(*target, *readback);
        list.barrier(*readback, ResourceState::CopyDst, ResourceState::HostRead);
    });

    device->invalidate_mapped(*readback);
    const auto pixels = device->mapped(*readback);
    REQUIRE(pixels.size() == kBytes);
    int wrong = 0;
    for (size_t i = 0; i < pixels.size(); i += 4) {
        const auto* texel = reinterpret_cast<const uint8_t*>(&pixels[i]);
        if (!near_byte(texel[0], 64) || !near_byte(texel[1], 128) || !near_byte(texel[2], 191) || texel[3] != 255) {
            ++wrong;
        }
    }
    CHECK(wrong == 0);

    device->destroy(*target);
    device->destroy(*readback);
    check_clean(*device);
}

TEST_CASE("clear a depth buffer and read it back") {
    auto device = make_device();
    if (!device) {
        return;
    }
    if (!device->capabilities().supports(Format::D32Float, FormatSupport::DepthStencil | FormatSupport::CopySrc)) {
        MESSAGE("skipped: d32_float can't be copied on this GPU");
        return;
    }
    const auto depth = device->create_texture({.width = 16,
                                               .height = 16,
                                               .format = Format::D32Float,
                                               .usage = TextureUsage::DepthStencil | TextureUsage::CopySrc,
                                               .debug_name = "test depth"});
    REQUIRE(depth.ok());
    const auto readback = device->create_buffer(
        {.size = uint64_t{16} * 16 * 4, .usage = BufferUsage::CopyDst, .memory = MemoryLocation::Readback});
    REQUIRE(readback.ok());

    run_frame(*device, [&](CommandList& list) {
        list.barrier(*depth, ResourceState::Undefined, ResourceState::DepthWrite);
        const DepthAttachment attachment{.texture = *depth, .clear_depth = 0.5f};
        list.begin_rendering({.depth = &attachment});
        list.end_rendering();
        list.barrier(*depth, ResourceState::DepthWrite, ResourceState::CopySrc);
        list.copy_texture_to_buffer(*depth, *readback);
        list.barrier(*readback, ResourceState::CopyDst, ResourceState::HostRead);
    });

    device->invalidate_mapped(*readback);
    const auto bytes = device->mapped(*readback);
    float first = 0.0f;
    float last = 0.0f;
    std::memcpy(&first, bytes.data(), sizeof(float));
    std::memcpy(&last, bytes.data() + bytes.size() - sizeof(float), sizeof(float));
    CHECK(first == 0.5f);
    CHECK(last == 0.5f);

    device->destroy(*depth);
    device->destroy(*readback);
    check_clean(*device);
}

TEST_CASE("upload a texture mip through a staging buffer and read it back") {
    auto device = make_device();
    if (!device) {
        return;
    }
    // Mip 1 of a 16x16 texture is 8x8.
    constexpr uint32_t kMipSize = 8;
    constexpr uint32_t kBytes = kMipSize * kMipSize * 4;
    const auto texture =
        device->create_texture({.width = 16,
                                .height = 16,
                                .mip_count = 2,
                                .format = Format::RGBA8Unorm,
                                .usage = TextureUsage::Sampled | TextureUsage::CopyDst | TextureUsage::CopySrc});
    REQUIRE(texture.ok());
    const auto staging =
        device->create_buffer({.size = kBytes, .usage = BufferUsage::CopySrc, .memory = MemoryLocation::Upload});
    const auto readback =
        device->create_buffer({.size = kBytes, .usage = BufferUsage::CopyDst, .memory = MemoryLocation::Readback});
    REQUIRE((staging.ok() && readback.ok()));

    auto upload = device->mapped(*staging);
    REQUIRE(upload.size() == kBytes);
    for (size_t i = 0; i < upload.size(); ++i) {
        upload[i] = static_cast<std::byte>((i * 7) & 0xFF);
    }
    device->flush_mapped(*staging);

    run_frame(*device, [&](CommandList& list) {
        list.barrier(*texture, ResourceState::Undefined, ResourceState::CopyDst);
        list.copy_buffer_to_texture(*staging, *texture, {.mip = 1});
        list.barrier(*texture, ResourceState::CopyDst, ResourceState::CopySrc);
        list.copy_texture_to_buffer(*texture, *readback, {.mip = 1});
        list.barrier(*readback, ResourceState::CopyDst, ResourceState::HostRead);
    });

    device->invalidate_mapped(*readback);
    const auto result = device->mapped(*readback);
    CHECK(std::memcmp(result.data(), upload.data(), kBytes) == 0);

    device->destroy(*texture);
    device->destroy(*staging);
    device->destroy(*readback);
    check_clean(*device);
}

TEST_CASE("fill and copy buffers") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const auto gpu = device->create_buffer({.size = 1024,
                                            .usage = BufferUsage::Storage | BufferUsage::CopySrc | BufferUsage::CopyDst,
                                            .debug_name = "gpu"});
    const auto readback =
        device->create_buffer({.size = 1024, .usage = BufferUsage::CopyDst, .memory = MemoryLocation::Readback});
    REQUIRE((gpu.ok() && readback.ok()));
    CHECK(device->mapped(*gpu).empty()); // GpuOnly isn't mapped

    run_frame(*device, [&](CommandList& list) {
        list.fill_buffer(*gpu, 0, 1024, 0xDEADBEEF);
        list.fill_buffer(*readback, 0, 1024, 0); // the copy below leaves the first 256 bytes alone
        list.barrier(*gpu, ResourceState::CopyDst, ResourceState::CopySrc);
        list.barrier(*readback, ResourceState::CopyDst, ResourceState::CopyDst);
        list.copy_buffer(*gpu, 0, *readback, 256, 768);
        list.barrier(*readback, ResourceState::CopyDst, ResourceState::HostRead);
    });

    device->invalidate_mapped(*readback);
    const auto bytes = device->mapped(*readback);
    uint32_t words[256];
    std::memcpy(words, bytes.data(), sizeof(words));
    CHECK(words[0] == 0);
    CHECK(words[63] == 0);
    CHECK(words[64] == 0xDEADBEEF);
    CHECK(words[255] == 0xDEADBEEF);

    device->destroy(*gpu);
    device->destroy(*readback);
    check_clean(*device);
}

TEST_CASE("destroyed resources live until the GPU is done with them") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const auto texture = device->create_texture(
        {.width = 32, .height = 32, .format = Format::RGBA8Unorm, .usage = TextureUsage::RenderTarget});
    REQUIRE(texture.ok());
    CHECK(device->stats().live_textures == 1);

    // Used and destroyed in the same frame: the image must outlive the GPU work.
    device->begin_frame();
    CommandList list = device->begin_command_list();
    list.barrier(*texture, ResourceState::Undefined, ResourceState::RenderTarget);
    const ColorAttachment color{.texture = *texture};
    list.begin_rendering({.colors = std::span(&color, 1)});
    list.end_rendering();
    device->destroy(*texture);
    CHECK(device->texture_desc(*texture) == nullptr); // the handle is stale at once
    device->destroy(*texture);                        // and destroying it again is ignored
    CHECK(device->stats().live_textures == 0);
    CHECK(device->stats().pending_destroys == 1);
    device->end_frame();

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        device->begin_frame();
        device->end_frame();
    }
    CHECK(device->stats().pending_destroys == 0);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("frames keep pace with the GPU") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const auto buffer = device->create_buffer({.size = 4096, .usage = BufferUsage::Storage | BufferUsage::CopyDst});
    REQUIRE(buffer.ok());
    constexpr uint32_t kFrames = 4 * kFramesInFlight + 1;
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        device->begin_frame();
        // Several lists per frame, submitted in order.
        for (uint32_t i = 0; i < 3; ++i) {
            CommandList list = device->begin_command_list();
            if (frame > 0 || i > 0) {
                list.barrier(*buffer, ResourceState::CopyDst, ResourceState::CopyDst);
            }
            list.fill_buffer(*buffer, 0, 4096, frame);
        }
        device->end_frame();
    }
    CHECK(device->stats().frame_index == kFrames);
    device->destroy(*buffer);
    device->wait_idle();
    CHECK(device->stats().pending_destroys == 0);
    check_clean(*device);
}
