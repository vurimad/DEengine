#include "core/job_system.h"
#include "core/log.h"
#include "core/memory_stats.h"
#include "platform/file.h"
#include "platform/time.h"
#include "platform/vfs.h"
#include "support/sandbox.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
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
using de::test::bytes_of;
using de::test::Sandbox;

namespace {

std::string_view text_of(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

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

TEST_CASE("test sandboxes each get a folder of their own") {
    const Sandbox first;
    const Sandbox second;
    CHECK(first.game().view() != second.game().view());
}

TEST_CASE("paths follow the rules: mount prefix, lowercase, forward slashes, no dot segments") {
    const QuietLog quiet;
    const Sandbox sandbox;
    const Vfs vfs = sandbox.make_vfs();
    CHECK(vfs.resolve("game:/characters/knight/knight.demesh").ok());
    CHECK(vfs.resolve("user:/saves/slot_0.sav").ok());
    CHECK(vfs.resolve("temp:/x").ok());

    for (const char* bad :
         {"characters/knight.demesh", "game:/", "game://a", "game:/a//b", "game:/a/", "game:/../x", "game:/a/./b",
          "game:/Knight.demesh", "game:/a\\b", "source:/a", "game:/a b", "user:/logs/con.txt", "game:/nul",
          "game:/a/aux.bin", "game:/com1.bin", "game:/lpt9", "game:/mesh.", "game:/dir./x"}) {
        CAPTURE(bad);
        CHECK_FALSE(vfs.resolve(bad).ok());
    }
    // Close to the reserved names, but fine everywhere.
    for (const char* good : {"game:/console.txt", "game:/com10.bin", "game:/nullable", "game:/a.b.c"}) {
        CAPTURE(good);
        CHECK(vfs.resolve(good).ok());
    }
    CHECK_FALSE(vfs.resolve(nullptr).ok());
}

TEST_CASE("src:/ is the source tree: mounted on request, readable and writable") {
    const QuietLog quiet;
    const Sandbox sandbox;
    const Vfs vfs = sandbox.make_vfs();
    REQUIRE(vfs.write_file("src:/shaders/test.slang", bytes_of("float4 x;")).ok());
    std::byte buffer[16];
    const auto read = vfs.read_file("src:/shaders/test.slang", buffer);
    REQUIRE(read.ok());
    CHECK(text_of(std::span(buffer, *read)) == "float4 x;");

    const auto file = vfs.file_info("src:/shaders/test.slang");
    REQUIRE(file.ok());
    CHECK(file->size == 9);
    CHECK(file->modified_time != 0);
    CHECK_FALSE(file->is_directory);
    const auto folder = vfs.file_info("src:/shaders");
    REQUIRE(folder.ok());
    CHECK(folder->is_directory);
    CHECK(vfs.file_info("src:/shaders/missing.slang").error().code == de::core::ErrorCode::NotFound);

    // A rewrite shows in the info (what hot reload watches).
    REQUIRE(vfs.write_file("src:/shaders/test.slang", bytes_of("float4 longer;")).ok());
    CHECK(vfs.file_info("src:/shaders/test.slang")->size == 14);

    // Without a src_root there is no src:/, and a src_root that doesn't exist is an error.
    const PathString game_root = sandbox.game();
    const PathString user_root = sandbox.folder("user");
    const PathString temp_root = sandbox.folder("temp");
    const PathString missing_root = sandbox.folder("not_there");
    auto no_src =
        Vfs::create({.game_root = game_root.c_str(), .user_root = user_root.c_str(), .temp_root = temp_root.c_str()});
    REQUIRE(no_src.ok());
    CHECK(no_src->resolve("src:/shaders/test.slang").error().code == de::core::ErrorCode::NotFound);
    CHECK(no_src->mount_root(de::platform::Mount::Src)[0] == '\0');
    CHECK_FALSE(no_src->write_file("src:/x.txt", bytes_of("x")).ok());
    CHECK_FALSE(Vfs::create({.game_root = game_root.c_str(),
                             .user_root = user_root.c_str(),
                             .temp_root = temp_root.c_str(),
                             .src_root = missing_root.c_str()})
                    .ok());
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

namespace {

struct OrderProbe {
    std::vector<int>* order = nullptr;
    int id = 0;
};

void record_order(void* context, const de::core::Result<size_t>& /*result*/) {
    const auto* probe = static_cast<OrderProbe*>(context);
    probe->order->push_back(probe->id);
}

} // namespace

TEST_CASE("queued reads are served highest priority first, in order within a priority") {
    const QuietLog quiet;
    const Sandbox sandbox;
    sandbox.add_game_file("a.bin", "a");

    // With no workers, completion callbacks wait in the job system's shared queue in the order
    // the reads finished, and run on this thread, in that order, inside wait().
    de::core::JobSystem jobs(de::core::JobSystemDesc{.worker_count = 0});
    Vfs vfs = sandbox.make_vfs(VfsDesc{.reader_threads = 1, .jobs = &jobs});

    constexpr int kPrefetches = 300;
    std::vector<int> order;
    order.reserve(kPrefetches + 2);
    std::vector<OrderProbe> probes(kPrefetches + 2);
    for (int i = 0; i < kPrefetches + 2; ++i) {
        probes[i] = OrderProbe{.order = &order, .id = i};
    }
    std::vector<std::byte> buffers(kPrefetches + 2);
    de::core::JobCounter done;
    const auto queue = [&](int id, IoPriority priority) {
        REQUIRE(vfs.read(ReadRequest{.path = "game:/a.bin",
                                     .dest = std::span(buffers).subspan(static_cast<size_t>(id), 1),
                                     .priority = priority,
                                     .on_complete = &record_order,
                                     .context = &probes[id],
                                     .counter = &done})
                    .ok());
    };

    // Paused, nothing starts: the whole queue is in place before the reader picks anything.
    vfs.pause();
    for (int i = 0; i < kPrefetches; ++i) {
        queue(i, IoPriority::Prefetch);
    }
    queue(kPrefetches, IoPriority::Near);
    queue(kPrefetches + 1, IoPriority::Critical);
    CHECK(vfs.stats().pending[static_cast<size_t>(IoPriority::Prefetch)] == kPrefetches);
    vfs.resume();
    jobs.wait(done);

    REQUIRE(order.size() == kPrefetches + 2);
    CHECK(order[0] == kPrefetches + 1); // critical first
    CHECK(order[1] == kPrefetches);     // then near
    for (int i = 0; i < kPrefetches; ++i) {
        CHECK(order[i + 2] == i); // then the prefetches, first in first out
    }
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
    vfs.pause(); // nothing starts until resume(): every cancel below hits a queued read
    for (int i = 0; i < kReads; ++i) {
        handles.push_back(*vfs.read(ReadRequest{.path = "game:/a.bin",
                                                .dest = std::span(buffers).subspan(static_cast<size_t>(i) * 8, 8),
                                                .priority = IoPriority::Far}));
    }
    // Cancel every other read; the rest still run.
    std::vector<CancelResult> results(kReads, CancelResult::UnknownHandle);
    for (int i = 0; i < kReads; i += 2) {
        results[i] = vfs.cancel(handles[i]);
        CHECK(results[i] == CancelResult::NotStarted);
        CHECK(vfs.status(handles[i]) == ReadStatus::Invalid); // the handle is freed
    }
    vfs.resume();
    for (int i = 1; i < kReads; i += 2) {
        REQUIRE(wait_for(vfs, handles[i]));
        CHECK(vfs.finish(handles[i]).ok());
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

namespace {

struct Delivery {
    std::atomic<int> calls{0};
    std::atomic<size_t> bytes{0};
    std::atomic<bool> failed{false};
};

void on_read(void* context, const de::core::Result<size_t>& result) {
    auto* delivery = static_cast<Delivery*>(context);
    if (result.ok()) {
        delivery->bytes = *result;
    } else {
        delivery->failed = true;
    }
    delivery->calls.fetch_add(1);
}

} // namespace

TEST_CASE("a completion callback gets the result, and waiting on its counter works right away") {
    const QuietLog quiet;
    const Sandbox sandbox;
    sandbox.add_game_file("a.bin", "abc");
    de::core::JobSystem jobs;
    Vfs vfs = sandbox.make_vfs(VfsDesc{.jobs = &jobs});

    Delivery found;
    Delivery missing;
    de::core::JobCounter done;
    std::byte buffer[8];
    std::byte other[8];
    const auto handle = vfs.read(ReadRequest{
        .path = "game:/a.bin", .dest = buffer, .on_complete = &on_read, .context = &found, .counter = &done});
    REQUIRE(handle.ok());
    REQUIRE(vfs.read(ReadRequest{.path = "game:/missing.bin",
                                 .dest = other,
                                 .on_complete = &on_read,
                                 .context = &missing,
                                 .counter = &done})
                .ok());
    jobs.wait(done); // counted from read(): can't return before both callbacks have run
    CHECK(found.calls.load() == 1);
    CHECK(found.bytes.load() == 3);
    CHECK(text_of(std::span(buffer, 3)) == "abc");
    CHECK(missing.calls.load() == 1);
    CHECK(missing.failed.load());
    // The callback finished the read: its handle is dead, and finish() isn't for it.
    CHECK(vfs.status(*handle) == ReadStatus::Invalid);
    CHECK_FALSE(vfs.finish(*handle).ok());
}

TEST_CASE("a cancelled read with a callback never calls it, and gives its count back") {
    const QuietLog quiet;
    const Sandbox sandbox;
    sandbox.add_game_file("a.bin", "abc");
    de::core::JobSystem jobs;
    Vfs vfs = sandbox.make_vfs(VfsDesc{.reader_threads = 1, .jobs = &jobs});

    constexpr int kReads = 100;
    Delivery delivery;
    de::core::JobCounter done;
    std::vector<std::byte> buffers(static_cast<size_t>(kReads) * 8);
    std::vector<ReadHandle> handles;
    handles.reserve(kReads);
    vfs.pause();
    for (int i = 0; i < kReads; ++i) {
        handles.push_back(*vfs.read(ReadRequest{.path = "game:/a.bin",
                                                .dest = std::span(buffers).subspan(static_cast<size_t>(i) * 8, 8),
                                                .priority = IoPriority::Prefetch,
                                                .on_complete = &on_read,
                                                .context = &delivery,
                                                .counter = &done}));
    }
    for (int i = 0; i < kReads; i += 2) {
        CHECK(vfs.cancel(handles[i]) == CancelResult::NotStarted);
    }
    vfs.resume();
    jobs.wait(done); // returns: cancelled reads gave their counts back
    CHECK(delivery.calls.load() == kReads / 2);
}

TEST_CASE("callbacks need a job system, and counters need callbacks") {
    const QuietLog quiet;
    const Sandbox sandbox;
    Vfs vfs = sandbox.make_vfs();
    Delivery delivery;
    de::core::JobCounter done;
    std::byte buffer[4];
    CHECK_FALSE(
        vfs.read(ReadRequest{.path = "game:/a.bin", .dest = buffer, .on_complete = &on_read, .context = &delivery})
            .ok());
    CHECK_FALSE(vfs.read(ReadRequest{.path = "game:/a.bin", .dest = buffer, .counter = &done}).ok());
}

TEST_CASE("cancelling queued reads frees their slots immediately") {
    const QuietLog quiet;
    const Sandbox sandbox;
    sandbox.add_game_file("a.bin", "a");
    Vfs vfs = sandbox.make_vfs(VfsDesc{.reader_threads = 1, .max_reads = 8});
    std::byte buffers[16];
    std::vector<ReadHandle> handles;
    handles.reserve(8);
    vfs.pause(); // all 8 slots stay queued: nothing is ever dequeued before the cancels
    for (int i = 0; i < 8; ++i) {
        handles.push_back(*vfs.read(
            ReadRequest{.path = "game:/a.bin", .dest = std::span(&buffers[i], 1), .priority = IoPriority::Prefetch}));
    }
    CHECK_FALSE(vfs.read(ReadRequest{.path = "game:/a.bin", .dest = std::span(&buffers[8], 1)}).ok()); // full
    for (const ReadHandle handle : handles) {
        CHECK(vfs.cancel(handle) == CancelResult::NotStarted);
    }
    // Every slot is usable again at once.
    std::vector<ReadHandle> again;
    again.reserve(8);
    for (int i = 0; i < 8; ++i) {
        const auto handle = vfs.read(ReadRequest{.path = "game:/a.bin", .dest = std::span(&buffers[8 + i], 1)});
        REQUIRE(handle.ok());
        again.push_back(*handle);
    }
    vfs.resume();
    for (const ReadHandle handle : again) {
        REQUIRE(wait_for(vfs, handle));
        CHECK(vfs.finish(handle).ok());
    }
}

TEST_CASE("a paused Vfs queues reads without starting them") {
    const QuietLog quiet;
    const Sandbox sandbox;
    sandbox.add_game_file("a.bin", "a");
    Vfs vfs = sandbox.make_vfs();
    std::byte buffer[1];
    vfs.pause();
    const auto handle = vfs.read(ReadRequest{.path = "game:/a.bin", .dest = buffer});
    REQUIRE(handle.ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(vfs.status(*handle) == ReadStatus::Pending);
    vfs.resume();
    REQUIRE(wait_for(vfs, *handle));
    CHECK(vfs.finish(*handle).ok());
}

TEST_CASE("concurrent writes to one path each land whole, and leave no temporary files") {
    const QuietLog quiet;
    const Sandbox sandbox;
    const Vfs vfs = sandbox.make_vfs();
    const std::string_view contents[4] = {"aaaaaaaaaaaaaaaa", "bbbbbbbbbbbbbbbb", "cccccccccccccccc",
                                          "dddddddddddddddd"};
    std::vector<std::thread> writers;
    std::atomic<int> failures{0};
    writers.reserve(4);
    for (const std::string_view text : contents) {
        writers.emplace_back([&vfs, &failures, text] {
            for (int i = 0; i < 20; ++i) {
                failures += vfs.write_file("user:/settings.cfg", bytes_of(text)).ok() ? 0 : 1;
            }
        });
    }
    for (std::thread& writer : writers) {
        writer.join();
    }
    CHECK(failures.load() == 0);
    std::byte buffer[32];
    const auto read = vfs.read_file("user:/settings.cfg", buffer);
    REQUIRE(read.ok());
    const std::string_view result = text_of(std::span(buffer, *read));
    CHECK((result == contents[0] || result == contents[1] || result == contents[2] || result == contents[3]));

    int entries = 0;
    REQUIRE(de::platform::list_directory(vfs.mount_root(de::platform::Mount::User), [&](const char*, bool) {
                ++entries;
            }).ok());
    CHECK(entries == 1); // just settings.cfg
}

TEST_CASE("temp:/ is per process, and folders of dead processes are cleaned up") {
    const QuietLog quiet;
    const Sandbox sandbox;
    // A leftover from a process that no longer exists (Windows ids are multiples of 4).
    PathString stale = sandbox.folder("temp");
    REQUIRE(stale.append("/deengine_temp/999999"));
    REQUIRE(de::platform::create_directories(stale.c_str()).ok());

    const Vfs vfs = sandbox.make_vfs();
    const std::string_view root = vfs.mount_root(de::platform::Mount::Temp);
    CHECK(root.ends_with("/deengine_temp/" + std::to_string(de::platform::current_process_id())));
    CHECK_FALSE(de::platform::path_exists(stale.c_str()));
}

TEST_CASE("paths longer than Windows' 260 characters work") {
    const QuietLog quiet;
    const Sandbox sandbox;
    const Vfs vfs = sandbox.make_vfs();
    PathString path("user:/");
    for (int i = 0; i < 12; ++i) {
        REQUIRE(path.append("a_long_folder_name_for_testing/"));
    }
    REQUIRE(path.append("file.bin"));
    REQUIRE(vfs.resolve(path.c_str())->size() > 300);
    REQUIRE(vfs.write_file(path.c_str(), bytes_of("deep")).ok());
    std::byte buffer[8];
    const auto read = vfs.read_file(path.c_str(), buffer);
    REQUIRE(read.ok());
    CHECK(text_of(std::span(buffer, *read)) == "deep");
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
