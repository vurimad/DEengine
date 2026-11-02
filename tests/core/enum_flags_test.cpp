#include "core/enum_flags.h"

#include <doctest/doctest.h>

#include <cstdint>

namespace {

enum class Fruit : uint8_t {
    None = 0,
    Apple = 1 << 0,
    Pear = 1 << 1,
    Fig = 1 << 7,
};
DE_ENUM_FLAGS(Fruit)

} // namespace

using de::core::has_all;
using de::core::has_any;

TEST_CASE("flag operators combine, mask and invert") {
    Fruit basket = Fruit::Apple | Fruit::Fig;
    CHECK(has_any(basket, Fruit::Apple));
    CHECK_FALSE(has_any(basket, Fruit::Pear));
    CHECK(has_any(basket, Fruit::Pear | Fruit::Fig));
    CHECK_FALSE(has_all(basket, Fruit::Pear | Fruit::Fig));
    CHECK(has_all(basket, Fruit::Apple | Fruit::Fig));
    CHECK(has_all(basket, Fruit::None));

    basket |= Fruit::Pear;
    CHECK(has_all(basket, Fruit::Apple | Fruit::Pear | Fruit::Fig));
    basket &= ~Fruit::Apple;
    CHECK(basket == (Fruit::Pear | Fruit::Fig));
    CHECK((basket & Fruit::Apple) == Fruit::None);
    static_assert((Fruit::Apple | Fruit::Pear) == static_cast<Fruit>(3));
}
