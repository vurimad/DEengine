#pragma once

#include "app/framework.h"
#include "tools/free_camera.h"

#include <flecs.h>

namespace de::app {

// The application deengine runs until the game's own lands: loads a cell and flies a free camera
// through it (WASD/QE and the right mouse button, or a gamepad). In dev builds F2 steps through
// the G-buffer's channels.
//   --cell ID   the cooked cell to load (asset ID, 16 hex digits; default: the test cell)
class Viewer final : public Application {
public:
    Viewer();
    bool startup(Engine& engine, const FrameworkArgs& args) override;
    void update(Engine& engine, float frame_s) override;

private:
    tools::FreeCameraActions camera_actions_{};
    tools::FreeCamera free_camera_;
    flecs::entity camera_;
#if DE_BUILD_DEV
    platform::ActionId next_view_{}; // F2: the next G-buffer view
#endif
};

} // namespace de::app
