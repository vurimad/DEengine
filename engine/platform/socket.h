#pragma once

#include "core/result.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace de::platform {

// A TCP connection that never blocks: reads and writes take what they can now. Closed when destroyed.
class TcpStream {
public:
    TcpStream() = default;
    ~TcpStream();
    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;
    TcpStream(TcpStream&& other) noexcept;
    TcpStream& operator=(TcpStream&& other) noexcept;

    bool is_open() const { return handle_ != kClosed; }
    // How many bytes came in: 0 if none are waiting. Fails once the other end has gone, closing this one.
    core::Result<size_t> read(std::span<std::byte> into);
    // How many of the bytes went out now (maybe fewer). Fails once the other end has gone, closing this one.
    core::Result<size_t> write(std::span<const std::byte> bytes);
    void close();

    // To connect from this machine (tests, tools): blocks until it's connected or refused.
    static core::Result<TcpStream> connect_loopback(uint16_t port);

private:
    friend class TcpListener;
    static constexpr uint64_t kClosed = ~uint64_t{0};
    explicit TcpStream(uint64_t handle) : handle_(handle) {}

    uint64_t handle_ = kClosed;
};

// Listens on the loopback address only (127.0.0.1): nothing off this machine can connect. Never blocks.
class TcpListener {
public:
    TcpListener() = default;
    ~TcpListener();
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;

    // Fails if the port is taken (another engine has it).
    static core::Result<TcpListener> listen_loopback(uint16_t port);
    bool is_open() const { return handle_ != TcpStream::kClosed; }
    uint16_t port() const { return port_; }
    // A connection that's waiting, or a closed stream if none is.
    TcpStream accept();
    void close();

private:
    uint64_t handle_ = TcpStream::kClosed;
    uint16_t port_ = 0;
};

} // namespace de::platform
