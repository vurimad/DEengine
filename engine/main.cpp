#include "core/log.h"
#include "core/string_id.h"
#include "core/version.h"
#include "platform/events.h"
#include "platform/input.h"
#include "platform/platform.h"
#include "platform/time.h"
#include "platform/window.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

using namespace de::core::literals;
using de::platform::ActionId;
using de::platform::ActionKind;
using de::platform::Input;

namespace {

// --frames N: exit after N frames (smoke runs); 0 runs until the window closes.
uint64_t frame_limit(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0) {
            return std::strtoull(argv[i + 1], nullptr, 10);
        }
    }
    return 0;
}

} // namespace

// Placeholder loop until the frame loop lands (M0b): opens the window, pumps events, samples input
// and logs actions, so the platform layer can be tried by hand.
int main(int argc, char** argv) {
    DE_LOG_INFO("engine", "DEengine %s", de::core::engine_version_string());

    auto platform = de::platform::Platform::create();
    if (!platform) {
        return EXIT_FAILURE;
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

    const uint64_t max_frames = frame_limit(argc, argv);
    uint64_t frame = 0;
    bool was_moving = false;
    bool was_minimized = false;
    while (!window->close_requested()) {
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

        ++frame;
        if (max_frames != 0 && frame >= max_frames) {
            break;
        }
        de::platform::sleep_ns(de::platform::kNsPerMs); // nothing renders yet: don't spin a core
    }

    DE_LOG_INFO("engine", "shutting down after %llu frames", static_cast<unsigned long long>(frame));
    return EXIT_SUCCESS;
}
