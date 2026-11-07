#include "app/framework.h"

#include "app/frame_clock.h"
#include "asset/registry.h"
#include "asset/terrain_format.h"
#include "core/array.h"
#include "core/fixed_string.h"
#include "core/job_system.h"
#include "core/log.h"
#include "core/memory_stats.h"
#include "core/profile.h"
#include "core/string_id.h"
#include "core/text.h"
#include "core/version.h"
#include "physics/physics.h"
#include "platform/events.h"
#include "platform/input.h"
#include "platform/platform.h"
#include "platform/thread.h"
#include "platform/time.h"
#include "platform/vfs.h"
#include "platform/window.h"
#include "render/graph/render_graph.h"
#include "render/rhi/device.h"
#include "render/rhi/upload.h"
#include "render/scene/gpu_assets.h"
#include "render/shader/shader_asset.h"
#include "render/shader/shader_compiler.h"
#include "render/shader/shader_library.h"
#include "tools/debug_ui.h"
#include "tools/stats_panel.h"
#include "world/render_extract.h"
#include "world/transform.h"
#include "world/world.h"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

namespace de::app {

using namespace core::literals;

namespace {

std::string_view as_text(const core::Array<std::byte>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// config/ is read from the source tree in dev builds (edits apply on the next run, no cook), and
// from game:/config/ (the cooker copies it there) in ship.
using ConfigPath = core::FixedString<64>;
ConfigPath config_path(bool has_src, const char* name) {
    ConfigPath path(has_src ? "src:/config/" : "game:/config/");
    static_cast<void>(path.append(name));
    return path;
}

// Input bindings are text (docs/design.md, Input): the engine's (from the source tree in dev
// builds, so edits apply on the next run without a cook), then the player's, whose lines replace
// the engine's for the actions they name. In dev builds every action is added, so a line naming
// none is a typo, and logged.
bool load_bindings(const platform::Vfs& vfs, platform::Input& input, bool has_src) {
    const ConfigPath path = config_path(has_src, "input.bindings");
    const char* const engine_path = path.c_str();
    const auto engine_bindings = vfs.read_whole_file(engine_path, core::heap_allocator(core::MemoryTag::Platform));
    if (!engine_bindings) {
        DE_LOG_ERROR("input", "%s: %s%s", engine_path, engine_bindings.error().message,
                     vfs.exists(engine_path) ? "" : " (run the cooker)");
        return false;
    }
    const core::Result<uint32_t> skipped =
        input.bind_text(as_text(*engine_bindings), {.replace = false, .warn_unknown = DE_BUILD_DEV != 0});
    if (!skipped) {
        return false; // logged, with the line
    }
    constexpr const char* kPlayerPath = "user:/input.bindings";
    const bool has_player = vfs.exists(kPlayerPath);
    if (has_player) {
        const auto player = vfs.read_whole_file(kPlayerPath, core::heap_allocator(core::MemoryTag::Platform));
        if (!player) {
            DE_LOG_WARN("input", "%s: %s; the defaults stand", kPlayerPath, player.error().message);
        } else if (!input.bind_text(as_text(*player), {.replace = true, .warn_unknown = true})) {
            DE_LOG_WARN("input", "%s has an error (logged); the defaults stand", kPlayerPath);
        }
    }
    DE_LOG_INFO("input", "bindings from %s%s (%u for actions this build doesn't have)", engine_path,
                has_player ? " and user:/input.bindings" : "", *skipped);
    return true;
}

} // namespace

std::string_view setting(std::string_view settings, std::string_view name) {
    std::string_view rest = core::skip_bom(settings);
    while (!rest.empty()) {
        const std::string_view line = core::strip_comment(core::next_line(rest));
        size_t split = 0;
        while (split < line.size() && !core::is_blank(line[split])) {
            ++split;
        }
        if (line.substr(0, split) == name) {
            return core::trim(line.substr(split));
        }
    }
    return {};
}

bool parse_args(int argc, char** argv, FrameworkArgs& out) {
    out.argc = argc;
    out.argv = argv;
    if (const char* frames = flag_value(out, "--frames")) {
        out.frames = std::strtoull(frames, nullptr, 10);
    }
    if (const char* game_dir = flag_value(out, "--game-dir")) {
        out.game_dir = game_dir;
    }
    out.src_dir = flag_value(out, "--src-dir");
    out.vsync = !has_flag(out, "--no-vsync");
    out.validation = DE_BUILD_DEV != 0 && !has_flag(out, "--no-validation");
    out.show_sticks = DE_BUILD_DEV != 0 && has_flag(out, "--show-sticks");
    out.fullscreen = has_flag(out, "--fullscreen");
    if (has_flag(out, "--window")) {
        const char* const window = flag_value(out, "--window");
        const std::string_view size = window != nullptr ? std::string_view(window) : std::string_view();
        const size_t split = size.find('x');
        uint32_t width = 0;
        uint32_t height = 0;
        // from_chars: digits only, and it takes neither signs nor spaces.
        const auto whole = [](std::string_view text, uint32_t& value) {
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            return error == std::errc{} && end == text.data() + text.size();
        };
        if (split == std::string_view::npos || !whole(size.substr(0, split), width) ||
            !whole(size.substr(split + 1), height) || width == 0 || height == 0 || width > 16384 || height > 16384) {
            DE_LOG_ERROR("engine", "--window needs a size like 2560x1440: '%.*s'", static_cast<int>(size.size()),
                         size.data());
            return false;
        }
        out.window_width_points = width;
        out.window_height_points = height;
    }
    if (const char* lod_error = flag_value(out, "--lod-error-pixels")) {
        char* end = nullptr;
        out.lod_error_pixels = std::strtof(lod_error, &end);
        if (end == lod_error || *end != '\0' || !(out.lod_error_pixels >= 0.0f)) {
            DE_LOG_ERROR("render", "--lod-error-pixels must be a number, 0 or more");
            return false;
        }
    }
    if (const char* lod_fade = flag_value(out, "--lod-fade")) {
        char* end = nullptr;
        out.lod_fade = std::strtof(lod_fade, &end);
        // The range test is negated, so NaN fails it too.
        if (end == lod_fade || *end != '\0' || !(out.lod_fade >= 0.0f && out.lod_fade < 1.0f)) {
            DE_LOG_ERROR("render", "--lod-fade must be a number from 0 to below 1");
            return false;
        }
    }
    out.shadow_caster_cull = !has_flag(out, "--no-shadow-caster-cull");
    out.merged_draws = !has_flag(out, "--no-merged-draws");
    if (const char* min_texels = flag_value(out, "--shadow-min-texels")) {
        char* end = nullptr;
        out.shadow_min_texels = std::strtof(min_texels, &end);
        if (end == min_texels || *end != '\0' || !(out.shadow_min_texels >= 0.0f)) {
            DE_LOG_ERROR("render", "--shadow-min-texels must be a number, 0 or more");
            return false;
        }
    }
    if (const char* deadzone = flag_value(out, "--deadzone")) {
        char* end = nullptr;
        out.deadzone = std::strtof(deadzone, &end);
        // The range test is negated, so NaN fails it too.
        if (end == deadzone || *end != '\0' ||
            !(out.deadzone >= 0.0f && out.deadzone <= platform::Input::kMaxStickDeadzone)) {
            DE_LOG_ERROR("input", "--deadzone must be between 0 and %.1f",
                         static_cast<double>(platform::Input::kMaxStickDeadzone));
            return false;
        }
    }
    return true;
}

const char* flag_value(const FrameworkArgs& args, const char* flag) {
    for (int i = 1; i + 1 < args.argc; ++i) {
        if (std::strcmp(args.argv[i], flag) == 0) {
            return args.argv[i + 1];
        }
    }
    return nullptr;
}

bool has_flag(const FrameworkArgs& args, const char* flag) {
    for (int i = 1; i < args.argc; ++i) {
        if (std::strcmp(args.argv[i], flag) == 0) {
            return true;
        }
    }
    return false;
}

int run_framework(Application& application, const FrameworkArgs& args) {
    DE_LOG_INFO("engine", "DEengine %s", core::engine_version_string());

    // --- The engine, bottom up (each made after what it uses, so it goes before it) -------------
    auto platform = platform::Platform::create();
    if (!platform) {
        return EXIT_FAILURE;
    }
    platform::set_current_thread_name("main");
    DE_PROFILE_THREAD_NAME("main");
    // Workers get OS names too, so debuggers and crash dumps show "worker 3" like the profiler does.
    core::JobSystem jobs(core::JobSystemDesc{
        .on_worker_start = [](uint32_t /*index*/, const char* name) { platform::set_current_thread_name(name); }});
    DE_LOG_INFO("engine", "job system: %u workers", jobs.worker_count());

    // game:/ is the cooked data; src:/ the source tree, in dev builds only (shaders compile from it
    // and hot reload; without it, they load from the cooker's .deshader files).
    const char* src_root = DE_BUILD_DEV != 0 ? (args.src_dir != nullptr ? args.src_dir : DE_SOURCE_DIR) : nullptr;
    auto vfs = platform::Vfs::create({.game_root = args.game_dir, .src_root = src_root, .jobs = &jobs});
    if (!vfs && src_root != nullptr) {
        // A dev build away from its source tree: run without src:/ rather than not at all.
        DE_LOG_WARN("engine", "source tree %s not found (pass --src-dir): running without src:/", src_root);
        src_root = nullptr;
        vfs = platform::Vfs::create({.game_root = args.game_dir, .jobs = &jobs});
    }
    if (!vfs) {
        return EXIT_FAILURE;
    }
    core::Result<shader::ShaderCompiler> compiler =
        core::fail(core::ErrorCode::Unsupported, "no src:/ to compile shaders from");
    if (src_root != nullptr) {
        compiler = shader::ShaderCompiler::create(*vfs);
    }
    if (!compiler && src_root != nullptr) {
        // Sources are there but can't be compiled: no hot reload.
        DE_LOG_WARN("engine", "the shader compiler didn't start (%s): using cooked shaders from game:/",
                    compiler.error().message);
    } else if (!compiler) {
        DE_LOG_INFO("engine", "shaders from cooked files in game:/ (%s)", compiler.error().message);
    }
    // Stale cooked shaders would crash the GPU: refused before there's a window or a device. The
    // cooker can't help if this is the wrong folder, so the message says which one it is.
    if (const core::Result<void> fresh =
            !compiler ? shader::check_cooked_shaders(*vfs, shader::built_source_hash()) : core::Result<void>();
        !fresh) {
        const core::Result<platform::PathString> folder = platform::working_directory();
        DE_LOG_ERROR("engine",
                     "can't start: %s. game:/ is '%s' (working directory '%s'): pass --game-dir if the cooked "
                     "data is elsewhere",
                     fresh.error().message, vfs->mount_root(platform::Mount::Game),
                     folder ? folder->c_str() : "unknown");
        return EXIT_FAILURE;
    }
    auto window = platform::Window::create({.title = "DEengine",
                                            .width_points = args.window_width_points,
                                            .height_points = args.window_height_points,
                                            .fullscreen = args.fullscreen});
    if (!window) {
        return EXIT_FAILURE;
    }
    auto device = rhi::Device::create({.window = &*window, .validation = args.validation, .vsync = args.vsync});
    if (!device) {
        DE_LOG_ERROR("engine", "no usable GPU: %s", device.error().message);
        return EXIT_FAILURE;
    }
    core::Result<shader::ShaderLibrary> shaders = compiler
                                                      ? shader::ShaderLibrary::create(*device, *vfs, jobs, *compiler)
                                                      : shader::ShaderLibrary::create_cooked(*device, *vfs, jobs);
    if (!shaders) {
        DE_LOG_ERROR("engine", "can't start: %s", shaders.error().message); // a cook started just now, say
        return EXIT_FAILURE;
    }
    auto uploader = rhi::Uploader::create(*device, {.staging_bytes = uint64_t{8} << 20U});
    if (!uploader) {
        DE_LOG_ERROR("render", "uploader: %s", uploader.error().message);
        return EXIT_FAILURE;
    }
    auto graph = render::RenderGraph::create(*device);
    if (!graph) {
        DE_LOG_ERROR("render", "render graph: %s", graph.error().message);
        return EXIT_FAILURE;
    }
    // Cooked assets by ID; made before the world, which releases what its cells hold through it,
    // and before GPU copies of what loads (they mirror it from then on).
    auto assets = asset::AssetRegistry::create({.vfs = &*vfs, .jobs = &jobs});
    if (!assets) {
        DE_LOG_ERROR("asset", "%s", assets.error().message);
        return EXIT_FAILURE;
    }
    // Before the world: the world's last entities let go of their bodies as it goes down.
    physics::Physics physics({.jobs = &jobs});
    auto world = world::World::create({.jobs = &jobs, .assets = &*assets});
    if (!world) {
        DE_LOG_ERROR("world", "%s", world.error().message);
        return EXIT_FAILURE;
    }
    physics.add_to_world(*world);
    auto gpu_assets = render::GpuAssets::create({.device = &*device, .uploader = &*uploader, .assets = &*assets});
    if (!gpu_assets) {
        DE_LOG_ERROR("render", "gpu assets: %s", gpu_assets.error().message);
        return EXIT_FAILURE;
    }
    std::optional<render::SceneRenderer> scene_renderer;
    auto created = render::SceneRenderer::create(
        {.device = &*device, .shaders = &*shaders, .gpu_assets = &*gpu_assets, .uploader = &*uploader, .vfs = &*vfs});
    if (created) {
        scene_renderer.emplace(std::move(*created));
        scene_renderer->set_lod_error_pixels(args.lod_error_pixels);
        scene_renderer->set_lod_fade(args.lod_fade);
        scene_renderer->set_shadow_caster_cull(args.shadow_caster_cull);
        scene_renderer->set_merged_draws(args.merged_draws);
        scene_renderer->set_shadow_min_texels(args.shadow_min_texels);
    } else {
        DE_LOG_ERROR("render", "scene renderer: %s (the window is only cleared)", created.error().message);
    }

    // Engine settings (text, like the bindings): read once, kept for setting().
    // Missing, it's empty: whoever needs a setting says so.
    const ConfigPath settings_path = config_path(src_root != nullptr, "engine.settings");
    core::Array<std::byte> settings(core::heap_allocator(core::MemoryTag::Platform));
    if (vfs->exists(settings_path.c_str())) {
        auto read = vfs->read_whole_file(settings_path.c_str(), core::heap_allocator(core::MemoryTag::Platform));
        if (!read) {
            DE_LOG_ERROR("engine", "%s: %s", settings_path.c_str(), read.error().message);
            return EXIT_FAILURE;
        }
        settings = std::move(*read);
    }

    platform::Input input;
    if (args.deadzone >= 0.0f) {
        input.set_stick_deadzone(args.deadzone);
    }
    Engine engine{.jobs = jobs,
                  .vfs = *vfs,
                  .window = *window,
                  .input = input,
                  .device = *device,
                  .uploader = *uploader,
                  .graph = *graph,
                  .shaders = *shaders,
                  .assets = *assets,
                  .world = *world,
                  .physics = physics,
                  .gpu_assets = *gpu_assets,
                  .settings = as_text(settings),
                  .lighting = {},
                  .view = render::SceneView::Lit,
                  .taa = setting(as_text(settings), "taa") == "on",
                  .terrain = {}};
    asset::AssetHandle shown_terrain;
    uint32_t shown_terrain_revision = 0;

    // --- Dev tools: the debug UI (F1: frame stats) and the raw stick readout ---------------------
#if DE_BUILD_DEV
    tools::DebugUi debug_ui = tools::DebugUi::create(*window, *device, engine.shaders, *uploader);
    tools::StatsPanel stats_panel;
    bool show_stats = true;
    const platform::ActionId toggle_stats = input.add_action("debug.toggle_stats"_sid, platform::ActionKind::Button);
    // The raw stick readout (--show-sticks), for measuring drift: the deadzone is off, so these show
    // the true resting values.
    const platform::ActionId sticks[4] = {input.add_action("debug.stick_left_x"_sid, platform::ActionKind::Axis),
                                          input.add_action("debug.stick_left_y"_sid, platform::ActionKind::Axis),
                                          input.add_action("debug.stick_right_x"_sid, platform::ActionKind::Axis),
                                          input.add_action("debug.stick_right_y"_sid, platform::ActionKind::Axis)};
    if (args.show_sticks) {
        input.set_stick_deadzone(0.0f);
        DE_LOG_INFO("input", "showing raw stick values; leave the sticks alone to measure drift");
    }
    uint64_t next_stick_report_ns = 0;
#endif

    if (!application.startup(engine, args) || !load_bindings(*vfs, input, src_root != nullptr)) {
        application.shutdown(engine); // what startup got before it failed
        return EXIT_FAILURE;
    }
    DE_LOG_INFO("input", "stick deadzone %.2f", static_cast<double>(input.stick_deadzone()));

    // --- The frame loop ---------------------------------------------------------------------------
    uint64_t frame = 0;
    uint64_t previous_frame_ns = platform::time_ns();
    FrameClock frame_clock;
    uint64_t title_start_ns = previous_frame_ns;
    uint32_t title_frames = 0;
    bool was_minimized = false;
    while (!window->close_requested()) {
        const uint64_t frame_start_allocations = core::heap_allocation_count(); // every thread, jobs included
        const uint64_t frame_start_ns = platform::time_ns();
        const double measured_s = static_cast<double>(frame_start_ns - previous_frame_ns) * 1e-9;
        // What the camera and the world move by.
        // Minimized, nothing is presented: there are no refreshes to count.
        const double frame_s =
            frame_clock.step(measured_s, device->vsync() && !window->minimized() ? window->refresh_hz() : 0.0);
        DE_PROFILE_PLOT("frame clock on the refresh rate", static_cast<int64_t>(frame_clock.holds_rate() ? 1 : 0));
        previous_frame_ns = frame_start_ns;

        platform::pump_events(*window, input);
        input.sample();
        if (window->minimized() != was_minimized) {
            was_minimized = window->minimized();
            DE_LOG_INFO("platform", was_minimized ? "minimized" : "restored");
        }
        if (window->resized()) {
            DE_LOG_INFO("platform", "drawable size %ux%u", window->pixel_width(), window->pixel_height());
        }
        shaders->update(); // hot reload: picks up edits to shaders/*.slang
        if (!application.update(engine, static_cast<float>(frame_s))) {
            break; // the application is done (quit)
        }
        assets->update();        // commits finished reads, unloads what nobody holds, reloads re-cooked files
        gpu_assets->update();    // meshes whose uploads are in become drawable
        world->advance(frame_s); // fixed ticks, then RenderExtract

#if DE_BUILD_DEV
        if (input.state(toggle_stats).pressed) {
            show_stats = !show_stats;
        }
        if (args.show_sticks && platform::time_ns() >= next_stick_report_ns) {
            next_stick_report_ns = platform::time_ns() + platform::kNsPerSecond / 2;
            DE_LOG_INFO(
                "input", "left %+.3f %+.3f   right %+.3f %+.3f", static_cast<double>(input.state(sticks[0]).value),
                static_cast<double>(input.state(sticks[1]).value), static_cast<double>(input.state(sticks[2]).value),
                static_cast<double>(input.state(sticks[3]).value));
        }
        // Whether or not this frame draws: a minimized frame's toggle counts too, as it does for the application.
        if (scene_renderer) {
            scene_renderer->set_cull_frozen(engine.freeze_cull);
        }
        DE_PROFILE_PLOT("cull frozen", static_cast<int64_t>(engine.freeze_cull ? 1 : 0));
        // The debug UI's frame; its textures go to the uploader before record().
        if (debug_ui.begin_frame(*window, static_cast<float>(measured_s))) {
            if (show_stats) {
                stats_panel.draw(&show_stats);
            }
            debug_ui.end_frame();
        }
#endif

        device->begin_frame();
        // What the application asks of the renderer, in every build.
        if (scene_renderer) {
            scene_renderer->set_fog_low_camera(engine.fog_low_camera);
            // The ground: when the application names another, or it's re-cooked.
            const uint32_t terrain_revision = engine.assets.revision(engine.terrain);
            if (engine.terrain != shown_terrain || terrain_revision != shown_terrain_revision) {
                shown_terrain = engine.terrain;
                shown_terrain_revision = terrain_revision;
                const auto* terrain = engine.assets.get<asset::TerrainAsset>(engine.terrain);
                scene_renderer->set_terrain(terrain, terrain != nullptr ? engine.assets.file(engine.terrain)
                                                                        : std::span<const std::byte>{});
            }
        }
        uploader->record(); // uploads land at the start of the frame
        const rhi::TextureHandle backbuffer = device->acquire_swapchain_texture();
        if (backbuffer.is_valid()) {
            graph->begin(device->swapchain_width(), device->swapchain_height());
            const render::GraphTexture back = graph->import_texture({.texture = backbuffer,
                                                                     .name = "backbuffer",
                                                                     .state = rhi::ResourceState::Present,
                                                                     .final_state = rhi::ResourceState::Present,
                                                                     .discard = true});
            if (scene_renderer) {
                scene_renderer->add_passes(*graph, back, device->swapchain_format(), world->render_frame(),
                                           engine.lighting, engine.view, engine.taa);
                if (scene_renderer->instances_stale()) {
                    world->resync_render_instances();
                }
            } else {
                // No scene renderer: the window is cleared.
                graph->add_pass(
                    "clear", [&](render::PassBuilder& pass) { pass.use(back, rhi::ResourceState::RenderTarget); },
                    [back](render::PassContext& context) {
                        const rhi::ColorAttachment color{.texture = context.texture(back),
                                                         .clear = render::kEmptySceneColor};
                        context.list.begin_rendering({.colors = std::span(&color, 1)});
                        context.list.end_rendering();
                    });
            }
#if DE_BUILD_DEV
            debug_ui.add_pass(*graph, back, device->swapchain_format());
#endif
            graph->execute();
            if (scene_renderer) { // a frame that drew: minimized, these would repeat the last one's
                const render::SceneRendererStats drawn = scene_renderer->stats();
                DE_PROFILE_PLOT("draws to cull", static_cast<int64_t>(drawn.draws));
                DE_PROFILE_PLOT("shadow draws to cull", static_cast<int64_t>(drawn.shadow_draws));
                DE_PROFILE_PLOT("indirect calls", static_cast<int64_t>(drawn.indirect_calls));
                DE_PROFILE_PLOT("visible draws", static_cast<int64_t>(drawn.visible_draws));
                DE_PROFILE_PLOT("visible fading draws", static_cast<int64_t>(drawn.visible_fading_draws));
                DE_PROFILE_PLOT("visible shadow draws", static_cast<int64_t>(drawn.visible_shadow_draws));
                DE_PROFILE_PLOT("merged draws", static_cast<int64_t>(drawn.merged_draws));
                DE_PROFILE_PLOT("terrain patches", static_cast<int64_t>(drawn.terrain.patches));
                DE_PROFILE_PLOT("terrain triangles", static_cast<int64_t>(drawn.terrain.triangles));
                DE_PROFILE_PLOT("terrain shadow triangles", static_cast<int64_t>(drawn.terrain.shadow_triangles));
                DE_PROFILE_PLOT("terrain blocks resident", static_cast<int64_t>(drawn.terrain.resident));
                DE_PROFILE_PLOT("terrain blocks wanted", static_cast<int64_t>(drawn.terrain.wanted));
                DE_PROFILE_PLOT("merged shadow draws", static_cast<int64_t>(drawn.merged_shadow_draws));
                DE_PROFILE_PLOT("draws lost", static_cast<int64_t>(drawn.draws_lost));
                DE_PROFILE_PLOT("visible triangles", static_cast<int64_t>(drawn.visible_triangles));
                DE_PROFILE_PLOT("visible shadow triangles", static_cast<int64_t>(drawn.visible_shadow_triangles));
                static_cast<void>(drawn);
            }
        }
        device->end_frame();
        if (!backbuffer.is_valid()) {
            world->resync_render_instances();           // what changes while nothing's drawn would be lost
            platform::sleep_ns(5 * platform::kNsPerMs); // minimized: nothing to show, don't spin a core
        }

        // Frame rate in the title, once a second.
        ++title_frames;
        if (const uint64_t now = platform::time_ns(); now - title_start_ns >= platform::kNsPerSecond) {
            const double seconds = static_cast<double>(now - title_start_ns) * 1e-9;
            char title[96];
            std::snprintf(title, sizeof(title), "DEengine  |  %.0f fps  %.2f ms",
                          static_cast<double>(title_frames) / seconds,
                          seconds * 1000.0 / static_cast<double>(title_frames));
            window->set_title(title);
            title_frames = 0;
            title_start_ns = now;
        }
        // The "zero heap allocations per frame" rule, made visible in the profiler and the panel.
        const uint64_t allocations = core::heap_allocation_count() - frame_start_allocations;
        DE_PROFILE_PLOT("heap allocations/frame", static_cast<int64_t>(allocations));
#if DE_BUILD_DEV
        const flecs::world& ecs = engine.world.ecs();
        const auto* active_camera = ecs.try_get<world::ActiveCamera>();
        const auto* camera_place =
            active_camera != nullptr && active_camera->entity != 0 && ecs.is_alive(active_camera->entity)
                ? ecs.entity(active_camera->entity).try_get<world::WorldTransform>()
                : nullptr;
        const render::SceneRendererStats cull_stats =
            scene_renderer ? scene_renderer->stats() : render::SceneRendererStats{};
        stats_panel.record(
            {.frame_ms = static_cast<float>(measured_s * 1000.0),
             .heap_allocations = allocations,
             .device = device->stats(),
             .memory = device->memory_stats(),
             .graph = graph->stats(),
             .uploads = uploader->stats(),
             .gpu_passes = graph->gpu_times(),
             .gpu_frame_ms = graph->gpu_frame_ms(),
             .has_camera = camera_place != nullptr,
             .camera_position = camera_place != nullptr ? camera_place->position : DVec3(0.0),
             .camera_rotation = camera_place != nullptr ? camera_place->rotation : Quat(1.0f, 0.0f, 0.0f, 0.0f),
             .cull_frozen = engine.freeze_cull,
             .visible_shadow_draws = cull_stats.visible_shadow_draws,
             .visible_shadow_triangles = cull_stats.visible_shadow_triangles});
#endif
        DE_PROFILE_FRAME();
        ++frame;
        if (args.frames != 0 && frame >= args.frames) {
            break;
        }
    }

    // --- Down -------------------------------------------------------------------------------------
    application.shutdown(engine);
    device->log_memory_report(5);
    device->wait_idle();
    const asset::AssetRegistryStats asset_stats = assets->stats();
    DE_LOG_INFO("asset", "%u assets held (%u failed), %llu bytes, %u reloads", asset_stats.assets, asset_stats.failed,
                static_cast<unsigned long long>(asset_stats.bytes), asset_stats.reloads);
    const render::GpuAssetsStats gpu_stats = gpu_assets->stats();
    const render::SceneRendererStats scene_stats =
        scene_renderer ? scene_renderer->stats() : render::SceneRendererStats{};
    DE_LOG_INFO(
        "render",
        "gpu assets: %u meshes, %u textures, %u materials, %llu bytes; last frame had %u of %u instances on the GPU, "
        "%u draws for %u parts, %u of them fading between LODs",
        gpu_stats.meshes, gpu_stats.textures, gpu_stats.materials, static_cast<unsigned long long>(gpu_stats.bytes),
        scene_stats.drawn, scene_stats.instances, scene_stats.visible_draws, scene_stats.draws,
        scene_stats.visible_fading_draws);
    if (scene_stats.terrain.blocks > 0) {
        DE_LOG_INFO("terrain",
                    "%u of %u blocks on the GPU, %u wanted, %u failed; %u patches, %u triangles in view, %u in shadows",
                    scene_stats.terrain.resident, scene_stats.terrain.blocks, scene_stats.terrain.wanted,
                    scene_stats.terrain.failed, scene_stats.terrain.patches, scene_stats.terrain.triangles,
                    scene_stats.terrain.shadow_triangles);
    }
    if (gpu_stats.meshes_dropped > 0) {
        DE_LOG_WARN("render", "%u meshes didn't fit in the geometry pool", gpu_stats.meshes_dropped);
    }
    const rhi::DeviceStats stats = device->stats();
    DE_LOG_INFO("engine", "shutting down after %llu frames, %llu world ticks (%u validation errors, %u warnings)",
                static_cast<unsigned long long>(frame), static_cast<unsigned long long>(world->tick_count()),
                stats.validation_errors, stats.validation_warnings);
    return stats.validation_errors == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace de::app
