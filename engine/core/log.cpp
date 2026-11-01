#include "core/log.h"

#include "core/assert.h"

#include <algorithm>
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

// True while this thread holds the log lock and is calling sinks.
constinit thread_local bool writing_to_sinks = false;

// Fallback when a sink logs or asserts while writing: the lock is already held.
void write_to_stderr(const LogMessage& message) {
    std::fprintf(stderr, "[%s] %s: %s\n", log_level_name(message.level), message.channel, message.text);
    if (message.level >= LogLevel::Error) {
        std::fflush(stderr);
    }
}

void dispatch(const LogMessage& message) {
    if (writing_to_sinks) {
        write_to_stderr(message);
        return;
    }
    LogState& state = log_state();
    const std::scoped_lock lock(state.mutex);
    writing_to_sinks = true;

    // Iterate a copy: a sink may add or remove sinks (itself included) from inside write().
    LogSink* sinks[kMaxSinks];
    const size_t sink_count = state.sink_count;
    std::copy_n(state.sinks, sink_count, sinks);

    for (size_t i = 0; i < sink_count; ++i) {
        sinks[i]->write(message);
    }
    if (message.level >= LogLevel::Error) {
        for (size_t i = 0; i < sink_count; ++i) {
            sinks[i]->flush();
        }
    }
    writing_to_sinks = false;
}

void format_and_dispatch(LogLevel level, const char* channel, const char* file, int line, const char* format,
                         va_list args) {
    char text[kMaxMessageLength];
    const int written = std::vsnprintf(text, kMaxMessageLength, format, args);
    if (written < 0) {
        std::snprintf(text, kMaxMessageLength, "(invalid log format: %s)", format);
    } else if (std::cmp_greater_equal(written, kMaxMessageLength)) {
        std::memcpy(text + kMaxMessageLength - 4, "...", 4);
    }
    dispatch(LogMessage{.level = level, .channel = channel, .text = text, .file = file, .line = line});
}

// Sink list edits. The caller holds the log lock, either directly or because it runs inside a
// sink's write() on the thread that holds it.
bool insert_sink(LogState& state, LogSink& sink) {
    for (size_t i = 0; i < state.sink_count; ++i) {
        if (state.sinks[i] == &sink) {
            return true;
        }
    }
    if (state.sink_count == kMaxSinks) {
        return false;
    }
    state.sinks[state.sink_count++] = &sink;
    return true;
}

void erase_sink(LogState& state, LogSink& sink) {
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
    if (writing_to_sinks) {
        added = insert_sink(state, sink); // called from a sink's write(): this thread holds the lock
    } else {
        const std::scoped_lock lock(state.mutex);
        added = insert_sink(state, sink);
    }
    // Checked outside the lock: a failure logs, and logging takes the lock.
    DE_VERIFY_MSG(added, "too many log sinks");
}

void remove_log_sink(LogSink& sink) {
    LogState& state = log_state();
    if (writing_to_sinks) {
        erase_sink(state, sink); // called from a sink's write(): this thread holds the lock
        return;
    }
    const std::scoped_lock lock(state.mutex);
    erase_sink(state, sink);
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
    va_list args;
    va_start(args, format);
    format_and_dispatch(level, channel, file, line, format, args);
    va_end(args);
}

// NOLINTNEXTLINE(modernize-avoid-variadic-functions): printf-style by design, checked by DE_PRINTF_FORMAT
void log_fatal(const char* channel, const char* file, int line, const char* format, ...) {
    va_list args;
    va_start(args, format);
    format_and_dispatch(LogLevel::Fatal, channel, file, line, format, args);
    va_end(args);
    abort_after_fatal_error(); // dispatch already flushed every sink: Fatal is above Error
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
