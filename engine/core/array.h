#pragma once

#include "core/allocator.h"
#include "core/assert.h"
#include "core/detail/std_allocator.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace de::core {

// Growable contiguous array (replaces std::vector).
//
// Phase 1: std::vector underneath, allocating through the given allocator. Only this API is
// public, so the implementation can be replaced without touching engine code.
// Not copyable: copies are explicit (assign). Converts implicitly to std::span.
template <typename T>
class Array {
    static_assert(!std::is_same_v<T, bool>, "Array<bool> is not supported: use Array<uint8_t> (or BitSet later)");

public:
    Array() : Array(heap_allocator()) {}
    explicit Array(Allocator& allocator) : items_(detail::StdAllocator<T>(allocator)) {}

    Array(const Array&) = delete;
    Array& operator=(const Array&) = delete;
    // Moves leave the source empty (guaranteed, not left to the std implementation).
    Array(Array&& other) noexcept : items_(std::move(other.items_)) {}
    Array& operator=(Array&& other) noexcept {
        if (this != &other) {
            items_ = std::move(other.items_);
            other.items_.clear();
        }
        return *this;
    }
    ~Array() = default;

    size_t size() const { return items_.size(); }
    size_t capacity() const { return items_.capacity(); }
    [[nodiscard]] bool empty() const { return items_.empty(); }

    void reserve(size_t capacity) { items_.reserve(capacity); }

    // New elements are value-initialized (zero for trivial types).
    void resize(size_t size) { items_.resize(size); }
    void resize(size_t size, const T& value) { items_.resize(size, value); }

    // New elements have unspecified contents and must be written before they are read.
    // Phase 1 still zero-fills them; don't rely on it.
    void resize_uninit(size_t size)
        requires std::is_trivially_copyable_v<T>
    {
        items_.resize(size);
    }

    // Destroys all elements, keeps the capacity.
    void clear() { items_.clear(); }
    void shrink_to_fit() { items_.shrink_to_fit(); }

    T& operator[](size_t index) {
        DE_ASSERT(index < items_.size());
        return items_[index];
    }
    const T& operator[](size_t index) const {
        DE_ASSERT(index < items_.size());
        return items_[index];
    }

    T& front() {
        DE_ASSERT(!items_.empty());
        return items_.front();
    }
    const T& front() const {
        DE_ASSERT(!items_.empty());
        return items_.front();
    }
    T& back() {
        DE_ASSERT(!items_.empty());
        return items_.back();
    }
    const T& back() const {
        DE_ASSERT(!items_.empty());
        return items_.back();
    }

    T* data() { return items_.data(); }
    const T* data() const { return items_.data(); }
    T* begin() { return items_.data(); }
    T* end() { return items_.data() + items_.size(); }
    const T* begin() const { return items_.data(); }
    const T* end() const { return items_.data() + items_.size(); }

    std::span<T> span() { return {items_.data(), items_.size()}; }
    std::span<const T> span() const { return {items_.data(), items_.size()}; }

    T& push(const T& value) { return items_.emplace_back(value); }
    T& push(T&& value) { return items_.emplace_back(std::move(value)); }

    template <typename... Args>
    T& emplace(Args&&... args) {
        return items_.emplace_back(std::forward<Args>(args)...);
    }

    void pop() {
        DE_ASSERT(!items_.empty());
        items_.pop_back();
    }

    // O(1): moves the last element into the hole, so order changes.
    void remove_swap(size_t index) {
        DE_ASSERT(index < items_.size());
        if (index != items_.size() - 1) {
            items_[index] = std::move(items_.back());
        }
        items_.pop_back();
    }

    // O(n): shifts the following elements down, keeping order.
    void remove_ordered(size_t index) {
        DE_ASSERT(index < items_.size());
        items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(index));
    }

    // Replaces the contents with copies of items. This is the explicit copy.
    // items must not point into this array.
    void assign(std::span<const T> items) {
        DE_ASSERT_MSG(!overlaps(items), "Array::assign from the array's own elements");
        items_.assign(items.begin(), items.end());
    }

    Allocator& allocator() const { return items_.get_allocator().allocator(); }

private:
    bool overlaps(std::span<const T> items) const {
        const auto first = reinterpret_cast<uintptr_t>(items.data());
        const auto last = first + items.size_bytes();
        const auto own_first = reinterpret_cast<uintptr_t>(items_.data());
        const auto own_last = own_first + items_.size() * sizeof(T);
        return first < own_last && own_first < last;
    }

    std::vector<T, detail::StdAllocator<T>> items_;
};

} // namespace de::core
