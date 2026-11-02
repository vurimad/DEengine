#include "core/array.h"
#include "render/rhi/device.h"
#include "render/rhi/mips.h"
#include "render/rhi/upload.h"
#include "support/gpu.h"
#include "support/readback.h"
#include "support/shader_kit.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>

using namespace de::rhi;
using de::test::as_span;
using de::test::check_clean;
using de::test::make_device;
using de::test::pattern;
using de::test::read_texture;
using de::test::SampleTexturePush;
using de::test::ShaderKit;
using de::test::test_heap;
using de::test::transfer_path;
using de::test::WriteStoragePush;

namespace {

constexpr TextureUsage kMipUsage = TextureUsage::Sampled | TextureUsage::CopySrc | TextureUsage::CopyDst;

// Byte offset of mip m (all layers) in texture_upload_size() order.
size_t mip_offset(const TextureDesc& desc, uint32_t mip) {
    return mip == 0 ? 0 : static_cast<size_t>(texture_upload_size(desc, {.first_mip = 0, .mip_count = mip}));
}

// An RGBA8 texel of a mip read back (one layer).
const std::byte* texel(std::span<const std::byte> bytes, const TextureDesc& desc, uint32_t mip, uint32_t layer,
                       uint32_t x, uint32_t y) {
    const MipExtent extent = mip_extent(desc, mip);
    const size_t layer_bytes = size_t{extent.width} * extent.height * extent.depth * 4;
    return bytes.data() + mip_offset(desc, mip) + (layer * layer_bytes) + ((size_t{y} * extent.width + x) * 4);
}

float srgb_to_linear(float value) {
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float value) {
    return value <= 0.0031308f ? value * 12.92f : (1.055f * std::pow(value, 1.0f / 2.4f)) - 0.055f;
}

// Every texel of every mip and layer is this RGBA8 color.
bool all_texels(std::span<const std::byte> bytes, const std::byte (&color)[4]) {
    for (size_t i = 0; i < bytes.size(); i += 4) {
        if (std::memcmp(bytes.data() + i, color, 4) != 0) {
            return false;
        }
    }
    return true;
}

// Samples texel centers of a width x height texture (mip 0) into a float4 per texel.
void sample_into(Device& device, ShaderKit& kit, de::shader::ProgramHandle program, uint32_t texture,
                 BufferHandle output, uint32_t width, uint32_t height) {
    device.begin_frame();
    CommandList list = device.begin_command_list();
    list.bind_pipeline(kit.pipeline(program));
    list.push_constants(SampleTexturePush{.output = device.buffer_address(output),
                                          .texture = texture,
                                          .sampler = sampler_index(DefaultSampler::PointClamp),
                                          .width = width,
                                          .height = height});
    list.dispatch((width + 7) / 8, (height + 7) / 8);
    list.barrier(BufferBarrier{.buffer = output,
                               .before = ResourceState::ShaderWrite,
                               .after = ResourceState::HostRead,
                               .before_stages = ShaderStages::Compute});
    device.end_frame();
    device.wait_idle();
    device.invalidate_mapped(output);
}

bool near(float a, float b, float tolerance) {
    return std::fabs(a - b) <= tolerance;
}

} // namespace

TEST_CASE("generated mips average the mip above, 2x2 texels per texel") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const bool transfer = transfer_path(*device);
    auto uploader = Uploader::create(*device, {.use_transfer_queue = transfer});
    REQUIRE(uploader.ok());
    const TextureDesc desc{.width = 16, .height = 8, .mip_count = 5, .format = Format::RGBA8Unorm, .usage = kMipUsage};
    REQUIRE(can_generate_mips(*device, desc));
    const auto texture = *device->create_texture(desc);
    const auto top = pattern(size_t{16} * 8 * 4, 3);
    REQUIRE(uploader
                ->upload_texture(texture, as_span(top),
                                 {.mip_count = 1, .after = ResourceState::CopySrc, .generate_mips = true})
                .ok());
    uploader->flush_and_wait();
    const auto bytes = read_texture(*device, texture);
    CHECK(std::memcmp(bytes.data(), top.data(), top.size()) == 0); // the uploaded mip is untouched

    // Each texel within one of the average of its 2x2 parents (the filter rounds its own way).
    int wrong = 0;
    for (uint32_t mip = 1; mip < desc.mip_count; ++mip) {
        const MipExtent extent = mip_extent(desc, mip);
        const MipExtent parent = mip_extent(desc, mip - 1); // a side of 1 repeats its texel
        for (uint32_t y = 0; y < extent.height; ++y) {
            for (uint32_t x = 0; x < extent.width; ++x) {
                for (int c = 0; c < 4; ++c) {
                    int sum = 0;
                    for (uint32_t dy = 0; dy < 2; ++dy) {
                        for (uint32_t dx = 0; dx < 2; ++dx) {
                            sum += static_cast<int>(texel(as_span(bytes), desc, mip - 1, 0,
                                                          std::min((2 * x) + dx, parent.width - 1),
                                                          std::min((2 * y) + dy, parent.height - 1))[c]);
                        }
                    }
                    const int got = static_cast<int>(texel(as_span(bytes), desc, mip, 0, x, y)[c]);
                    wrong += std::abs((got * 4) - sum) > 4 ? 1 : 0;
                }
            }
        }
    }
    CHECK(wrong == 0);
    device->destroy(texture);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("sRGB mips are filtered in linear space") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto uploader = Uploader::create(*device, {.use_transfer_queue = transfer_path(*device)});
    REQUIRE(uploader.ok());
    // A black and white checker: its 1x1 mip is linear mid-gray, which sRGB stores as ~188, not 128.
    const TextureDesc desc{.width = 2, .height = 2, .mip_count = 2, .format = Format::RGBA8Srgb, .usage = kMipUsage};
    if (!can_generate_mips(*device, desc)) {
        MESSAGE("skipped: no sRGB blits");
        return;
    }
    const auto texture = *device->create_texture(desc);
    const std::byte b{0};
    const std::byte w{255};
    const std::byte checker[16] = {b, b, b, w, w, w, w, w, w, w, w, w, b, b, b, w};
    REQUIRE(
        uploader
            ->upload_texture(texture, checker, {.mip_count = 1, .after = ResourceState::CopySrc, .generate_mips = true})
            .ok());
    uploader->flush_and_wait();
    const auto bytes = read_texture(*device, texture);
    const auto expected = static_cast<int>(std::lround(linear_to_srgb(0.5f) * 255.0f));
    CHECK(std::abs(static_cast<int>(bytes[16]) - expected) <= 2);
    CHECK(static_cast<int>(bytes[19]) == 255);
    device->destroy(texture);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("mips generate for every layer, cube face and slice, at any size") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto uploader = Uploader::create(*device, {.use_transfer_queue = transfer_path(*device)});
    REQUIRE(uploader.ok());
    const std::byte color[4] = {std::byte{40}, std::byte{120}, std::byte{200}, std::byte{255}};
    const TextureDesc descs[] = {
        {.width = 8,
         .height = 8,
         .mip_count = 4,
         .layer_count = 6,
         .format = Format::RGBA8Unorm,
         .usage = kMipUsage,
         .dimension = TextureDimension::Cube},
        {.width = 8, .height = 4, .mip_count = 4, .layer_count = 3, .format = Format::RGBA8Unorm, .usage = kMipUsage},
        {.width = 4,
         .height = 4,
         .depth = 8,
         .mip_count = 4,
         .format = Format::RGBA8Unorm,
         .usage = kMipUsage,
         .dimension = TextureDimension::Texture3D},
        {.width = 7, .height = 5, .mip_count = 3, .format = Format::RGBA8Unorm, .usage = kMipUsage}, // not powers of 2
    };
    for (const TextureDesc& desc : descs) {
        const auto texture = *device->create_texture(desc);
        const size_t top_bytes = static_cast<size_t>(texture_upload_size(desc, {.mip_count = 1}));
        de::core::Array<std::byte> top(test_heap());
        for (size_t i = 0; i < top_bytes; i += 4) {
            for (const std::byte channel : color) {
                top.push(channel);
            }
        }
        REQUIRE(uploader
                    ->upload_texture(texture, as_span(top),
                                     {.mip_count = 1, .after = ResourceState::CopySrc, .generate_mips = true})
                    .ok());
        uploader->flush_and_wait();
        CHECK(all_texels(as_span(read_texture(*device, texture)), color)); // a solid color stays solid
        device->destroy(texture);
    }
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("generate_mips regenerates a texture's mips from any mip, between any states") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto uploader = Uploader::create(*device, {.use_transfer_queue = transfer_path(*device)});
    REQUIRE(uploader.ok());
    const TextureDesc desc{.width = 8, .height = 8, .mip_count = 4, .format = Format::RGBA8Unorm, .usage = kMipUsage};
    const auto texture = *device->create_texture(desc);
    // Every mip a different solid color, sampled from fragment shaders.
    de::core::Array<std::byte> texels(test_heap());
    for (uint32_t mip = 0; mip < desc.mip_count; ++mip) {
        const MipExtent extent = mip_extent(desc, mip);
        for (uint32_t i = 0; i < extent.width * extent.height; ++i) {
            texels.push(static_cast<std::byte>(mip * 60));
            texels.push(std::byte{0});
            texels.push(std::byte{0});
            texels.push(std::byte{255});
        }
    }
    REQUIRE(uploader->upload_texture(texture, as_span(texels), {.after_stages = ShaderStages::Fragment}).ok());
    uploader->flush_and_wait();

    // Mips 2 and 3 from mip 1 (color 60), and mips 1.. from mip 0 (color 0) after that.
    device->begin_frame();
    CommandList list = device->begin_command_list();
    REQUIRE(generate_mips(*device, list, texture,
                          {.source_mip = 1,
                           .before = ResourceState::ShaderRead,
                           .before_stages = ShaderStages::Fragment,
                           .after = ResourceState::CopySrc})
                .ok());
    list.barrier(TextureBarrier{.texture = texture,
                                .before = ResourceState::ShaderRead,
                                .after = ResourceState::CopySrc,
                                .before_stages = ShaderStages::Fragment,
                                .range = {.first_mip = 0, .mip_count = 1}}); // to read it back too
    device->end_frame();
    device->wait_idle();
    auto bytes = read_texture(*device, texture);
    CHECK(static_cast<int>(texel(as_span(bytes), desc, 0, 0, 0, 0)[0]) == 0);
    CHECK(static_cast<int>(texel(as_span(bytes), desc, 1, 0, 0, 0)[0]) == 60);
    CHECK(static_cast<int>(texel(as_span(bytes), desc, 2, 0, 0, 0)[0]) == 60);
    CHECK(static_cast<int>(texel(as_span(bytes), desc, 3, 0, 0, 0)[0]) == 60);

    device->begin_frame();
    list = device->begin_command_list();
    REQUIRE(generate_mips(*device, list, texture, {.before = ResourceState::CopySrc, .after = ResourceState::CopySrc})
                .ok());
    device->end_frame();
    device->wait_idle();
    bytes = read_texture(*device, texture);
    CHECK(static_cast<int>(texel(as_span(bytes), desc, 3, 0, 0, 0)[0]) == 0);

    // Nothing to generate, or no handle: refused, nothing recorded.
    device->begin_frame();
    list = device->begin_command_list();
    CHECK(generate_mips(*device, list, texture, {.source_mip = 3}).error().code == de::core::ErrorCode::OutOfRange);
    CHECK(generate_mips(*device, list, TextureHandle{}, {}).error().code == de::core::ErrorCode::InvalidArgument);
    device->end_frame();
    device->destroy(texture);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("mips are regenerated when the top mip is uploaded again") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto uploader = Uploader::create(*device, {.use_transfer_queue = transfer_path(*device)});
    REQUIRE(uploader.ok());
    const TextureDesc desc{.width = 4, .height = 4, .mip_count = 3, .format = Format::RGBA8Unorm, .usage = kMipUsage};
    const auto texture = *device->create_texture(desc);
    for (const int value : {10, 90, 170}) {
        de::core::Array<std::byte> top(test_heap());
        for (int i = 0; i < 16; ++i) {
            top.push(static_cast<std::byte>(value));
            top.push(std::byte{0});
            top.push(std::byte{0});
            top.push(std::byte{255});
        }
        REQUIRE(uploader
                    ->upload_texture(texture, as_span(top),
                                     {.mip_count = 1, .after = ResourceState::CopySrc, .generate_mips = true})
                    .ok());
        uploader->flush_and_wait();
        const auto bytes = read_texture(*device, texture);
        CHECK(static_cast<int>(texel(as_span(bytes), desc, 2, 0, 0, 0)[0]) == value);
    }
    device->destroy(texture);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("mip generation refuses what it can't do") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto uploader = Uploader::create(*device);
    REQUIRE(uploader.ok());
    const auto bytes = pattern(1024, 1);
    const std::span<const std::byte> data = as_span(bytes);
    const auto compressed = *device->create_texture(
        {.width = 8, .height = 8, .mip_count = 4, .format = Format::BC1Unorm, .usage = kMipUsage});
    const auto no_copy_src = *device->create_texture({.width = 4,
                                                      .height = 4,
                                                      .mip_count = 3,
                                                      .format = Format::RGBA8Unorm,
                                                      .usage = TextureUsage::Sampled | TextureUsage::CopyDst});
    const auto texture = *device->create_texture(
        {.width = 4, .height = 4, .mip_count = 3, .format = Format::RGBA8Unorm, .usage = kMipUsage});
    CHECK(uploader->upload_texture(compressed, data.first(32), {.mip_count = 1, .generate_mips = true}).error().code ==
          de::core::ErrorCode::Unsupported);
    CHECK(uploader->upload_texture(no_copy_src, data.first(64), {.mip_count = 1, .generate_mips = true}).error().code ==
          de::core::ErrorCode::Unsupported);
    // From mip 0 only, with something left to generate.
    CHECK(uploader->upload_texture(texture, data.first(16), {.first_mip = 1, .mip_count = 1, .generate_mips = true})
              .error()
              .code == de::core::ErrorCode::InvalidArgument);
    CHECK(uploader->upload_texture(texture, data.first(84), {.generate_mips = true}).error().code ==
          de::core::ErrorCode::InvalidArgument);
    CHECK(uploader->idle());
    device->destroy(compressed);
    device->destroy(no_copy_src);
    device->destroy(texture);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("sRGB textures are stored through UNORM views and sampled as sRGB") {
    auto device = make_device();
    if (!device) {
        return;
    }
    ShaderKit kit(*device);
    constexpr uint32_t kSize = 8;
    const auto texture = device->create_texture({.width = kSize,
                                                 .height = kSize,
                                                 .format = Format::RGBA8Srgb,
                                                 .usage = TextureUsage::Storage | TextureUsage::Sampled});
    REQUIRE(texture.ok()); // sRGB has no storage of its own on most GPUs: the UNORM twin stores
    CHECK(storage_format(Format::RGBA8Srgb) == Format::RGBA8Unorm);
    CHECK(device->storage_texture_index(*texture) != kInvalidBindlessIndex);

    // The storage view writes raw values (no encoding): blue = 0.25 lands as the byte 64.
    device->begin_frame();
    CommandList list = device->begin_command_list();
    list.barrier(*texture, ResourceState::Undefined, ResourceState::ShaderWrite);
    list.bind_pipeline(kit.pipeline(kit.compute("tests.write_storage")));
    list.push_constants(
        WriteStoragePush{.texture = device->storage_texture_index(*texture), .width = kSize, .height = kSize});
    list.dispatch(1, 1);
    list.barrier(TextureBarrier{.texture = *texture,
                                .before = ResourceState::ShaderWrite,
                                .after = ResourceState::ShaderRead,
                                .before_stages = ShaderStages::Compute,
                                .after_stages = ShaderStages::Compute});
    device->end_frame();
    device->wait_idle();

    // Sampled through the sRGB view, that byte decodes to linear.
    const auto output = *device->create_buffer(
        {.size = uint64_t{kSize} * kSize * 16, .usage = BufferUsage::Storage, .memory = MemoryLocation::Readback});
    sample_into(*device, kit, kit.compute("tests.sample_texture"), device->texture_index(*texture), output, kSize,
                kSize);
    float sampled[4];
    std::memcpy(sampled, device->mapped(output).data(), sizeof(sampled));
    CHECK(near(sampled[2], srgb_to_linear(64.0f / 255.0f), 1.0f / 256.0f));
    CHECK(near(sampled[3], 1.0f, 1.0f / 256.0f));
    device->destroy(*texture);
    device->destroy(output);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("more block formats sample right: BC3, BC5, sRGB BC1, sizes not a multiple of 4") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto uploader = Uploader::create(*device, {.use_transfer_queue = transfer_path(*device)});
    REQUIRE(uploader.ok());
    ShaderKit kit(*device);
    const auto program = kit.compute("tests.sample_texture");
    const auto output = *device->create_buffer(
        {.size = uint64_t{16} * 16 * 16, .usage = BufferUsage::Storage, .memory = MemoryLocation::Readback});

    // Solid blocks: BC1 with both endpoints c (every index 0 picks c0); BC4 with both endpoints v.
    const auto bc1_block = [](uint16_t c, de::core::Array<std::byte>& out) {
        const std::byte lo{static_cast<uint8_t>(c & 0xFFU)};
        const std::byte hi{static_cast<uint8_t>(c >> 8U)};
        for (const std::byte b : {lo, hi, lo, hi, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}}) {
            out.push(b);
        }
    };
    const auto bc4_block = [](uint8_t v, de::core::Array<std::byte>& out) {
        for (const std::byte b : {std::byte{v}, std::byte{v}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
                                  std::byte{0}, std::byte{0}}) {
            out.push(b);
        }
    };
    struct Case {
        TextureDesc desc;
        uint32_t blocks; // in every mip together
        float expected[4];
    };
    constexpr uint16_t kGray565 = (16U << 11U) | (32U << 5U) | 16U; // 16/31, 32/63, 16/31
    const float gray_r = srgb_to_linear(16.0f / 31.0f);
    const float gray_g = srgb_to_linear(32.0f / 63.0f);
    const Case cases[] = {
        // BC3: BC4 alpha (128) then BC1 color (red). 8x8, 2 mips: 4 + 1 blocks.
        {.desc = {.width = 8, .height = 8, .mip_count = 2, .format = Format::BC3Unorm},
         .blocks = 5,
         .expected = {1.0f, 0.0f, 0.0f, 128.0f / 255.0f}},
        // BC5: red (255) then green (64). 4x4: one block.
        {.desc = {.width = 4, .height = 4, .format = Format::BC5Unorm},
         .blocks = 1,
         .expected = {1.0f, 64.0f / 255.0f, 0.0f, 1.0f}},
        // sRGB BC1 gray, decoded to linear.
        {.desc = {.width = 4, .height = 4, .format = Format::BC1Srgb},
         .blocks = 1,
         .expected = {gray_r, gray_g, gray_r, 1.0f}},
        // BC1 red at 6x10 with 3 mips: 2x3 + 1x2 + 1x1 blocks.
        {.desc = {.width = 6, .height = 10, .mip_count = 3, .format = Format::BC1Unorm},
         .blocks = 9,
         .expected = {1.0f, 0.0f, 0.0f, 1.0f}},
    };
    for (const Case& test : cases) {
        CAPTURE(format_info(test.desc.format).name);
        TextureDesc desc = test.desc;
        desc.usage = TextureUsage::Sampled | TextureUsage::CopyDst;
        const auto texture = *device->create_texture(desc);
        de::core::Array<std::byte> data(test_heap());
        for (uint32_t i = 0; i < test.blocks; ++i) {
            switch (desc.format) {
            case Format::BC3Unorm:
                bc4_block(128, data);
                bc1_block(0xF800, data);
                break;
            case Format::BC5Unorm:
                bc4_block(255, data);
                bc4_block(64, data);
                break;
            case Format::BC1Srgb:
                bc1_block(kGray565, data);
                break;
            default:
                bc1_block(0xF800, data);
                break;
            }
        }
        REQUIRE(data.size() == texture_upload_size(desc, {}));
        REQUIRE(uploader->upload_texture(texture, as_span(data), {.after_stages = ShaderStages::Compute}).ok());
        uploader->flush_and_wait();
        sample_into(*device, kit, program, device->texture_index(texture), output, desc.width, desc.height);
        int wrong = 0;
        for (uint32_t i = 0; i < desc.width * desc.height; ++i) {
            float got[4];
            std::memcpy(got, device->mapped(output).data() + (size_t{i} * sizeof(got)), sizeof(got));
            for (int c = 0; c < 4; ++c) {
                wrong += near(got[c], test.expected[c], 2.0f / 255.0f) ? 0 : 1;
            }
        }
        CHECK(wrong == 0);
        device->destroy(texture);
    }
    device->destroy(output);
    device->wait_idle();
    check_clean(*device);
}
