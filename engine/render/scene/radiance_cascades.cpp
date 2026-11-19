#include "render/scene/radiance_cascades.h"

#include "core/allocator.h"
#include "core/log.h"
#include "core/profile.h"
#include "render/rhi/command_list.h"
#include "render/rhi/device.h"
#include "render/shader/shader_library.h"
#include "world/render_extract.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <new>
#include <numbers>

namespace de::render {

namespace {

core::Allocator& render_heap() {
    return core::heap_allocator(core::MemoryTag::Render);
}

// Matches RcPyramidPush in shaders/scene/rc_pyramid.slang.
struct RcPyramidPush {
    uint64_t frame = 0;
    uint32_t depth_in = 0;
    uint32_t radiance_in = 0;
    uint32_t normal_in = 0;
    uint32_t depth_out = 0;
    uint32_t normal_out = 0;
    uint32_t radiance_out = 0;
    uint32_t size[2] = {};
    uint32_t source_size[2] = {};
    uint32_t mip = 0;
    uint32_t glow = 0;
    uint32_t scale = 1;
    uint32_t bounce = 0;
    uint32_t has_bounce = 0;
    uint32_t reserved = 0;
};
static_assert(sizeof(RcPyramidPush) == 72);

// Matches RcGlowPush in shaders/scene/rc_glow.slang.
struct RcGlowPush {
    uint32_t source = 0;
    uint32_t output = 0;
    uint32_t size[2] = {};
    uint32_t screen[2] = {};
    uint32_t scale = 1;
    uint32_t reserved = 0;
};
static_assert(sizeof(RcGlowPush) == 32);

// Matches RadianceCascadePush in shaders/scene/radiance_cascades.slang.
struct RadianceCascadePush {
    uint64_t frame = 0;
    uint32_t depth = 0;
    uint32_t normal = 0;
    uint32_t radiance = 0;
    uint32_t upper = 0;
    uint32_t output = 0;
    uint32_t top = 0;
    uint32_t size[2] = {};
    uint32_t probes[2] = {};
    uint32_t upper_probes[2] = {};
    uint32_t spacing = 0;
    uint32_t azimuths = 0;
    float start_px = 0.0f;
    float step_px = 0.0f;
    uint32_t steps = 0;
    float mip = 0.0f;
    float thickness_m = 0.0f;
    float tolerance = 0.0f;
    float phase = 0.0f;
    uint32_t scale = 1;
    uint32_t screen[2] = {};
};
static_assert(sizeof(RadianceCascadePush) == 104);

// Matches RcGatherPush in shaders/scene/rc_gather.slang.
struct RcGatherPush {
    uint64_t frame = 0;
    uint32_t depth = 0;
    uint32_t normal = 0;
    uint32_t radiance = 0;
    uint32_t upper = 0;
    uint32_t output = 0;
    uint32_t has_upper = 0;
    uint32_t size[2] = {};
    uint32_t screen[2] = {};
    uint32_t upper_probes[2] = {};
    uint32_t scale = 1;
    uint32_t azimuths = 0;
    uint32_t steps = 0;
    float start_px = 0.0f;
    float step_px = 0.0f;
    float thickness_m = 0.0f;
    float tolerance = 0.0f;
    float phase = 0.0f;
    float reserved_sky = 0.0f;
    float bounce = 1.0f;
    uint32_t white_sky = 0;
    uint32_t upper_wide = 0;
    uint32_t base_color = 0;
    uint32_t material = 0;
    uint32_t sent_on = 0;
    uint32_t sends_on = 0; // bit 0: keep what surfaces send on; bit 1: times the material's own occlusion
};
static_assert(sizeof(RcGatherPush) == 120);

constexpr rhi::Format kCascadeFormat = rhi::Format::RGBA16Float;
// Past this the texture of a cascade would be larger than a device is sure to allow.
constexpr uint32_t kMaxCascadeTexels = 16384;
constexpr uint32_t kMaxCascades = 12;
// A cascade whose pass would have fewer threads than this is wide (shaders/scene/radiance_cascades.slang): a thread
// an azimuth, not two, and every azimuth stored. Too few threads to fill the GPU, the pass takes as long as one
// does.
constexpr uint32_t kWideBelowThreads = 65536;

uint32_t groups(uint32_t size) {
    return (size + 7) / 8;
}

// The screen's pixels to one of the GI picture's, each way: the settings', or left to us, the most that keeps the GI
// picture 360 pixels high or more (2 up to 900p, 3 at 1080p, 4 from 1440p).
uint32_t scale_of(const RadianceCascadesSettings& settings, uint32_t height) {
    return settings.scale != 0 ? settings.scale : std::clamp(height / 360, 2U, 4U);
}

// A cascade's place in the hierarchy (shaders/scene/rc_common.slang): cascade 0's probes are the GI picture's
// pixels, and every length here is in those.
struct Layout {
    uint32_t spacing = 1;  // between probes, in pixels
    uint32_t azimuths = 0; // all of them: half are stored
    uint32_t probes[2] = {};
    float start_px = 0.0f; // the first sample's distance from the probe
    float step_px = 1.0f;  // between samples
    uint32_t steps = 0;
    float mip = 0.0f;  // of the pyramids: as coarse as the samples are apart
    bool wide = false; // it stores every azimuth, not the mean of each two (kWideBelowThreads)

    uint32_t stored() const { return wide ? azimuths : azimuths / 2; }
};

Layout layout_of(uint32_t cascade, const RadianceCascadesSettings& settings, const uint32_t size[2], uint32_t mips,
                 uint32_t gi_scale) {
    Layout layout;
    layout.spacing = 1U << cascade;
    layout.azimuths = settings.azimuths << cascade;
    layout.probes[0] = (size[0] + layout.spacing - 1) / layout.spacing;
    layout.probes[1] = (size[1] + layout.spacing - 1) / layout.spacing;
    layout.wide = cascade > 0 && layout.probes[0] * layout.probes[1] * (layout.azimuths / 2) < kWideBelowThreads;
    // Its interval is twice as long as the one before's and starts where that ends; the first starts half a pixel
    // out, so its samples fall on the pixels round the probe.
    const float scale = std::exp2(static_cast<float>(cascade));
    const float length = settings.interval_px * scale;
    const uint32_t coarsest = 1U << (mips - 1);
    // The widest step is given in the screen's pixels: in the GI picture's, a power of two no wider than that.
    const uint32_t widest = std::min(std::bit_floor(std::max(settings.max_step_px / gi_scale, 1U)), coarsest);
    uint32_t step = std::clamp(std::bit_floor(static_cast<uint32_t>(std::max(length * 0.5f, 1.0f))), 1U, widest);
    while (settings.max_march_steps != 0 && step < coarsest &&
           length / static_cast<float>(step) > static_cast<float>(settings.max_march_steps)) {
        step *= 2;
    }
    layout.step_px = static_cast<float>(step);
    layout.steps = static_cast<uint32_t>(std::ceil(length / layout.step_px));
    layout.mip = static_cast<float>(std::countr_zero(step));
    layout.start_px = 0.5f + settings.interval_px * (scale - 1.0f) + layout.step_px * 0.5f;
    return layout;
}

} // namespace

struct RadianceCascadesState {
    rhi::Device* device = nullptr;
    shader::ShaderLibrary* shaders = nullptr;
    shader::ProgramHandle glow_program;
    shader::ProgramHandle pyramid_program;
    shader::ProgramHandle cascade_program;
    shader::ProgramHandle gather_program;
    RadianceCascadesSettings settings;
    RadianceCascadesStats stats;
    // What surfaces sent on of the light gathered (settings.bounces), at the GI picture's size: last frame's is
    // read as this frame's is written, turn about.
    rhi::TextureHandle sent_on[2];
    bool sent_on_used[2] = {};     // it has been in a graph: it's in the state a graph leaves it in
    uint32_t sent_on_size[2] = {}; // the GI picture's, when they were made
    uint32_t sent_on_now = 0;      // the one last written
    uint64_t sent_on_frame = 0;    // the RenderFrame it was written in; 0: never

    void destroy_sent_on() {
        for (rhi::TextureHandle& texture : sent_on) {
            if (texture.is_valid()) {
                device->destroy(texture);
            }
            texture = {};
        }
        sent_on_used[0] = sent_on_used[1] = false;
        sent_on_frame = 0;
    }

    // The two at this size (a new size starts with nothing kept); false if they can't be made.
    bool ensure_sent_on(const uint32_t size[2]) {
        if (size[0] == sent_on_size[0] && size[1] == sent_on_size[1] && sent_on[0].is_valid()) {
            return true;
        }
        destroy_sent_on();
        sent_on_size[0] = size[0];
        sent_on_size[1] = size[1];
        for (rhi::TextureHandle& texture : sent_on) {
            const core::Result<rhi::TextureHandle> made =
                device->create_texture({.width = size[0],
                                        .height = size[1],
                                        .format = rhi::Format::RGBA16Float,
                                        .usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled,
                                        .debug_name = "rc sent on"});
            if (!made) {
                destroy_sent_on();
                sent_on_size[0] = sent_on_size[1] = 0;
                return false;
            }
            texture = *made;
        }
        return true;
    }
};

core::Result<RadianceCascades> RadianceCascades::create(const RadianceCascadesDesc& desc) {
    if (desc.device == nullptr || desc.shaders == nullptr) {
        return core::fail(core::ErrorCode::InvalidArgument, "radiance cascades need a device and a shader library");
    }
    auto* state = ::new (render_heap().allocate(sizeof(RadianceCascadesState), alignof(RadianceCascadesState)))
        RadianceCascadesState();
    state->device = desc.device;
    state->shaders = desc.shaders;
    RadianceCascades cascades(state);
    const auto program = [&](const char* module, const char* name) {
        const core::Result<shader::ProgramHandle> made =
            desc.shaders->create_compute({.module = module, .debug_name = name});
        if (!made) {
            DE_LOG_WARN("render", "%s's shader didn't build: no radiance cascades until it does", name);
        }
        return made ? *made : shader::ProgramHandle{};
    };
    state->glow_program = program("scene.rc_glow", "rc glow");
    state->pyramid_program = program("scene.rc_pyramid", "rc pyramid");
    state->cascade_program = program("scene.radiance_cascades", "radiance cascade");
    state->gather_program = program("scene.rc_gather", "rc gather");
    return cascades;
}

RadianceCascades::~RadianceCascades() {
    if (state_ == nullptr) {
        return;
    }
    state_->shaders->destroy(state_->glow_program);
    state_->shaders->destroy(state_->pyramid_program);
    state_->shaders->destroy(state_->cascade_program);
    state_->shaders->destroy(state_->gather_program);
    state_->destroy_sent_on();
    state_->~RadianceCascadesState();
    render_heap().deallocate(state_, sizeof(RadianceCascadesState), alignof(RadianceCascadesState));
}

RadianceCascades::RadianceCascades(RadianceCascades&& other) noexcept : state_(other.state_) {
    other.state_ = nullptr;
}

RadianceCascades& RadianceCascades::operator=(RadianceCascades&& other) noexcept {
    if (this != &other) {
        std::swap(state_, other.state_);
    }
    return *this;
}

void RadianceCascades::set_settings(const RadianceCascadesSettings& settings) {
    RadianceCascadesSettings& kept = state_->settings;
    kept = settings;
    kept.scale = std::min(settings.scale, 4U);
    kept.azimuths = std::clamp(settings.azimuths, 2U, 16U);
    kept.interval_px = std::clamp(settings.interval_px, 1.0f, 64.0f);
    kept.max_step_px = std::clamp(settings.max_step_px, 1U, 1U << (kRcPyramidMips - 1));
    kept.max_cascades = std::clamp(settings.max_cascades, 1U, kMaxCascades);
    kept.thickness_m = std::max(settings.thickness_m, 0.01f);
    kept.plane_tolerance = std::max(settings.plane_tolerance, 0.05f);
    kept.sky = std::max(settings.sky, 0.0f);
    kept.bounce = std::max(settings.bounce, 0.0f);
}

const RadianceCascadesSettings& RadianceCascades::settings() const {
    return state_->settings;
}

RadianceCascadesStats RadianceCascades::stats() const {
    return state_->stats;
}

GraphTexture RadianceCascades::add_glow_pass(RenderGraph& graph, GraphTexture hdr, uint32_t width, uint32_t height) {
    const rhi::PipelineHandle pipeline = state_->shaders->pipeline(state_->glow_program);
    if (!pipeline.is_valid() || width == 0 || height == 0) {
        return {};
    }
    // At the GI picture's size: a pixel the mean of its block of the screen.
    const uint32_t scale = scale_of(state_->settings, height);
    const uint32_t size[2] = {(width + scale - 1) / scale, (height + scale - 1) / scale};
    struct GlowPass {
        GraphTexture glow;
    };
    const auto& pass = graph.add_pass<GlowPass>(
        "rc glow",
        [&](PassBuilder& builder, GlowPass& data) {
            data.glow =
                builder.create_texture({.desc = {.width = size[0],
                                                 .height = size[1],
                                                 .format = rhi::Format::R16Float,
                                                 .usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled,
                                                 .debug_name = "rc glow"}});
            builder.use(hdr, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
            builder.use(data.glow, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
        },
        [hdr, pipeline, width, height, scale, gi_width = size[0], gi_height = size[1]](const GlowPass& data,
                                                                                       PassContext& context) {
            DE_PROFILE_ZONE("rc glow pass");
            const RcGlowPush push{.source = context.device.texture_index(context.texture(hdr)),
                                  .output = context.device.storage_texture_index(context.texture(data.glow)),
                                  .size = {gi_width, gi_height},
                                  .screen = {width, height},
                                  .scale = scale};
            context.list.bind_pipeline(pipeline);
            context.list.push_constants(push);
            context.list.dispatch(groups(gi_width), groups(gi_height));
        });
    return pass.glow;
}

RadianceCascadesResult RadianceCascades::add_passes(RenderGraph& graph, const RadianceCascadesInput& input) {
    RadianceCascadesState& state = *state_;
    state.stats = {};
    const rhi::PipelineHandle pyramid_pipeline = state.shaders->pipeline(state.pyramid_program);
    const rhi::PipelineHandle cascade_pipeline = state.shaders->pipeline(state.cascade_program);
    const rhi::PipelineHandle gather_pipeline = state.shaders->pipeline(state.gather_program);
    if (!pyramid_pipeline.is_valid() || !cascade_pipeline.is_valid() || !gather_pipeline.is_valid() ||
        input.frame == nullptr || !input.frame->camera.valid || input.width == 0 || input.height == 0 ||
        !input.glow.is_valid()) {
        return {};
    }
    const RadianceCascadesSettings& settings = state.settings;
    // The GI picture: the screen at 1 / scale of its size each way, rounded up.
    const uint32_t scale = scale_of(settings, input.height);
    const uint32_t screen[2] = {input.width, input.height};
    const uint32_t size[2] = {(screen[0] + scale - 1) / scale, (screen[1] + scale - 1) / scale};
    const uint32_t mips = std::min(kRcPyramidMips, static_cast<uint32_t>(std::bit_width(std::min(size[0], size[1]))));

    // As many cascades as it takes for their reach to cross the picture, at most max_cascades, and no cascade
    // whose texture would be too large to have.
    const float across_px = std::sqrt(static_cast<float>((size[0] * size[0]) + (size[1] * size[1])));
    uint32_t count = 1;
    while (count < settings.max_cascades &&
           0.5f + settings.interval_px * (std::exp2(static_cast<float>(count)) - 1.0f) < across_px) {
        ++count;
    }
    Layout layouts[kMaxCascades] = {};
    uint32_t marches = 0;
    uint32_t texels = 0;
    for (uint32_t i = 0; i < count; ++i) {
        layouts[i] = layout_of(i, settings, size, mips, scale);
        const Layout& layout = layouts[i];
        if (i > 0 && (layout.probes[0] * layout.stored() > kMaxCascadeTexels ||
                      layout.probes[1] * kRcElevations > kMaxCascadeTexels)) {
            count = i;
            break;
        }
        marches += layout.probes[0] * layout.probes[1] * layout.azimuths;
        texels += i > 0 ? layout.probes[0] * layout.probes[1] * layout.stored() * kRcElevations : 0;
    }
    state.stats = {.scale = scale, .cascades = count, .marches = marches, .texels = texels};
    // The topmost marches on to the picture's edge, so with fewer cascades than reach across it nothing far is
    // left out: it's only seen with the topmost's azimuths, not finer ones.
    Layout& topmost = layouts[count - 1];
    topmost.steps = std::max(topmost.steps,
                             static_cast<uint32_t>(std::ceil((across_px - topmost.start_px) / topmost.step_px)) + 1U);

    // What surfaces sent on last frame, for the pyramid to add to the lit picture, and where this frame's goes. Last
    // frame's counts only if it was the frame before this one, at this size, with no cut between.
    GraphTexture sent_on_last;
    GraphTexture sent_on;
    if (settings.bounces && input.base_color.is_valid() && input.material.is_valid() && state.ensure_sent_on(size)) {
        const uint32_t last = state.sent_on_now;
        const uint32_t now = last ^ 1U;
        if (state.sent_on_frame != 0 && state.sent_on_frame + 1 == input.frame->number &&
            input.frame->camera.previous_valid) {
            sent_on_last = graph.import_texture({.texture = state.sent_on[last],
                                                 .name = "rc sent on (last frame)",
                                                 .state = rhi::ResourceState::ShaderRead,
                                                 .final_state = rhi::ResourceState::ShaderRead});
        }
        sent_on = graph.import_texture(
            {.texture = state.sent_on[now],
             .name = "rc sent on",
             .state = state.sent_on_used[now] ? rhi::ResourceState::ShaderRead : rhi::ResourceState::Undefined,
             .final_state = rhi::ResourceState::ShaderRead,
             .discard = true});
        state.sent_on_used[now] = true;
        state.sent_on_now = now;
    } else {
        state.sent_on_frame = 0;
    }

    // The pyramids: the GI picture's depth, normals and light from the screen's, then each mip from the one before,
    // a barrier between.
    struct PyramidPass {
        GraphTexture depth;
        GraphTexture normal;
        GraphTexture radiance;
    };
    const auto& pyramid_pass = graph.add_pass<PyramidPass>(
        "rc pyramid",
        [&](PassBuilder& pass, PyramidPass& data) {
            const auto pyramid = [&](rhi::Format format, const char* name) {
                return pass.create_texture({.desc = {.width = size[0],
                                                     .height = size[1],
                                                     .mip_count = mips,
                                                     .format = format,
                                                     .usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled,
                                                     .debug_name = name}});
            };
            data.depth = pyramid(rhi::Format::R32Float, "rc depth");
            data.normal = pyramid(rhi::Format::RGBA16Float, "rc normal");
            data.radiance = pyramid(rhi::Format::RGBA16Float, "rc radiance");
            pass.use(input.depth, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
            pass.use(input.radiance, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
            pass.use(input.glow, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
            if (sent_on_last.is_valid()) {
                pass.use(sent_on_last, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
            }
            pass.use(data.depth, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            pass.use(data.normal, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            pass.use(data.radiance, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
        },
        [scene_depth = input.depth, scene_radiance = input.radiance, glow = input.glow, frame = input.frame_address,
         pyramid_pipeline, width = size[0], height = size[1], screen_width = screen[0], screen_height = screen[1],
         scale, mips, sent_on_last](const PyramidPass& data, PassContext& context) {
            DE_PROFILE_ZONE("rc pyramid pass");
            rhi::CommandList& list = context.list;
            const rhi::TextureHandle depth = context.texture(data.depth);
            const rhi::TextureHandle normal = context.texture(data.normal);
            const rhi::TextureHandle radiance = context.texture(data.radiance);
            list.bind_pipeline(pyramid_pipeline);
            uint32_t source[2] = {screen_width, screen_height};
            for (uint32_t mip = 0; mip < mips; ++mip) {
                const uint32_t mip_size[2] = {std::max(width >> mip, 1U), std::max(height >> mip, 1U)};
                if (mip > 0) {
                    // The mip before is written: this one reads it.
                    list.barrier(rhi::GlobalBarrier{.before = rhi::ResourceState::ShaderWrite,
                                                    .after = rhi::ResourceState::ShaderWrite,
                                                    .before_stages = rhi::ShaderStages::Compute,
                                                    .after_stages = rhi::ShaderStages::Compute});
                }
                RcPyramidPush push{.frame = frame,
                                   .depth_out = context.device.storage_texture_index(depth, mip),
                                   .normal_out = context.device.storage_texture_index(normal, mip),
                                   .radiance_out = context.device.storage_texture_index(radiance, mip),
                                   .size = {mip_size[0], mip_size[1]},
                                   .source_size = {source[0], source[1]},
                                   .mip = mip,
                                   .scale = scale};
                if (mip == 0) {
                    push.depth_in = context.device.texture_index(context.texture(scene_depth));
                    push.radiance_in = context.device.texture_index(context.texture(scene_radiance));
                    push.glow = context.device.texture_index(context.texture(glow));
                    if (sent_on_last.is_valid()) {
                        push.bounce = context.device.texture_index(context.texture(sent_on_last));
                        push.has_bounce = 1;
                    }
                } else {
                    push.depth_in = context.device.storage_texture_index(depth, mip - 1);
                    push.radiance_in = context.device.storage_texture_index(radiance, mip - 1);
                    push.normal_in = context.device.storage_texture_index(normal, mip - 1);
                }
                list.push_constants(push);
                list.dispatch(groups(mip_size[0]), groups(mip_size[1]));
                source[0] = mip_size[0];
                source[1] = mip_size[1];
            }
        });
    const GraphTexture depth = pyramid_pass.depth;
    const GraphTexture normal = pyramid_pass.normal;
    const GraphTexture radiance = pyramid_pass.radiance;

    // Cascade 0's azimuths start along the screen's x: its samples fall on pixels' middles.
    const float phase = -std::numbers::pi_v<float> / static_cast<float>(settings.azimuths);

    // The stored cascades, the topmost first: each merges the one above it.
    GraphTexture upper;
    for (uint32_t step = 0; step + 1 < count; ++step) {
        const uint32_t i = count - 1 - step;
        const Layout& layout = layouts[i];
        const bool top = i == count - 1;
        const uint32_t stored = layout.stored();
        const RadianceCascadePush push{
            .frame = input.frame_address,
            .top = (top ? 1U : 0U) | (layout.wide ? 2U : 0U) | (!top && layouts[i + 1].wide ? 4U : 0U),
            .size = {size[0], size[1]},
            .probes = {layout.probes[0], layout.probes[1]},
            .upper_probes = {top ? 1U : layouts[i + 1].probes[0], top ? 1U : layouts[i + 1].probes[1]},
            .spacing = layout.spacing,
            .azimuths = layout.azimuths,
            .start_px = layout.start_px,
            .step_px = layout.step_px,
            .steps = layout.steps,
            .mip = layout.mip,
            .thickness_m = settings.thickness_m,
            .tolerance = settings.plane_tolerance,
            .phase = phase,
            .scale = scale,
            .screen = {screen[0], screen[1]}};
        const uint32_t width = layout.probes[0] * stored;
        const uint32_t height = layout.probes[1] * kRcElevations;
        struct CascadePass {
            GraphTexture out;
        };
        const GraphTexture above = upper;
        const auto& cascade_pass = graph.add_pass<CascadePass>(
            "radiance cascade",
            [&](PassBuilder& pass, CascadePass& data) {
                data.out =
                    pass.create_texture({.desc = {.width = width,
                                                  .height = height,
                                                  .format = kCascadeFormat,
                                                  .usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled,
                                                  .debug_name = "radiance cascade"}});
                pass.use(depth, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
                pass.use(normal, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
                pass.use(radiance, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
                if (above.is_valid()) {
                    pass.use(above, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
                }
                pass.use(data.out, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            },
            [push, cascade_pipeline, above, depth, normal, radiance, width,
             probes_y = layout.probes[1]](const CascadePass& data, PassContext& context) {
                DE_PROFILE_ZONE("radiance cascade pass");
                RadianceCascadePush bound = push;
                bound.depth = context.device.texture_index(context.texture(depth));
                bound.normal = context.device.texture_index(context.texture(normal));
                bound.radiance = context.device.texture_index(context.texture(radiance));
                bound.upper = above.is_valid() ? context.device.texture_index(context.texture(above)) : 0U;
                bound.output = context.device.storage_texture_index(context.texture(data.out));
                context.list.bind_pipeline(cascade_pipeline);
                context.list.push_constants(bound);
                // A thread a probe and a stored azimuth: it writes every elevation.
                context.list.dispatch(groups(width), groups(probes_y));
            });
        upper = cascade_pass.out;
    }

    // The gather: cascade 0 marched for each of the GI picture's pixels and merged with cascade 1; what's kept is the
    // light's moments over directions, a colour's below the one before's.
    const Layout& first = layouts[0];
    const RcGatherPush gather_push{
        .frame = input.frame_address,
        .has_upper = upper.is_valid() ? 1U : 0U,
        .size = {size[0], size[1]},
        .screen = {screen[0], screen[1]},
        .upper_probes = {count > 1 ? layouts[1].probes[0] : 1U, count > 1 ? layouts[1].probes[1] : 1U},
        .scale = scale,
        .azimuths = first.azimuths,
        .steps = first.steps,
        .start_px = first.start_px,
        .step_px = first.step_px,
        .thickness_m = settings.thickness_m,
        .tolerance = settings.plane_tolerance,
        .phase = phase,
        .bounce = settings.bounce,
        .white_sky = settings.white_sky ? 1U : 0U,
        .upper_wide = count > 1 && layouts[1].wide ? 1U : 0U,
        .sends_on = sent_on.is_valid() ? (settings.material_occlusion ? 3U : 1U) : 0U};
    struct GatherPass {
        GraphTexture light;
    };
    const GraphTexture cascade_1 = upper;
    const auto& gather_pass = graph.add_pass<GatherPass>(
        "rc gather",
        [&](PassBuilder& pass, GatherPass& data) {
            data.light = pass.create_texture({.desc = {.width = size[0],
                                                       .height = size[1] * 3,
                                                       .format = kCascadeFormat,
                                                       .usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled,
                                                       .debug_name = "rc light"}});
            pass.use(depth, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
            pass.use(normal, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
            pass.use(radiance, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
            if (cascade_1.is_valid()) {
                pass.use(cascade_1, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
            }
            if (sent_on.is_valid()) {
                pass.use(input.base_color, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
                pass.use(input.material, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
                pass.use(sent_on, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            }
            pass.use(data.light, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
        },
        [gather_push, gather_pipeline, cascade_1, depth, normal, radiance, width = size[0], height = size[1], sent_on,
         base_color = input.base_color, material = input.material, kept = &state,
         frame_number = input.frame->number](const GatherPass& data, PassContext& context) {
            DE_PROFILE_ZONE("rc gather pass");
            RcGatherPush bound = gather_push;
            bound.depth = context.device.texture_index(context.texture(depth));
            bound.normal = context.device.texture_index(context.texture(normal));
            bound.radiance = context.device.texture_index(context.texture(radiance));
            bound.upper = cascade_1.is_valid() ? context.device.texture_index(context.texture(cascade_1)) : 0U;
            bound.output = context.device.storage_texture_index(context.texture(data.light));
            if (sent_on.is_valid()) {
                bound.base_color = context.device.texture_index(context.texture(base_color));
                bound.material = context.device.texture_index(context.texture(material));
                bound.sent_on = context.device.storage_texture_index(context.texture(sent_on));
                kept->sent_on_frame = frame_number; // only now: a pass that didn't run kept nothing
            }
            context.list.bind_pipeline(gather_pipeline);
            context.list.push_constants(bound);
            context.list.dispatch(groups(width), groups(height));
        });

    RadianceCascadesResult result;
    result.depth = depth;
    result.normal = normal;
    result.light = gather_pass.light;
    result.size[0] = size[0];
    result.size[1] = size[1];
    result.scale = scale;
    result.tolerance = settings.plane_tolerance;
    result.material_occlusion = settings.material_occlusion;
    return result;
}

} // namespace de::render
