#include "core/memory_stats.h"
#include "core/pool.h"
#include "test_support.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

using de::core::NoHeapAllocationScope;
using de::core::Pool;
using de::test::CountingAllocator;
using de::test::Tracked;

TEST_CASE("freed blocks are reused first") {
    Pool pool(32, 8);
    void* a = pool.allocate(32, 8);
    pool.deallocate(a, 32, 8);
    void* b = pool.allocate(32, 8);
    CHECK(a == b);
    pool.deallocate(b, 32, 8);
}

TEST_CASE("blocks are aligned and never overlap") {
    Pool pool(24, 64, 16);
    CHECK(pool.block_size() == 64);

    std::vector<void*> blocks;
    blocks.reserve(100);
    for (int i = 0; i < 100; ++i) {
        blocks.push_back(pool.allocate(24, 64));
        CHECK(reinterpret_cast<uintptr_t>(blocks.back()) % 64 == 0);
    }
    CHECK(pool.live_blocks() == 100);

    std::vector<uintptr_t> addresses;
    addresses.reserve(blocks.size());
    for (void* block : blocks) {
        addresses.push_back(reinterpret_cast<uintptr_t>(block));
    }
    std::ranges::sort(addresses);
    for (size_t i = 1; i < addresses.size(); ++i) {
        CHECK(addresses[i] - addresses[i - 1] >= pool.block_size());
    }

    for (void* block : blocks) {
        pool.deallocate(block, 24, 64);
    }
    CHECK(pool.live_blocks() == 0);
}

TEST_CASE("grows one chunk at a time from the backing allocator") {
    CountingAllocator backing;
    {
        Pool pool(16, 8, 10, backing);
        std::vector<void*> blocks;
        blocks.reserve(11);
        for (int i = 0; i < 10; ++i) {
            blocks.push_back(pool.allocate(16, 8));
        }
        CHECK(backing.allocations == 1);
        blocks.push_back(pool.allocate(16, 8));
        CHECK(backing.allocations == 2);
        CHECK(pool.capacity_blocks() == 20);
        for (void* block : blocks) {
            pool.deallocate(block, 16, 8);
        }
    }
    CHECK(backing.live_bytes == 0);
}

TEST_CASE("create and destroy construct and destroy objects") {
    const int live_before = Tracked::live;
    Pool pool(sizeof(Tracked), alignof(Tracked));
    auto* tracked = pool.create<Tracked>(5);
    CHECK(tracked->value == 5);
    CHECK(Tracked::live == live_before + 1);
    pool.destroy(tracked);
    CHECK(Tracked::live == live_before);
    CHECK(pool.live_blocks() == 0);
}

TEST_CASE("blocks smaller than a pointer still work") {
    Pool pool(1, 1);
    void* a = pool.allocate(1, 1);
    void* b = pool.allocate(1, 1);
    CHECK(a != b);
    pool.deallocate(a, 1, 1);
    pool.deallocate(b, 1, 1);
}

TEST_CASE("a pool with a free chunk stays off the heap") {
    Pool pool(64, 8, 64);
    pool.deallocate(pool.allocate(64, 8), 64, 8); // first chunk
    const NoHeapAllocationScope no_alloc;
    void* blocks[32];
    for (void*& block : blocks) {
        block = pool.allocate(64, 8);
    }
    for (void* block : blocks) {
        pool.deallocate(block, 64, 8);
    }
    CHECK(no_alloc.allocations() == 0);
}
