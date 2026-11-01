#include "platform/platform.h"

#include "core/assert.h"
#include "core/log.h"
#include "platform/debug.h"
#include "platform/thread.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_version.h>

#include <atomic>
#include <utility>

namespace de::platform {

namespace {

constinit std::atomic<bool> platform_alive{false};
constinit core::LogSink* installed_debugger_sink = nullptr;

} // namespace

core::Result<Platform> Platform::create(const PlatformDesc& desc) {
    DE_VERIFY_MSG(!platform_alive.exchange(true), "only one Platform may exist at a time");

    SDL_InitFlags flags = 0;
    if (desc.video) {
        flags |= SDL_INIT_VIDEO;
    }
    if (desc.gamepad) {
        flags |= SDL_INIT_GAMEPAD;
    }
    if (!SDL_Init(flags)) {
        DE_LOG_ERROR("platform", "SDL_Init failed: %s", SDL_GetError());
        platform_alive.store(false);
        return core::fail(core::ErrorCode::Unsupported, "the platform layer could not be initialized");
    }

    core::set_fatal_handler(&break_into_debugger);
    // Registered whether or not a debugger is attached yet: it writes only while one is.
    installed_debugger_sink = debugger_log_sink();
    if (installed_debugger_sink != nullptr) {
        core::add_log_sink(*installed_debugger_sink);
    }

    const int version = SDL_GetVersion();
    const bool debugger = is_debugger_attached();
    DE_LOG_INFO("platform", "SDL %d.%d.%d, %u hardware threads%s", SDL_VERSIONNUM_MAJOR(version),
                SDL_VERSIONNUM_MINOR(version), SDL_VERSIONNUM_MICRO(version), hardware_thread_count(),
                debugger ? ", debugger attached" : "");

    Platform platform;
    platform.owns_platform_ = true;
    return platform;
}

Platform::~Platform() {
    shutdown();
}

Platform::Platform(Platform&& other) noexcept : owns_platform_(std::exchange(other.owns_platform_, false)) {}

Platform& Platform::operator=(Platform&& other) noexcept {
    if (this != &other) {
        shutdown();
        owns_platform_ = std::exchange(other.owns_platform_, false);
    }
    return *this;
}

void Platform::shutdown() {
    if (!owns_platform_) {
        return;
    }
    owns_platform_ = false;
    if (installed_debugger_sink != nullptr) {
        core::remove_log_sink(*installed_debugger_sink);
        installed_debugger_sink = nullptr;
    }
    core::set_fatal_handler(nullptr);
    SDL_Quit();
    platform_alive.store(false);
}

} // namespace de::platform
