#pragma once

#include "core/allocator.h"

#include <cstddef>
#include <type_traits>

namespace de::core::detail {

// Adapts an Allocator to the std allocator requirements, so phase 1 containers built on std
// still allocate through the allocator they were given. Not for use outside core.
template <typename T>
class StdAllocator {
public:
    // NOLINTBEGIN(readability-identifier-naming): names required by the std allocator requirements
    using value_type = T;
    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using is_always_equal = std::false_type;
    // NOLINTEND(readability-identifier-naming)

    explicit StdAllocator(Allocator& allocator) noexcept : allocator_(&allocator) {}

    // Rebinding copy: required by std containers, so it must stay implicit.
    template <typename U>
    StdAllocator(const StdAllocator<U>& other) noexcept : allocator_(&other.allocator()) {} // NOLINT

    T* allocate(size_t count) { return allocate_array<T>(*allocator_, count); }
    void deallocate(T* ptr, size_t count) noexcept { deallocate_array(*allocator_, ptr, count); }

    Allocator& allocator() const noexcept { return *allocator_; }

    template <typename U>
    bool operator==(const StdAllocator<U>& other) const noexcept {
        return allocator_ == &other.allocator();
    }

private:
    Allocator* allocator_;
};

} // namespace de::core::detail
