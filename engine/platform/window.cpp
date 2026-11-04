#include "platform/window.h"

#include "core/log.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_vulkan.h>

#include <utility>

namespace de::platform {

core::Result<Window> Window::create(const WindowDesc& desc) {
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (desc.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    if (desc.fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN;
    }
    if (desc.vulkan_surface) {
        flags |= SDL_WINDOW_VULKAN;
    }

    SDL_Window* window =
        SDL_CreateWindow(desc.title, static_cast<int>(desc.width_points), static_cast<int>(desc.height_points), flags);
    if (window == nullptr) {
        DE_LOG_ERROR("platform", "SDL_CreateWindow failed: %s", SDL_GetError());
        return core::fail(core::ErrorCode::Unsupported, "the window could not be created");
    }
    return Window(window);
}

Window::Window(SDL_Window* window) : window_(window), window_id_(SDL_GetWindowID(window)) {
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    minimized_ = (flags & SDL_WINDOW_MINIMIZED) != 0;
    focused_ = (flags & SDL_WINDOW_INPUT_FOCUS) != 0;
    refresh_pixel_size();
    refresh_display();
    resized_ = false;
}

Window::~Window() {
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
    }
}

Window::Window(Window&& other) noexcept
    : window_(std::exchange(other.window_, nullptr)), window_id_(other.window_id_), pixel_width_(other.pixel_width_),
      pixel_height_(other.pixel_height_), refresh_hz_(other.refresh_hz_), close_requested_(other.close_requested_),
      minimized_(other.minimized_), focused_(other.focused_), resized_(other.resized_) {}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        if (window_ != nullptr) {
            SDL_DestroyWindow(window_);
        }
        window_ = std::exchange(other.window_, nullptr);
        window_id_ = other.window_id_;
        pixel_width_ = other.pixel_width_;
        pixel_height_ = other.pixel_height_;
        refresh_hz_ = other.refresh_hz_;
        close_requested_ = other.close_requested_;
        minimized_ = other.minimized_;
        focused_ = other.focused_;
        resized_ = other.resized_;
    }
    return *this;
}

void Window::set_title(const char* title) {
    SDL_SetWindowTitle(window_, title);
}

void Window::refresh_display() {
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window_));
    refresh_hz_ = mode != nullptr ? static_cast<double>(mode->refresh_rate) : 0.0;
}

void Window::refresh_pixel_size() {
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window_, &width, &height);
    const auto new_width = static_cast<uint32_t>(width);
    const auto new_height = static_cast<uint32_t>(height);
    resized_ = resized_ || new_width != pixel_width_ || new_height != pixel_height_;
    pixel_width_ = new_width;
    pixel_height_ = new_height;
}

std::span<const char* const> vulkan_instance_extensions() {
    uint32_t count = 0;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    if (extensions == nullptr) {
        DE_LOG_ERROR("platform", "SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
        return {};
    }
    return {extensions, count};
}

core::Result<uint64_t> create_vulkan_surface(const Window& window, void* vk_instance) {
    VkSurfaceKHR surface{}; // SDL declares the handle type without Vulkan's headers (no VK_NULL_HANDLE)
    if (!SDL_Vulkan_CreateSurface(window.native_handle(), static_cast<VkInstance>(vk_instance), nullptr, &surface)) {
        DE_LOG_ERROR("platform", "SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        return core::fail(core::ErrorCode::Unsupported, "the window's Vulkan surface could not be created");
    }
    static_assert(sizeof(VkSurfaceKHR) <= sizeof(uint64_t));
    return reinterpret_cast<uint64_t>(surface);
}

} // namespace de::platform
