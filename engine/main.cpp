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
#include "render/shader/shader_compiler.h"
#include "render/shader/shader_library.h"
#include "tools/debug_ui.h"
#include "tools/stats_panel.h"
#include "world/cell.h"
#include "world/world.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// Placeholder loop until the frame loop lands (M0b): opens the window, draws a triangle and a
// textured quad (a bindless texture) through the RHI with hot-reloaded Slang shaders, pumps events,
// samples input and logs actions, so the layers below can be tried by hand. Edit
// shaders/triangle.slang or shaders/textured_quad.slang while it runs.
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
    // Built once the swapchain format is known, and again if it changes. If the shader has an
    // error at startup, the library keeps watching it, and the triangle appears once it's fixed.
    de::shader::ProgramHandle triangle;
    de::shader::ProgramHandle quad;
    de::rhi::Format programs_format = de::rhi::Format::Unknown;

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

    // The quad's texture, made here until the cooker brings real ones (after every early return,
    // so nothing leaks): a gold and stone checker darkening toward the bottom.
    constexpr uint32_t kQuadTextureSize = 256;
    constexpr size_t kQuadTextureBytes = size_t{kQuadTextureSize} * kQuadTextureSize * 4;
    const auto quad_texture =
        device->create_texture({.width = kQuadTextureSize,
                                .height = kQuadTextureSize,
                                .format = de::rhi::Format::RGBA8Srgb,
                                .usage = de::rhi::TextureUsage::Sampled | de::rhi::TextureUsage::CopyDst,
                                .debug_name = "quad texture"});
    if (!quad_texture) {
        return EXIT_FAILURE;
    }
    {
        de::core::Array<std::byte> texels(de::core::heap_allocator(de::core::MemoryTag::Render));
        texels.resize(kQuadTextureBytes);
        for (uint32_t y = 0; y < kQuadTextureSize; ++y) {
            for (uint32_t x = 0; x < kQuadTextureSize; ++x) {
                const bool gold = ((x / 32) + (y / 32)) % 2 == 0;
                const bool edge = x % 32 == 0 || y % 32 == 0;
                const float shade = 1.0f - 0.5f * static_cast<float>(y) / kQuadTextureSize;
                const float base[3] = {gold ? 0.85f : 0.20f, gold ? 0.65f : 0.17f, gold ? 0.30f : 0.15f};
                std::byte* texel = &texels[(static_cast<size_t>(y) * kQuadTextureSize + x) * 4];
                for (int c = 0; c < 3; ++c) {
                    texel[c] = static_cast<std::byte>(edge ? 20 : static_cast<int>(base[c] * shade * 255.0f));
                }
                texel[3] = std::byte{255};
            }
        }
        const auto queued = uploader->upload_texture(
            *quad_texture, std::as_bytes(texels.span()),
            {.after = de::rhi::ResourceState::ShaderRead, .after_stages = de::rhi::ShaderStages::Fragment});
        if (!queued) {
            DE_LOG_ERROR("render", "quad texture upload: %s", queued.error().message);
            device->destroy(*quad_texture);
            return EXIT_FAILURE;
        }
    }

    // Cooked assets by ID: reads, reference counts and, in dev builds, hot reload of files the cooker
    // rewrites. Made before the world, which releases what its cells hold through it.
    auto assets = de::asset::AssetRegistry::create({.vfs = &*vfs, .jobs = &jobs});
    if (!assets) {
        DE_LOG_ERROR("asset", "%s", assets.error().message);
        device->destroy(*quad_texture);
        return EXIT_FAILURE;
    }
    // The game world: flecs, ticking at a fixed 60 Hz.
    auto world = de::world::World::create({.jobs = &jobs, .assets = &*assets});
    if (!world) {
        DE_LOG_ERROR("world", "%s", world.error().message);
        device->destroy(*quad_texture);
        return EXIT_FAILURE;
    }
    // One cell, loaded explicitly until the streamer picks cells around the player (M1). Its members
    // spawn once it and its meshes are loaded.
    de::asset::AssetId cell_id = kTestCell;
    if (const char* cell_flag = flag_value(argc, argv, "--cell");
        cell_flag != nullptr && !de::asset::parse_asset_id(cell_flag, cell_id)) {
        DE_LOG_ERROR("world", "--cell takes an asset ID, 16 hex digits: '%s'", cell_flag);
        device->destroy(*quad_texture);
        return EXIT_FAILURE;
    }
    de::world::load_cell(world->ecs(), cell_id);

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
        assets->update(); // commits finished reads, unloads what nobody holds, reloads re-cooked files

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

        // Clear the window to a slowly breathing dusk color, then draw the triangle and the quad.
        const de::rhi::TextureHandle backbuffer = device->acquire_swapchain_texture();
        if (backbuffer.is_valid() && shaders && programs_format != device->swapchain_format()) {
            shaders->destroy(triangle);
            shaders->destroy(quad);
            de::rhi::GraphicsPipelineDesc state{.color_count = 1, .debug_name = "triangle"};
            state.color_formats[0] = device->swapchain_format();
            const auto created_triangle = shaders->create_graphics({.module = "triangle", .state = state});
            triangle = created_triangle.ok() ? *created_triangle : de::shader::ProgramHandle{};
            state.debug_name = "textured quad";
            const auto created_quad = shaders->create_graphics({.module = "textured_quad", .state = state});
            quad = created_quad.ok() ? *created_quad : de::shader::ProgramHandle{};
            programs_format = device->swapchain_format();
        }
        if (backbuffer.is_valid()) {
            // The frame as a graph: one pass on the backbuffer, which comes in from the display (its
            // old contents dropped) and goes back to it. One render pass clears and draws both: a
            // pass per draw would reload the image from memory each time.
            const float seconds = static_cast<float>(de::platform::time_ns() - start_ns) * 1e-9f;
            const float glow = 0.5f + 0.5f * std::sin(seconds * 0.8f);
            graph->begin(device->swapchain_width(), device->swapchain_height());
            const de::render::GraphTexture back = graph->import_texture({.texture = backbuffer,
                                                                         .name = "backbuffer",
                                                                         .state = de::rhi::ResourceState::Present,
                                                                         .final_state = de::rhi::ResourceState::Present,
                                                                         .discard = true});
            struct Scene {
                de::render::GraphTexture back;
                de::rhi::Color dusk; // a slowly breathing dusk color
                de::rhi::PipelineHandle triangle;
                de::rhi::PipelineHandle quad;
                uint32_t quad_texture = 0;
                float seconds = 0.0f;
                float aspect = 1.0f;
            };
            graph->add_pass<Scene>(
                "scene",
                [&](de::render::PassBuilder& pass, Scene& scene) {
                    pass.use(back, de::rhi::ResourceState::RenderTarget);
                    scene = Scene{
                        .back = back,
                        .dusk = {.r = 0.05f + 0.10f * glow, .g = 0.04f + 0.05f * glow, .b = 0.06f, .a = 1.0f},
                        .triangle = shaders ? shaders->pipeline(triangle) : de::rhi::PipelineHandle{},
                        .quad = shaders ? shaders->pipeline(quad) : de::rhi::PipelineHandle{},
                        .quad_texture = device->texture_index(*quad_texture),
                        .seconds = seconds,
                        .aspect = static_cast<float>(device->swapchain_width()) /
                                  static_cast<float>(device->swapchain_height()),
                    };
                },
                [](const Scene& scene, de::render::PassContext& context) {
                    const de::rhi::ColorAttachment color{.texture = context.texture(scene.back), .clear = scene.dusk};
                    context.list.begin_rendering({.colors = std::span(&color, 1)});
                    if (scene.triangle.is_valid()) {
                        // Matches TrianglePush in shaders/triangle.slang.
                        struct TrianglePush {
                            float time_s;
                            float aspect;
                        };
                        context.list.bind_pipeline(scene.triangle);
                        context.list.push_constants(TrianglePush{.time_s = scene.seconds, .aspect = scene.aspect});
                        context.list.draw(3);
                    }
                    if (scene.quad.is_valid()) {
                        // Matches TexturedQuadPush in shaders/textured_quad.slang.
                        struct TexturedQuadPush {
                            float center[2];
                            float size[2];
                            uint32_t texture;
                            uint32_t sampler;
                            float time_s;
                        };
                        context.list.bind_pipeline(scene.quad);
                        context.list.push_constants(TexturedQuadPush{
                            .center = {0.62f, 0.0f},
                            .size = {0.7f / scene.aspect, 0.7f},
                            .texture = scene.quad_texture,
                            .sampler = de::rhi::sampler_index(de::rhi::DefaultSampler::AnisotropicWrap),
                            .time_s = scene.seconds,
                        });
                        context.list.draw(6);
                    }
                    context.list.end_rendering();
                });
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
    device->destroy(*quad_texture);
    device->wait_idle();
    const de::asset::AssetRegistryStats asset_stats = assets->stats();
    DE_LOG_INFO("asset", "%u assets held (%u failed), %llu bytes, %u reloads", asset_stats.assets, asset_stats.failed,
                static_cast<unsigned long long>(asset_stats.bytes), asset_stats.reloads);
    const de::rhi::DeviceStats stats = device->stats();
    DE_LOG_INFO("engine", "shutting down after %llu frames, %llu world ticks (%u validation errors, %u warnings)",
                static_cast<unsigned long long>(frame), static_cast<unsigned long long>(world->tick_count()),
                stats.validation_errors, stats.validation_warnings);
    return stats.validation_errors == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
