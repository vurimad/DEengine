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
};

constexpr uint64_t kInstancesOffset = 1024; // after the SceneFrame
constexpr uint64_t kInstanceBytes = 48;     // three float4 rows

// The G-buffer's formats (shaders/scene/scene_common.slang documents the channels).
constexpr rhi::Format kGBufferFormats[3] = {rhi::Format::RGBA8Srgb, rhi::Format::RG16Unorm, rhi::Format::RGBA8Unorm};
constexpr rhi::Format kHdrFormat = rhi::Format::RGBA16Float;
constexpr rhi::Format kDepthFormat = rhi::Format::D32Float;
constexpr rhi::Format kShadowMaskFormat = rhi::Format::RG8Unorm;

void copy3(const float (&from)[3], float (&to)[4]) {
    to[0] = from[0];
    to[1] = from[1];
    to[2] = from[2];
}

} // namespace

struct SceneRendererState {
    rhi::Device* device = nullptr;
    shader::ShaderLibrary* shaders = nullptr;
    GpuAssets* gpu_assets = nullptr;
    shader::ProgramHandle gbuffer_cull_back;
    shader::ProgramHandle gbuffer_cull_none; // double-sided materials
    shader::ProgramHandle deferred_light;
    shader::ProgramHandle tonemap;
    shader::ProgramHandle shadow_caster;        // masked: alpha-tested
    shader::ProgramHandle shadow_caster_opaque; // depth only
    core::Array<const GpuMesh*> shadow_meshes{
        core::heap_allocator(core::MemoryTag::Render)}; // per instance, this frame
    shader::ProgramHandle shadow_mask;
    SceneShadows shadows;
    rhi::Format programs_format = rhi::Format::Unknown; // the target's, for the tonemap
    rhi::BufferHandle frame_buffers[rhi::kFramesInFlight];
    uint64_t frame_capacity[rhi::kFramesInFlight] = {};
    SceneRendererStats stats;

    ~SceneRendererState() {
        for (const shader::ProgramHandle program : {gbuffer_cull_back, gbuffer_cull_none, deferred_light, tonemap,
                                                    shadow_caster, shadow_caster_opaque, shadow_mask}) {
            shaders->destroy(program);
        }
        for (const rhi::BufferHandle buffer : frame_buffers) {
            device->destroy(buffer);
        }
    }

    shader::ProgramHandle make(const char* module, const rhi::GraphicsPipelineDesc& state) const {
        const core::Result<shader::ProgramHandle> program =
            shaders->create_graphics({.module = module, .state = state});
        return program ? *program : shader::ProgramHandle{};
    }

    // The G-buffer and light programs: their formats are the renderer's own, so made once.
    void make_scene_programs() {
        rhi::GraphicsPipelineDesc gbuffer{.color_count = 4,
                                          .depth_format = kDepthFormat,
                                          .depth_test = true,
                                          .depth_write = true,
                                          .depth_compare = rhi::CompareOp::Greater,
                                          .debug_name = "gbuffer"};
        for (uint32_t i = 0; i < 3; ++i) {
            gbuffer.color_formats[i] = kGBufferFormats[i];
        }
        gbuffer.color_formats[3] = kHdrFormat;
        gbuffer_cull_back = make("scene.gbuffer", gbuffer);
        gbuffer.cull = rhi::CullMode::None;
        gbuffer.debug_name = "gbuffer (double-sided)";
        gbuffer_cull_none = make("scene.gbuffer", gbuffer);

        rhi::GraphicsPipelineDesc light{.color_count = 1, .cull = rhi::CullMode::None, .debug_name = "deferred light"};
        light.color_formats[0] = kHdrFormat;
        light.blend[0] = rhi::BlendMode::Additive;
        deferred_light = make("scene.deferred_light", light);

        // Casters: depth only, no culling (thin geometry casts from both sides).
        const rhi::GraphicsPipelineDesc caster{.depth_format = kDepthFormat,
                                               .depth_test = true,
                                               .depth_write = true,
                                               .depth_compare = rhi::CompareOp::Greater,
                                               .cull = rhi::CullMode::None,
                                               .debug_name = "shadow caster"};
        shadow_caster = make("scene.shadow_caster", caster);
        const core::Result<shader::ProgramHandle> opaque = shaders->create_graphics(
            {.module = "scene.shadow_caster", .fragment_entry = "fragment_opaque", .state = caster});
        shadow_caster_opaque = opaque ? *opaque : shader::ProgramHandle{};
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
                     float aspect) {
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
        std::memcpy(mapped.data(), &scene, sizeof(scene));
        for (size_t i = 0; i < frame.instances.size(); ++i) {
            std::memcpy(mapped.data() + kInstancesOffset + i * kInstanceBytes, frame.instances[i].transform,
                        kInstanceBytes);
        }
        device->flush_mapped(buffer, 0, kInstancesOffset + frame.instances.size() * kInstanceBytes);
        return true;
    }
};

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
    state->shadows = desc.shadows;
    state->shadows.resolution = std::clamp(state->shadows.resolution, 64U, 8192U); // the atlas is twice that
    state->shadows.max_penumbra_texels = std::max(state->shadows.max_penumbra_texels, 1.0f);
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
    state.make_tonemap(target_format);
    state.stats = {.instances = static_cast<uint32_t>(frame.instances.size())};
    const rhi::PipelineHandle gbuffer_back = state.shaders->pipeline(state.gbuffer_cull_back);
    const rhi::PipelineHandle gbuffer_none = state.shaders->pipeline(state.gbuffer_cull_none);
    const rhi::PipelineHandle light = state.shaders->pipeline(state.deferred_light);
    const rhi::PipelineHandle tonemap = state.shaders->pipeline(state.tonemap);
    const rhi::PipelineHandle caster = state.shaders->pipeline(state.shadow_caster);
    const rhi::PipelineHandle caster_opaque = state.shaders->pipeline(state.shadow_caster_opaque);
    const rhi::PipelineHandle mask = state.shaders->pipeline(state.shadow_mask);
    // This frame's constants, now: every pass below draws only if they're in.
    const rhi::BufferHandle frame_buffer =
        frame.camera.valid ? state.frame_buffer(frame.instances.size()) : rhi::BufferHandle{};
    const float aspect =
        static_cast<float>(graph.reference_width()) / static_cast<float>(std::max(graph.reference_height(), 1U));
    const bool written = frame_buffer.is_valid() && state.write_frame(frame_buffer, frame, lighting, aspect);
    const bool ready = written && gbuffer_back.is_valid() && gbuffer_none.is_valid() && light.is_valid() &&
                       tonemap.is_valid() && caster.is_valid() && caster_opaque.is_valid() && mask.is_valid();
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

    // The sun's shadow atlas: four cascades, each a tile, every instance drawn into each.
    struct ShadowPass {
        SceneRendererState* state = nullptr;
        const world::RenderFrame* frame = nullptr;
        GraphTexture atlas;
        rhi::PipelineHandle caster;
        rhi::PipelineHandle caster_opaque;
        uint64_t frame_address = 0;
        uint32_t cascade_size = 0;
    };
    const auto& shadow = graph.add_pass<ShadowPass>(
        "sun shadows",
        [&](PassBuilder& pass, ShadowPass& data) {
            data = {.state = &state,
                    .frame = &frame,
                    .atlas = {},
                    .caster = caster,
                    .caster_opaque = caster_opaque,
                    .frame_address = frame_address,
                    .cascade_size = cascade_size};
            data.atlas = pass.create_texture(
                {.desc = {.width = cascade_size * 2, .height = cascade_size * 2, .format = kDepthFormat}});
            pass.use(data.atlas, rhi::ResourceState::DepthWrite);
        },
        [](const ShadowPass& data, PassContext& context) {
            DE_PROFILE_ZONE("shadow pass");
            SceneRendererState& renderer = *data.state;
            const world::RenderFrame& extracted = *data.frame;
            ShadowCasterPush push{.frame = data.frame_address,
                                  .instances = data.frame_address + kInstancesOffset,
                                  .materials = renderer.gpu_assets->material_table_address(),
                                  .sampler = rhi::sampler_index(rhi::DefaultSampler::AnisotropicWrap)};
            // The meshes once, for every cascade.
            renderer.shadow_meshes.clear();
            for (const world::RenderInstance& instance : extracted.instances) {
                renderer.shadow_meshes.push(renderer.gpu_assets->mesh(instance.mesh));
            }
            for (uint32_t cascade = 0; cascade < kShadowCascadeCount; ++cascade) {
                const rhi::DepthAttachment depth{.texture = context.texture(data.atlas)};
                const rhi::Rect tile{.x = static_cast<int32_t>((cascade & 1U) * data.cascade_size),
                                     .y = static_cast<int32_t>((cascade >> 1U) * data.cascade_size),
                                     .width = data.cascade_size,
                                     .height = data.cascade_size};
                context.list.begin_rendering({.depth = &depth, .area = tile});
                push.cascade = cascade;
                rhi::PipelineHandle bound;
                for (size_t i = 0; i < renderer.shadow_meshes.size(); ++i) {
                    const GpuMesh* mesh = renderer.shadow_meshes[i];
                    if (mesh == nullptr) {
                        continue;
                    }
                    push.instance = static_cast<uint32_t>(i);
                    push.vertices = mesh->vertex_address;
                    context.list.bind_index_buffer(mesh->indices, 0, rhi::IndexType::Uint32);
                    for (const GpuMeshPart& part : mesh->parts) {
                        const rhi::PipelineHandle pipeline =
                            renderer.gpu_assets->masked(part.material) ? data.caster : data.caster_opaque;
                        if (!(pipeline == bound)) {
                            context.list.bind_pipeline(pipeline);
                            bound = pipeline;
                        }
                        push.material = part.material;
                        context.list.push_constants(push);
                        context.list.draw_indexed(part.index_count, 1, part.first_index);
                        ++renderer.stats.shadow_draws;
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
    };
    struct GBufferPass {
        SceneRendererState* state = nullptr;
        const world::RenderFrame* frame = nullptr;
        Targets targets;
        rhi::PipelineHandle cull_back;
        rhi::PipelineHandle cull_none;
        uint64_t frame_address = 0;
    };
    const auto& gbuffer = graph.add_pass<GBufferPass>(
        "gbuffer",
        [&](PassBuilder& pass, GBufferPass& data) {
            data = {.state = &state,
                    .frame = &frame,
                    .targets = {},
                    .cull_back = gbuffer_back,
                    .cull_none = gbuffer_none,
                    .frame_address = frame_address};
            for (uint32_t i = 0; i < 3; ++i) {
                data.targets.gb[i] =
                    pass.create_texture({.desc = {.format = kGBufferFormats[i]}, .reference_scale = 1.0f});
                pass.use(data.targets.gb[i], rhi::ResourceState::RenderTarget);
            }
            data.targets.hdr = pass.create_texture({.desc = {.format = kHdrFormat}, .reference_scale = 1.0f});
            data.targets.depth = pass.create_texture({.desc = {.format = kDepthFormat}, .reference_scale = 1.0f});
            pass.use(data.targets.hdr, rhi::ResourceState::RenderTarget);
            pass.use(data.targets.depth, rhi::ResourceState::DepthWrite);
        },
        [](const GBufferPass& data, PassContext& context) {
            DE_PROFILE_ZONE("gbuffer pass");
            SceneRendererState& renderer = *data.state;
            const world::RenderFrame& extracted = *data.frame;
            rhi::ColorAttachment colors[4];
            for (uint32_t i = 0; i < 3; ++i) {
                colors[i] = {.texture = context.texture(data.targets.gb[i])};
            }
            colors[3] = {.texture = context.texture(data.targets.hdr)};
            const rhi::DepthAttachment depth{.texture = context.texture(data.targets.depth)};
            context.list.begin_rendering({.colors = colors, .depth = &depth});
            GBufferPush push{.frame = data.frame_address,
                             .instances = data.frame_address + kInstancesOffset,
                             .materials = renderer.gpu_assets->material_table_address(),
                             .sampler = rhi::sampler_index(rhi::DefaultSampler::AnisotropicWrap)};
            rhi::PipelineHandle bound;
            for (size_t i = 0; i < extracted.instances.size(); ++i) {
                const GpuMesh* mesh = renderer.gpu_assets->mesh(extracted.instances[i].mesh);
                if (mesh == nullptr) {
                    continue;
                }
                ++renderer.stats.drawn;
                push.instance = static_cast<uint32_t>(i);
                push.vertices = mesh->vertex_address;
                context.list.bind_index_buffer(mesh->indices, 0, rhi::IndexType::Uint32);
                for (const GpuMeshPart& part : mesh->parts) {
                    const rhi::PipelineHandle pipeline =
                        renderer.gpu_assets->double_sided(part.material) ? data.cull_none : data.cull_back;
                    if (!(pipeline == bound)) {
                        context.list.bind_pipeline(pipeline);
                        bound = pipeline;
                    }
                    push.material = part.material;
                    context.list.push_constants(push);
                    context.list.draw_indexed(part.index_count, 1, part.first_index);
                    ++renderer.stats.draws;
                }
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

    const float exposure = std::exp2(lighting.exposure_ev);
    const auto view_index = static_cast<uint32_t>(view);
    const uint32_t srgb_target = rhi::format_info(target_format).srgb ? 1 : 0;
    graph.add_pass(
        "tonemap",
        [&](PassBuilder& pass) {
            pass.use(targets.hdr, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            for (const GraphTexture gb : targets.gb) {
                pass.use(gb, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            }
            pass.use(shadow_mask, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            pass.use(target, rhi::ResourceState::RenderTarget);
        },
        [targets, shadow_mask, target, tonemap, exposure, view_index, srgb_target](PassContext& context) {
            DE_PROFILE_ZONE("tonemap pass");
            const rhi::ColorAttachment color{.texture = context.texture(target), .load = rhi::LoadOp::DontCare};
            context.list.begin_rendering({.colors = std::span(&color, 1)});
            context.list.bind_pipeline(tonemap);
            context.list.push_constants(TonemapPush{
                .hdr = context.device.texture_index(context.texture(targets.hdr)),
                .gb0 = context.device.texture_index(context.texture(targets.gb[0])),
                .gb1 = context.device.texture_index(context.texture(targets.gb[1])),
                .gb2 = context.device.texture_index(context.texture(targets.gb[2])),
                .exposure = exposure,
                .view = view_index,
                .srgb_target = srgb_target,
                .shadow_mask = context.device.texture_index(context.texture(shadow_mask)),
            });
            context.list.draw(3);
            context.list.end_rendering();
        });
}

SceneRendererStats SceneRenderer::stats() const {
    return state_->stats;
}

} // namespace de::render
