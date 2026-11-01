#pragma once

#include "core/allocator.h"
#include "core/array.h"
#include "core/assert.h"
#include "core/hash.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace de::core {

// Generational handle into a SlotMap<T>: 8 bytes, trivially copyable, safe to keep.
// A handle to a removed element never resolves, even after its slot is reused.
template <typename T>
struct SlotHandle {
    uint32_t index = 0;
    uint32_t generation = 0; // 0 never belongs to a live element: a default handle is invalid

    constexpr bool is_valid() const { return generation != 0; }
    constexpr bool operator==(const SlotHandle&) const = default;
};

template <typename T>
struct Hash<SlotHandle<T>> {
    constexpr uint64_t operator()(SlotHandle<T> handle) const {
        return mix64((static_cast<uint64_t>(handle.generation) << 32U) | handle.index);
    }
};

// Dense storage with generational handles (docs/design.md, Core library).
// Values stay contiguous for fast iteration; removal swaps the last value into the hole,
// so iteration order is unspecified. Lookup by handle is O(1).
//
// Slot generations are odd while the slot holds a value and even while it is free, so a
// handle (always odd) only matches a live slot of the same generation. A slot whose generation
// would wrap is retired instead of reused (after 2^31 reuses: 8 bytes lost, safety kept).
template <typename T>
class SlotMap {
public:
    using Handle = SlotHandle<T>;

    SlotMap() : SlotMap(heap_allocator()) {}
    explicit SlotMap(Allocator& allocator) : values_(allocator), value_slots_(allocator), slots_(allocator) {}

    SlotMap(const SlotMap&) = delete;
    SlotMap& operator=(const SlotMap&) = delete;

    // The source is left empty and reusable: its free list must not point into its emptied slots.
    SlotMap(SlotMap&& other) noexcept
        : values_(std::move(other.values_)), value_slots_(std::move(other.value_slots_)),
          slots_(std::move(other.slots_)), free_head_(other.free_head_) {
        other.free_head_ = kNoSlot;
    }

    SlotMap& operator=(SlotMap&& other) noexcept {
        if (this != &other) {
            values_ = std::move(other.values_);
            value_slots_ = std::move(other.value_slots_);
            slots_ = std::move(other.slots_);
            free_head_ = other.free_head_;
            other.free_head_ = kNoSlot;
        }
        return *this;
    }

    ~SlotMap() = default;

    size_t size() const { return values_.size(); }
    [[nodiscard]] bool empty() const { return values_.empty(); }

    void reserve(size_t capacity) {
        values_.reserve(capacity);
        value_slots_.reserve(capacity);
        slots_.reserve(capacity);
    }

    template <typename... Args>
    Handle emplace(Args&&... args) {
        uint32_t slot_index = 0;
        if (free_head_ != kNoSlot) {
            slot_index = free_head_;
            free_head_ = slots_[slot_index].value_or_next_free;
        } else {
            DE_ASSERT_MSG(slots_.size() < kNoSlot, "SlotMap is full");
            slot_index = static_cast<uint32_t>(slots_.size());
            slots_.push(Slot{});
        }

        values_.emplace(std::forward<Args>(args)...);
        value_slots_.push(slot_index);

        Slot& slot = slots_[slot_index];
        slot.generation += 1; // even (free) -> odd (live)
        slot.value_or_next_free = static_cast<uint32_t>(values_.size() - 1);
        return Handle{.index = slot_index, .generation = slot.generation};
    }

    Handle insert(T value) { return emplace(std::move(value)); }

    // The value for handle, or null if it was removed (or never belonged to this map).
    [[nodiscard]] T* get(Handle handle) {
        const Slot* slot = live_slot(handle);
        return slot ? &values_[slot->value_or_next_free] : nullptr;
    }
    [[nodiscard]] const T* get(Handle handle) const {
        const Slot* slot = live_slot(handle);
        return slot ? &values_[slot->value_or_next_free] : nullptr;
    }

    [[nodiscard]] bool contains(Handle handle) const { return live_slot(handle) != nullptr; }

    // Returns true if handle was live.
    bool remove(Handle handle) {
        if (live_slot(handle) == nullptr) {
            return false;
        }
        Slot& slot = slots_[handle.index];
        const uint32_t value_index = slot.value_or_next_free;

        values_.remove_swap(value_index);
        value_slots_.remove_swap(value_index);
        if (value_index < values_.size()) {
            slots_[value_slots_[value_index]].value_or_next_free = value_index; // the value that moved in
        }

        slot.generation += 1; // odd (live) -> even (free)
        if (slot.generation == 0) {
            return true; // generation wrapped: retire the slot, so no old handle can ever match it again
        }
        slot.value_or_next_free = free_head_;
        free_head_ = handle.index;
        return true;
    }

    // Removes every value and invalidates every handle. Keeps the capacity.
    void clear() {
        values_.clear();
        value_slots_.clear();
        free_head_ = kNoSlot;
        for (size_t i = slots_.size(); i-- > 0;) {
            Slot& slot = slots_[i];
            if ((slot.generation & 1U) != 0) {
                slot.generation += 1;
            }
            if (slot.generation == 0) {
                continue; // retired (generation wrapped)
            }
            slot.value_or_next_free = free_head_;
            free_head_ = static_cast<uint32_t>(i);
        }
    }

    // Dense iteration over the values, in unspecified order.
    T* begin() { return values_.begin(); }
    T* end() { return values_.end(); }
    const T* begin() const { return values_.begin(); }
    const T* end() const { return values_.end(); }
    std::span<T> values() { return values_.span(); }
    std::span<const T> values() const { return values_.span(); }

    // The handle of the value at values()[value_index].
    Handle handle_at(size_t value_index) const {
        const uint32_t slot_index = value_slots_[value_index];
        return Handle{.index = slot_index, .generation = slots_[slot_index].generation};
    }

    Allocator& allocator() const { return values_.allocator(); }

private:
    struct Slot {
        uint32_t value_or_next_free = 0; // live: index into values_; free: next free slot
        uint32_t generation = 0;
    };

    static constexpr uint32_t kNoSlot = UINT32_MAX;

    const Slot* live_slot(Handle handle) const {
        if ((handle.generation & 1U) == 0 || handle.index >= slots_.size()) {
            return nullptr;
        }
        const Slot& slot = slots_[handle.index];
        return slot.generation == handle.generation ? &slot : nullptr;
    }

    Array<T> values_;
    Array<uint32_t> value_slots_; // slot index of each value
    Array<Slot> slots_;
    uint32_t free_head_ = kNoSlot;
};

} // namespace de::core
