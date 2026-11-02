#include "platform/events.h"

#include "core/log.h"
#include "core/profile.h"
#include "platform/imgui_events.h"
#include "platform/input.h"
#include "platform/window.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_video.h>

#include <cstdint>

namespace de::platform {

namespace {

// SDL reports gamepad axes as Sint16.
constexpr float kAxisRange = 32767.0f;

bool is_window_event(uint32_t type) {
    return type >= SDL_EVENT_WINDOW_FIRST && type <= SDL_EVENT_WINDOW_LAST;
}

} // namespace

void pump_events(Window& window, Input& input) {
    DE_PROFILE_ZONE("pump_events");
    window.resized_ = false;
    bool window_changed = false;

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // Other SDL windows (tools, message boxes) must not change this window's state.
        if (is_window_event(event.type) && event.window.windowID != window.window_id_) {
            continue;
        }
        imgui_platform_event(event); // debug UI, when it's on
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            window.close_requested_ = true;
            break;

        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_SHOWN:
        case SDL_EVENT_WINDOW_HIDDEN:
            window_changed = true;
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            window.focused_ = true;
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            window.focused_ = false;
            input.release_all(event.window.timestamp);
            break;

        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            if (!event.key.repeat) {
                input.push(RawInput{.source = InputSource::Key,
                                    .code = static_cast<int32_t>(event.key.scancode),
                                    .value = event.key.down ? 1.0f : 0.0f,
                                    .timestamp_ns = event.key.timestamp});
            }
            break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            input.push(RawInput{.source = InputSource::MouseButton,
                                .code = event.button.button,
                                .value = event.button.down ? 1.0f : 0.0f,
                                .timestamp_ns = event.button.timestamp});
            break;
        case SDL_EVENT_MOUSE_MOTION:
            input.push(RawInput{.source = InputSource::MouseMotionX,
                                .value = event.motion.xrel,
                                .timestamp_ns = event.motion.timestamp});
            input.push(RawInput{.source = InputSource::MouseMotionY,
                                .value = event.motion.yrel,
                                .timestamp_ns = event.motion.timestamp});
            break;

        case SDL_EVENT_GAMEPAD_ADDED:
            if (SDL_Gamepad* gamepad = SDL_OpenGamepad(event.gdevice.which)) {
                DE_LOG_INFO("input", "gamepad connected: %s", SDL_GetGamepadName(gamepad));
            }
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            DE_LOG_INFO("input", "gamepad disconnected");
            SDL_CloseGamepad(SDL_GetGamepadFromID(event.gdevice.which));
            input.remove_gamepad(event.gdevice.which, event.gdevice.timestamp);
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            input.push(RawInput{.source = InputSource::GamepadButton,
                                .code = event.gbutton.button,
                                .value = event.gbutton.down ? 1.0f : 0.0f,
                                .timestamp_ns = event.gbutton.timestamp,
                                .device = event.gbutton.which});
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            input.push(RawInput{.source = InputSource::GamepadAxis,
                                .code = event.gaxis.axis,
                                .value = static_cast<float>(event.gaxis.value) / kAxisRange,
                                .timestamp_ns = event.gaxis.timestamp,
                                .device = event.gaxis.which});
            break;

        default:
            break;
        }
    }

    if (window_changed) {
        // Read the state back from SDL rather than tracking event pairs: maximize, minimize and
        // restore don't always arrive in matching pairs (minimized from maximized comes back as
        // MAXIMIZED, not RESTORED).
        window.minimized_ = (SDL_GetWindowFlags(window.window_) & SDL_WINDOW_MINIMIZED) != 0;
        window.refresh_pixel_size();
    }
}

} // namespace de::platform
