#include "render/graph/render_graph.h"
#include "render/imgui/imgui_renderer.h"
#include "render/rhi/device.h"
#include "render/rhi/upload.h"
#include "support/gpu.h"
#include "support/shader_kit.h"
#include "tools/debug_ui.h"
#include "tools/stats_panel.h"

#include <doctest/doctest.h>
#include <imgui.h>

#include <cstdint>
#include <cstdlib>
#include <span>

using namespace de::rhi;
using de::render::GraphBuffer;
using de::render::GraphTexture;
using de::render::ImGuiRenderer;
using de::render::PassBuilder;
using de::render::PassContext;
using de::render::RenderGraph;
using de::test::check_clean;
using de::test::make_device;
using de::test::read_u32;
using de::test::ShaderKit;

namespace {

constexpr uint32_t kSize = 128;

struct ImGuiScope {
    ImGuiScope() {
        de::tools::create_imgui();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(kSize), static_cast<float>(kSize));
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        io.DeltaTime = 1.0f / 60.0f;
    }
    ~ImGuiScope() { de::tools::destroy_imgui(); }
    ImGuiScope(const ImGuiScope&) = delete;
    ImGuiScope& operator=(const ImGuiScope&) = delete;
};

// One frame: a window filling the target with some text, drawn by the renderer over a black
// clear, then copied out.
void draw_frame(Device& device, Uploader& uploader, RenderGraph& graph, ImGuiRenderer& renderer, TextureHandle target,
                BufferHandle readback, bool two_windows = false) {
    ImGui::NewFrame();
    // One window over all of it, or two side by side: two draw lists, the second's vertices after
    // the first's in the vertex buffer.
    const float width = two_windows ? kSize / 2.0f : static_cast<float>(kSize);
    for (int window = 0; window < (two_windows ? 2 : 1); ++window) {
        ImGui::SetNextWindowPos(ImVec2(width * static_cast<float>(window), 0.0f));
        ImGui::SetNextWindowSize(ImVec2(width, static_cast<float>(kSize)));
        ImGui::Begin(window == 0 ? "left" : "right", nullptr, ImGuiWindowFlags_NoDecoration);
        ImGui::TextUnformatted("Hello");
        ImGui::Button("Button");
        ImGui::End();
    }
    ImGui::Render();
    renderer.update_textures(ImGui::GetDrawData());

    device.begin_frame();
    uploader.record();
    graph.begin(kSize, kSize);
    const Format format = device.texture_desc(target)->format;
    const GraphTexture color = graph.import_texture({.texture = target, .name = "target"});
    const GraphBuffer out =
        graph.import_buffer({.buffer = readback, .name = "readback", .final_state = ResourceState::HostRead});
    graph.add_pass(
        "clear", [&](PassBuilder& pass) { pass.use(color, ResourceState::RenderTarget); },
        [color](PassContext& context) {
            const ColorAttachment attachment{.texture = context.texture(color), .clear = {.a = 1.0f}};
            context.list.begin_rendering({.colors = std::span(&attachment, 1)});
            context.list.end_rendering();
        });
    renderer.add_pass(graph, color, format, ImGui::GetDrawData());
    graph.add_pass(
        "copy out",
        [&](PassBuilder& pass) {
            pass.use(color, ResourceState::CopySrc);
            pass.use(out, ResourceState::CopyDst);
        },
        [color, out](PassContext& context) {
            context.list.copy_texture_to_buffer(context.texture(color), context.buffer(out));
        });
    graph.execute();
    device.end_frame();
    device.wait_idle();
    device.invalidate_mapped(readback);
}

struct Pixels {
    uint32_t background = 0; // the window's background where nothing else is (bottom right)
    uint32_t bright = 0;     // pixels with a channel above 200: text
};

Pixels look(Device& device, BufferHandle readback) {
    Pixels pixels{.background = read_u32(device, readback, (kSize * (kSize - 4)) + (kSize - 4))};
    for (uint32_t i = 0; i < kSize * kSize; ++i) {
        const uint32_t texel = read_u32(device, readback, i);
        pixels.bright += ((texel & 0xFFU) > 200 || ((texel >> 8U) & 0xFFU) > 200) ? 1 : 0;
    }
    return pixels;
}

int channel(uint32_t texel, uint32_t index) {
    return static_cast<int>((texel >> (index * 8U)) & 0xFFU);
}

} // namespace

TEST_CASE("Dear ImGui draws through the RHI and the render graph, sRGB targets included") {
    auto device = make_device();
    if (!device) {
        return;
    }
    ShaderKit kit(*device);
    auto uploader = Uploader::create(*device);
    REQUIRE(uploader.ok());
    auto graph = RenderGraph::create(*device);
    REQUIRE(graph.ok());
    const ImGuiScope imgui;
    auto renderer = ImGuiRenderer::create(*device, kit.library(), *uploader);
    REQUIRE(renderer.ok());
    const BufferHandle readback = *device->create_buffer(
        {.size = uint64_t{kSize} * kSize * 4, .usage = BufferUsage::CopyDst, .memory = MemoryLocation::Readback});

    Pixels seen[2];
    const Format formats[] = {Format::RGBA8Unorm, Format::RGBA8Srgb};
    for (int f = 0; f < 2; ++f) {
        CAPTURE(f);
        const TextureHandle target =
            *device->create_texture({.width = kSize,
                                     .height = kSize,
                                     .format = formats[f],
                                     .usage = TextureUsage::RenderTarget | TextureUsage::CopySrc});
        // The first frame creates the font atlas and draws with it at once (its upload lands at
        // the frame's start); the pipeline may take a few frames to compile.
        for (int frame = 0; frame < 30; ++frame) {
            draw_frame(*device, *uploader, *graph, *renderer, target, readback);
            seen[f] = look(*device, readback);
            if (seen[f].bright > 0) {
                break;
            }
            kit.library().update();
        }
        device->destroy(target);
    }
    // Text and a window background, on both targets; the sRGB one encodes the same colors.
    for (const Pixels& pixels : seen) {
        CHECK(pixels.bright > 20);
        CHECK(channel(pixels.background, 0) > 4);
    }
    for (uint32_t c = 0; c < 3; ++c) {
        CHECK(std::abs(channel(seen[0].background, c) - channel(seen[1].background, c)) <= 3);
    }
    device->destroy(readback);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("every draw list draws its own vertices") {
    auto device = make_device();
    if (!device) {
        return;
    }
    ShaderKit kit(*device);
    auto uploader = Uploader::create(*device);
    REQUIRE(uploader.ok());
    auto graph = RenderGraph::create(*device);
    REQUIRE(graph.ok());
    const ImGuiScope imgui;
    auto renderer = ImGuiRenderer::create(*device, kit.library(), *uploader);
    REQUIRE(renderer.ok());
    const TextureHandle target = *device->create_texture({.width = kSize,
                                                          .height = kSize,
                                                          .format = Format::RGBA8Unorm,
                                                          .usage = TextureUsage::RenderTarget | TextureUsage::CopySrc});
    const BufferHandle readback = *device->create_buffer(
        {.size = uint64_t{kSize} * kSize * 4, .usage = BufferUsage::CopyDst, .memory = MemoryLocation::Readback});
    // Text in each half: if the second list read the first list's vertices, its half would hold
    // nothing (or the first window again, over the left half).
    uint32_t left = 0;
    uint32_t right = 0;
    for (int frame = 0; frame < 30 && (left == 0 || right == 0); ++frame) {
        draw_frame(*device, *uploader, *graph, *renderer, target, readback, true);
        left = 0;
        right = 0;
        for (uint32_t y = 0; y < kSize; ++y) {
            for (uint32_t x = 0; x < kSize; ++x) {
                const uint32_t texel = read_u32(*device, readback, (y * kSize) + x);
                const bool bright = (texel & 0xFFU) > 200 || ((texel >> 8U) & 0xFFU) > 200;
                (x < kSize / 2 ? left : right) += bright ? 1 : 0;
            }
        }
        kit.library().update();
    }
    CHECK(ImGui::GetDrawData()->CmdLists.Size >= 2);
    CHECK(left > 10);
    CHECK(right > 10);
    device->destroy(target);
    device->destroy(readback);
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("the ImGui renderer gives its textures back, and a new one makes them again") {
    auto device = make_device();
    if (!device) {
        return;
    }
    ShaderKit kit(*device);
    auto uploader = Uploader::create(*device);
    REQUIRE(uploader.ok());
    const ImGuiScope imgui;
    const uint32_t textures_before = device->stats().live_textures;
    {
        auto renderer = ImGuiRenderer::create(*device, kit.library(), *uploader);
        REQUIRE(renderer.ok());
        ImGui::NewFrame();
        ImGui::Render();
        renderer->update_textures(ImGui::GetDrawData());
        CHECK(device->stats().live_textures == textures_before + 1); // the font atlas
        CHECK(ImGui::GetIO().Fonts->TexData->Status == ImTextureStatus_OK);
        uploader->flush_and_wait();
    }
    CHECK(device->stats().live_textures == textures_before);
    CHECK(ImGui::GetIO().Fonts->TexData->Status == ImTextureStatus_WantCreate); // pixels kept, ready for a renderer
    auto again = ImGuiRenderer::create(*device, kit.library(), *uploader);
    REQUIRE(again.ok());
    ImGui::NewFrame();
    ImGui::Render();
    again->update_textures(ImGui::GetDrawData());
    CHECK(ImGui::GetIO().Fonts->TexData->Status == ImTextureStatus_OK);
    uploader->flush_and_wait();
    device->wait_idle();
    check_clean(*device);
}

TEST_CASE("the stats panel draws whatever the frame's numbers are") {
    const ImGuiScope imgui;
    de::tools::StatsPanel panel;
    for (int frame = 0; frame < 300; ++frame) {
        panel.record({.frame_ms = 8.0f + static_cast<float>(frame % 7), .heap_allocations = frame % 50 == 0 ? 3U : 0U});
    }
    ImGui::GetIO().Fonts->Build();
    bool open = true;
    for (int frame = 0; frame < 2; ++frame) { // a new window is hidden on its first frame, while it sizes itself
        ImGui::NewFrame();
        panel.draw(&open);
        ImGui::Render();
    }
    CHECK(open);
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
}
