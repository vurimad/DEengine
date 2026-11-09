#include "console/console.h"
#include "platform/socket.h"
#include "platform/time.h"
#include "tools/agent_server.h"

#include <doctest/doctest.h>

#include <cstring>
#include <string_view>

using de::console::Console;
using de::core::Array;

namespace {

de::core::Allocator& tools_heap() {
    return de::core::heap_allocator(de::core::MemoryTag::Tools);
}

std::string_view text_of(const Array<char>& bytes) {
    return {bytes.data(), bytes.size()};
}

} // namespace

TEST_CASE("agent server: console.exec runs a line and answers with what it said") {
    Console console(tools_heap());
    bool taa = false;
    console.add_variable("r.taa", "temporal anti-aliasing", &taa);
    Array<char> response(tools_heap());
    de::tools::answer_request(
        console, R"({"jsonrpc":"2.0","id":7,"method":"console.exec","params":{"line":"r.taa on"}})", response);
    CHECK(taa);
    CHECK((text_of(response) == R"({"jsonrpc":"2.0","id":7,"result":{"ok":true,"output":["r.taa = on"]}})"));

    // A failing line still answers, ok false and why.
    response.clear();
    de::tools::answer_request(
        console, R"({"jsonrpc":"2.0","id":"a","method":"console.exec","params":{"line":"r.nope"}})", response);
    CHECK((text_of(response).find(R"("id":"a","result":{"ok":false,"output":["Unknown command)") !=
           std::string_view::npos));
}

TEST_CASE("agent server: completion, and JSON-RPC's errors for what isn't a request") {
    Console console(tools_heap());
    bool taa = false;
    console.add_variable("r.taa", "temporal anti-aliasing", &taa);
    Array<char> response(tools_heap());
    de::tools::answer_request(
        console, R"({"jsonrpc":"2.0","id":1,"method":"console.complete","params":{"prefix":"r."}})", response);
    CHECK((
        text_of(response) ==
        R"({"jsonrpc":"2.0","id":1,"result":[{"name":"r.taa","usage":"[on | off]","help":"temporal anti-aliasing"}]})"));

    response.clear();
    de::tools::answer_request(console, "{not json", response);
    CHECK((text_of(response) == R"({"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":"not JSON"}})"));

    response.clear();
    de::tools::answer_request(console, R"({"jsonrpc":"2.0","id":2,"method":"world.explode"})", response);
    CHECK((text_of(response).find(R"("error":{"code":-32601)") != std::string_view::npos));

    response.clear();
    de::tools::answer_request(console, R"({"jsonrpc":"2.0","id":3,"method":"console.exec","params":{}})", response);
    CHECK((text_of(response).find(R"("error":{"code":-32602)") != std::string_view::npos));

    // A notification (no id) runs, and gets no answer.
    response.clear();
    de::tools::answer_request(console, R"({"jsonrpc":"2.0","method":"console.exec","params":{"line":"r.taa on"}})",
                              response);
    CHECK(taa);
    CHECK(response.empty());
}

TEST_CASE("agent server: a request over the loopback socket is answered on the next polls") {
    Console console(tools_heap());
    bool taa = false;
    console.add_variable("r.taa", "temporal anti-aliasing", &taa);
    // Port 0: whatever the OS gives, so the test never meets a running engine's.
    auto server = de::tools::AgentServer::start(0);
    REQUIRE(server.ok());
    auto client = de::platform::TcpStream::connect_loopback(server->port());
    REQUIRE(client.ok());
    const std::string_view request = R"({"jsonrpc":"2.0","id":1,"method":"console.exec","params":{"line":"r.taa on"}})"
                                     "\n";
    REQUIRE(client->write(std::as_bytes(std::span(request.data(), request.size()))).ok());

    Array<char> answer(tools_heap());
    for (int round = 0; round < 2000 && (answer.empty() || answer.back() != '\n'); ++round) {
        server->poll(console);
        std::byte buffer[1024];
        const auto got = client->read(buffer);
        REQUIRE(got.ok());
        for (size_t i = 0; i < *got; ++i) {
            answer.push(static_cast<char>(buffer[i]));
        }
        if (*got == 0) {
            de::platform::sleep_ns(1'000'000);
        }
    }
    CHECK(taa);
    CHECK((text_of(answer) == "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"ok\":true,\"output\":[\"r.taa = on\"]}}\n"));
}
