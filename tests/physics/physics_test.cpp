#include "core/allocator.h"
#include "core/job_system.h"
#include "core/memory_stats.h"
#include "physics/physics.h"
#include "platform/time.h"
#include "world/transform.h"
#include "world/world.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>

using de::DVec3;
using de::Vec3;
using de::physics::BodyKind;
using de::physics::CharacterController;
using de::physics::Collider;
using de::physics::Physics;
using de::physics::PhysicsDesc;
using de::physics::QueryHit;
using de::physics::RigidBody;
using de::physics::ShapeKind;
using de::world::Transform;
using de::world::World;

namespace {

// A world with physics in it, physics outliving it.
struct Scene {
    explicit Scene(de::core::JobSystem* jobs = nullptr, const PhysicsDesc& desc = {})
        : physics([&] {
              PhysicsDesc with_jobs = desc;
              with_jobs.jobs = jobs;
              return with_jobs;
          }()),
          world(World::create({.jobs = jobs}).value()) {
        physics.add_to_world(world);
    }

    const flecs::world& ecs() { return world.ecs(); }

    void ticks(int count) {
        for (int i = 0; i < count; ++i) {
            world.tick();
        }
    }

    flecs::entity box(const DVec3& position, const Vec3& half_extents, BodyKind kind = BodyKind::Static) {
        flecs::entity entity = ecs().entity();
        entity.set(Collider{.shape = ShapeKind::Box, .half_extents_m = half_extents});
        entity.set(RigidBody{.kind = kind});
        de::world::place(entity, Transform{.position = position});
        return entity;
    }

    flecs::entity sphere(const DVec3& position, float radius_m = 0.5f) {
        flecs::entity entity = ecs().entity();
        entity.set(Collider{.shape = ShapeKind::Sphere, .radius_m = radius_m});
        entity.set(RigidBody{.kind = BodyKind::Dynamic});
        de::world::place(entity, Transform{.position = position});
        return entity;
    }

    flecs::entity character(const DVec3& position) {
        flecs::entity entity = ecs().entity();
        entity.set(CharacterController{});
        de::world::place(entity, Transform{.position = position});
        return entity;
    }

    Physics physics;
    World world;
};

DVec3 where(flecs::entity entity) {
    return entity.get<Transform>().position;
}

const CharacterController& controller(flecs::entity entity) {
    return entity.get<CharacterController>();
}

} // namespace

TEST_CASE("a dynamic body falls under gravity and comes to rest on a static one") {
    Scene scene;
    const flecs::entity floor = scene.box({0.0, -0.5, 0.0}, {50.0f, 0.5f, 50.0f});
    const flecs::entity ball = scene.sphere({0.0, 20.0, 0.0});
    scene.ticks(1);
    CHECK(scene.physics.stats().bodies == 2);
    // Half a second of free fall: about g t^2 / 2, from a little below the top, as a step of semi-implicit Euler goes.
    scene.ticks(29);
    const double fallen = 20.0 - where(ball).y;
    CHECK(fallen == doctest::Approx(0.5 * 9.81 * 0.25).epsilon(0.1));
    scene.ticks(240);
    // A resting contact sinks in by Jolt's slop, 2 cm.
    CHECK(where(ball).y == doctest::Approx(0.48).epsilon(0.01));
    CHECK(where(floor).y == -0.5);
}

TEST_CASE("what's asleep stops writing its transform, and static bodies never do") {
    Scene scene;
    scene.box({0.0, -0.5, 0.0}, {50.0f, 0.5f, 50.0f});
    const flecs::entity ball = scene.sphere({0.0, 0.6, 0.0});
    scene.ticks(120);
    REQUIRE(scene.physics.stats().bodies == 2);
    CHECK(scene.physics.stats().active_bodies == 0);
    // A hand on it: a place() would wake it, so what's there stays exactly as it was left.
    const DVec3 resting = where(ball);
    scene.ticks(30);
    CHECK(where(ball) == resting);
}

TEST_CASE("place() on a dynamic body teleports it, and it's at rest there") {
    Scene scene;
    const flecs::entity ball = scene.sphere({0.0, 1000.0, 0.0});
    scene.ticks(60);
    CHECK(where(ball).y < 1000.0);
    de::world::place(ball, Transform{.position = {5.0, 200.0, 0.0}});
    scene.ticks(1);
    // Where it was sent, with one tick's fall from standing and none of what it had.
    CHECK(where(ball).x == doctest::Approx(5.0));
    CHECK(where(ball).y == doctest::Approx(200.0).epsilon(0.001));
    CHECK(200.0 - where(ball).y < 0.01);
}

TEST_CASE("a kinematic body goes where its transform says, and pushes what's in the way") {
    Scene scene;
    const flecs::entity wall = scene.box({0.0, 1.0, 0.0}, {0.25f, 1.0f, 1.0f}, BodyKind::Kinematic);
    const flecs::entity crate = scene.box({2.0, 1.0, 0.0}, {0.5f, 0.5f, 0.5f}, BodyKind::Dynamic);
    scene.box({0.0, -0.5, 0.0}, {50.0f, 0.5f, 50.0f});
    scene.ticks(30);
    const double crate_x = where(crate).x;
    // A meter a second, along x.
    for (int i = 1; i <= 180; ++i) {
        wall.set(Transform{.position = {static_cast<double>(i) / 60.0, 1.0, 0.0}});
        scene.ticks(1);
    }
    CHECK(where(crate).x > crate_x + 0.4);
    // A step's motion behind what it was told, at most, and nothing pushes it.
    QueryHit hit;
    REQUIRE(scene.physics.ray_cast({-5.0, 1.0, 0.0}, {1.0f, 0.0f, 0.0f}, 20.0f, hit));
    CHECK(hit.entity == wall.id());
    CHECK(hit.position.x == doctest::Approx(3.0 - 0.25).epsilon(0.01));
}

TEST_CASE("a ray finds the nearest thing, says what and where, and can leave one out") {
    Scene scene;
    const flecs::entity near_box = scene.box({0.0, 0.0, 10.0}, {1.0f, 1.0f, 1.0f});
    const flecs::entity far_box = scene.box({0.0, 0.0, 20.0}, {1.0f, 1.0f, 1.0f});
    scene.ticks(1);

    QueryHit hit;
    REQUIRE(scene.physics.ray_cast({0.0, 0.0, 0.0}, {0.0f, 0.0f, 1.0f}, 100.0f, hit));
    CHECK(hit.entity == near_box.id());
    CHECK(hit.distance_m == doctest::Approx(9.0f));
    CHECK(hit.position.z == doctest::Approx(9.0));
    CHECK(hit.normal.z == doctest::Approx(-1.0f));

    REQUIRE(scene.physics.ray_cast({0.0, 0.0, 0.0}, {0.0f, 0.0f, 1.0f}, 100.0f, hit, {.ignore = near_box.id()}));
    CHECK(hit.entity == far_box.id());
    CHECK(hit.distance_m == doctest::Approx(19.0f));

    CHECK_FALSE(scene.physics.ray_cast({0.0, 0.0, 0.0}, {0.0f, 0.0f, 1.0f}, 5.0f, hit));
    CHECK_FALSE(scene.physics.ray_cast({0.0, 0.0, 0.0}, {0.0f, 1.0f, 0.0f}, 100.0f, hit));
}

TEST_CASE("a sphere cast stops a radius short of what it hits") {
    Scene scene;
    const flecs::entity wall = scene.box({0.0, 0.0, 10.0}, {1.0f, 1.0f, 1.0f});
    scene.ticks(1);
    QueryHit hit;
    REQUIRE(scene.physics.sphere_cast({0.0, 0.0, 0.0}, 0.5f, {0.0f, 0.0f, 1.0f}, 100.0f, hit));
    CHECK(hit.entity == wall.id());
    CHECK(hit.distance_m == doctest::Approx(8.5f).epsilon(0.001));
    CHECK(hit.position.z == doctest::Approx(9.0).epsilon(0.001));
    CHECK(hit.normal.z == doctest::Approx(-1.0f).epsilon(0.001));
    // It misses a ray's width beside the wall, but a sphere that wide doesn't.
    CHECK_FALSE(scene.physics.ray_cast({1.3, 0.0, 0.0}, {0.0f, 0.0f, 1.0f}, 100.0f, hit));
    CHECK(scene.physics.sphere_cast({1.3, 0.0, 0.0}, 0.5f, {0.0f, 0.0f, 1.0f}, 100.0f, hit));
    CHECK_FALSE(scene.physics.sphere_cast({0.0, 0.0, 0.0}, 0.5f, {0.0f, 0.0f, 1.0f}, 5.0f, hit));
}

TEST_CASE("a body goes with its entity, or with its collider or rigid body") {
    Scene scene;
    const flecs::entity first = scene.box({0.0, 0.0, 10.0}, {1.0f, 1.0f, 1.0f});
    const flecs::entity second = scene.box({0.0, 0.0, 20.0}, {1.0f, 1.0f, 1.0f});
    const flecs::entity third = scene.box({0.0, 0.0, 30.0}, {1.0f, 1.0f, 1.0f});
    scene.ticks(1);
    REQUIRE(scene.physics.stats().bodies == 3);

    QueryHit hit;
    first.destruct();
    scene.ticks(1);
    CHECK(scene.physics.stats().bodies == 2);
    REQUIRE(scene.physics.ray_cast({0.0, 0.0, 0.0}, {0.0f, 0.0f, 1.0f}, 100.0f, hit));
    CHECK(hit.entity == second.id());

    second.remove<Collider>();
    scene.ticks(1);
    CHECK(scene.physics.stats().bodies == 1);

    third.remove<RigidBody>();
    scene.ticks(1);
    CHECK(scene.physics.stats().bodies == 0);
    CHECK_FALSE(scene.physics.ray_cast({0.0, 0.0, 0.0}, {0.0f, 0.0f, 1.0f}, 100.0f, hit));

    // Given back, it's there again.
    third.set(RigidBody{});
    scene.ticks(1);
    CHECK(scene.physics.stats().bodies == 1);
}

TEST_CASE("an entity's scale reaches its shape") {
    Scene scene;
    const flecs::entity wall = scene.ecs().entity();
    wall.set(Collider{.shape = ShapeKind::Box, .half_extents_m = {1.0f, 1.0f, 1.0f}});
    wall.set(RigidBody{});
    de::world::place(wall, Transform{.position = {0.0, 0.0, 10.0}, .scale = {1.0f, 1.0f, 3.0f}});
    scene.ticks(1);
    QueryHit hit;
    REQUIRE(scene.physics.ray_cast({0.0, 0.0, 0.0}, {0.0f, 0.0f, 1.0f}, 100.0f, hit));
    CHECK(hit.distance_m == doctest::Approx(7.0f));
}

TEST_CASE("physics is precise far from the world's zero, where floats wouldn't be") {
    // 10,000 km out, the design doc's own test of its doubles: a float is about a meter apart there, and this is not on
    // a whole meter, so anything going through one lands visibly elsewhere.
    const DVec3 far{10'000'000.25, 500.25, -7'000'000.75};
    Scene scene;
    scene.box(far + DVec3(0.0, -0.5, 0.0), {50.0f, 0.5f, 50.0f});
    const flecs::entity ball = scene.sphere(far + DVec3(0.0, 3.0, 0.0));
    const flecs::entity walker = scene.character(far + DVec3(5.0, 0.0, 0.0));
    scene.ticks(300);
    CHECK(where(ball).y - far.y == doctest::Approx(0.48).epsilon(0.01));
    CHECK(std::abs(where(ball).x - far.x) < 1e-3);
    CHECK(std::abs(where(ball).z - far.z) < 1e-3);

    QueryHit hit;
    REQUIRE(scene.physics.ray_cast(far + DVec3(1.0, 10.0, 1.0), {0.0f, -1.0f, 0.0f}, 100.0f, hit));
    CHECK(hit.distance_m == doctest::Approx(10.0f).epsilon(0.0001));
    CHECK(std::abs(hit.position.y - far.y) < 1e-3);
    CHECK(std::abs(hit.position.x - (far.x + 1.0)) < 1e-3);
    REQUIRE(scene.physics.sphere_cast(far + DVec3(1.0, 10.0, 1.0), 0.5f, {0.0f, -1.0f, 0.0f}, 100.0f, hit));
    CHECK(hit.distance_m == doctest::Approx(9.5f).epsilon(0.001));
    CHECK(std::abs(hit.position.y - far.y) < 1e-2);
    CHECK(std::abs(hit.position.x - (far.x + 1.0)) < 1e-2);

    CHECK(controller(walker).grounded);
    CHECK(std::abs(where(walker).y - far.y) < 0.05);
}

namespace {

// A stack of crates and a rain of balls: what's sensitive to the order things happen in. The bits of where it all ended
// up.
uint64_t pile_result(de::core::JobSystem* jobs) {
    Scene scene(jobs);
    scene.box({0.0, -0.5, 0.0}, {30.0f, 0.5f, 30.0f});
    de::core::Array<flecs::entity> moving{de::core::heap_allocator(de::core::MemoryTag::Physics)};
    for (int layer = 0; layer < 6; ++layer) {
        for (int x = 0; x < 5; ++x) {
            moving.push(scene.box({static_cast<double>(x) * 1.02, 0.5 + static_cast<double>(layer) * 1.02,
                                   static_cast<double>(layer % 2) * 0.1},
                                  {0.5f, 0.5f, 0.5f}, BodyKind::Dynamic));
        }
    }
    for (int i = 0; i < 40; ++i) {
        moving.push(scene.sphere({2.0 + std::sin(static_cast<double>(i)) * 3.0, 10.0 + static_cast<double>(i) * 0.7,
                                  std::cos(static_cast<double>(i)) * 3.0},
                                 0.4f));
    }
    moving.push(scene.character({-5.0, 0.0, 0.0}));
    moving[moving.size() - 1].get_mut<CharacterController>().desired_velocity_mps = {3.0f, 0.0f, 0.0f};
    scene.ticks(240);
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (const flecs::entity entity : moving) {
        const auto& transform = entity.get<Transform>();
        for (int axis = 0; axis < 3; ++axis) {
            hash = (hash ^ std::bit_cast<uint64_t>(transform.position[axis])) * 0x100000001b3ULL;
        }
        for (int axis = 0; axis < 4; ++axis) {
            hash = (hash ^ std::bit_cast<uint32_t>(transform.rotation[axis])) * 0x100000001b3ULL;
        }
    }
    return hash;
}

} // namespace

TEST_CASE("an entity destroyed in the tick it would get its body has none left behind") {
    Scene scene;
    const flecs::entity doomed = scene.box({0.0, 0.0, 10.0}, {1.0f, 1.0f, 1.0f});
    const flecs::entity doomed_walker = scene.character({0.0, 0.0, 20.0});
    // Before the Physics phase, in the same tick, with the delete still waiting to happen when it's the deferred kind.
    scene.ecs().system("destroys them").kind(scene.world.phase(de::world::Phase::PreSim)).run([&](flecs::iter& it) {
        while (it.next()) {
        }
        if (it.world().is_alive(doomed)) {
            doomed.mut(it).destruct();
            doomed_walker.mut(it).destruct();
        }
    });
    scene.ticks(2);
    CHECK(scene.physics.stats().bodies == 0);
    CHECK(scene.physics.stats().characters == 0);
    QueryHit hit;
    CHECK_FALSE(scene.physics.ray_cast({0.0, 0.0, 0.0}, {0.0f, 0.0f, 1.0f}, 100.0f, hit));
}

TEST_CASE("a body made again as another kind behaves as that kind") {
    Scene scene;
    scene.box({0.0, -0.5, 0.0}, {50.0f, 0.5f, 50.0f});
    // A platform breaks loose: it falls from then on, and its Transform doesn't send it back.
    const flecs::entity platform = scene.box({0.0, 10.0, 0.0}, {1.0f, 0.2f, 1.0f}, BodyKind::Kinematic);
    scene.ticks(5);
    platform.remove<RigidBody>();
    scene.ticks(1);
    platform.set(RigidBody{.kind = BodyKind::Dynamic});
    scene.ticks(60);
    CHECK(where(platform).y < 7.0);

    // A dynamic body settled as static stays there, place() or not.
    const flecs::entity crate = scene.box({5.0, 0.5, 0.0}, {0.5f, 0.5f, 0.5f}, BodyKind::Dynamic);
    scene.ticks(120);
    const DVec3 settled = where(crate);
    crate.remove<RigidBody>();
    scene.ticks(1);
    crate.set(RigidBody{.kind = BodyKind::Static});
    scene.ticks(1);
    CHECK(scene.physics.stats().active_bodies <= 1);
    de::world::place(crate, Transform{.position = settled + DVec3(20.0, 0.0, 0.0)});
    scene.ticks(2);
    QueryHit hit;
    REQUIRE(scene.physics.ray_cast(settled + DVec3(0.0, 5.0, 0.0), {0.0f, -1.0f, 0.0f}, 10.0f, hit));
    CHECK(hit.entity == crate.id());

    // A dynamic body made kinematic follows its Transform, and doesn't write it.
    const flecs::entity lift = scene.box({-5.0, 0.5, 0.0}, {0.5f, 0.5f, 0.5f}, BodyKind::Dynamic);
    scene.ticks(120);
    lift.remove<RigidBody>();
    scene.ticks(1);
    lift.set(RigidBody{.kind = BodyKind::Kinematic});
    lift.set(Transform{.position = {-5.0, 8.0, 0.0}});
    scene.ticks(5);
    CHECK(where(lift).y == 8.0);
}

TEST_CASE("an entity given only a Transform has its body where and as big as that says") {
    Scene scene;
    const flecs::entity box = scene.ecs().entity();
    box.set(Collider{.shape = ShapeKind::Box, .half_extents_m = {1.0f, 1.0f, 1.0f}});
    box.set(RigidBody{});
    box.set(Transform{.position = {100.0, 0.0, 0.0}, .scale = {2.0f, 2.0f, 2.0f}});
    const flecs::entity walker = scene.ecs().entity();
    walker.set(CharacterController{});
    walker.set(Transform{.position = {100.0, 2.0, 0.0}});
    scene.ticks(1);
    QueryHit hit;
    REQUIRE(scene.physics.ray_cast({100.0, 10.0, 0.0}, {0.0f, -1.0f, 0.0f}, 20.0f, hit));
    CHECK(hit.entity == box.id());
    CHECK(hit.distance_m == doctest::Approx(8.0f));
    CHECK_FALSE(scene.physics.ray_cast({0.0, 10.0, 0.0}, {0.0f, -1.0f, 0.0f}, 20.0f, hit));
    // It's standing on that, nowhere near the world's zero.
    CHECK(where(walker).x == doctest::Approx(100.0));
    scene.ticks(30);
    CHECK(where(walker).y == doctest::Approx(2.0).epsilon(0.05));
    CHECK(controller(walker).grounded);
}

TEST_CASE("a body's Transform ends where physics has it, through the step it falls asleep in") {
    Scene scene;
    scene.box({0.0, -0.5, 0.0}, {50.0f, 0.5f, 50.0f});
    const flecs::entity ball = scene.sphere({1.25, 2.0, -0.75});
    scene.ticks(300);
    REQUIRE(scene.physics.stats().active_bodies == 0);
    QueryHit hit;
    REQUIRE(scene.physics.ray_cast(where(ball) + DVec3(0.0, 2.0, 0.0), {0.0f, -1.0f, 0.0f}, 3.0f, hit));
    REQUIRE(hit.entity == ball.id());
    // Its top, half a meter above its center, to well under what a last step of settling moves it.
    CHECK(std::abs(hit.position.y - (where(ball).y + 0.5)) < 1e-6);
}

TEST_CASE("a few bodies awake among many asleep cost what the few do, and a place() still reaches the rest") {
    de::core::JobSystem jobs({.worker_count = 2});
    Scene scene(&jobs);
    scene.box({0.0, -0.5, 0.0}, {500.0f, 0.5f, 500.0f});
    const auto ball_at = [&](const DVec3& position, bool asleep) {
        const flecs::entity entity = scene.ecs().entity();
        entity.set(Collider{.shape = ShapeKind::Sphere, .radius_m = 0.4f});
        entity.set(RigidBody{.kind = BodyKind::Dynamic, .start_asleep = asleep});
        de::world::place(entity, Transform{.position = position});
        return entity;
    };
    constexpr int kAsleep = 3000;
    de::core::Array<flecs::entity> sleepers{de::core::heap_allocator(de::core::MemoryTag::Physics)};
    for (int i = 0; i < kAsleep; ++i) {
        const int row = i / 60;
        sleepers.push(ball_at({static_cast<double>(i % 60) * 2.0, 0.4, 20.0 + (static_cast<double>(row) * 2.0)}, true));
    }
    const flecs::entity falling[3] = {ball_at({-20.0, 300.0, 0.0}, false), ball_at({-24.0, 300.0, 0.0}, false),
                                      ball_at({-28.0, 300.0, 0.0}, false)};
    scene.ticks(2); // bodies, then the first read of the tables they're in
    const DVec3 sleeper_was = where(sleepers[100]);
    const double falling_was = where(falling[0]).y;
    scene.ticks(1);
    CHECK(scene.physics.stats().transforms_written == 3);
    CHECK(scene.physics.stats().transforms_compared == 0); // its own writes don't look like a place()
    scene.ticks(1);
    CHECK(scene.physics.stats().transforms_written == 3);
    CHECK(scene.physics.stats().transforms_compared == 0);
    CHECK(where(falling[0]).y < falling_was);
    CHECK(where(sleepers[100]) == sleeper_was);

    // A place() on one: its table is looked through once, it goes there, and that doesn't stay the cost.
    de::world::place(sleepers[100], Transform{.position = sleeper_was + DVec3(0.0, 50.0, 0.0)});
    scene.ticks(1);
    CHECK(scene.physics.stats().transforms_compared == kAsleep + 3);
    CHECK(where(sleepers[100]).y > sleeper_was.y + 49.0);
    CHECK(scene.physics.stats().transforms_written == 4);
    scene.ticks(1);
    CHECK(scene.physics.stats().transforms_compared == 0);
    CHECK(where(sleepers[200]).y == doctest::Approx(0.4));
}

TEST_CASE("debris lands on the world, and passes through other moving things and queries") {
    Scene scene;
    const flecs::entity floor = scene.box({0.0, -0.5, 0.0}, {50.0f, 0.5f, 50.0f});
    const auto debris_at = [&](const DVec3& position) {
        const flecs::entity entity = scene.ecs().entity();
        entity.set(Collider{.shape = ShapeKind::Sphere, .radius_m = 0.5f});
        entity.set(RigidBody{.kind = BodyKind::Dynamic, .debris = true});
        de::world::place(entity, Transform{.position = position});
        return entity;
    };
    // Two in one place, and a solid ball that falls through them, as does a character.
    const flecs::entity lower = debris_at({0.0, 3.0, 0.0});
    const flecs::entity upper = debris_at({0.0, 3.2, 0.0});
    const flecs::entity solid = scene.sphere({5.0, 3.0, 0.0});
    const flecs::entity rubble = debris_at({5.0, 0.5, 0.0});
    const flecs::entity walker = scene.character({-3.0, 0.0, 0.0});
    const flecs::entity in_the_way = debris_at({-1.0, 0.5, 0.0});
    walker.get_mut<CharacterController>().desired_velocity_mps = {2.0f, 0.0f, 0.0f};
    scene.ticks(180);
    // Both on the floor: they didn't push each other up or apart.
    CHECK(where(lower).y == doctest::Approx(0.48).epsilon(0.02));
    CHECK(where(upper).y == doctest::Approx(0.48).epsilon(0.02));
    CHECK(std::abs(where(lower).x) < 0.01);
    CHECK(where(solid).y == doctest::Approx(0.48).epsilon(0.02));
    CHECK(where(rubble).x == doctest::Approx(5.0).epsilon(0.01));
    CHECK(where(walker).x > 2.0);
    CHECK(where(in_the_way).x == doctest::Approx(-1.0).epsilon(0.01));

    // Rays go through it, unless they ask.
    QueryHit hit;
    REQUIRE(scene.physics.ray_cast({0.0, 5.0, 0.0}, {0.0f, -1.0f, 0.0f}, 10.0f, hit));
    CHECK(hit.entity == floor.id());
    REQUIRE(scene.physics.ray_cast({0.0, 5.0, 0.0}, {0.0f, -1.0f, 0.0f}, 10.0f, hit, {.debris = true}));
    CHECK((hit.entity == lower.id() || hit.entity == upper.id()));
    REQUIRE(scene.physics.sphere_cast({0.0, 5.0, 0.0}, 0.1f, {0.0f, -1.0f, 0.0f}, 10.0f, hit));
    CHECK(hit.entity == floor.id());
    REQUIRE(scene.physics.sphere_cast({0.0, 5.0, 0.0}, 0.1f, {0.0f, -1.0f, 0.0f}, 10.0f, hit, {.debris = true}));
    CHECK((hit.entity == lower.id() || hit.entity == upper.id()));
}

TEST_CASE("a body can start asleep, until something touches it") {
    Scene scene;
    const flecs::entity floor =
        scene.box({0.0, -0.5, 0.0}, {5000.0f, 0.5f, 5000.0f}); // big enough for a tree of its own
    const flecs::entity waiting = scene.ecs().entity();
    waiting.set(Collider{.shape = ShapeKind::Sphere, .radius_m = 0.5f});
    waiting.set(RigidBody{.kind = BodyKind::Dynamic, .start_asleep = true});
    de::world::place(waiting, Transform{.position = {0.0, 3.0, 0.0}});
    scene.ticks(60);
    CHECK(scene.physics.stats().active_bodies == 0);
    CHECK(where(waiting).y == 3.0);
    // Something lands on it, and it falls too, onto the floor that's kept apart.
    const flecs::entity falling = scene.sphere({0.0, 5.0, 0.0});
    scene.ticks(300);
    CHECK(where(waiting).y < 1.0);
    CHECK(where(waiting).y > 0.0); // on the floor, not through it
    CHECK(where(falling).y < 2.0);
    QueryHit hit;
    REQUIRE(scene.physics.ray_cast({100.0, 5.0, 100.0}, {0.0f, -1.0f, 0.0f}, 10.0f, hit));
    CHECK(hit.entity == floor.id());
    CHECK(hit.distance_m == doctest::Approx(5.0f));
}

TEST_CASE("bodies of one shape and size share it, and a great many arrive at once") {
    Scene scene;
    constexpr int kBodies = 5000;
    for (int i = 0; i < kBodies; ++i) {
        const int row = i / 100;
        scene.box({static_cast<double>(i % 100) * 3.0, 0.0, static_cast<double>(row) * 3.0}, {0.5f, 0.5f, 0.5f});
    }
    const flecs::entity last = scene.box({500.0, 0.0, 0.0}, {2.0f, 0.5f, 0.5f});
    scene.sphere({1000.0, 0.0, 0.0});
    scene.sphere({1010.0, 0.0, 0.0});
    scene.ticks(1);
    CHECK(scene.physics.stats().bodies == kBodies + 3);
    CHECK(scene.physics.stats().shapes == 3);
    // All there to be found at once, whichever batch they were in.
    QueryHit hit;
    REQUIRE(scene.physics.ray_cast({297.0, 5.0, 147.0}, {0.0f, -1.0f, 0.0f}, 10.0f, hit));
    CHECK(hit.distance_m == doctest::Approx(4.5f));
    REQUIRE(scene.physics.ray_cast({500.0, 5.0, 0.0}, {0.0f, -1.0f, 0.0f}, 10.0f, hit));
    CHECK(hit.entity == last.id());
}

TEST_CASE("when all's asleep, nothing reads as having moved") {
    Scene scene;
    scene.box({0.0, -0.5, 0.0}, {50.0f, 0.5f, 50.0f});
    const flecs::entity ball = scene.sphere({0.0, 0.6, 0.0});
    scene.ticks(120);
    REQUIRE(scene.physics.stats().active_bodies == 0);
    // What reads transforms in the world's way finds no table changed: it's cheap for them all to stay asleep.
    const flecs::query<const Transform> readers =
        scene.ecs().query_builder<const Transform>().with<RigidBody>().cached().detect_changes().build();
    // Asking is what catches this query up with a table.
    readers.run([](flecs::iter& it) {
        while (it.next()) {
            static_cast<void>(it.changed());
        }
    });
    scene.ticks(5);
    bool changed = false;
    readers.run([&changed](flecs::iter& it) {
        while (it.next()) {
            changed = it.changed() || changed;
        }
    });
    CHECK_FALSE(changed);
    // A place() still reaches it.
    de::world::place(ball, Transform{.position = {3.0, 0.6, 0.0}});
    scene.ticks(1);
    CHECK(where(ball).x == doctest::Approx(3.0).epsilon(0.01));
}

TEST_CASE("the same world steps to the same bits, however many threads it has") {
    const uint64_t single_thread = pile_result(nullptr);
    CHECK(pile_result(nullptr) == single_thread);
    {
        de::core::JobSystem jobs({.worker_count = 1});
        CHECK(pile_result(&jobs) == single_thread);
    }
    {
        de::core::JobSystem jobs({.worker_count = 6});
        CHECK(pile_result(&jobs) == single_thread);
        CHECK(pile_result(&jobs) == single_thread);
    }
}

TEST_CASE("physics steps on the engine's memory, with only Jolt's one small block a tick while bodies move") {
    const uint64_t live_before = de::core::memory_stats(de::core::MemoryTag::Physics).live_bytes;
    {
        de::core::JobSystem jobs({.worker_count = 2});
        Scene scene(&jobs);
        scene.box({0.0, -0.5, 0.0}, {50.0f, 0.5f, 50.0f});
        de::core::Array<flecs::entity> balls{de::core::heap_allocator(de::core::MemoryTag::Physics)};
        for (int i = 0; i < 20; ++i) {
            balls.push(scene.sphere({static_cast<double>(i) * 1.5, 3.0, 0.0}));
        }
        const flecs::entity walker = scene.character({0.0, 0.0, 10.0});
        walker.get_mut<CharacterController>().desired_velocity_mps = {1.0f, 0.0f, 0.0f};
        scene.ticks(1);
        CHECK(de::core::memory_stats(de::core::MemoryTag::Physics).live_bytes > live_before);

        // The same fall and landing, over and over.
        const auto drop = [&] {
            for (size_t i = 0; i < balls.size(); ++i) {
                de::world::place(balls[i], Transform{.position = {static_cast<double>(i) * 1.5, 3.0, 0.0}});
            }
            const uint64_t before = de::core::memory_stats(de::core::MemoryTag::Physics).total_allocations;
            scene.ticks(150);
            return de::core::memory_stats(de::core::MemoryTag::Physics).total_allocations - before;
        };
        // Its broad phase takes a scratch block for each tree it rebuilds, whatever's in it. Nothing else should.
        static_cast<void>(drop());
        CHECK(drop() <= 2 * 150);
    }
    // All of it back, world and Physics gone.
    CHECK(de::core::memory_stats(de::core::MemoryTag::Physics).live_bytes == live_before);
}

TEST_CASE("static bodies and walking characters allocate nothing per tick") {
    de::core::JobSystem jobs({.worker_count = 2});
    Scene scene(&jobs);
    scene.box({0.0, -0.5, 0.0}, {500.0f, 0.5f, 500.0f});
    for (int i = 0; i < 100; ++i) {
        scene.box({static_cast<double>(i) * 3.0, 0.5, 40.0}, {0.5f, 0.5f, 0.5f});
    }
    const flecs::entity walkers[] = {scene.character({0.0, 0.0, 0.0}), scene.character({0.0, 0.0, 5.0}),
                                     scene.character({0.0, 0.0, 10.0})};
    for (const flecs::entity walker : walkers) {
        walker.get_mut<CharacterController>().desired_velocity_mps = {2.0f, 0.0f, 0.0f};
    }
    scene.ticks(10);
    const uint64_t before = de::core::memory_stats(de::core::MemoryTag::Physics).total_allocations;
    scene.ticks(120);
    CHECK(de::core::memory_stats(de::core::MemoryTag::Physics).total_allocations == before);
}

TEST_CASE("a character falls to the ground, stands on it, and walks where it's asked") {
    Scene scene;
    scene.box({0.0, -0.5, 0.0}, {50.0f, 0.5f, 50.0f});
    const flecs::entity walker = scene.character({0.0, 3.0, 0.0});
    scene.ticks(10);
    CHECK_FALSE(controller(walker).grounded);
    CHECK(where(walker).y < 3.0);
    scene.ticks(120);
    CHECK(controller(walker).grounded);
    CHECK(std::abs(where(walker).y) < 0.05);
    CHECK(controller(walker).ground_normal.y == doctest::Approx(1.0f));
    CHECK(scene.physics.stats().characters == 1);

    // Four meters a second for a second: four meters, along x, at the same height.
    walker.get_mut<CharacterController>().desired_velocity_mps = {4.0f, 0.0f, 0.0f};
    scene.ticks(60);
    CHECK(where(walker).x == doctest::Approx(4.0).epsilon(0.03));
    CHECK(std::abs(where(walker).y) < 0.05);
    CHECK(controller(walker).grounded);
    walker.get_mut<CharacterController>().desired_velocity_mps = {0.0f, 0.0f, 0.0f};
    scene.ticks(2);
    const DVec3 stopped = where(walker);
    scene.ticks(30);
    CHECK(where(walker).x == doctest::Approx(stopped.x).epsilon(0.001));

    // place() sends it there.
    de::world::place(walker, Transform{.position = {-10.0, 0.0, 5.0}});
    scene.ticks(1);
    CHECK(where(walker).x == doctest::Approx(-10.0));
    CHECK(where(walker).z == doctest::Approx(5.0));

    walker.destruct();
    scene.ticks(1);
    CHECK(scene.physics.stats().characters == 0);
}

TEST_CASE("a wall stops a character, a step doesn't, and a slope too steep doesn't let it up") {
    Scene scene;
    scene.box({0.0, -0.5, 0.0}, {50.0f, 0.5f, 50.0f});
    // A wall ahead along +x.
    scene.box({3.0, 2.0, 0.0}, {0.5f, 2.0f, 10.0f});
    const flecs::entity blocked = scene.character({0.0, 0.0, 0.0});
    blocked.get_mut<CharacterController>().desired_velocity_mps = {3.0f, 0.0f, 0.0f};
    // A step ahead along -x, 0.3 m up, lower than a character's step.
    scene.box({-3.0, 0.15, 20.0}, {0.5f, 0.15f, 3.0f});
    const flecs::entity climbing = scene.character({0.0, 0.0, 20.0});
    climbing.get_mut<CharacterController>().desired_velocity_mps = {-1.2f, 0.0f, 0.0f};
    // One 1 m up, higher than that.
    scene.box({-3.0, 0.5, 30.0}, {0.5f, 0.5f, 3.0f});
    const flecs::entity stopped = scene.character({0.0, 0.0, 30.0});
    stopped.get_mut<CharacterController>().desired_velocity_mps = {-1.2f, 0.0f, 0.0f};
    // A 70 degree slope ahead along +x.
    const flecs::entity slope = scene.ecs().entity();
    slope.set(Collider{.shape = ShapeKind::Box, .half_extents_m = {5.0f, 0.5f, 5.0f}});
    slope.set(RigidBody{});
    const float angle = glm::radians(70.0f);
    de::world::place(
        slope, Transform{.position = {6.0, 2.5, -30.0}, .rotation = glm::angleAxis(angle, Vec3(0.0f, 0.0f, 1.0f))});
    const flecs::entity sliding = scene.character({0.0, 0.0, -30.0});
    sliding.get_mut<CharacterController>().desired_velocity_mps = {3.0f, 0.0f, 0.0f};

    scene.ticks(180);
    CHECK(where(blocked).x < 3.0 - 0.5 - 0.3);
    CHECK(where(blocked).x > 1.5);
    CHECK(where(climbing).x < -2.0);
    CHECK(where(climbing).y == doctest::Approx(0.3).epsilon(0.1));
    CHECK(where(stopped).x > -2.5);
    CHECK(std::abs(where(stopped).y) < 0.05);
    CHECK(where(sliding).y < 0.7);
}

TEST_CASE("a character pushes a dynamic body it walks into") {
    Scene scene;
    scene.box({0.0, -0.5, 0.0}, {50.0f, 0.5f, 50.0f});
    // Light enough for a push of Jolt's default strength to beat its friction.
    const flecs::entity crate = scene.ecs().entity();
    crate.set(Collider{.shape = ShapeKind::Box, .half_extents_m = {0.5f, 0.5f, 0.5f}});
    crate.set(RigidBody{.kind = BodyKind::Dynamic, .mass_kg = 5.0f});
    de::world::place(crate, Transform{.position = {3.0, 0.5, 0.0}});
    const flecs::entity walker = scene.character({0.0, 0.0, 0.0});
    walker.get_mut<CharacterController>().desired_velocity_mps = {2.0f, 0.0f, 0.0f};
    scene.ticks(180);
    CHECK(where(crate).x > 3.2);
}

namespace {

double median_step_us(Scene& scene, int ticks) {
    de::core::Array<double> steps{de::core::heap_allocator(de::core::MemoryTag::Physics)};
    for (int i = 0; i < ticks; ++i) {
        const uint64_t start = de::platform::time_ns();
        scene.ticks(1);
        steps.push(static_cast<double>(de::platform::time_ns() - start) / 1000.0);
    }
    std::ranges::sort(steps);
    return steps[steps.size() / 2];
}

} // namespace

// Not a check: numbers to read. Small unless DE_PHYSICS_BENCH is set, and only worth reading from a dev or ship build.
TEST_CASE("benchmark: physics at scale") {
    const bool big = std::getenv("DE_PHYSICS_BENCH") != nullptr;
    const int statics = big ? 100000 : 1000;
    const int dynamics = big ? 10000 : 100;
    const int ticks = big ? 120 : 5;
    const auto grid = [](int i, double spacing) {
        const int row = i / 300;
        return DVec3((i % 300) * spacing, 0.0, row * spacing);
    };
    for (const uint32_t workers : {7U, 0U}) {
        de::core::JobSystem jobs({.worker_count = workers});
        PhysicsDesc desc;
        if (big) {
            desc.max_body_pairs = 1024U * 1024U;
            desc.max_contact_constraints = 512U * 1024U;
        }
        const auto report = [&](const char* name, Scene& scene, double step_us) {
            const de::physics::PhysicsStats stats = scene.physics.stats();
            MESSAGE(workers, " workers, ", doctest::String(name), ": ", step_us, " us a tick, ", stats.bodies,
                    " bodies, ", stats.active_bodies, " awake at the end");
        };
        {
            Scene scene(&jobs, desc);
            uint64_t start = de::platform::time_ns();
            for (int i = 0; i < statics; ++i) {
                scene.box(grid(i, 2.0), {0.5f, 0.5f, 0.5f});
            }
            const double spawned_ms = static_cast<double>(de::platform::time_ns() - start) / 1e6;
            start = de::platform::time_ns();
            scene.ticks(1);
            MESSAGE(workers, " workers, static bodies: ", spawned_ms, " ms to spawn, ",
                    static_cast<double>(de::platform::time_ns() - start) / 1e6, " ms to add");
            report("static only", scene, median_step_us(scene, ticks));
            const double middle = 280.0;
            for (int i = 0; i < dynamics; ++i) {
                const int level = i / 10000;
                scene.sphere({middle + (static_cast<double>(i % 100) * 1.1) - 55.0,
                              5.0 + (static_cast<double>(level) * 1.1),
                              middle + (static_cast<double>((i / 100) % 100) * 1.1) - 55.0},
                             0.5f);
            }
            scene.ticks(1);
            report("spheres falling onto them", scene, median_step_us(scene, ticks));
            report("spheres in a heap", scene, median_step_us(scene, ticks * 2));
            // Among what they might collide with, not in an empty world, with the heap settling nearby.
            const int walkers = big ? 200 : 10;
            for (int i = 0; i < walkers; ++i) {
                const flecs::entity walker = scene.character({static_cast<double>(i) * 2.0, 3.0, 2.0});
                walker.get_mut<CharacterController>().desired_velocity_mps = {0.0f, 0.0f, 1.0f};
            }
            scene.ticks(1);
            report("with 200 characters too", scene, median_step_us(scene, ticks));
        }
        {
            Scene scene(&jobs, desc);
            scene.box({0.0, -0.5, 0.0}, {2000.0f, 0.5f, 2000.0f});
            const int walkers = big ? 200 : 10;
            for (int i = 0; i < walkers; ++i) {
                const flecs::entity walker = scene.character({static_cast<double>(i) * 2.0, 0.0, 0.0});
                walker.get_mut<CharacterController>().desired_velocity_mps = {0.0f, 0.0f, 1.0f};
            }
            scene.ticks(1);
            report("200 characters alone", scene, median_step_us(scene, ticks));
        }
        {
            Scene scene(&jobs, desc);
            scene.box({0.0, -0.5, 0.0}, {2000.0f, 0.5f, 2000.0f});
            for (int i = 0; i < dynamics * 2; ++i) {
                const int level = i / 100;
                const flecs::entity entity = scene.ecs().entity();
                entity.set(Collider{.shape = ShapeKind::Box, .half_extents_m = {0.3f, 0.3f, 0.3f}});
                entity.set(RigidBody{.kind = BodyKind::Dynamic, .debris = true});
                de::world::place(entity, Transform{.position = {static_cast<double>(i % 100) * 0.2,
                                                                2.0 + (static_cast<double>(level) * 0.2), 0.0}});
            }
            scene.ticks(1);
            report("debris falling", scene, median_step_us(scene, ticks));
            scene.ticks(600);
            report("debris, long after", scene, median_step_us(scene, ticks));
        }
        {
            // What a streamed world mostly is: a great many settled, and a few moving, in one table.
            Scene scene(&jobs, desc);
            scene.box({0.0, -0.5, 0.0}, {2000.0f, 0.5f, 2000.0f});
            for (int i = 0; i < statics; ++i) {
                const flecs::entity entity = scene.ecs().entity();
                entity.set(Collider{.shape = ShapeKind::Sphere, .radius_m = 0.4f});
                entity.set(RigidBody{.kind = BodyKind::Dynamic, .start_asleep = true});
                de::world::place(entity, Transform{.position = grid(i, 2.0) + DVec3(-300.0, 0.4, -100.0)});
            }
            scene.ticks(1);
            report("asleep, none awake", scene, median_step_us(scene, ticks));
            for (int i = 0; i < 100; ++i) {
                const flecs::entity entity = scene.ecs().entity();
                entity.set(Collider{.shape = ShapeKind::Sphere, .radius_m = 0.4f});
                entity.set(RigidBody{.kind = BodyKind::Dynamic});
                de::world::place(entity, Transform{.position = {-400.0 - (static_cast<double>(i) * 2.0), 2000.0, 0.0}});
            }
            scene.ticks(1);
            report("asleep, and 100 falling", scene, median_step_us(scene, ticks));
        }
    }
}
