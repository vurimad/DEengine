#include "core/frame_allocator.h"
#include "core/memory_stats.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

using de::core::FrameAllocator;
using de::core::NoHeapAllocationScope;

TEST_CASE("allocations are aligned and come from the current frame") {
    FrameAllocator frames(4096);
    void* a = frames.allocate(10, 64);
    void* b = frames.allocate(10, 8);
    CHECK(reinterpret_cast<uintptr_t>(a) % 64 == 0);
    CHECK(b > a);
    CHECK(frames.used_bytes() >= 20);
    CHECK(frames.bytes_per_frame() >= 4096);
}

TEST_CASE("data stays valid while its frame is in flight, then the buffer is reused") {
    for (size_t frames_in_flight = 1; frames_in_flight <= FrameAllocator::kMaxFramesInFlight; ++frames_in_flight) {
        CAPTURE(frames_in_flight);
        FrameAllocator frames(4096, frames_in_flight);
        CHECK(frames.frames_in_flight() == frames_in_flight);

        auto* first = static_cast<int*>(frames.allocate(sizeof(int), alignof(int)));
        *first = 42;

        // Later stages may still read the frames in flight: their data must survive.
        for (size_t i = 1; i < frames_in_flight; ++i) {
            frames.begin_frame();
            auto* other = static_cast<int*>(frames.allocate(sizeof(int), alignof(int)));
            *other = 7;
            CHECK(other != first);
            CHECK(*first == 42);
        }

        frames.begin_frame(); // back to the first buffer
        CHECK(frames.allocate(sizeof(int), alignof(int)) == first);
        CHECK(frames.frame_index() == frames_in_flight);
    }
}

TEST_CASE("defaults to two frames in flight") {
    const FrameAllocator frames(4096);
    CHECK(frames.frames_in_flight() == 2);
}

TEST_CASE("three frames in flight keep frame N's data through frames N+1 and N+2") {
    // Game thread builds N+2 while the render thread records N+1 and the GPU still reads N.
    FrameAllocator frames(4096, 3);
    auto* frame_n = static_cast<int*>(frames.allocate(sizeof(int), alignof(int)));
    *frame_n = 1;
    frames.begin_frame();
    auto* frame_n1 = static_cast<int*>(frames.allocate(sizeof(int), alignof(int)));
    *frame_n1 = 2;
    frames.begin_frame();
    auto* frame_n2 = static_cast<int*>(frames.allocate(sizeof(int), alignof(int)));
    *frame_n2 = 3;
    CHECK(*frame_n == 1);
    CHECK(*frame_n1 == 2);
    CHECK(frame_n != frame_n1);
    CHECK(frame_n1 != frame_n2);
    CHECK(frame_n != frame_n2);
}

TEST_CASE("begin_frame resets usage and keeps the peak") {
    FrameAllocator frames(8192);
    frames.allocate(5000, 8);
    frames.begin_frame();
    CHECK(frames.used_bytes() == 0);
    frames.allocate(10, 8);
    CHECK(frames.peak_bytes() >= 5000);
}

TEST_CASE("threads allocate concurrently without overlapping") {
    constexpr size_t kThreads = 4;
    constexpr size_t kPerThread = 1000;
    constexpr size_t kSize = 16;
    FrameAllocator frames(kThreads * kPerThread * kSize);

    std::vector<std::byte*> blocks(kThreads * kPerThread);
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (size_t t = 0; t < kThreads; ++t) {
        workers.emplace_back([&frames, &blocks, t] {
            for (size_t i = 0; i < kPerThread; ++i) {
                auto* block = static_cast<std::byte*>(frames.allocate(kSize, kSize));
                std::memset(block, static_cast<int>(t + 1), kSize);
                blocks[t * kPerThread + i] = block;
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    // Each thread still finds its own bytes: no block was handed out twice.
    for (size_t t = 0; t < kThreads; ++t) {
        for (size_t i = 0; i < kPerThread; ++i) {
            const std::byte* block = blocks[t * kPerThread + i];
            CHECK(block[0] == static_cast<std::byte>(t + 1));
            CHECK(block[kSize - 1] == static_cast<std::byte>(t + 1));
        }
    }
    std::ranges::sort(blocks);
    CHECK(std::ranges::adjacent_find(blocks) == blocks.end());
    CHECK(frames.used_bytes() == kThreads * kPerThread * kSize); // 16-byte blocks need no padding
}

TEST_CASE("frame allocations never touch the heap") {
    FrameAllocator frames(4096);
    const NoHeapAllocationScope no_alloc;
    for (int frame = 0; frame < 4; ++frame) {
        frames.begin_frame();
        frames.allocate(1000, 16);
        frames.allocate(1000, 16);
    }
    CHECK(no_alloc.allocations() == 0);
}
