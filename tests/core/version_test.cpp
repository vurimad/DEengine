#include "core/version.h"

#include <doctest/doctest.h>

#include <cstdio>
#include <cstring>

TEST_CASE("engine version string matches the numeric version") {
    const de::core::Version version = de::core::engine_version();

    char expected[32] = {};
    std::snprintf(expected, sizeof(expected), "%u.%u.%u", version.major, version.minor, version.patch);

    CHECK(std::strcmp(de::core::engine_version_string(), expected) == 0);
}
