#include "core/log.h"

#include "core/assert.h"

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <utility>

namespace de::core {

namespace {

constexpr size_t kMaxSinks = 8;
constexpr size_t kMaxMessageLength = 2048; // including the terminator; longer messages end in "..."

class ConsoleSink final : public LogSink {
public:
    void write(const LogMessage& message) override {
        if (message.level >= LogLevel::Warning) {
            // file(line): first, so IDEs and terminals make warnings and errors clickable.
            std::fprintf(stderr, "%s(%d): [%s] %s: %s\n", message.file, message.line, log_level_name(message.level),
                         message.channel, message.text);
        } else {
            std::fprintf(stderr, "[%s] %s: %s\n", log_level_name(message.level), message.channel, message.text);
        }
    }

    void flush() override { std::fflush(stderr); }
};

struct LogState {
    std::mutex mutex;
    LogSink* sinks[kMaxSinks] = {};
    size_t sink_count = 0;
    std::atomic<LogLevel> minimum_level{DE_BUILD_DEV ? LogLevel::Debug : LogLevel::Info};
    ConsoleSink console;

    LogState() {
        sinks[0] = &console;
        sink_count = 1;
    }
};

// Never destroyed, so logging keeps working while other static objects are destroyed.
LogState& log_state() {
    alignas(LogState) static std::byte storage[sizeof(LogState)];
    static auto& state = *::new (static_cast<void*>(storage)) LogState();
    return state;
}

constinit thread_local bool writing_to_sinks = false;

void write_to_stderr(const LogMessage& message) {
    std::fprintf(stderr, "[%s] %s: %s\n", log_level_name(message.level), message.channel, message.text);
}

void dispatch(const LogMessage& message) {
    if (writing_to_sinks) {
        // A sink logged or asserted while writing: don't re-enter the lock.
        write_to_stderr(message);
        return;
    }
    LogState& state = log_state();
    writing_to_sinks = true;
    {
        const std::scoped_lock lock(state.mutex);
        for (size_t i = 0; i < state.sink_count; ++i) {
            state.sinks[i]->write(message);
        }
        if (message.level >= LogLevel::Error) {
            for (size_t i = 0; i < state.sink_count; ++i) {
                state.sinks[i]->flush();
            }
        }
    }
    writing_to_sinks = false;
}

void format_text(char* text, const char* format, va_list args) {
    const int written = std::vsnprintf(text, kMaxMessageLength, format, args);
    if (written < 0) {
        std::snprintf(text, kMaxMessageLength, "(invalid log format: %s)", format);
    } else if (std::cmp_greater_equal(written, kMaxMessageLength)) {
        std::memcpy(text + kMaxMessageLength - 4, "...", 4);
    }
}

} // namespace

const char* log_level_name(LogLevel level) {
    switch (level) {
    case LogLevel::Trace:
        return "trace";
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warning:
        return "warning";
    case LogLevel::Error:
        return "error";
    case LogLevel::Fatal:
        return "fatal";
    }
    return "unknown";
}

void add_log_sink(LogSink& sink) {
    LogState& state = log_state();
    bool added = false;
    {
        const std::scoped_lock lock(state.mutex);
        for (size_t i = 0; i < state.sink_count; ++i) {
            if (state.sinks[i] == &sink) {
                return;
            }
        }
        if (state.sink_count < kMaxSinks) {
            state.sinks[state.sink_count++] = &sink;
            added = true;
        }
    }
    // Checked outside the lock: a failure logs, and logging takes the lock.
    DE_VERIFY_MSG(added, "too many log sinks");
}

void remove_log_sink(LogSink& sink) {
    LogState& state = log_state();
    const std::scoped_lock lock(state.mutex);
    for (size_t i = 0; i < state.sink_count; ++i) {
        if (state.sinks[i] == &sink) {
            for (size_t j = i + 1; j < state.sink_count; ++j) {
                state.sinks[j - 1] = state.sinks[j];
            }
            --state.sink_count;
            state.sinks[state.sink_count] = nullptr;
            return;
        }
    }
}

LogSink& console_log_sink() {
    return log_state().console;
}

void set_log_level(LogLevel minimum) {
    log_state().minimum_level.store(minimum, std::memory_order_relaxed);
}

LogLevel log_level() {
    return log_state().minimum_level.load(std::memory_order_relaxed);
}

// NOLINTNEXTLINE(modernize-avoid-variadic-functions): printf-style by design, checked by DE_PRINTF_FORMAT
void log_message(LogLevel level, const char* channel, const char* file, int line, const char* format, ...) {
    if (level < log_level()) {
        return;
    }
    char text[kMaxMessageLength];
    va_list args;
    va_start(args, format);
    format_text(text, format, args);
    va_end(args);
    dispatch(LogMessage{.level = level, .channel = channel, .text = text, .file = file, .line = line});
}

// NOLINTNEXTLINE(modernize-avoid-variadic-functions): printf-style by design, checked by DE_PRINTF_FORMAT
void log_fatal(const char* channel, const char* file, int line, const char* format, ...) {
    char text[kMaxMessageLength];
    va_list args;
    va_start(args, format);
    format_text(text, format, args);
    va_end(args);
    dispatch(LogMessage{.level = LogLevel::Fatal, .channel = channel, .text = text, .file = file, .line = line});
    flush_log();
    abort_after_fatal_error();
}

void flush_log() {
    if (writing_to_sinks) {
        std::fflush(stderr);
        return;
    }
    LogState& state = log_state();
    const std::scoped_lock lock(state.mutex);
    for (size_t i = 0; i < state.sink_count; ++i) {
        state.sinks[i]->flush();
    }
}

} // namespace de::core
