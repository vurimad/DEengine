#include "asset/collision_format.h"
#include "asset/mesh_format.h"
#include "asset/registry.h"
#include "cooker/collision_cooker.h"
#include "cooker/cooker.h"
#include "core/allocator.h"
#include "core/job_system.h"
#include "core/log.h"
#include "core/memory_stats.h"
#include "physics/physics.h"
#include "platform/file.h"
#include "platform/time.h"
#include "platform/vfs.h"
#include "support/gltf_source.h"
#include "support/sandbox.h"
#include "world/cell.h"
#include "world/render_extract.h"
#include "world/transform.h"
#include "world/world.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using de::DVec3;
using de::Quat;
using de::Vec3;
using de::asset::AssetHandle;
using de::asset::AssetId;
using de::asset::AssetRegistry;
using de::asset::AssetType;
using de::physics::BodyKind;
using de::physics::CharacterController;
using de::physics::Collider;
using de::physics::Physics;
using de::physics::PhysicsDesc;
using de::physics::QueryHit;
using de::physics::RigidBody;
using de::physics::ShapeKind;
using de::test::Sandbox;
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

namespace {

struct InATableOfItsOwn {};

// Counts what physics complains of, instead of printing it.
class PhysicsErrors final : public de::core::LogSink {
public:
    PhysicsErrors() {
        de::core::remove_log_sink(de::core::console_log_sink());
        de::core::add_log_sink(*this);
    }
    ~PhysicsErrors() {
        de::core::remove_log_sink(*this);
        de::core::add_log_sink(de::core::console_log_sink());
    }
    PhysicsErrors(PhysicsErrors&&) = delete;
    PhysicsErrors& operator=(PhysicsErrors&&) = delete;

    void write(const de::core::LogMessage& message) override {
        if (message.level == de::core::LogLevel::Error && std::strcmp(message.channel, "physics") == 0) {
            ++count;
        }
    }
    int count = 0;
};

// Holds a worker until it's let go, which leaves jobs queued that only the thread waiting for them can get to.
struct HeldWorkers {
    std::atomic<uint32_t> held{0};
    std::atomic<bool> let_go{false};
};

void hold_worker(void* context) {
    auto& workers = *static_cast<HeldWorkers*>(context);
    workers.held.fetch_add(1);
    while (!workers.let_go.load()) {
        std::this_thread::yield();
    }
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
    // A slope gentle enough, and a ball damped enough, that it goes to sleep while it's still creeping down.
    const flecs::entity slope = scene.ecs().entity();
    slope.set(Collider{.shape = ShapeKind::Box, .half_extents_m = {50.0f, 0.5f, 50.0f}});
    slope.set(RigidBody{});
    slope.set(Transform{.position = {0.0, -0.5, 0.0}, .rotation = glm::angleAxis(0.02f, Vec3(0.0f, 0.0f, 1.0f))});
    const flecs::entity ball = scene.ecs().entity();
    ball.set(Collider{.shape = ShapeKind::Sphere, .radius_m = 0.5f});
    ball.set(RigidBody{.kind = BodyKind::Dynamic, .linear_damping = 8.0f, .angular_damping = 8.0f});
    ball.set(Transform{.position = {0.0, 0.52, 0.0}});
    double last_step_m = 0.0;
    double last_x = 0.0;
    for (int tick = 0; tick < 600 && (tick == 0 || scene.physics.stats().active_bodies > 0); ++tick) {
        scene.ticks(1);
        const double x = where(ball).x;
        if (x != last_x) {
            last_step_m = std::abs(x - last_x);
        }
        last_x = x;
    }
    REQUIRE(scene.physics.stats().active_bodies == 0);
    REQUIRE(last_step_m > 3e-5); // or this says nothing: a step left out would be too small to see
    // From the side it was going, where a step's move shows in full.
    QueryHit hit;
    REQUIRE(scene.physics.ray_cast(where(ball) - DVec3(5.0, 0.0, 0.0), {1.0f, 0.0f, 0.0f}, 10.0f, hit));
    REQUIRE(hit.entity == ball.id());
    CHECK(std::abs(hit.position.x - (where(ball).x - 0.5)) < 1e-5);
    // Drawn as still going for one more tick, then as where it stopped, or it'd shimmer over its last move for good.
    const auto previous = [&] { return ball.get<de::world::PreviousWorldTransform>().value.position; };
    CHECK(previous() != where(ball));
    scene.ticks(1);
    CHECK(previous() == where(ball));
}

TEST_CASE("what reads transforms sees what physics moved, in every table") {
    Scene scene;
    const flecs::entity plain[2] = {scene.sphere({0.0, 500.0, 0.0}), scene.sphere({4.0, 500.0, 0.0})};
    const flecs::entity marked = scene.sphere({8.0, 500.0, 0.0});
    marked.add<InATableOfItsOwn>();
    scene.ticks(3);
    CHECK(scene.physics.stats().transforms_written == 3);
    for (const flecs::entity entity : {plain[0], plain[1], marked}) {
        const auto* const world = entity.try_get<de::world::WorldTransform>();
        REQUIRE(world != nullptr);
        CHECK(where(entity).y < 500.0);
        CHECK(world->position == where(entity));
    }
}

TEST_CASE("a kinematic body is told where to go, and doesn't write its Transform") {
    Scene scene;
    const flecs::entity lift = scene.box({0.0, 0.0, 0.0}, {1.0f, 1.0f, 1.0f}, BodyKind::Kinematic);
    scene.ticks(1);
    lift.set(Transform{.position = {0.0, 5.0, 0.0}});
    scene.ticks(1);
    CHECK(scene.physics.stats().active_bodies == 1);
    CHECK(scene.physics.stats().transforms_written == 0);
    CHECK(where(lift).y == 5.0);
}

TEST_CASE("a body's rotation reaches its Transform, and a place() can turn it alone") {
    Scene scene;
    scene.box({0.0, -0.5, 0.0}, {50.0f, 0.5f, 50.0f});
    // Let go on a corner, it settles flat, turned from how it began.
    const Quat tilted = glm::angleAxis(0.7f, glm::normalize(Vec3(1.0f, 0.0f, 0.5f)));
    const flecs::entity crate = scene.ecs().entity();
    crate.set(Collider{.shape = ShapeKind::Box, .half_extents_m = {0.5f, 0.5f, 0.5f}});
    crate.set(RigidBody{.kind = BodyKind::Dynamic});
    crate.set(Transform{.position = {0.0, 3.0, 0.0}, .rotation = tilted});
    scene.ticks(300);
    CHECK(std::abs(glm::dot(crate.try_get<Transform>()->rotation, tilted)) < 0.99f);

    // A quarter of a turn on the spot: its corner reaches past where its side was.
    const flecs::entity high = scene.box({10.0, 200.0, 0.0}, {1.0f, 1.0f, 1.0f}, BodyKind::Dynamic);
    scene.ticks(1);
    const Vec3 down(0.0f, -1.0f, 0.0f);
    QueryHit hit;
    CHECK_FALSE(scene.physics.ray_cast(where(high) + DVec3(1.2, 5.0, 0.0), down, 10.0f, hit));
    constexpr float kEighthTurnRad = 0.785398f;
    de::world::place(
        high, Transform{.position = where(high), .rotation = glm::angleAxis(kEighthTurnRad, Vec3(0.0f, 1.0f, 0.0f))});
    scene.ticks(1);
    CHECK(scene.physics.ray_cast(where(high) + DVec3(1.2, 5.0, 0.0), down, 10.0f, hit));
}

TEST_CASE("no job of a step's is left behind when physics goes, with no worker free to run them") {
    constexpr uint32_t kWorkers = 2;
    de::core::JobSystem jobs({.worker_count = kWorkers});
    HeldWorkers workers;
    de::core::JobCounter held;
    for (uint32_t i = 0; i < kWorkers; ++i) {
        jobs.submit(de::core::Job{.entry = &hold_worker, .context = &workers, .counter = &held, .name = "hold"});
    }
    while (workers.held.load() < kWorkers) {
        std::this_thread::yield();
    }
    {
        Scene scene(&jobs);
        scene.box({0.0, -0.5, 0.0}, {50.0f, 0.5f, 50.0f});
        for (int i = 0; i < 50; ++i) {
            scene.sphere({static_cast<double>(i) * 1.5, 2000.0, 0.0});
        }
        // Long enough to run out of jobs if each step left its own behind, which would hang here.
        scene.ticks(400);
        CHECK(scene.physics.stats().active_bodies == 50);
    } // Jolt asserts in Debug if a job is still held by what would run it
    workers.let_go.store(true);
    jobs.wait(held);
}

TEST_CASE("a child's body is made where its parent is, even if neither has had a tick") {
    Scene scene;
    const auto child_of = [&](flecs::entity parent) {
        const flecs::entity child = scene.ecs().entity().child_of(parent);
        child.set(Collider{.shape = ShapeKind::Box, .half_extents_m = {1.0f, 1.0f, 1.0f}});
        child.set(RigidBody{});
        child.set(Transform{.position = {0.0, 5.0, 0.0}});
        return child;
    };
    const auto hit_at_x = [&](double x, flecs::entity expected) {
        QueryHit hit;
        REQUIRE(scene.physics.ray_cast({x, 20.0, 0.0}, {0.0f, -1.0f, 0.0f}, 30.0f, hit));
        CHECK(hit.entity == expected.id());
        CHECK(hit.distance_m == doctest::Approx(14.0f));
    };
    // Both new, with only a Transform on the parent.
    const flecs::entity fresh = scene.ecs().entity();
    fresh.set(Transform{.position = {100.0, 0.0, 0.0}});
    const flecs::entity of_fresh = child_of(fresh);
    scene.ticks(1);
    hit_at_x(100.0, of_fresh);

    // A parent that's been there, moved this tick.
    const flecs::entity moved = scene.ecs().entity();
    de::world::place(moved, Transform{.position = {200.0, 0.0, 0.0}});
    scene.ticks(1);
    moved.set(Transform{.position = {300.0, 0.0, 0.0}});
    const flecs::entity of_moved = child_of(moved);
    scene.ticks(1);
    hit_at_x(300.0, of_moved);

    // Two levels up.
    const flecs::entity top = scene.ecs().entity();
    top.set(Transform{.position = {400.0, 0.0, 0.0}});
    const flecs::entity middle = scene.ecs().entity().child_of(top);
    middle.set(Transform{.position = {50.0, 0.0, 0.0}});
    const flecs::entity of_middle = child_of(middle);
    scene.ticks(1);
    hit_at_x(450.0, of_middle);
}

TEST_CASE("what rests on a body wakes when that body goes or is sent away") {
    const auto stack_on = [](Scene& scene, flecs::entity support) {
        const flecs::entity ball = scene.ecs().entity();
        ball.set(Collider{.shape = ShapeKind::Sphere, .radius_m = 0.5f});
        ball.set(RigidBody{.kind = BodyKind::Dynamic});
        const DVec3 at = where(support);
        de::world::place(ball, Transform{.position = at + DVec3(0.0, 1.02, 0.0)});
        scene.ticks(300);
        REQUIRE(scene.physics.stats().active_bodies == 0);
        REQUIRE(where(ball).y > 1.0);
        return ball;
    };
    constexpr double kOnTheGround = 0.6;
    {
        Scene scene;
        scene.box({0.0, -0.5, 0.0}, {50.0f, 0.5f, 50.0f});
        const flecs::entity crate = scene.box({0.0, 0.5, 0.0}, {0.5f, 0.5f, 0.5f}, BodyKind::Dynamic);
        const flecs::entity ball = stack_on(scene, crate);
        crate.destruct();
        scene.ticks(90);
        CHECK(where(ball).y < kOnTheGround);
    }
    {
        Scene scene;
        scene.box({0.0, -0.5, 0.0}, {50.0f, 0.5f, 50.0f});
        const flecs::entity crate = scene.box({0.0, 0.5, 0.0}, {0.5f, 0.5f, 0.5f}, BodyKind::Dynamic);
        const flecs::entity ball = stack_on(scene, crate);
        de::world::place(crate, Transform{.position = {30.0, 5.0, 0.0}});
        scene.ticks(90);
        CHECK(where(ball).y < kOnTheGround);
    }
    {
        // What a tick replaces: made asleep where something's just gone, it isn't woken for that.
        Scene scene;
        scene.box({0.0, -0.5, 0.0}, {50.0f, 0.5f, 50.0f});
        const flecs::entity old = scene.box({0.0, 0.5, 0.0}, {0.5f, 0.5f, 0.5f});
        scene.ticks(2);
        old.destruct();
        const flecs::entity replacement = scene.ecs().entity();
        replacement.set(Collider{.shape = ShapeKind::Box, .half_extents_m = {0.5f, 0.5f, 0.5f}});
        replacement.set(RigidBody{.kind = BodyKind::Dynamic, .start_asleep = true});
        de::world::place(replacement, Transform{.position = {0.0, 0.5, 0.0}});
        scene.ticks(1);
        CHECK(scene.physics.stats().active_bodies == 0);
    }
    {
        // A static one, with a piece of debris on it.
        Scene scene;
        scene.box({0.0, -0.5, 0.0}, {50.0f, 0.5f, 50.0f});
        const flecs::entity shelf = scene.box({0.0, 2.0, 0.0}, {2.0f, 0.5f, 2.0f});
        const flecs::entity piece = scene.ecs().entity();
        piece.set(Collider{.shape = ShapeKind::Sphere, .radius_m = 0.5f});
        piece.set(RigidBody{.kind = BodyKind::Dynamic, .debris = true});
        de::world::place(piece, Transform{.position = {0.0, 3.02, 0.0}});
        scene.ticks(300);
        REQUIRE(scene.physics.stats().active_bodies == 0);
        shelf.remove<RigidBody>();
        scene.ticks(90);
        CHECK(where(piece).y < kOnTheGround);
    }
}

TEST_CASE("a body made asleep stays asleep, at whatever rotation it was made") {
    Scene scene;
    scene.box({0.0, -0.5, 0.0}, {500.0f, 0.5f, 500.0f});
    for (int i = 0; i < 300; ++i) {
        const flecs::entity entity = scene.ecs().entity();
        entity.set(Collider{.shape = ShapeKind::Box, .half_extents_m = {0.4f, 0.4f, 0.4f}});
        entity.set(RigidBody{.kind = BodyKind::Dynamic, .start_asleep = true});
        const float angle = static_cast<float>(i) * 0.37f;
        // Not quite unit length, as what's been through a file or a lot of multiplying isn't.
        const Quat turned =
            glm::angleAxis(angle, glm::normalize(Vec3(1.0f, static_cast<float>(i % 7), 0.5f))) * 1.000001f;
        entity.set(Transform{.position = {static_cast<double>(i), 10.0, 0.0}, .rotation = turned});
    }
    scene.ticks(3);
    CHECK(scene.physics.stats().active_bodies == 0);
    // A quaternion and its negative are the same turn, so this moves nothing.
    const flecs::entity last = scene.ecs().entity();
    last.set(Collider{.shape = ShapeKind::Box, .half_extents_m = {0.4f, 0.4f, 0.4f}});
    last.set(RigidBody{.kind = BodyKind::Dynamic, .start_asleep = true});
    const Quat turned = glm::angleAxis(1.0f, Vec3(0.0f, 1.0f, 0.0f));
    last.set(Transform{.position = {-5.0, 10.0, 0.0}, .rotation = turned});
    scene.ticks(2);
    last.set(Transform{.position = {-5.0, 10.0, 0.0}, .rotation = -turned});
    scene.ticks(1);
    CHECK(scene.physics.stats().active_bodies == 0);
}

TEST_CASE("many small turns by place() add up") {
    Scene scene;
    const flecs::entity crate = scene.box({0.0, 500.0, 0.0}, {0.5f, 0.5f, 0.5f}, BodyKind::Dynamic);
    scene.ticks(1);
    constexpr float kStepRad = 0.0005f; // under 0.03 of a degree a tick
    constexpr int kSteps = 120;
    const Quat step = glm::angleAxis(kStepRad, Vec3(0.0f, 1.0f, 0.0f));
    for (int i = 0; i < kSteps; ++i) {
        const Quat now = crate.get<Transform>().rotation;
        de::world::place(crate, Transform{.position = where(crate), .rotation = glm::normalize(step * now)});
        scene.ticks(1);
    }
    const Quat ended = crate.get<Transform>().rotation;
    const float angle_rad = 2.0f * std::acos(std::min(std::abs(ended.w), 1.0f));
    CHECK(angle_rad == doctest::Approx(kStepRad * kSteps).epsilon(0.05));
}

TEST_CASE("a character sent away from a turning platform goes where it was sent") {
    Scene scene;
    const flecs::entity platform = scene.box({0.0, -0.5, 0.0}, {20.0f, 0.5f, 20.0f}, BodyKind::Kinematic);
    const flecs::entity walker = scene.character({5.0, 0.05, 0.0});
    Quat turned(1.0f, 0.0f, 0.0f, 0.0f);
    const Quat step = glm::angleAxis(1.0f / 60.0f, Vec3(0.0f, 1.0f, 0.0f)); // a radian a second
    const auto turn_it = [&] {
        turned = glm::normalize(step * turned);
        platform.set(Transform{.position = {0.0, -0.5, 0.0}, .rotation = turned});
    };
    for (int i = 0; i < 30; ++i) {
        turn_it();
        scene.ticks(1);
    }
    REQUIRE(controller(walker).grounded);
    const DVec3 target{2000.0, 50.0, 0.0};
    de::world::place(walker, Transform{.position = target});
    turn_it();
    scene.ticks(1);
    // By what it stood on, at this distance from its axis, it'd be going at 2 km/s.
    CHECK(glm::length(where(walker) - target) < 1.0);
    CHECK_FALSE(controller(walker).grounded);
}

TEST_CASE("several ticks in one frame send what moved in them once, and what stopped in them where it stopped") {
    Scene scene;
    const flecs::entity eye = scene.ecs().entity().set(de::world::Camera{});
    de::world::place(eye, Transform{.position = {0.0, 50.0, 0.0}});
    scene.ecs().set(de::world::ActiveCamera{.entity = eye.id()});
    scene.box({0.0, -0.5, 0.0}, {50.0f, 0.5f, 50.0f});
    const auto ball_at = [&](const DVec3& position) {
        const flecs::entity entity = scene.ecs().entity();
        entity.set(de::world::MeshInstance{});
        entity.set(Collider{.shape = ShapeKind::Sphere, .radius_m = 0.5f});
        entity.set(RigidBody{.kind = BodyKind::Dynamic});
        de::world::place(entity, Transform{.position = position});
        return entity;
    };
    const flecs::entity high = ball_at({20.0, 500.0, 0.0});
    const flecs::entity ball = ball_at({0.0, 2.0, 0.0});
    scene.world.advance(1.0 / 60.0);
    scene.world.advance(1.0 / 60.0);
    // A hitch: three ticks' worth, in which each moved in all three, and each is sent once.
    scene.world.advance(3.0 / 60.0);
    CHECK(scene.world.render_frame().instances.size() == 2);

    // Slow frames all the way to where it stops, so that its last move, and coming to rest, share frames with ticks
    // that moved nothing of its. What the renderer is left with has to be where it is, with no motion in it.
    de::world::RenderTransform last_sent;
    de::world::RenderTransform last_previous;
    const uint32_t slot = ball.get<de::world::RenderSlot>().slot;
    high.destruct();
    for (int frame = 0; frame < 120; ++frame) {
        scene.world.advance(5.0 / 60.0);
        for (const de::world::RenderInstance& instance : scene.world.render_frame().instances) {
            if (instance.slot == slot) {
                last_sent = instance.transform;
                last_previous = instance.previous_transform;
            }
        }
    }
    REQUIRE(scene.physics.stats().active_bodies == 0);
    const DVec3 origin = scene.world.render_frame().origin;
    CHECK(last_sent.position_m(1, origin) == doctest::Approx(static_cast<float>(where(ball).y)).epsilon(0.001));
    CHECK(last_previous.position_m(1, origin) == last_sent.position_m(1, origin));
}

TEST_CASE("a moved row gone before the frame that would send it isn't asked about") {
    Scene scene;
    const flecs::entity eye = scene.ecs().entity().set(de::world::Camera{});
    de::world::place(eye, Transform{.position = {0.0, 50.0, 0.0}});
    scene.ecs().set(de::world::ActiveCamera{.entity = eye.id()});
    const flecs::entity ball = scene.ecs().entity();
    ball.set(de::world::MeshInstance{});
    ball.set(Collider{.shape = ShapeKind::Sphere, .radius_m = 0.5f});
    ball.set(RigidBody{.kind = BodyKind::Dynamic});
    de::world::place(ball, Transform{.position = {0.0, 500.0, 0.0}});
    scene.world.advance(1.0 / 60.0);
    scene.world.advance(1.0 / 60.0);
    ball.destruct();
    scene.world.advance(1.0 / 240.0); // no tick in it, so it still has its list from the last
    CHECK(scene.world.render_frame().freed_slots.size() == 1);
    for (const de::world::RenderInstance& instance : scene.world.render_frame().instances) {
        CHECK(instance.slot == de::world::kSkipRenderInstance);
    }
}

TEST_CASE("the broad phase is rebuilt for a first load, and not for a cell among many") {
    Scene scene;
    const auto spawn = [&](int count, double y) {
        for (int i = 0; i < count; ++i) {
            scene.box({static_cast<double>(i) * 2.0, y, 0.0}, {0.5f, 0.5f, 0.5f});
        }
    };
    spawn(5000, 0.0);
    scene.ticks(1);
    CHECK(scene.physics.stats().broad_phase_rebuilds == 1);
    spawn(1100, 20.0);
    scene.ticks(1);
    CHECK(scene.physics.stats().bodies == 6100);
    CHECK(scene.physics.stats().broad_phase_rebuilds == 1);
    // What came in is there all the same, from now.
    QueryHit hit;
    CHECK(scene.physics.ray_cast({2000.0, 40.0, 0.0}, {0.0f, -1.0f, 0.0f}, 50.0f, hit));
    CHECK(hit.position.y == doctest::Approx(20.5));
    // A few, however empty it is, aren't worth one either.
    Scene other;
    other.box({0.0, 0.0, 0.0}, {0.5f, 0.5f, 0.5f});
    other.ticks(1);
    CHECK(other.physics.stats().broad_phase_rebuilds == 0);
}

TEST_CASE("a moving body or character under a parent with a transform is refused, and says so") {
    PhysicsErrors errors; // NOLINT(misc-const-correctness): the log writes to it
    Scene scene;
    const flecs::entity parent = scene.ecs().entity();
    de::world::place(parent, Transform{.position = {100.0, 0.0, 0.0}});
    scene.ticks(1);
    const auto body_under = [&](flecs::entity on, BodyKind kind) {
        const flecs::entity child = scene.ecs().entity().child_of(on);
        child.set(Collider{.shape = ShapeKind::Box, .half_extents_m = {1.0f, 1.0f, 1.0f}});
        child.set(RigidBody{.kind = kind});
        child.set(Transform{.position = {5.0, 0.0, 0.0}});
        return child;
    };
    // Made that way. A static one can be: it's put where it is, and stays.
    const flecs::entity dynamic_child = body_under(parent, BodyKind::Dynamic);
    const flecs::entity kinematic_child = body_under(parent, BodyKind::Kinematic);
    body_under(parent, BodyKind::Static);
    const flecs::entity walker_child = scene.ecs().entity().child_of(parent);
    walker_child.set(CharacterController{});
    walker_child.set(Transform{.position = {5.0, 2.0, 0.0}});
    scene.ticks(3);
    CHECK(scene.physics.stats().bodies == 1);
    CHECK(scene.physics.stats().characters == 0);
    CHECK(errors.count == 3); // once each
    // They're left to the transform hierarchy, untouched.
    CHECK(where(dynamic_child).x == 5.0);
    CHECK(where(kinematic_child).x == 5.0);
    CHECK(where(walker_child).y == 2.0);

    // Given a parent later, both ways of having one.
    const flecs::entity falling = scene.sphere({0.0, 500.0, 0.0});
    const flecs::entity lift = scene.box({20.0, 0.0, 0.0}, {1.0f, 1.0f, 1.0f}, BodyKind::Kinematic);
    const flecs::entity adopted = scene.sphere({-20.0, 500.0, 0.0});
    const flecs::entity walker = scene.character({40.0, 2.0, 0.0});
    scene.ticks(2);
    REQUIRE(scene.physics.stats().bodies == 4);
    REQUIRE(scene.physics.stats().characters == 1);
    de::world::attach(falling, parent);
    de::world::attach(lift, parent);
    de::world::attach(walker, parent);
    adopted.child_of(parent);
    const DVec3 falling_local = where(falling);
    const DVec3 lift_local = where(lift);
    scene.ticks(3);
    CHECK(scene.physics.stats().bodies == 1);
    CHECK(scene.physics.stats().characters == 0);
    CHECK(errors.count == 7);
    CHECK(where(falling) == falling_local);
    CHECK(where(lift) == lift_local);

    // A parent with no transform is only a way of keeping things together.
    const flecs::entity scope = scene.ecs().entity();
    const flecs::entity free = scene.ecs().entity().child_of(scope);
    free.set(Collider{.shape = ShapeKind::Sphere, .radius_m = 0.5f});
    free.set(RigidBody{.kind = BodyKind::Dynamic});
    free.set(Transform{.position = {0.0, 500.0, 40.0}});
    scene.ticks(10);
    CHECK(where(free).y < 500.0);
    CHECK(errors.count == 7);
}

TEST_CASE("a new body takes one table move, whatever its kind") {
    for (const BodyKind kind : {BodyKind::Dynamic, BodyKind::Kinematic}) {
        Scene scene;
        const flecs::entity handle = scene.ecs().lookup("de::BodyHandle");
        const flecs::entity tag =
            scene.ecs().lookup(kind == BodyKind::Dynamic ? "de::DynamicBody" : "de::KinematicBody");
        REQUIRE(handle.is_valid());
        REQUIRE(tag.is_valid());
        // In one move, what it's given all arrives together.
        int handles = 0;
        bool with_its_tag = false;
        scene.ecs().observer<>().with(handle).event(flecs::OnAdd).each([&](flecs::entity entity) {
            ++handles;
            with_its_tag = entity.has(tag);
        });
        scene.box({0.0, 0.0, 0.0}, {0.5f, 0.5f, 0.5f}, kind);
        scene.ticks(1);
        CHECK(handles == 1);
        CHECK(with_its_tag);
        CHECK(scene.physics.stats().bodies == 1);
    }
}

TEST_CASE("physics moves a row, and leaves its table alone") {
    Scene scene;
    const flecs::entity asleep = scene.ecs().entity();
    asleep.set(Collider{.shape = ShapeKind::Sphere, .radius_m = 0.5f});
    asleep.set(RigidBody{.kind = BodyKind::Dynamic, .start_asleep = true});
    de::world::place(asleep, Transform{.position = {50.0, 500.0, 0.0}});
    const flecs::entity falling = scene.ecs().entity();
    falling.set(Collider{.shape = ShapeKind::Sphere, .radius_m = 0.5f});
    falling.set(RigidBody{.kind = BodyKind::Dynamic});
    de::world::place(falling, Transform{.position = {0.0, 500.0, 0.0}});
    scene.ticks(3);

    const auto changed = [](auto& query) {
        bool any = false;
        query.run([&](flecs::iter& it) {
            while (it.next()) {
                any = any || it.changed();
            }
        });
        return any;
    };
    auto transforms = scene.ecs().query_builder<const Transform>().cached().detect_changes().build();
    auto worlds = scene.ecs().query_builder<const de::world::WorldTransform>().cached().detect_changes().build();
    auto previous =
        scene.ecs().query_builder<const de::world::PreviousWorldTransform>().cached().detect_changes().build();
    changed(transforms);
    changed(worlds);
    changed(previous);
    const double was_at = falling.get<de::world::WorldTransform>().position.y;
    scene.ticks(1);
    // Nothing for the world's copies of the table to do, or for the renderer to look through.
    CHECK_FALSE(changed(transforms));
    CHECK_FALSE(changed(worlds));
    CHECK_FALSE(changed(previous));
    // And still all there, for this tick's move: what's seen goes from where it was to where it is.
    const auto& world = falling.get<de::world::WorldTransform>();
    CHECK(world.position.y < was_at);
    CHECK(world.position == where(falling));
    CHECK(falling.get<de::world::PreviousWorldTransform>().value.position.y == was_at);
    CHECK(asleep.get<de::world::WorldTransform>().position.y == 500.0);
}

TEST_CASE("what physics moves reaches the renderer each frame, and where it stopped") {
    Scene scene;
    const flecs::entity eye = scene.ecs().entity().set(de::world::Camera{});
    de::world::place(eye, Transform{.position = {0.0, 50.0, 0.0}});
    scene.ecs().set(de::world::ActiveCamera{.entity = eye.id()});
    scene.box({0.0, -0.5, 0.0}, {50.0f, 0.5f, 50.0f});
    const auto ball_at = [&](const DVec3& position, bool sleeping) {
        const flecs::entity entity = scene.ecs().entity();
        entity.set(de::world::MeshInstance{});
        entity.set(Collider{.shape = ShapeKind::Sphere, .radius_m = 0.5f});
        entity.set(RigidBody{.kind = BodyKind::Dynamic, .start_asleep = sleeping});
        de::world::place(entity, Transform{.position = position});
        return entity;
    };
    const flecs::entity ball = ball_at({0.0, 3.0, 0.0}, false);
    const flecs::entity neighbour = ball_at({10.0, 0.5, 0.0}, true);
    const auto sent = [&](flecs::entity entity) -> const de::world::RenderInstance* {
        const uint32_t slot = entity.get<de::world::RenderSlot>().slot;
        for (const de::world::RenderInstance& instance : scene.world.render_frame().instances) {
            if (instance.slot == slot) {
                return &instance;
            }
        }
        return nullptr;
    };
    scene.world.advance(1.0 / 60.0);
    scene.world.advance(1.0 / 60.0);
    // Frames between ticks too, as what's drawn moves between them.
    float last_y = 1e9f;
    for (int frame = 0; frame < 6; ++frame) {
        scene.world.advance(1.0 / 240.0);
        const de::world::RenderInstance* const instance = sent(ball);
        REQUIRE(instance != nullptr);
        const float y = instance->transform.position_m(1, scene.world.render_frame().origin);
        CHECK(y < last_y);
        last_y = y;
        CHECK(sent(neighbour) == nullptr);
        CHECK(scene.world.render_frame().instances.size() == 1); // its table wasn't looked through
    }
    for (int frame = 0; frame < 600 && scene.physics.stats().active_bodies > 0; ++frame) {
        scene.world.advance(1.0 / 60.0);
    }
    REQUIRE(scene.physics.stats().active_bodies == 0);
    // The tick after it stopped: sent once more, as where it is with no motion in it.
    scene.world.advance(1.0 / 60.0);
    const de::world::RenderInstance* const last = sent(ball);
    REQUIRE(last != nullptr);
    CHECK(last->transform.position_m(1, scene.world.render_frame().origin) ==
          doctest::Approx(static_cast<float>(where(ball).y)).epsilon(0.001));
    CHECK(last->previous_transform.position_m(1, scene.world.render_frame().origin) ==
          last->transform.position_m(1, scene.world.render_frame().origin));
    // Then it's left alone.
    scene.world.advance(1.0 / 60.0);
    CHECK(sent(ball) == nullptr);
    CHECK(scene.world.render_frame().instances.empty());
    scene.world.resync_render_instances();
    scene.world.advance(1.0 / 60.0);
    const de::world::RenderInstance* const resting = sent(ball);
    REQUIRE(resting != nullptr);
    CHECK(resting->transform.position_m(1, scene.world.render_frame().origin) ==
          doctest::Approx(static_cast<float>(where(ball).y)).epsilon(0.001));
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

// The mesh to measure with, as a .demesh: the boulder, from the repository's cooked/, when there's one and this is for
// reading. Otherwise a wavy one cooked here, so this keeps working.
std::vector<std::byte> benchmark_mesh(bool big, de::platform::Vfs& vfs, de::core::JobSystem& jobs,
                                      const Sandbox& sandbox, std::string& name) {
    const auto read = [](const de::platform::Vfs& from, AssetId wanted) {
        const de::asset::CookedPath path = de::asset::cooked_path(wanted, AssetType::Mesh);
        std::vector<std::byte> bytes(static_cast<size_t>(from.file_size(path.c_str()).value()));
        REQUIRE(from.read_file(path.c_str(), bytes).ok());
        return bytes;
    };
    const AssetId boulder = de::asset::asset_id([] {
        de::asset::Guid guid;
        REQUIRE(de::asset::parse_guid("6681c1d7-0c12-4528-9523-2642e902ae2d", guid));
        return guid;
    }());
    if (big) {
        const auto cooked = de::platform::Vfs::create({.game_root = DE_SOURCE_DIR "/cooked"});
        if (cooked && cooked->exists(de::asset::cooked_path(boulder, AssetType::Mesh).c_str())) {
            name = "boulder_01";
            return read(*cooked, boulder);
        }
        MESSAGE("no cooked boulder (run the cooker): measuring a wavy grid instead");
    }
    name = "wavy grid";
    std::vector<de::test::Vertex> vertices;
    std::vector<uint32_t> indices;
    de::test::grid(64, vertices, indices);
    const de::platform::PathString source = [&] {
        de::platform::PathString path = sandbox.folder("src");
        REQUIRE(path.append("/content"));
        REQUIRE(de::platform::create_directories(path.c_str()).ok());
        REQUIRE(path.append("/wavy.gltf"));
        return path;
    }();
    const std::string text = de::test::gltf(vertices, indices, 0.0f, R"([{"name": "paint"}])", "[]", "[]");
    REQUIRE(de::platform::write_file_atomic(source.c_str(), de::test::bytes_of(text)).ok());
    REQUIRE(de::cooker::cook(vfs, jobs, nullptr).ok());
    AssetId id;
    const auto list = [&](const char* folder, bool is_directory) {
        de::platform::PathString path("game:/");
        REQUIRE(path.append(folder));
        if (!is_directory) {
            return;
        }
        static_cast<void>(vfs.list(path.c_str(), [&](const char* file, bool) {
            const std::string_view file_name(file);
            if (file_name.ends_with(".demesh")) {
                REQUIRE(de::asset::parse_asset_id(file_name.substr(0, de::asset::kAssetIdTextLength), id));
            }
        }));
    };
    static_cast<void>(vfs.list("game:/", list));
    REQUIRE(id.is_valid());
    return read(vfs, id);
}

// What a mesh costs next to a box: cooking, memory, rays, falling bodies and characters, at each limit on triangles.
void mesh_benchmark(bool big) {
    const Sandbox sandbox;
    de::core::JobSystem jobs({.worker_count = 7});
    de::platform::Vfs vfs = sandbox.make_vfs({.jobs = &jobs, .game_writable = true});
    std::string name;
    const std::vector<std::byte> mesh_file = benchmark_mesh(big, vfs, jobs, sandbox, name);
    struct alignas(16) Block {
        std::byte bytes[16];
    };
    std::vector<Block> aligned(mesh_file.size() / sizeof(Block));
    std::memcpy(aligned.data(), mesh_file.data(), mesh_file.size());
    const auto* const mesh = reinterpret_cast<const de::asset::MeshAsset*>(aligned.data());
    const Vec3 mesh_min(mesh->bounds.min[0], mesh->bounds.min[1], mesh->bounds.min[2]);
    const Vec3 mesh_max(mesh->bounds.max[0], mesh->bounds.max[1], mesh->bounds.max[2]);
    const Vec3 half_size = (mesh_max - mesh_min) * 0.5f;
    const Vec3 center = (mesh_max + mesh_min) * 0.5f;
    MESSAGE(doctest::String(name.c_str()), ": ", mesh->indices.count, " indices over all its LODs, ", mesh_file.size(),
            " bytes of .demesh, ", half_size.x * 2.0f, " x ", half_size.y * 2.0f, " x ", half_size.z * 2.0f, " m");

    auto assets = AssetRegistry::create({.vfs = &vfs, .jobs = &jobs, .hot_reload = false});
    REQUIRE(assets.ok());
    const float spacing = std::max(half_size.x, half_size.z) * 2.5f;
    const int side = big ? 100 : 8;
    const auto field = [&](int i) {
        const int row = i / side;
        return DVec3((i % side) * static_cast<double>(spacing) + 0.37, 0.0, row * static_cast<double>(spacing) + 0.19);
    };
    const int fallers = big ? 10000 : 100;
    const int walkers = big ? 200 : 10;
    const int ticks = big ? 240 : 5;
    const int rays = big ? 200000 : 2000;
    // Over the field, on no pattern a grid would line up with.
    const auto ray_origin = [&](int i, double height) {
        const double width = static_cast<double>(spacing) * side;
        const double u = std::fmod(static_cast<double>(i) * 0.7548776662466927, 1.0);
        const double v = std::fmod(static_cast<double>(i) * 0.5698402909980532, 1.0);
        return DVec3(u * width, height, v * width);
    };
    const auto time_rays = [&](Scene& scene, const Vec3& direction, const char* what) {
        int hits = 0;
        QueryHit hit;
        const uint64_t start = de::platform::time_ns();
        for (int i = 0; i < rays; ++i) {
            hits +=
                scene.physics.ray_cast(ray_origin(i, static_cast<double>(half_size.y) * 4.0), direction, 500.0f, hit)
                    ? 1
                    : 0;
        }
        const double each_ns = static_cast<double>(de::platform::time_ns() - start) / rays;
        MESSAGE("  rays ", doctest::String(what), ": ", each_ns, " ns each, on one thread, ", hits, " of ", rays,
                " hit");
    };
    // Things falling onto the field, then walking on it, for whatever it's made of.
    const auto time_field = [&](Scene& scene) {
        time_rays(scene, {0.0f, -1.0f, 0.0f}, "straight down");
        time_rays(scene, glm::normalize(Vec3(1.0f, -0.25f, 0.5f)), "along the field");
        // Ground under it all, so what rolls off comes to rest.
        const double width = static_cast<double>(spacing) * (side - 1);
        scene.box({width * 0.5, static_cast<double>(mesh_min.y) - 0.5, width * 0.5},
                  {static_cast<float>(width), 0.5f, static_cast<float>(width)});
        const int per_row = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(fallers))));
        for (int i = 0; i < fallers; ++i) {
            const int row = i / per_row;
            const double x = (i % per_row) * width / per_row;
            const double z = row * width / per_row;
            scene.sphere({x + 0.13, static_cast<double>(half_size.y) * 2.0 + 2.0, z + 0.29}, 0.25f);
        }
        scene.ticks(1);
        const double falling = median_step_us(scene, big ? 45 : 3);
        const double landing = median_step_us(scene, ticks);
        for (int i = 0; i < walkers; ++i) {
            const flecs::entity walker =
                scene.character(field(i) + DVec3(0.0, static_cast<double>(half_size.y) * 2.5, 0.0));
            walker.get_mut<CharacterController>().desired_velocity_mps = {0.5f, 0.0f, 1.0f};
        }
        scene.ticks(big ? 30 : 1);
        const double walking = median_step_us(scene, ticks);
        const de::physics::PhysicsStats stats = scene.physics.stats();
        MESSAGE("  ", fallers, " spheres falling: ", falling, " us a tick; landing: ", landing, " us; with ", walkers,
                " characters: ", walking, " us; ", stats.bodies, " bodies, ", stats.active_bodies, " awake at the end");
    };

    PhysicsDesc scene_desc;
    scene_desc.assets = &*assets;
    if (big) {
        scene_desc.max_body_pairs = 1024U * 1024U;
        scene_desc.max_contact_constraints = 512U * 1024U;
    }
    {
        Scene boxes(&jobs, scene_desc);
        for (int i = 0; i < side * side; ++i) {
            boxes.box(field(i) + DVec3(center), half_size);
        }
        boxes.ticks(1);
        MESSAGE("a box of its bounds, ", side * side, " of them:");
        time_field(boxes);
    }

    std::vector<double> finest_heights;
    constexpr uint32_t kLimits[] = {100000000U, 8192U, 4096U, 2048U, 1024U, 512U, 256U};
    for (const uint32_t limit :
         big ? std::span<const uint32_t>(kLimits) : std::span<const uint32_t>(kLimits).subspan(3, 1)) {
        const AssetId id = AssetId::from_value(0x7000 + limit);
        de::cooker::MeshSettings settings;
        settings.collision_triangles = limit;
        de::cooker::Diagnostics diagnostics(false);
        double best_ms = 1e30;
        for (int run = 0; run < (big ? 5 : 1); ++run) {
            const uint64_t start = de::platform::time_ns();
            const auto cooked = de::cooker::cook_collision(mesh_file, settings, id, "benchmark", diagnostics);
            best_ms = std::min(best_ms, static_cast<double>(de::platform::time_ns() - start) / 1e6);
            REQUIRE(cooked.ok());
            REQUIRE(vfs.write_file(de::asset::cooked_path(id, AssetType::Collision).c_str(), cooked->span()).ok());
        }

        Scene scene(&jobs, scene_desc);
        const AssetHandle handle = assets->load<de::asset::CollisionAsset>(id);
        assets->wait_all();
        const auto* const collision = assets->get<de::asset::CollisionAsset>(handle);
        REQUIRE(collision != nullptr);
        const auto live = [] { return de::core::memory_stats(de::core::MemoryTag::Physics).live_bytes; };
        const auto instance = [&](int i) {
            const flecs::entity entity = scene.ecs().entity();
            entity.set(de::world::MeshCollider{.collision = handle});
            de::world::place(entity, Transform{.position = field(i)});
        };
        scene.ticks(1);
        const uint64_t live_empty = live();
        const double empty_us = median_step_us(scene, 16);
        instance(0);
        const uint64_t start = de::platform::time_ns();
        scene.ticks(1);
        const double first_us = static_cast<double>(de::platform::time_ns() - start) / 1000.0;
        const uint64_t live_one = live();
        const int thousand = big ? 1000 : 32;
        for (int i = 1; i < thousand; ++i) {
            instance(i);
        }
        scene.ticks(1);
        const uint64_t live_thousand = live();
        MESSAGE("collision of at most ", limit, " triangles: LOD ", collision->source_lod, ", ", collision->triangles,
                " triangles, ", collision->vertices, " vertices, error ", collision->error_m, " m; cooked in ", best_ms,
                " ms (from a cooked mesh, best of a few), ", assets->file(handle).size(), " bytes of .dephys");
        MESSAGE("  memory: the first body and its shape ", live_one - live_empty, " bytes; the next ", thousand - 1,
                " instances ", live_thousand - live_one, " bytes, ", (live_thousand - live_one) / (thousand - 1),
                " each. The tick that restored it took ", first_us, " us, an empty one ", empty_us, " us");

        // How far it lies from what's drawn at its finest: straight down on one of them, across it.
        std::vector<double> heights;
        constexpr int kSamples = 64;
        std::vector<double> errors;
        for (int row = 0; row < kSamples; ++row) {
            for (int column = 0; column < kSamples; ++column) {
                const DVec3 at = field(0) + DVec3(center) +
                                 DVec3((static_cast<double>(column) + 0.5) / kSamples * 2.0 - 1.0, 0.0,
                                       (static_cast<double>(row) + 0.5) / kSamples * 2.0 - 1.0) *
                                     DVec3(half_size);
                QueryHit hit;
                const bool hits = scene.physics.ray_cast(at + DVec3(0.0, static_cast<double>(half_size.y) * 4.0, 0.0),
                                                         {0.0f, -1.0f, 0.0f}, 500.0f, hit);
                heights.push_back(hits ? hit.position.y : -1e9);
                if (!finest_heights.empty()) {
                    const double finest = finest_heights[heights.size() - 1];
                    if (hits && finest > -1e8) {
                        errors.push_back(std::abs(hit.position.y - finest));
                    }
                }
            }
        }
        if (finest_heights.empty()) {
            finest_heights = std::move(heights);
        } else {
            // The worst are rays just past its outline, which land far down its side on one and not the other.
            std::ranges::sort(errors);
            double total = 0.0;
            for (const double error : errors) {
                total += error;
            }
            MESSAGE("  against its finest, on ", errors.size(),
                    " rays straight down: ", total / static_cast<double>(errors.size()), " m on average, median ",
                    errors[errors.size() / 2], ", 99th percentile ", errors[errors.size() * 99 / 100], ", worst ",
                    errors.back());
        }
        for (int i = thousand; i < side * side; ++i) {
            instance(i);
        }
        scene.ticks(1);
        MESSAGE("  ", side * side, " of them:");
        time_field(scene);
        assets->release(handle);
        assets->update();
    }
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
        {
            // The same with what moves in a table of its own, which is what the world's per-table transforms want.
            Scene scene(&jobs, desc);
            scene.box({0.0, -0.5, 0.0}, {2000.0f, 0.5f, 2000.0f});
            for (int i = 0; i < statics; ++i) {
                const flecs::entity entity = scene.ecs().entity();
                entity.set(Collider{.shape = ShapeKind::Sphere, .radius_m = 0.4f});
                entity.set(RigidBody{.kind = BodyKind::Dynamic, .start_asleep = true});
                de::world::place(entity, Transform{.position = grid(i, 2.0) + DVec3(-300.0, 0.4, -100.0)});
            }
            for (int i = 0; i < 100; ++i) {
                const flecs::entity entity = scene.ecs().entity();
                entity.add<InATableOfItsOwn>();
                entity.set(Collider{.shape = ShapeKind::Sphere, .radius_m = 0.4f});
                entity.set(RigidBody{.kind = BodyKind::Dynamic});
                de::world::place(entity, Transform{.position = {-400.0 - (static_cast<double>(i) * 2.0), 2000.0, 0.0}});
            }
            scene.ticks(1);
            report("asleep, and 100 falling apart", scene, median_step_us(scene, ticks));
        }
        {
            // A streamed cell: what a tick takes when 1,100 arrive among a great many.
            Scene scene(&jobs, desc);
            for (int i = 0; i < statics; ++i) {
                scene.box(grid(i, 2.0), {0.5f, 0.5f, 0.5f});
            }
            scene.ticks(1);
            const int cell_size = big ? 1100 : 10;
            std::array<double, 7> arrival_us{};
            for (size_t cell = 0; cell < arrival_us.size(); ++cell) {
                for (int i = 0; i < cell_size; ++i) {
                    scene.box(grid(i, 2.0) + DVec3(0.0, 10.0 + (static_cast<double>(cell) * 5.0), 0.0),
                              {0.5f, 0.5f, 0.5f});
                }
                const uint64_t start = de::platform::time_ns();
                scene.ticks(1);
                arrival_us[cell] = static_cast<double>(de::platform::time_ns() - start) / 1e3;
            }
            std::ranges::sort(arrival_us);
            report("a cell of 1,100 arrives", scene, arrival_us[arrival_us.size() / 2]);
            report("the tick after it", scene, median_step_us(scene, ticks));
        }
        // Rendered: what the frames cost at 240 fps, with a mesh on every row, so extraction is in it. The median frame
        // has no tick in it, and the mean shares out the ones that do.
        for (const bool apart : {false, true}) {
            Scene scene(&jobs, desc);
            const flecs::entity eye = scene.ecs().entity().set(de::world::Camera{});
            de::world::place(eye, Transform{.position = {0.0, 50.0, 0.0}});
            scene.ecs().set(de::world::ActiveCamera{.entity = eye.id()});
            scene.box({0.0, -0.5, 0.0}, {2000.0f, 0.5f, 2000.0f});
            for (int i = 0; i < statics; ++i) {
                const flecs::entity entity = scene.ecs().entity();
                entity.set(de::world::MeshInstance{});
                entity.set(Collider{.shape = ShapeKind::Sphere, .radius_m = 0.4f});
                entity.set(RigidBody{.kind = BodyKind::Dynamic, .start_asleep = true});
                de::world::place(entity, Transform{.position = grid(i, 2.0) + DVec3(-300.0, 0.4, -100.0)});
            }
            scene.world.advance(1.0 / 60.0);
            scene.world.advance(1.0 / 60.0);
            const auto frames = [&](const char* name_median, const char* name_mean) {
                const int count = big ? 480 : 8;
                double total_us = 0.0;
                std::vector<double> frame_us;
                frame_us.reserve(static_cast<size_t>(count));
                for (int frame = 0; frame < count; ++frame) {
                    const uint64_t start = de::platform::time_ns();
                    scene.world.advance(1.0 / 240.0);
                    frame_us.push_back(static_cast<double>(de::platform::time_ns() - start) / 1e3);
                    total_us += frame_us.back();
                }
                std::ranges::sort(frame_us);
                report(name_median, scene, frame_us[frame_us.size() / 2]);
                report(name_mean, scene, total_us / count);
            };
            if (!apart) {
                frames("with meshes, all asleep, median frame", "with meshes, all asleep, mean frame");
            }
            for (int i = 0; i < 100; ++i) {
                const flecs::entity entity = scene.ecs().entity();
                if (apart) {
                    entity.add<InATableOfItsOwn>();
                }
                entity.set(de::world::MeshInstance{});
                entity.set(Collider{.shape = ShapeKind::Sphere, .radius_m = 0.4f});
                entity.set(RigidBody{.kind = BodyKind::Dynamic});
                de::world::place(entity, Transform{.position = {-400.0 - (static_cast<double>(i) * 2.0), 3000.0, 0.0}});
            }
            scene.world.advance(1.0 / 60.0);
            if (apart) {
                frames("with meshes, 100 falling apart, median frame", "with meshes, 100 falling apart, mean frame");
            } else {
                frames("with meshes, 100 falling among them, median frame",
                       "with meshes, 100 falling among them, mean frame");
            }
        }
    }
    mesh_benchmark(big);
}
