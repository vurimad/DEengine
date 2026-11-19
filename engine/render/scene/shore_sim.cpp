#include "render/scene/shore_sim.h"

#include "core/allocator.h"
#include "core/profile.h"
#include "render/rhi/command_list.h"
#include "render/rhi/device.h"
#include "render/shader/shader_library.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <new>
#include <utility>

namespace de::render {

namespace {

core::Allocator& render_heap() {
    static core::Allocator& heap = core::heap_allocator(core::MemoryTag::Render);
    return heap;
}

// Matches ShoreSimPush in shaders/scene/shore_sim.slang.
struct ShoreSimPush {
    uint64_t frame = 0;
    uint64_t levels = 0;
    uint64_t nodes = 0;
    uint64_t slots = 0;
    uint64_t heights = 0;
    uint32_t state_in = 0;
    uint32_t state_out = 0;
    uint32_t surface = 0;
    uint32_t shore_material = 0;
    uint32_t flags = 0;
    uint32_t finest_level = 0;
    uint32_t terrain_origin_steps[2] = {};
    uint32_t terrain_size_steps[2] = {};
    int32_t corner_cell[2] = {};
    int32_t previous_corner_cell[2] = {};
    float corner[2] = {}; // at 96: a float2 sits on 8 bytes in the shader's layout
    float sea_level_m = 0.0f;
    float dt_s = 0.0f;
    float time_offset_s = 0.0f;
    float wave_amplitude_m = 0.0f;
    float wave_period_s = 0.0f;
};
static_assert(sizeof(ShoreSimPush) <= 128);
static_assert(offsetof(ShoreSimPush, corner) == 96);

constexpr uint32_t kFlagTerrain = 1;
constexpr uint32_t kFlagFresh = 2;
constexpr uint32_t kFlagPrevious = 4; // last frame's surface is there, for the foam it held

} // namespace

ShoreSimWindow shore_sim_window(const DVec3& camera, const DVec3& origin) {
    ShoreSimWindow window;
    const double cell = kShoreSimCellM;
    const auto half = static_cast<int64_t>(kShoreSimSide / 2);
    const auto x = static_cast<int64_t>(std::floor(camera.x / cell)) - half;
    const auto z = static_cast<int64_t>(std::floor(camera.z / cell)) - half;
    window.corner_cell[0] = static_cast<int32_t>(x);
    window.corner_cell[1] = static_cast<int32_t>(z);
    window.corner[0] = static_cast<float>((static_cast<double>(x) * cell) - origin.x);
    window.corner[1] = static_cast<float>((static_cast<double>(z) * cell) - origin.z);
    return window;
}

struct ShoreSimState {
    rhi::Device* device = nullptr;
    shader::ShaderLibrary* shaders = nullptr;
    shader::ProgramHandle prepare_program;
    shader::ProgramHandle velocity_program;
    shader::ProgramHandle height_program;
    shader::ProgramHandle surface_program;
    rhi::TextureHandle state[2]; // h, u, w, b: [0] the state between frames, [1] mid-step
    rhi::TextureHandle surfaces[2];
    uint32_t current = 0; // which surface this frame writes
    bool ran = false;     // the state holds water from an earlier frame
    bool surface_written[2] = {};
    int32_t corner_cell[2] = {};
    double sea_level_m = 0.0;
    uint32_t shore_material = 0;
    double owed_s = 0.0; // time not yet stepped
    double time_s = 0.0; // the world's, when it last ran

    ~ShoreSimState() {
        for (const shader::ProgramHandle program :
             {prepare_program, velocity_program, height_program, surface_program}) {
            shaders->destroy(program);
        }
        for (const rhi::TextureHandle texture : {state[0], state[1], surfaces[0], surfaces[1]}) {
            if (texture.is_valid()) {
                device->destroy(texture);
            }
        }
    }
};

core::Result<ShoreSim> ShoreSim::create(rhi::Device& device, shader::ShaderLibrary& shaders) {
    auto* state = ::new (render_heap().allocate(sizeof(ShoreSimState), alignof(ShoreSimState))) ShoreSimState();
    state->device = &device;
    state->shaders = &shaders;
    ShoreSim sim(state);
    const auto make = [&](rhi::TextureHandle& out, rhi::Format format, const char* name) -> core::Result<void> {
        core::Result<rhi::TextureHandle> made =
            device.create_texture({.width = kShoreSimSide,
                                   .height = kShoreSimSide,
                                   .format = format,
                                   .usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled,
                                   .debug_name = name});
        if (!made) {
            return core::fail(made.error());
        }
        out = *made;
        return {};
    };
    for (const core::Result<void> made : {make(state->state[0], rhi::Format::RGBA32Float, "shore sim state"),
                                          make(state->state[1], rhi::Format::RGBA32Float, "shore sim step"),
                                          make(state->surfaces[0], rhi::Format::RGBA16Float, "shore sim surface"),
                                          make(state->surfaces[1], rhi::Format::RGBA16Float, "shore sim surface")}) {
        if (!made) {
            return core::fail(made.error());
        }
    }
    const auto program = [&](const char* entry, const char* name) {
        const core::Result<shader::ProgramHandle> made =
            shaders.create_compute({.module = "scene.shore_sim", .entry = entry, .debug_name = name});
        return made ? *made : shader::ProgramHandle{};
    };
    state->prepare_program = program("prepare_main", "shore sim prepare");
    state->velocity_program = program("velocity_main", "shore sim velocity");
    state->height_program = program("height_main", "shore sim height");
    state->surface_program = program("surface_main", "shore sim surface");
    return sim;
}

ShoreSim::~ShoreSim() {
    if (state_ == nullptr) {
        return;
    }
    state_->~ShoreSimState();
    render_heap().deallocate(state_, sizeof(ShoreSimState), alignof(ShoreSimState));
}

ShoreSim::ShoreSim(ShoreSim&& other) noexcept : state_(std::exchange(other.state_, nullptr)) {}

ShoreSim& ShoreSim::operator=(ShoreSim&& other) noexcept {
    if (this != &other) {
        const ShoreSim discarded(std::exchange(state_, std::exchange(other.state_, nullptr)));
    }
    return *this;
}

rhi::TextureHandle ShoreSim::surface() const {
    return state_->surfaces[state_->current];
}

rhi::TextureHandle ShoreSim::previous_surface() const {
    const uint32_t previous = state_->current ^ 1U;
    return state_->surface_written[previous] ? state_->surfaces[previous] : surface();
}

bool ShoreSim::running() const {
    const ShoreSimState& state = *state_;
    return state.shaders->pipeline(state.prepare_program).is_valid() &&
           state.shaders->pipeline(state.velocity_program).is_valid() &&
           state.shaders->pipeline(state.height_program).is_valid() &&
           state.shaders->pipeline(state.surface_program).is_valid();
}

void ShoreSim::reset() {
    state_->ran = false;
    state_->owed_s = 0.0;
}

ShoreSimFrame ShoreSim::add_pass(RenderGraph& graph, const ShoreSimInput& input) {
    ShoreSimState& state = *state_;
    ShoreSimFrame frame;
    const rhi::PipelineHandle prepare = state.shaders->pipeline(state.prepare_program);
    const rhi::PipelineHandle velocity = state.shaders->pipeline(state.velocity_program);
    const rhi::PipelineHandle height = state.shaders->pipeline(state.height_program);
    const rhi::PipelineHandle surface = state.shaders->pipeline(state.surface_program);
    if (!prepare.is_valid() || !velocity.is_valid() || !height.is_valid() || !surface.is_valid()) {
        return frame;
    }
    // A new sea starts at rest; so does a camera that jumped farther than the window.
    const ShoreSimWindow window = shore_sim_window(input.camera, input.origin);
    const bool jumped = std::abs(window.corner_cell[0] - state.corner_cell[0]) >= static_cast<int32_t>(kShoreSimSide) ||
                        std::abs(window.corner_cell[1] - state.corner_cell[1]) >= static_cast<int32_t>(kShoreSimSide);
    const bool fresh =
        !state.ran || jumped || input.sea_level_m != state.sea_level_m || input.shore_material != state.shore_material;
    state.sea_level_m = input.sea_level_m;
    state.shore_material = input.shore_material;

    // Fixed steps: as many as the time owed holds, a few at most. The world's time going back (a new world, a
    // reload) owes nothing.
    const double moved_s = state.ran ? input.time_s - state.time_s : 0.0;
    state.time_s = input.time_s;
    state.owed_s = fresh ? 0.0 : std::min(state.owed_s + std::max(moved_s, 0.0), kShoreSimStepS * kShoreSimMostSteps);
    const auto steps = static_cast<uint32_t>(state.owed_s / kShoreSimStepS);
    state.owed_s -= steps * kShoreSimStepS;

    const uint32_t previous = state.current ^ 1U;
    const uint32_t current = state.current;
    state.current ^= 1U;
    const bool has_previous = state.surface_written[previous] && !fresh;
    state.surface_written[current] = true;
    const auto import = [&](rhi::TextureHandle texture, const char* name, rhi::ResourceState before,
                            rhi::ResourceState after) {
        return graph.import_texture({.texture = texture, .name = name, .state = before, .final_state = after});
    };
    const GraphTexture state_texture = import(
        state.state[0], "shore sim state", state.ran ? rhi::ResourceState::ShaderWrite : rhi::ResourceState::Undefined,
        rhi::ResourceState::ShaderWrite);
    const GraphTexture step_texture =
        import(state.state[1], "shore sim step", rhi::ResourceState::Undefined, rhi::ResourceState::ShaderWrite);
    frame.surface = import(state.surfaces[current], "shore sim surface", rhi::ResourceState::Undefined,
                           rhi::ResourceState::ShaderRead);
    frame.previous_surface = has_previous ? import(state.surfaces[previous], "last shore sim surface",
                                                   rhi::ResourceState::ShaderRead, rhi::ResourceState::ShaderRead)
                                          : frame.surface;

    ShoreSimPush push;
    push.frame = input.frame;
    if (input.terrain != nullptr) {
        push.levels = input.terrain->levels;
        push.nodes = input.terrain->nodes;
        push.slots = input.terrain->slots;
        push.heights = input.terrain->heights;
        push.finest_level = input.terrain->finest_level;
        std::copy_n(input.terrain->origin_steps, 2, push.terrain_origin_steps);
        std::copy_n(input.terrain->size_steps, 2, push.terrain_size_steps);
        push.flags |= kFlagTerrain;
    }
    if (fresh) {
        push.flags |= kFlagFresh;
    }
    if (has_previous) {
        push.flags |= kFlagPrevious;
    }
    push.shore_material = input.shore_material;
    std::copy_n(window.corner_cell, 2, push.corner_cell);
    std::copy_n(state.ran && !jumped ? state.corner_cell : window.corner_cell, 2, push.previous_corner_cell);
    push.sea_level_m = static_cast<float>(input.sea_level_m);
    push.dt_s = static_cast<float>(kShoreSimStepS);
    push.wave_amplitude_m = input.wave_amplitude_m;
    push.wave_period_s = input.wave_period_s;
    std::copy_n(window.corner, 2, push.corner);
    state.corner_cell[0] = window.corner_cell[0];
    state.corner_cell[1] = window.corner_cell[1];
    state.ran = true;

    const GraphTexture surface_texture = frame.surface;
    const GraphTexture previous_texture = frame.previous_surface;
    graph.add_pass(
        "shore sim",
        [&](PassBuilder& pass) {
            pass.use(state_texture, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            pass.use(step_texture, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            pass.use(surface_texture, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
            if (has_previous) {
                pass.use(previous_texture, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Compute);
            }
        },
        [push, steps, state_texture, step_texture, surface_texture, prepare, velocity, height,
         surface](PassContext& context) {
            DE_PROFILE_ZONE("shore sim pass");
            rhi::CommandList& list = context.list;
            const rhi::GlobalBarrier step{.before = rhi::ResourceState::ShaderWrite,
                                          .after = rhi::ResourceState::ShaderWrite,
                                          .before_stages = rhi::ShaderStages::Compute,
                                          .after_stages = rhi::ShaderStages::Compute};
            ShoreSimPush now = push;
            now.state_in = context.device.storage_texture_index(context.texture(state_texture));
            now.state_out = context.device.storage_texture_index(context.texture(step_texture));
            now.surface = context.device.storage_texture_index(context.texture(surface_texture));
            constexpr uint32_t kGroups = kShoreSimSide / 8;
            list.bind_pipeline(prepare);
            list.push_constants(now);
            list.dispatch(kGroups, kGroups);
            list.barrier(step);
            for (uint32_t i = 0; i < steps; ++i) {
                // The steps' times: the last is the frame's.
                now.time_offset_s = static_cast<float>((steps - 1 - i) * kShoreSimStepS);
                list.bind_pipeline(velocity);
                list.push_constants(now);
                list.dispatch(kGroups, kGroups);
                list.barrier(step);
                list.bind_pipeline(height);
                list.push_constants(now);
                list.dispatch(kGroups, kGroups);
                list.barrier(step);
            }
            // The surface, and the foam on it, carried as far as the frame's steps moved the water.
            now.dt_s = static_cast<float>(steps * kShoreSimStepS);
            list.bind_pipeline(surface);
            list.push_constants(now);
            list.dispatch(kGroups, kGroups);
        });
    frame.window = window;
    frame.valid = true;
    return frame;
}

} // namespace de::render
