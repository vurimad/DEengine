#pragma once

#include "asset/asset_id.h"
#include "cooker/json.h"
#include "core/hash_map.h"

#include <cstdint>

namespace de::cooker {

class CookState;

// Every file under a src:/ folder ("content", "shaders"), sorted. Hidden entries (".gitkeep") are
// skipped, and so are names that break the path rules, with everything under them: reported if
// report_bad_names (content and metas must follow the rules; shaders/README.md needn't).
void list_tree(const platform::Vfs& vfs, std::string_view root, bool report_bad_names, Diagnostics& diagnostics,
               core::Array<SourcePath>& files);

// What the cooker makes of a source file, by extension.
enum class SourceKind : uint8_t {
    Texture,  // .png .jpg .jpeg .tga .bmp
    Gltf,     // .gltf .glb
    Fbx,      // .fbx
    Material, // .material: our own text material (JSON)
    Cell,     // .cell: the entities placed in one cell of the world (JSON)
};

const char* source_kind_name(SourceKind kind);
// False for files that aren't assets of their own (a glTF's .bin buffers, notes, unknown types).
bool source_kind_of(std::string_view path, SourceKind& kind);

// One asset source under content/, with its meta.
struct Source {
    SourcePath path{}; // "content/rocks/rock.png"
    SourceKind kind = SourceKind::Texture;
    asset::Guid guid{};
    asset::AssetId id{}; // of its main asset: asset::asset_id(guid)
    JsonDoc meta{};      // the parsed meta file, for its settings sections
};

struct ScanOptions {
    // Create metas for new files and move the metas of re-linked ones. Off (--check): only report
    // what would change, as errors.
    bool write_metas = true;
};

struct ScanStats {
    uint32_t metas_created = 0;
    uint32_t metas_relinked = 0;
};

// The source tree and its mirrored meta tree (docs/design.md, Asset formats): every file under
// content/ has a meta at the same relative path under meta/ ("content/rocks/rock.png" ->
// "meta/rocks/rock.png.json") holding its GUID and the settings that differ from the folder rules
// (meta/**/_folder.json). Scanning matches the two trees:
// - a new file gets a meta with a fresh GUID;
// - a meta whose file is gone is an orphan: if the file moved outside the tools, the last cook's
//   content hash finds it and the meta moves along (re-linked); otherwise it's reported. A new
//   file that might be an unresolved orphan's (same extension) gets no GUID until that's settled:
//   the cooker never silently mints a new GUID for an existing asset;
// - names that break the path rules (uppercase, spaces), broken metas and folder rules, and
//   repeated GUIDs are reported, and those files left out. When two metas share a GUID, the file
//   the last cook knew by it keeps it: the copy is the one reported.
class SourceTree {
public:
    static SourceTree scan(const platform::Vfs& vfs, const CookState& previous, const ScanOptions& options,
                           Diagnostics& diagnostics, ScanStats* stats = nullptr);

    // Sorted by path.
    std::span<const Source> sources() const { return sources_.span(); }
    // Sources left out for a broken meta, a GUID another file has, or no meta yet: what was cooked
    // from them stays until they're sorted out.
    std::span<const SourcePath> skipped() const { return skipped_.span(); }
    const Source* find(std::string_view path) const;

    // Layers one settings section ("texture", "mesh") onto settings, which holds the defaults:
    // folder rules from the root down (each file's rules in order, those whose pattern matches the
    // file's name), then the meta's own section. folder_rules false: the meta's section only (a
    // model's embedded images, which the rules' name patterns don't describe).
    void resolve_settings(const Source& source, const char* section, JsonBuilder& settings,
                          bool folder_rules = true) const;

private:
    struct FolderRules {
        SourcePath folder; // the content folder they apply to: "content", "content/rocks"
        uint32_t depth = 0;
        JsonDoc doc;
    };

    core::Array<Source> sources_{tools_heap()};
    core::Array<SourcePath> skipped_{tools_heap()};
    core::Array<FolderRules> rules_{tools_heap()}; // root first
    core::HashMap<uint64_t, uint32_t> by_path_{tools_heap()};
};

} // namespace de::cooker
