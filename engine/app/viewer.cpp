#include "app/viewer.h"

#include "asset/asset_id.h"
#include "asset/world_format.h"
#include "core/log.h"
#include "core/profile.h"
#include "core/string_id.h"
#include "asset/mesh_format.h"
#include "physics/physics.h"
#include "platform/input.h"
#include "render/scene/scene_renderer.h"
#include "world/cell.h"
#include "world/render_extract.h"
#include "world/transform.h"
#include "world/world.h"

#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdint>
#include <string_view>

namespace de::app {

using namespace core::literals;

Viewer::Viewer() : free_camera_({0.0, 0.0, 0.0}, 0.0f, 0.0f) {}

bool Viewer::startup(Engine& engine, const FrameworkArgs& args) {
    const char* const flag = flag_value(args, "--world");
    const std::string_view text = flag != nullptr ? std::string_view(flag) : setting(engine.settings, "world");
    asset::AssetId id;
    if (!asset::parse_asset_id(text, id)) {
        DE_LOG_ERROR("world",
                     "no world to load: --world, or world in config/engine.settings, takes an asset ID "
                     "(16 hex digits): '%.*s'",
                     static_cast<int>(text.size()), text.data());
        return false;
    }
    // Loaded before the first frame (a loading screen's worth), with its environment.
    world_ = engine.assets.load<asset::WorldAsset>(id, platform::IoPriority::Critical);
    engine.assets.wait_all();
    const auto* world = engine.assets.get<asset::WorldAsset>(world_);
    if (world == nullptr) {
        DE_LOG_ERROR("world", "world %.*s didn't load (logged)", static_cast<int>(text.size()), text.data());
        return false;
    }
    free_camera_ = tools::FreeCamera({world->start_position[0], world->start_position[1], world->start_position[2]},
                                     world->start_yaw_rad, world->start_pitch_rad);
    apply_world(engine);

    quit_ = engine.input.add_action("quit"_sid, platform::ActionKind::Button);
    camera_actions_ = tools::add_free_camera_actions(engine.input);
    camera_ = engine.world.ecs().entity("free_camera").set(world::Camera{.vertical_fov_rad = 1.0f});
    engine.world.ecs().set(world::ActiveCamera{.entity = camera_.id()});
#if DE_BUILD_DEV
    next_view_ = engine.input.add_action("debug.next_view"_sid, platform::ActionKind::Button);
    toggle_taa_ = engine.input.add_action("debug.toggle_taa"_sid, platform::ActionKind::Button);
    freeze_cull_ = engine.input.add_action("debug.freeze_cull"_sid, platform::ActionKind::Button);
    drop_rocks_ = engine.input.add_action("debug.drop_rocks"_sid, platform::ActionKind::Button);
    drop_rubble_ = engine.input.add_action("debug.drop_rubble"_sid, platform::ActionKind::Button);
    engine.freeze_cull = has_flag(args, "--freeze-cull");
    if (engine.freeze_cull) {
        DE_LOG_INFO("render", "cull frozen at the first frame's camera (--freeze-cull; F4 to go live)");
    }
#endif
    return true;
}

void Viewer::apply_world(Engine& engine) {
    world_revision_ = engine.assets.revision(world_);
    const auto* world = engine.assets.get<asset::WorldAsset>(world_);
    if (world == nullptr) {
        return;
    }
    const std::span<const std::byte> file = engine.assets.file(world_);
    const std::span<const asset::WorldCell> listed = world->cells.get(file.data());
    const auto lists = [&](asset::AssetId id) {
        for (const asset::WorldCell& cell : listed) {
            if (cell.cell == id) {
                return true;
            }
        }
        return false;
    };
    // A few cells, at load and when the world is re-cooked: linear searches are fine.
    for (size_t i = cells_.size(); i-- > 0;) {
        if (!lists(cells_[i].id)) {
            cells_[i].entity.destruct();
            cells_.remove_swap(i);
        }
    }
    for (const asset::WorldCell& cell : listed) {
        bool loaded = false;
        for (const LoadedCell& have : cells_) {
            loaded = loaded || have.id == cell.cell;
        }
        if (const flecs::entity entity = loaded ? flecs::entity() : world::load_cell(engine.world.ecs(), cell.cell);
            entity.is_valid()) {
            cells_.push({.id = cell.cell, .entity = entity}); // load_cell logged an invalid one
        }
    }
    // No environment: the lighting stays as it is (the engine's defaults at first).
    environment_ = world->environment == asset::WorldAsset::kNone
                       ? asset::AssetHandle{}
                       : engine.assets.find(world->header.dependencies.get(file.data())[world->environment]);
    environment_revision_ = 0; // applied by the next update()
    DE_LOG_INFO("world", "%zu cells, environment %s", listed.size(),
                engine.assets.state(environment_) == asset::AssetState::Ready ? "ready" : "missing");
}

bool Viewer::update(Engine& engine, float frame_s) {
    DE_PROFILE_ZONE("Viewer::update");
    if (engine.input.state(quit_).pressed) {
        return false;
    }
    if (engine.assets.revision(world_) != world_revision_) {
        apply_world(engine); // re-cooked; the camera stays where it is
    }
    if (const uint32_t revision = engine.assets.revision(environment_); revision != environment_revision_) {
        environment_revision_ = revision;
        if (const auto* environment = engine.assets.get<asset::EnvironmentAsset>(environment_)) {
            engine.lighting = render::scene_lighting(*environment);
            DE_LOG_INFO("render", "lighting from the environment (revision %u)", revision);
        }
    }
    free_camera_.update(engine.input, camera_actions_, frame_s, camera_);
#if DE_BUILD_DEV
    if (engine.input.state(next_view_).pressed) {
        engine.view = static_cast<render::SceneView>((static_cast<uint32_t>(engine.view) + 1) %
                                                     static_cast<uint32_t>(render::SceneView::Count));
        DE_LOG_INFO("render", "view: %s", render::scene_view_name(engine.view));
    }
    if (engine.input.state(toggle_taa_).pressed) {
        engine.taa = !engine.taa;
        DE_LOG_INFO("render", "TAA %s", engine.taa ? "on" : "off");
    }
    if (engine.input.state(drop_rocks_).pressed) {
        drop_rocks(engine, 30, false);
    }
    if (engine.input.state(drop_rubble_).pressed) {
        drop_rocks(engine, 1000, true);
    }
    if (engine.input.state(freeze_cull_).pressed) {
        engine.freeze_cull = !engine.freeze_cull;
        DE_LOG_INFO("render", "cull %s", engine.freeze_cull ? "frozen where the camera is" : "live");
    }
#endif
    return true;
}

#if DE_BUILD_DEV
void Viewer::drop_rocks(Engine& engine, uint32_t count, bool rubble) {
    if (!rock_mesh_.is_valid()) {
        // The test level's boulder (content/props/boulder_01).
        asset::Guid guid;
        if (!asset::parse_guid("6681c1d7-0c12-4528-9523-2642e902ae2d", guid)) {
            return;
        }
        rock_mesh_ = engine.assets.load<asset::MeshAsset>(asset::asset_id(guid), platform::IoPriority::Critical);
        engine.assets.wait_all();
    }
    const auto* mesh = engine.assets.get<asset::MeshAsset>(rock_mesh_);
    if (mesh == nullptr) {
        DE_LOG_ERROR("physics", "no rock to drop: the boulder's mesh didn't load");
        return;
    }
    if (rock_half_.x == 0.0f) {
        // A box around the mesh, a little inside it so rocks settle into each other: the bounding sphere is far too big for
        // a wide, flat rock, which then hangs in the air.
        Vec3 low(1e9f);
        Vec3 high(-1e9f);
        for (const asset::MeshVertex& vertex : mesh->vertices.get(engine.assets.file(rock_mesh_).data())) {
            const Vec3 position(vertex.position[0], vertex.position[1], vertex.position[2]);
            low = glm::min(low, position);
            high = glm::max(high, position);
        }
        rock_center_ = (low + high) * 0.5f;
        rock_half_ = (high - low) * 0.5f * 0.92f;
        DE_LOG_INFO("physics", "rock box: %.2f x %.2f x %.2f m at scale 1; bounding sphere radius %.2f m",
                    static_cast<double>(rock_half_.x * 2.0f), static_cast<double>(rock_half_.y * 2.0f),
                    static_cast<double>(rock_half_.z * 2.0f), static_cast<double>(mesh->bounds.radius));
    }
    flecs::world& ecs = engine.world.ecs();
    if (!floor_made_) {
        // The ground has no collision of its own until mesh collision lands: a floor where the test level's ground is.
        floor_made_ = true;
        const flecs::entity floor = ecs.entity();
        world::place(floor, world::Transform{.position = {64.0, -0.5, 64.0}});
        floor.set(physics::Collider{.shape = physics::ShapeKind::Box, .half_extents_m = Vec3(400.0f, 0.5f, 400.0f)});
        floor.set(physics::RigidBody{.kind = physics::BodyKind::Static, .friction = 0.8f});
    }
    const auto random = [this] {
        rock_random_ ^= rock_random_ << 13U;
        rock_random_ ^= rock_random_ >> 7U;
        rock_random_ ^= rock_random_ << 17U;
        return static_cast<float>(rock_random_ % 100000U) / 100000.0f;
    };
    // In front of the camera, and above it. The camera's -Z is ahead.
    const auto& view = camera_.get<world::WorldTransform>();
    const DVec3 ahead = DVec3(view.rotation * Vec3(0.0f, 0.0f, -1.0f));
    const DVec3 middle = view.position + (DVec3(ahead.x, 0.0, ahead.z) * 14.0) + DVec3(0.0, rubble ? 25.0 : 10.0, 0.0);
    const auto side = static_cast<uint32_t>(std::ceil(std::cbrt(static_cast<float>(count))));
    const float spacing_m = rubble ? 0.9f : 2.2f;
    for (uint32_t i = 0; i < count; ++i) {
        const auto column = static_cast<float>(i % side) - (static_cast<float>(side) * 0.5f);
        const auto row = static_cast<float>((i / side) % side) - (static_cast<float>(side) * 0.5f);
        const auto layer = static_cast<float>(i / (side * side));
        const float scale = rubble ? 0.12f + (random() * 0.18f) : 0.35f + (random() * 0.6f);
        const Quat rotation = glm::angleAxis(random() * 6.2831853f, glm::normalize(Vec3(random() - 0.5f, random() - 0.5f, random() - 0.5f) + Vec3(0.0f, 0.001f, 0.0f)));
        // The body is a box around the mesh, whose middle isn't where its origin is, so the mesh hangs off it.
        const flecs::entity body = ecs.entity();
        world::place(body, world::Transform{.position = middle + DVec3(column * spacing_m, layer * spacing_m, row * spacing_m),
                                            .rotation = rotation,
                                            .scale = Vec3(scale)});
        body.set(physics::Collider{.shape = physics::ShapeKind::Box, .half_extents_m = rock_half_});
        body.set(physics::RigidBody{.kind = physics::BodyKind::Dynamic,
                                    .mass_kg = 200.0f * scale * scale * scale,
                                    .friction = 0.8f,
                                    .restitution = 0.25f,
                                    .debris = rubble});
        const flecs::entity shown = ecs.entity();
        world::attach(shown, body);
        world::place(shown, world::Transform{.position = -DVec3(rock_center_)});
        shown.set(world::MeshInstance{.mesh = rock_mesh_});
    }
    rocks_dropped_ += count;
    DE_LOG_INFO("physics", "%u dropped, %u so far", count, rocks_dropped_);
}
#endif

void Viewer::shutdown(Engine& engine) {
#if DE_BUILD_DEV
    if (rock_mesh_.is_valid()) {
        engine.assets.release(rock_mesh_);
    }
#endif
    if (world_.is_valid()) {
        engine.assets.release(world_);
    }
    world_ = {};
    environment_ = {};
}

} // namespace de::app
