#include "tools/agent_server.h"

#include "console/console.h"
#include "core/allocator.h"
#include "core/log.h"
#include "core/profile.h"

#include <yyjson.h>

#include <algorithm>
#include <cstring>

namespace de::tools {

namespace {

void* json_malloc(void* /*context*/, size_t size) {
    return core::tagged_malloc(core::MemoryTag::Tools, size);
}

void* json_realloc(void* /*context*/, void* pointer, size_t /*old_size*/, size_t size) {
    return core::tagged_realloc(core::MemoryTag::Tools, pointer, size);
}

void json_free(void* /*context*/, void* pointer) {
    core::tagged_free(core::MemoryTag::Tools, pointer);
}

constexpr yyjson_alc kJsonAllocator{
    .malloc = &json_malloc, .realloc = &json_realloc, .free = &json_free, .ctx = nullptr};

// JSON-RPC's own error codes.
constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr size_t kMostCompletions = 64;

// A response being built, written out as one line.
class Response {
public:
    Response() : doc_(yyjson_mut_doc_new(&kJsonAllocator)) {
        root_ = yyjson_mut_obj(doc_);
        yyjson_mut_doc_set_root(doc_, root_);
        yyjson_mut_obj_add_str(doc_, root_, "jsonrpc", "2.0");
    }
    ~Response() { yyjson_mut_doc_free(doc_); }
    Response(const Response&) = delete;
    Response& operator=(const Response&) = delete;
    Response(Response&&) = delete;
    Response& operator=(Response&&) = delete;

    yyjson_mut_doc* doc() { return doc_; }
    // The request's id, as it came (a number or a string), or null when it couldn't be read.
    void set_id(yyjson_val* id) {
        yyjson_mut_obj_add_val(doc_, root_, "id",
                               id != nullptr ? yyjson_val_mut_copy(doc_, id) : yyjson_mut_null(doc_));
    }
    void set_result(yyjson_mut_val* result) { yyjson_mut_obj_add_val(doc_, root_, "result", result); }
    void set_error(int code, const char* message) {
        yyjson_mut_val* error = yyjson_mut_obj(doc_);
        yyjson_mut_obj_add_int(doc_, error, "code", code);
        yyjson_mut_obj_add_strcpy(doc_, error, "message", message);
        yyjson_mut_obj_add_val(doc_, root_, "error", error);
    }
    void write(core::Array<char>& out) {
        size_t length = 0;
        char* text = yyjson_mut_write_opts(doc_, YYJSON_WRITE_NOFLAG, &kJsonAllocator, &length, nullptr);
        if (text != nullptr) {
            const size_t at = out.size();
            out.resize(at + length);
            std::memcpy(out.data() + at, text, length);
            kJsonAllocator.free(nullptr, text);
        }
    }

private:
    yyjson_mut_doc* doc_ = nullptr;
    yyjson_mut_val* root_ = nullptr;
};

// The text of a string member of an object, or empty and false.
bool string_member(yyjson_val* object, const char* name, std::string_view& out) {
    yyjson_val* value = yyjson_obj_get(object, name);
    if (!yyjson_is_str(value)) {
        return false;
    }
    out = std::string_view(yyjson_get_str(value), yyjson_get_len(value));
    return true;
}

void exec(console::Console& console, yyjson_val* params, Response& response) {
    std::string_view line;
    if (!string_member(params, "line", line)) {
        response.set_error(kInvalidParams, R"(console.exec takes {"line": "..."})");
        return;
    }
    core::Array<console::Line> said(core::heap_allocator(core::MemoryTag::Tools));
    const bool ok = console.execute(line, &said);
    yyjson_mut_doc* doc = response.doc();
    yyjson_mut_val* result = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, result, "ok", ok);
    yyjson_mut_val* output = yyjson_mut_arr(doc);
    for (const console::Line& said_line : said) {
        if (said_line.severity != console::Severity::Echo) { // the caller knows what it sent
            yyjson_mut_arr_add_strncpy(doc, output, said_line.text.c_str(), said_line.text.size());
        }
    }
    yyjson_mut_obj_add_val(doc, result, "output", output);
    response.set_result(result);
}

void complete(const console::Console& console, yyjson_val* params, Response& response) {
    std::string_view prefix;
    static_cast<void>(string_member(params, "prefix", prefix)); // none: everything
    console::Entry entries[kMostCompletions];
    const size_t count = console.complete(prefix, entries);
    yyjson_mut_doc* doc = response.doc();
    yyjson_mut_val* result = yyjson_mut_arr(doc);
    for (size_t i = 0; i < count; ++i) {
        yyjson_mut_val* entry = yyjson_mut_arr_add_obj(doc, result);
        yyjson_mut_obj_add_strcpy(doc, entry, "name", entries[i].name.c_str());
        yyjson_mut_obj_add_strcpy(doc, entry, "usage", entries[i].usage.c_str());
        yyjson_mut_obj_add_strcpy(doc, entry, "help", entries[i].help.c_str());
    }
    response.set_result(result);
}

} // namespace

void answer_request(console::Console& console, std::string_view request, core::Array<char>& response) {
    Response answer;
    yyjson_read_err error{};
    // yyjson doesn't write to the text without YYJSON_READ_INSITU.
    yyjson_doc* doc = yyjson_read_opts(const_cast<char*>(request.data()), request.size(), YYJSON_READ_NOFLAG,
                                       &kJsonAllocator, &error);
    if (doc == nullptr) {
        answer.set_id(nullptr);
        answer.set_error(kParseError, "not JSON");
        answer.write(response);
        return;
    }
    yyjson_val* root = yyjson_doc_get_root(doc);
    yyjson_val* id = yyjson_obj_get(root, "id");
    std::string_view method;
    if (!yyjson_is_obj(root) || !string_member(root, "method", method)) {
        answer.set_id(id);
        answer.set_error(kInvalidRequest, "a request is an object with a method (batches aren't taken)");
        answer.write(response);
        yyjson_doc_free(doc);
        return;
    }
    yyjson_val* params = yyjson_obj_get(root, "params");
    answer.set_id(id);
    if (method == "console.exec") {
        exec(console, params, answer);
    } else if (method == "console.complete") {
        complete(console, params, answer);
    } else {
        answer.set_error(kMethodNotFound, "no such method (console.exec, console.complete)");
    }
    if (id != nullptr) { // a notification gets no answer
        answer.write(response);
    }
    yyjson_doc_free(doc);
}

core::Result<AgentServer> AgentServer::start(uint16_t first_port) {
    for (uint16_t offset = 0; offset < kAgentPortsTried; ++offset) {
        core::Result<platform::TcpListener> listener =
            platform::TcpListener::listen_loopback(static_cast<uint16_t>(first_port + offset));
        if (listener) {
            AgentServer server;
            server.listener_ = std::move(*listener);
            return server;
        }
    }
    return core::fail(core::ErrorCode::AlreadyExists, "every port tried is taken");
}

void AgentServer::poll(console::Console& console) {
    DE_PROFILE_ZONE("agent server");
    // Whoever is waiting, in a free place; past kMaxAgentConnections, turned away (closed).
    for (platform::TcpStream stream = listener_.accept(); stream.is_open(); stream = listener_.accept()) {
        Connection* slot = std::ranges::find_if(connections_, [](const Connection& c) { return !c.stream.is_open(); });
        if (slot == std::end(connections_)) {
            DE_LOG_WARN("agents", "a connection turned away: %zu are open", kMaxAgentConnections);
            continue;
        }
        slot->stream = std::move(stream);
        slot->in.clear();
        slot->out.clear();
    }
    for (Connection& connection : connections_) {
        if (connection.stream.is_open()) {
            serve(connection, console);
        }
    }
}

void AgentServer::serve(Connection& connection, console::Console& console) {
    // What came in, a line a request.
    std::byte buffer[4096];
    while (connection.stream.is_open()) {
        const core::Result<size_t> got = connection.stream.read(buffer);
        if (!got || *got == 0) {
            break;
        }
        const size_t at = connection.in.size();
        connection.in.resize(at + *got);
        std::memcpy(connection.in.data() + at, buffer, *got);
        if (connection.in.size() > kMaxAgentRequestBytes) {
            DE_LOG_WARN("agents", "a request over %zu bytes: the connection is closed", kMaxAgentRequestBytes);
            connection.stream.close();
            return;
        }
    }
    size_t used = 0;
    for (;;) {
        const std::string_view rest(connection.in.data() + used, connection.in.size() - used);
        const size_t newline = rest.find('\n');
        if (newline == std::string_view::npos) {
            break;
        }
        std::string_view line = rest.substr(0, newline);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        used += newline + 1;
        if (!line.empty()) {
            const size_t before = connection.out.size();
            answer_request(console, line, connection.out);
            if (connection.out.size() != before) {
                connection.out.push('\n');
            }
        }
    }
    if (used > 0) {
        std::memmove(connection.in.data(), connection.in.data() + used, connection.in.size() - used);
        connection.in.resize(connection.in.size() - used);
    }
    // What can go out now; the rest next frame.
    if (!connection.out.empty() && connection.stream.is_open()) {
        const core::Result<size_t> sent = connection.stream.write(std::as_bytes(connection.out.span()));
        if (sent && *sent > 0) {
            std::memmove(connection.out.data(), connection.out.data() + *sent, connection.out.size() - *sent);
            connection.out.resize(connection.out.size() - *sent);
        }
    }
}

} // namespace de::tools
