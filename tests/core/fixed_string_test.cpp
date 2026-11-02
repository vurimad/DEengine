#include "core/fixed_string.h"

#include <doctest/doctest.h>

#include <cstring>

using de::core::FixedString;

TEST_CASE("assign, append and read back") {
    FixedString<16> text("game:/");
    CHECK(text.size() == 6);
    REQUIRE(text.append("knight"));
    REQUIRE(text.append('/'));
    CHECK(text == "game:/knight/");
    CHECK(std::strcmp(text.c_str(), "game:/knight/") == 0);
    CHECK(text[0] == 'g');
}

TEST_CASE("an append that doesn't fit changes nothing") {
    FixedString<8> text("12345");
    CHECK_FALSE(text.append("6789"));
    CHECK(text == "12345");
    CHECK(text.append("678"));
    CHECK(text.size() == 8);
    CHECK_FALSE(text.append('9'));
}

TEST_CASE("assign keeps what fits and says so") {
    FixedString<4> text;
    CHECK(text.empty());
    CHECK_FALSE(text.assign("abcdef"));
    CHECK(text == "abcd");
    CHECK(text.assign("xy"));
    CHECK(text == "xy");
    text.clear();
    CHECK(text.empty());
    CHECK(std::strlen(text.c_str()) == 0);
}
