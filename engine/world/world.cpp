#include "world/world.h"

#include "core/allocator.h"
#include "core/assert.h"
#include "core/job_system.h"
#include "core/log.h"
#include "core/profile.h"
#include "world/cell.h"
#include "world/flecs_os.h"
#include "world/transform.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <utility>

namespace de::world {

namespace {

core::Allocator& world_heap() {
    return core::heap_allocator(core::MemoryTag::World);
}

// Tags on the phase entities: which pipeline a phase's systems run in.
struct SimulationPhase {};
struct FramePhase {};

constexpr const char* kPhaseNames[] = {"Input", "PreSim", "Combat", "Physics", "Animation", "PostSim", "RenderExtract"};
static_assert(std::size(kPhaseNames) == static_cast<size_t>(Phase::Count));

// A pipeline running the systems whose phase carries Tag, phases in DependsOn order and systems in
// the order they were made (flecs' built-in pipeline, with the tag in place of flecs::Phase).
template <typename Tag>
flecs::entity make_pipeline(flecs::world& ecs, const char* name) {
    return ecs.pipeline()
        .with(flecs::System)
        .with<Tag>()
        .cascade(flecs::DependsOn)
        .without(flecs::Disabled)
        .up(flecs::DependsOn)
        .without(flecs::Disabled)
        .up(flecs::ChildOf)
        .build()
        .set_name(name);
}

} // namespace

struct WorldState {
    // Before ecs, so they outlive the world's systems, which write into them.
    RenderFrame render_frame;
    RenderExtractScratch extract_scratch;
    CellSpawnScratch cell_scratch;
    flecs::world ecs;
    flecs::entity phases[static_cast<size_t>(Phase::Count)];
    flecs::entity simulation; // the pipelines
    flecs::entity frame;
    double tick_dt_s = 1.0 / 60.0;
    uint32_t max_ticks_per_frame = 4;
    double accumulator_s = 0.0;
    uint64_t ticks = 0;
    float alpha = 0.0f;
    float alpha_step = 0.0f; // how much of a tick the last advance() took
    core::JobSystem* jobs = nullptr;
    int64_t merges = 0; // flecs' merge count at the last advance()
};

core::Result<World> World::create(const WorldDesc& desc) {
    if (!(desc.tick_rate_hz > 0.0) || desc.max_ticks_per_frame == 0) {
        return core::fail(core::ErrorCode::InvalidArgument, "the world needs a positive tick rate and ticks per frame");
    }
    detail::install_flecs_os_api();

    auto* state = ::new (world_heap().allocate(sizeof(WorldState), alignof(WorldState))) WorldState();
    state->tick_dt_s = 1.0 / desc.tick_rate_hz;
    state->max_ticks_per_frame = desc.max_ticks_per_frame;
    state->jobs = desc.jobs;
    flecs::world& ecs = state->ecs;
    register_transform(ecs);
    register_cells(ecs, desc.assets);
    register_render_components(ecs);

    // The phases: the simulation ones in a DependsOn chain, RenderExtract on its own. Only our tags,
    // not flecs::Phase: flecs' built-in pipeline (progress()) must never run them.
    ecs.component<SimulationPhase>();
    ecs.component<FramePhase>();
    flecs::entity previous;
    for (size_t i = 0; i < static_cast<size_t>(Phase::RenderExtract); ++i) {
        const flecs::entity phase = ecs.entity(kPhaseNames[i]).add<SimulationPhase>();
        if (previous.is_valid()) {
            phase.depends_on(previous);
        }
        state->phases[i] = phase;
        previous = phase;
    }
    // World transforms in a phase of their own after PostSim, so they follow every game system
    // that moves something, whenever it was made.
    const flecs::entity transforms = ecs.entity("de::WorldTransformsPhase").add<SimulationPhase>().depends_on(previous);
    // Cells spawn before extraction too: a frame may run no tick.
    const flecs::entity spawn_cells = ecs.entity("de::SpawnCellsPhase").add<FramePhase>();
    state->phases[static_cast<size_t>(Phase::RenderExtract)] =
        ecs.entity(kPhaseNames[static_cast<size_t>(Phase::RenderExtract)]).add<FramePhase>().depends_on(spawn_cells);
    state->simulation = make_pipeline<SimulationPhase>(ecs, "de::SimulationPipeline");
    state->frame = make_pipeline<FramePhase>(ecs, "de::FramePipeline");

    add_transform_systems(ecs, state->phases[static_cast<size_t>(Phase::Input)], transforms);
    if (desc.assets != nullptr) {
        add_cell_systems(ecs, state->phases[static_cast<size_t>(Phase::Input)], spawn_cells, *desc.assets,
                         state->cell_scratch);
    }
    add_render_extract(ecs, state->phases[static_cast<size_t>(Phase::RenderExtract)],
                       {.frame = &state->render_frame,
                        .scratch = &state->extract_scratch,
                        .alpha = &state->alpha,
                        .alpha_step = &state->alpha_step,
                        .jobs = desc.jobs});
    return World(state);
}

World::~World() {
    if (state_ == nullptr) {
        return;
    }
    state_->~WorldState();
    world_heap().deallocate(state_, sizeof(WorldState), alignof(WorldState));
}

World::World(World&& other) noexcept : state_(std::exchange(other.state_, nullptr)) {}

World& World::operator=(World&& other) noexcept {
    if (this != &other) {
        const World discarded(std::exchange(state_, std::exchange(other.state_, nullptr)));
    }
    return *this;
}

flecs::world& World::ecs() {
    return state_->ecs;
}

const RenderFrame& World::render_frame() const {
    return state_->render_frame;
}

core::JobSystem* World::jobs() const {
    return state_->jobs;
}

flecs::entity World::phase(Phase phase) const {
    DE_ASSERT(phase < Phase::Count);
    return state_->phases[static_cast<size_t>(phase)];
}

void World::tick() {
    DE_PROFILE_ZONE("world tick");
    // frame_begin gives systems the step (it.delta_time()). flecs' own timers (interval, rate,
    // tick sources) don't run: their systems live in phases our pipelines don't have. Time systems
    // by the fixed tick instead.
    const flecs::world& ecs = state_->ecs;
    ecs.frame_begin(static_cast<ecs_ftime_t>(state_->tick_dt_s));
    ecs.run_pipeline(state_->simulation, static_cast<ecs_ftime_t>(state_->tick_dt_s));
    ecs.frame_end();
    ++state_->ticks;
}

FrameStep World::advance(double real_dt_s) {
    DE_PROFILE_ZONE("world advance");
    WorldState& state = *state_;
    FrameStep step;
    state.accumulator_s += std::max(real_dt_s, 0.0);
    while (state.accumulator_s >= state.tick_dt_s && step.ticks < state.max_ticks_per_frame) {
        tick();
        state.accumulator_s -= state.tick_dt_s;
        ++step.ticks;
    }
    if (state.accumulator_s >= state.tick_dt_s) {
        // More time than the ticks allowed: drop it, keeping only the part of a tick left over.
        state.accumulator_s = std::fmod(state.accumulator_s, state.tick_dt_s);
        step.dropped_time = true;
    }
    state.alpha = static_cast<float>(state.accumulator_s / state.tick_dt_s);
    state.alpha_step = static_cast<float>(std::max(real_dt_s, 0.0) / state.tick_dt_s);
    step.alpha = state.alpha;
    {
        DE_PROFILE_ZONE("world frame");
        const auto frame_dt = static_cast<ecs_ftime_t>(std::max(real_dt_s, 1e-6)); // 0 would mean "measure it"
        state.ecs.frame_begin(frame_dt);
        state.ecs.run_pipeline(state.frame, frame_dt);
        state.ecs.frame_end();
    }
    // Fragmentation and sync points, visible (World data rules: measured).
    const ecs_world_info_t* info = ecs_get_world_info(state.ecs.c_ptr());
    DE_PROFILE_PLOT("world tables", static_cast<int64_t>(info->table_count));
    DE_PROFILE_PLOT("world merges/frame", info->merge_count_total - state.merges);
    state.merges = info->merge_count_total;
    return step;
}

double World::tick_dt_s() const {
    return state_->tick_dt_s;
}

uint64_t World::tick_count() const {
    return state_->ticks;
}

float World::alpha() const {
    return state_->alpha;
}

} // namespace de::world
