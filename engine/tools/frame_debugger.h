#pragma once

#include "render/graph/render_graph.h"
#include "render/rhi/rhi.h"

#include <cstdint>
#include <string_view>

namespace de::console {
class Console;
class Output;
} // namespace de::console

namespace de::rhi {
class Device;
}

namespace de::shader {
class ShaderLibrary;
}

namespace de::tools {

struct FrameDebuggerState;

// The texel the frame debugger's probe last read (FrameDebugger::probe()).
struct ProbeValue {
    bool valid = false; // a readback has come in for the texture viewed now
    uint32_t x = 0;
    uint32_t y = 0;
    float value[4] = {}; // as stored: an integer texture's value in the first; channels a format lacks read 0, 0, 1
};

// The frame debugger (docs/design.md, Rendering: the render graph's debug view): what the GPU did this frame, pass
// by pass, while the game runs. Nothing is captured or replayed: the render graph already knows every pass, what it
// reads and writes and how long it took, so the debugger reads that, live.
//
//   - The passes in the order they ran, each with its GPU time (averaged, and its peak), what it recorded (draws,
//     indirect calls and the most draws they can make, dispatches, copies) and its share of the frame. Culled passes
//     are listed too. Mark keeps the times as they are now, and every pass then shows how far it has moved from
//     that: change a setting, read the difference.
//   - A pass's textures and buffers: what it writes, what it reads, their sizes and formats.
//   - Any of those textures as that pass left it, before a later pass writes over it or the pool hands it to
//     another: a pass of the debugger's own, added to the graph right after the one chosen (RenderGraph's pass hook),
//     draws the texture into the view. Zoom and pan by texels, unfiltered; a channel alone; a range of values
//     stretched over the display; mips, layers, cube faces, 3D slices; depth and integer textures. The texel under
//     the mouse is read back as stored.
//
// Closed, it costs nothing: no hook, no passes, nothing copied. Open, the frame has up to four small passes more,
// which it leaves out of what it lists and counts. The window's numbers and its list change a few times a second,
// not every frame, so they can be read; the texture shown is live.
//
// Each frame:
//     debugger.draw();                               // in the UI frame: its window, while it's open
//     graph.begin(...);
//     debugger.begin_frame(graph, ui_format);        // watches the passes as they're added
//     ... the frame's passes ...
//     ui.add_pass(graph, back, format, {debugger.shown()...});   // the UI reads the view's texture
//     graph.execute();
//     debugger.end_frame(graph);                     // takes the frame's passes in
//
// With Dear ImGui (draw() between NewFrame() and Render()); the rest works without it (tests, the console's
// commands). Move-only; the device and the shader library must outlive it.
class FrameDebugger {
public:
    static FrameDebugger create(rhi::Device& device, shader::ShaderLibrary& shaders);

    ~FrameDebugger();
    FrameDebugger(FrameDebugger&& other) noexcept;
    FrameDebugger& operator=(FrameDebugger&& other) noexcept;
    FrameDebugger(const FrameDebugger&) = delete;
    FrameDebugger& operator=(const FrameDebugger&) = delete;

    bool open() const;
    void set_open(bool open);

    // Its window, while open; its close button closes it.
    void draw();

    // After graph.begin(). ui_format: the format of what the UI draws into (the view's texture is shown there).
    void begin_frame(render::RenderGraph& graph, rhi::Format ui_format);
    // The view's texture in this frame's graph, for the UI's pass to read; invalid when the view drew nothing this
    // frame.
    render::GraphTexture shown() const;
    // After graph.execute().
    void end_frame(render::RenderGraph& graph);

    // What draw() does with the mouse, by name and number (the console's commands, tests).
    //
    // The pass of that name to look at (occurrence: which of several with one name, from 0), and one of its textures
    // by name; an empty texture: the first it writes, once the pass has been seen. They needn't be in the frame yet.
    void select(std::string_view pass, uint32_t occurrence = 0, std::string_view texture = {});
    // The view's size in pixels (draw() sets it to its panel's).
    void set_view_size(uint32_t width, uint32_t height);
    // The texel to read back from now on; probe() has it a few frames later.
    void probe_at(uint32_t x, uint32_t y);
    ProbeValue probe() const;
    // Whether the last frame drew the texture chosen, and if not, why (a line of text; empty when it did).
    bool viewing() const;
    const char* view_problem() const;
    // The passes as text: one a line, with its time and what it recorded. pass (from 0, as listed): that one's
    // textures and buffers instead.
    void report(console::Output& out, int32_t pass = -1) const;

    // frame_debugger [on | off], frame_debugger.report [pass], frame_debugger.select <pass> [texture],
    // frame_debugger.probe [x y]. Taken away again when the debugger goes; the console must outlive it.
    void add_commands(console::Console& console);

private:
    explicit FrameDebugger(FrameDebuggerState* state) : state_(state) {}

    FrameDebuggerState* state_ = nullptr;
};

} // namespace de::tools
