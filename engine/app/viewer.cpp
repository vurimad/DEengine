#include "app/viewer.h"

#include "asset/asset_id.h"
#include "core/log.h"
#include "core/profile.h"
#include "core/string_id.h"
#include "platform/input.h"
#include "world/cell.h"
#include "world/render_extract.h"
#include "world/world.h"

#include <cstdint>

namespace de::app {

using namespace core::literals;

namespace {

// content/world/test/cell_0_0.cell, seen from its south side: until a world asset lists the cells
// and where the player starts.
constexpr asset::AssetId kTestCell = asset::AssetId::from_value(0xbf4c53ca6b99a5e7);
constexpr DVec3 kStartPosition{64.0, 2.0, 74.0};
constexpr float kStartPitchRad = -0.12f;

} // namespace

Viewer::Viewer() : free_camera_(kStartPosition, 0.0f, kStartPitchRad) {}

bool Viewer::startup(Engine& engine, const FrameworkArgs& args) {
    asset::AssetId cell = kTestCell;
    if (const char* flag = flag_value(args, "--cell"); flag != nullptr && !asset::parse_asset_id(flag, cell)) {
        DE_LOG_ERROR("world", "--cell takes an asset ID, 16 hex digits: '%s'", flag);
        return false;
    }
    // One cell, until the streamer picks cells around the player (M1). Its members spawn once it and
    // its meshes are loaded.
    world::load_cell(engine.world.ecs(), cell);

    camera_actions_ = tools::add_free_camera_actions(engine.input);
    camera_ = engine.world.ecs().entity("free_camera").set(world::Camera{.vertical_fov_rad = 1.0f});
    engine.world.ecs().set(world::ActiveCamera{.entity = camera_.id()});
#if DE_BUILD_DEV
    next_view_ = engine.input.add_action("debug.next_view"_sid, platform::ActionKind::Button);
#endif
    return true;
}

void Viewer::update(Engine& engine, float frame_s) {
    DE_PROFILE_ZONE("Viewer::update");
    free_camera_.update(engine.input, camera_actions_, frame_s, camera_);
#if DE_BUILD_DEV
    if (engine.input.state(next_view_).pressed) {
        engine.view = static_cast<render::SceneView>((static_cast<uint32_t>(engine.view) + 1) %
                                                     static_cast<uint32_t>(render::SceneView::Count));
        DE_LOG_INFO("render", "view: %s", render::scene_view_name(engine.view));
    }
#endif
}

} // namespace de::app
