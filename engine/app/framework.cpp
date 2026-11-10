#include "app/framework.h"

#include "app/frame_clock.h"
#include "asset/registry.h"
#include "asset/terrain_format.h"
#include "asset/terrain_layers_format.h"
#include "console/console.h"
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
#include "platform/project.h"
#include "platform/thread.h"
#include "platform/time.h"
#include "platform/vfs.h"
#include "platform/window.h"
#include "render/graph/render_graph.h"
#include "render/imgui/imgui_renderer.h"
#include "render/rhi/device.h"
#include "render/rhi/upload.h"
#include "render/scene/gpu_assets.h"
#include "render/shader/shader_asset.h"
#include "render/shader/shader_compiler.h"
#include "render/shader/shader_library.h"
#include "tools/agent_server.h"
#include "tools/console_ui.h"
#include "tools/debug_ui.h"
#include "tools/stats_panel.h"
#include "world/render_extract.h"
#include "world/transform.h"
#include "world/world.h"

#include <imgui.h>

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

#if DE_BUILD_DEV
// A log sink added for as long as it's in scope.
class LogFeedScope {
public:
    explicit LogFeedScope(core::LogSink& sink) : sink_(sink) { core::add_log_sink(sink_); }
    ~LogFeedScope() { core::remove_log_sink(sink_); }
    LogFeedScope(const LogFeedScope&) = delete;
    LogFeedScope& operator=(const LogFeedScope&) = delete;
    LogFeedScope(LogFeedScope&&) = delete;
    LogFeedScope& operator=(LogFeedScope&&) = delete;

private:
    core::LogSink& sink_;
};

// The framework's own console commands (dev builds): the frame stats, the renderer's switches, the camera, quitting.
struct DevConsole {
    Engine& engine;
    tools::StatsPanel& stats;
    tools::CameraPose goto_pose{};
    bool go = false; // goto_pose is next frame's camera
    bool quit = false;
};

// A scene view's name as the console takes it: "base color" -> "base_color".
core::FixedString<31> view_word(render::SceneView view) {
    const std::string_view name = render::scene_view_name(view);
    char word[32] = {};
    const size_t length = std::min(name.size(), sizeof(word) - 1);
    for (size_t i = 0; i < length; ++i) {
        word[i] = name[i] == ' ' ? '_' : name[i];
    }
    return core::FixedString<31>(std::string_view(word, length));
}

void stat_command(void* user, const console::Args& args, console::Output& out) {
    auto& dev = *static_cast<DevConsole*>(user);
    const uint32_t shown = dev.engine.frame_stats ? dev.stats.sections() : 0;
    if (args.empty()) {
        out.info("frame stats %s (F1); a section with stat <section>:", dev.engine.frame_stats ? "shown" : "hidden");
        for (const tools::StatsSectionName& section : tools::kStatsSectionNames) {
            out.info("  %-14.*s %s", static_cast<int>(section.name.size()), section.name.data(),
                     (shown & section.bits) != 0 ? "on" : "off");
        }
        return;
    }
    const std::string_view word = args[0];
    if (word == "none") {
        dev.stats.set_sections(0);
        dev.engine.frame_stats = false;
        out.info("frame stats hidden");
        return;
    }
    uint32_t sections = 0;
    if (!tools::parse_stats_sections(word, sections) || word.find(',') != std::string_view::npos) {
        out.error("No section '%.*s' (stat alone lists them; stats takes several)", static_cast<int>(word.size()),
                  word.data());
        return;
    }
    if (word == "default" || word == "all") {
        dev.stats.set_sections(sections);
        dev.engine.frame_stats = true;
        out.info("frame stats: %.*s", static_cast<int>(word.size()), word.data());
        return;
    }
    // As Unreal's: on if it wasn't showing, off if it was; the stats hide with nothing left on.
    const uint32_t next = (shown & sections) == sections ? shown & ~sections : shown | sections;
    dev.stats.set_sections(next);
    dev.engine.frame_stats = next != 0;
    out.info("stat %.*s: %s", static_cast<int>(word.size()), word.data(), (next & sections) != 0 ? "on" : "off");
}

void stats_command(void* user, const console::Args& args, console::Output& out) {
    auto& dev = *static_cast<DevConsole*>(user);
    uint32_t sections = 0;
    if (args.size() != 1 || !tools::parse_stats_sections(args[0], sections)) {
        out.error("stats takes sections, comma separated: frame, graph, allocations, camera, cull, memory, gpu, "
                  "render_graph, uploads, device, default, all");
        return;
    }
    dev.stats.set_sections(sections);
    dev.engine.frame_stats = sections != 0;
    out.info("frame stats: %.*s", static_cast<int>(args[0].size()), args[0].data());
}

void view_command(void* user, const console::Args& args, console::Output& out) {
    auto& dev = *static_cast<DevConsole*>(user);
    const auto count = static_cast<uint32_t>(render::SceneView::Count);
    if (!args.empty()) {
        // Any case, with spaces or underscores: "base_color", "Base Color".
        const auto same = [](std::string_view typed, std::string_view name) {
            size_t a = 0;
            size_t b = 0;
            const auto skip = [](std::string_view text, size_t& at) {
                while (at < text.size() && (text[at] == '_' || text[at] == ' ')) {
                    ++at;
                }
            };
            for (;;) {
                skip(typed, a);
                skip(name, b);
                if (a == typed.size() || b == name.size()) {
                    return a == typed.size() && b == name.size();
                }
                const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
                if (lower(typed[a++]) != lower(name[b++])) {
                    return false;
                }
            }
        };
        uint32_t found = count;
        for (uint32_t i = 0; i < count; ++i) {
            if (same(args[0], render::scene_view_name(static_cast<render::SceneView>(i)))) {
                found = i;
            }
        }
        if (found == count) {
            out.error("No view '%.*s'", static_cast<int>(args[0].size()), args[0].data());
        } else {
            dev.engine.view = static_cast<render::SceneView>(found);
        }
    }
    char names[console::kLineLength + 1] = {};
    size_t length = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const int written = std::snprintf(names + length, sizeof(names) - length, "%s%s", i == 0 ? "" : ", ",
                                          view_word(static_cast<render::SceneView>(i)).c_str());
        length = std::min(length + static_cast<size_t>(std::max(written, 0)), sizeof(names) - 1);
    }
    out.info("r.view = %s (%s)", view_word(dev.engine.view).c_str(), names);
}

void goto_command(void* user, const console::Args& args, console::Output& out) {
    auto& dev = *static_cast<DevConsole*>(user);
    double place[3] = {};
    float turn_deg[2] = {};
    const bool turned = args.size() == 5;
    bool ok = args.size() == 3 || turned;
    for (size_t i = 0; i < 3 && ok; ++i) {
        ok = console::Args::parse_double(args[i], place[i]);
    }
    for (size_t i = 0; i < 2 && ok && turned; ++i) {
        ok = console::Args::parse_float(args[3 + i], turn_deg[i]);
    }
    if (!ok) {
        out.error("camera.goto takes x y z (meters), and yaw and pitch (degrees) if it's to face elsewhere");
        return;
    }
    // Facing as the camera does now, unless told.
    float yaw_rad = 0.0f;
    float pitch_rad = 0.0f;
    const flecs::world& ecs = dev.engine.world.ecs();
    const auto* active = ecs.try_get<world::ActiveCamera>();
    const auto* place_now = active != nullptr && active->entity != 0 && ecs.is_alive(active->entity)
                                ? ecs.entity(active->entity).try_get<world::WorldTransform>()
                                : nullptr;
    if (place_now != nullptr) {
        const Vec3 forward = place_now->rotation * Vec3(0.0f, 0.0f, -1.0f);
        yaw_rad = std::atan2(-forward.x, -forward.z);
        pitch_rad = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
    }
    if (turned) {
        yaw_rad = glm::radians(turn_deg[0]);
        pitch_rad = glm::radians(turn_deg[1]);
    }
    dev.goto_pose = {.position = DVec3(place[0], place[1], place[2]), .yaw_rad = yaw_rad, .pitch_rad = pitch_rad};
    dev.go = true;
    out.info("camera to %.2f %.2f %.2f, yaw %.1f, pitch %.1f", place[0], place[1], place[2],
             static_cast<double>(glm::degrees(yaw_rad)), static_cast<double>(glm::degrees(pitch_rad)));
}

void quit_command(void* user, const console::Args& /*args*/, console::Output& out) {
    static_cast<DevConsole*>(user)->quit = true;
    out.info("closing");
}

void console_show_command(void* user, const console::Args& args, console::Output& out) {
    auto& window = *static_cast<tools::ConsoleUi*>(user);
    bool open = !window.is_open();
    if (!args.empty() && !console::Args::parse_bool(args[0], open)) {
        out.error("console.show takes on or off");
        return;
    }
    window.set_open(open);
    out.info("console window %s", open ? "open" : "closed");
}

void physics_stats_command(void* user, const console::Args& /*args*/, console::Output& out) {
    const physics::PhysicsStats stats = static_cast<DevConsole*>(user)->engine.physics.stats();
    out.info("bodies %u (%u awake), characters %u, shapes %u, ground tiles %u", stats.bodies, stats.active_bodies,
             stats.characters, stats.shapes, stats.terrain_tiles);
}

void add_dev_commands(console::Console& console, DevConsole& dev) {
    console.add_command("physics.stats", "", "Bodies, characters, shapes and the ground's collision tiles, now.",
                        &physics_stats_command, &dev);
    console.add_command("stat", "[section | none | default | all]",
                        "Show or hide one section of the frame stats (F1). With nothing after it, lists the sections.",
                        &stat_command, &dev);
    console.add_command("stats", "<section,section,...>", "Show exactly these frame stats sections (as --stats does).",
                        &stats_command, &dev);
    console.add_variable("r.taa", "Temporal anti-aliasing on or off (F3).", &dev.engine.taa, &dev);
    console.add_variable(
        "physics.show_colliders",
        "Draw physics' colliders near the camera: static blue, ground green, moving orange (grey asleep), "
        "characters yellow.",
        &dev.engine.show_colliders, &dev);
    console.add_variable("physics.show_selected_colliders",
                         "Draw only the application's chosen entities' colliders (the editor's selection).",
                         &dev.engine.show_selected_colliders, &dev);
    console.add_variable("r.freeze_cull", "Freeze culling where the camera is now, to look around it (F4).",
                         &dev.engine.freeze_cull, &dev);
    console.add_variable("r.fog_low_camera", "Thin the height fog when the camera is low (F5).",
                         &dev.engine.fog_low_camera, &dev);
    console.add_command("r.view", "[name]",
                        "Show the scene lit or as one buffer: base_color, normal, roughness... (F2).", &view_command,
                        &dev);
    console.add_command("camera.goto", "<x> <y> <z> [yaw pitch]",
                        "Move the camera to a place in meters; yaw and pitch in degrees turn it too.", &goto_command,
                        &dev);
    console.add_command("quit", "", "Close the engine.", &quit_command, &dev);
}
#endif

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
    out.project = flag_value(out, "--project");
    out.vsync = !has_flag(out, "--no-vsync");
    if (const char* cap = flag_value(out, "--fps-cap")) {
        out.fps_cap = static_cast<uint32_t>(std::strtoul(cap, nullptr, 10));
    }
    out.validation = DE_BUILD_DEV != 0 && !has_flag(out, "--no-validation");
    out.show_sticks = DE_BUILD_DEV != 0 && has_flag(out, "--show-sticks");
    out.fullscreen = has_flag(out, "--fullscreen");
#if DE_BUILD_DEV
    if (const char* stats = flag_value(out, "--stats")) {
        if (!tools::parse_stats_sections(stats, out.stats_sections)) {
            DE_LOG_ERROR("app",
                         "--stats: '%s' names a section there isn't (frame, graph, allocations, camera, cull, "
                         "memory, gpu, render_graph, uploads, device, default, all)",
                         stats);
            return false;
        }
    }
#endif
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
#if DE_BUILD_DEV
    if (!parse_eyes_flags(out, out.eyes)) {
        return false; // logged
    }
    out.vsync = out.vsync && !out.eyes.no_vsync;
#endif
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

    // game:/ is the cooked data. Dev builds also mount the open project's sources as src:/ and the engine's own
    // (shaders/) as engine:/: shaders compile from engine:/ and hot reload; without them, they load from the
    // cooker's .deshader files. The project's cooked/ is game:/ unless --game-dir says otherwise.
    const char* engine_root = DE_BUILD_DEV != 0 ? DE_SOURCE_DIR : nullptr;
    const char* src_root = nullptr;
    platform::Project project;
    if (DE_BUILD_DEV != 0) {
        const char* given = args.project != nullptr ? args.project : DE_DEFAULT_PROJECT;
        if (const core::Result<platform::Project> found = platform::find_project(given, engine_root)) {
            project = *found;
            src_root = project.root.c_str();
            DE_LOG_INFO("engine", "project %s: %s", project.name.c_str(), project.root.c_str());
        } else {
            DE_LOG_WARN("engine", "project %s: %s: running without src:/", given, found.error().message);
        }
    }
    const char* game_root = args.game_dir;
    platform::PathString project_cooked;
    if (src_root != nullptr && flag_value(args, "--game-dir") == nullptr && project_cooked.assign(src_root) &&
        project_cooked.append("/cooked")) {
        game_root = project_cooked.c_str();
    }
    auto vfs = platform::Vfs::create(
        {.game_root = game_root, .src_root = src_root, .engine_root = engine_root, .jobs = &jobs});
    if (!vfs && engine_root != nullptr) {
        // A dev build away from its repository: run without src:/ and engine:/ rather than not at all.
        DE_LOG_WARN("engine", "the engine's sources %s or the project's aren't there: running without them",
                    engine_root);
        src_root = nullptr;
        engine_root = nullptr;
        vfs = platform::Vfs::create({.game_root = game_root, .jobs = &jobs});
    }
    if (!vfs) {
        return EXIT_FAILURE;
    }
    core::Result<shader::ShaderCompiler> compiler =
        core::fail(core::ErrorCode::Unsupported, "no engine:/ to compile shaders from");
    if (engine_root != nullptr) {
        compiler = shader::ShaderCompiler::create(*vfs);
    }
    if (!compiler && engine_root != nullptr) {
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
#if DE_BUILD_DEV
    AgentEyes eyes;
    if (!eyes.start(args.eyes, *vfs, config_path(src_root != nullptr, "camera.views").c_str())) {
        return EXIT_FAILURE; // logged, and no window has opened
    }
    const bool scripted = eyes.scripted();
    const bool swapchain_copy_src = eyes.takes_pictures();
#else
    const bool scripted = false;
    const bool swapchain_copy_src = false;
#endif
    auto window = platform::Window::create({.title = "DEengine",
                                            .width_points = args.window_width_points,
                                            .height_points = args.window_height_points,
                                            .fullscreen = args.fullscreen});
    if (!window) {
        return EXIT_FAILURE;
    }
    auto device = rhi::Device::create({.window = &*window,
                                       .validation = args.validation,
                                       .vsync = args.vsync,
                                       .swapchain_copy_src = swapchain_copy_src});
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
    physics::Physics physics({.jobs = &jobs, .assets = &*assets});
    auto world = world::World::create({.jobs = &jobs, .assets = &*assets});
    if (!world) {
        DE_LOG_ERROR("world", "%s", world.error().message);
        return EXIT_FAILURE;
    }
    physics.add_to_world(*world);
#if DE_BUILD_DEV
    core::Array<world::DebugLine> debug_lines(
        core::heap_allocator(core::MemoryTag::Physics)); // kept: no allocation a frame
#endif
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

    console::Console console(core::heap_allocator(core::MemoryTag::Tools));
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
                  .console = console,
                  .settings = as_text(settings),
                  .lighting = {},
                  .view = render::SceneView::Lit,
                  .taa = setting(as_text(settings), "taa") == "on",
                  .terrain = {},
                  .terrain_layers = {},
                  .water = {},
                  .fog_low_camera = true};
    asset::AssetHandle shown_terrain;
    uint32_t shown_terrain_revision = 0;
    asset::AssetHandle shown_layers;
    uint32_t shown_layers_revision = 0;

    // --- Dev tools: the debug UI (F1: frame stats) and the raw stick readout ---------------------
#if DE_BUILD_DEV
    tools::DebugUi debug_ui = tools::DebugUi::create(*window, *device, engine.shaders, *uploader);
    tools::StatsPanel stats_panel;
    // What it shows changes from run to run: not in a picture to compare, unless it's asked for (--stats).
    engine.frame_stats = !scripted || has_flag(args, "--stats");
    stats_panel.set_sections(args.stats_sections != 0 ? args.stats_sections : tools::kStatsDefault);
    engine.scripted = scripted;
    tools::ConsoleUi console_ui(console);
    console_ui.load_history(*vfs);
    engine.console_ui = &console_ui;
    engine.stats_panel = &stats_panel;
    // The engine's log, into the console's (the Output panel, the console window).
    console::LogFeed log_feed(core::heap_allocator(core::MemoryTag::Tools));
    const LogFeedScope log_feed_scope(log_feed);
    DevConsole dev_console{.engine = engine, .stats = stats_panel};
    add_dev_commands(console, dev_console);
    console.add_command("console.show", "[on | off]", "Open or close the console window (the ` key).",
                        &console_show_command, &console_ui);
    // The agents' door (tools/agent_server.h): not for a picture, which takes no input from anywhere.
    std::optional<tools::AgentServer> agent_server;
    if (!scripted && !has_flag(args, "--no-agent-server")) {
        if (core::Result<tools::AgentServer> started = tools::AgentServer::start(); started) {
            agent_server.emplace(std::move(*started));
            DE_LOG_INFO("agents", "commands on 127.0.0.1:%u (JSON-RPC, a request a line; tools/console.py)",
                        static_cast<unsigned>(agent_server->port()));
        } else {
            DE_LOG_WARN("agents", "no socket for agents: %s", started.error().message);
        }
    }
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
    // The application's viewport panel (Engine::viewport): the scene goes into this texture instead of the window.
    rhi::TextureHandle viewport_texture;
    rhi::TextureDesc viewport_desc{.width = 0, .height = 0};
    rhi::ResourceState viewport_state = rhi::ResourceState::Undefined;
    bool scene_in_viewport = false; // this frame
#endif

    if (!application.startup(engine, args) || !load_bindings(*vfs, input, src_root != nullptr)) {
        application.shutdown(engine); // what startup got before it failed
        return EXIT_FAILURE;
    }
    DE_LOG_INFO("input", "stick deadzone %.2f", static_cast<double>(input.stick_deadzone()));
#if DE_BUILD_DEV
    // --exec "stat gpu; r.taa 0": what Unreal's -ExecCmds does, once everything is up.
    if (const char* exec = flag_value(args, "--exec")) {
        core::Array<console::Line> said(core::heap_allocator(core::MemoryTag::Tools));
        static_cast<void>(console.execute(exec, &said));
        for (const console::Line& line : said) {
            if (line.severity == console::Severity::Info) {
                DE_LOG_INFO("console", "%s", line.text.c_str());
            }
        }
    }
#endif
#if DE_BUILD_DEV
    if (scripted && !eyes.wait_for_scene(*window, input, *assets, *gpu_assets, *uploader, *device)) {
        application.shutdown(engine);
        static_cast<void>(eyes.finish(*device));
        return EXIT_FAILURE;
    }
#endif

    // --- The frame loop ---------------------------------------------------------------------------
    uint64_t frame = 0;
    uint64_t previous_frame_ns = platform::time_ns();
    FrameClock frame_clock;
    uint64_t title_start_ns = previous_frame_ns;
    uint32_t title_frames = 0;
    bool was_minimized = false;
    while (true) {
        const uint64_t frame_start_allocations = core::heap_allocation_count(); // every thread, jobs included
        const uint64_t frame_start_ns = platform::time_ns();
        const double measured_s = static_cast<double>(frame_start_ns - previous_frame_ns) * 1e-9;
        // What the camera and the world move by.
        // Minimized, nothing is presented: there are no refreshes to count. Capped, frames aren't paced by them either.
        const double clock_s = frame_clock.step(
            measured_s, device->vsync() && !window->minimized() && args.fps_cap == 0 ? window->refresh_hz() : 0.0);
#if DE_BUILD_DEV
        const double frame_s = eyes.frame_s(clock_s);
        engine.camera_pose = eyes.begin_frame();
        if (dev_console.go) {
            engine.camera_pose = &dev_console.goto_pose; // camera.goto
            dev_console.go = false;
        }
#else
        const double frame_s = clock_s;
#endif
        DE_PROFILE_PLOT("frame clock on the refresh rate", static_cast<int64_t>(frame_clock.holds_rate() ? 1 : 0));
        previous_frame_ns = frame_start_ns;

        platform::pump_events(*window, input);
        if (window->close_requested()) {
            if (application.may_close(engine)) {
                break;
            }
            window->cancel_close();
        }
#if DE_BUILD_DEV
        // Typing in the console (or any text field) mustn't fly the camera or press F1.
        if (ImGui::GetIO().WantTextInput) {
            input.release_all(platform::time_ns());
        }
#endif
        if (!scripted) { // a stray key or a drifting stick mustn't change a script's pictures
            input.sample();
        }
#if DE_BUILD_DEV
        log_feed.take(console);
        if (agent_server) {
            agent_server->poll(console);
        }
        if (dev_console.quit) {
            break;
        }
#endif
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
        assets->update();     // commits finished reads, unloads what nobody holds, reloads re-cooked files
        gpu_assets->update(); // meshes whose uploads are in become drawable
        // The ground's collision: the terrain the application names (the registry's bytes, current after update()).
        physics.set_terrain(assets->get<asset::TerrainAsset>(engine.terrain), &*vfs);
        world->advance(frame_s); // fixed ticks, then RenderExtract

#if DE_BUILD_DEV
        if (input.state(toggle_stats).pressed) {
            engine.frame_stats = !engine.frame_stats;
            if (stats_panel.sections() == 0) {
                stats_panel.set_sections(tools::kStatsDefault); // after "stat none"
            }
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
        // The viewport's texture, at the size the application asked for last frame; none if it didn't ask. The old one
        // goes once the GPU is done with it.
        if (engine.viewport.want_width != viewport_desc.width || engine.viewport.want_height != viewport_desc.height ||
            device->swapchain_format() != viewport_desc.format) {
            device->destroy(viewport_texture);
            viewport_texture = {};
            viewport_state = rhi::ResourceState::Undefined;
            viewport_desc = {.width = engine.viewport.want_width,
                             .height = engine.viewport.want_height,
                             .format = device->swapchain_format(),
                             .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled |
                                      rhi::TextureUsage::CopySrc,
                             .debug_name = "viewport"};
            if (viewport_desc.width > 0 && viewport_desc.height > 0 && viewport_desc.format != rhi::Format::Unknown) {
                if (auto made = device->create_texture(viewport_desc); made) {
                    viewport_texture = *made;
                } else {
                    DE_LOG_ERROR("render", "no %ux%u viewport: %s", viewport_desc.width, viewport_desc.height,
                                 made.error().message);
                }
            }
        }
        engine.viewport.texture_id = viewport_texture.is_valid()
                                         ? render::ImGuiRenderer::texture_id(device->texture_index(viewport_texture))
                                         : 0;
        engine.viewport.width = viewport_texture.is_valid() ? viewport_desc.width : 0;
        engine.viewport.height = viewport_texture.is_valid() ? viewport_desc.height : 0;
        engine.viewport.want_width = 0; // asked again by ui(), every frame it's shown
        engine.viewport.want_height = 0;
        engine.viewport.ui_width = 0.0f;
        engine.viewport.ui_height = 0.0f;
        engine.viewport.overlays_drawn = false;
        // The debug UI's frame; its textures go to the uploader before record().
        if (debug_ui.begin_frame(*window, static_cast<float>(measured_s))) {
            application.ui(engine);
            // Over the game's picture: the editor's panel, when it's shown in one, or the window.
            const SceneViewport& shown = engine.viewport;
            const ImGuiViewport* window_view = ImGui::GetMainViewport();
            const bool in_panel = shown.ui_width > 0.0f && shown.ui_height > 0.0f;
            const ImVec4 picture =
                in_panel ? ImVec4(shown.ui_x, shown.ui_y, shown.ui_width, shown.ui_height)
                         : ImVec4(window_view->Pos.x, window_view->Pos.y, window_view->Size.x, window_view->Size.y);
            if (!shown.overlays_drawn) {
                if (engine.frame_stats) {
                    stats_panel.draw_overlay(picture.x, picture.y, picture.z, picture.w);
                }
                if (engine.freeze_cull) {
                    tools::draw_viewport_notice(ImGui::GetBackgroundDrawList(), picture.x, picture.y, picture.z,
                                                "CULL FROZEN (F4)");
                }
            }
            console_ui.draw(picture.x, picture.y, picture.z, picture.w);
            debug_ui.end_frame();
        }
        scene_in_viewport = viewport_texture.is_valid() && engine.viewport.want_width > 0;
#endif

        device->begin_frame();
        // What the application asks of the renderer, in every build.
        if (scene_renderer) {
            scene_renderer->set_fog_low_camera(engine.fog_low_camera);
            scene_renderer->set_water(engine.water);
#if DE_BUILD_DEV
            scene_renderer->set_editor_overlay(engine.editor_overlay);
            // Physics' colliders around the camera, when asked for.
            debug_lines.clear();
            if (const world::RenderFrame& drawn = world->render_frame(); drawn.camera.valid) {
                const DVec3 camera = drawn.origin + DVec3(drawn.camera.transform[3], drawn.camera.transform[7],
                                                          drawn.camera.transform[11]);
                if (engine.show_colliders) {
                    physics.collect_collider_lines(camera, Engine::kColliderReachM, debug_lines,
                                                   render::SceneRenderer::kMaxDebugLines);
                } else if (engine.show_selected_colliders) {
                    if (engine.collider_ground) {
                        physics.collect_collider_lines(camera, Engine::kColliderReachM, debug_lines,
                                                       render::SceneRenderer::kMaxDebugLines, true);
                    }
                    physics.collect_entity_collider_lines(world->ecs(), engine.collider_entities, debug_lines,
                                                          render::SceneRenderer::kMaxDebugLines);
                }
            }
            scene_renderer->set_debug_lines(debug_lines.span());
            engine.editor_overlay.pick = false; // asked once
#endif
            // The ground: when the application names another, or it's re-cooked.
            const uint32_t terrain_revision = engine.assets.revision(engine.terrain);
            if (engine.terrain != shown_terrain || terrain_revision != shown_terrain_revision) {
                shown_terrain = engine.terrain;
                shown_terrain_revision = terrain_revision;
                const auto* terrain = engine.assets.get<asset::TerrainAsset>(engine.terrain);
                scene_renderer->set_terrain(terrain, terrain != nullptr ? engine.assets.file(engine.terrain)
                                                                        : std::span<const std::byte>{});
            }
            const uint32_t layers_revision = engine.assets.revision(engine.terrain_layers);
            if (engine.terrain_layers != shown_layers || layers_revision != shown_layers_revision) {
                shown_layers = engine.terrain_layers;
                shown_layers_revision = layers_revision;
                const auto* layers = engine.assets.get<asset::TerrainLayersAsset>(engine.terrain_layers);
                scene_renderer->set_terrain_layers(layers, layers != nullptr ? engine.assets.file(engine.terrain_layers)
                                                                             : std::span<const std::byte>{});
            }
        }
        uploader->record(); // uploads land at the start of the frame
        const rhi::TextureHandle backbuffer = device->acquire_swapchain_texture();
        if (backbuffer.is_valid()) {
            const auto clear = [&](render::GraphTexture target) {
                graph->add_pass(
                    "clear", [&](render::PassBuilder& pass) { pass.use(target, rhi::ResourceState::RenderTarget); },
                    [target](render::PassContext& context) {
                        const rhi::ColorAttachment color{.texture = context.texture(target),
                                                         .clear = render::kEmptySceneColor};
                        context.list.begin_rendering({.colors = std::span(&color, 1)});
                        context.list.end_rendering();
                    });
            };
            // The scene at the window's size, or at the viewport's, into its texture.
#if DE_BUILD_DEV
            const bool in_viewport = scene_in_viewport;
            graph->begin(in_viewport ? viewport_desc.width : device->swapchain_width(),
                         in_viewport ? viewport_desc.height : device->swapchain_height());
#else
            graph->begin(device->swapchain_width(), device->swapchain_height());
#endif
            const render::GraphTexture back = graph->import_texture({.texture = backbuffer,
                                                                     .name = "backbuffer",
                                                                     .state = rhi::ResourceState::Present,
                                                                     .final_state = rhi::ResourceState::Present,
                                                                     .discard = true});
#if DE_BUILD_DEV
            render::GraphTexture scene_target = back;
            if (in_viewport) {
                // Left where the UI reads it; the window under the UI is cleared.
                scene_target = graph->import_texture({.texture = viewport_texture,
                                                      .name = "viewport",
                                                      .state = viewport_state,
                                                      .stages = rhi::ShaderStages::Fragment,
                                                      .final_state = rhi::ResourceState::ShaderRead,
                                                      .final_stages = rhi::ShaderStages::Fragment,
                                                      .discard = true});
                viewport_state = rhi::ResourceState::ShaderRead;
                clear(back);
            }
#else
            const render::GraphTexture scene_target = back;
#endif
            if (scene_renderer) {
                scene_renderer->add_passes(*graph, scene_target, device->swapchain_format(), world->render_frame(),
                                           engine.lighting, engine.view, engine.taa);
                if (scene_renderer->instances_stale()) {
                    world->resync_render_instances();
                }
#if DE_BUILD_DEV
                engine.pick_answer = scene_renderer->pick_answer();
#endif
            } else {
                clear(scene_target); // no scene renderer: cleared
            }
#if DE_BUILD_DEV
            debug_ui.add_pass(*graph, back, device->swapchain_format(),
                              in_viewport ? std::span<const render::GraphTexture>(&scene_target, 1)
                                          : std::span<const render::GraphTexture>{});
            eyes.add_picture_pass(*device, *graph, back, backbuffer);
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
#if DE_BUILD_DEV
        eyes.end_frame(*device, *vfs, backbuffer.is_valid());
#endif
        if (!backbuffer.is_valid()) {
            world->resync_render_instances();           // what changes while nothing's drawn would be lost
            platform::sleep_ns(5 * platform::kNsPerMs); // minimized: nothing to show, don't spin a core
        }
        if (args.fps_cap > 0) {
            const uint64_t period_ns = platform::kNsPerSecond / args.fps_cap;
            if (const uint64_t spent_ns = platform::time_ns() - frame_start_ns; spent_ns < period_ns) {
                platform::sleep_ns(period_ns - spent_ns);
            }
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
#if DE_BUILD_DEV
        if (eyes.done()) {
            break;
        }
#endif
    }

    // --- Down -------------------------------------------------------------------------------------
    application.shutdown(engine);
#if DE_BUILD_DEV
    if (!scripted) {
        console_ui.save_history(*vfs);
    }
#endif
#if DE_BUILD_DEV
    device->destroy(viewport_texture);
#endif
#if DE_BUILD_DEV
    const bool eyes_ok = eyes.finish(*device);
#else
    const bool eyes_ok = true;
#endif
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
    return stats.validation_errors == 0 && eyes_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace de::app
