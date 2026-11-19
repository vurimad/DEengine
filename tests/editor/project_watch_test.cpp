#include "core/job_system.h"
#include "editor/project_watch.h"
#include "platform/file.h"
#include "support/sandbox.h"

#include <doctest/doctest.h>

#include <string_view>

using de::editor::ProjectWatch;

namespace {

// A file among a sandbox project's sources ("content/a.cell").
void write_source(const de::test::Sandbox& sandbox, std::string_view relative, std::string_view text) {
    de::platform::PathString path = sandbox.folder("src");
    REQUIRE(path.append('/'));
    REQUIRE(path.append(relative));
    const de::platform::PathString parent(path.view().substr(0, path.view().find_last_of('/')));
    REQUIRE(de::platform::create_directories(parent.c_str()).ok());
    REQUIRE(de::platform::write_file_atomic(path.c_str(), de::test::bytes_of(text)).ok());
}

} // namespace

TEST_CASE("the project watch's stamp follows the sources: a file added, changed in size or gone") {
    const de::test::Sandbox sandbox;
    de::core::JobSystem jobs({.worker_count = 0});
    write_source(sandbox, "content/world/a.cell", "{}");
    write_source(sandbox, "meta/world/a.cell.json", R"({"guid": "1"})");
    write_source(sandbox, "config/engine.settings", "taa on\n");
    const de::platform::Vfs vfs = sandbox.make_vfs({.jobs = &jobs});

    uint32_t files = 0;
    const uint64_t first = ProjectWatch::look(vfs, &files);
    CHECK(files == 3);
    CHECK(first != 0);
    // Nothing changed: the same number, however often it's asked.
    CHECK(ProjectWatch::look(vfs) == first);

    // A file added, in a folder of its own.
    write_source(sandbox, "content/props/rock/rock.glb", "glTF");
    const uint64_t added = ProjectWatch::look(vfs, &files);
    CHECK(files == 4);
    CHECK(added != first);

    // One changed in size.
    write_source(sandbox, "content/world/a.cell", R"({"entities": []})");
    const uint64_t changed = ProjectWatch::look(vfs, &files);
    CHECK(files == 4);
    CHECK(changed != added);

    // One gone.
    de::platform::PathString gone = sandbox.folder("src");
    REQUIRE(gone.append("/content/props/rock/rock.glb"));
    REQUIRE(de::platform::delete_tree(gone.c_str()).ok());
    const uint64_t removed = ProjectWatch::look(vfs, &files);
    CHECK(files == 3);
    CHECK(removed != changed);

    // What's cooked isn't a source: writing there changes nothing.
    sandbox.add_game_file("3f/3f8a0c1e5b2d4e61.demesh", "cooked");
    CHECK(ProjectWatch::look(vfs) == removed);
}

TEST_CASE("the project watch looks on a job, and is settled once two looks agree") {
    const de::test::Sandbox sandbox;
    de::core::JobSystem jobs({.worker_count = 0}); // no workers: a background job runs where it's submitted
    write_source(sandbox, "content/a.cell", "{}");
    const de::platform::Vfs vfs = sandbox.make_vfs({.jobs = &jobs});

    ProjectWatch watch;
    CHECK(watch.stamp() == 0);
    CHECK_FALSE(watch.settled());
    // The first update starts a look; the next takes it in. One look says nothing of what's still being written.
    watch.update(vfs, jobs, 0.0f);
    watch.update(vfs, jobs, 0.0f);
    CHECK(watch.ended() == 1);
    CHECK(watch.stamp() == ProjectWatch::look(vfs));
    CHECK_FALSE(watch.settled());
    // Not before the interval has passed.
    watch.update(vfs, jobs, ProjectWatch::kIntervalS * 0.5f);
    CHECK(watch.started() == 1);
    watch.update(vfs, jobs, ProjectWatch::kIntervalS);
    watch.update(vfs, jobs, 0.0f);
    CHECK(watch.ended() == 2);
    CHECK(watch.settled());

    // A change unsettles it until two looks have seen the same again; look_now() doesn't wait for the interval.
    const uint64_t before = watch.stamp();
    write_source(sandbox, "content/b.cell", "{}");
    watch.look_now();
    watch.update(vfs, jobs, 0.0f);
    watch.update(vfs, jobs, 0.0f);
    CHECK(watch.stamp() != before);
    CHECK_FALSE(watch.settled());
    watch.look_now();
    watch.update(vfs, jobs, 0.0f);
    watch.update(vfs, jobs, 0.0f);
    CHECK(watch.settled());
    CHECK(watch.files() == 2);
    watch.stop(jobs);
}
