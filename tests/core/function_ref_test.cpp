#include "core/function_ref.h"

#include <doctest/doctest.h>

using de::core::FunctionRef;

static_assert(sizeof(FunctionRef<void()>) == 2 * sizeof(void*), "two pointers, nothing else");

namespace {

int apply(FunctionRef<int(int)> function, int value) {
    return function(value);
}

int add_one(int value) {
    return value + 1;
}

struct Counter {
    int calls = 0;
    void operator()() { ++calls; }
};

} // namespace

TEST_CASE("calls a capturing lambda") {
    int offset = 10;
    CHECK(apply([&](int value) { return value + offset; }, 5) == 15);
    offset = 20;
    CHECK(apply([&](int value) { return value + offset; }, 5) == 25);
}

TEST_CASE("calls a free function") {
    CHECK(apply(add_one, 1) == 2);
    CHECK(apply(&add_one, 2) == 3);
}

TEST_CASE("refers to the callable instead of copying it") {
    Counter counter;
    const FunctionRef<void()> ref = counter;
    ref();
    ref();
    CHECK(counter.calls == 2);
}

TEST_CASE("forwards arguments and return values") {
    int target = 0;
    const auto set = [](int& out, int value) { out = value; };
    const FunctionRef<void(int&, int)> ref = set;
    ref(target, 7);
    CHECK(target == 7);
}
