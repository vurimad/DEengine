#pragma once

#include "core/math.h"
#include "core/result.h"
#include "render/graph/render_graph.h"
#include "render/rhi/rhi.h"

#include <cstdint>
#include <span>

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

// Local lights (docs/design.md, Local lights): the frame's point lights and spots, on the GPU, and which of them
// reach each part of the view. The CPU works per light (which reach what's in view, the nearest first when there are
// too many); the GPU per cluster, in a compute pass (shaders/scene/light_cull.slang), and per pixel where something
// is lit (shaders/scene/local_lights.slang).

// The view's clusters: a grid across the picture whatever its size, and slices in depth, each thicker than the last
// by one ratio from kLightSliceNearM (everything nearer is the first slice) to kLightSliceFarM (everything farther
// the last).
inline constexpr uint32_t kLightGridX = 32;
inline constexpr uint32_t kLightGridY = 18;
inline constexpr uint32_t kLightGridZ = 32;
inline constexpr uint32_t kLightClusters = kLightGridX * kLightGridY * kLightGridZ;
inline constexpr float kLightSliceNearM = 0.5f;
inline constexpr float kLightSliceFarM = 512.0f;
// The most lights a frame lights with: those that reach what's in view, the nearest for their size first. A bit each
// in a cluster's words.
inline constexpr uint32_t kMaxFrameLights = 256;
inline constexpr uint32_t kLightWordBits = 32;
// A light is lit from this many of its radius away at most, fading over the last fifth of that: a candle that
// reaches 3 m shows from 300 m, a fire that reaches 20 m from 2 km.
inline constexpr float kLightDrawDistancePerRadius = 100.0f;

// A light as the GPU keeps it (LocalLight in shaders/scene/scene_common.slang).
struct GpuLocalLight {
    float position_radius[4] = {};            // xyz: relative to the render origin; w: where it ends, in meters
    float color_cone[4] = {0, 0, 0, 1};       // rgb: times its intensity; w: a spot's fade, 1 over its width in cosine
    float direction_cone[4] = {0, 0, -1, -2}; // xyz: a spot's axis; w: the cosine of its cone (below any: a point)
    // Its first face in the local shadow atlas (below 0: none); faces; tan(half fov); how much its shadow shows,
    // 0 to 1 (it fades in and out).
    float shadow[4] = {-1, 0, 0, 0};
};
static_assert(sizeof(GpuLocalLight) == 64);

// Local lights' shadows (docs/design.md, Local lights: shadows): the lights whose reach covers most of the picture get
// faces in the local shadow atlas, a point light six (a cube), a spot one, until the faces or the atlas run out; the
// rest light without shadows. A face is a tile of 128 texels or more, the power of 2 nearest above its reach's share
// of the picture's height times kLocalShadowTexelsPerCoverage, up to 256 for a point light's and 512 for a spot's
// (a shadow's edge is soft by the light's size, in meters, whatever the tile's: shaders/scene/local_lights.slang),
// and a shadow view of the GPU cull. There's room for 32 point lights at their largest, so which light has a shadow
// and how large doesn't turn on what the others want until a view holds more than that. Steady: a light keeps its
// tiles' size until another has been wanted for kLocalShadowResizeAfterS, one with a shadow is kept over one that
// covers a little more, and a shadow fades out and in (at kLocalShadowFadePerS) as its light grows small on the
// picture, never when it comes into view.
inline constexpr uint32_t kLocalShadowFaces = 192;
// The atlas, in cells of kLocalShadowCell texels: a face takes 1, 2 x 2 or 4 x 4 of them, packed largest first, each on
// a multiple of its own size, so the cells never split up.
inline constexpr uint32_t kLocalShadowCell = 128;
inline constexpr uint32_t kLocalShadowCellsX = 32;
inline constexpr uint32_t kLocalShadowCellsY = 24;
inline constexpr uint32_t kLocalShadowAtlasWidth = kLocalShadowCell * kLocalShadowCellsX;
inline constexpr uint32_t kLocalShadowAtlasHeight = kLocalShadowCell * kLocalShadowCellsY;
inline constexpr uint32_t kLocalShadowLargest = 4 * kLocalShadowCell;     // a spot's one face
inline constexpr uint32_t kLocalShadowLargestCube = 2 * kLocalShadowCell; // each of a point light's six
// A face's near plane: nothing nearer the light than this casts (kLocalShadowNearM in
// shaders/scene/local_lights.slang).
inline constexpr float kLocalShadowNearM = 0.05f;
// A light whose reach is as tall as the picture wants tiles of this many texels.
inline constexpr float kLocalShadowTexelsPerCoverage = 512.0f;
// How long another size must be wanted before a light's tiles change to it.
inline constexpr float kLocalShadowResizeAfterS = 0.5f;
// A shadow fades out as its light wants fewer than twice this many texels, gone at this many.
inline constexpr float kLocalShadowFadeTexels = 32.0f;
// How fast a shadow fades in or out: all of it in 1 / this seconds.
inline constexpr float kLocalShadowFadePerS = 2.0f;

// A face of the local shadow atlas, this frame.
struct LocalShadowFace {
    float view_proj[16] = {}; // render-origin space to its clip space (rows; reverse Z, no far plane)
    float position[3] = {};   // the light's
    float forward[3] = {};    // where the face looks
    float far_m = 0.0f;       // the light's radius: nothing farther is lit by it
    uint32_t tile[3] = {};    // its tile in the atlas: x, y, size, in texels
    // Whether its tile is drawn again this frame. A light's tiles keep their places in the atlas, which is kept from
    // frame to frame, and what they hold stays right while nothing in the light's reach changes: then they're not
    // culled for or drawn, only read.
    bool draw = true;
};

// A place where what casts shadows changed since the last frame (a thing moved, came or went): a sphere relative to
// the frame's render origin. A light whose reach it touches draws its tiles again.
struct LocalShadowChange {
    float center[3] = {};
    float radius_m = 0.0f;
};

// What the frame's constants say of the lights (SceneFrame's lights, light_clusters, light_grid, light_slices).
struct LocalLightFrame {
    LocalShadowFace faces[kLocalShadowFaces] = {};
    uint32_t face_count = 0; // faces used this frame, from the first
    uint64_t lights = 0;     // this frame's GpuLocalLights
    uint64_t clusters = 0;   // grid[3] words a cluster
    uint32_t grid[4] = {};   // clusters across, up and deep; words a cluster: 0 with no lights, and nothing is lit
    float slices[4] = {};    // a depth's slice is log2(depth) * [0] + [1]
};

struct LocalLightStats {
    uint32_t lights = 0;   // in the frame
    uint32_t lit = 0;      // of those, reaching what's in view and near enough: sent to the GPU
    uint32_t dropped = 0;  // more than kMaxFrameLights did: the farthest for their size weren't lit
    uint32_t shadowed = 0; // of those lit, how many have shadows
    uint32_t shadow_faces = 0;
    uint32_t shadow_faces_drawn = 0; // of those, drawn again this frame: the rest hold what they held
};

// Which slice of the grid a depth along the view is in.
uint32_t light_slice(float depth_m);

struct LocalLightsDesc {
    rhi::Device* device = nullptr;
    shader::ShaderLibrary* shaders = nullptr;
};

struct LocalLightsState;

// On the thread that renders. The device and shader library must outlive it. Move-only.
class LocalLights {
public:
    static core::Result<LocalLights> create(const LocalLightsDesc& desc);

    ~LocalLights();
    LocalLights(LocalLights&& other) noexcept;
    LocalLights& operator=(LocalLights&& other) noexcept;
    LocalLights(const LocalLights&) = delete;
    LocalLights& operator=(const LocalLights&) = delete;

    // Takes the frame's lights: writes those that light something in view for the GPU, and says what the frame's
    // constants hold of them. Before the frame's constants are written. Nothing is lit while its shader is missing.
    // `changed`: where casters changed since the last update(); `all_changed`: anywhere (the default: every tile is
    // drawn every frame, as before tiles were kept).
    LocalLightFrame update(const world::RenderFrame& frame, float aspect,
                           std::span<const LocalShadowChange> changed = {}, bool all_changed = true);
    // Adds "light cull", which fills the clusters' words, if the last update() kept any light: before any pass that
    // lights. frame_address: the scene's SceneFrame on the GPU, written with what update() returned. Returns the
    // clusters' buffer, which every pass that lights reads (fragment or compute); invalid: no lights this frame.
    GraphBuffer add_cull_pass(RenderGraph& graph, uint64_t frame_address, const world::RenderFrame& frame,
                              float aspect);

    // The most lights that have shadows in a frame (the most covering first); the rest light without. Up to what
    // the faces and the atlas hold, which is the default.
    void set_most_shadowed(uint32_t lights);

    LocalLightStats stats() const;
    // The clusters' words, for tests to read back: kLightClusters times the frame's words a cluster.
    rhi::BufferHandle clusters() const;

private:
    explicit LocalLights(LocalLightsState* state) : state_(state) {}

    LocalLightsState* state_ = nullptr;
};

} // namespace de::render
