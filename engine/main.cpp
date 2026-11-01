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

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>

using namespace de::core::literals;
using de::platform::ActionId;
using de::platform::ActionKind;
using de::platform::Input;

namespace {

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

// Placeholder loop until the frame loop lands (M0b): opens the window, pumps events, samples input
// and logs actions, so the platform layer can be tried by hand.
//   --frames N       exit after N frames (smoke runs)
//   --game-dir DIR   folder mounted as game:/ (default ./cooked)
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
    const de::core::JobSystem jobs(de::core::JobSystemDesc{
        .on_worker_start = [](uint32_t /*index*/, const char* name) { de::platform::set_current_thread_name(name); }});
    DE_LOG_INFO("engine", "job system: %u workers", jobs.worker_count());

    // game:/ is the cooked data folder (the cooker arrives in M0b): ./cooked unless --game-dir says otherwise.
    const char* game_dir = flag_value(argc, argv, "--game-dir");
    auto vfs = de::platform::Vfs::create({.game_root = game_dir != nullptr ? game_dir : "cooked"});
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

    const char* frames_flag = flag_value(argc, argv, "--frames");
    const uint64_t max_frames = frames_flag != nullptr ? std::strtoull(frames_flag, nullptr, 10) : 0;
    uint64_t next_stick_report_ns = 0;
    uint64_t frame = 0;
    bool was_moving = false;
    bool was_minimized = false;
    while (!window->close_requested()) {
        const uint64_t frame_start_allocations = de::core::heap_allocation_count(); // every thread, jobs included
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

        // The "zero heap allocations per frame" rule, made visible in the profiler.
        DE_PROFILE_PLOT("heap allocations/frame",
                        static_cast<int64_t>(de::core::heap_allocation_count() - frame_start_allocations));
        DE_PROFILE_FRAME();

        ++frame;
        if (max_frames != 0 && frame >= max_frames) {
            break;
        }
        de::platform::sleep_ns(de::platform::kNsPerMs); // nothing renders yet: don't spin a core
    }

    DE_LOG_INFO("engine", "shutting down after %llu frames", static_cast<unsigned long long>(frame));
    return EXIT_SUCCESS;
}
