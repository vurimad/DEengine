#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace de::core {

// 64-bit finalizer (MurmurHash3 fmix64). Spreads every input bit, so hash tables don't
// degrade on sequential or aligned keys (std::hash is the identity for integers on some STLs).
constexpr uint64_t mix64(uint64_t x) {
    x ^= x >> 33U;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33U;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33U;
    return x;
}

// FNV-1a, 64-bit. constexpr so names can be hashed at compile time.
constexpr uint64_t fnv1a_64(const char* data, size_t length) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < length; ++i) {
        hash ^= static_cast<uint8_t>(data[i]);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

// Hash<T> is specialized per key type. Integers, enums and pointers are provided here;
// other key types specialize it next to their definition.
template <typename T>
struct Hash;

template <typename T>
    requires(std::is_integral_v<T> || std::is_enum_v<T>)
struct Hash<T> {
    constexpr uint64_t operator()(T value) const { return mix64(static_cast<uint64_t>(value)); }
};

template <typename T>
struct Hash<T*> {
    uint64_t operator()(const T* ptr) const { return mix64(reinterpret_cast<uintptr_t>(ptr)); }
};

} // namespace de::core
