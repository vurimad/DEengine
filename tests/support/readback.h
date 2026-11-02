#pragma once

#include "core/array.h"
#include "render/rhi/device.h"
#include "render/rhi/upload.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

// Readback and upload helpers for GPU tests.
namespace de::test {

inline de::core::Allocator& test_heap() {
    return de::core::heap_allocator(de::core::MemoryTag::Render);
}

// Bytes that differ per position and per seed, so a misplaced or stale copy shows.
inline de::core::Array<std::byte> pattern(size_t size, uint32_t seed) {
    de::core::Array<std::byte> bytes(test_heap());
    bytes.resize(size);
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::byte>(((i * 31) + (size_t{seed} * 101) + (i >> 8U)) & 0xFFU);
    }
    return bytes;
}

inline std::span<const std::byte> as_span(const de::core::Array<std::byte>& bytes) {
    return bytes.span();
}

// Every mip and layer of a texture in CopySrc state, read back in rhi::texture_upload_size() order.
inline de::core::Array<std::byte> read_texture(rhi::Device& device, rhi::TextureHandle texture) {
    const rhi::TextureDesc desc = *device.texture_desc(texture);
    const uint64_t size = rhi::texture_upload_size(desc, {});
    const auto readback = device.create_buffer(
        {.size = size, .usage = rhi::BufferUsage::CopyDst, .memory = rhi::MemoryLocation::Readback});
    REQUIRE(readback.ok());
    device.begin_frame();
    rhi::CommandList list = device.begin_command_list();
    uint64_t offset = 0;
    for (uint32_t mip = 0; mip < desc.mip_count; ++mip) {
        const uint64_t layer_bytes =
            rhi::texture_upload_size(desc, {.first_mip = mip, .mip_count = 1}) / desc.layer_count;
        for (uint32_t layer = 0; layer < desc.layer_count; ++layer) {
            list.copy_texture_to_buffer(texture, *readback, {.buffer_offset = offset, .mip = mip, .layer = layer});
            offset += layer_bytes;
        }
    }
    list.barrier(rhi::BufferBarrier{
        .buffer = *readback, .before = rhi::ResourceState::CopyDst, .after = rhi::ResourceState::HostRead});
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
inline de::core::Array<std::byte> read_buffer(rhi::Device& device, rhi::BufferHandle buffer) {
    const uint64_t size = device.buffer_desc(buffer)->size;
    const auto readback = device.create_buffer(
        {.size = size, .usage = rhi::BufferUsage::CopyDst, .memory = rhi::MemoryLocation::Readback});
    REQUIRE(readback.ok());
    device.begin_frame();
    rhi::CommandList list = device.begin_command_list();
    list.copy_buffer(buffer, 0, *readback, 0, size);
    list.barrier(rhi::BufferBarrier{
        .buffer = *readback, .before = rhi::ResourceState::CopyDst, .after = rhi::ResourceState::HostRead});
    device.end_frame();
    device.wait_idle();
    device.invalidate_mapped(*readback);
    de::core::Array<std::byte> bytes(test_heap());
    bytes.resize(static_cast<size_t>(size));
    std::memcpy(bytes.data(), device.mapped(*readback).data(), static_cast<size_t>(size));
    device.destroy(*readback);
    return bytes;
}

inline bool same_bytes(std::span<const std::byte> a, std::span<const std::byte> b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

// One frame that records the uploader's next batch.
inline void record_frame(rhi::Device& device, rhi::Uploader& uploader) {
    device.begin_frame();
    uploader.record();
    device.end_frame();
}

// One frame that copies the first 32 bytes of buffer to a readback buffer, not waited for.
inline void copy_out(rhi::Device& device, rhi::BufferHandle buffer, rhi::BufferHandle readback) {
    device.begin_frame();
    rhi::CommandList list = device.begin_command_list();
    list.copy_buffer(buffer, 0, readback, 0, 32);
    list.barrier(rhi::BufferBarrier{
        .buffer = readback, .before = rhi::ResourceState::CopyDst, .after = rhi::ResourceState::HostRead});
    device.end_frame();
}

// Runs the rest of the test case once per uploader path (doctest subcases): the graphics queue, and
// the transfer queue when the device has one. Call it once per test case, unconditionally.
inline bool transfer_path(const rhi::Device& device) {
    bool transfer = false;
    SUBCASE("graphics queue") {}
    if (device.capabilities().transfer_queue) {
        SUBCASE("transfer queue") {
            transfer = true;
        }
    }
    return transfer;
}

} // namespace de::test
