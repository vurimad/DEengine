#pragma once

#include "core/job_system.h"
#include "platform/vfs.h"

#include <cstdint>

namespace de::editor {

// The open project's sources as one number: every file under src:/content, src:/meta and src:/config, by its path,
// its size and its time. The editor looks every so often, on a background job, to tell that something changed them
// from outside (a model dropped in, a generator's output, a file edited by hand) and cook.
//
//     watch.update(engine.vfs, engine.jobs, dt_s);       // each frame
//     if (watch.settled() && watch.stamp() != cooked) {  // changed, and no longer changing
//         ...cook...
//     }
//
// One look runs at a time. It reads names and times only, never a file's bytes: a few milliseconds for a few
// thousand files, off the frame's thread.
class ProjectWatch {
public:
    // How long between two looks.
    static constexpr float kIntervalS = 1.5f;

    ProjectWatch() = default;
    ProjectWatch(const ProjectWatch&) = delete;
    ProjectWatch& operator=(const ProjectWatch&) = delete;

    // Each frame: takes in a look that has ended, and starts the next when it's due.
    void update(const platform::Vfs& vfs, core::JobSystem& jobs, float dt_s);
    // The next look starts at the next update(), not after the interval.
    void look_now() { due_s_ = 0.0f; }
    // Waits for a look in flight: before the Vfs or this goes.
    void stop(core::JobSystem& jobs);

    // Looks started and ended so far. A look that ends with `ended() >= n` started after started() read n - 1.
    uint32_t started() const { return started_; }
    uint32_t ended() const { return ended_; }
    // What the last look that ended saw (0 before any has), and how many files.
    uint64_t stamp() const { return stamp_; }
    uint32_t files() const { return files_; }
    // The last two looks saw the same: nothing is still being written.
    bool settled() const { return ended_ >= 2 && stamp_ == previous_; }

    // One look, here and now: what update() runs on a job. For tests and tools.
    static uint64_t look(const platform::Vfs& vfs, uint32_t* files = nullptr);

private:
    static void run(void* context);

    const platform::Vfs* vfs_ = nullptr;
    core::JobCounter counter_;
    bool running_ = false;
    float due_s_ = 0.0f;
    uint32_t started_ = 0;
    uint32_t ended_ = 0;
    uint64_t stamp_ = 0;
    uint64_t previous_ = 0;
    uint32_t files_ = 0;
    // The look in flight's: read once its counter says it's done.
    uint64_t looked_stamp_ = 0;
    uint32_t looked_files_ = 0;
};

} // namespace de::editor
