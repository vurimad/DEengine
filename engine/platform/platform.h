#pragma once

#include "core/result.h"

namespace de::platform {

struct PlatformDesc {
    bool video = true;   // windows and window events; off for headless runs and tools
    bool gamepad = true; // gamepad hot-plug and input
};

// Owns the OS layer for the process: SDL, and the hooks that send core's fatal errors and logs to
// the debugger (a failed assert breaks on its line; logs appear in the debugger's output window).
// Create one at startup before any window, and destroy it last. One per process.
class Platform {
public:
    static core::Result<Platform> create(const PlatformDesc& desc = {});

    ~Platform();
    Platform(Platform&& other) noexcept;
    Platform& operator=(Platform&& other) noexcept;
    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;

private:
    Platform() = default;
    void shutdown();

    bool owns_platform_ = false;
};

} // namespace de::platform
