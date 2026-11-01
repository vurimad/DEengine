#pragma once

#include "core/allocator.h"
#include "core/assert.h"

#include <cstddef>
#include <cstring>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace de::core {

// Array that keeps its first N elements inside the object and only uses the allocator beyond
// that. For lists that are usually short: hit lists, children, per-call scratch.
//
// Own implementation (std has no small vector). Not copyable; moves relocate the elements
// when they are inline and steal the heap block otherwise. A moved-from InlineArray is empty
// and inline, ready for reuse. Converts implicitly to std::span.
template <typename T, size_t N>
class InlineArray {
    static_assert(N > 0, "InlineArray needs at least one inline element: use Array");

public:
    InlineArray() : InlineArray(heap_allocator()) {}
    explicit InlineArray(Allocator& allocator) : data_(inline_data()), allocator_(&allocator) {}

    InlineArray(const InlineArray&) = delete;
    InlineArray& operator=(const InlineArray&) = delete;

    InlineArray(InlineArray&& other) noexcept : data_(inline_data()), allocator_(other.allocator_) { take(other); }

    InlineArray& operator=(InlineArray&& other) noexcept {
        if (this != &other) {
            release();
            allocator_ = other.allocator_;
            take(other);
        }
        return *this;
    }

    ~InlineArray() { release(); }

    size_t size() const { return size_; }
    size_t capacity() const { return capacity_; }
    [[nodiscard]] bool empty() const { return size_ == 0; }

    // True while the elements live inside the object (no heap block).
    bool is_inline() const { return data_ == inline_data(); }

    void reserve(size_t capacity) {
        if (capacity > capacity_) {
            grow(capacity);
        }
    }

    // New elements are value-initialized (zero for trivial types).
    void resize(size_t size) {
        if (size > size_) {
            reserve(size);
            for (size_t i = size_; i < size; ++i) {
                ::new (static_cast<void*>(data_ + i)) T();
            }
        } else {
            destroy_range(size, size_);
        }
        size_ = size;
    }

    // Destroys all elements, keeps the capacity (and the heap block, if any).
    void clear() {
        destroy_range(0, size_);
        size_ = 0;
    }

    T& operator[](size_t index) {
        DE_ASSERT(index < size_);
        return data_[index];
    }
    const T& operator[](size_t index) const {
        DE_ASSERT(index < size_);
        return data_[index];
    }

    T& front() {
        DE_ASSERT(size_ > 0);
        return data_[0];
    }
    const T& front() const {
        DE_ASSERT(size_ > 0);
        return data_[0];
    }
    T& back() {
        DE_ASSERT(size_ > 0);
        return data_[size_ - 1];
    }
    const T& back() const {
        DE_ASSERT(size_ > 0);
        return data_[size_ - 1];
    }

    T* data() { return data_; }
    const T* data() const { return data_; }
    T* begin() { return data_; }
    T* end() { return data_ + size_; }
    const T* begin() const { return data_; }
    const T* end() const { return data_ + size_; }

    std::span<T> span() { return {data_, size_}; }
    std::span<const T> span() const { return {data_, size_}; }

    template <typename... Args>
    T& emplace(Args&&... args) {
        if (size_ == capacity_) {
            return emplace_grow(std::forward<Args>(args)...);
        }
        T* item = ::new (static_cast<void*>(data_ + size_)) T(std::forward<Args>(args)...);
        ++size_;
        return *item;
    }

    T& push(const T& value) { return emplace(value); }
    T& push(T&& value) { return emplace(std::move(value)); }

    void pop() {
        DE_ASSERT(size_ > 0);
        --size_;
        data_[size_].~T();
    }

    // O(1): moves the last element into the hole, so order changes.
    void remove_swap(size_t index) {
        DE_ASSERT(index < size_);
        const size_t last = size_ - 1;
        if (index != last) {
            data_[index] = std::move(data_[last]);
        }
        pop();
    }

    // O(n): shifts the following elements down, keeping order.
    void remove_ordered(size_t index) {
        DE_ASSERT(index < size_);
        for (size_t i = index + 1; i < size_; ++i) {
            data_[i - 1] = std::move(data_[i]);
        }
        pop();
    }

    Allocator& allocator() const { return *allocator_; }

private:
    T* inline_data() { return reinterpret_cast<T*>(inline_storage_); }
    const T* inline_data() const { return reinterpret_cast<const T*>(inline_storage_); }

    void destroy_range(size_t first, size_t last) {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            for (size_t i = first; i < last; ++i) {
                data_[i].~T();
            }
        }
    }

    // Moves count elements into uninitialized destination and destroys the originals.
    static void relocate(T* source, size_t count, T* destination) {
        if constexpr (std::is_trivially_copyable_v<T>) {
            if (count > 0) {
                std::memcpy(static_cast<void*>(destination), static_cast<const void*>(source), count * sizeof(T));
            }
        } else {
            for (size_t i = 0; i < count; ++i) {
                ::new (static_cast<void*>(destination + i)) T(std::move(source[i]));
                source[i].~T();
            }
        }
    }

    size_t grown_capacity(size_t minimum) const {
        const size_t doubled = capacity_ * 2;
        return doubled > minimum ? doubled : minimum;
    }

    void grow(size_t minimum) {
        const size_t new_capacity = grown_capacity(minimum);
        T* new_data = allocate_array<T>(*allocator_, new_capacity);
        relocate(data_, size_, new_data);
        free_heap_block();
        data_ = new_data;
        capacity_ = new_capacity;
    }

    template <typename... Args>
    T& emplace_grow(Args&&... args) {
        const size_t new_capacity = grown_capacity(size_ + 1);
        T* new_data = allocate_array<T>(*allocator_, new_capacity);
        // Construct the new element before relocating: args may refer to an element of this array.
        T* item = ::new (static_cast<void*>(new_data + size_)) T(std::forward<Args>(args)...);
        relocate(data_, size_, new_data);
        free_heap_block();
        data_ = new_data;
        capacity_ = new_capacity;
        ++size_;
        return *item;
    }

    void free_heap_block() {
        if (!is_inline()) {
            deallocate_array(*allocator_, data_, capacity_);
        }
    }

    // Destroys everything and frees the heap block, leaving an empty inline array.
    void release() {
        clear();
        free_heap_block();
        data_ = inline_data();
        capacity_ = N;
    }

    // Takes other's elements. This array must be empty and inline.
    void take(InlineArray& other) {
        if (other.is_inline()) {
            relocate(other.data_, other.size_, data_);
            size_ = other.size_;
        } else {
            data_ = other.data_;
            size_ = other.size_;
            capacity_ = other.capacity_;
            other.data_ = other.inline_data();
            other.capacity_ = N;
        }
        other.size_ = 0;
    }

    T* data_;
    size_t size_ = 0;
    size_t capacity_ = N;
    Allocator* allocator_;
    alignas(T) std::byte inline_storage_[sizeof(T) * N];
};

} // namespace de::core
