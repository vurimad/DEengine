#pragma once

#include "core/result.h"

#include <flecs.h>

#include <cstdint>

namespace de::core {
class JobSystem;
}

namespace de::world {

struct WorldState;

// The world's phases, in order (docs/design.md, Core systems). Input to PostSim run on every fixed
// tick; RenderExtract once per rendered frame, after the ticks, with the interpolation alpha.
enum class Phase : uint8_t {
    Input,
    PreSim,
    Combat,
    Physics,
    Animation,
    PostSim,
    RenderExtract,
    Count,
};

struct WorldDesc {
    double tick_rate_hz = 60.0;
    // A long frame (a hitch, a breakpoint) runs at most this many ticks; the time beyond is dropped,
    // so the game slows down for a moment instead of spiralling.
    uint32_t max_ticks_per_frame = 4;
    // Multithreaded systems run as flecs task threads on this JobSystem. They synchronize with each
    // other, so they need workers free at once: at most worker_count() of them. 0: one thread.
    core::JobSystem* jobs = nullptr;
    uint32_t task_threads = 0;
};

// What one advance() did.
struct FrameStep {
    uint32_t ticks = 0;        // fixed ticks run
    float alpha = 0.0f;        // how far between the last tick and the next the frame is, in [0, 1)
    bool dropped_time = false; // more time passed than max_ticks_per_frame cover
};

// The game world (docs/design.md, Core systems): a flecs world, set up with the engine's memory,
// log and job system (World data rules), its phases as two pipelines, and the fixed 60 Hz tick.
//
//     auto world = World::create({.jobs = &jobs});
//     world->ecs().system<Transform, const Velocity>("move").kind(world->phase(Phase::PreSim))...;
//     each frame: world->advance(frame_seconds);   // ticks as needed, then RenderExtract
//
// Move-only. Several may exist one after another (tests), but only one with task threads at a time.
class World {
public:
    static core::Result<World> create(const WorldDesc& desc = {});

    ~World();
    World(World&& other) noexcept;
    World& operator=(World&& other) noexcept;
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    flecs::world& ecs();
    // The phase entity a system goes in: .kind(world.phase(Phase::Combat)).
    flecs::entity phase(Phase phase) const;

    // Real time since the last call: runs the fixed ticks that fit (the simulation phases), then
    // RenderExtract once. Systems get the fixed step (or the frame time, in RenderExtract) as
    // delta_time.
    FrameStep advance(double real_dt_s);
    // One fixed tick now, without RenderExtract (tests, replays, the headless mode).
    void tick();

    double tick_dt_s() const;
    uint64_t tick_count() const;
    float alpha() const; // of the last advance()

private:
    explicit World(WorldState* state) : state_(state) {}

    WorldState* state_ = nullptr;
};

} // namespace de::world
