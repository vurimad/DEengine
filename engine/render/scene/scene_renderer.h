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

// The sun and ambient of the placeholder pass (renderer step 1), linear.
struct SceneLighting {
    float sun_direction[3] = {0.35f, 0.8f, 0.3f}; // toward the sun, normalized when used
    float sun_color[3] = {1.6f, 1.45f, 1.2f};
    float ambient_color[3] = {0.18f, 0.21f, 0.27f};
};

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

// Draws a world::RenderFrame (renderer step 1, docs/design.md, Rendering): every instance whose
// mesh is on the GPU, a draw per mesh part, lit by one sun and an ambient term, with a reverse-Z
// depth buffer of its own, looking through the frame's camera (its -Z axis). A placeholder for the
// deferred pipeline: step 2 swaps the pass, not its inputs. Per-frame data (the view and the
// instance transforms) goes through a mapped buffer per frame in flight, grown as needed and
// otherwise reused: no allocation per frame once settled. The device, shader library and
// GpuAssets must outlive it. Move-only.
class SceneRenderer {
public:
    static core::Result<SceneRenderer> create(const SceneRendererDesc& desc);

    ~SceneRenderer();
    SceneRenderer(SceneRenderer&& other) noexcept;
    SceneRenderer& operator=(SceneRenderer&& other) noexcept;
    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    // Adds the pass that clears target to clear and draws frame into it (frame must stay valid
    // until the graph executes). Nothing is drawn without a camera, or while the shaders are
    // missing (the clear still happens).
    void add_pass(RenderGraph& graph, GraphTexture target, rhi::Format target_format, const world::RenderFrame& frame,
                  const SceneLighting& lighting, rhi::Color clear);

    SceneRendererStats stats() const;

private:
    explicit SceneRenderer(SceneRendererState* state) : state_(state) {}

    SceneRendererState* state_ = nullptr;
};

// The view-projection of a camera (3x4 rows, relative to the render origin, looking down its -Z)
// with an infinite reverse-Z perspective: rows of a 4x4, so clip = rows . (p, 1).
void camera_view_proj(const float (&camera)[12], float vertical_fov_rad, float near_m, float aspect, float (&out)[16]);

} // namespace de::render
