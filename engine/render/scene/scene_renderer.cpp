#include "render/scene/scene_renderer.h"

#include "core/allocator.h"
#include "core/log.h"
#include "core/math.h"
#include "core/profile.h"
#include "render/rhi/device.h"
#include "render/scene/gpu_assets.h"
#include "world/render_extract.h"

#include <glm/gtc/matrix_access.hpp>
#include <glm/mat4x4.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
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
    float fog_density[4] = {};           // constant, height base, falloff, base height (render-origin space)
    float fog_extinction[4] = {};        // tint, ambient height attenuation
    float fog_sun_forward[4] = {};       // colour, sun height attenuation
    float fog_sun_back[4] = {};          // colour, sun start
    float fog_ambient[4] = {};           // colour, start
    float fog_range[4] = {};             // end, froxel grid reach, slice offset, slices
    float previous_view_proj[16] = {};   // last frame's camera, relative to this frame's render origin
    float unjittered_view_proj[16] = {}; // view_proj without TAA's sub-pixel offset
    float taa_params[4] = {};            // last frame's near, TAA's sample (0 without TAA), its offset in NDC
};
static_assert(sizeof(SceneFrame) <= 1024);

// Matches GBufferPush in shaders/scene/gbuffer.slang.
struct GBufferPush {
    uint64_t frame = 0;
    uint64_t instances = 0;
    uint64_t vertices = 0;
    uint64_t materials = 0;
    uint64_t draws = 0;
    uint32_t sampler = 0;
};

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
};
static_assert(sizeof(GpuDraw) == 8);

// Matches ShadowMaskPush in shaders/scene/shadow_mask.slang.
struct ShadowMaskPush {
    uint64_t frame = 0;
    uint32_t depth = 0;
    uint32_t gb1 = 0;
    uint32_t atlas = 0;
    uint32_t sampler = 0;
};

// Matches DeferredLightPush in shaders/scene/deferred_light.slang.
struct DeferredLightPush {
    uint64_t frame = 0;
    uint32_t gb0 = 0;
    uint32_t gb1 = 0;
    uint32_t gb2 = 0;
    uint32_t depth = 0;
    uint32_t shadow_mask = 0;
};

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
    float shape[4] = {};        // x: 1 for an ellipsoid; y: how much noise eats away; z: its height fade, in meters
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
constexpr uint32_t kFroxelTilePixels = 8;
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
};

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
};

constexpr uint64_t kFogVolumesOffset = 1024; // after the SceneFrame
// The nearest this many in reach go into the froxel grid each frame.
constexpr uint32_t kMaxFogVolumes = 64;
static_assert(kMaxFogVolumes <= 64, "a slice's mask has a bit per volume");
// Per slice of the grid, which volumes reach it: a froxel tests only those.
constexpr uint64_t kFogSliceMasksOffset = kFogVolumesOffset + kMaxFogVolumes * sizeof(GpuFogVolume);
constexpr uint64_t kInstanceBytes = 96; // six float4 rows: its transform, and last frame's

// The G-buffer's formats (shaders/scene/scene_common.slang documents the channels).
constexpr rhi::Format kGBufferFormats[3] = {rhi::Format::RGBA8Srgb, rhi::Format::RG16Unorm, rhi::Format::RGBA8Unorm};
constexpr rhi::Format kHdrFormat = rhi::Format::RGBA16Float;
constexpr rhi::Format kMotionFormat = rhi::Format::RG16Float;
constexpr rhi::Format kDepthFormat = rhi::Format::D32Float;
constexpr rhi::Format kShadowMaskFormat = rhi::Format::RG8Unorm;

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
// The draw lists the GPU cull fills: a list per kind for the camera, then opaque and masked for each cascade, whose
// casters differ only by the alpha test.
constexpr uint32_t kCameraListCount = kDrawKindCount;
constexpr uint32_t kDrawListCount = kCameraListCount + 2 * kShadowCascadeCount;

constexpr uint32_t cascade_list(uint32_t cascade, bool masked) {
    return kCameraListCount + (2 * cascade) + (masked ? 1U : 0U);
}

// Matches GpuCullView and GpuDrawList in shaders/scene/scene_common.slang.
struct GpuCullView {
    float planes[6][4] = {};
    uint32_t plane_count = 0;
    uint32_t first_list = 0;
    uint32_t lists_by_kind = 0;
    uint32_t lod_perspective = 0;
    float depth_plane[4] = {};
    float lod_scale = 0.0f;
    float padding_end[3] = {};
};
static_assert(sizeof(GpuCullView) == 144);

struct GpuDrawList {
    uint32_t first_slot = 0;
    uint32_t capacity = 0;
};

constexpr uint32_t kCullViewCount = 1 + kShadowCascadeCount;
// The counts the cull writes: per draw list, then the triangles it kept for the camera and for the cascades.
constexpr uint32_t kCullCounterCount = kDrawListCount + 2;
constexpr uint32_t kNoMesh = UINT32_MAX;
// The views and lists, then what changed in the instances, which stay on the GPU between frames.
constexpr uint64_t kCullViewsOffset = kFogSliceMasksOffset + kFroxelSlices * sizeof(uint64_t);
constexpr uint64_t kDrawListsOffset = kCullViewsOffset + kCullViewCount * sizeof(GpuCullView);
constexpr uint64_t kInstanceUpdatesOffset = kDrawListsOffset + kDrawListCount * sizeof(GpuDrawList);

// Matches InstanceUpdate in shaders/scene/instance_update.slang.
struct GpuInstanceUpdate {
    float rows[24] = {};
    uint32_t slot = 0;
    uint32_t mesh = 0;
    uint32_t padding[2] = {};
};
static_assert(sizeof(GpuInstanceUpdate) == 112);
static_assert(sizeof(world::RenderInstance::transform) * 2 == kInstanceBytes);

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
    uint64_t slot_capacity = 0;

    // The draw arguments come first in the cull's buffer, then the records.
    uint64_t records_offset() const { return slot_capacity * sizeof(rhi::DrawIndexedIndirectArgs); }
    uint64_t buffer_bytes() const { return records_offset() + slot_capacity * sizeof(GpuDraw); }
};

// Matches CullPush in shaders/scene/cull.slang.
struct CullPush {
    uint64_t instances = 0;
    uint64_t instance_meshes = 0;
    uint64_t meshes = 0;
    uint64_t lods = 0;
    uint64_t parts = 0;
    uint64_t materials = 0;
    uint64_t views = 0;
    uint64_t lists = 0;
    uint64_t args = 0;
    uint64_t records = 0;
    uint64_t counts = 0;
    uint32_t instance_count = 0;
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
    shader::ProgramHandle deferred_light;
    shader::ProgramHandle tonemap;
    shader::ProgramHandle shadow_caster_opaque; // depth only
    shader::ProgramHandle shadow_caster_masked; // alpha-tested
    shader::ProgramHandle shadow_mask;
    shader::ProgramHandle fog_program;
    shader::ProgramHandle fog_inject;
    shader::ProgramHandle fog_integrate;
    shader::ProgramHandle cull;
    shader::ProgramHandle instance_update;
    // The instances, on the GPU between frames, and what the CPU keeps of them: each slot's mesh, to size the draw
    // lists without walking the instances.
    rhi::BufferHandle instance_rows;
    rhi::BufferHandle instance_meshes;
    uint64_t instance_rows_address = 0;
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
    rhi::BufferHandle cull_readback[rhi::kFramesInFlight];
    CullPlan cull_readback_plan[rhi::kFramesInFlight];
    bool cull_readback_written[rhi::kFramesInFlight] = {}; // recorded, and not yet read
    bool cull_readback_used[rhi::kFramesInFlight] = {}; // copied to before: the graph has to order this copy after it
    bool warned_draw_limit = false;
    float lod_error_pixels = 1.0f;
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

    ~SceneRendererState() {
        for (const shader::ProgramHandle program :
             {gbuffer_opaque, gbuffer_masked, deferred_light, tonemap, shadow_caster_opaque, shadow_caster_masked,
              shadow_mask, fog_program, fog_inject, fog_integrate, cull, instance_update, taa_program}) {
            shaders->destroy(program);
        }
        for (const rhi::BufferHandle buffer : frame_buffers) {
            device->destroy(buffer);
        }
        for (const rhi::BufferHandle buffer : cull_readback) {
            device->destroy(buffer);
        }
        device->destroy(instance_rows);
        device->destroy(instance_meshes);
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

        rhi::GraphicsPipelineDesc light{.color_count = 1, .cull = rhi::CullMode::None, .debug_name = "deferred light"};
        light.color_formats[0] = kHdrFormat;
        light.blend[0] = rhi::BlendMode::Additive;
        deferred_light = make("scene.deferred_light", light);
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
        const core::Result<shader::ProgramHandle> cull_program =
            shaders->create_compute({.module = "scene.cull", .debug_name = "gpu cull"});
        cull = cull_program ? *cull_program : shader::ProgramHandle{};
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
        rhi::GraphicsPipelineDesc mask{.color_count = 1, .cull = rhi::CullMode::None, .debug_name = "shadow mask"};
        mask.color_formats[0] = kShadowMaskFormat;
        shadow_mask = make("scene.shadow_mask", mask);
    }

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
            const uint32_t kept = std::min(counts[list], cull_readback_plan[slot].lists[list].capacity);
            (list < kCameraListCount ? stats.visible_draws : stats.visible_shadow_draws) += kept;
        }
        stats.visible_triangles = counts[kDrawListCount];
        stats.visible_shadow_triangles = counts[kDrawListCount + 1];
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
    void write_fog_volumes(std::span<std::byte> mapped, const world::RenderFrame& frame, float froxel_reach,
                           float aspect) {
        fog_volumes_now = 0;
        stats.fog_volumes = 0;
        stats.fog_volumes_dropped = 0;
        if (froxel_reach <= 0.0f || frame.fog_volumes.empty()) {
            return;
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
            gpu.noise[0] = volume.noise_offset_tiles[0];
            gpu.noise[1] = volume.noise_offset_tiles[1];
            gpu.noise[2] = volume.noise_offset_tiles[2];
            gpu.noise[3] = volume.noise_tiles_per_m;
            std::memcpy(mapped.data() + kFogVolumesOffset + k * sizeof(GpuFogVolume), &gpu, sizeof(gpu));
        }
        std::memcpy(mapped.data() + kFogSliceMasksOffset, slice_masks, sizeof(slice_masks));
        stats.fog_volumes = fog_volumes_now;
        stats.fog_volumes_dropped = dropped;
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
            std::memcpy(update.rows, instance.transform, kInstanceBytes / 2);
            std::memcpy(update.rows + 12, instance.previous_transform, kInstanceBytes / 2);
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

    // The cull's views, what changed in the instances, and from them the draw lists' sizes.
    void write_cull_data(std::span<std::byte> mapped, const world::RenderFrame& frame, const SceneFrame& scene,
                         uint32_t height, CullPlan& plan) {
        // At 0, LOD 0 everywhere, however small a LOD's error.
        const auto lod_scale_for = [this](float per_unit_of_error) {
            return lod_error_pixels > 0.0f ? per_unit_of_error / lod_error_pixels : -1.0f;
        };
        // Made here and copied: this memory is fast to write and very slow to read.
        GpuCullView views[kCullViewCount];
        // The camera: its sides, which meet at its eye, so they cover what's behind it too. Projection has no far
        // plane.
        views[0] = {.plane_count = 4,
                    .first_list = 0,
                    .lists_by_kind = 1,
                    .lod_perspective = 1,
                    .lod_scale = lod_scale_for(static_cast<float>(height) /
                                               (2.0f * std::tan(frame.camera.vertical_fov_rad * 0.5f)))};
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
        // A cascade: its sides and both ends of its depth (0 <= z <= w). Depth isn't clamped, so a caster past either
        // end would be clipped anyway. How far toward the sun a cascade reaches is SceneShadows::caster_reach_m.
        for (uint32_t c = 0; c < kShadowCascadeCount; ++c) {
            GpuCullView& view = views[1 + c];
            view = {.plane_count = 6,
                    .first_list = cascade_list(c, false),
                    .lists_by_kind = 0,
                    .lod_scale = lod_scale_for(1.0f / std::max(scene.cascade_texel_m[c], 1e-6f))};
            const float* const view_proj = &scene.cascade_view_proj[static_cast<size_t>(c) * 16];
            set_side_planes(view, view_proj);
            std::memcpy(view.planes[4], view_proj + 8, sizeof(view.planes[4]));
            normalize_plane(view.planes[4]);
            set_plane(view.planes[5], view_proj, 2, -1.0f);
        }
        std::memcpy(mapped.data() + kCullViewsOffset, views, sizeof(views));

        write_instance_updates(mapped, frame, plan);
        // What the instances can draw, from how many there are of each mesh.
        uint64_t kind_parts[kDrawKindCount] = {};
        const std::span<const GpuMeshKinds> kinds = gpu_assets->mesh_kinds();
        for (size_t mesh = 0; mesh < used_mesh_slots; ++mesh) {
            const uint32_t count = mesh_instances[mesh];
            if (count == 0) {
                continue;
            }
            bool drawable = false;
            for (uint32_t kind = 0; kind < kDrawKindCount; ++kind) {
                kind_parts[kind] += uint64_t{count} * kinds[mesh].parts[kind];
                drawable = drawable || kinds[mesh].parts[kind] > 0;
            }
            plan.drawn += drawable ? count : 0;
        }
        // Each list holds all that could land in it, so the cull needs no first pass to count them.
        uint32_t slot = 0;
        const uint32_t call_limit = device->capabilities().max_draw_indirect_count;
        const auto add_list = [&](uint32_t index, uint64_t wanted) {
            const auto capacity = static_cast<uint32_t>(std::min<uint64_t>(wanted, UINT32_MAX));
            // What one counted call can't reach is left out, and said so. The spec only promises 65535 draws in a call,
            // though desktop drivers take billions. Splitting a counted call needs the count known on the CPU.
            if (capacity > call_limit && !warned_draw_limit) {
                DE_LOG_WARN("render",
                            "a draw list holds %u draws, and this device draws %u in a call: the rest are lost",
                            capacity, call_limit);
                warned_draw_limit = true;
            }
            const uint32_t kept = std::min(capacity, call_limit);
            plan.lists[index] = {.first_slot = slot, .capacity = kept};
            slot += kept;
        };
        for (uint32_t kind = 0; kind < kDrawKindCount; ++kind) {
            add_list(kind, kind_parts[kind]);
            plan.draws += static_cast<uint32_t>(kind_parts[kind]);
        }
        for (uint32_t c = 0; c < kShadowCascadeCount; ++c) {
            add_list(cascade_list(c, false), kind_parts[0] + kind_parts[1]);
            add_list(cascade_list(c, true), kind_parts[2] + kind_parts[3]);
            plan.shadow_draws += static_cast<uint32_t>(kind_parts[0] + kind_parts[1] + kind_parts[2] + kind_parts[3]);
        }
        std::memcpy(mapped.data() + kDrawListsOffset, plan.lists, sizeof(plan.lists));
        // A power of two, so a graph buffer lasts while the counts move around.
        plan.slot_capacity = 1024;
        while (plan.slot_capacity < slot) {
            plan.slot_capacity *= 2;
        }
    }

    // Writes the frame's constants, instance transforms and what the cull reads; false without a buffer.
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
        const float* d = lighting.sun_direction;
        const float length = std::max(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), 1e-6f);
        for (int i = 0; i < 3; ++i) {
            scene.sun_direction[i] = d[i] / length;
        }
        copy3(lighting.sun_color, scene.sun_color);
        copy3(lighting.sky_color, scene.sky_color);
        copy3(lighting.ground_color, scene.ground_color);
        copy3(lighting.shadow_color, scene.shadow_color);
        const float forward[3] = {-frame.camera.transform[2], -frame.camera.transform[6], -frame.camera.transform[10]};
        const float forward_length =
            std::max(std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]), 1e-6f);
        for (int i = 0; i < 3; ++i) {
            scene.camera_forward[i] = forward[i] / forward_length;
        }
        const float sun[3] = {scene.sun_direction[0], scene.sun_direction[1], scene.sun_direction[2]};
        ShadowCascade cascades[kShadowCascadeCount];
        fit_shadow_cascades(frame.camera.transform, frame.camera.vertical_fov_rad, frame.camera.near_m, aspect, sun,
                            shadows, cascades);
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
        scene.fog_density[1] = fog.height_density_per_m;
        scene.fog_density[2] = fog.height_falloff_per_m;
        scene.fog_density[3] = base_height;
        copy3(fog.extinction_tint, scene.fog_extinction);
        scene.fog_extinction[3] = fog.ambient_height_attenuation;
        copy3(fog.sun_forward, scene.fog_sun_forward);
        scene.fog_sun_forward[3] = fog.sun_height_attenuation;
        copy3(fog.sun_back, scene.fog_sun_back);
        scene.fog_sun_back[3] = fog.sun_start_m;
        copy3(fog.ambient, scene.fog_ambient);
        scene.fog_ambient[3] = fog.start_m;
        scene.fog_range[0] = fog.end_m;
        scene.fog_range[1] = froxel_reach;
        scene.fog_range[2] = kFroxelSliceOffsetM;
        scene.fog_range[3] = static_cast<float>(kFroxelSlices);
        std::memcpy(mapped.data(), &scene, sizeof(scene));
        write_fog_volumes(mapped, frame, froxel_reach, aspect);
        write_cull_data(mapped, frame, scene, height, plan);
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
    lighting.exposure_ev = environment.exposure_ev;
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

void fit_shadow_cascades(const float (&camera)[12], float vertical_fov_rad, float near_m, float aspect,
                         const float (&sun_direction)[3], const SceneShadows& shadows,
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
    double slice_near = near;
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
        // The center snapped to the shadow texel grid, in the light's axes: no swimming.
        glm::dvec3 center = position + forward * z;
        const double cx = std::floor(glm::dot(center, right) / texel) * texel;
        const double cy = std::floor(glm::dot(center, up) / texel) * texel;
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
    const core::Result<rhi::BufferHandle> rows =
        desc.device->create_buffer({.size = uint64_t{desc.max_instances} * kInstanceBytes,
                                    .usage = rhi::BufferUsage::Storage,
                                    .debug_name = "instance rows"});
    if (!rows) {
        return core::fail(rows.error());
    }
    state->instance_rows = *rows;
    state->instance_rows_address = desc.device->buffer_address(*rows);
    const core::Result<rhi::BufferHandle> meshes =
        desc.device->create_buffer({.size = uint64_t{desc.max_instances} * sizeof(uint32_t),
                                    .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst,
                                    .debug_name = "instance meshes"});
    if (!meshes) {
        return core::fail(meshes.error());
    }
    state->instance_meshes = *meshes;
    state->instance_meshes_address = desc.device->buffer_address(*meshes);
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
    // A frame not drawn left a gap in the numbers (hand-made frames have no numbers).
    state.instances_stale_now = frame.number != 0 && frame.number != state.last_frame_number + 1;
    if (state.waiting_for_mesh_slot && state.gpu_assets->mesh_slots_released() != state.mesh_slots_released_seen) {
        state.waiting_for_mesh_slot = false;
        state.instances_stale_now = true; // they're sent again, next frame
    }
    state.last_frame_number = frame.number;
    const rhi::PipelineHandle gbuffer_opaque = state.shaders->pipeline(state.gbuffer_opaque);
    const rhi::PipelineHandle gbuffer_masked = state.shaders->pipeline(state.gbuffer_masked);
    const rhi::PipelineHandle light = state.shaders->pipeline(state.deferred_light);
    const rhi::PipelineHandle tonemap = state.shaders->pipeline(state.tonemap);
    const rhi::PipelineHandle caster_opaque = state.shaders->pipeline(state.shadow_caster_opaque);
    const rhi::PipelineHandle caster_masked = state.shaders->pipeline(state.shadow_caster_masked);
    const rhi::PipelineHandle mask = state.shaders->pipeline(state.shadow_mask);
    const rhi::PipelineHandle fog = state.shaders->pipeline(state.fog_program);
    const rhi::PipelineHandle cull = state.shaders->pipeline(state.cull);
    const rhi::PipelineHandle instance_update = state.shaders->pipeline(state.instance_update);
    const rhi::PipelineHandle fog_inject = state.shaders->pipeline(state.fog_inject);
    const rhi::PipelineHandle fog_integrate = state.shaders->pipeline(state.fog_integrate);
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
    const bool taa_on =
        taa && view == SceneView::Lit && taa_pipeline.is_valid() && state.ensure_temporal(width, height);
    if (taa_on) {
        state.taa_sample = state.taa_sample % kTaaSamples + 1;
    }
    // This frame's constants and draws, now: every pass below draws only if they're in.
    const rhi::BufferHandle frame_buffer =
        frame.camera.valid ? state.frame_buffer(frame_bytes(frame.instances.size() + frame.freed_slots.size()))
                           : rhi::BufferHandle{};
    CullPlan plan;
    const bool written = frame_buffer.is_valid() && state.write_frame(frame_buffer, plan, frame, lighting, froxel_reach,
                                                                      taa_on ? state.taa_sample : 0, width, height);
    const bool ready = written && gbuffer_opaque.is_valid() && gbuffer_masked.is_valid() && light.is_valid() &&
                       tonemap.is_valid() && caster_opaque.is_valid() && caster_masked.is_valid() && mask.is_valid() &&
                       cull.is_valid() && instance_update.is_valid();
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
    state.stats.instances = state.live_instances;
    state.stats.instance_updates = plan.updates;
    state.stats.drawn = plan.drawn;
    state.stats.draws = plan.draws;
    state.stats.shadow_draws = plan.shadow_draws;

    // The GPU cull: what's in each view, into the draw lists the G-buffer and shadow passes draw.
    struct CullClearPass {
        GraphBuffer counts;
    };
    const auto& cull_clear = graph.add_pass<CullClearPass>(
        "cull clear",
        [&](PassBuilder& pass, CullClearPass& data) {
            data.counts = pass.create_buffer({.size = kCullCounterCount * sizeof(uint32_t),
                                              .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect |
                                                       rhi::BufferUsage::CopyDst | rhi::BufferUsage::CopySrc,
                                              .debug_name = "draw counts"});
            pass.use(data.counts, rhi::ResourceState::CopyDst);
        },
        [](const CullClearPass& data, PassContext& context) {
            context.list.fill_buffer(context.buffer(data.counts), 0, kCullCounterCount * sizeof(uint32_t), 0);
        });
    const GraphBuffer draw_counts = cull_clear.counts;

    // What changed in the instances. They stay between frames, so the graph is told how they were left.
    const GraphBuffer instance_rows_graph = graph.import_buffer(
        {.buffer = state.instance_rows,
         .name = "instance rows",
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
                pass.use(instance_rows_graph, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
                pass.use(instance_meshes_graph, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            },
            [renderer = &state, pipeline = instance_update, frame_address,
             updates = plan.updates](PassContext& context) {
                DE_PROFILE_ZONE("instance update pass");
                const InstanceUpdatePush push{.updates = frame_address + kInstanceUpdatesOffset,
                                              .instances = renderer->instance_rows_address,
                                              .instance_meshes = renderer->instance_meshes_address,
                                              .count = updates};
                context.list.bind_pipeline(pipeline);
                context.list.push_constants(push);
                context.list.dispatch((updates + 63) / 64);
            });
    }
    struct CullPass {
        SceneRendererState* state = nullptr;
        GraphBuffer counts;
        GraphBuffer draws;
        rhi::PipelineHandle pipeline;
        uint64_t frame_address = 0;
        CullPlan plan;
    };
    const auto& cull_pass = graph.add_pass<CullPass>(
        "gpu cull",
        [&](PassBuilder& pass, CullPass& data) {
            data = {.state = &state,
                    .counts = draw_counts,
                    .draws = {},
                    .pipeline = cull,
                    .frame_address = frame_address,
                    .plan = plan};
            data.draws = pass.create_buffer({.size = plan.buffer_bytes(),
                                             .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect,
                                             .debug_name = "draw lists"});
            pass.use(data.draws, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            pass.use(data.counts, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            pass.use(instance_rows_graph, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
            pass.use(instance_meshes_graph, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
        },
        [](const CullPass& data, PassContext& context) {
            DE_PROFILE_ZONE("gpu cull pass");
            const SceneRendererState& renderer = *data.state;
            // Before the mesh table holds anything, there's nothing to cull against.
            if (data.plan.slots == 0 || !renderer.gpu_assets->mesh_table_ready()) {
                return;
            }
            const uint64_t draws = renderer.device->buffer_address(context.buffer(data.draws));
            const CullPush push{.instances = renderer.instance_rows_address,
                                .instance_meshes = renderer.instance_meshes_address,
                                .meshes = renderer.gpu_assets->mesh_table_address(),
                                .lods = renderer.gpu_assets->lod_table_address(),
                                .parts = renderer.gpu_assets->part_table_address(),
                                .materials = renderer.gpu_assets->material_table_address(),
                                .views = data.frame_address + kCullViewsOffset,
                                .lists = data.frame_address + kDrawListsOffset,
                                .args = draws,
                                .records = draws + data.plan.records_offset(),
                                .counts = renderer.device->buffer_address(context.buffer(data.counts)),
                                .instance_count = data.plan.slots};
            context.list.bind_pipeline(data.pipeline);
            context.list.push_constants(push);
            context.list.dispatch((data.plan.slots + 63) / 64);
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

    // The sun's shadow atlas: four cascades, each a tile, every instance drawn into each.
    struct ShadowPass {
        SceneRendererState* state = nullptr;
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
            data.atlas = pass.create_texture(
                {.desc = {.width = cascade_size * 2, .height = cascade_size * 2, .format = kDepthFormat}});
            pass.use(data.atlas, rhi::ResourceState::DepthWrite);
            pass.use(instance_rows_graph, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Vertex);
        },
        [](const ShadowPass& data, PassContext& context) {
            DE_PROFILE_ZONE("shadow pass");
            SceneRendererState& renderer = *data.state;
            const rhi::BufferHandle args = context.buffer(data.draws);
            const rhi::BufferHandle counts = context.buffer(data.counts);
            ShadowCasterPush push{.frame = data.frame_address,
                                  .instances = renderer.instance_rows_address,
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
    const auto cull_slot = static_cast<uint32_t>(state.device->frame_index() % rhi::kFramesInFlight);

    struct Targets {
        GraphTexture gb[3];
        GraphTexture hdr;
        GraphTexture depth;
        GraphTexture motion;
    };
    struct GBufferPass {
        SceneRendererState* state = nullptr;
        Targets targets;
        rhi::PipelineHandle opaque;
        rhi::PipelineHandle masked;
        uint64_t frame_address = 0;
        GraphBuffer draws;
        GraphBuffer counts;
        CullPlan plan;
    };
    const auto& gbuffer = graph.add_pass<GBufferPass>(
        "gbuffer",
        [&](PassBuilder& pass, GBufferPass& data) {
            data = {.state = &state,
                    .targets = {},
                    .opaque = gbuffer_opaque,
                    .masked = gbuffer_masked,
                    .frame_address = frame_address,
                    .draws = draw_lists,
                    .counts = draw_counts,
                    .plan = plan};
            pass.use(draw_lists, rhi::ResourceState::IndirectArgument | rhi::ResourceState::ShaderRead,
                     rhi::ShaderStages::Vertex);
            pass.use(draw_counts, rhi::ResourceState::IndirectArgument);
            for (uint32_t i = 0; i < 3; ++i) {
                data.targets.gb[i] =
                    pass.create_texture({.desc = {.format = kGBufferFormats[i]}, .reference_scale = 1.0f});
                pass.use(data.targets.gb[i], rhi::ResourceState::RenderTarget);
            }
            data.targets.hdr = pass.create_texture({.desc = {.format = kHdrFormat}, .reference_scale = 1.0f});
            data.targets.depth =
                taa_on ? kept_depth : pass.create_texture({.desc = {.format = kDepthFormat}, .reference_scale = 1.0f});
            data.targets.motion = pass.create_texture({.desc = {.format = kMotionFormat}, .reference_scale = 1.0f});
            pass.use(data.targets.hdr, rhi::ResourceState::RenderTarget);
            pass.use(data.targets.motion, rhi::ResourceState::RenderTarget);
            pass.use(data.targets.depth, rhi::ResourceState::DepthWrite);
            pass.use(instance_rows_graph, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Vertex);
        },
        [](const GBufferPass& data, PassContext& context) {
            DE_PROFILE_ZONE("gbuffer pass");
            SceneRendererState& renderer = *data.state;
            rhi::ColorAttachment colors[5];
            for (uint32_t i = 0; i < 3; ++i) {
                colors[i] = {.texture = context.texture(data.targets.gb[i])};
            }
            colors[3] = {.texture = context.texture(data.targets.hdr)};
            colors[4] = {.texture = context.texture(data.targets.motion)};
            const rhi::DepthAttachment depth{.texture = context.texture(data.targets.depth)};
            context.list.begin_rendering({.colors = colors, .depth = &depth});
            context.list.bind_index_buffer(renderer.gpu_assets->pool_index_buffer(), 0, rhi::IndexType::Uint32);
            const GBufferPush push{.frame = data.frame_address,
                                   .instances = renderer.instance_rows_address,
                                   .vertices = renderer.gpu_assets->pool_vertex_address(),
                                   .materials = renderer.gpu_assets->material_table_address(),
                                   .draws = renderer.device->buffer_address(context.buffer(data.draws)) +
                                            data.plan.records_offset(),
                                   .sampler = rhi::sampler_index(rhi::DefaultSampler::AnisotropicWrap)};
            context.list.push_constants(push);
            for (uint32_t kind = 0; kind < kDrawKindCount; ++kind) {
                if (data.plan.lists[kind].capacity == 0) {
                    continue;
                }
                context.list.bind_pipeline(is_masked(kind) ? data.masked : data.opaque); // culls back faces
                if (is_double_sided(kind)) {
                    context.list.set_cull_mode(rhi::CullMode::None);
                }
                renderer.draw_list(context.list, context.buffer(data.draws), context.buffer(data.counts), data.plan,
                                   kind);
            }
            context.list.end_rendering();
        });
    const Targets targets = gbuffer.targets;

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

    // The shadow mask: how much sun each pixel sees.
    struct MaskPass {
        GraphTexture mask;
    };
    const auto& mask_pass = graph.add_pass<MaskPass>(
        "shadow mask",
        [&](PassBuilder& pass, MaskPass& data) {
            data.mask = pass.create_texture({.desc = {.format = kShadowMaskFormat}, .reference_scale = 1.0f});
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

    graph.add_pass(
        "deferred light",
        [&](PassBuilder& pass) {
            for (const GraphTexture gb : targets.gb) {
                pass.use(gb, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            }
            pass.use(targets.depth, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            pass.use(shadow_mask, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            pass.use(targets.hdr, rhi::ResourceState::RenderTarget);
        },
        [targets, shadow_mask, frame_address, light](PassContext& context) {
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
            });
            context.list.draw(3);
            context.list.end_rendering();
        });

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
        const uint32_t grid_width = std::max((graph.reference_width() + kFroxelTilePixels - 1) / kFroxelTilePixels, 1U);
        const uint32_t grid_height =
            std::max((graph.reference_height() + kFroxelTilePixels - 1) / kFroxelTilePixels, 1U);
        const rhi::TextureDesc grid{.width = grid_width,
                                    .height = grid_height,
                                    .depth = kFroxelSlices,
                                    .format = kFroxelFormat,
                                    .dimension = rhi::TextureDimension::Texture3D};
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
                        data.volume = froxels_kept ? kept_froxels : pass.create_texture({.desc = grid});
                        pass.use(atlas, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
                        pass.use(data.volume, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
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
                                 data.volume = pass.create_texture({.desc = grid});
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
                        data.fogged = pass.create_texture({.desc = {.format = kHdrFormat}, .reference_scale = 1.0f});
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

    const float exposure = std::exp2(lighting.exposure_ev);

    // TAA: this frame into its history (what the tonemap shows, and next frame's history).
    GraphTexture resolved = fogged;
    if (taa_on) {
        graph.add_pass(
            "taa",
            [&](PassBuilder& pass) {
                pass.use(fogged, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                pass.use(targets.depth, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                pass.use(targets.motion, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                if (has_history) {
                    pass.use(last_history, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                    pass.use(last_depth, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                    pass.use(last_object_motion, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
                }
                pass.use(kept_history, rhi::ResourceState::RenderTarget);
                pass.use(kept_object_motion, rhi::ResourceState::RenderTarget);
            },
            [fogged, targets, kept_history, kept_object_motion, last_history, last_depth, last_object_motion,
             has_history, frame_address, taa_pipeline, exposure, renderer = &state,
             frame_number = frame.number](PassContext& context) {
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
                });
                context.list.draw(3);
                context.list.end_rendering();
            });
        resolved = kept_history;
    }

    const auto view_index = static_cast<uint32_t>(view);
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
            pass.use(target, rhi::ResourceState::RenderTarget);
        },
        [targets, resolved, shadow_mask, target, tonemap, exposure, view_index, srgb_target](PassContext& context) {
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
            });
            context.list.draw(3);
            context.list.end_rendering();
        });
}

void SceneRenderer::set_lod_error_pixels(float pixels) {
    state_->lod_error_pixels = pixels;
}

bool SceneRenderer::instances_stale() const {
    return state_->instances_stale_now;
}

SceneRendererStats SceneRenderer::stats() const {
    return state_->stats;
}

} // namespace de::render
