#include "render/scene/scene_renderer.h"

#include "core/allocator.h"
#include "core/log.h"
#include "core/math.h"
#include "core/profile.h"
#include "platform/time.h"
#include "render/rhi/device.h"
#include "render/rhi/upload.h"
#include "render/scene/gpu_assets.h"
#include "render/scene/ocean.h"
#include "render/scene/shore_sim.h"
#include "world/render_extract.h"

#include <glm/gtc/matrix_access.hpp>
#include <glm/mat4x4.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <optional>
#include <utility>

namespace de::render {

namespace {

core::Allocator& render_heap() {
    return core::heap_allocator(core::MemoryTag::Render);
}

// Matches SceneFrame in shaders/scene/scene_common.slang.
struct SceneFrame {
    float view_proj[16] = {};
    float inv_view_proj[16] = {};
    float camera_position[4] = {};
    float sun_direction[4] = {};
    float sun_color[4] = {};
    float sky_color[4] = {};
    float ground_color[4] = {};
    float shadow_color[4] = {};
    float cascade_view_proj[kShadowCascadeCount * 16] = {};
    float cascade_far[kShadowCascadeCount] = {};
    float cascade_texel_m[kShadowCascadeCount] = {};
    float cascade_depth_per_m[kShadowCascadeCount] = {};
    float camera_forward[4] = {};
    float shadow_params[4] = {}; // blend band, atlas texel, penumbra per meter, max penumbra (texels)
    // FogMedium in shaders/common/fog.slang.
    float fog_density[4] = {};            // constant, height base, falloff, base height (render-origin space)
    float fog_extinction[4] = {};         // tint, ambient height attenuation
    float fog_sun_forward[4] = {};        // colour, sun height attenuation
    float fog_sun_back[4] = {};           // colour, sun start
    float fog_ambient[4] = {};            // colour, start
    float fog_range[4] = {};              // end, froxel grid reach, slice offset, slices
    float previous_view_proj[16] = {};    // last frame's camera, relative to this frame's render origin
    float unjittered_view_proj[16] = {};  // view_proj without TAA's sub-pixel offset
    float taa_params[4] = {};             // last frame's near, TAA's sample (0 without TAA), its offset in NDC
    uint32_t render_origin_steps[4] = {}; // world::render_position_steps of the origin, per axis
    // The wind (shaders/scene/wind.slang).
    float wind[4] = {};        // toward where it blows (x, z, unit), its speed, what a gust adds (m/s)
    float wind_field[4] = {};  // a gust's size (m), turbulence, the time and last frame's, wrapped to kWindPeriodS
    float wind_offset[4] = {}; // the gust field's offset from the render origin (m, x, z), and last frame's
    // The terrain's light map (shaders/scene/terrain_light.slang): the material table, the map's slot in it and 1 if
    // there is one, and where the terrain lies, in render steps.
    uint64_t materials = 0;
    uint32_t terrain_light[2] = {};
    uint32_t terrain_origin_steps[2] = {};
    uint32_t terrain_size_steps[2] = {};
    float sea[2] = {}; // the sea's level, relative to the render origin, and 1 if there is one
    // The surf's simulation (shore_sim.h), 1 in shore_sim if it runs this frame: its window's corner relative to the
    // render origin and in cells of the world, and its surface this frame and last (bindless indices).
    float shore_corner[2] = {};
    int32_t shore_corner_cell[2] = {};
    uint32_t shore_surface = 0;
    uint32_t shore_previous_surface = 0;
    uint32_t shore_sim = 0;
    uint32_t shore_padding = 0;
    // A plain backdrop instead of the sky (SceneBackdrop): its colours, and 1 in the top's w if there is one.
    float backdrop_top[4] = {};
    float backdrop_bottom[4] = {};
    // Local lights (local_lights.h, LocalLightFrame): this frame's, the clusters' words, the grid and its slices.
    uint64_t lights = 0;
    uint64_t light_clusters = 0;
    uint32_t light_grid[4] = {};
    float light_slices[4] = {};
    uint64_t trace = 0;              // trace_scene.h: this frame's GpuTraceScene; 0 where nothing is traced
    uint64_t local_shadow_views = 0; // kLocalShadowViewsOffset in the frame's buffer
    uint64_t light_boxes = 0;        // light_boxes.h: this frame's GpuLightBoxes
    uint32_t light_box_count = 0;
    uint32_t sky_volume_count = 0; // the first this many of fog_volumes say how open to the sky their place is
    uint64_t fog_volumes = 0;      // kFogVolumesOffset in the frame's buffer
};
static_assert(sizeof(SceneFrame) == 1056);
static_assert(offsetof(SceneFrame, local_shadow_views) == 1024);

// Matches TraceViewPush in shaders/scene/trace_view.slang.
struct TraceViewPush {
    uint64_t frame = 0;
};

// Wind rhythms repeat within this, so its time can wrap without a jump; shaders/scene/wind.slang's kWindPeriodS.
constexpr double kWindPeriodS = 256.0;
// The gust field repeats every this many patches; wind.slang's kWindCells.
constexpr double kWindCells = 64.0;

// A value wrapped into [0, period), as a float.
float wrapped(double value, double period) {
    const double inside = value - period * std::floor(value / period);
    return inside < period ? static_cast<float>(inside) : 0.0f; // it can round up to the period
}

// The wind into the frame: its times wrapped to kWindPeriodS, and where the gust field lies (it travels with the wind)
// as an offset from the render origin, wrapped to the field's size. In double, however long the world has run and
// wherever the origin is. Lighting made in code is held to the asset's limits here.
void write_wind(const SceneWind& wind, const world::RenderFrame& frame, SceneFrame& scene) {
    using W = asset::WindLimits;
    const double length = std::hypot(static_cast<double>(wind.direction[0]), static_cast<double>(wind.direction[1]));
    const double dx = length > 1e-6 ? wind.direction[0] / length : 1.0;
    const double dz = length > 1e-6 ? wind.direction[1] / length : 0.0;
    const float speed = std::clamp(wind.speed_mps, 0.0f, W::kMaxSpeedMps);
    const float gust = std::clamp(wind.gust_mps, 0.0f, W::kMaxSpeedMps);
    const double size = std::clamp(static_cast<double>(wind.gust_size_m), static_cast<double>(W::kMinGustSizeM),
                                   static_cast<double>(W::kMaxGustSizeM));
    scene.wind[0] = static_cast<float>(dx);
    scene.wind[1] = static_cast<float>(dz);
    scene.wind[2] = speed;
    scene.wind[3] = gust;
    scene.wind_field[0] = static_cast<float>(size);
    scene.wind_field[1] = std::clamp(wind.turbulence, 0.0f, 1.0f);
    scene.wind_field[2] = wrapped(frame.time_s, kWindPeriodS);
    scene.wind_field[3] = wrapped(frame.previous_time_s, kWindPeriodS);
    // Gusts ride along about as fast as the air between them.
    const double travel = static_cast<double>(speed) + 0.5 * static_cast<double>(gust);
    const double period_m = size * kWindCells;
    scene.wind_offset[0] = wrapped(frame.origin.x - dx * travel * frame.time_s, period_m);
    scene.wind_offset[1] = wrapped(frame.origin.z - dz * travel * frame.time_s, period_m);
    scene.wind_offset[2] = wrapped(frame.origin.x - dx * travel * frame.previous_time_s, period_m);
    scene.wind_offset[3] = wrapped(frame.origin.z - dz * travel * frame.previous_time_s, period_m);
}

// Matches GBufferPush in shaders/scene/gbuffer.slang.
struct GBufferPush {
    uint64_t frame = 0;
    uint64_t instances = 0;
    uint64_t vertices = 0;
    uint64_t materials = 0;
    uint64_t draws = 0;
    uint32_t sampler = 0;
    uint32_t depth = 0; // the scene's depth: the editor's selection mask tests against it
    uint32_t unjittered =
        0; // 1: through the view without TAA's jitter (the selection mask: an outline that keeps still)
};

#if DE_BUILD_DEV
// Matches DebugVertex and DebugLinesPush in shaders/scene/debug_lines.slang.
struct GpuDebugVertex {
    float position[3] = {};
    uint32_t color = 0;
};
static_assert(sizeof(GpuDebugVertex) == 16);
struct DebugLinesPush {
    uint64_t frame = 0;
    uint64_t vertices = 0;
    float alpha = 1.0f;
    uint32_t srgb_target = 0;
};

// Matches TerrainSelectionPush in shaders/scene/terrain_selection.slang.
struct TerrainSelectionPush {
    uint32_t ids = 0;
    uint32_t mask = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// Matches OutlinePush in shaders/scene/outline.slang.
struct OutlinePush {
    uint32_t mask = 0;
    float radius_px = 2.0f;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t srgb_target = 0;
};

// The editor's instance IDs: an R32 target beside the G-buffer's, the instance's slot plus one (0: none).
inline constexpr rhi::Format kIdFormat = rhi::Format::R32Uint;
// The selection's mask: covered, and seen (nothing nearer was drawn there).
inline constexpr rhi::Format kSelectionMaskFormat = rhi::Format::RG8Unorm;
inline constexpr uint32_t kMaxSelectionDraws = 1024;
#endif

// Matches ShadowCasterPush in shaders/scene/shadow_caster.slang.
struct ShadowCasterPush {
    uint64_t frame = 0;
    uint64_t instances = 0;
    uint64_t vertices = 0;
    uint64_t materials = 0;
    uint64_t draws = 0;
    uint32_t sampler = 0;
    uint32_t cascade = 0;
};

// Matches GpuDraw in shaders/scene/scene_common.slang: found through a draw's first instance.
struct GpuDraw {
    uint32_t instance = 0;
    uint32_t material = 0;
    float fade = 0.0f;
};
static_assert(sizeof(GpuDraw) == 12);

// Matches ShadowMaskPush in shaders/scene/shadow_mask.slang.
struct ShadowMaskPush {
    uint64_t frame = 0;
    uint32_t depth = 0;
    uint32_t gb1 = 0;
    uint32_t atlas = 0;
    uint32_t sampler = 0;
};

// Matches RcApplyPush in shaders/scene/rc_apply.slang.
struct RcApplyPush {
    uint64_t frame = 0;
    uint32_t gb0 = 0;
    uint32_t gb1 = 0;
    uint32_t gb2 = 0;
    uint32_t scene_depth = 0;
    uint32_t depth = 0;
    uint32_t normal = 0;
    uint32_t light = 0;
    uint32_t scale = 1;
    uint32_t size[2] = {};
    uint32_t gi_size[2] = {};
    float tolerance = 0.0f;
    uint32_t flags = 0; // kRcApply...
    float reserved_sky = 0.0f;
    uint32_t reserved = 0;
};
static_assert(sizeof(RcApplyPush) == 72);
constexpr uint32_t kRcApplyMaterialOcclusion = 1; // times the material's own occlusion
constexpr uint32_t kRcApplyIndirect = 2;          // only the light gathered (SceneView::Indirect)
constexpr uint32_t kRcApplySpecular = 4;          // what surfaces mirror too (set_specular_ambient)

// Matches DeferredLightPush in shaders/scene/deferred_light.slang.
struct DeferredLightPush {
    uint64_t frame = 0;
    uint32_t gb0 = 0;
    uint32_t gb1 = 0;
    uint32_t gb2 = 0;
    uint32_t depth = 0;
    uint32_t shadow_mask = 0;
    uint32_t indirect = 0; // 1: only the ambient that falls on each surface (SceneView::Indirect)
    uint32_t ao = 0;       // gtao.h's occlusion, if ao_on
    uint32_t ao_on = 0;
    uint32_t local_shadows = 0; // the local shadow atlas; 0: none
    uint32_t ambient_off = 0;   // bit 0: no diffuse ambient; bit 1: no specular ambient. Both while the radiance
                                // cascades add them (radiance_cascades.h); bit 1 alone: set_specular_ambient
};
static_assert(sizeof(DeferredLightPush) == 48);

// Matches FogPush in shaders/scene/fog.slang.
struct FogPush {
    uint64_t frame = 0;
    uint32_t hdr = 0;
    uint32_t depth = 0;
    uint32_t integrated = 0;
    uint32_t sampler = 0;
    uint64_t fog_volumes = 0;
    uint64_t fog_slice_masks = 0;
    uint32_t fog_volume_count = 0;
    uint32_t padding = 0;
};

// Matches FogVolumeData in shaders/common/fog.slang.
struct GpuFogVolume {
    float to_local[12] = {};
    float half_extents[4] = {}; // w: edge, in meters
    float tint_density[4] = {}; // w: extinction per meter
    float shape[4] = {};        // x: 1 for an ellipsoid; y: how much noise eats away; z: its height fade, in meters;
                                // w: how open to the sky its place is, negative if it doesn't say
    float noise[4] = {}; // xyz: how far the noise has moved past the render origin, in tiles, wrapped to [0, 4); w: its
                         // tiles a meter
};
static_assert(sizeof(GpuFogVolume) == 112);

// Matches FogInjectPush in shaders/scene/fog_inject.slang.
struct FogInjectPush {
    uint64_t frame = 0;
    uint64_t fog_volumes = 0;
    uint64_t fog_slice_masks = 0;
    uint32_t volume = 0;
    uint32_t atlas = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t history = 0;
    uint32_t sampler = 0;
    float jitter_x = 0.0f;
    float jitter_y = 0.0f;
    float jitter_z = 0.0f;
    uint32_t fog_volume_count = 0;
    uint32_t noise = 0;
};

// Matches FogIntegratePush in shaders/scene/fog_integrate.slang.
struct FogIntegratePush {
    uint64_t frame = 0;
    uint32_t froxels = 0;
    uint32_t integrated = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// The froxel grid: a column per this many pixels, and slices spaced exponentially from the camera with
// this offset, so the first ones aren't vanishingly thin.
// A froxel's column on the picture: 8 pixels up to 1080p, wider on a larger picture, so the grid is about 240 x 135
// columns whatever the picture's size and the fog costs the same (fog is soft: a column of 11 pixels at 1440p shows
// what one of 8 does, and at 8 the grid there had as many froxels as the picture has pixels).
constexpr uint32_t froxel_tile_pixels(uint32_t picture_height) {
    return std::clamp((picture_height + 67U) / 135U, 8U, 16U);
}
constexpr uint32_t kFroxelSlices = 64;
constexpr uint32_t kFroxelJitterSamples = 16; // the places a froxel's samples go through, over and over
constexpr uint32_t kFroxelIdleFrames = 240;   // how long the kept textures wait for fog volumes to come back in view
constexpr float kFroxelSliceOffsetM = 2.0f;
constexpr rhi::Format kFroxelFormat = rhi::Format::RGBA16Float;

// Matches TaaPush in shaders/scene/taa.slang.
struct TaaPush {
    uint64_t frame = 0;
    uint32_t current = 0;
    uint32_t history = 0;
    uint32_t depth = 0;
    uint32_t previous_depth = 0;
    uint32_t motion = 0;
    uint32_t previous_object_motion = 0;
    uint32_t sampler = 0;
    uint32_t has_history = 0;
    float exposure = 1.0f;
    uint32_t coverage = 0;
    uint32_t has_coverage = 0;
    uint32_t reserved = 0;
    uint64_t adapted = 0; // auto_exposure.h's multiplier, times exposure; 0: exposure alone
};
static_assert(sizeof(TaaPush) == 64);

// TAA's sub-pixel offsets repeat after this many frames.
constexpr uint32_t kTaaSamples = 16;

// The radical inverse of index in a base: a Halton sequence's value, in (0, 1).
float halton(uint32_t index, uint32_t base) {
    float value = 0.0f;
    float fraction = 1.0f;
    for (; index > 0; index /= base) {
        fraction /= static_cast<float>(base);
        value += fraction * static_cast<float>(index % base);
    }
    return value;
}

// Matches TonemapPush in shaders/scene/tonemap.slang.
struct TonemapPush {
    uint32_t hdr = 0;
    uint32_t gb0 = 0;
    uint32_t gb1 = 0;
    uint32_t gb2 = 0;
    float exposure = 1.0f;
    uint32_t view = 0;
    uint32_t srgb_target = 0;
    uint32_t shadow_mask = 0;
    uint32_t motion = 0;
    uint32_t ao = 0; // gtao.h's occlusion, if ao_on
    uint32_t ao_on = 0;
    uint32_t reserved = 0;
    uint64_t adapted = 0; // auto_exposure.h's multiplier, times exposure; 0: exposure alone
};
static_assert(sizeof(TonemapPush) == 56);

// shaders/scene/sky.slang's kLeastSkyShare: the fog's ambient is written times the world's openness to the sky, but
// never less than this of it, so a volume more open than its world can still be given its share.
constexpr float kLeastSkyShare = 1.0f / 1024.0f;
constexpr uint64_t kFogVolumesOffset = 1280; // after the SceneFrame
static_assert(sizeof(SceneFrame) <= kFogVolumesOffset);
// The nearest this many in reach go into the froxel grid each frame.
constexpr uint32_t kMaxFogVolumes = 64;
static_assert(kMaxFogVolumes <= 64, "a slice's mask has a bit per volume");
// Per slice of the grid, which volumes reach it: a froxel tests only those.
constexpr uint64_t kFogSliceMasksOffset = kFogVolumesOffset + kMaxFogVolumes * sizeof(GpuFogVolume);
constexpr uint64_t kInstanceBytes = 96; // a GpuInstance: its transform, and last frame's

// The G-buffer's formats (shaders/scene/scene_common.slang documents the channels).
constexpr rhi::Format kGBufferFormats[3] = {rhi::Format::RGBA8Srgb, rhi::Format::RG16Unorm, rhi::Format::RGBA8Unorm};
// The graph's textures by name, for its logs and the frame debugger.
constexpr const char* kGBufferNames[3] = {"gbuffer base color", "gbuffer normal", "gbuffer material"};
constexpr rhi::Format kHdrFormat = rhi::Format::RGBA16Float;
constexpr rhi::Format kMotionFormat = rhi::Format::RG16Float;
constexpr rhi::Format kDepthFormat = rhi::Format::D32Float;
constexpr rhi::Format kShadowMaskFormat = rhi::Format::RG8Unorm;
// How much of each pixel see-through things cover, added up as they're drawn.
constexpr rhi::Format kCoverageFormat = rhi::Format::R8Unorm;
constexpr rhi::Format kWaterBehindDepthFormat = rhi::Format::R32Float;

// Matches WaterBehindPush in shaders/scene/water_behind.slang.
struct WaterBehindPush {
    uint32_t hdr = 0;
    uint32_t depth = 0;
};

// Matches WaterPush in shaders/scene/water.slang.
struct WaterPush {
    uint64_t frame = 0;
    uint64_t levels = 0;
    uint32_t behind_color = 0;
    uint32_t behind_depth = 0;
    float sea_level = 0.0f; // relative to the render origin
    uint32_t atlas = 0;     // the sun's shadow atlas
    uint32_t waves = 0;
    uint32_t previous_waves = 0;
    uint32_t slopes = 0;
    uint32_t shore_material = 0; // the shore map's material slot, if flags has kWaterShoreFlag
    float tile_offset[kOceanCascades][2] = {};
    uint32_t flags = 0;            // kWaterShoreFlag, kWaterBandsFlag, kWaterFilmFlag
    float surf_period_s = 0.0f;    // the surf's sets: the swell's period
    float surf_height_m = 0.0f;    // their height, where they start to feel the bottom
    uint32_t foam_material = 0;    // the foam's material slot; 0: foam drawn from noise
    float swell_direction[2] = {}; // toward where the swell travels (x, z): its crests lie across it
    // A lake's or pond's outline (shape not kWaterSea): its middle (x, z, relative to the render origin) and half
    // extents along its own axes, its own x in the world's, and how much of the sea's waves it has.
    float bounds[4] = {};
    float axis[2] = {1.0f, 0.0f};
    uint32_t shape = 0;
    float wave_scale = 1.0f;
};
static_assert(sizeof(WaterPush) == 128);

// WaterPush::flags: a shore map to read; the shore waves' bands drawn in color (SceneWater::show_bands); the surf from
// the shore material's film (SceneWater::surf_film).
constexpr uint32_t kWaterShoreFlag = 1;
constexpr uint32_t kWaterBandsFlag = 2;
constexpr uint32_t kWaterFilmFlag = 4;
constexpr uint32_t kWaterWavesFlag = 8; // the ocean's waves are there to read

// WaterPush::shape.
constexpr uint32_t kWaterSea = 0;
constexpr uint32_t kWaterRectangle = 1;
constexpr uint32_t kWaterEllipse = 2;
// Lakes and ponds drawn a frame, the first the frame lists.
constexpr uint32_t kMaxWaterBodies = 64;

void copy3(const float (&from)[3], float (&to)[4]) {
    to[0] = from[0];
    to[1] = from[1];
    to[2] = from[2];
}

constexpr bool is_masked(uint32_t kind) {
    return (kind & 2U) != 0;
}
constexpr bool is_double_sided(uint32_t kind) {
    return (kind & 1U) != 0;
}
// The shadow views: the sun's cascades, then the local lights' faces (local_lights.h).
constexpr uint32_t kShadowViewCount = kShadowCascadeCount + kLocalShadowFaces;
// The draw lists the GPU cull fills: a list per kind for the camera, again for what's fading between LODs, then opaque
// and masked for each shadow view, whose casters differ only by the alpha test, then what blends, for the camera: a
// draw each, which the transparent pass puts in order and draws.
constexpr uint32_t kCameraListCount = 2 * kDrawKindCount;
constexpr uint32_t kBlendList = kCameraListCount + 2 * kShadowViewCount;
constexpr uint32_t kDrawListCount = kBlendList + 1;
// The most see-through draws a view holds: each is measured against every other to order them (scene.blend_sort).
constexpr uint32_t kMaxBlendDraws = 4096;

constexpr uint32_t cascade_list(uint32_t cascade, bool masked) {
    return kCameraListCount + (2 * cascade) + (masked ? 1U : 0U);
}

// Matches GpuCullView and GpuDrawList in shaders/scene/scene_common.slang.
struct GpuCullView {
    float planes[kShadowCasterPlanes + 1][4] = {}; // for a cascade, how far toward the sun it goes too
    uint32_t plane_count = 0;
    uint32_t lod_perspective = 0;
    uint32_t padding[2] = {};
    float depth_plane[4] = {};
    float lod_scale = 0.0f;
    float lod_fade_end = 1.0f;
    float min_radius_m = 0.0f;
    float padding_end = 0.0f;
};
static_assert(sizeof(GpuCullView) == 240);

struct GpuDrawList {
    uint32_t first_slot = 0;
    uint32_t capacity = 0;
};

constexpr uint32_t kCullViewCount = 1 + kShadowViewCount;
// A local face's draw lists hold this many records at most: a light's reach holds a few things, and every face's
// lists room for everything in the world would be most of the cull's memory.
constexpr uint64_t kLocalFaceRecords = 2048;
// The cull's part keys have room for this many parts in each group to begin with
// (SceneRendererState::ensure_part_keys).
constexpr uint32_t kFirstPartKeyParts = 4096;
// The counts the cull writes: draws per list; the triangles it kept for the camera and for the cascades; records used
// and lost in all; and then records per list. As in shaders/scene/scene_common.slang.
constexpr uint32_t kCountTriangles = kDrawListCount;
constexpr uint32_t kCountRecordsLost = kDrawListCount + 3;
constexpr uint32_t kCountDraws = kDrawListCount + 4;
constexpr uint32_t kCountRecordsPerList = kDrawListCount + 6;
constexpr uint32_t kCullCounterCount = kCountRecordsPerList + kDrawListCount;
// The camera's plain draws, what's fading, each shadow view, and what blends.
constexpr uint32_t kCullGroups = 3 + kShadowViewCount;
constexpr uint32_t kNoMesh = UINT32_MAX;
// The water's levels around the camera (water.h).
constexpr uint64_t kWaterLevelsOffset = kFogSliceMasksOffset + kFroxelSlices * sizeof(uint64_t);
// The local shadow atlas's faces: 4 rows each, then its tile (SceneFrame::local_shadow_views).
constexpr uint64_t kLocalShadowFaceBytes = uint64_t{5} * 4 * sizeof(float);
constexpr uint64_t kLocalShadowViewsOffset = kWaterLevelsOffset + kWaterLevels * sizeof(GpuWaterLevel);
// The views and lists, then what changed in the instances, which stay on the GPU between frames.
constexpr uint64_t kCullViewsOffset = kLocalShadowViewsOffset + (uint64_t{kLocalShadowFaces} * kLocalShadowFaceBytes);
constexpr uint64_t kDrawListsOffset = kCullViewsOffset + kCullViewCount * sizeof(GpuCullView);
constexpr uint64_t kInstanceUpdatesOffset = kDrawListsOffset + kDrawListCount * sizeof(GpuDrawList);

// Matches InstanceUpdate in shaders/scene/instance_update.slang.
struct GpuInstanceUpdate {
    world::RenderTransform transform;
    world::RenderTransform previous_transform;
    uint32_t slot = 0;
    uint32_t mesh = 0;
    uint32_t padding[2] = {};
};
static_assert(sizeof(GpuInstanceUpdate) == 112);
static_assert(sizeof(world::RenderTransform) * 2 == kInstanceBytes);

constexpr uint64_t frame_bytes(uint64_t update_count) {
    return kInstanceUpdatesOffset + std::max<uint64_t>(update_count, 1) * sizeof(GpuInstanceUpdate);
}

// What the frame's instances may draw, which sizes what the cull writes.
struct CullPlan {
    GpuDrawList lists[kDrawListCount];
    uint32_t slots = 0;        // to cull
    uint32_t updates = 0;      // changes to apply
    uint32_t clear_first = 0;  // from here to slots, they hold nothing yet
    uint32_t drawn = 0;        // instances whose mesh is on the GPU
    uint32_t draws = 0;        // their parts, into the G-buffer
    uint32_t shadow_draws = 0; // and into the cascades
    uint64_t draw_capacity = 0;
    uint64_t record_capacity = 0;

    // The draw arguments come first in the cull's buffer, then the records.
    uint64_t records_offset() const { return draw_capacity * sizeof(rhi::DrawIndexedIndirectArgs); }
    uint64_t buffer_bytes() const { return records_offset() + record_capacity * sizeof(GpuDraw); }
};

// Matches CullPush in shaders/scene/cull.slang.
struct CullPush {
    uint64_t frame = 0;
    uint64_t instances = 0;
    uint64_t instance_meshes = 0;
    uint64_t meshes = 0;
    uint64_t lods = 0;
    uint64_t parts = 0;
    uint64_t views = 0;
    uint64_t records = 0;
    uint64_t part_keys = 0;
    uint64_t part_used = 0;
    uint64_t part_dispatch = 0;
    uint32_t instance_count = 0;
    uint32_t part_count = 0;
    uint32_t writing = 0;
};

// Matches CullAssignPush in shaders/scene/cull_assign.slang.
struct CullAssignPush {
    uint64_t parts = 0;
    uint64_t materials = 0;
    uint64_t lists = 0;
    uint64_t args = 0;
    uint64_t counts = 0;
    uint64_t part_keys = 0;
    uint64_t part_used = 0;
    uint32_t part_count = 0;
    uint32_t record_capacity = 0;
    uint32_t merged = 0;
    uint32_t resetting = 0;
};

// Matches BlendSortPush in shaders/scene/blend_sort.slang.
struct BlendSortPush {
    uint64_t frame = 0;
    uint64_t instances = 0;
    uint64_t instance_meshes = 0;
    uint64_t meshes = 0;
    uint64_t records = 0;
    uint64_t args = 0;
    uint64_t sorted = 0;
    uint64_t keys = 0;
    uint64_t counts = 0;
    uint32_t capacity = 0;
    uint32_t placing = 0;
};

// Matches TransparentPush in shaders/scene/transparent.slang.
struct TransparentPush {
    uint64_t frame = 0;
    uint64_t instances = 0;
    uint64_t vertices = 0;
    uint64_t materials = 0;
    uint64_t draws = 0;
    uint32_t sampler = 0;
    uint32_t atlas = 0;
    uint32_t integrated = 0;
    uint32_t grid_sampler = 0;
    uint32_t fog = 0;
    uint32_t froxels = 0;
};

// Matches InstanceUpdatePush in shaders/scene/instance_update.slang.
struct InstanceUpdatePush {
    uint64_t updates = 0;
    uint64_t instances = 0;
    uint64_t instance_meshes = 0;
    uint32_t count = 0;
};

// Scales a plane so that it measures meters.
void normalize_plane(float (&plane)[4]) {
    const float length_squared = (plane[0] * plane[0]) + (plane[1] * plane[1]) + (plane[2] * plane[2]);
    const float scale = 1.0f / std::sqrt(std::max(length_squared, 1e-12f));
    for (float& value : plane) {
        value *= scale;
    }
}

// The plane where w + sign * a row of a matrix is 0, inside where it's positive.
void set_plane(float (&plane)[4], const float* view_proj, uint32_t row, float sign) {
    for (uint32_t k = 0; k < 4; ++k) {
        plane[k] = view_proj[12 + k] + (sign * view_proj[(row * 4) + k]);
    }
    normalize_plane(plane);
}

// A plane relative to a render origin, for an origin this far from it: the same plane in the world.
void move_plane_origin(float (&plane)[4], const DVec3& moved) {
    plane[3] =
        static_cast<float>(static_cast<double>(plane[3]) + (static_cast<double>(plane[0]) * moved.x) +
                           (static_cast<double>(plane[1]) * moved.y) + (static_cast<double>(plane[2]) * moved.z));
}

// The sides of a clip space, inside for |x| <= w and |y| <= w.
void set_side_planes(GpuCullView& view, const float* view_proj) {
    set_plane(view.planes[0], view_proj, 0, 1.0f);
    set_plane(view.planes[1], view_proj, 0, -1.0f);
    set_plane(view.planes[2], view_proj, 1, 1.0f);
    set_plane(view.planes[3], view_proj, 1, -1.0f);
}

} // namespace

struct SceneRendererState {
    rhi::Device* device = nullptr;
    shader::ShaderLibrary* shaders = nullptr;
    GpuAssets* gpu_assets = nullptr;
    // Opaque materials draw with a shader that has no discard (early depth testing holds), masked
    // ones with the alpha test; double-sided ones turn culling off (dynamic state).
    shader::ProgramHandle gbuffer_opaque;
    shader::ProgramHandle gbuffer_masked;
    shader::ProgramHandle gbuffer_opaque_fading;
    shader::ProgramHandle gbuffer_masked_fading;
    shader::ProgramHandle deferred_light;
    shader::ProgramHandle rc_apply;         // the radiance cascades' light, added onto the lit picture
    shader::ProgramHandle rc_apply_replace; // and in place of it, for the indirect light view
    shader::ProgramHandle tonemap;
    shader::ProgramHandle shadow_caster_opaque; // depth only
    shader::ProgramHandle shadow_caster_masked; // alpha-tested
    shader::ProgramHandle shadow_mask;
    shader::ProgramHandle fog_program;
    shader::ProgramHandle fog_inject;
    shader::ProgramHandle fog_integrate;
    shader::ProgramHandle cull;
    shader::ProgramHandle cull_assign;
    shader::ProgramHandle instance_update;
    // What blends: its draws put farthest first, then drawn over the fogged image.
    shader::ProgramHandle blend_sort;
    shader::ProgramHandle transparent_program;
    // The instances, on the GPU between frames, and what the CPU keeps of them: each slot's mesh, to size the draw
    // lists without walking the instances.
    rhi::BufferHandle instance_data;
    rhi::BufferHandle instance_meshes;
    // What the cull counts per part and group, 0 between frames; what's in use this frame; and the dispatch over those.
    // They stay, so a frame clears only what it used.
    rhi::BufferHandle part_keys;
    rhi::BufferHandle part_used;
    uint32_t part_key_parts = 0; // parts the keys have room for, in each group: the stride between groups
    rhi::BufferHandle part_dispatch;
    uint64_t part_keys_address = 0;
    uint64_t part_used_address = 0;
    uint64_t part_dispatch_address = 0;
    bool part_buffers_read = false;
    bool part_keys_zero = false; // false at first, and if a frame didn't get as far as putting them back
    uint64_t instance_data_address = 0;
    uint64_t instance_meshes_address = 0;
    uint32_t max_instances = 0;
    uint32_t cleared_slots = 0;                        // slots the GPU knows hold nothing or something real
    core::Array<uint32_t> slot_written{render_heap()}; // the update each slot was last in
    uint32_t update_number = 0;
    uint32_t used_mesh_slots = 0; // none past this has instances
    bool warned_mesh_table = false;
    // Some instance has no mesh slot because the table was full: when one comes free, it needs sending again.
    bool waiting_for_mesh_slot = false;
    uint64_t mesh_slots_released_seen = 0;
    bool instance_buffers_read = false; // and the graph has left them for reading
    bool instances_stale_now = false;
    bool warned_instance_limit = false;
    uint64_t last_frame_number = 0;
    core::Array<uint32_t> slot_meshes{render_heap()};
    core::Array<uint32_t> mesh_instances{render_heap()}; // by mesh slot
    uint32_t live_instances = 0;
    // What each frame slot's cull kept, to read once that slot comes round again and its frame is done.
#if DE_BUILD_DEV
    // The editor's (set_editor_overlay): the G-buffer's programs with an instance ID out, made when first asked
    // (opaque, masked, both fading, terrain, grass); the selection mask's and the outline's; the selection's draws;
    // picks on their way back.
    shader::ProgramHandle id_programs[6];
    bool id_programs_made = false;
    shader::ProgramHandle selection_program;
    shader::ProgramHandle terrain_selection_program;
    shader::ProgramHandle outline_program;
    rhi::Format outline_format = rhi::Format::Unknown;
    // The debug views' lines: where in view, and where hidden; made for the target's format when there are lines.
    std::span<const world::DebugLine> debug_lines;
    shader::ProgramHandle debug_seen_program;
    shader::ProgramHandle debug_hidden_program;
    rhi::Format debug_format = rhi::Format::Unknown;
    rhi::BufferHandle debug_buffers[rhi::kFramesInFlight];
    EditorOverlay editor;
    core::Array<SelectedMesh> selected{core::heap_allocator(core::MemoryTag::Render)};
    rhi::BufferHandle selection_draws[rhi::kFramesInFlight];
    rhi::BufferHandle pick_readback[rhi::kFramesInFlight];
    bool pick_waiting[rhi::kFramesInFlight] = {};
    bool pick_used[rhi::kFramesInFlight] = {};
    PickAnswer pick;
#endif
    rhi::BufferHandle cull_readback[rhi::kFramesInFlight];
    CullPlan cull_readback_plan[rhi::kFramesInFlight];
    bool cull_readback_written[rhi::kFramesInFlight] = {}; // recorded, and not yet read
    bool cull_readback_used[rhi::kFramesInFlight] = {}; // copied to before: the graph has to order this copy after it
    bool warned_draw_limit = false;
    bool warned_draws_lost = false;
    float lod_error_pixels = 1.0f;
    float lod_fade = 0.2f;
    bool merged_draws = true;
    uint32_t max_draws_per_call = UINT32_MAX;
    bool shadow_caster_cull = true;
    float shadow_min_texels = 0.0f;
    // A frozen cull (set_cull_frozen): its views, once a frame has given them, kept here because the frame's memory is
    // very slow to read, and the render origin they're relative to.
    bool cull_frozen = false;
    bool frozen_views_held = false;
    GpuCullView frozen_views[kCullViewCount] = {};
    DVec3 frozen_origin{0.0};
    SceneShadows shadows; // this frame's, within limits
    shader::ProgramHandle taa_program;
    // What TAA keeps of a frame for the next: two sets, a frame's target and the last frame's to read.
    struct TemporalSet {
        rhi::TextureHandle history;
        rhi::TextureHandle depth;
        rhi::TextureHandle object_motion;
        bool written = false;
    };
    TemporalSet temporal[2];
    uint32_t temporal_width = 0; // what they were last made at: a failure isn't tried again at that size
    uint32_t temporal_height = 0;
    uint32_t temporal_current = 0;
    uint64_t temporal_frame = 0; // the RenderFrame whose TAA pass last ran: temporal[temporal_current] then
    uint32_t taa_sample = 0;
    // The froxels of a frame, kept for the next's reprojection: two, a frame's target and the last's to read.
    rhi::TextureHandle froxel_sets[2];
    bool froxels_written[2] = {};
    uint32_t froxels_width = 0;
    uint32_t froxels_height = 0;
    uint32_t froxels_current = 0;
    uint32_t froxels_idle_frames = 0;
    uint64_t froxels_frame = 0;   // the RenderFrame the injection last ran for: froxel_sets[froxels_current] then
    float froxels_reach_m = 0.0f; // where its slices reached
    uint32_t fog_volumes_now = 0; // written into this frame's constants
    rhi::Format programs_format = rhi::Format::Unknown; // the target's, for the tonemap
    rhi::BufferHandle frame_buffers[rhi::kFramesInFlight];
    uint64_t frame_capacity[rhi::kFramesInFlight] = {};
    SceneRendererStats stats;
    std::optional<Terrain> terrain;
    shader::ProgramHandle terrain_gbuffer;
    shader::ProgramHandle terrain_shadow;
    shader::ProgramHandle terrain_select;
    shader::ProgramHandle grass_gbuffer;
    shader::ProgramHandle grass_scatter;
    bool fog_low_camera = true;
    bool terrain_hidden = false; // the ground not drawn, though the water still reads it (set_terrain_hidden)
    // Water: the world's, its programs, and its mesh's indices (water.h), uploaded once.
    SceneWater water;
    shader::ProgramHandle water_behind;
    shader::ProgramHandle water_program;
    rhi::Uploader* uploader = nullptr;
    rhi::BufferHandle water_indices;
    uint64_t water_index_ticket = 0;
    std::optional<Ocean> ocean;              // the sea's waves
    std::optional<ShoreSim> shore_sim;       // the surf near the camera
    std::optional<ParticleSystem> particles; // every effect's; none without a registry
    std::optional<LocalLights> local_lights; // the frame's point lights and spots, and the view's clusters of them
    LocalLightFrame lights_now;              // what this frame's constants say of them
    std::optional<TraceScene> trace;         // what rays are traced against; none where nothing is traced
    std::optional<Gtao> gtao;                // ambient occlusion on the screen
    std::optional<RadianceCascades> radiance_cascades; // light that has bounced, on the screen (set_radiance_cascades)
    bool radiance_cascades_on = false;                 // set_radiance_cascades
    bool specular_ambient = true;                      // set_specular_ambient
    uint32_t debug_jitter = 0;                         // set_debug_jitter
    // The local shadow atlas, kept from frame to frame: a light's tiles are drawn again only when something in its
    // reach changed (LocalShadowFace::draw).
    rhi::TextureHandle local_atlas;
    bool local_atlas_used = false;  // it has been in a graph: it's in the state a graph leaves it in
    bool local_shadow_cache = true; // set_local_shadow_cache
    // What each instance slot holds, as the lights' shadows need it: where it is and what mesh, to say where casters
    // changed (local_shadow_changes).
    struct SlotCaster {
        world::RenderTransform transform;
        asset::AssetHandle mesh;
    };
    core::Array<SlotCaster> slot_casters{render_heap()};
    core::Array<uint32_t> casters_waiting{render_heap()}; // slots whose mesh wasn't on the GPU when they changed
    core::Array<LocalShadowChange> shadow_changes{render_heap()};

    // Where what casts shadows changed since the last frame, into shadow_changes: each instance that came, went or
    // moved, where it was and where it is, and each whose mesh has only now reached the GPU (it's drawn from this
    // frame). True if it can't be said where (everything was sent again, or tiles aren't to be kept): then every
    // tile is drawn.
    bool local_shadow_changes(const world::RenderFrame& frame) {
        shadow_changes.clear();
        const auto changed_at = [&](const SlotCaster& caster) {
            const GpuMesh* mesh = caster.mesh.is_valid() ? gpu_assets->mesh(caster.mesh) : nullptr;
            if (mesh == nullptr) {
                return false;
            }
            LocalShadowChange change;
            float longest = 0.0f;
            for (int axis = 0; axis < 3; ++axis) {
                const float* row = caster.transform.rows[axis].linear;
                change.center[axis] = caster.transform.position_m(axis, frame.origin) +
                                      (row[0] * mesh->bounds_center[0]) + (row[1] * mesh->bounds_center[1]) +
                                      (row[2] * mesh->bounds_center[2]);
                const float* r0 = caster.transform.rows[0].linear;
                const float* r1 = caster.transform.rows[1].linear;
                const float* r2 = caster.transform.rows[2].linear;
                longest = std::max(longest, (r0[axis] * r0[axis]) + (r1[axis] * r1[axis]) + (r2[axis] * r2[axis]));
            }
            change.radius_m = mesh->bounds_radius_m * std::sqrt(longest);
            shadow_changes.push(change);
            return true;
        };
        for (size_t i = 0; i < frame.instances.size(); ++i) {
            const world::RenderInstance& instance = frame.instances[i];
            const uint32_t slot = instance.slot == world::kAutoRenderSlot ? static_cast<uint32_t>(i) : instance.slot;
            if (slot >= max_instances) { // past the limit, or kSkipRenderInstance
                continue;
            }
            if (slot >= slot_casters.size()) {
                slot_casters.resize(slot + 1);
            }
            changed_at(slot_casters[slot]); // where it was
            slot_casters[slot] = {.transform = instance.transform, .mesh = instance.mesh};
            if (instance.mesh.is_valid() &&
                (!changed_at(slot_casters[slot]) || !gpu_assets->mesh_on_gpu(instance.mesh))) {
                casters_waiting.push(slot);
            }
        }
        for (const uint32_t slot : frame.freed_slots) {
            if (slot < slot_casters.size()) {
                changed_at(slot_casters[slot]);
                slot_casters[slot] = {};
            }
        }
        for (size_t w = 0; w < casters_waiting.size();) {
            const SlotCaster& caster = slot_casters[casters_waiting[w]];
            if (!caster.mesh.is_valid()) {
                casters_waiting.remove_swap(w);
            } else if (gpu_assets->mesh_on_gpu(caster.mesh)) {
                changed_at(caster);
                casters_waiting.remove_swap(w);
            } else {
                ++w;
            }
        }
        return !local_shadow_cache || frame.all_instances;
    }
    std::optional<PathTracer> path_tracer;                     // the other renderer (set_path_traced); only with trace
    bool path_traced = false;                                  // set_path_traced
    bool lighting_compare = false;                             // set_lighting_compare
    std::optional<LightBoxes> light_boxes;                     // set_light_boxes; only with trace
    bool light_boxes_on = true;                                // set_light_boxes_on
    LightBoxes::Frame light_boxes_now;                         // this frame's
    std::optional<AutoExposure> auto_exposure;                 // the exposure, following the scene
    bool auto_exposure_on = false;                             // set_auto_exposure
    uint64_t exposure_clock_ns = 0;                            // when the last frame adapted it
    rhi::BufferHandle exposure_readback[rhi::kFramesInFlight]; // the state, a few frames late, for the stats
    bool exposure_readback_written[rhi::kFramesInFlight] = {};
    bool gtao_on = true;                                                 // set_ambient_occlusion
    shader::ProgramHandle trace_view;                                    // SceneView::Traced
    GpuWaterLevel water_levels_now[kWaterLevels];                        // this frame's, as written into its constants
    core::Array<world::RenderWaterBody> water_bodies_now{render_heap()}; // this frame's lakes and ponds, to draw

    // How much of the height fog is left where the frame's camera is.
    [[nodiscard]] float fog_low_camera_share(const SceneFog& fog, const world::RenderFrame& frame) const {
        if (!fog_low_camera || fog.low_camera_keep >= 1.0f) {
            return 1.0f;
        }
        const auto camera_y = static_cast<float>(frame.origin.y + static_cast<double>(frame.camera.transform[7]));
        const float span = std::max(fog.low_camera_above_m - fog.low_camera_below_m, 1e-3f);
        const float t = std::clamp((camera_y - fog.low_camera_below_m) / span, 0.0f, 1.0f);
        return fog.low_camera_keep + ((1.0f - fog.low_camera_keep) * t * t * (3.0f - (2.0f * t)));
    }

    ~SceneRendererState() {
        for (const shader::ProgramHandle program : {gbuffer_opaque,
                                                    gbuffer_masked,
                                                    gbuffer_opaque_fading,
                                                    gbuffer_masked_fading,
                                                    deferred_light,
                                                    rc_apply,
                                                    rc_apply_replace,
                                                    tonemap,
                                                    shadow_caster_opaque,
                                                    shadow_caster_masked,
                                                    shadow_mask,
                                                    fog_program,
                                                    fog_inject,
                                                    fog_integrate,
                                                    cull,
                                                    cull_assign,
                                                    instance_update,
                                                    blend_sort,
                                                    transparent_program,
                                                    taa_program,
                                                    terrain_gbuffer,
                                                    terrain_shadow,
                                                    terrain_select,
                                                    grass_gbuffer,
                                                    grass_scatter,
                                                    water_behind,
                                                    water_program,
                                                    trace_view}) {
            shaders->destroy(program);
        }
        if (water_indices.is_valid()) {
            device->destroy(water_indices);
        }
        for (const rhi::BufferHandle buffer : frame_buffers) {
            device->destroy(buffer);
        }
        for (const rhi::BufferHandle buffer : cull_readback) {
            device->destroy(buffer);
        }
#if DE_BUILD_DEV
        for (const shader::ProgramHandle program : id_programs) {
            shaders->destroy(program);
        }
        shaders->destroy(selection_program);
        shaders->destroy(terrain_selection_program);
        shaders->destroy(outline_program);
        shaders->destroy(debug_seen_program);
        shaders->destroy(debug_hidden_program);
        for (const rhi::BufferHandle buffer : debug_buffers) {
            device->destroy(buffer);
        }
        for (const rhi::BufferHandle buffer : selection_draws) {
            device->destroy(buffer);
        }
        for (const rhi::BufferHandle buffer : pick_readback) {
            device->destroy(buffer);
        }
#endif
        device->destroy(instance_data);
        device->destroy(instance_meshes);
        device->destroy(part_keys);
        device->destroy(part_used);
        device->destroy(part_dispatch);
        if (local_atlas.is_valid()) {
            device->destroy(local_atlas);
        }
        destroy_temporal();
        destroy_froxels();
    }

    void destroy_froxels() {
        for (rhi::TextureHandle& texture : froxel_sets) {
            if (texture.is_valid()) {
                device->destroy(texture);
            }
            texture = {};
        }
        froxels_written[0] = froxels_written[1] = false;
        froxels_width = 0;
        froxels_height = 0;
    }

    // The kept froxels at this size (a new size starts with no history); false if they can't be made.
    bool ensure_froxels(const rhi::TextureDesc& grid) {
        if (grid.width == froxels_width && grid.height == froxels_height) {
            return froxel_sets[0].is_valid();
        }
        destroy_froxels();
        froxels_width = grid.width;
        froxels_height = grid.height;
        rhi::TextureDesc desc = grid;
        desc.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled;
        desc.debug_name = "froxels";
        for (rhi::TextureHandle& texture : froxel_sets) {
            const core::Result<rhi::TextureHandle> made = device->create_texture(desc);
            if (!made) {
                destroy_froxels();
                froxels_width = grid.width; // not tried again at this size
                froxels_height = grid.height;
                return false;
            }
            texture = *made;
        }
        return true;
    }

    void destroy_temporal() {
        for (TemporalSet& set : temporal) {
            for (const rhi::TextureHandle texture : {set.history, set.depth, set.object_motion}) {
                if (texture.is_valid()) {
                    device->destroy(texture);
                }
            }
            set = {};
        }
    }

    // The cull's part keys (a count per group and part, then where its records go) and the parts in use: room for parts
    // parts in each group, grown to the next power of two when the loaded parts outgrow them (never past the table),
    // each group's keys that far apart. Remade empty, so the next frame clears them.
    bool ensure_part_keys(uint32_t parts) {
        if (parts <= part_key_parts && part_keys.is_valid()) {
            return true;
        }
        uint32_t room = std::max(part_key_parts, kFirstPartKeyParts);
        while (room < parts) {
            room *= 2;
        }
        room = std::min(room, gpu_assets->part_table_size());
        for (const rhi::BufferHandle buffer : {part_keys, part_used}) {
            if (buffer.is_valid()) {
                device->destroy(buffer);
            }
        }
        const uint64_t count = uint64_t{kCullGroups} * room;
        const core::Result<rhi::BufferHandle> keys =
            device->create_buffer({.size = count * sizeof(uint32_t),
                                   .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst,
                                   .debug_name = "part keys"});
        const core::Result<rhi::BufferHandle> used = device->create_buffer(
            {.size = count * sizeof(uint32_t), .usage = rhi::BufferUsage::Storage, .debug_name = "parts in use"});
        if (!keys || !used) {
            part_keys = keys ? *keys : rhi::BufferHandle{};
            part_used = used ? *used : rhi::BufferHandle{};
            part_key_parts = 0;
            return false;
        }
        part_keys = *keys;
        part_used = *used;
        part_keys_address = device->buffer_address(part_keys);
        part_used_address = device->buffer_address(part_used);
        part_key_parts = room;
        part_keys_zero = false;    // cleared before it's used
        part_buffers_read = false; // nothing of them is kept
        return true;
    }

    // TAA's textures at this size (a new size starts with no history); false if they can't be made.
    bool ensure_temporal(uint32_t width, uint32_t height) {
        if (width == temporal_width && height == temporal_height) {
            return temporal[0].history.is_valid();
        }
        destroy_temporal();
        temporal_width = width;
        temporal_height = height;
        for (TemporalSet& set : temporal) {
            const auto make = [&](rhi::Format format, rhi::TextureUsage usage, const char* name) {
                const core::Result<rhi::TextureHandle> texture =
                    device->create_texture({.width = width,
                                            .height = height,
                                            .format = format,
                                            .usage = usage | rhi::TextureUsage::Sampled,
                                            .debug_name = name});
                return texture ? *texture : rhi::TextureHandle{};
            };
            set.history = make(kHdrFormat, rhi::TextureUsage::RenderTarget, "taa history");
            set.depth = make(kDepthFormat, rhi::TextureUsage::DepthStencil, "scene depth");
            set.object_motion = make(kMotionFormat, rhi::TextureUsage::RenderTarget, "taa object motion");
            if (!set.history.is_valid() || !set.depth.is_valid() || !set.object_motion.is_valid()) {
                destroy_temporal();
                return false;
            }
        }
        return true;
    }

    shader::ProgramHandle make(const char* module, const rhi::GraphicsPipelineDesc& state,
                               const char* fragment_entry = "fragment_main") const {
        const core::Result<shader::ProgramHandle> program =
            shaders->create_graphics({.module = module, .fragment_entry = fragment_entry, .state = state});
        return program ? *program : shader::ProgramHandle{};
    }

    // The G-buffer and light programs: their formats are the renderer's own, so made once.
    void make_scene_programs() {
        rhi::GraphicsPipelineDesc gbuffer{.color_count = 5,
                                          .depth_format = kDepthFormat,
                                          .depth_test = true,
                                          .depth_write = true,
                                          .depth_compare = rhi::CompareOp::Greater,
                                          .debug_name = "gbuffer"};
        for (uint32_t i = 0; i < 3; ++i) {
            gbuffer.color_formats[i] = kGBufferFormats[i];
        }
        gbuffer.color_formats[3] = kHdrFormat;
        gbuffer.color_formats[4] = kMotionFormat;
        gbuffer_opaque = make("scene.gbuffer", gbuffer, "fragment_opaque");
        gbuffer.debug_name = "gbuffer (masked)";
        gbuffer_masked = make("scene.gbuffer", gbuffer, "fragment_masked");
        gbuffer.debug_name = "gbuffer (fading)";
        gbuffer_opaque_fading = make("scene.gbuffer", gbuffer, "fragment_opaque_fading");
        gbuffer.debug_name = "gbuffer (masked, fading)";
        gbuffer_masked_fading = make("scene.gbuffer", gbuffer, "fragment_masked_fading");
        gbuffer.debug_name = "terrain";
        terrain_gbuffer = make("scene.terrain", gbuffer);
        rhi::GraphicsPipelineDesc grass = gbuffer;
        grass.cull = rhi::CullMode::None; // a blade is seen from both sides
        grass.debug_name = "grass";
        grass_gbuffer = make("scene.grass", grass);

        rhi::GraphicsPipelineDesc light{.color_count = 1, .cull = rhi::CullMode::None, .debug_name = "deferred light"};
        light.color_formats[0] = kHdrFormat;
        light.blend[0] = rhi::BlendMode::Additive;
        deferred_light = make("scene.deferred_light", light);
        rhi::GraphicsPipelineDesc gathered = light;
        gathered.debug_name = "rc apply";
        rc_apply = make("scene.rc_apply", gathered);
        gathered.blend[0] = rhi::BlendMode::Opaque;
        gathered.debug_name = "rc apply (indirect view)";
        rc_apply_replace = make("scene.rc_apply", gathered);
        if (trace) {
            // Over the lit image, not added to it: the view is the rays' alone.
            rhi::GraphicsPipelineDesc traced{.color_count = 1, .cull = rhi::CullMode::None, .debug_name = "trace view"};
            traced.color_formats[0] = kHdrFormat;
            trace_view = make("scene.trace_view", traced);
        }
        rhi::GraphicsPipelineDesc fogged{.color_count = 1, .cull = rhi::CullMode::None, .debug_name = "fog"};
        fogged.color_formats[0] = kHdrFormat;
        fog_program = make("scene.fog", fogged);
        rhi::GraphicsPipelineDesc resolve{.color_count = 2, .cull = rhi::CullMode::None, .debug_name = "taa"};
        resolve.color_formats[0] = kHdrFormat;
        resolve.color_formats[1] = kMotionFormat;
        taa_program = make("scene.taa", resolve);
        const core::Result<shader::ProgramHandle> inject =
            shaders->create_compute({.module = "scene.fog_inject", .debug_name = "fog inject"});
        fog_inject = inject ? *inject : shader::ProgramHandle{};
        const core::Result<shader::ProgramHandle> select =
            shaders->create_compute({.module = "scene.terrain_select", .debug_name = "terrain select"});
        terrain_select = select ? *select : shader::ProgramHandle{};
        const core::Result<shader::ProgramHandle> scatter =
            shaders->create_compute({.module = "scene.grass_scatter", .debug_name = "grass scatter"});
        grass_scatter = scatter ? *scatter : shader::ProgramHandle{};
        const core::Result<shader::ProgramHandle> cull_program =
            shaders->create_compute({.module = "scene.cull", .debug_name = "gpu cull"});
        cull = cull_program ? *cull_program : shader::ProgramHandle{};
        const core::Result<shader::ProgramHandle> assign_program =
            shaders->create_compute({.module = "scene.cull_assign", .debug_name = "gpu cull assign"});
        cull_assign = assign_program ? *assign_program : shader::ProgramHandle{};
        const core::Result<shader::ProgramHandle> update_program =
            shaders->create_compute({.module = "scene.instance_update", .debug_name = "instance update"});
        instance_update = update_program ? *update_program : shader::ProgramHandle{};
        const core::Result<shader::ProgramHandle> integrate =
            shaders->create_compute({.module = "scene.fog_integrate", .debug_name = "fog integrate"});
        fog_integrate = integrate ? *integrate : shader::ProgramHandle{};

        // Casters: depth only, no culling (thin geometry casts from both sides).
        const rhi::GraphicsPipelineDesc caster{.depth_format = kDepthFormat,
                                               .depth_test = true,
                                               .depth_write = true,
                                               .depth_compare = rhi::CompareOp::Greater,
                                               .cull = rhi::CullMode::None,
                                               .debug_name = "shadow caster"};
        shadow_caster_opaque = make("scene.shadow_caster", caster, "fragment_opaque");
        shadow_caster_masked = make("scene.shadow_caster", caster, "fragment_masked");
        terrain_shadow = make("scene.terrain_shadow", caster);
        rhi::GraphicsPipelineDesc mask{.color_count = 1, .cull = rhi::CullMode::None, .debug_name = "shadow mask"};
        mask.color_formats[0] = kShadowMaskFormat;
        shadow_mask = make("scene.shadow_mask", mask);

        rhi::GraphicsPipelineDesc behind{.color_count = 2, .cull = rhi::CullMode::None, .debug_name = "water behind"};
        behind.color_formats[0] = kHdrFormat;
        behind.color_formats[1] = kWaterBehindDepthFormat;
        water_behind = make("scene.water_behind", behind);
        // Seen from above and below.
        rhi::GraphicsPipelineDesc surface{.color_count = 2,
                                          .depth_format = kDepthFormat,
                                          .depth_test = true,
                                          .depth_write = true,
                                          .depth_compare = rhi::CompareOp::Greater,
                                          .cull = rhi::CullMode::None,
                                          .debug_name = "water"};
        surface.color_formats[0] = kHdrFormat;
        surface.color_formats[1] = kMotionFormat;
        water_program = make("scene.water", surface);

        // What blends: over the fogged image by its alpha (premultiplied, so one state serves glass and glows), tested
        // against the scene's depth and writing none, seen from both sides. Its second target adds up how much of
        // each pixel it covers, mixed the same way: one blend state for every target is all a device has to offer.
        rhi::GraphicsPipelineDesc see_through{.color_count = 2,
                                              .depth_format = kDepthFormat,
                                              .depth_test = true,
                                              .depth_write = false,
                                              .depth_compare = rhi::CompareOp::GreaterEqual,
                                              .cull = rhi::CullMode::None,
                                              .debug_name = "transparents"};
        see_through.color_formats[0] = kHdrFormat;
        see_through.color_formats[1] = kCoverageFormat;
        see_through.blend[0] = rhi::BlendMode::Premultiplied;
        see_through.blend[1] = rhi::BlendMode::Premultiplied;
        transparent_program = make("scene.transparent", see_through);
        const core::Result<shader::ProgramHandle> sort_program =
            shaders->create_compute({.module = "scene.blend_sort", .debug_name = "transparent sort"});
        blend_sort = sort_program ? *sort_program : shader::ProgramHandle{};
    }

#if DE_BUILD_DEV
    // The editor's programs: the G-buffer's with an instance ID out (EDITOR_IDS), and the selection mask's. Made the
    // first time the editor asks; the outline's follows the target's format.
    void make_editor_programs(rhi::Format target_format) {
        if (!id_programs_made) {
            id_programs_made = true;
            static constexpr shader::ShaderDefine kIds[] = {{.name = "EDITOR_IDS", .value = "1"}};
            rhi::GraphicsPipelineDesc gbuffer{.color_count = 6,
                                              .depth_format = kDepthFormat,
                                              .depth_test = true,
                                              .depth_write = true,
                                              .depth_compare = rhi::CompareOp::Greater,
                                              .debug_name = "gbuffer (ids)"};
            for (uint32_t i = 0; i < 3; ++i) {
                gbuffer.color_formats[i] = kGBufferFormats[i];
            }
            gbuffer.color_formats[3] = kHdrFormat;
            gbuffer.color_formats[4] = kMotionFormat;
            gbuffer.color_formats[5] = kIdFormat;
            const auto with_ids = [&](const char* module, const rhi::GraphicsPipelineDesc& state, const char* entry) {
                const core::Result<shader::ProgramHandle> program = shaders->create_graphics(
                    {.module = module, .fragment_entry = entry, .defines = kIds, .state = state});
                return program ? *program : shader::ProgramHandle{};
            };
            id_programs[0] = with_ids("scene.gbuffer", gbuffer, "fragment_opaque");
            id_programs[1] = with_ids("scene.gbuffer", gbuffer, "fragment_masked");
            id_programs[2] = with_ids("scene.gbuffer", gbuffer, "fragment_opaque_fading");
            id_programs[3] = with_ids("scene.gbuffer", gbuffer, "fragment_masked_fading");
            id_programs[4] = with_ids("scene.terrain", gbuffer, "fragment_main");
            rhi::GraphicsPipelineDesc grass = gbuffer;
            grass.cull = rhi::CullMode::None;
            id_programs[5] = with_ids("scene.grass", grass, "fragment_main");
            // The selection mask: its own depth, so only the selection's nearest surface counts at a pixel.
            rhi::GraphicsPipelineDesc mask{.color_count = 1,
                                           .depth_format = kDepthFormat,
                                           .depth_test = true,
                                           .depth_write = true,
                                           .depth_compare = rhi::CompareOp::Greater,
                                           .cull = rhi::CullMode::None,
                                           .debug_name = "editor selection"};
            mask.color_formats[0] = kSelectionMaskFormat;
            selection_program = make("scene.gbuffer", mask, "fragment_selection");
            const core::Result<shader::ProgramHandle> terrain_mask = shaders->create_compute(
                {.module = "scene.terrain_selection", .debug_name = "editor terrain selection"});
            terrain_selection_program = terrain_mask ? *terrain_mask : shader::ProgramHandle{};
        }
        if (target_format != outline_format) {
            shaders->destroy(outline_program);
            rhi::GraphicsPipelineDesc outline{
                .color_count = 1, .cull = rhi::CullMode::None, .debug_name = "editor outline"};
            outline.color_formats[0] = target_format;
            outline.blend[0] = rhi::BlendMode::Alpha;
            outline_program = make("scene.outline", outline);
            outline_format = target_format;
        }
    }

    // The pick this slot's last frame asked for, now that it's done (begin_frame waited for it).
    void read_pick() {
        const auto slot = static_cast<uint32_t>(device->frame_index() % rhi::kFramesInFlight);
        if (!pick_waiting[slot]) {
            return;
        }
        pick_waiting[slot] = false;
        device->invalidate_mapped(pick_readback[slot]);
        uint32_t id = 0;
        std::memcpy(&id, device->mapped(pick_readback[slot]).data(), sizeof(id));
        pick.slot = id == 0 || id == kEditorTerrainId ? kNothingPicked : id - 1;
        ++pick.count;
    }
#endif

    // The tonemap writes the caller's target: made again when its format changes.
    void make_tonemap(rhi::Format target_format) {
        if (target_format == programs_format) {
            return;
        }
        shaders->destroy(tonemap);
        rhi::GraphicsPipelineDesc post{.color_count = 1, .cull = rhi::CullMode::None, .debug_name = "tonemap"};
        post.color_formats[0] = target_format;
        tonemap = make("scene.tonemap", post);
        programs_format = target_format;
    }

    // The stats from what this slot's last frame culled. Its frame is done: begin_frame waited for it.
    void read_cull_stats() {
        const auto slot = static_cast<uint32_t>(device->frame_index() % rhi::kFramesInFlight);
        if (!cull_readback_written[slot]) {
            return;
        }
        cull_readback_written[slot] = false;
        device->invalidate_mapped(cull_readback[slot]);
        const std::span<const std::byte> mapped = device->mapped(cull_readback[slot]);
        uint32_t counts[kCullCounterCount];
        std::memcpy(counts, mapped.data(), sizeof(counts));
        for (uint32_t list = 0; list < kDrawListCount; ++list) {
            const uint32_t records = counts[kCountRecordsPerList + list];
            (list < kCameraListCount || list == kBlendList ? stats.visible_draws : stats.visible_shadow_draws) +=
                records;
            if (list >= kDrawKindCount && list < kCameraListCount) {
                stats.visible_fading_draws += records;
            }
        }
        stats.visible_blend_draws = counts[kCountRecordsPerList + kBlendList];
        stats.draws_lost = counts[kCountRecordsLost];
        if (stats.draws_lost > 0 && !warned_draws_lost) {
            DE_LOG_WARN(
                "render",
                "%u draws had no room: a material changing kind can do this for a frame, or a device call limit",
                stats.draws_lost);
            warned_draws_lost = true;
        }
        stats.merged_draws = counts[kCountDraws];
        stats.merged_shadow_draws = counts[kCountDraws + 1];
        stats.visible_triangles = counts[kCountTriangles];
        stats.visible_shadow_triangles = counts[kCountTriangles + 1];
    }

    // A list the cull filled, with the bound pipeline and the geometry pool's index buffer.
    void draw_list(rhi::CommandList& list, rhi::BufferHandle args, rhi::BufferHandle counts, const CullPlan& plan,
                   uint32_t index) {
        const GpuDrawList& draws = plan.lists[index];
        list.draw_indexed_indirect_count(args, uint64_t{draws.first_slot} * sizeof(rhi::DrawIndexedIndirectArgs),
                                         counts, uint64_t{index} * sizeof(uint32_t), draws.capacity);
        ++stats.indirect_calls;
    }

    // This frame slot's buffer, at least this big: written by the CPU now, read by this frame's GPU
    // work (the slot's last frame is done: begin_frame waited for it).
    rhi::BufferHandle frame_buffer(uint64_t needed) {
        const auto slot = static_cast<uint32_t>(device->frame_index() % rhi::kFramesInFlight);
        if (frame_capacity[slot] < needed) {
            device->destroy(frame_buffers[slot]);
            const uint64_t capacity = std::max<uint64_t>(needed + needed / 2, uint64_t{64} * 1024);
            const core::Result<rhi::BufferHandle> buffer = device->create_buffer({.size = capacity,
                                                                                  .usage = rhi::BufferUsage::Storage,
                                                                                  .memory = rhi::MemoryLocation::Upload,
                                                                                  .debug_name = "scene frame"});
            frame_buffers[slot] = buffer ? *buffer : rhi::BufferHandle{};
            frame_capacity[slot] = buffer ? capacity : 0;
        }
        return frame_buffers[slot];
    }

    // What sets a fog volume's place in the view, worked out once a frame. The camera's axes without any
    // scale it may carry, as the view itself.
    struct FogVolumeCamera {
        Vec3 position{0.0f};
        Vec3 right{1.0f, 0.0f, 0.0f};
        Vec3 up{0.0f, 1.0f, 0.0f};
        Vec3 forward{0.0f, 0.0f, -1.0f};
        float tan_x = 0.0f;
        float tan_y = 0.0f;
    };
    static FogVolumeCamera fog_volume_camera(const world::RenderFrame& frame, float aspect) {
        const float* camera = frame.camera.transform;
        const float tan_y = std::tan(frame.camera.vertical_fov_rad * 0.5f);
        return {.position = Vec3(camera[3], camera[7], camera[11]),
                .right = glm::normalize(Vec3(camera[0], camera[4], camera[8])),
                .up = glm::normalize(Vec3(camera[1], camera[5], camera[9])),
                .forward = glm::normalize(Vec3(-camera[2], -camera[6], -camera[10])),
                .tan_x = tan_y * aspect,
                .tan_y = tan_y};
    }

    // Where a fog volume is from the camera, by its bounding sphere, and whether it reaches what the froxel
    // grid sees: in the view and within its reach, which is in view depth. What that lets through costs a
    // little and adds nothing.
    struct FogVolumeView {
        float ahead = 0.0f;
        float radius = 0.0f;
        float nearest = 0.0f; // of its sphere from the camera: 0 around the camera
        bool visible = false;
    };
    static FogVolumeView fog_volume_view(const FogVolumeCamera& camera, const world::RenderFogVolume& volume,
                                         float froxel_reach) {
        const Vec3 to = Vec3(volume.center[0], volume.center[1], volume.center[2]) - camera.position;
        const Vec3 half_extents(volume.half_extents[0], volume.half_extents[1], volume.half_extents[2]);
        const float ahead = glm::dot(to, camera.forward);
        const float aside = std::abs(glm::dot(to, camera.right));
        const float above = std::abs(glm::dot(to, camera.up));
        const float radius =
            volume.ellipsoid ? std::max({half_extents.x, half_extents.y, half_extents.z}) : glm::length(half_extents);
        const bool visible = ahead - radius <= froxel_reach && ahead >= -radius &&
                             (aside - ahead * camera.tan_x) / std::sqrt(1.0f + camera.tan_x * camera.tan_x) <= radius &&
                             (above - ahead * camera.tan_y) / std::sqrt(1.0f + camera.tan_y * camera.tan_y) <= radius;
        return {
            .ahead = ahead, .radius = radius, .nearest = std::max(glm::length(to) - radius, 0.0f), .visible = visible};
    }

    // The fog volumes that reach the froxel grid, the nearest if there are too many, into the frame's
    // buffer. Each gets a bit in the masks of the slices its sphere's depth covers.
    // `sky_factor`: what a volume's openness to the sky is multiplied by (the radiance cascades' for the sky), or
    // negative while nothing reads it. Those that say how open their place is are written first: how many is
    // returned, for the frame's constants (shaders/scene/sky.slang's open_sky).
    uint32_t write_fog_volumes(std::span<std::byte> mapped, const world::RenderFrame& frame, float froxel_reach,
                               float aspect, float sky_factor) {
        fog_volumes_now = 0;
        stats.fog_volumes = 0;
        stats.fog_volumes_dropped = 0;
        if (froxel_reach <= 0.0f || frame.fog_volumes.empty()) {
            return 0;
        }
        struct KeptVolume {
            uint32_t index = 0;
            FogVolumeView seen;
        };
        // Once full, a heap with the farthest first: anything nearer takes its place.
        const auto nearer = [](const KeptVolume& a, const KeptVolume& b) { return a.seen.nearest < b.seen.nearest; };
        const FogVolumeCamera camera = fog_volume_camera(frame, aspect);
        KeptVolume kept[kMaxFogVolumes];
        uint32_t dropped = 0;
        for (uint32_t i = 0; i < frame.fog_volumes.size(); ++i) {
            const FogVolumeView seen = fog_volume_view(camera, frame.fog_volumes[i], froxel_reach);
            if (!seen.visible) {
                continue;
            }
            if (fog_volumes_now < kMaxFogVolumes) {
                kept[fog_volumes_now++] = {.index = i, .seen = seen};
                if (fog_volumes_now == kMaxFogVolumes) {
                    std::make_heap(kept, kept + kMaxFogVolumes, nearer);
                }
                continue;
            }
            ++dropped;
            if (seen.nearest < kept[0].seen.nearest) {
                std::pop_heap(kept, kept + kMaxFogVolumes, nearer);
                kept[kMaxFogVolumes - 1] = {.index = i, .seen = seen};
                std::push_heap(kept, kept + kMaxFogVolumes, nearer);
            }
        }
        const auto says_sky = [&](const KeptVolume& volume) {
            return sky_factor >= 0.0f && frame.fog_volumes[volume.index].under_roof >= 0.0f;
        };
        const auto sky_volumes =
            static_cast<uint32_t>(std::stable_partition(kept, kept + fog_volumes_now, says_sky) - kept);
        uint64_t slice_masks[kFroxelSlices] = {};
        const auto slice_of = [&](float depth) {
            const float t = std::log(std::max(depth, 0.0f) / kFroxelSliceOffsetM + 1.0f) /
                            std::log(froxel_reach / kFroxelSliceOffsetM + 1.0f);
            return static_cast<uint32_t>(
                std::clamp(t * static_cast<float>(kFroxelSlices), 0.0f, static_cast<float>(kFroxelSlices - 1)));
        };
        for (uint32_t k = 0; k < fog_volumes_now; ++k) {
            const world::RenderFogVolume& volume = frame.fog_volumes[kept[k].index];
            const FogVolumeView& seen = kept[k].seen;
            const uint32_t last_slice = slice_of(seen.ahead + seen.radius);
            for (uint32_t slice = slice_of(seen.ahead - seen.radius); slice <= last_slice; ++slice) {
                slice_masks[slice] |= uint64_t{1} << k;
            }
            GpuFogVolume gpu;
            std::memcpy(gpu.to_local, volume.to_local, sizeof(gpu.to_local));
            gpu.half_extents[0] = volume.half_extents[0];
            gpu.half_extents[1] = volume.half_extents[1];
            gpu.half_extents[2] = volume.half_extents[2];
            gpu.half_extents[3] = volume.edge_m;
            gpu.tint_density[0] = volume.tint[0];
            gpu.tint_density[1] = volume.tint[1];
            gpu.tint_density[2] = volume.tint[2];
            gpu.tint_density[3] = volume.density_per_m;
            gpu.shape[0] = volume.ellipsoid ? 1.0f : 0.0f;
            gpu.shape[1] = volume.noise_amount;
            gpu.shape[2] = volume.height_fade_m;
            gpu.shape[3] = k < sky_volumes ? (1.0f - std::min(volume.under_roof, 1.0f)) * sky_factor : -1.0f;
            gpu.noise[0] = volume.noise_offset_tiles[0];
            gpu.noise[1] = volume.noise_offset_tiles[1];
            gpu.noise[2] = volume.noise_offset_tiles[2];
            gpu.noise[3] = volume.noise_tiles_per_m;
            std::memcpy(mapped.data() + kFogVolumesOffset + k * sizeof(GpuFogVolume), &gpu, sizeof(gpu));
        }
        std::memcpy(mapped.data() + kFogSliceMasksOffset, slice_masks, sizeof(slice_masks));
        stats.fog_volumes = fog_volumes_now;
        stats.fog_volumes_dropped = dropped;
        return sky_volumes;
    }

    // One slot's mesh, as the CPU knows it.
    void set_slot_mesh(uint32_t slot, uint32_t mesh) {
        const uint32_t was = slot_meshes[slot];
        if (was != kNoMesh) {
            --mesh_instances[was];
            --live_instances;
        }
        if (mesh != kNoMesh) {
            ++mesh_instances[mesh];
            ++live_instances;
            used_mesh_slots = std::max(used_mesh_slots, mesh + 1);
        }
        slot_meshes[slot] = mesh;
    }

    // What changed, as records for the update pass to scatter, and what the CPU keeps of it.
    void write_instance_updates(std::span<std::byte> mapped, const world::RenderFrame& frame, CullPlan& plan) {
        auto* const updates = reinterpret_cast<GpuInstanceUpdate*>(mapped.data() + kInstanceUpdatesOffset);
        const auto wanted_slots =
            frame.slot_count != 0 ? frame.slot_count : static_cast<uint32_t>(frame.instances.size());
        plan.slots = std::min(wanted_slots, max_instances);
        if (wanted_slots > max_instances && !warned_instance_limit) {
            DE_LOG_ERROR("render", "%u instances, past the limit of %u: the rest aren't drawn", wanted_slots,
                         max_instances);
            warned_instance_limit = true;
        }
        // All of them, or a list that has shrunk: what's gone from it won't say so.
        const uint32_t kept_slots = frame.all_instances ? 0 : plan.slots;
        for (uint32_t slot = kept_slots; slot < slot_meshes.size(); ++slot) {
            set_slot_mesh(slot, kNoMesh);
        }
        slot_meshes.resize(plan.slots, kNoMesh);
        slot_written.resize(plan.slots, 0);
        // Slots coming into use may hold anything, or what was there before the list last shrank.
        plan.clear_first = frame.all_instances ? 0 : cleared_slots;
        ++update_number;

        uint32_t count = 0;
        for (size_t i = 0; i < frame.instances.size(); ++i) {
            const world::RenderInstance& instance = frame.instances[i];
            const uint32_t slot = instance.slot == world::kAutoRenderSlot ? static_cast<uint32_t>(i) : instance.slot;
            if (slot >= plan.slots) { // past the limit, or kSkipRenderInstance
                continue;
            }
            DE_ASSERT_MSG(slot_written[slot] != update_number, "two records for a slot would land in either order");
            slot_written[slot] = update_number;
            const uint32_t mesh = gpu_assets->mesh_slot(instance.mesh);
            if (trace && mesh != kNoMesh) {
                trace->want(mesh, instance.mesh); // rays should see it too
            }
            if (mesh == kNoMesh && instance.mesh.is_valid()) {
                waiting_for_mesh_slot = true;
                mesh_slots_released_seen = gpu_assets->mesh_slots_released();
                if (!warned_mesh_table) {
                    DE_LOG_ERROR("render",
                                 "the mesh table is full, or a mesh went before its instances: they aren't drawn");
                    warned_mesh_table = true;
                }
            }
            set_slot_mesh(slot, mesh);
            GpuInstanceUpdate& update = updates[count++];
            update.transform = instance.transform;
            update.previous_transform = instance.previous_transform;
            update.slot = slot;
            update.mesh = mesh;
        }
        // The records land in any order, so a slot freed and taken again in one frame gets only the one above.
        for (const uint32_t slot : frame.freed_slots) {
            if (slot < plan.slots && slot_written[slot] != update_number) {
                set_slot_mesh(slot, kNoMesh);
                updates[count].slot = slot;
                updates[count].mesh = kNoMesh;
                ++count;
            }
        }
        plan.updates = count;
    }

    // The views this frame gives the cull.
    void make_cull_views(const world::RenderFrame& frame, const SceneFrame& scene,
                         const ShadowCascade (&cascades)[kShadowCascadeCount], uint32_t height,
                         GpuCullView (&views)[kCullViewCount]) const {
        // At 0, LOD 0 everywhere, however small a LOD's error.
        const auto lod_scale_for = [this](float per_unit_of_error) {
            return lod_error_pixels > 0.0f ? per_unit_of_error / lod_error_pixels : -1.0f;
        };
        // The camera: its sides, which meet at its eye, so they cover what's behind it too. Projection has no far
        // plane.
        views[0] = {.plane_count = 4,
                    .lod_perspective = 1,
                    .lod_scale = lod_scale_for(static_cast<float>(height) /
                                               (2.0f * std::tan(frame.camera.vertical_fov_rad * 0.5f))),
                    .lod_fade_end = 1.0f - lod_fade};
        // Depth along where it looks, its -Z axis. A camera's transform may scale.
        const float* const camera = frame.camera.transform;
        const float look[3] = {-camera[2], -camera[6], -camera[10]};
        const float look_length = std::sqrt((look[0] * look[0]) + (look[1] * look[1]) + (look[2] * look[2]));
        for (uint32_t k = 0; k < 3; ++k) {
            views[0].depth_plane[k] = look[k] / look_length;
        }
        views[0].depth_plane[3] = -((views[0].depth_plane[0] * camera[3]) + (views[0].depth_plane[1] * camera[7]) +
                                    (views[0].depth_plane[2] * camera[11]));
        set_side_planes(views[0], scene.view_proj); // jittered, as it's drawn
        // A cascade: what can shadow what it's read for, as far toward the sun as its depth goes (z <= w: depth isn't
        // clamped, so a caster past that would be clipped anyway, and how far that is is SceneShadows::caster_reach_m).
        // With that cull off, all its map could hold: its sides and both ends of its depth.
        for (uint32_t c = 0; c < kShadowCascadeCount; ++c) {
            GpuCullView& view = views[1 + c];
            view = {.lod_scale = lod_scale_for(1.0f / std::max(scene.cascade_texel_m[c], 1e-6f)),
                    .min_radius_m = 0.5f * shadow_min_texels * scene.cascade_texel_m[c]};
            const float* const view_proj = &scene.cascade_view_proj[static_cast<size_t>(c) * 16];
            if (shadow_caster_cull) {
                view.plane_count = cascades[c].caster_plane_count;
                std::memcpy(view.planes, cascades[c].caster_planes, sizeof(cascades[c].caster_planes));
            } else {
                set_side_planes(view, view_proj);
                std::memcpy(view.planes[4], view_proj + 8, sizeof(view.planes[4]));
                normalize_plane(view.planes[4]);
                view.plane_count = 5;
            }
            set_plane(view.planes[view.plane_count], view_proj, 2, -1.0f);
            ++view.plane_count;
        }
        // A local light's face: its sides and near, and as far as the light reaches along its axis. One not used this
        // frame holds nothing: a plane nothing is inside.
        for (uint32_t f = 0; f < kLocalShadowFaces; ++f) {
            GpuCullView& view = views[1 + kShadowCascadeCount + f];
            if (f >= lights_now.face_count || !lights_now.faces[f].draw) { // or its tile holds what it held
                view = {.plane_count = 1};
                view.planes[0][3] = -1e30f;
                continue;
            }
            const LocalShadowFace& face = lights_now.faces[f];
            // Its LOD as the camera's is chosen: a face is a perspective view from the light, so a texel's size grows
            // with the distance along its axis, and what's near the light takes its finest LOD (a light's reach as the
            // texel size took coarse LODs for everything, and shadows didn't match the shapes drawn).
            // The projection's second row is its up axis times 1 / tan(half the field of view).
            const float focal =
                std::sqrt((face.view_proj[4] * face.view_proj[4]) + (face.view_proj[5] * face.view_proj[5]) +
                          (face.view_proj[6] * face.view_proj[6]));
            const float tan_half = focal > 1e-6f ? 1.0f / focal : 1.0f;
            view = {.lod_perspective = 1,
                    .lod_scale = lod_scale_for(static_cast<float>(face.tile[2]) / (2.0f * tan_half)),
                    .lod_fade_end = 1.0f};
            for (int k = 0; k < 3; ++k) {
                view.depth_plane[k] = face.forward[k];
            }
            view.depth_plane[3] = -((face.forward[0] * face.position[0]) + (face.forward[1] * face.position[1]) +
                                    (face.forward[2] * face.position[2]));
            set_side_planes(view, face.view_proj);
            view.plane_count = 4;
            set_plane(view.planes[view.plane_count++], face.view_proj, 2, -1.0f); // near: z <= w
            // Far: dot(-forward, p) + forward . position + reach >= 0.
            float* far = view.planes[view.plane_count++];
            for (int k = 0; k < 3; ++k) {
                far[k] = -face.forward[k];
            }
            far[3] = (face.forward[0] * face.position[0]) + (face.forward[1] * face.position[1]) +
                     (face.forward[2] * face.position[2]) + face.far_m;
        }
    }

    // The frozen views, relative to this frame's render origin. Always worked out from the origin they froze at, so
    // rounding doesn't add up as it moves.
    void frozen_cull_views(const DVec3& origin, GpuCullView (&views)[kCullViewCount]) const {
        const DVec3 moved = origin - frozen_origin;
        for (uint32_t v = 0; v < kCullViewCount; ++v) {
            views[v] = frozen_views[v];
            for (uint32_t p = 0; p < views[v].plane_count; ++p) {
                move_plane_origin(views[v].planes[p], moved);
            }
            move_plane_origin(views[v].depth_plane, moved);
        }
    }

    // The cull's views, what changed in the instances, and from them the draw lists' sizes.
    void write_cull_data(std::span<std::byte> mapped, const world::RenderFrame& frame, const SceneFrame& scene,
                         const ShadowCascade (&cascades)[kShadowCascadeCount], uint32_t height, CullPlan& plan) {
        // Made here and copied: this memory is fast to write and very slow to read.
        GpuCullView views[kCullViewCount];
        if (cull_frozen && frozen_views_held) {
            frozen_cull_views(frame.origin, views);
        } else {
            make_cull_views(frame, scene, cascades, height, views);
            if (cull_frozen) {
                std::copy_n(views, kCullViewCount, frozen_views);
                frozen_origin = frame.origin;
                frozen_views_held = true;
            }
        }
        std::memcpy(mapped.data() + kCullViewsOffset, views, sizeof(views));

        write_instance_updates(mapped, frame, plan);
        // What the instances can draw, from how many there are of each mesh.
        uint64_t kind_parts[kDrawKindCount] = {};
        uint64_t fading_parts[kDrawKindCount] = {};
        uint64_t blend_parts = 0;
        const std::span<const GpuMeshKinds> kinds = gpu_assets->mesh_kinds();
        for (size_t mesh = 0; mesh < used_mesh_slots; ++mesh) {
            const uint32_t count = mesh_instances[mesh];
            if (count == 0) {
                continue;
            }
            bool drawable = false;
            for (uint32_t kind = 0; kind < kDrawKindCount; ++kind) {
                kind_parts[kind] += uint64_t{count} * kinds[mesh].parts[kind];
                // Two LODs at once, each with at most that many. Not lod_fade: a frozen view keeps its fades.
                fading_parts[kind] += kinds[mesh].has_lods && views[0].lod_fade_end < 1.0f
                                          ? uint64_t{2} * count * kinds[mesh].parts[kind]
                                          : 0;
                drawable = drawable || kinds[mesh].parts[kind] > 0;
            }
            blend_parts += uint64_t{count} * kinds[mesh].blend_parts;
            drawable = drawable || kinds[mesh].blend_parts > 0;
            plan.drawn += drawable ? count : 0;
        }
        // Each list has room for a draw for all that could land in it, which merged is at most one per part in the
        // table, and there's a record for each of them.
        uint32_t slot = 0;
        uint64_t records = 0;
        const uint32_t call_limit = std::min(device->capabilities().max_draw_indirect_count, max_draws_per_call);
        const auto add_list = [&](uint32_t index, uint64_t wanted, bool merges = true) {
            records += wanted;
            const auto capacity = static_cast<uint32_t>(std::min<uint64_t>(wanted, UINT32_MAX));
            // What one counted call can't reach is left out, and said so. The spec only promises 65535 draws in a call,
            // though desktop drivers take billions. Splitting a counted call needs the count known on the CPU.
            const uint32_t draws =
                merged_draws && merges ? std::min(capacity, gpu_assets->part_table_used()) : capacity;
            if (draws > call_limit && !warned_draw_limit) {
                DE_LOG_WARN("render",
                            "a draw list may hold %u draws, and this device draws %u in a call: the rest are lost",
                            draws, call_limit);
                warned_draw_limit = true;
            }
            const uint32_t kept = std::min(draws, call_limit);
            plan.lists[index] = {.first_slot = slot, .capacity = kept};
            slot += kept;
        };
        for (uint32_t kind = 0; kind < kDrawKindCount; ++kind) {
            add_list(kind, kind_parts[kind]);
            plan.draws += static_cast<uint32_t>(kind_parts[kind]);
        }
        for (uint32_t kind = 0; kind < kDrawKindCount; ++kind) {
            add_list(kDrawKindCount + kind, fading_parts[kind]);
        }
        for (uint32_t c = 0; c < kShadowCascadeCount; ++c) {
            add_list(cascade_list(c, false), kind_parts[0] + kind_parts[1]);
            add_list(cascade_list(c, true), kind_parts[2] + kind_parts[3]);
            plan.shadow_draws += static_cast<uint32_t>(kind_parts[0] + kind_parts[1] + kind_parts[2] + kind_parts[3]);
        }
        for (uint32_t f = 0; f < kLocalShadowFaces; ++f) {
            const bool used = f < lights_now.face_count && lights_now.faces[f].draw;
            add_list(cascade_list(kShadowCascadeCount + f, false),
                     used ? std::min(kind_parts[0] + kind_parts[1], kLocalFaceRecords) : 0);
            add_list(cascade_list(kShadowCascadeCount + f, true),
                     used ? std::min(kind_parts[2] + kind_parts[3], kLocalFaceRecords) : 0);
        }
        // What blends: a draw each, never merged, since each has its own place in the order. Past the limit, the rest
        // find no room and are lost, as the cull's counts say.
        add_list(kBlendList, std::min<uint64_t>(blend_parts, kMaxBlendDraws), false);
        std::memcpy(mapped.data() + kDrawListsOffset, plan.lists, sizeof(plan.lists));
        // A power of two, so a graph buffer lasts while the counts move around.
        plan.draw_capacity = 1024;
        while (plan.draw_capacity < slot) {
            plan.draw_capacity *= 2;
        }
        plan.record_capacity = 1024;
        while (plan.record_capacity < records) {
            plan.record_capacity *= 2;
        }
    }

    // Writes the frame's constants, instance transforms and what the cull reads; false without a buffer.
    // Whether the surf's simulation runs this frame: a sea with its shore map in, and its shaders there.
    [[nodiscard]] bool shore_sim_runs() const {
        // Not while the surf is its film's.
        return water.sea && !water.surf_film && shore_sim && shore_sim->running() && water.shore.is_valid() &&
               gpu_assets != nullptr && gpu_assets->material_complete(water.shore);
    }

    bool write_frame(rhi::BufferHandle buffer, CullPlan& plan, const world::RenderFrame& frame,
                     const SceneLighting& lighting, float froxel_reach, uint32_t taa_sample_now, uint32_t width,
                     uint32_t height) {
        const std::span<std::byte> mapped = device->mapped(buffer);
        if (mapped.empty()) {
            return false;
        }
        const float aspect = static_cast<float>(width) / static_cast<float>(std::max(height, 1U));
        SceneFrame scene;
        camera_view_proj(frame.camera.transform, frame.camera.vertical_fov_rad, frame.camera.near_m, aspect,
                         scene.view_proj);
        std::memcpy(scene.unjittered_view_proj, scene.view_proj, sizeof(scene.view_proj));
        if (taa_sample_now != 0) {
            // A sub-pixel offset in clip space: x and y move by an offset times w.
            const float offset_x = (halton(taa_sample_now, 2) - 0.5f) * 2.0f / static_cast<float>(width);
            const float offset_y = (halton(taa_sample_now, 3) - 0.5f) * 2.0f / static_cast<float>(height);
            for (int column = 0; column < 4; ++column) {
                scene.view_proj[column] += offset_x * scene.view_proj[12 + column];
                scene.view_proj[4 + column] += offset_y * scene.view_proj[12 + column];
            }
            scene.taa_params[2] = offset_x;
            scene.taa_params[3] = offset_y;
        }
        scene.taa_params[0] = frame.camera.previous_valid ? frame.camera.previous_near_m : frame.camera.near_m;
        scene.taa_params[1] = static_cast<float>(taa_sample_now);
        if (!invert_4x4(scene.view_proj, scene.inv_view_proj)) {
            return false;
        }
        // Last frame's view (at this frame's render origin); after a cut, or at first, this one's.
        if (frame.camera.previous_valid) {
            camera_view_proj(frame.camera.previous_transform, frame.camera.previous_vertical_fov_rad,
                             frame.camera.previous_near_m, aspect, scene.previous_view_proj);
        } else {
            std::memcpy(scene.previous_view_proj, scene.unjittered_view_proj, sizeof(scene.view_proj));
        }
        scene.camera_position[0] = frame.camera.transform[3];
        scene.camera_position[1] = frame.camera.transform[7];
        scene.camera_position[2] = frame.camera.transform[11];
        for (int axis = 0; axis < 3; ++axis) {
            scene.render_origin_steps[axis] = world::render_position_steps(frame.origin[axis]);
        }
        const float* d = lighting.sun_direction;
        const float length = std::max(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), 1e-6f);
        for (int i = 0; i < 3; ++i) {
            scene.sun_direction[i] = d[i] / length;
        }
        copy3(lighting.sun_color, scene.sun_color);
        copy3(lighting.sky_color, scene.sky_color);
        // How open to the sky the world is (shaders/scene/sky.slang's open_sky), for what lights by the sky's ambient
        // without seeing what's above it: what the radiance cascades can't see, the fog, lit particles, water. The
        // environment's, times the cascades' factor for the sky; only while they light the picture (the flat
        // ambient they stand in for knows no roof either, and the two should agree). In a hall with a roof the open
        // sky's blue on the fog alone would be most of the picture.
        const bool placed = radiance_cascades && radiance_cascades_on;
        const float sky_factor = placed ? radiance_cascades->settings().sky : -1.0f;
        const float sky_share = placed ? (1.0f - std::clamp(lighting.under_roof, 0.0f, 1.0f)) * sky_factor : 1.0f;
        scene.sky_color[3] = sky_share;
        copy3(lighting.ground_color, scene.ground_color);
        copy3(lighting.shadow_color, scene.shadow_color);
        if (lighting.backdrop.enabled) {
            copy3(lighting.backdrop.top, scene.backdrop_top);
            copy3(lighting.backdrop.bottom, scene.backdrop_bottom);
            scene.backdrop_top[3] = 1.0f;
        }
        scene.trace = trace ? trace->scene_address() : 0;
        scene.local_shadow_views = device->buffer_address(buffer) + kLocalShadowViewsOffset;
        scene.light_boxes = light_boxes_now.boxes;
        scene.light_box_count = light_boxes_now.count;
        for (uint32_t f = 0; f < lights_now.face_count; ++f) {
            const LocalShadowFace& face = lights_now.faces[f];
            std::byte* at = mapped.data() + kLocalShadowViewsOffset + (size_t{f} * kLocalShadowFaceBytes);
            std::memcpy(at, face.view_proj, 16 * sizeof(float));
            const float tile[4] = {static_cast<float>(face.tile[0]), static_cast<float>(face.tile[1]),
                                   static_cast<float>(face.tile[2]), 0.0f};
            std::memcpy(at + (16 * sizeof(float)), tile, sizeof(tile));
        }
        scene.lights = lights_now.lights;
        scene.light_clusters = lights_now.clusters;
        for (int i = 0; i < 4; ++i) {
            scene.light_grid[i] = lights_now.grid[i];
            scene.light_slices[i] = lights_now.slices[i];
        }
        const float forward[3] = {-frame.camera.transform[2], -frame.camera.transform[6], -frame.camera.transform[10]};
        const float forward_length =
            std::max(std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]), 1e-6f);
        for (int i = 0; i < 3; ++i) {
            scene.camera_forward[i] = forward[i] / forward_length;
        }
        const float sun[3] = {scene.sun_direction[0], scene.sun_direction[1], scene.sun_direction[2]};
        ShadowCascade cascades[kShadowCascadeCount];
        fit_shadow_cascades(frame.camera.transform, frame.origin, frame.camera.vertical_fov_rad, frame.camera.near_m,
                            aspect, sun, shadows, cascades);
        for (uint32_t c = 0; c < kShadowCascadeCount; ++c) {
            std::memcpy(&scene.cascade_view_proj[static_cast<size_t>(c) * 16], cascades[c].view_proj,
                        sizeof(cascades[c].view_proj));
            scene.cascade_far[c] = cascades[c].far_m;
            scene.cascade_texel_m[c] = cascades[c].texel_m;
            scene.cascade_depth_per_m[c] = cascades[c].depth_per_m;
        }
        scene.shadow_params[0] = shadows.blend_band;
        scene.shadow_params[1] = 1.0f / static_cast<float>(shadows.resolution * 2);
        scene.shadow_params[2] = shadows.penumbra_per_m;
        scene.shadow_params[3] = shadows.max_penumbra_texels;
        const SceneFog& fog = lighting.fog;
        const auto base_height = static_cast<float>(fog.base_height_m - frame.origin.y);
        scene.fog_density[0] = fog.density_per_m;
        scene.fog_density[1] = fog.height_density_per_m * fog_low_camera_share(fog, frame);
        scene.fog_density[2] = fog.height_falloff_per_m;
        scene.fog_density[3] = base_height;
        copy3(fog.extinction_tint, scene.fog_extinction);
        scene.fog_extinction[3] = fog.ambient_height_attenuation;
        copy3(fog.sun_forward, scene.fog_sun_forward);
        scene.fog_sun_forward[3] = fog.sun_height_attenuation;
        copy3(fog.sun_back, scene.fog_sun_back);
        scene.fog_sun_back[3] = fog.sun_start_m;
        for (int i = 0; i < 3; ++i) {
            scene.fog_ambient[i] = fog.ambient[i] * std::max(sky_share, kLeastSkyShare);
        }
        scene.fog_ambient[3] = fog.start_m;
        scene.fog_range[0] = fog.end_m;
        scene.fog_range[1] = froxel_reach;
        scene.fog_range[2] = kFroxelSliceOffsetM;
        scene.fog_range[3] = static_cast<float>(kFroxelSlices);
        write_wind(lighting.wind, frame, scene);
        if (gpu_assets != nullptr) {
            scene.materials = gpu_assets->material_table_address();
        }
        if (terrain) {
            terrain->extent(scene.terrain_origin_steps, scene.terrain_size_steps);
        }
        if (uint32_t slot = 0; terrain && gpu_assets != nullptr &&
                               terrain->light_map(slot, scene.terrain_origin_steps, scene.terrain_size_steps)) {
            scene.terrain_light[0] = slot;
            scene.terrain_light[1] = 1;
        }
        if (water.sea) {
            scene.sea[0] = static_cast<float>(water.sea_level_m - frame.origin.y);
            scene.sea[1] = 1.0f;
        }
        if (shore_sim_runs() && shore_sim) {
            const DVec3 camera =
                frame.origin + DVec3(frame.camera.transform[3], frame.camera.transform[7], frame.camera.transform[11]);
            const ShoreSimWindow window = shore_sim_window(camera, frame.origin);
            std::copy_n(window.corner, 2, scene.shore_corner);
            std::copy_n(window.corner_cell, 2, scene.shore_corner_cell);
            scene.shore_surface = device->texture_index(shore_sim->surface());
            scene.shore_previous_surface = device->texture_index(shore_sim->previous_surface());
            scene.shore_sim = 1;
        }
        scene.sky_volume_count = write_fog_volumes(mapped, frame, froxel_reach, aspect, sky_factor);
        scene.fog_volumes = device->buffer_address(buffer) + kFogVolumesOffset;
        std::memcpy(mapped.data(), &scene, sizeof(scene));
        if (water.sea || !frame.water_bodies.empty()) {
            const DVec3 camera =
                frame.origin + DVec3(frame.camera.transform[3], frame.camera.transform[7], frame.camera.transform[11]);
            water_levels(camera, frame.origin, water_levels_now);
            std::memcpy(mapped.data() + kWaterLevelsOffset, water_levels_now, sizeof(water_levels_now));
        }
        write_cull_data(mapped, frame, scene, cascades, height, plan);
        device->flush_mapped(buffer, 0, frame_bytes(plan.updates));
        return true;
    }
};

SceneLighting scene_lighting(const asset::EnvironmentAsset& environment) {
    SceneLighting lighting;
    std::copy_n(environment.sun_direction, 3, lighting.sun_direction);
    std::copy_n(environment.sun_color, 3, lighting.sun_color);
    std::copy_n(environment.sky_color, 3, lighting.sky_color);
    std::copy_n(environment.ground_color, 3, lighting.ground_color);
    std::copy_n(environment.shadow_color, 3, lighting.shadow_color);
    lighting.under_roof = environment.under_roof;
    lighting.exposure_ev = environment.exposure_ev;
    lighting.exposure_below_ev = environment.exposure_below_ev;
    lighting.exposure_above_ev = environment.exposure_above_ev;
    lighting.exposure_adaptation = environment.exposure_adaptation;
    lighting.shadows = {.resolution = environment.shadow_resolution,
                        .distance_m = environment.shadow_distance_m,
                        .split_lambda = environment.shadow_split_lambda,
                        .blend_band = environment.shadow_blend_band,
                        .caster_reach_m = environment.shadow_caster_reach_m,
                        .penumbra_per_m = environment.shadow_penumbra_per_m,
                        .max_penumbra_texels = environment.shadow_max_penumbra_texels};
    SceneFog& fog = lighting.fog;
    fog.density_per_m = environment.fog_density_per_m;
    fog.height_density_per_m = environment.fog_height_density_per_m;
    fog.height_falloff_per_m = environment.fog_height_falloff_per_m;
    fog.base_height_m = environment.fog_base_height_m;
    std::copy_n(environment.fog_extinction_tint, 3, fog.extinction_tint);
    std::copy_n(environment.fog_sun_forward, 3, fog.sun_forward);
    std::copy_n(environment.fog_sun_back, 3, fog.sun_back);
    std::copy_n(environment.fog_ambient, 3, fog.ambient);
    fog.sun_height_attenuation = environment.fog_sun_height_attenuation;
    fog.ambient_height_attenuation = environment.fog_ambient_height_attenuation;
    fog.start_m = environment.fog_start_m;
    fog.sun_start_m = environment.fog_sun_start_m;
    fog.end_m = environment.fog_end_m;
    fog.volume_distance_m = environment.fog_volume_distance_m;
    fog.low_camera_keep = environment.fog_low_camera_keep;
    fog.low_camera_below_m = environment.fog_low_camera_below_m;
    fog.low_camera_above_m = environment.fog_low_camera_above_m;
    lighting.wind = {.direction = {environment.wind_direction[0], environment.wind_direction[1]},
                     .speed_mps = environment.wind_speed_mps,
                     .gust_mps = environment.wind_gust_mps,
                     .gust_size_m = environment.wind_gust_size_m,
                     .turbulence = environment.wind_turbulence};
    lighting.sea = {.swell_height_m = environment.sea_swell_height_m,
                    .swell_period_s = environment.sea_swell_period_s,
                    .swell_direction = {environment.sea_swell_direction[0], environment.sea_swell_direction[1]},
                    .fetch_m = environment.sea_fetch_m,
                    .choppiness = environment.sea_choppiness};
    return lighting;
}

const char* scene_view_name(SceneView view) {
    switch (view) {
    case SceneView::Lit:
        return "lit";
    case SceneView::BaseColor:
        return "base color";
    case SceneView::Normal:
        return "normal";
    case SceneView::Roughness:
        return "roughness";
    case SceneView::Metalness:
        return "metalness";
    case SceneView::Occlusion:
        return "occlusion";
    case SceneView::Translucency:
        return "translucency";
    case SceneView::Motion:
        return "motion";
    case SceneView::ShadowMask:
        return "shadow mask";
    case SceneView::Cascades:
        return "shadow cascades";
    case SceneView::TerrainLevel:
        return "terrain levels";
    case SceneView::Traced:
        return "ray traced";
    case SceneView::Indirect:
        return "indirect light";
    case SceneView::AmbientOcclusion:
        return "ambient occlusion";
    case SceneView::Count:
        break;
    }
    return "unknown";
}

void camera_view_proj(const float (&camera)[12], float vertical_fov_rad, float near_m, float aspect, float (&out)[16]) {
    // The camera's rotation (columns of its 3x4) and position; the view is its inverse: the
    // transposed rotation, and minus the position in camera axes.
    const float right[3] = {camera[0], camera[4], camera[8]};
    const float up[3] = {camera[1], camera[5], camera[9]};
    const float back[3] = {camera[2], camera[6], camera[10]};
    const float position[3] = {camera[3], camera[7], camera[11]};
    const auto dot = [](const float (&a)[3], const float (&b)[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const auto length = [&](const float (&a)[3]) { return std::sqrt(dot(a, a)); };
    // Unit axes, in case the camera carries a scale.
    const float rl = 1.0f / std::max(length(right), 1e-6f);
    const float ul = 1.0f / std::max(length(up), 1e-6f);
    const float bl = 1.0f / std::max(length(back), 1e-6f);
    const float view[3][4] = {
        {right[0] * rl, right[1] * rl, right[2] * rl, -dot(right, position) * rl},
        {up[0] * ul, up[1] * ul, up[2] * ul, -dot(up, position) * ul},
        {back[0] * bl, back[1] * bl, back[2] * bl, -dot(back, position) * bl},
    };
    // Infinite reverse-Z perspective looking down -Z: depth = near / -z, 1 at the near plane.
    const float f = 1.0f / std::tan(vertical_fov_rad * 0.5f);
    const float sx = f / std::max(aspect, 1e-6f);
    for (int c = 0; c < 4; ++c) {
        out[0 + c] = sx * view[0][c];
        out[4 + c] = f * view[1][c];
        out[8 + c] = c == 3 ? near_m : 0.0f;
        out[12 + c] = -view[2][c];
    }
}

namespace {

// The planes of what can shadow a slice of the view: the slice swept toward the sun, and a margin around it. A box with
// corners numbered by bits (x, y, and near or far): a face that the sweep moves away from stays, and so does the edge
// between it and one the sweep crosses, as a plane along the sun. A slice's sides aren't parallel, so more of them can
// stay than a box's would, but it's at most 11 planes: and with one left out, what's inside only gets bigger.
uint32_t shadow_caster_planes(const glm::dvec3 (&corners)[8], const glm::dvec3& toward_sun, double margin_m,
                              float (&out)[kShadowCasterPlanes][4]) {
    glm::dvec3 middle(0.0);
    for (const glm::dvec3& corner : corners) {
        middle += corner * 0.125;
    }
    uint32_t count = 0;
    const auto add = [&](glm::dvec3 normal, const glm::dvec3& on_it) {
        const double length = glm::length(normal);
        if (length < 1e-9) {
            return; // an edge along the sun: the faces beside it already bound the sweep
        }
        DE_ASSERT(count < kShadowCasterPlanes);
        if (count == kShadowCasterPlanes) {
            return; // with one left out, what's inside only gets bigger
        }
        normal /= length;
        if (glm::dot(normal, middle - on_it) < 0.0) {
            normal = -normal;
        }
        out[count][0] = static_cast<float>(normal.x);
        out[count][1] = static_cast<float>(normal.y);
        out[count][2] = static_cast<float>(normal.z);
        out[count][3] = static_cast<float>(margin_m - glm::dot(normal, on_it));
        ++count;
    };
    // A face is one bit held, with its corners running over the other two.
    bool stays[3][2] = {};
    for (uint32_t bit = 0; bit < 3; ++bit) {
        const uint32_t next = 1U << ((bit + 1) % 3);
        const uint32_t after = 1U << ((bit + 2) % 3);
        for (uint32_t side = 0; side < 2; ++side) {
            const glm::dvec3& first = corners[side << bit];
            glm::dvec3 normal =
                glm::cross(corners[(side << bit) | next] - first, corners[(side << bit) | after] - first);
            if (glm::dot(normal, middle - first) < 0.0) {
                normal = -normal; // inward
            }
            stays[bit][side] = glm::dot(normal, toward_sun) >= 0.0;
            if (stays[bit][side]) {
                add(normal, first);
            }
        }
    }
    // An edge runs along one bit, with the other two held: those are its two faces.
    for (uint32_t bit = 0; bit < 3; ++bit) {
        const uint32_t next = (bit + 1) % 3;
        const uint32_t after = (bit + 2) % 3;
        for (uint32_t side_next = 0; side_next < 2; ++side_next) {
            for (uint32_t side_after = 0; side_after < 2; ++side_after) {
                if (stays[next][side_next] != stays[after][side_after]) {
                    const glm::dvec3& first = corners[(side_next << next) | (side_after << after)];
                    const glm::dvec3& second = corners[(side_next << next) | (side_after << after) | (1U << bit)];
                    add(glm::cross(second - first, toward_sun), first);
                }
            }
        }
    }
    return count;
}

} // namespace

void fit_shadow_cascades(const float (&camera)[12], const DVec3& origin, float vertical_fov_rad, float near_m,
                         float aspect, const float (&sun_direction)[3], const SceneShadows& shadows,
                         ShadowCascade (&out)[kShadowCascadeCount]) {
    const glm::dvec3 position(camera[3], camera[7], camera[11]);
    const glm::dvec3 forward = -glm::normalize(glm::dvec3(camera[2], camera[6], camera[10]));
    const double tan_y = std::tan(static_cast<double>(vertical_fov_rad) * 0.5);
    const double tan_x = tan_y * aspect;
    // The light's axes: back toward the sun, and any right and up across it.
    const glm::dvec3 sun(sun_direction[0], sun_direction[1], sun_direction[2]);
    const glm::dvec3 back = glm::dot(sun, sun) > 1e-12 ? glm::normalize(sun) : glm::dvec3(0, 1, 0);
    const glm::dvec3 reference = std::abs(back.y) > 0.99 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0);
    const glm::dvec3 right = glm::normalize(glm::cross(reference, back));
    const glm::dvec3 up = glm::cross(back, right);
    const double near = std::max(static_cast<double>(near_m), 0.05);
    const double far = std::max(static_cast<double>(shadows.distance_m), near * 2.0);
    const double lambda = std::clamp(static_cast<double>(shadows.split_lambda), 0.0, 1.0);
    const double blend_band = std::clamp(static_cast<double>(shadows.blend_band), 0.0, 1.0);
    const glm::dvec3 camera_right = glm::normalize(glm::dvec3(camera[0], camera[4], camera[8]));
    const glm::dvec3 camera_up = glm::normalize(glm::dvec3(camera[1], camera[5], camera[9]));
    // Wider than the view: TAA's offset moves it by up to half a pixel, and this covers that down to a view 50 pixels
    // tall.
    constexpr double kViewMargin = 1.02;
    double slice_near = near;
    double before_slice =
        0.0; // where the cascade before starts, as cascade_band in shaders/scene/scene_common.slang has it
    for (uint32_t c = 0; c < kShadowCascadeCount; ++c) {
        // The practical split scheme: a blend of logarithmic and even splits.
        const double t = static_cast<double>(c + 1) / static_cast<double>(kShadowCascadeCount);
        const double slice_far = lambda * near * std::pow(far / near, t) + (1.0 - lambda) * (near + (far - near) * t);
        // The slice's bounding sphere: on the view axis (the corners are symmetric about it), with a
        // radius that depends only on the slice, not on where the camera looks.
        const double diagonal2 = tan_x * tan_x + tan_y * tan_y;
        // Center at distance z on the axis, equidistant from a near and a far corner (clamped inside).
        double z = (slice_far * slice_far * (1.0 + diagonal2) - slice_near * slice_near * (1.0 + diagonal2)) /
                   (2.0 * (slice_far - slice_near));
        z = std::clamp(z, slice_near, slice_far);
        const double radius_near = std::sqrt(slice_near * slice_near * diagonal2 + (z - slice_near) * (z - slice_near));
        const double radius_far = std::sqrt(slice_far * slice_far * diagonal2 + (slice_far - z) * (slice_far - z));
        double radius = std::max(radius_near, radius_far);
        radius = std::ceil(radius * 16.0) / 16.0; // steps of 1/16 m: stable
        const double texel = 2.0 * radius / static_cast<double>(shadows.resolution);
        // The center snapped to the shadow texel grid, in the light's axes: no swimming. The grid is the world's: the
        // origin's step isn't a whole number of texels, so a grid relative to it would jump as it moves.
        glm::dvec3 center = position + forward * z;
        const glm::dvec3 world_center = center + origin;
        const double cx = std::floor(glm::dot(world_center, right) / texel) * texel - glm::dot(origin, right);
        const double cy = std::floor(glm::dot(world_center, up) / texel) * texel - glm::dot(origin, up);
        const double cz = glm::dot(center, back);
        center = right * cx + up * cy + back * cz;
        // Clip space: x, y over the sphere; depth 1 nearest the sun (reverse Z), across the sphere
        // and the reach toward the sun that catches casters outside the view.
        const double depth_range = 2.0 * radius + static_cast<double>(shadows.caster_reach_m);
        const double rows[3][3] = {{right.x / radius, right.y / radius, right.z / radius},
                                   {up.x / radius, up.y / radius, up.z / radius},
                                   {back.x / depth_range, back.y / depth_range, back.z / depth_range}};
        const double offsets[3] = {-glm::dot(right, center) / radius, -glm::dot(up, center) / radius,
                                   (radius - glm::dot(back, center)) / depth_range};
        ShadowCascade& cascade = out[c];
        for (int r = 0; r < 3; ++r) {
            for (int k = 0; k < 3; ++k) {
                cascade.view_proj[r * 4 + k] = static_cast<float>(rows[r][k]);
            }
            cascade.view_proj[r * 4 + 3] = static_cast<float>(offsets[r]);
        }
        cascade.view_proj[12] = 0.0f;
        cascade.view_proj[13] = 0.0f;
        cascade.view_proj[14] = 0.0f;
        cascade.view_proj[15] = 1.0f;
        cascade.far_m = static_cast<float>(slice_far);
        cascade.texel_m = static_cast<float>(texel);
        cascade.depth_per_m = static_cast<float>(1.0 / depth_range);
        // What it's read for starts where the cascade before starts handing pixels on, or for the first, at the eye:
        // fog is read from there, whatever the near plane. The shadow mask reads around a pixel: its search and its
        // taps each reach up to about 1.25 of the widest penumbra, and a pixel's normal offset and bilinear filtering
        // add 3 texels at most.
        const double read_near = c == 0 ? 1e-3 : before_slice + ((slice_near - before_slice) * (1.0 - blend_band));
        glm::dvec3 corners[8];
        for (uint32_t i = 0; i < 8; ++i) {
            const double depth = (i & 4U) != 0 ? slice_far : read_near;
            const double x = ((i & 1U) != 0 ? 1.0 : -1.0) * depth * tan_x * kViewMargin;
            const double y = ((i & 2U) != 0 ? 1.0 : -1.0) * depth * tan_y * kViewMargin;
            corners[i] = position + (forward * depth) + (camera_right * x) + (camera_up * y);
        }
        const double filter_m = texel * (1.25 * std::max(static_cast<double>(shadows.max_penumbra_texels), 1.0) + 3.0);
        cascade.caster_plane_count = shadow_caster_planes(corners, back, filter_m, cascade.caster_planes);
        before_slice = c == 0 ? 0.0 : slice_near;
        slice_near = slice_far;
    }
}

bool invert_4x4(const float (&m)[16], float (&out)[16]) {
    // glm is column-major: element [column][row].
    glm::dmat4 matrix;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            matrix[c][r] = m[r * 4 + c];
        }
    }
    if (std::abs(glm::determinant(matrix)) < 1e-30) {
        return false;
    }
    const glm::dmat4 inverse = glm::inverse(matrix);
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[r * 4 + c] = static_cast<float>(inverse[c][r]);
        }
    }
    return true;
}

core::Result<SceneRenderer> SceneRenderer::create(const SceneRendererDesc& desc) {
    if (desc.device == nullptr || desc.shaders == nullptr || desc.gpu_assets == nullptr) {
        return core::fail(core::ErrorCode::InvalidArgument, "SceneRenderer needs a device, shaders and GpuAssets");
    }
    auto* state =
        ::new (render_heap().allocate(sizeof(SceneRendererState), alignof(SceneRendererState))) SceneRendererState();
    state->device = desc.device;
    state->shaders = desc.shaders;
    state->gpu_assets = desc.gpu_assets;
    state->max_instances = desc.max_instances;
    state->mesh_instances.resize(desc.gpu_assets->mesh_kinds().size(), 0);
    SceneRenderer renderer(state);
    if (desc.uploader != nullptr && desc.vfs != nullptr) {
        core::Result<Terrain> terrain = Terrain::create(
            {.device = desc.device, .uploader = desc.uploader, .vfs = desc.vfs, .gpu_assets = desc.gpu_assets});
        if (!terrain) {
            return core::fail(terrain.error());
        }
        state->terrain.emplace(std::move(*terrain));
    }
    const core::Result<rhi::BufferHandle> rows =
        desc.device->create_buffer({.size = uint64_t{desc.max_instances} * kInstanceBytes,
                                    .usage = rhi::BufferUsage::Storage,
                                    .debug_name = "instance data"});
    if (!rows) {
        return core::fail(rows.error());
    }
    state->instance_data = *rows;
    state->instance_data_address = desc.device->buffer_address(*rows);
    const core::Result<rhi::BufferHandle> meshes =
        desc.device->create_buffer({.size = uint64_t{desc.max_instances} * sizeof(uint32_t),
                                    .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst,
                                    .debug_name = "instance meshes"});
    if (!meshes) {
        return core::fail(meshes.error());
    }
    state->instance_meshes = *meshes;
    state->instance_meshes_address = desc.device->buffer_address(*meshes);
    if (!state->ensure_part_keys(kFirstPartKeyParts)) {
        return core::fail(core::ErrorCode::OutOfMemory, "the cull's part keys couldn't be made");
    }
    const core::Result<rhi::BufferHandle> part_dispatch = desc.device->create_buffer(
        {.size = 3 * sizeof(uint32_t),
         .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect | rhi::BufferUsage::CopyDst,
         .debug_name = "parts dispatch"});
    if (!part_dispatch) {
        return core::fail(part_dispatch.error());
    }
    state->part_dispatch = *part_dispatch;
    state->part_dispatch_address = desc.device->buffer_address(*part_dispatch);
    if (desc.uploader != nullptr) {
        // The water's mesh: one index buffer for every level (water.h). Without an uploader, no water.
        const core::Array<uint16_t> indices = water_mesh_indices(render_heap());
        const core::Result<rhi::BufferHandle> water_indices =
            desc.device->create_buffer({.size = indices.size() * sizeof(uint16_t),
                                        .usage = rhi::BufferUsage::Index | rhi::BufferUsage::CopyDst,
                                        .debug_name = "water indices"});
        if (!water_indices) {
            return core::fail(water_indices.error());
        }
        state->water_indices = *water_indices;
        if (const core::Result<void> queued = desc.uploader->upload_buffer(
                state->water_indices, 0, std::as_bytes(indices.span()), rhi::ResourceState::IndexBuffer,
                rhi::ShaderStages::All, rhi::BufferRange::Unread);
            !queued) {
            return core::fail(queued.error());
        }
        state->water_index_ticket = desc.uploader->buffer_upload_ticket();
        state->uploader = desc.uploader;
        core::Result<Ocean> ocean = Ocean::create(*desc.device, *desc.shaders);
        if (!ocean) {
            return core::fail(ocean.error());
        }
        state->ocean.emplace(std::move(*ocean));
        core::Result<ShoreSim> shore_sim = ShoreSim::create(*desc.device, *desc.shaders);
        if (!shore_sim) {
            return core::fail(shore_sim.error());
        }
        state->shore_sim.emplace(std::move(*shore_sim));
    }
    if (desc.assets != nullptr) {
        core::Result<ParticleSystem> particles = ParticleSystem::create({.device = desc.device,
                                                                         .shaders = desc.shaders,
                                                                         .assets = desc.assets,
                                                                         .pool_blocks = desc.particle_pool_blocks,
                                                                         .catch_up_steps = desc.particle_catch_up_steps,
                                                                         .color_format = kHdrFormat,
                                                                         .coverage_format = kCoverageFormat,
                                                                         .depth_format = kDepthFormat});
        if (!particles) {
            return core::fail(particles.error());
        }
        state->particles.emplace(std::move(*particles));
    }
    if (desc.ray_tracing) {
        core::Result<TraceScene> trace =
            TraceScene::create({.device = desc.device,
                                .shaders = desc.shaders,
                                .gpu_assets = desc.gpu_assets,
                                .assets = desc.assets,
                                .max_instances = desc.max_instances,
                                .max_meshes = static_cast<uint32_t>(desc.gpu_assets->mesh_kinds().size())});
        if (trace) {
            state->trace.emplace(std::move(*trace));
            // Light that has bounced is baked by tracing: only with the above.
            core::Result<LightBoxes> boxes = LightBoxes::create({.device = desc.device, .shaders = desc.shaders});
            if (boxes) {
                state->light_boxes.emplace(std::move(*boxes));
            }
            core::Result<PathTracer> path_tracer =
                PathTracer::create({.device = desc.device, .shaders = desc.shaders, .hdr_format = kHdrFormat});
            if (path_tracer) {
                state->path_tracer.emplace(std::move(*path_tracer));
            }
        } else {
            DE_LOG_INFO("render", "no ray tracing: %s", trace.error().message);
        }
    }
    core::Result<AutoExposure> exposure = AutoExposure::create({.device = desc.device, .shaders = desc.shaders});
    if (!exposure) {
        return core::fail(exposure.error());
    }
    state->auto_exposure.emplace(std::move(*exposure));
    for (rhi::BufferHandle& buffer : state->exposure_readback) {
        const core::Result<rhi::BufferHandle> made =
            desc.device->create_buffer({.size = sizeof(GpuExposureState),
                                        .usage = rhi::BufferUsage::CopyDst,
                                        .memory = rhi::MemoryLocation::Readback,
                                        .debug_name = "exposure readback"});
        if (!made) {
            return core::fail(made.error());
        }
        buffer = *made;
    }
    core::Result<Gtao> gtao = Gtao::create({.device = desc.device, .shaders = desc.shaders});
    if (!gtao) {
        return core::fail(gtao.error());
    }
    state->gtao.emplace(std::move(*gtao));
    core::Result<RadianceCascades> cascades =
        RadianceCascades::create({.device = desc.device, .shaders = desc.shaders});
    if (!cascades) {
        return core::fail(cascades.error());
    }
    state->radiance_cascades.emplace(std::move(*cascades));
    core::Result<LocalLights> local_lights = LocalLights::create({.device = desc.device, .shaders = desc.shaders});
    if (!local_lights) {
        return core::fail(local_lights.error());
    }
    state->local_lights.emplace(std::move(*local_lights));
    state->make_scene_programs();
    return renderer;
}

SceneRenderer::~SceneRenderer() {
    if (state_ == nullptr) {
        return;
    }
    state_->~SceneRendererState();
    render_heap().deallocate(state_, sizeof(SceneRendererState), alignof(SceneRendererState));
}

SceneRenderer::SceneRenderer(SceneRenderer&& other) noexcept : state_(std::exchange(other.state_, nullptr)) {}

SceneRenderer& SceneRenderer::operator=(SceneRenderer&& other) noexcept {
    if (this != &other) {
        const SceneRenderer discarded(std::exchange(state_, std::exchange(other.state_, nullptr)));
    }
    return *this;
}

void SceneRenderer::set_water(const SceneWater& water) {
    state_->water = water;
}

void SceneRenderer::add_passes(RenderGraph& graph, GraphTexture target, rhi::Format target_format,
                               const world::RenderFrame& frame, const SceneLighting& lighting, SceneView view, bool taa,
                               rhi::Color empty) {
    DE_PROFILE_ZONE("scene passes setup");
    SceneRendererState& state = *state_;
    // An environment asset is within the limits already (validated when loaded); lighting made in code
    // is held to them here: the resolution sizes the atlas (twice it), the penumbra a search.
    using Limits = asset::ShadowLimits;
    state.shadows = lighting.shadows;
    state.shadows.resolution = std::clamp(state.shadows.resolution, Limits::kMinResolution, Limits::kMaxResolution);
    state.shadows.max_penumbra_texels =
        std::clamp(state.shadows.max_penumbra_texels, Limits::kMinPenumbraTexels, Limits::kMaxPenumbraTexels);
    state.make_tonemap(target_format);
    state.stats = {};
    state.read_cull_stats();
#if DE_BUILD_DEV
    state.read_pick();
    if (state.editor.enabled) {
        state.make_editor_programs(target_format);
    }
#endif
    // A frame not drawn left a gap in the numbers (hand-made frames have no numbers).
    state.instances_stale_now = frame.number != 0 && frame.number != state.last_frame_number + 1;
    if (state.waiting_for_mesh_slot && state.gpu_assets->mesh_slots_released() != state.mesh_slots_released_seen) {
        state.waiting_for_mesh_slot = false;
        state.instances_stale_now = true; // they're sent again, next frame
    }
    state.last_frame_number = frame.number;
    const rhi::PipelineHandle gbuffer_opaque = state.shaders->pipeline(state.gbuffer_opaque);
    const rhi::PipelineHandle gbuffer_masked = state.shaders->pipeline(state.gbuffer_masked);
    const rhi::PipelineHandle gbuffer_opaque_fading = state.shaders->pipeline(state.gbuffer_opaque_fading);
    const rhi::PipelineHandle gbuffer_masked_fading = state.shaders->pipeline(state.gbuffer_masked_fading);
    const rhi::PipelineHandle light = state.shaders->pipeline(state.deferred_light);
    const rhi::PipelineHandle tonemap = state.shaders->pipeline(state.tonemap);
    const rhi::PipelineHandle caster_opaque = state.shaders->pipeline(state.shadow_caster_opaque);
    const rhi::PipelineHandle caster_masked = state.shaders->pipeline(state.shadow_caster_masked);
    const rhi::PipelineHandle mask = state.shaders->pipeline(state.shadow_mask);
    const rhi::PipelineHandle fog = state.shaders->pipeline(state.fog_program);
    const rhi::PipelineHandle cull = state.shaders->pipeline(state.cull);
    const rhi::PipelineHandle cull_assign = state.shaders->pipeline(state.cull_assign);
    const rhi::PipelineHandle instance_update = state.shaders->pipeline(state.instance_update);
    const rhi::PipelineHandle fog_inject = state.shaders->pipeline(state.fog_inject);
    const rhi::PipelineHandle fog_integrate = state.shaders->pipeline(state.fog_integrate);
    const rhi::PipelineHandle terrain_gbuffer = state.shaders->pipeline(state.terrain_gbuffer);
    const rhi::PipelineHandle terrain_shadow = state.shaders->pipeline(state.terrain_shadow);
    const rhi::PipelineHandle terrain_select = state.shaders->pipeline(state.terrain_select);
    const rhi::PipelineHandle grass_gbuffer = state.shaders->pipeline(state.grass_gbuffer);
    const rhi::PipelineHandle grass_scatter = state.shaders->pipeline(state.grass_scatter);
    // Fog only in the lit view, if there is any; if its shaders are broken, no fog rather than no
    // scene. The grid likewise (the closed form goes on alone). What it adds over the closed form is
    // the sun's shadows, so it reaches no farther than they do.
    // Fog volumes alone keep it on only where they can be seen.
    bool volume_in_view = false;
    const bool world_fog = lighting.fog.density_per_m > 0.0f || lighting.fog.height_density_per_m > 0.0f;
    if (view == SceneView::Lit && !world_fog && lighting.fog.volume_distance_m > 0.0f) {
        const float reach = std::min(lighting.fog.volume_distance_m, state.shadows.distance_m);
        const float view_aspect =
            static_cast<float>(graph.reference_width()) / static_cast<float>(std::max(graph.reference_height(), 1U));
        const SceneRendererState::FogVolumeCamera volume_camera =
            SceneRendererState::fog_volume_camera(frame, view_aspect);
        for (const world::RenderFogVolume& volume : frame.fog_volumes) {
            if (SceneRendererState::fog_volume_view(volume_camera, volume, reach).visible) {
                volume_in_view = true;
                break;
            }
        }
    }
    const bool foggy = view == SceneView::Lit && fog.is_valid() && (world_fog || volume_in_view);
    const bool froxels_on =
        foggy && lighting.fog.volume_distance_m > 0.0f && fog_inject.is_valid() && fog_integrate.is_valid();
    const float froxel_reach = froxels_on ? std::min(lighting.fog.volume_distance_m, state.shadows.distance_m) : 0.0f;
    state.stats.froxel_reach_m = froxel_reach;
    // TAA in the lit view; without its shader or its textures, the frame goes on without it.
    const rhi::PipelineHandle taa_pipeline = state.shaders->pipeline(state.taa_program);
    const uint32_t width = graph.reference_width();
    const uint32_t height = graph.reference_height();
    // Path traced, the lit picture is the path tracer's: no TAA, probes or GTAO under it.
    const bool path_traced = state.path_traced && state.trace && state.path_tracer && view == SceneView::Lit;
    // Compared, the rasterized picture is drawn as always and the path tracer adds up hidden behind it.
    const bool compared =
        !path_traced && state.lighting_compare && state.trace && state.path_tracer && view == SceneView::Lit;
    const bool taa_on = taa && view == SceneView::Lit && !path_traced && !compared && taa_pipeline.is_valid() &&
                        state.ensure_temporal(width, height);
    if (taa_on) {
        state.taa_sample = state.taa_sample % kTaaSamples + 1;
    }
    // The frame's local lights, for its constants: those that reach what's in view.
    const float light_aspect = static_cast<float>(width) / static_cast<float>(std::max(height, 1U));
    const bool casters_all_changed = state.local_shadow_changes(frame);
    state.lights_now = state.local_lights
                           ? state.local_lights->update(frame, light_aspect, state.shadow_changes, casters_all_changed)
                           : LocalLightFrame{};
    state.stats.lights = state.local_lights ? state.local_lights->stats() : LocalLightStats{};
    // This frame's constants and draws, now: every pass below draws only if they're in.
    const rhi::BufferHandle frame_buffer =
        frame.camera.valid ? state.frame_buffer(frame_bytes(frame.instances.size() + frame.freed_slots.size()))
                           : rhi::BufferHandle{};
    CullPlan plan;
    // The light boxes this frame (path traced, none: the path tracer's light has bounced already). Their bake waits
    // for what rays meet to be all there.
    state.light_boxes_now = {};
    if (state.light_boxes && state.trace && state.light_boxes_on && !path_traced) {
        const TraceStats traced = state.trace->stats();
        state.light_boxes_now = state.light_boxes->update(frame.origin, traced.waiting == 0 && traced.instances > 0);
    }
    state.stats.light_boxes = state.light_boxes ? state.light_boxes->stats() : LightBoxStats{};
    const bool written =
        frame_buffer.is_valid() && state.write_frame(frame_buffer, plan, frame, lighting, froxel_reach,
                                                     taa_on ? state.taa_sample : state.debug_jitter, width, height);
    const bool ready = written && gbuffer_opaque.is_valid() && gbuffer_masked.is_valid() &&
                       gbuffer_opaque_fading.is_valid() && gbuffer_masked_fading.is_valid() && light.is_valid() &&
                       tonemap.is_valid() && caster_opaque.is_valid() && caster_masked.is_valid() && mask.is_valid() &&
                       cull.is_valid() && cull_assign.is_valid() && instance_update.is_valid();
    if (!ready) {
        // Nothing to look through, or shaders missing: the target is cleared, and this frame's changes are lost.
        state.instances_stale_now = true;
        graph.add_pass(
            "scene (cleared)", [&](PassBuilder& pass) { pass.use(target, rhi::ResourceState::RenderTarget); },
            [target, empty](PassContext& context) {
                const rhi::ColorAttachment color{.texture = context.texture(target), .clear = empty};
                context.list.begin_rendering({.colors = std::span(&color, 1)});
                context.list.end_rendering();
            });
        return;
    }
    const uint64_t frame_address = state.device->buffer_address(frame_buffer);
    const uint32_t cascade_size = state.shadows.resolution;
    // What rays see: bottom levels for the meshes this frame's instances asked for, and this frame's scene.
    if (state.trace) {
        state.trace->update(state.instance_data_address, state.instance_meshes_address, state.mesh_instances.span());
    }
    // Which local lights reach each cluster of the view: before anything is lit. Invalid: none this frame.
    const GraphBuffer light_clusters =
        state.local_lights ? state.local_lights->add_cull_pass(graph, frame_address, frame, light_aspect)
                           : GraphBuffer{};
    state.stats.instances = state.live_instances;
    state.stats.instance_updates = plan.updates;
    state.stats.drawn = plan.drawn;
    state.stats.draws = plan.draws;
    state.stats.shadow_draws = plan.shadow_draws;

    // The GPU cull: what's in each view, into the draw lists the G-buffer and shadow passes draw. Its part keys have
    // room for every part loaded.
    if (!state.ensure_part_keys(state.gpu_assets->part_table_used())) {
        DE_LOG_ERROR("render", "the cull's part keys couldn't grow to %u parts", state.gpu_assets->part_table_used());
    }
    const GraphBuffer part_keys_graph = graph.import_buffer(
        {.buffer = state.part_keys,
         .name = "part keys",
         .state = state.part_buffers_read ? rhi::ResourceState::ShaderWrite : rhi::ResourceState::Undefined,
         .final_state = rhi::ResourceState::ShaderWrite});
    const GraphBuffer part_used_graph = graph.import_buffer(
        {.buffer = state.part_used,
         .name = "parts in use",
         .state = state.part_buffers_read ? rhi::ResourceState::ShaderRead : rhi::ResourceState::Undefined,
         .final_state = rhi::ResourceState::ShaderRead});
    const GraphBuffer part_dispatch_graph = graph.import_buffer(
        {.buffer = state.part_dispatch,
         .name = "parts dispatch",
         .state = state.part_buffers_read ? rhi::ResourceState::IndirectArgument : rhi::ResourceState::Undefined,
         .final_state = rhi::ResourceState::IndirectArgument});
    state.part_buffers_read = true;
    struct CullClearPass {
        SceneRendererState* state = nullptr;
        GraphBuffer counts;
    };
    const auto& cull_clear = graph.add_pass<CullClearPass>(
        "cull clear",
        [&](PassBuilder& pass, CullClearPass& data) {
            data.state = &state;
            data.counts = pass.create_buffer({.size = kCullCounterCount * sizeof(uint32_t),
                                              .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect |
                                                       rhi::BufferUsage::CopyDst | rhi::BufferUsage::CopySrc,
                                              .debug_name = "draw counts"});
            pass.use(data.counts, rhi::ResourceState::CopyDst);
            pass.use(part_keys_graph, rhi::ResourceState::CopyDst);
            pass.use(part_dispatch_graph, rhi::ResourceState::CopyDst);
        },
        [part_keys_graph, part_dispatch_graph](const CullClearPass& data, PassContext& context) {
            context.list.fill_buffer(context.buffer(data.counts), 0, kCullCounterCount * sizeof(uint32_t), 0);
            if (!data.state->part_keys_zero) {
                context.list.fill_buffer(context.buffer(part_keys_graph), 0,
                                         uint64_t{kCullGroups} * data.state->part_key_parts * sizeof(uint32_t), 0);
            }
            data.state->part_keys_zero = false; // until the frame gets as far as putting them back
            // None so far, one by one: a dispatch of 1 group for each.
            context.list.fill_buffer(context.buffer(part_dispatch_graph), 0, sizeof(uint32_t), 0);
            context.list.fill_buffer(context.buffer(part_dispatch_graph), sizeof(uint32_t), 2 * sizeof(uint32_t), 1);
        });
    const GraphBuffer draw_counts = cull_clear.counts;

    // What changed in the instances. They stay between frames, so the graph is told how they were left.
    const GraphBuffer instance_data_graph = graph.import_buffer(
        {.buffer = state.instance_data,
         .name = "instance data",
         .state = state.instance_buffers_read ? rhi::ResourceState::ShaderRead : rhi::ResourceState::Undefined,
         .final_state = rhi::ResourceState::ShaderRead});
    const GraphBuffer instance_meshes_graph = graph.import_buffer(
        {.buffer = state.instance_meshes,
         .name = "instance meshes",
         .state = state.instance_buffers_read ? rhi::ResourceState::ShaderRead : rhi::ResourceState::Undefined,
         .final_state = rhi::ResourceState::ShaderRead});
    state.instance_buffers_read = true;
    if (plan.clear_first < plan.slots) {
        graph.add_pass(
            "instances clear", [&](PassBuilder& pass) { pass.use(instance_meshes_graph, rhi::ResourceState::CopyDst); },
            [instance_meshes_graph, first = uint64_t{plan.clear_first} * sizeof(uint32_t),
             bytes = uint64_t{plan.slots - plan.clear_first} * sizeof(uint32_t)](PassContext& context) {
                context.list.fill_buffer(context.buffer(instance_meshes_graph), first, bytes, kNoMesh);
            });
    }
    state.cleared_slots = plan.slots;
    if (plan.updates > 0) {
        graph.add_pass(
            "instance update",
            [&](PassBuilder& pass) {
                pass.use(instance_data_graph, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
                pass.use(instance_meshes_graph, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            },
            [renderer = &state, pipeline = instance_update, frame_address,
             updates = plan.updates](PassContext& context) {
                DE_PROFILE_ZONE("instance update pass");
                const InstanceUpdatePush push{.updates = frame_address + kInstanceUpdatesOffset,
                                              .instances = renderer->instance_data_address,
                                              .instance_meshes = renderer->instance_meshes_address,
                                              .count = updates};
                context.list.bind_pipeline(pipeline);
                context.list.push_constants(push);
                context.list.dispatch((updates + 63) / 64);
            });
    }
    // The top level over the instances as they are now, for whatever traces this frame.
    if (state.trace) {
        state.trace->add_passes(graph, {.frame_address = frame_address,
                                        .instance_data = instance_data_graph,
                                        .instance_meshes = instance_meshes_graph,
                                        .instance_data_address = state.instance_data_address,
                                        .instance_meshes_address = state.instance_meshes_address,
                                        .slots = plan.slots});
        state.stats.trace = state.trace->stats();
    }
    // The light boxes' bake, against that top level, and their maps for the light.
    std::span<const GraphTexture> box_maps;
    if (state.light_boxes_now.count > 0) {
        box_maps = state.light_boxes->add_passes(graph, frame_address, state.stats.lights.lit);
    }
    // The cull counts what's in each view, each part in use gets its draw and its records, the cull writes them, and
    // what was used goes back to 0.
    struct CullPass {
        SceneRendererState* state = nullptr;
        GraphBuffer counts;
        GraphBuffer draws;
        rhi::PipelineHandle pipeline;
        uint64_t frame_address = 0;
        CullPlan plan;
        uint32_t part_count = 0;
    };
    const auto run_cull = [](const CullPass& data, PassContext& context, uint32_t writing) {
        const SceneRendererState& renderer = *data.state;
        // Before the mesh table holds anything, there's nothing to cull against.
        if (data.plan.slots == 0 || !renderer.gpu_assets->mesh_table_ready()) {
            return;
        }
        const uint64_t draws = data.draws.is_valid() ? renderer.device->buffer_address(context.buffer(data.draws)) : 0;
        const CullPush push{.frame = data.frame_address,
                            .instances = renderer.instance_data_address,
                            .instance_meshes = renderer.instance_meshes_address,
                            .meshes = renderer.gpu_assets->mesh_table_address(),
                            .lods = renderer.gpu_assets->lod_table_address(),
                            .parts = renderer.gpu_assets->part_table_address(),
                            .views = data.frame_address + kCullViewsOffset,
                            .records = draws + data.plan.records_offset(),
                            .part_keys = renderer.part_keys_address,
                            .part_used = renderer.part_used_address,
                            .part_dispatch = renderer.part_dispatch_address,
                            .instance_count = data.plan.slots,
                            .part_count = data.part_count,
                            .writing = writing};
        context.list.bind_pipeline(data.pipeline);
        context.list.push_constants(push);
        context.list.dispatch((data.plan.slots + 63) / 64);
    };
    const auto run_assign = [part_dispatch_graph](const CullPass& data, PassContext& context, uint32_t resetting) {
        const SceneRendererState& renderer = *data.state;
        if (data.plan.slots == 0 || !renderer.gpu_assets->mesh_table_ready()) {
            return;
        }
        const CullAssignPush push{
            .parts = renderer.gpu_assets->part_table_address(),
            .materials = renderer.gpu_assets->material_table_address(),
            .lists = data.frame_address + kDrawListsOffset,
            .args = data.draws.is_valid() ? renderer.device->buffer_address(context.buffer(data.draws)) : 0,
            .counts = data.counts.is_valid() ? renderer.device->buffer_address(context.buffer(data.counts)) : 0,
            .part_keys = renderer.part_keys_address,
            .part_used = renderer.part_used_address,
            .part_count = data.part_count,
            .record_capacity = static_cast<uint32_t>(std::min<uint64_t>(data.plan.record_capacity, UINT32_MAX)),
            .merged = renderer.merged_draws ? 1U : 0U,
            .resetting = resetting};
        context.list.bind_pipeline(data.pipeline);
        context.list.push_constants(push);
        context.list.dispatch_indirect(context.buffer(part_dispatch_graph), 0);
    };
    const CullPass cull_base{.state = &state,
                             .counts = {},
                             .draws = {},
                             .pipeline = cull,
                             .frame_address = frame_address,
                             .plan = plan,
                             .part_count = state.part_key_parts};
    graph.add_pass<CullPass>(
        "gpu cull count",
        [&](PassBuilder& pass, CullPass& data) {
            data = cull_base;
            pass.use(part_keys_graph, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            pass.use(part_used_graph, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            pass.use(part_dispatch_graph, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            pass.use(instance_data_graph, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
            pass.use(instance_meshes_graph, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
        },
        [run_cull](const CullPass& data, PassContext& context) {
            DE_PROFILE_ZONE("gpu cull count pass");
            run_cull(data, context, 0);
        });
    const auto& cull_pass = graph.add_pass<CullPass>(
        "gpu cull assign",
        [&](PassBuilder& pass, CullPass& data) {
            data = cull_base;
            data.pipeline = cull_assign;
            data.counts = draw_counts;
            data.draws = pass.create_buffer({.size = plan.buffer_bytes(),
                                             .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect,
                                             .debug_name = "draw lists"});
            pass.use(data.draws, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            pass.use(draw_counts, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            pass.use(part_keys_graph, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            pass.use(part_used_graph, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
            pass.use(part_dispatch_graph, rhi::ResourceState::IndirectArgument);
        },
        [run_assign](const CullPass& data, PassContext& context) {
            DE_PROFILE_ZONE("gpu cull assign pass");
            run_assign(data, context, 0);
        });
    graph.add_pass<CullPass>(
        "gpu cull write",
        [&](PassBuilder& pass, CullPass& data) {
            data = cull_base;
            data.draws = cull_pass.draws;
            pass.use(data.draws, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            pass.use(part_keys_graph, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            pass.use(instance_data_graph, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
            pass.use(instance_meshes_graph, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
        },
        [run_cull](const CullPass& data, PassContext& context) {
            DE_PROFILE_ZONE("gpu cull write pass");
            run_cull(data, context, 1);
        });
    graph.add_pass<CullPass>(
        "gpu cull reset",
        [&](PassBuilder& pass, CullPass& data) {
            data = cull_base;
            data.pipeline = cull_assign;
            pass.use(part_keys_graph, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            pass.use(part_used_graph, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
            pass.use(part_dispatch_graph, rhi::ResourceState::IndirectArgument);
        },
        [run_assign](const CullPass& data, PassContext& context) {
            DE_PROFILE_ZONE("gpu cull reset pass");
            run_assign(data, context, 1);
            data.state->part_keys_zero = true;
        });
    const GraphBuffer draw_lists = cull_pass.draws;

    // TAA's textures this frame: this frame's depth, object motion and result are kept for the next; last
    // frame's are read. History needs last frame to have been drawn here, and no camera cut.
    const bool has_history = taa_on && frame.camera.previous_valid && state.temporal_frame + 1 == frame.number &&
                             state.temporal[state.temporal_current].written;
    GraphTexture kept_depth;
    GraphTexture kept_object_motion;
    GraphTexture kept_history;
    GraphTexture last_depth;
    GraphTexture last_object_motion;
    GraphTexture last_history;
    if (taa_on) {
        const SceneRendererState::TemporalSet& last = state.temporal[state.temporal_current];
        state.temporal_current ^= 1U;
        SceneRendererState::TemporalSet& now = state.temporal[state.temporal_current];
        const rhi::ResourceState was = now.written ? rhi::ResourceState::ShaderRead : rhi::ResourceState::Undefined;
        const auto keep = [&](rhi::TextureHandle texture, const char* name) {
            return graph.import_texture({.texture = texture,
                                         .name = name,
                                         .state = was,
                                         .final_state = rhi::ResourceState::ShaderRead,
                                         .discard = true});
        };
        kept_depth = keep(now.depth, "scene depth");
        kept_object_motion = keep(now.object_motion, "taa object motion");
        kept_history = keep(now.history, "taa history");
        if (has_history) {
            const auto read = [&](rhi::TextureHandle texture, const char* name) {
                return graph.import_texture({.texture = texture,
                                             .name = name,
                                             .state = rhi::ResourceState::ShaderRead,
                                             .final_state = rhi::ResourceState::ShaderRead});
            };
            last_depth = read(last.depth, "last depth");
            last_object_motion = read(last.object_motion, "last object motion");
            last_history = read(last.history, "last taa history");
        }
        now.written = true; // the graph leaves it in ShaderRead, even if its passes are skipped
    }

    // The ground: streamed by where the camera is, its patches picked before anything draws them.
    TerrainFrame terrain_frame;
    if (state.terrain) {
        const DVec3 camera_world = frame.origin + DVec3(static_cast<double>(frame.camera.transform[3]),
                                                        static_cast<double>(frame.camera.transform[7]),
                                                        static_cast<double>(frame.camera.transform[11]));
        state.terrain->update(camera_world);
        state.stats.terrain = state.terrain->stats();
        terrain_frame =
            state.terrain->add_select_passes(graph, terrain_select, frame_address, frame_address + kCullViewsOffset);
        if (view == SceneView::Lit) {
            state.terrain->add_grass_passes(graph, grass_scatter, frame_address, frame_address + kCullViewsOffset,
                                            terrain_frame);
        }
    }

    // The sun's shadow atlas: four cascades, each a tile, every instance drawn into each.
    struct ShadowPass {
        SceneRendererState* state = nullptr;
        TerrainFrame terrain;
        rhi::PipelineHandle terrain_pipeline;
        GraphTexture atlas;
        rhi::PipelineHandle opaque;
        rhi::PipelineHandle masked;
        uint64_t frame_address = 0;
        GraphBuffer draws;
        GraphBuffer counts;
        CullPlan plan;
        uint32_t cascade_size = 0;
    };
    const auto& shadow = graph.add_pass<ShadowPass>(
        "sun shadows",
        [&](PassBuilder& pass, ShadowPass& data) {
            data = {.state = &state,
                    .terrain = terrain_frame,
                    .terrain_pipeline = terrain_shadow,
                    .atlas = {},
                    .opaque = caster_opaque,
                    .masked = caster_masked,
                    .frame_address = frame_address,
                    .draws = draw_lists,
                    .counts = draw_counts,
                    .plan = plan,
                    .cascade_size = cascade_size};
            pass.use(draw_lists, rhi::ResourceState::IndirectArgument | rhi::ResourceState::ShaderRead,
                     rhi::ShaderStages::Vertex);
            pass.use(draw_counts, rhi::ResourceState::IndirectArgument);
            data.atlas = pass.create_texture({.desc = {.width = cascade_size * 2,
                                                       .height = cascade_size * 2,
                                                       .format = kDepthFormat,
                                                       .debug_name = "shadow atlas"}});
            pass.use(data.atlas, rhi::ResourceState::DepthWrite);
            pass.use(instance_data_graph, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Vertex);
            if (state.terrain) {
                state.terrain->use_draws(pass, terrain_frame);
            }
        },
        [](const ShadowPass& data, PassContext& context) {
            DE_PROFILE_ZONE("shadow pass");
            SceneRendererState& renderer = *data.state;
            const rhi::BufferHandle args = context.buffer(data.draws);
            const rhi::BufferHandle counts = context.buffer(data.counts);
            ShadowCasterPush push{.frame = data.frame_address,
                                  .instances = renderer.instance_data_address,
                                  .vertices = renderer.gpu_assets->pool_vertex_address(),
                                  .materials = renderer.gpu_assets->material_table_address(),
                                  .draws = renderer.device->buffer_address(context.buffer(data.draws)) +
                                           data.plan.records_offset(),
                                  .sampler = rhi::sampler_index(rhi::DefaultSampler::AnisotropicWrap)};
            for (uint32_t cascade = 0; cascade < kShadowCascadeCount; ++cascade) {
                const rhi::DepthAttachment depth{.texture = context.texture(data.atlas)};
                const rhi::Rect tile{.x = static_cast<int32_t>((cascade & 1U) * data.cascade_size),
                                     .y = static_cast<int32_t>((cascade >> 1U) * data.cascade_size),
                                     .width = data.cascade_size,
                                     .height = data.cascade_size};
                context.list.begin_rendering({.depth = &depth, .area = tile});
                if (renderer.terrain && !renderer.terrain_hidden) {
                    renderer.terrain->draw_shadow(context, data.terrain, data.terrain_pipeline, data.frame_address,
                                                  cascade);
                }
                context.list.bind_index_buffer(renderer.gpu_assets->pool_index_buffer(), 0, rhi::IndexType::Uint32);
                push.cascade = cascade;
                context.list.push_constants(push);
                // Casters don't cull back faces (thin geometry casts from both sides): the kinds differ
                // only by the alpha test.
                for (const bool masked : {false, true}) {
                    const uint32_t index = cascade_list(cascade, masked);
                    if (data.plan.lists[index].capacity > 0) {
                        context.list.bind_pipeline(masked ? data.masked : data.opaque);
                        renderer.draw_list(context.list, args, counts, data.plan, index);
                    }
                }
                context.list.end_rendering();
            }
        });
    const GraphTexture atlas = shadow.atlas;

    // The local lights' shadows: each face to draw again this frame into its tile of the local shadow atlas, which is
    // kept from frame to frame (the tiles of lights in whose reach nothing changed hold what they held).
    GraphTexture local_atlas;
    if (state.lights_now.face_count > 0 && !state.local_atlas.is_valid()) {
        const core::Result<rhi::TextureHandle> kept_atlas =
            state.device->create_texture({.width = kLocalShadowAtlasWidth,
                                          .height = kLocalShadowAtlasHeight,
                                          .format = kDepthFormat,
                                          .usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::Sampled,
                                          .debug_name = "local shadow atlas"});
        state.local_atlas = kept_atlas ? *kept_atlas : rhi::TextureHandle{};
        state.local_atlas_used = false;
    }
    if (state.lights_now.face_count > 0 && state.local_atlas.is_valid()) {
        const uint32_t faces = state.lights_now.face_count;
        uint32_t to_draw = 0;
        for (uint32_t f = 0; f < faces; ++f) {
            to_draw += state.lights_now.faces[f].draw ? 1U : 0U;
        }
        local_atlas = graph.import_texture(
            {.texture = state.local_atlas,
             .name = "local shadow atlas",
             .state = state.local_atlas_used ? rhi::ResourceState::ShaderRead : rhi::ResourceState::Undefined,
             .final_state = rhi::ResourceState::ShaderRead});
        state.local_atlas_used = true;
        if (to_draw > 0) {
            graph.add_pass(
                "local shadows",
                [&](PassBuilder& pass) {
                    pass.use(draw_lists, rhi::ResourceState::IndirectArgument | rhi::ResourceState::ShaderRead,
                             rhi::ShaderStages::Vertex);
                    pass.use(draw_counts, rhi::ResourceState::IndirectArgument);
                    pass.use(local_atlas, rhi::ResourceState::DepthWrite);
                    pass.use(instance_data_graph, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Vertex);
                },
                [&state, faces, frame_address, caster_opaque, caster_masked, draws = draw_lists, counts = draw_counts,
                 plan, local_atlas, tiles = state.lights_now.faces](PassContext& context) {
                    DE_PROFILE_ZONE("local shadows pass");
                    const rhi::BufferHandle args = context.buffer(draws);
                    const rhi::BufferHandle count_buffer = context.buffer(counts);
                    ShadowCasterPush push{.frame = frame_address,
                                          .instances = state.instance_data_address,
                                          .vertices = state.gpu_assets->pool_vertex_address(),
                                          .materials = state.gpu_assets->material_table_address(),
                                          .draws = state.device->buffer_address(args) + plan.records_offset(),
                                          .sampler = rhi::sampler_index(rhi::DefaultSampler::AnisotropicWrap)};
                    for (uint32_t f = 0; f < faces; ++f) {
                        if (!tiles[f].draw) {
                            continue; // its tile holds what it held
                        }
                        // Cleared over its own tile only: the rest of the atlas is other lights'.
                        const rhi::DepthAttachment depth{.texture = context.texture(local_atlas)};
                        const rhi::Rect tile{.x = static_cast<int32_t>(tiles[f].tile[0]),
                                             .y = static_cast<int32_t>(tiles[f].tile[1]),
                                             .width = tiles[f].tile[2],
                                             .height = tiles[f].tile[2]};
                        context.list.begin_rendering({.depth = &depth, .area = tile});
                        context.list.bind_index_buffer(state.gpu_assets->pool_index_buffer(), 0,
                                                       rhi::IndexType::Uint32);
                        push.cascade = kShadowCascadeCount + f;
                        context.list.push_constants(push);
                        for (const bool masked : {false, true}) {
                            const uint32_t index = cascade_list(kShadowCascadeCount + f, masked);
                            if (plan.lists[index].capacity > 0) {
                                context.list.bind_pipeline(masked ? caster_masked : caster_opaque);
                                state.draw_list(context.list, args, count_buffer, plan, index);
                            }
                        }
                        context.list.end_rendering();
                    }
                });
        }
    }
    const auto cull_slot = static_cast<uint32_t>(state.device->frame_index() % rhi::kFramesInFlight);

    struct Targets {
        GraphTexture gb[3];
        GraphTexture hdr;
        GraphTexture depth;
        GraphTexture motion;
    };
    struct GBufferPass {
        SceneRendererState* state = nullptr;
        TerrainFrame terrain;
        rhi::PipelineHandle terrain_pipeline;
        rhi::PipelineHandle grass_pipeline;
        uint32_t screen_height = 0;
        bool terrain_levels = false;
        Targets targets;
        rhi::PipelineHandle opaque;
        rhi::PipelineHandle masked;
        rhi::PipelineHandle opaque_fading;
        rhi::PipelineHandle masked_fading;
        uint64_t frame_address = 0;
        GraphBuffer draws;
        GraphBuffer counts;
        CullPlan plan;
        GraphTexture ids; // the editor's: each pixel's instance, if it asked
    };
    // The editor's IDs: the same draws through the same shaders, with one more output.
    bool with_ids = false;
    rhi::PipelineHandle id_pipelines[6];
#if DE_BUILD_DEV
    if (state.editor.enabled) {
        with_ids = true;
        for (uint32_t i = 0; i < 6; ++i) {
            id_pipelines[i] = state.shaders->pipeline(state.id_programs[i]);
            with_ids = with_ids && id_pipelines[i].is_valid();
        }
    }
#endif
    const auto& gbuffer = graph.add_pass<GBufferPass>(
        "gbuffer",
        [&](PassBuilder& pass, GBufferPass& data) {
            data = {.state = &state,
                    .terrain = terrain_frame,
                    .terrain_pipeline = with_ids ? id_pipelines[4] : terrain_gbuffer,
                    .grass_pipeline = with_ids ? id_pipelines[5] : grass_gbuffer,
                    .screen_height = graph.reference_height(),
                    .terrain_levels = view == SceneView::TerrainLevel,
                    .targets = {},
                    .opaque = with_ids ? id_pipelines[0] : gbuffer_opaque,
                    .masked = with_ids ? id_pipelines[1] : gbuffer_masked,
                    .opaque_fading = with_ids ? id_pipelines[2] : gbuffer_opaque_fading,
                    .masked_fading = with_ids ? id_pipelines[3] : gbuffer_masked_fading,
                    .frame_address = frame_address,
                    .draws = draw_lists,
                    .counts = draw_counts,
                    .plan = plan,
                    .ids = {}};
#if DE_BUILD_DEV
            if (with_ids) {
                data.ids = pass.create_texture(
                    {.desc = {.format = kIdFormat, .debug_name = "editor ids"}, .reference_scale = 1.0f});
                pass.use(data.ids, rhi::ResourceState::RenderTarget);
            }
#endif
            pass.use(draw_lists, rhi::ResourceState::IndirectArgument | rhi::ResourceState::ShaderRead,
                     rhi::ShaderStages::Vertex);
            pass.use(draw_counts, rhi::ResourceState::IndirectArgument);
            for (uint32_t i = 0; i < 3; ++i) {
                data.targets.gb[i] = pass.create_texture(
                    {.desc = {.format = kGBufferFormats[i], .debug_name = kGBufferNames[i]}, .reference_scale = 1.0f});
                pass.use(data.targets.gb[i], rhi::ResourceState::RenderTarget);
            }
            data.targets.hdr =
                pass.create_texture({.desc = {.format = kHdrFormat, .debug_name = "hdr"}, .reference_scale = 1.0f});
            data.targets.depth = taa_on ? kept_depth
                                        : pass.create_texture({.desc = {.format = kDepthFormat, .debug_name = "depth"},
                                                               .reference_scale = 1.0f});
            data.targets.motion = pass.create_texture(
                {.desc = {.format = kMotionFormat, .debug_name = "motion"}, .reference_scale = 1.0f});
            pass.use(data.targets.hdr, rhi::ResourceState::RenderTarget);
            pass.use(data.targets.motion, rhi::ResourceState::RenderTarget);
            pass.use(data.targets.depth, rhi::ResourceState::DepthWrite);
            pass.use(instance_data_graph, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Vertex);
            if (state.terrain) {
                state.terrain->use_draws(pass, terrain_frame);
                state.terrain->use_grass(pass, terrain_frame);
            }
        },
        [](const GBufferPass& data, PassContext& context) {
            DE_PROFILE_ZONE("gbuffer pass");
            SceneRendererState& renderer = *data.state;
            rhi::ColorAttachment colors[6];
            for (uint32_t i = 0; i < 3; ++i) {
                colors[i] = {.texture = context.texture(data.targets.gb[i])};
            }
            colors[3] = {.texture = context.texture(data.targets.hdr)};
            colors[4] = {.texture = context.texture(data.targets.motion)};
            const bool ids = data.ids.is_valid();
            if (ids) {
                colors[5] = {.texture = context.texture(data.ids)}; // cleared to 0: no instance
            }
            const rhi::DepthAttachment depth{.texture = context.texture(data.targets.depth)};
            context.list.begin_rendering({.colors = std::span(colors, ids ? 6 : 5), .depth = &depth});
            // Grass, then the ground: blades are cheap to shade and the ground isn't, so what they cover isn't shaded.
            // The ground next: it hides the most, so what's behind a hill fails early depth.
            if (renderer.terrain && !renderer.terrain_hidden) {
                renderer.terrain->draw_grass(context, data.terrain, data.grass_pipeline, data.frame_address,
                                             data.screen_height);
                renderer.terrain->draw_gbuffer(context, data.terrain, data.terrain_pipeline, data.frame_address,
                                               data.terrain_levels);
            }
            context.list.bind_index_buffer(renderer.gpu_assets->pool_index_buffer(), 0, rhi::IndexType::Uint32);
            const GBufferPush push{.frame = data.frame_address,
                                   .instances = renderer.instance_data_address,
                                   .vertices = renderer.gpu_assets->pool_vertex_address(),
                                   .materials = renderer.gpu_assets->material_table_address(),
                                   .draws = renderer.device->buffer_address(context.buffer(data.draws)) +
                                            data.plan.records_offset(),
                                   .sampler = rhi::sampler_index(rhi::DefaultSampler::AnisotropicWrap)};
            context.list.push_constants(push);
            // What's fading goes last: it has no early depth test, so everything else helps it.
            for (uint32_t list = 0; list < kCameraListCount; ++list) {
                if (data.plan.lists[list].capacity == 0) {
                    continue;
                }
                const uint32_t kind = list % kDrawKindCount;
                const bool fading = list >= kDrawKindCount;
                const rhi::PipelineHandle masked = fading ? data.masked_fading : data.masked;
                const rhi::PipelineHandle opaque = fading ? data.opaque_fading : data.opaque;
                context.list.bind_pipeline(is_masked(kind) ? masked : opaque); // culls back faces
                if (is_double_sided(kind)) {
                    context.list.set_cull_mode(rhi::CullMode::None);
                }
                renderer.draw_list(context.list, context.buffer(data.draws), context.buffer(data.counts), data.plan,
                                   list);
            }
            context.list.end_rendering();
        });
    const Targets targets = gbuffer.targets;
    if (state.terrain) {
        state.terrain->add_stats_pass(graph, terrain_frame);
    }
#if DE_BUILD_DEV
    const GraphTexture selection_mask = add_editor_passes(graph, gbuffer.ids, targets.depth, frame_address, cull_slot);
#endif

    // The cull's counts, for the stats. Made once: without it the frame goes on, and they stay 0.
    if (!state.cull_readback[cull_slot].is_valid()) {
        const core::Result<rhi::BufferHandle> readback =
            state.device->create_buffer({.size = kCullCounterCount * sizeof(uint32_t),
                                         .usage = rhi::BufferUsage::CopyDst,
                                         .memory = rhi::MemoryLocation::Readback,
                                         .debug_name = "draw counts readback"});
        state.cull_readback[cull_slot] = readback ? *readback : rhi::BufferHandle{};
    }
    if (state.cull_readback[cull_slot].is_valid()) {
        state.cull_readback_plan[cull_slot] = plan;
        const GraphBuffer readback =
            graph.import_buffer({.buffer = state.cull_readback[cull_slot],
                                 .name = "draw counts readback",
                                 .state = state.cull_readback_used[cull_slot] ? rhi::ResourceState::HostRead
                                                                              : rhi::ResourceState::Undefined,
                                 .final_state = rhi::ResourceState::HostRead});
        state.cull_readback_used[cull_slot] = true;
        graph.add_pass(
            "cull stats",
            [&](PassBuilder& pass) {
                pass.use(draw_counts, rhi::ResourceState::CopySrc);
                pass.use(readback, rhi::ResourceState::CopyDst);
            },
            [draw_counts, readback, written = &state.cull_readback_written[cull_slot]](PassContext& context) {
                context.list.copy_buffer(context.buffer(draw_counts), 0, context.buffer(readback), 0,
                                         kCullCounterCount * sizeof(uint32_t));
                *written = true;
            });
    }

    // How open each pixel's surface is to the ambient, from what's near it on the screen: the light reads it.
    // Not while the radiance cascades light the picture: they see what's near on the screen themselves.
    const bool cascades_wanted = state.radiance_cascades && state.radiance_cascades_on && !path_traced &&
                                 (view == SceneView::Lit || view == SceneView::Indirect);
    const bool occluded =
        state.gtao && state.gtao_on && !path_traced && !cascades_wanted &&
        (view == SceneView::Lit || view == SceneView::Indirect || view == SceneView::AmbientOcclusion);
    const GraphTexture ao = occluded ? state.gtao->add_passes(graph, {.frame_address = frame_address,
                                                                      .depth = targets.depth,
                                                                      .normals = targets.gb[1],
                                                                      .frame = &frame,
                                                                      .width = width,
                                                                      .height = height})
                                     : GraphTexture{};

    // Light that has bounced from the radiance cascades, in place of the flat ambient: they read the deferred light's
    // picture, which leaves the diffuse ambient out for them to add.
    const rhi::PipelineHandle rc_apply =
        state.shaders->pipeline(view == SceneView::Indirect ? state.rc_apply_replace : state.rc_apply);
    const bool cascaded = state.radiance_cascades && state.radiance_cascades_on && !path_traced &&
                          rc_apply.is_valid() && (view == SceneView::Lit || view == SceneView::Indirect);
    state.stats.radiance_cascades = {};

    // The shadow mask: how much sun each pixel sees.
    struct MaskPass {
        GraphTexture mask;
    };
    const auto& mask_pass = graph.add_pass<MaskPass>(
        "shadow mask",
        [&](PassBuilder& pass, MaskPass& data) {
            data.mask = pass.create_texture(
                {.desc = {.format = kShadowMaskFormat, .debug_name = "shadow mask"}, .reference_scale = 1.0f});
            pass.use(targets.depth, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            pass.use(targets.gb[1], rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            pass.use(atlas, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            pass.use(data.mask, rhi::ResourceState::RenderTarget);
        },
        [targets, atlas, mask, frame_address](const MaskPass& data, PassContext& context) {
            DE_PROFILE_ZONE("shadow mask pass");
            const rhi::ColorAttachment color{.texture = context.texture(data.mask), .load = rhi::LoadOp::DontCare};
            context.list.begin_rendering({.colors = std::span(&color, 1)});
            context.list.bind_pipeline(mask);
            context.list.push_constants(ShadowMaskPush{
                .frame = frame_address,
                .depth = context.device.texture_index(context.texture(targets.depth)),
                .gb1 = context.device.texture_index(context.texture(targets.gb[1])),
                .atlas = context.device.texture_index(context.texture(atlas)),
                .sampler = rhi::sampler_index(rhi::DefaultSampler::PointClamp),
            });
            context.list.draw(3);
            context.list.end_rendering();
        });
    const GraphTexture shadow_mask = mask_pass.mask;

    // What glows, for the radiance cascades: the HDR target before the light is added holds only that.
    const GraphTexture rc_glow =
        cascades_wanted ? state.radiance_cascades->add_glow_pass(graph, targets.hdr, width, height) : GraphTexture{};

    graph.add_pass(
        "deferred light",
        [&](PassBuilder& pass) {
            for (const GraphTexture gb : targets.gb) {
                pass.use(gb, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            }
            pass.use(targets.depth, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            pass.use(shadow_mask, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            if (light_clusters.is_valid()) {
                pass.use(light_clusters, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            }
            if (ao.is_valid()) {
                pass.use(ao, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            }
            if (local_atlas.is_valid()) {
                pass.use(local_atlas, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            }
            for (const GraphTexture map : box_maps) {
                pass.use(map, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            }
            pass.use(targets.hdr, rhi::ResourceState::RenderTarget);
        },
        [targets, shadow_mask, frame_address, light, ao, local_atlas, cascaded,
         specular_ambient = state.specular_ambient,
         indirect = view == SceneView::Indirect && !cascaded](PassContext& context) {
            DE_PROFILE_ZONE("deferred light pass");
            const rhi::ColorAttachment color{.texture = context.texture(targets.hdr), .load = rhi::LoadOp::Load};
            context.list.begin_rendering({.colors = std::span(&color, 1)});
            context.list.bind_pipeline(light);
            context.list.push_constants(DeferredLightPush{
                .frame = frame_address,
                .gb0 = context.device.texture_index(context.texture(targets.gb[0])),
                .gb1 = context.device.texture_index(context.texture(targets.gb[1])),
                .gb2 = context.device.texture_index(context.texture(targets.gb[2])),
                .depth = context.device.texture_index(context.texture(targets.depth)),
                .shadow_mask = context.device.texture_index(context.texture(shadow_mask)),
                .indirect = indirect ? 1U : 0U,
                .ao = ao.is_valid() ? context.device.texture_index(context.texture(ao)) : 0U,
                .ao_on = ao.is_valid() ? 1U : 0U,
                .local_shadows =
                    local_atlas.is_valid() ? context.device.texture_index(context.texture(local_atlas)) : 0U,
                .ambient_off = cascaded ? 3U : (specular_ambient ? 0U : 2U),
            });
            context.list.draw(3);
            context.list.end_rendering();
        });

    if (cascaded) {
        const RadianceCascadesResult cascades =
            state.radiance_cascades->add_passes(graph, {.frame_address = frame_address,
                                                        .depth = targets.depth,
                                                        .radiance = targets.hdr,
                                                        .glow = rc_glow,
                                                        .base_color = targets.gb[0],
                                                        .material = targets.gb[2],
                                                        .frame = &frame,
                                                        .width = width,
                                                        .height = height});
        state.stats.radiance_cascades = state.radiance_cascades->stats();
        if (cascades.valid()) {
            graph.add_pass(
                "rc apply",
                [&](PassBuilder& pass) {
                    for (const GraphTexture gb : targets.gb) {
                        pass.use(gb, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                    }
                    pass.use(targets.depth, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                    pass.use(cascades.depth, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                    pass.use(cascades.normal, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                    pass.use(cascades.light, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                    pass.use(targets.hdr, rhi::ResourceState::RenderTarget);
                },
                [targets, cascades, rc_apply, frame_address, width, height, specular_ambient = state.specular_ambient,
                 indirect = view == SceneView::Indirect](PassContext& context) {
                    DE_PROFILE_ZONE("rc apply pass");
                    const rhi::ColorAttachment color{.texture = context.texture(targets.hdr),
                                                     .load = rhi::LoadOp::Load};
                    context.list.begin_rendering({.colors = std::span(&color, 1)});
                    context.list.bind_pipeline(rc_apply);
                    context.list.push_constants(RcApplyPush{
                        .frame = frame_address,
                        .gb0 = context.device.texture_index(context.texture(targets.gb[0])),
                        .gb1 = context.device.texture_index(context.texture(targets.gb[1])),
                        .gb2 = context.device.texture_index(context.texture(targets.gb[2])),
                        .scene_depth = context.device.texture_index(context.texture(targets.depth)),
                        .depth = context.device.texture_index(context.texture(cascades.depth)),
                        .normal = context.device.texture_index(context.texture(cascades.normal)),
                        .light = context.device.texture_index(context.texture(cascades.light)),
                        .scale = cascades.scale,
                        .size = {width, height},
                        .gi_size = {cascades.size[0], cascades.size[1]},
                        .tolerance = cascades.tolerance,
                        .flags = (cascades.material_occlusion ? kRcApplyMaterialOcclusion : 0U) |
                                 (indirect ? kRcApplyIndirect : 0U) | (specular_ambient ? kRcApplySpecular : 0U),
                    });
                    context.list.draw(3);
                    context.list.end_rendering();
                });
        }
    }

    // The rays' own view: what a ray through each pixel meets, in place of the lit image.
    const rhi::PipelineHandle trace_view = state.shaders->pipeline(state.trace_view);
    if (view == SceneView::Traced && state.trace && trace_view.is_valid()) {
        graph.add_pass(
            "trace view", [&](PassBuilder& pass) { pass.use(targets.hdr, rhi::ResourceState::RenderTarget); },
            [targets, trace_view, frame_address](PassContext& context) {
                DE_PROFILE_ZONE("trace view pass");
                const rhi::ColorAttachment color{.texture = context.texture(targets.hdr),
                                                 .load = rhi::LoadOp::DontCare};
                context.list.begin_rendering({.colors = std::span(&color, 1)});
                context.list.bind_pipeline(trace_view);
                context.list.push_constants(TraceViewPush{.frame = frame_address});
                context.list.draw(3);
                context.list.end_rendering();
            });
    }

    // Path traced: the path tracer's mean so far, in place of the rasterized light. Compared: the same, hidden, and
    // the rasterized light set against it.
    state.stats.path_tracer_samples = 0;
    state.stats.compare = {};
    GraphTexture compare_heat;
    if (path_traced || compared) {
        state.path_tracer->add_passes(graph,
                                      {.frame_address = frame_address,
                                       .hdr = targets.hdr,
                                       .frame = &frame,
                                       .lighting = &lighting,
                                       .width = width,
                                       .height = height,
                                       .light_count = state.stats.lights.lit},
                                      path_traced);
        state.stats.path_tracer_samples = state.path_tracer->samples();
        state.stats.path_tracer_restart = state.path_tracer->restart_reason();
        state.stats.path_tracer_restarts = state.path_tracer->restarts();
        state.stats.path_tracer_bounces = state.path_tracer->max_bounces();
        if (compared) {
            compare_heat = state.path_tracer->add_compare(
                graph, {.raster = targets.hdr, .depth = targets.depth, .width = width, .height = height});
            state.stats.compare = state.path_tracer->compare_stats();
        }
    }

    // Water: what's behind it kept, then its surface drawn onto the lit image, with its depth and motion, before the
    // fog, which then takes it as any surface. Only in the lit view; without its shaders or its mesh, no water.
    const rhi::PipelineHandle water_behind = state.shaders->pipeline(state.water_behind);
    const rhi::PipelineHandle water = state.shaders->pipeline(state.water_program);
    if (view == SceneView::Lit && (state.water.sea || !frame.water_bodies.empty()) && water_behind.is_valid() &&
        water.is_valid() && state.uploader != nullptr && state.uploader->recorded(state.water_index_ticket)) {
        struct WaterBehindPass {
            GraphTexture color;
            GraphTexture depth;
        };
        const auto& behind = graph.add_pass<WaterBehindPass>(
            "water behind",
            [&](PassBuilder& pass, WaterBehindPass& data) {
                data.color = pass.create_texture(
                    {.desc = {.format = kHdrFormat, .debug_name = "water behind color"}, .reference_scale = 1.0f});
                data.depth = pass.create_texture(
                    {.desc = {.format = kWaterBehindDepthFormat, .debug_name = "water behind depth"},
                     .reference_scale = 1.0f});
                pass.use(targets.hdr, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                pass.use(targets.depth, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                pass.use(data.color, rhi::ResourceState::RenderTarget);
                pass.use(data.depth, rhi::ResourceState::RenderTarget);
            },
            [targets, water_behind](const WaterBehindPass& data, PassContext& context) {
                DE_PROFILE_ZONE("water behind pass");
                const rhi::ColorAttachment colors[2] = {
                    {.texture = context.texture(data.color), .load = rhi::LoadOp::DontCare},
                    {.texture = context.texture(data.depth), .load = rhi::LoadOp::DontCare}};
                context.list.begin_rendering({.colors = std::span(colors)});
                context.list.bind_pipeline(water_behind);
                context.list.push_constants(
                    WaterBehindPush{.hdr = context.device.texture_index(context.texture(targets.hdr)),
                                    .depth = context.device.texture_index(context.texture(targets.depth))});
                context.list.draw(3);
                context.list.end_rendering();
            });
        const GraphTexture behind_color = behind.color;
        const GraphTexture behind_depth = behind.depth;
        const auto sea_level = static_cast<float>(state.water.sea_level_m - frame.origin.y);
        // The waves, brought to this frame's time. Without them (their shaders missing), a flat sea.
        OceanFrame ocean;
        if (state.ocean) {
            ocean = state.ocean->add_pass(graph, lighting.sea, lighting.wind.speed_mps, lighting.wind.direction,
                                          frame.time_s, frame.time_s - frame.previous_time_s, frame.origin);
        }
        // The shore map, once it's all in (a default texture standing in would read as a shore everywhere).
        uint32_t shore_slot = 0;
        if (state.water.shore.is_valid() && state.gpu_assets->material_complete(state.water.shore)) {
            shore_slot = state.gpu_assets->material_slot(state.water.shore);
        }
        uint32_t foam_slot = 0;
        if (state.water.foam.is_valid() && state.gpu_assets->material_complete(state.water.foam)) {
            foam_slot = state.gpu_assets->material_slot(state.water.foam);
        }
        // The surf: the swell's sets, about half its height where they start to feel the bottom; with no swell, the
        // wind's own waves breaking.
        const SceneSea& sea = lighting.sea;
        const float wind_waves_m = 0.21f * lighting.wind.speed_mps * lighting.wind.speed_mps / 9.81f;
        WaterPush water_push{
            .frame = frame_address,
            .levels = frame_address + kWaterLevelsOffset,
            .sea_level = sea_level,
            .shore_material = shore_slot,
            .flags = (shore_slot != 0 ? kWaterShoreFlag : 0U) | (state.water.show_bands ? kWaterBandsFlag : 0U) |
                     (state.water.surf_film ? kWaterFilmFlag : 0U) | (ocean.valid ? kWaterWavesFlag : 0U),
            .surf_period_s = sea.swell_height_m > 0.0f ? sea.swell_period_s : 6.0f,
            .surf_height_m = std::max(0.5f * sea.swell_height_m, 0.4f * wind_waves_m),
            .foam_material = foam_slot,
            .swell_direction = {sea.swell_direction[0], sea.swell_direction[1]},
            .shape = kWaterSea};
        for (uint32_t c = 0; c < kOceanCascades; ++c) {
            water_push.tile_offset[c][0] = ocean.tile_offset[c][0];
            water_push.tile_offset[c][1] = ocean.tile_offset[c][1];
        }
        // The surf near the camera, simulated (shore_sim.h): stepped to this frame before the water reads it. The
        // frame's constants already say it runs (write_frame), on the same terms.
        ShoreSimFrame shore;
        if (state.shore_sim_runs() && state.shore_sim) {
            TerrainHeights heights;
            const bool has_heights = state.terrain && state.terrain->heights(heights);
            const ShoreSimInput input{.camera = frame.origin + DVec3(static_cast<double>(frame.camera.transform[3]),
                                                                     static_cast<double>(frame.camera.transform[7]),
                                                                     static_cast<double>(frame.camera.transform[11])),
                                      .origin = frame.origin,
                                      .sea_level_m = state.water.sea_level_m,
                                      .time_s = frame.time_s,
                                      .frame = frame_address,
                                      .shore_material = shore_slot,
                                      .terrain = has_heights ? &heights : nullptr,
                                      .wave_amplitude_m = water_push.surf_height_m,
                                      .wave_period_s = water_push.surf_period_s};
            shore = state.shore_sim->add_pass(graph, input);
        }
        // Lakes and ponds: kept for the pass (the frame is gone by then). Same allocation each frame once it's grown.
        state.water_bodies_now.clear();
        for (const world::RenderWaterBody& body : frame.water_bodies) {
            if (state.water_bodies_now.size() == kMaxWaterBodies) {
                break;
            }
            state.water_bodies_now.push(body);
        }
        const bool draw_sea = state.water.sea;
        graph.add_pass(
            "water",
            [&](PassBuilder& pass) {
                pass.use(behind_color, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                pass.use(behind_depth, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                if (light_clusters.is_valid()) {
                    pass.use(light_clusters, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                }
                // What the place lets in: the sun's cascades, and the probes' light.
                pass.use(atlas, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                for (const GraphTexture map : box_maps) { // the light boxes' light where it stands
                    pass.use(map, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                }
                pass.use(targets.hdr, rhi::ResourceState::RenderTarget);
                pass.use(targets.motion, rhi::ResourceState::RenderTarget);
                pass.use(targets.depth, rhi::ResourceState::DepthWrite);
                if (shore.valid) {
                    pass.use(shore.surface, rhi::ResourceState::ShaderRead, rhi::ShaderStages::All);
                    if (shore.previous_surface.index != shore.surface.index) {
                        pass.use(shore.previous_surface, rhi::ResourceState::ShaderRead, rhi::ShaderStages::All);
                    }
                }
                if (ocean.valid) {
                    pass.use(ocean.waves, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Vertex);
                    if (ocean.has_previous) {
                        pass.use(ocean.previous_waves, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Vertex);
                    }
                    pass.use(ocean.slopes, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                }
            },
            [targets, behind_color, behind_depth, water, water_push, ocean, draw_sea, atlas,
             renderer = &state](PassContext& context) {
                DE_PROFILE_ZONE("water pass");
                const rhi::ColorAttachment colors[2] = {
                    {.texture = context.texture(targets.hdr), .load = rhi::LoadOp::Load},
                    {.texture = context.texture(targets.motion), .load = rhi::LoadOp::Load}};
                const rhi::DepthAttachment depth{.texture = context.texture(targets.depth), .load = rhi::LoadOp::Load};
                context.list.begin_rendering({.colors = std::span(colors), .depth = &depth});
                context.list.bind_pipeline(water);
                WaterPush push = water_push;
                push.behind_color = context.device.texture_index(context.texture(behind_color));
                push.behind_depth = context.device.texture_index(context.texture(behind_depth));
                push.atlas = context.device.texture_index(context.texture(atlas));
                if (ocean.valid) {
                    push.waves = context.device.texture_index(context.texture(ocean.waves));
                    push.previous_waves = context.device.texture_index(context.texture(ocean.previous_waves));
                    push.slopes = context.device.texture_index(context.texture(ocean.slopes));
                }
                context.list.bind_index_buffer(renderer->water_indices, 0, rhi::IndexType::Uint16);
                // A draw per level: each has its own hole. The instance says which level it is.
                if (draw_sea) {
                    context.list.push_constants(push);
                    for (uint32_t level = 0; level < kWaterLevels; ++level) {
                        const uint32_t hole = renderer->water_levels_now[level].hole;
                        context.list.draw_indexed(water_index_count(hole), 1, water_first_index(hole), 0, level);
                    }
                }
                // Each lake and pond: the same levels at its own height, their vertices held inside its outline (the
                // squares outside it fold onto its edge and draw nothing), only the levels that reach it. No shore
                // map: its edge is where the ground rises through it, which the water's depth shows.
                for (const world::RenderWaterBody& body : renderer->water_bodies_now.span()) {
                    WaterPush lake = push;
                    lake.sea_level = body.center[1];
                    lake.flags = push.flags & kWaterWavesFlag; // the ocean's waves, scaled, and nothing of the shore
                    lake.surf_height_m = 0.0f;
                    lake.bounds[0] = body.center[0];
                    lake.bounds[1] = body.center[2];
                    lake.bounds[2] = body.half_extents[0];
                    lake.bounds[3] = body.half_extents[1];
                    lake.axis[0] = body.axis[0];
                    lake.axis[1] = body.axis[1];
                    lake.shape = body.ellipse ? kWaterEllipse : kWaterRectangle;
                    lake.wave_scale = body.waves;
                    // Its box in the world's axes.
                    const float reach_x = (body.half_extents[0] * std::abs(body.axis[0])) +
                                          (body.half_extents[1] * std::abs(body.axis[1]));
                    const float reach_z = (body.half_extents[0] * std::abs(body.axis[1])) +
                                          (body.half_extents[1] * std::abs(body.axis[0]));
                    context.list.push_constants(lake);
                    for (uint32_t level = 0; level < kWaterLevels; ++level) {
                        const GpuWaterLevel& at = renderer->water_levels_now[level];
                        const float half = at.spacing_m * static_cast<float>(kWaterLevelQuads) * 0.5f;
                        if (std::abs(at.center[0] - body.center[0]) > half + reach_x ||
                            std::abs(at.center[1] - body.center[2]) > half + reach_z) {
                            continue;
                        }
                        context.list.draw_indexed(water_index_count(at.hole), 1, water_first_index(at.hole), 0, level);
                    }
                }
                context.list.end_rendering();
            });
    }

    // The froxel grid, near the camera: each froxel's fog and light, then each column walked front to
    // back (so the composite reads the fog from the camera to any depth with one sample).
    GraphTexture integrated;
    if (!froxels_on) {
        // A grid no one's using holds no memory; but fog volumes just out of view may well be back next frame.
        const bool volumes_may_return =
            view == SceneView::Lit && lighting.fog.volume_distance_m > 0.0f && !frame.fog_volumes.empty();
        ++state.froxels_idle_frames;
        if (!volumes_may_return || state.froxels_idle_frames > kFroxelIdleFrames) {
            state.destroy_froxels();
        }
    } else {
        state.froxels_idle_frames = 0;
        const uint32_t tile_pixels = froxel_tile_pixels(graph.reference_height());
        const uint32_t grid_width = std::max((graph.reference_width() + tile_pixels - 1) / tile_pixels, 1U);
        const uint32_t grid_height = std::max((graph.reference_height() + tile_pixels - 1) / tile_pixels, 1U);
        const rhi::TextureDesc grid{.width = grid_width,
                                    .height = grid_height,
                                    .depth = kFroxelSlices,
                                    .format = kFroxelFormat,
                                    .dimension = rhi::TextureDimension::Texture3D};
        // The two the passes make each frame, by name (the graph's logs, the frame debugger).
        rhi::TextureDesc injected_grid = grid;
        injected_grid.debug_name = "froxels";
        rhi::TextureDesc integrated_grid = grid;
        integrated_grid.debug_name = "fog volume";
        struct GridPass {
            GraphTexture volume;
        };
        // Kept across frames when they can be made; if not, the grid works alone, without history.
        const bool froxels_kept = state.ensure_froxels(grid);
        const bool froxels_have_history =
            froxels_kept && frame.camera.previous_valid && state.froxels_frame + 1 == frame.number &&
            state.froxels_written[state.froxels_current] && state.froxels_reach_m == froxel_reach;
        GraphTexture kept_froxels;
        GraphTexture last_froxels;
        if (froxels_kept) {
            if (froxels_have_history) {
                last_froxels = graph.import_texture({.texture = state.froxel_sets[state.froxels_current],
                                                     .name = "last froxels",
                                                     .state = rhi::ResourceState::ShaderRead,
                                                     .final_state = rhi::ResourceState::ShaderRead});
            }
            state.froxels_current ^= 1U;
            kept_froxels = graph.import_texture({.texture = state.froxel_sets[state.froxels_current],
                                                 .name = "froxels",
                                                 .state = state.froxels_written[state.froxels_current]
                                                              ? rhi::ResourceState::ShaderRead
                                                              : rhi::ResourceState::Undefined,
                                                 .final_state = rhi::ResourceState::ShaderRead,
                                                 .discard = true});
            state.froxels_written[state.froxels_current] = true; // the graph leaves it so, even if it's skipped
        }
        // A different place in each froxel every frame.
        const auto froxel_sample = static_cast<uint32_t>(frame.number % kFroxelJitterSamples) + 1;
        const FogInjectPush injection{.frame = frame_address,
                                      .sampler = rhi::sampler_index(rhi::DefaultSampler::LinearClamp),
                                      .jitter_x = halton(froxel_sample, 2),
                                      .jitter_y = halton(froxel_sample, 3),
                                      .jitter_z = halton(froxel_sample, 5)};
        const GraphTexture froxels =
            graph
                .add_pass<GridPass>(
                    "fog inject",
                    [&](PassBuilder& pass, GridPass& data) {
                        data.volume = froxels_kept ? kept_froxels : pass.create_texture({.desc = injected_grid});
                        pass.use(atlas, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
                        pass.use(data.volume, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
                        if (light_clusters.is_valid()) { // the local lights, glowing in it
                            pass.use(light_clusters, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
                        }
                        for (const GraphTexture map : box_maps) { // the fog's ambient, where a light box holds it
                            pass.use(map, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
                        }
                        if (froxels_have_history) {
                            pass.use(last_froxels, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
                        }
                    },
                    [atlas, last_froxels, froxels_have_history, injection, fog_inject, grid_width, grid_height,
                     renderer = &state, frame_number = frame.number,
                     fog_volumes_address = frame_address + kFogVolumesOffset,
                     froxel_reach](const GridPass& data, PassContext& context) {
                        DE_PROFILE_ZONE("fog inject pass");
                        // Only now: a skipped pass leaves no history.
                        renderer->froxels_frame = frame_number;
                        renderer->froxels_reach_m = froxel_reach;
                        context.list.bind_pipeline(fog_inject);
                        FogInjectPush push = injection;
                        push.volume = context.device.storage_texture_index(context.texture(data.volume));
                        push.atlas = context.device.texture_index(context.texture(atlas));
                        push.width = grid_width;
                        push.height = grid_height;
                        push.fog_volumes = fog_volumes_address;
                        push.fog_slice_masks = fog_volumes_address + (kFogSliceMasksOffset - kFogVolumesOffset);
                        push.fog_volume_count = renderer->fog_volumes_now;
                        push.noise = renderer->gpu_assets->fog_noise_index();
                        push.history =
                            froxels_have_history ? context.device.texture_index(context.texture(last_froxels)) : 0;
                        context.list.push_constants(push);
                        context.list.dispatch((grid_width + 3) / 4, (grid_height + 3) / 4, (kFroxelSlices + 3) / 4);
                    })
                .volume;
        integrated = graph
                         .add_pass<GridPass>(
                             "fog integrate",
                             [&](PassBuilder& pass, GridPass& data) {
                                 data.volume = pass.create_texture({.desc = integrated_grid});
                                 pass.use(froxels, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
                                 pass.use(data.volume, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
                             },
                             [froxels, frame_address, fog_integrate, grid_width, grid_height](const GridPass& data,
                                                                                              PassContext& context) {
                                 DE_PROFILE_ZONE("fog integrate pass");
                                 context.list.bind_pipeline(fog_integrate);
                                 context.list.push_constants(FogIntegratePush{
                                     .frame = frame_address,
                                     .froxels = context.device.texture_index(context.texture(froxels)),
                                     .integrated = context.device.storage_texture_index(context.texture(data.volume)),
                                     .width = grid_width,
                                     .height = grid_height,
                                 });
                                 context.list.dispatch((grid_width + 7) / 8, (grid_height + 7) / 8, 1);
                             })
                         .volume;
    }

    // Fog: the lit image into a new one, dimmed by the fog in front of each pixel, with its light added.
    // Without fog, or in a debug view (which shows no lit image), there is no pass: the tonemap reads
    // the lit image.
    struct FogPass {
        GraphTexture fogged;
    };
    GraphTexture fogged = targets.hdr;
    if (foggy) {
        fogged =
            graph
                .add_pass<FogPass>(
                    "fog",
                    [&](PassBuilder& pass, FogPass& data) {
                        data.fogged = pass.create_texture(
                            {.desc = {.format = kHdrFormat, .debug_name = "hdr fogged"}, .reference_scale = 1.0f});
                        pass.use(targets.hdr, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                        pass.use(targets.depth, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                        if (froxels_on) {
                            pass.use(integrated, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                        }
                        pass.use(data.fogged, rhi::ResourceState::RenderTarget);
                    },
                    [targets, frame_address, fog, froxels_on, integrated, renderer = &state](const FogPass& data,
                                                                                             PassContext& context) {
                        DE_PROFILE_ZONE("fog pass");
                        const rhi::ColorAttachment color{.texture = context.texture(data.fogged),
                                                         .load = rhi::LoadOp::DontCare};
                        context.list.begin_rendering({.colors = std::span(&color, 1)});
                        context.list.bind_pipeline(fog);
                        context.list.push_constants(FogPush{
                            .frame = frame_address,
                            .hdr = context.device.texture_index(context.texture(targets.hdr)),
                            .depth = context.device.texture_index(context.texture(targets.depth)),
                            .integrated = froxels_on ? context.device.texture_index(context.texture(integrated)) : 0,
                            .sampler = rhi::sampler_index(rhi::DefaultSampler::LinearClamp),
                            .fog_volumes = frame_address + kFogVolumesOffset,
                            .fog_slice_masks = frame_address + kFogSliceMasksOffset,
                            .fog_volume_count = froxels_on ? renderer->fog_volumes_now : 0,
                        });
                        context.list.draw(3);
                        context.list.end_rendering();
                    })
                .fogged;
    }

    // What blends: the cull's draws of it put farthest first, then each mixed over the fogged image, lit as it's drawn.
    // Only in the lit view, and only when something among the instances blends; without its shaders, it isn't drawn.
    GraphTexture coverage; // how much of each pixel it covers: TAA trusts its history less there
    const rhi::PipelineHandle blend_sort = state.shaders->pipeline(state.blend_sort);
    const rhi::PipelineHandle transparent = state.shaders->pipeline(state.transparent_program);
    const uint32_t blend_capacity = plan.lists[kBlendList].capacity;
    if (view == SceneView::Lit && blend_capacity > 0 && blend_sort.is_valid() && transparent.is_valid() &&
        state.gpu_assets->mesh_table_ready()) {
        struct BlendSortPass {
            SceneRendererState* state = nullptr;
            GraphBuffer keys;
            GraphBuffer sorted;
        };
        const auto run_sort = [draw_lists, draw_counts, blend_sort, frame_address, blend_capacity,
                               records_offset = plan.records_offset(), first_slot = plan.lists[kBlendList].first_slot](
                                  const BlendSortPass& data, PassContext& context, uint32_t placing) {
            const SceneRendererState& renderer = *data.state;
            const uint64_t lists = renderer.device->buffer_address(context.buffer(draw_lists));
            const BlendSortPush push{
                .frame = frame_address,
                .instances = renderer.instance_data_address,
                .instance_meshes = renderer.instance_meshes_address,
                .meshes = renderer.gpu_assets->mesh_table_address(),
                .records = lists + records_offset,
                .args = lists + uint64_t{first_slot} * sizeof(rhi::DrawIndexedIndirectArgs),
                .sorted = data.sorted.is_valid() ? renderer.device->buffer_address(context.buffer(data.sorted)) : 0,
                .keys = renderer.device->buffer_address(context.buffer(data.keys)),
                .counts = renderer.device->buffer_address(context.buffer(draw_counts)),
                .capacity = blend_capacity,
                .placing = placing};
            context.list.bind_pipeline(blend_sort);
            context.list.push_constants(push);
            context.list.dispatch((blend_capacity + 63) / 64);
        };
        const auto& measure = graph.add_pass<BlendSortPass>(
            "transparent measure",
            [&](PassBuilder& pass, BlendSortPass& data) {
                data.state = &state;
                data.keys = pass.create_buffer({.size = uint64_t{blend_capacity} * sizeof(float),
                                                .usage = rhi::BufferUsage::Storage,
                                                .debug_name = "transparent distances"});
                pass.use(data.keys, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
                pass.use(draw_lists, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
                pass.use(draw_counts, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
                pass.use(instance_data_graph, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
                pass.use(instance_meshes_graph, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
            },
            [run_sort](const BlendSortPass& data, PassContext& context) {
                DE_PROFILE_ZONE("transparent measure pass");
                run_sort(data, context, 0);
            });
        const GraphBuffer blend_keys = measure.keys;
        const auto& sort = graph.add_pass<BlendSortPass>(
            "transparent sort",
            [&](PassBuilder& pass, BlendSortPass& data) {
                data.state = &state;
                data.keys = blend_keys;
                data.sorted =
                    pass.create_buffer({.size = uint64_t{blend_capacity} * sizeof(rhi::DrawIndexedIndirectArgs),
                                        .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect,
                                        .debug_name = "transparent draws"});
                pass.use(data.sorted, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
                pass.use(blend_keys, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
                pass.use(draw_lists, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
                pass.use(draw_counts, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
            },
            [run_sort](const BlendSortPass& data, PassContext& context) {
                DE_PROFILE_ZONE("transparent sort pass");
                run_sort(data, context, 1);
            });
        const GraphBuffer blend_draws = sort.sorted;
        struct TransparentPass {
            SceneRendererState* state = nullptr;
            GraphTexture coverage;
        };
        coverage =
            graph
                .add_pass<TransparentPass>(
                    "transparents",
                    [&](PassBuilder& pass, TransparentPass& data) {
                        data.state = &state;
                        data.coverage = pass.create_texture(
                            {.desc = {.format = kCoverageFormat, .debug_name = "transparent coverage"},
                             .reference_scale = 1.0f});
                        pass.use(fogged, rhi::ResourceState::RenderTarget);
                        pass.use(data.coverage, rhi::ResourceState::RenderTarget);
                        pass.use(targets.depth, rhi::ResourceState::DepthRead);
                        pass.use(atlas, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                        if (froxels_on) {
                            pass.use(integrated, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                        }
                        if (light_clusters.is_valid()) {
                            pass.use(light_clusters, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                        }
                        pass.use(blend_draws, rhi::ResourceState::IndirectArgument);
                        pass.use(draw_counts, rhi::ResourceState::IndirectArgument);
                        pass.use(draw_lists, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Vertex);
                        pass.use(instance_data_graph, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Vertex);
                    },
                    [fogged, targets, atlas, integrated, froxels_on, foggy, blend_draws, draw_counts, draw_lists,
                     transparent, frame_address, blend_capacity,
                     records_offset = plan.records_offset()](const TransparentPass& data, PassContext& context) {
                        DE_PROFILE_ZONE("transparent pass");
                        SceneRendererState& renderer = *data.state;
                        const rhi::ColorAttachment colors[2] = {
                            {.texture = context.texture(fogged), .load = rhi::LoadOp::Load},
                            {.texture = context.texture(data.coverage)}}; // cleared to 0: nothing covered
                        const rhi::DepthAttachment depth{
                            .texture = context.texture(targets.depth), .load = rhi::LoadOp::Load, .read_only = true};
                        context.list.begin_rendering({.colors = colors, .depth = &depth});
                        context.list.bind_pipeline(transparent);
                        context.list.bind_index_buffer(renderer.gpu_assets->pool_index_buffer(), 0,
                                                       rhi::IndexType::Uint32);
                        context.list.push_constants(TransparentPush{
                            .frame = frame_address,
                            .instances = renderer.instance_data_address,
                            .vertices = renderer.gpu_assets->pool_vertex_address(),
                            .materials = renderer.gpu_assets->material_table_address(),
                            .draws = renderer.device->buffer_address(context.buffer(draw_lists)) + records_offset,
                            .sampler = rhi::sampler_index(rhi::DefaultSampler::AnisotropicWrap),
                            .atlas = context.device.texture_index(context.texture(atlas)),
                            .integrated = froxels_on ? context.device.texture_index(context.texture(integrated)) : 0,
                            .grid_sampler = rhi::sampler_index(rhi::DefaultSampler::LinearClamp),
                            .fog = foggy ? 1U : 0U,
                            .froxels = froxels_on ? 1U : 0U,
                        });
                        // The draws in their order, as many as the cull counted.
                        context.list.draw_indexed_indirect_count(
                            context.buffer(blend_draws), 0, context.buffer(draw_counts),
                            uint64_t{kBlendList} * sizeof(uint32_t), blend_capacity);
                        ++renderer.stats.indirect_calls;
                        context.list.end_rendering();
                    })
                .coverage;
    }

    // Particles: stepped if a step is due, then drawn over the same image and into the same coverage.
    state.stats.particles = {};
    if (view == SceneView::Lit && state.particles) {
        coverage = state.particles->add_passes(
            graph, {.frame = &frame,
                    .frame_address = frame_address,
                    .aspect = static_cast<float>(width) / static_cast<float>(std::max(height, 1U)),
                    .wind_mps = lighting.wind.speed_mps + lighting.wind.gust_mps,
                    .color = fogged,
                    .coverage = coverage,
                    .depth = targets.depth,
                    .atlas = atlas,
                    .integrated = integrated,
                    .fog = foggy,
                    .froxels = froxels_on});
        state.stats.particles = state.particles->stats();
    }

    // The exposure: the environment's, or what auto exposure makes of this frame round it (the compensation is in its
    // key, so nothing more multiplies it).
    AutoExposure::Result adapted;
    const bool exposed_view = view == SceneView::Lit || view == SceneView::Indirect || view == SceneView::Traced;
    {
        const uint64_t now_ns = platform::time_ns();
        const float step_s = state.exposure_clock_ns != 0
                                 ? static_cast<float>(static_cast<double>(now_ns - state.exposure_clock_ns) * 1e-9)
                                 : 0.0f;
        state.exposure_clock_ns = now_ns;
        if (state.auto_exposure && state.auto_exposure_on && exposed_view) {
            adapted = state.auto_exposure->add_passes(graph, {.image = fogged,
                                                              .width = width,
                                                              .height = height,
                                                              .base_ev = lighting.exposure_ev,
                                                              .below_ev = lighting.exposure_below_ev,
                                                              .above_ev = lighting.exposure_above_ev,
                                                              .adaptation = lighting.exposure_adaptation,
                                                              .step_s = step_s});
        }
    }
    const float exposure = adapted.exposure_address != 0 ? 1.0f : std::exp2(lighting.exposure_ev);
    // A few frames late, what it came to, for the stats: a copy into this frame slot's readback buffer.
    if (adapted.state.is_valid()) {
        const size_t slot = state.device->frame_index() % rhi::kFramesInFlight;
        const rhi::BufferHandle readback = state.exposure_readback[slot];
        state.exposure_readback_written[slot] = true;
        graph.add_pass(
            "exposure readback",
            [&](PassBuilder& pass) {
                pass.use(adapted.state, rhi::ResourceState::CopySrc);
                pass.never_cull();
            },
            [kept = adapted.state, readback](PassContext& context) {
                context.list.copy_buffer(context.buffer(kept), 0, readback, 0, sizeof(GpuExposureState));
                context.list.barrier(rhi::BufferBarrier{
                    .buffer = readback, .before = rhi::ResourceState::CopyDst, .after = rhi::ResourceState::HostRead});
            });
    }

    // TAA: this frame into its history (what the tonemap shows, and next frame's history).
    GraphTexture resolved = fogged;
    if (taa_on) {
        graph.add_pass(
            "taa",
            [&](PassBuilder& pass) {
                pass.use(fogged, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                pass.use(targets.depth, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                pass.use(targets.motion, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                if (coverage.is_valid()) {
                    pass.use(coverage, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                }
                if (has_history) {
                    pass.use(last_history, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                    pass.use(last_depth, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                    pass.use(last_object_motion, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                }
                if (adapted.state.is_valid()) {
                    pass.use(adapted.state, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                }
                pass.use(kept_history, rhi::ResourceState::RenderTarget);
                pass.use(kept_object_motion, rhi::ResourceState::RenderTarget);
            },
            [fogged, targets, coverage, kept_history, kept_object_motion, last_history, last_depth, last_object_motion,
             has_history, frame_address, taa_pipeline, exposure, renderer = &state,
             adapted_address = adapted.exposure_address, frame_number = frame.number](PassContext& context) {
                DE_PROFILE_ZONE("taa pass");
                renderer->temporal_frame = frame_number; // only now: a skipped pass leaves no history
                const rhi::ColorAttachment colors[2] = {
                    {.texture = context.texture(kept_history), .load = rhi::LoadOp::DontCare},
                    {.texture = context.texture(kept_object_motion), .load = rhi::LoadOp::DontCare}};
                context.list.begin_rendering({.colors = colors});
                context.list.bind_pipeline(taa_pipeline);
                const auto index = [&](GraphTexture texture) {
                    return context.device.texture_index(context.texture(texture));
                };
                context.list.push_constants(TaaPush{
                    .frame = frame_address,
                    .current = index(fogged),
                    .history = has_history ? index(last_history) : 0,
                    .depth = index(targets.depth),
                    .previous_depth = has_history ? index(last_depth) : 0,
                    .motion = index(targets.motion),
                    .previous_object_motion = has_history ? index(last_object_motion) : 0,
                    .sampler = rhi::sampler_index(rhi::DefaultSampler::LinearClamp),
                    .has_history = has_history ? 1U : 0U,
                    .exposure = exposure,
                    .coverage = coverage.is_valid() ? index(coverage) : 0,
                    .has_coverage = coverage.is_valid() ? 1U : 0U,
                    .adapted = adapted_address,
                });
                context.list.draw(3);
                context.list.end_rendering();
            });
        resolved = kept_history;
    }
    if (compare_heat.is_valid()) {
        resolved = compare_heat; // shown as data (the tonemap's view 14)
    }

    // Terrain's levels are in the base color.
    const auto view_index =
        static_cast<uint32_t>(view == SceneView::TerrainLevel                            ? SceneView::BaseColor
                              : view == SceneView::Traced || view == SceneView::Indirect ? SceneView::Lit
                                                                                         : view);
    static_assert(static_cast<uint32_t>(SceneView::AmbientOcclusion) == 13, "the tonemap's view 13");
    static_assert(static_cast<uint32_t>(SceneView::Count) <= 14, "the tonemap's view 14 is the compare's heat map");
    const uint32_t srgb_target = rhi::format_info(target_format).srgb ? 1 : 0;
    graph.add_pass(
        "tonemap",
        [&](PassBuilder& pass) {
            pass.use(resolved, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            for (const GraphTexture gb : targets.gb) {
                pass.use(gb, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            }
            pass.use(shadow_mask, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            pass.use(targets.motion, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            if (ao.is_valid()) {
                pass.use(ao, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            }
            if (adapted.state.is_valid()) {
                pass.use(adapted.state, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            }
            pass.use(target, rhi::ResourceState::RenderTarget);
        },
        [targets, resolved, shadow_mask, target, tonemap, exposure, view_index, srgb_target, ao,
         adapted_address = adapted.exposure_address](PassContext& context) {
            DE_PROFILE_ZONE("tonemap pass");
            const rhi::ColorAttachment color{.texture = context.texture(target), .load = rhi::LoadOp::DontCare};
            context.list.begin_rendering({.colors = std::span(&color, 1)});
            context.list.bind_pipeline(tonemap);
            context.list.push_constants(TonemapPush{
                .hdr = context.device.texture_index(context.texture(resolved)),
                .gb0 = context.device.texture_index(context.texture(targets.gb[0])),
                .gb1 = context.device.texture_index(context.texture(targets.gb[1])),
                .gb2 = context.device.texture_index(context.texture(targets.gb[2])),
                .exposure = exposure,
                .view = view_index,
                .srgb_target = srgb_target,
                .shadow_mask = context.device.texture_index(context.texture(shadow_mask)),
                .motion = context.device.texture_index(context.texture(targets.motion)),
                .ao = ao.is_valid() ? context.device.texture_index(context.texture(ao)) : 0U,
                .ao_on = ao.is_valid() ? 1U : 0U,
                .adapted = adapted_address,
            });
            context.list.draw(3);
            context.list.end_rendering();
        });
#if DE_BUILD_DEV
    add_editor_outline(graph, selection_mask, target, srgb_target);
    add_debug_lines(graph, targets.depth, target, target_format, frame_address, frame.origin, cull_slot, srgb_target);
#endif
}

#if DE_BUILD_DEV
void SceneRenderer::set_debug_lines(std::span<const world::DebugLine> lines) {
    state_->debug_lines = lines;
}

void SceneRenderer::add_debug_lines(RenderGraph& graph, GraphTexture depth, GraphTexture target,
                                    rhi::Format target_format, uint64_t frame_address, const DVec3& origin,
                                    uint32_t slot, uint32_t srgb_target) {
    SceneRendererState& state = *state_;
    if (state.debug_lines.empty() || !depth.is_valid()) {
        return;
    }
    DE_PROFILE_ZONE("debug lines");
    // Its pipelines, for the target's format: one where the scene's depth says in view, one where it's hidden.
    if (target_format != state.debug_format) {
        state.shaders->destroy(state.debug_seen_program);
        state.shaders->destroy(state.debug_hidden_program);
        rhi::GraphicsPipelineDesc lines{.color_count = 1,
                                        .depth_format = kDepthFormat,
                                        .depth_test = true,
                                        .depth_write = false,
                                        .depth_compare = rhi::CompareOp::GreaterEqual,
                                        .topology = rhi::Topology::LineList,
                                        .cull = rhi::CullMode::None,
                                        .debug_name = "debug lines"};
        lines.color_formats[0] = target_format;
        lines.blend[0] = rhi::BlendMode::Alpha;
        const auto make = [&](const rhi::GraphicsPipelineDesc& desc) {
            const core::Result<shader::ProgramHandle> program =
                state.shaders->create_graphics({.module = "scene.debug_lines", .state = desc});
            return program ? *program : shader::ProgramHandle{};
        };
        state.debug_seen_program = make(lines);
        lines.depth_compare = rhi::CompareOp::Less;
        state.debug_hidden_program = make(lines);
        state.debug_format = target_format;
    }
    const rhi::PipelineHandle seen = state.shaders->pipeline(state.debug_seen_program);
    const rhi::PipelineHandle hidden = state.shaders->pipeline(state.debug_hidden_program);
    if (!seen.is_valid() || !hidden.is_valid()) {
        return;
    }
    // This slot's vertices, relative to the render origin (its last frame is done).
    if (!state.debug_buffers[slot].is_valid()) {
        const core::Result<rhi::BufferHandle> buffer =
            state.device->create_buffer({.size = uint64_t{kMaxDebugLines} * 2 * sizeof(GpuDebugVertex),
                                         .usage = rhi::BufferUsage::Storage,
                                         .memory = rhi::MemoryLocation::Upload,
                                         .debug_name = "debug lines"});
        state.debug_buffers[slot] = buffer ? *buffer : rhi::BufferHandle{};
    }
    if (!state.debug_buffers[slot].is_valid()) {
        return;
    }
    const std::span<std::byte> mapped = state.device->mapped(state.debug_buffers[slot]);
    auto* vertices = reinterpret_cast<GpuDebugVertex*>(mapped.data());
    const auto count = static_cast<uint32_t>(std::min<size_t>(state.debug_lines.size(), kMaxDebugLines));
    // Those drawn through what hides them first, so the hidden pass draws only them; then the rest.
    uint32_t written = 0;
    const auto write = [&](const world::DebugLine& line) {
        const DVec3 from = line.from - origin;
        const DVec3 to = line.to - origin;
        vertices[static_cast<size_t>(written) * 2] = {
            .position = {static_cast<float>(from.x), static_cast<float>(from.y), static_cast<float>(from.z)},
            .color = line.color};
        vertices[(static_cast<size_t>(written) * 2) + 1] = {
            .position = {static_cast<float>(to.x), static_cast<float>(to.y), static_cast<float>(to.z)},
            .color = line.color};
        ++written;
    };
    for (size_t i = 0; i < count; ++i) {
        if (state.debug_lines[i].through) {
            write(state.debug_lines[i]);
        }
    }
    const uint32_t through = written;
    for (size_t i = 0; i < count; ++i) {
        if (!state.debug_lines[i].through) {
            write(state.debug_lines[i]);
        }
    }
    state.device->flush_mapped(state.debug_buffers[slot], 0, uint64_t{count} * 2 * sizeof(GpuDebugVertex));
    const DebugLinesPush push{.frame = frame_address,
                              .vertices = state.device->buffer_address(state.debug_buffers[slot]),
                              .alpha = 1.0f,
                              .srgb_target = srgb_target};
    graph.add_pass(
        "debug lines",
        [&](PassBuilder& pass) {
            pass.use(target, rhi::ResourceState::RenderTarget);
            pass.use(depth, rhi::ResourceState::DepthRead);
        },
        [target, depth, seen, hidden, push, count, through](PassContext& context) {
            DE_PROFILE_ZONE("debug lines pass");
            const rhi::ColorAttachment color{.texture = context.texture(target), .load = rhi::LoadOp::Load};
            const rhi::DepthAttachment scene_depth{
                .texture = context.texture(depth), .load = rhi::LoadOp::Load, .read_only = true};
            context.list.begin_rendering({.colors = std::span(&color, 1), .depth = &scene_depth});
            // Hidden first, faint; then what's in view over it.
            DebugLinesPush faint = push;
            faint.alpha = 0.25f;
            if (through > 0) {
                context.list.bind_pipeline(hidden);
                context.list.push_constants(faint);
                context.list.draw(through * 2);
            }
            context.list.bind_pipeline(seen);
            context.list.push_constants(push);
            context.list.draw(count * 2);
            context.list.end_rendering();
        });
}

void SceneRenderer::set_editor_overlay(const EditorOverlay& overlay) {
    SceneRendererState& state = *state_;
    state.editor = overlay;
    state.selected.assign(overlay.selected);
    state.editor.selected = state.selected.span();
}

PickAnswer SceneRenderer::pick_answer() const {
    return state_->pick;
}

GraphTexture SceneRenderer::add_editor_passes(RenderGraph& graph, GraphTexture ids, GraphTexture depth,
                                              uint64_t frame_address, uint32_t slot) {
    SceneRendererState& state = *state_;
    // A pick: the ID under the pixel, copied to where the CPU reads it once this frame is done.
    if (ids.is_valid() && state.editor.pick && state.editor.pick_x < graph.reference_width() &&
        state.editor.pick_y < graph.reference_height()) {
        if (!state.pick_readback[slot].is_valid()) {
            const core::Result<rhi::BufferHandle> readback =
                state.device->create_buffer({.size = sizeof(uint32_t),
                                             .usage = rhi::BufferUsage::CopyDst,
                                             .memory = rhi::MemoryLocation::Readback,
                                             .debug_name = "editor pick readback"});
            state.pick_readback[slot] = readback ? *readback : rhi::BufferHandle{};
        }
        if (state.pick_readback[slot].is_valid()) {
            const GraphBuffer readback = graph.import_buffer(
                {.buffer = state.pick_readback[slot],
                 .name = "editor pick readback",
                 .state = state.pick_used[slot] ? rhi::ResourceState::HostRead : rhi::ResourceState::Undefined,
                 .final_state = rhi::ResourceState::HostRead});
            state.pick_used[slot] = true;
            const uint32_t x = state.editor.pick_x;
            const uint32_t y = state.editor.pick_y;
            graph.add_pass(
                "editor pick",
                [&](PassBuilder& pass) {
                    pass.use(ids, rhi::ResourceState::CopySrc);
                    pass.use(readback, rhi::ResourceState::CopyDst);
                },
                [ids, readback, x, y, waiting = &state.pick_waiting[slot]](PassContext& context) {
                    context.list.copy_texture_to_buffer(context.texture(ids), context.buffer(readback),
                                                        {.x = x, .y = y, .width = 1, .height = 1});
                    *waiting = true;
                });
        }
    }
    state.editor.pick = false;

    // The terrain chosen: its mask from the IDs the G-buffer wrote.
    if (state.editor.enabled && state.editor.terrain_selected) {
        const rhi::PipelineHandle terrain = state.shaders->pipeline(state.terrain_selection_program);
        if (!terrain.is_valid() || !ids.is_valid()) {
            return {};
        }
        struct TerrainSelectionPass {
            GraphTexture mask;
        };
        const auto& pass = graph.add_pass<TerrainSelectionPass>(
            "editor terrain selection",
            [&](PassBuilder& builder, TerrainSelectionPass& data) {
                data.mask = builder.create_texture(
                    {.desc = {.format = rhi::Format::RGBA8Unorm, .debug_name = "terrain selection mask"},
                     .reference_scale = 1.0f});
                builder.use(data.mask, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
                builder.use(ids, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            },
            [ids, terrain](const TerrainSelectionPass& data, PassContext& context) {
                DE_PROFILE_ZONE("editor terrain selection pass");
                const rhi::TextureHandle mask = context.texture(data.mask);
                const rhi::TextureDesc* desc = context.device.texture_desc(mask);
                if (desc == nullptr) {
                    return;
                }
                context.list.bind_pipeline(terrain);
                context.list.push_constants(TerrainSelectionPush{
                    .ids = context.device.storage_texture_index(context.texture(ids)),
                    .mask = context.device.storage_texture_index(mask),
                    .width = desc->width,
                    .height = desc->height,
                });
                context.list.dispatch((desc->width + 7) / 8, (desc->height + 7) / 8, 1);
            });
        return pass.mask;
    }

    // The selection's mask: its meshes alone (their finest LOD), each pixel covered and whether it's seen.
    const rhi::PipelineHandle selection = state.shaders->pipeline(state.selection_program);
    if (!state.editor.enabled || state.selected.empty() || !selection.is_valid()) {
        return {};
    }
    if (!state.selection_draws[slot].is_valid()) {
        const core::Result<rhi::BufferHandle> buffer =
            state.device->create_buffer({.size = kMaxSelectionDraws * sizeof(GpuDraw),
                                         .usage = rhi::BufferUsage::Storage,
                                         .memory = rhi::MemoryLocation::Upload,
                                         .debug_name = "editor selection draws"});
        state.selection_draws[slot] = buffer ? *buffer : rhi::BufferHandle{};
    }
    if (!state.selection_draws[slot].is_valid()) {
        return {};
    }
    struct SelectionDraw {
        uint32_t first_index = 0;
        uint32_t index_count = 0;
        int32_t vertex_offset = 0;
    };
    struct SelectionPass {
        GraphTexture mask;
        GraphTexture own_depth;
        rhi::PipelineHandle pipeline;
        GBufferPush push;
        SelectionDraw draws[kMaxSelectionDraws];
        uint32_t draw_count = 0;
    };
    // Its parts, as draw records the G-buffer's vertex shader reads (written now: this slot's last frame is done).
    const std::span<std::byte> mapped = state.device->mapped(state.selection_draws[slot]);
    auto* records = reinterpret_cast<GpuDraw*>(mapped.data());
    uint32_t count = 0;
    for (const SelectedMesh& selected : state.selected) {
        const GpuMesh* mesh = state.gpu_assets->mesh(selected.mesh);
        if (mesh == nullptr || mesh->lods.empty()) {
            continue;
        }
        for (const GpuMeshPart& part : mesh->lod_parts(0)) {
            if (count == kMaxSelectionDraws) {
                break;
            }
            records[count] = {.instance = selected.slot, .material = part.material, .fade = 0.0f};
            ++count;
        }
    }
    if (count == 0) {
        return {};
    }
    state.device->flush_mapped(state.selection_draws[slot], 0, uint64_t{count} * sizeof(GpuDraw));
    const uint64_t draws_address = state.device->buffer_address(state.selection_draws[slot]);
    const auto& pass = graph.add_pass<SelectionPass>(
        "editor selection",
        [&](PassBuilder& builder, SelectionPass& data) {
            data.mask = builder.create_texture(
                {.desc = {.format = kSelectionMaskFormat, .debug_name = "selection mask"}, .reference_scale = 1.0f});
            data.own_depth = builder.create_texture(
                {.desc = {.format = kDepthFormat, .debug_name = "selection depth"}, .reference_scale = 1.0f});
            builder.use(data.mask, rhi::ResourceState::RenderTarget);
            builder.use(data.own_depth, rhi::ResourceState::DepthWrite);
            builder.use(depth, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            data.pipeline = selection;
            data.push = {.frame = frame_address,
                         .instances = state.instance_data_address,
                         .vertices = state.gpu_assets->pool_vertex_address(),
                         .materials = state.gpu_assets->material_table_address(),
                         .draws = draws_address,
                         .sampler = rhi::sampler_index(rhi::DefaultSampler::AnisotropicWrap),
                         .depth = 0,
                         .unjittered = 1};
            uint32_t at = 0;
            for (const SelectedMesh& selected : state.selected) {
                const GpuMesh* mesh = state.gpu_assets->mesh(selected.mesh);
                if (mesh == nullptr || mesh->lods.empty()) {
                    continue;
                }
                for (const GpuMeshPart& part : mesh->lod_parts(0)) {
                    if (at == count) {
                        break;
                    }
                    data.draws[at++] = {.first_index = part.first_index,
                                        .index_count = part.index_count,
                                        .vertex_offset = static_cast<int32_t>(part.vertex_offset)};
                }
            }
            data.draw_count = at;
        },
        [depth, &state](const SelectionPass& data, PassContext& context) {
            DE_PROFILE_ZONE("editor selection pass");
            const rhi::ColorAttachment color{.texture = context.texture(data.mask)};
            const rhi::DepthAttachment own{.texture = context.texture(data.own_depth)};
            context.list.begin_rendering({.colors = std::span(&color, 1), .depth = &own});
            context.list.bind_pipeline(data.pipeline);
            context.list.bind_index_buffer(state.gpu_assets->pool_index_buffer(), 0, rhi::IndexType::Uint32);
            GBufferPush push = data.push;
            push.depth = context.device.texture_index(context.texture(depth));
            context.list.push_constants(push);
            for (uint32_t i = 0; i < data.draw_count; ++i) {
                const SelectionDraw& draw = data.draws[i];
                context.list.draw_indexed(draw.index_count, 1, draw.first_index, draw.vertex_offset, i);
            }
            context.list.end_rendering();
        });
    return pass.mask;
}

void SceneRenderer::add_editor_outline(RenderGraph& graph, GraphTexture mask, GraphTexture target,
                                       uint32_t srgb_target) {
    const SceneRendererState& state = *state_;
    const rhi::PipelineHandle outline = state.shaders->pipeline(state.outline_program);
    if (!mask.is_valid() || !outline.is_valid()) {
        return;
    }
    const OutlinePush push{.mask = 0,
                           .radius_px = std::clamp(state.editor.outline_px, 1.0f, 8.0f),
                           .width = graph.reference_width(),
                           .height = graph.reference_height(),
                           .srgb_target = srgb_target};
    graph.add_pass(
        "editor outline",
        [&](PassBuilder& pass) {
            pass.use(mask, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            pass.use(target, rhi::ResourceState::RenderTarget);
        },
        [mask, target, outline, push](PassContext& context) {
            DE_PROFILE_ZONE("editor outline pass");
            const rhi::ColorAttachment color{.texture = context.texture(target), .load = rhi::LoadOp::Load};
            context.list.begin_rendering({.colors = std::span(&color, 1)});
            context.list.bind_pipeline(outline);
            OutlinePush data = push;
            data.mask = context.device.texture_index(context.texture(mask));
            context.list.push_constants(data);
            context.list.draw(3);
            context.list.end_rendering();
        });
}
#endif

void SceneRenderer::set_lod_error_pixels(float pixels) {
    state_->lod_error_pixels = pixels;
}

void SceneRenderer::set_lod_fade(float share) {
    state_->lod_fade = share;
}

void SceneRenderer::set_merged_draws(bool on) {
    state_->merged_draws = on;
}

void SceneRenderer::set_max_draws_per_call(uint32_t draws) {
    state_->max_draws_per_call = draws;
}

void SceneRenderer::set_shadow_caster_cull(bool on) {
    state_->shadow_caster_cull = on;
}

void SceneRenderer::set_shadow_min_texels(float texels) {
    state_->shadow_min_texels = texels;
}

void SceneRenderer::set_cull_frozen(bool frozen) {
    state_->cull_frozen = frozen;
    state_->frozen_views_held = frozen && state_->frozen_views_held; // what freezes next takes a new view
}

void SceneRenderer::set_fog_low_camera(bool on) {
    state_->fog_low_camera = on;
}

void SceneRenderer::set_auto_exposure(bool on) {
    if (on && !state_->auto_exposure_on && state_->auto_exposure) {
        state_->auto_exposure->snap();
    }
    state_->auto_exposure_on = on;
}

void SceneRenderer::set_auto_exposure_settings(const AutoExposureSettings& settings) {
    if (state_->auto_exposure) {
        state_->auto_exposure->set_settings(settings);
    }
}

bool SceneRenderer::auto_exposure_state(GpuExposureState& exposure) const {
    // This frame slot's copy was made kFramesInFlight frames ago, and the GPU is done with it (begin_frame waited).
    const size_t slot = state_->device->frame_index() % rhi::kFramesInFlight;
    if (!state_->auto_exposure_on || !state_->exposure_readback_written[slot]) {
        return false;
    }
    state_->device->invalidate_mapped(state_->exposure_readback[slot]);
    std::memcpy(&exposure, state_->device->mapped(state_->exposure_readback[slot]).data(), sizeof(exposure));
    return exposure.has_value != 0;
}

void SceneRenderer::set_ambient_occlusion(bool on) {
    state_->gtao_on = on;
}

void SceneRenderer::set_radiance_cascades(bool on) {
    state_->radiance_cascades_on = on;
}

void SceneRenderer::set_specular_ambient(bool on) {
    state_->specular_ambient = on;
}

void SceneRenderer::set_debug_jitter(uint32_t sample) {
    state_->debug_jitter = std::min(sample, kTaaSamples);
}

void SceneRenderer::set_local_shadow_cache(bool on) {
    state_->local_shadow_cache = on;
}

void SceneRenderer::set_most_shadowed_lights(uint32_t lights) {
    if (state_->local_lights) {
        state_->local_lights->set_most_shadowed(lights);
    }
}

void SceneRenderer::set_radiance_cascades_settings(const RadianceCascadesSettings& settings) {
    if (state_->radiance_cascades) {
        state_->radiance_cascades->set_settings(settings);
    }
}

void SceneRenderer::set_ambient_occlusion_settings(const GtaoSettings& settings) {
    if (state_->gtao) {
        state_->gtao->set_settings(settings);
    }
}

PathTracer* SceneRenderer::path_tracer() {
    return state_->path_tracer ? &*state_->path_tracer : nullptr;
}

void SceneRenderer::set_path_traced(bool on) {
    state_->path_traced = on;
}

void SceneRenderer::set_light_boxes(std::span<const LightBoxDesc> boxes) {
    if (state_->light_boxes) {
        state_->light_boxes->set_boxes(boxes);
    }
}

void SceneRenderer::set_light_boxes_on(bool on) {
    state_->light_boxes_on = on;
}

void SceneRenderer::rebake_light_boxes() {
    if (state_->light_boxes) {
        state_->light_boxes->rebake();
    }
}

void SceneRenderer::set_lighting_compare(bool on) {
    state_->lighting_compare = on;
}

void SceneRenderer::set_terrain_hidden(bool hidden) {
    state_->terrain_hidden = hidden;
}

void SceneRenderer::set_terrain(const asset::TerrainAsset* terrain, std::span<const std::byte> file) {
    if (state_->terrain) {
        state_->terrain->set(terrain, file);
    }
}

void SceneRenderer::set_terrain_layers(const asset::TerrainLayersAsset* layers, std::span<const std::byte> file) {
    if (state_->terrain) {
        state_->terrain->set_layers(layers, file);
    }
}

bool SceneRenderer::instances_stale() const {
    return state_->instances_stale_now;
}

SceneRendererStats SceneRenderer::stats() const {
    return state_->stats;
}

const ParticleSystem* SceneRenderer::particles() const {
    return state_->particles ? &*state_->particles : nullptr;
}

} // namespace de::render
