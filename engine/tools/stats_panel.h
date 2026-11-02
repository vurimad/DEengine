#pragma once

#include "render/graph/render_graph.h"
#include "render/rhi/device.h"
#include "render/rhi/upload.h"

#include <cstddef>
#include <cstdint>

namespace de::tools {

// One frame's numbers, gathered by whoever runs the frame.
struct FrameStats {
    float frame_ms = 0.0f;         // CPU time from one frame's start to the next
    uint64_t heap_allocations = 0; // heap allocations during the frame, every thread (the rule: 0)
    rhi::DeviceStats device{};
    rhi::GpuMemoryStats memory{};
    render::RenderGraphStats graph{};
    rhi::UploaderStats uploads{};
};

// The frame stats panel (M0a): frame time and its history, heap allocations per frame, GPU memory
// by category, the render graph, uploads and the device. Draws with Dear ImGui, between NewFrame()
// and Render().
class StatsPanel {
public:
    void record(const FrameStats& stats);
    // A window the user can close: open is set to false then.
    void draw(bool* open) const;

private:
    static constexpr size_t kHistory = 240;

    float frame_ms_[kHistory] = {};
    size_t next_ = 0;
    size_t count_ = 0;
    uint64_t frames_with_allocations_ = 0;
    FrameStats last_;
};

} // namespace de::tools
