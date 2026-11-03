#include "cooker/source_tree.h"

#include "cooker/cook_state.h"
#include "core/hash.h"
#include "platform/random.h"
#include "platform/vfs.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace de::cooker {

namespace {

constexpr std::string_view kContent = "content";
constexpr std::string_view kMeta = "meta";
constexpr std::string_view kFolderRules = "_folder.json";
constexpr std::string_view kMetaExtension = ".json";

bool path_less(const SourcePath& a, const SourcePath& b) {
    return a.view() < b.view();
}

} // namespace

void list_tree(const platform::Vfs& vfs, std::string_view root, bool report_bad_names, Diagnostics& diagnostics,
               core::Array<SourcePath>& files) {
    core::Array<SourcePath> folders(tools_heap());
    folders.push(SourcePath(root));
    while (!folders.empty()) {
        const SourcePath folder = folders.back();
        folders.pop();
        const core::Result<void> listed =
            vfs.list(src_path(folder.view()).c_str(), [&](const char* name, bool is_directory) {
                if (name[0] == '.') {
                    return;
                }
                SourcePath child = folder;
                if (!child.append('/') || !child.append(name)) {
                    diagnostics.error(folder.view(), "'%s': the path is longer than %zu characters (skipped)", name,
                                      kMaxSourcePath);
                    return;
                }
                if (!vfs.resolve(src_path(child.view()).c_str()).ok()) {
                    if (report_bad_names) {
                        diagnostics.error(child.view(), "names use lowercase letters, digits and _ - . only: rename "
                                                        "it (skipped, with anything inside)");
                    }
                    return;
                }
                (is_directory ? folders : files).push(child);
            });
        if (!listed && folder.view() != root) {
            diagnostics.error(folder.view(), "the folder could not be listed: %s", listed.error().message);
        }
    }
    std::ranges::sort(files, path_less);
}

namespace {

// "content/rocks/rock.png" -> "meta/rocks/rock.png.json"
bool meta_path_of(std::string_view source, SourcePath& meta) {
    meta.assign(kMeta);
    return meta.append(source.substr(kContent.size())) && meta.append(kMetaExtension);
}

// The new meta's text: just the GUID; settings are added by people when they differ from the rules.
core::Array<char> new_meta_text(asset::Guid guid) {
    const JsonBuilder json;
    const asset::GuidText text = asset::format_guid(guid);
    yyjson_mut_obj_add_strncpy(json.doc(), json.root(), "guid", text.c_str(), text.size());
    return json.write(true);
}

core::Result<asset::Guid> mint_guid() {
    std::byte bytes[16];
    if (const core::Result<void> random = platform::random_bytes(bytes); !random) {
        return core::fail(random.error());
    }
    asset::Guid guid;
    std::memcpy(&guid.high, bytes, 8);
    std::memcpy(&guid.low, bytes + 8, 8);
    // A version 4 (random) UUID, so other tools read it as one.
    guid.high = (guid.high & ~0xF000ULL) | 0x4000ULL;
    guid.low = (guid.low & ~(0xC000ULL << 48U)) | (0x8000ULL << 48U);
    return guid;
}

// Settings sections are objects: anything else would replace the layers below it whole (merge
// patches), silently dropping every default.
bool sections_are_objects(yyjson_val* object, std::string_view path, Diagnostics& diagnostics) {
    bool ok = true;
    for (const char* section : {"texture", "mesh"}) {
        yyjson_val* value = yyjson_obj_get(object, section);
        if (value != nullptr && !yyjson_is_obj(value)) {
            diagnostics.error(path, "\"%s\" should be an object of settings", section);
            ok = false;
        }
    }
    return ok;
}

// A folder rules file: {"rules": [{"match": "<pattern>", "texture": {...}, "mesh": {...}}, ...]}.
bool valid_rules(yyjson_val* root, std::string_view path, Diagnostics& diagnostics) {
    yyjson_val* rules = yyjson_obj_get(root, "rules");
    if (!yyjson_is_obj(root) || !yyjson_is_arr(rules)) {
        diagnostics.error(path, "should be {\"rules\": [...]} (the file is ignored)");
        return false;
    }
    bool ok = true;
    size_t index = 0;
    size_t count = 0;
    yyjson_val* rule = nullptr;
    yyjson_arr_foreach(rules, index, count, rule) {
        if (!yyjson_is_obj(rule) || !yyjson_is_str(yyjson_obj_get(rule, "match"))) {
            diagnostics.error(path, "rule %zu needs a \"match\" pattern (the file is ignored)", index + 1);
            ok = false;
            continue;
        }
        static constexpr const char* kKeys[] = {"match", "texture", "mesh"};
        const JsonReader reader{.object = rule, .path = path, .diagnostics = &diagnostics};
        reader.check_keys(kKeys, "rule");
        ok = sections_are_objects(rule, path, diagnostics) && ok;
    }
    return ok;
}

// A meta's GUID, checked; its other sections are read when the source cooks.
bool read_meta(const platform::Vfs& vfs, std::string_view meta_path, Diagnostics& diagnostics, JsonDoc& doc,
               asset::Guid& guid) {
    const core::Result<core::Array<std::byte>> text = read_whole_file(vfs, src_path(meta_path).c_str());
    if (!text) {
        diagnostics.error(meta_path, "could not be read: %s", text.error().message);
        return false;
    }
    core::Result<JsonDoc> parsed = JsonDoc::parse(text->span(), meta_path, diagnostics);
    if (!parsed) {
        return false;
    }
    yyjson_val* value = yyjson_obj_get(parsed->root(), "guid");
    if (!yyjson_is_str(value) || !asset::parse_guid({yyjson_get_str(value), yyjson_get_len(value)}, guid)) {
        diagnostics.error(meta_path, R"(needs a "guid" like "6f9619ff-8b86-4011-b42d-00c04fc964ff")");
        return false;
    }
    static constexpr const char* kKeys[] = {"guid", "texture", "mesh"};
    const JsonReader reader{.object = parsed->root(), .path = meta_path, .diagnostics = &diagnostics};
    reader.check_keys(kKeys, "meta");
    if (!sections_are_objects(parsed->root(), meta_path, diagnostics)) {
        return false;
    }
    doc = std::move(*parsed);
    return true;
}

struct MetaFile {
    SourcePath path{};   // "meta/rocks/rock.png.json"
    SourcePath source{}; // "content/rocks/rock.png"
    bool used = false;
};

struct NewFile {
    SourcePath path{};
    SourceKind kind = SourceKind::Texture;
    uint64_t hash = 0;
    bool hashed = false;
    bool settled = false;
};

uint64_t hash_of(const platform::Vfs& vfs, NewFile& file) {
    if (!file.hashed) {
        const core::Result<core::Array<std::byte>> bytes = read_whole_file(vfs, src_path(file.path.view()).c_str());
        file.hash = bytes ? core::hash_bytes(bytes->data(), bytes->size()) : 0;
        file.hashed = true;
    }
    return file.hash;
}

} // namespace

const char* source_kind_name(SourceKind kind) {
    switch (kind) {
    case SourceKind::Texture:
        return "texture";
    case SourceKind::Gltf:
        return "gltf";
    case SourceKind::Fbx:
        return "fbx";
    case SourceKind::Material:
        return "material";
    case SourceKind::Cell:
        return "cell";
    }
    return "unknown";
}

bool source_kind_of(std::string_view path, SourceKind& kind) {
    const std::string_view extension = extension_of(path);
    struct Entry {
        std::string_view extension;
        SourceKind kind;
    };
    static constexpr Entry kKinds[] = {
        {.extension = ".png", .kind = SourceKind::Texture},       {.extension = ".jpg", .kind = SourceKind::Texture},
        {.extension = ".jpeg", .kind = SourceKind::Texture},      {.extension = ".tga", .kind = SourceKind::Texture},
        {.extension = ".bmp", .kind = SourceKind::Texture},       {.extension = ".gltf", .kind = SourceKind::Gltf},
        {.extension = ".glb", .kind = SourceKind::Gltf},          {.extension = ".fbx", .kind = SourceKind::Fbx},
        {.extension = ".material", .kind = SourceKind::Material}, {.extension = ".cell", .kind = SourceKind::Cell},
    };
    for (const Entry& entry : kKinds) {
        if (entry.extension == extension) {
            kind = entry.kind;
            return true;
        }
    }
    return false;
}

SourceTree SourceTree::scan(const platform::Vfs& vfs, const CookState& previous, const ScanOptions& options,
                            Diagnostics& diagnostics, ScanStats* stats) {
    SourceTree tree;
    ScanStats counts;
    core::Array<SourcePath> content_files(tools_heap());
    core::Array<SourcePath> meta_paths(tools_heap());
    list_tree(vfs, kContent, true, diagnostics, content_files);
    list_tree(vfs, kMeta, true, diagnostics, meta_paths);

    // Folder rules, and metas by the source they're for.
    core::Array<MetaFile> metas(tools_heap());
    core::HashMap<uint64_t, uint32_t> meta_by_source(tools_heap());
    for (const SourcePath& path : meta_paths) {
        const std::string_view relative = path.view().substr(kMeta.size());
        if (file_name_of(path.view()) == kFolderRules) {
            FolderRules rules;
            rules.folder.assign(kContent);
            if (!rules.folder.append(folder_of(relative))) {
                continue;
            }
            const core::Result<core::Array<std::byte>> text = read_whole_file(vfs, src_path(path.view()).c_str());
            core::Result<JsonDoc> doc = text ? JsonDoc::parse(text->span(), path.view(), diagnostics)
                                             : core::Result<JsonDoc>(core::fail(text.error()));
            if (!doc || !valid_rules(doc->root(), path.view(), diagnostics)) {
                continue;
            }
            rules.depth =
                static_cast<uint32_t>(std::count(rules.folder.view().begin(), rules.folder.view().end(), '/'));
            rules.doc = std::move(*doc);
            tree.rules_.push(std::move(rules));
            continue;
        }
        if (!relative.ends_with(kMetaExtension)) {
            diagnostics.warning(path.view(), "not a meta file (metas are <source file>.json): ignored");
            continue;
        }
        MetaFile meta{.path = path};
        meta.source.assign(kContent);
        if (!meta.source.append(relative.substr(0, relative.size() - kMetaExtension.size()))) {
            continue;
        }
        meta_by_source.set(path_key(meta.source.view()), static_cast<uint32_t>(metas.size()));
        metas.push(meta);
    }
    std::ranges::stable_sort(tree.rules_, {}, &FolderRules::depth);

    // Sources with metas; new ones wait until orphans are settled.
    core::Array<NewFile> new_files(tools_heap());
    for (const SourcePath& path : content_files) {
        SourceKind kind = SourceKind::Texture;
        if (!source_kind_of(path.view(), kind)) {
            const std::string_view extension = extension_of(path.view());
            if (extension != ".bin" && extension != ".md" && extension != ".txt") {
                diagnostics.warning(path.view(), "not a type of source the cooker knows: ignored");
            }
            continue;
        }
        const uint32_t* index = meta_by_source.find(path_key(path.view()));
        if (index == nullptr || metas[*index].source != path.view()) {
            new_files.push(NewFile{.path = path, .kind = kind});
            continue;
        }
        MetaFile& meta = metas[*index];
        meta.used = true;
        Source source{.path = path, .kind = kind};
        if (read_meta(vfs, meta.path.view(), diagnostics, source.meta, source.guid)) {
            source.id = asset::asset_id(source.guid);
            tree.sources_.push(std::move(source));
        } else {
            tree.skipped_.push(path);
        }
    }

    // Orphans: re-link the ones whose file moved, by the content hash the last cook saw.
    core::Array<SourcePath> unresolved_extensions(tools_heap());
    for (const MetaFile& meta : metas) {
        if (meta.used) {
            continue;
        }
        JsonDoc doc;
        asset::Guid guid;
        if (!read_meta(vfs, meta.path.view(), diagnostics, doc, guid)) {
            continue;
        }
        const CookRecord* record = previous.find(guid);
        NewFile* match = nullptr;
        uint32_t matches = 0;
        for (NewFile& file : new_files) {
            if (record != nullptr && !file.settled &&
                extension_of(file.path.view()) == extension_of(meta.source.view()) &&
                hash_of(vfs, file) == record->source_hash) {
                match = &file;
                ++matches;
            }
        }
        if (matches != 1) {
            diagnostics.error(meta.path.view(),
                              matches > 1 ? "has no source file, and several new files have its old contents: move it "
                                            "next to the right one by hand"
                                          : "has no source file: move it with its file (same path under meta/, plus "
                                            ".json), or delete it if the file is gone for good");
            unresolved_extensions.push(SourcePath(extension_of(meta.source.view())));
            continue;
        }
        SourcePath new_meta;
        if (!meta_path_of(match->path.view(), new_meta)) {
            continue;
        }
        if (!options.write_metas) {
            diagnostics.error(match->path.view(), "moved without its meta: run the cooker to move %s to %s",
                              meta.path.c_str(), new_meta.c_str());
            match->settled = true;
            continue;
        }
        const core::Result<core::Array<std::byte>> text = read_whole_file(vfs, src_path(meta.path.view()).c_str());
        if (!text || !vfs.write_file(src_path(new_meta.view()).c_str(), text->span()).ok() ||
            !vfs.remove_file(src_path(meta.path.view()).c_str()).ok()) {
            diagnostics.error(match->path.view(), "moved without its meta, and %s could not be moved to %s",
                              meta.path.c_str(), new_meta.c_str());
            match->settled = true;
            continue;
        }
        diagnostics.warning(match->path.view(), "moved without its meta: re-linked %s (by content) and moved it to %s",
                            meta.path.c_str(), new_meta.c_str());
        match->settled = true;
        ++counts.metas_relinked;
        tree.sources_.push(Source{.path = match->path,
                                  .kind = match->kind,
                                  .guid = guid,
                                  .id = asset::asset_id(guid),
                                  .meta = std::move(doc)});
    }

    // New files get a GUID, unless they may belong to an orphan still unresolved.
    for (const NewFile& file : new_files) {
        if (file.settled) {
            continue;
        }
        const std::string_view extension = extension_of(file.path.view());
        const bool maybe_orphaned =
            std::ranges::any_of(unresolved_extensions, [&](const SourcePath& other) { return other == extension; });
        SourcePath meta_path;
        if (!meta_path_of(file.path.view(), meta_path)) {
            diagnostics.error(file.path.view(), "the meta's path would be too long");
            continue;
        }
        if (maybe_orphaned) {
            diagnostics.error(file.path.view(), "has no meta, and an orphaned meta of the same type is reported "
                                                "above: settle that first (no GUID minted, so none is lost)");
            tree.skipped_.push(file.path);
            continue;
        }
        if (!options.write_metas) {
            diagnostics.error(file.path.view(), "has no meta (%s): run the cooker to create it", meta_path.c_str());
            tree.skipped_.push(file.path);
            continue;
        }
        const core::Result<asset::Guid> guid = mint_guid();
        const core::Array<char> text = guid ? new_meta_text(*guid) : core::Array<char>(tools_heap());
        if (!guid || text.empty() || !vfs.write_file(src_path(meta_path.view()).c_str(), std::as_bytes(text.span()))) {
            diagnostics.error(file.path.view(), "its new meta %s could not be written", meta_path.c_str());
            continue;
        }
        ++counts.metas_created;
        JsonDoc doc;
        asset::Guid read_back;
        if (read_meta(vfs, meta_path.view(), diagnostics, doc, read_back)) {
            tree.sources_.push(Source{.path = file.path,
                                      .kind = file.kind,
                                      .guid = read_back,
                                      .id = asset::asset_id(read_back),
                                      .meta = std::move(doc)});
        }
    }

    // One GUID, one source: a copied meta would make two files the same asset.
    // The one the last cook knew by that GUID keeps it, so the copy is the one reported (deleting
    // the original's meta instead would break every reference to it); with no history, the first.
    std::ranges::sort(tree.sources_, [](const Source& a, const Source& b) { return a.path.view() < b.path.view(); });
    core::HashMap<uint64_t, uint32_t> owner(tools_heap()); // asset ID -> index in sources_
    for (uint32_t i = 0; i < tree.sources_.size(); ++i) {
        const Source& source = tree.sources_[i];
        const uint32_t* first = owner.find(source.id.value());
        const CookRecord* known = first != nullptr ? previous.find(source.guid) : nullptr;
        if (first == nullptr || (known != nullptr && known->path == source.path.view())) {
            owner.set(source.id.value(), i);
        }
    }
    core::Array<Source> unique(tools_heap());
    for (uint32_t i = 0; i < tree.sources_.size(); ++i) {
        Source& source = tree.sources_[i];
        const uint32_t kept = *owner.find(source.id.value());
        if (kept != i) {
            diagnostics.error(source.path.view(),
                              "its meta has the same GUID as %s's (a copied meta?): delete this file's meta to "
                              "give it a new GUID (skipped)",
                              tree.sources_[kept].path.c_str());
            tree.skipped_.push(source.path);
            continue;
        }
        unique.push(std::move(source));
    }
    tree.sources_ = std::move(unique);
    for (size_t i = 0; i < tree.sources_.size(); ++i) {
        tree.by_path_.set(path_key(tree.sources_[i].path.view()), static_cast<uint32_t>(i));
    }
    if (stats != nullptr) {
        *stats = counts;
    }
    return tree;
}

const Source* SourceTree::find(std::string_view path) const {
    const uint32_t* index = by_path_.find(path_key(path));
    return index != nullptr && sources_[*index].path == path ? &sources_[*index] : nullptr;
}

void SourceTree::resolve_settings(const Source& source, const char* section, JsonBuilder& settings,
                                  bool folder_rules) const {
    const std::string_view path = source.path.view();
    const std::string_view name = file_name_of(path);
    for (const FolderRules& rules : folder_rules ? rules_.span() : std::span<const FolderRules>()) {
        const std::string_view folder = rules.folder.view();
        if (!path.starts_with(folder) || path.size() <= folder.size() || path[folder.size()] != '/') {
            continue;
        }
        size_t index = 0;
        size_t count = 0;
        yyjson_val* rule = nullptr;
        yyjson_arr_foreach(yyjson_obj_get(rules.doc.root(), "rules"), index, count, rule) {
            yyjson_val* pattern = yyjson_obj_get(rule, "match");
            if (yyjson_is_str(pattern) && glob_match({yyjson_get_str(pattern), yyjson_get_len(pattern)}, name)) {
                settings.merge(yyjson_obj_get(rule, section));
            }
        }
    }
    settings.merge(yyjson_obj_get(source.meta.root(), section));
}

} // namespace de::cooker
