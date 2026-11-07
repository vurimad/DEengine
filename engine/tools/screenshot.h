#pragma once

#include "core/allocator.h"
#include "core/array.h"
#include "core/result.h"
#include "render/graph/render_graph.h"
#include "render/rhi/device.h"
#include "render/rhi/rhi.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace de::tools {

// A PNG of a picture as the GPU keeps it: 8 bits a channel in RGBA or BGRA order, the rows tightly packed,
// sRGB or not (the bytes are kept as they are: what the screen shows). No alpha: what's in it isn't shown.
// Fails on any other format.
core::Result<core::Array<std::byte>> encode_screenshot_png(std::span<const std::byte> texels, uint32_t width,
                                                           uint32_t height, rhi::Format format,
                                                           core::Allocator& allocator);

// Reads a texture back at the end of a frame and makes a PNG of it. Costs nothing until a picture is asked for;
// its readback buffer is then kept for the next one, until release().
//
//     shot.add_pass(device, graph, back, backbuffer); // last, so it sees what the frame's passes drew
//     graph.execute();
//     device.end_frame();
//     auto png = shot.take_png(device, allocator);    // waits for the GPU
class Screenshot {
public:
    // Fails, without adding anything, if the texture can't be copied from (CopySrc usage: a swapchain has it with
    // DeviceDesc::swapchain_copy_src, if the display allows it) or has a format encode_screenshot_png can't take.
    core::Result<void> add_pass(rhi::Device& device, render::RenderGraph& graph, render::GraphTexture target,
                                rhi::TextureHandle texture);
    // After the frame with the pass ended. Waits until the GPU is idle.
    core::Result<core::Array<std::byte>> take_png(rhi::Device& device, core::Allocator& allocator);
    // Lets go of the readback buffer; before the device goes.
    void release(rhi::Device& device);

private:
    rhi::BufferHandle readback_;
    bool readback_used_ = false; // copied to before: its first use now comes after that one
    bool picture_pending_ = false;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    rhi::Format format_ = rhi::Format::Unknown;
};

} // namespace de::tools
