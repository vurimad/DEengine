#pragma once

#include "core/job_system.h"
#include "core/result.h"
#include "world/render_extract.h"

#include <flecs.h>

#include <cstddef>
#include <cstdint>

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
    // For systems that split their rows across threads (for_each_row_parallel); null: one thread.
    // flecs' own threads stay off: its workers wait on each other at every merge, which a job system
    // can't promise.
    core::JobSystem* jobs = nullptr;
    // Cells release the assets they hold through it (docs/design.md, World model); it must outlive
    // the world. Null: cells hold nothing (tests of the world alone).
    asset::AssetRegistry* assets = nullptr;
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
// Never call ecs().progress(): the phases run only through advance() and tick(). Move-only.
class World {
public:
    static core::Result<World> create(const WorldDesc& desc = {});

    ~World();
    World(World&& other) noexcept;
    World& operator=(World&& other) noexcept;
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    flecs::world& ecs();
    core::JobSystem* jobs() const;
    // What the last advance() extracted for the renderer: the camera and every MeshInstance,
    // interpolated, relative to the render origin. Valid until the next advance().
    const RenderFrame& render_frame() const;
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

// Runs row(i) for every i in [0, rows), split across the JobSystem's threads in batches; serial
// without one or when it all fits in one batch. For a system over big tables: inside its loop over
// tables, each row is written by one thread only, and nothing structural happens (no add, remove
// or set on other entities).
//     for_each_row_parallel(world.jobs(), it.count(), 256, [&](size_t i) { transforms[i] = ...; });
template <typename Row>
void for_each_row_parallel(core::JobSystem* jobs, size_t rows, size_t batch, const Row& row) {
    if (jobs == nullptr || rows <= batch) {
        for (size_t i = 0; i < rows; ++i) {
            row(i);
        }
        return;
    }
    jobs->parallel_for(
        rows, batch,
        [](size_t begin, size_t end, void* context) {
            const Row& each = *static_cast<const Row*>(context);
            for (size_t i = begin; i < end; ++i) {
                each(i);
            }
        },
        const_cast<Row*>(&row), "world rows");
}

} // namespace de::world
