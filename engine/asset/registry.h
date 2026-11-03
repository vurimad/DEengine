#pragma once

#include "asset/format.h"
#include "core/result.h"
#include "core/slot_map.h"
#include "platform/vfs.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace de::core {
class JobSystem;
}

namespace de::asset {

struct RegistryState;
struct Entry;

// Stable while the asset is held: look up its contents with get() each time they're used.
using AssetHandle = core::SlotHandle<Entry>;

enum class AssetState : uint8_t {
    Loading, // its file is being read and checked, or its dependencies are loading
    Ready,   // loaded and checked, with every dependency Ready or Failed
    Failed,  // missing, unreadable, corrupt, or of another type than asked (logged); also for a stale handle
};

enum class AssetEvent : uint8_t {
    Ready,    // first loaded (dependencies included)
    Failed,   // couldn't load; whoever draws it uses a placeholder
    Reloaded, // its file changed and was loaded again in place (its new dependencies settled first):
              // pointers from get() changed
    Unloaded, // its last reference went: the handle is stale from now on
};

struct AssetNotice {
    AssetEvent event = AssetEvent::Ready;
    AssetHandle handle;
    AssetRef asset;
};

// Called from update(), on its thread, for every event in the order they happened. A listener may
// load() and release() (the events that causes are told in the same update), but not call
// update() or wait_all().
using AssetListener = void (*)(void* context, const AssetNotice& notice);

struct AssetRegistryDesc {
    // Cooked files come from game:/. It must run completions on a job system (VfsDesc::jobs).
    platform::Vfs* vfs = nullptr;
    core::JobSystem* jobs = nullptr; // reads start, finish and are checked on it
    // Watch loaded files and load them again when they change (the cooker re-cooked them). A job
    // asks the OS, never the main thread.
    bool hot_reload = DE_BUILD_DEV != 0;
    // How many held files each poll checks, round robin (one poll in flight at a time).
    uint32_t reload_checks_per_update = 32;
};

struct AssetRegistryStats {
    uint32_t assets = 0;  // held, in any state
    uint32_t loading = 0; // reads and checks in flight, first loads and reloads
    uint32_t failed = 0;
    uint32_t reloads = 0; // files loaded again after they changed
    uint64_t bytes = 0;   // of loaded files
};

// Cooked assets by ID and type (docs/design.md, File system & IO): load() starts reading an asset
// and the assets it depends on, with the IO priority asked for; the read lands straight in the
// asset's memory, is checked (load_in_place, and that the file holds the asset asked for) on the
// job system, and is committed by update() on the main thread, one batch per frame. Assets are
// reference counted: each load() needs a release(), and an asset nobody holds is unloaded, its
// dependencies released with it. With hot reload on, a file the cooker rewrites is loaded again
// and, once its new dependencies are settled, swapped in place; listeners are told (Reloaded).
// A dependency that would close a cycle (a corrupt file) is logged and skipped.
//
// A missing or broken asset is Failed, with an error logged naming its ID and file; assets that
// depend on it still become Ready, and draw it with a placeholder (the renderer's job).
//
// Call everything from one thread, the one that calls update(). Reads make progress on the job
// system's workers (with none, only inside wait_all()). Move-only; the VFS and job system must
// outlive it.
class AssetRegistry {
public:
    // Fails without a VFS or job system, or with a VFS that has no job system for completions.
    static core::Result<AssetRegistry> create(const AssetRegistryDesc& desc);

    ~AssetRegistry();
    AssetRegistry(AssetRegistry&& other) noexcept;
    AssetRegistry& operator=(AssetRegistry&& other) noexcept;
    AssetRegistry(const AssetRegistry&) = delete;
    AssetRegistry& operator=(const AssetRegistry&) = delete;

    // Holds the asset: starts loading it (and what it depends on) unless it is loaded or loading
    // already. Never blocks. An invalid ID or type gives a Failed asset; the same ID asked for as
    // another type is another asset (and fails, having no such file). A later load() of a held
    // asset with a higher priority doesn't speed up a read already queued.
    AssetHandle load(AssetRef asset, platform::IoPriority priority = platform::IoPriority::Near);
    template <typename T>
    AssetHandle load(AssetId id, platform::IoPriority priority = platform::IoPriority::Near) {
        return load({.id = id, .type = T::kType}, priority);
    }

    // Lets go of one hold. With none left, the asset is unloaded at the next update().
    void release(AssetHandle handle);

    AssetState state(AssetHandle handle) const;
    // The handle of a held asset, without holding it again; invalid if nothing holds it (even if
    // it is still loaded, waiting for the next update() to unload it).
    AssetHandle find(AssetRef asset) const;
    AssetRef asset(AssetHandle handle) const;

    // The asset's file, and its contents as T, where the file lies: null and empty unless Ready
    // (and of type T). Valid until an update() reports it Reloaded or Unloaded.
    std::span<const std::byte> file(AssetHandle handle) const;
    template <typename T>
    const T* get(AssetHandle handle) const {
        const std::span<const std::byte> bytes = file(handle);
        return bytes.empty() || asset(handle).type != T::kType ? nullptr : reinterpret_cast<const T*>(bytes.data());
    }

    // Which version of its contents file() gives: 0 unless Ready, then 1, one more for each reload
    // swapped in. What was built from an asset is up to date while this is unchanged.
    uint32_t revision(AssetHandle handle) const;

    // The assets it depends on, held for it, in its header's order; empty until its file is loaded.
    // A dependency that would close a cycle (a corrupt file) is left out, so don't index this by a
    // header position: find() the header's entry.
    std::span<const AssetHandle> dependencies(AssetHandle handle) const;

    // Once per frame, on the registry's thread: commits finished reads in one batch, unloads
    // assets nobody holds, checks a few files for changes (hot reload) and swaps in the ones
    // reloaded, and tells listeners. With nothing to do it returns quickly and doesn't allocate.
    void update();

    // Waits for every read in flight and commits them (startup, tools, tests). Loads that start
    // because of them (dependencies) are waited for too.
    void wait_all();

    static constexpr size_t kMaxListeners = 8;
    core::Result<void> add_listener(AssetListener listener, void* context);
    // Stops telling a listener (added with the same function and context); nothing if it isn't
    // one. For listeners that go before the registry. Not from inside a listener.
    void remove_listener(AssetListener listener, void* context);

    AssetRegistryStats stats() const;

private:
    explicit AssetRegistry(RegistryState* state) : state_(state) {}

    RegistryState* state_ = nullptr;
};

} // namespace de::asset
