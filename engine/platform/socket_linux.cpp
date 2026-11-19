#include "platform/socket.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

namespace de::platform {

namespace {

int as_socket(uint64_t handle) {
    return static_cast<int>(handle);
}

bool would_block() {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

// Non-blocking, and small writes go out at once (a request is a line, its answer another).
bool prepare(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    const int no_delay = 1;
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
           setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay)) == 0;
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
        ::close(as_socket(handle_));
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
    const ssize_t got = recv(as_socket(handle_), into.data(), into.size(), 0);
    if (got > 0) {
        return static_cast<size_t>(got);
    }
    if (got < 0 && would_block()) {
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
    // No SIGPIPE when the other end has gone: an error instead.
    const ssize_t sent = send(as_socket(handle_), bytes.data(), bytes.size(), MSG_NOSIGNAL);
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
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP); // not for programs started from here
    if (fd < 0) {
        return core::fail(core::ErrorCode::IoError, "no socket");
    }
    const sockaddr_in address = loopback(port);
    if (connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 || !prepare(fd)) {
        ::close(fd);
        return core::fail(core::ErrorCode::NotFound, "nothing listens on that port");
    }
    return TcpStream(static_cast<uint64_t>(fd));
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
        ::close(as_socket(handle_));
        handle_ = TcpStream::kClosed;
    }
}

core::Result<TcpListener> TcpListener::listen_loopback(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP); // not for programs started from here
    if (fd < 0) {
        return core::fail(core::ErrorCode::IoError, "no socket");
    }
    // A port left in TIME_WAIT by the last run can be had again; two listeners on one port still can't.
    const int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address = loopback(port);
    socklen_t length = sizeof(address);
    if (bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 || listen(fd, 4) != 0 ||
        !prepare(fd) || getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        ::close(fd);
        return core::fail(core::ErrorCode::AlreadyExists, "the port is taken");
    }
    TcpListener listener;
    listener.handle_ = static_cast<uint64_t>(fd);
    listener.port_ = ntohs(address.sin_port);
    return listener;
}

TcpStream TcpListener::accept() {
    if (!is_open()) {
        return {};
    }
    const int fd = ::accept4(as_socket(handle_), nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) {
        return {};
    }
    if (!prepare(fd)) {
        ::close(fd);
        return {};
    }
    return TcpStream(static_cast<uint64_t>(fd));
}

} // namespace de::platform
