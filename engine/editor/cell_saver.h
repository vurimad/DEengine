#pragma once

#include "asset/asset_id.h"
#include "core/allocator.h"
#include "core/array.h"
#include "core/function_ref.h"
#include "core/hash_map.h"
#include "platform/file.h"

#include <flecs.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace de::asset {
class AssetRegistry;
}
namespace de::platform {
class Vfs;
}

namespace de::editor {

// The world's edits, written back to its cells' text sources (docs/design.md, Editor & tooling: edits save to the
// text sources, never to cooked data; World model: nobody files things into cells by hand, the editor files each into
// the cell its position falls in).

// The .cell and .world sources under src:/content: which file a loaded cell came from, each cell file's place in the
// grid, and which cells each world lists.
class CellSources {
public:
    struct Cell {
        asset::AssetId id;
        platform::PathString path; // "src:/content/world/test/cell_0_0.cell"
        int32_t x = 0;
        int32_t z = 0;
        bool placed = false; // its coord could be read
    };
    struct World {
        asset::AssetId id;
        platform::PathString path;
    };

    explicit CellSources(core::Allocator& allocator)
        : cells_(allocator), worlds_(allocator), listed_(allocator), by_id_(allocator) {}
    // Looks through src:/content again. False if it couldn't be listed (logged).
    bool scan(const platform::Vfs& vfs);

    // The source of a cell asset, or null.
    const Cell* find(asset::AssetId id) const;
    // The world a cell file belongs to: of those listing it, the one with the ID wanted if it's among them, else the
    // first. Its index for world(), or UINT32_MAX if no world lists it.
    uint32_t world_of(std::string_view cell_path, asset::AssetId wanted) const;
    const World& world(uint32_t index) const { return worlds_[index]; }
    // The cell a world lists at that place in the grid, or null.
    const Cell* cell_at(uint32_t world, int32_t x, int32_t z) const;
    size_t size() const { return cells_.size(); }

private:
    core::Array<Cell> cells_;
    core::Array<World> worlds_;
    struct Listing {
        uint32_t world = 0;
        uint32_t cell = 0;
    };
    core::Array<Listing> listed_;
    core::HashMap<uint64_t, uint32_t> by_id_; // a cell asset's ID -> cells_
};

// Something's key while it's being saved and cooked: its cell's place in the grid and its place in the cell's source.
// A respawn keeps it (an entity's ID it doesn't), and a save that moves it to another file says where it went.
struct MemberKey {
    int32_t x = 0;
    int32_t z = 0;
    uint32_t source_index = 0;
    bool operator==(const MemberKey&) const = default;
};
struct MemberMove {
    MemberKey from;
    MemberKey to;
};

struct CellSaveReport {
    uint32_t files_written = 0; // cells (and worlds) whose text changed
    uint32_t entities_changed = 0;
    uint32_t refiled = 0;      // moved into the file of the cell they stand in now (with what's under them)
    uint32_t cells_made = 0;   // files made for cells that had none, and added to their world
    uint32_t cells_failed = 0; // a source that couldn't be found, read, understood or written (logged)
    uint32_t outside_cell = 0; // out of their cell's square, but no world lists their file to file them by: left
};

// For every loaded cell, each member as its file states it, where it differs from what the cell was cooked from: its
// transform (relative to its parent there, or the world; position, rotation and scale each only if it changed) and its
// fog volume's values, so a file diffs as its edits. A root that now stands in another cell's square goes to that
// cell's file in the same world (world: the one open, among those listing the file), with everything under it; a file
// is made for a cell that has none, next to the one it came from, and added to the world. Files whose text changed are
// written (atomically). written gets each path written; moves, where things went in the files (from where the cooked
// cells have them).
CellSaveReport save_cells(const flecs::world& ecs, const asset::AssetRegistry& assets, const platform::Vfs& vfs,
                          const CellSources& sources, asset::AssetId world,
                          core::Array<platform::PathString>* written = nullptr,
                          core::Array<MemberMove>* moves = nullptr);

// Of a cook report's text (game:/cook_report.json): whether it has an error for this source ("src:/content/a.cell").
bool cook_report_has_error(std::string_view report, std::string_view source);

// Follows edited things through their cells being spawned again (hot reload, after a save is cooked), and into the cell
// a save moved them to: each is known by its MemberKey, not by its entity.
class MemberTracker {
public:
    explicit MemberTracker(core::Allocator& allocator)
        : keys_(allocator), revisions_(allocator), members_(allocator), moved_(allocator) {}

    // Notes where an entity came from, while it's alive (a member of a loaded cell; anything else is let be).
    void note(const flecs::world& ecs, const asset::AssetRegistry& assets, flecs::entity_t entity);
    // A save moved things in the files: their keys follow.
    void apply(std::span<const MemberMove> moves);
    // After cells were spawned again (or for the first time) since the last call: each noted entity that's gone moves
    // to the member spawned in its place (moved(from, to)), and is known as that from then on.
    void follow(const flecs::world& ecs, const asset::AssetRegistry& assets,
                core::FunctionRef<void(flecs::entity_t from, flecs::entity_t to)> moved);

private:
    static uint64_t pack(const MemberKey& key);

    core::HashMap<flecs::entity_t, MemberKey> keys_;
    core::HashMap<flecs::entity_t, uint32_t> revisions_; // each cell's spawned revision, as last seen
    core::HashMap<uint64_t, flecs::entity_t> members_;   // every member by its key, while following
    struct Moved {
        flecs::entity_t from = 0;
        flecs::entity_t to = 0;
        MemberKey key;
    };
    core::Array<Moved> moved_;
};

} // namespace de::editor
