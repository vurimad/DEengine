#pragma once

// DE_ASSERT checks invariants in debug and dev builds and compiles away in ship builds; its
// condition must not have side effects. DE_VERIFY checks in every build, for conditions whose
// failure would corrupt memory (sizes from data, budgets).
//
// A failure logs the condition and location at Fatal level, flushes the log, runs the fatal
// handler and aborts.

namespace de::core {

// Runs after a fatal failure has been logged, right before abort. The platform layer installs
// one that breaks into an attached debugger. nullptr restores the default (abort only).
using FatalHandler = void (*)();
void set_fatal_handler(FatalHandler handler);

[[noreturn]] void assert_failed(const char* condition, const char* message, const char* file, int line);

// Runs the fatal handler and aborts, for code that has already logged why (DE_FATAL).
[[noreturn]] void abort_after_fatal_error();

} // namespace de::core

#define DE_VERIFY(condition)                                                                                           \
    ((condition) ? static_cast<void>(0) : ::de::core::assert_failed(#condition, nullptr, __FILE__, __LINE__))
#define DE_VERIFY_MSG(condition, message)                                                                              \
    ((condition) ? static_cast<void>(0) : ::de::core::assert_failed(#condition, (message), __FILE__, __LINE__))

#if DE_BUILD_DEV
#define DE_ASSERT(condition)                                                                                           \
    ((condition) ? static_cast<void>(0) : ::de::core::assert_failed(#condition, nullptr, __FILE__, __LINE__))
#define DE_ASSERT_MSG(condition, message)                                                                              \
    ((condition) ? static_cast<void>(0) : ::de::core::assert_failed(#condition, (message), __FILE__, __LINE__))
#else
// Still type-checked and counts as a use of its variables, but never evaluated.
#define DE_ASSERT(condition) static_cast<void>(true || (condition))
#define DE_ASSERT_MSG(condition, message) static_cast<void>(true || (condition) || (static_cast<void>(message), false))
#endif
