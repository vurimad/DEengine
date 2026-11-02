#pragma once

#include <type_traits>

namespace de::core {

// True if flags has at least one of bits set.
template <typename E>
    requires std::is_enum_v<E>
constexpr bool has_any(E flags, E bits) {
    using U = std::underlying_type_t<E>;
    return (static_cast<U>(flags) & static_cast<U>(bits)) != 0;
}

// True if flags has every one of bits set.
template <typename E>
    requires std::is_enum_v<E>
constexpr bool has_all(E flags, E bits) {
    using U = std::underlying_type_t<E>;
    return (static_cast<U>(flags) & static_cast<U>(bits)) == static_cast<U>(bits);
}

} // namespace de::core

// Bitwise operators for an enum class used as a set of flags. Use it at namespace scope, next to the enum:
//     enum class TextureUsage : uint8_t { None = 0, Sampled = 1 << 0, Storage = 1 << 1 };
//     DE_ENUM_FLAGS(TextureUsage)
// Enum names a type here, which can't be parenthesized.
// NOLINTBEGIN(bugprone-macro-parentheses)
#define DE_ENUM_FLAGS(Enum)                                                                                            \
    constexpr Enum operator|(Enum a, Enum b) {                                                                         \
        using U = std::underlying_type_t<Enum>;                                                                        \
        return static_cast<Enum>(static_cast<U>(static_cast<U>(a) | static_cast<U>(b)));                               \
    }                                                                                                                  \
    constexpr Enum operator&(Enum a, Enum b) {                                                                         \
        using U = std::underlying_type_t<Enum>;                                                                        \
        return static_cast<Enum>(static_cast<U>(static_cast<U>(a) & static_cast<U>(b)));                               \
    }                                                                                                                  \
    constexpr Enum operator~(Enum a) {                                                                                 \
        using U = std::underlying_type_t<Enum>;                                                                        \
        return static_cast<Enum>(static_cast<U>(~static_cast<U>(a)));                                                  \
    }                                                                                                                  \
    constexpr Enum& operator|=(Enum& a, Enum b) {                                                                      \
        return a = a | b;                                                                                              \
    }                                                                                                                  \
    constexpr Enum& operator&=(Enum& a, Enum b) {                                                                      \
        return a = a & b;                                                                                              \
    }
// NOLINTEND(bugprone-macro-parentheses)
