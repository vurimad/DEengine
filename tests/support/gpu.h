#pragma once

#include "render/rhi/device.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>

namespace de::test {

// A headless device with validation and synchronization validation on. Without a Vulkan 1.3
// device the calling test skips (returns), unless DE_REQUIRE_GPU is set, as on CI's Linux runners
// (lavapipe): then it fails.
//     auto device = make_device();
//     if (!device) { return; }
inline core::Result<rhi::Device> make_device() {
    auto device = rhi::Device::create({.app_name = "de_tests", .validation = true, .sync_validation = true});
    if (!device) {
        const char* require = std::getenv("DE_REQUIRE_GPU");
        if (require != nullptr && require[0] != '\0' && require[0] != '0') {
            FAIL("no Vulkan 1.3 device, and DE_REQUIRE_GPU is set: ", device.error().message);
        }
        MESSAGE("skipped: no Vulkan 1.3 device (", device.error().message, ")");
    }
    return device;
}

// No validation message of any kind so far.
inline void check_clean(const rhi::Device& device) {
    const rhi::DeviceStats stats = device.stats();
    CHECK(stats.validation_errors == 0);
    CHECK(stats.validation_warnings == 0);
}

// The index-th 32-bit word of a mapped (Readback or Upload) buffer.
inline uint32_t read_u32(rhi::Device& device, rhi::BufferHandle buffer, size_t index) {
    uint32_t value = 0;
    std::memcpy(&value, device.mapped(buffer).data() + (index * sizeof(uint32_t)), sizeof(value));
    return value;
}

// Uploads mip 0, layer 0 of a fresh texture (CopyDst usage) through a staging buffer, leaves it
// in after state for after_stages, and waits for the GPU.
inline void upload_texture(rhi::Device& device, rhi::TextureHandle texture, std::span<const std::byte> texels,
                           rhi::ResourceState after, rhi::ShaderStages after_stages = rhi::ShaderStages::All) {
    const auto staging = device.create_buffer(
        {.size = texels.size(), .usage = rhi::BufferUsage::CopySrc, .memory = rhi::MemoryLocation::Upload});
    REQUIRE(staging.ok());
    std::memcpy(device.mapped(*staging).data(), texels.data(), texels.size());
    device.flush_mapped(*staging);
    device.begin_frame();
    rhi::CommandList list = device.begin_command_list();
    list.barrier(texture, rhi::ResourceState::Undefined, rhi::ResourceState::CopyDst);
    list.copy_buffer_to_texture(*staging, texture);
    list.barrier(rhi::TextureBarrier{
        .texture = texture, .before = rhi::ResourceState::CopyDst, .after = after, .after_stages = after_stages});
    device.end_frame();
    device.wait_idle();
    device.destroy(*staging);
}

// Push constants of shaders/tests/fill_buffer.slang (and the hot reload test shaders).
struct FillBufferPush {
    uint64_t output = 0;
    uint32_t value = 0;
    uint32_t count = 0;
};

// Push constants of shaders/tests/add_buffers.slang.
struct AddBuffersPush {
    uint64_t input = 0;
    uint64_t output = 0;
    uint32_t value = 0;
    uint32_t count = 0;
};

// Push constants of shaders/tests/sample_texture.slang.
struct SampleTexturePush {
    uint64_t output = 0; // float4 per texel
    uint32_t texture = 0;
    uint32_t sampler = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// Push constants of shaders/tests/write_storage.slang.
struct WriteStoragePush {
    uint32_t texture = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

} // namespace de::test
