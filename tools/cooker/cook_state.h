#pragma once

#include "asset/asset_id.h"
#include "asset/format.h"
#include "cooker/settings.h"
#include "core/hash_map.h"

#include <cstdint>

namespace de::cooker {

struct CookedOutput {
    asset::AssetId id{};
    asset::AssetType type = asset::AssetType::Unknown;
};

// A texture a model or material uses, and how: textures take their defaults from it.
struct TextureUse {
    SourcePath path{}; // the texture source
    TextureUsage usage = TextureUsage::Unknown;
};

// What the last cook did with one source.
struct CookRecord {
    SourcePath path{};        // "content/rocks/rock.png", "shaders/imgui.slang"
    asset::Guid guid{};       // none for shaders
    uint64_t source_hash = 0; // of the source file's bytes: re-links a moved file's meta
    uint64_t input_hash = 0;  // of everything the outputs depend on: unchanged means up to date
    core::Array<CookedOutput> outputs{tools_heap()};
    core::Array<SourcePath> dependencies{tools_heap()}; // other files it read (shader imports, glTF buffers)
    core::Array<TextureUse> texture_uses{tools_heap()};
};

// The cooker's memory between runs, in game:/cook_state.json next to the cooked files (build
// output, never committed): what each source cooked to and from what, so unchanged sources are
// skipped, outputs of deleted sources removed, and metas of files moved outside the tools re-linked
// by content hash. Also maps cooked files (named by asset ID) back to their sources for people.
class CookState {
public:
    static constexpr const char* kPath = "game:/cook_state.json";

    // A missing or unreadable state is empty (everything cooks); a broken one is reported as a warning.
    static CookState load(const platform::Vfs& vfs, Diagnostics& diagnostics);
    core::Result<void> save(const platform::Vfs& vfs) const;

    const CookRecord* find(std::string_view path) const;
    const CookRecord* find(asset::Guid guid) const;
    std::span<const CookRecord> records() const { return records_.span(); }

    // Replaces the record for record.path, or adds it.
    void put(CookRecord record);
    void remove(std::string_view path);

private:
    void reindex();

    core::Array<CookRecord> records_{tools_heap()};
    core::HashMap<uint64_t, uint32_t> by_path_{tools_heap()}; // path hash -> index into records_
};

} // namespace de::cooker
