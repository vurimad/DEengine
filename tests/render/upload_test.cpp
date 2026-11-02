#include "core/array.h"
#include "render/rhi/device.h"
#include "render/rhi/upload.h"
#include "support/gpu.h"
#include "support/shader_kit.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>

using namespace de::rhi;
using de::test::check_clean;
using de::test::make_device;
using de::test::SampleTexturePush;
using de::test::ShaderKit;

namespace {

de::core::Allocator& test_heap() {
    return de::core::heap_allocator(de::core::MemoryTag::Render);
}

// Bytes that differ per position and per seed, so a misplaced or stale copy shows.
de::core::Array<std::byte> pattern(size_t size, uint32_t seed) {
    de::core::Array<std::byte> bytes(test_heap());
    bytes.resize(size);
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::byte>(((i * 31) + (size_t{seed} * 101) + (i >> 8U)) & 0xFFU);
    }
    return bytes;
}

std::span<const std::byte> as_span(const de::core::Array<std::byte>& bytes) {
    return bytes.span();
}

// Every mip and layer of a texture in CopySrc state, read back in texture_upload_size() order.
de::core::Array<std::byte> read_texture(Device& device, TextureHandle texture) {
    const TextureDesc desc = *device.texture_desc(texture);
    const uint64_t size = texture_upload_size(desc, {});
    const auto readback =
        device.create_buffer({.size = size, .usage = BufferUsage::CopyDst, .memory = MemoryLocation::Readback});
    REQUIRE(readback.ok());
    device.begin_frame();
    CommandList list = device.begin_command_list();
    uint64_t offset = 0;
    for (uint32_t mip = 0; mip < desc.mip_count; ++mip) {
        const uint64_t layer_bytes = texture_upload_size(desc, {.first_mip = mip, .mip_count = 1}) / desc.layer_count;
        for (uint32_t layer = 0; layer < desc.layer_count; ++layer) {
            list.copy_texture_to_buffer(texture, *readback, {.buffer_offset = offset, .mip = mip, .layer = layer});
            offset += layer_bytes;
        }
    }
    list.barrier(
        BufferBarrier{.buffer = *readback, .before = ResourceState::CopyDst, .after = ResourceState::HostRead});
    device.end_frame();
    device.wait_idle();
    device.invalidate_mapped(*readback);
    de::core::Array<std::byte> bytes(test_heap());
    bytes.resize(static_cast<size_t>(size));
    std::memcpy(bytes.data(), device.mapped(*readback).data(), static_cast<size_t>(size));
    device.destroy(*readback);
    return bytes;
}

// A whole buffer in CopySrc state, read back.
de::core::Array<std::byte> read_buffer(Device& device, BufferHandle buffer) {
    const uint64_t size = device.buffer_desc(buffer)->size;
    const auto readback =
        device.create_buffer({.size = size, .usage = BufferUsage::CopyDst, .memory = MemoryLocation::Readback});
    REQUIRE(readback.ok());
    device.begin_frame();
    CommandList list = device.begin_command_list();
    list.copy_buffer(buffer, 0, *readback, 0, size);
    list.barrier(
        BufferBarrier{.buffer = *readback, .before = ResourceState::CopyDst, .after = ResourceState::HostRead});
    device.end_frame();
    device.wait_idle();
    device.invalidate_mapped(*readback);
    de::core::Array<std::byte> bytes(test_heap());
    bytes.resize(static_cast<size_t>(size));
    std::memcpy(bytes.data(), device.mapped(*readback).data(), static_cast<size_t>(size));
    device.destroy(*readback);
    return bytes;
}

bool same_bytes(std::span<const std::byte> a, std::span<const std::byte> b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

// One frame that records the uploader's next batch.
void record_frame(Device& device, Uploader& uploader) {
    device.begin_frame();
    CommandList list = device.begin_command_list();
    uploader.record(list);
    device.end_frame();
}

constexpr TextureUsage kCopyBoth = TextureUsage::CopyDst | TextureUsage::CopySrc;
constexpr BufferUsage kBufferCopyBoth = BufferUsage::CopyDst | BufferUsage::CopySrc;

} // namespace

TEST_CASE("texture_upload_size: mips, layers, slices and whole blocks") {
    const TextureDesc rgba{.width = 4, .height = 4, .mip_count = 3, .format = Format::RGBA8Unorm};
    CHECK(texture_upload_size(rgba, {}) == 64 + 16 + 4);
    CHECK(texture_upload_size(rgba, {.first_mip = 1}) == 16 + 4);
    CHECK(texture_upload_size(rgba, {.first_mip = 2, .mip_count = 1}) == 4);
    CHECK(texture_upload_size(rgba, {.first_mip = 3}) == 0);                 // past the last mip
    CHECK(texture_upload_size(rgba, {.first_mip = 1, .mip_count = 3}) == 0); // runs past it
    CHECK(texture_upload_size(rgba, {.first_mip = 0, .mip_count = 0}) == 0); // nothing

    const TextureDesc cube{.width = 8,
                           .height = 8,
                           .mip_count = 2,
                           .layer_count = 6,
                           .format = Format::RGBA8Unorm,
                           .dimension = TextureDimension::Cube};
    CHECK(texture_upload_size(cube, {}) == 6 * (256 + 64));

    // BC1: 8 bytes per 4x4 block, and a mip smaller than a block still takes a whole one.
    const TextureDesc bc1{.width = 8, .height = 8, .mip_count = 4, .format = Format::BC1Unorm};
    CHECK(texture_upload_size(bc1, {}) == 32 + 8 + 8 + 8);
    const TextureDesc bc7{.width = 12, .height = 4, .format = Format::BC7Unorm};
    CHECK(texture_upload_size(bc7, {}) == 3 * 16);

    const TextureDesc volume{.width = 4,
                             .height = 4,
                             .depth = 4,
                             .mip_count = 3,
                             .format = Format::R8Unorm,
                             .dimension = TextureDimension::Texture3D};
    CHECK(texture_upload_size(volume, {}) == 64 + 8 + 1);
}

TEST_CASE("uploads reject bad requests and queue nothing") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto uploader = Uploader::create(*device);
    REQUIRE(uploader.ok());
    CHECK(Uploader::create(*device, {.staging_bytes = 16}).error().code == de::core::ErrorCode::InvalidArgument);

    const auto texture = *device->create_texture({.width = 4,
                                                  .height = 4,
                                                  .mip_count = 3,
                                                  .format = Format::RGBA8Unorm,
                                                  .usage = TextureUsage::CopyDst | TextureUsage::Sampled});
    const auto sampled_only = *device->create_texture(
        {.width = 4, .height = 4, .format = Format::RGBA8Unorm, .usage = TextureUsage::Sampled});
    const auto buffer = *device->create_buffer({.size = 256, .usage = BufferUsage::CopyDst | BufferUsage::Storage});
    const auto storage_only = *device->create_buffer({.size = 256, .usage = BufferUsage::Storage});
    const auto bytes = pattern(1024, 1);
    const std::span<const std::byte> data = as_span(bytes);

    // Textures: exact size only.
    CHECK(uploader->upload_texture(texture, data.first(84 - 1)).error().code == de::core::ErrorCode::InvalidArgument);
    CHECK(uploader->upload_texture(texture, data.first(84 + 4)).error().code == de::core::ErrorCode::InvalidArgument);
    CHECK(uploader->upload_texture(texture, data.first(64)).error().code == de::core::ErrorCode::InvalidArgument);
    CHECK(uploader->upload_texture(texture, data.first(64), {.first_mip = 3}).error().code ==
          de::core::ErrorCode::OutOfRange);
    CHECK(uploader->upload_texture(texture, data.first(64), {.mip_count = 4}).error().code ==
          de::core::ErrorCode::OutOfRange);
    CHECK(uploader->upload_texture(sampled_only, data.first(64)).error().code == de::core::ErrorCode::InvalidArgument);
    CHECK(uploader->upload_texture(texture, data.first(84), {.after = ResourceState::Undefined}).error().code ==
          de::core::ErrorCode::InvalidArgument);
    CHECK(uploader
              ->upload_texture(texture, data.first(84),
                               {.after = ResourceState::RenderTarget | ResourceState::ShaderRead})
              .error()
              .code == de::core::ErrorCode::InvalidArgument);

    // Buffers: the range must fit.
    CHECK(uploader->upload_buffer(buffer, 0, data.first(257), ResourceState::ShaderRead).error().code ==
          de::core::ErrorCode::OutOfRange);
    CHECK(uploader->upload_buffer(buffer, 200, data.first(57), ResourceState::ShaderRead).error().code ==
          de::core::ErrorCode::OutOfRange);
    CHECK(uploader->upload_buffer(buffer, 300, data.first(1), ResourceState::ShaderRead).error().code ==
          de::core::ErrorCode::OutOfRange);
    CHECK(uploader->upload_buffer(buffer, 0, {}, ResourceState::ShaderRead).error().code ==
          de::core::ErrorCode::OutOfRange);
    CHECK(uploader->upload_buffer(storage_only, 0, data.first(16), ResourceState::ShaderRead).error().code ==
          de::core::ErrorCode::InvalidArgument);

    // Stale handles.
    device->destroy(texture);
    device->destroy(buffer);
    CHECK(uploader->upload_texture(texture, data.first(84)).error().code == de::core::ErrorCode::InvalidArgument);
    CHECK(uploader->upload_buffer(buffer, 0, data.first(16), ResourceState::ShaderRead).error().code ==
          de::core::ErrorCode::InvalidArgument);

    CHECK(uploader->idle());
    CHECK(uploader->stats().queued_uploads == 0);
    CHECK(uploader->stats().staging_bytes_in_use == 0);
    device->destroy(sampled_only);
    device->destroy(storage_only);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("every mip and layer arrives where it belongs") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto uploader = Uploader::create(*device);
    REQUIRE(uploader.ok());
    const TextureDesc descs[] = {
        {.width = 16, .height = 8, .mip_count = 5, .format = Format::RGBA8Unorm, .usage = kCopyBoth},
        {.width = 8, .height = 8, .mip_count = 4, .layer_count = 3, .format = Format::RGBA8Unorm, .usage = kCopyBoth},
        {.width = 8,
         .height = 8,
         .mip_count = 2,
         .layer_count = 6,
         .format = Format::RGBA8Unorm,
         .usage = kCopyBoth | TextureUsage::Sampled,
         .dimension = TextureDimension::Cube},
        {.width = 8,
         .height = 4,
         .depth = 4,
         .mip_count = 3,
         .format = Format::RGBA8Unorm,
         .usage = kCopyBoth,
         .dimension = TextureDimension::Texture3D},
        {.width = 6, .height = 5, .mip_count = 3, .format = Format::R16Float, .usage = kCopyBoth},
    };
    uint32_t seed = 0;
    for (const TextureDesc& desc : descs) {
        CAPTURE(seed);
        const auto texture = *device->create_texture(desc);
        const auto texels = pattern(static_cast<size_t>(texture_upload_size(desc, {})), ++seed);
        REQUIRE(uploader->upload_texture(texture, as_span(texels), {.after = ResourceState::CopySrc}).ok());
        CHECK_FALSE(uploader->idle());
        uploader->flush_and_wait();
        CHECK(uploader->idle());
        CHECK(same_bytes(as_span(read_texture(*device, texture)), as_span(texels)));
        device->destroy(texture);
    }
    const UploaderStats stats = uploader->stats();
    CHECK(stats.queued_uploads == 0);
    CHECK(stats.queued_bytes == 0);
    CHECK(stats.dedicated_staging == 0);
    CHECK(stats.uploaded_bytes > 0);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("mips stream in separately, and again, from the state they were left in") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto uploader = Uploader::create(*device);
    REQUIRE(uploader.ok());
    const TextureDesc desc{.width = 16,
                           .height = 16,
                           .mip_count = 5,
                           .format = Format::RGBA8Unorm,
                           .usage = kCopyBoth | TextureUsage::Sampled};
    const auto texture = *device->create_texture(desc);
    const auto texels = pattern(static_cast<size_t>(texture_upload_size(desc, {})), 7);
    const std::span<const std::byte> all = as_span(texels);
    const size_t mip0 = size_t{16} * 16 * 4;

    // The small mips first (as streaming does), left for fragment shaders; then the top one.
    REQUIRE(
        uploader->upload_texture(texture, all.subspan(mip0), {.first_mip = 1, .after_stages = ShaderStages::Fragment})
            .ok());
    uploader->flush_and_wait();
    REQUIRE(uploader->upload_texture(texture, all.first(mip0), {.mip_count = 1, .after = ResourceState::CopySrc}).ok());
    uploader->flush_and_wait();
    // The small mips again, from ShaderRead now: everything ends in CopySrc to be read back.
    REQUIRE(
        uploader->upload_texture(texture, all.subspan(mip0), {.first_mip = 1, .after = ResourceState::CopySrc}).ok());
    uploader->flush_and_wait();
    CHECK(same_bytes(as_span(read_texture(*device, texture)), all));

    // A whole re-upload, over mips in CopySrc, with new contents.
    const auto newer = pattern(all.size(), 8);
    REQUIRE(uploader->upload_texture(texture, as_span(newer), {.after = ResourceState::CopySrc}).ok());
    uploader->flush_and_wait();
    CHECK(same_bytes(as_span(read_texture(*device, texture)), as_span(newer)));

    device->destroy(texture);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("block-compressed uploads sample as their colors") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto uploader = Uploader::create(*device);
    REQUIRE(uploader.ok());
    ShaderKit kit(*device);
    const auto program = kit.compute("tests.sample_texture");
    const auto output = *device->create_buffer(
        {.size = uint64_t{16} * 16, .usage = BufferUsage::Storage, .memory = MemoryLocation::Readback});

    // 8x8 with 4 mips (the last two smaller than a block). BC1: both endpoints pure red (565),
    // every index 0. BC4: both endpoints 255, every index 0.
    const auto bc1 = *device->create_texture({.width = 8,
                                              .height = 8,
                                              .mip_count = 4,
                                              .format = Format::BC1Unorm,
                                              .usage = TextureUsage::Sampled | TextureUsage::CopyDst});
    const auto bc4 = *device->create_texture({.width = 8,
                                              .height = 8,
                                              .mip_count = 4,
                                              .format = Format::BC4Unorm,
                                              .usage = TextureUsage::Sampled | TextureUsage::CopyDst});
    constexpr std::byte kRedBlock[8] = {std::byte{0x00}, std::byte{0xF8}, std::byte{0x00}, std::byte{0xF8}};
    constexpr std::byte kWhiteBlock[8] = {std::byte{0xFF}, std::byte{0xFF}};
    de::core::Array<std::byte> bc1_data(test_heap());
    de::core::Array<std::byte> bc4_data(test_heap());
    for (int block = 0; block < 4 + 1 + 1 + 1; ++block) {
        for (int i = 0; i < 8; ++i) {
            bc1_data.push(kRedBlock[i]);
            bc4_data.push(kWhiteBlock[i]);
        }
    }
    REQUIRE(uploader->upload_texture(bc1, as_span(bc1_data), {.after_stages = ShaderStages::Compute}).ok());
    REQUIRE(uploader->upload_texture(bc4, as_span(bc4_data), {.after_stages = ShaderStages::Compute}).ok());
    uploader->flush_and_wait();

    const auto sample = [&](TextureHandle texture) {
        device->begin_frame();
        CommandList list = device->begin_command_list();
        list.bind_pipeline(kit.pipeline(program));
        list.push_constants(SampleTexturePush{.output = device->buffer_address(output),
                                              .texture = device->texture_index(texture),
                                              .sampler = sampler_index(DefaultSampler::PointClamp),
                                              .width = 4,
                                              .height = 4});
        list.dispatch(1, 1);
        list.barrier(BufferBarrier{.buffer = output,
                                   .before = ResourceState::ShaderWrite,
                                   .after = ResourceState::HostRead,
                                   .before_stages = ShaderStages::Compute});
        device->end_frame();
        device->wait_idle();
        device->invalidate_mapped(output);
    };
    const auto all_texels = [&](float r, float g, float b, float a) {
        int wrong = 0;
        for (size_t i = 0; i < 16; ++i) {
            float texel[4];
            std::memcpy(texel, device->mapped(output).data() + (i * sizeof(texel)), sizeof(texel));
            const float expected[4] = {r, g, b, a};
            for (int c = 0; c < 4; ++c) {
                wrong += std::fabs(texel[c] - expected[c]) > 1.0f / 256.0f ? 1 : 0;
            }
        }
        return wrong == 0;
    };
    sample(bc1);
    CHECK(all_texels(1.0f, 0.0f, 0.0f, 1.0f));
    sample(bc4);
    CHECK(all_texels(1.0f, 0.0f, 0.0f, 1.0f));

    device->destroy(bc1);
    device->destroy(bc4);
    device->destroy(output);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("the frame budget spreads uploads over frames, in order") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto uploader = Uploader::create(*device, {.frame_budget_bytes = 2048});
    REQUIRE(uploader.ok());
    // Four 1 KiB textures: two per frame.
    const TextureDesc desc{.width = 16, .height = 16, .format = Format::RGBA8Unorm, .usage = kCopyBoth};
    TextureHandle textures[4];
    de::core::Array<std::byte> texels[4] = {
        de::core::Array<std::byte>(test_heap()), de::core::Array<std::byte>(test_heap()),
        de::core::Array<std::byte>(test_heap()), de::core::Array<std::byte>(test_heap())};
    for (uint32_t i = 0; i < 4; ++i) {
        textures[i] = *device->create_texture(desc);
        texels[i] = pattern(1024, 20 + i);
        REQUIRE(uploader->upload_texture(textures[i], as_span(texels[i]), {.after = ResourceState::CopySrc}).ok());
    }
    CHECK(uploader->stats().queued_uploads == 4);
    CHECK(uploader->stats().queued_bytes == 4096);
    record_frame(*device, *uploader);
    CHECK(uploader->stats().queued_uploads == 2);
    record_frame(*device, *uploader);
    CHECK(uploader->idle());

    // One upload over the budget still goes, alone.
    const TextureDesc big{.width = 32, .height = 32, .format = Format::RGBA8Unorm, .usage = kCopyBoth};
    const auto big_texture = *device->create_texture(big);
    const auto big_texels = pattern(4096, 30);
    REQUIRE(uploader->upload_texture(big_texture, as_span(big_texels), {.after = ResourceState::CopySrc}).ok());
    REQUIRE(uploader->upload_texture(textures[0], as_span(texels[1]), {.after = ResourceState::CopySrc}).ok());
    record_frame(*device, *uploader);
    CHECK(uploader->stats().queued_uploads == 1);
    record_frame(*device, *uploader);
    CHECK(uploader->idle());
    device->wait_idle();

    CHECK(same_bytes(as_span(read_texture(*device, textures[0])), as_span(texels[1]))); // the later upload won
    for (uint32_t i = 1; i < 4; ++i) {
        CHECK(same_bytes(as_span(read_texture(*device, textures[i])), as_span(texels[i])));
    }
    CHECK(same_bytes(as_span(read_texture(*device, big_texture)), as_span(big_texels)));
    for (const TextureHandle texture : textures) {
        device->destroy(texture);
    }
    device->destroy(big_texture);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("uploads to one resource are recorded in order, a batch apart") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto uploader = Uploader::create(*device);
    REQUIRE(uploader.ok());
    const auto buffer = *device->create_buffer({.size = 64, .usage = kBufferCopyBoth});
    const auto first = pattern(64, 1);
    const auto second = pattern(32, 2);
    REQUIRE(uploader->upload_buffer(buffer, 0, as_span(first), ResourceState::CopySrc).ok());
    REQUIRE(uploader->upload_buffer(buffer, 16, as_span(second), ResourceState::CopySrc).ok());
    record_frame(*device, *uploader);
    CHECK(uploader->stats().queued_uploads == 1); // the second waits for the next batch
    uploader->flush_and_wait();

    const auto bytes = read_buffer(*device, buffer);
    CHECK(same_bytes(as_span(bytes).first(16), as_span(first).first(16)));
    CHECK(same_bytes(as_span(bytes).subspan(16, 32), as_span(second)));
    CHECK(same_bytes(as_span(bytes).subspan(48), as_span(first).subspan(48)));
    device->destroy(buffer);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("the staging ring wraps and reuses memory the GPU is done with") {
    auto device = make_device();
    if (!device) {
        return;
    }
    // Room for four uploads of 1000 bytes; forty go through it, one per frame.
    auto uploader = Uploader::create(*device, {.staging_bytes = 4096, .frame_budget_bytes = 4096});
    REQUIRE(uploader.ok());
    constexpr uint32_t kUploads = 40;
    constexpr size_t kChunk = 1000;
    const auto buffer = *device->create_buffer({.size = kUploads * kChunk, .usage = kBufferCopyBoth});
    const auto expected = pattern(kUploads * kChunk, 3);
    uint64_t most_in_use = 0;
    for (uint32_t i = 0; i < kUploads; ++i) {
        REQUIRE(uploader
                    ->upload_buffer(buffer, i * kChunk, as_span(expected).subspan(i * kChunk, kChunk),
                                    ResourceState::CopySrc)
                    .ok());
        most_in_use = std::max(most_in_use, uploader->stats().staging_bytes_in_use);
        record_frame(*device, *uploader);
    }
    device->wait_idle();
    const UploaderStats stats = uploader->stats();
    CHECK(stats.dedicated_staging == 0); // never out of ring
    CHECK(most_in_use <= 4096);
    CHECK(stats.uploaded_bytes == kUploads * kChunk);
    CHECK(same_bytes(as_span(read_buffer(*device, buffer)), as_span(expected)));
    device->destroy(buffer);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("an upload too big for the ring, or one that finds it full, gets staging of its own") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto uploader = Uploader::create(*device, {.staging_bytes = 4096});
    REQUIRE(uploader.ok());
    const uint32_t buffers_before = device->stats().live_buffers;
    const auto buffer = *device->create_buffer({.size = 16384, .usage = kBufferCopyBoth});
    const auto expected = pattern(16384, 4);
    // Bigger than the ring.
    REQUIRE(uploader->upload_buffer(buffer, 0, as_span(expected).first(8192), ResourceState::CopySrc).ok());
    CHECK(uploader->stats().dedicated_staging == 1);
    // Four of 2 KiB: two fill the ring, two find it full (nothing has been recorded yet).
    for (uint32_t i = 0; i < 4; ++i) {
        REQUIRE(uploader
                    ->upload_buffer(buffer, 8192 + (i * 2048), as_span(expected).subspan(8192 + (i * 2048), 2048),
                                    ResourceState::CopySrc)
                    .ok());
    }
    CHECK(uploader->stats().dedicated_staging == 3);
    uploader->flush_and_wait();
    CHECK(same_bytes(as_span(read_buffer(*device, buffer)), as_span(expected)));
    device->destroy(buffer);
    device->wait_idle();
    CHECK(device->stats().live_buffers == buffers_before); // the dedicated staging is gone
    check_clean(*device);
}

TEST_CASE("uploads queued but never recorded don't leak") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const uint32_t buffers_before = device->stats().live_buffers;
    const auto buffer = *device->create_buffer({.size = 8192, .usage = kBufferCopyBoth});
    {
        auto uploader = Uploader::create(*device, {.staging_bytes = 4096});
        REQUIRE(uploader.ok());
        const auto bytes = pattern(8192, 5);
        REQUIRE(uploader->upload_buffer(buffer, 0, as_span(bytes), ResourceState::CopySrc).ok()); // dedicated
        REQUIRE(uploader->upload_buffer(buffer, 0, as_span(bytes).first(1024), ResourceState::CopySrc).ok());
        auto moved = std::move(*uploader);
        CHECK(moved.stats().queued_uploads == 2);
    }
    device->destroy(buffer);
    device->wait_idle();
    CHECK(device->stats().live_buffers == buffers_before);
    check_clean(*device);
}

TEST_CASE("a buffer's ranges left in different read states are all waited for") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto uploader = Uploader::create(*device);
    REQUIRE(uploader.ok());
    const auto buffer = *device->create_buffer({.size = 64, .usage = kBufferCopyBoth | BufferUsage::Storage});
    const auto readback =
        *device->create_buffer({.size = 64, .usage = BufferUsage::CopyDst, .memory = MemoryLocation::Readback});
    const auto bytes = pattern(64, 6);
    REQUIRE(uploader->upload_buffer(buffer, 0, as_span(bytes).first(32), ResourceState::CopySrc).ok());
    REQUIRE(
        uploader
            ->upload_buffer(buffer, 32, as_span(bytes).subspan(32), ResourceState::ShaderRead, ShaderStages::Compute)
            .ok());
    uploader->flush_and_wait();

    // A copy reads the first range, then an upload overwrites it in the same frame: its barrier must
    // wait for the copy, though the buffer's last upload left a compute read state.
    const auto newer = pattern(32, 7);
    REQUIRE(uploader->upload_buffer(buffer, 0, as_span(newer), ResourceState::CopySrc).ok());
    device->begin_frame();
    CommandList list = device->begin_command_list();
    list.copy_buffer(buffer, 0, readback, 0, 32);
    uploader->record(list);
    list.barrier(BufferBarrier{.buffer = readback, .before = ResourceState::CopyDst, .after = ResourceState::HostRead});
    device->end_frame();
    device->wait_idle();
    device->invalidate_mapped(readback);
    CHECK(std::memcmp(device->mapped(readback).data(), bytes.data(), 32) == 0); // read before the overwrite

    device->destroy(buffer);
    device->destroy(readback);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("uploads to resources destroyed before they are recorded are dropped") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto uploader = Uploader::create(*device, {.staging_bytes = 4096});
    REQUIRE(uploader.ok());
    const uint32_t buffers_before = device->stats().live_buffers;
    const TextureDesc desc{.width = 16, .height = 16, .format = Format::RGBA8Unorm, .usage = kCopyBoth};
    const auto doomed_texture = *device->create_texture(desc);
    const auto doomed_buffer = *device->create_buffer({.size = 8192, .usage = kBufferCopyBoth});
    const auto kept = *device->create_texture(desc);
    const auto texels = pattern(1024, 9);
    const auto big = pattern(8192, 10);
    REQUIRE(uploader->upload_texture(doomed_texture, as_span(texels)).ok());
    REQUIRE(uploader->upload_buffer(doomed_buffer, 0, as_span(big), ResourceState::ShaderRead).ok()); // dedicated
    REQUIRE(uploader->upload_texture(kept, as_span(texels), {.after = ResourceState::CopySrc}).ok());
    device->destroy(doomed_texture);
    device->destroy(doomed_buffer);
    uploader->flush_and_wait();

    const UploaderStats stats = uploader->stats();
    CHECK(stats.dropped_uploads == 2);
    CHECK(stats.uploaded_bytes == 1024);
    CHECK(stats.queued_bytes == 0);
    CHECK(same_bytes(as_span(read_texture(*device, kept)), as_span(texels)));
    device->destroy(kept);
    device->wait_idle();
    CHECK(device->stats().live_buffers == buffers_before);
    check_clean(*device);
}

TEST_CASE("long streams of uploads keep their order and bookkeeping") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto uploader = Uploader::create(*device);
    REQUIRE(uploader.ok());
    // Rounds of 100 small uploads to one buffer, recorded one per frame (each waits for the last):
    // the queue and the ring's region list fill up, drain and compact many times over.
    constexpr uint32_t kRounds = 3;
    constexpr uint32_t kPerRound = 100;
    constexpr size_t kChunk = 16;
    constexpr size_t kTotal = size_t{kRounds} * kPerRound * kChunk;
    const auto buffer = *device->create_buffer({.size = kTotal, .usage = kBufferCopyBoth});
    const auto expected = pattern(kTotal, 11);
    for (uint32_t round = 0; round < kRounds; ++round) {
        for (uint32_t i = 0; i < kPerRound; ++i) {
            const size_t offset = ((size_t{round} * kPerRound) + i) * kChunk;
            REQUIRE(
                uploader
                    ->upload_buffer(buffer, offset, as_span(expected).subspan(offset, kChunk), ResourceState::CopySrc)
                    .ok());
        }
        for (uint32_t i = 0; i < kPerRound / 2; ++i) {
            record_frame(*device, *uploader);
        }
        CHECK(uploader->stats().queued_uploads == (round + 1) * (kPerRound / 2)); // half of each round left
    }
    uploader->flush_and_wait();
    CHECK(uploader->stats().uploaded_bytes == kTotal);
    CHECK(uploader->stats().dedicated_staging == 0);
    CHECK(same_bytes(as_span(read_buffer(*device, buffer)), as_span(expected)));
    device->destroy(buffer);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("a first upload of some mips leaves every mip usable") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto uploader = Uploader::create(*device);
    REQUIRE(uploader.ok());
    const TextureDesc desc{.width = 16, .height = 16, .mip_count = 5, .format = Format::RGBA8Unorm, .usage = kCopyBoth};
    const auto texture = *device->create_texture(desc);
    const auto texels = pattern(static_cast<size_t>(texture_upload_size(desc, {})), 12);
    const size_t mip0 = size_t{16} * 16 * 4;
    // Only the small mips; mip 0 must still leave Undefined, or reading it back is a layout error.
    REQUIRE(
        uploader
            ->upload_texture(texture, as_span(texels).subspan(mip0), {.first_mip = 1, .after = ResourceState::CopySrc})
            .ok());
    uploader->flush_and_wait();
    const auto bytes = read_texture(*device, texture);
    CHECK(same_bytes(as_span(bytes).subspan(mip0), as_span(texels).subspan(mip0)));
    device->destroy(texture);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("an upload bigger than the budget isn't held back by a dropped one ahead of it") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto uploader = Uploader::create(*device, {.frame_budget_bytes = 1024});
    REQUIRE(uploader.ok());
    const auto doomed = *device->create_buffer({.size = 256, .usage = kBufferCopyBoth});
    const auto buffer = *device->create_buffer({.size = 4096, .usage = kBufferCopyBoth});
    const auto bytes = pattern(4096, 13);
    REQUIRE(uploader->upload_buffer(doomed, 0, as_span(bytes).first(256), ResourceState::CopySrc).ok());
    REQUIRE(uploader->upload_buffer(buffer, 0, as_span(bytes), ResourceState::CopySrc).ok());
    device->destroy(doomed);
    record_frame(*device, *uploader);
    CHECK(uploader->idle());
    device->wait_idle();
    CHECK(same_bytes(as_span(read_buffer(*device, buffer)), as_span(bytes)));
    device->destroy(buffer);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("a buffer range left written doesn't hide readers of another range") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto uploader = Uploader::create(*device);
    REQUIRE(uploader.ok());
    const auto buffer = *device->create_buffer({.size = 64, .usage = kBufferCopyBoth | BufferUsage::Storage});
    const auto readback =
        *device->create_buffer({.size = 64, .usage = BufferUsage::CopyDst, .memory = MemoryLocation::Readback});
    const auto bytes = pattern(64, 14);
    REQUIRE(uploader->upload_buffer(buffer, 0, as_span(bytes).first(32), ResourceState::CopySrc).ok());
    REQUIRE(
        uploader
            ->upload_buffer(buffer, 32, as_span(bytes).subspan(32), ResourceState::ShaderWrite, ShaderStages::Compute)
            .ok());
    uploader->flush_and_wait();

    // The first range is copied from, then overwritten in the same frame: the last upload left a
    // write state, but the copy's read must still be waited for.
    const auto newer = pattern(32, 15);
    REQUIRE(uploader->upload_buffer(buffer, 0, as_span(newer), ResourceState::CopySrc).ok());
    device->begin_frame();
    CommandList list = device->begin_command_list();
    list.copy_buffer(buffer, 0, readback, 0, 32);
    uploader->record(list);
    list.barrier(BufferBarrier{.buffer = readback, .before = ResourceState::CopyDst, .after = ResourceState::HostRead});
    device->end_frame();
    device->wait_idle();
    device->invalidate_mapped(readback);
    CHECK(std::memcmp(device->mapped(readback).data(), bytes.data(), 32) == 0);

    device->destroy(buffer);
    device->destroy(readback);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("big batches split their barriers, and shader states need stages") {
    auto device = make_device();
    if (!device) {
        return;
    }
    auto uploader = Uploader::create(*device);
    REQUIRE(uploader.ok());
    // Forty textures in one batch: more first-upload barriers than one command holds inline.
    const TextureDesc desc{.width = 4, .height = 4, .mip_count = 3, .format = Format::RGBA8Unorm, .usage = kCopyBoth};
    constexpr uint32_t kCount = 40;
    TextureHandle textures[kCount];
    const auto texels = pattern(static_cast<size_t>(texture_upload_size(desc, {})), 16);
    for (TextureHandle& texture : textures) {
        texture = *device->create_texture(desc);
        REQUIRE(uploader->upload_texture(texture, as_span(texels), {.after = ResourceState::CopySrc}).ok());
    }
    record_frame(*device, *uploader);
    CHECK(uploader->idle());
    device->wait_idle();
    for (const TextureHandle texture : textures) {
        CHECK(same_bytes(as_span(read_texture(*device, texture)), as_span(texels)));
        device->destroy(texture);
    }

    const auto buffer = *device->create_buffer({.size = 64, .usage = kBufferCopyBoth});
    CHECK(uploader->upload_buffer(buffer, 0, as_span(texels).first(16), ResourceState::ShaderRead, ShaderStages::None)
              .error()
              .code == de::core::ErrorCode::InvalidArgument);
    device->destroy(buffer);
    device->wait_idle();
    check_clean(*device);
}
