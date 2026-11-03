#include "core/text.h"

#include <doctest/doctest.h>

#include <string_view>

using namespace std::string_view_literals;

TEST_CASE("text lines split, trim and lose their comments") {
    std::string_view text = de::core::skip_bom("\xEF\xBB\xBF  one two \r\n\nthree # four\nfive"sv);
    CHECK((de::core::trim(de::core::next_line(text)) == "one two"));
    CHECK(de::core::next_line(text).empty());
    CHECK((de::core::strip_comment(de::core::next_line(text)) == "three"));
    CHECK((de::core::next_line(text) == "five")); // no final line end
    CHECK(text.empty());
    CHECK((de::core::skip_bom("plain"sv) == "plain"));
    CHECK(de::core::strip_comment("  # only a comment"sv).empty());
}
