#pragma once

#include "core/assert.h"

#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace de::core {

enum class ErrorCode : uint16_t {
    Unknown,
    InvalidArgument,
    NotFound,
    AlreadyExists,
    OutOfMemory,
    OutOfRange,
    Unsupported,
    IoError,
    CorruptData,
    Timeout,
};

// The general-purpose error. message must have static storage duration (a string literal),
// so creating or passing an error never allocates. Systems may define their own error types.
struct Error {
    ErrorCode code = ErrorCode::Unknown;
    const char* message = "";
};

// Wraps an error on its way into a Result, so Result<Error, Error> is never ambiguous.
template <typename E>
struct Failure {
    E error;
};

template <typename E>
constexpr Failure<std::decay_t<E>> fail(E&& error) {
    return {std::forward<E>(error)};
}

inline Failure<Error> fail(ErrorCode code, const char* message) {
    return {Error{.code = code, .message = message}};
}

// A value or an error, in place of exceptions (docs/design.md, Code conventions).
//     Result<Mesh> load_mesh(AssetId id);
//     if (!mesh) { return fail(ErrorCode::NotFound, "mesh not in archive index"); }
template <typename T, typename E = Error>
class [[nodiscard]] Result {
    static_assert(!std::is_reference_v<T>, "Result<T&> is not supported: use a pointer");
    static_assert(!std::is_void_v<E>, "Result needs an error type");

public:
    Result(T value) : value_(std::move(value)), has_value_(true) {}                     // NOLINT: implicit by design
    Result(Failure<E> failure) : error_(std::move(failure.error)), has_value_(false) {} // NOLINT

    Result(const Result& other)
        requires(std::is_copy_constructible_v<T> && std::is_copy_constructible_v<E>)
        : has_value_(other.has_value_) {
        construct_from(other);
    }

    Result(Result&& other) noexcept : has_value_(other.has_value_) { construct_from(std::move(other)); }

    Result& operator=(const Result& other)
        requires(std::is_copy_constructible_v<T> && std::is_copy_constructible_v<E>)
    {
        if (this != &other) {
            destroy();
            has_value_ = other.has_value_;
            construct_from(other);
        }
        return *this;
    }

    Result& operator=(Result&& other) noexcept {
        if (this != &other) {
            destroy();
            has_value_ = other.has_value_;
            construct_from(std::move(other));
        }
        return *this;
    }

    ~Result() { destroy(); }

    bool ok() const { return has_value_; }
    explicit operator bool() const { return has_value_; }

    T& value() & {
        DE_ASSERT_MSG(has_value_, "Result::value() called on an error");
        return value_;
    }
    const T& value() const& {
        DE_ASSERT_MSG(has_value_, "Result::value() called on an error");
        return value_;
    }
    T&& value() && {
        DE_ASSERT_MSG(has_value_, "Result::value() called on an error");
        return std::move(value_);
    }

    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }
    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }

    T value_or(T fallback) const&
        requires std::is_copy_constructible_v<T>
    {
        return has_value_ ? value_ : std::move(fallback);
    }

    const E& error() const {
        DE_ASSERT_MSG(!has_value_, "Result::error() called on a value");
        return error_;
    }

private:
    template <typename Other>
    void construct_from(Other&& other) {
        if (has_value_) {
            ::new (static_cast<void*>(&value_)) T(std::forward<Other>(other).value_);
        } else {
            ::new (static_cast<void*>(&error_)) E(std::forward<Other>(other).error_);
        }
    }

    void destroy() {
        if (has_value_) {
            value_.~T();
        } else {
            error_.~E();
        }
    }

    // Anonymous union members are private through the enclosing class; clang-tidy reads them as public.
    union {
        T value_; // NOLINT(readability-identifier-naming)
        E error_; // NOLINT(readability-identifier-naming)
    };
    bool has_value_;
};

// Success carries no value: `return {};` for success, `return fail(...)` for errors.
template <typename E>
class [[nodiscard]] Result<void, E> {
    static_assert(std::is_default_constructible_v<E>, "Result<void, E> needs a default-constructible E");

public:
    Result() = default;
    Result(Failure<E> failure) : error_(std::move(failure.error)), has_error_(true) {} // NOLINT: implicit by design

    bool ok() const { return !has_error_; }
    explicit operator bool() const { return !has_error_; }

    const E& error() const {
        DE_ASSERT_MSG(has_error_, "Result::error() called on success");
        return error_;
    }

private:
    E error_{};
    bool has_error_ = false;
};

} // namespace de::core
