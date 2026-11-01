#include "core/assert.h"

#include "core/log.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace de::core {

namespace {

constinit std::atomic<FatalHandler> fatal_handler{nullptr};
constinit thread_local bool reporting_failure = false;

} // namespace

void set_fatal_handler(FatalHandler handler) {
    fatal_handler.store(handler);
}

void abort_after_fatal_error() {
    if (const FatalHandler handler = fatal_handler.load()) {
        handler();
    }
    std::abort();
}

void assert_failed(const char* condition, const char* message, const char* file, int line) {
    if (reporting_failure) {
        // A failure while reporting a failure: skip the log, which may be what is failing.
        std::fprintf(stderr, "%s(%d): assertion failed while reporting a failure: %s\n", file, line, condition);
        std::abort();
    }
    reporting_failure = true;
    log_message(LogLevel::Fatal, "assert", file, line, "assertion failed: %s%s%s", condition,
                message != nullptr ? " -- " : "", message != nullptr ? message : "");
    flush_log();
    abort_after_fatal_error();
}

} // namespace de::core
