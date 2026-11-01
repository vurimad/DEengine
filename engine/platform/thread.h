#pragma once

#include <cstddef>
#include <cstdint>
#include <thread>

namespace de::platform {

// Names the calling thread for debuggers, profilers and crash dumps.
// Linux keeps the first 15 characters.
void set_current_thread_name(const char* name);

// Copies the calling thread's name into buffer (empty if unnamed). False if the OS can't say.
bool current_thread_name(char* buffer, size_t size);

uint64_t current_thread_id();

// Logical cores (hardware threads); at least 1.
uint32_t hardware_thread_count();

// An OS thread that starts on construction and is joined on destruction. It takes a plain
// function and a context pointer, so starting one never needs an owning callable.
//     Thread worker("worker 0", &run_worker, &worker_state);
class Thread {
public:
    using Entry = void (*)(void* context);

    static constexpr size_t kMaxNameLength = 31;

    Thread() = default;
    Thread(const char* name, Entry entry, void* context);
    ~Thread();

    Thread(Thread&& other) noexcept = default;
    Thread& operator=(Thread&& other) noexcept;
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    bool joinable() const { return thread_.joinable(); }
    void join();

private:
    std::thread thread_;
};

} // namespace de::platform
