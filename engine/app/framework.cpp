#include "app/framework.h"

#include "asset/registry.h"
#include "core/array.h"
#include "core/job_system.h"
#include "core/log.h"
#include "core/memory_stats.h"
#include "core/profile.h"
#include "core/string_id.h"
#include "core/version.h"
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
#include "render/shader/shader_compiler.h"
#include "render/shader/shader_library.h"
#include "tools/debug_ui.h"
#include "tools/stats_panel.h"
#include "world/world.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>

namespace de::app {

using namespace core::literals;

namespace {

// A text file read whole; empty if it isn't there.
core::Array<char> read_text(const platform::Vfs& vfs, const char* path) {
    core::Array<char> text(core::heap_allocator(core::MemoryTag::Platform));
    const core::Result<uint64_t> size = vfs.file_size(path);
    if (!size) {
        return text;
    }
    text.resize(static_cast<size_t>(*size));
    if (!vfs.read_file(path, std::as_writable_bytes(text.span())).ok()) {
        text.clear();
    }
    return text;
}

// Input bindings are text (docs/design.md, Input): the engine's (from the source tree in dev
// builds, so edits apply on the next run without a cook), then the player's, which add to them.
bool load_bindings(const platform::Vfs& vfs, platform::Input& input, bool has_src) {
    const char* const engine_path = has_src ? "src:/config/input.bindings" : "game:/config/input.bindings";
    const core::Array<char> engine_bindings = read_text(vfs, engine_path);
    if (engine_bindings.empty()) {
        DE_LOG_ERROR("input", "no bindings at %s (run the cooker)", engine_path);
        return false;
    }
    uint32_t skipped = 0;
    if (!input.bind_text({engine_bindings.data(), engine_bindings.size()}, &skipped)) {
        return false; // logged, with the line
    }
    const core::Array<char> player = read_text(vfs, "user:/input.bindings");
    if (!player.empty() && !input.bind_text({player.data(), player.size()})) {
        DE_LOG_WARN("input", "user:/input.bindings stopped at an error (logged)");
    }
    DE_LOG_INFO("input", "bindings from %s%s (%u for actions this build doesn't have)", engine_path,
                player.empty() ? "" : " and user:/input.bindings", skipped);
    return true;
}

} // namespace

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
    if (const char* deadzone = flag_value(out, "--deadzone")) {
        out.deadzone = std::strtof(deadzone, nullptr);
        if (out.deadzone < 0.0f || out.deadzone > platform::Input::kMaxStickDeadzone) {
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
    auto window = platform::Window::create({.title = "DEengine"});
    if (!window) {
        return EXIT_FAILURE;
    }
    auto device = rhi::Device::create({.window = &*window, .validation = args.validation, .vsync = args.vsync});
    if (!device) {
        DE_LOG_ERROR("engine", "no usable GPU: %s", device.error().message);
        return EXIT_FAILURE;
    }
    core::Result<shader::ShaderCompiler> compiler =
        core::fail(core::ErrorCode::Unsupported, "no src:/ to compile shaders from");
    if (src_root != nullptr) {
        compiler = shader::ShaderCompiler::create(*vfs);
    }
    core::Result<shader::ShaderLibrary> shaders = compiler
                                                      ? shader::ShaderLibrary::create(*device, *vfs, jobs, *compiler)
                                                      : shader::ShaderLibrary::create_cooked(*device, *vfs, jobs);
    if (!compiler && src_root != nullptr) {
        // Sources are there but can't be compiled: no hot reload, and cooked shaders may be stale.
        DE_LOG_WARN("engine", "the shader compiler didn't start (%s): using cooked shaders from game:/",
                    compiler.error().message);
    } else if (!compiler) {
        DE_LOG_INFO("engine", "shaders from cooked files in game:/ (%s)", compiler.error().message);
    }
    auto uploader = rhi::Uploader::create(*device, {.staging_bytes = uint64_t{8} << 20U});
    auto graph = render::RenderGraph::create(*device);
    // Cooked assets by ID; made before the world, which releases what its cells hold through it,
    // and before GPU copies of what loads (they mirror it from then on).
    auto assets = asset::AssetRegistry::create({.vfs = &*vfs, .jobs = &jobs});
    if (!uploader || !graph || !assets) {
        DE_LOG_ERROR("engine", "the uploader, render graph or asset registry didn't start");
        return EXIT_FAILURE;
    }
    auto world = world::World::create({.jobs = &jobs, .assets = &*assets});
    auto gpu_assets = render::GpuAssets::create({.device = &*device, .uploader = &*uploader, .assets = &*assets});
    if (!world || !gpu_assets) {
        DE_LOG_ERROR("engine", "the world or GPU assets didn't start");
        return EXIT_FAILURE;
    }
    std::optional<render::SceneRenderer> scene_renderer;
    if (shaders) {
        if (auto created = render::SceneRenderer::create(
                {.device = &*device, .shaders = &*shaders, .gpu_assets = &*gpu_assets, .shadows = {}})) {
            scene_renderer.emplace(std::move(*created));
        }
    }

    platform::Input input;
    if (args.deadzone >= 0.0f) {
        input.set_stick_deadzone(args.deadzone);
    }
    const platform::ActionId quit = input.add_action("quit"_sid, platform::ActionKind::Button);
    Engine engine{.jobs = jobs,
                  .vfs = *vfs,
                  .window = *window,
                  .input = input,
                  .device = *device,
                  .uploader = *uploader,
                  .graph = *graph,
                  .shaders = shaders ? &*shaders : nullptr,
                  .assets = *assets,
                  .world = *world,
                  .gpu_assets = *gpu_assets,
                  .lighting = {},
                  .view = render::SceneView::Lit};

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
        return EXIT_FAILURE;
    }
    DE_LOG_INFO("input", "stick deadzone %.2f", static_cast<double>(input.stick_deadzone()));

    // --- The frame loop ---------------------------------------------------------------------------
    uint64_t frame = 0;
    uint64_t previous_frame_ns = platform::time_ns();
    uint64_t title_start_ns = previous_frame_ns;
    uint32_t title_frames = 0;
    bool was_minimized = false;
    while (!window->close_requested()) {
        const uint64_t frame_start_allocations = core::heap_allocation_count(); // every thread, jobs included
        const uint64_t frame_start_ns = platform::time_ns();
        const double frame_s = static_cast<double>(frame_start_ns - previous_frame_ns) * 1e-9;
        previous_frame_ns = frame_start_ns;

        platform::pump_events(*window, input);
        input.sample();
        if (input.state(quit).pressed) {
            break;
        }
        if (window->minimized() != was_minimized) {
            was_minimized = window->minimized();
            DE_LOG_INFO("platform", was_minimized ? "minimized" : "restored");
        }
        if (window->resized()) {
            DE_LOG_INFO("platform", "drawable size %ux%u", window->pixel_width(), window->pixel_height());
        }
        if (shaders) {
            shaders->update(); // hot reload: picks up edits to shaders/*.slang
        }
        application.update(engine, static_cast<float>(frame_s));
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
        // The debug UI's frame; its textures go to the uploader before record().
        if (debug_ui.begin_frame(*window, static_cast<float>(frame_s))) {
            if (show_stats) {
                stats_panel.draw(&show_stats);
            }
            debug_ui.end_frame();
        }
#endif

        device->begin_frame();
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
                                           engine.lighting, engine.view);
            } else {
                // No shaders: the window is cleared to the scene's empty colour.
                graph->add_pass(
                    "clear", [&](render::PassBuilder& pass) { pass.use(back, rhi::ResourceState::RenderTarget); },
                    [back](render::PassContext& context) {
                        const rhi::ColorAttachment color{.texture = context.texture(back),
                                                         .clear = {.r = 0.10f, .g = 0.11f, .b = 0.15f, .a = 1.0f}};
                        context.list.begin_rendering({.colors = std::span(&color, 1)});
                        context.list.end_rendering();
                    });
            }
#if DE_BUILD_DEV
            debug_ui.add_pass(*graph, back, device->swapchain_format());
#endif
            graph->execute();
        }
        device->end_frame();
        if (!backbuffer.is_valid()) {
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
        stats_panel.record({.frame_ms = static_cast<float>(frame_s * 1000.0),
                            .heap_allocations = allocations,
                            .device = device->stats(),
                            .memory = device->memory_stats(),
                            .graph = graph->stats(),
                            .uploads = uploader->stats()});
#endif
        DE_PROFILE_FRAME();
        ++frame;
        if (args.frames != 0 && frame >= args.frames) {
            break;
        }
    }

    // --- Down -------------------------------------------------------------------------------------
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
        "gpu assets: %u meshes, %u textures, %u materials, %llu bytes; last frame drew %u of %u instances in %u draws",
        gpu_stats.meshes, gpu_stats.textures, gpu_stats.materials, static_cast<unsigned long long>(gpu_stats.bytes),
        scene_stats.drawn, scene_stats.instances, scene_stats.draws);
    const rhi::DeviceStats stats = device->stats();
    DE_LOG_INFO("engine", "shutting down after %llu frames, %llu world ticks (%u validation errors, %u warnings)",
                static_cast<unsigned long long>(frame), static_cast<unsigned long long>(world->tick_count()),
                stats.validation_errors, stats.validation_warnings);
    return stats.validation_errors == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace de::app
