#include "world/flecs_os.h"

#include "core/allocator.h"
#include "core/assert.h"
#include "core/job_system.h"
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

// flecs frees and reallocates without a size; tagged allocators need it, so each block starts
// with its size.
constexpr size_t kHeader = alignof(std::max_align_t);

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

// --- Task threads on the JobSystem ---

// A task flecs started: a job, and its counter to join on. Fixed slots: no allocation per task.
struct TaskSlot {
    core::JobCounter counter;
    ecs_os_thread_callback_t callback = nullptr;
    void* param = nullptr;
    std::atomic<bool> used{false};
};

constexpr size_t kMaxTasks = 64;
TaskSlot g_tasks[kMaxTasks];
std::atomic<core::JobSystem*> g_jobs{nullptr};

void run_task(void* context) {
    auto* slot = static_cast<TaskSlot*>(context);
    slot->callback(slot->param);
}

ecs_os_thread_t task_new(ecs_os_thread_callback_t callback, void* param) {
    core::JobSystem* jobs = g_jobs.load(std::memory_order_acquire);
    DE_VERIFY_MSG(jobs != nullptr, "flecs started a task thread, but no world gave it a JobSystem");
    for (size_t i = 0; i < kMaxTasks; ++i) {
        bool expected = false;
        if (g_tasks[i].used.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            TaskSlot& slot = g_tasks[i];
            slot.callback = callback;
            slot.param = param;
            jobs->submit(
                core::Job{.entry = run_task, .context = &slot, .counter = &slot.counter, .name = "flecs task"});
            return static_cast<ecs_os_thread_t>(i + 1);
        }
    }
    DE_VERIFY_MSG(false, "more than 64 flecs tasks at once");
    return 0;
}

void* task_join(ecs_os_thread_t thread) {
    const auto index = static_cast<size_t>(thread) - 1;
    DE_ASSERT(index < kMaxTasks);
    TaskSlot& slot = g_tasks[index];
    g_jobs.load(std::memory_order_acquire)->wait(slot.counter);
    slot.used.store(false, std::memory_order_release);
    return nullptr;
}

std::atomic<bool> g_installed{false};

} // namespace

void install_flecs_os_api() {
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true)) {
        return;
    }
    ecs_set_os_api_impl(); // threads, mutexes and time from flecs' own implementation
    ecs_os_api_t api = ecs_os_api;
    api.malloc_ = hooked_malloc;
    api.calloc_ = hooked_calloc;
    api.realloc_ = hooked_realloc;
    api.free_ = hooked_free;
    api.log_ = hooked_log;
    api.abort_ = hooked_abort;
    api.fopen_ = hooked_fopen;
    api.task_new_ = task_new;
    api.task_join_ = task_join;
    ecs_os_set_api(&api);
}

void set_flecs_jobs(core::JobSystem* jobs) {
    g_jobs.store(jobs, std::memory_order_release);
}

} // namespace de::world::detail
