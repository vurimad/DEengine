#pragma once

#include "core/result.h"
#include "platform/file.h"

#include <span>

struct SDL_Process;

namespace de::platform {

// A program run beside the engine: the editor's cooker, after a save. Started in the background, its output thrown
// away and no console window of its own; asked, never waited on, whether it has ended. For tools, not the game.
//
//     auto cooker = Process::start(args);   // args[0]: the program's path
//     ...each frame: if (cooker->finished(code)) { ... }
class Process {
public:
    // Starts args[0] with the rest as its arguments.
    static core::Result<Process> start(std::span<const char* const> args);

    Process() = default;
    ~Process();
    Process(Process&& other) noexcept;
    Process& operator=(Process&& other) noexcept;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // Whether it's started and hasn't been seen to end.
    bool running() const { return process_ != nullptr && !ended_; }
    // Whether it has ended, without waiting: its exit code in exit_code then (true from then on).
    bool finished(int& exit_code);

private:
    explicit Process(SDL_Process* process) : process_(process) {}
    void release();

    SDL_Process* process_ = nullptr;
    bool ended_ = false;
    int exit_code_ = 0;
};

// A program beside the engine's own executable, by name ("cooker": its .exe on Windows).
core::Result<PathString> program_beside_engine(const char* name);

} // namespace de::platform
