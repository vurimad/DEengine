#include "platform/debug.h"

#include "core/log.h"

#include <windows.h>

#include <cstdio>

namespace de::platform {

namespace {

// Writes "file(line): [level] channel: text" lines, so double-clicking one in the Visual Studio
// output window jumps to the source.
class DebuggerSink final : public core::LogSink {
public:
    void write(const core::LogMessage& message) override {
        char line[2304];
        std::snprintf(line, sizeof(line), "%s(%d): [%s] %s: %s\n", message.file, message.line,
                      core::log_level_name(message.level), message.channel, message.text);
        OutputDebugStringA(line);
    }
};

DebuggerSink debugger_sink;

} // namespace

bool is_debugger_attached() {
    return IsDebuggerPresent() != 0;
}

void break_into_debugger() {
    if (IsDebuggerPresent() != 0) {
        DebugBreak();
    }
}

core::LogSink* debugger_log_sink() {
    return &debugger_sink;
}

} // namespace de::platform
