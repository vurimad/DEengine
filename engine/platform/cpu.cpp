#include "platform/cpu.h"

namespace de::platform {

namespace {

struct Needed {
    uint32_t CpuRegisters::* in;
    uint32_t bit;
    const char* name;
};

// What /arch:AVX2 and -mavx2 -mbmi -mpopcnt -mlzcnt -mf16c let a compiler use, and it does: BMI and LZCNT show up in
// the executable outside anything that asks for them by name. FMA doesn't at the moment, but /arch:AVX2 allows it.
constexpr Needed kNeeded[] = {
    {.in = &CpuRegisters::leaf1_ecx, .bit = 19, .name = "SSE4.1"},
    {.in = &CpuRegisters::leaf1_ecx, .bit = 20, .name = "SSE4.2"},
    {.in = &CpuRegisters::leaf1_ecx, .bit = 23, .name = "POPCNT"},
    {.in = &CpuRegisters::leaf1_ecx, .bit = 28, .name = "AVX"},
    {.in = &CpuRegisters::leaf7_ebx, .bit = 5, .name = "AVX2"},
    {.in = &CpuRegisters::leaf1_ecx, .bit = 12, .name = "FMA"},
    {.in = &CpuRegisters::leaf1_ecx, .bit = 29, .name = "F16C"},
    {.in = &CpuRegisters::leaf7_ebx, .bit = 3, .name = "BMI1"},
    {.in = &CpuRegisters::leaf7_ebx, .bit = 8, .name = "BMI2"},
    {.in = &CpuRegisters::extended1_ecx, .bit = 5, .name = "LZCNT"},
};

// The two bits that say the OS saves the SSE and AVX registers, without which AVX faults however new the CPU.
constexpr uint64_t kOsSavesAvx = 0x6;

size_t append(char* out, size_t at, size_t out_size, const char* text) {
    while (*text != '\0' && at + 1 < out_size) {
        out[at] = *text;
        ++at;
        ++text;
    }
    return at;
}

} // namespace

const char* first_missing_cpu_feature(const CpuRegisters& registers) {
    for (const Needed& needed : kNeeded) {
        if (((registers.*needed.in >> needed.bit) & 1U) == 0) {
            return needed.name;
        }
    }
    return (registers.xcr0 & kOsSavesAvx) == kOsSavesAvx ? nullptr : "operating system support for AVX";
}

void cpu_requirement_message(const char* missing, char* out, size_t out_size) {
    if (out_size == 0) {
        return;
    }
    size_t at =
        append(out, 0, out_size, "This needs a processor with AVX2: Intel from 2013 or AMD from 2015 on. Missing: ");
    at = append(out, at, out_size, missing);
    out[at] = '\0';
}

} // namespace de::platform
