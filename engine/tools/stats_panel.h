#pragma once

#include "render/graph/render_graph.h"
#include "render/rhi/device.h"
#include "render/rhi/upload.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace de::tools {

// How many of these frames took over 1.5 times their lower quartile: an uneven loop, even at a good frame
// rate. The lower quartile, so neither one hitch nor a rhythm of short and long frames can hide it.
inline constexpr size_t kFrameHistory = 240;
size_t late_frames(std::span<const float> frame_ms); // at most kFrameHistory

// One frame's numbers, gathered by whoever runs the frame.
struct FrameStats {
    float frame_ms = 0.0f;         // CPU time from one frame's start to the next
    uint64_t heap_allocations = 0; // heap allocations during the frame, every thread (the rule: 0)
    rhi::DeviceStats device{};
    rhi::GpuMemoryStats memory{};
    render::RenderGraphStats graph{};
    rhi::UploaderStats uploads{};
    std::span<const render::PassGpuTime> gpu_passes{}; // RenderGraph::gpu_times(): copied by record()
    float gpu_frame_ms = 0.0f;
};

// The frame stats panel (M0a): frame time and its history, heap allocations per frame, GPU memory
// by category, GPU time per render graph pass, the render graph, uploads and the device. Draws with Dear ImGui, between
// NewFrame() and Render().
class StatsPanel {
public:
    StatsPanel() = default;
    // last_ points into this panel's own copy of the GPU times.
    StatsPanel(const StatsPanel&) = delete;
    StatsPanel& operator=(const StatsPanel&) = delete;
    StatsPanel(StatsPanel&&) = delete;
    StatsPanel& operator=(StatsPanel&&) = delete;

    void record(const FrameStats& stats);
    // A window the user can close: open is set to false then.
    void draw(bool* open) const;

private:
    float frame_ms_[kFrameHistory] = {};
    size_t next_ = 0;
    size_t count_ = 0;
    uint64_t frames_with_allocations_ = 0;
    size_t late_ = 0; // of the history, counted now and then: it's read as a slow number
    FrameStats last_; // its gpu_passes point at gpu_passes_
    render::PassGpuTime gpu_passes_[render::kMaxTimedPasses] = {};
};

} // namespace de::tools
