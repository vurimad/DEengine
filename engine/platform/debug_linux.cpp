#include "platform/debug.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace de::platform {

bool is_debugger_attached() {
    // A non-zero TracerPid in /proc/self/status means a debugger (ptrace) is attached.
    FILE* status = std::fopen("/proc/self/status", "r");
    if (status == nullptr) {
        return false;
    }
    char line[256];
    bool attached = false;
    while (std::fgets(line, sizeof(line), status) != nullptr) {
        if (std::strncmp(line, "TracerPid:", 10) == 0) {
            attached = std::strtol(line + 10, nullptr, 10) != 0;
            break;
        }
    }
    std::fclose(status);
    return attached;
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
