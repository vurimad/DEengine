#include "core/result.h"
#include "test_support.h"

#include <doctest/doctest.h>

#include <cstring>

using de::core::Error;
using de::core::ErrorCode;
using de::core::fail;
using de::core::Result;
using de::test::MoveOnly;
using de::test::Tracked;

namespace {

Result<int> parse_digit(char c) {
    if (c < '0' || c > '9') {
        return fail(ErrorCode::InvalidArgument, "not a digit");
    }
    return c - '0';
}

Result<void> check_positive(int value) {
    if (value <= 0) {
        return fail(ErrorCode::OutOfRange, "value must be positive");
    }
    return {};
}

} // namespace

TEST_CASE("holds a value") {
    const Result<int> result = parse_digit('7');
    REQUIRE(result.ok());
    CHECK(static_cast<bool>(result));
    CHECK(result.value() == 7);
    CHECK(*result == 7);
}

TEST_CASE("holds an error with code and message") {
    const Result<int> result = parse_digit('x');
    REQUIRE_FALSE(result.ok());
    CHECK(result.error().code == ErrorCode::InvalidArgument);
    CHECK(std::strcmp(result.error().message, "not a digit") == 0);
    CHECK(result.value_or(-1) == -1);
}

TEST_CASE("void results") {
    CHECK(check_positive(1).ok());
    const Result<void> failed = check_positive(0);
    REQUIRE_FALSE(failed.ok());
    CHECK(failed.error().code == ErrorCode::OutOfRange);
}

TEST_CASE("custom error types") {
    enum class LoadError { Missing, Corrupt };
    const Result<float, LoadError> result = fail(LoadError::Corrupt);
    REQUIRE_FALSE(result.ok());
    CHECK(result.error() == LoadError::Corrupt);
}

TEST_CASE("move-only values move out") {
    Result<MoveOnly> result = MoveOnly(5);
    REQUIRE(result.ok());
    const MoveOnly taken = std::move(result).value();
    CHECK(taken.value == 5);
}

TEST_CASE("copies and moves construct and destroy the value exactly once") {
    const int live_before = Tracked::live;
    {
        Result<Tracked> a = Tracked(1);
        Result<Tracked> b = a; // copy
        Result<Tracked> c = std::move(a);
        CHECK(b->value == 1);
        CHECK(c->value == 1);

        const Result<Tracked> failed = fail(Error{.code = ErrorCode::NotFound, .message = "missing"});
        b = failed; // value -> error: the value is destroyed
        CHECK_FALSE(b.ok());
        b = std::move(c); // error -> value
        CHECK(b->value == 1);
        CHECK(Tracked::live - live_before == 3); // a and c hold moved-from values, b holds 1
    }
    CHECK(Tracked::live == live_before);
}
