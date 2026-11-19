#include "render/scene/auto_exposure.h"

#include "core/allocator.h"
#include "core/log.h"
#include "core/profile.h"
#include "render/rhi/command_list.h"
#include "render/rhi/device.h"
#include "render/shader/shader_library.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace de::render {

namespace {

core::Allocator& render_heap() {
    return core::heap_allocator(core::MemoryTag::Render);
}

// Matches ExposureHistogramPush in shaders/scene/exposure_histogram.slang.
struct ExposureHistogramPush {
    uint64_t histogram = 0;
    uint32_t size[2] = {}; // at 8: a uint2's place in the shader's block
    uint32_t image = 0;
    float min_log2 = 0.0f;
    float bins_per_log2 = 0.0f;
    uint32_t reserved = 0;
};
static_assert(sizeof(ExposureHistogramPush) == 32);

// Matches ExposureAdaptPush in shaders/scene/exposure_adapt.slang.
struct ExposureAdaptPush {
    uint64_t histogram = 0;
    uint64_t state = 0;
    float min_log2 = 0.0f;
    float log2_per_bin = 0.0f;
    float low_share = 0.0f;
    float high_share = 0.0f;
    float key_log2 = 0.0f;
    float min_exposure_log2 = 0.0f;
    float max_exposure_log2 = 0.0f;
    float brighten_rate = 0.0f;
    float darken_rate = 0.0f;
    float step_s = 0.0f;
    uint32_t snap = 0;
    float base_log2 = 0.0f;
    float share = 1.0f;
};
static_assert(sizeof(ExposureAdaptPush) == 72);

constexpr float kBinsPerLog2 = static_cast<float>(kExposureBins - 1) / (kExposureMaxLog2 - kExposureMinLog2);

} // namespace

struct AutoExposureState {
    rhi::Device* device = nullptr;
    shader::ShaderLibrary* shaders = nullptr;
    shader::ProgramHandle histogram_program;
    shader::ProgramHandle adapt_program;
    rhi::BufferHandle state; // GpuExposureState, the GPU's
    bool state_used = false; // by some frame: the graph is told how that left it
    bool snap = true;
    AutoExposureSettings settings;
};

core::Result<AutoExposure> AutoExposure::create(const AutoExposureDesc& desc) {
    if (desc.device == nullptr || desc.shaders == nullptr) {
        return core::fail(core::ErrorCode::InvalidArgument, "auto exposure needs a device and a shader library");
    }
    auto* state =
        ::new (render_heap().allocate(sizeof(AutoExposureState), alignof(AutoExposureState))) AutoExposureState();
    state->device = desc.device;
    state->shaders = desc.shaders;
    AutoExposure exposure(state);
    const core::Result<rhi::BufferHandle> kept = desc.device->create_buffer(
        {.size = sizeof(GpuExposureState),
         .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst | rhi::BufferUsage::CopySrc, // tests read it
         .debug_name = "exposure"});
    if (!kept) {
        return core::fail(kept.error());
    }
    state->state = *kept;
    const auto program = [&](const char* module, const char* name) {
        const core::Result<shader::ProgramHandle> made =
            desc.shaders->create_compute({.module = module, .debug_name = name});
        if (!made) {
            DE_LOG_WARN("render", "%s's shader didn't build: the environment's exposure until it does", name);
        }
        return made ? *made : shader::ProgramHandle{};
    };
    state->histogram_program = program("scene.exposure_histogram", "exposure histogram");
    state->adapt_program = program("scene.exposure_adapt", "exposure adapt");
    return exposure;
}

AutoExposure::~AutoExposure() {
    if (state_ == nullptr) {
        return;
    }
    state_->shaders->destroy(state_->histogram_program);
    state_->shaders->destroy(state_->adapt_program);
    state_->device->destroy(state_->state);
    state_->~AutoExposureState();
    render_heap().deallocate(state_, sizeof(AutoExposureState), alignof(AutoExposureState));
}

AutoExposure::AutoExposure(AutoExposure&& other) noexcept : state_(other.state_) {
    other.state_ = nullptr;
}

AutoExposure& AutoExposure::operator=(AutoExposure&& other) noexcept {
    if (this != &other) {
        std::swap(state_, other.state_);
    }
    return *this;
}

void AutoExposure::snap() {
    state_->snap = true;
}

void AutoExposure::set_settings(const AutoExposureSettings& settings) {
    state_->settings = settings;
    AutoExposureSettings& kept = state_->settings;
    kept.low_share = std::clamp(settings.low_share, 0.0f, 0.99f);
    kept.high_share = std::clamp(settings.high_share, kept.low_share + 0.01f, 1.0f);
    kept.key = std::max(settings.key, 1e-4f);
    kept.below_ev = std::max(settings.below_ev, 0.0f);
    kept.adaptation = std::clamp(settings.adaptation, 0.0f, 1.0f);
    kept.above_ev = std::max(settings.above_ev, 0.0f);
}

const AutoExposureSettings& AutoExposure::settings() const {
    return state_->settings;
}

rhi::BufferHandle AutoExposure::state_buffer() const {
    return state_->state;
}

AutoExposure::Result AutoExposure::add_passes(RenderGraph& graph, const AutoExposureInput& input) {
    AutoExposureState& state = *state_;
    const rhi::PipelineHandle histogram_pipeline = state.shaders->pipeline(state.histogram_program);
    const rhi::PipelineHandle adapt_pipeline = state.shaders->pipeline(state.adapt_program);
    if (!histogram_pipeline.is_valid() || !adapt_pipeline.is_valid() || !input.image.is_valid() || input.width == 0 ||
        input.height == 0) {
        return {};
    }
    const AutoExposureSettings& settings = state.settings;
    // The state stays between frames: the first frame's starts as nothing (has_value 0).
    const GraphBuffer kept =
        graph.import_buffer({.buffer = state.state,
                             .name = "exposure",
                             .state = state.state_used ? rhi::ResourceState::ShaderRead : rhi::ResourceState::Undefined,
                             .final_state = rhi::ResourceState::ShaderRead});
    const bool first = !state.state_used;
    state.state_used = true;

    struct HistogramPass {
        GraphBuffer histogram;
    };
    const uint32_t width = input.width;
    const uint32_t height = input.height;
    const auto& counted = graph.add_pass<HistogramPass>(
        "exposure histogram",
        [&](PassBuilder& pass, HistogramPass& data) {
            data.histogram = pass.create_buffer({.size = kExposureBins * sizeof(uint32_t),
                                                 .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst,
                                                 .debug_name = "exposure histogram"});
            pass.use(input.image, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
            pass.use(data.histogram, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            if (first) {
                pass.use(kept, rhi::ResourceState::CopyDst);
            }
        },
        [image = input.image, histogram_pipeline, width, height, first, kept](const HistogramPass& data,
                                                                              PassContext& context) {
            DE_PROFILE_ZONE("exposure histogram pass");
            const rhi::BufferHandle histogram = context.buffer(data.histogram);
            context.list.fill_buffer(histogram, 0, kExposureBins * sizeof(uint32_t), 0);
            if (first) {
                context.list.fill_buffer(context.buffer(kept), 0, sizeof(GpuExposureState), 0);
            }
            context.list.barrier(rhi::GlobalBarrier{.before = rhi::ResourceState::CopyDst,
                                                    .after = rhi::ResourceState::ShaderWrite,
                                                    .after_stages = rhi::ShaderStages::Compute});
            const ExposureHistogramPush push{.histogram = context.device.buffer_address(histogram),
                                             .size = {width, height},
                                             .image = context.device.texture_index(context.texture(image)),
                                             .min_log2 = kExposureMinLog2,
                                             .bins_per_log2 = kBinsPerLog2};
            context.list.bind_pipeline(histogram_pipeline);
            context.list.push_constants(push);
            // A thread every other pixel each way: a group covers 32 x 32 pixels.
            context.list.dispatch((width + 31) / 32, (height + 31) / 32);
        });
    const GraphBuffer histogram = counted.histogram;

    const ExposureAdaptPush adapt{
        .min_log2 = kExposureMinLog2,
        .log2_per_bin = 1.0f / kBinsPerLog2,
        .low_share = settings.low_share,
        .high_share = settings.high_share,
        .key_log2 = std::log2(settings.key) + settings.compensation_ev,
        .min_exposure_log2 = input.base_ev - (input.below_ev >= 0.0f ? input.below_ev : settings.below_ev),
        .max_exposure_log2 = input.base_ev + (input.above_ev >= 0.0f ? input.above_ev : settings.above_ev),
        .brighten_rate = settings.brighten_per_s,
        .darken_rate = settings.darken_per_s,
        .step_s = std::clamp(input.step_s, 0.0f, 0.25f),
        .snap = state.snap ? 1U : 0U,
        .base_log2 = input.base_ev,
        .share = input.adaptation >= 0.0f ? std::min(input.adaptation, 1.0f) : settings.adaptation};
    state.snap = false;
    graph.add_pass(
        "exposure adapt",
        [&](PassBuilder& pass) {
            pass.use(histogram, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
            pass.use(kept, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            pass.never_cull();
        },
        [adapt, adapt_pipeline, histogram, kept](PassContext& context) {
            DE_PROFILE_ZONE("exposure adapt pass");
            ExposureAdaptPush push = adapt;
            push.histogram = context.device.buffer_address(context.buffer(histogram));
            push.state = context.device.buffer_address(context.buffer(kept));
            context.list.bind_pipeline(adapt_pipeline);
            context.list.push_constants(push);
            context.list.dispatch(1);
        });
    return {.exposure_address = state.device->buffer_address(state.state) + offsetof(GpuExposureState, exposure),
            .state = kept};
}

} // namespace de::render
