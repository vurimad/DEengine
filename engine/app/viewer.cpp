#include "app/viewer.h"

#include "asset/asset_id.h"
#include "asset/world_format.h"
#include "core/log.h"
#include "core/profile.h"
#include "core/string_id.h"
#include "platform/input.h"
#include "render/scene/scene_renderer.h"
#include "world/cell.h"
#include "world/render_extract.h"
#include "world/world.h"

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
#endif
    return true;
}

void Viewer::shutdown(Engine& engine) {
    if (world_.is_valid()) {
        engine.assets.release(world_);
    }
    world_ = {};
    environment_ = {};
}

} // namespace de::app
