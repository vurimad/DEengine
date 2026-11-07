#pragma once

#include <cstddef>
#include <cstdint>

// Physics builds Jolt for AVX2, and that isn't contained by physics: Jolt's static initializers run before main, and
// the linker keeps whichever copy of a shared inline function it sees first, so AVX-encoded glm, flecs and std
// functions end up used by the whole program. Whatever links physics therefore checks the CPU here before any C++
// static initializer runs, and leaves with a message instead of an illegal instruction. Nothing in here may use what's
// shared with code built for AVX2, so no std templates and no snprintf.
namespace de::platform {

// What the CPU says about itself, in the registers it says it in.
struct CpuRegisters {
    uint32_t leaf1_ecx = 0;
    uint32_t leaf7_ebx = 0;
    uint32_t extended1_ecx = 0;
    uint64_t xcr0 = 0; // what the OS saves when it switches threads; 0 if it won't say
};

// This machine's.
CpuRegisters read_cpu_registers();

// The first thing a program with physics in it needs and these don't have, or null if they have it all.
const char* first_missing_cpu_feature(const CpuRegisters& registers);

// What to tell the player about it. Always ends with a null.
void cpu_requirement_message(const char* missing, char* out, size_t out_size);

// Whether the check ran, which it did before anything else if this is linked in. Physics asks, which is what links it.
bool cpu_checked_at_startup();

} // namespace de::platform
