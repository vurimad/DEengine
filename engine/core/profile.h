#pragma once

// Profiler instrumentation (Tracy). Debug and dev builds link the Tracy client, which defines
// TRACY_ENABLE; ship builds don't, and every macro here compiles to nothing (arguments are still
// type-checked and count as used). The client is on-demand: until the Tracy viewer connects,
// zones cost a branch.
//
//     void update_streaming() { DE_PROFILE_ZONE("streaming"); ... }

#if defined(TRACY_ENABLE)

#include <tracy/Tracy.hpp>

#include <cstring>

// A zone covering the rest of the scope. name must be a string literal.
#define DE_PROFILE_ZONE(name) ZoneScopedN(name)

// Renames the current zone at runtime, e.g. with a job's name.
#define DE_PROFILE_ZONE_NAME(text) ZoneName((text), std::strlen(text))

// Marks the end of a frame on the main thread.
#define DE_PROFILE_FRAME() FrameMark

// A value over time in the profiler, e.g. heap allocations per frame. name must be a literal.
#define DE_PROFILE_PLOT(name, value) TracyPlot(name, value)

// Names the calling thread in the profiler (the OS name is set by the platform layer).
#define DE_PROFILE_THREAD_NAME(name) tracy::SetThreadName(name)

#else

#define DE_PROFILE_ZONE(name) static_cast<void>(sizeof(name))
#define DE_PROFILE_ZONE_NAME(text) static_cast<void>(true || (static_cast<void>(text), false))
#define DE_PROFILE_FRAME() static_cast<void>(0)
#define DE_PROFILE_PLOT(name, value) static_cast<void>(true || (static_cast<void>(value), false))
#define DE_PROFILE_THREAD_NAME(name) static_cast<void>(true || (static_cast<void>(name), false))

#endif
