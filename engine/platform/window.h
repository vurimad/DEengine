#pragma once

#include "core/result.h"

#include <cstdint>

struct SDL_Window;

namespace de::platform {

class Input;

struct WindowDesc {
    const char* title = "DEengine";
    uint32_t width = 1600; // in screen coordinates; the drawable size in pixels may be larger (high DPI)
    uint32_t height = 900;
    bool resizable = true;
    bool fullscreen = false;
    bool vulkan_surface = true; // the window will present through a Vulkan swapchain
};

// The game window. Its state is refreshed by pump_events() once per rendered frame.
// Move-only: it owns the OS window.
class Window {
public:
    static core::Result<Window> create(const WindowDesc& desc = {});

    ~Window();
    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // The user asked to close the window (or quit the app).
    bool close_requested() const { return close_requested_; }
    bool minimized() const { return minimized_; }
    bool focused() const { return focused_; }

    // The drawable size in pixels: what the swapchain must match.
    uint32_t pixel_width() const { return pixel_width_; }
    uint32_t pixel_height() const { return pixel_height_; }

    // True during the frame in which the drawable size changed: recreate the swapchain.
    bool resized() const { return resized_; }

    void set_title(const char* title);

    // The SDL window, for the RHI backend's surface creation only.
    SDL_Window* native_handle() const { return window_; }

private:
    friend void pump_events(Window& window, Input& input);

    explicit Window(SDL_Window* window);
    void refresh_pixel_size();

    SDL_Window* window_ = nullptr;
    uint32_t window_id_ = 0;
    uint32_t pixel_width_ = 0;
    uint32_t pixel_height_ = 0;
    bool close_requested_ = false;
    bool minimized_ = false;
    bool focused_ = true;
    bool resized_ = false;
};

} // namespace de::platform
