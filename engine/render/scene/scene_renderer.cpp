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
    float fog_density[4] = {};         // constant, height base, falloff, base height (render-origin space)
    float fog_extinction[4] = {};      // tint, ambient height attenuation
    float fog_sun_forward[4] = {};     // colour, sun height attenuation
    float fog_sun_back[4] = {};        // colour, sun start
    float fog_ambient[4] = {};         // colour, start
    float fog_range[4] = {};           // end, froxel grid reach, slice offset, slices
    float previous_view_proj[16] = {}; // last frame's camera, relative to this frame's render origin
};
static_assert(sizeof(SceneFrame) <= 1024);

// Matches GBufferPush in shaders/scene/gbuffer.slang.
struct GBufferPush {
    uint64_t frame = 0;
    uint64_t instances = 0;
    uint64_t vertices = 0;
    uint64_t materials = 0;
    uint32_t instance = 0;
    uint32_t material = 0;
    uint32_t sampler = 0;
};

// Matches ShadowCasterPush in shaders/scene/shadow_caster.slang.
struct ShadowCasterPush {
    uint64_t frame = 0;
    uint64_t instances = 0;
    uint64_t vertices = 0;
    uint64_t materials = 0;
    uint32_t instance = 0;
    uint32_t material = 0;
    uint32_t sampler = 0;
    uint32_t cascade = 0;
};

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
};

// Matches FogInjectPush in shaders/scene/fog_inject.slang.
struct FogInjectPush {
    uint64_t frame = 0;
    uint32_t volume = 0;
    uint32_t atlas = 0;
    uint32_t width = 0;
    uint32_t height = 0;
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
constexpr float kFroxelSliceOffsetM = 2.0f;
constexpr rhi::Format kFroxelFormat = rhi::Format::RGBA16Float;

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

constexpr uint64_t kInstancesOffset = 1024; // after the SceneFrame
constexpr uint64_t kInstanceBytes = 96;     // six float4 rows: its transform, and last frame's

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

// A mesh part to draw this frame, and its kind: [masked][double-sided] as two bits.
struct PartDraw {
    uint64_t vertices = 0;
    rhi::BufferHandle indices;
    uint32_t instance = 0;
    uint32_t material = 0;
    uint32_t first_index = 0;
    uint32_t index_count = 0;
};

constexpr uint32_t kDrawKindCount = 4; // opaque, opaque double-sided, masked, masked double-sided

constexpr uint32_t draw_kind(GpuMaterialFlags flags) {
    return (core::has_any(flags, GpuMaterialFlags::Masked) ? 2U : 0U) |
           (core::has_any(flags, GpuMaterialFlags::DoubleSided) ? 1U : 0U);
}
constexpr bool is_masked(uint32_t kind) {
    return (kind & 2U) != 0;
}
constexpr bool is_double_sided(uint32_t kind) {
    return (kind & 1U) != 0;
}

// Draws parts with the bound pipeline, binding index buffers as they change. Returns the draws.
template <typename Push>
uint32_t draw_parts(rhi::CommandList& list, std::span<const PartDraw> parts, Push push) {
    rhi::BufferHandle bound;
    for (const PartDraw& part : parts) {
        if (!(part.indices == bound)) {
            list.bind_index_buffer(part.indices, 0, rhi::IndexType::Uint32);
            bound = part.indices;
        }
        push.instance = part.instance;
        push.vertices = part.vertices;
        push.material = part.material;
        list.push_constants(push);
        list.draw_indexed(part.index_count, 1, part.first_index);
    }
    return static_cast<uint32_t>(parts.size());
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
    // This frame's mesh parts by kind, for the shadow and G-buffer passes: opaque first, so masked
    // pixels behind them fail early depth; each kind one pipeline state. Capacity kept between frames.
    core::Array<PartDraw> draws[kDrawKindCount] = {
        core::Array<PartDraw>(core::heap_allocator(core::MemoryTag::Render)),
        core::Array<PartDraw>(core::heap_allocator(core::MemoryTag::Render)),
        core::Array<PartDraw>(core::heap_allocator(core::MemoryTag::Render)),
        core::Array<PartDraw>(core::heap_allocator(core::MemoryTag::Render))};
    SceneShadows shadows; // this frame's, within limits
    // Last frame's camera, for motion vectors: what it was relative to the render origin then.
    float previous_camera[12] = {};
    float previous_fov_rad = 0.0f;
    float previous_near_m = 0.0f;
    DVec3 previous_origin{0.0};
    bool has_previous_camera = false;
    rhi::Format programs_format = rhi::Format::Unknown; // the target's, for the tonemap
    rhi::BufferHandle frame_buffers[rhi::kFramesInFlight];
    uint64_t frame_capacity[rhi::kFramesInFlight] = {};
    SceneRendererStats stats;

    ~SceneRendererState() {
        for (const shader::ProgramHandle program :
             {gbuffer_opaque, gbuffer_masked, deferred_light, tonemap, shadow_caster_opaque, shadow_caster_masked,
              shadow_mask, fog_program, fog_inject, fog_integrate}) {
            shaders->destroy(program);
        }
        for (const rhi::BufferHandle buffer : frame_buffers) {
            device->destroy(buffer);
        }
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
        const core::Result<shader::ProgramHandle> inject =
            shaders->create_compute({.module = "scene.fog_inject", .debug_name = "fog inject"});
        fog_inject = inject ? *inject : shader::ProgramHandle{};
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

    // The frame's parts whose meshes are on the GPU, by kind; one material lookup each.
    void sort_draws(const world::RenderFrame& frame) {
        for (core::Array<PartDraw>& list : draws) {
            list.clear();
        }
        for (size_t i = 0; i < frame.instances.size(); ++i) {
            const GpuMesh* mesh = gpu_assets->mesh(frame.instances[i].mesh);
            if (mesh == nullptr) {
                continue;
            }
            ++stats.drawn;
            for (const GpuMeshPart& part : mesh->parts) {
                draws[draw_kind(gpu_assets->material_flags(part.material))].push({.vertices = mesh->vertex_address,
                                                                                  .indices = mesh->indices,
                                                                                  .instance = static_cast<uint32_t>(i),
                                                                                  .material = part.material,
                                                                                  .first_index = part.first_index,
                                                                                  .index_count = part.index_count});
            }
        }
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

    // This frame slot's buffer, big enough for count instances: written by the CPU now, read by
    // this frame's GPU work (the slot's last frame is done: begin_frame waited for it).
    rhi::BufferHandle frame_buffer(size_t count) {
        const auto slot = static_cast<uint32_t>(device->frame_index() % rhi::kFramesInFlight);
        const uint64_t needed = kInstancesOffset + std::max<uint64_t>(count, 1) * kInstanceBytes;
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

    // Writes the frame's constants and instance transforms; false without a buffer.
    bool write_frame(rhi::BufferHandle buffer, const world::RenderFrame& frame, const SceneLighting& lighting,
                     float froxel_reach, float aspect) {
        const std::span<std::byte> mapped = device->mapped(buffer);
        if (mapped.empty()) {
            return false;
        }
        SceneFrame scene;
        camera_view_proj(frame.camera.transform, frame.camera.vertical_fov_rad, frame.camera.near_m, aspect,
                         scene.view_proj);
        if (!invert_4x4(scene.view_proj, scene.inv_view_proj)) {
            return false;
        }
        // Last frame's camera, moved to this frame's render origin; the same as this one's at first.
        if (has_previous_camera) {
            for (const int row : {0, 1, 2}) {
                const double shifted =
                    static_cast<double>(previous_camera[row * 4 + 3]) + previous_origin[row] - frame.origin[row];
                previous_camera[row * 4 + 3] = static_cast<float>(shifted);
            }
            camera_view_proj(previous_camera, previous_fov_rad, previous_near_m, aspect, scene.previous_view_proj);
        } else {
            std::memcpy(scene.previous_view_proj, scene.view_proj, sizeof(scene.view_proj));
        }
        std::memcpy(previous_camera, frame.camera.transform, sizeof(previous_camera));
        previous_fov_rad = frame.camera.vertical_fov_rad;
        previous_near_m = frame.camera.near_m;
        previous_origin = frame.origin;
        has_previous_camera = true;
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
        for (size_t i = 0; i < frame.instances.size(); ++i) {
            static_assert(offsetof(world::RenderInstance, previous_transform) == 48); // right after transform
            std::memcpy(mapped.data() + kInstancesOffset + i * kInstanceBytes, frame.instances[i].transform,
                        kInstanceBytes);
        }
        device->flush_mapped(buffer, 0, kInstancesOffset + frame.instances.size() * kInstanceBytes);
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
    state->make_scene_programs();
    return SceneRenderer(state);
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
                               const world::RenderFrame& frame, const SceneLighting& lighting, SceneView view,
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
    state.stats = {.instances = static_cast<uint32_t>(frame.instances.size())};
    const rhi::PipelineHandle gbuffer_opaque = state.shaders->pipeline(state.gbuffer_opaque);
    const rhi::PipelineHandle gbuffer_masked = state.shaders->pipeline(state.gbuffer_masked);
    const rhi::PipelineHandle light = state.shaders->pipeline(state.deferred_light);
    const rhi::PipelineHandle tonemap = state.shaders->pipeline(state.tonemap);
    const rhi::PipelineHandle caster_opaque = state.shaders->pipeline(state.shadow_caster_opaque);
    const rhi::PipelineHandle caster_masked = state.shaders->pipeline(state.shadow_caster_masked);
    const rhi::PipelineHandle mask = state.shaders->pipeline(state.shadow_mask);
    const rhi::PipelineHandle fog = state.shaders->pipeline(state.fog_program);
    const rhi::PipelineHandle fog_inject = state.shaders->pipeline(state.fog_inject);
    const rhi::PipelineHandle fog_integrate = state.shaders->pipeline(state.fog_integrate);
    // Fog only in the lit view, if there is any; if its shaders are broken, no fog rather than no
    // scene. The grid likewise (the closed form goes on alone). What it adds over the closed form is
    // the sun's shadows, so it reaches no farther than they do.
    const bool foggy = view == SceneView::Lit && fog.is_valid() &&
                       (lighting.fog.density_per_m > 0.0f || lighting.fog.height_density_per_m > 0.0f);
    const bool froxels_on =
        foggy && lighting.fog.volume_distance_m > 0.0f && fog_inject.is_valid() && fog_integrate.is_valid();
    const float froxel_reach = froxels_on ? std::min(lighting.fog.volume_distance_m, state.shadows.distance_m) : 0.0f;
    state.stats.froxel_reach_m = froxel_reach;
    // This frame's constants, now: every pass below draws only if they're in.
    const rhi::BufferHandle frame_buffer =
        frame.camera.valid ? state.frame_buffer(frame.instances.size()) : rhi::BufferHandle{};
    const float aspect =
        static_cast<float>(graph.reference_width()) / static_cast<float>(std::max(graph.reference_height(), 1U));
    const bool written =
        frame_buffer.is_valid() && state.write_frame(frame_buffer, frame, lighting, froxel_reach, aspect);
    const bool ready = written && gbuffer_opaque.is_valid() && gbuffer_masked.is_valid() && light.is_valid() &&
                       tonemap.is_valid() && caster_opaque.is_valid() && caster_masked.is_valid() && mask.is_valid();
    if (!ready) {
        // Nothing to look through, or shaders missing: the target is cleared.
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
    state.sort_draws(frame);

    // The sun's shadow atlas: four cascades, each a tile, every instance drawn into each.
    struct ShadowPass {
        SceneRendererState* state = nullptr;
        GraphTexture atlas;
        rhi::PipelineHandle opaque;
        rhi::PipelineHandle masked;
        uint64_t frame_address = 0;
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
                    .cascade_size = cascade_size};
            data.atlas = pass.create_texture(
                {.desc = {.width = cascade_size * 2, .height = cascade_size * 2, .format = kDepthFormat}});
            pass.use(data.atlas, rhi::ResourceState::DepthWrite);
        },
        [](const ShadowPass& data, PassContext& context) {
            DE_PROFILE_ZONE("shadow pass");
            SceneRendererState& renderer = *data.state;
            ShadowCasterPush push{.frame = data.frame_address,
                                  .instances = data.frame_address + kInstancesOffset,
                                  .materials = renderer.gpu_assets->material_table_address(),
                                  .sampler = rhi::sampler_index(rhi::DefaultSampler::AnisotropicWrap)};
            for (uint32_t cascade = 0; cascade < kShadowCascadeCount; ++cascade) {
                const rhi::DepthAttachment depth{.texture = context.texture(data.atlas)};
                const rhi::Rect tile{.x = static_cast<int32_t>((cascade & 1U) * data.cascade_size),
                                     .y = static_cast<int32_t>((cascade >> 1U) * data.cascade_size),
                                     .width = data.cascade_size,
                                     .height = data.cascade_size};
                context.list.begin_rendering({.depth = &depth, .area = tile});
                push.cascade = cascade;
                // Casters cull nothing (thin geometry casts from both sides): the kinds differ only
                // by the alpha test.
                for (uint32_t kind = 0; kind < kDrawKindCount; ++kind) {
                    if (!renderer.draws[kind].empty()) {
                        context.list.bind_pipeline(is_masked(kind) ? data.masked : data.opaque);
                        renderer.stats.shadow_draws += draw_parts(context.list, renderer.draws[kind].span(), push);
                    }
                }
                context.list.end_rendering();
            }
        });
    const GraphTexture atlas = shadow.atlas;

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
    };
    const auto& gbuffer = graph.add_pass<GBufferPass>(
        "gbuffer",
        [&](PassBuilder& pass, GBufferPass& data) {
            data = {.state = &state,
                    .targets = {},
                    .opaque = gbuffer_opaque,
                    .masked = gbuffer_masked,
                    .frame_address = frame_address};
            for (uint32_t i = 0; i < 3; ++i) {
                data.targets.gb[i] =
                    pass.create_texture({.desc = {.format = kGBufferFormats[i]}, .reference_scale = 1.0f});
                pass.use(data.targets.gb[i], rhi::ResourceState::RenderTarget);
            }
            data.targets.hdr = pass.create_texture({.desc = {.format = kHdrFormat}, .reference_scale = 1.0f});
            data.targets.depth = pass.create_texture({.desc = {.format = kDepthFormat}, .reference_scale = 1.0f});
            data.targets.motion = pass.create_texture({.desc = {.format = kMotionFormat}, .reference_scale = 1.0f});
            pass.use(data.targets.hdr, rhi::ResourceState::RenderTarget);
            pass.use(data.targets.motion, rhi::ResourceState::RenderTarget);
            pass.use(data.targets.depth, rhi::ResourceState::DepthWrite);
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
            const GBufferPush push{.frame = data.frame_address,
                                   .instances = data.frame_address + kInstancesOffset,
                                   .materials = renderer.gpu_assets->material_table_address(),
                                   .sampler = rhi::sampler_index(rhi::DefaultSampler::AnisotropicWrap)};
            for (uint32_t kind = 0; kind < kDrawKindCount; ++kind) {
                if (renderer.draws[kind].empty()) {
                    continue;
                }
                context.list.bind_pipeline(is_masked(kind) ? data.masked : data.opaque); // culls back faces
                if (is_double_sided(kind)) {
                    context.list.set_cull_mode(rhi::CullMode::None);
                }
                renderer.stats.draws += draw_parts(context.list, renderer.draws[kind].span(), push);
            }
            context.list.end_rendering();
        });
    const Targets targets = gbuffer.targets;

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
    if (froxels_on) {
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
        const GraphTexture froxels =
            graph
                .add_pass<GridPass>(
                    "fog inject",
                    [&](PassBuilder& pass, GridPass& data) {
                        data.volume = pass.create_texture({.desc = grid});
                        pass.use(atlas, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
                        pass.use(data.volume, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
                    },
                    [atlas, frame_address, fog_inject, grid_width, grid_height](const GridPass& data,
                                                                                PassContext& context) {
                        DE_PROFILE_ZONE("fog inject pass");
                        context.list.bind_pipeline(fog_inject);
                        context.list.push_constants(FogInjectPush{
                            .frame = frame_address,
                            .volume = context.device.storage_texture_index(context.texture(data.volume)),
                            .atlas = context.device.texture_index(context.texture(atlas)),
                            .width = grid_width,
                            .height = grid_height,
                        });
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
                    [targets, frame_address, fog, froxels_on, integrated](const FogPass& data, PassContext& context) {
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
                        });
                        context.list.draw(3);
                        context.list.end_rendering();
                    })
                .fogged;
    }

    const float exposure = std::exp2(lighting.exposure_ev);
    const auto view_index = static_cast<uint32_t>(view);
    const uint32_t srgb_target = rhi::format_info(target_format).srgb ? 1 : 0;
    graph.add_pass(
        "tonemap",
        [&](PassBuilder& pass) {
            pass.use(fogged, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            for (const GraphTexture gb : targets.gb) {
                pass.use(gb, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            }
            pass.use(shadow_mask, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            pass.use(targets.motion, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            pass.use(target, rhi::ResourceState::RenderTarget);
        },
        [targets, fogged, shadow_mask, target, tonemap, exposure, view_index, srgb_target](PassContext& context) {
            DE_PROFILE_ZONE("tonemap pass");
            const rhi::ColorAttachment color{.texture = context.texture(target), .load = rhi::LoadOp::DontCare};
            context.list.begin_rendering({.colors = std::span(&color, 1)});
            context.list.bind_pipeline(tonemap);
            context.list.push_constants(TonemapPush{
                .hdr = context.device.texture_index(context.texture(fogged)),
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

SceneRendererStats SceneRenderer::stats() const {
    return state_->stats;
}

} // namespace de::render
