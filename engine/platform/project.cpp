#include "platform/project.h"

#include <string_view>

namespace de::platform {
namespace {

// Forward slashes, no trailing one, and a .deproject file's folder for the file.
PathString folder_of(std::string_view given) {
    PathString path;
    for (const char c : given) {
        if (!path.append(c == '\\' ? '/' : c)) {
            return {};
        }
    }
    while (path.size() > 1 && path.view().back() == '/') {
        path.truncate(path.size() - 1);
    }
    if (path.view().ends_with(".deproject")) {
        const size_t slash = path.view().rfind('/');
        path.truncate(slash == std::string_view::npos ? 0 : slash);
        if (path.empty()) {
            path.assign(".");
        }
    }
    return path;
}

} // namespace

core::Result<Project> find_project(const char* given, const char* engine_root) {
    using core::ErrorCode;
    if (given == nullptr || given[0] == '\0') {
        return core::fail(ErrorCode::InvalidArgument, "no project given");
    }
    PathString folder = folder_of(given);
    if (folder.empty()) {
        return core::fail(ErrorCode::InvalidArgument, "the project's path is too long");
    }
    if (!path_exists(folder.c_str()) && engine_root != nullptr) {
        PathString beside_engine = folder_of(engine_root);
        if (!beside_engine.append('/') || !beside_engine.append(folder.view())) {
            return core::fail(ErrorCode::InvalidArgument, "the project's path is too long");
        }
        folder = beside_engine;
    }
    if (!path_exists(folder.c_str())) {
        return core::fail(ErrorCode::NotFound, "no such project folder");
    }
    // Absolute, so another process (the cooker the editor starts) finds it from anywhere.
    const std::string_view text = folder.view();
    const bool absolute = text.starts_with('/') || (text.size() > 1 && text[1] == ':');
    if (!absolute) {
        const core::Result<PathString> here = working_directory();
        if (!here) {
            return core::fail(here.error());
        }
        PathString full = folder_of(here->view());
        if (!full.append('/') || !full.append(text.starts_with("./") ? text.substr(2) : text)) {
            return core::fail(ErrorCode::InvalidArgument, "the project's path is too long");
        }
        folder = full;
    }
    // Its name: the folder's, which its .deproject file carries.
    const size_t slash = folder.view().rfind('/');
    const std::string_view leaf = slash == std::string_view::npos ? folder.view() : folder.view().substr(slash + 1);
    Project project;
    project.root = folder;
    if (leaf.empty() || leaf == "." || !project.name.assign(leaf)) {
        return core::fail(ErrorCode::InvalidArgument, "the project's folder needs a name");
    }
    PathString marker = folder;
    if (!marker.append('/') || !marker.append(leaf) || !marker.append(".deproject")) {
        return core::fail(ErrorCode::InvalidArgument, "the project's path is too long");
    }
    if (!path_exists(marker.c_str())) {
        return core::fail(ErrorCode::NotFound, "not a project: its folder has no <name>.deproject");
    }
    return project;
}

} // namespace de::platform
