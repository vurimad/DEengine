#pragma once

namespace de::core {
class LogSink;
} // namespace de::core

namespace de::platform {

bool is_debugger_attached();

// Breaks into the debugger if one is attached; does nothing otherwise. Installed as core's
// fatal handler by Platform, so a failed assert stops on the failing line under a debugger.
void break_into_debugger();

// A log sink for the debugger's output window (OutputDebugString on Windows), or null where
// debuggers already show stderr (Linux).
core::LogSink* debugger_log_sink();

} // namespace de::platform
