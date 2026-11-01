#include "platform/thread.h"

#include <windows.h>

namespace de::platform {

void set_current_thread_name(const char* name) {
    wchar_t wide[64];
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 64) > 0) {
        SetThreadDescription(GetCurrentThread(), wide);
    }
}

bool current_thread_name(char* buffer, size_t size) {
    if (size == 0) {
        return false;
    }
    PWSTR description = nullptr;
    if (FAILED(GetThreadDescription(GetCurrentThread(), &description))) {
        return false;
    }
    const int written =
        WideCharToMultiByte(CP_UTF8, 0, description, -1, buffer, static_cast<int>(size), nullptr, nullptr);
    LocalFree(description);
    if (written == 0) {
        buffer[0] = '\0';
        return false;
    }
    return true;
}

uint64_t current_thread_id() {
    return GetCurrentThreadId();
}

} // namespace de::platform
