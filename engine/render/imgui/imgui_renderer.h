#pragma once

#include "core/result.h"
#include "render/graph/render_graph.h"
#include "render/rhi/rhi.h"

#include <cstdint>

struct ImDrawData;

namespace de::rhi {
class Device;
class Uploader;
} // namespace de::rhi

namespace de::shader {
class ShaderLibrary;
}

namespace de::render {

struct ImGuiRendererState;

// Draws Dear ImGui through the RHI and the render graph: vertices through a buffer pointer,
// textures by bindless index, no descriptor sets (shaders/imgui.slang). It owns the textures
// ImGui asks for (its font atlas), created and updated through the uploader.
//
// Each frame, after ImGui::Render():
//     renderer.update_textures(ImGui::GetDrawData());        // before uploader.record()
//     ...
//     renderer.add_pass(graph, backbuffer, format, ImGui::GetDrawData());
//
// Needs the ImGui context to exist from create() to destruction. Move-only.
class ImGuiRenderer {
public:
    static core::Result<ImGuiRenderer> create(rhi::Device& device, shader::ShaderLibrary& shaders,
                                              rhi::Uploader& uploader);

    ~ImGuiRenderer();
    ImGuiRenderer(ImGuiRenderer&& other) noexcept;
    ImGuiRenderer& operator=(ImGuiRenderer&& other) noexcept;
    ImGuiRenderer(const ImGuiRenderer&) = delete;
    ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;

    // Creates, updates and destroys the textures ImGui asks for. Its uploads land with the
    // uploader's next record(); a texture is drawn only once its first upload has.
    void update_textures(ImDrawData* draw_data);

    // A pass drawing draw_data over target (a color texture of `format`, contents kept), between
    // graph.begin() and execute(). draw_data must stay valid until execute(): ImGui keeps it until
    // the next NewFrame().
    void add_pass(RenderGraph& graph, GraphTexture target, rhi::Format format, const ImDrawData* draw_data);

    // The ImTextureID that shows an engine texture with ImGui::Image(): its bindless index (a
    // sampled 2D texture, in ShaderRead state for fragment shaders when ImGui draws).
    static uint64_t texture_id(uint32_t bindless_index) { return bindless_index; }

private:
    explicit ImGuiRenderer(ImGuiRendererState* state) : state_(state) {}

    ImGuiRendererState* state_ = nullptr;
};

} // namespace de::render
