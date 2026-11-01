#include "platform/thread.h"

#include <pthread.h>
#include <unistd.h>

namespace de::platform {

namespace {

// Linux thread names hold 15 characters plus the terminator.
constexpr size_t kLinuxNameCapacity = 16;

} // namespace

void set_current_thread_name(const char* name) {
    char truncated[kLinuxNameCapacity] = {};
    for (size_t i = 0; i + 1 < kLinuxNameCapacity && name[i] != '\0'; ++i) {
        truncated[i] = name[i];
    }
    pthread_setname_np(pthread_self(), truncated);
}

bool current_thread_name(char* buffer, size_t size) {
    if (size == 0) {
        return false;
    }
    char name[kLinuxNameCapacity] = {};
    if (pthread_getname_np(pthread_self(), name, sizeof(name)) != 0) {
        buffer[0] = '\0';
        return false;
    }
    size_t i = 0;
    for (; i + 1 < size && name[i] != '\0'; ++i) {
        buffer[i] = name[i];
    }
    buffer[i] = '\0';
    return true;
}

uint64_t current_thread_id() {
    return static_cast<uint64_t>(gettid());
}

} // namespace de::platform
