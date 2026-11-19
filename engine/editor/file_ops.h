#pragma once

#include "core/allocator.h"
#include "core/array.h"
#include "core/fixed_string.h"
#include "core/function_ref.h"
#include "core/result.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace de::platform {
class Vfs;
}

namespace de::editor {

// The editor's file operations on the open project's sources (src:/content/), which keep the project whole: a file
// or folder moved takes its meta (src:/meta/..., so it keeps its asset ID) and every text source that names it by a
// relative path is rewritten to follow it, as are the moved text sources' own references; one deleted goes to the
// recycle bin with its meta, after the editor has said who still names it.
using SourcePath = core::FixedString<255>;

// A move of a file or a folder (and all in it): "src:/content/props/rock" to "src:/content/rocks/rock".
struct SourceMove {
    SourcePath from;
    SourcePath to;
};

// Text sources: JSON (with comments) that can name other files by relative path: .world .cell .material .terrain
// .terrainlayers .environment .prefab, and .gltf (its buffers and images).
bool names_files(std::string_view path);

// The meta of a source: "src:/content/a/b.gltf" is "src:/meta/a/b.gltf.json", a folder "src:/content/a" is
// "src:/meta/a". False if path isn't under src:/content/ or doesn't fit.
bool meta_path(std::string_view path, bool folder, SourcePath& out);

// Where a path is after the moves: itself if none takes it (false), else where the move of it, or of a folder it's
// in, puts it (true).
bool moved_path(std::string_view path, std::span<const SourceMove> moves, SourcePath& out);

// Each string literal of a JSON text with // and /* */ comments: the offsets of its contents (between the quotes).
void for_each_string(std::string_view text, core::FunctionRef<void(size_t begin, size_t end)> visit);

// Whether a string in a text source is a reference: a relative path whose last part has an extension ("rock.gltf",
// "../water/sea_foam.material"); names, keys and folder names aren't.
bool looks_like_reference(std::string_view value);

// A text source's text after the moves. It was at old_path and is now at new_path (the same if it stays); each of
// its references (relative to its folder, naming something exists() says is there) is made to name where that thing
// is now, from where the file is now. False if nothing in it changes.
bool rewrite_references(std::string_view text, std::string_view old_path, std::string_view new_path,
                        std::span<const SourceMove> moves, core::FunctionRef<bool(std::string_view)> exists,
                        core::Array<char>& out);

// Whether a text source (at path) names target, or anything in it if it's a folder.
bool names_target(std::string_view text, std::string_view path, std::string_view target);

// Every file under a folder ("src:/content"), at any depth, by full path.
void list_files(const platform::Vfs& vfs, std::string_view folder, core::Array<SourcePath>& out);

// The text sources that name target (or anything in it), except those inside it.
void find_users(const platform::Vfs& vfs, std::string_view target, core::Array<SourcePath>& users);

struct FileOpReport {
    uint32_t moved = 0;     // files and folders
    uint32_t rewritten = 0; // text sources whose references changed
};

// Moves sources as above. Refused before anything moves if one isn't there, a target is taken, a folder would go
// into itself, or a path is outside src:/content/ or breaks the path rules (lowercase, no spaces).
core::Result<FileOpReport> move_sources(const platform::Vfs& vfs, std::span<const SourceMove> moves);

// Makes a folder under src:/content/.
core::Result<void> make_source_folder(const platform::Vfs& vfs, std::string_view path);

// Sends sources (files or folders) and their metas to the recycle bin.
core::Result<uint32_t> trash_sources(const platform::Vfs& vfs, std::span<const SourcePath> paths);

// A name as a path part: lowercase, spaces and other marks as '_' ("Rock 01.GLTF" is "rock_01.gltf"). False if
// nothing's left, or it's "." or "..".
bool clean_file_name(std::string_view name, std::span<char> out, size_t& length);

} // namespace de::editor
