#include "core/allocator.h"

#include <new>

namespace de::core {

namespace {

class HeapAllocator final : public Allocator {
public:
    void* allocate(size_t size, size_t alignment) override {
        DE_ASSERT_MSG(alignment != 0 && (alignment & (alignment - 1)) == 0, "alignment must be a power of two");
        if (alignment <= __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
            return ::operator new(size);
        }
        return ::operator new(size, std::align_val_t{alignment});
    }

    void deallocate(void* ptr, size_t size, size_t alignment) override {
        if (alignment <= __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
            ::operator delete(ptr, size);
            return;
        }
        ::operator delete(ptr, size, std::align_val_t{alignment});
    }
};

} // namespace

Allocator& heap_allocator() {
    // Stateless and trivially destructible, so it stays usable during static destruction.
    static HeapAllocator heap;
    return heap;
}

} // namespace de::core
