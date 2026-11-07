#include "core/array.h"
#include "core/log.h"
#include "image_diff/image_diff.h"
#include "render/graph/render_graph.h"
#include "support/gpu.h"
#include "tools/screenshot.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <span>

using de::image_diff::decode_png;
using de::tools::encode_screenshot_png;
using de::tools::Screenshot;
namespace rhi = de::rhi;
namespace render = de::render;

namespace {

de::core::Allocator& tools_heap() {
    return de::core::heap_allocator(de::core::MemoryTag::Tools);
}

// A different colour at every texel, in the order the channels are kept: a misplaced or swapped one shows.
de::core::Array<std::byte> pattern(uint32_t width, uint32_t height, uint32_t seed) {
    de::core::Array<std::byte> bytes(tools_heap());
    bytes.resize(size_t{width} * height * 4);
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>(((i * 37) + (size_t{seed} * 11) + 5) & 0xFFU);
    }
    return bytes;
}

// The picture is what the bytes say: first_red is where red is kept in a texel (0 for RGBA, 2 for BGRA), and alpha is
// never kept.
void check_picture(const de::image_diff::Image& image, std::span<const std::byte> texels, uint32_t width,
                   uint32_t height, size_t first_red) {
    REQUIRE(image.width == width);
    REQUIRE(image.height == height);
    const size_t blue = 2 - first_red;
    for (size_t i = 0; i < size_t{width} * height; ++i) {
        const std::byte* const in = texels.data() + (i * 4);
        const uint8_t* const out = image.rgba.data() + (i * 4);
        CHECK(out[0] == static_cast<uint8_t>(in[first_red]));
        CHECK(out[1] == static_cast<uint8_t>(in[1]));
        CHECK(out[2] == static_cast<uint8_t>(in[blue]));
        CHECK(out[3] == 255);
    }
}

} // namespace

TEST_CASE("a screenshot's PNG has the texels' colours, whichever way round they're kept") {
    constexpr uint32_t kWidth = 5;
    constexpr uint32_t kHeight = 3;
    const auto texels = pattern(kWidth, kHeight, 1);
    for (const rhi::Format format : {rhi::Format::RGBA8Unorm, rhi::Format::RGBA8Srgb}) {
        const auto png = encode_screenshot_png(texels.span(), kWidth, kHeight, format, tools_heap());
        REQUIRE(png.ok());
        const auto image = decode_png(png->span());
        REQUIRE(image.ok());
        check_picture(*image, texels.span(), kWidth, kHeight, 0);
    }
    for (const rhi::Format format : {rhi::Format::BGRA8Unorm, rhi::Format::BGRA8Srgb}) {
        const auto png = encode_screenshot_png(texels.span(), kWidth, kHeight, format, tools_heap());
        REQUIRE(png.ok());
        const auto image = decode_png(png->span());
        REQUIRE(image.ok());
        check_picture(*image, texels.span(), kWidth, kHeight, 2);
    }
}

TEST_CASE("a screenshot needs bytes that fit its size, in a format it knows") {
    const auto texels = pattern(4, 4, 1);
    REQUIRE(encode_screenshot_png(texels.span(), 4, 4, rhi::Format::BGRA8Srgb, tools_heap()).ok());
    CHECK_FALSE(encode_screenshot_png(texels.span(), 4, 3, rhi::Format::BGRA8Srgb, tools_heap()).ok());
    CHECK_FALSE(encode_screenshot_png(texels.span(), 0, 0, rhi::Format::BGRA8Srgb, tools_heap()).ok());
    CHECK_FALSE(encode_screenshot_png(texels.span(), 4, 4, rhi::Format::RGBA16Float, tools_heap()).ok());
    CHECK_FALSE(encode_screenshot_png(texels.span(), 4, 4, rhi::Format::RGB10A2Unorm, tools_heap()).ok());
}

TEST_CASE("a screenshot reads a texture back at the end of a frame, over and over, with validation clean") {
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    auto graph = render::RenderGraph::create(*device);
    REQUIRE(graph.ok());
    Screenshot shot;

    const auto picture = [&](uint32_t width, uint32_t height, rhi::Format format, uint32_t seed) {
        const auto texture = device->create_texture(
            {.width = width,
             .height = height,
             .format = format,
             .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc | rhi::TextureUsage::CopyDst});
        REQUIRE(texture.ok());
        const auto texels = pattern(width, height, seed);
        de::test::upload_texture(*device, *texture, texels.span(), rhi::ResourceState::RenderTarget);

        device->begin_frame();
        graph->begin(width, height);
        const render::GraphTexture back = graph->import_texture({.texture = *texture,
                                                                 .name = "picture",
                                                                 .state = rhi::ResourceState::RenderTarget,
                                                                 .final_state = rhi::ResourceState::RenderTarget});
        REQUIRE(shot.add_pass(*device, *graph, back, *texture).ok());
        graph->execute();
        device->end_frame();

        const auto png = shot.take_png(*device, tools_heap());
        REQUIRE(png.ok());
        const auto image = decode_png(png->span());
        REQUIRE(image.ok());
        check_picture(*image, texels.span(), width, height, format == rhi::Format::BGRA8Srgb ? 2 : 0);
        device->destroy(*texture);
    };

    picture(16, 9, rhi::Format::BGRA8Srgb, 1);
    picture(16, 9, rhi::Format::BGRA8Srgb, 2); // the same readback buffer again
    picture(16, 9, rhi::Format::RGBA8Unorm, 3);
    picture(7, 21, rhi::Format::BGRA8Srgb, 4); // another size: another buffer
    shot.release(*device);
    picture(4, 4, rhi::Format::BGRA8Srgb, 5); // and after letting go
    shot.release(*device);
    de::test::check_clean(*device);
}

TEST_CASE("a screenshot of what can't be copied says so, and adds nothing to the frame") {
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    auto graph = render::RenderGraph::create(*device);
    REQUIRE(graph.ok());
    const auto no_copy = device->create_texture(
        {.width = 4, .height = 4, .format = rhi::Format::BGRA8Srgb, .usage = rhi::TextureUsage::RenderTarget});
    REQUIRE(no_copy.ok());
    const auto wrong_format =
        device->create_texture({.width = 4,
                                .height = 4,
                                .format = rhi::Format::RGBA16Float,
                                .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc});
    REQUIRE(wrong_format.ok());

    Screenshot shot;
    device->begin_frame();
    graph->begin(4, 4);
    const render::GraphTexture back = graph->import_texture({.texture = *no_copy,
                                                             .name = "picture",
                                                             .state = rhi::ResourceState::Undefined,
                                                             .final_state = rhi::ResourceState::RenderTarget});
    CHECK_FALSE(shot.add_pass(*device, *graph, back, *no_copy).ok());
    CHECK_FALSE(shot.add_pass(*device, *graph, back, *wrong_format).ok());
    graph->execute();
    device->end_frame();
    device->wait_idle();
    CHECK(graph->stats().passes == 0);
    // Nothing was recorded, so there's nothing to take.
    const de::core::LogLevel saved = de::core::log_level();
    de::core::set_log_level(de::core::LogLevel::Fatal);
    CHECK_FALSE(shot.take_png(*device, tools_heap()).ok());
    de::core::set_log_level(saved);
    shot.release(*device);
    device->destroy(*no_copy);
    device->destroy(*wrong_format);
    de::test::check_clean(*device);
}
