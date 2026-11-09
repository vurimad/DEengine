#include "platform/socket.h"

// clang-format off: ws2tcpip.h needs winsock2.h first
#include <winsock2.h>
#include <ws2tcpip.h>
// clang-format on

#include <algorithm>
#include <climits>

namespace de::platform {

namespace {

// Winsock, started the first time a socket is wanted and stopped when the program ends.
bool winsock_ready() {
    struct Winsock {
        bool ok = false;
        Winsock() {
            WSADATA data{};
            ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        }
        ~Winsock() {
            if (ok) {
                WSACleanup();
            }
        }
        Winsock(const Winsock&) = delete;
        Winsock& operator=(const Winsock&) = delete;
        Winsock(Winsock&&) = delete;
        Winsock& operator=(Winsock&&) = delete;
    };
    static const Winsock kWinsock;
    return kWinsock.ok;
}

// INVALID_SOCKET, as an unsigned value (the macro casts ~0).
constexpr SOCKET kNoSocket = ~SOCKET{0};

SOCKET as_socket(uint64_t handle) {
    return static_cast<SOCKET>(handle);
}

bool would_block() {
    return WSAGetLastError() == WSAEWOULDBLOCK;
}

// Non-blocking, and small writes go out at once (a request is a line, its answer another).
bool prepare(SOCKET socket) {
    u_long on = 1;
    const BOOL no_delay = TRUE;
    return ioctlsocket(socket, FIONBIO, &on) == 0 &&
           setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay), sizeof(no_delay)) ==
               0;
}

sockaddr_in loopback(uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

} // namespace

TcpStream::~TcpStream() {
    close();
}

TcpStream::TcpStream(TcpStream&& other) noexcept : handle_(other.handle_) {
    other.handle_ = kClosed;
}

TcpStream& TcpStream::operator=(TcpStream&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = kClosed;
    }
    return *this;
}

void TcpStream::close() {
    if (handle_ != kClosed) {
        closesocket(as_socket(handle_));
        handle_ = kClosed;
    }
}

core::Result<size_t> TcpStream::read(std::span<std::byte> into) {
    if (!is_open()) {
        return core::fail(core::ErrorCode::IoError, "the connection is closed");
    }
    if (into.empty()) {
        return size_t{0};
    }
    const int got = recv(as_socket(handle_), reinterpret_cast<char*>(into.data()),
                         static_cast<int>(std::min<size_t>(into.size(), INT_MAX)), 0);
    if (got > 0) {
        return static_cast<size_t>(got);
    }
    if (got == SOCKET_ERROR && would_block()) {
        return size_t{0};
    }
    close();
    return core::fail(core::ErrorCode::IoError,
                      got == 0 ? "the other end closed the connection" : "the connection failed");
}

core::Result<size_t> TcpStream::write(std::span<const std::byte> bytes) {
    if (!is_open()) {
        return core::fail(core::ErrorCode::IoError, "the connection is closed");
    }
    if (bytes.empty()) {
        return size_t{0};
    }
    const int sent = send(as_socket(handle_), reinterpret_cast<const char*>(bytes.data()),
                          static_cast<int>(std::min<size_t>(bytes.size(), INT_MAX)), 0);
    if (sent >= 0) {
        return static_cast<size_t>(sent);
    }
    if (would_block()) {
        return size_t{0};
    }
    close();
    return core::fail(core::ErrorCode::IoError, "the connection failed");
}

core::Result<TcpStream> TcpStream::connect_loopback(uint16_t port) {
    if (!winsock_ready()) {
        return core::fail(core::ErrorCode::Unsupported, "Winsock didn't start");
    }
    const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kNoSocket) {
        return core::fail(core::ErrorCode::IoError, "no socket");
    }
    const sockaddr_in address = loopback(port);
    if (connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 || !prepare(socket)) {
        closesocket(socket);
        return core::fail(core::ErrorCode::NotFound, "nothing listens on that port");
    }
    return TcpStream(static_cast<uint64_t>(socket));
}

TcpListener::~TcpListener() {
    close();
}

TcpListener::TcpListener(TcpListener&& other) noexcept : handle_(other.handle_), port_(other.port_) {
    other.handle_ = TcpStream::kClosed;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        port_ = other.port_;
        other.handle_ = TcpStream::kClosed;
    }
    return *this;
}

void TcpListener::close() {
    if (handle_ != TcpStream::kClosed) {
        closesocket(as_socket(handle_));
        handle_ = TcpStream::kClosed;
    }
}

core::Result<TcpListener> TcpListener::listen_loopback(uint16_t port) {
    if (!winsock_ready()) {
        return core::fail(core::ErrorCode::Unsupported, "Winsock didn't start");
    }
    const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kNoSocket) {
        return core::fail(core::ErrorCode::IoError, "no socket");
    }
    // Ours alone: another program can't bind the same port over it.
    const BOOL exclusive = TRUE;
    setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in address = loopback(port);
    int length = sizeof(address);
    if (bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 || listen(socket, 4) != 0 ||
        !prepare(socket) || getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        closesocket(socket);
        return core::fail(core::ErrorCode::AlreadyExists, "the port is taken");
    }
    TcpListener listener;
    listener.handle_ = static_cast<uint64_t>(socket);
    listener.port_ = ntohs(address.sin_port);
    return listener;
}

TcpStream TcpListener::accept() {
    if (!is_open()) {
        return {};
    }
    const SOCKET socket = ::accept(as_socket(handle_), nullptr, nullptr);
    if (socket == kNoSocket) {
        return {};
    }
    if (!prepare(socket)) {
        closesocket(socket);
        return {};
    }
    return TcpStream(static_cast<uint64_t>(socket));
}

} // namespace de::platform
