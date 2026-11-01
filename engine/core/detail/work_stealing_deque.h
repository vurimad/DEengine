#pragma once

#include "core/allocator.h"
#include "core/assert.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

namespace de::core::detail {

// Fixed-capacity Chase-Lev work-stealing deque, using the memory orderings proven correct in
// Le, Pop, Cohen and Zappa Nardelli, "Correct and Efficient Work-Stealing for Weak Memory
// Models" (PPoPP 2013).
//
// One owner thread pushes and pops at the bottom (LIFO: the newest job, still warm in cache).
// Any thread steals from the top (FIFO: the oldest job, usually the biggest chunk of work).
// Items are stored word by word in relaxed atomics, so a thief that loses the race may read a
// mix of old and new words but never races in the C++ sense; its compare-exchange fails and the
// value is thrown away.
template <typename T>
class WorkStealingDeque {
    static_assert(std::is_trivially_copyable_v<T>, "items are copied word by word");
    static_assert(sizeof(T) % sizeof(uintptr_t) == 0, "items must be a whole number of words");

    static constexpr size_t kWords = sizeof(T) / sizeof(uintptr_t);

    struct Slot {
        std::atomic<uintptr_t> words[kWords];
    };

public:
    // capacity: a power of two.
    WorkStealingDeque(size_t capacity, Allocator& allocator)
        : slots_(allocate_array<Slot>(allocator, capacity)), mask_(capacity - 1), capacity_(capacity),
          allocator_(&allocator) {
        DE_VERIFY_MSG(is_power_of_two(capacity), "deque capacity must be a power of two");
        for (size_t i = 0; i < capacity; ++i) {
            ::new (static_cast<void*>(&slots_[i])) Slot{};
        }
    }

    ~WorkStealingDeque() { deallocate_array(*allocator_, slots_, capacity_); }

    WorkStealingDeque(const WorkStealingDeque&) = delete;
    WorkStealingDeque& operator=(const WorkStealingDeque&) = delete;
    WorkStealingDeque(WorkStealingDeque&&) = delete;
    WorkStealingDeque& operator=(WorkStealingDeque&&) = delete;

    // Owner only. False when full.
    bool push(const T& item) {
        const int64_t bottom = bottom_.load(std::memory_order_relaxed);
        const int64_t top = top_.load(std::memory_order_acquire);
        if (bottom - top >= static_cast<int64_t>(capacity_)) {
            return false;
        }
        write(bottom, item);
        bottom_.store(bottom + 1, std::memory_order_release);
        return true;
    }

    // Owner only. False when empty.
    bool pop(T& out) {
        const int64_t bottom = bottom_.load(std::memory_order_relaxed) - 1;
        bottom_.store(bottom, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        int64_t top = top_.load(std::memory_order_relaxed);

        if (top > bottom) { // empty
            bottom_.store(bottom + 1, std::memory_order_relaxed);
            return false;
        }
        out = read(bottom);
        if (top == bottom) {
            // The last item: race the thieves for it.
            const bool won =
                top_.compare_exchange_strong(top, top + 1, std::memory_order_seq_cst, std::memory_order_relaxed);
            bottom_.store(bottom + 1, std::memory_order_relaxed);
            return won;
        }
        return true;
    }

    // Any thread. False when empty or when another thread took the item first.
    bool steal(T& out) {
        int64_t top = top_.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const int64_t bottom = bottom_.load(std::memory_order_acquire);
        if (top >= bottom) {
            return false;
        }
        const T item = read(top);
        if (!top_.compare_exchange_strong(top, top + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
            return false;
        }
        out = item;
        return true;
    }

    // Approximate when other threads are active.
    size_t size_estimate() const {
        const int64_t size = bottom_.load(std::memory_order_relaxed) - top_.load(std::memory_order_relaxed);
        return size > 0 ? static_cast<size_t>(size) : 0;
    }

    size_t capacity() const { return capacity_; }

private:
    void write(int64_t index, const T& item) {
        uintptr_t words[kWords];
        std::memcpy(words, &item, sizeof(T));
        Slot& slot = slots_[static_cast<size_t>(index) & mask_];
        for (size_t i = 0; i < kWords; ++i) {
            slot.words[i].store(words[i], std::memory_order_relaxed);
        }
    }

    T read(int64_t index) const {
        uintptr_t words[kWords];
        const Slot& slot = slots_[static_cast<size_t>(index) & mask_];
        for (size_t i = 0; i < kWords; ++i) {
            words[i] = slot.words[i].load(std::memory_order_relaxed);
        }
        T item;
        std::memcpy(&item, words, sizeof(T));
        return item;
    }

    // Owner and thieves hammer different ends: keep them on separate cache lines.
    alignas(64) std::atomic<int64_t> top_{0};
    alignas(64) std::atomic<int64_t> bottom_{0};
    alignas(64) Slot* slots_;
    size_t mask_;
    size_t capacity_;
    Allocator* allocator_;
};

} // namespace de::core::detail
