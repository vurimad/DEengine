#pragma once

#include "core/fixed_string.h"
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

// Writes data to path atomically: a temporary file next to it, flushed, then renamed over it.
// A crash leaves either the old file or the new one, never half of one.
core::Result<void> write_file_atomic(const char* path, std::span<const std::byte> data);

bool path_exists(const char* path);

// Creates path and any missing parents. Succeeds if it already exists.
core::Result<void> create_directories(const char* path);

// Deletes path and everything under it. Succeeds if it doesn't exist.
core::Result<void> delete_tree(const char* path);

// The OS folders the VFS mounts under (without a trailing slash).
core::Result<PathString> user_data_folder(); // Windows: Saved Games; Linux: ~/.local/share
core::Result<PathString> system_temp_folder();

} // namespace de::platform
