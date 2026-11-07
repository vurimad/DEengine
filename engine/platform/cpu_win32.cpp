#include "platform/cpu.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <intrin.h>

namespace de::platform {

namespace {

constinit bool checked_at_startup = false;

void __cdecl check_cpu_at_startup() {
    const char* const missing = first_missing_cpu_feature(read_cpu_registers());
    if (missing != nullptr) {
        char message[256];
        cpu_requirement_message(missing, message, sizeof(message));
        MessageBoxA(nullptr, message, "Can't start", MB_OK | MB_ICONERROR);
        ExitProcess(1);
    }
    checked_at_startup = true;
}

} // namespace

CpuRegisters read_cpu_registers() {
    CpuRegisters out;
    int regs[4] = {};
    __cpuid(regs, 0);
    const int max_leaf = regs[0];
    if (max_leaf >= 1) {
        __cpuid(regs, 1);
        out.leaf1_ecx = static_cast<uint32_t>(regs[2]);
    }
    if (max_leaf >= 7) {
        __cpuidex(regs, 7, 0);
        out.leaf7_ebx = static_cast<uint32_t>(regs[1]);
    }
    constexpr unsigned kExtendedLeaf1 = 0x80000001U;
    __cpuid(regs, 0x80000000U);
    if (static_cast<unsigned>(regs[0]) >= kExtendedLeaf1) {
        __cpuid(regs, kExtendedLeaf1);
        out.extended1_ecx = static_cast<uint32_t>(regs[2]);
    }
    constexpr uint32_t kOsUsesXsave = 1U << 27U;
    if ((out.leaf1_ecx & kOsUsesXsave) != 0) {
        out.xcr0 = _xgetbv(0);
    }
    return out;
}

bool cpu_checked_at_startup() {
    return checked_at_startup;
}

} // namespace de::platform

// The C runtime runs what's in .CRT$XCA to .CRT$XCZ in order, and ordinary static initializers are in XCU. Named and
// included, so the linker doesn't drop what nothing refers to.
#pragma section(".CRT$XCB", read)
extern "C" {
extern void(__cdecl* const de_platform_check_cpu)();
__declspec(allocate(".CRT$XCB")) void(__cdecl* const de_platform_check_cpu)() = &de::platform::check_cpu_at_startup;
}
#pragma comment(linker, "/include:de_platform_check_cpu")
