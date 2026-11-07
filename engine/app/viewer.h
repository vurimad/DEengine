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
// world or environment applies as it lands. In dev builds F2 steps through the G-buffer's channels, and F4 freezes
// the cull where the camera is.
//   --world ID      the cooked world (asset ID, 16 hex digits; default: config/engine.settings' world)
//   --freeze-cull   dev builds: start with the cull frozen, at the first frame's camera
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
#if DE_BUILD_DEV
    // A demo of physics: F6 drops rocks in front of the camera, F7 a shower of rubble.
    void drop_rocks(Engine& engine, uint32_t count, bool rubble);
    platform::ActionId drop_rocks_{};
    platform::ActionId drop_rubble_{};
    asset::AssetHandle rock_mesh_;
    bool floor_made_ = false;
    uint32_t rocks_dropped_ = 0;
    Vec3 rock_center_{0.0f}; // of the box around the mesh, in its own space
    Vec3 rock_half_{0.0f};
    uint64_t rock_random_ = 0x9e3779b97f4a7c15ULL;
#endif
    uint32_t world_revision_ = 0;
    asset::AssetHandle environment_; // the world's dependency: held for it
    uint32_t environment_revision_ = 0;
    core::Array<LoadedCell> cells_{core::heap_allocator(core::MemoryTag::World)};
    platform::ActionId quit_{}; // Escape, gamepad Back: no game to pause yet
    tools::FreeCameraActions camera_actions_{};
    tools::FreeCamera free_camera_;
    flecs::entity camera_;
#if DE_BUILD_DEV
    platform::ActionId next_view_{};   // F2: the next G-buffer view
    platform::ActionId toggle_taa_{};  // F3: temporal anti-aliasing on and off, to compare
    platform::ActionId freeze_cull_{}; // F4: the cull stays where it is, and the camera can leave it
#endif
};

} // namespace de::app
