#include "platform/events.h"

#include "core/log.h"
#include "platform/input.h"
#include "platform/window.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>

namespace de::platform {

namespace {

// SDL reports gamepad axes as Sint16.
constexpr float kAxisRange = 32767.0f;

} // namespace

void pump_events(Window& window, Input& input) {
    window.resized_ = false;
    bool refresh_size = false;

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
            window.close_requested_ = true;
            break;

        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (event.window.windowID == window.window_id_) {
                window.close_requested_ = true;
            }
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            refresh_size = true;
            break;
        case SDL_EVENT_WINDOW_MINIMIZED:
            window.minimized_ = true;
            break;
        case SDL_EVENT_WINDOW_RESTORED:
            window.minimized_ = false;
            refresh_size = true;
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
            input.release_all(event.gdevice.timestamp);
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            input.push(RawInput{.source = InputSource::GamepadButton,
                                .code = event.gbutton.button,
                                .value = event.gbutton.down ? 1.0f : 0.0f,
                                .timestamp_ns = event.gbutton.timestamp});
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            input.push(RawInput{.source = InputSource::GamepadAxis,
                                .code = event.gaxis.axis,
                                .value = static_cast<float>(event.gaxis.value) / kAxisRange,
                                .timestamp_ns = event.gaxis.timestamp});
            break;

        default:
            break;
        }
    }

    if (refresh_size) {
        window.refresh_pixel_size();
    }
}

} // namespace de::platform
