#include "platform/cpu.h"

#include <doctest/doctest.h>

#include <cstring>

using de::platform::CpuRegisters;

namespace {

CpuRegisters has_everything() {
    return {.leaf1_ecx = 0xFFFFFFFFU, .leaf7_ebx = 0xFFFFFFFFU, .extended1_ecx = 0xFFFFFFFFU, .xcr0 = 0x7U};
}

} // namespace

TEST_CASE("a CPU that has what physics needs is missing nothing") {
    CHECK(de::platform::first_missing_cpu_feature(has_everything()) == nullptr);
}

TEST_CASE("each thing physics needs of a CPU is asked for by its own bit") {
    struct Case {
        uint32_t CpuRegisters::* in;
        uint32_t bit;
        const char* name;
    };
    constexpr Case kCases[] = {
        {&CpuRegisters::leaf1_ecx, 19, "SSE4.1"}, {&CpuRegisters::leaf1_ecx, 20, "SSE4.2"},
        {&CpuRegisters::leaf1_ecx, 23, "POPCNT"}, {&CpuRegisters::leaf1_ecx, 28, "AVX"},
        {&CpuRegisters::leaf7_ebx, 5, "AVX2"},    {&CpuRegisters::leaf1_ecx, 12, "FMA"},
        {&CpuRegisters::leaf1_ecx, 29, "F16C"},   {&CpuRegisters::leaf7_ebx, 3, "BMI1"},
        {&CpuRegisters::leaf7_ebx, 8, "BMI2"},    {&CpuRegisters::extended1_ecx, 5, "LZCNT"},
    };
    for (const Case& test : kCases) {
        CpuRegisters registers = has_everything();
        registers.*test.in &= ~(1U << test.bit);
        const char* const missing = de::platform::first_missing_cpu_feature(registers);
        REQUIRE(missing != nullptr);
        CHECK(std::strcmp(missing, test.name) == 0);
    }
}

TEST_CASE("a CPU with AVX isn't enough if the OS doesn't save its registers") {
    CpuRegisters registers = has_everything();
    registers.xcr0 = 0x3U; // x87 and SSE, no AVX
    CHECK(de::platform::first_missing_cpu_feature(registers) != nullptr);
    registers.xcr0 = 0x5U; // AVX, but not the SSE it builds on
    CHECK(de::platform::first_missing_cpu_feature(registers) != nullptr);
}

TEST_CASE("the message says what's missing, and fits what it's given") {
    char message[256];
    de::platform::cpu_requirement_message("BMI2", message, sizeof(message));
    CHECK(std::strstr(message, "AVX2") != nullptr);
    CHECK(std::strstr(message, "Missing: BMI2") != nullptr);
    char small[8];
    de::platform::cpu_requirement_message("BMI2", small, sizeof(small));
    CHECK(std::strlen(small) == 7);
}

TEST_CASE("this machine has it all, and the check ran before the tests did") {
    CHECK(de::platform::first_missing_cpu_feature(de::platform::read_cpu_registers()) == nullptr);
    CHECK(de::platform::cpu_checked_at_startup());
}
