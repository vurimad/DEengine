#include "render/rhi/device.h"
#include "render/rhi/upload.h"
#include "support/gpu.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>

using namespace de::rhi;
using de::test::check_clean;
using de::test::make_device;

namespace {

uint64_t category_bytes(const Device& device, GpuMemoryCategory category) {
    return device.memory_stats().category_bytes(category);
}

uint32_t category_count(const Device& device, GpuMemoryCategory category) {
    return device.memory_stats().category_count(category);
}

} // namespace

TEST_CASE("GPU memory is counted per category as resources come and go") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const GpuMemoryStats before = device->memory_stats();

    // One of each category, with their raw sizes as lower bounds of what they take.
    const TextureDesc sampled_desc{.width = 256,
                                   .height = 256,
                                   .mip_count = 9,
                                   .format = Format::RGBA8Unorm,
                                   .usage = TextureUsage::Sampled | TextureUsage::CopyDst};
    const auto sampled = *device->create_texture(sampled_desc);
    const auto target = *device->create_texture({.width = 128,
                                                 .height = 128,
                                                 .format = Format::RGBA16Float,
                                                 .usage = TextureUsage::RenderTarget | TextureUsage::Storage});
    const auto storage = *device->create_texture(
        {.width = 64, .height = 64, .format = Format::R32Float, .usage = TextureUsage::Storage});
    const auto gpu_buffer = *device->create_buffer({.size = 100000, .usage = BufferUsage::Storage});
    const auto upload =
        *device->create_buffer({.size = 5000, .usage = BufferUsage::CopySrc, .memory = MemoryLocation::Upload});
    const auto readback =
        *device->create_buffer({.size = 7000, .usage = BufferUsage::CopyDst, .memory = MemoryLocation::Readback});

    const GpuMemoryStats stats = device->memory_stats();
    const auto grew = [&](GpuMemoryCategory category) {
        return stats.category_bytes(category) - before.category_bytes(category);
    };
    const auto added = [&](GpuMemoryCategory category) {
        return stats.category_count(category) - before.category_count(category);
    };
    CHECK(grew(GpuMemoryCategory::Textures) >= texture_upload_size(sampled_desc, {}));
    CHECK(added(GpuMemoryCategory::Textures) == 1);
    CHECK(grew(GpuMemoryCategory::RenderTargets) >= uint64_t{128} * 128 * 8); // render target wins over storage
    CHECK(added(GpuMemoryCategory::RenderTargets) == 1);
    CHECK(grew(GpuMemoryCategory::StorageTextures) >= uint64_t{64} * 64 * 4);
    CHECK(added(GpuMemoryCategory::StorageTextures) == 1);
    CHECK(grew(GpuMemoryCategory::GpuBuffers) >= 100000);
    CHECK(grew(GpuMemoryCategory::UploadBuffers) >= 5000);
    CHECK(grew(GpuMemoryCategory::ReadbackBuffers) >= 7000);
    CHECK(stats.total_bytes() > before.total_bytes());
    CHECK(stats.device_local_budget_bytes > 0);
    CHECK(stats.device_local_used_bytes > 0);

    // Destroyed memory waits for the GPU, then it's gone.
    const uint64_t sampled_bytes = grew(GpuMemoryCategory::Textures);
    device->destroy(sampled);
    CHECK(category_bytes(*device, GpuMemoryCategory::Textures) == before.category_bytes(GpuMemoryCategory::Textures));
    CHECK(category_count(*device, GpuMemoryCategory::Textures) == before.category_count(GpuMemoryCategory::Textures));
    CHECK(device->memory_stats().pending_destroy_bytes == before.pending_destroy_bytes + sampled_bytes);
    for (const TextureHandle texture : {target, storage}) {
        device->destroy(texture);
    }
    for (const BufferHandle buffer : {gpu_buffer, upload, readback}) {
        device->destroy(buffer);
    }
    device->wait_idle();
    const GpuMemoryStats after = device->memory_stats();
    CHECK(after.pending_destroy_bytes == 0);
    for (size_t i = 0; i < static_cast<size_t>(GpuMemoryCategory::Count); ++i) {
        CHECK(after.bytes[i] == before.bytes[i]);
        CHECK(after.counts[i] == before.counts[i]);
    }
    check_clean(*device);
}

TEST_CASE("the largest textures come biggest first, with their names") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const uint32_t sizes[] = {64, 512, 128, 1024, 256};
    const char* names[] = {"tiny", "big", "small", "huge", "medium"};
    TextureHandle textures[5];
    for (size_t i = 0; i < 5; ++i) {
        textures[i] = *device->create_texture({.width = sizes[i],
                                               .height = sizes[i],
                                               .format = Format::RGBA8Unorm,
                                               .usage = TextureUsage::Sampled,
                                               .debug_name = names[i]});
    }
    TextureMemory top[3];
    REQUIRE(device->largest_textures(top) == 3);
    CHECK(top[0].texture == textures[3]);
    CHECK(std::strcmp(top[0].name.c_str(), "huge") == 0);
    CHECK(top[0].desc.width == 1024);
    CHECK(top[0].desc.debug_name == nullptr);
    CHECK(top[0].bytes >= uint64_t{1024} * 1024 * 4);
    CHECK(top[0].category == GpuMemoryCategory::Textures);
    CHECK(std::strcmp(top[1].name.c_str(), "big") == 0);
    CHECK(std::strcmp(top[2].name.c_str(), "medium") == 0);
    CHECK(top[0].bytes >= top[1].bytes);
    CHECK(top[1].bytes >= top[2].bytes);

    // More room than textures: all of them (the device's own fallbacks aren't textures).
    TextureMemory all[16];
    CHECK(device->largest_textures(all) == 5);
    CHECK(std::strcmp(all[4].name.c_str(), "tiny") == 0);
    CHECK(device->largest_textures({}) == 0);

    // Long names are cut, and the report logs without trouble.
    const char* long_name = "a_texture_name_that_goes_on_and_on_well_past_sixty_three_characters_of_text";
    const auto named = *device->create_texture({.width = 4096,
                                                .height = 4096,
                                                .format = Format::R8Unorm,
                                                .usage = TextureUsage::Sampled,
                                                .debug_name = long_name});
    REQUIRE(device->largest_textures(std::span(top).first(1)) == 1);
    CHECK(top[0].name.size() == 63);
    CHECK(std::strncmp(top[0].name.c_str(), long_name, 63) == 0);
    device->log_memory_report(3);

    device->destroy(named);
    for (const TextureHandle texture : textures) {
        device->destroy(texture);
    }
    device->wait_idle();
    CHECK(device->largest_textures(all) == 0);
    check_clean(*device);
}
