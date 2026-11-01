#include "core/string_id.h"

#include <doctest/doctest.h>

using de::core::StringId;
using namespace de::core::literals;

// Hashed at compile time.
static_assert("longsword_r1_1"_sid == StringId("longsword_r1_1", 14));
static_assert("longsword_r1_1"_sid != "longsword_r1_2"_sid);
static_assert(!StringId().is_valid());
static_assert(""_sid.is_valid()); // the empty name is a name

TEST_CASE("matches the FNV-1a 64-bit reference values") {
    CHECK(""_sid.value() == 0xcbf29ce484222325ULL);
    CHECK("a"_sid.value() == 0xaf63dc4c8601ec8cULL);
    CHECK("foobar"_sid.value() == 0x85944171f73967e8ULL);
}

TEST_CASE("from_value round-trips cooked IDs") {
    constexpr StringId kPoiseDamage = "poise_damage"_sid;
    CHECK(StringId::from_value(kPoiseDamage.value()) == kPoiseDamage);
}

TEST_CASE("orders by value for sorted tables") {
    const StringId a = StringId::from_value(1);
    const StringId b = StringId::from_value(2);
    CHECK(a < b);
    CHECK_FALSE(b < a);
}
