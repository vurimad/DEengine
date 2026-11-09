#include "platform/process.h"

#include "core/array.h"
#include "core/log.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_process.h>
#include <SDL3/SDL_properties.h>

#include <utility>

namespace de::platform {

core::Result<Process> Process::start(std::span<const char* const> args) {
    if (args.empty() || args[0] == nullptr) {
        return core::fail(core::ErrorCode::InvalidArgument, "no program to run");
    }
    // SDL takes the arguments null-terminated.
    core::Array<const char*> terminated(core::heap_allocator(core::MemoryTag::Platform));
    terminated.reserve(args.size() + 1);
    for (const char* arg : args) {
        terminated.push(arg);
    }
    terminated.push(nullptr);

    const SDL_PropertiesID properties = SDL_CreateProperties();
    if (properties == 0) {
        return core::fail(core::ErrorCode::OutOfMemory, "the process's properties couldn't be made");
    }
    // SDL reads the array through a void*.
    static_cast<void>(SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER,
                                             static_cast<void*>(terminated.data())));
    static_cast<void>(SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL));
    static_cast<void>(SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL));
    static_cast<void>(SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL));
    // No console window popping up for a console program.
    static_cast<void>(SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true));
    SDL_Process* process = SDL_CreateProcessWithProperties(properties);
    SDL_DestroyProperties(properties);
    if (process == nullptr) {
        DE_LOG_ERROR("platform", "can't run %s: %s", args[0], SDL_GetError());
        return core::fail(core::ErrorCode::NotFound, "the program couldn't be started");
    }
    return Process(process);
}

Process::~Process() {
    release();
}

Process::Process(Process&& other) noexcept
    : process_(std::exchange(other.process_, nullptr)), ended_(other.ended_), exit_code_(other.exit_code_) {}

Process& Process::operator=(Process&& other) noexcept {
    if (this != &other) {
        release();
        process_ = std::exchange(other.process_, nullptr);
        ended_ = other.ended_;
        exit_code_ = other.exit_code_;
    }
    return *this;
}

void Process::release() {
    if (process_ != nullptr) {
        // A program still running goes on to its end on its own.
        SDL_DestroyProcess(process_);
        process_ = nullptr;
    }
}

bool Process::finished(int& exit_code) {
    if (process_ == nullptr) {
        return false;
    }
    if (!ended_) {
        int code = 0;
        if (!SDL_WaitProcess(process_, false, &code)) {
            return false;
        }
        ended_ = true;
        exit_code_ = code;
    }
    exit_code = exit_code_;
    return true;
}

core::Result<PathString> program_beside_engine(const char* name) {
    const char* folder = SDL_GetBasePath(); // ends in a separator
    if (folder == nullptr) {
        return core::fail(core::ErrorCode::NotFound, "the engine's folder isn't known");
    }
    PathString path;
    if (!path.assign(folder) || !path.append(name) || !path.append(program_suffix())) {
        return core::fail(core::ErrorCode::OutOfRange, "the program's path is too long");
    }
    return path;
}

} // namespace de::platform
