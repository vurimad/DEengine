#include "core/job_system.h"

#include "core/assert.h"
#include "core/detail/work_stealing_deque.h"
#include "core/profile.h"

#include <algorithm>
#include <mutex>
#include <new>
#include <thread>

namespace de::core {

namespace {

constexpr uint32_t kNotInSystem = UINT32_MAX;

// Looks for work this many times (yielding in between) before going to sleep: new work often
// follows right away, and waking a sleeping thread costs far more than a few yields.
constexpr int kSpinsBeforeSleep = 64;

// One cache line per queue: each thread hammers its own.
struct alignas(64) ThreadQueue {
    ThreadQueue(size_t capacity, Allocator& allocator) : jobs(capacity, allocator) {}
    detail::WorkStealingDeque<Job> jobs;
};

// "worker 12" without printf.
void format_worker_name(char (&out)[24], uint32_t index) {
    constexpr const char* kPrefix = "worker ";
    size_t length = 0;
    while (kPrefix[length] != '\0') {
        out[length] = kPrefix[length];
        ++length;
    }
    char digits[10];
    size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + index % 10);
        index /= 10;
    } while (index != 0);
    while (count > 0) {
        out[length++] = digits[--count];
    }
    out[length] = '\0';
}

} // namespace

// Sleeping and waking. The hot paths (submit, take) touch no shared atomic unless a thread is
// asleep: a submitter publishes its job, then (after a seq_cst fence) reads `sleepers`. A thread
// going to sleep increments `sleepers`, fences, and then looks for work one last time. Two
// seq_cst fences rule out both sides missing each other: either the sleeper sees the job, or the
// submitter sees the sleeper and bumps `epoch`, which ends the sleeper's atomic wait.
struct JobSystemState {
    Allocator* allocator = nullptr;
    uint32_t worker_count = 0;
    ThreadQueue* queues = nullptr; // thread_count() of them; index 0 is the creating thread
    std::thread* workers = nullptr;
    void (*on_worker_start)(uint32_t, const char*) = nullptr;

    // Jobs submitted by threads outside the system: a fixed ring behind one lock.
    std::mutex shared_mutex;
    Job* shared_jobs = nullptr;
    size_t shared_capacity = 0;
    size_t shared_head = 0;
    std::atomic<size_t> shared_count{0};

    // Background jobs: a fixed ring behind one lock, taken by idle workers only.
    std::mutex background_mutex;
    Job* background_jobs = nullptr;
    size_t background_capacity = 0;
    size_t background_head = 0;
    std::atomic<size_t> background_count{0};

    alignas(64) std::atomic<uint32_t> sleepers{0}; // threads asleep or about to be
    alignas(64) std::atomic<uint32_t> epoch{0};    // bumped to wake sleepers
    std::atomic<bool> running{true};

    uint32_t thread_count() const { return worker_count + 1; }

    static void add(JobCounter& counter, uint32_t count) {
        counter.pending_.fetch_add(count, std::memory_order_relaxed);
    }

    // True when this was the counter's last job.
    static bool finish(JobCounter& counter) { return counter.pending_.fetch_sub(1, std::memory_order_acq_rel) == 1; }
};

namespace {

// Which system the calling thread belongs to, and its queue index there.
constinit thread_local JobSystemState* current_system = nullptr;
constinit thread_local uint32_t current_index = kNotInSystem;

uint32_t index_in(const JobSystemState& state) {
    return current_system == &state ? current_index : kNotInSystem;
}

// Called after publishing work, or when a counter reaches zero.
void wake(JobSystemState& state, bool everyone) {
    std::atomic_thread_fence(std::memory_order_seq_cst); // pairs with the fence in sleep()
    if (state.sleepers.load(std::memory_order_relaxed) == 0) {
        return; // the common case while busy: nothing shared is written
    }
    state.epoch.fetch_add(1, std::memory_order_relaxed);
    if (everyone) {
        state.epoch.notify_all();
    } else {
        state.epoch.notify_one();
    }
}

// Blocks until woken, unless ready() is already true once this thread has announced itself.
template <typename Ready>
void sleep(JobSystemState& state, Ready ready) {
    const uint32_t seen = state.epoch.load(std::memory_order_relaxed);
    state.sleepers.fetch_add(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst); // pairs with the fence in wake()
    if (!ready()) {
        state.epoch.wait(seen, std::memory_order_relaxed);
    }
    state.sleepers.fetch_sub(1, std::memory_order_relaxed);
}

// Whether any queue holds a job, without taking it.
bool has_work(const JobSystemState& state) {
    if (state.shared_count.load(std::memory_order_relaxed) > 0) {
        return true;
    }
    for (uint32_t i = 0; i < state.thread_count(); ++i) {
        if (state.queues[i].jobs.size_estimate() > 0) {
            return true;
        }
    }
    return false;
}

bool has_background_work(const JobSystemState& state) {
    return state.background_count.load(std::memory_order_relaxed) > 0;
}

bool pop_background(JobSystemState& state, Job& out) {
    if (!has_background_work(state)) {
        return false;
    }
    const std::scoped_lock lock(state.background_mutex);
    const size_t count = state.background_count.load(std::memory_order_relaxed);
    if (count == 0) {
        return false;
    }
    out = state.background_jobs[state.background_head];
    state.background_head = (state.background_head + 1) % state.background_capacity;
    state.background_count.store(count - 1, std::memory_order_relaxed);
    return true;
}

void run(JobSystemState& state, const Job& job) {
    DE_PROFILE_ZONE("job");
    DE_PROFILE_ZONE_NAME(job.name);
    job.entry(job.context);
    if (job.counter != nullptr && JobSystemState::finish(*job.counter)) {
        wake(state, true); // someone may be asleep in wait() on this counter
    }
}

bool pop_shared(JobSystemState& state, Job& out) {
    if (state.shared_count.load(std::memory_order_relaxed) == 0) {
        return false; // checked without the lock first: the shared queue is usually empty
    }
    const std::scoped_lock lock(state.shared_mutex);
    const size_t count = state.shared_count.load(std::memory_order_relaxed);
    if (count == 0) {
        return false;
    }
    out = state.shared_jobs[state.shared_head];
    state.shared_head = (state.shared_head + 1) % state.shared_capacity;
    state.shared_count.store(count - 1, std::memory_order_relaxed);
    return true;
}

// Own queue first (newest, cache-warm), then the shared queue, then steal from the others.
bool take(JobSystemState& state, uint32_t self, Job& out) {
    if ((self != kNotInSystem && state.queues[self].jobs.pop(out)) || pop_shared(state, out)) {
        return true;
    }
    const uint32_t threads = state.thread_count();
    const uint32_t start = self != kNotInSystem ? self + 1 : 0;
    for (uint32_t i = 0; i < threads; ++i) {
        const uint32_t victim = (start + i) % threads;
        if (victim != self && state.queues[victim].jobs.steal(out)) {
            return true;
        }
    }
    return false;
}

// Queues jobs from the calling thread; jobs that don't fit run right away instead.
// One lock for the whole batch when the caller is outside the system.
void enqueue(JobSystemState& state, std::span<const Job> jobs) {
    const uint32_t self = index_in(state);
    size_t queued = 0;
    if (self != kNotInSystem) {
        while (queued < jobs.size() && state.queues[self].jobs.push(jobs[queued])) {
            ++queued;
        }
    } else {
        const std::scoped_lock lock(state.shared_mutex);
        size_t count = state.shared_count.load(std::memory_order_relaxed);
        for (; queued < jobs.size() && count < state.shared_capacity; ++queued, ++count) {
            state.shared_jobs[(state.shared_head + count) % state.shared_capacity] = jobs[queued];
        }
        state.shared_count.store(count, std::memory_order_relaxed);
    }
    if (queued > 0) {
        wake(state, queued > 1);
    }
    // The queue is full: do the rest of the work now rather than fail or allocate.
    for (size_t i = queued; i < jobs.size(); ++i) {
        run(state, jobs[i]);
    }
}

void worker_main(JobSystemState* state, uint32_t index) {
    current_system = state;
    current_index = index;
    char name[24];
    format_worker_name(name, index - 1);
    DE_PROFILE_THREAD_NAME(name);
    if (state->on_worker_start != nullptr) {
        state->on_worker_start(index - 1, name);
    }

    Job job;
    int idle = 0;
    for (;;) {
        // Regular work first; background jobs only when there is none.
        if (take(*state, index, job) || pop_background(*state, job)) {
            run(*state, job);
            idle = 0;
            continue;
        }
        if (!state->running.load(std::memory_order_acquire)) {
            return; // stopping, and no work is left
        }
        if (++idle < kSpinsBeforeSleep) {
            std::this_thread::yield();
            continue;
        }
        sleep(*state, [state] {
            return !state->running.load(std::memory_order_acquire) || has_work(*state) || has_background_work(*state);
        });
        idle = 0;
    }
}

struct ParallelFor {
    JobSystem::RangeEntry entry = nullptr;
    void* context = nullptr;
    size_t count = 0;
    size_t batch_size = 1;
    std::atomic<size_t> next{0};
};

void run_ranges(void* context) {
    auto* work = static_cast<ParallelFor*>(context);
    for (;;) {
        const size_t begin = work->next.fetch_add(work->batch_size, std::memory_order_relaxed);
        if (begin >= work->count) {
            return;
        }
        work->entry(begin, std::min(begin + work->batch_size, work->count), work->context);
    }
}

} // namespace

JobSystem::JobSystem(const JobSystemDesc& desc, Allocator& allocator) {
    DE_VERIFY_MSG(current_system == nullptr, "this thread already belongs to a JobSystem");

    state_ = ::new (allocator.allocate(sizeof(JobSystemState), alignof(JobSystemState))) JobSystemState();
    JobSystemState& state = *state_;
    state.allocator = &allocator;
    state.on_worker_start = desc.on_worker_start;
    state.worker_count = desc.worker_count != JobSystemDesc::kAutoWorkers
                             ? desc.worker_count
                             : std::max(1U, std::thread::hardware_concurrency()) - 1;

    const uint32_t threads = state.thread_count();
    state.queues = allocate_array<ThreadQueue>(allocator, threads);
    for (uint32_t i = 0; i < threads; ++i) {
        ::new (static_cast<void*>(&state.queues[i])) ThreadQueue(desc.queue_capacity, allocator);
    }
    state.shared_capacity = desc.queue_capacity;
    state.shared_jobs = allocate_array<Job>(allocator, state.shared_capacity);
    state.background_capacity = desc.queue_capacity;
    state.background_jobs = allocate_array<Job>(allocator, state.background_capacity);

    current_system = &state;
    current_index = 0;

    state.workers = allocate_array<std::thread>(allocator, state.worker_count);
    for (uint32_t i = 0; i < state.worker_count; ++i) {
        ::new (static_cast<void*>(&state.workers[i])) std::thread(worker_main, &state, i + 1);
    }
}

JobSystem::~JobSystem() {
    JobSystemState& state = *state_;
    // Its thread-local registration can only be cleared on the thread that holds it.
    DE_VERIFY_MSG(index_in(state) == 0, "a JobSystem must be destroyed on the thread that created it");

    // Jobs nobody waited on still run: drain what's left, with the workers' help.
    Job job;
    while (take(state, 0, job) || pop_background(state, job)) {
        run(state, job);
    }

    state.running.store(false, std::memory_order_release);
    state.epoch.fetch_add(1, std::memory_order_relaxed);
    state.epoch.notify_all();
    for (uint32_t i = 0; i < state.worker_count; ++i) {
        state.workers[i].join();
        state.workers[i].~thread();
    }
    DE_ASSERT_MSG(!has_work(state) && !has_background_work(state), "JobSystem destroyed with jobs still queued");

    Allocator& allocator = *state.allocator;
    deallocate_array(allocator, state.workers, state.worker_count);
    deallocate_array(allocator, state.shared_jobs, state.shared_capacity);
    deallocate_array(allocator, state.background_jobs, state.background_capacity);
    for (uint32_t i = 0; i < state.thread_count(); ++i) {
        state.queues[i].~ThreadQueue();
    }
    deallocate_array(allocator, state.queues, state.thread_count());
    current_system = nullptr;
    current_index = kNotInSystem;
    state.~JobSystemState();
    allocator.deallocate(state_, sizeof(JobSystemState), alignof(JobSystemState));
}

void JobSystem::submit(const Job& job) {
    submit(std::span<const Job>(&job, 1));
}

void JobSystem::submit(std::span<const Job> jobs) {
    for (const Job& job : jobs) {
        DE_ASSERT(job.entry != nullptr);
        DE_ASSERT(job.name != nullptr);
        if (job.counter != nullptr) {
            JobSystemState::add(*job.counter, 1);
        }
    }
    enqueue(*state_, jobs);
}

void JobSystem::submit_background(const Job& job) {
    DE_ASSERT(job.entry != nullptr);
    DE_ASSERT(job.name != nullptr);
    JobSystemState& state = *state_;
    if (job.counter != nullptr) {
        JobSystemState::add(*job.counter, 1);
    }
    bool queued = false;
    if (state.worker_count > 0) {
        const std::scoped_lock lock(state.background_mutex);
        const size_t count = state.background_count.load(std::memory_order_relaxed);
        if (count < state.background_capacity) {
            state.background_jobs[(state.background_head + count) % state.background_capacity] = job;
            state.background_count.store(count + 1, std::memory_order_relaxed);
            queued = true;
        }
    }
    if (!queued) {
        run(state, job); // no workers (or the ring is full): the caller runs it
        return;
    }
    // Everyone: a waiter woken in place of a worker would leave the job alone.
    wake(state, true);
}

void JobSystem::reserve(JobCounter& counter, uint32_t count) {
    JobSystemState::add(counter, count);
}

void JobSystem::submit_reserved(const Job& job) {
    DE_ASSERT(job.entry != nullptr);
    DE_ASSERT(job.name != nullptr);
    enqueue(*state_, std::span<const Job>(&job, 1));
}

void JobSystem::release_reserved(JobCounter& counter, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        if (JobSystemState::finish(counter)) {
            wake(*state_, true); // someone may be asleep in wait() on this counter
        }
    }
}

void JobSystem::wait(JobCounter& counter) {
    DE_PROFILE_ZONE("wait");
    JobSystemState& state = *state_;
    const uint32_t self = index_in(state);
    Job job;
    int idle = 0;
    while (!counter.done()) {
        if (take(state, self, job)) {
            run(state, job);
            idle = 0;
            continue;
        }
        if (++idle < kSpinsBeforeSleep) {
            std::this_thread::yield();
            continue;
        }
        // The counter's last jobs run elsewhere: sleep until they finish or new work arrives.
        sleep(state, [&] { return counter.done() || has_work(state); });
        idle = 0;
    }
}

void JobSystem::parallel_for(size_t count, size_t batch_size, RangeEntry entry, void* context, const char* name) {
    DE_ASSERT(entry != nullptr);
    DE_ASSERT(name != nullptr);
    DE_ASSERT_MSG(batch_size > 0, "parallel_for needs a batch size of at least 1");
    // Checked in every build: larger values would overflow the range arithmetic below.
    DE_VERIFY_MSG(count <= SIZE_MAX / 2 && batch_size <= SIZE_MAX / 2, "parallel_for count or batch size too large");
    if (count == 0) {
        return;
    }
    DE_PROFILE_ZONE("parallel_for");
    DE_PROFILE_ZONE_NAME(name);

    ParallelFor work;
    work.entry = entry;
    work.context = context;
    work.count = count;
    work.batch_size = batch_size;

    // One helper job per other thread, at most one per range; the caller works too.
    const size_t ranges = (count + batch_size - 1) / batch_size;
    const size_t helpers = std::min<size_t>(ranges, thread_count()) - 1;
    JobCounter counter;
    const Job helper{.entry = &run_ranges, .context = &work, .counter = &counter, .name = name};
    for (size_t i = 0; i < helpers; ++i) {
        submit(helper);
    }
    run_ranges(&work);
    wait(counter);
}

uint32_t JobSystem::worker_count() const {
    return state_->worker_count;
}

} // namespace de::core
