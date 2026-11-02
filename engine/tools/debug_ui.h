#pragma once

#include "render/graph/render_graph.h"
#include "render/rhi/rhi.h"

namespace de::platform {
class Window;
}

namespace de::rhi {
class Device;
class Uploader;
} // namespace de::rhi

namespace de::shader {
class ShaderLibrary;
}

namespace de::tools {

// The Dear ImGui context for debug and editor UI. Its memory comes from the Tools heap (tagged,
// counted per frame like everything else); docking is on; nothing is saved to an ini file (files
// go through the VFS only). One at a time. DebugUi below uses these; tests use them alone.
void create_imgui();
void destroy_imgui();

struct DebugUiState;

// The debug UI of a running engine: the ImGui context, its platform side and its renderer, made
// together and torn down together in the right order (renderer, platform, context) when it goes.
// Without a shader library there is no renderer, and no UI frames are built.
//
//     if (ui.begin_frame(window, dt)) { ...ImGui windows...; ui.end_frame(); }   // before uploader.record()
//     ...
//     ui.add_pass(graph, backbuffer, format);
class DebugUi {
public:
    static DebugUi create(platform::Window& window, rhi::Device& device, shader::ShaderLibrary* shaders,
                          rhi::Uploader& uploader);

    ~DebugUi();
    DebugUi(DebugUi&& other) noexcept;
    DebugUi& operator=(DebugUi&& other) noexcept;
    DebugUi(const DebugUi&) = delete;
    DebugUi& operator=(const DebugUi&) = delete;

    // Starts a UI frame; false when there's no renderer: then build nothing and skip end_frame().
    bool begin_frame(platform::Window& window, float delta_s);
    // Ends it: ImGui renders, and its texture requests go to the uploader (before its record()).
    void end_frame();
    // Draws the frame's UI over target, if a UI frame was built.
    void add_pass(render::RenderGraph& graph, render::GraphTexture target, rhi::Format format);

private:
    explicit DebugUi(DebugUiState* state) : state_(state) {}

    DebugUiState* state_ = nullptr;
};

} // namespace de::tools
