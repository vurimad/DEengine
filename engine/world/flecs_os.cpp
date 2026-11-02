#include "world/flecs_os.h"

#include "core/allocator.h"
#include "core/assert.h"
#include "core/log.h"

#include <flecs.h>
#include <flecs/addons/os_api_impl.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace de::world::detail {

namespace {

core::Allocator& world_heap() {
    return core::heap_allocator(core::MemoryTag::World);
}

// flecs frees and reallocates without a size; tagged allocators need it, so each block starts with
// its size. A whole cache line: flecs needs malloc's 16-byte alignment (its columns hold aligned
// types), and component columns should start on cache lines, as large malloc blocks do; a smaller
// header would shift every column off them.
constexpr size_t kHeader = 64;

void* hooked_malloc(ecs_size_t size) {
    auto* block = static_cast<std::byte*>(world_heap().allocate(static_cast<size_t>(size) + kHeader, kHeader));
    const auto bytes = static_cast<size_t>(size);
    std::memcpy(block, &bytes, sizeof(bytes));
    return block + kHeader;
}

size_t block_size(void* pointer) {
    size_t size = 0;
    std::memcpy(&size, static_cast<std::byte*>(pointer) - kHeader, sizeof(size));
    return size;
}

void hooked_free(void* pointer) {
    if (pointer == nullptr) {
        return;
    }
    world_heap().deallocate(static_cast<std::byte*>(pointer) - kHeader, block_size(pointer) + kHeader, kHeader);
}

void* hooked_calloc(ecs_size_t size) {
    void* pointer = hooked_malloc(size);
    std::memset(pointer, 0, static_cast<size_t>(size));
    return pointer;
}

void* hooked_realloc(void* pointer, ecs_size_t size) {
    if (pointer == nullptr) {
        return hooked_malloc(size);
    }
    void* grown = hooked_malloc(size);
    std::memcpy(grown, pointer, std::min(block_size(pointer), static_cast<size_t>(size)));
    hooked_free(pointer);
    return grown;
}

void hooked_log(int32_t level, const char* file, int32_t line, const char* message) {
    // flecs: -4 fatal, -3 error, -2 warning, 0 info, above 0 debug and tracing.
    const char* where = file != nullptr ? file : "flecs";
    if (level <= -3) {
        DE_LOG_ERROR("world", "%s(%d): %s", where, line, message);
    } else if (level == -2) {
        DE_LOG_WARN("world", "%s(%d): %s", where, line, message);
    } else if (level <= 0) {
        DE_LOG_INFO("world", "%s", message);
    } else {
        DE_LOG_DEBUG("world", "%s", message);
    }
}

void hooked_abort() {
    DE_VERIFY_MSG(false, "flecs hit a fatal error (logged above)");
}

FILE* hooked_fopen(const char* path, const char* /*mode*/) {
    DE_LOG_ERROR("world", "flecs tried to open '%s': files go through the VFS", path != nullptr ? path : "");
    return nullptr;
}

// flecs' worker threads wait on each other at every merge, which a job system can't promise
// (a job may run inline, or late); parallel systems use JobSystem::parallel_for instead.
ecs_os_thread_t hooked_no_threads(ecs_os_thread_callback_t /*callback*/, void* /*param*/) {
    DE_VERIFY_MSG(false, "flecs threads are off: split a system's rows with JobSystem::parallel_for");
    return 0;
}

std::atomic<bool> g_installed{false};

} // namespace

void install_flecs_os_api() {
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true)) {
        return;
    }
    // Threads, mutexes and time from flecs' own implementation. It also marks the API set, so
    // ecs_os_set_api() would ignore anything after it: the hooks go straight into ecs_os_api, before
    // any world exists (nothing allocated yet with the defaults).
    ecs_set_os_api_impl();
    ecs_os_api.malloc_ = hooked_malloc;
    ecs_os_api.calloc_ = hooked_calloc;
    ecs_os_api.realloc_ = hooked_realloc;
    ecs_os_api.free_ = hooked_free;
    ecs_os_api.log_ = hooked_log;
    ecs_os_api.abort_ = hooked_abort;
    ecs_os_api.fopen_ = hooked_fopen;
    ecs_os_api.thread_new_ = hooked_no_threads;
    ecs_os_api.task_new_ = hooked_no_threads;
}

bool flecs_os_api_installed() {
    return ecs_os_api.malloc_ == hooked_malloc && ecs_os_api.free_ == hooked_free && ecs_os_api.log_ == hooked_log &&
           ecs_os_api.task_new_ == hooked_no_threads;
}

} // namespace de::world::detail
