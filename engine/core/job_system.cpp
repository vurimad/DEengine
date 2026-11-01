#include "core/job_system.h"

#include "core/assert.h"
#include "core/detail/work_stealing_deque.h"
#include "core/profile.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <new>
#include <thread>

namespace de::core {

namespace {

constexpr uint32_t kNotInSystem = UINT32_MAX;

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

    // Jobs submitted and not yet taken. Sleeping workers wake when it rises above zero.
    alignas(64) std::atomic<int64_t> queued{0};
    std::mutex sleep_mutex;
    std::condition_variable wake;
    std::atomic<uint32_t> sleepers{0};
    std::atomic<bool> running{true};

    uint32_t thread_count() const { return worker_count + 1; }

    static void add(JobCounter& counter, uint32_t count) {
        counter.pending_.fetch_add(count, std::memory_order_relaxed);
    }
    static void finish(JobCounter& counter) { counter.pending_.fetch_sub(1, std::memory_order_release); }
};

namespace {

// Which system the calling thread belongs to, and its queue index there.
constinit thread_local JobSystemState* current_system = nullptr;
constinit thread_local uint32_t current_index = kNotInSystem;

uint32_t index_in(const JobSystemState& state) {
    return current_system == &state ? current_index : kNotInSystem;
}

void run(const Job& job) {
    DE_PROFILE_ZONE("job");
    DE_PROFILE_ZONE_NAME(job.name);
    job.entry(job.context);
    if (job.counter != nullptr) {
        JobSystemState::finish(*job.counter);
    }
}

bool push_shared(JobSystemState& state, const Job& job) {
    const std::scoped_lock lock(state.shared_mutex);
    const size_t count = state.shared_count.load(std::memory_order_relaxed);
    if (count == state.shared_capacity) {
        return false;
    }
    state.shared_jobs[(state.shared_head + count) % state.shared_capacity] = job;
    state.shared_count.store(count + 1, std::memory_order_relaxed);
    return true;
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
    bool found = (self != kNotInSystem && state.queues[self].jobs.pop(out)) || pop_shared(state, out);
    const uint32_t threads = state.thread_count();
    const uint32_t start = self != kNotInSystem ? self + 1 : 0;
    for (uint32_t i = 0; !found && i < threads; ++i) {
        const uint32_t victim = (start + i) % threads;
        found = victim != self && state.queues[victim].jobs.steal(out);
    }
    if (found) {
        state.queued.fetch_sub(1, std::memory_order_seq_cst);
    }
    return found;
}

void wake_one(JobSystemState& state) {
    if (state.sleepers.load(std::memory_order_seq_cst) == 0) {
        return;
    }
    // Taking the lock orders this wake-up after a worker that is about to sleep has checked
    // `queued`, so the notification can't be lost.
    {
        const std::scoped_lock lock(state.sleep_mutex);
    }
    state.wake.notify_one();
}

void worker_main(JobSystemState* state, uint32_t index) {
    current_system = state;
    current_index = index;
    char name[24];
    format_worker_name(name, index);
    DE_PROFILE_THREAD_NAME(name);
    if (state->on_worker_start != nullptr) {
        state->on_worker_start(index, name);
    }

    Job job;
    for (;;) {
        if (take(*state, index, job)) {
            run(job);
            continue;
        }
        // New work often follows right away: look again briefly before sleeping.
        bool found = false;
        for (int spin = 0; spin < 64 && !found; ++spin) {
            std::this_thread::yield();
            found = take(*state, index, job);
        }
        if (found) {
            run(job);
            continue;
        }

        std::unique_lock lock(state->sleep_mutex);
        state->sleepers.fetch_add(1, std::memory_order_seq_cst);
        state->wake.wait(lock, [state] {
            return !state->running.load(std::memory_order_seq_cst) || state->queued.load(std::memory_order_seq_cst) > 0;
        });
        state->sleepers.fetch_sub(1, std::memory_order_seq_cst);
        if (!state->running.load(std::memory_order_seq_cst) && state->queued.load(std::memory_order_seq_cst) <= 0) {
            return;
        }
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

    current_system = &state;
    current_index = 0;

    state.workers = allocate_array<std::thread>(allocator, state.worker_count);
    for (uint32_t i = 0; i < state.worker_count; ++i) {
        ::new (static_cast<void*>(&state.workers[i])) std::thread(worker_main, &state, i + 1);
    }
}

JobSystem::~JobSystem() {
    JobSystemState& state = *state_;
    const uint32_t self = index_in(state);

    // Jobs nobody waited on still run: drain what's left, with the workers' help.
    Job job;
    while (take(state, self, job)) {
        run(job);
    }

    {
        const std::scoped_lock lock(state.sleep_mutex);
        state.running.store(false, std::memory_order_seq_cst);
    }
    state.wake.notify_all();
    for (uint32_t i = 0; i < state.worker_count; ++i) {
        state.workers[i].join();
        state.workers[i].~thread();
    }
    DE_ASSERT_MSG(state.queued.load() == 0, "JobSystem destroyed with jobs still queued");

    Allocator& allocator = *state.allocator;
    deallocate_array(allocator, state.workers, state.worker_count);
    deallocate_array(allocator, state.shared_jobs, state.shared_capacity);
    for (uint32_t i = 0; i < state.thread_count(); ++i) {
        state.queues[i].~ThreadQueue();
    }
    deallocate_array(allocator, state.queues, state.thread_count());
    if (current_system == state_) {
        current_system = nullptr;
        current_index = kNotInSystem;
    }
    state.~JobSystemState();
    allocator.deallocate(state_, sizeof(JobSystemState), alignof(JobSystemState));
}

void JobSystem::submit(const Job& job) {
    DE_ASSERT(job.entry != nullptr);
    DE_ASSERT(job.name != nullptr);
    JobSystemState& state = *state_;
    if (job.counter != nullptr) {
        JobSystemState::add(*job.counter, 1);
    }
    state.queued.fetch_add(1, std::memory_order_seq_cst);

    const uint32_t self = index_in(state);
    const bool queued = self != kNotInSystem ? state.queues[self].jobs.push(job) : push_shared(state, job);
    if (!queued) {
        // The queue is full: do the work now rather than fail or allocate.
        state.queued.fetch_sub(1, std::memory_order_seq_cst);
        run(job);
        return;
    }
    wake_one(state);
}

void JobSystem::submit(std::span<const Job> jobs) {
    for (const Job& job : jobs) {
        submit(job);
    }
}

void JobSystem::wait(JobCounter& counter) {
    DE_PROFILE_ZONE("wait");
    JobSystemState& state = *state_;
    const uint32_t self = index_in(state);
    Job job;
    while (!counter.done()) {
        if (take(state, self, job)) {
            run(job);
        } else {
            std::this_thread::yield();
        }
    }
}

void JobSystem::parallel_for(size_t count, size_t batch_size, RangeEntry entry, void* context, const char* name) {
    DE_ASSERT(entry != nullptr);
    DE_ASSERT_MSG(batch_size > 0, "parallel_for needs a batch size of at least 1");
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
