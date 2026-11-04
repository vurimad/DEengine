#pragma once

#include "core/result.h"

#include <cstdint>
#include <span>

struct SDL_Window;

namespace de::platform {

class Input;

struct WindowDesc {
    const char* title = "DEengine";
    // In points (screen coordinates). On high-DPI displays the drawable size in pixels is larger:
    // read Window::pixel_width() / pixel_height() for the swapchain.
    uint32_t width_points = 1600;
    uint32_t height_points = 900;
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
    // The refresh rate of the display the window is on; 0 if it isn't known.
    double refresh_hz() const;
    uint32_t pixel_height() const { return pixel_height_; }

    // True during the frame in which the drawable size changed: recreate the swapchain.
    bool resized() const { return resized_; }

    void set_title(const char* title);

    // The SDL window. Engine code doesn't need it: the RHI backend creates its surface through
    // create_vulkan_surface below.
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

// --- Vulkan surface support, for the RHI's Vulkan backend ---
// Vulkan handles cross this boundary as plain pointers and integers, so neither side includes the
// other's headers: SDL stays in platform/, Vulkan in render/rhi/vulkan/.

// The instance extensions windows need for presentation (VK_KHR_surface plus the OS one).
// Empty if video isn't initialized or Vulkan isn't available.
std::span<const char* const> vulkan_instance_extensions();

// Creates a VkSurfaceKHR for the window on the given VkInstance and returns it as an integer.
// The caller destroys it with vkDestroySurfaceKHR before the window goes away.
core::Result<uint64_t> create_vulkan_surface(const Window& window, void* vk_instance);

} // namespace de::platform
