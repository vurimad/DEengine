#include "platform/vfs.h"

#include "core/log.h"
#include "core/profile.h"
#include "platform/thread.h"
#include "platform/time.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <new>
#include <string_view>
#include <utility>

namespace de::platform {

namespace {

using core::ErrorCode;
using core::fail;

constexpr uint32_t kNoSlot = UINT32_MAX;

struct ParsedPath {
    Mount mount = Mount::Game;
    std::string_view relative;
};

bool is_path_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/';
}

// "game:/characters/knight.demesh" -> Game + "characters/knight.demesh", enforcing the path rules:
// lowercase, forward slashes, no empty, "." or ".." segments (docs/design.md, File system & IO).
core::Result<ParsedPath> parse_path(const char* path) {
    if (path == nullptr) {
        return fail(ErrorCode::InvalidArgument, "null path");
    }
    const std::string_view text(path);
    struct Prefix {
        std::string_view text;
        Mount mount;
    };
    static constexpr Prefix kPrefixes[] = {
        {.text = "game:/", .mount = Mount::Game},
        {.text = "user:/", .mount = Mount::User},
        {.text = "temp:/", .mount = Mount::Temp},
    };
    for (const Prefix& prefix : kPrefixes) {
        if (!text.starts_with(prefix.text)) {
            continue;
        }
        const std::string_view relative = text.substr(prefix.text.size());
        if (relative.empty()) {
            return fail(ErrorCode::InvalidArgument, "path names no file");
        }
        size_t segment_start = 0;
        for (size_t i = 0; i <= relative.size(); ++i) {
            if (i < relative.size() && relative[i] != '/') {
                if (!is_path_char(relative[i])) {
                    return fail(ErrorCode::InvalidArgument, "paths use lowercase letters, digits, _ - . and /");
                }
                continue;
            }
            const std::string_view segment = relative.substr(segment_start, i - segment_start);
            if (segment.empty() || segment == "." || segment == "..") {
                return fail(ErrorCode::InvalidArgument, "path has an empty, . or .. segment");
            }
            segment_start = i + 1;
        }
        return ParsedPath{.mount = prefix.mount, .relative = relative};
    }
    return fail(ErrorCode::InvalidArgument, "path must start with game:/, user:/ or temp:/");
}

core::Result<PathString> join(const PathString& root, std::string_view relative) {
    PathString path = root;
    if (!path.append('/') || !path.append(relative)) {
        return fail(ErrorCode::InvalidArgument, "path is too long");
    }
    return path;
}

struct RequestSlot {
    std::atomic<ReadStatus> status{ReadStatus::Invalid};
    std::atomic<uint32_t> generation{1};

    // Written under the queue lock before the read starts, then owned by one reader thread.
    PathString native_path;
    std::span<std::byte> dest;
    uint64_t offset = 0;
    uint64_t size = kWholeFile;
    IoPriority priority = IoPriority::Near;
    core::Job on_complete;
    uint64_t submit_ns = 0;
    bool cancelled = false;
    uint32_t next_free = kNoSlot;

    // Written by the reader before status becomes Done or Failed.
    size_t bytes_read = 0;
    core::Error error;
};

} // namespace

struct VfsState {
    core::Allocator* allocator = nullptr;
    PathString roots[3];
    core::JobSystem* jobs = nullptr;

    RequestSlot* slots = nullptr;
    uint32_t slot_count = 0;

    // Everything below up to `stopping` is guarded by `mutex`: the one lock on the request
    // queue the design allows (docs/design.md, Performance rules).
    std::mutex mutex;
    std::condition_variable work;
    uint32_t* queues[kIoPriorityCount] = {}; // rings of slot indices, one per priority
    uint32_t queue_head[kIoPriorityCount] = {};
    uint32_t queue_size[kIoPriorityCount] = {};
    uint32_t free_head = kNoSlot;
    bool stopping = false;

    Thread* readers = nullptr;
    uint32_t reader_count = 0;

    std::atomic<uint64_t> reads_completed{0};
    std::atomic<uint64_t> reads_failed{0};
    std::atomic<uint64_t> bytes_read{0};
    std::atomic<uint64_t> latency_ns{0};
};

namespace {

bool any_queued(const VfsState& state) {
    for (const uint32_t size : state.queue_size) {
        if (size > 0) {
            return true;
        }
    }
    return false;
}

// Highest priority first; first in, first out within a priority. Caller holds the lock.
uint32_t pop_queued(VfsState& state) {
    for (size_t p = 0; p < kIoPriorityCount; ++p) {
        if (state.queue_size[p] > 0) {
            const uint32_t index = state.queues[p][state.queue_head[p]];
            state.queue_head[p] = (state.queue_head[p] + 1) % state.slot_count;
            --state.queue_size[p];
            return index;
        }
    }
    return kNoSlot;
}

// Returns a slot to the free list. Caller holds the lock.
void release_slot(VfsState& state, uint32_t index) {
    RequestSlot& slot = state.slots[index];
    uint32_t next_generation = slot.generation.load(std::memory_order_relaxed) + 1;
    if (next_generation == 0) {
        next_generation = 1; // 0 marks the invalid handle
    }
    slot.generation.store(next_generation, std::memory_order_relaxed);
    slot.status.store(ReadStatus::Invalid, std::memory_order_release);
    slot.cancelled = false;
    slot.next_free = state.free_head;
    state.free_head = index;
}

RequestSlot* live_slot(VfsState& state, ReadHandle handle) {
    if (!handle.is_valid() || handle.index >= state.slot_count) {
        return nullptr;
    }
    RequestSlot& slot = state.slots[handle.index];
    return slot.generation.load(std::memory_order_acquire) == handle.generation ? &slot : nullptr;
}

void perform_read(VfsState& state, RequestSlot& slot) {
    DE_PROFILE_ZONE("vfs_read");
    slot.bytes_read = 0;
    slot.error = core::Error{};
    bool ok = false;

    core::Result<File> file = File::open_read(slot.native_path.c_str());
    if (!file) {
        slot.error = file.error();
    } else if (const core::Result<uint64_t> file_size = file->size(); !file_size) {
        slot.error = file_size.error();
    } else if (slot.offset > *file_size) {
        slot.error = {.code = ErrorCode::OutOfRange, .message = "read offset is past the end of the file"};
    } else {
        const uint64_t available = *file_size - slot.offset;
        const uint64_t length = slot.size == kWholeFile ? available : slot.size;
        if (length > slot.dest.size()) {
            slot.error = {.code = ErrorCode::OutOfRange, .message = "read destination is too small"};
        } else if (const core::Result<size_t> read =
                       file->read_at(slot.offset, slot.dest.first(static_cast<size_t>(length)));
                   !read) {
            slot.error = read.error();
        } else {
            slot.bytes_read = *read;
            ok = true;
        }
    }

    const uint64_t latency = time_ns() - slot.submit_ns;
    if (ok) {
        state.reads_completed.fetch_add(1, std::memory_order_relaxed);
        state.bytes_read.fetch_add(slot.bytes_read, std::memory_order_relaxed);
        state.latency_ns.fetch_add(latency, std::memory_order_relaxed);
    } else {
        state.reads_failed.fetch_add(1, std::memory_order_relaxed);
    }
    // Copy the job out first: once the status is published, the caller may finish() the read
    // and the slot may be reused.
    const core::Job on_complete = slot.on_complete;
    slot.status.store(ok ? ReadStatus::Done : ReadStatus::Failed, std::memory_order_release);
    if (on_complete.entry != nullptr && state.jobs != nullptr) {
        state.jobs->submit(on_complete);
    }
}

void reader_main(void* context) {
    VfsState& state = *static_cast<VfsState*>(context);
    for (;;) {
        RequestSlot* slot = nullptr;
        {
            std::unique_lock lock(state.mutex);
            state.work.wait(lock, [&state] { return state.stopping || any_queued(state); });
            if (state.stopping) {
                return;
            }
            const uint32_t index = pop_queued(state);
            if (state.slots[index].cancelled) {
                release_slot(state, index); // cancelled while it waited
                continue;
            }
            slot = &state.slots[index];
            slot->status.store(ReadStatus::InFlight, std::memory_order_release);
        }
        perform_read(state, *slot);
    }
}

// One separator style in roots, logs and tools: forward slashes work on every OS.
PathString normalized(PathString path) {
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '\\') {
            path[i] = '/';
        }
    }
    return path;
}

// <configured or OS folder>/<folder_name>: the Vfs always works inside a folder it names itself.
core::Result<PathString> owned_root(const char* configured, core::Result<PathString> (*os_folder)(),
                                    std::string_view folder_name) {
    PathString base;
    if (configured != nullptr) {
        base.assign(configured);
    } else {
        const core::Result<PathString> found = os_folder();
        if (!found) {
            return fail(found.error());
        }
        base = *found;
    }
    return join(normalized(base), folder_name);
}

} // namespace

core::Result<Vfs> Vfs::create(const VfsDesc& desc) {
    if (desc.game_root == nullptr) {
        return fail(ErrorCode::InvalidArgument, "VfsDesc::game_root is required");
    }
    DE_VERIFY_MSG(desc.max_reads > 0 && desc.reader_threads > 0, "the Vfs needs reader threads and read slots");

    const PathString game_root = normalized(PathString(desc.game_root));
    const core::Result<PathString> user_root = owned_root(desc.user_root, &user_data_folder, "DEengine");
    const core::Result<PathString> temp_root = owned_root(desc.temp_root, &system_temp_folder, "deengine_temp");
    if (!user_root || !temp_root) {
        DE_LOG_ERROR("vfs", "user:/ or temp:/ has no folder to mount");
        return fail(!user_root ? user_root.error() : temp_root.error());
    }
    if (!path_exists(game_root.c_str())) {
        DE_LOG_WARN("vfs", "game:/ folder %s doesn't exist yet: reads from game:/ will fail", game_root.c_str());
    }
    // temp:/ starts empty. Only the folder the Vfs named itself is cleared, never a configured path.
    if (!delete_tree(temp_root->c_str()).ok() || !create_directories(temp_root->c_str()).ok() ||
        !create_directories(user_root->c_str()).ok()) {
        DE_LOG_ERROR("vfs", "could not prepare %s and %s", user_root->c_str(), temp_root->c_str());
        return fail(ErrorCode::IoError, "user:/ or temp:/ could not be prepared");
    }

    core::Allocator& allocator =
        desc.allocator != nullptr ? *desc.allocator : core::heap_allocator(core::MemoryTag::Platform);
    auto* state = ::new (allocator.allocate(sizeof(VfsState), alignof(VfsState))) VfsState();
    state->allocator = &allocator;
    state->roots[static_cast<size_t>(Mount::Game)] = game_root;
    state->roots[static_cast<size_t>(Mount::User)] = *user_root;
    state->roots[static_cast<size_t>(Mount::Temp)] = *temp_root;
    state->jobs = desc.jobs;

    state->slot_count = desc.max_reads;
    state->slots = core::allocate_array<RequestSlot>(allocator, state->slot_count);
    for (uint32_t i = 0; i < state->slot_count; ++i) {
        ::new (static_cast<void*>(&state->slots[i])) RequestSlot();
        state->slots[i].next_free = i + 1 < state->slot_count ? i + 1 : kNoSlot;
    }
    state->free_head = 0;
    for (uint32_t*& queue : state->queues) {
        queue = core::allocate_array<uint32_t>(allocator, state->slot_count);
    }

    state->reader_count = desc.reader_threads;
    state->readers = core::allocate_array<Thread>(allocator, state->reader_count);
    for (uint32_t i = 0; i < state->reader_count; ++i) {
        ::new (static_cast<void*>(&state->readers[i])) Thread("vfs reader", &reader_main, state);
    }

    DE_LOG_INFO("vfs", "game:/ = %s", game_root.c_str());
    DE_LOG_INFO("vfs", "user:/ = %s", user_root->c_str());
    DE_LOG_INFO("vfs", "temp:/ = %s", temp_root->c_str());

    Vfs vfs;
    vfs.state_ = state;
    return vfs;
}

Vfs::~Vfs() {
    if (state_ == nullptr) {
        return;
    }
    VfsState& state = *state_;
    {
        const std::scoped_lock lock(state.mutex);
        state.stopping = true;
    }
    state.work.notify_all();
    core::Allocator& allocator = *state.allocator;
    for (uint32_t i = 0; i < state.reader_count; ++i) {
        state.readers[i].~Thread(); // joins: reads in flight complete first
    }
    core::deallocate_array(allocator, state.readers, state.reader_count);
    for (uint32_t* queue : state.queues) {
        core::deallocate_array(allocator, queue, state.slot_count);
    }
    for (uint32_t i = 0; i < state.slot_count; ++i) {
        state.slots[i].~RequestSlot();
    }
    core::deallocate_array(allocator, state.slots, state.slot_count);
    state.~VfsState();
    allocator.deallocate(state_, sizeof(VfsState), alignof(VfsState));
}

Vfs::Vfs(Vfs&& other) noexcept : state_(std::exchange(other.state_, nullptr)) {}

Vfs& Vfs::operator=(Vfs&& other) noexcept {
    if (this != &other) {
        Vfs discarded;
        discarded.state_ = std::exchange(state_, std::exchange(other.state_, nullptr));
    }
    return *this;
}

core::Result<PathString> Vfs::resolve(const char* path) const {
    const core::Result<ParsedPath> parsed = parse_path(path);
    if (!parsed) {
        return fail(parsed.error());
    }
    return join(state_->roots[static_cast<size_t>(parsed->mount)], parsed->relative);
}

const char* Vfs::mount_root(Mount mount) const {
    return state_->roots[static_cast<size_t>(mount)].c_str();
}

bool Vfs::exists(const char* path) const {
    const core::Result<PathString> native = resolve(path);
    return native.ok() && path_exists(native->c_str());
}

core::Result<uint64_t> Vfs::file_size(const char* path) const {
    const core::Result<PathString> native = resolve(path);
    if (!native) {
        return fail(native.error());
    }
    const core::Result<File> file = File::open_read(native->c_str());
    if (!file) {
        return fail(file.error());
    }
    return file->size();
}

core::Result<size_t> Vfs::read_file(const char* path, std::span<std::byte> dest) const {
    DE_PROFILE_ZONE("vfs_read_file");
    const core::Result<PathString> native = resolve(path);
    if (!native) {
        return fail(native.error());
    }
    const core::Result<File> file = File::open_read(native->c_str());
    if (!file) {
        return fail(file.error());
    }
    const core::Result<uint64_t> size = file->size();
    if (!size) {
        return fail(size.error());
    }
    if (*size > dest.size()) {
        return fail(ErrorCode::OutOfRange, "read destination is too small");
    }
    return file->read_at(0, dest.first(static_cast<size_t>(*size)));
}

core::Result<void> Vfs::write_file(const char* path, std::span<const std::byte> data) const {
    DE_PROFILE_ZONE("vfs_write_file");
    const core::Result<ParsedPath> parsed = parse_path(path);
    if (!parsed) {
        return fail(parsed.error());
    }
    if (parsed->mount == Mount::Game) {
        return fail(ErrorCode::Unsupported, "game:/ is read only");
    }
    const core::Result<PathString> native = join(state_->roots[static_cast<size_t>(parsed->mount)], parsed->relative);
    if (!native) {
        return fail(native.error());
    }
    const size_t last_slash = native->view().find_last_of('/');
    const PathString folder(native->view().substr(0, last_slash));
    if (const core::Result<void> created = create_directories(folder.c_str()); !created) {
        return created;
    }
    return write_file_atomic(native->c_str(), data);
}

core::Result<ReadHandle> Vfs::read(const ReadRequest& request) {
    const core::Result<PathString> native = resolve(request.path);
    if (!native) {
        return fail(native.error());
    }
    VfsState& state = *state_;
    ReadHandle handle;
    {
        const std::scoped_lock lock(state.mutex);
        if (state.free_head == kNoSlot) {
            return fail(ErrorCode::OutOfRange, "too many reads in flight (VfsDesc::max_reads)");
        }
        const uint32_t index = state.free_head;
        RequestSlot& slot = state.slots[index];
        state.free_head = slot.next_free;

        slot.native_path = *native;
        slot.dest = request.dest;
        slot.offset = request.offset;
        slot.size = request.size;
        slot.priority = request.priority;
        slot.on_complete = request.on_complete;
        slot.submit_ns = time_ns();
        slot.cancelled = false;
        slot.status.store(ReadStatus::Pending, std::memory_order_release);

        const auto p = static_cast<size_t>(request.priority);
        state.queues[p][(state.queue_head[p] + state.queue_size[p]) % state.slot_count] = index;
        ++state.queue_size[p];
        handle = ReadHandle{.index = index, .generation = slot.generation.load(std::memory_order_relaxed)};
    }
    state.work.notify_one();
    return handle;
}

ReadStatus Vfs::status(ReadHandle handle) const {
    RequestSlot* slot = live_slot(*state_, handle);
    return slot != nullptr ? slot->status.load(std::memory_order_acquire) : ReadStatus::Invalid;
}

core::Result<size_t> Vfs::finish(ReadHandle handle) {
    VfsState& state = *state_;
    RequestSlot* slot = live_slot(state, handle);
    if (slot == nullptr) {
        return fail(ErrorCode::InvalidArgument, "unknown or already finished read");
    }
    const ReadStatus status = slot->status.load(std::memory_order_acquire);
    if (status != ReadStatus::Done && status != ReadStatus::Failed) {
        return fail(ErrorCode::InvalidArgument, "read hasn't finished: poll status() first");
    }
    const size_t bytes = slot->bytes_read;
    const core::Error error = slot->error;
    {
        const std::scoped_lock lock(state.mutex);
        release_slot(state, handle.index);
    }
    if (status == ReadStatus::Failed) {
        return fail(error);
    }
    return bytes;
}

CancelResult Vfs::cancel(ReadHandle handle) {
    VfsState& state = *state_;
    // Under the queue lock, a Pending read can't become InFlight: readers take it under the same lock.
    const std::scoped_lock lock(state.mutex);
    RequestSlot* slot = live_slot(state, handle);
    if (slot == nullptr) {
        return CancelResult::UnknownHandle;
    }
    switch (slot->status.load(std::memory_order_acquire)) {
    case ReadStatus::Pending: {
        // Still queued: kill the handle now; the reader that dequeues it frees the slot.
        slot->cancelled = true;
        const uint32_t next_generation = slot->generation.load(std::memory_order_relaxed) + 1;
        slot->generation.store(next_generation == 0 ? 1 : next_generation, std::memory_order_relaxed);
        return CancelResult::NotStarted;
    }
    case ReadStatus::InFlight:
        return CancelResult::InFlight;
    case ReadStatus::Done:
    case ReadStatus::Failed:
        release_slot(state, handle.index);
        return CancelResult::DiscardedResult;
    case ReadStatus::Invalid:
        break;
    }
    return CancelResult::UnknownHandle;
}

IoStats Vfs::stats() const {
    VfsState& state = *state_;
    IoStats stats;
    stats.reads_completed = state.reads_completed.load(std::memory_order_relaxed);
    stats.reads_failed = state.reads_failed.load(std::memory_order_relaxed);
    stats.bytes_read = state.bytes_read.load(std::memory_order_relaxed);
    stats.total_latency_ns = state.latency_ns.load(std::memory_order_relaxed);
    const std::scoped_lock lock(state.mutex);
    for (size_t p = 0; p < kIoPriorityCount; ++p) {
        stats.pending[p] = state.queue_size[p];
    }
    return stats;
}

} // namespace de::platform
