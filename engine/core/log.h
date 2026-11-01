#pragma once

#include "core/compiler.h"

#include <cstdint>

namespace de::core {

enum class LogLevel : uint8_t {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Fatal,
};

const char* log_level_name(LogLevel level);

struct LogMessage {
    LogLevel level = LogLevel::Info;
    const char* channel = ""; // the system, as a string literal: "render", "streaming"
    const char* text = "";    // formatted; valid only during LogSink::write
    const char* file = "";
    int line = 0;
};

// Receives log messages. write() runs under the log lock, one message at a time, on the thread
// that logged. Error and Fatal messages are followed by flush() on every sink.
// Inside write(), a sink may add or remove sinks (itself included); the change applies from the
// next message. If a sink logs or asserts, that message goes straight to stderr instead.
class LogSink {
public:
    LogSink(const LogSink&) = delete;
    LogSink& operator=(const LogSink&) = delete;

    virtual void write(const LogMessage& message) = 0;
    virtual void flush() {}

protected:
    LogSink() = default;
    ~LogSink() = default;
    LogSink(LogSink&&) = default;
    LogSink& operator=(LogSink&&) = default;
};

// Logging is process-wide, like the heap: the platform layer adds a debugger sink and the VFS a
// file sink at startup. A sink must outlive its registration. At most 8 sinks.
void add_log_sink(LogSink& sink);
void remove_log_sink(LogSink& sink);

// Writes to stderr. Installed by default.
LogSink& console_log_sink();

// Messages below the minimum are dropped before they are formatted.
// Default: Debug in debug and dev builds, Info in ship builds.
void set_log_level(LogLevel minimum);
LogLevel log_level();

// Use the DE_LOG_* macros: they add the source location and compile out Trace and Debug in ship builds.
void log_message(LogLevel level, const char* channel, const char* file, int line, const char* format, ...)
    DE_PRINTF_FORMAT(5, 6);
[[noreturn]] void log_fatal(const char* channel, const char* file, int line, const char* format, ...)
    DE_PRINTF_FORMAT(4, 5);
void flush_log();

} // namespace de::core

#define DE_LOG(level, channel, ...) ::de::core::log_message((level), (channel), __FILE__, __LINE__, __VA_ARGS__)

#if DE_BUILD_DEV
#define DE_LOG_TRACE(channel, ...) DE_LOG(::de::core::LogLevel::Trace, channel, __VA_ARGS__)
#define DE_LOG_DEBUG(channel, ...) DE_LOG(::de::core::LogLevel::Debug, channel, __VA_ARGS__)
#else
// Compiled out, but the arguments are still type-checked and count as used.
#define DE_LOG_TRACE(channel, ...)                                                                                     \
    static_cast<void>(true || (DE_LOG(::de::core::LogLevel::Trace, channel, __VA_ARGS__), false))
#define DE_LOG_DEBUG(channel, ...)                                                                                     \
    static_cast<void>(true || (DE_LOG(::de::core::LogLevel::Debug, channel, __VA_ARGS__), false))
#endif

#define DE_LOG_INFO(channel, ...) DE_LOG(::de::core::LogLevel::Info, channel, __VA_ARGS__)
#define DE_LOG_WARN(channel, ...) DE_LOG(::de::core::LogLevel::Warning, channel, __VA_ARGS__)
#define DE_LOG_ERROR(channel, ...) DE_LOG(::de::core::LogLevel::Error, channel, __VA_ARGS__)

// Logs at Fatal level and aborts through the fatal handler, in every build.
#define DE_FATAL(channel, ...) ::de::core::log_fatal((channel), __FILE__, __LINE__, __VA_ARGS__)
