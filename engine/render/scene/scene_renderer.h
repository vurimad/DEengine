#pragma once

#include "core/result.h"
#include "render/graph/render_graph.h"
#include "render/rhi/rhi.h"
#include "render/shader/shader_library.h"

#include <cstdint>

namespace de::world {
struct RenderFrame;
}

namespace de::render {

class GpuAssets;

// The scene's light (renderer step 2), linear. Values are relative to exposure: a sun of 4 with
// an exposure of 1 is a bright day.
struct SceneLighting {
    float sun_direction[3] = {-0.55f, 0.32f, -0.75f}; // toward the sun, normalized when used: low, ahead
    float sun_color[3] = {4.6f, 3.6f, 2.4f};          // late afternoon gold
    float sky_color[3] = {0.30f, 0.40f, 0.62f};       // ambient from above
    float ground_color[3] = {0.12f, 0.10f, 0.07f};    // ambient from below
    float shadow_color[3] = {0.0f, 0.0f, 0.0f};       // what shadowed sunlight keeps (no shadows yet)
    float exposure_ev = 0.0f;
};

// What the window shows: the lit image, or one G-buffer channel.
enum class SceneView : uint32_t {
    Lit,
    BaseColor,
    Normal,
    Roughness,
    Metalness,
    Occlusion,
    Translucency,
    Count,
};

const char* scene_view_name(SceneView view);

struct SceneRendererDesc {
    rhi::Device* device = nullptr;
    shader::ShaderLibrary* shaders = nullptr;
    GpuAssets* gpu_assets = nullptr;
};

struct SceneRendererStats {
    uint32_t instances = 0; // in the frame
    uint32_t drawn = 0;     // with their mesh on the GPU
    uint32_t draws = 0;     // draw calls (one per mesh part)
};

struct SceneRendererState;

// Draws a world::RenderFrame, deferred (renderer step 2, docs/design.md, Rendering):
//   gbuffer         every instance whose mesh is on the GPU, a draw per mesh part: the G-buffer
//                   (base color + lighting group, octahedral normal, roughness/metalness/occlusion/
//                   translucency), emissive into the HDR target, reverse-Z depth
//   deferred light  a full-screen pass adding the sun and the ambient onto the HDR target, and the
//                   sky where nothing was drawn
//   tonemap         exposure and a filmic curve into the target (or a G-buffer channel: SceneView)
// It looks through the frame's camera (down its -Z). Per-frame data (view, light, instance
// transforms) goes through a mapped buffer per frame in flight, grown as needed: no allocation
// per frame once settled. The device, shader library and GpuAssets must outlive it. Move-only.
class SceneRenderer {
public:
    static core::Result<SceneRenderer> create(const SceneRendererDesc& desc);

    ~SceneRenderer();
    SceneRenderer(SceneRenderer&& other) noexcept;
    SceneRenderer& operator=(SceneRenderer&& other) noexcept;
    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    // Adds the passes that draw frame into target (frame must stay valid until the graph
    // executes). Without a camera, or while the shaders are missing, the target is cleared.
    void add_passes(RenderGraph& graph, GraphTexture target, rhi::Format target_format, const world::RenderFrame& frame,
                    const SceneLighting& lighting, SceneView view = SceneView::Lit);

    SceneRendererStats stats() const;

private:
    explicit SceneRenderer(SceneRendererState* state) : state_(state) {}

    SceneRendererState* state_ = nullptr;
};

// The view-projection of a camera (3x4 rows, relative to the render origin, looking down its -Z)
// with an infinite reverse-Z perspective: rows of a 4x4, so clip = rows . (p, 1).
void camera_view_proj(const float (&camera)[12], float vertical_fov_rad, float near_m, float aspect, float (&out)[16]);

// The inverse of a 4x4 (rows); false (out untouched) if it's singular.
bool invert_4x4(const float (&m)[16], float (&out)[16]);

} // namespace de::render
