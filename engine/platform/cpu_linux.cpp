#include "platform/cpu.h"

#include <cpuid.h>
#include <unistd.h>

namespace de::platform {

namespace {

constinit bool checked_at_startup = false;

size_t length_of(const char* text) {
    size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

// 101 is the first a program may use: before every ordinary static initializer, whichever file it's in.
__attribute__((constructor(101))) void check_cpu_at_startup() {
    const char* const missing = first_missing_cpu_feature(read_cpu_registers());
    if (missing != nullptr) {
        char message[256];
        cpu_requirement_message(missing, message, sizeof(message));
        if (write(STDERR_FILENO, message, length_of(message)) >= 0 && write(STDERR_FILENO, "\n", 1) >= 0) {
            _exit(1);
        }
        _exit(1);
    }
    checked_at_startup = true;
}

} // namespace

CpuRegisters read_cpu_registers() {
    CpuRegisters out;
    unsigned eax = 0;
    unsigned ebx = 0;
    unsigned ecx = 0;
    unsigned edx = 0;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) != 0) {
        out.leaf1_ecx = ecx;
    }
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) != 0) {
        out.leaf7_ebx = ebx;
    }
    if (__get_cpuid(0x80000001U, &eax, &ebx, &ecx, &edx) != 0) {
        out.extended1_ecx = ecx;
    }
    constexpr uint32_t kOsUsesXsave = 1U << 27U;
    if ((out.leaf1_ecx & kOsUsesXsave) != 0) {
        uint32_t low = 0;
        uint32_t high = 0;
        __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
        out.xcr0 = (static_cast<uint64_t>(high) << 32U) | low;
    }
    return out;
}

bool cpu_checked_at_startup() {
    return checked_at_startup;
}

} // namespace de::platform
