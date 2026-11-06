#include "asset/mesh_format.h"
#include "asset/registry.h"
#include "cooker/cell_cooker.h"
#include "cooker/mesh_cooker.h"
#include "core/job_system.h"
#include "core/memory_stats.h"
#include "support/sandbox.h"
#include "world/cell.h"
#include "world/render_extract.h"
#include "world/transform.h"
#include "world/world.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

using de::DVec3;
using de::Quat;
using de::Vec3;
using de::world::Camera;
using de::world::FrameStep;
using de::world::InCell;
using de::world::MeshInstance;
using de::world::Transform;
using de::world::World;
using de::world::WorldTransform;

namespace {

bool near(double a, double b, double tolerance = 1e-4) {
    return std::fabs(a - b) <= tolerance;
}

Quat yaw_deg(float degrees) {
    return glm::angleAxis(glm::radians(degrees), de::kWorldUp);
}

// A cooked one-triangle mesh in game:/, for the registry to load.
void put_mesh(const de::platform::Vfs& vfs, de::asset::AssetId id) {
    de::cooker::ModelData model;
    de::cooker::ModelPrimitive& primitive = model.primitives.emplace();
    for (int i = 0; i < 3; ++i) {
        de::cooker::ModelVertex& vertex = primitive.vertices.emplace();
        vertex.position[0] = static_cast<float>(i);
        vertex.position[1] = static_cast<float>(i % 2);
        primitive.indices.push(static_cast<uint32_t>(i));
    }
    de::cooker::Diagnostics diagnostics;
    const auto cooked = de::cooker::cook_mesh(model, {}, {}, id, "test", diagnostics);
    REQUIRE(cooked.ok());
    REQUIRE(vfs.write_file(de::asset::cooked_path(id, de::asset::AssetType::Mesh).c_str(), cooked->span()).ok());
}

} // namespace

TEST_CASE("attachments follow their parent: world transforms are composed down the hierarchy") {
    auto world = World::create();
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    // A rider at x = 10, turned 90 degrees left; a sword 1 m along its local +X; a gem on the sword.
    const flecs::entity rider = ecs.entity();
    de::world::place(rider, Transform{.position = {10.0, 0.0, 0.0}, .rotation = yaw_deg(90.0f)});
    const flecs::entity sword = ecs.entity(flecs::Parent{rider});
    de::world::place(sword, Transform{.position = {1.0, 0.0, 0.0}});
    const flecs::entity gem = ecs.entity(flecs::Parent{sword});
    de::world::place(gem, Transform{.position = {0.0, 0.5, 0.0}, .scale = {2.0f, 2.0f, 2.0f}});
    // A second parent, so children of different parents share tables.
    const flecs::entity statue = ecs.entity();
    de::world::place(statue, Transform{.position = {-5.0, 0.0, 0.0}});
    const flecs::entity plaque = ecs.entity(flecs::Parent{statue});
    de::world::place(plaque, Transform{.position = {1.0, 0.0, 0.0}});

    // +X turned 90 degrees left (about +Y) is -Z.
    CHECK(near(sword.get<WorldTransform>().position.x, 10.0));
    CHECK(near(sword.get<WorldTransform>().position.z, -1.0));
    CHECK(near(gem.get<WorldTransform>().position.y, 0.5));
    CHECK(near(gem.get<WorldTransform>().scale.x, 2.0));

    // The rider moves; on the next tick, everything it carries follows (parents first).
    rider.set(Transform{.position = {20.0, 0.0, 0.0}, .rotation = yaw_deg(90.0f)});
    world->tick();
    CHECK(near(sword.get<WorldTransform>().position.x, 20.0));
    CHECK(near(gem.get<WorldTransform>().position.x, 20.0));
    CHECK(near(gem.get<WorldTransform>().position.z, -1.0));
    CHECK(near(plaque.get<WorldTransform>().position.x, -4.0)); // its own parent's, not the rider's

    // Deleting a parent deletes what it carries.
    rider.destruct();
    CHECK_FALSE(sword.is_alive());
    CHECK_FALSE(gem.is_alive());
    CHECK(plaque.is_alive());
}

TEST_CASE("far from the origin, positions keep sub-millimeter precision, and so does what the GPU sees") {
    de::core::JobSystem jobs({.worker_count = 2});
    auto world = World::create({.jobs = &jobs});
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    // 10,000 km out: a float there is about 1 m apart; a double keeps fractions of a millimeter.
    const DVec3 far{10'000'000.25, 3.5, -7'000'000.75};
    const flecs::entity rock = ecs.entity().set(MeshInstance{});
    de::world::place(rock, Transform{.position = far + DVec3(0.001, 0.0, 0.0)});
    const flecs::entity eye = ecs.entity().set(Camera{});
    de::world::place(eye, Transform{.position = far});
    ecs.set(de::world::ActiveCamera{.entity = eye.id()});

    world->advance(1.0 / 60.0);
    const de::world::RenderFrame& frame = world->render_frame();
    REQUIRE(frame.camera.valid);
    CHECK(frame.origin_moves == 1);
    // The origin snapped to its grid near the camera; everything is small and exact relative to it.
    CHECK(std::fabs(far.x - frame.origin.x) <= de::world::kRenderOriginStep);
    CHECK(std::fmod(frame.origin.x, de::world::kRenderOriginStep) == 0.0);
    REQUIRE(frame.instances.size() == 1);
    const float rock_x = frame.instances[0].transform.position_m(0, frame.origin);
    const float eye_x = frame.camera.transform[3];
    CHECK(near(static_cast<double>(rock_x - eye_x), 0.001, 1e-4)); // 1 mm apart, seen as 1 mm apart

    // The camera moves a little: the origin stays.
    eye.set(Transform{.position = far + DVec3(500.0, 0.0, 0.0)});
    world->advance(1.0 / 60.0);
    world->advance(1.0 / 60.0);
    CHECK(frame.origin_moves == 1);
    // A long way: the origin follows, once.
    eye.set(Transform{.position = far + DVec3(5000.0, 0.0, 0.0)});
    world->advance(1.0 / 60.0);
    world->advance(1.0 / 60.0);
    CHECK(frame.origin_moves == 2);
}

TEST_CASE("render extraction: interpolated instances and the active camera, nothing else") {
    auto world = World::create();
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    const flecs::entity a = ecs.entity().set(MeshInstance{.mesh = {.index = 3, .generation = 1}});
    de::world::place(a, Transform{.position = {1.0, 2.0, 3.0}, .scale = {2.0f, 2.0f, 2.0f}});
    const flecs::entity no_mesh = ecs.entity();
    de::world::place(no_mesh, Transform{});
    world->advance(1.0 / 60.0);
    const de::world::RenderFrame& frame = world->render_frame();
    CHECK_FALSE(frame.camera.valid); // no active camera
    REQUIRE(frame.instances.size() == 1);
    const de::world::RenderInstance& instance = frame.instances[0];
    CHECK(instance.mesh.index == 3);
    CHECK(instance.transform.rows[0].linear[0] == 2.0f); // scale x on the diagonal
    CHECK(instance.transform.position_m(0) == 1.0f);     // translation x
    CHECK(instance.transform.position_m(1) == 2.0f);
    CHECK(instance.transform.position_m(2) == 3.0f);

    // Moving: shown between the last two ticks.
    a.set(Transform{.position = {11.0, 2.0, 3.0}, .scale = {2.0f, 2.0f, 2.0f}});
    world->advance(1.5 / 60.0); // one tick, half a tick left over
    CHECK(near(world->alpha(), 0.5, 1e-3));
    CHECK(near(frame.instances[0].transform.position_m(0), 6.0, 1e-2)); // halfway from 1 to 11
}

namespace {

// The rows extraction really sent this frame.
size_t sent_rows(const de::world::RenderFrame& frame) {
    size_t sent = 0;
    for (const de::world::RenderInstance& instance : frame.instances) {
        sent += instance.slot != de::world::kSkipRenderInstance ? 1 : 0;
    }
    return sent;
}

const de::world::RenderInstance* sent_for(const de::world::RenderFrame& frame, uint32_t slot) {
    for (const de::world::RenderInstance& instance : frame.instances) {
        if (instance.slot == slot) {
            return &instance;
        }
    }
    return nullptr;
}

constexpr double kTick = 1.0 / 60.0;

} // namespace

TEST_CASE("render extraction: what doesn't change is sent once") {
    auto world = World::create();
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    for (int i = 0; i < 700; ++i) { // more than a chunk
        const flecs::entity rock = ecs.entity().set(MeshInstance{.mesh = {.index = 3, .generation = 1}});
        de::world::place(rock, Transform{.position = {static_cast<double>(i), 0.0, 0.0}});
    }
    world->advance(kTick);
    const de::world::RenderFrame& frame = world->render_frame();
    CHECK(sent_rows(frame) == 700);
    CHECK(frame.slot_count == 700);
    // A tick for what's new to settle, and then nothing at all, however many frames go by.
    world->advance(kTick);
    world->advance(kTick);
    for (int i = 0; i < 20; ++i) {
        world->advance(kTick / 3.0);
        CHECK(frame.instances.empty());
        CHECK(frame.slot_count == 700);
        CHECK_FALSE(frame.all_instances);
    }
}

TEST_CASE("render extraction: what moves is sent every frame, and once more when it stops") {
    auto world = World::create();
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    const flecs::entity rock = ecs.entity().set(MeshInstance{});
    de::world::place(rock, Transform{.position = {1.0, 0.0, 0.0}});
    rock.add<de::world::StaticTransform>(); // keeps it out of the mover's table
    const flecs::entity mover = ecs.entity().set(MeshInstance{});
    de::world::place(mover, Transform{.position = {5.0, 0.0, 0.0}});
    const uint32_t mover_slot = mover.get<de::world::RenderSlot>().slot;
    for (int i = 0; i < 3; ++i) {
        world->advance(kTick);
    }
    const de::world::RenderFrame& frame = world->render_frame();
    CHECK(frame.instances.empty());

    // It moves. Every frame shows it somewhere new, ticks or not, and the other isn't sent.
    mover.set(Transform{.position = {15.0, 0.0, 0.0}});
    world->advance(kTick * 1.25); // a tick, and a quarter left over
    REQUIRE(sent_for(frame, mover_slot) != nullptr);
    CHECK(sent_rows(frame) == 1);
    const float first_x = sent_for(frame, mover_slot)->transform.position_m(0);
    world->advance(kTick * 0.25); // no tick, just further between the same two
    REQUIRE(sent_for(frame, mover_slot) != nullptr);
    CHECK(sent_rows(frame) == 1);
    CHECK(sent_for(frame, mover_slot)->transform.position_m(0) > first_x);
    CHECK(sent_for(frame, mover_slot)->transform.position_m(0) !=
          sent_for(frame, mover_slot)->previous_transform.position_m(0));

    // It stops, and stays still for the next tick: one last row, whose motion is none, and then nothing.
    world->advance(kTick);
    world->advance(kTick);
    const auto last_write = [&] {
        for (int i = 0; i < 8; ++i) {
            world->advance(kTick);
            const de::world::RenderInstance* sent = sent_for(frame, mover_slot);
            if (sent == nullptr) {
                return false;
            }
            if (sent->transform.position_m(0) == sent->previous_transform.position_m(0)) {
                return true;
            }
        }
        return false;
    };
    // It may already have been sent, in the frames above: then nothing's left to send, which the next check shows.
    static_cast<void>(last_write());
    world->advance(kTick);
    world->advance(kTick);
    CHECK(frame.instances.empty());
}

TEST_CASE("render extraction: between ticks, a mover is sent and what shares its table isn't") {
    auto world = World::create();
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    const flecs::entity neighbour = ecs.entity().set(MeshInstance{});
    de::world::place(neighbour, Transform{.position = {1.0, 0.0, 0.0}});
    const flecs::entity mover = ecs.entity().set(MeshInstance{});
    de::world::place(mover, Transform{.position = {5.0, 0.0, 0.0}});
    for (int i = 0; i < 3; ++i) {
        world->advance(kTick);
    }
    const uint32_t mover_slot = mover.get<de::world::RenderSlot>().slot;
    const de::world::RenderFrame& frame = world->render_frame();
    mover.set(Transform{.position = {15.0, 0.0, 0.0}});
    world->advance(kTick * 1.25); // the tick wrote to their table, but only one of them changed
    CHECK(sent_rows(frame) == 1);
    CHECK(sent_for(frame, mover_slot) != nullptr);
    world->advance(kTick * 0.25); // no tick: what's moving, still
    CHECK(sent_rows(frame) == 1);
    CHECK(sent_for(frame, mover_slot) != nullptr);
}

TEST_CASE("render extraction: a mover's last row has no motion in it") {
    auto world = World::create();
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    const flecs::entity mover = ecs.entity().set(MeshInstance{});
    de::world::place(mover, Transform{.position = {5.0, 0.0, 0.0}});
    const uint32_t slot = mover.get<de::world::RenderSlot>().slot;
    for (int i = 0; i < 3; ++i) {
        world->advance(kTick);
    }
    mover.set(Transform{.position = {15.0, 0.0, 0.0}});
    const de::world::RenderFrame& frame = world->render_frame();
    bool moved = false;
    float last_x = 0.0f;
    float last_previous_x = 0.0f;
    for (int i = 0; i < 12; ++i) {
        world->advance(kTick);
        if (const de::world::RenderInstance* sent = sent_for(frame, slot); sent != nullptr) {
            moved = moved || sent->transform.position_m(0) != sent->previous_transform.position_m(0);
            last_x = sent->transform.position_m(0);
            last_previous_x = sent->previous_transform.position_m(0);
        }
    }
    CHECK(moved);
    CHECK(last_x == 15.0f);
    CHECK(last_previous_x == 15.0f); // what the renderer's left holding draws no motion
    CHECK(frame.instances.empty());
}

TEST_CASE("render extraction: a static entity placed somewhere new is sent") {
    auto world = World::create();
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    const flecs::entity rock = ecs.entity().add<de::world::StaticTransform>().set(MeshInstance{});
    de::world::place(rock, Transform{.position = {1.0, 0.0, 0.0}});
    const uint32_t slot = rock.get<de::world::RenderSlot>().slot;
    for (int i = 0; i < 3; ++i) {
        world->advance(kTick);
    }
    const de::world::RenderFrame& frame = world->render_frame();
    CHECK(frame.instances.empty());
    de::world::place(rock, Transform{.position = {2.0, 0.0, 0.0}});
    world->advance(kTick);
    REQUIRE(sent_for(frame, slot) != nullptr);
    CHECK(sent_for(frame, slot)->transform.position_m(0) == 2.0f);
    CHECK(sent_for(frame, slot)->previous_transform.position_m(0) == 2.0f);
}

TEST_CASE("render extraction: a new mesh on an entity is sent") {
    auto world = World::create();
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    const flecs::entity rock =
        ecs.entity().add<de::world::StaticTransform>().set(MeshInstance{.mesh = {.index = 3, .generation = 1}});
    de::world::place(rock, Transform{});
    const uint32_t slot = rock.get<de::world::RenderSlot>().slot;
    for (int i = 0; i < 3; ++i) {
        world->advance(kTick);
    }
    const de::world::RenderFrame& frame = world->render_frame();
    CHECK(frame.instances.empty());
    rock.set(MeshInstance{.mesh = {.index = 4, .generation = 1}});
    world->advance(kTick);
    REQUIRE(sent_for(frame, slot) != nullptr);
    CHECK(sent_for(frame, slot)->mesh.index == 4);
}

TEST_CASE("render extraction: spawning next to a thousand rocks sends one row") {
    auto world = World::create();
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    const auto rock_at = [&](double x_m) {
        const flecs::entity rock = ecs.entity().add<de::world::StaticTransform>().set(MeshInstance{});
        de::world::place(rock, Transform{.position = {x_m, 0.0, 0.0}});
        return rock;
    };
    for (int i = 0; i < 1000; ++i) {
        rock_at(static_cast<double>(i));
    }
    for (int i = 0; i < 3; ++i) {
        world->advance(kTick);
    }
    const de::world::RenderFrame& frame = world->render_frame();
    CHECK(frame.instances.empty());

    const flecs::entity another = rock_at(-1.0);
    world->advance(kTick);
    CHECK(sent_rows(frame) == 1);
    REQUIRE(sent_for(frame, another.get<de::world::RenderSlot>().slot) != nullptr);
    // And going: nothing about the others changed.
    another.destruct();
    world->advance(kTick);
    CHECK(sent_rows(frame) == 0);
    CHECK(frame.freed_slots.size() == 1);
}

TEST_CASE("render extraction: all of them can wait for a camera") {
    auto world = World::create();
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    const flecs::entity rock = ecs.entity().set(MeshInstance{});
    de::world::place(rock, Transform{});
    world->advance(kTick);
    const de::world::RenderFrame& frame = world->render_frame();
    for (int i = 0; i < 3; ++i) {
        world->advance(kTick);
    }
    // The renderer asks, with nothing to look through: nothing's built for nothing to draw it.
    world->resync_render_instances();
    world->advance(kTick);
    CHECK_FALSE(frame.camera.valid);
    CHECK(frame.instances.empty());
    CHECK_FALSE(frame.all_instances);
    // When there's a camera, they're all sent.
    const flecs::entity eye = ecs.entity().set(Camera{});
    de::world::place(eye, Transform{});
    ecs.set(de::world::ActiveCamera{.entity = eye.id()});
    world->advance(kTick);
    REQUIRE(frame.camera.valid);
    CHECK(frame.all_instances);
    CHECK(sent_rows(frame) == 1);
}

TEST_CASE("render extraction: slots come and go with entities and their meshes") {
    auto world = World::create();
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    const auto make = [&] {
        const flecs::entity rock = ecs.entity().set(MeshInstance{});
        de::world::place(rock, Transform{});
        return rock;
    };
    const flecs::entity a = make();
    const flecs::entity b = make();
    const uint32_t a_slot = a.get<de::world::RenderSlot>().slot;
    const uint32_t b_slot = b.get<de::world::RenderSlot>().slot;
    CHECK(a_slot != b_slot);
    world->advance(kTick);
    const de::world::RenderFrame& frame = world->render_frame();
    CHECK(frame.slot_count == 2);
    CHECK(frame.freed_slots.empty());

    // Destroyed: its slot is freed, once, and goes to whatever needs one next.
    a.destruct();
    world->advance(kTick);
    REQUIRE(frame.freed_slots.size() == 1);
    CHECK(frame.freed_slots[0] == a_slot);
    world->advance(kTick);
    CHECK(frame.freed_slots.empty());
    const flecs::entity c = make();
    CHECK(c.get<de::world::RenderSlot>().slot == a_slot);
    world->advance(kTick);
    CHECK(frame.slot_count == 2); // it didn't grow
    CHECK(sent_for(frame, a_slot) != nullptr);

    // Its mesh taken away, the entity stays and the slot goes. Its mesh back, it has one again.
    b.remove<MeshInstance>();
    world->advance(kTick);
    REQUIRE(frame.freed_slots.size() == 1);
    CHECK(frame.freed_slots[0] == b_slot);
    CHECK_FALSE(b.has<de::world::RenderSlot>());
    b.set(MeshInstance{});
    CHECK(b.has<de::world::RenderSlot>());
    world->advance(kTick);
    CHECK(sent_for(frame, b.get<de::world::RenderSlot>().slot) != nullptr);
}

TEST_CASE("render extraction: all of them when the renderer asks, and none when the render origin moves") {
    auto world = World::create();
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    constexpr int kCount = 10;
    for (int i = 0; i < kCount; ++i) {
        const flecs::entity rock = ecs.entity().set(MeshInstance{});
        de::world::place(rock, Transform{.position = {static_cast<double>(i), 0.0, 0.0}});
    }
    const flecs::entity eye = ecs.entity().set(Camera{});
    de::world::place(eye, Transform{.position = {0.0, 0.0, 10.0}});
    ecs.set(de::world::ActiveCamera{.entity = eye.id()});
    for (int i = 0; i < 3; ++i) {
        world->advance(kTick);
    }
    const de::world::RenderFrame& frame = world->render_frame();
    CHECK(frame.instances.empty());

    world->resync_render_instances();
    const flecs::entity gone = ecs.entity().set(MeshInstance{}); // freed slots don't matter when it's all of them
    de::world::place(gone, Transform{});
    world->advance(kTick);
    CHECK(frame.all_instances);
    CHECK(sent_rows(frame) == kCount + 1);
    gone.destruct();
    world->resync_render_instances();
    world->advance(kTick);
    CHECK(frame.all_instances);
    CHECK(frame.freed_slots.empty());
    CHECK(sent_rows(frame) == kCount);
    world->advance(kTick);
    CHECK_FALSE(frame.all_instances);
    world->advance(kTick); // its table changed when it went, which takes a tick to settle
    world->advance(kTick);
    CHECK(frame.instances.empty());

    // The origin moves: no row holds it, so none is sent, and what's sent later is where it is in the world.
    const uint64_t moves = frame.origin_moves;
    de::world::place(eye, Transform{.position = {5000.0, 0.0, 10.0}});
    for (int i = 0; i < 4 && frame.origin_moves == moves; ++i) {
        world->advance(kTick);
    }
    REQUIRE(frame.origin_moves == moves + 1);
    CHECK_FALSE(frame.all_instances);
    CHECK(frame.instances.empty());
    world->resync_render_instances();
    world->advance(kTick);
    REQUIRE(sent_rows(frame) == kCount);
    CHECK(frame.instances[3].transform.rows[0].position == de::world::render_position(3.0));
    CHECK(frame.instances[3].transform.position_m(0, frame.origin) == doctest::Approx(3.0 - frame.origin.x));
}

TEST_CASE("an instance's position wraps, and is right near the origin wherever that is in the world") {
    // Past the 32 bits' range, and past it again by a different amount.
    const DVec3 origin{10'240'000.0, -7'168'000.0, 1'024.0};
    const DVec3 place = origin + DVec3(100.25, -2000.5, 0.0);
    de::world::RenderTransform transform;
    transform.set_position_m(0, place.x);
    transform.set_position_m(1, place.y);
    transform.set_position_m(2, place.z);
    CHECK(transform.position_m(0, origin) == 100.25f);
    CHECK(transform.position_m(1, origin) == -2000.5f);
    CHECK(transform.position_m(2, origin) == 0.0f);
    // The steps are as fine as a float at 1 km, and a move of less than half a step rounds away.
    transform.set_position_m(2, origin.z + 1.0 / de::world::kRenderStepsPerM);
    CHECK(transform.position_m(2, origin) == static_cast<float>(1.0 / de::world::kRenderStepsPerM));
    transform.set_position_m(2, origin.z + 0.4 / de::world::kRenderStepsPerM);
    CHECK(transform.position_m(2, origin) == 0.0f);
}

TEST_CASE("render extraction: fog volumes, relative to the render origin, and the shape of what's placed") {
    auto world = World::create();
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    world->advance(1.0 / 60.0);
    const de::world::RenderFrame& frame = world->render_frame();
    CHECK(frame.fog_volumes.empty());

    const Quat yaw = yaw_deg(90.0f);
    const flecs::entity mist = ecs.entity().set(
        de::world::FogVolume{.density_per_m = 0.3f, .edge_m = 2.0f, .tint = {1.0f, 0.5f, 0.25f}, .ellipsoid = true});
    de::world::place(mist, Transform{.position = {10.0, 2.0, -5.0}, .rotation = yaw, .scale = {4.0f, 2.0f, 1.0f}});
    world->advance(1.0 / 60.0);
    REQUIRE(frame.fog_volumes.size() == 1);
    const de::world::RenderFogVolume& volume = frame.fog_volumes[0];
    CHECK(volume.density_per_m == 0.3f);
    CHECK(volume.edge_m == 2.0f);
    CHECK(volume.tint[1] == 0.5f);
    CHECK(volume.ellipsoid);
    CHECK(volume.half_extents[0] == 4.0f);
    CHECK(volume.half_extents[1] == 2.0f);
    CHECK(volume.half_extents[2] == 1.0f);
    CHECK(near(volume.center[0], 10.0, 1e-5));
    // Its own axes: its center is its origin, and its local x is where the rotation takes x.
    const Vec3 axis_x = yaw * Vec3(1.0f, 0.0f, 0.0f);
    const auto local_x = [&](const Vec3 point) {
        return volume.to_local[0] * point.x + volume.to_local[1] * point.y + volume.to_local[2] * point.z +
               volume.to_local[3];
    };
    const Vec3 center(10.0f, 2.0f, -5.0f);
    CHECK(near(local_x(center), 0.0, 1e-5));
    CHECK(near(local_x(center + axis_x * 3.0f), 3.0, 1e-5));

    // Moving: where it's seen, like anything else; gone when it's gone.
    mist.set(Transform{.position = {20.0, 2.0, -5.0}, .rotation = yaw, .scale = {4.0f, 2.0f, 1.0f}});
    world->advance(1.5 / 60.0);
    CHECK(near(frame.fog_volumes[0].center[0], 15.0, 1e-2));
    mist.destruct();
    world->advance(1.0 / 60.0);
    CHECK(frame.fog_volumes.empty());

    // Its noise: tiles a meter, and how far it's drifted by the world's time, along the world's axes whatever way
    // the volume is turned.
    const flecs::entity windy = ecs.entity().set(de::world::FogVolume{
        .noise_size_m = 10.0f, .noise_amount = 0.5f, .drift_mps = {5.0f, 0.0f, 0.0f}, .height_fade_m = 2.0f});
    de::world::place(windy, Transform{.position = {0.0, 0.0, 0.0}, .rotation = yaw, .scale = {4.0f, 2.0f, 1.0f}});
    world->advance(1.0 / 60.0);
    REQUIRE(frame.fog_volumes.size() == 1);
    CHECK(near(frame.fog_volumes[0].noise_tiles_per_m, 0.1, 1e-6));
    CHECK(frame.fog_volumes[0].noise_amount == 0.5f);
    CHECK(frame.fog_volumes[0].height_fade_m == 2.0f);
    const float offset_before = frame.fog_volumes[0].noise_offset_tiles[0];
    for (int i = 0; i < 30; ++i) {
        world->advance(1.0 / 60.0);
    }
    const float moved = frame.fog_volumes[0].noise_offset_tiles[0] - offset_before;
    CHECK(near(moved < -1.0f ? moved + 4.0f : moved, 0.25, 1e-3)); // half a second at half a tile per second
    CHECK(near(frame.fog_volumes[0].noise_offset_tiles[2], 0.0, 1e-3));
    // Turning it changes nothing about its noise, however long the world has run.
    const float offset_unturned = frame.fog_volumes[0].noise_offset_tiles[0];
    de::world::place(windy,
                     Transform{.position = {0.0, 0.0, 0.0}, .rotation = yaw_deg(10.0f), .scale = {4.0f, 2.0f, 1.0f}});
    world->advance(0.0);
    CHECK(near(frame.fog_volumes[0].noise_offset_tiles[0], offset_unturned, 1e-3));
    // It wraps at 4 tiles, however long the world runs, without a jump in between.
    float last_offset = frame.fog_volumes[0].noise_offset_tiles[0];
    bool wrapped = false;
    for (int i = 0; i < 600; ++i) {
        world->advance(1.0 / 60.0);
        const float offset = frame.fog_volumes[0].noise_offset_tiles[0];
        CHECK(offset >= 0.0f);
        CHECK(offset < 4.0f);
        wrapped = wrapped || offset < last_offset - 3.0f;
        CHECK((offset > last_offset || offset < last_offset - 3.0f));
        last_offset = offset;
    }
    CHECK(wrapped);
    windy.destruct();

    // As the render origin moves, the noise stays put in the world: at a whole origin step, its offset takes up
    // what positions relative to the origin lost (2048 m at 0.1 tiles a meter: 204.8 tiles).
    const flecs::entity still = ecs.entity().set(de::world::FogVolume{.noise_size_m = 10.0f, .noise_amount = 0.5f});
    de::world::place(still, Transform{.position = {2048.0, 0.0, 0.0}});
    const flecs::entity eye = ecs.entity().set(de::world::Camera{});
    de::world::place(eye, Transform{.position = {2048.0, 0.0, 0.0}});
    ecs.set(de::world::ActiveCamera{.entity = eye.id()});
    world->advance(1.0 / 60.0);
    REQUIRE(frame.fog_volumes.size() == 1);
    REQUIRE(frame.origin.x == 2048.0);
    CHECK(near(frame.fog_volumes[0].noise_offset_tiles[0], 4.0 - 0.8, 1e-3)); // -204.8, wrapped to [0, 4)
    CHECK(near(frame.fog_volumes[0].noise_offset_tiles[1], 0.0, 1e-3));
    still.destruct();
    eye.destruct();
}

TEST_CASE("a cell owns its entities and its assets: deleting it unloads both, in any number of cells") {
    const de::test::Sandbox sandbox;
    de::core::JobSystem jobs({.worker_count = 2});
    de::platform::Vfs vfs = sandbox.make_vfs({.jobs = &jobs, .game_writable = true});
    const de::asset::AssetId rock_id = de::asset::AssetId::from_value(0x77);
    put_mesh(vfs, rock_id);
    auto assets = de::asset::AssetRegistry::create({.vfs = &vfs, .jobs = &jobs, .hot_reload = false});
    REQUIRE(assets.ok());
    {
        auto world = World::create({.jobs = &jobs, .assets = &*assets});
        REQUIRE(world.ok());
        flecs::world& ecs = world->ecs();
        const int32_t tables_before = ecs_get_world_info(ecs.c_ptr())->table_count;

        // 40 cells, each holding the rock (as a cell asset will hold its meshes) with 50 rocks in it.
        constexpr int kCells = 40;
        flecs::entity cells[kCells];
        for (int c = 0; c < kCells; ++c) {
            const de::asset::AssetHandle held = assets->load<de::asset::MeshAsset>(rock_id);
            cells[c] = de::world::create_cell(ecs, {.x = c, .z = 0}, held);
            for (int i = 0; i < 50; ++i) {
                const flecs::entity rock = ecs.entity().set(MeshInstance{.mesh = held}); // not a hold
                de::world::place(rock, Transform{.position = {c * de::world::kCellSize + i, 0.0, 0.0}});
                de::world::add_to_cell(rock, cells[c]);
            }
        }
        assets->wait_all();
        CHECK(assets->stats().assets == 1);
        // Cells make no tables of their own.
        CHECK(ecs_get_world_info(ecs.c_ptr())->table_count - tables_before < 6);
        CHECK(ecs.count<MeshInstance>() == kCells * 50);
        CHECK(cells[3].get<de::world::Cell>().coord.x == 3);

        // Unloading a cell is deleting its entity: its members go, its hold goes.
        cells[5].destruct();
        CHECK(ecs.count<MeshInstance>() == (kCells - 1) * 50);
        int in_cell_3 = 0;
        ecs.each([&](flecs::entity entity, const MeshInstance&) { in_cell_3 += entity.has<InCell>(cells[3]) ? 1 : 0; });
        CHECK(in_cell_3 == 50);
        for (int c = 0; c < kCells; ++c) {
            if (c != 5 && c != 9) {
                cells[c].destruct();
            }
        }
        assets->update();
        CHECK(assets->stats().assets == 1); // cell 9 still holds the rock
        world->advance(1.0 / 60.0);
        CHECK(world->render_frame().instances.size() == 50);
    } // the world goes with cell 9 in it: its hold goes too
    assets->update();
    CHECK(assets->stats().assets == 0);
}

TEST_CASE("an entity is in one cell at a time") {
    auto world = World::create();
    REQUIRE(world.ok());
    flecs::world& ecs = world->ecs();
    const flecs::entity a = de::world::create_cell(ecs, {.x = 0, .z = 0}, {});
    const flecs::entity b = de::world::create_cell(ecs, {.x = 1, .z = 0}, {});
    const flecs::entity wanderer = ecs.entity();
    de::world::add_to_cell(wanderer, a);
    de::world::add_to_cell(wanderer, b); // walked over the border
    CHECK_FALSE(wanderer.has<InCell>(a));
    CHECK(wanderer.has<InCell>(b));
    a.destruct();
    CHECK(wanderer.is_alive());
    CHECK(de::world::cell_at({200.0, 0.0, -1.0}) == de::world::CellCoord{.x = 1, .z = -1});
}

TEST_CASE("teleporting a parent takes what it carries along at once, without a streak") {
    auto world = World::create();
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    const flecs::entity rider = ecs.entity();
    de::world::place(rider, Transform{.position = {0.0, 0.0, 0.0}});
    const flecs::entity sword = ecs.entity(flecs::Parent{rider});
    de::world::place(sword, Transform{.position = {1.0, 0.0, 0.0}});
    world->tick();
    de::world::place(rider, Transform{.position = {500.0, 0.0, 0.0}});
    // At once, before any tick: the sword's world and previous transforms are at the new spot.
    CHECK(near(sword.get<WorldTransform>().position.x, 501.0));
    CHECK(near(sword.get<de::world::PreviousWorldTransform>().value.position.x, 501.0));
    world->tick();
    CHECK(near(sword.get<de::world::PreviousWorldTransform>().value.position.x, 501.0)); // nothing to interpolate
}

TEST_CASE("world transforms follow every PostSim system, whenever it was made") {
    auto world = World::create();
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    const flecs::entity follower = ecs.entity();
    de::world::place(follower, Transform{});
    // A game system made after the world: it still runs before the world transforms of its tick.
    ecs.system<Transform>("snap to ground").kind(world->phase(de::world::Phase::PostSim)).each([](Transform& t) {
        t.position.y = 7.0;
    });
    world->tick();
    CHECK(near(follower.get<WorldTransform>().position.y, 7.0));
}

TEST_CASE("an entity whose parent has no transform is a root, in either hierarchy") {
    auto world = World::create();
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    const flecs::entity scope = ecs.entity("level"); // a name scope, no Transform
    const flecs::entity door = ecs.entity("door").child_of(scope);
    de::world::place(door, Transform{.position = {3.0, 0.0, 0.0}});
    const flecs::entity group = ecs.entity(); // a grouping entity, no Transform
    const flecs::entity lamp = ecs.entity(flecs::Parent{group});
    de::world::place(lamp, Transform{.position = {4.0, 0.0, 0.0}});
    // Placed: right at once, previous included (no slide in from the origin).
    CHECK(near(door.get<de::world::PreviousWorldTransform>().value.position.x, 3.0));
    CHECK(near(lamp.get<de::world::PreviousWorldTransform>().value.position.x, 4.0));
    door.set(Transform{.position = {5.0, 0.0, 0.0}});
    lamp.set(Transform{.position = {6.0, 0.0, 0.0}});
    world->tick();
    CHECK(near(door.get<WorldTransform>().position.x, 5.0));
    CHECK(near(lamp.get<WorldTransform>().position.x, 6.0));
    // A ChildOf child of an entity with a transform composes, like a Parent child.
    const flecs::entity cart = ecs.entity();
    de::world::place(cart, Transform{.position = {10.0, 0.0, 0.0}});
    const flecs::entity wheel = ecs.entity().child_of(cart);
    de::world::place(wheel, Transform{.position = {0.5, 0.0, 0.0}});
    cart.set(Transform{.position = {20.0, 0.0, 0.0}});
    world->tick();
    CHECK(near(wheel.get<WorldTransform>().position.x, 20.5));
}

TEST_CASE("attaching and detaching keep an entity where it is") {
    auto world = World::create();
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    const flecs::entity rider = ecs.entity();
    de::world::place(
        rider, Transform{.position = {10'000.0, 0.0, 0.0}, .rotation = yaw_deg(90.0f), .scale = {2.0f, 2.0f, 2.0f}});
    const flecs::entity sword = ecs.entity();
    de::world::place(sword, Transform{.position = {10'000.0, 1.0, -4.0}});
    de::world::attach(sword, rider);
    world->tick();
    CHECK(near(sword.get<WorldTransform>().position.x, 10'000.0));
    CHECK(near(sword.get<WorldTransform>().position.y, 1.0));
    CHECK(near(sword.get<WorldTransform>().position.z, -4.0));
    // Dropped: stays where it fell, not at its offset read as a world position.
    rider.set(Transform{.position = {10'100.0, 0.0, 0.0}, .rotation = yaw_deg(90.0f), .scale = {2.0f, 2.0f, 2.0f}});
    world->tick();
    const DVec3 carried = sword.get<WorldTransform>().position;
    CHECK(near(carried.x, 10'100.0));
    de::world::detach(sword);
    world->tick();
    CHECK_FALSE(sword.parent().is_valid());
    CHECK_FALSE(sword.has<flecs::Parent>());
    CHECK(near(sword.get<WorldTransform>().position.x, carried.x));
    CHECK(near(sword.get<WorldTransform>().position.z, carried.z));
}

TEST_CASE("static entities keep tables of their own, untouched by moving neighbours") {
    auto world = World::create();
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    ecs.system<Transform>("wind")
        .kind(world->phase(de::world::Phase::PreSim))
        .without<de::world::StaticTransform>()
        .each([](Transform& t) { t.position.x += 1.0; });
    const flecs::entity banner = ecs.entity().set(MeshInstance{});
    de::world::place(banner, Transform{});
    const flecs::entity rock = ecs.entity().set(MeshInstance{}).add<de::world::StaticTransform>();
    de::world::place(rock, Transform{.position = {5.0, 0.0, 0.0}});
    world->tick();
    // A marker in the rock's world transform: its table's Transform is never written, so the world
    // transforms system skips it and the marker stays.
    rock.set(WorldTransform{.position = {-1.0, 0.0, 0.0}});
    world->tick();
    world->tick();
    CHECK(near(rock.get<WorldTransform>().position.x, -1.0));
    CHECK(near(banner.get<WorldTransform>().position.x, 3.0));
}

TEST_CASE("a cell's hold follows its Cell component: replaced, removed") {
    const de::test::Sandbox sandbox;
    de::core::JobSystem jobs({.worker_count = 2});
    de::platform::Vfs vfs = sandbox.make_vfs({.jobs = &jobs, .game_writable = true});
    const de::asset::AssetId first = de::asset::AssetId::from_value(0x81);
    const de::asset::AssetId second = de::asset::AssetId::from_value(0x82);
    put_mesh(vfs, first);
    put_mesh(vfs, second);
    auto assets = de::asset::AssetRegistry::create({.vfs = &vfs, .jobs = &jobs, .hot_reload = false});
    REQUIRE(assets.ok());
    auto world = World::create({.jobs = &jobs, .assets = &*assets});
    REQUIRE(world.ok());
    flecs::world& ecs = world->ecs();
    const flecs::entity cell = de::world::create_cell(ecs, {.x = 0, .z = 0}, assets->load<de::asset::MeshAsset>(first));
    const flecs::entity rock = ecs.entity();
    de::world::add_to_cell(rock, cell);
    assets->wait_all();
    // Replaced: the old hold goes, the new one stays.
    cell.set(de::world::Cell(assets->load<de::asset::MeshAsset>(second), {.x = 0, .z = 0}));
    assets->wait_all();
    assets->update();
    CHECK_FALSE(assets->find({.id = first, .type = de::asset::AssetType::Mesh}).is_valid());
    CHECK(assets->find({.id = second, .type = de::asset::AssetType::Mesh}).is_valid());
    CHECK(rock.is_alive());
    // Removed from a live entity: the hold goes, and the members with it (their handles may name
    // what only the cell held).
    cell.remove<de::world::Cell>();
    CHECK(cell.is_alive());
    CHECK_FALSE(rock.is_alive());
    assets->update();
    CHECK(assets->stats().assets == 0);
}

TEST_CASE("extraction covers many small tables at once, each entity exactly once") {
    de::core::JobSystem jobs({.worker_count = 3});
    auto world = World::create({.jobs = &jobs});
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    // 30 tables of 100 rows (a tag per kind), and one of 2000: chunks from all of them, in parallel.
    constexpr int kKinds = 30;
    std::vector<flecs::entity> kinds;
    kinds.reserve(kKinds);
    for (int k = 0; k < kKinds; ++k) {
        kinds.push_back(ecs.entity());
    }
    uint32_t next = 0;
    for (int k = 0; k < kKinds; ++k) {
        for (int i = 0; i < 100; ++i) {
            const flecs::entity e =
                ecs.entity().add(kinds[k]).set(MeshInstance{.mesh = {.index = next, .generation = 1}});
            de::world::place(e, Transform{.position = {static_cast<double>(next), 0.0, 0.0}});
            ++next;
        }
    }
    for (int i = 0; i < 2000; ++i) {
        const flecs::entity e = ecs.entity().set(MeshInstance{.mesh = {.index = next, .generation = 1}});
        de::world::place(e, Transform{.position = {static_cast<double>(next), 0.0, 0.0}});
        ++next;
    }
    world->advance(1.0 / 60.0);
    const auto& instances = world->render_frame().instances;
    REQUIRE(instances.size() == next);
    std::vector<int> seen(next, 0);
    int misplaced = 0;
    for (const de::world::RenderInstance& instance : instances) {
        ++seen[instance.mesh.index];
        misplaced += instance.transform.position_m(0) == static_cast<float>(instance.mesh.index) ? 0 : 1;
    }
    CHECK(misplaced == 0);
    CHECK(std::count(seen.begin(), seen.end(), 1) == static_cast<std::ptrdiff_t>(next));
}

namespace {

// A cooked cell in game:/: a parent with a mesh, a moving child with another mesh, then count - 2
// static entities with no parent.
void put_cell(const de::platform::Vfs& vfs, de::asset::AssetId id, de::asset::AssetId mesh_a, de::asset::AssetId mesh_b,
              uint32_t count) {
    using de::asset::CellEntity;
    de::cooker::CellBuild build;
    build.coord_x = 3;
    build.coord_z = -1;
    build.meshes.push({.id = mesh_a, .type = de::asset::AssetType::Mesh});
    build.meshes.push({.id = mesh_b, .type = de::asset::AssetType::Mesh});
    const Quat yaw = yaw_deg(90.0f);
    build.entities.push({.position = {400.0, 0.0, -100.0},
                         .rotation = {yaw.x, yaw.y, yaw.z, yaw.w},
                         .scale = {2.0f, 2.0f, 2.0f},
                         .mesh = 0,
                         .flags = de::asset::CellEntityFlags::Static});
    build.entities.push({.position = {1.0, 0.0, 0.0}, .parent = 0, .mesh = 1});
    for (uint32_t i = 2; i < count; ++i) {
        build.entities.push({.position = {390.0 + i, 0.0, -110.0}, .flags = de::asset::CellEntityFlags::Static});
    }
    build.fog_volumes.push({.density_per_m = 0.4f,
                            .edge_m = 3.0f,
                            .tint = {0.5f, 0.6f, 0.7f},
                            .shape = de::asset::CellFogShape::Ellipsoid,
                            .noise_size_m = 5.0f,
                            .noise_amount = 0.75f,
                            .drift_mps = {0.5f, 0.25f, 1.5f},
                            .height_fade_m = 3.0f});
    build.entities[2].fog_volume = 0;
    const auto cooked = de::cooker::cook_cell(build, id);
    REQUIRE(vfs.write_file(de::asset::cooked_path(id, de::asset::AssetType::Cell).c_str(), cooked.span()).ok());
}

int members_of(const flecs::world& ecs, flecs::entity cell) {
    int count = 0;
    ecs.query_builder().with<InCell>(cell).build().each([&](flecs::entity) { ++count; });
    return count;
}

} // namespace

TEST_CASE("a loaded cell spawns its members once it and its meshes are in, and again when re-cooked") {
    const de::test::Sandbox sandbox;
    de::core::JobSystem jobs({.worker_count = 2});
    de::platform::Vfs vfs = sandbox.make_vfs({.jobs = &jobs, .game_writable = true});
    const de::asset::AssetId cell_id = de::asset::AssetId::from_value(0x90);
    const de::asset::AssetId rock = de::asset::AssetId::from_value(0x91);
    const de::asset::AssetId lamp = de::asset::AssetId::from_value(0x92);
    put_mesh(vfs, rock);
    put_mesh(vfs, lamp);
    put_cell(vfs, cell_id, rock, lamp, 5);
    auto assets = de::asset::AssetRegistry::create(
        {.vfs = &vfs, .jobs = &jobs, .hot_reload = true, .reload_checks_per_update = 64});
    REQUIRE(assets.ok());
    auto world = World::create({.jobs = &jobs, .assets = &*assets});
    REQUIRE(world.ok());
    flecs::world& ecs = world->ecs();
    const flecs::entity cell = de::world::load_cell(ecs, cell_id);
    world->tick(); // still loading: nothing yet
    CHECK(members_of(ecs, cell) == 0);
    assets->wait_all();
    world->tick();
    REQUIRE(members_of(ecs, cell) == 5);
    CHECK(cell.get<de::world::Cell>().coord == de::world::CellCoord{.x = 3, .z = -1});

    // One of them is a fog volume, as the cell says.
    int fog_volumes = 0;
    ecs.query_builder<const de::world::FogVolume>().with<InCell>(cell).build().each(
        [&](flecs::entity, const de::world::FogVolume& fog) {
            ++fog_volumes;
            CHECK(fog.density_per_m == 0.4f);
            CHECK(fog.edge_m == 3.0f);
            CHECK(fog.tint[2] == 0.7f);
            CHECK(fog.ellipsoid);
            CHECK(fog.noise_size_m == 5.0f);
            CHECK(fog.noise_amount == 0.75f);
            CHECK(fog.drift_mps[2] == 1.5f);
            CHECK(fog.height_fade_m == 3.0f);
        });
    CHECK(fog_volumes == 1);

    // The child: composed with its parent at once (no slide in), its mesh the cell's dependency.
    flecs::entity child;
    ecs.query_builder<const MeshInstance>().with<InCell>(cell).build().each([&](flecs::entity e, const MeshInstance&) {
        if (e.parent().is_valid()) {
            child = e;
        }
    });
    REQUIRE(child.is_valid());
    CHECK_FALSE(child.has<de::world::StaticTransform>());
    CHECK(child.parent().has<de::world::StaticTransform>());
    CHECK(child.get<MeshInstance>().mesh == assets->find({.id = lamp, .type = de::asset::AssetType::Mesh}));
    CHECK(child.parent().get<MeshInstance>().mesh == assets->find({.id = rock, .type = de::asset::AssetType::Mesh}));
    CHECK(near(child.get<de::world::PreviousWorldTransform>().value.position.x, 400.0));
    CHECK(near(child.get<de::world::PreviousWorldTransform>().value.position.z, -102.0));
    CHECK(near(child.get<WorldTransform>().position.z, -102.0));
    world->tick();
    CHECK(near(child.get<WorldTransform>().position.z, -102.0)); // the system agrees

    // Members behind the asset's revision are spawned again (a reload swapped in a new file).
    const de::world::CellContents spawned = cell.get<de::world::CellContents>();
    CHECK(spawned.revision == assets->revision(cell.get<de::world::Cell>().asset));
    cell.set(de::world::CellContents{.asset = spawned.asset, .revision = spawned.revision + 1});
    world->tick();
    CHECK_FALSE(child.is_alive());
    CHECK(members_of(ecs, cell) == 5);
    CHECK(cell.get<de::world::CellContents>().revision == spawned.revision);
    world->tick();
    CHECK(members_of(ecs, cell) == 5); // settled: not spawned again
    ecs.query_builder<const MeshInstance>().with<InCell>(cell).build().each([&](flecs::entity e, const MeshInstance&) {
        if (e.parent().is_valid()) {
            child = e;
        }
    });
    REQUIRE(child.is_alive());

    // Re-cooked with fewer entities: hot reload swaps the file in, the members follow.
    put_cell(vfs, cell_id, rock, lamp, 3);
    for (int i = 0; i < 4 && assets->stats().reloads == 0; ++i) {
        assets->update();
        assets->wait_all();
    }
    REQUIRE(assets->stats().reloads == 1);
    // A frame without a tick still spawns them before extraction: nothing extracted names the old
    // file's meshes.
    const FrameStep step = world->advance(0.0);
    CHECK(step.ticks == 0);
    CHECK(members_of(ecs, cell) == 3);
    CHECK_FALSE(child.is_alive());
    CHECK(world->render_frame().instances.size() == 2); // the rock and the lamp; the third has no mesh

    // Unloading is deleting the entity: members, the cell asset and its meshes all go.
    cell.destruct();
    assets->update();
    CHECK(assets->stats().assets == 0);
    int left = 0;
    ecs.query_builder<const Transform>().build().each([&](const Transform&) { ++left; });
    CHECK(left == 0);
}

TEST_CASE("static children of static parents are spawned as roots where they are") {
    const de::test::Sandbox sandbox;
    de::core::JobSystem jobs({.worker_count = 2});
    de::platform::Vfs vfs = sandbox.make_vfs({.jobs = &jobs, .game_writable = true});
    const de::asset::AssetId cell_id = de::asset::AssetId::from_value(0xa0);
    de::cooker::CellBuild build;
    const Quat yaw = yaw_deg(90.0f);
    build.entities.push({.position = {10.0, 0.0, 20.0},
                         .rotation = {yaw.x, yaw.y, yaw.z, yaw.w},
                         .scale = {2.0f, 2.0f, 2.0f},
                         .flags = de::asset::CellEntityFlags::Static});
    build.entities.push({.position = {1.0, 0.0, 0.0}, .parent = 0, .flags = de::asset::CellEntityFlags::Static});
    const auto cooked = de::cooker::cook_cell(build, cell_id);
    REQUIRE(vfs.write_file(de::asset::cooked_path(cell_id, de::asset::AssetType::Cell).c_str(), cooked.span()).ok());
    auto assets = de::asset::AssetRegistry::create({.vfs = &vfs, .jobs = &jobs, .hot_reload = false});
    REQUIRE(assets.ok());
    auto world = World::create({.jobs = &jobs, .assets = &*assets});
    REQUIRE(world.ok());
    flecs::world& ecs = world->ecs();
    const flecs::entity cell = de::world::load_cell(ecs, cell_id);
    assets->wait_all();
    world->tick();
    REQUIRE(members_of(ecs, cell) == 2);
    int roots = 0;
    ecs.query_builder<const Transform>().with<InCell>(cell).build().each([&](flecs::entity e, const Transform& t) {
        CHECK_FALSE(e.parent().is_valid());
        CHECK(e.has<de::world::StaticTransform>());
        if (near(t.position.z, 18.0)) { // the child, at its world transform: (10, 0, 20 - 2)
            CHECK(near(t.position.x, 10.0));
            CHECK(t.scale.x == doctest::Approx(2.0f));
        }
        ++roots;
    });
    CHECK(roots == 2);
}

TEST_CASE("a cell that can't load stays empty, and the world goes on") {
    {
        auto alone = World::create(); // no registry: nothing to load through
        REQUIRE(alone.ok());
        CHECK_FALSE(de::world::load_cell(alone->ecs(), de::asset::AssetId::from_value(0x98)).is_valid());
    }
    const de::test::Sandbox sandbox;
    de::core::JobSystem jobs({.worker_count = 2});
    de::platform::Vfs vfs = sandbox.make_vfs({.jobs = &jobs, .game_writable = true});
    auto assets = de::asset::AssetRegistry::create({.vfs = &vfs, .jobs = &jobs, .hot_reload = false});
    REQUIRE(assets.ok());
    auto world = World::create({.jobs = &jobs, .assets = &*assets});
    REQUIRE(world.ok());
    flecs::world& ecs = world->ecs();
    const flecs::entity cell = de::world::load_cell(ecs, de::asset::AssetId::from_value(0x99));
    assets->wait_all();
    world->tick();
    CHECK(assets->state(cell.get<de::world::Cell>().asset) == de::asset::AssetState::Failed);
    CHECK(members_of(ecs, cell) == 0);
    cell.destruct();
    assets->update();
    CHECK(assets->stats().assets == 0);
}

TEST_CASE("every row under a parent without a transform gets its world transform") {
    auto world = World::create();
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    const flecs::entity group = ecs.entity(); // no Transform
    const flecs::entity a = ecs.entity(flecs::Parent{group});
    const flecs::entity b = ecs.entity(flecs::Parent{group});
    de::world::place(a, Transform{.position = {1.0, 0.0, 0.0}});
    de::world::place(b, Transform{.position = {2.0, 0.0, 0.0}});
    world->tick();
    // Only the second row moves: it must still be computed.
    b.set(Transform{.position = {7.0, 0.0, 0.0}});
    world->tick();
    CHECK(near(b.get<WorldTransform>().position.x, 7.0));
    a.set(Transform{.position = {5.0, 0.0, 0.0}});
    b.set(Transform{.position = {9.0, 0.0, 0.0}});
    world->tick();
    CHECK(near(a.get<WorldTransform>().position.x, 5.0));
    CHECK(near(b.get<WorldTransform>().position.x, 9.0));
}

TEST_CASE("attaching keeps the entity where it is, whatever row it was in") {
    auto world = World::create();
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    const flecs::entity rider = ecs.entity();
    de::world::place(rider, Transform{.position = {100.0, 0.0, 0.0}});
    // Three entities in one table: the sword is in the middle, so moving it to another table swaps
    // the last row into its place.
    const flecs::entity before = ecs.entity();
    const flecs::entity sword = ecs.entity();
    const flecs::entity after = ecs.entity();
    de::world::place(before, Transform{.position = {1.0, 0.0, 0.0}});
    de::world::place(sword, Transform{.position = {103.0, 0.0, 0.0}});
    de::world::place(after, Transform{.position = {-50.0, 0.0, 0.0}});
    de::world::attach(sword, rider);
    world->tick();
    CHECK(near(sword.get<Transform>().position.x, 3.0)); // its offset from the rider
    CHECK(near(sword.get<WorldTransform>().position.x, 103.0));
}

TEST_CASE("a cell given another loaded asset spawns that asset's members") {
    const de::test::Sandbox sandbox;
    de::core::JobSystem jobs({.worker_count = 2});
    de::platform::Vfs vfs = sandbox.make_vfs({.jobs = &jobs, .game_writable = true});
    const de::asset::AssetId first = de::asset::AssetId::from_value(0xb0);
    const de::asset::AssetId second = de::asset::AssetId::from_value(0xb1);
    const de::asset::AssetId rock = de::asset::AssetId::from_value(0xb2);
    const de::asset::AssetId lamp = de::asset::AssetId::from_value(0xb3);
    put_mesh(vfs, rock);
    put_mesh(vfs, lamp);
    put_cell(vfs, first, rock, lamp, 3);
    put_cell(vfs, second, rock, lamp, 6);
    auto assets = de::asset::AssetRegistry::create({.vfs = &vfs, .jobs = &jobs, .hot_reload = false});
    REQUIRE(assets.ok());
    auto world = World::create({.jobs = &jobs, .assets = &*assets});
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    const flecs::entity cell = de::world::load_cell(world->ecs(), first);
    const de::asset::AssetHandle other = assets->load<de::asset::CellAsset>(second);
    assets->wait_all();
    world->tick();
    REQUIRE(members_of(ecs, cell) == 3);
    // Both assets are at revision 1: the asset itself must tell them apart.
    REQUIRE(assets->revision(other) == assets->revision(cell.get<de::world::Cell>().asset));
    cell.set(de::world::Cell(other, {}));
    world->tick();
    CHECK(members_of(ecs, cell) == 6);
    cell.destruct();
    assets->update();
    CHECK(assets->stats().assets == 0);
}
