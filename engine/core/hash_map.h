#pragma once

#include "core/allocator.h"
#include "core/detail/std_allocator.h"
#include "core/hash.h"

#include <cstddef>
#include <unordered_map>
#include <utility>

namespace de::core {

// Hash map (replaces std::unordered_map).
//
// Phase 1: std::unordered_map underneath, allocating through the given allocator and hashing
// with Hash<K>. The API is shaped for the open-addressing table that replaces it: lookups
// return a pointer or null, nothing inserts behind your back, and iteration order is unspecified.
// Not copyable. Pointers returned by find stay valid until the map is modified.
//
// Phase 1 cost to know: MSVC's unordered_map allocates a sentinel node for the moved-from map,
// so moving a HashMap allocates on Windows. Don't move maps per frame until the own table lands.
template <typename K, typename V, typename H = Hash<K>>
class HashMap {
public:
    // What iteration yields: `for (auto [key, value] : map)` or `entry.key` / `entry.value`.
    struct Entry {
        const K& key;
        V& value;
    };
    struct ConstEntry {
        const K& key;
        const V& value;
    };

private:
    struct StdHasher {
        H hasher;
        size_t operator()(const K& key) const { return static_cast<size_t>(hasher(key)); }
    };
    struct StdEqual {
        bool operator()(const K& a, const K& b) const { return a == b; }
    };
    using StdPair = std::pair<const K, V>;
    using StdMap = std::unordered_map<K, V, StdHasher, StdEqual, detail::StdAllocator<StdPair>>;

    template <typename StdIterator, typename EntryType>
    class Iterator {
    public:
        explicit Iterator(StdIterator it) : it_(std::move(it)) {}
        EntryType operator*() const { return EntryType{it_->first, it_->second}; }
        Iterator& operator++() {
            ++it_;
            return *this;
        }
        bool operator==(const Iterator& other) const { return it_ == other.it_; }

    private:
        StdIterator it_;
    };

public:
    HashMap() : HashMap(heap_allocator()) {}
    explicit HashMap(Allocator& allocator)
        : map_(0, StdHasher{}, StdEqual{}, detail::StdAllocator<StdPair>(allocator)) {}

    HashMap(const HashMap&) = delete;
    HashMap& operator=(const HashMap&) = delete;
    HashMap(HashMap&&) noexcept = default;
    HashMap& operator=(HashMap&&) noexcept = default;
    ~HashMap() = default;

    size_t size() const { return map_.size(); }
    [[nodiscard]] bool empty() const { return map_.empty(); }
    void reserve(size_t count) { map_.reserve(count); }
    void clear() { map_.clear(); }

    // The value for key, or null.
    [[nodiscard]] V* find(const K& key) {
        auto it = map_.find(key);
        return it == map_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] const V* find(const K& key) const {
        auto it = map_.find(key);
        return it == map_.end() ? nullptr : &it->second;
    }

    [[nodiscard]] bool contains(const K& key) const { return map_.find(key) != map_.end(); }

    // Inserts key or overwrites its value. Returns the stored value.
    V& set(const K& key, V value) { return map_.insert_or_assign(key, std::move(value)).first->second; }

    // The value for key, default-constructed first if missing (the explicit form of operator[]).
    V& find_or_insert(const K& key) { return map_.try_emplace(key).first->second; }

    // Returns true if key was present.
    bool remove(const K& key) { return map_.erase(key) != 0; }

    auto begin() { return Iterator<typename StdMap::iterator, Entry>(map_.begin()); }
    auto end() { return Iterator<typename StdMap::iterator, Entry>(map_.end()); }
    auto begin() const { return Iterator<typename StdMap::const_iterator, ConstEntry>(map_.begin()); }
    auto end() const { return Iterator<typename StdMap::const_iterator, ConstEntry>(map_.end()); }

    Allocator& allocator() const { return map_.get_allocator().allocator(); }

private:
    StdMap map_;
};

} // namespace de::core
