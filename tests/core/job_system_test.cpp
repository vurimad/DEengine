#include "core/job_system.h"
#include "core/memory_stats.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <thread>
#include <vector>

using de::core::Job;
using de::core::JobCounter;
using de::core::JobSystem;
using de::core::JobSystemDesc;

namespace {

void add_one(void* context) {
    static_cast<std::atomic<int>*>(context)->fetch_add(1, std::memory_order_relaxed);
}

void mark_range(size_t begin, size_t end, void* context) {
    auto* hits = static_cast<std::atomic<uint8_t>*>(context);
    for (size_t i = begin; i < end; ++i) {
        hits[i].fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace

TEST_CASE("every submitted job runs exactly once") {
    JobSystem jobs;
    std::atomic<int> runs{0};
    JobCounter counter;
    for (int i = 0; i < 10'000; ++i) {
        jobs.submit(Job{.entry = &add_one, .context = &runs, .counter = &counter, .name = "add_one"});
    }
    jobs.wait(counter);
    CHECK(counter.done());
    CHECK(runs.load() == 10'000);
}

TEST_CASE("worker counts: automatic, explicit and none") {
    {
        const JobSystem jobs;
        CHECK(jobs.worker_count() == std::max(1U, std::thread::hardware_concurrency()) - 1);
    }
    {
        const JobSystem jobs(JobSystemDesc{.worker_count = 2});
        CHECK(jobs.worker_count() == 2);
        CHECK(jobs.thread_count() == 3);
    }
    {
        // No workers: everything runs on the waiting thread.
        JobSystem jobs(JobSystemDesc{.worker_count = 0});
        std::atomic<int> runs{0};
        JobCounter counter;
        for (int i = 0; i < 100; ++i) {
            jobs.submit(Job{.entry = &add_one, .context = &runs, .counter = &counter});
        }
        jobs.wait(counter);
        CHECK(runs.load() == 100);
    }
}

TEST_CASE("parallel_for visits every index exactly once") {
    JobSystem jobs;
    for (const size_t count : {size_t{0}, size_t{1}, size_t{7}, size_t{1000}, size_t{100'003}}) {
        for (const size_t batch : {size_t{1}, size_t{64}, size_t{5000}}) {
            CAPTURE(count);
            CAPTURE(batch);
            std::vector<std::atomic<uint8_t>> hits(count);
            jobs.parallel_for(count, batch, &mark_range, hits.data());
            int wrong = 0;
            for (const std::atomic<uint8_t>& hit : hits) {
                wrong += hit.load() != 1 ? 1 : 0;
            }
            CHECK(wrong == 0);
        }
    }
}

TEST_CASE("a job can submit and wait for its own child jobs") {
    struct Parent {
        JobSystem* jobs = nullptr;
        std::atomic<int> children{0};
    };
    const auto parent_entry = [](void* context) {
        auto* parent = static_cast<Parent*>(context);
        JobCounter children;
        for (int i = 0; i < 50; ++i) {
            parent->jobs->submit(Job{.entry = &add_one, .context = &parent->children, .counter = &children});
        }
        parent->jobs->wait(children); // waits by running jobs, so nesting can't deadlock
    };

    JobSystem jobs;
    Parent parents[20];
    JobCounter counter;
    for (Parent& parent : parents) {
        parent.jobs = &jobs;
        jobs.submit(Job{.entry = parent_entry, .context = &parent, .counter = &counter, .name = "parent"});
    }
    jobs.wait(counter);
    for (const Parent& parent : parents) {
        CHECK(parent.children.load() == 50);
    }
}

namespace {

// Waits without helping, so only the workers can finish the jobs. False on timeout.
bool wait_without_helping(const JobCounter& counter) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!counter.done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

} // namespace

TEST_CASE("workers pick up jobs submitted from outside the system") {
    JobSystem jobs(JobSystemDesc{.worker_count = 2});
    std::atomic<int> runs{0};
    bool finished = false;
    std::thread outsider([&] {
        JobCounter counter;
        for (int i = 0; i < 1000; ++i) {
            jobs.submit(Job{.entry = &add_one, .context = &runs, .counter = &counter});
        }
        finished = wait_without_helping(counter);
    });
    outsider.join();
    CHECK(finished);
    CHECK(runs.load() == 1000);
}

TEST_CASE("an outside thread can also wait by helping") {
    JobSystem jobs;
    std::atomic<int> runs{0};
    std::thread outsider([&] {
        JobCounter counter;
        for (int i = 0; i < 1000; ++i) {
            jobs.submit(Job{.entry = &add_one, .context = &runs, .counter = &counter});
        }
        jobs.wait(counter);
    });
    outsider.join();
    CHECK(runs.load() == 1000);
}

TEST_CASE("a full queue runs the job immediately instead of dropping it") {
    JobSystem jobs(JobSystemDesc{.worker_count = 1, .queue_capacity = 8});
    std::atomic<int> runs{0};
    JobCounter counter;
    for (int i = 0; i < 1000; ++i) {
        jobs.submit(Job{.entry = &add_one, .context = &runs, .counter = &counter});
    }
    jobs.wait(counter);
    CHECK(runs.load() == 1000);
}

TEST_CASE("sleeping workers wake up for new work") {
    JobSystem jobs(JobSystemDesc{.worker_count = 4});
    std::atomic<int> runs{0};
    for (int round = 0; round < 5; ++round) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20)); // long enough for workers to sleep
        JobCounter counter;
        for (int i = 0; i < 200; ++i) {
            jobs.submit(Job{.entry = &add_one, .context = &runs, .counter = &counter});
        }
        // The test thread doesn't help: only woken workers can finish these jobs.
        REQUIRE(wait_without_helping(counter));
    }
    CHECK(runs.load() == 1000);
}

TEST_CASE("a single job submitted to sleeping workers still runs") {
    JobSystem jobs(JobSystemDesc{.worker_count = 3});
    std::atomic<int> runs{0};
    for (int round = 0; round < 20; ++round) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        JobCounter counter;
        jobs.submit(Job{.entry = &add_one, .context = &runs, .counter = &counter});
        REQUIRE(wait_without_helping(counter));
    }
    CHECK(runs.load() == 20);
}

TEST_CASE("wait() sleeps while a worker runs the last job, and wakes when it finishes") {
    const auto slow = [](void* context) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        static_cast<std::atomic<int>*>(context)->fetch_add(1);
    };
    JobSystem jobs(JobSystemDesc{.worker_count = 1});
    std::atomic<int> runs{0};
    JobCounter counter;
    jobs.submit(Job{.entry = slow, .context = &runs, .counter = &counter, .name = "slow"});
    std::this_thread::sleep_for(std::chrono::milliseconds(10)); // let the worker steal it
    jobs.wait(counter); // finds nothing to run, sleeps, and must be woken by the job's completion
    CHECK(runs.load() == 1);
}

namespace {

std::atomic<uint32_t> started_workers{0};
std::atomic<uint32_t> worker_index_mask{0};
std::atomic<uint32_t> badly_named_workers{0};

// Runs on the worker threads, so it records instead of asserting.
void record_worker_start(uint32_t index, const char* name) {
    worker_index_mask.fetch_or(1U << index);
    if (std::strncmp(name, "worker ", 7) != 0) {
        badly_named_workers.fetch_add(1);
    }
    started_workers.fetch_add(1);
}

} // namespace

TEST_CASE("each worker announces itself once with an index in [0, worker_count)") {
    started_workers = 0;
    worker_index_mask = 0;
    {
        const JobSystem jobs(JobSystemDesc{.worker_count = 4, .on_worker_start = &record_worker_start});
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (started_workers.load() < 4 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    CHECK(started_workers.load() == 4);
    CHECK(worker_index_mask.load() == 0b1111U);
    CHECK(badly_named_workers.load() == 0);
}

TEST_CASE("a batch submitted at once runs completely") {
    JobSystem jobs(JobSystemDesc{.worker_count = 4});
    std::atomic<int> runs{0};
    JobCounter counter;
    std::vector<Job> batch(300, Job{.entry = &add_one, .context = &runs, .counter = &counter});
    jobs.submit(batch);
    REQUIRE(wait_without_helping(counter));
    CHECK(runs.load() == 300);
}

TEST_CASE("destroying the system runs jobs nobody waited for") {
    std::atomic<int> runs{0};
    {
        JobSystem jobs(JobSystemDesc{.worker_count = 0});
        for (int i = 0; i < 10; ++i) {
            jobs.submit(Job{.entry = &add_one, .context = &runs});
        }
    }
    CHECK(runs.load() == 10);
}

TEST_CASE("submitting, waiting and parallel_for stay off the heap") {
    JobSystem jobs;
    std::atomic<int> runs{0};
    std::vector<std::atomic<uint8_t>> hits(4096);

    const de::core::NoHeapAllocationScope no_alloc;
    JobCounter counter;
    for (int i = 0; i < 500; ++i) {
        jobs.submit(Job{.entry = &add_one, .context = &runs, .counter = &counter});
    }
    jobs.wait(counter);
    jobs.parallel_for(hits.size(), 64, &mark_range, hits.data());
    CHECK(no_alloc.allocations() == 0);
    CHECK(runs.load() == 500);
}
