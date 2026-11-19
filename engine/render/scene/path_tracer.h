#pragma once

#include "core/array.h"
#include "core/result.h"
#include "render/graph/render_graph.h"
#include "render/rhi/rhi.h"

#include <cstdint>

namespace de::rhi {
class Device;
}
namespace de::shader {
class ShaderLibrary;
}
namespace de::world {
struct RenderFrame;
}

namespace de::render {

struct SceneLighting;

// The path tracer (docs/design.md, Global illumination: the plan from 2026-11-14, step 1): a renderer of its own
// beside the rasterized one (SceneRenderer::set_path_traced), the light of a view by tracing paths with the scene's
// own rays, materials, lights and sky (shaders/scene/path_tracer.slang), a sample a pixel a frame added to the mean of
// those before. It starts again when anything it sees changes: the camera, the picture's size, the sun and sky, the
// local lights, or any instance. Every fast layer of the lighting is measured against it. Only where the renderer
// traces rays (SceneRendererDesc::ray_tracing).

struct PathTracerInput {
    uint64_t frame_address = 0;                // the scene's SceneFrame, with its top level for rays
    GraphTexture hdr;                          // written whole with the mean so far
    const world::RenderFrame* frame = nullptr; // the camera and what changed
    const SceneLighting* lighting = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t light_count = 0; // of the frame's local lights (SceneFrame::lights)
};

// The rasterized light to compare with the path tracer's (add_compare).
struct PathTracerCompareInput {
    GraphTexture raster; // the light leaving each surface, before the fog (the HDR target after the light pass)
    GraphTexture depth;  // the scene's: 0 is the sky, which isn't compared
    uint32_t width = 0;
    uint32_t height = 0;
};

// How far the rasterized light is from the path tracer's, a few frames ago: over the pixels that aren't sky, the mean
// of |log2(rasterized / traced)| (each pixel counted as 2 stops off at most), and the shares off by more than 10% and
// 25%, too dark and too bright by 10%.
struct LightingCompareStats {
    uint32_t pixels = 0;
    float mean_stops = 0.0f;
    float over_10 = 0.0f;
    float over_25 = 0.0f;
    float darker_10 = 0.0f;
    float brighter_10 = 0.0f;
};

struct PathTracerDesc {
    rhi::Device* device = nullptr;
    shader::ShaderLibrary* shaders = nullptr;
    rhi::Format hdr_format = rhi::Format::Unknown; // the HDR target's
};

// It stops adding samples here: enough for a converged picture of anything it's meant for.
inline constexpr uint32_t kPathTracerMaxSamples = 65536;

struct PathTracerState;

// On the thread that renders. The device and shader library must outlive it. Move-only.
class PathTracer {
public:
    static core::Result<PathTracer> create(const PathTracerDesc& desc);

    ~PathTracer();
    PathTracer(PathTracer&& other) noexcept;
    PathTracer& operator=(PathTracer&& other) noexcept;
    PathTracer(const PathTracer&) = delete;
    PathTracer& operator=(const PathTracer&) = delete;

    // Adds "path tracer" (a sample a pixel into the mean) and, with show, "path tracer show" (the mean into the HDR
    // target; without, input.hdr isn't used). False, with nothing added, while a shader of its is missing or with no
    // camera.
    bool add_passes(RenderGraph& graph, const PathTracerInput& input, bool show = true);
    // After add_passes this frame: "lighting compare", the rasterized light against the mean so far. Returns the heat
    // map (RGBA16F, the picture's size: grey agrees, blue too dark, red too bright, full at a stop), to be shown as
    // data; invalid while its shader is missing. Its sums come back a few frames later (compare_stats).
    GraphTexture add_compare(RenderGraph& graph, const PathTracerCompareInput& input);
    LightingCompareStats compare_stats() const;

    // Samples a pixel the mean holds, once the frames that added them have run.
    uint32_t samples() const;
    // Why it last started again ("camera", "sun and sky", "local lights", "size", "instances", "a frame not drawn",
    // "asked"), and how many times it has.
    const char* restart_reason() const;
    uint32_t restarts() const;
    // Starts again on the next frame.
    void reset();
    // Bounces a path takes at most (Russian roulette ends most before).
    void set_max_bounces(uint32_t bounces);
    uint32_t max_bounces() const;
    // The mean (RGBA32F, the picture's size: rgb the mean, a the samples), for tests to read back.
    rhi::TextureHandle accumulation() const;
    // Reads the mean back on the next frame it runs; take_saved() hands it over once the GPU has written it (a few
    // frames later): RGBA floats, top row first, and how many samples it held.
    void request_save();
    bool take_saved(core::Array<float>& rgba, uint32_t& width, uint32_t& height, uint32_t& samples);

private:
    explicit PathTracer(PathTracerState* state) : state_(state) {}

    PathTracerState* state_ = nullptr;
};

} // namespace de::render
