#include "core/assert.h"
#include "core/log.h"

#include <doctest/doctest.h>

// Compiled in every preset: in ship builds DE_ASSERT and the dev-only log levels vanish, but
// their arguments must still count as used, or this file fails to build with warnings as errors.
TEST_CASE("DE_ASSERT arguments count as used in every build") {
    const int hit_points = 3;
    const char* reason = "hit points must be positive";
    DE_ASSERT(hit_points > 0);
    DE_ASSERT_MSG(hit_points > 0, reason);
    CHECK(DE_BUILD_DEV + DE_BUILD_SHIP == 1);
}

TEST_CASE("DE_LOG_TRACE and DE_LOG_DEBUG arguments count as used in every build") {
    const int frame = 7;
    const de::core::LogLevel saved = de::core::log_level();
    de::core::set_log_level(de::core::LogLevel::Info); // keep the test output quiet
    DE_LOG_TRACE("test", "frame %d", frame);
    DE_LOG_DEBUG("test", "frame %d", frame);
    de::core::set_log_level(saved);
    CHECK(frame == 7);
}

TEST_CASE("DE_VERIFY evaluates its condition in every build") {
    int calls = 0;
    DE_VERIFY(++calls == 1);
    DE_VERIFY_MSG(++calls == 2, "evaluated exactly once");
    CHECK(calls == 2);
}
