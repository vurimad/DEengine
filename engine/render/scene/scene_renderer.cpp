#include "render/scene/scene_renderer.h"

#include "core/allocator.h"
#include "core/log.h"
#include "core/profile.h"
#include "render/rhi/device.h"
#include "render/scene/gpu_assets.h"
#include "world/render_extract.h"

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

// Matches SceneFrame in shaders/scene/basic_lit.slang.
struct SceneFrame {
    float view_proj[16] = {};
    float sun_direction[4] = {};
    float sun_color[4] = {};
    float ambient_color[4] = {};
};

// Matches BasicLitPush in shaders/scene/basic_lit.slang.
struct BasicLitPush {
    uint64_t frame = 0;
    uint64_t instances = 0;
    uint64_t vertices = 0;
    uint64_t materials = 0;
    uint32_t instance = 0;
    uint32_t material = 0;
    uint32_t sampler = 0;
};

constexpr uint64_t kInstancesOffset = 256; // after the SceneFrame
constexpr uint64_t kInstanceBytes = 48;    // three float4 rows

} // namespace

struct SceneRendererState {
    rhi::Device* device = nullptr;
    shader::ShaderLibrary* shaders = nullptr;
    GpuAssets* gpu_assets = nullptr;
    shader::ProgramHandle cull_back;
    shader::ProgramHandle cull_none; // double-sided materials
    rhi::Format programs_format = rhi::Format::Unknown;
    rhi::BufferHandle frame_buffers[rhi::kFramesInFlight];
    uint64_t frame_capacity[rhi::kFramesInFlight] = {};
    SceneRendererStats stats;

    ~SceneRendererState() {
        shaders->destroy(cull_back);
        shaders->destroy(cull_none);
        for (const rhi::BufferHandle buffer : frame_buffers) {
            device->destroy(buffer);
        }
    }

    void make_programs(rhi::Format format) {
        if (format == programs_format) {
            return;
        }
        shaders->destroy(cull_back);
        shaders->destroy(cull_none);
        rhi::GraphicsPipelineDesc state{.color_count = 1,
                                        .depth_format = rhi::Format::D32Float,
                                        .depth_test = true,
                                        .depth_write = true,
                                        .depth_compare = rhi::CompareOp::Greater,
                                        .debug_name = "basic lit"};
        state.color_formats[0] = format;
        const auto back = shaders->create_graphics({.module = "scene.basic_lit", .state = state});
        state.cull = rhi::CullMode::None;
        state.debug_name = "basic lit (double-sided)";
        const auto none = shaders->create_graphics({.module = "scene.basic_lit", .state = state});
        cull_back = back ? *back : shader::ProgramHandle{};
        cull_none = none ? *none : shader::ProgramHandle{};
        programs_format = format;
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
};

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

core::Result<SceneRenderer> SceneRenderer::create(const SceneRendererDesc& desc) {
    if (desc.device == nullptr || desc.shaders == nullptr || desc.gpu_assets == nullptr) {
        return core::fail(core::ErrorCode::InvalidArgument, "SceneRenderer needs a device, shaders and GpuAssets");
    }
    auto* state =
        ::new (render_heap().allocate(sizeof(SceneRendererState), alignof(SceneRendererState))) SceneRendererState();
    state->device = desc.device;
    state->shaders = desc.shaders;
    state->gpu_assets = desc.gpu_assets;
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

void SceneRenderer::add_pass(RenderGraph& graph, GraphTexture target, rhi::Format target_format,
                             const world::RenderFrame& frame, const SceneLighting& lighting, rhi::Color clear) {
    DE_PROFILE_ZONE("scene pass setup");
    SceneRendererState& state = *state_;
    state.make_programs(target_format);
    state.stats = {.instances = static_cast<uint32_t>(frame.instances.size())};

    struct Pass {
        SceneRendererState* state = nullptr;
        const world::RenderFrame* frame = nullptr;
        GraphTexture target;
        GraphTexture depth;
        rhi::Color clear;
        SceneLighting lighting;
        rhi::PipelineHandle cull_back;
        rhi::PipelineHandle cull_none;
        rhi::BufferHandle frame_buffer;
    };
    graph.add_pass<Pass>(
        "scene",
        [&](PassBuilder& pass, Pass& data) {
            data.state = &state;
            data.frame = &frame;
            data.target = target;
            data.clear = clear;
            data.lighting = lighting;
            data.depth = pass.create_texture({.desc = {.format = rhi::Format::D32Float}, .reference_scale = 1.0f});
            pass.use(target, rhi::ResourceState::RenderTarget);
            pass.use(data.depth, rhi::ResourceState::DepthWrite);
            data.cull_back = state.shaders->pipeline(state.cull_back);
            data.cull_none = state.shaders->pipeline(state.cull_none);
            data.frame_buffer = frame.camera.valid ? state.frame_buffer(frame.instances.size()) : rhi::BufferHandle{};
        },
        [](const Pass& data, PassContext& context) {
            DE_PROFILE_ZONE("scene pass");
            const rhi::ColorAttachment color{.texture = context.texture(data.target), .clear = data.clear};
            const rhi::DepthAttachment depth{.texture = context.texture(data.depth)};
            context.list.begin_rendering({.colors = std::span(&color, 1), .depth = &depth});
            SceneRendererState& renderer = *data.state;
            if (data.frame_buffer.is_valid() && data.cull_back.is_valid() && data.cull_none.is_valid()) {
                const rhi::TextureDesc& size = context.desc(data.target);
                const world::RenderFrame& extracted = *data.frame;
                const std::span<std::byte> mapped = context.device.mapped(data.frame_buffer);
                SceneFrame scene;
                camera_view_proj(extracted.camera.transform, extracted.camera.vertical_fov_rad, extracted.camera.near_m,
                                 static_cast<float>(size.width) / static_cast<float>(std::max(size.height, 1U)),
                                 scene.view_proj);
                const SceneLighting& light = data.lighting;
                const float sun_length = std::sqrt(light.sun_direction[0] * light.sun_direction[0] +
                                                   light.sun_direction[1] * light.sun_direction[1] +
                                                   light.sun_direction[2] * light.sun_direction[2]);
                for (int i = 0; i < 3; ++i) {
                    scene.sun_direction[i] = light.sun_direction[i] / std::max(sun_length, 1e-6f);
                    scene.sun_color[i] = light.sun_color[i];
                    scene.ambient_color[i] = light.ambient_color[i];
                }
                std::memcpy(mapped.data(), &scene, sizeof(scene));
                for (size_t i = 0; i < extracted.instances.size(); ++i) {
                    std::memcpy(mapped.data() + kInstancesOffset + i * kInstanceBytes, extracted.instances[i].transform,
                                kInstanceBytes);
                }
                context.device.flush_mapped(data.frame_buffer, 0,
                                            kInstancesOffset + extracted.instances.size() * kInstanceBytes);

                const uint64_t address = context.device.buffer_address(data.frame_buffer);
                BasicLitPush push{.frame = address,
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
}

SceneRendererStats SceneRenderer::stats() const {
    return state_->stats;
}

} // namespace de::render
