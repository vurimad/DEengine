#include "asset/registry.h"

#include "asset/cell_format.h"
#include "asset/material_format.h"
#include "asset/mesh_format.h"
#include "asset/shader_format.h"
#include "asset/texture_format.h"
#include "core/allocator.h"
#include "core/array.h"
#include "core/hash_map.h"
#include "core/inline_array.h"
#include "core/job_system.h"
#include "core/log.h"
#include "core/profile.h"

#include <algorithm>
#include <mutex>
#include <new>
#include <utility>

namespace de::asset {

namespace {

core::Allocator& streaming_heap() {
    return core::heap_allocator(core::MemoryTag::Streaming);
}

// A cooked file in memory aligned as load_in_place needs.
struct Buffer {
    std::byte* data = nullptr;
    size_t size = 0;
};

Buffer allocate_buffer(size_t size) {
    return {.data = static_cast<std::byte*>(streaming_heap().allocate(std::max<size_t>(size, 1), kAssetAlignment)),
            .size = size};
}

void free_buffer(Buffer& buffer) {
    if (buffer.data != nullptr) {
        streaming_heap().deallocate(buffer.data, std::max<size_t>(buffer.size, 1), kAssetAlignment);
    }
    buffer = {};
}

// load_in_place for the asset's type: everything the engine reads from the file is checked here,
// on a job, before it reaches the main thread.
core::Result<void> check(AssetType type, std::span<const std::byte> file) {
    const auto checked = [&](const auto& loaded) -> core::Result<void> {
        if (!loaded) {
            return core::fail(loaded.error());
        }
        return {};
    };
    switch (type) {
    case AssetType::Mesh:
        return checked(load_in_place<MeshAsset>(file));
    case AssetType::Texture:
        return checked(load_in_place<TextureAsset>(file));
    case AssetType::Material:
        return checked(load_in_place<MaterialAsset>(file));
    case AssetType::Shader:
        return checked(load_in_place<ShaderAsset>(file));
    case AssetType::Cell:
        return checked(load_in_place<CellAsset>(file));
    case AssetType::Unknown:
        break;
    }
    return core::fail(core::ErrorCode::InvalidArgument, "unknown asset type");
}

bool same_file(const platform::FileInfo& a, const platform::FileInfo& b) {
    return a.size == b.size && a.modified_time == b.modified_time;
}

} // namespace

struct Entry {
    RegistryState* registry = nullptr;
    AssetHandle handle;
    AssetRef asset;
    platform::IoPriority priority = platform::IoPriority::Near;
    uint32_t refs = 0;
    AssetState state = AssetState::Loading;
    bool file_loaded = false;   // its own file is in; dependencies may still be loading
    bool queued_unload = false; // in RegistryState::unload_queue
    Buffer data;
    platform::FileInfo info{}; // of the file loaded or last tried: hot reload compares against it
    core::Array<AssetHandle> dependencies{streaming_heap()};
    core::Array<AssetHandle> waiting{streaming_heap()}; // assets waiting for this one to be Ready or Failed
    uint32_t dependencies_pending = 0;

    // A reload whose file is in and checked, waiting for its new dependencies before it's swapped
    // in: until then the loaded version stays, so Ready keeps meaning "dependencies settled".
    bool reload_waiting = false;
    Buffer reload_data;
    platform::FileInfo reload_info{};
    core::Array<AssetHandle> reload_dependencies{streaming_heap()};
    uint32_t reload_pending = 0;

    // The read in flight owns these until update() commits it.
    bool in_flight = false;
    bool reloading = false;
    Buffer pending;
    platform::FileInfo pending_info{};
    const char* pending_error = nullptr; // null: the file is in and checked

    bool busy() const { return in_flight || reload_waiting || state == AssetState::Loading; }
};

namespace {

// One file checked by the hot reload poll, off the main thread.
struct PollItem {
    AssetHandle handle;
    CookedPath path;
    platform::FileInfo before{}; // what the entry had when the poll started
    platform::FileInfo now{};
    bool found = false;
};

} // namespace

struct RegistryState {
    AssetRegistryDesc desc;
    core::SlotMap<Entry*> entries{streaming_heap()};
    core::HashMap<AssetRef, AssetHandle> by_ref{streaming_heap()};
    core::JobCounter in_flight; // read jobs and their completions

    // Finished reads, from jobs: the one lock between them and the main thread. Jobs fill one
    // queue while update() commits the other; the two trade places by index, so neither array is
    // ever moved or reallocated in steady state.
    std::mutex completed_mutex;
    core::Array<Entry*> completed[2] = {core::Array<Entry*>(streaming_heap()), core::Array<Entry*>(streaming_heap())};
    uint32_t filling = 0;

    core::Array<AssetHandle> unload_queue{streaming_heap()};

    // Events wait here until update() tells listeners; listeners may load and release, and the
    // events that causes go to the other queue, told in the same update.
    core::Array<AssetNotice> notices[2] = {core::Array<AssetNotice>(streaming_heap()),
                                           core::Array<AssetNotice>(streaming_heap())};
    uint32_t notices_filling = 0;
    bool telling = false;
    struct Listener {
        AssetListener function = nullptr;
        void* context = nullptr;
    };
    Listener listeners[AssetRegistry::kMaxListeners] = {};
    uint32_t listener_count = 0;

    // Hot reload: one job at a time asks the OS about a few held files.
    core::Array<PollItem> poll_items{streaming_heap()};
    uint32_t poll_count = 0;
    bool poll_submitted = false;
    core::JobCounter poll_counter;
    uint32_t poll_cursor = 0;

    uint32_t loading = 0;
    uint32_t failed = 0;
    uint32_t reloads = 0;
    uint64_t bytes = 0;
};

namespace {

core::SlotHandle<Entry*> to_slot(AssetHandle handle) {
    return {.index = handle.index, .generation = handle.generation};
}

AssetHandle from_slot(core::SlotHandle<Entry*> handle) {
    return {.index = handle.index, .generation = handle.generation};
}

Entry* find_entry(const RegistryState& state, AssetHandle handle) {
    Entry* const* entry = state.entries.get(to_slot(handle));
    return entry != nullptr ? *entry : nullptr;
}

void finish_read(Entry& entry) {
    RegistryState& state = *entry.registry;
    const std::scoped_lock lock(state.completed_mutex);
    state.completed[state.filling].push(&entry);
}

void read_done(void* context, const core::Result<size_t>& result) {
    DE_PROFILE_ZONE("asset check");
    Entry& entry = *static_cast<Entry*>(context);
    const std::span<const std::byte> file(entry.pending.data, entry.pending.size);
    if (!result || *result != entry.pending.size) {
        entry.pending_error = "the file could not be read whole (changed while read?)";
    } else if (const core::Result<void> checked = check(entry.asset.type, file); !checked) {
        entry.pending_error = checked.error().message;
    } else if (reinterpret_cast<const AssetHeader*>(file.data())->id != entry.asset.id) {
        entry.pending_error = "the file holds another asset (stale, copied or misplaced)";
    }
    finish_read(entry);
}

void start_read(void* context) {
    DE_PROFILE_ZONE("asset read");
    Entry& entry = *static_cast<Entry*>(context);
    RegistryState& state = *entry.registry;
    const CookedPath path = cooked_path(entry.asset.id, entry.asset.type);
    const core::Result<platform::FileInfo> info = state.desc.vfs->file_info(path.c_str());
    if (!info || info->is_directory) {
        entry.pending_error = "no cooked file (not cooked yet, or cooked as another type)";
        finish_read(entry);
        return;
    }
    entry.pending_info = *info;
    if (info->size < sizeof(AssetHeader) || info->size > kMaxAssetFileSize) {
        entry.pending_error = "the cooked file is too short or too long to be one";
        finish_read(entry);
        return;
    }
    entry.pending = allocate_buffer(static_cast<size_t>(info->size));
    const core::Result<platform::ReadHandle> read =
        state.desc.vfs->read({.path = path.c_str(),
                              .dest = std::span(entry.pending.data, entry.pending.size),
                              .priority = entry.priority,
                              .on_complete = &read_done,
                              .context = &entry,
                              .counter = &state.in_flight});
    if (!read) {
        entry.pending_error = read.error().message;
        finish_read(entry);
    }
}

void notify(RegistryState& state, AssetEvent event, const Entry& entry) {
    state.notices[state.notices_filling].push(
        AssetNotice{.event = event, .handle = entry.handle, .asset = entry.asset});
}

void submit_read(RegistryState& state, Entry& entry, bool reloading) {
    entry.in_flight = true;
    entry.reloading = reloading;
    entry.pending_error = nullptr;
    entry.pending_info = {};
    ++state.loading;
    state.desc.jobs->submit(
        core::Job{.entry = &start_read, .context = &entry, .counter = &state.in_flight, .name = "asset read"});
}

void log_failure(const Entry& entry, const char* what, const char* error) {
    const AssetIdText id = format_asset_id(entry.asset.id);
    const CookedPath path = cooked_path(entry.asset.id, entry.asset.type);
    DE_LOG_ERROR("asset", "%s %s %s (%s): %s", what, asset_type_name(entry.asset.type), id.c_str(), path.c_str(),
                 error);
}

void queue_unload(RegistryState& state, Entry& entry) {
    if (entry.refs == 0 && !entry.queued_unload) {
        entry.queued_unload = true;
        state.unload_queue.push(entry.handle);
    }
}

AssetHandle acquire(RegistryState& state, AssetRef asset, platform::IoPriority priority);
void drop(RegistryState& state, AssetHandle handle);
void become_ready(RegistryState& state, Entry& entry);
void finish_reload(RegistryState& state, Entry& entry);

// The asset reached Ready or Failed: assets waiting on it, to load or to reload, count one fewer.
void resolve_waiting(RegistryState& state, Entry& entry) {
    for (const AssetHandle handle : entry.waiting) {
        Entry* parent = find_entry(state, handle);
        if (parent == nullptr) {
            continue;
        }
        if (parent->state == AssetState::Loading && parent->dependencies_pending > 0) {
            if (--parent->dependencies_pending == 0 && parent->file_loaded) {
                become_ready(state, *parent);
            }
        } else if (parent->reload_waiting && parent->reload_pending > 0) {
            if (--parent->reload_pending == 0) {
                finish_reload(state, *parent);
            }
        }
    }
    entry.waiting.clear();
}

void become_ready(RegistryState& state, Entry& entry) {
    entry.state = AssetState::Ready;
    notify(state, AssetEvent::Ready, entry);
    resolve_waiting(state, entry);
}

// Whether target is among from's dependencies, directly or through others: linking target -> from
// would close a cycle, and two assets holding each other would never be unloaded.
bool reaches(const RegistryState& state, AssetHandle from, AssetHandle target) {
    core::InlineArray<AssetHandle, 32> stack(streaming_heap());
    stack.push(from);
    for (uint32_t visited = 0; !stack.empty() && visited < 4096; ++visited) {
        const AssetHandle handle = stack.back();
        stack.pop();
        if (handle == target) {
            return true;
        }
        const Entry* entry = find_entry(state, handle);
        if (entry == nullptr) {
            continue;
        }
        for (const AssetHandle dependency : entry->dependencies) {
            stack.push(dependency);
        }
        for (const AssetHandle dependency : entry->reload_dependencies) {
            stack.push(dependency);
        }
    }
    return false;
}

// Holds what a header lists for entry, into `into`; those still loading make it wait, counted in
// `pending`. A dependency that is the asset itself, or that depends on it, would close a cycle: it
// is logged and skipped (the cooker writes no cycles; a corrupt file might).
void link_dependencies(RegistryState& state, Entry& entry, const Buffer& file, core::Array<AssetHandle>& into,
                       uint32_t& pending) {
    const auto* header = reinterpret_cast<const AssetHeader*>(file.data);
    for (const AssetRef& ref : header->dependencies.get(file.data)) {
        const AssetHandle* known = state.by_ref.find(ref);
        if (ref == entry.asset || (known != nullptr && reaches(state, *known, entry.handle))) {
            log_failure(entry, "skipping a dependency of", "it would make a cycle (a corrupt file?)");
            continue;
        }
        const AssetHandle handle = acquire(state, ref, entry.priority);
        into.push(handle);
        Entry* dependency = find_entry(state, handle);
        if (dependency != nullptr && dependency->state == AssetState::Loading) {
            dependency->waiting.push(entry.handle);
            ++pending;
        }
    }
}

// A reload whose dependencies are settled: swapped in place. The handle stays; the old file and
// the dependencies it held go.
void finish_reload(RegistryState& state, Entry& entry) {
    state.bytes -= entry.data.size;
    free_buffer(entry.data);
    entry.data = std::exchange(entry.reload_data, Buffer{});
    entry.info = entry.reload_info;
    state.bytes += entry.data.size;
    const core::Array<AssetHandle> old = std::move(entry.dependencies);
    entry.dependencies = std::move(entry.reload_dependencies);
    entry.reload_dependencies = core::Array<AssetHandle>(streaming_heap());
    entry.reload_waiting = false;
    for (const AssetHandle dependency : old) {
        drop(state, dependency);
    }
    ++state.reloads;
    DE_LOG_INFO("asset", "reloaded %s %s", asset_type_name(entry.asset.type), format_asset_id(entry.asset.id).c_str());
    notify(state, AssetEvent::Reloaded, entry);
}

void commit(RegistryState& state, Entry& entry) {
    entry.in_flight = false;
    --state.loading;
    if (entry.reloading) {
        entry.reloading = false;
        if (entry.pending_error != nullptr) {
            entry.info = entry.pending_info; // a broken version isn't retried until the file changes again
            log_failure(entry, "reloading", entry.pending_error);
            DE_LOG_WARN("asset", "keeping the loaded version of %s", format_asset_id(entry.asset.id).c_str());
            free_buffer(entry.pending);
        } else {
            // The new version waits for its new dependencies; they're held before the old ones go,
            // so dependencies both versions share stay loaded.
            entry.reload_data = std::exchange(entry.pending, Buffer{});
            entry.reload_info = entry.pending_info;
            entry.info = entry.pending_info;
            entry.reload_waiting = true;
            entry.reload_pending = 0;
            link_dependencies(state, entry, entry.reload_data, entry.reload_dependencies, entry.reload_pending);
            if (entry.reload_pending == 0) {
                finish_reload(state, entry);
            }
        }
        queue_unload(state, entry);
        return;
    }
    entry.info = entry.pending_info;
    if (entry.pending_error != nullptr) {
        log_failure(entry, "can't load", entry.pending_error);
        free_buffer(entry.pending);
        entry.state = AssetState::Failed;
        ++state.failed;
        notify(state, AssetEvent::Failed, entry);
        resolve_waiting(state, entry);
        queue_unload(state, entry);
        return;
    }
    entry.data = std::exchange(entry.pending, Buffer{});
    state.bytes += entry.data.size;
    entry.file_loaded = true;
    link_dependencies(state, entry, entry.data, entry.dependencies, entry.dependencies_pending);
    if (entry.dependencies_pending == 0) {
        become_ready(state, entry);
    }
    queue_unload(state, entry);
}

// Commits the reads finished so far; false if there were none.
bool commit_finished(RegistryState& state) {
    uint32_t taken = 0;
    {
        const std::scoped_lock lock(state.completed_mutex);
        taken = state.filling;
        state.filling ^= 1U;
    }
    core::Array<Entry*>& committing = state.completed[taken];
    if (committing.empty()) {
        return false;
    }
    DE_PROFILE_ZONE("asset commit");
    for (Entry* entry : committing) {
        commit(state, *entry);
    }
    committing.clear();
    return true;
}

AssetHandle acquire(RegistryState& state, AssetRef asset, platform::IoPriority priority) {
    if (const AssetHandle* held = state.by_ref.find(asset); held != nullptr) {
        ++find_entry(state, *held)->refs;
        return *held;
    }
    auto* entry = ::new (streaming_heap().allocate(sizeof(Entry), alignof(Entry))) Entry();
    entry->registry = &state;
    entry->asset = asset;
    entry->priority = priority;
    entry->refs = 1;
    entry->handle = from_slot(state.entries.insert(entry));
    state.by_ref.set(asset, entry->handle);
    if (!asset.id.is_valid() || asset_type_extension(asset.type)[0] == '\0') {
        log_failure(*entry, "can't load", "invalid asset ID or type");
        entry->state = AssetState::Failed;
        ++state.failed;
        notify(state, AssetEvent::Failed, *entry);
        return entry->handle;
    }
    submit_read(state, *entry, false);
    return entry->handle;
}

void drop(RegistryState& state, AssetHandle handle) {
    Entry* entry = find_entry(state, handle);
    DE_ASSERT_MSG(entry == nullptr || entry->refs > 0, "an asset released more often than loaded");
    if (entry == nullptr || entry->refs == 0) {
        return;
    }
    --entry->refs;
    queue_unload(state, *entry);
}

// Frees an entry and everything it owns: the unload path and the registry's destructor.
void destroy_entry(Entry* entry) {
    free_buffer(entry->data);
    free_buffer(entry->pending);
    free_buffer(entry->reload_data);
    entry->~Entry();
    streaming_heap().deallocate(entry, sizeof(Entry), alignof(Entry));
}

// Assets nobody holds, and what they held in turn (the queue grows as it goes).
void unload_released(RegistryState& state) {
    if (state.unload_queue.empty()) {
        return;
    }
    DE_PROFILE_ZONE("asset unload");
    for (size_t i = 0; i < state.unload_queue.size(); ++i) {
        Entry* entry = find_entry(state, state.unload_queue[i]);
        if (entry == nullptr) {
            continue;
        }
        entry->queued_unload = false;
        if (entry->refs > 0 || entry->in_flight) {
            continue; // held again, or queued again when its read is committed
        }
        for (const AssetHandle dependency : entry->dependencies) {
            drop(state, dependency);
        }
        for (const AssetHandle dependency : entry->reload_dependencies) {
            drop(state, dependency);
        }
        state.bytes -= entry->data.size;
        state.failed -= entry->state == AssetState::Failed ? 1 : 0;
        notify(state, AssetEvent::Unloaded, *entry);
        state.by_ref.remove(entry->asset);
        state.entries.remove(to_slot(entry->handle));
        destroy_entry(entry);
    }
    state.unload_queue.clear();
}

void poll_job(void* context) {
    DE_PROFILE_ZONE("asset hot reload poll");
    RegistryState& state = *static_cast<RegistryState*>(context);
    for (uint32_t i = 0; i < state.poll_count; ++i) {
        PollItem& item = state.poll_items[i];
        const core::Result<platform::FileInfo> info = state.desc.vfs->file_info(item.path.c_str());
        item.found = info.ok() && !info->is_directory;
        item.now = item.found ? *info : platform::FileInfo{};
    }
}

// Hot reload, off the main thread: a job asks the OS about a few held files (round robin), and
// the next update() acts on its answers. A changed file is read again; a failed asset whose file
// appeared or changed is tried again; a deleted file keeps the loaded version.
void poll_changes(RegistryState& state) {
    if (!state.desc.hot_reload) {
        return;
    }
    if (state.poll_submitted) {
        if (!state.poll_counter.done()) {
            return;
        }
        state.poll_submitted = false;
        for (uint32_t i = 0; i < state.poll_count; ++i) {
            const PollItem& item = state.poll_items[i];
            Entry* entry = find_entry(state, item.handle);
            if (entry == nullptr || entry->refs == 0 || entry->busy() || !same_file(entry->info, item.before) ||
                !item.found || same_file(item.now, item.before)) {
                continue;
            }
            if (entry->state == AssetState::Failed) {
                entry->state = AssetState::Loading;
                --state.failed;
                submit_read(state, *entry, false);
            } else {
                submit_read(state, *entry, true);
            }
        }
    }
    const std::span<Entry*> all = state.entries.values();
    const size_t checks = std::min<size_t>(state.poll_items.size(), all.size());
    state.poll_count = 0;
    for (size_t i = 0; i < checks; ++i) {
        state.poll_cursor = static_cast<uint32_t>(state.poll_cursor % all.size());
        const Entry& entry = *all[state.poll_cursor++];
        if (entry.refs == 0 || entry.busy()) {
            continue;
        }
        PollItem& item = state.poll_items[state.poll_count++];
        item.handle = entry.handle;
        item.path = cooked_path(entry.asset.id, entry.asset.type);
        item.before = entry.info;
    }
    if (state.poll_count > 0) {
        state.poll_submitted = true;
        state.desc.jobs->submit(
            core::Job{.entry = &poll_job, .context = &state, .counter = &state.poll_counter, .name = "asset poll"});
    }
}

void tell_listeners(RegistryState& state) {
    if (state.telling) {
        return; // a listener's own load() or release(): the loop below tells those too
    }
    state.telling = true;
    while (!state.notices[state.notices_filling].empty()) {
        const uint32_t current = state.notices_filling;
        state.notices_filling ^= 1U; // what listeners cause goes to the other queue
        // Listeners push to the other queue only, so this one doesn't change while it's told.
        for (const AssetNotice& notice : state.notices[current]) {
            for (uint32_t l = 0; l < state.listener_count; ++l) {
                state.listeners[l].function(state.listeners[l].context, notice);
            }
        }
        state.notices[current].clear();
    }
    state.telling = false;
}

} // namespace

core::Result<AssetRegistry> AssetRegistry::create(const AssetRegistryDesc& desc) {
    if (desc.vfs == nullptr || desc.jobs == nullptr) {
        return core::fail(core::ErrorCode::InvalidArgument, "the asset registry needs a VFS and a job system");
    }
    if (desc.vfs->jobs() == nullptr) {
        return core::fail(core::ErrorCode::InvalidArgument,
                          "the asset registry's VFS needs a job system for read completions (VfsDesc::jobs)");
    }
    auto* state = ::new (streaming_heap().allocate(sizeof(RegistryState), alignof(RegistryState))) RegistryState();
    state->desc = desc;
    state->poll_items.resize(desc.hot_reload ? std::max(desc.reload_checks_per_update, 1U) : 0);
    return AssetRegistry(state);
}

AssetRegistry::~AssetRegistry() {
    if (state_ == nullptr) {
        return;
    }
    // Reads and polls in flight write into the state: let them finish first.
    state_->desc.jobs->wait(state_->in_flight);
    state_->desc.jobs->wait(state_->poll_counter);
    for (Entry* entry : state_->entries) {
        destroy_entry(entry);
    }
    state_->~RegistryState();
    streaming_heap().deallocate(state_, sizeof(RegistryState), alignof(RegistryState));
}

AssetRegistry::AssetRegistry(AssetRegistry&& other) noexcept : state_(std::exchange(other.state_, nullptr)) {}

AssetRegistry& AssetRegistry::operator=(AssetRegistry&& other) noexcept {
    if (this != &other) {
        const AssetRegistry discarded(std::exchange(state_, std::exchange(other.state_, nullptr)));
    }
    return *this;
}

AssetHandle AssetRegistry::load(AssetRef asset, platform::IoPriority priority) {
    return acquire(*state_, asset, priority);
}

void AssetRegistry::release(AssetHandle handle) {
    drop(*state_, handle);
}

AssetState AssetRegistry::state(AssetHandle handle) const {
    const Entry* entry = find_entry(*state_, handle);
    return entry != nullptr ? entry->state : AssetState::Failed;
}

AssetHandle AssetRegistry::find(AssetRef asset) const {
    const AssetHandle* held = state_->by_ref.find(asset);
    return held != nullptr && find_entry(*state_, *held)->refs > 0 ? *held : AssetHandle{};
}

AssetRef AssetRegistry::asset(AssetHandle handle) const {
    const Entry* entry = find_entry(*state_, handle);
    return entry != nullptr ? entry->asset : AssetRef{};
}

std::span<const std::byte> AssetRegistry::file(AssetHandle handle) const {
    const Entry* entry = find_entry(*state_, handle);
    if (entry == nullptr || entry->state != AssetState::Ready) {
        return {};
    }
    return {entry->data.data, entry->data.size};
}

std::span<const AssetHandle> AssetRegistry::dependencies(AssetHandle handle) const {
    const Entry* entry = find_entry(*state_, handle);
    return entry != nullptr ? entry->dependencies.span() : std::span<const AssetHandle>();
}

void AssetRegistry::update() {
    DE_PROFILE_ZONE("asset registry update");
    RegistryState& state = *state_;
    DE_ASSERT_MSG(!state.telling, "update() from an asset listener");
    commit_finished(state);
    unload_released(state);
    poll_changes(state);
    tell_listeners(state);
    DE_PROFILE_PLOT("assets", static_cast<int64_t>(state.entries.size()));
    DE_PROFILE_PLOT("assets loading", static_cast<int64_t>(state.loading));
}

void AssetRegistry::wait_all() {
    DE_PROFILE_ZONE("asset registry wait all");
    RegistryState& state = *state_;
    DE_ASSERT_MSG(!state.telling, "wait_all() from an asset listener");
    state.desc.jobs->wait(state.poll_counter); // its answers are acted on by the next update()
    while (true) {
        state.desc.jobs->wait(state.in_flight);
        if (!commit_finished(state) && state.loading == 0) {
            break;
        }
    }
    unload_released(state);
    tell_listeners(state);
}

core::Result<void> AssetRegistry::add_listener(AssetListener listener, void* context) {
    RegistryState& state = *state_;
    if (listener == nullptr || state.listener_count == kMaxListeners) {
        return core::fail(core::ErrorCode::OutOfRange, "no listener, or the registry has as many as it takes");
    }
    state.listeners[state.listener_count++] = {.function = listener, .context = context};
    return {};
}

AssetRegistryStats AssetRegistry::stats() const {
    const RegistryState& state = *state_;
    return {
        .assets = static_cast<uint32_t>(state.entries.size()),
        .loading = state.loading,
        .failed = state.failed,
        .reloads = state.reloads,
        .bytes = state.bytes,
    };
}

} // namespace de::asset
