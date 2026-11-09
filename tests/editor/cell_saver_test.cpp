#include "asset/cell_format.h"
#include "asset/registry.h"
#include "cooker/cooker.h"
#include "core/job_system.h"
#include "core/math.h"
#include "editor/cell_saver.h"
#include "editor/source_files.h"
#include "platform/file.h"
#include "platform/vfs.h"
#include "support/sandbox.h"
#include "world/cell.h"
#include "world/render_extract.h"
#include "world/transform.h"
#include "world/world.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <string_view>

using de::DVec3;
using de::Quat;
using de::Vec3;
using de::asset::AssetRegistry;
using de::test::Sandbox;
using de::world::Transform;
using de::world::World;

namespace {

de::core::Allocator& heap() {
    return de::core::heap_allocator(de::core::MemoryTag::Editor);
}

// A world of cells cooked from sources in a sandbox, reloading what's cooked again.
struct Scene {
    Scene()
        : jobs({.worker_count = 0}), vfs(sandbox.make_vfs({.jobs = &jobs, .game_writable = true})),
          assets(AssetRegistry::create({.vfs = &vfs, .jobs = &jobs, .hot_reload = true}).value()),
          world(World::create({.jobs = &jobs, .assets = &assets}).value()) {}

    void write_source(std::string_view relative, std::string_view text) const {
        de::platform::PathString path = sandbox.folder("src");
        REQUIRE(path.append('/'));
        REQUIRE(path.append(relative));
        const de::platform::PathString parent(path.view().substr(0, path.view().find_last_of('/')));
        REQUIRE(de::platform::create_directories(parent.c_str()).ok());
        REQUIRE(de::platform::write_file_atomic(path.c_str(), de::test::bytes_of(text)).ok());
    }

    std::string read_source(const char* path) const {
        const auto bytes = vfs.read_whole_file(path, heap());
        REQUIRE(bytes.ok());
        return {reinterpret_cast<const char*>(bytes->data()), bytes->size()};
    }

    void cook() { REQUIRE(de::cooker::cook(vfs, jobs, nullptr).ok()); }

    // Loads and reloads what's due, then a tick: what's ready spawns.
    void settle() {
        for (int i = 0; i < 4; ++i) {
            assets.update();
            assets.wait_all();
        }
        world.tick();
        world.tick();
    }

    // The member with that name in its cell's file (in the cell at x, z, if one's given).
    flecs::entity member(std::string_view name, const int32_t* x = nullptr, const int32_t* z = nullptr) {
        flecs::entity found;
        world.ecs().each([&](flecs::entity entity, const de::world::CellMember& member) {
            const flecs::entity cell = entity.target<de::world::InCell>();
            const auto* cooked = assets.get<de::asset::CellAsset>(cell.get<de::world::Cell>().asset);
            if (cooked != nullptr && cooked->name(cooked, cooked->entities.get(cooked)[member.index]) == name &&
                (x == nullptr || (cooked->coord_x == *x && cooked->coord_z == *z))) {
                found = entity;
            }
        });
        REQUIRE(found.is_valid());
        return found;
    }

    Sandbox sandbox;
    de::core::JobSystem jobs;
    de::platform::Vfs vfs;
    AssetRegistry assets;
    World world;
};

constexpr std::string_view kCell = R"({
  // A camp, its pieces relative to it.
  "coord": [0, 0],
  "entities": [
    {"name": "camp", "position": [64, 0, 64], "rotation_deg": [0, 90, 0]},
    {"name": "statue", "parent": "camp", "position": [-3.5, 1.21, -3], "rotation_deg": [0, 40, 0], "scale": 1.2},
    {"name": "boulder", "position": [58, 0, 70]}, // a comment, kept
    {"position": [10, 0, 10]},
    {"name": "untouched", "position": [20, 0, 20], "rotation_deg": [180, 0, 180]}
  ]
}
)";

} // namespace

TEST_CASE("saving writes what was edited into the cell's source, and nothing else, and it comes back when cooked") {
    Scene scene;
    scene.write_source("content/world/camp.cell", kCell);
    scene.cook();
    de::editor::CellSources sources(heap());
    REQUIRE(sources.scan(scene.vfs));
    CHECK(sources.size() == 1);
    de::asset::Guid guid;
    de::asset::AssetId cell_id;
    REQUIRE(de::editor::source_asset_id(scene.vfs, "src:/content/world/camp.cell", guid, cell_id));
    REQUIRE(sources.find(cell_id) != nullptr);
    static_cast<void>(de::world::load_cell(scene.world.ecs(), cell_id));
    scene.settle();
    const flecs::world& ecs = scene.world.ecs();

    // Edits, as the editor's commands make them: each placed where it goes, in the world.
    const flecs::entity boulder = scene.member("boulder");
    de::world::place(boulder, Transform{.position = {60.5, 0.0, 70.25}});
    // The statue is static, so it's a root where it stands: a meter along the world's x is a meter along the camp's z
    // in its file (the camp is turned a quarter about y).
    const flecs::entity statue = scene.member("statue");
    Transform statue_at = statue.get<Transform>();
    statue_at.position.x += 1.0;
    de::world::place(statue, statue_at);
    const flecs::entity unnamed = scene.member("");
    de::world::place(unnamed, Transform{.position = {10.0, 0.0, 10.0},
                                        .rotation = glm::angleAxis(glm::radians(30.0f), Vec3(0.0f, 1.0f, 0.0f))});

    de::editor::MemberTracker tracker(heap());
    for (const flecs::entity edited : {boulder, statue, unnamed}) {
        tracker.note(ecs, scene.assets, edited.id());
    }
    tracker.follow(ecs, scene.assets, [](flecs::entity_t, flecs::entity_t) { FAIL("nothing was spawned again"); });

    const de::editor::CellSaveReport report = de::editor::save_cells(ecs, scene.assets, scene.vfs, sources, {});
    CHECK(report.files_written == 1);
    CHECK(report.entities_changed == 3);
    CHECK(report.cells_failed == 0);
    CHECK(report.outside_cell == 0);
    std::string expected(kCell);
    const auto change = [&expected](std::string_view from, std::string_view to) {
        const size_t at = expected.find(from);
        REQUIRE(at != std::string::npos);
        expected.replace(at, from.size(), to);
    };
    change(R"({"name": "boulder", "position": [58, 0, 70]})", R"({"name": "boulder", "position": [60.5, 0, 70.25]})");
    change(R"("position": [-3.5, 1.21, -3])", R"("position": [-3.5, 1.21, -2])");
    change(R"({"position": [10, 0, 10]})", R"({"position": [10, 0, 10], "rotation_deg": [0, 30, 0]})");
    CHECK(scene.read_source("src:/content/world/camp.cell") == expected);

    // Saved again before a cook: the file already says it all.
    CHECK(de::editor::save_cells(ecs, scene.assets, scene.vfs, sources, {}).files_written == 0);

    // Cooked: hot reload spawns the members anew, where they were saved, and the tracker follows them.
    scene.cook();
    scene.settle();
    int moves = 0;
    bool boulder_followed = false;
    const flecs::entity boulder_now = scene.member("boulder");
    tracker.follow(ecs, scene.assets, [&](flecs::entity_t from, flecs::entity_t to) {
        ++moves;
        boulder_followed = boulder_followed || (from == boulder.id() && to == boulder_now.id());
    });
    CHECK(moves == 3);
    CHECK(boulder_followed);
    CHECK_FALSE(boulder.is_alive());
    CHECK(boulder_now.get<Transform>().position.x == doctest::Approx(60.5));
    CHECK(boulder_now.get<Transform>().position.z == doctest::Approx(70.25));
    const DVec3 statue_now = scene.member("statue").get<Transform>().position;
    CHECK(statue_now.x == doctest::Approx(statue_at.position.x).epsilon(1e-6));
    CHECK(statue_now.z == doctest::Approx(statue_at.position.z).epsilon(1e-6));
    // As cooked now, nothing differs.
    CHECK(de::editor::save_cells(ecs, scene.assets, scene.vfs, sources, {}).entities_changed == 0);
}

TEST_CASE("saving refuses a source that changed since it was cooked, rather than write into the wrong entries") {
    Scene scene;
    scene.write_source("content/world/camp.cell", kCell);
    scene.cook();
    de::editor::CellSources sources(heap());
    REQUIRE(sources.scan(scene.vfs));
    de::asset::Guid guid;
    de::asset::AssetId cell_id;
    REQUIRE(de::editor::source_asset_id(scene.vfs, "src:/content/world/camp.cell", guid, cell_id));
    static_cast<void>(de::world::load_cell(scene.world.ecs(), cell_id));
    scene.settle();
    de::world::place(scene.member("boulder"), Transform{.position = {60.0, 0.0, 70.0}});
    // Someone puts an entity first in the file, by hand, and doesn't cook.
    std::string edited(kCell);
    edited.insert(edited.find(R"(    {"name": "camp")"), "    {\"name\": \"new\"},\n");
    scene.write_source("content/world/camp.cell", edited);
    const de::editor::CellSaveReport report =
        de::editor::save_cells(scene.world.ecs(), scene.assets, scene.vfs, sources, {});
    CHECK(report.cells_failed == 1);
    CHECK(report.files_written == 0);
    CHECK(scene.read_source("src:/content/world/camp.cell") == edited);
}

TEST_CASE("a cook report says whether a saved source failed, not whether anything else did") {
    constexpr std::string_view kReport = R"({"ok": false, "stats": {}, "diagnostics": [
 {"severity": "warning", "path": "content/world/camp.cell", "message": "a warning only"},
 {"severity": "error", "path": "content/terrain/steep.terrain", "message": "too steep"}
]})";
    CHECK_FALSE(de::editor::cook_report_has_error(kReport, "src:/content/world/camp.cell"));
    CHECK(de::editor::cook_report_has_error(kReport, "src:/content/terrain/steep.terrain"));
    CHECK(de::editor::cook_report_has_error(kReport, "content/terrain/steep.terrain"));
}

namespace {

constexpr std::string_view kEnvironment = R"({
  "sun": {"elevation_deg": 19, "azimuth_deg": 324, "color": [1, 0.783, 0.522], "intensity": 4.6},
  "sky": {"color": [0.30, 0.40, 0.62], "intensity": 1},
  "ground": {"color": [0.12, 0.10, 0.07], "intensity": 1},
  "shadow_color": [0.05, 0.06, 0.09],
  "exposure_ev": 0,
  "shadows": {"resolution": 2048, "distance_m": 140, "split_lambda": 0.75, "blend_band": 0.12,
              "caster_reach_m": 200, "penumbra_per_m": 0.06, "max_penumbra_texels": 6},
  "fog": {"density_per_m": 0.000005, "height_density_per_m": 0.0008, "height_falloff_per_m": 0.01,
          "base_height_m": 0, "extinction_tint": [0.8, 1.0, 1.3],
          "sun_forward": {"color": [1, 0.85, 0.7], "intensity": 0.25},
          "sun_back": {"color": [0.5, 0.6, 0.8], "intensity": 0.08}, "sun_height_attenuation": 1,
          "ambient": {"color": [0.30, 0.40, 0.62], "intensity": 1}, "ambient_height_attenuation": 1,
          "start_m": 0, "sun_start_m": 5, "end_m": 20000, "volume_distance_m": 128}
}
)";

de::asset::AssetId id_of(const Scene& scene, const char* path) {
    de::asset::Guid guid;
    de::asset::AssetId id;
    REQUIRE(de::editor::source_asset_id(scene.vfs, path, guid, id));
    return id;
}

} // namespace

TEST_CASE("saving writes a fog volume's edited values into its object, and only those") {
    Scene scene;
    scene.write_source("content/world/camp.cell", R"({
  "coord": [0, 0],
  "entities": [
    {"name": "mist", "position": [64, 1, 58], "fog_volume": {"shape": "box", "density_per_m": 0.1, "edge_m": 4}}
  ]
}
)");
    scene.cook();
    de::editor::CellSources sources(heap());
    REQUIRE(sources.scan(scene.vfs));
    static_cast<void>(de::world::load_cell(scene.world.ecs(), id_of(scene, "src:/content/world/camp.cell")));
    scene.settle();
    const flecs::entity mist = scene.member("mist");
    de::world::FogVolume fog = mist.get<de::world::FogVolume>();
    fog.density_per_m = 0.25f;
    fog.ellipsoid = true;
    fog.tint[1] = 0.5f;
    mist.set(fog);
    const de::editor::CellSaveReport report =
        de::editor::save_cells(scene.world.ecs(), scene.assets, scene.vfs, sources, {});
    CHECK(report.files_written == 1);
    CHECK(
        scene.read_source("src:/content/world/camp.cell")
            .find(
                R"("fog_volume": {"shape": "ellipsoid", "density_per_m": 0.25, "edge_m": 4, "tint": [1, 0.5, 1]}})") !=
        std::string::npos);
}

TEST_CASE("saving files what was moved into another cell's square into that cell's file, or a new one in the world") {
    Scene scene;
    scene.write_source("content/w/e.environment", kEnvironment);
    scene.write_source("content/w/w.world", R"({
  "environment": "e.environment",
  "start": {"position": [64, 2, 64]},
  "cells": [
    "a.cell",
    "b.cell"
  ]
}
)");
    scene.write_source("content/w/a.cell", R"({
  "coord": [0, 0],
  "entities": [
    {"name": "stays", "position": [10, 0, 10]},
    {"name": "camp", "position": [64, 0, 64]},
    {"name": "statue", "parent": "camp", "position": [2, 0, 0]},
    {"name": "rock", "position": [20, 0, 20]}, // a rock
    {"name": "stays_too", "position": [30, 0, 30]}
  ]
}
)");
    scene.write_source("content/w/b.cell", R"({
  "coord": [1, 0],
  "entities": [
    {"name": "camp", "position": [150, 0, 20]}
  ]
}
)");
    scene.cook();
    de::editor::CellSources sources(heap());
    REQUIRE(sources.scan(scene.vfs));
    const de::asset::AssetId world_id = id_of(scene, "src:/content/w/w.world");
    static_cast<void>(de::world::load_cell(scene.world.ecs(), id_of(scene, "src:/content/w/a.cell")));
    static_cast<void>(de::world::load_cell(scene.world.ecs(), id_of(scene, "src:/content/w/b.cell")));
    scene.settle();
    const flecs::world& ecs = scene.world.ecs();

    // The camp (and the statue on it) into cell [1, 0]; the rock into [2, 0], which has no file yet.
    constexpr int32_t kZero = 0;
    const flecs::entity camp = scene.member("camp", &kZero, &kZero); // b has a camp too
    const flecs::entity statue = scene.member("statue");
    const flecs::entity rock = scene.member("rock");
    const flecs::entity stays_too = scene.member("stays_too");
    de::world::place(camp, Transform{.position = {200.0, 0.0, 64.0}});
    de::world::place(statue, Transform{.position = {202.0, 0.0, 64.0}});
    de::world::place(rock, Transform{.position = {300.0, 0.0, 10.0}});
    de::editor::MemberTracker tracker(heap());
    for (const flecs::entity edited : {camp, statue, rock, stays_too}) {
        tracker.note(ecs, scene.assets, edited.id());
    }

    de::core::Array<de::editor::MemberMove> moves(heap());
    const de::editor::CellSaveReport report =
        de::editor::save_cells(ecs, scene.assets, scene.vfs, sources, world_id, nullptr, &moves);
    CHECK(report.cells_failed == 0);
    CHECK(report.refiled == 2);
    CHECK(report.cells_made == 1);
    CHECK(report.files_written == 4); // a, b, the new cell, the world
    CHECK(scene.read_source("src:/content/w/a.cell") == R"({
  "coord": [0, 0],
  "entities": [
    {"name": "stays", "position": [10, 0, 10]},
    {"name": "stays_too", "position": [30, 0, 30]}
  ]
}
)");
    // The camp's name was taken there: it's camp_2, and the statue names it so.
    CHECK(scene.read_source("src:/content/w/b.cell") == R"({
  "coord": [1, 0],
  "entities": [
    {"name": "camp", "position": [150, 0, 20]},
    {"name": "camp_2", "position": [200, 0, 64]},
    {"name": "statue", "parent": "camp_2", "position": [2, 0, 0]}
  ]
}
)");
    CHECK(scene.read_source("src:/content/w/cell_2_0.cell") == R"({
  "coord": [2, 0],
  "entities": [
    {"name": "rock", "position": [300, 0, 10]}
  ]
}
)");
    CHECK(scene.read_source("src:/content/w/w.world") == R"({
  "environment": "e.environment",
  "start": {"position": [64, 2, 64]},
  "cells": [
    "a.cell",
    "b.cell",
    "cell_2_0.cell"
  ]
}
)");

    // Cooked, and the new cell loaded (the viewer does that when its world is cooked again): each edited thing is
    // followed to where it is now.
    tracker.apply(moves.span());
    scene.cook();
    scene.settle();
    REQUIRE(sources.scan(scene.vfs));
    static_cast<void>(de::world::load_cell(scene.world.ecs(), id_of(scene, "src:/content/w/cell_2_0.cell")));
    scene.settle();
    int followed = 0;
    tracker.follow(ecs, scene.assets, [&](flecs::entity_t, flecs::entity_t) { ++followed; });
    CHECK(followed == 4);
    CHECK(scene.member("camp_2").get<Transform>().position.x == doctest::Approx(200.0));
    CHECK(scene.member("rock").get<Transform>().position.x == doctest::Approx(300.0));
    // And nothing more to save.
    CHECK(de::editor::save_cells(ecs, scene.assets, scene.vfs, sources, world_id).files_written == 0);
}
