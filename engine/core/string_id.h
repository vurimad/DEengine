#pragma once

#include "core/hash.h"

#include <cstddef>
#include <cstdint>

namespace de::core {

// A name hashed to 64 bits (FNV-1a). Runtime code compares and looks up IDs, never strings;
// strings exist only at load time, in tools and in debug views (docs/design.md, Performance rules).
// The cooker checks for collisions, so equal IDs mean equal names.
class StringId {
public:
    // The invalid ID.
    constexpr StringId() = default;

    constexpr StringId(const char* data, size_t length) : value_(fnv1a_64(data, length)) {}

    // An ID read back from cooked data.
    static constexpr StringId from_value(uint64_t value) {
        StringId id;
        id.value_ = value;
        return id;
    }

    constexpr uint64_t value() const { return value_; }
    constexpr bool is_valid() const { return value_ != 0; }

    constexpr bool operator==(const StringId&) const = default;
    constexpr bool operator<(const StringId& other) const { return value_ < other.value_; }

private:
    uint64_t value_ = 0;
};

namespace literals {

// "longsword_r1_1"_sid, hashed at compile time.
constexpr StringId operator""_sid(const char* data, size_t length) {
    return {data, length};
}

} // namespace literals

template <>
struct Hash<StringId> {
    // Already a hash.
    constexpr uint64_t operator()(StringId id) const { return id.value(); }
};

} // namespace de::core
