#pragma once

#include "core/allocator.h"
#include "core/job_system.h"
#include "core/result.h"
#include "platform/file.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace de::platform {

struct VfsState;

// Mount points (docs/design.md, File system & IO). Paths are "mount:/relative/path": lowercase,
// forward slashes, no "." or ".." segments.
enum class Mount : uint8_t {
    Game, // game:/  cooked game data. Read only. Dev: loose files; ship: archives (later)
    User, // user:/  saves, settings, logs, caches. Read and write
    Temp, // temp:/  scratch, emptied every time the Vfs starts. Read and write
};

enum class IoPriority : uint8_t {
    Critical, // the player and the boss, now
    Near,     // cells about to activate
    Far,      // distant LODs
    Prefetch, // predicted from travel direction
};

inline constexpr size_t kIoPriorityCount = 4;

// Where an async read stands. Handles of finished or cancelled reads read as Invalid.
enum class ReadStatus : uint8_t {
    Invalid,
    Pending,
    InFlight,
    Done,
    Failed,
};

// What Vfs::cancel() did.
enum class CancelResult : uint8_t {
    NotStarted,      // dropped before it began: the destination was never touched
    DiscardedResult, // it had already finished: the result is dropped, the destination is free
    InFlight,        // too late: the destination is still in use, and finish() is still due
    UnknownHandle,   // finished, cancelled or never issued: nothing uses the destination
};

struct ReadHandle {
    uint32_t index = 0;
    uint32_t generation = 0;

    constexpr bool is_valid() const { return generation != 0; }
    constexpr bool operator==(const ReadHandle&) const = default;
};

inline constexpr uint64_t kWholeFile = UINT64_MAX;

struct ReadRequest {
    const char* path = nullptr; // "game:/characters/knight/knight.demesh"
    std::span<std::byte> dest;  // read straight into this memory, never into an intermediate copy
    uint64_t offset = 0;
    uint64_t size = kWholeFile; // kWholeFile: from offset to the end, which must fit in dest
    IoPriority priority = IoPriority::Near;
    core::Job on_complete = {}; // optional: submitted to VfsDesc::jobs when the read finishes
};

struct VfsDesc {
    const char* game_root = nullptr;      // folder with the cooked game data (required)
    const char* user_root = nullptr;      // null: the OS user data folder; the Vfs uses <root>/DEengine
    const char* temp_root = nullptr;      // null: the OS temp folder; the Vfs uses <root>/deengine_temp
    uint32_t reader_threads = 2;          // phase 1: blocking positional reads on a small pool
    uint32_t max_reads = 1024;            // reads in flight at once
    core::JobSystem* jobs = nullptr;      // where ReadRequest::on_complete jobs go
    core::Allocator* allocator = nullptr; // null: heap_allocator(MemoryTag::Platform)
};

struct IoStats {
    uint64_t reads_completed = 0;
    uint64_t reads_failed = 0;
    uint64_t bytes_read = 0;
    uint64_t total_latency_ns = 0; // submit to completion, summed over completed reads
    uint32_t pending[kIoPriorityCount] = {};
};

// The virtual file system: mount points over OS folders, path rules, and async reads (docs/design.md,
// File system & IO). Engine code reads and writes files only through it.
//
// Async reads go straight into caller memory, highest priority first; poll status() and call
// finish() to collect the result, or get a completion job. Phase 1 runs blocking positional reads
// on reader_threads threads behind one lock on the request queue; phase 2 swaps in IoRing /
// io_uring behind the same API. Blocking read_file() and write_file() are for startup and tools.
//
// Reading, polling and finishing never allocate. Thread-safe. Move-only.
class Vfs {
public:
    static core::Result<Vfs> create(const VfsDesc& desc);

    ~Vfs();
    Vfs(Vfs&& other) noexcept;
    Vfs& operator=(Vfs&& other) noexcept;
    Vfs(const Vfs&) = delete;
    Vfs& operator=(const Vfs&) = delete;

    // The OS path a VFS path maps to (for tools and logs). Fails on a malformed path.
    core::Result<PathString> resolve(const char* path) const;

    bool exists(const char* path) const;
    core::Result<uint64_t> file_size(const char* path) const;

    // Blocking: the whole file into dest (which must be large enough). Returns the bytes read.
    core::Result<size_t> read_file(const char* path, std::span<std::byte> dest) const;

    // Blocking and atomic (temp file, then rename); creates missing folders. user:/ and temp:/ only.
    core::Result<void> write_file(const char* path, std::span<const std::byte> data) const;

    // Queues a read. dest must stay valid until finish() returns, or until cancel() returns
    // anything but InFlight.
    core::Result<ReadHandle> read(const ReadRequest& request);

    ReadStatus status(ReadHandle handle) const;

    // Collects a Done or Failed read and frees its handle. Returns the bytes read.
    core::Result<size_t> finish(ReadHandle handle);

    // Drops a read that hasn't started, or the result of one that has finished, and frees its
    // handle. A read in flight can't be stopped: InFlight means dest is still in use.
    CancelResult cancel(ReadHandle handle);

    IoStats stats() const;

    // The OS folder behind a mount point.
    const char* mount_root(Mount mount) const;

private:
    Vfs() = default;
    VfsState* state_ = nullptr;
};

} // namespace de::platform
