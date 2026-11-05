#include "core/job_system.h"
#include "core/memory_stats.h"
#include "world/flecs_os.h"
#include "world/transform.h"
#include "world/world.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string_view>

using de::DVec3;
using de::Quat;
using de::Vec3;
using de::world::FrameStep;
using de::world::MeshInstance;
using de::world::Phase;
using de::world::PreviousWorldTransform;
using de::world::Transform;
using de::world::World;
using de::world::WorldTransform;

namespace {

struct Velocity {
    Vec3 meters_per_s{0.0f};
};

// A system in phase that appends its phase's number to a log, as letters (A for Input...).
void log_phase(World& world, Phase phase, char* log, size_t* length) {
    world.ecs().system(nullptr).kind(world.phase(phase)).run([log, length, phase](flecs::iter& it) {
        while (it.next()) {
        }
        log[(*length)++] = static_cast<char>('A' + static_cast<int>(phase));
    });
}

void add_movement(World& world) {
    world.ecs()
        .system<Transform, const Velocity>("move")
        .kind(world.phase(Phase::PreSim))
        .each([](flecs::iter& it, size_t, Transform& transform, const Velocity& velocity) {
            transform.position += DVec3(velocity.meters_per_s) * static_cast<double>(it.delta_time());
        });
}

bool near(double a, double b) {
    return std::fabs(a - b) <= 1e-4;
}

} // namespace

TEST_CASE("the world runs its phases in order on every tick, and RenderExtract once a frame") {
    auto world = World::create();
    REQUIRE(world.ok());
    char log[64] = {};
    size_t length = 0;
    // Made out of order: the phases decide when they run, not the order systems were made in.
    for (const Phase phase : {Phase::RenderExtract, Phase::PostSim, Phase::Input, Phase::Physics, Phase::PreSim,
                              Phase::Animation, Phase::Combat}) {
        log_phase(*world, phase, log, &length);
    }
    const FrameStep step = world->advance(2.0 / 60.0 + 1e-6);
    CHECK(step.ticks == 2);
    CHECK(std::string_view(log, length) == "ABCDEFABCDEFG");
}

TEST_CASE("fixed ticks: time adds up across frames, long frames drop what doesn't fit") {
    auto world = World::create({.tick_rate_hz = 60.0, .max_ticks_per_frame = 4});
    REQUIRE(world.ok());
    float seen_dt = 0.0f;
    world->ecs().system(nullptr).kind(world->phase(Phase::Combat)).run([&seen_dt](flecs::iter& it) {
        while (it.next()) {
        }
        seen_dt = static_cast<float>(it.delta_time());
    });
    const double tick = 1.0 / 60.0;
    CHECK(world->advance(tick * 0.5).ticks == 0);
    CHECK(near(world->alpha(), 0.5f));
    CHECK(world->advance(tick * 0.75).ticks == 1); // 1.25 ticks in all
    CHECK(near(world->alpha(), 0.25f));
    CHECK(near(seen_dt, static_cast<float>(tick))); // systems see the fixed step
    const FrameStep hitch = world->advance(tick * 10.5);
    CHECK(hitch.ticks == 4);
    CHECK(hitch.dropped_time);
    CHECK(world->tick_count() == 5);
    CHECK(world->alpha() < 1.0f);
}

TEST_CASE("world transforms interpolate between ticks, and static tables aren't copied") {
    auto world = World::create();
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    add_movement(*world);
    // Transform brings WorldTransform and PreviousWorldTransform with it.
    const flecs::entity mover = ecs.entity().set(Transform{}).set(Velocity{.meters_per_s = {60.0f, 0.0f, 0.0f}});
    CHECK(mover.has<WorldTransform>());
    CHECK(mover.has<PreviousWorldTransform>());
    // place(): where it should be at once, previous included (no slide in from the origin).
    const flecs::entity statue = ecs.entity();
    de::world::place(statue, Transform{.position = {5.0, 0.0, 0.0}});
    CHECK(near(statue.get<WorldTransform>().position.x, 5.0));
    CHECK(near(statue.get<PreviousWorldTransform>().value.position.x, 5.0));

    world->tick();
    world->tick();
    CHECK(near(mover.get<Transform>().position.x, 2.0));
    CHECK(near(mover.get<WorldTransform>().position.x, 2.0));               // computed at the end of PostSim
    CHECK(near(mover.get<PreviousWorldTransform>().value.position.x, 1.0)); // as the second tick began
    CHECK(near(de::world::interpolate(mover.get<PreviousWorldTransform>().value, mover.get<WorldTransform>(), 0.25f)
                   .position.x,
               1.25));
    CHECK(near(statue.get<PreviousWorldTransform>().value.position.x, 5.0));

    // The statue's table: nothing writes its transforms, so its PreviousWorldTransform is left alone
    // (a marker shows it). The mover's table keeps being copied.
    statue.set(PreviousWorldTransform{.value = {.position = {-1.0, 0.0, 0.0}}});
    world->tick();
    CHECK(near(statue.get<PreviousWorldTransform>().value.position.x, -1.0));
    CHECK(near(mover.get<PreviousWorldTransform>().value.position.x, 2.0));

    // Rotations take the short way round.
    const WorldTransform half = de::world::interpolate(
        WorldTransform{}, WorldTransform{.rotation = glm::angleAxis(glm::radians(90.0f), de::kWorldUp)}, 0.5f);
    CHECK(near(glm::degrees(glm::angle(half.rotation)), 45.0));
}

TEST_CASE("each instance knows where it was a frame ago, from the same two ticks") {
    auto world = World::create();
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    add_movement(*world);
    constexpr double kTick = 1.0 / 60.0;
    // Moving at a meter a tick; standing still; and place()d mid-flight.
    const flecs::entity mover =
        ecs.entity().set(Transform{}).set(Velocity{.meters_per_s = {60.0f, 0.0f, 0.0f}}).set(de::world::MeshInstance{});
    const flecs::entity statue = ecs.entity().set(de::world::MeshInstance{});
    de::world::place(statue, Transform{.position = {-100.0, 0.0, 0.0}});

    const auto instance_x = [&](flecs::entity entity, bool previous) {
        const de::world::RenderFrame& frame = world->render_frame();
        const bool want_statue = entity == statue; // told apart by where they are: it's far behind
        for (const de::world::RenderInstance& instance : frame.instances) {
            const float x = previous ? instance.previous_transform[3] : instance.transform[3];
            if (want_statue == (instance.transform[3] < -50.0f)) {
                return x;
            }
        }
        FAIL("no such instance");
        return 0.0f;
    };
    world->advance(kTick * 1.25); // a tick, and a quarter of the next
    const float first = instance_x(mover, false);
    CHECK(first == doctest::Approx(0.25f));
    // Half a tick later, no tick between: a frame ago is where it was drawn.
    world->advance(kTick * 0.5);
    CHECK(instance_x(mover, false) == doctest::Approx(0.75f));
    CHECK(instance_x(mover, true) == doctest::Approx(first));
    // Half a tick later again, over a tick: the same, through the tick boundary.
    world->advance(kTick * 0.5);
    CHECK(instance_x(mover, false) == doctest::Approx(1.25f));
    CHECK(instance_x(mover, true) == doctest::Approx(0.75f));
    // What doesn't move has no motion.
    CHECK(instance_x(statue, false) == doctest::Approx(-100.0f));
    CHECK(instance_x(statue, true) == doctest::Approx(-100.0f));
    // place(): no motion, however far it jumped.
    de::world::place(mover, Transform{.position = {3.0, 0.0, 0.0}});
    world->advance(kTick * 0.1);
    CHECK(instance_x(mover, false) == doctest::Approx(3.0f));
    CHECK(instance_x(mover, true) == doctest::Approx(3.0f));
    // A frame longer than a tick (30 fps here) goes back past the previous tick by the same motion:
    // the whole frame's motion, not a tick's.
    world->advance(kTick * 2.0);
    world->advance(kTick * 2.0);
    CHECK(instance_x(mover, false) - instance_x(mover, true) == doctest::Approx(2.0f));
    // A hitch: no more than the ticks ran (four by default), not the second that passed.
    world->advance(1.0);
    CHECK(instance_x(mover, false) - instance_x(mover, true) == doctest::Approx(4.0f));
    // A scale growing from zero doesn't go negative a frame before it began.
    const WorldTransform before = de::world::pose_at(WorldTransform{.scale = {0.1f, 0.1f, 0.1f}},
                                                     WorldTransform{.scale = {1.0f, 1.0f, 1.0f}}, -0.5f);
    CHECK(before.scale.x == doctest::Approx(0.1f));
}

TEST_CASE("the view a frame ago is what was drawn, at this frame's origin, and a cut drops it") {
    auto world = World::create();
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    const flecs::entity camera = ecs.entity().set(de::world::Camera{.vertical_fov_rad = 0.9f});
    de::world::place(camera, Transform{.position = {1.0, 0.0, 0.0}});
    ecs.set(de::world::ActiveCamera{.entity = camera.id()});
    const de::world::RenderView& view = world->render_frame().camera;
    constexpr double kFrame = 1.0 / 240.0;
    // The first frame has nothing to look back at.
    world->advance(kFrame);
    REQUIRE(view.valid);
    CHECK_FALSE(view.previous_valid);
    // A free camera is place()d every frame, so its ticks don't say where it was: what was drawn does.
    de::world::place(camera, Transform{.position = {2.0, 0.0, 0.0}});
    world->advance(kFrame);
    REQUIRE(view.previous_valid);
    CHECK(view.transform[3] == doctest::Approx(2.0f));
    CHECK(view.previous_transform[3] == doctest::Approx(1.0f));
    CHECK(view.previous_vertical_fov_rad == doctest::Approx(0.9f));
    // The render origin jumps with it: the same world position, relative to the new origin.
    de::world::place(camera, Transform{.position = {2000.0, 0.0, 0.0}});
    world->advance(kFrame);
    CHECK(world->render_frame().origin.x == 2048.0);
    CHECK(view.transform[3] == doctest::Approx(-48.0f));
    CHECK(view.previous_transform[3] == doctest::Approx(2.0f - 2048.0f));
    // A cut: no looking back, once.
    de::world::place(camera, Transform{.position = {2001.0, 0.0, 0.0}});
    ecs.get_mut<de::world::ActiveCamera>().cut = true;
    world->advance(kFrame);
    CHECK_FALSE(view.previous_valid);
    CHECK_FALSE(ecs.get<de::world::ActiveCamera>().cut);
    world->advance(kFrame);
    CHECK(view.previous_valid);
    // Another camera is a cut by itself.
    const flecs::entity other = ecs.entity().set(de::world::Camera{});
    de::world::place(other, Transform{.position = {2002.0, 0.0, 0.0}});
    ecs.set(de::world::ActiveCamera{.entity = other.id()});
    world->advance(kFrame);
    CHECK_FALSE(view.previous_valid);
    world->advance(kFrame);
    CHECK(view.previous_valid);
}

TEST_CASE("components are reflected: inspectable and serializable") {
    auto world = World::create();
    REQUIRE(world.ok());
    const flecs::entity entity =
        world->ecs().entity("thing").set(Transform{.position = {1.0, 2.0, 3.0}, .scale = {2.0f, 2.0f, 2.0f}});
    const flecs::string json = entity.to_json();
    const std::string_view text(json.c_str());
    CAPTURE(text);
    CHECK(text.find("\"position\":{\"x\":1, \"y\":2, \"z\":3}") != std::string_view::npos);
    CHECK(text.find("\"scale\":{\"x\":2, \"y\":2, \"z\":2}") != std::string_view::npos);
    CHECK(text.find("\"rotation\":{\"x\":0, \"y\":0, \"z\":0, \"w\":1}") != std::string_view::npos);
    // The quaternion is x, y, z, w in memory, as reflected.
    const Quat rotation(4.0f, 1.0f, 2.0f, 3.0f); // w, x, y, z
    float memory[4];
    std::memcpy(memory, &rotation, sizeof(memory));
    CHECK(memory[0] == 1.0f);
    CHECK(memory[3] == 4.0f);
}

TEST_CASE("flecs runs on the engine: its memory tagged World, none per tick in steady state") {
    const de::core::MemoryStats before_world = de::core::memory_stats(de::core::MemoryTag::World);
    {
        auto world = World::create();
        REQUIRE(world.ok());
        REQUIRE(de::world::detail::flecs_os_api_installed());
        // flecs' own allocations, through our heap: far more than the World object alone.
        CHECK(de::core::memory_stats(de::core::MemoryTag::World).total_allocations - before_world.total_allocations >
              100);
        add_movement(*world);
        for (int i = 0; i < 1000; ++i) {
            world->ecs()
                .entity()
                .set(Transform{})
                .set(MeshInstance{})
                .set(Velocity{.meters_per_s = {1.0f, 0.0f, 0.0f}});
        }
        for (int i = 0; i < 30; ++i) {
            world->advance(1.0 / 60.0); // warm up: tables, caches, stages
        }
        const uint64_t before = de::core::heap_allocation_count();
        for (int i = 0; i < 120; ++i) {
            world->advance(1.0 / 60.0);
        }
        CHECK(de::core::heap_allocation_count() - before == 0); // flecs' and extraction's included
        CHECK(world->render_frame().instances.size() == 1000);
    }
    // Every block given back.
    CHECK(de::core::memory_stats(de::core::MemoryTag::World).live_allocations == before_world.live_allocations);
}

TEST_CASE("flecs' own pipeline never runs the engine's phases") {
    auto world = World::create();
    REQUIRE(world.ok());
    int runs = 0;
    world->ecs().system(nullptr).kind(world->phase(Phase::Combat)).run([&runs](flecs::iter& it) {
        while (it.next()) {
        }
        ++runs;
    });
    world->ecs().progress(1.0f / 60.0f); // what a stray call, or an addon, would do
    CHECK(runs == 0);
    world->tick();
    CHECK(runs == 1);
}

TEST_CASE("systems split their rows across the JobSystem, each with the fixed step") {
    de::core::JobSystem jobs({.worker_count = 3});
    auto world = World::create({.jobs = &jobs});
    REQUIRE(world.ok());
    REQUIRE(world->jobs() == &jobs);
    world->ecs()
        .system<Transform, const Velocity>("move in parallel")
        .kind(world->phase(Phase::PreSim))
        .run([pool = world->jobs()](flecs::iter& it) {
            while (it.next()) {
                const auto transforms = it.field<Transform>(0);
                const auto velocities = it.field<const Velocity>(1);
                const auto dt = static_cast<float>(it.delta_time());
                de::world::for_each_row_parallel(pool, it.count(), 512, [&](size_t i) {
                    transforms[i].position += DVec3(velocities[i].meters_per_s * dt);
                });
            }
        });
    constexpr int kEntities = 20000;
    for (int i = 0; i < kEntities; ++i) {
        world->ecs().entity().set(Transform{}).set(Velocity{.meters_per_s = {60.0f, 0.0f, 0.0f}}); // 1 m per tick
    }
    for (int i = 0; i < 10; ++i) {
        world->tick();
    }
    // Every entity moved once per tick, by the fixed step.
    int wrong = 0;
    world->ecs().each([&wrong](const Transform& transform) { wrong += near(transform.position.x, 10.0) ? 0 : 1; });
    CHECK(wrong == 0);
}

TEST_CASE("benchmark: 10k entities in typical shapes, ticked and extracted") {
    de::core::JobSystem jobs;
    auto world = World::create({.jobs = &jobs});
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    add_movement(*world);
    // Enemies of a few kinds (prefabs whose shared data isn't copied), moving; props standing still.
    struct EnemyStats {
        float max_health = 100.0f;
        float poise = 30.0f;
    };
    ecs.component<EnemyStats>().add(flecs::OnInstantiate, flecs::Inherit);
    flecs::entity kinds[4];
    for (int k = 0; k < 4; ++k) {
        kinds[k] = ecs.prefab().set(EnemyStats{.max_health = 100.0f * static_cast<float>(k + 1)});
    }
    for (int i = 0; i < 6000; ++i) {
        ecs.entity()
            .is_a(kinds[i % 4])
            .set(Transform{.position = {static_cast<double>(i), 0.0, 0.0}})
            .set(MeshInstance{})
            .set(Velocity{.meters_per_s = {0.0f, 0.0f, 1.0f}});
    }
    for (int i = 0; i < 4000; ++i) {
        ecs.entity().set(Transform{.position = {0.0, 0.0, static_cast<double>(i)}}).set(MeshInstance{});
    }
    // Extraction is the world's own (render_frame()): every MeshInstance's interpolated matrix,
    // relative to the render origin, written once into one flat array.
    const flecs::query<const WorldTransform, const MeshInstance> renderable =
        ecs.query_builder<const WorldTransform, const MeshInstance>().cached().build();

    for (int i = 0; i < 10; ++i) {
        world->advance(1.0 / 60.0);
    }
    const auto start = std::chrono::steady_clock::now();
    constexpr int kFrames = 120;
    for (int i = 0; i < kFrames; ++i) {
        world->advance(1.0 / 60.0);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    // Tables follow kinds, not entity count: an IsA pair is a table per prefab (4 enemy kinds, their
    // stats stored once on the prefabs), and the props one more.
    const int32_t tables = renderable.c_ptr() != nullptr ? ecs_query_count(renderable.c_ptr()).tables : 0;
    MESSAGE("10k entities: ", ms / kFrames, " ms per frame (tick + extraction), ", tables, " tables extracted");
    CHECK(world->render_frame().slot_count == 10000);
    CHECK(tables == 5);
}
