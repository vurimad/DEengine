#pragma once

#include "asset/registry.h"
#include "render/scene/scene_renderer.h"

#include <cstdint>
#include <string_view>

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
//   --deadzone X     stick deadzone, 0..0.9 (default 0.3)
//   --lod-error-pixels X  how far a LOD may lie from LOD 0 (default 1); 0 draws LOD 0 everywhere
//   --lod-fade X     how long LODs take to fade into each other, as a share of a LOD's error (default 0.2); 0 cuts
//   --no-merged-draws        a draw per object, instead of one instanced draw per mesh part, to compare
//   --no-shadow-caster-cull  cascades draw all their maps could hold, to compare: no picture should change
//   --shadow-min-texels X    casters narrower than this many of a cascade's texels don't draw in it (default 0: all
//   draw)
//   --show-sticks    dev builds: deadzone off, raw stick values logged twice a second (drift)
struct FrameworkArgs {
    int argc = 0;
    char** argv = nullptr;
    uint64_t frames = 0; // 0: until the window closes
    const char* game_dir = "cooked";
    const char* src_dir = nullptr;       // null: this repository, in dev builds
    float deadzone = -1.0f;              // below 0: the default
    float lod_error_pixels = 1.0f;       // --lod-error-pixels: 0 draws LOD 0 everywhere, to compare with
    float lod_fade = 0.2f;               // --lod-fade: 0 cuts between LODs, to compare with
    float shadow_min_texels = 0.0f;      // --shadow-min-texels
    bool shadow_caster_cull = true;      // --no-shadow-caster-cull
    bool merged_draws = true;            // --no-merged-draws
    uint32_t window_width_points = 1600; // --window 2560x1440
    uint32_t window_height_points = 900;
    bool fullscreen = false; // --fullscreen: the display's own size, whatever the window's
    bool vsync = true;
    bool validation = true;
    bool show_sticks = false;
};

// False (logged) on a malformed flag.
bool parse_args(int argc, char** argv, FrameworkArgs& out);
// The value after a flag ("--world 3f8a..."), or null if it isn't there.
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
    shader::ShaderLibrary& shaders;
    asset::AssetRegistry& assets;
    world::World& world;
    render::GpuAssets& gpu_assets;
    // config/engine.settings, as text (read with setting(engine.settings, "name")).
    std::string_view settings;
    // What each frame draws, as the application sets it.
    render::SceneLighting lighting;
    render::SceneView view = render::SceneView::Lit;
    bool taa = false; // temporal anti-aliasing; `taa on` in config/engine.settings starts it on
    asset::AssetHandle terrain; // the ground, a .deterrain the application keeps loaded; none: no ground
    bool fog_low_camera = true; // SceneRenderer::set_fog_low_camera
#if DE_BUILD_DEV
    bool freeze_cull = false; // a debug view of the cull: SceneRenderer::set_cull_frozen
#endif
};

// A value from settings text like config/engine.settings (Engine::settings): one setting a line, a
// name and a value; '#' starts a comment. Empty if it isn't there.
std::string_view setting(std::string_view settings, std::string_view name);

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
    // frame_s is what this frame moves things by (FrameClock): over 0, in even steps while the frame rate
    // holds the refresh rate, otherwise the time measured.
    virtual bool update(Engine& engine, float frame_s) = 0;
    // After the last frame (or a failed start: whenever startup() ran), while the engine is still
    // up: let go of what it holds.
    virtual void shutdown(Engine& /*engine*/) {}
};

// Brings the engine up, runs the application in the frame loop until the window closes, the
// application ends the run or --frames are done, and takes it down. The process's exit code: failure if anything
// couldn't start, or validation reported errors.
int run_framework(Application& application, const FrameworkArgs& args);

} // namespace de::app
