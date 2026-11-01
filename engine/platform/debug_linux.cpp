#include "platform/debug.h"

#include <fcntl.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <cstring>

namespace de::platform {

bool is_debugger_attached() {
    // A non-zero TracerPid in /proc/self/status means a debugger (ptrace) is attached.
    // Raw open/read into a stack buffer, no stdio: this runs on the fatal path, where the heap
    // may be what failed.
    const int file = ::open("/proc/self/status", O_RDONLY | O_CLOEXEC);
    if (file < 0) {
        return false;
    }
    char text[2048];
    const ssize_t length = ::read(file, text, sizeof(text) - 1);
    ::close(file);
    if (length <= 0) {
        return false;
    }
    text[length] = '\0';
    const char* tracer = std::strstr(text, "TracerPid:");
    return tracer != nullptr && std::strtol(tracer + 10, nullptr, 10) != 0;
}

void break_into_debugger() {
    if (is_debugger_attached()) {
        std::raise(SIGTRAP);
    }
}

core::LogSink* debugger_log_sink() {
    return nullptr; // Linux debuggers show stderr, where the console sink already writes
}

} // namespace de::platform
