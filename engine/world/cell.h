#pragma once

#include "asset/registry.h"
#include "core/math.h"

#include <flecs.h>

#include <cstdint>

namespace de::world {

// The world is a grid of cells (docs/design.md, Open world & streaming: world model).
inline constexpr double kCellSize = 128.0;

struct CellCoord {
    int32_t x = 0; // along world X
    int32_t z = 0; // along world Z
    constexpr bool operator==(const CellCoord&) const = default;
};

// On a cell's entity: the cell asset it was spawned from, held by it. Removing this (deleting the
// cell entity) releases the hold, and with it everything the cell's asset depends on.
struct Cell {
    asset::AssetHandle asset;
    CellCoord coord;
};

// (InCell, cell): which cell an entity belongs to. Doesn't fragment tables (cells would otherwise
// make a table each), and deletes its entities with their cell: unloading a cell is deleting its
// entity. An entity is in at most one cell.
struct InCell {};

// Registers the components (reflection included) and the observer that releases a cell's asset
// through `assets` when the cell goes. World::create calls it.
void register_cells(flecs::world& ecs, asset::AssetRegistry* assets);

// A cell's entity, taking over a hold on its asset (load() it, hand the handle here; the cell
// releases it). Its members are added with add_to_cell(). Deleting the entity unloads the cell.
flecs::entity create_cell(flecs::world& ecs, CellCoord coord, asset::AssetHandle asset);

// Makes entity a member of cell (deleted with it).
void add_to_cell(flecs::entity entity, flecs::entity cell);

// The cell a world position lies in.
CellCoord cell_at(const DVec3& position);

} // namespace de::world
