#include "platform/thread.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace de::platform {

namespace {

// Windows keeps the first 63 bytes, so the UTF-16 conversion always fits its buffer.
constexpr size_t kWindowsNameCapacity = 64;

} // namespace

void set_current_thread_name(const char* name) {
    char truncated[kWindowsNameCapacity] = {};
    for (size_t i = 0; i + 1 < kWindowsNameCapacity && name[i] != '\0'; ++i) {
        truncated[i] = name[i];
    }
    wchar_t wide[kWindowsNameCapacity];
    if (MultiByteToWideChar(CP_UTF8, 0, truncated, -1, wide, static_cast<int>(kWindowsNameCapacity)) > 0) {
        SetThreadDescription(GetCurrentThread(), wide);
    }
}

bool current_thread_name(char* buffer, size_t size) {
    if (size == 0) {
        return false;
    }
    buffer[0] = '\0';
    PWSTR description = nullptr;
    if (FAILED(GetThreadDescription(GetCurrentThread(), &description))) {
        return false;
    }
    char utf8[256];
    const int written = WideCharToMultiByte(CP_UTF8, 0, description, -1, utf8, sizeof(utf8), nullptr, nullptr);
    LocalFree(description);
    if (written == 0) {
        return false;
    }
    // Truncate to the caller's buffer, like the Linux version.
    const size_t length = std::min(std::strlen(utf8), size - 1);
    std::memcpy(buffer, utf8, length);
    buffer[length] = '\0';
    return true;
}

uint64_t current_thread_id() {
    return GetCurrentThreadId();
}

} // namespace de::platform
