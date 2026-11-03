#pragma once

#include "app/framework.h"
#include "asset/registry.h"
#include "core/array.h"
#include "tools/free_camera.h"

#include <flecs.h>

namespace de::app {

// The application deengine runs until the game's own lands: loads a world (every cell of it, until
// the streamer picks cells around the player), lights it by its environment, and flies a free camera
// through it from the world's start (WASD/QE and the right mouse button, or a gamepad). A re-cooked
// world or environment applies as it lands. In dev builds F2 steps through the G-buffer's channels.
//   --world ID   the cooked world (asset ID, 16 hex digits; default: config/engine.settings' world)
class Viewer final : public Application {
public:
    Viewer();
    bool startup(Engine& engine, const FrameworkArgs& args) override;
    bool update(Engine& engine, float frame_s) override;
    void shutdown(Engine& engine) override;

private:
    struct LoadedCell {
        asset::AssetId id;
        flecs::entity entity;
    };

    // The world's cells (those it no longer lists unloaded, new ones loaded) and its environment.
    void apply_world(Engine& engine);

    asset::AssetHandle world_;
    uint32_t world_revision_ = 0;
    asset::AssetHandle environment_; // the world's dependency: held for it
    uint32_t environment_revision_ = 0;
    core::Array<LoadedCell> cells_{core::heap_allocator(core::MemoryTag::World)};
    platform::ActionId quit_{}; // Escape, gamepad Back: no game to pause yet
    tools::FreeCameraActions camera_actions_{};
    tools::FreeCamera free_camera_;
    flecs::entity camera_;
#if DE_BUILD_DEV
    platform::ActionId next_view_{}; // F2: the next G-buffer view
#endif
};

} // namespace de::app
