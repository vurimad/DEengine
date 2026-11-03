#pragma once

#include "asset/environment_format.h"
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

// The number of sun shadow cascades: tiles of a 2 x 2 atlas (shaders/scene/scene_common.slang's
// kCascadeCount mirrors it).
inline constexpr uint32_t kShadowCascadeCount = 4;

// The sun's shadows: cascaded shadow maps in one atlas, resolved per pixel into
// a screen-space mask.
struct SceneShadows {
    uint32_t resolution = 2048;       // per cascade (asset::ShadowLimits); the atlas is 2 x 2 of them
    float distance_m = 140.0f;        // along the view; shadows fade out over the last cascade's band
    float split_lambda = 0.75f;       // 0: even cascade splits, 1: logarithmic
    float blend_band = 0.12f;         // of each cascade, dithered into the next
    float caster_reach_m = 200.0f;    // how far toward the sun casters are caught
    float penumbra_per_m = 0.06f;     // penumbra width per meter between occluder and receiver
    float max_penumbra_texels = 6.0f; // the softest a shadow gets, in shadow texels (at least 1)
};

// Fog (shaders/common/fog.slang has the model): a constant extinction plus a height fog around a
// base height, coloured per channel, lit by the sun (a colour looking toward it, another looking
// away) and an ambient colour, from start_m to end_m along each view ray (the sky is at end_m).
// Near the camera, a froxel grid out to volume_distance_m (no farther than the sun's shadows reach)
// adds those shadows to it (light shafts); past the grid, or without one (0), the fog goes on in
// closed form. The defaults are no fog.
struct SceneFog {
    float density_per_m = 0.0f;        // extinction everywhere
    float height_density_per_m = 0.0f; // the height fog's extinction at its base
    float height_falloff_per_m = 0.05f;
    double base_height_m = 0.0; // world space
    float extinction_tint[3] = {1.0f, 1.0f, 1.0f};
    float sun_forward[3] = {}; // in-scatter looking toward the sun, times its colour
    float sun_back[3] = {};    // looking away from it
    float sun_height_attenuation = 1.0f;
    float ambient[3] = {}; // isotropic in-scatter
    float ambient_height_attenuation = 1.0f;
    float start_m = 0.0f;
    float sun_start_m = 0.0f; // where sun haze starts: none on the camera's nose
    float end_m = 20000.0f;
    float volume_distance_m = 0.0f; // the froxel grid's reach, in view depth; 0: no grid
};

// The scene's light, linear: what an environment asset gives (scene_lighting()).
// Values are relative to exposure: a sun of 4 with an exposure of 1 is a bright day. The defaults
// are a late afternoon, for tests and tools without an environment.
struct SceneLighting {
    float sun_direction[3] = {-0.55f, 0.32f, -0.75f}; // toward the sun, normalized when used: low, ahead
    float sun_color[3] = {4.6f, 3.6f, 2.4f};          // late afternoon gold
    float sky_color[3] = {0.30f, 0.40f, 0.62f};       // ambient from above
    float ground_color[3] = {0.12f, 0.10f, 0.07f};    // ambient from below
    float shadow_color[3] = {0.05f, 0.06f, 0.09f};    // what shadowed sunlight keeps: a cool tint, not black
    float exposure_ev = 0.0f;
    SceneShadows shadows{}; // the sun's
    SceneFog fog{};
};

// An environment asset (.deenv) as the renderer's lighting.
SceneLighting scene_lighting(const asset::EnvironmentAsset& environment);

// What a frame shows with no scene (and the window without shaders): a dusk blue.
inline constexpr rhi::Color kEmptySceneColor{.r = 0.10f, .g = 0.11f, .b = 0.15f, .a = 1.0f};

// What the window shows: the lit image, or one G-buffer channel.
enum class SceneView : uint32_t {
    Lit,
    BaseColor,
    Normal,
    Roughness,
    Metalness,
    Occlusion,
    Translucency,
    ShadowMask, // sunlight seen
    Cascades,   // which shadow cascade each pixel uses (red, green, blue, yellow)
    Count,
};

const char* scene_view_name(SceneView view);

struct SceneRendererDesc {
    rhi::Device* device = nullptr;
    shader::ShaderLibrary* shaders = nullptr;
    GpuAssets* gpu_assets = nullptr;
};

struct SceneRendererStats {
    uint32_t instances = 0;      // in the frame
    uint32_t drawn = 0;          // with their mesh on the GPU
    uint32_t draws = 0;          // draw calls (one per mesh part)
    uint32_t shadow_draws = 0;   // into the shadow cascades
    float froxel_reach_m = 0.0f; // the froxel grid's reach (no farther than the shadows); 0: no grid
};

// A cascade of the sun's shadow map: where it reaches along the view, and its projection.
struct ShadowCascade {
    float far_m = 0.0f;       // its far edge, as distance along the view
    float texel_m = 0.0f;     // a shadow texel's size
    float depth_per_m = 0.0f; // depth units per meter toward the sun
    float view_proj[16] = {}; // rows: render-origin space to the cascade's clip space
};

// Fits the cascades to a camera (3x4 rows, looking down -Z) and the sun (toward it; straight up if
// it's zero):
// splits between near_m and the shadow distance, a bounding sphere per slice (its size doesn't
// change as the camera turns), centered on a texel grid (it doesn't swim as the camera moves).
void fit_shadow_cascades(const float (&camera)[12], float vertical_fov_rad, float near_m, float aspect,
                         const float (&sun_direction)[3], const SceneShadows& shadows,
                         ShadowCascade (&out)[kShadowCascadeCount]);

struct SceneRendererState;

// Draws a world::RenderFrame, deferred (docs/design.md, Rendering: frame passes):
//   sun shadows     every instance into each cascade's tile of the shadow atlas, depth only
//   gbuffer         every instance whose mesh is on the GPU, a draw per mesh part: the G-buffer
//                   (base color + lighting group, octahedral normal, roughness/metalness/occlusion/
//                   translucency), emissive into the HDR target, reverse-Z depth
//   shadow mask     a full-screen pass resolving the cascades: how much sun each pixel sees
//   deferred light  a full-screen pass adding the sun (through the mask) and the ambient onto the
//                   HDR target, and the sky where nothing was drawn
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
    // executes). Without a camera, or while the shaders are missing, the target is cleared to
    // empty.
    void add_passes(RenderGraph& graph, GraphTexture target, rhi::Format target_format, const world::RenderFrame& frame,
                    const SceneLighting& lighting, SceneView view = SceneView::Lit,
                    rhi::Color empty = kEmptySceneColor);

    SceneRendererStats stats() const;

private:
    explicit SceneRenderer(SceneRendererState* state) : state_(state) {}

    SceneRendererState* state_ = nullptr;
};

// The view-projection of a camera (3x4 rows, relative to the render origin, looking down its -Z)
// with an infinite reverse-Z perspective: rows of a 4x4, so clip = rows . (p, 1).
void camera_view_proj(const float (&camera)[12], float vertical_fov_rad, float near_m, float aspect, float (&out)[16]);

// The inverse of a 4x4 (rows), through glm in doubles; false (out untouched) if it's singular.
bool invert_4x4(const float (&m)[16], float (&out)[16]);

} // namespace de::render
