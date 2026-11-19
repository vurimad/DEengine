#include "render/scene/gtao.h"

#include "core/allocator.h"
#include "core/log.h"
#include "core/math.h"
#include "core/profile.h"
#include "render/rhi/command_list.h"
#include "render/rhi/device.h"
#include "render/shader/shader_library.h"
#include "world/render_extract.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace de::render {

namespace {

core::Allocator& render_heap() {
    return core::heap_allocator(core::MemoryTag::Render);
}

// Matches AoDepthPush in shaders/scene/ao_depth.slang.
struct AoDepthPush {
    uint32_t depth = 0;
    uint32_t output = 0;
    uint32_t size[2] = {};
    uint32_t source_size[2] = {};
    uint32_t mip = 0;
    float near_m = 0.1f;
    float spread_m = 0.0f;
    uint32_t reserved = 0;
};
static_assert(sizeof(AoDepthPush) == 40);

// Matches GtaoPush in shaders/scene/gtao.slang.
struct GtaoPush {
    uint64_t frame = 0;
    uint64_t reserved = 0; // a block's float4 sits on a multiple of 16
    float right[4] = {};
    float up[4] = {};
    float forward[4] = {};
    uint32_t depth = 0;
    uint32_t normals = 0;
    uint32_t output = 0;
    uint32_t slices = 0;
    uint32_t steps = 0;
    float radius_m = 0.0f;
    float falloff_share = 0.0f;
    float max_radius_px = 0.0f;
    uint32_t size[2] = {};
    float texel[2] = {};
};
static_assert(sizeof(GtaoPush) == 112);

// Matches AoDenoisePush in shaders/scene/ao_denoise.slang.
struct AoDenoisePush {
    uint32_t input = 0;
    uint32_t output = 0;
    uint32_t depth = 0;
    uint32_t spacing = 1;
    uint32_t size[2] = {};
    float tolerance = 0.0f;
    uint32_t reserved = 0;
};
static_assert(sizeof(AoDenoisePush) == 32);

// How far off the surface a tap of the denoise may be, as a share of its depth, and still count fully.
constexpr float kDenoiseTolerance = 0.02f;

uint32_t groups(uint32_t size) {
    return (size + 7) / 8;
}

} // namespace

struct GtaoState {
    rhi::Device* device = nullptr;
    shader::ShaderLibrary* shaders = nullptr;
    shader::ProgramHandle depth_program;
    shader::ProgramHandle gtao_program;
    shader::ProgramHandle denoise_program;
    GtaoSettings settings;
};

core::Result<Gtao> Gtao::create(const GtaoDesc& desc) {
    if (desc.device == nullptr || desc.shaders == nullptr) {
        return core::fail(core::ErrorCode::InvalidArgument, "ambient occlusion needs a device and a shader library");
    }
    auto* state = ::new (render_heap().allocate(sizeof(GtaoState), alignof(GtaoState))) GtaoState();
    state->device = desc.device;
    state->shaders = desc.shaders;
    Gtao gtao(state);
    const auto program = [&](const char* module, const char* name) {
        const core::Result<shader::ProgramHandle> made =
            desc.shaders->create_compute({.module = module, .debug_name = name});
        if (!made) {
            DE_LOG_WARN("render", "%s's shader didn't build: no ambient occlusion until it does", name);
        }
        return made ? *made : shader::ProgramHandle{};
    };
    state->depth_program = program("scene.ao_depth", "ao depth");
    state->gtao_program = program("scene.gtao", "gtao");
    state->denoise_program = program("scene.ao_denoise", "ao denoise");
    return gtao;
}

Gtao::~Gtao() {
    if (state_ == nullptr) {
        return;
    }
    state_->shaders->destroy(state_->depth_program);
    state_->shaders->destroy(state_->gtao_program);
    state_->shaders->destroy(state_->denoise_program);
    state_->~GtaoState();
    render_heap().deallocate(state_, sizeof(GtaoState), alignof(GtaoState));
}

Gtao::Gtao(Gtao&& other) noexcept : state_(other.state_) {
    other.state_ = nullptr;
}

Gtao& Gtao::operator=(Gtao&& other) noexcept {
    if (this != &other) {
        std::swap(state_, other.state_);
    }
    return *this;
}

void Gtao::set_settings(const GtaoSettings& settings) {
    state_->settings = settings;
    state_->settings.slices = std::clamp(settings.slices, 1U, 8U);
    state_->settings.steps = std::clamp(settings.steps, 1U, 16U);
    state_->settings.radius_m = std::max(settings.radius_m, 0.01f);
    state_->settings.falloff_share = std::clamp(settings.falloff_share, 0.0f, 0.95f);
}

const GtaoSettings& Gtao::settings() const {
    return state_->settings;
}

GraphTexture Gtao::add_passes(RenderGraph& graph, const GtaoInput& input) {
    const GtaoState& state = *state_;
    const rhi::PipelineHandle depth_pipeline = state.shaders->pipeline(state.depth_program);
    const rhi::PipelineHandle gtao_pipeline = state.shaders->pipeline(state.gtao_program);
    const rhi::PipelineHandle denoise_pipeline = state.shaders->pipeline(state.denoise_program);
    if (!depth_pipeline.is_valid() || !gtao_pipeline.is_valid() || !denoise_pipeline.is_valid() ||
        input.frame == nullptr || !input.frame->camera.valid || input.width == 0 || input.height == 0) {
        return {};
    }
    const GtaoSettings& settings = state.settings;
    const uint32_t width = input.width;
    const uint32_t height = input.height;

    // The depth pyramid: depth along the view, then each mip from the one before, a barrier between.
    struct DepthPass {
        GraphTexture pyramid;
    };
    const float near_m = input.frame->camera.near_m;
    const float spread_m = settings.radius_m * 0.25f;
    const auto& depth_pass = graph.add_pass<DepthPass>(
        "ao depth",
        [&](PassBuilder& pass, DepthPass& data) {
            data.pyramid =
                pass.create_texture({.desc = {.width = width,
                                              .height = height,
                                              .mip_count = kGtaoDepthMips,
                                              .format = rhi::Format::R32Float,
                                              .usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled,
                                              .debug_name = "ao depth"}});
            pass.use(input.depth, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
            pass.use(data.pyramid, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
        },
        [depth = input.depth, depth_pipeline, width, height, near_m, spread_m](const DepthPass& data,
                                                                               PassContext& context) {
            DE_PROFILE_ZONE("ao depth pass");
            rhi::CommandList& list = context.list;
            const rhi::TextureHandle pyramid = context.texture(data.pyramid);
            list.bind_pipeline(depth_pipeline);
            uint32_t source[2] = {width, height};
            for (uint32_t mip = 0; mip < kGtaoDepthMips; ++mip) {
                const uint32_t size[2] = {std::max(width >> mip, 1U), std::max(height >> mip, 1U)};
                if (mip > 0) {
                    // The mip before is written: this one reads it.
                    list.barrier(rhi::GlobalBarrier{.before = rhi::ResourceState::ShaderWrite,
                                                    .after = rhi::ResourceState::ShaderWrite,
                                                    .before_stages = rhi::ShaderStages::Compute,
                                                    .after_stages = rhi::ShaderStages::Compute});
                }
                const AoDepthPush push{.depth = mip == 0 ? context.device.texture_index(context.texture(depth))
                                                         : context.device.storage_texture_index(pyramid, mip - 1),
                                       .output = context.device.storage_texture_index(pyramid, mip),
                                       .size = {size[0], size[1]},
                                       .source_size = {source[0], source[1]},
                                       .mip = mip,
                                       .near_m = near_m,
                                       .spread_m = spread_m};
                list.push_constants(push);
                list.dispatch(groups(size[0]), groups(size[1]));
                source[0] = size[0];
                source[1] = size[1];
            }
        });
    const GraphTexture pyramid = depth_pass.pyramid;

    // The camera's axes in the world, and its field of view.
    const float* camera = input.frame->camera.transform;
    const Vec3 right = glm::normalize(Vec3(camera[0], camera[4], camera[8]));
    const Vec3 up = glm::normalize(Vec3(camera[1], camera[5], camera[9]));
    const Vec3 forward = -glm::normalize(Vec3(camera[2], camera[6], camera[10]));
    const float tan_y = std::tan(input.frame->camera.vertical_fov_rad * 0.5f);
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const GtaoPush gtao_push{.frame = input.frame_address,
                             .right = {right.x, right.y, right.z, tan_y * aspect},
                             .up = {up.x, up.y, up.z, tan_y},
                             .forward = {forward.x, forward.y, forward.z, near_m},
                             .slices = settings.slices,
                             .steps = settings.steps,
                             .radius_m = settings.radius_m,
                             .falloff_share = settings.falloff_share,
                             .max_radius_px = std::max(settings.max_radius_share * static_cast<float>(height), 2.0f),
                             .size = {width, height},
                             .texel = {1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height)}};
    const rhi::TextureDesc occlusion_desc{.width = width,
                                          .height = height,
                                          .format = rhi::Format::R8Unorm,
                                          .usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled,
                                          .debug_name = "ao"};
    struct GtaoPass {
        GraphTexture raw;
    };
    const auto& traced = graph.add_pass<GtaoPass>(
        "gtao",
        [&](PassBuilder& pass, GtaoPass& data) {
            data.raw = pass.create_texture({.desc = occlusion_desc});
            pass.use(pyramid, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
            pass.use(input.normals, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
            pass.use(data.raw, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
        },
        [gtao_push, gtao_pipeline, pyramid, normals = input.normals](const GtaoPass& data, PassContext& context) {
            DE_PROFILE_ZONE("gtao pass");
            GtaoPush push = gtao_push;
            push.depth = context.device.texture_index(context.texture(pyramid));
            push.normals = context.device.texture_index(context.texture(normals));
            push.output = context.device.storage_texture_index(context.texture(data.raw));
            context.list.bind_pipeline(gtao_pipeline);
            context.list.push_constants(push);
            context.list.dispatch(groups(push.size[0]), groups(push.size[1]));
        });

    // The denoise: twice, its taps one pixel apart and then two.
    GraphTexture occlusion = traced.raw;
    for (uint32_t spacing = 1; spacing <= 2; ++spacing) {
        struct DenoisePass {
            GraphTexture out;
        };
        const GraphTexture in = occlusion;
        const auto& denoised = graph.add_pass<DenoisePass>(
            "ao denoise",
            [&](PassBuilder& pass, DenoisePass& data) {
                data.out = pass.create_texture({.desc = occlusion_desc});
                pass.use(in, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
                pass.use(pyramid, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
                pass.use(data.out, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            },
            [in, pyramid, denoise_pipeline, spacing, width, height](const DenoisePass& data, PassContext& context) {
                DE_PROFILE_ZONE("ao denoise pass");
                const AoDenoisePush push{.input = context.device.texture_index(context.texture(in)),
                                         .output = context.device.storage_texture_index(context.texture(data.out)),
                                         .depth = context.device.texture_index(context.texture(pyramid)),
                                         .spacing = spacing,
                                         .size = {width, height},
                                         .tolerance = kDenoiseTolerance};
                context.list.bind_pipeline(denoise_pipeline);
                context.list.push_constants(push);
                context.list.dispatch(groups(width), groups(height));
            });
        occlusion = denoised.out;
    }
    return occlusion;
}

} // namespace de::render
