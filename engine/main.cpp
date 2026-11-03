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
#include "render/scene/scene_renderer.h"
#include "render/shader/shader_compiler.h"
#include "render/shader/shader_library.h"
#include "tools/debug_ui.h"
#include "tools/free_camera.h"
#include "tools/stats_panel.h"
#include "world/cell.h"
#include "world/world.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

using namespace de::core::literals;
using de::platform::ActionId;
using de::platform::ActionKind;
using de::platform::Input;

namespace {

// content/world/test/cell_0_0.cell: the test scene, until there's a world asset listing cells.
constexpr de::asset::AssetId kTestCell = de::asset::AssetId::from_value(0xbf4c53ca6b99a5e7);

// The value after a flag ("--frames 120"), or null if the flag isn't there.
const char* flag_value(int argc, char** argv, const char* flag) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) {
            return argv[i + 1];
        }
    }
    return nullptr;
}

bool has_flag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

// The engine's loop until the game's own lands: opens the window, loads a cell and draws it (renderer
// step 2: deferred, with a sun, a hemisphere ambient and a filmic tonemap) through a free camera (WASD/QE and
// the right mouse button, or a gamepad; F2 steps through G-buffer views), with hot-reloaded shaders
// and assets; pumps events,
// samples input and logs actions.
//   --frames N       exit after N frames (smoke runs; validation errors fail the run)
//   --cell ID        the cooked cell to load (asset ID, 16 hex digits; default: the test cell)
//   --no-vsync       present without waiting for vertical blank (mailbox or immediate)
//   --no-validation  skip the Vulkan validation layer in debug and dev builds
//   --game-dir DIR   folder mounted as game:/ (default ./cooked)
//   --src-dir DIR    folder mounted as src:/ in dev builds (default: this repository)
//   --deadzone X     stick deadzone, 0..0.9 (default 0.15)
//   --show-sticks    deadzone off; log raw stick values twice a second, to measure drift
int main(int argc, char** argv) {
    DE_LOG_INFO("engine", "DEengine %s", de::core::engine_version_string());

    auto platform = de::platform::Platform::create();
    if (!platform) {
        return EXIT_FAILURE;
    }
    de::platform::set_current_thread_name("main");
    DE_PROFILE_THREAD_NAME("main");

    // Workers get OS names too, so debuggers and crash dumps show "worker 3" like the profiler does.
    de::core::JobSystem jobs(de::core::JobSystemDesc{
        .on_worker_start = [](uint32_t /*index*/, const char* name) { de::platform::set_current_thread_name(name); }});
    DE_LOG_INFO("engine", "job system: %u workers", jobs.worker_count());

    // game:/ is the cooked data folder (tools/cooker writes it): ./cooked unless --game-dir says otherwise.
    const char* game_dir = flag_value(argc, argv, "--game-dir");
    // src:/ is the source tree, in dev builds only: shaders compile from it and hot reload. Without
    // it (ship builds), shaders load from the cooker's .deshader files in game:/.
    const char* src_dir = flag_value(argc, argv, "--src-dir");
    const char* src_root = DE_BUILD_DEV != 0 ? (src_dir != nullptr ? src_dir : DE_SOURCE_DIR) : nullptr;
    auto vfs = de::platform::Vfs::create(
        {.game_root = game_dir != nullptr ? game_dir : "cooked", .src_root = src_root, .jobs = &jobs});
    if (!vfs && src_root != nullptr) {
        // A dev build away from its source tree (copied to another machine, the repo moved): run
        // without src:/, and so without shaders, rather than not at all.
        DE_LOG_WARN("engine", "source tree %s not found (pass --src-dir): running without src:/", src_root);
        src_root = nullptr;
        vfs = de::platform::Vfs::create({.game_root = game_dir != nullptr ? game_dir : "cooked", .jobs = &jobs});
    }
    if (!vfs) {
        return EXIT_FAILURE;
    }
    // A round trip through temp:/ (emptied at every start): blocking atomic write, async read back.
    {
        constexpr std::string_view kProbe = "DEengine vfs probe";
        if (!vfs->write_file("temp:/vfs_probe.txt", std::as_bytes(std::span(kProbe.data(), kProbe.size()))).ok()) {
            return EXIT_FAILURE;
        }
        std::byte buffer[64];
        const auto read =
            vfs->read({.path = "temp:/vfs_probe.txt", .dest = buffer, .priority = de::platform::IoPriority::Critical});
        while (read.ok() && (vfs->status(*read) == de::platform::ReadStatus::Pending ||
                             vfs->status(*read) == de::platform::ReadStatus::InFlight)) {
            de::platform::sleep_ns(de::platform::kNsPerMs / 10);
        }
        const auto bytes = read.ok() ? vfs->finish(*read) : de::core::Result<size_t>(de::core::fail(read.error()));
        const bool round_trip = bytes.ok() && std::string_view(reinterpret_cast<const char*>(buffer), *bytes) == kProbe;
        DE_LOG_INFO("vfs", "temp:/ round trip %s", round_trip ? "ok" : "FAILED");
        if (!round_trip) {
            return EXIT_FAILURE;
        }
    }

    auto window = de::platform::Window::create({.title = "DEengine"});
    if (!window) {
        return EXIT_FAILURE;
    }

    auto device = de::rhi::Device::create({
        .window = &*window,
        .validation = DE_BUILD_DEV != 0 && !has_flag(argc, argv, "--no-validation"),
        .vsync = !has_flag(argc, argv, "--no-vsync"),
    });
    if (!device) {
        DE_LOG_ERROR("engine", "no usable GPU: %s", device.error().message);
        return EXIT_FAILURE;
    }

    de::core::Result<de::shader::ShaderCompiler> compiler =
        de::core::fail(de::core::ErrorCode::Unsupported, "no src:/ to compile shaders from");
    if (src_root != nullptr) {
        compiler = de::shader::ShaderCompiler::create(*vfs);
    }
    de::core::Result<de::shader::ShaderLibrary> shaders =
        compiler ? de::shader::ShaderLibrary::create(*device, *vfs, jobs, *compiler)
                 : de::shader::ShaderLibrary::create_cooked(*device, *vfs, jobs);
    if (!compiler && src_root != nullptr) {
        // Sources are there but can't be compiled: no hot reload, and cooked shaders may be stale.
        DE_LOG_WARN("engine", "the shader compiler didn't start (%s): using cooked shaders from game:/",
                    compiler.error().message);
    } else if (!compiler) {
        DE_LOG_INFO("engine", "shaders from cooked files in game:/ (%s)", compiler.error().message);
    }

    Input input;
    const ActionId quit = input.add_action("quit"_sid, ActionKind::Button);
    const ActionId attack = input.add_action("attack_light"_sid, ActionKind::Button);
    const ActionId roll = input.add_action("roll"_sid, ActionKind::Button);
    const ActionId move_x = input.add_action("move_x"_sid, ActionKind::Axis);
    const ActionId move_y = input.add_action("move_y"_sid, ActionKind::Axis);
    // Hard-coded until bindings move to a data file.
    bool bound = true;
    const auto bind = [&](ActionId action, const char* binding, float scale = 1.0f) {
        bound = input.bind(action, binding, scale).ok() && bound;
    };
    bind(quit, "key:Escape");
    bind(quit, "gamepad:back");
    bind(attack, "mouse:left");
    bind(attack, "gamepad:rightshoulder");
    bind(roll, "key:Space");
    bind(roll, "gamepad:b");
    bind(move_x, "key:D");
    bind(move_x, "key:A", -1.0f);
    bind(move_x, "gamepad:leftx");
    bind(move_y, "key:W");
    bind(move_y, "key:S", -1.0f);
    bind(move_y, "gamepad:lefty", -1.0f); // stick up is negative y
    const ActionId toggle_stats = input.add_action("toggle_stats"_sid, ActionKind::Button);
    bind(toggle_stats, "key:F1");
    const ActionId next_view = input.add_action("debug.next_view"_sid, ActionKind::Button); // G-buffer views
    bind(next_view, "key:F2");
    if (!bound) {
        return EXIT_FAILURE;
    }

    // Raw stick readout for measuring drift: the deadzone is off, so these show the true resting values.
    const bool show_sticks = has_flag(argc, argv, "--show-sticks");
    const ActionId sticks[4] = {
        input.add_action("stick_left_x"_sid, ActionKind::Axis),
        input.add_action("stick_left_y"_sid, ActionKind::Axis),
        input.add_action("stick_right_x"_sid, ActionKind::Axis),
        input.add_action("stick_right_y"_sid, ActionKind::Axis),
    };
    bind(sticks[0], "gamepad:leftx");
    bind(sticks[1], "gamepad:lefty");
    bind(sticks[2], "gamepad:rightx");
    bind(sticks[3], "gamepad:righty");
    if (show_sticks) {
        input.set_stick_deadzone(0.0f);
        DE_LOG_INFO("input", "showing raw stick values; leave the sticks alone to measure drift");
    } else if (const char* deadzone = flag_value(argc, argv, "--deadzone")) {
        const float value = std::strtof(deadzone, nullptr);
        if (value < 0.0f || value > Input::kMaxStickDeadzone) {
            DE_LOG_ERROR("input", "--deadzone must be between 0 and %.1f",
                         static_cast<double>(Input::kMaxStickDeadzone));
            return EXIT_FAILURE;
        }
        input.set_stick_deadzone(value);
    }
    DE_LOG_INFO("input", "stick deadzone %.2f", static_cast<double>(input.stick_deadzone()));

    // GPU uploads: queued at any time, recorded at the start of each frame.
    auto uploader = de::rhi::Uploader::create(*device, {.staging_bytes = uint64_t{8} << 20U});
    if (!uploader) {
        DE_LOG_ERROR("render", "uploader: %s", uploader.error().message);
        return EXIT_FAILURE;
    }

    // The frame graph: each frame's passes and the barriers between them.
    auto graph = de::render::RenderGraph::create(*device);
    if (!graph) {
        DE_LOG_ERROR("render", "render graph: %s", graph.error().message);
        return EXIT_FAILURE;
    }

    // Cooked assets by ID: reads, reference counts and, in dev builds, hot reload of files the cooker
    // rewrites. Made before the world, which releases what its cells hold through it.
    auto assets = de::asset::AssetRegistry::create({.vfs = &*vfs, .jobs = &jobs});
    if (!assets) {
        DE_LOG_ERROR("asset", "%s", assets.error().message);
        return EXIT_FAILURE;
    }
    // The game world: flecs, ticking at a fixed 60 Hz.
    auto world = de::world::World::create({.jobs = &jobs, .assets = &*assets});
    if (!world) {
        DE_LOG_ERROR("world", "%s", world.error().message);
        return EXIT_FAILURE;
    }
    // One cell, loaded explicitly until the streamer picks cells around the player (M1). Its members
    // spawn once it and its meshes are loaded.
    de::asset::AssetId cell_id = kTestCell;
    if (const char* cell_flag = flag_value(argc, argv, "--cell");
        cell_flag != nullptr && !de::asset::parse_asset_id(cell_flag, cell_id)) {
        DE_LOG_ERROR("world", "--cell takes an asset ID, 16 hex digits: '%s'", cell_flag);
        return EXIT_FAILURE;
    }
    // GPU copies of what the registry loads, from now on: made before anything loads.
    auto gpu_assets = de::render::GpuAssets::create({.device = &*device, .uploader = &*uploader, .assets = &*assets});
    if (!gpu_assets) {
        DE_LOG_ERROR("render", "gpu assets: %s", gpu_assets.error().message);
        return EXIT_FAILURE;
    }
    std::optional<de::render::SceneRenderer> scene_renderer;
    if (shaders) {
        auto created = de::render::SceneRenderer::create(
            {.device = &*device, .shaders = &*shaders, .gpu_assets = &*gpu_assets, .shadows = {}});
        if (created) {
            scene_renderer.emplace(std::move(*created));
        }
    }
    de::world::load_cell(world->ecs(), cell_id);

    // A free camera, looking at the test camp from its south side.
    const auto camera_actions = de::tools::add_free_camera_actions(input);
    if (!camera_actions) {
        DE_LOG_ERROR("input", "free camera bindings: %s", camera_actions.error().message);
        return EXIT_FAILURE;
    }
    de::tools::FreeCamera free_camera({64.0, 2.0, 74.0}, 0.0f, -0.12f);
    // The scene's light, and what the window shows: F2 steps through the G-buffer's channels.
    const de::render::SceneLighting scene_lighting;
    de::render::SceneView scene_view = de::render::SceneView::Lit;
    const flecs::entity camera = world->ecs().entity("free_camera").set(de::world::Camera{.vertical_fov_rad = 1.0f});
    world->ecs().set(de::world::ActiveCamera{.entity = camera.id()});

    // Debug UI: Dear ImGui, with the frame stats panel (F1 shows and hides it). Made after the
    // device, uploader and graph, so it goes before them.
    de::tools::DebugUi debug_ui =
        de::tools::DebugUi::create(*window, *device, shaders ? &*shaders : nullptr, *uploader);
    de::tools::StatsPanel stats_panel;
    bool show_stats = true;

    const char* frames_flag = flag_value(argc, argv, "--frames");
    const uint64_t max_frames = frames_flag != nullptr ? std::strtoull(frames_flag, nullptr, 10) : 0;
    uint64_t next_stick_report_ns = 0;
    uint64_t frame = 0;
    const uint64_t start_ns = de::platform::time_ns();
    uint64_t title_start_ns = start_ns;
    uint32_t title_frames = 0;
    bool was_moving = false;
    bool was_minimized = false;
    uint64_t previous_frame_ns = start_ns;
    while (!window->close_requested()) {
        const uint64_t frame_start_allocations = de::core::heap_allocation_count(); // every thread, jobs included
        const uint64_t frame_start_ns = de::platform::time_ns();
        const double frame_s = static_cast<double>(frame_start_ns - previous_frame_ns) * 1e-9;
        previous_frame_ns = frame_start_ns;
        de::platform::pump_events(*window, input);
        input.sample();

        if (input.state(quit).pressed) {
            break;
        }
        if (input.state(attack).pressed) {
            DE_LOG_INFO("input", "attack_light");
        }
        if (input.state(roll).pressed) {
            DE_LOG_INFO("input", "roll");
        }
        const bool moving = input.state(move_x).down || input.state(move_y).down;
        if (moving != was_moving) {
            DE_LOG_INFO("input", moving ? "moving" : "stopped");
            was_moving = moving;
        }
        if (window->minimized() != was_minimized) {
            was_minimized = window->minimized();
            DE_LOG_INFO("platform", was_minimized ? "minimized" : "restored");
        }
        if (window->resized()) {
            DE_LOG_INFO("platform", "drawable size %ux%u", window->pixel_width(), window->pixel_height());
        }
        if (show_sticks && de::platform::time_ns() >= next_stick_report_ns) {
            next_stick_report_ns = de::platform::time_ns() + de::platform::kNsPerSecond / 2;
            DE_LOG_INFO(
                "input", "left %+.3f %+.3f   right %+.3f %+.3f", static_cast<double>(input.state(sticks[0]).value),
                static_cast<double>(input.state(sticks[1]).value), static_cast<double>(input.state(sticks[2]).value),
                static_cast<double>(input.state(sticks[3]).value));
        }

        if (shaders) {
            shaders->update(); // hot reload: picks up edits to shaders/*.slang
        }
        free_camera.update(input, *camera_actions, static_cast<float>(frame_s), camera);
        if (input.state(next_view).pressed) {
            scene_view = static_cast<de::render::SceneView>((static_cast<uint32_t>(scene_view) + 1) %
                                                            static_cast<uint32_t>(de::render::SceneView::Count));
            DE_LOG_INFO("render", "view: %s", de::render::scene_view_name(scene_view));
        }
        assets->update();     // commits finished reads, unloads what nobody holds, reloads re-cooked files
        gpu_assets->update(); // meshes whose uploads are in become drawable

        world->advance(frame_s); // fixed ticks, then RenderExtract

        // The debug UI for this frame. Its textures go to the uploader before record().
        if (input.state(toggle_stats).pressed) {
            show_stats = !show_stats;
        }
        if (debug_ui.begin_frame(*window, static_cast<float>(frame_s))) {
            if (show_stats) {
                stats_panel.draw(&show_stats);
            }
            debug_ui.end_frame();
        }

        device->begin_frame();
        uploader->record(); // uploads land at the start of the frame

        // The scene into the window, then the debug UI over it.
        const de::rhi::TextureHandle backbuffer = device->acquire_swapchain_texture();
        if (backbuffer.is_valid()) {
            graph->begin(device->swapchain_width(), device->swapchain_height());
            const de::render::GraphTexture back = graph->import_texture({.texture = backbuffer,
                                                                         .name = "backbuffer",
                                                                         .state = de::rhi::ResourceState::Present,
                                                                         .final_state = de::rhi::ResourceState::Present,
                                                                         .discard = true});
            // Without shaders (no SceneRenderer), the window is cleared to the scene's empty color.
            static constexpr de::rhi::Color kDusk{.r = 0.10f, .g = 0.11f, .b = 0.15f, .a = 1.0f};
            if (scene_renderer) {
                scene_renderer->add_passes(*graph, back, device->swapchain_format(), world->render_frame(),
                                           scene_lighting, scene_view, kDusk);
            } else {
                graph->add_pass(
                    "clear",
                    [&](de::render::PassBuilder& pass) { pass.use(back, de::rhi::ResourceState::RenderTarget); },
                    [back](de::render::PassContext& context) {
                        const de::rhi::ColorAttachment color{.texture = context.texture(back), .clear = kDusk};
                        context.list.begin_rendering({.colors = std::span(&color, 1)});
                        context.list.end_rendering();
                    });
            }
            debug_ui.add_pass(*graph, back, device->swapchain_format());
            graph->execute();
        }
        device->end_frame();
        if (!backbuffer.is_valid()) {
            de::platform::sleep_ns(5 * de::platform::kNsPerMs); // minimized: nothing to show, don't spin a core
        }

        // Frame rate in the title, once a second.
        ++title_frames;
        if (const uint64_t now = de::platform::time_ns(); now - title_start_ns >= de::platform::kNsPerSecond) {
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
        const uint64_t allocations = de::core::heap_allocation_count() - frame_start_allocations;
        DE_PROFILE_PLOT("heap allocations/frame", static_cast<int64_t>(allocations));
        stats_panel.record({.frame_ms = static_cast<float>(frame_s * 1000.0),
                            .heap_allocations = allocations,
                            .device = device->stats(),
                            .memory = device->memory_stats(),
                            .graph = graph->stats(),
                            .uploads = uploader->stats()});
        DE_PROFILE_FRAME();

        ++frame;
        if (max_frames != 0 && frame >= max_frames) {
            break;
        }
    }

    device->log_memory_report(5);
    device->wait_idle();
    const de::asset::AssetRegistryStats asset_stats = assets->stats();
    DE_LOG_INFO("asset", "%u assets held (%u failed), %llu bytes, %u reloads", asset_stats.assets, asset_stats.failed,
                static_cast<unsigned long long>(asset_stats.bytes), asset_stats.reloads);
    const de::render::GpuAssetsStats gpu_stats = gpu_assets->stats();
    const de::render::SceneRendererStats scene_stats =
        scene_renderer ? scene_renderer->stats() : de::render::SceneRendererStats{};
    DE_LOG_INFO(
        "render",
        "gpu assets: %u meshes, %u textures, %u materials, %llu bytes; last frame drew %u of %u instances in %u draws",
        gpu_stats.meshes, gpu_stats.textures, gpu_stats.materials, static_cast<unsigned long long>(gpu_stats.bytes),
        scene_stats.drawn, scene_stats.instances, scene_stats.draws);
    const de::rhi::DeviceStats stats = device->stats();
    DE_LOG_INFO("engine", "shutting down after %llu frames, %llu world ticks (%u validation errors, %u warnings)",
                static_cast<unsigned long long>(frame), static_cast<unsigned long long>(world->tick_count()),
                stats.validation_errors, stats.validation_warnings);
    return stats.validation_errors == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
