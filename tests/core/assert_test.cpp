#include "core/assert.h"

#include <doctest/doctest.h>

// Compiled in every preset: in ship builds the asserts vanish, but their arguments must still
// count as used, or this file fails to build with warnings as errors.
TEST_CASE("DE_ASSERT arguments count as used in every build") {
    const int hit_points = 3;
    const char* reason = "hit points must be positive";
    DE_ASSERT(hit_points > 0);
    DE_ASSERT_MSG(hit_points > 0, reason);
    CHECK(DE_BUILD_DEV + DE_BUILD_SHIP == 1);
}
