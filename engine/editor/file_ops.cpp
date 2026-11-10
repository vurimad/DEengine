#include "editor/file_ops.h"

#include "core/log.h"
#include "core/profile.h"
#include "editor/cell_source.h"
#include "platform/vfs.h"

#include <algorithm>
#include <cstring>

namespace de::editor {
namespace {

using core::ErrorCode;
using core::fail;

constexpr std::string_view kContent = "src:/content";
constexpr std::string_view kMeta = "src:/meta";

core::Allocator& file_ops_heap() {
    return core::heap_allocator(core::MemoryTag::Editor);
}

std::string_view folder_of(std::string_view path) {
    const size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string_view() : path.substr(0, slash);
}

bool under_content(std::string_view path) {
    return path.size() > kContent.size() + 1 && path.starts_with(kContent) && path[kContent.size()] == '/';
}

// Whether path is target or inside it.
bool within(std::string_view path, std::string_view target) {
    return path == target || (path.size() > target.size() && path.starts_with(target) && path[target.size()] == '/');
}

// What a reference in the text source at path names, as a full path; false if it isn't one, or doesn't fit.
bool resolve_reference(std::string_view path, std::string_view value, SourcePath& out) {
    if (!looks_like_reference(value)) {
        return false;
    }
    char joined[SourcePath::capacity() + 1];
    size_t length = 0;
    if (!joined_path(folder_of(path), value, joined, length)) {
        return false;
    }
    return out.assign(std::string_view(joined, length));
}

bool path_rules_hold(std::string_view path) {
    if (!under_content(path)) {
        return false;
    }
    const std::string_view relative = path.substr(kContent.size() + 1);
    size_t start = 0;
    while (start <= relative.size()) {
        size_t end = relative.find('/', start);
        end = end == std::string_view::npos ? relative.size() : end;
        const std::string_view part = relative.substr(start, end - start);
        if (part.empty() || part == "." || part == ".." || part.back() == '.') {
            return false;
        }
        for (const char c : part) {
            const bool fine = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
            if (!fine) {
                return false;
            }
        }
        start = end + 1;
    }
    return true;
}

} // namespace

bool names_files(std::string_view path) {
    for (const std::string_view extension :
         {".world", ".cell", ".material", ".terrain", ".terrainlayers", ".environment", ".gltf"}) {
        if (path.ends_with(extension)) {
            return true;
        }
    }
    return false;
}

bool meta_path(std::string_view path, bool folder, SourcePath& out) {
    if (!under_content(path)) {
        return false;
    }
    out.assign(kMeta);
    return out.append(path.substr(kContent.size())) && (folder || out.append(".json"));
}

bool moved_path(std::string_view path, std::span<const SourceMove> moves, SourcePath& out) {
    for (const SourceMove& move : moves) {
        if (within(path, move.from.view())) {
            out.assign(move.to.view());
            return out.append(path.substr(move.from.size()));
        }
    }
    out.assign(path);
    return false;
}

void for_each_string(std::string_view text, core::FunctionRef<void(size_t begin, size_t end)> visit) {
    size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            const size_t end = text.find('\n', i);
            i = end == std::string_view::npos ? text.size() : end;
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            const size_t end = text.find("*/", i + 2);
            i = end == std::string_view::npos ? text.size() : end + 2;
        } else if (c == '"') {
            const size_t begin = i + 1;
            size_t at = begin;
            while (at < text.size() && text[at] != '"') {
                at += text[at] == '\\' ? 2 : 1;
            }
            if (at > text.size()) {
                return; // an escape at the very end: no closing quote
            }
            visit(begin, std::min(at, text.size()));
            i = at + 1;
        } else {
            ++i;
        }
    }
}

bool looks_like_reference(std::string_view value) {
    if (value.empty() || value.find_first_of(":\\%?#") != std::string_view::npos || value.front() == '/') {
        return false;
    }
    const size_t slash = value.rfind('/');
    const std::string_view leaf = slash == std::string_view::npos ? value : value.substr(slash + 1);
    const size_t dot = leaf.rfind('.');
    return dot != std::string_view::npos && dot > 0 && dot + 1 < leaf.size() && leaf != "..";
}

bool rewrite_references(std::string_view text, std::string_view old_path, std::string_view new_path,
                        std::span<const SourceMove> moves, core::FunctionRef<bool(std::string_view)> exists,
                        core::Array<char>& out) {
    out.clear();
    size_t copied = 0;
    bool changed = false;
    const auto put = [&out](std::string_view part) {
        const size_t at = out.size();
        out.resize(at + part.size());
        std::memcpy(out.data() + at, part.data(), part.size());
    };
    for_each_string(text, [&](size_t begin, size_t end) {
        const std::string_view value = text.substr(begin, end - begin);
        SourcePath target;
        if (!resolve_reference(old_path, value, target)) {
            return;
        }
        SourcePath now;
        const bool target_moved = moved_path(target.view(), moves, now);
        if (!target_moved && (new_path == old_path || !exists(target.view()))) {
            return; // neither end moved, or it isn't a file of the project's
        }
        char relative[SourcePath::capacity() + 1];
        size_t length = 0;
        if (!relative_path(folder_of(new_path), now.view(), relative, length)) {
            return;
        }
        const std::string_view replacement(relative, length);
        if (replacement == value) {
            return;
        }
        put(text.substr(copied, begin - copied));
        put(replacement);
        copied = end;
        changed = true;
    });
    if (!changed) {
        out.clear();
        return false;
    }
    put(text.substr(copied));
    return true;
}

bool names_target(std::string_view text, std::string_view path, std::string_view target) {
    bool found = false;
    for_each_string(text, [&](size_t begin, size_t end) {
        SourcePath named;
        if (!found && resolve_reference(path, text.substr(begin, end - begin), named)) {
            found = within(named.view(), target);
        }
    });
    return found;
}

void list_files(const platform::Vfs& vfs, std::string_view folder, core::Array<SourcePath>& out) {
    core::Array<SourcePath> folders(file_ops_heap());
    folders.push(SourcePath(folder));
    while (!folders.empty()) {
        const SourcePath at = folders.back();
        folders.pop();
        static_cast<void>(vfs.list(at.c_str(), [&](const char* name, bool is_directory) {
            if (name[0] == '.') {
                return;
            }
            SourcePath child = at;
            if (!child.append('/') || !child.append(name)) {
                return;
            }
            if (is_directory) {
                folders.push(child);
            } else {
                out.push(child);
            }
        }));
    }
}

void find_users(const platform::Vfs& vfs, std::string_view target, core::Array<SourcePath>& users) {
    DE_PROFILE_ZONE("find_users");
    core::Array<SourcePath> files(file_ops_heap());
    list_files(vfs, kContent, files);
    for (const SourcePath& file : files) {
        if (!names_files(file.view()) || within(file.view(), target)) {
            continue;
        }
        const auto bytes = vfs.read_whole_file(file.c_str(), file_ops_heap());
        if (bytes && names_target({reinterpret_cast<const char*>(bytes->data()), bytes->size()}, file.view(), target)) {
            users.push(file);
        }
    }
}

core::Result<FileOpReport> move_sources(const platform::Vfs& vfs, std::span<const SourceMove> moves) {
    DE_PROFILE_ZONE("move_sources");
    // Everything checked before anything moves.
    for (const SourceMove& move : moves) {
        if (!under_content(move.from.view()) || !path_rules_hold(move.to.view())) {
            return fail(ErrorCode::InvalidArgument,
                        "files move within content/, to names in lowercase letters, digits, _ - and .");
        }
        if (!vfs.exists(move.from.c_str())) {
            return fail(ErrorCode::NotFound, "what's moved isn't there");
        }
        if (vfs.exists(move.to.c_str())) {
            return fail(ErrorCode::AlreadyExists, "something has that name there already");
        }
        if (within(move.to.view(), move.from.view())) {
            return fail(ErrorCode::InvalidArgument, "a folder can't go into itself");
        }
    }
    // Every text source's references, worked out from where things are now.
    struct Rewrite {
        SourcePath path; // where it will be
        core::Array<char> text{file_ops_heap()};
    };
    core::Array<SourcePath> files(file_ops_heap());
    list_files(vfs, kContent, files);
    core::Array<Rewrite> rewrites(file_ops_heap());
    core::Array<char> text(file_ops_heap());
    const auto exists = [&vfs](std::string_view path) {
        const SourcePath copy(path);
        return vfs.exists(copy.c_str());
    };
    for (const SourcePath& file : files) {
        if (!names_files(file.view())) {
            continue;
        }
        const auto bytes = vfs.read_whole_file(file.c_str(), file_ops_heap());
        if (!bytes) {
            continue;
        }
        SourcePath now;
        static_cast<void>(moved_path(file.view(), moves, now));
        if (rewrite_references({reinterpret_cast<const char*>(bytes->data()), bytes->size()}, file.view(), now.view(),
                               moves, exists, text)) {
            Rewrite& rewrite = rewrites.emplace();
            rewrite.path = now;
            rewrite.text.assign(text.span());
        }
    }
    // The moves, each with its meta, then the rewritten texts where they are now.
    FileOpReport report;
    for (const SourceMove& move : moves) {
        const bool folder = vfs.file_info(move.from.c_str()).ok() && vfs.file_info(move.from.c_str())->is_directory;
        if (const core::Result<void> moved = vfs.move(move.from.c_str(), move.to.c_str()); !moved) {
            DE_LOG_ERROR("editor", "%s didn't move to %s: %s", move.from.c_str(), move.to.c_str(),
                         moved.error().message);
            return fail(moved.error());
        }
        ++report.moved;
        SourcePath meta_from;
        SourcePath meta_to;
        if (meta_path(move.from.view(), folder, meta_from) && meta_path(move.to.view(), folder, meta_to) &&
            vfs.exists(meta_from.c_str())) {
            if (const core::Result<void> moved = vfs.move(meta_from.c_str(), meta_to.c_str()); !moved) {
                DE_LOG_WARN("editor", "%s's meta didn't move (the cooker relinks it by content): %s", move.to.c_str(),
                            moved.error().message);
            }
        }
    }
    for (const Rewrite& rewrite : rewrites) {
        if (const core::Result<void> written = vfs.write_file(rewrite.path.c_str(), std::as_bytes(rewrite.text.span()));
            !written) {
            DE_LOG_ERROR("editor", "%s's references weren't rewritten: %s", rewrite.path.c_str(),
                         written.error().message);
            continue;
        }
        ++report.rewritten;
    }
    return report;
}

core::Result<void> make_source_folder(const platform::Vfs& vfs, std::string_view path) {
    if (!path_rules_hold(path)) {
        return fail(ErrorCode::InvalidArgument,
                    "folders go in content/, named in lowercase letters, digits, _ - and .");
    }
    const SourcePath folder(path);
    if (vfs.exists(folder.c_str())) {
        return fail(ErrorCode::AlreadyExists, "something has that name there already");
    }
    return vfs.create_folder(folder.c_str());
}

core::Result<uint32_t> trash_sources(const platform::Vfs& vfs, std::span<const SourcePath> paths) {
    uint32_t trashed = 0;
    for (const SourcePath& path : paths) {
        if (!under_content(path.view())) {
            return fail(ErrorCode::InvalidArgument, "only what's in content/ is deleted here");
        }
        const core::Result<platform::FileInfo> info = vfs.file_info(path.c_str());
        if (!info) {
            continue; // gone already (in a folder deleted before it)
        }
        if (const core::Result<void> gone = vfs.move_to_trash(path.c_str()); !gone) {
            return fail(gone.error());
        }
        ++trashed;
        SourcePath meta;
        if (meta_path(path.view(), info->is_directory, meta) && vfs.exists(meta.c_str())) {
            if (const core::Result<void> gone = vfs.move_to_trash(meta.c_str()); !gone) {
                DE_LOG_WARN("editor", "%s's meta stayed (cooker --check reports it): %s", path.c_str(),
                            gone.error().message);
            }
        }
    }
    return trashed;
}

bool clean_file_name(std::string_view name, std::span<char> out, size_t& length) {
    length = 0;
    bool gap = false;
    for (const char raw : name) {
        const char c = raw >= 'A' && raw <= 'Z' ? static_cast<char>(raw - 'A' + 'a') : raw;
        const bool fine = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_';
        if (!fine) {
            gap = length > 0; // one '_' for a run of spaces and marks, none in front
            continue;
        }
        if (gap && c != '.') { // none before an extension
            if (length + 1 >= out.size()) {
                return false;
            }
            out[length++] = '_';
        }
        gap = false;
        if (length + 1 >= out.size()) {
            return false;
        }
        out[length++] = c;
    }
    out[length] = '\0';
    const std::string_view cleaned(out.data(), length);
    return !cleaned.empty() && cleaned != "." && cleaned != ".." && cleaned.back() != '.';
}

} // namespace de::editor
