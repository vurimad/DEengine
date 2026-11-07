#include "asset/collision_format.h"
#include "asset/mesh_format.h"
#include "asset/registry.h"
#include "cooker/cooker.h"
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
#include "world/transform.h"
#include "world/world.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using de::DVec3;
using de::Vec3;
using de::asset::AssetHandle;
using de::asset::AssetId;
using de::asset::AssetRegistry;
using de::asset::AssetState;
using de::asset::AssetType;
using de::asset::CollisionAsset;
using de::physics::BodyKind;
using de::physics::CharacterController;
using de::physics::Collider;
using de::physics::Physics;
using de::physics::QueryHit;
using de::physics::RigidBody;
using de::physics::ShapeKind;
using de::test::Sandbox;
using de::test::Vertex;
using de::world::MeshCollider;
using de::world::Transform;
using de::world::World;

namespace {

// Not on anything round, in any way: a float is about a meter apart here, so what goes through one lands elsewhere.
constexpr DVec3 kFar{10'000'000.37, 500.19, -7'000'000.81};

// A square of n x n cells, half_size_m to each side, as a height above x and z. Its triangles face up.
std::string height_field(uint32_t n, float half_size_m, const std::function<float(float, float)>& height,
                         float node_y = 0.0f) {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    for (uint32_t row = 0; row <= n; ++row) {
        for (uint32_t column = 0; column <= n; ++column) {
            const float u = static_cast<float>(column) / static_cast<float>(n);
            const float v = static_cast<float>(row) / static_cast<float>(n);
            const float x = (u * 2.0f - 1.0f) * half_size_m;
            const float z = (v * 2.0f - 1.0f) * half_size_m;
            vertices.push_back({.position = {x, height(x, z), z}, .normal = {0, 1, 0}, .uv = {u, v}});
        }
    }
    for (uint32_t row = 0; row < n; ++row) {
        for (uint32_t column = 0; column < n; ++column) {
            const uint32_t i = row * (n + 1) + column;
            indices.insert(indices.end(), {i, i + n + 1, i + 1, i + 1, i + n + 1, i + n + 2});
        }
    }
    return de::test::gltf(vertices, indices, node_y, R"([{"name": "paint"}])", "[]", "[]");
}

// y = x / 4 + 1.125, sixteen meters across, which no LOD can get wrong.
float ramp_height(float x, float /*z*/) {
    return x * 0.25f + 1.125f;
}

std::string ramp(float node_y = 0.0f) {
    return height_field(8, 8.0f, ramp_height, node_y);
}

float hill_height(float x, float z) {
    return 1.5f * std::sin(x * 0.4f) * std::cos(z * 0.3f);
}

// Enough triangles for its collision to be a coarser LOD.
std::string hill() {
    return height_field(64, 12.0f, hill_height);
}

std::string flat(float height_m) {
    return height_field(16, 20.0f, [height_m](float, float) { return height_m; });
}

// A world with physics in it, its assets cooked from sources in a sandbox.
struct Scene {
    explicit Scene(uint32_t workers = 2, bool hot_reload = false)
        : jobs({.worker_count = workers}), vfs(sandbox.make_vfs({.jobs = &jobs, .game_writable = true})),
          assets(AssetRegistry::create({.vfs = &vfs, .jobs = &jobs, .hot_reload = hot_reload}).value()),
          physics({.jobs = &jobs, .assets = &assets}),
          world(World::create({.jobs = &jobs, .assets = &assets}).value()) {
        physics.add_to_world(world);
    }

    const flecs::world& ecs() { return world.ecs(); }

    void write_source(std::string_view relative, std::string_view text) const {
        de::platform::PathString path = sandbox.folder("src");
        REQUIRE(path.append('/'));
        REQUIRE(path.append(relative));
        const de::platform::PathString parent(path.view().substr(0, path.view().find_last_of('/')));
        REQUIRE(de::platform::create_directories(parent.c_str()).ok());
        REQUIRE(de::platform::write_file_atomic(path.c_str(), de::test::bytes_of(text)).ok());
    }

    void cook() {
        const de::cooker::CookReport report = de::cooker::cook(vfs, jobs, nullptr);
        REQUIRE(report.ok());
    }

    // The ID a source cooked to, from its meta.
    AssetId id_of(std::string_view source) const {
        de::platform::PathString meta("src:/meta/");
        REQUIRE(meta.append(source));
        REQUIRE(meta.append(".json"));
        const auto size = vfs.file_size(meta.c_str());
        REQUIRE(size.ok());
        std::string text(static_cast<size_t>(*size), '\0');
        REQUIRE(vfs.read_file(meta.c_str(), std::as_writable_bytes(std::span(text.data(), text.size()))).ok());
        const size_t at = text.find(R"("guid": ")");
        REQUIRE(at != std::string::npos);
        de::asset::Guid guid;
        REQUIRE(de::asset::parse_guid(std::string_view(text).substr(at + 9, de::asset::kGuidTextLength), guid));
        return de::asset::asset_id(guid);
    }

    // A model cooked from this source, with its collision loaded and held, until the end of the test.
    AssetHandle model(std::string_view name, std::string_view source) {
        std::string relative = "content/props/";
        relative += name;
        relative += ".gltf";
        write_source(relative, source);
        cook();
        std::string under_content = "props/";
        under_content += name;
        under_content += ".gltf";
        const AssetHandle handle = assets.load<CollisionAsset>(de::asset::collision_asset_id(id_of(under_content)));
        assets.wait_all();
        REQUIRE(assets.state(handle) == AssetState::Ready);
        held.push_back(handle);
        return handle;
    }

    flecs::entity instance(AssetHandle collision, const Transform& transform) {
        const flecs::entity entity = ecs().entity();
        entity.set(MeshCollider{.collision = collision});
        de::world::place(entity, transform);
        return entity;
    }

    flecs::entity ball(const DVec3& position, float radius_m = 0.5f) {
        const flecs::entity entity = ecs().entity();
        entity.set(Collider{.shape = ShapeKind::Sphere, .radius_m = radius_m});
        entity.set(RigidBody{.kind = BodyKind::Dynamic});
        de::world::place(entity, Transform{.position = position});
        return entity;
    }

    void ticks(int count) {
        for (int i = 0; i < count; ++i) {
            assets.update();
            world.tick();
        }
    }

    // How far below a ray straight down from origin it hits; negative with no hit.
    double surface_y(const DVec3& origin) const {
        QueryHit hit;
        if (!physics.ray_cast(origin, {0.0f, -1.0f, 0.0f}, 200.0f, hit)) {
            return -1e9;
        }
        return hit.position.y;
    }

    ~Scene() {
        for (const AssetHandle handle : held) {
            assets.release(handle);
        }
    }
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene(Scene&&) = delete;
    Scene& operator=(Scene&&) = delete;

    Sandbox sandbox;
    de::core::JobSystem jobs;
    de::platform::Vfs vfs;
    AssetRegistry assets;
    Physics physics;
    World world;
    std::vector<AssetHandle> held;
};

DVec3 where(flecs::entity entity) {
    return entity.get<Transform>().position;
}

// Counts what physics complains of, while it lasts.
class PhysicsErrors final : public de::core::LogSink {
public:
    PhysicsErrors() { de::core::add_log_sink(*this); }
    ~PhysicsErrors() { de::core::remove_log_sink(*this); }
    PhysicsErrors(PhysicsErrors&&) = delete;
    PhysicsErrors& operator=(PhysicsErrors&&) = delete;

    void write(const de::core::LogMessage& message) override {
        if (message.level == de::core::LogLevel::Error && std::string_view(message.channel) == "physics") {
            ++count;
        }
    }

    int count = 0;
};

} // namespace

TEST_CASE("a cooked mesh's shape restores, and a ray hits it where the mesh is") {
    Scene scene;
    const AssetHandle collision = scene.model("ramp", ramp());
    const flecs::entity entity = scene.instance(collision, Transform{});
    scene.ticks(1);
    CHECK(scene.physics.stats().bodies == 1);

    const auto expect_hit = [&](double x, double z) {
        CAPTURE(x);
        CAPTURE(z);
        QueryHit hit;
        REQUIRE(scene.physics.ray_cast({x, 50.0, z}, {0.0f, -1.0f, 0.0f}, 100.0f, hit));
        CHECK(hit.entity == entity.id());
        CHECK(hit.position.y == doctest::Approx(x * 0.25 + 1.125).epsilon(1e-4));
        CHECK(hit.distance_m == doctest::Approx(50.0 - (x * 0.25 + 1.125)).epsilon(1e-4));
        // Straight up from the slope, tilted toward -x.
        CHECK(hit.normal.y == doctest::Approx(4.0 / std::sqrt(17.0)).epsilon(1e-3));
        CHECK(hit.normal.x == doctest::Approx(-1.0 / std::sqrt(17.0)).epsilon(1e-3));
    };
    expect_hit(0.37, 0.19);
    expect_hit(-6.81, 3.53);
    expect_hit(7.91, -7.13);
    // Past its edge, there's nothing.
    QueryHit hit;
    CHECK_FALSE(scene.physics.ray_cast({8.2, 50.0, 0.0}, {0.0f, -1.0f, 0.0f}, 100.0f, hit));
    CHECK_FALSE(scene.physics.ray_cast({0.0, 50.0, 8.2}, {0.0f, -1.0f, 0.0f}, 100.0f, hit));
    // A sphere touches it a radius from its center, along the normal.
    REQUIRE(scene.physics.sphere_cast({0.0, 50.0, 0.0}, 0.5f, {0.0f, -1.0f, 0.0f}, 100.0f, hit));
    CHECK(hit.entity == entity.id());
    CHECK(hit.position.y == doctest::Approx(1.125 + (0.5 / std::sqrt(17.0)) * 0.25).epsilon(1e-2));
}

TEST_CASE("a mesh is where it's put, far from the world's zero") {
    Scene scene;
    const AssetHandle collision = scene.model("ramp", ramp());
    const flecs::entity plain = scene.instance(collision, Transform{.position = kFar});
    const DVec3 scaled_at = kFar + DVec3(61.0, 0.0, 0.0);
    const flecs::entity scaled =
        scene.instance(collision, Transform{.position = scaled_at, .scale = {2.0f, 3.0f, 0.5f}});
    scene.ticks(1);
    QueryHit hit;
    REQUIRE(scene.physics.ray_cast(kFar + DVec3(1.37, 50.0, 2.19), {0.0f, -1.0f, 0.0f}, 100.0f, hit));
    CHECK(hit.entity == plain.id());
    CHECK(std::abs(hit.position.y - (kFar.y + 1.37 * 0.25 + 1.125)) < 1e-4);
    REQUIRE(scene.physics.ray_cast(scaled_at + DVec3(10.3, 50.0, 3.7), {0.0f, -1.0f, 0.0f}, 100.0f, hit));
    CHECK(hit.entity == scaled.id());
    CHECK(std::abs(hit.position.y - (scaled_at.y + 0.375 * 10.3 + 3.375)) < 1e-4);
    // Its edges are where they should be, to a tenth of what a float could say here.
    CHECK(scene.physics.ray_cast(kFar + DVec3(7.95, 50.0, 0.7), {0.0f, -1.0f, 0.0f}, 100.0f, hit));
    CHECK_FALSE(scene.physics.ray_cast(kFar + DVec3(8.05, 50.0, 0.7), {0.0f, -1.0f, 0.0f}, 100.0f, hit));
    CHECK(scene.physics.ray_cast(kFar + DVec3(-7.95, 50.0, 0.7), {0.0f, -1.0f, 0.0f}, 100.0f, hit));
    CHECK_FALSE(scene.physics.ray_cast(kFar + DVec3(-8.05, 50.0, 0.7), {0.0f, -1.0f, 0.0f}, 100.0f, hit));
    CHECK(scene.physics.ray_cast(kFar + DVec3(0.7, 50.0, 7.95), {0.0f, -1.0f, 0.0f}, 100.0f, hit));
    CHECK_FALSE(scene.physics.ray_cast(kFar + DVec3(0.7, 50.0, 8.05), {0.0f, -1.0f, 0.0f}, 100.0f, hit));
    // What's cast at it, too.
    REQUIRE(scene.physics.sphere_cast(kFar + DVec3(1.37, 50.0, 2.19), 0.5f, {0.0f, -1.0f, 0.0f}, 100.0f, hit));
    CHECK(std::abs(hit.position.y - (kFar.y + (1.37 + 0.5 / std::sqrt(17.0)) * 0.25 + 1.125)) < 1e-2);
}

TEST_CASE("the collision of a coarser LOD keeps close to the mesh it came from") {
    Scene scene;
    const AssetHandle collision = scene.model("hill", hill());
    const auto* const asset = scene.assets.get<CollisionAsset>(collision);
    REQUIRE(asset != nullptr);
    CHECK(asset->source_lod > 0);
    CHECK(asset->triangles <= 2048);
    CHECK(asset->triangles > 16);
    scene.instance(collision, Transform{});
    scene.ticks(1);
    double worst = 0.0;
    for (int i = 0; i < 400; ++i) {
        const int row = i / 20;
        const double x = -11.3 + (i % 20) * 1.13;
        const double z = -11.3 + row * 1.13;
        const double y = scene.surface_y({x, 50.0, z});
        REQUIRE(y > -1e8);
        worst = std::max(worst, std::abs(y - hill_height(static_cast<float>(x), static_cast<float>(z))));
    }
    MESSAGE("the hill's collision is ", asset->triangles, " triangles, at most ", worst, " m from the true surface");
    CHECK(worst < 0.05);
}

TEST_CASE("an instance's scale and rotation reach its shape") {
    Scene scene;
    const AssetHandle collision = scene.model("ramp", ramp());
    const DVec3 at{31.0, 2.0, 0.0};
    const flecs::entity stretched = scene.instance(collision, Transform{.position = at, .scale = {2.0f, 3.0f, 0.5f}});
    scene.ticks(1);
    // y = 3 (x / 2 / 4 + 1.125), out to 16 m along x and only 4 m along z.
    CHECK(scene.surface_y(at + DVec3(10.3, 50.0, 3.7)) == doctest::Approx(at.y + 0.375 * 10.3 + 3.375).epsilon(1e-4));
    CHECK(scene.surface_y(at + DVec3(15.5, 50.0, 0.7)) == doctest::Approx(at.y + 0.375 * 15.5 + 3.375).epsilon(1e-4));
    CHECK(scene.surface_y(at + DVec3(16.2, 50.0, 0.0)) < -1e8);
    CHECK(scene.surface_y(at + DVec3(0.0, 50.0, 4.2)) < -1e8);
    stretched.destruct();

    // Turned a quarter about y, the slope climbs along -z, 16 m each way, and its other axis is 4 m each way along x.
    const DVec3 turned_at{-40.0, 1.0, 12.0};
    const de::Quat yaw = glm::angleAxis(glm::radians(90.0f), Vec3(0.0f, 1.0f, 0.0f));
    scene.instance(collision, Transform{.position = turned_at, .rotation = yaw, .scale = {2.0f, 1.0f, 0.5f}});
    scene.ticks(2);
    CHECK(scene.surface_y(turned_at + DVec3(1.1, 50.0, -9.0)) ==
          doctest::Approx(turned_at.y + 0.125 * 9.0 + 1.125).epsilon(1e-4));
    CHECK(scene.surface_y(turned_at + DVec3(4.2, 50.0, 0.0)) < -1e8);

    // Mirrored: the same mesh climbs the other way, facing up still, and holds what lands on it.
    const DVec3 mirrored_at{0.0, 0.0, 60.0};
    scene.instance(collision, Transform{.position = mirrored_at, .scale = {-1.0f, 1.0f, 1.0f}});
    const flecs::entity ball = scene.ball(mirrored_at + DVec3(0.0, 4.0, 0.0));
    scene.ticks(2);
    CHECK(scene.surface_y(mirrored_at + DVec3(5.0, 50.0, 0.7)) == doctest::Approx(-0.25 * 5.0 + 1.125).epsilon(1e-4));
    scene.ticks(75);
    const double above_surface = where(ball).y - (-0.25 * where(ball).x + 1.125);
    CHECK(above_surface == doctest::Approx(0.5 / std::cos(std::atan(0.25))).epsilon(0.05));
    CHECK(scene.physics.stats().bodies == 3);
}

TEST_CASE("a scale of nothing gets no body, and it's said once") {
    Scene scene;
    const AssetHandle collision = scene.model("ramp", ramp());
    scene.instance(collision, Transform{.position = {0.5, 0.0, 0.0}, .scale = {1.0f, 0.0f, 1.0f}});
    PhysicsErrors errors; // NOLINT(misc-const-correctness): the log writes to it
    scene.ticks(3);
    CHECK(errors.count == 1);
    CHECK(scene.physics.stats().bodies == 0);
    CHECK(scene.physics.stats().shapes == 1);
}

TEST_CASE("a scale too small for Jolt gets no body either") {
    Scene scene;
    const AssetHandle collision = scene.model("ramp", ramp());
    scene.instance(collision, Transform{.position = {0.5, 0.0, 0.0}, .scale = {1e-7f, 1.0f, 1.0f}});
    PhysicsErrors errors; // NOLINT(misc-const-correctness): the log writes to it
    scene.ticks(3);
    CHECK(errors.count == 1);
    CHECK(scene.physics.stats().bodies == 0);
}

TEST_CASE("a Collider beside a mesh collider owns the body, and keeps it when the mesh collider goes") {
    Scene scene;
    const AssetHandle collision = scene.model("ramp", ramp());
    const flecs::entity entity = scene.instance(collision, Transform{.position = {0.5, 20.0, 0.0}});
    entity.set(Collider{.shape = ShapeKind::Sphere, .radius_m = 0.5f});
    entity.set(RigidBody{.kind = BodyKind::Dynamic, .mass_kg = 1.0f});
    scene.ticks(30);
    CHECK(scene.physics.stats().bodies == 1);
    const double falling = entity.get<Transform>().position.y;
    CHECK(falling < 20.0);
    entity.remove<de::world::MeshCollider>();
    scene.ticks(1);
    CHECK(scene.physics.stats().bodies == 1);
    // Carried on falling, not made again where it was, at rest.
    CHECK(entity.get<Transform>().position.y < falling);
}

TEST_CASE("many instances of a mesh share one restored shape, and each scale one wrapper around it") {
    Scene scene;
    const AssetHandle collision = scene.model("hill", hill());
    const auto* const asset = scene.assets.get<CollisionAsset>(collision);
    REQUIRE(asset != nullptr);
    const uint64_t shape_bytes = asset->shape.count;
    REQUIRE(shape_bytes > 4096);
    const auto live_bytes = [] { return de::core::memory_stats(de::core::MemoryTag::Physics).live_bytes; };

    constexpr int kInstances = 1000;
    const auto place_some = [&](int first, int count, float scale) {
        for (int i = first; i < first + count; ++i) {
            const int row = i / 32;
            scene.instance(collision, Transform{.position = {(i % 32) * 30.0 + 0.37, 0.0, row * 30.0 + 0.19},
                                                .scale = Vec3(scale)});
        }
    };
    place_some(0, 1, 1.0f);
    scene.ticks(1);
    CHECK(scene.physics.stats().shapes == 1);
    const uint64_t after_one = live_bytes();
    place_some(1, kInstances - 1, 1.0f);
    scene.ticks(1);
    CHECK(scene.physics.stats().bodies == kInstances);
    CHECK(scene.physics.stats().shapes == 1);
    const uint64_t per_instance = (live_bytes() - after_one) / (kInstances - 1);
    MESSAGE("a shape of ", shape_bytes, " bytes; ", per_instance, " bytes for each instance after the first");
    CHECK(per_instance < shape_bytes / 4);

    // Scaled, the same one under a wrapper per scale.
    place_some(kInstances, 40, 1.6f);
    place_some(kInstances + 40, 40, 2.4f);
    place_some(kInstances + 80, 40, 1.6f);
    scene.ticks(1);
    CHECK(scene.physics.stats().bodies == kInstances + 120);
    CHECK(scene.physics.stats().shapes == 3);
    CHECK(scene.surface_y({31.5, 50.0, 0.9}) == doctest::Approx(hill_height(1.13f, 0.71f)).epsilon(0.05));
}

TEST_CASE("a dynamic body lands on a mesh, and stays there, far from the world's zero") {
    for (const DVec3& origin : {DVec3(0.0), kFar}) {
        CAPTURE(origin.x);
        Scene scene;
        const AssetHandle collision = scene.model("floor", flat(0.625f));
        scene.instance(collision, Transform{.position = origin});
        const flecs::entity ball = scene.ball(origin + DVec3(1.37, 3.0, -2.19));
        const flecs::entity crate = scene.ecs().entity();
        crate.set(Collider{.shape = ShapeKind::Box, .half_extents_m = {0.5f, 0.5f, 0.5f}});
        crate.set(RigidBody{.kind = BodyKind::Dynamic});
        de::world::place(crate, Transform{.position = origin + DVec3(-3.19, 4.0, 2.37)});
        scene.ticks(300);
        CHECK(where(ball).y - origin.y == doctest::Approx(0.625 + 0.48).epsilon(0.01));
        CHECK(std::abs(where(ball).x - (origin.x + 1.37)) < 1e-3);
        CHECK(std::abs(where(ball).z - (origin.z - 2.19)) < 1e-3);
        CHECK(where(crate).y - origin.y == doctest::Approx(0.625 + 0.5).epsilon(0.03));
        CHECK(std::abs(where(crate).x - (origin.x - 3.19)) < 1e-3);
        CHECK(scene.physics.stats().active_bodies == 0);
    }
}

TEST_CASE("what rests on a mesh wakes when its collision is cooked again, and when its body goes") {
    Scene scene(2, true);
    const AssetHandle collision = scene.model("floor", flat(0.625f));
    const flecs::entity ground = scene.instance(collision, Transform{.position = {0.37, 0.0, 0.19}});
    const flecs::entity ball = scene.ball({1.37, 3.0, -2.19});
    scene.ticks(300);
    REQUIRE(scene.physics.stats().active_bodies == 0);
    // Its radius above the floor, less up to what Jolt lets contacts sink.
    constexpr double kRestingM = 0.5;
    constexpr double kSinksM = 0.021;
    REQUIRE(std::abs(where(ball).y - (0.625 + kRestingM)) <= kSinksM);

    // Half a meter lower: asleep, it would hang in the air.
    scene.write_source("content/props/floor.gltf", flat(0.125f));
    scene.cook();
    for (int i = 0; i < 4; ++i) {
        scene.assets.update();
        scene.assets.wait_all();
    }
    REQUIRE(scene.assets.revision(collision) == 2);
    scene.world.tick();
    scene.world.tick();
    CHECK(scene.physics.stats().bodies == 2);
    CHECK(scene.physics.stats().active_bodies == 1);
    scene.ticks(300);
    CHECK(std::abs(where(ball).y - (0.125 + kRestingM)) <= kSinksM);
    CHECK(scene.physics.stats().active_bodies == 0);

    ground.destruct();
    scene.ticks(1);
    CHECK(scene.physics.stats().bodies == 1);
    CHECK(scene.physics.stats().active_bodies == 1);
}

TEST_CASE("a character walks on a mesh, following its surface, far from the world's zero") {
    for (const DVec3& origin : {DVec3(0.0), kFar}) {
        CAPTURE(origin.x);
        Scene scene;
        const AssetHandle collision = scene.model("hill", hill());
        scene.instance(collision, Transform{.position = origin});
        const flecs::entity walker = scene.ecs().entity();
        walker.set(CharacterController{.desired_velocity_mps = {1.5f, 0.0f, 0.5f}});
        de::world::place(walker, Transform{.position = origin + DVec3(-8.37, 3.0, 1.19)});
        scene.ticks(60); // down to the ground
        REQUIRE(walker.get<CharacterController>().grounded);
        const DVec3 start = where(walker);
        double worst = 0.0;
        for (int i = 0; i < 480; ++i) {
            scene.ticks(1);
            const DVec3 local = where(walker) - origin;
            worst = std::max(worst,
                             std::abs(local.y - hill_height(static_cast<float>(local.x), static_cast<float>(local.z))));
        }
        // Eight seconds at that speed: a capsule stands off a slope a little, and steep ones slow it some.
        const DVec3 walked = where(walker) - start;
        CHECK(walked.x > 8.0);
        CHECK(walked.x < 12.5);
        CHECK(walked.z > 2.0);
        CHECK(worst < 0.25);
        CHECK(walker.get<CharacterController>().grounded);
    }
}

TEST_CASE("mesh bodies allocate nothing per tick, nor do characters on ground they've walked") {
    Scene scene;
    const AssetHandle collision = scene.model("hill", hill());
    scene.instance(collision, Transform{.position = kFar});
    scene.instance(collision, Transform{.position = kFar + DVec3(24.0, 0.0, 0.0), .scale = {1.0f, 1.25f, 1.0f}});
    constexpr int kWalkers = 3;
    const flecs::entity walkers[kWalkers] = {scene.ecs().entity(), scene.ecs().entity(), scene.ecs().entity()};
    const auto start_of = [](int i) { return kFar + DVec3(-9.0, 3.0, static_cast<double>(i) * 3.0); };
    for (int i = 0; i < kWalkers; ++i) {
        walkers[i].set(CharacterController{.desired_velocity_mps = {1.0f, 0.0f, 0.25f}});
        de::world::place(walkers[i], Transform{.position = start_of(i)});
    }
    const auto walk = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) {
            scene.world.tick();
        }
    };
    const auto allocations = [] { return de::core::memory_stats(de::core::MemoryTag::Physics).total_allocations; };
    // Jolt's lists of what a character touches grow to the most it's met, so the same ground the second time around.
    walk(240);
    for (int i = 0; i < kWalkers; ++i) {
        de::world::place(walkers[i], Transform{.position = start_of(i)});
    }
    const uint64_t before = allocations();
    walk(240);
    CHECK(allocations() == before);
    CHECK(walkers[0].get<CharacterController>().grounded);
}

TEST_CASE("the same world on a mesh steps to the same bits, however many threads it has") {
    const auto result = [](uint32_t workers) {
        Scene scene(workers);
        const AssetHandle collision = scene.model("hill", hill());
        scene.instance(collision, Transform{.position = kFar});
        std::vector<flecs::entity> moving;
        moving.reserve(41);
        for (int i = 0; i < 40; ++i) {
            moving.push_back(scene.ball(kFar + DVec3(std::sin(static_cast<double>(i)) * 8.0, 4.0 + i * 0.7,
                                                     std::cos(static_cast<double>(i)) * 8.0),
                                        0.4f));
        }
        const flecs::entity walker = scene.ecs().entity();
        walker.set(CharacterController{.desired_velocity_mps = {1.0f, 0.0f, 0.5f}});
        de::world::place(walker, Transform{.position = kFar + DVec3(-5.0, 3.0, 0.0)});
        moving.push_back(walker);
        scene.ticks(240);
        uint64_t hash = 0xcbf29ce484222325ULL;
        for (const flecs::entity entity : moving) {
            const de::DVec3 position = where(entity);
            for (int axis = 0; axis < 3; ++axis) {
                hash = (hash ^ std::bit_cast<uint64_t>(position[axis])) * 0x100000001b3ULL;
            }
        }
        return hash;
    };
    const uint64_t single_thread = result(0);
    CHECK(result(1) == single_thread);
    CHECK(result(6) == single_thread);
}

TEST_CASE("a mesh body waits for its asset without blocking anything, and then appears") {
    // With no workers a read only moves inside wait_all(): the asset stays loading for as long as this needs.
    Scene scene(0);
    scene.write_source("content/props/ramp.gltf", ramp());
    scene.cook();
    const AssetHandle collision =
        scene.assets.load<CollisionAsset>(de::asset::collision_asset_id(scene.id_of("props/ramp.gltf")));
    scene.held.push_back(collision);
    REQUIRE(scene.assets.state(collision) == AssetState::Loading);
    const flecs::entity entity = scene.instance(collision, Transform{.position = {0.37, 0.0, 0.19}});
    // Waiting isn't a failure that mends itself.
    PhysicsErrors errors; // NOLINT(misc-const-correctness): the log writes to it
    for (int i = 0; i < 60; ++i) {
        scene.world.tick();
        CHECK(scene.physics.stats().bodies == 0);
        CHECK(scene.physics.stats().shapes == 0);
    }
    CHECK(scene.assets.state(collision) == AssetState::Loading); // nothing went and got it
    QueryHit hit;
    CHECK_FALSE(scene.physics.ray_cast({0.68, 50.0, 0.46}, {0.0f, -1.0f, 0.0f}, 100.0f, hit));

    scene.assets.wait_all();
    REQUIRE(scene.assets.state(collision) == AssetState::Ready);
    scene.world.tick();
    CHECK(scene.physics.stats().bodies == 1);
    REQUIRE(scene.physics.ray_cast({0.68, 50.0, 0.46}, {0.0f, -1.0f, 0.0f}, 100.0f, hit));
    CHECK(hit.entity == entity.id());
    CHECK(errors.count == 0);
}

TEST_CASE("a cell's members with collision get bodies when it loads and lose them when it goes") {
    constexpr std::string_view kCell = R"({
  "coord": [0, 0],
  "entities": [
    {"name": "camp", "position": [40.37, 0, 40.19], "scale": 2},
    {"name": "ground", "parent": "camp", "mesh": "../props/floor.gltf", "collision": "mesh"},
    {"name": "boulder_1", "mesh": "../props/ramp.gltf", "position": [20.19, 10, 20.37], "collision": "mesh"},
    {"name": "boulder_2", "mesh": "../props/ramp.gltf", "position": [80.37, 10, 90.19], "scale": 1.6, "collision": "mesh"},
    {"name": "boulder_3", "mesh": "../props/ramp.gltf", "position": [100.19, 10, 90.37], "collision": "mesh"},
    {"name": "just_drawn", "mesh": "../props/ramp.gltf", "position": [110.37, 10, 20.19]},
    {"name": "banner", "mesh": "../props/ramp.gltf", "position": [10.19, 30, 110.37], "static": false},
  ],
})";
    // With no workers, so it's sure when things have loaded.
    Scene scene(0);
    scene.write_source("content/props/floor.gltf", flat(0.0f));
    scene.write_source("content/props/ramp.gltf", ramp());
    scene.write_source("content/world/camp.cell", kCell);
    scene.cook();
    const AssetId cell_id = scene.id_of("world/camp.cell");

    const flecs::entity cell = de::world::load_cell(scene.world.ecs(), cell_id);
    // Not until what it needs is loaded, however many ticks that takes.
    for (int i = 0; i < 5; ++i) {
        scene.ticks(1);
        CHECK(scene.physics.stats().bodies == 0);
    }
    scene.assets.wait_all();
    scene.ticks(1);
    // The ground and three boulders. Four shapes: the ground's and the scale it has from what it's on, the ramp's and
    // the one scale around it.
    CHECK(scene.physics.stats().bodies == 4);
    CHECK(scene.physics.stats().shapes == 4);
    // Static ones cost nothing awake, and a static child lies where its parent puts it.
    CHECK(scene.physics.stats().active_bodies == 0);
    QueryHit hit;
    REQUIRE(scene.physics.ray_cast({20.5, 50.0, 20.64}, {0.0f, -1.0f, 0.0f}, 100.0f, hit));
    CHECK(hit.position.y == doctest::Approx(10.0 + 0.31 * 0.25 + 1.125).epsilon(1e-4));
    const flecs::entity boulder = scene.ecs().entity(hit.entity);
    CHECK(boulder.has<de::world::InCell>(cell));
    CHECK(boulder.has<de::world::StaticTransform>());
    REQUIRE(scene.physics.ray_cast({40.37 + 39.0, 5.0, 40.19 - 39.0}, {0.0f, -1.0f, 0.0f}, 100.0f, hit));
    CHECK(hit.position.y == doctest::Approx(0.0).epsilon(1e-3));
    // Past 40 m, twice the floor's 20, it's not there. What's only drawn has no body.
    CHECK_FALSE(scene.physics.ray_cast({40.37 + 40.5, 5.0, 40.19}, {0.0f, -1.0f, 0.0f}, 100.0f, hit));
    CHECK_FALSE(scene.physics.ray_cast({110.37, 50.0, 20.19}, {0.0f, -1.0f, 0.0f}, 100.0f, hit));

    // Something lands on it.
    const flecs::entity ball = scene.ball({40.37, 3.0, 40.19});
    scene.ticks(180);
    CHECK(where(ball).y == doctest::Approx(0.48).epsilon(0.02));

    // The asset's held for as long as the cell is, and goes after its bodies.
    const de::asset::AssetRef ramp_ref{.id = de::asset::collision_asset_id(scene.id_of("props/ramp.gltf")),
                                       .type = AssetType::Collision};
    CHECK(scene.assets.find(ramp_ref).is_valid());
    cell.destruct();
    scene.ticks(2);
    CHECK(scene.physics.stats().bodies == 1); // the ball
    CHECK_FALSE(scene.physics.ray_cast({20.5, 50.0, 20.64}, {0.0f, -1.0f, 0.0f}, 100.0f, hit));
    scene.ticks(1); // what lets go runs at the next update
    CHECK_FALSE(scene.assets.find(ramp_ref).is_valid());
    CHECK(scene.physics.stats().shapes == 1); // the ball's: the meshes went with their assets

    // And it comes back.
    const flecs::entity again = de::world::load_cell(scene.world.ecs(), cell_id);
    scene.assets.wait_all();
    scene.ticks(1);
    CHECK(scene.physics.stats().bodies == 5);
    CHECK(scene.physics.stats().shapes == 5);
    again.destruct();
    scene.ticks(3);
    CHECK(scene.physics.stats().bodies == 1);
}

TEST_CASE("two cells share one shape, which stays until the last of them goes") {
    Scene scene(0);
    scene.write_source("content/props/ramp.gltf", ramp());
    scene.write_source("content/world/a.cell", R"({"coord": [0, 0], "entities": [
        {"mesh": "../props/ramp.gltf", "position": [20.37, 0, 20.19], "collision": "mesh"}]})");
    scene.write_source("content/world/b.cell", R"({"coord": [1, 0], "entities": [
        {"mesh": "../props/ramp.gltf", "position": [148.37, 0, 20.19], "collision": "mesh"}]})");
    scene.cook();
    const flecs::entity a = de::world::load_cell(scene.world.ecs(), scene.id_of("world/a.cell"));
    const flecs::entity b = de::world::load_cell(scene.world.ecs(), scene.id_of("world/b.cell"));
    scene.assets.wait_all();
    scene.ticks(1);
    CHECK(scene.physics.stats().bodies == 2);
    CHECK(scene.physics.stats().shapes == 1);
    a.destruct();
    scene.ticks(3);
    CHECK(scene.physics.stats().bodies == 1);
    CHECK(scene.physics.stats().shapes == 1);
    CHECK(scene.surface_y({148.68, 50.0, 20.46}) == doctest::Approx(0.31 * 0.25 + 1.125).epsilon(1e-4));
    // Another cell's body, made after the first let go, from the shape that's there.
    b.destruct();
    scene.ticks(3);
    CHECK(scene.physics.stats().bodies == 0);
    CHECK(scene.physics.stats().shapes == 0);
}

TEST_CASE("a mesh body goes with its collider or its entity") {
    Scene scene;
    const AssetHandle collision = scene.model("ramp", ramp());
    const flecs::entity first = scene.instance(collision, Transform{.position = {0.37, 0.0, 0.0}});
    const flecs::entity second = scene.instance(collision, Transform{.position = {50.37, 0.0, 0.0}});
    scene.ticks(1);
    REQUIRE(scene.physics.stats().bodies == 2);
    first.remove<MeshCollider>();
    scene.ticks(1);
    CHECK(scene.physics.stats().bodies == 1);
    second.destruct();
    scene.ticks(1);
    CHECK(scene.physics.stats().bodies == 0);
    first.set(MeshCollider{.collision = collision}); // given back, it's there again
    scene.ticks(1);
    CHECK(scene.physics.stats().bodies == 1);
    // A collider beside it wins, and there's one body.
    const flecs::entity both = scene.ecs().entity();
    both.set(MeshCollider{.collision = collision});
    both.set(Collider{.shape = ShapeKind::Sphere});
    both.set(RigidBody{});
    de::world::place(both, Transform{.position = {100.0, 0.0, 0.0}});
    scene.ticks(1);
    CHECK(scene.physics.stats().bodies == 2);
}

namespace {

// Rewrites a cooked file, its content hash made right or not, for what load_in_place doesn't look at.
void tamper_with(const Scene& scene, AssetId id, bool rehash, void (*change)(CollisionAsset&, std::span<std::byte>)) {
    const de::asset::CookedPath path = de::asset::cooked_path(id, AssetType::Collision);
    const auto size = scene.vfs.file_size(path.c_str());
    REQUIRE(size.ok());
    struct alignas(16) Block {
        std::byte bytes[16];
    };
    std::vector<Block> blocks(static_cast<size_t>(*size) / sizeof(Block));
    const std::span<std::byte> file(reinterpret_cast<std::byte*>(blocks.data()), static_cast<size_t>(*size));
    REQUIRE(scene.vfs.read_file(path.c_str(), file).ok());
    auto* const asset = reinterpret_cast<CollisionAsset*>(file.data());
    change(*asset, file);
    if (rehash) {
        asset->header.content_hash = de::asset::content_hash(file);
    }
    REQUIRE(scene.vfs.write_file(path.c_str(), file).ok());
}

} // namespace

TEST_CASE("collision that can't be used gets no bodies, and it's said once for each asset") {
    Scene scene(2);
    scene.write_source("content/props/ramp.gltf", ramp());
    scene.write_source("content/props/other.gltf", ramp());
    scene.cook();
    const AssetId corrupt = de::asset::collision_asset_id(scene.id_of("props/ramp.gltf"));
    const AssetId from_elsewhere = de::asset::collision_asset_id(scene.id_of("props/other.gltf"));
    // A flipped bit, somewhere only the content hash would notice.
    tamper_with(scene, corrupt, false, [](CollisionAsset& asset, std::span<std::byte> file) {
        file[asset.shape.offset + asset.shape.count / 2] ^= std::byte{1};
    });
    // Bytes from another Jolt, the file itself good.
    tamper_with(scene, from_elsewhere, true, [](CollisionAsset& asset, std::span<std::byte>) { ++asset.shape_format; });

    const AssetHandle corrupt_handle = scene.assets.load<CollisionAsset>(corrupt);
    const AssetHandle elsewhere_handle = scene.assets.load<CollisionAsset>(from_elsewhere);
    const AssetHandle missing_handle = scene.assets.load<CollisionAsset>(AssetId::from_value(0x51));
    scene.assets.wait_all();
    scene.held.insert(scene.held.end(), {corrupt_handle, elsewhere_handle, missing_handle});
    REQUIRE(scene.assets.state(corrupt_handle) == AssetState::Ready);
    REQUIRE(scene.assets.state(elsewhere_handle) == AssetState::Ready);
    REQUIRE(scene.assets.state(missing_handle) == AssetState::Failed);

    const flecs::entity entities[] = {
        scene.instance(corrupt_handle, Transform{}),
        scene.instance(corrupt_handle, Transform{.position = {50.0, 0.0, 0.0}}),
        scene.instance(elsewhere_handle, Transform{}),
        scene.instance(elsewhere_handle, Transform{.position = {50.0, 0.0, 0.0}}),
        scene.instance(missing_handle, Transform{}),
        scene.instance(AssetHandle{}, Transform{}), // what a corrupt cell's skipped dependency leaves
    };
    PhysicsErrors errors; // NOLINT(misc-const-correctness): the log writes to it
    scene.ticks(3);
    CHECK(errors.count == 4);
    CHECK(scene.physics.stats().bodies == 0);
    CHECK(scene.physics.stats().shapes == 0);
    for (const flecs::entity entity : entities) {
        entity.destruct(); // nothing to take away, and that's fine
    }
    scene.ticks(1);
}

TEST_CASE("a physics with no asset registry gives mesh colliders no bodies") {
    Physics physics;
    World world = World::create({}).value();
    physics.add_to_world(world);
    const flecs::entity entity = world.ecs().entity();
    entity.set(MeshCollider{.collision = {.index = 3, .generation = 1}});
    de::world::place(entity, Transform{});
    world.tick();
    world.tick();
    CHECK(physics.stats().bodies == 0);
    entity.destruct();
    world.tick();
}

TEST_CASE("collision cooked again reaches the bodies that use it, and a bad file that's mended does too") {
    Scene scene(2, true);
    const auto settle = [&] {
        for (int i = 0; i < 4; ++i) {
            scene.assets.update();
            scene.assets.wait_all();
        }
    };
    const AssetHandle collision = scene.model("ramp", ramp());
    const flecs::entity entity = scene.instance(collision, Transform{.position = {0.37, 0.0, 0.19}});
    scene.ticks(1);
    CHECK(scene.surface_y({2.5, 50.0, 0.46}) == doctest::Approx(2.13 * 0.25 + 1.125).epsilon(1e-3));

    // The same mesh two meters up, in the source. Its body keeps its entity and gets the new shape, a tick or so later.
    scene.write_source("content/props/ramp.gltf", ramp(2.0f));
    scene.cook();
    settle();
    REQUIRE(scene.assets.revision(collision) == 2);
    scene.world.tick();
    scene.world.tick();
    CHECK(scene.physics.stats().bodies == 1);
    CHECK(scene.physics.stats().shapes == 1);
    CHECK(scene.surface_y({2.5, 50.0, 0.46}) == doctest::Approx(2.13 * 0.25 + 3.125).epsilon(1e-3));
    QueryHit hit;
    REQUIRE(scene.physics.ray_cast({2.5, 50.0, 0.46}, {0.0f, -1.0f, 0.0f}, 100.0f, hit));
    CHECK(hit.entity == entity.id());

    // Corrupted: said so, and it has no body, until a cook mends it.
    tamper_with(scene, de::asset::collision_asset_id(scene.id_of("props/ramp.gltf")), false,
                [](CollisionAsset& asset, std::span<std::byte> file) {
                    file[asset.shape.offset + asset.shape.count / 2] ^= std::byte{1};
                });
    settle();
    REQUIRE(scene.assets.revision(collision) == 3);
    scene.world.tick();
    scene.world.tick();
    CHECK(scene.physics.stats().bodies == 0);
    de::cooker::CookOptions options;
    options.force = true;
    REQUIRE(de::cooker::cook(scene.vfs, scene.jobs, nullptr, options).ok());
    settle();
    REQUIRE(scene.assets.revision(collision) == 4);
    scene.world.tick();
    scene.world.tick();
    CHECK(scene.physics.stats().bodies == 1);
    CHECK(scene.surface_y({2.5, 50.0, 0.46}) == doctest::Approx(2.13 * 0.25 + 3.125).epsilon(1e-3));
}

TEST_CASE("every byte a mesh collider's memory took comes back") {
    const uint64_t live_before = de::core::memory_stats(de::core::MemoryTag::Physics).live_bytes;
    {
        Scene scene(0);
        scene.write_source("content/props/ramp.gltf", ramp());
        scene.write_source("content/world/a.cell", R"({"coord": [0, 0], "entities": [
            {"mesh": "../props/ramp.gltf", "position": [20.37, 0, 20.19], "scale": 3, "collision": "mesh"}]})");
        scene.cook();
        const flecs::entity cell = de::world::load_cell(scene.world.ecs(), scene.id_of("world/a.cell"));
        scene.assets.wait_all();
        scene.ticks(1);
        CHECK(scene.physics.stats().shapes == 2);
        cell.destruct();
        scene.ticks(3);
        CHECK(scene.physics.stats().shapes == 0);
    }
    CHECK(de::core::memory_stats(de::core::MemoryTag::Physics).live_bytes == live_before);
}
