#pragma once

#include <cstdint>

namespace de::platform {

inline constexpr uint64_t kNsPerMs = 1'000'000;
inline constexpr uint64_t kNsPerSecond = 1'000'000'000;

// Monotonic nanoseconds. The same clock as input event timestamps (RawInput::timestamp_ns),
// so a key press can be placed precisely between simulation ticks.
uint64_t time_ns();

// Sleeps at least duration_ns. The OS may oversleep by up to a scheduler tick.
void sleep_ns(uint64_t duration_ns);

} // namespace de::platform
