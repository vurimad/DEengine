#include "core/job_system.h"
#include "core/log.h"
#include "core/memory_stats.h"
#include "platform/file.h"
#include "platform/time.h"
#include "platform/vfs.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using de::platform::CancelResult;
using de::platform::IoPriority;
using de::platform::PathString;
using de::platform::ReadHandle;
using de::platform::ReadRequest;
using de::platform::ReadStatus;
using de::platform::Vfs;
using de::platform::VfsDesc;

namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

std::string_view text_of(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// A scratch folder per test: game/, user/ and temp/ roots under the OS temp folder, deleted at the end.
class Sandbox {
public:
    Sandbox() {
        static std::atomic<int> counter{0};
        root_ = *de::platform::system_temp_folder();
        REQUIRE(root_.append("/deengine_tests/"));
        char id[32];
        const unsigned long long stamp =
            de::platform::time_ns() + static_cast<unsigned long long>(counter.fetch_add(1));
        size_t length = 0;
        for (unsigned long long value = stamp; value != 0 || length == 0; value /= 10) {
            id[length++] = static_cast<char>('0' + value % 10);
        }
        while (length > 0) {
            REQUIRE(root_.append(id[--length]));
        }
        REQUIRE(de::platform::create_directories(game().c_str()).ok());
    }
    ~Sandbox() { static_cast<void>(de::platform::delete_tree(root_.c_str())); }
    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;
    Sandbox(Sandbox&&) = delete;
    Sandbox& operator=(Sandbox&&) = delete;

    PathString folder(std::string_view name) const {
        PathString path = root_;
        REQUIRE(path.append('/'));
        REQUIRE(path.append(name));
        return path;
    }
    PathString game() const { return folder("game"); }

    // Puts a file into the game data folder, as the cooker would.
    void add_game_file(std::string_view relative, std::string_view contents) const {
        PathString path = game();
        REQUIRE(path.append('/'));
        REQUIRE(path.append(relative));
        const PathString parent(path.view().substr(0, path.view().find_last_of('/')));
        REQUIRE(de::platform::create_directories(parent.c_str()).ok());
        REQUIRE(de::platform::write_file_atomic(path.c_str(), bytes_of(contents)).ok());
    }

    Vfs make_vfs(VfsDesc desc = {}) const {
        const PathString game_root = game();
        const PathString user_root = folder("user");
        const PathString temp_root = folder("temp");
        desc.game_root = game_root.c_str();
        desc.user_root = user_root.c_str();
        desc.temp_root = temp_root.c_str();
        auto vfs = Vfs::create(desc);
        REQUIRE(vfs.ok());
        return std::move(vfs).value();
    }

private:
    PathString root_;
};

// Polls until the read leaves Pending / InFlight. False on timeout.
bool wait_for(const Vfs& vfs, ReadHandle handle) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        const ReadStatus status = vfs.status(handle);
        if (status != ReadStatus::Pending && status != ReadStatus::InFlight) {
            return true;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

class QuietLog {
public:
    QuietLog() : saved_(de::core::log_level()) { de::core::set_log_level(de::core::LogLevel::Error); }
    ~QuietLog() { de::core::set_log_level(saved_); }
    QuietLog(const QuietLog&) = delete;
    QuietLog& operator=(const QuietLog&) = delete;
    QuietLog(QuietLog&&) = delete;
    QuietLog& operator=(QuietLog&&) = delete;

private:
    de::core::LogLevel saved_;
};

} // namespace

TEST_CASE("paths follow the rules: mount prefix, lowercase, forward slashes, no dot segments") {
    const QuietLog quiet;
    const Sandbox sandbox;
    const Vfs vfs = sandbox.make_vfs();
    CHECK(vfs.resolve("game:/characters/knight/knight.demesh").ok());
    CHECK(vfs.resolve("user:/saves/slot_0.sav").ok());
    CHECK(vfs.resolve("temp:/x").ok());

    for (const char* bad : {"characters/knight.demesh", "game:/", "game://a", "game:/a//b", "game:/a/", "game:/../x",
                            "game:/a/./b", "game:/Knight.demesh", "game:/a\\b", "src:/a", "game:/a b"}) {
        CAPTURE(bad);
        CHECK_FALSE(vfs.resolve(bad).ok());
    }
    CHECK_FALSE(vfs.resolve(nullptr).ok());
}

TEST_CASE("write then read back on user:/ and temp:/, creating folders") {
    const QuietLog quiet;
    const Sandbox sandbox;
    const Vfs vfs = sandbox.make_vfs();
    REQUIRE(vfs.write_file("user:/saves/slot_0.sav", bytes_of("level 12, 3 flasks")).ok());
    REQUIRE(vfs.write_file("temp:/scratch/a.bin", bytes_of("scratch")).ok());

    CHECK(vfs.exists("user:/saves/slot_0.sav"));
    CHECK(*vfs.file_size("user:/saves/slot_0.sav") == 18);
    std::byte buffer[64];
    const auto read = vfs.read_file("user:/saves/slot_0.sav", buffer);
    REQUIRE(read.ok());
    CHECK(text_of(std::span(buffer, *read)) == "level 12, 3 flasks");

    // Overwriting replaces the whole file.
    REQUIRE(vfs.write_file("user:/saves/slot_0.sav", bytes_of("new")).ok());
    CHECK(*vfs.file_size("user:/saves/slot_0.sav") == 3);
    CHECK_FALSE(vfs.exists("user:/saves/slot_0.sav.tmp")); // the temporary file is gone
}

TEST_CASE("game:/ is read only, and missing files fail cleanly") {
    const QuietLog quiet;
    const Sandbox sandbox;
    sandbox.add_game_file("data/hello.txt", "hello");
    const Vfs vfs = sandbox.make_vfs();
    CHECK_FALSE(vfs.write_file("game:/data/hello.txt", bytes_of("nope")).ok());
    std::byte buffer[16];
    const auto missing = vfs.read_file("game:/data/missing.txt", buffer);
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().code == de::core::ErrorCode::NotFound);
    const auto too_small = vfs.read_file("game:/data/hello.txt", std::span(buffer, 2));
    CHECK_FALSE(too_small.ok());
}

TEST_CASE("temp:/ starts empty every time; user:/ keeps its files") {
    const QuietLog quiet;
    const Sandbox sandbox;
    {
        const Vfs vfs = sandbox.make_vfs();
        REQUIRE(vfs.write_file("temp:/leftover.bin", bytes_of("x")).ok());
        REQUIRE(vfs.write_file("user:/settings.json", bytes_of("{}")).ok());
    }
    const Vfs vfs = sandbox.make_vfs();
    CHECK_FALSE(vfs.exists("temp:/leftover.bin"));
    CHECK(vfs.exists("user:/settings.json"));
}

TEST_CASE("async reads land in the caller's memory, whole files and ranges") {
    const QuietLog quiet;
    const Sandbox sandbox;
    sandbox.add_game_file("cells/0_0.decell", "0123456789abcdef");
    Vfs vfs = sandbox.make_vfs();

    std::byte whole[32] = {};
    const auto whole_read = vfs.read(ReadRequest{.path = "game:/cells/0_0.decell", .dest = whole});
    REQUIRE(whole_read.ok());
    std::byte part[4] = {};
    const auto part_read =
        vfs.read(ReadRequest{.path = "game:/cells/0_0.decell", .dest = part, .offset = 10, .size = 4});
    REQUIRE(part_read.ok());

    REQUIRE(wait_for(vfs, *whole_read));
    REQUIRE(wait_for(vfs, *part_read));
    CHECK(vfs.status(*whole_read) == ReadStatus::Done);
    const auto whole_bytes = vfs.finish(*whole_read);
    REQUIRE(whole_bytes.ok());
    CHECK(text_of(std::span(whole, *whole_bytes)) == "0123456789abcdef");
    const auto part_bytes = vfs.finish(*part_read);
    REQUIRE(part_bytes.ok());
    CHECK(text_of(std::span(part, *part_bytes)) == "abcd");

    // A finished handle is dead.
    CHECK(vfs.status(*whole_read) == ReadStatus::Invalid);
    CHECK_FALSE(vfs.finish(*whole_read).ok());
}

TEST_CASE("failed async reads report why") {
    const QuietLog quiet;
    const Sandbox sandbox;
    sandbox.add_game_file("big.bin", "0123456789");
    Vfs vfs = sandbox.make_vfs();
    std::byte small[4];
    const auto too_small = vfs.read(ReadRequest{.path = "game:/big.bin", .dest = small});
    const auto missing = vfs.read(ReadRequest{.path = "game:/nothing.bin", .dest = small});
    REQUIRE(too_small.ok());
    REQUIRE(missing.ok());
    REQUIRE(wait_for(vfs, *too_small));
    REQUIRE(wait_for(vfs, *missing));
    CHECK(vfs.status(*missing) == ReadStatus::Failed);
    CHECK(vfs.finish(*too_small).error().code == de::core::ErrorCode::OutOfRange);
    CHECK(vfs.finish(*missing).error().code == de::core::ErrorCode::NotFound);
    CHECK(vfs.stats().reads_failed == 2);
    CHECK_FALSE(vfs.read(ReadRequest{.path = "game:/Bad", .dest = small}).ok()); // rejected up front
}

TEST_CASE("many concurrent reads all complete with the right contents") {
    const QuietLog quiet;
    const Sandbox sandbox;
    constexpr int kFiles = 64;
    char name[32];
    for (int i = 0; i < kFiles; ++i) {
        std::memcpy(name, "f00.bin", 8);
        name[1] = static_cast<char>('0' + i / 10);
        name[2] = static_cast<char>('0' + i % 10);
        sandbox.add_game_file(name, std::string_view(name, 3));
    }
    Vfs vfs = sandbox.make_vfs(VfsDesc{.reader_threads = 4});

    std::vector<std::byte> buffers(static_cast<size_t>(kFiles) * 8);
    std::vector<ReadHandle> handles;
    handles.reserve(kFiles);
    std::vector<PathString> paths(kFiles);
    for (int i = 0; i < kFiles; ++i) {
        paths[i].assign("game:/f00.bin");
        paths[i][7] = static_cast<char>('0' + i / 10);
        paths[i][8] = static_cast<char>('0' + i % 10);
        const auto priority = static_cast<IoPriority>(i % 4);
        const auto handle = vfs.read(ReadRequest{.path = paths[i].c_str(),
                                                 .dest = std::span(buffers).subspan(static_cast<size_t>(i) * 8, 8),
                                                 .priority = priority});
        REQUIRE(handle.ok());
        handles.push_back(*handle);
    }
    for (int i = 0; i < kFiles; ++i) {
        REQUIRE(wait_for(vfs, handles[i]));
        const auto bytes = vfs.finish(handles[i]);
        REQUIRE(bytes.ok());
        const std::string_view got = text_of(std::span(buffers).subspan(static_cast<size_t>(i) * 8, *bytes));
        CHECK(got == std::string_view(paths[i].c_str() + 6, 3));
    }
    const auto stats = vfs.stats();
    CHECK(stats.reads_completed == kFiles);
    CHECK(stats.bytes_read == kFiles * 3);
}

TEST_CASE("a critical read overtakes queued prefetches") {
    const QuietLog quiet;
    const Sandbox sandbox;
    sandbox.add_game_file("a.bin", "a");
    Vfs vfs = sandbox.make_vfs(VfsDesc{.reader_threads = 1});

    constexpr int kPrefetches = 300;
    std::vector<std::byte> buffers(kPrefetches);
    std::vector<ReadHandle> prefetches;
    prefetches.reserve(kPrefetches);
    for (int i = 0; i < kPrefetches; ++i) {
        prefetches.push_back(*vfs.read(ReadRequest{.path = "game:/a.bin",
                                                   .dest = std::span(buffers).subspan(static_cast<size_t>(i), 1),
                                                   .priority = IoPriority::Prefetch}));
    }
    std::byte critical_buffer[1];
    const ReadHandle critical =
        *vfs.read(ReadRequest{.path = "game:/a.bin", .dest = critical_buffer, .priority = IoPriority::Critical});
    REQUIRE(wait_for(vfs, critical));

    int prefetches_done = 0;
    for (const ReadHandle handle : prefetches) {
        prefetches_done += vfs.status(handle) == ReadStatus::Done ? 1 : 0;
    }
    CHECK(prefetches_done < kPrefetches / 2); // it didn't wait its turn behind them
    for (const ReadHandle handle : prefetches) {
        REQUIRE(wait_for(vfs, handle));
        CHECK(vfs.finish(handle).ok());
    }
    CHECK(vfs.finish(critical).ok());
}

TEST_CASE("cancelling a queued read means it never touches the destination") {
    const QuietLog quiet;
    const Sandbox sandbox;
    sandbox.add_game_file("a.bin", "abcdefgh");
    Vfs vfs = sandbox.make_vfs(VfsDesc{.reader_threads = 1});

    constexpr int kReads = 200;
    std::vector<std::byte> buffers(static_cast<size_t>(kReads) * 8, std::byte{0x55});
    std::vector<ReadHandle> handles;
    handles.reserve(kReads);
    for (int i = 0; i < kReads; ++i) {
        handles.push_back(*vfs.read(ReadRequest{.path = "game:/a.bin",
                                                .dest = std::span(buffers).subspan(static_cast<size_t>(i) * 8, 8),
                                                .priority = IoPriority::Far}));
    }
    // Cancel from the back: the newest reads are still queued, the oldest may have run already.
    std::vector<CancelResult> results(kReads, CancelResult::UnknownHandle);
    int not_started = 0;
    for (int i = kReads - 1; i >= 0; --i) {
        results[i] = vfs.cancel(handles[i]);
        if (results[i] != CancelResult::InFlight) {
            CHECK(vfs.status(handles[i]) == ReadStatus::Invalid); // the handle is freed
        }
        not_started += results[i] == CancelResult::NotStarted ? 1 : 0;
    }
    CHECK(not_started > 0);
    for (int i = 0; i < kReads; ++i) {
        if (results[i] == CancelResult::InFlight) {
            REQUIRE(wait_for(vfs, handles[i]));
            CHECK(vfs.finish(handles[i]).ok());
        }
    }
    // Give a reader time to (wrongly) process anything cancelled, then check untouched memory.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    for (int i = 0; i < kReads; ++i) {
        if (results[i] == CancelResult::NotStarted) {
            const std::span<std::byte> dest = std::span(buffers).subspan(static_cast<size_t>(i) * 8, 8);
            bool untouched = true;
            for (const std::byte b : dest) {
                untouched = untouched && b == std::byte{0x55};
            }
            CHECK(untouched);
        }
    }
}

TEST_CASE("running out of read slots is an error, not a block") {
    const QuietLog quiet;
    const Sandbox sandbox;
    sandbox.add_game_file("a.bin", "a");
    Vfs vfs = sandbox.make_vfs(VfsDesc{.reader_threads = 1, .max_reads = 2});
    std::byte buffers[3];
    std::vector<ReadHandle> handles;
    int refused = 0;
    for (std::byte& buffer : buffers) {
        const auto handle = vfs.read(ReadRequest{.path = "game:/a.bin", .dest = std::span(&buffer, 1)});
        if (handle.ok()) {
            handles.push_back(*handle);
        } else {
            ++refused;
            CHECK(handle.error().code == de::core::ErrorCode::OutOfRange);
        }
    }
    // Two slots: the third read is refused unless a reader finished one already... and even then
    // nobody has called finish(), so the slot is still taken.
    CHECK(refused == 1);
    for (const ReadHandle handle : handles) {
        REQUIRE(wait_for(vfs, handle));
        CHECK(vfs.finish(handle).ok());
    }
}

TEST_CASE("a completion job runs when the read finishes") {
    const QuietLog quiet;
    const Sandbox sandbox;
    sandbox.add_game_file("a.bin", "abc");
    de::core::JobSystem jobs;
    Vfs vfs = sandbox.make_vfs(VfsDesc{.jobs = &jobs});

    std::atomic<int> completions{0};
    de::core::JobCounter done;
    std::byte buffer[8];
    const auto handle =
        vfs.read(ReadRequest{.path = "game:/a.bin",
                             .dest = buffer,
                             .on_complete = de::core::Job{
                                 .entry = [](void* context) { static_cast<std::atomic<int>*>(context)->fetch_add(1); },
                                 .context = &completions,
                                 .counter = &done,
                                 .name = "read done"}});
    REQUIRE(handle.ok());
    // The job is submitted from a reader thread, so the counter only counts it from then on:
    // wait for the read first, then for the job.
    REQUIRE(wait_for(vfs, *handle));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (completions.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        jobs.wait(done);
    }
    CHECK(completions.load() == 1);
    CHECK(vfs.finish(*handle).ok());
}

TEST_CASE("queueing, polling and finishing reads stay off the heap") {
    const QuietLog quiet;
    const Sandbox sandbox;
    sandbox.add_game_file("a.bin", "abc");
    Vfs vfs = sandbox.make_vfs();
    std::byte buffer[8];

    const de::core::NoHeapAllocationScope no_alloc;
    for (int i = 0; i < 20; ++i) {
        const auto handle = vfs.read(ReadRequest{.path = "game:/a.bin", .dest = buffer});
        REQUIRE(handle.ok());
        REQUIRE(wait_for(vfs, *handle));
        REQUIRE(vfs.finish(*handle).ok());
    }
    CHECK(no_alloc.allocations() == 0);
}
