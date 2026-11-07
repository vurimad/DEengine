#include "app/agent_eyes.h"

#include "app/framework.h"
#include "asset/registry.h"
#include "core/allocator.h"
#include "core/log.h"
#include "core/profile.h"
#include "platform/events.h"
#include "platform/input.h"
#include "platform/time.h"
#include "platform/vfs.h"
#include "platform/window.h"
#include "render/graph/render_graph.h"
#include "render/rhi/device.h"
#include "render/rhi/upload.h"
#include "render/scene/gpu_assets.h"

#include <glm/trigonometric.hpp>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <string_view>
#include <system_error>
#include <utility>

namespace de::app {

namespace {

// Past this, a flight is a typo in its step or its speed, not a run anyone wants: about 4.6 hours at 60 Hz.
constexpr uint64_t kMostFlightFrames = 1'000'000;
constexpr uint64_t kMostSceneWaitNs = 120 * platform::kNsPerSecond;
constexpr double kMostFixedStepS = 1.0;

// A flag's value. False (logged) if the flag is there without one.
bool read_value(const FrameworkArgs& args, const char* flag, const char*& out) {
    if (!has_flag(args, flag)) {
        return true;
    }
    out = flag_value(args, flag);
    if (out == nullptr) {
        DE_LOG_ERROR("eyes", "%s needs a value", flag);
    }
    return out != nullptr;
}

template <typename Whole>
bool read_whole(const FrameworkArgs& args, const char* flag, Whole least, Whole& out) {
    const char* text = nullptr;
    if (!read_value(args, flag, text)) {
        return false;
    }
    if (text == nullptr) {
        return true;
    }
    const std::string_view word(text);
    const auto [stop, error] = std::from_chars(word.data(), word.data() + word.size(), out);
    if (error != std::errc{} || stop != word.data() + word.size() || out < least) {
        DE_LOG_ERROR("eyes", "%s must be a whole number, at least %llu: '%s'", flag,
                     static_cast<unsigned long long>(least), text);
        return false;
    }
    return true;
}

// A number over 0, and at most most.
bool read_positive(const FrameworkArgs& args, const char* flag, double most, double& out) {
    const char* text = nullptr;
    if (!read_value(args, flag, text)) {
        return false;
    }
    if (text == nullptr) {
        return true;
    }
    const std::string_view word(text);
    const auto [stop, error] = std::from_chars(word.data(), word.data() + word.size(), out);
    // The range test is negated, so NaN fails it too.
    if (error != std::errc{} || stop != word.data() + word.size() || !(out > 0.0 && out <= most)) {
        DE_LOG_ERROR("eyes", "%s must be a number over 0, at most %g: '%s'", flag, most, text);
        return false;
    }
    return true;
}

// Flags that mean nothing, or something else, next to each other. False (logged) if both are there.
bool apart(const FrameworkArgs& args, const char* first, const char* second) {
    if (has_flag(args, first) && has_flag(args, second)) {
        DE_LOG_ERROR("eyes", "%s and %s don't go together", first, second);
        return false;
    }
    return true;
}

// A flag that says how to do what another asks for. False (logged) if it's there without it.
bool needs(const FrameworkArgs& args, const char* flag, const char* what_for) {
    if (has_flag(args, flag) && !has_flag(args, what_for)) {
        DE_LOG_ERROR("eyes", "%s means nothing without %s", flag, what_for);
        return false;
    }
    return true;
}

// Whether a picture can be written there, as far as can be told before the run: the mount and the path rules.
bool writable_png(const platform::Vfs& vfs, const char* path, const char* flag) {
    const std::string_view text(path);
    if (text.starts_with("game:/")) {
        DE_LOG_ERROR("eyes", "%s: game:/ is read only: '%s'", flag, path);
        return false;
    }
    if (!text.ends_with(".png")) {
        DE_LOG_ERROR("eyes", "%s writes a PNG, so its path ends in .png: '%s'", flag, path);
        return false;
    }
    const core::Result<platform::PathString> resolved = vfs.resolve(path);
    if (!resolved) {
        DE_LOG_ERROR("eyes", "%s: %s: '%s'", flag, resolved.error().message, path);
    }
    return resolved.ok();
}

bool numbered_picture(const char* prefix, uint64_t number, platform::PathString& path) {
    char name[32];
    std::snprintf(name, sizeof(name), "_%04llu.png", static_cast<unsigned long long>(number));
    return path.assign(prefix) && path.append(name);
}

} // namespace

bool parse_eyes_flags(const FrameworkArgs& args, EyesFlags& out) {
    const char* camera = nullptr;
    const char* still = nullptr;
    if (!read_value(args, "--camera", camera) || !read_value(args, "--view", out.view) ||
        !read_value(args, "--views-file", out.views_file) || !read_value(args, "--still", still) ||
        !read_value(args, "--screenshot", out.screenshot) || !read_value(args, "--camera-path", out.path_file) ||
        !read_value(args, "--path-shots", out.path_shots) ||
        !read_whole<uint64_t>(args, "--screenshot-frame", 1, out.screenshot_frame) ||
        !read_whole<uint32_t>(args, "--settle", 1, out.settle_frames) ||
        !read_whole<uint32_t>(args, "--path-every", 1, out.path_every) ||
        !read_positive(args, "--fixed-step", kMostFixedStepS, out.fixed_step_s) ||
        !read_positive(args, "--path-speed", 1e6, out.path_speed_mps)) {
        return false;
    }
    if (!apart(args, "--camera", "--view") || !apart(args, "--still", "--screenshot") ||
        !apart(args, "--still", "--screenshot-frame") || !apart(args, "--camera-path", "--camera") ||
        !apart(args, "--camera-path", "--view") || !apart(args, "--camera-path", "--still") ||
        !apart(args, "--camera-path", "--screenshot") || !apart(args, "--camera-path", "--frames") ||
        !needs(args, "--views-file", "--view") || !needs(args, "--screenshot-frame", "--screenshot") ||
        !needs(args, "--path-shots", "--camera-path") || !needs(args, "--path-speed", "--camera-path") ||
        !needs(args, "--path-every", "--path-shots")) {
        return false;
    }
    if (has_flag(args, "--settle") && still == nullptr && out.screenshot == nullptr && out.path_file == nullptr) {
        DE_LOG_ERROR("eyes", "--settle means nothing without a picture or a path to settle for");
        return false;
    }
    if (camera != nullptr) {
        const core::Result<tools::CameraPose> pose = tools::parse_camera_pose(camera);
        if (!pose) {
            DE_LOG_ERROR("eyes", "--camera: %s: '%s'", pose.error().message, camera);
            return false;
        }
        out.camera = *pose;
    }
    if (still != nullptr) {
        out.screenshot = still;
    }
    if (still != nullptr || out.path_file != nullptr) {
        out.no_vsync = true;
        if (out.fixed_step_s == 0.0) {
            out.fixed_step_s = kEyesFrameS;
        }
    }
    if (out.screenshot != nullptr) {
        if (out.screenshot_frame == 0) {
            out.screenshot_frame = out.settle_frames;
        }
        if (args.frames != 0 && args.frames < out.screenshot_frame) {
            DE_LOG_ERROR("eyes", "--frames %llu ends the run before its picture, at frame %llu",
                         static_cast<unsigned long long>(args.frames),
                         static_cast<unsigned long long>(out.screenshot_frame));
            return false;
        }
        out.run_frames = args.frames;
    }
    return true;
}

bool AgentEyes::start(const EyesFlags& flags, const platform::Vfs& vfs, const char* default_views) {
    flags_ = flags;
    pose_ = flags.camera;
    if (flags.view != nullptr) {
        const char* const views = flags.views_file != nullptr ? flags.views_file : default_views;
        auto text = vfs.read_whole_file(views, core::heap_allocator(core::MemoryTag::Tools));
        if (!text) {
            DE_LOG_ERROR("eyes", "--view: %s: %s", views, text.error().message);
            return false;
        }
        const core::Result<tools::CameraPose> view =
            tools::find_camera_view({reinterpret_cast<const char*>(text->data()), text->size()}, flags.view, views);
        if (!view) {
            return false; // logged
        }
        pose_ = *view;
    }
    if (pose_) {
        DE_LOG_INFO("eyes", "camera at %.6f %.6f %.6f, yaw %.3f, pitch %.3f degrees", pose_->position.x,
                    pose_->position.y, pose_->position.z, static_cast<double>(glm::degrees(pose_->yaw_rad)),
                    static_cast<double>(glm::degrees(pose_->pitch_rad)));
    }
    if (flags.screenshot != nullptr) {
        if (!writable_png(vfs, flags.screenshot, "--screenshot")) {
            return false;
        }
        last_frame_ = std::max(flags.screenshot_frame, flags.run_frames);
    }
    if (flags.path_file != nullptr) {
        auto text = vfs.read_whole_file(flags.path_file, core::heap_allocator(core::MemoryTag::Tools));
        if (!text) {
            DE_LOG_ERROR("eyes", "--camera-path: %s: %s", flags.path_file, text.error().message);
            return false;
        }
        auto path =
            tools::CameraPath::parse({reinterpret_cast<const char*>(text->data()), text->size()}, flags.path_file);
        if (!path) {
            return false; // logged
        }
        if (flags.path_speed_mps > 0.0) {
            path->set_speed_mps(flags.path_speed_mps);
        }
        const uint64_t flight = path->flight_frames(flags.fixed_step_s);
        if (flight > kMostFlightFrames) {
            DE_LOG_ERROR("eyes", "%s: %.1f m at %.1f m/s and %g s a frame would take %llu frames", flags.path_file,
                         path->length_m(), path->speed_mps(), flags.fixed_step_s,
                         static_cast<unsigned long long>(flight));
            return false;
        }
        if (flags.path_shots != nullptr) {
            platform::PathString first;
            if (!numbered_picture(flags.path_shots, 0, first) || !writable_png(vfs, first.c_str(), "--path-shots")) {
                return false;
            }
        }
        last_frame_ = flags.settle_frames + flight;
        DE_LOG_INFO("eyes", "%s: %.1f m at %.1f m/s, %llu frames after %u to settle", flags.path_file, path->length_m(),
                    path->speed_mps(), static_cast<unsigned long long>(flight), flags.settle_frames);
        path_.emplace(std::move(*path));
    }
    return true;
}

bool AgentEyes::wait_for_scene(platform::Window& window, platform::Input& input, asset::AssetRegistry& assets,
                               render::GpuAssets& gpu_assets, rhi::Uploader& uploader, rhi::Device& device) const {
    DE_PROFILE_ZONE("agent eyes wait for scene");
    const uint64_t start_ns = platform::time_ns();
    uint32_t frames = 0;
    while (true) {
        platform::pump_events(window, input); // the OS asks after a window that doesn't
        assets.wait_all();
        gpu_assets.update();
        if (assets.stats().loading == 0 && gpu_assets.stats().pending == 0 && uploader.idle()) {
            DE_LOG_INFO("eyes", "the scene is on the GPU after %u frames of uploads, %.2f s", frames,
                        static_cast<double>(platform::time_ns() - start_ns) * 1e-9);
            return true;
        }
        if (platform::time_ns() - start_ns > kMostSceneWaitNs || window.close_requested()) {
            DE_LOG_ERROR("eyes", "the scene didn't get to the GPU: %u assets loading, %u waiting for uploads",
                         assets.stats().loading, gpu_assets.stats().pending);
            return false;
        }
        device.begin_frame();
        uploader.record();
        device.end_frame();
        ++frames;
    }
}

const tools::CameraPose* AgentEyes::pose_at(uint64_t frame) {
    if (path_) {
        const uint64_t hold = flags_.settle_frames;
        pose_ = path_->at_flight_frame(frame > hold ? frame - hold - 1 : 0, flags_.fixed_step_s);
        return &*pose_;
    }
    // A script holds it there; otherwise it's placed once, and flown from there.
    return pose_ && (scripted() || frame == 1) ? &*pose_ : nullptr;
}

bool AgentEyes::picture_at(uint64_t frame, platform::PathString& path) const {
    if (flags_.screenshot != nullptr) {
        return frame == flags_.screenshot_frame && path.assign(flags_.screenshot);
    }
    if (flags_.path_shots == nullptr || frame <= flags_.settle_frames) {
        return false;
    }
    const uint64_t flight = frame - flags_.settle_frames - 1;
    return flight % flags_.path_every == 0 && numbered_picture(flags_.path_shots, flight / flags_.path_every, path);
}

const tools::CameraPose* AgentEyes::begin_frame() {
    DE_PROFILE_ZONE("agent eyes");
    ++frame_;
    return pose_at(frame_);
}

void AgentEyes::add_picture_pass(rhi::Device& device, render::RenderGraph& graph, render::GraphTexture back,
                                 rhi::TextureHandle backbuffer) {
    if (!takes_pictures() || !picture_at(frame_, picture_path_)) {
        return;
    }
    if (const core::Result<void> added = screenshot_.add_pass(device, graph, back, backbuffer); !added) {
        DE_LOG_ERROR("eyes", "%s: %s", picture_path_.c_str(), added.error().message);
        failed_ = true;
        return;
    }
    picture_due_ = true;
}

void AgentEyes::end_frame(rhi::Device& device, const platform::Vfs& vfs, bool drew) {
    if (scripted() && !drew) {
        DE_LOG_ERROR("eyes", "frame %llu wasn't drawn (the window minimized?): what comes after isn't repeatable",
                     static_cast<unsigned long long>(frame_));
        failed_ = true;
    }
    if (!picture_due_) {
        return;
    }
    picture_due_ = false;
    const auto png = screenshot_.take_png(device, core::heap_allocator(core::MemoryTag::Tools));
    if (!png) {
        DE_LOG_ERROR("eyes", "%s: %s", picture_path_.c_str(), png.error().message);
        failed_ = true;
        return;
    }
    if (const core::Result<void> written = vfs.write_file(picture_path_.c_str(), png->span()); !written) {
        DE_LOG_ERROR("eyes", "%s: %s", picture_path_.c_str(), written.error().message);
        failed_ = true;
        return;
    }
    const core::Result<platform::PathString> resolved = vfs.resolve(picture_path_.c_str());
    DE_LOG_INFO("eyes", "frame %llu: wrote %s (%s)", static_cast<unsigned long long>(frame_), picture_path_.c_str(),
                resolved ? resolved->c_str() : "?");
}

bool AgentEyes::finish(rhi::Device& device) {
    screenshot_.release(device);
    if (!failed_ && frame_ < last_frame_) {
        DE_LOG_ERROR("eyes", "the run ended at frame %llu, before its end at frame %llu",
                     static_cast<unsigned long long>(frame_), static_cast<unsigned long long>(last_frame_));
        failed_ = true;
    }
    return !failed_;
}

} // namespace de::app
