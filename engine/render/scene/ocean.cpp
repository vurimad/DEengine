#include "render/scene/ocean.h"

#include "core/allocator.h"
#include "core/log.h"
#include "core/profile.h"
#include "render/rhi/command_list.h"
#include "render/rhi/device.h"
#include "render/shader/shader_library.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <utility>

namespace de::render {

namespace {

constexpr double kGravity = 9.81;
constexpr double kTwoPi = 6.283185307179586;
// As shaders/scene/ocean.slang has them.
constexpr double kSwellGamma = 7.0;
constexpr double kWindSpreadPower = 8.0;
constexpr double kSwellSpreadPower = 40.0;
// Past this dimensionless fetch the wind's waves are fully grown (Pierson-Moskowitz): JONSWAP's fits hold below it.
constexpr double kGrownFetch = 22000.0;
constexpr float kLeastWindMps = 0.5f;

core::Allocator& render_heap() {
    static core::Allocator& heap = core::heap_allocator(core::MemoryTag::Render);
    return heap;
}

// Matches OceanPush in shaders/scene/ocean.slang.
struct OceanPush {
    uint32_t spectrum = 0;
    uint32_t fields = 0;
    uint32_t waves = 0;
    uint32_t slopes = 0;
    uint32_t previous_slopes = 0;
    uint32_t source_waves = 0;
    uint32_t source_slopes = 0;
    uint32_t direction = 0;
    float time_s = 0.0f;
    float choppiness = 0.0f;
    float frame_s = 0.0f;
    float wind_speed_mps = 0.0f;
    float wind_direction[2] = {};
    float wind_alpha = 0.0f;
    float wind_peak = 0.0f;
    float swell_direction[2] = {};
    float swell_alpha = 0.0f;
    float swell_peak = 0.0f;
    float wind_spread = 0.0f;
    float swell_spread = 0.0f;
    uint32_t mip_size = 0;
    uint32_t has_previous = 0;
};
static_assert(sizeof(OceanPush) <= 128);

// JONSWAP's shape with alpha 1: energy per frequency, as the shader has it.
double jonswap_shape(double omega, double peak, double gamma) {
    if (omega <= 0.0) {
        return 0.0;
    }
    const double sigma = omega <= peak ? 0.07 : 0.09;
    const double r = std::exp(-((omega - peak) * (omega - peak)) / (2.0 * sigma * sigma * peak * peak));
    const double ratio = peak / omega;
    return kGravity * kGravity / std::pow(omega, 5.0) * std::exp(-1.25 * std::pow(ratio, 4.0)) * std::pow(gamma, r);
}

// The spreading's normalization: one over the integral of cos^2s of half the angle, all the way round.
double spread_normalization(double power) {
    constexpr int kSteps = 4096;
    double sum = 0.0;
    for (int i = 0; i < kSteps; ++i) {
        const double angle = -kTwoPi / 2.0 + ((i + 0.5) * kTwoPi / kSteps);
        sum += std::pow(std::cos(angle * 0.5), 2.0 * power);
    }
    return 1.0 / (sum * kTwoPi / kSteps);
}

void normalized(const float (&in)[2], float (&out)[2], float fallback_x, float fallback_z) {
    const float length = std::sqrt((in[0] * in[0]) + (in[1] * in[1]));
    out[0] = length > 1e-6f ? in[0] / length : fallback_x;
    out[1] = length > 1e-6f ? in[1] / length : fallback_z;
}

} // namespace

OceanSpectrum ocean_spectrum(const SceneSea& sea, float wind_speed_mps, const float (&wind_direction)[2]) {
    OceanSpectrum out;
    normalized(wind_direction, out.wind_direction, 1.0f, 0.0f);
    normalized(sea.swell_direction, out.swell_direction, 0.0f, -1.0f);
    if (wind_speed_mps >= kLeastWindMps) {
        const double wind = wind_speed_mps;
        const double fetch = std::clamp(kGravity * std::max(static_cast<double>(sea.fetch_m), 1.0) / (wind * wind), 1.0,
                                        kGrownFetch);
        out.wind_alpha = static_cast<float>(0.076 * std::pow(fetch, -0.22));
        out.wind_peak = static_cast<float>(22.0 * kGravity / wind * std::pow(fetch, -1.0 / 3.0));
    }
    if (sea.swell_height_m > 0.0f && sea.swell_period_s > 0.0f) {
        // Its alpha so the energy under it, the surface's variance, is (Hs / 4)^2.
        const double peak = kTwoPi / sea.swell_period_s;
        constexpr int kSteps = 4000;
        const double from = 0.2 * peak;
        const double to = 8.0 * peak;
        const double step = (to - from) / kSteps;
        double energy = 0.0;
        for (int i = 0; i < kSteps; ++i) {
            energy += jonswap_shape(from + ((i + 0.5) * step), peak, kSwellGamma) * step;
        }
        const double variance = (sea.swell_height_m / 4.0) * (sea.swell_height_m / 4.0);
        out.swell_alpha = static_cast<float>(variance / energy);
        out.swell_peak = static_cast<float>(peak);
    }
    out.wind_spread = static_cast<float>(spread_normalization(kWindSpreadPower));
    out.swell_spread = static_cast<float>(spread_normalization(kSwellSpreadPower));
    out.choppiness = sea.choppiness;
    return out;
}

struct OceanState {
    rhi::Device* device = nullptr;
    shader::ShaderLibrary* shaders = nullptr;
    shader::ProgramHandle spectrum_program;
    shader::ProgramHandle evolve_program;
    shader::ProgramHandle fft_program;
    shader::ProgramHandle assemble_program;
    shader::ProgramHandle mips_program;
    rhi::TextureHandle spectrum;
    rhi::TextureHandle fields;
    rhi::TextureHandle waves[2];
    rhi::TextureHandle slopes[2];
    uint32_t current = 0;
    bool written[2] = {};
    bool spectrum_made = false;
    OceanSpectrum made_from;

    ~OceanState() {
        for (const shader::ProgramHandle program :
             {spectrum_program, evolve_program, fft_program, assemble_program, mips_program}) {
            shaders->destroy(program);
        }
        for (const rhi::TextureHandle texture : {spectrum, fields, waves[0], waves[1], slopes[0], slopes[1]}) {
            if (texture.is_valid()) {
                device->destroy(texture);
            }
        }
    }
};

core::Result<Ocean> Ocean::create(rhi::Device& device, shader::ShaderLibrary& shaders) {
    auto* state = ::new (render_heap().allocate(sizeof(OceanState), alignof(OceanState))) OceanState();
    state->device = &device;
    state->shaders = &shaders;
    Ocean ocean(state);
    const auto make = [&](rhi::TextureHandle& out, uint32_t layers, uint32_t mips, rhi::Format format,
                          const char* name) -> core::Result<void> {
        core::Result<rhi::TextureHandle> made =
            device.create_texture({.width = kOceanSize,
                                   .height = kOceanSize,
                                   .mip_count = mips,
                                   .layer_count = layers,
                                   .format = format,
                                   .usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled,
                                   .debug_name = name}); // more than one layer: an array
        if (!made) {
            return core::fail(made.error());
        }
        out = *made;
        return {};
    };
    for (const core::Result<void> made :
         {make(state->spectrum, kOceanCascades, 1, rhi::Format::RGBA32Float, "ocean spectrum"),
          make(state->fields, kOceanCascades * 2, 1, rhi::Format::RGBA32Float, "ocean fields"),
          make(state->waves[0], kOceanCascades, kOceanMips, rhi::Format::RGBA16Float, "ocean waves"),
          make(state->waves[1], kOceanCascades, kOceanMips, rhi::Format::RGBA16Float, "ocean waves"),
          make(state->slopes[0], kOceanCascades, kOceanMips, rhi::Format::RGBA16Float, "ocean slopes"),
          make(state->slopes[1], kOceanCascades, kOceanMips, rhi::Format::RGBA16Float, "ocean slopes")}) {
        if (!made) {
            return core::fail(made.error());
        }
    }
    const auto program = [&](const char* entry, const char* name) {
        const core::Result<shader::ProgramHandle> made =
            shaders.create_compute({.module = "scene.ocean", .entry = entry, .debug_name = name});
        return made ? *made : shader::ProgramHandle{};
    };
    state->spectrum_program = program("spectrum_main", "ocean spectrum");
    state->evolve_program = program("evolve_main", "ocean evolve");
    state->fft_program = program("fft_main", "ocean fft");
    state->assemble_program = program("assemble_main", "ocean assemble");
    state->mips_program = program("mips_main", "ocean mips");
    return ocean;
}

Ocean::~Ocean() {
    if (state_ == nullptr) {
        return;
    }
    state_->~OceanState();
    render_heap().deallocate(state_, sizeof(OceanState), alignof(OceanState));
}

Ocean::Ocean(Ocean&& other) noexcept : state_(std::exchange(other.state_, nullptr)) {}

Ocean& Ocean::operator=(Ocean&& other) noexcept {
    if (this != &other) {
        const Ocean discarded(std::exchange(state_, std::exchange(other.state_, nullptr)));
    }
    return *this;
}

OceanFrame Ocean::add_pass(RenderGraph& graph, const SceneSea& sea, float wind_speed_mps,
                           const float (&wind_direction)[2], double time_s, double frame_s, const DVec3& origin) {
    OceanState& state = *state_;
    OceanFrame frame;
    const rhi::PipelineHandle spectrum = state.shaders->pipeline(state.spectrum_program);
    const rhi::PipelineHandle evolve = state.shaders->pipeline(state.evolve_program);
    const rhi::PipelineHandle fft = state.shaders->pipeline(state.fft_program);
    const rhi::PipelineHandle assemble = state.shaders->pipeline(state.assemble_program);
    const rhi::PipelineHandle mips = state.shaders->pipeline(state.mips_program);
    if (!spectrum.is_valid() || !evolve.is_valid() || !fft.is_valid() || !assemble.is_valid() || !mips.is_valid()) {
        return frame;
    }
    const OceanSpectrum wanted = ocean_spectrum(sea, wind_speed_mps, wind_direction);
    const bool make_spectrum = !state.spectrum_made || !(wanted == state.made_from);
    state.spectrum_made = true;
    state.made_from = wanted;

    const uint32_t previous = state.current;
    state.current ^= 1U;
    const uint32_t current = state.current;
    const bool has_previous = state.written[previous];
    state.written[current] = true; // the graph leaves it so, even if the pass is culled
    const auto import = [&](rhi::TextureHandle texture, const char* name, rhi::ResourceState before,
                            rhi::ResourceState after) {
        return graph.import_texture({.texture = texture, .name = name, .state = before, .final_state = after});
    };
    // The spectrum and the fields stay in storage between frames; a new spectrum doesn't need the old one kept.
    const GraphTexture spectrum_texture =
        import(state.spectrum, "ocean spectrum", make_spectrum ? rhi::ResourceState::Undefined : rhi::ResourceState::ShaderWrite,
               rhi::ResourceState::ShaderWrite);
    const GraphTexture fields_texture =
        import(state.fields, "ocean fields", rhi::ResourceState::Undefined, rhi::ResourceState::ShaderWrite);
    frame.waves = import(state.waves[current], "ocean waves", rhi::ResourceState::Undefined, rhi::ResourceState::ShaderRead);
    frame.slopes = import(state.slopes[current], "ocean slopes", rhi::ResourceState::Undefined, rhi::ResourceState::ShaderRead);
    GraphTexture previous_slopes;
    if (has_previous) {
        frame.previous_waves =
            import(state.waves[previous], "last ocean waves", rhi::ResourceState::ShaderRead, rhi::ResourceState::ShaderRead);
        previous_slopes = import(state.slopes[previous], "last ocean slopes", rhi::ResourceState::ShaderRead,
                                 rhi::ResourceState::ShaderRead);
    } else {
        frame.previous_waves = frame.waves;
    }
    frame.has_previous = has_previous;

    OceanPush push;
    push.time_s = static_cast<float>(std::fmod(std::max(time_s, 0.0), 256.0));
    push.choppiness = wanted.choppiness;
    push.frame_s = static_cast<float>(std::max(frame_s, 0.0));
    push.wind_speed_mps = wind_speed_mps;
    std::copy_n(wanted.wind_direction, 2, push.wind_direction);
    push.wind_alpha = wanted.wind_alpha;
    push.wind_peak = wanted.wind_peak;
    std::copy_n(wanted.swell_direction, 2, push.swell_direction);
    push.swell_alpha = wanted.swell_alpha;
    push.swell_peak = wanted.swell_peak;
    push.wind_spread = wanted.wind_spread;
    push.swell_spread = wanted.swell_spread;
    push.has_previous = has_previous ? 1U : 0U;

    const GraphTexture waves_texture = frame.waves;
    const GraphTexture slopes_texture = frame.slopes;
    graph.add_pass(
        "ocean",
        [&](PassBuilder& pass) {
            pass.use(spectrum_texture, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            pass.use(fields_texture, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            pass.use(waves_texture, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            pass.use(slopes_texture, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            if (has_previous) {
                pass.use(previous_slopes, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
            }
        },
        [push, make_spectrum, has_previous, spectrum_texture, fields_texture, waves_texture, slopes_texture,
         previous_slopes, spectrum, evolve, fft, assemble, mips](PassContext& context) {
            DE_PROFILE_ZONE("ocean pass");
            rhi::CommandList& list = context.list;
            const rhi::GlobalBarrier step{.before = rhi::ResourceState::ShaderWrite,
                                          .after = rhi::ResourceState::ShaderWrite,
                                          .before_stages = rhi::ShaderStages::Compute,
                                          .after_stages = rhi::ShaderStages::Compute};
            const rhi::TextureHandle waves_handle = context.texture(waves_texture);
            const rhi::TextureHandle slopes_handle = context.texture(slopes_texture);
            OceanPush now = push;
            now.spectrum = context.device.storage_texture_index(context.texture(spectrum_texture));
            now.fields = context.device.storage_texture_index(context.texture(fields_texture));
            now.waves = context.device.storage_texture_index(waves_handle);
            now.slopes = context.device.storage_texture_index(slopes_handle);
            now.previous_slopes = has_previous ? context.device.texture_index(context.texture(previous_slopes)) : 0;
            constexpr uint32_t kGroups = kOceanSize / 8;
            if (make_spectrum) {
                list.bind_pipeline(spectrum);
                list.push_constants(now);
                list.dispatch(kGroups, kGroups, kOceanCascades);
                list.barrier(step);
            }
            list.bind_pipeline(evolve);
            list.push_constants(now);
            list.dispatch(kGroups, kGroups, kOceanCascades);
            list.barrier(step);
            list.bind_pipeline(fft);
            for (uint32_t direction = 0; direction < 2; ++direction) {
                now.direction = direction;
                list.push_constants(now);
                list.dispatch(1, kOceanSize, kOceanCascades * 2);
                list.barrier(step);
            }
            list.bind_pipeline(assemble);
            list.push_constants(now);
            list.dispatch(kGroups, kGroups, kOceanCascades);
            list.barrier(step);
            list.bind_pipeline(mips);
            for (uint32_t mip = 1; mip < kOceanMips; ++mip) {
                const uint32_t size = kOceanSize >> mip;
                now.source_waves = context.device.storage_texture_index(waves_handle, mip - 1);
                now.source_slopes = context.device.storage_texture_index(slopes_handle, mip - 1);
                now.waves = context.device.storage_texture_index(waves_handle, mip);
                now.slopes = context.device.storage_texture_index(slopes_handle, mip);
                now.mip_size = size;
                list.push_constants(now);
                list.dispatch((size + 7) / 8, (size + 7) / 8, kOceanCascades);
                list.barrier(step);
            }
        });

    for (uint32_t c = 0; c < kOceanCascades; ++c) {
        const double tile = kOceanTileM[c];
        frame.tile_offset[c][0] = static_cast<float>(std::fmod(origin.x, tile) / tile);
        frame.tile_offset[c][1] = static_cast<float>(std::fmod(origin.z, tile) / tile);
    }
    frame.valid = true;
    return frame;
}

} // namespace de::render
