#include "platform/imgui_platform.h"

#include "core/assert.h"
#include "platform/imgui_events.h"
#include "platform/window.h"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_video.h>
#include <imgui.h>

#include <cfloat>

namespace de::platform {

namespace {

struct ImGuiPlatformState {
    bool initialized = false;
    bool text_input = false;
    char* clipboard = nullptr; // the last text handed to ImGui, owned by SDL
};

ImGuiPlatformState g_imgui;

const char* get_clipboard(ImGuiContext* /*context*/) {
    if (g_imgui.clipboard != nullptr) {
        SDL_free(g_imgui.clipboard);
    }
    g_imgui.clipboard = SDL_GetClipboardText();
    return g_imgui.clipboard;
}

void set_clipboard(ImGuiContext* /*context*/, const char* text) {
    SDL_SetClipboardText(text);
}

// Letters and digits by key (the layout's meaning), the rest by position.
ImGuiKey to_imgui_key(SDL_Keycode key, SDL_Scancode scancode) {
    if (key >= SDLK_A && key <= SDLK_Z) {
        return static_cast<ImGuiKey>(ImGuiKey_A + static_cast<int>(key - SDLK_A));
    }
    if (key >= SDLK_0 && key <= SDLK_9) {
        return static_cast<ImGuiKey>(ImGuiKey_0 + static_cast<int>(key - SDLK_0));
    }
    if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12) {
        return static_cast<ImGuiKey>(ImGuiKey_F1 + static_cast<int>(scancode - SDL_SCANCODE_F1));
    }
    switch (scancode) {
    case SDL_SCANCODE_TAB:
        return ImGuiKey_Tab;
    case SDL_SCANCODE_LEFT:
        return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT:
        return ImGuiKey_RightArrow;
    case SDL_SCANCODE_UP:
        return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN:
        return ImGuiKey_DownArrow;
    case SDL_SCANCODE_PAGEUP:
        return ImGuiKey_PageUp;
    case SDL_SCANCODE_PAGEDOWN:
        return ImGuiKey_PageDown;
    case SDL_SCANCODE_HOME:
        return ImGuiKey_Home;
    case SDL_SCANCODE_END:
        return ImGuiKey_End;
    case SDL_SCANCODE_INSERT:
        return ImGuiKey_Insert;
    case SDL_SCANCODE_DELETE:
        return ImGuiKey_Delete;
    case SDL_SCANCODE_BACKSPACE:
        return ImGuiKey_Backspace;
    case SDL_SCANCODE_SPACE:
        return ImGuiKey_Space;
    case SDL_SCANCODE_RETURN:
        return ImGuiKey_Enter;
    case SDL_SCANCODE_KP_ENTER:
        return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_ESCAPE:
        return ImGuiKey_Escape;
    case SDL_SCANCODE_LCTRL:
        return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_RCTRL:
        return ImGuiKey_RightCtrl;
    case SDL_SCANCODE_LSHIFT:
        return ImGuiKey_LeftShift;
    case SDL_SCANCODE_RSHIFT:
        return ImGuiKey_RightShift;
    case SDL_SCANCODE_LALT:
        return ImGuiKey_LeftAlt;
    case SDL_SCANCODE_RALT:
        return ImGuiKey_RightAlt;
    case SDL_SCANCODE_LGUI:
        return ImGuiKey_LeftSuper;
    case SDL_SCANCODE_RGUI:
        return ImGuiKey_RightSuper;
    case SDL_SCANCODE_GRAVE:
        return ImGuiKey_GraveAccent;
    case SDL_SCANCODE_MINUS:
        return ImGuiKey_Minus;
    case SDL_SCANCODE_EQUALS:
        return ImGuiKey_Equal;
    case SDL_SCANCODE_LEFTBRACKET:
        return ImGuiKey_LeftBracket;
    case SDL_SCANCODE_RIGHTBRACKET:
        return ImGuiKey_RightBracket;
    case SDL_SCANCODE_BACKSLASH:
        return ImGuiKey_Backslash;
    case SDL_SCANCODE_SEMICOLON:
        return ImGuiKey_Semicolon;
    case SDL_SCANCODE_APOSTROPHE:
        return ImGuiKey_Apostrophe;
    case SDL_SCANCODE_COMMA:
        return ImGuiKey_Comma;
    case SDL_SCANCODE_PERIOD:
        return ImGuiKey_Period;
    case SDL_SCANCODE_SLASH:
        return ImGuiKey_Slash;
    default:
        return ImGuiKey_None;
    }
}

int to_imgui_button(uint8_t button) {
    switch (button) {
    case SDL_BUTTON_LEFT:
        return ImGuiMouseButton_Left;
    case SDL_BUTTON_RIGHT:
        return ImGuiMouseButton_Right;
    case SDL_BUTTON_MIDDLE:
        return ImGuiMouseButton_Middle;
    case SDL_BUTTON_X1:
        return 3;
    case SDL_BUTTON_X2:
        return 4;
    default:
        return -1;
    }
}

} // namespace

void imgui_platform_init(Window& window) {
    DE_ASSERT_MSG(ImGui::GetCurrentContext() != nullptr, "imgui_platform_init needs an ImGui context");
    DE_ASSERT_MSG(!g_imgui.initialized, "imgui_platform_init twice");
    static_cast<void>(window);
    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformName = "deengine_sdl3";
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    platform_io.Platform_GetClipboardTextFn = get_clipboard;
    platform_io.Platform_SetClipboardTextFn = set_clipboard;
    g_imgui = ImGuiPlatformState{.initialized = true};
}

void imgui_platform_shutdown() {
    if (!g_imgui.initialized) {
        return;
    }
    if (g_imgui.clipboard != nullptr) {
        SDL_free(g_imgui.clipboard);
    }
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui::GetIO().BackendPlatformName = nullptr;
        ImGui::GetPlatformIO().Platform_GetClipboardTextFn = nullptr;
        ImGui::GetPlatformIO().Platform_SetClipboardTextFn = nullptr;
    }
    g_imgui = ImGuiPlatformState{};
}

void imgui_platform_new_frame(Window& window, float delta_s) {
    DE_ASSERT_MSG(g_imgui.initialized, "imgui_platform_new_frame before imgui_platform_init");
    ImGuiIO& io = ImGui::GetIO();
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window.native_handle(), &width, &height);
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    if (width > 0 && height > 0) {
        io.DisplayFramebufferScale = ImVec2(static_cast<float>(window.pixel_width()) / static_cast<float>(width),
                                            static_cast<float>(window.pixel_height()) / static_cast<float>(height));
    }
    io.DeltaTime = delta_s > 0.0f ? delta_s : 1.0f / 60.0f;

    // Typed text arrives only while text input is on: only while a text field wants it, so the
    // OS input method doesn't pop up during play.
    if (io.WantTextInput != g_imgui.text_input) {
        g_imgui.text_input = io.WantTextInput;
        if (g_imgui.text_input) {
            SDL_StartTextInput(window.native_handle());
        } else {
            SDL_StopTextInput(window.native_handle());
        }
    }
}

void imgui_platform_event(const SDL_Event& event) {
    if (!g_imgui.initialized || ImGui::GetCurrentContext() == nullptr) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        io.AddMousePosEvent(event.motion.x, event.motion.y);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (const int button = to_imgui_button(event.button.button); button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        io.AddMouseWheelEvent(-event.wheel.x, event.wheel.y);
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        const SDL_Keymod mods = event.key.mod;
        io.AddKeyEvent(ImGuiMod_Ctrl, (mods & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (mods & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (mods & SDL_KMOD_ALT) != 0);
        io.AddKeyEvent(ImGuiMod_Super, (mods & SDL_KMOD_GUI) != 0);
        if (const ImGuiKey key = to_imgui_key(event.key.key, event.key.scancode); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        break;
    }
    case SDL_EVENT_TEXT_INPUT:
        io.AddInputCharactersUTF8(event.text.text);
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        io.AddFocusEvent(true);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        io.AddFocusEvent(false);
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        break;
    default:
        break;
    }
}

} // namespace de::platform
