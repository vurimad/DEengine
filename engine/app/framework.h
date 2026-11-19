#pragma once

#include "app/agent_eyes.h"
#include "asset/registry.h"
#include "render/scene/scene_renderer.h"

#include <flecs.h>

#include <cstdint>
#include <span>
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
namespace de::console {
class Console;
}
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
namespace de::physics {
class Physics;
}
namespace de::tools {
class ConsoleUi;
class FrameDebugger;
class StatsPanel;
} // namespace de::tools
namespace de::world {
class World;
}

namespace de::app {

// What the command line asks of the framework. The application reads its own flags with
// flag_value() / has_flag().
//   --frames N       exit after N frames (smoke runs; validation errors fail the run)
//   --no-vsync       present without waiting for vertical blank
//   --no-validation  skip the Vulkan validation layer in debug and dev builds
//   --no-shader-cache  compile every shader from its source at start, taking none from user:/shader_cache
//   --shader-compiles N  at most N shaders compiled at once (default: a quarter of the workers, 4 at most)
//   --game-dir DIR   folder mounted as game:/ (default ./cooked; a dev build without one there uses its source tree's)
//   --project DIR    dev builds: the project to open, its folder or .deproject file, relative to the working
//                    directory or the engine's repository (default: the build's, projects/sandbox); its sources
//                    are src:/, its cooked/ game:/
//   --deadzone X     stick deadzone, 0..0.9 (default 0.3)
//   --lod-error-pixels X  how far a LOD may lie from LOD 0 (default 1); 0 draws LOD 0 everywhere
//   --lod-fade X     how long LODs take to fade into each other, as a share of a LOD's error (default 0.2); 0 cuts
//   --no-merged-draws        a draw per object, instead of one instanced draw per mesh part, to compare
//   --no-shadow-caster-cull  cascades draw all their maps could hold, to compare: no picture should change
//   --shadow-min-texels X    casters narrower than this many of a cascade's texels don't draw in it (default 0: all
//   draw)
//   --show-sticks    dev builds: deadzone off, raw stick values logged twice a second (drift)
//   Dev builds: the agent's eyes (--camera, --still, --camera-path, --fixed-step and the rest): app/agent_eyes.h
struct FrameworkArgs {
    int argc = 0;
    char** argv = nullptr;
    uint64_t frames = 0; // 0: until the window closes
    const char* game_dir = "cooked";
    const char* project = nullptr;       // --project: dev builds; null: the build's default (DE_DEFAULT_PROJECT)
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
    uint32_t fps_cap = 0; // --fps-cap 60: no more frames a second than this, on a machine others use; 0: none
    // Dev builds: --frame-debugger starts with the frame debugger open (tools/frame_debugger.h), for pictures of it.
    bool validation = true;
    bool shader_cache = true;     // --no-shader-cache: off
    uint32_t shader_compiles = 0; // --shader-compiles N; 0: the default
    bool show_sticks = false;
#if DE_BUILD_DEV
    EyesFlags eyes;
    uint32_t stats_sections =
        0; // --stats frame,gpu,...: what the frame stats show (tools/stats_panel.h); 0 the default
#endif
};

// False (logged) on a malformed flag.
bool parse_args(int argc, char** argv, FrameworkArgs& out);
// The value after a flag ("--world 3f8a..."), or null if it isn't there.
const char* flag_value(const FrameworkArgs& args, const char* flag);
bool has_flag(const FrameworkArgs& args, const char* flag);

#if DE_BUILD_DEV
// Dev builds: the scene drawn into a texture the application shows in its UI (the editor's viewport panel), instead of
// over the whole window. The application asks for it in ui() every frame it shows it; a frame that doesn't ask draws
// the scene to the window as usual.
struct SceneViewport {
    // Asked by the application in ui(): the size in pixels. A new size takes effect the next frame (the image is
    // stretched for that one frame).
    uint32_t want_width = 0;
    uint32_t want_height = 0;
    // Set by the framework before ui(): what shows this frame's scene with ImGui::Image, 0 until there is a texture,
    // and its size in pixels.
    uint64_t texture_id = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    // Asked by the application in ui() with the size: where the picture is in the UI (its coordinates), for what's
    // drawn over it (the frame stats). 0 wide: the whole window.
    float ui_x = 0.0f;
    float ui_y = 0.0f;
    float ui_width = 0.0f;
    float ui_height = 0.0f;
    // Set by the application when it drew the picture's overlays itself, inside its panel: the frame stats
    // (StatsPanel::draw_in_window) and the notices (tools::draw_viewport_notice).
    bool overlays_drawn = false;
};
#endif

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
    physics::Physics& physics;
    render::GpuAssets& gpu_assets;
    // Named commands and variables run from text: the console window (`), --exec, the agents' socket. An application
    // adds its own, and removes them (Console::remove) when it shuts down.
    console::Console& console;
    // config/engine.settings, as text (read with setting(engine.settings, "name")).
    std::string_view settings;
    // What each frame draws, as the application sets it.
    render::SceneLighting lighting;
    render::SceneView view = render::SceneView::Lit;
    bool taa = false;                   // temporal anti-aliasing; `taa on` in config/engine.settings starts it on
    int32_t debug_jitter = 0;           // one of TAA's sub-pixel offsets with TAA off: SceneRenderer::set_debug_jitter
    bool local_shadow_cache = true;     // SceneRenderer::set_local_shadow_cache
    int32_t most_shadowed_lights = 256; // SceneRenderer::set_most_shadowed_lights
    uint32_t fps_cap = 0;       // no more frames a second than this (--fps-cap; the editor's Preferences); 0: none
    asset::AssetHandle terrain; // the ground, a .deterrain the application keeps loaded; none: no ground
    asset::AssetHandle terrain_layers;    // how it looks, a .delayers the application keeps loaded; none: a plain look
    render::SceneWater water;             // the world's water: none at first
    bool fog_low_camera = true;           // SceneRenderer::set_fog_low_camera
    bool path_traced = false;             // the lit picture path traced, not rasterized: SceneRenderer::set_path_traced
    bool lighting_compare = false;        // the rasterized light against the path tracer's: set_lighting_compare
    int32_t path_tracer_bounces = 16;     // the most a path takes (0: direct light only): PathTracer::set_max_bounces
    int32_t path_tracer_bounces_set = -1; // what was last given it
    bool light_boxes = true;              // the light boxes read where they hold a point: set_light_boxes_on
    bool ambient_occlusion = true;        // on the screen (GTAO): SceneRenderer::set_ambient_occlusion
    bool radiance_cascades = false;       // bounced light from them: SceneRenderer::set_radiance_cascades
    // Their tuning (render::RadianceCascadesSettings has what each is).
    int32_t rc_scale = 0;
    int32_t rc_azimuths = 4;
    float rc_interval_px = 2.0f;
    int32_t rc_max_step_px = 16;
    int32_t rc_march_steps = 32;
    int32_t rc_cascades = 10;
    float rc_thickness_m = 100.0f;
    float rc_tolerance = 1.5f;
    float rc_sky = 1.0f;
    float rc_bounce = 1.0f;
    bool rc_bounces = true; // light bounces more than once (RadianceCascadesSettings::bounces)
    bool rc_white_sky = false;
    bool rc_material_occlusion = true;
    bool specular_ambient = true; // surfaces reflect the ambient: SceneRenderer::set_specular_ambient
    bool auto_exposure = true;    // following the scene: SceneRenderer::set_auto_exposure
#if DE_BUILD_DEV
    bool freeze_cull = false;  // a debug view of the cull: SceneRenderer::set_cull_frozen
    bool water_bands = false;  // the shore waves' bands in color: SceneWater::show_bands
    bool surf_film = true;     // the surf from its film: SceneWater::surf_film
    bool hide_terrain = false; // the ground left out of the picture: SceneRenderer::set_terrain_hidden
    // Physics' colliders drawn over the scene within kColliderReachM of the camera (the editor's toolbar,
    // physics.show_colliders in the console), as lines along their triangles, colored by kind.
    bool show_colliders = false;
    static constexpr float kColliderReachM = 60.0f;
    // Only some: these entities' colliders (each one's own, or its nearest parent's), and the ground's near the camera
    // if collider_ground; the application sets them each frame (the editor's selection). show_colliders shows all.
    bool show_selected_colliders = false;
    std::span<const flecs::entity_t> collider_entities{};
    bool collider_ground = false;
    // Where the application's camera goes this frame (the agent's eyes), or null to leave it alone.
    const tools::CameraPose* camera_pose = nullptr;
    SceneViewport viewport{}; // the scene in a panel (the editor), or over the window
    bool frame_stats = true;  // the frame stats panel shows (F1); off in pictures, and at first in the editor
    bool scripted = false;    // a picture or a path taken by script (the agent's eyes): no input, and nothing to keep
    // The console's window and its pieces, for panels of an application's own (the editor's Output and command line).
    tools::ConsoleUi* console_ui = nullptr;
    // The frame stats, for an application that draws them in a panel of its own (the editor's Playground).
    tools::StatsPanel* stats_panel = nullptr;
    // The frame debugger (tools/frame_debugger.h): the frame's passes, their GPU time and work, their textures. The
    // framework draws its window while it's open (F8, the console's frame_debugger, --frame-debugger); an
    // application opens and closes it from its own menus.
    tools::FrameDebugger* frame_debugger = nullptr;
    // The editor's selection outline and picks (SceneRenderer::set_editor_overlay): off unless it asks. A pick asked
    // for is sent once; its answer comes back frames later in pick_answer, whose count grows by one each time.
    render::EditorOverlay editor_overlay{};
    render::PickAnswer pick_answer{};
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
    // The window was asked to close (its close button, Alt+F4): true lets the run end, false keeps it going (the
    // editor asks about unsaved edits first, then ends the run from update()).
    virtual bool may_close(Engine& /*engine*/) { return true; }
#if DE_BUILD_DEV
    // Dev builds, every frame that builds a debug UI, after update() and the world's advance: the application's
    // ImGui windows (the editor's panels). Engine::viewport puts the scene in one of them.
    virtual void ui(Engine& /*engine*/) {}
#endif
    // Every frame that draws, once the frame has begun and its uploads are recorded, before the frame's own passes:
    // what the application draws into textures of its own, each through a graph of its own (app/preview_scene.h), for
    // this frame's UI or scene to show.
    virtual void draw(Engine& /*engine*/) {}
    // After the last frame (or a failed start: whenever startup() ran), while the engine is still
    // up: let go of what it holds.
    virtual void shutdown(Engine& /*engine*/) {}
};

#if DE_BUILD_DEV
// What the game's picture says about itself in its corner (tools::draw_viewport_notice), written into text, or null
// when it has nothing to say: shaders still compiling for the first time (the picture lacks what they draw until
// they're in), else the frozen cull.
const char* viewport_notice(const Engine& engine, std::span<char> text);
#endif

// Brings the engine up, runs the application in the frame loop until the window closes, the
// application ends the run or --frames are done, and takes it down. The process's exit code: failure if anything
// couldn't start, or validation reported errors.
int run_framework(Application& application, const FrameworkArgs& args);

} // namespace de::app
