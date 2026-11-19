#pragma once

#include "core/math.h"
#include "render/graph/render_graph.h"
#include "render/rhi/device.h"
#include "render/rhi/upload.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace de::tools {

inline constexpr size_t kFrameHistory = 240;

// How many of these frames (at most kFrameHistory) took over 1.5 times their lower quartile: an uneven
// loop, even at a good frame rate. The lower quartile, so neither one hitch nor a rhythm of short and long
// frames can hide it.
size_t late_frames(std::span<const float> frame_ms);

// What the frame stats show (--stats): sections, as bits.
inline constexpr uint32_t kStatsFrame = 1U << 0;       // frames a second and their time, the worst, the late ones
inline constexpr uint32_t kStatsGraph = 1U << 1;       // frame time over the last kFrameHistory frames
inline constexpr uint32_t kStatsAllocations = 1U << 2; // heap allocations in the frame
inline constexpr uint32_t kStatsCamera = 1U << 3;      // where the camera is and where it looks
inline constexpr uint32_t kStatsCull = 1U << 4;        // what the frozen cull kept (F4), while it's frozen
inline constexpr uint32_t kStatsMemory = 1U << 5;      // GPU memory, by category
inline constexpr uint32_t kStatsGpu = 1U << 6;         // GPU time, by render graph pass
inline constexpr uint32_t kStatsRenderGraph = 1U << 7; // passes, barriers, the pool
inline constexpr uint32_t kStatsUploads = 1U << 8;     // queued, staging, uploaded
inline constexpr uint32_t kStatsDevice = 1U << 9;      // live objects, bindless entries, validation messages
inline constexpr uint32_t kStatsParticles = 1U << 10;  // effects, their emitters and particles, the pool: with any
inline constexpr uint32_t kStatsLights = 1U << 11;     // local lights: there, lighting the view, dropped: with any
inline constexpr uint32_t kStatsRays = 1U << 12;       // what rays are traced against: where anything is
inline constexpr uint32_t kStatsDefault = kStatsFrame | kStatsGraph | kStatsAllocations | kStatsCamera | kStatsCull |
                                          kStatsMemory | kStatsGpu | kStatsRenderGraph | kStatsParticles |
                                          kStatsLights | kStatsRays;
inline constexpr uint32_t kStatsAll = (1U << 13U) - 1U;

// The sections' names, as --stats and the console's stat take them.
struct StatsSectionName {
    std::string_view name;
    uint32_t bits = 0;
};
inline constexpr StatsSectionName kStatsSectionNames[] = {{.name = "frame", .bits = kStatsFrame},
                                                          {.name = "graph", .bits = kStatsGraph},
                                                          {.name = "allocations", .bits = kStatsAllocations},
                                                          {.name = "camera", .bits = kStatsCamera},
                                                          {.name = "cull", .bits = kStatsCull},
                                                          {.name = "memory", .bits = kStatsMemory},
                                                          {.name = "gpu", .bits = kStatsGpu},
                                                          {.name = "render_graph", .bits = kStatsRenderGraph},
                                                          {.name = "uploads", .bits = kStatsUploads},
                                                          {.name = "device", .bits = kStatsDevice},
                                                          {.name = "particles", .bits = kStatsParticles},
                                                          {.name = "lights", .bits = kStatsLights},
                                                          {.name = "rays", .bits = kStatsRays}};

// Sections by name, comma separated: those above, fps (frame, as Unreal's), default and all. False on a name it
// doesn't know.
bool parse_stats_sections(std::string_view text, uint32_t& sections);

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
    // The auto exposure, a few frames late (SceneRenderer::auto_exposure_state): where it is and where it's going, as
    // powers of 2. has_exposure false while it's off.
    bool has_exposure = false;
    float exposure_ev = 0.0f;
    float exposure_target_ev = 0.0f;
    bool cull_frozen = false; // SceneRenderer::set_cull_frozen: said in the panel, with what shadows can be judged by
    uint32_t visible_shadow_draws = 0;
    uint32_t visible_shadow_triangles = 0;
    // The particle system's (render::ParticleStats): effects in the world, emitters near enough to hold particles,
    // those in view, those the pool had no room for, particles alive (about), and how much of the pool is held.
    uint32_t particle_effects = 0;
    uint32_t particle_emitters = 0;
    uint32_t particle_emitters_drawn = 0;
    uint32_t particle_emitters_starved = 0;
    uint32_t particles = 0;
    uint32_t particle_blocks = 0;
    uint32_t particle_pool_blocks = 0;
    // Local lights (render::LocalLightStats): in the world, lighting what's in view, and left unlit because more
    // than a frame lights with reached it.
    uint32_t lights = 0;
    uint32_t lights_lit = 0;
    uint32_t lights_dropped = 0;
    uint32_t lights_shadowed = 0; // of those lit, how many have shadows, in how many faces
    uint32_t shadow_faces = 0;
    // What rays are traced against (render::TraceStats): meshes with a bottom level, those waiting for one, and
    // the instance slots in the top level. All 0 where nothing is traced.
    uint32_t trace_meshes = 0;
    uint32_t trace_waiting = 0;
    uint32_t trace_instances = 0;
    uint64_t trace_triangles = 0;
    // The path tracer, while the picture is path traced: its samples, and why it last started again.
    uint32_t path_tracer_samples = 0;
    const char* path_tracer_restart = "";
    uint32_t path_tracer_restarts = 0;
    uint32_t path_tracer_bounces = 0;
    // The light boxes (render::LightBoxStats).
    uint32_t light_boxes = 0;
    uint32_t light_box_voxels = 0;
    float light_boxes_baked = 0.0f;
    // The lighting compare (SceneRenderer::set_lighting_compare), while it's on: render::LightingCompareStats.
    uint32_t compare_pixels = 0;
    float compare_mean_stops = 0.0f;
    float compare_over_10 = 0.0f;
    float compare_over_25 = 0.0f;
    float compare_darker_10 = 0.0f;
    float compare_brighter_10 = 0.0f;
};

// The frame stats (M0a): frame time and its history, heap allocations per frame, GPU memory by category, GPU time per
// render graph pass, the render graph, uploads and the device, the sections chosen with set_sections (--stats). Drawn
// over the game's picture, see-through, taking no clicks. With Dear ImGui, between NewFrame() and Render().
class StatsPanel {
public:
    StatsPanel() = default;
    // last_ points into this panel's own copy of the GPU times.
    StatsPanel(const StatsPanel&) = delete;
    StatsPanel& operator=(const StatsPanel&) = delete;
    StatsPanel(StatsPanel&&) = delete;
    StatsPanel& operator=(StatsPanel&&) = delete;

    void record(const FrameStats& stats);
    void set_sections(uint32_t sections) { sections_ = sections; }
    uint32_t sections() const { return sections_; }
    // Over the game's picture, from its top-left corner (x, y in the UI's coordinates), no bigger than it: a window of
    // its own, over everything (a game's window, where nothing else is).
    void draw_overlay(float x, float y, float width, float height);
    // The same, as a child of the window being drawn (the editor's Playground): under anything over that window (a
    // drawer, a menu), clipped to it.
    void draw_in_window(float x, float y, float width, float height);

private:
    float frame_ms_[kFrameHistory] = {};
    size_t next_ = 0;
    size_t count_ = 0;
    uint64_t frames_with_allocations_ = 0;
    size_t late_ = 0; // of the history, counted now and then while drawn: it's read as a slow number
    uint32_t draws_until_late_ = 0;
    uint32_t sections_ = kStatsDefault;

    void draw_contents();
    FrameStats last_; // its gpu_passes point at gpu_passes_
    render::PassGpuTime gpu_passes_[render::kMaxTimedPasses] = {};
};

} // namespace de::tools
