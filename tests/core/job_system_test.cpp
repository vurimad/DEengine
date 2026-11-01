#include "core/job_system.h"
#include "core/memory_stats.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
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

TEST_CASE("threads outside the system can submit and wait") {
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
        jobs.wait(counter);
    }
    CHECK(runs.load() == 1000);
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
