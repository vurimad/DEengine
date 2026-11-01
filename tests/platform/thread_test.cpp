#include "platform/thread.h"
#include "platform/time.h"

#include <doctest/doctest.h>

#include <atomic>
#include <cstring>

using de::platform::Thread;

namespace {

struct WorkerState {
    std::atomic<int> runs{0};
    char name[32] = {};
    uint64_t thread_id = 0;
};

void run_worker(void* context) {
    auto* state = static_cast<WorkerState*>(context);
    de::platform::current_thread_name(state->name, sizeof(state->name));
    state->thread_id = de::platform::current_thread_id();
    state->runs.fetch_add(1);
}

} // namespace

TEST_CASE("a thread runs its entry with its context and is joined") {
    WorkerState state;
    {
        const Thread worker("de-test-worker", &run_worker, &state);
    } // joined here
    CHECK(state.runs.load() == 1);
    CHECK(std::strcmp(state.name, "de-test-worker") == 0);
    CHECK(state.thread_id != de::platform::current_thread_id());
}

TEST_CASE("a moved thread is still joined exactly once") {
    WorkerState state;
    Thread first("de-test-move", &run_worker, &state);
    Thread second(std::move(first));
    CHECK_FALSE(first.joinable()); // NOLINT(bugprone-use-after-move): moved-from state is specified
    second.join();
    CHECK_FALSE(second.joinable());
    CHECK(state.runs.load() == 1);
}

namespace {

constexpr const char* kLongName =
    "a very long thread name that no operating system keeps whole, not even windows, far past 63 bytes";

struct NameProbe {
    char full[128] = {};
    char small[6] = {};
    bool small_ok = false;
};

void read_names(void* context) {
    auto* probe = static_cast<NameProbe*>(context);
    de::platform::set_current_thread_name(kLongName);
    de::platform::current_thread_name(probe->full, sizeof(probe->full));
    probe->small_ok = de::platform::current_thread_name(probe->small, sizeof(probe->small));
}

} // namespace

TEST_CASE("long thread names are cut, not dropped, and reads truncate to the buffer") {
    NameProbe probe;
    {
        const Thread thread("prober", &read_names, &probe);
    }
    // Windows keeps 63 bytes and Linux 15: either way the name is a prefix of the long name.
    const size_t kept = std::strlen(probe.full);
    CHECK(kept >= 15);
    CHECK(kept <= 63);
    CHECK(std::strncmp(probe.full, kLongName, kept) == 0);
    CHECK(probe.small_ok);
    CHECK(std::strcmp(probe.small, "a ver") == 0);
}

TEST_CASE("the machine reports at least one hardware thread") {
    CHECK(de::platform::hardware_thread_count() >= 1);
}

TEST_CASE("time is monotonic and sleep waits at least as long as asked") {
    const uint64_t start = de::platform::time_ns();
    de::platform::sleep_ns(2 * de::platform::kNsPerMs);
    const uint64_t end = de::platform::time_ns();
    CHECK(end >= start + 2 * de::platform::kNsPerMs);
}
