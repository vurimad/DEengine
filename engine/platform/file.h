#pragma once

#include "core/fixed_string.h"
#include "core/function_ref.h"
#include "core/result.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace de::platform {

// Native file access. Only the platform layer touches OS files; engine code goes through the VFS
// (platform/vfs.h). Paths are UTF-8; forward slashes work on every OS.

inline constexpr size_t kMaxPathLength = 1023;
using PathString = core::FixedString<kMaxPathLength>;

// A file opened for reading. Reads are positional, so several threads may read one File at once.
// Move-only: it owns the OS handle.
class File {
public:
    static core::Result<File> open_read(const char* path);

    File() = default;
    ~File();
    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    bool is_open() const { return handle_ != kInvalid; }
    core::Result<uint64_t> size() const;

    // Reads up to dest.size() bytes at offset into dest. Returns the bytes read: fewer only at
    // the end of the file.
    core::Result<size_t> read_at(uint64_t offset, std::span<std::byte> dest) const;

private:
    static constexpr intptr_t kInvalid = -1;

    explicit File(intptr_t handle) : handle_(handle) {}
    void close();

    intptr_t handle_ = kInvalid;
};

// Writes data to path atomically: a uniquely named temporary file next to it, flushed, then
// renamed over it. A crash leaves either the old file or the new one, never half of one, and
// concurrent writes to one path each land whole (the last rename wins).
core::Result<void> write_file_atomic(const char* path, std::span<const std::byte> data);

bool path_exists(const char* path);

struct FileInfo {
    uint64_t size = 0;
    uint64_t modified_time = 0; // OS file clock: compare two values, don't read meaning into one
    bool is_directory = false;
};

// Size and last write time of a file or folder. NotFound if nothing is there.
core::Result<FileInfo> file_info(const char* path);

// Creates path and any missing parents. Succeeds if it already exists.
core::Result<void> create_directories(const char* path);

// Deletes path and everything under it, never following links. Succeeds if it doesn't exist.
core::Result<void> delete_tree(const char* path);

// Calls visit(name, is_directory) for each entry of a folder, except "." and "..".
core::Result<void> list_directory(const char* path, core::FunctionRef<void(const char* name, bool is_directory)> visit);

uint32_t current_process_id();

// Whether a process with this id is running (ids can be reused, so it may be another program).
bool process_alive(uint32_t process_id);

// The OS folders the VFS mounts under (without a trailing slash).
core::Result<PathString> user_data_folder(); // Windows: Saved Games; Linux: ~/.local/share
core::Result<PathString> system_temp_folder();

} // namespace de::platform
