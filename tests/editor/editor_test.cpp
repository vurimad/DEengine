#include "asset/registry.h"
#include "core/job_system.h"
#include "editor/commands.h"
#include "editor/gizmo.h"
#include "editor/icons.h"
#include "editor/navigation.h"
#include "editor/outline.h"
#include "editor/source_files.h"
#include "editor/widgets.h"
#include "platform/vfs.h"
#include "support/sandbox.h"
#include "tools/debug_ui.h"
#include "world/cell.h"
#include "world/render_extract.h"
#include "world/transform.h"
#include "world/world.h"

#include <doctest/doctest.h>
#include <glm/gtc/quaternion.hpp>
#include <imgui_internal.h> // the dock builder

#include <cmath>
#include <cstring>
#include <numbers>
#include <span>
#include <string_view>

using de::DVec3;
using de::editor::CommandStack;
using de::editor::Outline;
using de::test::Sandbox;
using de::world::Camera;
using de::world::Transform;
using de::world::World;
using de::world::WorldTransform;

namespace {

template <typename T>
std::span<const std::byte> bytes_of(const T& value) {
    return std::as_bytes(std::span(&value, 1));
}

de::core::Allocator& editor_heap() {
    return de::core::heap_allocator(de::core::MemoryTag::Editor);
}

flecs::entity cell_at(flecs::world& ecs, int32_t x, int32_t z) {
    return ecs.entity().set(de::world::Cell(de::asset::AssetHandle{}, de::world::CellCoord{.x = x, .z = z}));
}

flecs::entity placed(flecs::world& ecs, const DVec3& position) {
    const flecs::entity entity = ecs.entity();
    de::world::place(entity, Transform{.position = position});
    return entity;
}

} // namespace

TEST_CASE("editor commands: a set is undone and redone, and one drag of a field is one command") {
    auto world = World::create();
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    const flecs::entity camera = ecs.entity().set(Camera{.vertical_fov_rad = 1.0f});
    const flecs::id_t camera_id = ecs.component<Camera>().id();
    const auto fov = [&] { return camera.get<Camera>().vertical_fov_rad; };
    CommandStack commands(editor_heap());

    CHECK(commands.saved_value(camera, camera_id) == nullptr); // not edited yet
    CHECK(commands.set_component(camera, camera_id, bytes_of(Camera{.vertical_fov_rad = 1.2f})));
    CHECK(fov() == 1.2f);
    // What it was before the first edit is kept, for the reset arrow, whatever comes after.
    const auto saved_fov = [&] {
        Camera saved;
        std::memcpy(&saved, commands.saved_value(camera, camera_id), sizeof(saved));
        return saved.vertical_fov_rad;
    };
    REQUIRE(commands.saved_value(camera, camera_id) != nullptr);
    CHECK(saved_fov() == 1.0f);
    // A drag: every frame's value under one key makes one command, from where it started to where it ended.
    for (const float dragged : {1.3f, 1.4f, 1.5f}) {
        CHECK(commands.set_component(camera, camera_id, bytes_of(Camera{.vertical_fov_rad = dragged}), 77));
    }
    CHECK(saved_fov() == 1.0f);
    commands.seal();
    CHECK(commands.size() == 2);
    CHECK(fov() == 1.5f);
    CHECK((std::string_view(commands.undo_name(ecs)) == "Camera"));

    CHECK(commands.undo(ecs));
    CHECK(fov() == 1.2f);
    CHECK(commands.undo(ecs));
    CHECK(fov() == 1.0f);
    CHECK_FALSE(commands.undo(ecs));
    CHECK(commands.redo(ecs));
    CHECK(fov() == 1.2f);

    // A new set forgets what was undone.
    CHECK(commands.set_component(camera, camera_id, bytes_of(Camera{.vertical_fov_rad = 0.5f})));
    CHECK_FALSE(commands.can_redo());
    CHECK(commands.size() == 2);

    // Nothing is done for a value of the wrong size or an entity without the component.
    const float alone = 2.0f;
    CHECK_FALSE(commands.set_component(camera, camera_id, bytes_of(alone)));
    CHECK_FALSE(commands.set_component(ecs.entity(), camera_id, bytes_of(Camera{})));
    CHECK(commands.size() == 2);

    // Its entity gone, a command is dropped when it comes up.
    camera.destruct();
    CHECK_FALSE(commands.undo(ecs));
    CHECK(commands.size() == 1);
    commands.clear();
    CHECK_FALSE(commands.can_undo());
    CHECK(commands.saved_value(camera, camera_id) == nullptr);
}

TEST_CASE("editor commands: a transform is placed at once, and what's attached to it goes with it") {
    auto world = World::create();
    REQUIRE(world.ok());
    flecs::world& ecs = world->ecs();
    const flecs::entity parent = placed(ecs, {1.0, 2.0, 3.0});
    const flecs::entity child = ecs.entity();
    de::world::attach(child, parent);
    de::world::place(child, Transform{.position = {0.0, 1.0, 0.0}});
    CHECK(child.get<WorldTransform>().position == DVec3(1.0, 3.0, 3.0));

    CommandStack commands(editor_heap());
    const flecs::id_t transform_id = ecs.component<Transform>().id();
    REQUIRE(commands.set_component(parent, transform_id, bytes_of(Transform{.position = {10.0, 0.0, 0.0}})));
    // No tick: the world transforms are there already.
    CHECK(parent.get<WorldTransform>().position == DVec3(10.0, 0.0, 0.0));
    CHECK(child.get<WorldTransform>().position == DVec3(10.0, 1.0, 0.0));
    CHECK(commands.undo(ecs));
    CHECK(parent.get<WorldTransform>().position == DVec3(1.0, 2.0, 3.0));
    CHECK(child.get<WorldTransform>().position == DVec3(1.0, 3.0, 3.0));
}

TEST_CASE("the outline lists the cells by their coordinates, their members, and what's in no cell") {
    auto world = World::create();
    REQUIRE(world.ok());
    flecs::world& ecs = world->ecs();
    const flecs::entity east = cell_at(ecs, 1, 0);
    const flecs::entity origin = cell_at(ecs, 0, 0);
    const flecs::entity north = cell_at(ecs, 0, 1);
    const flecs::entity tree = placed(ecs, {5.0, 0.0, 5.0}).add<de::world::InCell>(origin);
    const flecs::entity branch = placed(ecs, {5.0, 3.0, 5.0}).add<de::world::InCell>(origin);
    de::world::attach(branch, tree);
    const flecs::entity rock = placed(ecs, {140.0, 0.0, 2.0}).add<de::world::InCell>(east);
    const flecs::entity camera = placed(ecs, {0.0, 10.0, 0.0});
    const flecs::entity dropped = placed(ecs, {1.0, 10.0, 0.0});
    const flecs::entity shown = ecs.entity();
    de::world::attach(shown, dropped);
    de::world::place(shown, Transform{});

    Outline outline(editor_heap());
    outline.rebuild(ecs);
    REQUIRE(outline.cells().size() == 3);
    CHECK(outline.cells()[0] == origin.id());
    CHECK(outline.cells()[1] == east.id());
    CHECK(outline.cells()[2] == north.id());
    REQUIRE(outline.roots(origin).size() == 1);
    CHECK(outline.roots(origin)[0].entity == tree.id());
    REQUIRE(outline.children(tree).size() == 1);
    CHECK(outline.children(tree)[0].entity == branch.id());
    CHECK(outline.children(tree)[0].cell == origin.id());
    REQUIRE(outline.roots(east).size() == 1);
    CHECK(outline.roots(east)[0].entity == rock.id());
    CHECK(outline.roots(north).empty());
    // In no cell: the camera and the dropped rock, with what shows it attached.
    const std::span<const Outline::Item> loose = outline.roots(0);
    REQUIRE(loose.size() == 2);
    CHECK((loose[0].entity == camera.id() || loose[1].entity == camera.id()));
    CHECK((loose[0].entity == dropped.id() || loose[1].entity == dropped.id()));
    REQUIRE(outline.children(dropped).size() == 1);
    CHECK(outline.children(dropped)[0].entity == shown.id());
    CHECK(outline.items().size() == 6);
}

TEST_CASE("entity labels: a flecs name, a cell's coordinates, or what it is and its number") {
    Sandbox sandbox;
    de::core::JobSystem jobs{{.worker_count = 1}};
    de::platform::Vfs vfs = sandbox.make_vfs({.jobs = &jobs});
    auto registry = de::asset::AssetRegistry::create({.vfs = &vfs, .jobs = &jobs, .hot_reload = false});
    REQUIRE(registry.ok());
    auto world = World::create();
    REQUIRE(world.ok());
    flecs::world& ecs = world->ecs();
    char label[64];
    de::editor::entity_label(*registry, ecs.entity("free_camera"), label);
    CHECK((std::string_view(label) == "free_camera"));
    de::editor::entity_label(*registry, cell_at(ecs, -3, 7), label);
    CHECK((std::string_view(label) == "cell [-3, 7]"));
    const flecs::entity fog = ecs.entity().set(de::world::FogVolume{});
    de::editor::entity_label(*registry, fog, label);
    CHECK((std::string_view(label).starts_with("Fog volume ")));
    // Cut short when it doesn't fit.
    char small[5];
    de::editor::entity_label(*registry, ecs.entity("a_long_name"), small);
    CHECK((std::string_view(small) == "a_lo"));
    // Gone (a panel still holding it after its cell spawned again): said so, not asked of.
    const flecs::entity gone = ecs.entity("gone");
    const flecs::entity_t id = gone.id();
    gone.destruct();
    char gone_label[32];
    de::editor::entity_label(*registry, ecs.entity(id), gone_label);
    CHECK((std::string_view(gone_label) == "(gone)"));
}

TEST_CASE("searching is by part of a name, in either case") {
    using de::editor::contains_ignoring_case;
    CHECK(contains_ignoring_case("Boulder_01", "boul"));
    CHECK(contains_ignoring_case("Boulder_01", "R_0"));
    CHECK(contains_ignoring_case("Boulder_01", ""));
    CHECK_FALSE(contains_ignoring_case("Boulder_01", "boulders"));
    CHECK_FALSE(contains_ignoring_case("", "a"));
}

TEST_CASE("source files: what they are, and the asset a file cooks to from its meta") {
    using de::editor::file_kind;
    CHECK((file_kind("wc.world") == "World"));
    CHECK((file_kind("coast.terrainlayers") == "Terrain layers"));
    CHECK((file_kind("sea_foam.png") == "Texture"));
    CHECK((file_kind("notes.txt") == ".txt"));
    CHECK((file_kind("README") == "File"));

    Sandbox sandbox;
    const de::platform::Vfs vfs = sandbox.make_vfs({});
    constexpr std::string_view kMeta = "{\n  \"guid\": \"44535131-2ebf-4f2d-85fe-ba8d4af9ec17\"\n}\n";
    REQUIRE(vfs.write_file("src:/meta/terrain/a.world.json", std::as_bytes(std::span(kMeta))).ok());
    de::asset::Guid guid;
    de::asset::AssetId id;
    REQUIRE(de::editor::source_asset_id(vfs, "src:/content/terrain/a.world", guid, id));
    de::asset::Guid expected;
    REQUIRE(de::asset::parse_guid("44535131-2ebf-4f2d-85fe-ba8d4af9ec17", expected));
    CHECK(guid == expected);
    CHECK(id == de::asset::asset_id(expected));
    CHECK_FALSE(de::editor::source_asset_id(vfs, "src:/content/terrain/b.world", guid, id));   // no meta
    CHECK_FALSE(de::editor::source_asset_id(vfs, "src:/meta/terrain/a.world.json", guid, id)); // not in content/
}

TEST_CASE("icons: paths, arcs, circles and rectangles parse into shapes") {
    using de::editor::Icon;
    de::editor::IconSet icons(editor_heap());
    // Three subpaths; the numbers after a command repeat it.
    CHECK(icons.parse(Icon::Move, R"(<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6">)"
                                  R"(<path d="M12 3v18M3 12h18M12 3l-3 3 3 3"/></svg>)"));
    CHECK(icons.shape_count(Icon::Move) == 3);
    // An arc, its flags run together, a relative curve.
    CHECK(icons.parse(Icon::Rotate,
                      R"(<svg viewBox="0 0 24 24"><path d="M20 12a8 8 0 1 1-2.6-5.9M20 4v4.5h-4.5"/></svg>)"));
    CHECK(icons.shape_count(Icon::Rotate) == 2);
    CHECK(icons.parse(Icon::Eye, R"(<svg viewBox="0 0 24 24"><path d="M2 12s4-7 10-7 10 7 10 7-4 7-10 7S2 12 2 12z"/>)"
                                 R"(<circle cx="12" cy="12" r="3"/></svg>)"));
    CHECK(icons.shape_count(Icon::Eye) == 2);
    CHECK(icons.parse(Icon::Stop, R"(<svg viewBox="0 0 24 24" fill="currentColor"><rect x="6" y="6" width="12" )"
                                  R"(height="12" rx="1.5"/></svg>)"));
    CHECK(icons.shape_count(Icon::Stop) == 1);
    CHECK_FALSE(icons.parse(Icon::Zoom, "not an icon"));
    CHECK_FALSE(icons.parse(Icon::Zoom, R"(<svg viewBox="0 0 24 24"><path d="M1 1Q2 2 3 3"/></svg>)")); // no Q
    CHECK_FALSE(icons.parse(Icon::Zoom, R"(<svg viewBox="0 0 24 24"><path d="M1 1z 2 2"/></svg>)"));    // no loop
    CHECK(icons.shape_count(Icon::Zoom) == 0);
}

TEST_CASE("the view puts points on screen and sends rays back through them") {
    using de::Quat;
    using de::Vec3;
    const de::editor::View view{.position = {100.0, 5.0, 50.0},
                                .rotation = glm::angleAxis(0.3f, Vec3(0.0f, 1.0f, 0.0f)) *
                                            glm::angleAxis(-0.2f, Vec3(1.0f, 0.0f, 0.0f)),
                                .vertical_fov_rad = 1.0f,
                                .min = ImVec2(10.0f, 20.0f),
                                .size = ImVec2(800.0f, 600.0f)};
    const DVec3 point = view.position + DVec3(view.rotation * Vec3(2.0f, 1.0f, -10.0f));
    ImVec2 screen;
    REQUIRE(view.project(point, screen));
    CHECK(glm::dot(view.ray(screen), glm::normalize(Vec3(point - view.position))) > 0.99999f);
    // The middle of the screen looks ahead; behind the camera isn't on screen.
    CHECK(glm::dot(view.ray(ImVec2(410.0f, 320.0f)), view.rotation * Vec3(0.0f, 0.0f, -1.0f)) > 0.99999f);
    CHECK_FALSE(view.project(view.position + DVec3(view.rotation * Vec3(0.0f, 0.0f, 10.0f)), screen));
    // A pixel 10 m ahead covers the view's height there over the screen's.
    const float expected = 2.0f * 10.0f * std::tan(0.5f) / 600.0f;
    CHECK(view.meters_per_pixel(view.position + DVec3(view.rotation * Vec3(0.0f, 0.0f, -10.0f))) ==
          doctest::Approx(expected));
}

TEST_CASE("gizmo: where a ray comes closest to a handle's axis, and a child's place from its world") {
    using de::Vec3;
    float along = 0.0f;
    // From above and in front of the X axis, looking down at its point 5 m along.
    REQUIRE(de::editor::closest_on_axis(Vec3(5.0f, 3.0f, 10.0f), glm::normalize(Vec3(0.0f, -3.0f, -10.0f)),
                                        Vec3(1.0f, 0.0f, 0.0f), along));
    CHECK(along == doctest::Approx(5.0f));
    CHECK_FALSE(
        de::editor::closest_on_axis(Vec3(0.0f, 1.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), along));

    const WorldTransform parent{.position = {10.0, 2.0, -4.0},
                                .rotation = glm::angleAxis(0.7f, glm::normalize(Vec3(0.2f, 1.0f, 0.1f))),
                                .scale = Vec3(2.0f)};
    const WorldTransform world{.position = {13.0, 1.0, -2.0},
                               .rotation = glm::angleAxis(-0.4f, Vec3(1.0f, 0.0f, 0.0f)),
                               .scale = Vec3(3.0f, 2.0f, 1.0f)};
    const WorldTransform back = de::world::compose(parent, de::world::local_of(parent, world));
    CHECK(glm::length(back.position - world.position) < 1e-4);
    CHECK(std::abs(glm::dot(back.rotation, world.rotation)) > 0.99999f);
    CHECK(glm::length(back.scale - world.scale) < 1e-4f);
}

TEST_CASE("gizmo: turning and scaling about the geometry's centre move the pivot round it") {
    using de::DVec3;
    using de::Vec3;
    using de::world::WorldTransform;
    const WorldTransform start{.position = {10.0, 0.0, 5.0}, .rotation = {1.0f, 0.0f, 0.0f, 0.0f}, .scale = Vec3(2.0f)};
    const DVec3 center(10.0, 1.0, 5.0); // a metre above the pivot

    // A quarter turn about +Z: the pivot, below the centre, swings to its +X side.
    const WorldTransform turned = de::editor::turned_about(
        start, center, glm::angleAxis(0.5f * std::numbers::pi_v<float>, Vec3(0.0f, 0.0f, 1.0f)));
    CHECK(glm::length(turned.position - DVec3(11.0, 1.0, 5.0)) < 1e-5);
    CHECK(std::abs(glm::dot(turned.rotation,
                            glm::angleAxis(0.5f * std::numbers::pi_v<float>, Vec3(0.0f, 0.0f, 1.0f)))) > 0.99999f);
    // About the pivot itself, it stays put.
    CHECK(glm::length(de::editor::turned_about(start, start.position, turned.rotation).position - start.position) <
          1e-9);

    // Twice as tall about the centre: the pivot goes twice as far below it; the other axes leave it be.
    const Vec3 axes[3] = {Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f)};
    const WorldTransform scaled = de::editor::scaled_about(start, center, axes, Vec3(3.0f, 2.0f, 1.0f));
    CHECK(glm::length(scaled.position - DVec3(10.0, -1.0, 5.0)) < 1e-5);
    CHECK(glm::length(scaled.scale - Vec3(6.0f, 4.0f, 2.0f)) < 1e-5f);
}

TEST_CASE("gizmo: snapping lands a turn on a whole step of its own angle") {
    using de::Vec3;
    constexpr float kDeg = std::numbers::pi_v<float> / 180.0f;
    const de::Quat start = glm::angleAxis(40.0f * kDeg, Vec3(0.0f, 1.0f, 0.0f));
    // 22 degrees on from 40 lands on 60; 10 back lands on 30; unsnapped, it's as dragged.
    CHECK(de::editor::snapped_turn(start, Vec3(0.0f, 1.0f, 0.0f), 22.0f * kDeg, 15.0f * kDeg) ==
          doctest::Approx(20.0f * kDeg));
    CHECK(de::editor::snapped_turn(start, Vec3(0.0f, 1.0f, 0.0f), -10.0f * kDeg, 15.0f * kDeg) ==
          doctest::Approx(-10.0f * kDeg));
    CHECK(de::editor::snapped_turn(start, Vec3(0.0f, 1.0f, 0.0f), 7.0f * kDeg, 0.0f) == doctest::Approx(7.0f * kDeg));
    // About an axis it hasn't turned round, its own angle is 0: steps from there.
    CHECK(de::editor::snapped_turn(start, Vec3(1.0f, 0.0f, 0.0f), 22.0f * kDeg, 15.0f * kDeg) ==
          doctest::Approx(15.0f * kDeg));
}

namespace {

// A frame of the editor's handles on an entity, the mouse at a point with the left button down or up.
void gizmo_frame(de::editor::Gizmo& gizmo, const de::editor::View& view, de::editor::Tool tool,
                 const de::editor::Snap& snap, flecs::entity entity, ImVec2 mouse, bool down, CommandStack& commands) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(mouse.x, mouse.y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
    ImGui::NewFrame();
    static_cast<void>(gizmo.update(view, ImGui::GetForegroundDrawList(), tool, de::editor::Space::World, snap, entity,
                                   entity.get<WorldTransform>().position, true, commands));
    ImGui::EndFrame();
}

// The mouse moved out from a point on screen, a pixel at a time, until that handle is under it: where it is then.
ImVec2 find_handle(de::editor::Gizmo& gizmo, const de::editor::View& view, de::editor::Tool tool,
                   const de::editor::Snap& snap, flecs::entity entity, ImVec2 from, ImVec2 way, int handle,
                   CommandStack& commands) {
    ImVec2 at = from;
    for (int step = 0; step < 400 && gizmo.hovered_handle() != handle; ++step) {
        at = ImVec2(from.x + (way.x * static_cast<float>(step)), from.y + (way.y * static_cast<float>(step)));
        gizmo_frame(gizmo, view, tool, snap, entity, at, false, commands);
    }
    return at;
}

} // namespace

TEST_CASE("gizmo: a drag on a ring or a scaling handle snaps with snapping on") {
    using de::Vec3;
    constexpr float kDeg = std::numbers::pi_v<float> / 180.0f;
    de::tools::create_imgui();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.Fonts->AddFontDefault();

    auto world = World::create();
    REQUIRE(world.ok());
    const flecs::world& ecs = world->ecs();
    const flecs::entity statue = ecs.entity();
    de::world::place(statue, Transform{.position = {0.0, 0.0, 0.0},
                                       .rotation = glm::angleAxis(40.0f * kDeg, Vec3(0.0f, 1.0f, 0.0f)),
                                       .scale = Vec3(1.2f)});
    CommandStack commands(editor_heap());
    // From in front and above, looking down at it.
    const de::editor::View view{.position = {0.0, 7.0, 7.0},
                                .rotation = glm::angleAxis(-45.0f * kDeg, Vec3(1.0f, 0.0f, 0.0f)),
                                .vertical_fov_rad = 1.0f,
                                .min = ImVec2(0.0f, 0.0f),
                                .size = ImVec2(800.0f, 600.0f)};
    ImVec2 center;
    REQUIRE(view.project(DVec3(0.0), center));
    const de::editor::Snap snap{
        .move = false, .move_m = 0.5f, .rotate = true, .rotate_deg = 15.0f, .scale = true, .scale_step = 0.25f};
    de::editor::Gizmo gizmo;

    // Turning: down from beside the middle to the upright axis's ring (its near half is below the middle), then along.
    const ImVec2 ring = find_handle(gizmo, view, de::editor::Tool::Rotate, snap, statue,
                                    ImVec2(center.x + 20.0f, center.y), ImVec2(0.0f, 1.0f), 1, commands);
    REQUIRE(gizmo.hovered_handle() == 1);
    gizmo_frame(gizmo, view, de::editor::Tool::Rotate, snap, statue, ring, true, commands);
    gizmo_frame(gizmo, view, de::editor::Tool::Rotate, snap, statue, ImVec2(ring.x + 47.0f, ring.y), true, commands);
    gizmo_frame(gizmo, view, de::editor::Tool::Rotate, snap, statue, ImVec2(ring.x + 47.0f, ring.y), false, commands);
    const de::Quat turned = statue.get<WorldTransform>().rotation;
    const float heading_deg = 2.0f * std::atan2(turned.y, turned.w) / kDeg;
    CHECK(std::abs(heading_deg - 40.0f) > 1.0f);
    CHECK(std::abs(std::remainder(heading_deg, 15.0f)) < 1e-2f);

    // Scaling: out from the middle along its own first axis to that handle, then further out.
    const de::Quat now = statue.get<WorldTransform>().rotation;
    ImVec2 tip;
    REQUIRE(view.project(DVec3(now * Vec3(1.0f, 0.0f, 0.0f)), tip));
    const float length = std::hypot(tip.x - center.x, tip.y - center.y);
    const ImVec2 way((tip.x - center.x) / length, (tip.y - center.y) / length);
    const ImVec2 handle = find_handle(gizmo, view, de::editor::Tool::Scale, snap, statue,
                                      ImVec2(center.x + (way.x * 20.0f), center.y + (way.y * 20.0f)), way, 0, commands);
    REQUIRE(gizmo.hovered_handle() == 0);
    const ImVec2 further(handle.x + (way.x * 40.0f), handle.y + (way.y * 40.0f));
    gizmo_frame(gizmo, view, de::editor::Tool::Scale, snap, statue, handle, true, commands);
    gizmo_frame(gizmo, view, de::editor::Tool::Scale, snap, statue, further, true, commands);
    gizmo_frame(gizmo, view, de::editor::Tool::Scale, snap, statue, further, false, commands);
    const Vec3 scale = statue.get<WorldTransform>().scale;
    CHECK(scale.x > 1.3f);
    CHECK(std::abs(std::remainder(scale.x, 0.25f)) < 1e-4f);
    CHECK(scale.y == doctest::Approx(1.2f));
    CHECK(scale.z == doctest::Approx(1.2f));
    de::tools::destroy_imgui();
}

TEST_CASE("panels are resized from either side of the gap between them, and the mouse says so") {
    de::tools::create_imgui();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.Fonts->AddFontDefault();
    ImGui::GetStyle().WindowBorderHoverPadding = 6.0f;
    ImGui::GetStyle().DockingSeparatorSize = 2.0f;
    ImGui::GetStyle().WindowPadding = ImVec2(0.0f, 0.0f); // as the Playground's: its scene reaches its edges
    // Two panels side by side, each all one button, as the Playground's scene is: their items reach the gap.
    bool laid_out = false;
    const auto frame = [&](ImVec2 mouse) {
        io.AddMousePosEvent(mouse.x, mouse.y);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(800.0f, 600.0f));
        ImGui::Begin("host", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking);
        const ImGuiID dock = ImGui::GetID("dock");
        if (!laid_out) {
            laid_out = true;
            ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dock, ImVec2(800.0f, 600.0f));
            ImGuiID right = 0;
            const ImGuiID left = ImGui::DockBuilderSplitNode(dock, ImGuiDir_Left, 0.5f, nullptr, &right);
            ImGui::DockBuilderDockWindow("left", left);
            ImGui::DockBuilderDockWindow("right", right);
            ImGui::DockBuilderFinish(dock);
        }
        de::editor::dock_space_with_splitters(dock, ImVec2(800.0f, 600.0f), ImGuiDockNodeFlags_None);
        ImGui::End();
        for (const char* name : {"left", "right"}) {
            ImGui::Begin(name);
            ImGui::InvisibleButton("all of it", ImGui::GetContentRegionAvail());
            ImGui::End();
        }
        const ImGuiMouseCursor cursor = ImGui::GetMouseCursor();
        ImGui::EndFrame();
        return cursor;
    };
    for (int i = 0; i < 3; ++i) {
        static_cast<void>(frame(ImVec2(600.0f, 300.0f))); // settles the layout
    }
    // Where the gap is, from the left panel's own edge.
    const ImGuiWindow* left_window = ImGui::FindWindowByName("left");
    REQUIRE(left_window != nullptr);
    const float gap = left_window->Pos.x + left_window->Size.x;
    // Four pixels inside the right panel, over its button: a few frames on (the cursor waits for the hover to stay).
    ImGuiMouseCursor cursor = ImGuiMouseCursor_Arrow;
    for (int i = 0; i < 6; ++i) {
        cursor = frame(ImVec2(gap + 2.0f + 4.0f, 300.0f));
    }
    CHECK(cursor == ImGuiMouseCursor_ResizeEW);
    // Well inside it, the panel's own.
    for (int i = 0; i < 6; ++i) {
        cursor = frame(ImVec2(gap + 60.0f, 300.0f));
    }
    CHECK(cursor == ImGuiMouseCursor_Arrow);
    de::tools::destroy_imgui();
}

TEST_CASE("navigation: framing eases the camera back from a sphere, and the wheel zooms toward the pivot") {
    using de::Vec3;
    de::tools::create_imgui();
    de::editor::Navigation navigation;
    de::tools::CameraPose pose{.position = {0.0, 2.0, 0.0}, .yaw_rad = 0.4f, .pitch_rad = -0.2f};
    const Vec3 forward = de::editor::forward_of(pose);
    const de::editor::View view{.position = pose.position, .vertical_fov_rad = 1.0f, .size = ImVec2(800.0f, 600.0f)};
    const DVec3 center(30.0, 1.0, -40.0);
    navigation.frame(pose, center, 2.0f, 1.0f);
    // Halfway through the ease it's on its way; after it, the sphere's centre is the pivot, straight ahead.
    CHECK(navigation.update(view, false, false, 0.05f, pose));
    CHECK(glm::length(pose.position - DVec3(0.0, 2.0, 0.0)) > 0.0);
    CHECK(navigation.update(view, false, false, 1.0f, pose));
    const DVec3 pivot = pose.position + DVec3(forward * navigation.pivot_distance_m());
    CHECK(glm::length(pivot - center) < 1e-3);
    CHECK(navigation.pivot_distance_m() == doctest::Approx(2.0f * 2.0f / std::tan(0.5f)));
    // A notch of the wheel takes a fifth of the way to the pivot, which stays where it was.
    const float before = navigation.pivot_distance_m();
    ImGui::GetIO().MouseWheel = 1.0f;
    CHECK(navigation.update(view, true, false, 0.016f, pose));
    ImGui::GetIO().MouseWheel = 0.0f;
    CHECK(navigation.pivot_distance_m() == doctest::Approx(before * 0.8f));
    CHECK(glm::length((pose.position + DVec3(forward * navigation.pivot_distance_m())) - center) < 1e-3);
    // Flying takes the camera: nothing else moves it.
    ImGui::GetIO().MouseWheel = 1.0f;
    CHECK_FALSE(navigation.update(view, true, true, 0.016f, pose));
    ImGui::GetIO().MouseWheel = 0.0f;
    de::tools::destroy_imgui();
}

TEST_CASE("a drawer over the docked panels stays in front of them, and behind its own popups") {
    de::tools::create_imgui();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.Fonts->AddFontDefault();
    bool laid_out = false;
    bool focus_panel = false;
    bool open_popup = false;
    bool item_hovered = false;
    const auto frame = [&](ImVec2 mouse) {
        io.AddMousePosEvent(mouse.x, mouse.y);
        ImGui::NewFrame();
        if (focus_panel) {
            ImGui::SetWindowFocus("panel");
            focus_panel = false;
        }
        // The editor's host: the panels are docked in it, and it never comes in front of what's over them.
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(800.0f, 600.0f));
        ImGui::Begin("host", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        const ImGuiID dock = ImGui::GetID("dock");
        if (!laid_out) {
            laid_out = true;
            ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dock, ImVec2(800.0f, 600.0f));
            ImGui::DockBuilderDockWindow("panel", dock);
            ImGui::DockBuilderFinish(dock);
        }
        de::editor::dock_space_with_splitters(dock, ImVec2(800.0f, 600.0f), ImGuiDockNodeFlags_None);
        ImGui::End();
        ImGui::Begin("panel");
        ImGui::InvisibleButton("all of it", ImGui::GetContentRegionAvail());
        ImGui::End();
        // The drawer over the bottom half, and a popup of its own (the favourites' list, say) over it.
        if (de::editor::begin_over_panels("drawer", ImVec2(0.0f, 300.0f), ImVec2(800.0f, 300.0f),
                                          ImGuiWindowFlags_NoDecoration)) {
            if (open_popup) {
                ImGui::OpenPopup("list");
                open_popup = false;
            }
            ImGui::SetNextWindowPos(ImVec2(100.0f, 350.0f));
            if (ImGui::BeginPopup("list")) {
                ImGui::Selectable("a favourite", false, ImGuiSelectableFlags_None, ImVec2(200.0f, 40.0f));
                item_hovered = ImGui::IsItemHovered();
                ImGui::EndPopup();
            }
        }
        ImGui::End();
        const ImGuiWindow* hovered = GImGui->HoveredWindow;
        ImGui::EndFrame();
        return hovered;
    };
    const ImVec2 over_both(400.0f, 450.0f); // the drawer, with the panel under it
    for (int i = 0; i < 4; ++i) {
        static_cast<void>(frame(over_both));
    }
    const ImGuiWindow* drawer = ImGui::FindWindowByName("drawer");
    REQUIRE(drawer != nullptr);
    CHECK(frame(over_both) == drawer);
    // The panel under it taking the keyboard doesn't bring it in front.
    focus_panel = true;
    for (int i = 0; i < 4; ++i) {
        static_cast<void>(frame(over_both));
    }
    CHECK(frame(over_both) == drawer);
    // Its popup stays in front of it, frame after frame, and its item takes the mouse.
    open_popup = true;
    for (int i = 0; i < 6; ++i) {
        static_cast<void>(frame(ImVec2(150.0f, 370.0f)));
    }
    CHECK(item_hovered);
    de::tools::destroy_imgui();
}
