#pragma once

#include "core/math.h"
#include "render/graph/render_graph.h"
#include "render/rhi/device.h"
#include "render/rhi/upload.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace de::tools {

inline constexpr size_t kFrameHistory = 240;

// How many of these frames (at most kFrameHistory) took over 1.5 times their lower quartile: an uneven
// loop, even at a good frame rate. The lower quartile, so neither one hitch nor a rhythm of short and long
// frames can hide it.
size_t late_frames(std::span<const float> frame_ms);

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
    bool has_camera = false; // the active camera's place, to come back to a spot
    DVec3 camera_position{0.0};
    Quat camera_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    bool cull_frozen = false; // SceneRenderer::set_cull_frozen: said in the panel, with what shadows can be judged by
    uint32_t visible_shadow_draws = 0;
    uint32_t visible_shadow_triangles = 0;
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
    void draw(bool* open);

private:
    float frame_ms_[kFrameHistory] = {};
    size_t next_ = 0;
    size_t count_ = 0;
    uint64_t frames_with_allocations_ = 0;
    size_t late_ = 0; // of the history, counted now and then while drawn: it's read as a slow number
    uint32_t draws_until_late_ = 0;
    FrameStats last_; // its gpu_passes point at gpu_passes_
    render::PassGpuTime gpu_passes_[render::kMaxTimedPasses] = {};
};

} // namespace de::tools
