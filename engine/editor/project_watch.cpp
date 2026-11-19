#include "editor/project_watch.h"

#include "core/allocator.h"
#include "core/array.h"
#include "core/memory_tag.h"
#include "core/profile.h"

#include <string_view>

namespace de::editor {

namespace {

constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

uint64_t mix(uint64_t hash, std::string_view text) {
    for (const char c : text) {
        hash = (hash ^ static_cast<uint8_t>(c)) * kFnvPrime;
    }
    return hash;
}

uint64_t mix(uint64_t hash, uint64_t value) {
    for (int byte = 0; byte < 8; ++byte) {
        hash = (hash ^ ((value >> (byte * 8)) & 0xffU)) * kFnvPrime;
    }
    return hash;
}

} // namespace

uint64_t ProjectWatch::look(const platform::Vfs& vfs, uint32_t* files) {
    DE_PROFILE_ZONE("ProjectWatch::look");
    // Each file adds its own number, so the order the OS lists a folder in doesn't matter.
    uint64_t stamp = 0;
    uint32_t count = 0;
    core::Array<platform::PathString> folders(core::heap_allocator(core::MemoryTag::Editor));
    for (const char* root : {"src:/content", "src:/meta", "src:/config"}) {
        platform::PathString path;
        if (path.assign(root) && vfs.exists(root)) {
            folders.push(path);
        }
    }
    while (!folders.empty()) {
        const platform::PathString folder = folders.back();
        folders.pop();
        static_cast<void>(vfs.list(folder.c_str(), [&](const char* name, bool is_directory) {
            platform::PathString path = folder;
            if (!path.append('/') || !path.append(name)) {
                return; // too long a path to be a source: the cooker says so
            }
            if (is_directory) {
                folders.push(path);
                return;
            }
            uint64_t one = mix(kFnvOffset, path.view());
            if (const core::Result<platform::FileInfo> info = vfs.file_info(path.c_str())) {
                one = mix(mix(one, info->size), info->modified_time);
            }
            stamp += one;
            ++count;
        }));
    }
    if (files != nullptr) {
        *files = count;
    }
    // Never 0, which means no look yet.
    return stamp != 0 ? stamp : 1;
}

void ProjectWatch::run(void* context) {
    auto* watch = static_cast<ProjectWatch*>(context);
    watch->looked_stamp_ = look(*watch->vfs_, &watch->looked_files_);
}

void ProjectWatch::update(const platform::Vfs& vfs, core::JobSystem& jobs, float dt_s) {
    if (running_) {
        if (!counter_.done()) {
            return;
        }
        running_ = false;
        previous_ = stamp_;
        stamp_ = looked_stamp_;
        files_ = looked_files_;
        ++ended_;
    }
    due_s_ -= dt_s;
    if (due_s_ > 0.0f) {
        return;
    }
    due_s_ = kIntervalS;
    vfs_ = &vfs;
    running_ = true;
    ++started_;
    // On an idle worker: it's never in a frame's way.
    jobs.submit_background(
        {.entry = &ProjectWatch::run, .context = this, .counter = &counter_, .name = "project watch"});
}

void ProjectWatch::stop(core::JobSystem& jobs) {
    if (running_) {
        jobs.wait(counter_);
        running_ = false;
    }
}

} // namespace de::editor
