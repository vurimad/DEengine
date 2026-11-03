#pragma once

#include "asset/cell_format.h"
#include "asset/registry.h"
#include "core/array.h"
#include "core/math.h"
#include "world/transform.h"

#include <flecs.h>

#include <cstdint>

namespace de::world {

// The world is a grid of cells (docs/design.md, Open world & streaming: world model).
inline constexpr double kCellSize = asset::kCellSize;

struct CellCoord {
    int32_t x = 0; // along world X
    int32_t z = 0; // along world Z
    constexpr bool operator==(const CellCoord&) const = default;
};

// On a cell's entity: the cell asset it was spawned from, held by it: exactly one hold per Cell.
// Removing it (or deleting the cell entity) releases the hold, and deletes the cell's members with
// it, since they may name assets only the cell held; replacing it releases the old hold. Not
// copyable (a copy would be a second release of one hold): a cell entity can't be cloned.
struct Cell {
    asset::AssetHandle asset;
    CellCoord coord;

    Cell() = default;
    Cell(asset::AssetHandle held, CellCoord at) : asset(held), coord(at) {}
    Cell(const Cell&) = delete;
    Cell& operator=(const Cell&) = delete;
    Cell(Cell&& other) noexcept : asset(other.asset), coord(other.coord) { other.asset = {}; }
    Cell& operator=(Cell&& other) noexcept {
        asset = other.asset;
        coord = other.coord;
        other.asset = {};
        return *this;
    }
    ~Cell() = default;
};

// On a cell entity made by load_cell(): the file its members were spawned from (null: none yet).
// Its members always match the cell's loaded file: spawned once it's Ready, spawned again when hot
// reload swaps in a new one.
// The file is told by its address and its content hash: after two reloads between ticks, the
// newest file may lie where the first one did.
struct CellContents {
    const void* spawned_from = nullptr; // compared, never read: it may be gone
    uint64_t content_hash = 0;
};

// (InCell, cell): which cell an entity belongs to. Doesn't fragment tables (cells would otherwise
// make a table each), and deletes its entities with their cell: unloading a cell is deleting its
// entity. An entity is in at most one cell.
struct InCell {};

// Registers the components (reflection included) and the hooks that release a cell's asset through
// `assets` when the cell goes, and keeps `assets` for load_cell(). World::create calls it.
void register_cells(flecs::world& ecs, asset::AssetRegistry* assets);

// Starts loading a cooked cell (.decell) and returns its entity. Once the cell and every mesh it
// uses are loaded, its members are spawned in one batch at the start of a tick (Input phase): each
// placed (world and previous transforms set, so nothing slides in), a member of the cell, with its
// mesh, its parent and, for static ones, StaticTransform. Cell::coord is set then. A cell that
// fails to load stays empty (the registry logs why). Deleting the entity unloads it. It loads
// through the world's registry (WorldDesc::assets); without one it logs an error and returns an
// invalid entity.
flecs::entity load_cell(flecs::world& ecs, asset::AssetId id,
                        platform::IoPriority priority = platform::IoPriority::Near);

// Spawning's working memory, kept between uses so it doesn't allocate once settled.
struct CellSpawnScratch {
    core::Array<flecs::entity_t> pending{core::heap_allocator(core::MemoryTag::World)};
    core::Array<flecs::entity_t> members{core::heap_allocator(core::MemoryTag::World)};
    core::Array<WorldTransform> worlds{core::heap_allocator(core::MemoryTag::World)};
};

// Adds the system that spawns cells' members (see load_cell), in phase. scratch must outlive the
// world. World::create calls it when it has a registry.
void add_cell_systems(flecs::world& ecs, flecs::entity phase, asset::AssetRegistry& assets, CellSpawnScratch& scratch);

// A cell's entity, taking over a hold on its asset (load() it, hand the handle here; the cell
// releases it). Its members are added with add_to_cell(). Deleting the entity unloads the cell.
flecs::entity create_cell(flecs::world& ecs, CellCoord coord, asset::AssetHandle asset);

// Makes entity a member of cell (deleted with it).
void add_to_cell(flecs::entity entity, flecs::entity cell);

// The cell a world position lies in.
CellCoord cell_at(const DVec3& position);

} // namespace de::world
