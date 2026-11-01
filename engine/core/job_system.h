#pragma once

#include "core/allocator.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace de::core {

struct JobSystemState;

using JobEntry = void (*)(void* context);

// Counts a group of unfinished jobs; JobSystem::wait() returns when it reaches zero.
// It must outlive the jobs that reference it.
class JobCounter {
public:
    JobCounter() = default;
    JobCounter(const JobCounter&) = delete;
    JobCounter& operator=(const JobCounter&) = delete;
    JobCounter(JobCounter&&) = delete;
    JobCounter& operator=(JobCounter&&) = delete;
    ~JobCounter() = default;

    bool done() const { return pending_.load(std::memory_order_acquire) == 0; }

private:
    friend struct JobSystemState;
    std::atomic<uint32_t> pending_{0};
};

// A unit of work: a plain function and a context pointer, so submitting never needs an owning
// callable. The context must stay valid until the job has run.
struct Job {
    JobEntry entry = nullptr;
    void* context = nullptr;
    JobCounter* counter = nullptr; // counted down when the job finishes; may be null
    const char* name = "job";      // a string literal, shown on the job's profiler zone
};

struct JobSystemDesc {
    static constexpr uint32_t kAutoWorkers = UINT32_MAX;

    // kAutoWorkers: one per hardware thread, minus the thread that creates the system (it helps
    // while it waits). 0: no workers; jobs run inside wait() on the waiting thread.
    uint32_t worker_count = kAutoWorkers;

    // Jobs each thread's queue holds (a power of two). A thread whose queue is full runs the
    // job it is submitting immediately instead.
    uint32_t queue_capacity = 4096;

    // Runs first on each worker thread, with its index in [0, worker_count) and its name
    // ("worker 3"), e.g. to set the OS thread name through the platform layer. Core already
    // names it in the profiler.
    void (*on_worker_start)(uint32_t worker_index, const char* name) = nullptr;
};

// Work-stealing job system (docs/design.md, High-level architecture: work-stealing, fiber-free).
//
// Each thread of the system owns a queue: it pops its own newest jobs and steals the oldest
// jobs of others when it runs dry. The thread that creates the system is part of it (index 0)
// and runs jobs while it waits instead of blocking. Other threads (an IO thread, say) may submit
// too, through a small shared queue. Idle threads, and threads waiting on a counter whose jobs
// run elsewhere, sleep until work arrives or the counter reaches zero. The system must be
// destroyed on the thread that created it.
//
// Submitting, waiting and parallel_for never allocate. Parallelism comes from partitioning
// data, not locking it: give each job its own range to write (docs/design.md, Performance rules).
class JobSystem {
public:
    explicit JobSystem(const JobSystemDesc& desc = {}, Allocator& allocator = heap_allocator(MemoryTag::Core));
    ~JobSystem(); // runs any jobs still queued, then stops the workers

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;
    JobSystem(JobSystem&&) = delete;
    JobSystem& operator=(JobSystem&&) = delete;

    void submit(const Job& job);
    void submit(std::span<const Job> jobs);

    // Runs queued jobs on the calling thread until counter reaches zero.
    void wait(JobCounter& counter);

    // Calls entry(begin, end, context) over [0, count) in ranges of at most batch_size items, on
    // every thread of the system including the caller, and returns when all are done. Threads
    // pull ranges as they finish, so uneven work balances itself.
    using RangeEntry = void (*)(size_t begin, size_t end, void* context);
    void parallel_for(size_t count, size_t batch_size, RangeEntry entry, void* context,
                      const char* name = "parallel_for");

    uint32_t worker_count() const;

    // Workers plus the creating thread.
    uint32_t thread_count() const { return worker_count() + 1; }

private:
    JobSystemState* state_;
};

} // namespace de::core
