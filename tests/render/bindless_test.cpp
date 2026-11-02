#include "render/rhi/device.h"
#include "support/gpu.h"
#include "support/shader_kit.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>

using namespace de::rhi;
using de::test::check_clean;
using de::test::make_device;
using de::test::read_u32;
using de::test::SampleTexturePush;
using de::test::ShaderKit;
using de::test::upload_texture;
using de::test::WriteStoragePush;

namespace {

struct Float4 {
    float r;
    float g;
    float b;
    float a;
};

Float4 read_float4(Device& device, BufferHandle buffer, size_t index) {
    Float4 value{};
    std::memcpy(&value, device.mapped(buffer).data() + (index * sizeof(Float4)), sizeof(value));
    return value;
}

bool near(float a, float b) {
    return std::fabs(a - b) <= 1.0f / 512.0f;
}

// Samples every texel center of a width x height texture through the bindless table, by index.
void sample_all(Device& device, ShaderKit& kit, de::shader::ProgramHandle program, uint32_t texture,
                DefaultSampler sampler, BufferHandle output, uint32_t width, uint32_t height) {
    device.begin_frame();
    CommandList list = device.begin_command_list();
    list.bind_pipeline(kit.pipeline(program));
    list.push_constants(SampleTexturePush{.output = device.buffer_address(output),
                                          .texture = texture,
                                          .sampler = sampler_index(sampler),
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

} // namespace

TEST_CASE("texture indices: unique, kept until destroyed, reused once the GPU is done") {
    auto device = make_device();
    if (!device) {
        return;
    }
    CHECK(device->stats().bindless_textures == 1); // the fallback
    const TextureDesc sampled{.width = 4, .height = 4, .format = Format::RGBA8Unorm, .usage = TextureUsage::Sampled};
    TextureHandle textures[3];
    uint32_t indices[3];
    for (int i = 0; i < 3; ++i) {
        textures[i] = *device->create_texture(sampled);
        indices[i] = device->texture_index(textures[i]);
        CHECK(indices[i] != kInvalidBindlessIndex);
        CHECK(indices[i] != kFallbackTextureIndex);
    }
    CHECK(indices[0] != indices[1]);
    CHECK(indices[1] != indices[2]);
    CHECK(device->stats().bindless_textures == 4);

    // Only textures with the usage get entries.
    const auto target = *device->create_texture(
        {.width = 4, .height = 4, .format = Format::RGBA8Unorm, .usage = TextureUsage::RenderTarget});
    CHECK(device->texture_index(target) == kInvalidBindlessIndex);
    CHECK(device->storage_texture_index(target) == kInvalidBindlessIndex);
    if (device->capabilities().supports(Format::RGBA8Unorm, FormatSupport::Storage)) {
        const auto storage = *device->create_texture(
            {.width = 4, .height = 4, .format = Format::RGBA8Unorm, .usage = TextureUsage::Storage});
        CHECK(device->storage_texture_index(storage) != kInvalidBindlessIndex);
        CHECK(device->texture_index(storage) == kInvalidBindlessIndex);
        CHECK(device->stats().bindless_storage_textures == 1);
        device->destroy(storage);
    }

    // A destroyed texture's entry isn't handed out again while its frame may still be running...
    device->destroy(textures[1]);
    CHECK(device->texture_index(textures[1]) == kInvalidBindlessIndex);
    const auto early = *device->create_texture(sampled);
    CHECK(device->texture_index(early) != indices[1]);
    // ...but once the GPU is done with it, it is.
    for (uint32_t i = 0; i < kFramesInFlight + 1; ++i) {
        device->begin_frame();
        device->end_frame();
    }
    const auto late = *device->create_texture(sampled);
    CHECK(device->texture_index(late) == indices[1]);

    for (const TextureHandle texture : {textures[0], textures[2], target, early, late}) {
        device->destroy(texture);
    }
    device->wait_idle();
    CHECK(device->stats().bindless_textures == 1);
    CHECK(device->stats().bindless_storage_textures == 0);
    check_clean(*device);
}

TEST_CASE("shaders sample textures by index, and a missing index draws the fallback") {
    auto device = make_device();
    if (!device) {
        return;
    }
    ShaderKit kit(*device);
    const auto program = kit.compute("tests.sample_texture");

    constexpr uint32_t kSize = 4;
    uint8_t texels[kSize * kSize * 4];
    for (uint32_t i = 0; i < kSize * kSize; ++i) {
        texels[(i * 4) + 0] = static_cast<uint8_t>(i * 16);
        texels[(i * 4) + 1] = static_cast<uint8_t>(255 - (i * 16));
        texels[(i * 4) + 2] = static_cast<uint8_t>((i % 2) * 255);
        texels[(i * 4) + 3] = 255;
    }
    const auto texture = device->create_texture({.width = kSize,
                                                 .height = kSize,
                                                 .format = Format::RGBA8Unorm,
                                                 .usage = TextureUsage::Sampled | TextureUsage::CopyDst,
                                                 .debug_name = "test pattern"});
    REQUIRE(texture.ok());
    upload_texture(*device, *texture, std::as_bytes(std::span(texels)), ResourceState::ShaderRead,
                   ShaderStages::Compute);
    const auto output = device->create_buffer({.size = uint64_t{kSize} * kSize * sizeof(Float4),
                                               .usage = BufferUsage::Storage,
                                               .memory = MemoryLocation::Readback});
    REQUIRE(output.ok());

    sample_all(*device, kit, program, device->texture_index(*texture), DefaultSampler::PointClamp, *output, kSize,
               kSize);
    int wrong = 0;
    for (uint32_t i = 0; i < kSize * kSize; ++i) {
        const Float4 texel = read_float4(*device, *output, i);
        if (!near(texel.r, static_cast<float>(texels[size_t{i} * 4]) / 255.0f) ||
            !near(texel.g, static_cast<float>(texels[(i * 4) + 1]) / 255.0f) ||
            !near(texel.b, static_cast<float>(texels[(i * 4) + 2]) / 255.0f) || !near(texel.a, 1.0f)) {
            ++wrong;
        }
    }
    CHECK(wrong == 0);

    // Linear filtering at a texel center returns the texel too (a different sampler, same index).
    sample_all(*device, kit, program, device->texture_index(*texture), DefaultSampler::LinearClamp, *output, kSize,
               kSize);
    CHECK(near(read_float4(*device, *output, 5).r, static_cast<float>(texels[size_t{5} * 4]) / 255.0f));

    // Entry 0 is the fallback: magenta.
    sample_all(*device, kit, program, kFallbackTextureIndex, DefaultSampler::PointClamp, *output, 1, 1);
    const Float4 fallback = read_float4(*device, *output, 0);
    CHECK((near(fallback.r, 1.0f) && near(fallback.g, 0.0f) && near(fallback.b, 1.0f) && near(fallback.a, 1.0f)));

    // A destroyed texture's entry points back at the fallback once the GPU is done with it.
    const uint32_t old_index = device->texture_index(*texture);
    device->destroy(*texture);
    device->wait_idle();
    sample_all(*device, kit, program, old_index, DefaultSampler::PointClamp, *output, 1, 1);
    CHECK(near(read_float4(*device, *output, 0).g, 0.0f));

    device->destroy(*output);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("shaders write storage textures by index") {
    auto device = make_device();
    if (!device) {
        return;
    }
    if (!device->capabilities().supports(Format::RGBA8Unorm, FormatSupport::Storage | FormatSupport::CopySrc)) {
        MESSAGE("skipped: rgba8_unorm can't be a storage texture on this GPU");
        return;
    }
    ShaderKit kit(*device);
    const auto program = kit.compute("tests.write_storage");
    constexpr uint32_t kSize = 16;
    const auto texture = device->create_texture({.width = kSize,
                                                 .height = kSize,
                                                 .format = Format::RGBA8Unorm,
                                                 .usage = TextureUsage::Storage | TextureUsage::CopySrc});
    const auto readback = device->create_buffer(
        {.size = uint64_t{kSize} * kSize * 4, .usage = BufferUsage::CopyDst, .memory = MemoryLocation::Readback});
    REQUIRE((texture.ok() && readback.ok()));

    device->begin_frame();
    CommandList list = device->begin_command_list();
    list.barrier(TextureBarrier{.texture = *texture,
                                .before = ResourceState::Undefined,
                                .after = ResourceState::ShaderWrite,
                                .after_stages = ShaderStages::Compute});
    list.bind_pipeline(kit.pipeline(program));
    list.push_constants(
        WriteStoragePush{.texture = device->storage_texture_index(*texture), .width = kSize, .height = kSize});
    list.dispatch(kSize / 8, kSize / 8);
    list.barrier(TextureBarrier{.texture = *texture,
                                .before = ResourceState::ShaderWrite,
                                .after = ResourceState::CopySrc,
                                .before_stages = ShaderStages::Compute});
    list.copy_texture_to_buffer(*texture, *readback);
    list.barrier(*readback, ResourceState::CopyDst, ResourceState::HostRead);
    device->end_frame();
    device->wait_idle();
    device->invalidate_mapped(*readback);

    // texel (x, y) = (x / 16, y / 16, 0.25, 1) in unorm8
    const auto texel = [&](uint32_t x, uint32_t y) { return read_u32(*device, *readback, (y * kSize) + x); };
    const auto expected = [](uint32_t x, uint32_t y) {
        const auto channel = [](float value) { return static_cast<uint32_t>(std::lround(value * 255.0f)); };
        return channel(static_cast<float>(x) / kSize) | (channel(static_cast<float>(y) / kSize) << 8U) |
               (channel(0.25f) << 16U) | (255U << 24U);
    };
    CHECK(texel(0, 0) == expected(0, 0));
    CHECK(texel(5, 9) == expected(5, 9));
    CHECK(texel(15, 15) == expected(15, 15));

    device->destroy(*texture);
    device->destroy(*readback);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("a depth-stencil texture is sampled through its depth") {
    auto device = make_device();
    if (!device) {
        return;
    }
    Format format = Format::Unknown;
    for (const Format candidate : {Format::D32FloatS8Uint, Format::D24UnormS8Uint}) {
        if (device->capabilities().supports(candidate, FormatSupport::DepthStencil | FormatSupport::Sampled)) {
            format = candidate;
            break;
        }
    }
    if (format == Format::Unknown) {
        MESSAGE("skipped: no sampled depth-stencil format on this GPU");
        return;
    }
    ShaderKit kit(*device);
    const auto program = kit.compute("tests.sample_texture");
    const auto depth = device->create_texture(
        {.width = 8, .height = 8, .format = format, .usage = TextureUsage::DepthStencil | TextureUsage::Sampled});
    const auto output = device->create_buffer(
        {.size = sizeof(Float4), .usage = BufferUsage::Storage, .memory = MemoryLocation::Readback});
    REQUIRE((depth.ok() && output.ok()));

    device->begin_frame();
    CommandList list = device->begin_command_list();
    list.barrier(*depth, ResourceState::Undefined, ResourceState::DepthWrite);
    const DepthAttachment clear{.texture = *depth, .clear_depth = 0.75f, .clear_stencil = 3};
    list.begin_rendering({.depth = &clear});
    list.end_rendering();
    list.barrier(TextureBarrier{.texture = *depth,
                                .before = ResourceState::DepthWrite,
                                .after = ResourceState::ShaderRead,
                                .after_stages = ShaderStages::Compute});
    device->end_frame();
    device->wait_idle();

    sample_all(*device, kit, program, device->texture_index(*depth), DefaultSampler::PointClamp, *output, 1, 1);
    CHECK(std::fabs(read_float4(*device, *output, 0).r - 0.75f) < 1e-3f);

    device->destroy(*depth);
    device->destroy(*output);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("the library accepts the bindless tables and refuses other descriptor slots") {
    auto device = make_device();
    if (!device) {
        return;
    }
    ShaderKit kit(*device);
    CHECK(kit.pipeline(kit.compute("tests.sample_texture")).is_valid());
    CHECK(kit.pipeline(kit.compute("tests.write_storage")).is_valid());
    // A shader with its own slot builds no pipeline (kept, without one).
    const auto slots = kit.library().create_compute({.module = "tests.own_slot"});
    REQUIRE(slots.ok());
    CHECK_FALSE(kit.pipeline(*slots).is_valid());
    check_clean(*device);
}
