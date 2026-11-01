#include "core/log.h"

#include <doctest/doctest.h>

#include <cstdio>
#include <cstring>

using de::core::LogLevel;
using de::core::LogMessage;
using de::core::LogSink;

namespace {

// Captures messages for inspection and keeps the console quiet while it is installed.
class CaptureSink final : public LogSink {
public:
    CaptureSink() {
        de::core::remove_log_sink(de::core::console_log_sink());
        de::core::add_log_sink(*this);
    }
    ~CaptureSink() {
        de::core::remove_log_sink(*this);
        de::core::add_log_sink(de::core::console_log_sink());
    }
    CaptureSink(CaptureSink&&) = delete;
    CaptureSink& operator=(CaptureSink&&) = delete;

    void write(const LogMessage& message) override {
        ++count;
        level = message.level;
        channel = message.channel;
        line = message.line;
        std::snprintf(text, sizeof(text), "%s", message.text); // message.text dies after write()
    }

    int count = 0;
    LogLevel level = LogLevel::Trace;
    const char* channel = "";
    int line = 0;
    char text[4096] = {};
};

// Restores the minimum log level when a test changes it.
class ScopedLogLevel {
public:
    explicit ScopedLogLevel(LogLevel level) : saved_(de::core::log_level()) { de::core::set_log_level(level); }
    ~ScopedLogLevel() { de::core::set_log_level(saved_); }
    ScopedLogLevel(const ScopedLogLevel&) = delete;
    ScopedLogLevel& operator=(const ScopedLogLevel&) = delete;
    ScopedLogLevel(ScopedLogLevel&&) = delete;
    ScopedLogLevel& operator=(ScopedLogLevel&&) = delete;

private:
    LogLevel saved_;
};

} // namespace

TEST_CASE("formats messages and routes them to sinks") {
    CaptureSink sink; // NOLINT(misc-const-correctness): the log writes into it
    DE_LOG_INFO("render", "loaded %d meshes in %.1f ms", 12, 3.5);
    REQUIRE(sink.count == 1);
    CHECK(std::strcmp(sink.text, "loaded 12 meshes in 3.5 ms") == 0);
    CHECK(std::strcmp(sink.channel, "render") == 0);
    CHECK(sink.level == LogLevel::Info);
    CHECK(sink.line > 0);
}

TEST_CASE("messages below the minimum level are dropped") {
    CaptureSink sink; // NOLINT(misc-const-correctness): the log writes into it
    const ScopedLogLevel level(LogLevel::Warning);
    DE_LOG_INFO("test", "dropped");
    DE_LOG_WARN("test", "kept");
    DE_LOG_ERROR("test", "kept too");
    CHECK(sink.count == 2);
    CHECK(sink.level == LogLevel::Error);
}

TEST_CASE("trace and debug exist only in debug and dev builds") {
    CaptureSink sink; // NOLINT(misc-const-correctness): the log writes into it
    const ScopedLogLevel level(LogLevel::Trace);
    DE_LOG_TRACE("test", "trace %d", 1);
    DE_LOG_DEBUG("test", "debug %d", 2);
    CHECK(sink.count == (DE_BUILD_DEV ? 2 : 0));
}

TEST_CASE("long messages are truncated with an ellipsis") {
    CaptureSink sink; // NOLINT(misc-const-correctness): the log writes into it
    char long_text[5000];
    std::memset(long_text, 'a', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = '\0';
    DE_LOG_INFO("test", "%s", long_text);
    const size_t length = std::strlen(sink.text);
    CHECK(length == 2047);
    CHECK(std::strcmp(sink.text + length - 3, "...") == 0);
}

TEST_CASE("removed sinks receive nothing") {
    CaptureSink sink; // NOLINT(misc-const-correctness): the log writes into it
    de::core::remove_log_sink(sink);
    DE_LOG_INFO("test", "nobody listens");
    CHECK(sink.count == 0);
    de::core::add_log_sink(sink); // the destructor removes it again
}

TEST_CASE("a sink can remove itself from inside write()") {
    class OneShotSink final : public LogSink {
    public:
        void write(const LogMessage& /*message*/) override {
            ++count;
            de::core::remove_log_sink(*this); // e.g. a file sink disabling itself after an IO error
        }
        int count = 0;
    };

    CaptureSink capture; // NOLINT(misc-const-correctness): the log writes into it
    OneShotSink once;
    de::core::add_log_sink(once);
    DE_LOG_INFO("test", "first");
    DE_LOG_INFO("test", "second");
    CHECK(once.count == 1);
    CHECK(capture.count == 2);
}

TEST_CASE("a sink can add another sink from inside write(); it hears from the next message") {
    class LateSink final : public LogSink {
    public:
        void write(const LogMessage& /*message*/) override { ++count; }
        int count = 0;
    };
    class InstallerSink final : public LogSink {
    public:
        explicit InstallerSink(LateSink& late) : late_(&late) {}
        void write(const LogMessage& /*message*/) override { de::core::add_log_sink(*late_); }

    private:
        LateSink* late_;
    };

    CaptureSink capture; // NOLINT(misc-const-correctness): the log writes into it
    LateSink late;
    InstallerSink installer(late);
    de::core::add_log_sink(installer);
    DE_LOG_INFO("test", "installs the late sink");
    CHECK(late.count == 0);
    DE_LOG_INFO("test", "reaches the late sink");
    CHECK(late.count == 1);
    de::core::remove_log_sink(installer);
    de::core::remove_log_sink(late);
    CHECK(capture.count == 2);
}

TEST_CASE("a sink that logs doesn't deadlock") {
    class EchoSink final : public LogSink {
    public:
        void write(const LogMessage& /*message*/) override {
            ++count;
            DE_LOG_INFO("test", "logged from inside a sink"); // goes straight to stderr
        }
        int count = 0;
    };

    CaptureSink quiet; // NOLINT(misc-const-correctness): keeps the console quiet; the log writes into it
    EchoSink echo;
    de::core::add_log_sink(echo);
    DE_LOG_INFO("test", "outer");
    de::core::remove_log_sink(echo);
    CHECK(echo.count == 1);
    CHECK(quiet.count == 1);
}
