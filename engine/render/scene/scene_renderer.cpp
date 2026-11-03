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
};
static_assert(sizeof(SceneFrame) <= 256);

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

// Matches DeferredLightPush in shaders/scene/deferred_light.slang.
struct DeferredLightPush {
    uint64_t frame = 0;
    uint32_t gb0 = 0;
    uint32_t gb1 = 0;
    uint32_t gb2 = 0;
    uint32_t depth = 0;
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
};

constexpr uint64_t kInstancesOffset = 256; // after the SceneFrame
constexpr uint64_t kInstanceBytes = 48;    // three float4 rows

// The G-buffer's formats (shaders/scene/scene_common.slang documents the channels).
constexpr rhi::Format kGBufferFormats[3] = {rhi::Format::RGBA8Srgb, rhi::Format::RG16Unorm, rhi::Format::RGBA8Unorm};
constexpr rhi::Format kHdrFormat = rhi::Format::RGBA16Float;
constexpr rhi::Format kDepthFormat = rhi::Format::D32Float;

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
    rhi::Format programs_format = rhi::Format::Unknown; // the target's, for the tonemap
    rhi::BufferHandle frame_buffers[rhi::kFramesInFlight];
    uint64_t frame_capacity[rhi::kFramesInFlight] = {};
    SceneRendererStats stats;
    bool frame_written = false; // this frame's SceneFrame: set by the G-buffer pass, read by the light pass

    ~SceneRendererState() {
        for (const shader::ProgramHandle program : {gbuffer_cull_back, gbuffer_cull_none, deferred_light, tonemap}) {
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
    state.make_tonemap(target_format);
    state.stats = {.instances = static_cast<uint32_t>(frame.instances.size())};
    const rhi::PipelineHandle gbuffer_back = state.shaders->pipeline(state.gbuffer_cull_back);
    const rhi::PipelineHandle gbuffer_none = state.shaders->pipeline(state.gbuffer_cull_none);
    const rhi::PipelineHandle light = state.shaders->pipeline(state.deferred_light);
    const rhi::PipelineHandle tonemap = state.shaders->pipeline(state.tonemap);
    const bool ready = frame.camera.valid && gbuffer_back.is_valid() && gbuffer_none.is_valid() && light.is_valid() &&
                       tonemap.is_valid();
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

    struct Targets {
        GraphTexture gb[3];
        GraphTexture hdr;
        GraphTexture depth;
    };
    struct GBufferPass {
        SceneRendererState* state = nullptr;
        const world::RenderFrame* frame = nullptr;
        SceneLighting lighting;
        Targets targets;
        rhi::PipelineHandle cull_back;
        rhi::PipelineHandle cull_none;
        rhi::BufferHandle frame_buffer;
    };
    const auto& gbuffer = graph.add_pass<GBufferPass>(
        "gbuffer",
        [&](PassBuilder& pass, GBufferPass& data) {
            data = {.state = &state,
                    .frame = &frame,
                    .lighting = lighting,
                    .targets = {},
                    .cull_back = gbuffer_back,
                    .cull_none = gbuffer_none,
                    .frame_buffer = state.frame_buffer(frame.instances.size())};
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
            const rhi::TextureDesc& size = context.desc(data.targets.hdr);
            const float aspect = static_cast<float>(size.width) / static_cast<float>(std::max(size.height, 1U));
            renderer.frame_written = data.frame_buffer.is_valid() &&
                                     renderer.write_frame(data.frame_buffer, extracted, data.lighting, aspect);
            if (renderer.frame_written) {
                const uint64_t address = context.device.buffer_address(data.frame_buffer);
                GBufferPush push{.frame = address,
                                 .instances = address + kInstancesOffset,
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
            }
            context.list.end_rendering();
        });
    const Targets targets = gbuffer.targets;
    const rhi::BufferHandle frame_buffer = gbuffer.frame_buffer;

    graph.add_pass(
        "deferred light",
        [&](PassBuilder& pass) {
            for (const GraphTexture gb : targets.gb) {
                pass.use(gb, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            }
            pass.use(targets.depth, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Fragment);
            pass.use(targets.hdr, rhi::ResourceState::RenderTarget);
        },
        [&state, targets, frame_buffer, light](PassContext& context) {
            DE_PROFILE_ZONE("deferred light pass");
            const rhi::ColorAttachment color{.texture = context.texture(targets.hdr), .load = rhi::LoadOp::Load};
            context.list.begin_rendering({.colors = std::span(&color, 1)});
            if (state.frame_written) { // the G-buffer pass ran just before, in this execute()
                context.list.bind_pipeline(light);
                context.list.push_constants(DeferredLightPush{
                    .frame = context.device.buffer_address(frame_buffer),
                    .gb0 = context.device.texture_index(context.texture(targets.gb[0])),
                    .gb1 = context.device.texture_index(context.texture(targets.gb[1])),
                    .gb2 = context.device.texture_index(context.texture(targets.gb[2])),
                    .depth = context.device.texture_index(context.texture(targets.depth)),
                });
                context.list.draw(3);
            }
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
            pass.use(target, rhi::ResourceState::RenderTarget);
        },
        [targets, target, tonemap, exposure, view_index, srgb_target](PassContext& context) {
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
            });
            context.list.draw(3);
            context.list.end_rendering();
        });
}

SceneRendererStats SceneRenderer::stats() const {
    return state_->stats;
}

} // namespace de::render
