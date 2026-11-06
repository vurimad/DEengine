#include "platform/vfs.h"

#include "core/log.h"
#include "core/profile.h"
#include "platform/thread.h"
#include "platform/time.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
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

// Windows treats these names as devices in any folder, with or without an extension
// ("con.txt" is the console), so a path using one would mean different things per OS.
bool is_reserved_name(std::string_view segment) {
    const std::string_view stem = segment.substr(0, segment.find('.'));
    for (const std::string_view name : {"con", "prn", "aux", "nul"}) {
        if (stem == name) {
            return true;
        }
    }
    const bool numbered_device =
        stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '0' && stem[3] <= '9';
    return numbered_device;
}

// "game:/characters/knight.demesh" -> Game + "characters/knight.demesh", enforcing the path rules
// (docs/design.md, File system & IO): lowercase, forward slashes, no empty, "." or ".." segments,
// and nothing that means something else on another OS (device names, a trailing dot).
// allow_root: "src:/" alone names the mount's root folder (for listing).
core::Result<ParsedPath> parse_path(const char* path, bool allow_root = false) {
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
        {.text = "src:/", .mount = Mount::Src},
    };
    for (const Prefix& prefix : kPrefixes) {
        if (!text.starts_with(prefix.text)) {
            continue;
        }
        const std::string_view relative = text.substr(prefix.text.size());
        if (relative.empty() && allow_root) {
            return ParsedPath{.mount = prefix.mount, .relative = relative};
        }
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
            if (segment.back() == '.') {
                return fail(ErrorCode::InvalidArgument, "path segment ends with a dot (Windows drops it)");
            }
            if (is_reserved_name(segment)) {
                return fail(ErrorCode::InvalidArgument, "path uses a name Windows reserves for devices");
            }
            segment_start = i + 1;
        }
        return ParsedPath{.mount = prefix.mount, .relative = relative};
    }
    return fail(ErrorCode::InvalidArgument, "path must start with game:/, user:/, temp:/ or src:/");
}

core::Result<PathString> join(const PathString& root, std::string_view relative) {
    PathString path = root;
    if (!path.append('/') || !path.append(relative)) {
        return fail(ErrorCode::InvalidArgument, "path is too long");
    }
    return path;
}

// One separator style in roots, logs and tools: forward slashes work on every OS.
PathString normalized(PathString path) {
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '\\') {
            path[i] = '/';
        }
    }
    path.trim_trailing('/', '/', 1);
    return path;
}

bool append_number(PathString& path, uint32_t value) {
    char digits[10];
    size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    bool ok = true;
    while (count > 0) {
        ok = path.append(digits[--count]) && ok;
    }
    return ok;
}

struct RequestSlot {
    std::atomic<ReadStatus> status{ReadStatus::Invalid};
    std::atomic<uint32_t> generation{1};

    // Written under the queue lock while queued, then owned by one reader thread.
    PathString native_path;
    std::span<std::byte> dest;
    uint64_t offset = 0;
    uint64_t size = kWholeFile;
    IoPriority priority = IoPriority::Near;
    ReadCallback on_complete = nullptr;
    void* context = nullptr;
    core::JobCounter* counter = nullptr;
    uint64_t submit_ns = 0;

    // Queue links while Pending (intrusive, so cancel() unlinks in O(1)); free-list link otherwise.
    uint32_t prev = kNoSlot;
    uint32_t next = kNoSlot;

    // Written by the reader before status becomes Done or Failed.
    size_t bytes_read = 0;
    core::Error error;

    void* owner = nullptr; // the VfsState, for the completion job
};

void bump_generation(RequestSlot& slot) {
    const uint32_t next = slot.generation.load(std::memory_order_relaxed) + 1;
    slot.generation.store(next == 0 ? 1 : next, std::memory_order_release); // 0 marks the invalid handle
}

struct Queue {
    uint32_t head = kNoSlot;
    uint32_t tail = kNoSlot;
    uint32_t size = 0;
};

} // namespace

struct VfsState {
    core::Allocator* allocator = nullptr;
    PathString roots[kMountCount]; // empty: not mounted
    bool game_writable = false;
    core::JobSystem* jobs = nullptr;

    RequestSlot* slots = nullptr;
    uint32_t slot_count = 0;

    // Everything below up to `stopping` is guarded by `mutex`: the one lock on the request queue
    // the design allows (docs/design.md, Performance rules).
    std::mutex mutex;
    std::condition_variable work;
    Queue queues[kIoPriorityCount];
    uint32_t free_head = kNoSlot;
    bool stopping = false;
    bool paused = false;

    Thread* readers = nullptr;
    uint32_t reader_count = 0;

    std::atomic<uint64_t> reads_completed{0};
    std::atomic<uint64_t> reads_failed{0};
    std::atomic<uint64_t> bytes_read{0};
    std::atomic<uint64_t> latency_ns{0};
};

namespace {

// The queue helpers below run with the lock held.

void push_queued(VfsState& state, uint32_t index) {
    RequestSlot& slot = state.slots[index];
    Queue& queue = state.queues[static_cast<size_t>(slot.priority)];
    slot.prev = queue.tail;
    slot.next = kNoSlot;
    if (queue.tail != kNoSlot) {
        state.slots[queue.tail].next = index;
    } else {
        queue.head = index;
    }
    queue.tail = index;
    ++queue.size;
}

void unlink_queued(VfsState& state, uint32_t index) {
    RequestSlot& slot = state.slots[index];
    Queue& queue = state.queues[static_cast<size_t>(slot.priority)];
    (slot.prev != kNoSlot ? state.slots[slot.prev].next : queue.head) = slot.next;
    (slot.next != kNoSlot ? state.slots[slot.next].prev : queue.tail) = slot.prev;
    slot.prev = kNoSlot;
    slot.next = kNoSlot;
    --queue.size;
}

// Highest priority first; first in, first out within a priority.
uint32_t pop_queued(VfsState& state) {
    for (const Queue& queue : state.queues) {
        if (queue.head != kNoSlot) {
            const uint32_t index = queue.head;
            unlink_queued(state, index);
            return index;
        }
    }
    return kNoSlot;
}

bool any_queued(const VfsState& state) {
    for (const Queue& queue : state.queues) {
        if (queue.head != kNoSlot) {
            return true;
        }
    }
    return false;
}

void release_slot(VfsState& state, uint32_t index) {
    RequestSlot& slot = state.slots[index];
    bump_generation(slot);
    slot.status.store(ReadStatus::Invalid, std::memory_order_release);
    slot.on_complete = nullptr;
    slot.counter = nullptr;
    slot.next = state.free_head;
    state.free_head = index;
}

RequestSlot* live_slot(VfsState& state, ReadHandle handle) {
    if (!handle.is_valid() || handle.index >= state.slot_count) {
        return nullptr;
    }
    RequestSlot& slot = state.slots[handle.index];
    return slot.generation.load(std::memory_order_acquire) == handle.generation ? &slot : nullptr;
}

core::Result<size_t> result_of(const RequestSlot& slot) {
    if (slot.status.load(std::memory_order_acquire) == ReadStatus::Failed) {
        return fail(slot.error);
    }
    return slot.bytes_read;
}

// The completion job: hands the result to the callback, then frees the slot. The slot stays
// allocated until then, so the job's context pointer can't dangle.
void deliver_result(void* context) {
    RequestSlot& slot = *static_cast<RequestSlot*>(context);
    auto& state = *static_cast<VfsState*>(slot.owner);
    slot.on_complete(slot.context, result_of(slot));
    const std::scoped_lock lock(state.mutex);
    release_slot(state, static_cast<uint32_t>(&slot - state.slots));
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
    // A read with a callback belongs to its completion job from here on: finish() and cancel()
    // leave it alone, and the job frees it.
    const bool has_callback = slot.on_complete != nullptr;
    core::JobCounter* counter = slot.counter;
    slot.status.store(ok ? ReadStatus::Done : ReadStatus::Failed, std::memory_order_release);
    if (has_callback) {
        state.jobs->submit_reserved(
            core::Job{.entry = &deliver_result, .context = &slot, .counter = counter, .name = "vfs read done"});
    }
}

void reader_main(void* context) {
    VfsState& state = *static_cast<VfsState*>(context);
    for (;;) {
        RequestSlot* slot = nullptr;
        {
            std::unique_lock lock(state.mutex);
            state.work.wait(lock, [&state] { return state.stopping || (!state.paused && any_queued(state)); });
            if (state.stopping) {
                return;
            }
            slot = &state.slots[pop_queued(state)];
            slot->status.store(ReadStatus::InFlight, std::memory_order_release);
        }
        perform_read(state, *slot);
    }
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

// temp:/ is <base>/deengine_temp/<process id>, so instances (game and editor) never clear each
// other's files. Folders of processes that are gone are cleaned up on the way.
core::Result<PathString> prepare_temp_root(const char* configured) {
    const core::Result<PathString> parent = owned_root(configured, &system_temp_folder, "deengine_temp");
    if (!parent) {
        return fail(parent.error());
    }
    if (const core::Result<void> created = create_directories(parent->c_str()); !created) {
        return fail(created.error());
    }
    static_cast<void>(list_directory(parent->c_str(), [&](const char* name, bool is_directory) {
        char* end = nullptr;
        const unsigned long process = std::strtoul(name, &end, 10);
        const bool numeric = is_directory && end != name && *end == '\0';
        if (numeric && static_cast<uint32_t>(process) != current_process_id() &&
            !process_alive(static_cast<uint32_t>(process))) {
            PathString stale = *parent;
            if (stale.append('/') && stale.append(name)) {
                static_cast<void>(delete_tree(stale.c_str())); // best effort
            }
        }
    }));

    PathString root = *parent;
    if (!root.append('/') || !append_number(root, current_process_id())) {
        return fail(ErrorCode::InvalidArgument, "temp path is too long");
    }
    // Our own folder starts empty. If something in it can't be deleted, keep going.
    if (!delete_tree(root.c_str()).ok()) {
        DE_LOG_WARN("vfs", "temp:/ could not be emptied completely: %s", root.c_str());
    }
    if (const core::Result<void> created = create_directories(root.c_str()); !created) {
        return fail(created.error());
    }
    return root;
}

} // namespace

core::Result<PathString> working_directory() {
    char* folder = SDL_GetCurrentDirectory();
    if (folder == nullptr) {
        return fail(ErrorCode::IoError, "the working directory isn't known");
    }
    const PathString path = normalized(PathString(std::string_view(folder)));
    SDL_free(folder);
    return path;
}

core::Result<Vfs> Vfs::create(const VfsDesc& desc) {
    if (desc.game_root == nullptr) {
        return fail(ErrorCode::InvalidArgument, "VfsDesc::game_root is required");
    }
    DE_VERIFY_MSG(desc.max_reads > 0 && desc.max_reads < kNoSlot && desc.reader_threads > 0,
                  "the Vfs needs reader threads and read slots");

    const PathString game_root = normalized(PathString(desc.game_root));
    const core::Result<PathString> user_root = owned_root(desc.user_root, &user_data_folder, "DEengine");
    const core::Result<PathString> temp_root = prepare_temp_root(desc.temp_root);
    if (!user_root || !temp_root || !create_directories(user_root->c_str()).ok()) {
        DE_LOG_ERROR("vfs", "user:/ or temp:/ could not be prepared");
        return fail(ErrorCode::IoError, "user:/ or temp:/ could not be prepared");
    }
    if (!path_exists(game_root.c_str())) {
        DE_LOG_WARN("vfs", "game:/ folder %s doesn't exist yet: reads from game:/ will fail", game_root.c_str());
    }
    const PathString src_root = desc.src_root != nullptr ? normalized(PathString(desc.src_root)) : PathString();
    if (desc.src_root != nullptr && (src_root.empty() || !path_exists(src_root.c_str()))) {
        DE_LOG_ERROR("vfs", "src:/ folder %s doesn't exist", src_root.c_str());
        return fail(ErrorCode::NotFound, "the src:/ folder doesn't exist");
    }

    core::Allocator& allocator =
        desc.allocator != nullptr ? *desc.allocator : core::heap_allocator(core::MemoryTag::Platform);
    auto* state = ::new (allocator.allocate(sizeof(VfsState), alignof(VfsState))) VfsState();
    state->allocator = &allocator;
    state->roots[static_cast<size_t>(Mount::Game)] = game_root;
    state->roots[static_cast<size_t>(Mount::User)] = *user_root;
    state->roots[static_cast<size_t>(Mount::Temp)] = *temp_root;
    state->roots[static_cast<size_t>(Mount::Src)] = src_root;
    state->game_writable = desc.game_writable;
    state->jobs = desc.jobs;

    state->slot_count = desc.max_reads;
    state->slots = core::allocate_array<RequestSlot>(allocator, state->slot_count);
    for (uint32_t i = 0; i < state->slot_count; ++i) {
        auto* slot = ::new (static_cast<void*>(&state->slots[i])) RequestSlot();
        slot->owner = state;
        slot->next = i + 1 < state->slot_count ? i + 1 : kNoSlot;
    }
    state->free_head = 0;

    state->reader_count = desc.reader_threads;
    state->readers = core::allocate_array<Thread>(allocator, state->reader_count);
    for (uint32_t i = 0; i < state->reader_count; ++i) {
        ::new (static_cast<void*>(&state->readers[i])) Thread("vfs reader", &reader_main, state);
    }

    DE_LOG_INFO("vfs", "game:/ = %s", game_root.c_str());
    DE_LOG_INFO("vfs", "user:/ = %s", user_root->c_str());
    DE_LOG_INFO("vfs", "temp:/ = %s", temp_root->c_str());
    if (!src_root.empty()) {
        DE_LOG_INFO("vfs", "src:/ = %s", src_root.c_str());
    }

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
    const PathString& root = state_->roots[static_cast<size_t>(parsed->mount)];
    if (root.empty()) {
        return fail(ErrorCode::NotFound, "src:/ isn't mounted");
    }
    return join(root, parsed->relative);
}

core::JobSystem* Vfs::jobs() const {
    return state_->jobs;
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

core::Result<FileInfo> Vfs::file_info(const char* path) const {
    const core::Result<PathString> native = resolve(path);
    if (!native) {
        return fail(native.error());
    }
    return platform::file_info(native->c_str());
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

core::Result<core::Array<std::byte>> Vfs::read_whole_file(const char* path, core::Allocator& allocator) const {
    const core::Result<uint64_t> size = file_size(path);
    if (!size) {
        return fail(size.error());
    }
    core::Array<std::byte> bytes(allocator);
    bytes.resize_uninit(static_cast<size_t>(*size));
    const core::Result<size_t> read = read_file(path, bytes.span());
    if (!read) {
        return fail(read.error());
    }
    if (*read != bytes.size()) {
        return fail(ErrorCode::IoError, "the file changed size while it was read");
    }
    return bytes;
}

core::Result<void> Vfs::write_file(const char* path, std::span<const std::byte> data) const {
    DE_PROFILE_ZONE("vfs_write_file");
    const core::Result<ParsedPath> parsed = parse_path(path);
    if (!parsed) {
        return fail(parsed.error());
    }
    if (parsed->mount == Mount::Game && !state_->game_writable) {
        return fail(ErrorCode::Unsupported, "game:/ is read only");
    }
    const core::Result<PathString> native = resolve(path);
    if (!native) {
        return fail(native.error());
    }
    PathString folder = *native;
    folder.truncate(native->view().find_last_of('/'));
    if (const core::Result<void> created = create_directories(folder.c_str()); !created) {
        return created;
    }
    return write_file_atomic(native->c_str(), data);
}

core::Result<void> Vfs::remove_file(const char* path) const {
    const core::Result<ParsedPath> parsed = parse_path(path);
    if (!parsed) {
        return fail(parsed.error());
    }
    if (parsed->mount == Mount::Game && !state_->game_writable) {
        return fail(ErrorCode::Unsupported, "game:/ is read only");
    }
    const core::Result<PathString> native = resolve(path);
    if (!native) {
        return fail(native.error());
    }
    const core::Result<FileInfo> info = platform::file_info(native->c_str());
    if (!info) {
        return info.error().code == ErrorCode::NotFound ? core::Result<void>() : fail(info.error());
    }
    if (info->is_directory) {
        return fail(ErrorCode::InvalidArgument, "remove_file() removes files, not folders");
    }
    return delete_tree(native->c_str());
}

core::Result<void> Vfs::list(const char* folder,
                             core::FunctionRef<void(const char* name, bool is_directory)> visit) const {
    const core::Result<ParsedPath> parsed = parse_path(folder, true);
    if (!parsed) {
        return fail(parsed.error());
    }
    const PathString& root = state_->roots[static_cast<size_t>(parsed->mount)];
    if (root.empty()) {
        return fail(ErrorCode::NotFound, "src:/ isn't mounted");
    }
    if (parsed->relative.empty()) {
        return list_directory(root.c_str(), visit);
    }
    const core::Result<PathString> native = join(root, parsed->relative);
    if (!native) {
        return fail(native.error());
    }
    return list_directory(native->c_str(), visit);
}

core::Result<ReadHandle> Vfs::read(const ReadRequest& request) {
    VfsState& state = *state_;
    if (request.on_complete == nullptr && request.counter != nullptr) {
        return fail(ErrorCode::InvalidArgument, "a read counter needs a completion callback");
    }
    if (request.on_complete != nullptr && state.jobs == nullptr) {
        return fail(ErrorCode::InvalidArgument, "completion callbacks need VfsDesc::jobs");
    }
    const core::Result<PathString> native = resolve(request.path);
    if (!native) {
        return fail(native.error());
    }

    ReadHandle handle;
    {
        const std::scoped_lock lock(state.mutex);
        if (state.free_head == kNoSlot) {
            return fail(ErrorCode::OutOfRange, "too many reads in flight (VfsDesc::max_reads)");
        }
        const uint32_t index = state.free_head;
        RequestSlot& slot = state.slots[index];
        state.free_head = slot.next;

        slot.native_path = *native;
        slot.dest = request.dest;
        slot.offset = request.offset;
        slot.size = request.size;
        slot.priority = request.priority;
        slot.on_complete = request.on_complete;
        slot.context = request.context;
        slot.counter = request.counter;
        slot.submit_ns = time_ns();
        if (slot.counter != nullptr) {
            state.jobs->reserve(*slot.counter); // counted now, so waiting right after read() works
        }
        slot.status.store(ReadStatus::Pending, std::memory_order_release);
        push_queued(state, index);
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
    // Checked and released under the lock, so a concurrent cancel() or finish() can't free the
    // slot twice.
    const std::scoped_lock lock(state.mutex);
    RequestSlot* slot = live_slot(state, handle);
    if (slot == nullptr) {
        return fail(ErrorCode::InvalidArgument, "unknown or already finished read");
    }
    if (slot->on_complete != nullptr) {
        return fail(ErrorCode::InvalidArgument, "this read finishes through its completion callback");
    }
    const ReadStatus status = slot->status.load(std::memory_order_acquire);
    if (status != ReadStatus::Done && status != ReadStatus::Failed) {
        return fail(ErrorCode::InvalidArgument, "read hasn't finished: poll status() first");
    }
    const core::Result<size_t> result = result_of(*slot);
    release_slot(state, handle.index);
    return result;
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
    case ReadStatus::Pending:
        // Unlinked and freed right away, so cancelled reads never pile up behind busier priorities.
        unlink_queued(state, handle.index);
        if (slot->counter != nullptr) {
            state.jobs->release_reserved(*slot->counter); // the callback will never run
        }
        release_slot(state, handle.index);
        return CancelResult::NotStarted;
    case ReadStatus::InFlight:
        return CancelResult::InFlight;
    case ReadStatus::Done:
    case ReadStatus::Failed:
        if (slot->on_complete != nullptr) {
            return CancelResult::InFlight; // its completion callback is still due
        }
        release_slot(state, handle.index);
        return CancelResult::DiscardedResult;
    case ReadStatus::Invalid:
        break;
    }
    return CancelResult::UnknownHandle;
}

void Vfs::pause() {
    const std::scoped_lock lock(state_->mutex);
    state_->paused = true;
}

void Vfs::resume() {
    {
        const std::scoped_lock lock(state_->mutex);
        state_->paused = false;
    }
    state_->work.notify_all();
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
        stats.pending[p] = state.queues[p].size;
    }
    return stats;
}

} // namespace de::platform
