#include "platform/thread.h"

#include "core/assert.h"

#include <algorithm>
#include <utility>

namespace de::platform {

namespace {

struct ThreadName {
    char text[Thread::kMaxNameLength + 1] = {};
};

ThreadName copy_name(const char* name) {
    ThreadName copy;
    if (name != nullptr) {
        for (size_t i = 0; i < Thread::kMaxNameLength && name[i] != '\0'; ++i) {
            copy.text[i] = name[i];
        }
    }
    return copy;
}

} // namespace

Thread::Thread(const char* name, Entry entry, void* context) {
    DE_ASSERT(entry != nullptr);
    const ThreadName thread_name = copy_name(name);
    thread_ = std::thread([thread_name, entry, context] {
        set_current_thread_name(thread_name.text);
        entry(context);
    });
}

Thread::~Thread() {
    join();
}

Thread& Thread::operator=(Thread&& other) noexcept {
    if (this != &other) {
        join();
        thread_ = std::move(other.thread_);
    }
    return *this;
}

void Thread::join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

uint32_t hardware_thread_count() {
    return std::max(1U, std::thread::hardware_concurrency());
}

} // namespace de::platform
