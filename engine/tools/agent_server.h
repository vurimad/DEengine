#pragma once

#include "core/array.h"
#include "core/result.h"
#include "platform/socket.h"

#include <cstdint>
#include <string_view>

namespace de::console {
class Console;
}

namespace de::tools {

// The engine's door for agents (docs/design.md, AI-friendly: an engine server for agents), dev builds: JSON-RPC 2.0
// over a TCP socket on 127.0.0.1 only, a request a line and its answer a line. Its methods run the console's commands,
// the same text a person types in the console window:
//   console.exec      {"line": "stat gpu; r.taa 0"}  ->  {"ok": true, "output": ["stat gpu: on", ...]}
//   console.complete  {"prefix": "r."}               ->  [{"name": "r.taa", "usage": "[on | off]", "help": "..."}, ...]
// tools/console.py sends one from a shell. poll() runs on the main thread, once a frame, and never blocks; with nobody
// connected it costs one accept() that finds no one.
inline constexpr uint16_t kAgentPort = 37017;
inline constexpr uint16_t kAgentPortsTried = 8; // kAgentPort and the next ones, while other engines hold them
inline constexpr size_t kMaxAgentConnections = 4;
inline constexpr size_t kMaxAgentRequestBytes = size_t{64} * 1024;

class AgentServer {
public:
    // On the first port of kAgentPortsTried from first_port that's free.
    static core::Result<AgentServer> start(uint16_t first_port = kAgentPort);
    uint16_t port() const { return listener_.port(); }
    void poll(console::Console& console);

private:
    struct Connection {
        platform::TcpStream stream;
        core::Array<char> in{core::heap_allocator(core::MemoryTag::Tools)};
        core::Array<char> out{core::heap_allocator(core::MemoryTag::Tools)};
    };
    void serve(Connection& connection, console::Console& console);

    platform::TcpListener listener_;
    Connection connections_[kMaxAgentConnections];
};

// The answer to one request (a line of JSON, without its newline), appended to response: a JSON-RPC response, or
// nothing for a notification (a request with no id).
void answer_request(console::Console& console, std::string_view request, core::Array<char>& response);

} // namespace de::tools
