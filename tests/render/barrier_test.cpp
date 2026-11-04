#include "render/rhi/device.h"
#include "support/gpu.h"
#include "support/shader_kit.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>

using namespace de::rhi;
using de::test::AddBuffersPush;
using de::test::check_clean;
using de::test::FillBufferPush;
using de::test::make_device;
using de::test::read_u32;
using de::test::ShaderKit;

TEST_CASE("resource states: one write, or reads that go together") {
    // Buffers combine any read states.
    CHECK(is_valid_state(ResourceState::Undefined, false));
    CHECK(is_valid_state(ResourceState::IndirectArgument | ResourceState::ShaderRead, false));
    CHECK(is_valid_state(ResourceState::VertexBuffer | ResourceState::IndexBuffer | ResourceState::CopySrc, false));
    CHECK(is_valid_state(ResourceState::ShaderWrite, false));
    CHECK_FALSE(is_valid_state(ResourceState::ShaderWrite | ResourceState::ShaderRead, false)); // writes stand alone
    CHECK_FALSE(is_valid_state(ResourceState::CopyDst | ResourceState::CopySrc, false));
    CHECK_FALSE(is_valid_state(ResourceState::RenderTarget, false)); // texture-only state

    // Textures combine only read states that share a layout.
    CHECK(is_valid_state(ResourceState::DepthRead | ResourceState::ShaderRead, true));
    CHECK(is_valid_state(ResourceState::Present, true));
    CHECK_FALSE(is_valid_state(ResourceState::ShaderRead | ResourceState::CopySrc, true)); // two layouts
    CHECK_FALSE(is_valid_state(ResourceState::DepthWrite | ResourceState::ShaderRead, true));
    CHECK_FALSE(is_valid_state(ResourceState::IndirectArgument, true)); // buffer-only state

    CHECK((resource_state_name(ResourceState::Undefined).view() == "undefined"));
    CHECK(
        (resource_state_name(ResourceState::DepthRead | ResourceState::ShaderRead).view() == "depth_read|shader_read"));
}

TEST_CASE("unknown state bits are invalid, and names cover every state") {
    const auto unknown = static_cast<ResourceState>(1U << 14);
    CHECK_FALSE(is_valid_state(unknown, false));
    CHECK_FALSE(is_valid_state(unknown, true));
    CHECK_FALSE(is_valid_state(ResourceState::ShaderRead | unknown, false));
    CHECK((resource_state_name(kAllResourceStates).view().find("general") != std::string_view::npos));
    CHECK((resource_state_name(ResourceState::CopySrc | unknown).view() == "copy_src|unknown"));
}

TEST_CASE("a batch merges global barriers without widening their shader stages") {
    BarrierBatch batch;
    CHECK(batch.global() == nullptr);
    // Each half names one side; the empty side's default stages must not widen the merge.
    batch.add(GlobalBarrier{.before = ResourceState::ShaderWrite, .before_stages = ShaderStages::Compute});
    batch.add(GlobalBarrier{.after = ResourceState::ShaderRead, .after_stages = ShaderStages::Compute});
    batch.add(GlobalBarrier{.before = ResourceState::CopyDst, .after = ResourceState::IndirectArgument});
    const GlobalBarrier* merged = batch.global();
    REQUIRE(merged != nullptr);
    CHECK(merged->before == (ResourceState::ShaderWrite | ResourceState::CopyDst));
    CHECK(merged->after == (ResourceState::ShaderRead | ResourceState::IndirectArgument));
    CHECK(merged->before_stages == ShaderStages::Compute);
    CHECK(merged->after_stages == ShaderStages::Compute);
    batch.add(GlobalBarrier{.after = ResourceState::UniformBuffer, .after_stages = ShaderStages::Vertex});
    CHECK(batch.global()->after_stages == (ShaderStages::Compute | ShaderStages::Vertex));

    batch.clear(); // dropped unrecorded (no device here)
    CHECK(batch.empty());
    CHECK(batch.global() == nullptr);
}

TEST_CASE("compute passes chain through stage-narrowed buffer barriers, ranges included") {
    auto device = make_device();
    if (!device) {
        return;
    }
    ShaderKit kit(*device);
    const auto fill = kit.compute("tests.fill_buffer");
    const auto add = kit.compute("tests.add_buffers");
    constexpr uint32_t kCount = 256;
    const auto input = device->create_buffer({.size = uint64_t{kCount} * 4, .usage = BufferUsage::Storage});
    const auto output = device->create_buffer(
        {.size = uint64_t{kCount} * 4, .usage = BufferUsage::Storage, .memory = MemoryLocation::Readback});
    REQUIRE((input.ok() && output.ok()));

    device->begin_frame();
    CommandList list = device->begin_command_list();
    list.bind_pipeline(kit.pipeline(fill));
    list.push_constants(FillBufferPush{.output = device->buffer_address(*input), .value = 100, .count = kCount});
    list.dispatch(kCount / 64);
    // Only compute wrote it and only compute reads it, and only the first half matters below.
    BarrierBatch batch;
    batch.add(BufferBarrier{.buffer = *input,
                            .before = ResourceState::ShaderWrite,
                            .after = ResourceState::ShaderRead,
                            .before_stages = ShaderStages::Compute,
                            .after_stages = ShaderStages::Compute,
                            .size = uint64_t{kCount} / 2 * 4});
    CHECK_FALSE(batch.empty());
    batch.flush(list);
    CHECK(batch.empty());
    list.bind_pipeline(kit.pipeline(add));
    list.push_constants(AddBuffersPush{.input = device->buffer_address(*input),
                                       .output = device->buffer_address(*output),
                                       .value = 5,
                                       .count = kCount / 2});
    list.dispatch(kCount / 2 / 64);
    list.barrier(BufferBarrier{.buffer = *output,
                               .before = ResourceState::ShaderWrite,
                               .after = ResourceState::HostRead,
                               .before_stages = ShaderStages::Compute});
    device->end_frame();
    device->wait_idle();
    device->invalidate_mapped(*output);

    CHECK(read_u32(*device, *output, 0) == 105);
    CHECK(read_u32(*device, *output, kCount / 2 - 1) == 100 + (kCount / 2 - 1) + 5);

    device->destroy(*input);
    device->destroy(*output);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("a global barrier orders every buffer at once") {
    auto device = make_device();
    if (!device) {
        return;
    }
    ShaderKit kit(*device);
    const auto fill = kit.compute("tests.fill_buffer");
    const auto add = kit.compute("tests.add_buffers");
    constexpr uint32_t kCount = 64;
    BufferHandle inputs[3];
    BufferHandle outputs[3];
    for (int i = 0; i < 3; ++i) {
        inputs[i] = *device->create_buffer({.size = uint64_t{kCount} * 4, .usage = BufferUsage::Storage});
        outputs[i] = *device->create_buffer(
            {.size = uint64_t{kCount} * 4, .usage = BufferUsage::Storage, .memory = MemoryLocation::Readback});
    }

    device->begin_frame();
    CommandList list = device->begin_command_list();
    list.bind_pipeline(kit.pipeline(fill));
    for (uint32_t i = 0; i < 3; ++i) {
        list.push_constants(
            FillBufferPush{.output = device->buffer_address(inputs[i]), .value = i * 1000, .count = kCount});
        list.dispatch(1);
    }
    // One barrier for all three, merged in a batch from two halves.
    BarrierBatch batch;
    batch.add(GlobalBarrier{.before = ResourceState::ShaderWrite, .before_stages = ShaderStages::Compute});
    batch.add(GlobalBarrier{.after = ResourceState::ShaderRead, .after_stages = ShaderStages::Compute});
    batch.flush(list);
    list.bind_pipeline(kit.pipeline(add));
    for (uint32_t i = 0; i < 3; ++i) {
        list.push_constants(AddBuffersPush{.input = device->buffer_address(inputs[i]),
                                           .output = device->buffer_address(outputs[i]),
                                           .value = 1,
                                           .count = kCount});
        list.dispatch(1);
    }
    list.barrier(GlobalBarrier{.before = ResourceState::ShaderWrite,
                               .after = ResourceState::HostRead,
                               .before_stages = ShaderStages::Compute});
    device->end_frame();
    device->wait_idle();

    for (uint32_t i = 0; i < 3; ++i) {
        device->invalidate_mapped(outputs[i]);
        CHECK(read_u32(*device, outputs[i], 0) == i * 1000 + 1);
        CHECK(read_u32(*device, outputs[i], kCount - 1) == i * 1000 + kCount);
        device->destroy(inputs[i]);
        device->destroy(outputs[i]);
    }
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("a target reused every frame is discarded from its real previous state, without waiting") {
    auto device = make_device();
    if (!device) {
        return;
    }
    constexpr uint32_t kSize = 8;
    constexpr uint32_t kFrames = 4;
    constexpr uint64_t kBytes = uint64_t{kSize} * kSize * 4;
    const auto target = device->create_texture({.width = kSize,
                                                .height = kSize,
                                                .format = Format::RGBA8Unorm,
                                                .usage = TextureUsage::RenderTarget | TextureUsage::CopySrc});
    const auto readback = device->create_buffer(
        {.size = kBytes * kFrames, .usage = BufferUsage::CopyDst, .memory = MemoryLocation::Readback});
    REQUIRE((target.ok() && readback.ok()));

    // Frames back to back, no waiting in between: synchronization validation checks that each
    // frame's discard waits for the previous frame's copy out of the same texture.
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        device->begin_frame();
        CommandList list = device->begin_command_list();
        list.barrier(TextureBarrier{.texture = *target,
                                    .before = frame == 0 ? ResourceState::Undefined : ResourceState::CopySrc,
                                    .after = ResourceState::RenderTarget,
                                    .discard = true});
        const float shade = static_cast<float>(frame + 1) / 4.0f;
        const ColorAttachment color{.texture = *target, .clear = {.r = shade, .g = 0, .b = 0, .a = 1}};
        list.begin_rendering({.colors = std::span(&color, 1)});
        list.end_rendering();
        list.barrier(*target, ResourceState::RenderTarget, ResourceState::CopySrc);
        list.copy_texture_to_buffer(*target, *readback, {.buffer_offset = kBytes * frame});
        if (frame + 1 == kFrames) {
            list.barrier(*readback, ResourceState::CopyDst, ResourceState::HostRead);
        }
        device->end_frame();
    }
    device->wait_idle();
    device->invalidate_mapped(*readback);
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        const auto texel = static_cast<uint8_t>(device->mapped(*readback)[kBytes * frame]);
        CHECK(std::abs(static_cast<int>(texel) - static_cast<int>((frame + 1) * 255 / 4)) <= 1);
    }

    device->destroy(*target);
    device->destroy(*readback);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("begin_frame waits for the previous frame") {
    auto device = make_device();
    if (!device) {
        return;
    }
    constexpr uint32_t kSize = 256;
    const auto target = device->create_texture({.width = kSize,
                                                .height = kSize,
                                                .format = Format::RGBA8Unorm,
                                                .usage = TextureUsage::RenderTarget | TextureUsage::CopySrc});
    const auto readback = device->create_buffer(
        {.size = uint64_t{kSize} * kSize * 4, .usage = BufferUsage::CopyDst, .memory = MemoryLocation::Readback});
    REQUIRE((target.ok() && readback.ok()));
    // Each frame's copy is there once the next frame begins, with nothing else waited for.
    for (uint32_t frame = 0; frame < 32; ++frame) {
        device->begin_frame();
        CommandList list = device->begin_command_list();
        list.barrier(TextureBarrier{.texture = *target,
                                    .before = frame == 0 ? ResourceState::Undefined : ResourceState::CopySrc,
                                    .after = ResourceState::RenderTarget,
                                    .discard = true});
        const float shade = static_cast<float>(frame % 4 + 1) / 4.0f;
        const ColorAttachment color{.texture = *target, .clear = {.r = shade, .g = 0, .b = 0, .a = 1}};
        list.begin_rendering({.colors = std::span(&color, 1)});
        list.end_rendering();
        list.barrier(*target, ResourceState::RenderTarget, ResourceState::CopySrc);
        list.copy_texture_to_buffer(*target, *readback);
        list.barrier(*readback, ResourceState::CopyDst, ResourceState::HostRead);
        device->end_frame();

        device->begin_frame();
        device->invalidate_mapped(*readback);
        const auto texel = static_cast<uint8_t>(device->mapped(*readback)[0]);
        CAPTURE(frame);
        CHECK(std::abs(static_cast<int>(texel) - static_cast<int>((frame % 4 + 1) * 255 / 4)) <= 1);
        device->end_frame();
    }
    device->wait_idle();
    device->destroy(*target);
    device->destroy(*readback);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("a depth buffer can be tested read-only and read by shaders at once") {
    auto device = make_device();
    if (!device) {
        return;
    }
    if (!device->capabilities().supports(Format::D32Float, FormatSupport::DepthStencil | FormatSupport::Sampled |
                                                               FormatSupport::CopySrc)) {
        MESSAGE("skipped: d32_float can't be sampled or copied on this GPU");
        return;
    }
    const auto depth =
        device->create_texture({.width = 16,
                                .height = 16,
                                .format = Format::D32Float,
                                .usage = TextureUsage::DepthStencil | TextureUsage::Sampled | TextureUsage::CopySrc});
    const auto readback = device->create_buffer(
        {.size = uint64_t{16} * 16 * 4, .usage = BufferUsage::CopyDst, .memory = MemoryLocation::Readback});
    REQUIRE((depth.ok() && readback.ok()));
    constexpr ResourceState kTestedAndSampled = ResourceState::DepthRead | ResourceState::ShaderRead;

    device->begin_frame();
    CommandList list = device->begin_command_list();
    list.barrier(*depth, ResourceState::Undefined, ResourceState::DepthWrite);
    const DepthAttachment clear{.texture = *depth, .clear_depth = 0.25f};
    list.begin_rendering({.depth = &clear});
    list.end_rendering();
    list.barrier(TextureBarrier{.texture = *depth,
                                .before = ResourceState::DepthWrite,
                                .after = kTestedAndSampled,
                                .after_stages = ShaderStages::Fragment});
    // A pass that depth-tests against it read-only (a decal or transparent pass would draw here).
    const DepthAttachment tested{.texture = *depth, .load = LoadOp::Load, .read_only = true};
    list.begin_rendering({.depth = &tested});
    list.end_rendering();
    list.barrier(TextureBarrier{.texture = *depth,
                                .before = kTestedAndSampled,
                                .after = ResourceState::CopySrc,
                                .before_stages = ShaderStages::Fragment});
    list.copy_texture_to_buffer(*depth, *readback);
    list.barrier(*readback, ResourceState::CopyDst, ResourceState::HostRead);
    device->end_frame();
    device->wait_idle();
    device->invalidate_mapped(*readback);

    float value = 0.0f;
    std::memcpy(&value, device->mapped(*readback).data(), sizeof(value));
    CHECK(value == 0.25f); // the read-only pass kept the contents

    device->destroy(*depth);
    device->destroy(*readback);
    device->wait_idle();
    check_clean(*device);
}
