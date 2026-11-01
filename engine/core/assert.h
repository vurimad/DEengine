#pragma once

// DE_ASSERT checks invariants in debug and dev builds and compiles to nothing in ship builds.
// The condition is never evaluated in ship builds, so it must not have side effects.
//
// Minimal version: prints the failure and aborts. Logging integration and a debugger break
// arrive with the logging task (M0a).

namespace de::core {

[[noreturn]] void assert_failed(const char* condition, const char* message, const char* file, int line);

} // namespace de::core

#if DE_BUILD_DEV
#define DE_ASSERT(condition)                                                                                           \
    ((condition) ? static_cast<void>(0) : ::de::core::assert_failed(#condition, nullptr, __FILE__, __LINE__))
#define DE_ASSERT_MSG(condition, message)                                                                              \
    ((condition) ? static_cast<void>(0) : ::de::core::assert_failed(#condition, (message), __FILE__, __LINE__))
#else
// Still type-checked and counts as a use of its variables, but never evaluated.
#define DE_ASSERT(condition) static_cast<void>(true || (condition))
#define DE_ASSERT_MSG(condition, message) static_cast<void>(true || (condition))
#endif
