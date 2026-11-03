#pragma once

#include "render/scene/scene_renderer.h"

#include <cstdint>

namespace de::core {
class JobSystem;
}
namespace de::platform {
class Vfs;
class Window;
class Input;
} // namespace de::platform
namespace de::rhi {
class Device;
class Uploader;
} // namespace de::rhi
namespace de::render {
class RenderGraph;
class GpuAssets;
} // namespace de::render
namespace de::shader {
class ShaderLibrary;
}
namespace de::asset {
class AssetRegistry;
}
namespace de::world {
class World;
}

namespace de::app {

// What the command line asks of the framework. The application reads its own flags with
// flag_value() / has_flag().
//   --frames N       exit after N frames (smoke runs; validation errors fail the run)
//   --no-vsync       present without waiting for vertical blank
//   --no-validation  skip the Vulkan validation layer in debug and dev builds
//   --game-dir DIR   folder mounted as game:/ (default ./cooked)
//   --src-dir DIR    folder mounted as src:/ in dev builds (default: this repository)
//   --deadzone X     stick deadzone, 0..0.9 (default 0.15)
//   --show-sticks    dev builds: deadzone off, raw stick values logged twice a second (drift)
struct FrameworkArgs {
    int argc = 0;
    char** argv = nullptr;
    uint64_t frames = 0; // 0: until the window closes
    const char* game_dir = "cooked";
    const char* src_dir = nullptr; // null: this repository, in dev builds
    float deadzone = -1.0f;        // below 0: the default
    bool vsync = true;
    bool validation = true;
    bool show_sticks = false;
};

// False (logged) on a malformed flag.
bool parse_args(int argc, char** argv, FrameworkArgs& out);
// The value after a flag ("--cell 3f8a..."), or null if it isn't there.
const char* flag_value(const FrameworkArgs& args, const char* flag);
bool has_flag(const FrameworkArgs& args, const char* flag);

// The running engine, for the application: its systems, and what the frame draws.
struct Engine {
    core::JobSystem& jobs;
    platform::Vfs& vfs;
    platform::Window& window;
    platform::Input& input;
    rhi::Device& device;
    rhi::Uploader& uploader;
    render::RenderGraph& graph;
    shader::ShaderLibrary* shaders; // null when no shaders could be loaded (the window is cleared)
    asset::AssetRegistry& assets;
    world::World& world;
    render::GpuAssets& gpu_assets;
    // What each frame draws, as the application sets it.
    render::SceneLighting lighting;
    render::SceneView view = render::SceneView::Lit;
};

// What runs inside the framework: the game, or a tool. The framework owns the engine and the frame
// loop; the application says what to load and does its per-frame work.
class Application {
public:
    Application() = default;
    virtual ~Application() = default;
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    Application(Application&&) = delete;
    Application& operator=(Application&&) = delete;

    // Once the engine is up, before the first frame: add input actions (their bindings are read
    // after this), load the world. False stops the run.
    virtual bool startup(Engine& engine, const FrameworkArgs& args) = 0;
    // Every frame, after input is sampled and before the world advances. False ends the run (the
    // application's quit: the framework has no quit of its own, so the game can ask first).
    virtual bool update(Engine& engine, float frame_s) = 0;
};

// Brings the engine up, runs the application in the frame loop until the window closes, the
// application ends the run or --frames are done, and takes it down. The process's exit code: failure if anything
// couldn't start, or validation reported errors.
int run_framework(Application& application, const FrameworkArgs& args);

} // namespace de::app
