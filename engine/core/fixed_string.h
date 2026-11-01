#pragma once

#include "core/assert.h"

#include <cstddef>
#include <cstring>
#include <string_view>

namespace de::core {

// A string with fixed inline capacity that never allocates (replaces std::string where text is
// needed at runtime: paths, names in tools and debug views). N counts characters; one more byte
// holds the terminator. Appends that don't fit are refused, and the string is left unchanged.
template <size_t N>
class FixedString {
public:
    constexpr FixedString() = default;

    // Truncates text that doesn't fit; check fits() first when that matters.
    explicit FixedString(std::string_view text) { assign(text); }

    const char* c_str() const { return text_; }
    size_t size() const { return size_; }
    [[nodiscard]] bool empty() const { return size_ == 0; }
    static constexpr size_t capacity() { return N; }
    std::string_view view() const { return {text_, size_}; }

    static constexpr bool fits(size_t length) { return length <= N; }

    void clear() {
        size_ = 0;
        text_[0] = '\0';
    }

    // Replaces the contents. Returns false (and keeps the first N characters) if text is too long.
    bool assign(std::string_view text) {
        const size_t length = text.size() <= N ? text.size() : N;
        text.copy(text_, length);
        size_ = length;
        text_[size_] = '\0';
        return length == text.size();
    }

    // Appends text, or returns false and changes nothing if it doesn't fit.
    [[nodiscard]] bool append(std::string_view text) {
        if (text.size() > N - size_) {
            return false;
        }
        std::memcpy(text_ + size_, text.data(), text.size());
        size_ += text.size();
        text_[size_] = '\0';
        return true;
    }

    [[nodiscard]] bool append(char c) { return append(std::string_view(&c, 1)); }

    char& operator[](size_t index) {
        DE_ASSERT(index < size_);
        return text_[index];
    }
    char operator[](size_t index) const {
        DE_ASSERT(index < size_);
        return text_[index];
    }

    bool operator==(std::string_view other) const { return view() == other; }

private:
    char text_[N + 1] = {};
    size_t size_ = 0;
};

} // namespace de::core
