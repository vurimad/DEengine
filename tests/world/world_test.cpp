#include "core/job_system.h"
#include "core/memory_stats.h"
#include "world/transform.h"
#include "world/world.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string_view>

using de::Quat;
using de::Vec3;
using de::world::FrameStep;
using de::world::Phase;
using de::world::PreviousTransform;
using de::world::Transform;
using de::world::World;

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
            transform.position += velocity.meters_per_s * static_cast<float>(it.delta_time());
        });
}

bool near(float a, float b) {
    return std::fabs(a - b) <= 1e-4f;
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

TEST_CASE("transforms interpolate between ticks, and static tables aren't copied") {
    auto world = World::create();
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    add_movement(*world);
    const flecs::entity mover = ecs.entity()
                                    .set(Transform{})
                                    .set(PreviousTransform{})
                                    .set(Velocity{.meters_per_s = {60.0f, 0.0f, 0.0f}}); // 1 m per tick
    const flecs::entity statue = ecs.entity().set(Transform{.position = {5.0f, 0.0f, 0.0f}}).set(PreviousTransform{});

    world->tick();
    world->tick();
    CHECK(near(mover.get<Transform>().position.x, 2.0f));
    CHECK(near(mover.get<PreviousTransform>().value.position.x, 1.0f)); // as the second tick began
    CHECK(near(de::world::interpolate(mover.get<PreviousTransform>().value, mover.get<Transform>(), 0.25f).position.x,
               1.25f));
    CHECK(near(statue.get<PreviousTransform>().value.position.x, 5.0f));

    // The statue's table: nothing writes its Transform, so its PreviousTransform is left alone
    // (a marker shows it). The mover's table keeps being copied.
    statue.set(PreviousTransform{.value = {.position = {-1.0f, 0.0f, 0.0f}}});
    world->tick();
    CHECK(near(statue.get<PreviousTransform>().value.position.x, -1.0f));
    CHECK(near(mover.get<PreviousTransform>().value.position.x, 2.0f));

    // Rotations take the short way round.
    const Transform half = de::world::interpolate(
        Transform{}, Transform{.rotation = glm::angleAxis(glm::radians(90.0f), de::kWorldUp)}, 0.5f);
    CHECK(near(glm::degrees(glm::angle(half.rotation)), 45.0f));
}

TEST_CASE("components are reflected: inspectable and serializable") {
    auto world = World::create();
    REQUIRE(world.ok());
    const flecs::entity entity =
        world->ecs().entity("thing").set(Transform{.position = {1.0f, 2.0f, 3.0f}, .scale = {2.0f, 2.0f, 2.0f}});
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

TEST_CASE("flecs memory is ours: tagged World, and no heap allocations per tick in steady state") {
    const uint64_t world_before = de::core::memory_stats(de::core::MemoryTag::World).live_allocations;
    {
        auto world = World::create();
        REQUIRE(world.ok());
        CHECK(de::core::memory_stats(de::core::MemoryTag::World).live_allocations > world_before);
        add_movement(*world);
        for (int i = 0; i < 1000; ++i) {
            world->ecs()
                .entity()
                .set(Transform{})
                .set(PreviousTransform{})
                .set(Velocity{.meters_per_s = {1.0f, 0.0f, 0.0f}});
        }
        for (int i = 0; i < 30; ++i) {
            world->advance(1.0 / 60.0); // warm up: tables, caches, stages
        }
        const uint64_t before = de::core::heap_allocation_count();
        for (int i = 0; i < 120; ++i) {
            world->advance(1.0 / 60.0);
        }
        CHECK(de::core::heap_allocation_count() - before == 0);
    }
    CHECK(de::core::memory_stats(de::core::MemoryTag::World).live_allocations == world_before); // all given back
}

TEST_CASE("multithreaded systems run as task threads on the JobSystem") {
    de::core::JobSystem jobs({.worker_count = 3});
    CHECK(World::create({.jobs = &jobs, .task_threads = 4}).error().code == de::core::ErrorCode::InvalidArgument);
    auto world = World::create({.jobs = &jobs, .task_threads = 2});
    REQUIRE(world.ok());
    add_movement(*world);
    world->ecs()
        .system<Transform>("spread")
        .kind(world->phase(Phase::Physics))
        .multi_threaded()
        .each([](Transform& transform) { transform.scale.y += 1.0f; });
    constexpr int kEntities = 20000;
    for (int i = 0; i < kEntities; ++i) {
        world->ecs().entity().set(Transform{}).set(Velocity{.meters_per_s = {60.0f, 0.0f, 0.0f}});
    }
    for (int i = 0; i < 10; ++i) {
        world->tick();
    }
    // Every entity was touched once per tick, by exactly one thread.
    int wrong = 0;
    world->ecs().each([&wrong](const Transform& transform) {
        wrong += (near(transform.scale.y, 11.0f) && near(transform.position.x, 10.0f)) ? 0 : 1;
    });
    CHECK(wrong == 0);
}

TEST_CASE("benchmark: 10k entities in typical shapes, ticked and extracted") {
    auto world = World::create();
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
            .set(Transform{.position = {static_cast<float>(i), 0.0f, 0.0f}})
            .set(PreviousTransform{})
            .set(Velocity{.meters_per_s = {0.0f, 0.0f, 1.0f}});
    }
    for (int i = 0; i < 4000; ++i) {
        ecs.entity().set(Transform{.position = {0.0f, 0.0f, static_cast<float>(i)}}).set(PreviousTransform{});
    }
    // Extraction: every entity's interpolated matrix, written once into one flat output.
    static de::Mat4 extracted[10000];
    size_t extracted_count = 0;
    flecs::query<const Transform, const PreviousTransform> renderable =
        ecs.query_builder<const Transform, const PreviousTransform>().cached().build();
    ecs.system(nullptr).kind(world->phase(Phase::RenderExtract)).run([&](flecs::iter& it) {
        while (it.next()) {
        }
        extracted_count = 0;
        const float alpha = world->alpha();
        renderable.run([&](flecs::iter& table) {
            while (table.next()) {
                const auto current = table.field<const Transform>(0);
                const auto previous = table.field<const PreviousTransform>(1);
                for (const size_t i : table) {
                    extracted[extracted_count++] =
                        de::world::to_matrix(de::world::interpolate(previous[i].value, current[i], alpha));
                }
            }
        });
    });

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
    CHECK(extracted_count == 10000);
    CHECK(tables == 5);
}
