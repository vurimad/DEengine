#pragma once

#include "core/fixed_string.h"
#include "core/result.h"
#include "platform/file.h"

namespace de::platform {

// A project (docs/design.md, Projects): a game's own folder, apart from the engine, holding content/, meta/ and
// config/ (mounted as src:/ in dev builds), and cooked/ (game:/ unless told otherwise). It's named by the
// <name>.deproject file in it, whose folder has the same name: projects/dreadworld/dreadworld.deproject.
struct Project {
    PathString root;              // its folder, forward slashes: "D:/DE/projects/dreadworld"
    core::FixedString<63> name{}; // "dreadworld"
};

// The project given on a command line (--project): its folder or its .deproject file, relative to the working
// directory, or else to the engine's repository (engine_root), so "projects/sandbox" works from anywhere. Fails if
// there's no such folder, or no <name>.deproject in it.
core::Result<Project> find_project(const char* given, const char* engine_root);

} // namespace de::platform
