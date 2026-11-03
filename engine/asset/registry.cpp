#include "asset/registry.h"

#include "asset/material_format.h"
#include "asset/mesh_format.h"
#include "asset/shader_format.h"
#include "asset/texture_format.h"
#include "core/allocator.h"
#include "core/array.h"
#include "core/hash_map.h"
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
    bool type_mismatch_logged = false;
    Buffer data;
    platform::FileInfo info{}; // of the file loaded or last tried: hot reload compares against it
    core::Array<AssetHandle> dependencies{streaming_heap()};
    core::Array<AssetHandle> waiting{streaming_heap()}; // assets waiting for this one to be Ready or Failed
    uint32_t dependencies_pending = 0;

    // The read in flight owns these until update() commits it.
    bool in_flight = false;
    bool reloading = false;
    Buffer pending;
    platform::FileInfo pending_info{};
    const char* pending_error = nullptr; // null: the file is in and checked
};

struct RegistryState {
    platform::Vfs* vfs = nullptr;
    core::JobSystem* jobs = nullptr;
    AssetRegistryDesc desc;
    core::SlotMap<Entry*> entries{streaming_heap()};
    core::HashMap<AssetId, AssetHandle> by_id{streaming_heap()};
    core::JobCounter in_flight; // read jobs and their completions

    // Finished reads, from jobs: the one lock between them and the main thread. Jobs fill one
    // queue while update() commits the other; the two trade places by index, so neither array is
    // ever moved or reallocated in steady state.
    std::mutex completed_mutex;
    core::Array<Entry*> completed[2] = {core::Array<Entry*>(streaming_heap()), core::Array<Entry*>(streaming_heap())};
    uint32_t filling = 0;

    core::Array<AssetHandle> unload_queue{streaming_heap()};
    core::Array<AssetNotice> notices{streaming_heap()};
    struct Listener {
        AssetListener function = nullptr;
        void* context = nullptr;
    };
    Listener listeners[AssetRegistry::kMaxListeners] = {};
    uint32_t listener_count = 0;

    uint32_t reload_cursor = 0;
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
    }
    finish_read(entry);
}

void start_read(void* context) {
    DE_PROFILE_ZONE("asset read");
    Entry& entry = *static_cast<Entry*>(context);
    RegistryState& state = *entry.registry;
    const CookedPath path = cooked_path(entry.asset.id, entry.asset.type);
    const core::Result<platform::FileInfo> info = state.vfs->file_info(path.c_str());
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
        state.vfs->read({.path = path.c_str(),
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
    state.notices.push(AssetNotice{.event = event, .handle = entry.handle, .asset = entry.asset});
}

void submit_read(RegistryState& state, Entry& entry, bool reloading) {
    entry.in_flight = true;
    entry.reloading = reloading;
    entry.pending_error = nullptr;
    entry.pending_info = {};
    ++state.loading;
    state.jobs->submit(
        core::Job{.entry = &start_read, .context = &entry, .counter = &state.in_flight, .name = "asset read"});
}

void log_failure(const Entry& entry, const char* what, const char* error) {
    const AssetIdText id = format_asset_id(entry.asset.id);
    const CookedPath path = cooked_path(entry.asset.id, entry.asset.type);
    DE_LOG_ERROR("asset", "%s %s %s (%s): %s", what, asset_type_name(entry.asset.type), id.c_str(), path.c_str(),
                 error);
}

void become_ready(RegistryState& state, Entry& entry);

// The asset reached Ready or Failed: assets waiting on it count one dependency fewer.
void resolve_waiting(RegistryState& state, Entry& entry) {
    for (const AssetHandle handle : entry.waiting) {
        Entry* parent = find_entry(state, handle);
        if (parent == nullptr || parent->state != AssetState::Loading || parent->dependencies_pending == 0) {
            continue;
        }
        if (--parent->dependencies_pending == 0 && parent->file_loaded) {
            become_ready(state, *parent);
        }
    }
    entry.waiting.clear();
}

void become_ready(RegistryState& state, Entry& entry) {
    entry.state = AssetState::Ready;
    notify(state, AssetEvent::Ready, entry);
    resolve_waiting(state, entry);
}

void queue_unload(RegistryState& state, Entry& entry) {
    if (entry.refs == 0 && !entry.queued_unload) {
        entry.queued_unload = true;
        state.unload_queue.push(entry.handle);
    }
}

AssetHandle acquire(RegistryState& state, AssetRef asset, platform::IoPriority priority);
void drop(RegistryState& state, AssetHandle handle);

// Holds what the file's header lists; a dependency still loading makes the asset wait for it. An
// asset listing itself would wait for itself forever: that entry is skipped. (The cooker writes no
// cycles; a longer one in a corrupt file leaves its assets Loading, nothing worse.)
void link_dependencies(RegistryState& state, Entry& entry, core::Array<AssetHandle>& into) {
    const auto* header = reinterpret_cast<const AssetHeader*>(entry.data.data);
    for (const AssetRef& ref : header->dependencies.get(entry.data.data)) {
        if (ref.id == entry.asset.id) {
            log_failure(entry, "skipping a dependency of", "it lists itself");
            continue;
        }
        into.push(acquire(state, ref, entry.priority));
    }
}

void commit(RegistryState& state, Entry& entry) {
    entry.in_flight = false;
    --state.loading;
    if (entry.reloading) {
        entry.reloading = false;
        entry.info = entry.pending_info; // a broken version isn't retried until the file changes again
        if (entry.pending_error != nullptr) {
            log_failure(entry, "reloading", entry.pending_error);
            DE_LOG_WARN("asset", "keeping the loaded version of %s", format_asset_id(entry.asset.id).c_str());
            free_buffer(entry.pending);
        } else {
            // In place: the handle stays, the contents and dependencies change. The new
            // dependencies are held before the old ones are let go, so shared ones stay loaded.
            state.bytes -= entry.data.size;
            free_buffer(entry.data);
            entry.data = std::exchange(entry.pending, Buffer{});
            state.bytes += entry.data.size;
            ++state.reloads;
            const core::Array<AssetHandle> old = std::move(entry.dependencies);
            entry.dependencies = core::Array<AssetHandle>(streaming_heap());
            link_dependencies(state, entry, entry.dependencies);
            for (const AssetHandle dependency : old) {
                drop(state, dependency);
            }
            DE_LOG_INFO("asset", "reloaded %s %s", asset_type_name(entry.asset.type),
                        format_asset_id(entry.asset.id).c_str());
            notify(state, AssetEvent::Reloaded, entry);
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
    link_dependencies(state, entry, entry.dependencies);
    for (const AssetHandle handle : entry.dependencies) {
        Entry* dependency = find_entry(state, handle);
        if (dependency != nullptr && dependency->state == AssetState::Loading) {
            dependency->waiting.push(entry.handle);
            ++entry.dependencies_pending;
        }
    }
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
    if (const AssetHandle* held = state.by_id.find(asset.id); held != nullptr) {
        Entry& entry = *find_entry(state, *held);
        ++entry.refs;
        if (entry.asset.type != asset.type && !entry.type_mismatch_logged) {
            entry.type_mismatch_logged = true;
            DE_LOG_ERROR("asset", "%s is held as a %s and asked for as a %s: get() of the other type gives null",
                         format_asset_id(asset.id).c_str(), asset_type_name(entry.asset.type),
                         asset_type_name(asset.type));
        }
        return *held;
    }
    auto* entry = ::new (streaming_heap().allocate(sizeof(Entry), alignof(Entry))) Entry();
    entry->registry = &state;
    entry->asset = asset;
    entry->priority = priority;
    entry->refs = 1;
    entry->handle = from_slot(state.entries.insert(entry));
    if (asset.id.is_valid()) {
        state.by_id.set(asset.id, entry->handle);
    }
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

void free_entry(RegistryState& state, Entry* entry) {
    free_buffer(entry->data);
    free_buffer(entry->pending);
    state.entries.remove(to_slot(entry->handle));
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
        state.bytes -= entry->data.size;
        state.failed -= entry->state == AssetState::Failed ? 1 : 0;
        notify(state, AssetEvent::Unloaded, *entry);
        if (const AssetHandle* held = state.by_id.find(entry->asset.id); held != nullptr && *held == entry->handle) {
            state.by_id.remove(entry->asset.id);
        }
        free_entry(state, entry);
    }
    state.unload_queue.clear();
}

// A few held files per update, round robin: loaded ones that changed are read again; failed ones
// whose file appeared or changed are tried again.
void check_changes(RegistryState& state) {
    const std::span<Entry*> all = state.entries.values();
    if (!state.desc.hot_reload || all.empty()) {
        return;
    }
    DE_PROFILE_ZONE("asset hot reload poll");
    const size_t checks = std::min<size_t>(state.desc.reload_checks_per_update, all.size());
    for (size_t i = 0; i < checks; ++i) {
        state.reload_cursor = static_cast<uint32_t>(state.reload_cursor % all.size());
        Entry& entry = *all[state.reload_cursor++];
        if (entry.in_flight || entry.refs == 0 || entry.state == AssetState::Loading) {
            continue;
        }
        const CookedPath path = cooked_path(entry.asset.id, entry.asset.type);
        const core::Result<platform::FileInfo> info = state.vfs->file_info(path.c_str());
        const platform::FileInfo now = info ? *info : platform::FileInfo{};
        if (same_file(now, entry.info) || !info) {
            continue; // unchanged, or gone: a deleted file keeps the loaded version
        }
        if (entry.state == AssetState::Failed) {
            entry.state = AssetState::Loading;
            --state.failed;
            submit_read(state, entry, false);
        } else {
            submit_read(state, entry, true);
        }
    }
}

void tell_listeners(RegistryState& state) {
    if (state.notices.empty()) {
        return;
    }
    for (const AssetNotice& notice : state.notices) {
        for (uint32_t i = 0; i < state.listener_count; ++i) {
            state.listeners[i].function(state.listeners[i].context, notice);
        }
    }
    state.notices.clear();
}

} // namespace

core::Result<AssetRegistry> AssetRegistry::create(const AssetRegistryDesc& desc) {
    if (desc.vfs == nullptr || desc.jobs == nullptr) {
        return core::fail(core::ErrorCode::InvalidArgument, "the asset registry needs a VFS and a job system");
    }
    auto* state = ::new (streaming_heap().allocate(sizeof(RegistryState), alignof(RegistryState))) RegistryState();
    state->vfs = desc.vfs;
    state->jobs = desc.jobs;
    state->desc = desc;
    return AssetRegistry(state);
}

AssetRegistry::~AssetRegistry() {
    if (state_ == nullptr) {
        return;
    }
    // Reads in flight write into entries: let them finish first.
    state_->jobs->wait(state_->in_flight);
    for (Entry* entry : state_->entries) {
        free_buffer(entry->data);
        free_buffer(entry->pending);
        entry->~Entry();
        streaming_heap().deallocate(entry, sizeof(Entry), alignof(Entry));
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
    const AssetHandle* held = state_->by_id.find(asset.id);
    return held != nullptr && find_entry(*state_, *held)->asset.type == asset.type ? *held : AssetHandle{};
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
    commit_finished(state);
    unload_released(state);
    check_changes(state);
    tell_listeners(state);
    DE_PROFILE_PLOT("assets", static_cast<int64_t>(state.entries.size()));
    DE_PROFILE_PLOT("assets loading", static_cast<int64_t>(state.loading));
}

void AssetRegistry::wait_all() {
    DE_PROFILE_ZONE("asset registry wait all");
    RegistryState& state = *state_;
    while (true) {
        state.jobs->wait(state.in_flight);
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
