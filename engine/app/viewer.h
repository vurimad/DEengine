#pragma once

#include "app/framework.h"
#include "asset/registry.h"
#include "core/array.h"
#include "tools/free_camera.h"

#include <flecs.h>

#include <algorithm>

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

#if DE_BUILD_DEV
    // For the editor, which runs a viewer inside it. Escape doesn't quit (the window's close does).
    void set_quit_on_escape(bool quit) { quit_on_escape_ = quit; }
    // Whether the free camera reads input this frame (the mouse is over the viewport); otherwise it stays put.
    void set_camera_input(bool on) { camera_input_ = on; }
    const tools::FreeCameraActions& camera_actions() const { return camera_actions_; }
    // Puts the camera there, looking that way (yaw about world up, pitch up from level), as if it had flown.
    void set_camera_pose(const tools::CameraPose& pose) { free_camera_.set_pose(pose); }
    void set_camera_speed(float speed_mps) {
        free_camera_.speed_mps =
            std::clamp(speed_mps, tools::FreeCamera::kMinSpeedMps, tools::FreeCamera::kMaxSpeedMps);
    }
    const tools::FreeCamera& camera() const { return free_camera_; }
    flecs::entity camera_entity() const { return camera_; }
    // Lets go of the world shown (its cells unload) and opens another, the camera at its start. False (logged) if it
    // doesn't load: the old one is gone either way.
    bool open_world(Engine& engine, asset::AssetId id);
    asset::AssetId world_id() const { return world_id_; }
    // The world startup() opens in place of the settings' (the editor's: what it had open last), unless --world names
    // one. If it doesn't load, the settings' world opens after all.
    void set_first_world(asset::AssetId id) { first_world_ = id; }
#endif

private:
    struct LoadedCell {
        asset::AssetId id;
        flecs::entity entity;
    };

    // The world's cells (those it no longer lists unloaded, new ones loaded) and its environment.
    void apply_world(Engine& engine);

    asset::AssetHandle world_;
    asset::AssetId world_id_;
    asset::AssetId first_world_; // set_first_world()
    // Loads a world before the first frame (or when the editor opens one), the camera at its start.
    bool load_world(Engine& engine, asset::AssetId id);
#if DE_BUILD_DEV
    bool quit_on_escape_ = true;
    bool camera_input_ = true;
    // A demo of physics: F6 drops rocks in front of the camera, F7 a shower of rubble.
    void drop_rocks(Engine& engine, uint32_t count, bool rubble);
    platform::ActionId drop_rocks_{};
    platform::ActionId drop_rubble_{};
    asset::AssetHandle rock_mesh_;
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
    platform::ActionId next_view_{};             // F2: the next G-buffer view
    platform::ActionId toggle_taa_{};            // F3: temporal anti-aliasing on and off, to compare
    platform::ActionId freeze_cull_{};           // F4: the cull stays where it is, and the camera can leave it
    platform::ActionId toggle_fog_low_camera_{}; // F5: height fog thinning with a low camera on and off, to compare
#endif
};

} // namespace de::app
