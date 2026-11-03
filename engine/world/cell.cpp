#include "world/cell.h"

#include <cmath>

namespace de::world {

void register_cells(flecs::world& ecs, asset::AssetRegistry* assets) {
    ecs.component<asset::AssetHandle>("de::AssetHandle").member<uint32_t>("index").member<uint32_t>("generation");
    ecs.component<CellCoord>("de::CellCoord").member<int32_t>("x").member<int32_t>("z");
    ecs.component<InCell>("de::InCell")
        .add(flecs::Relationship)
        .add(flecs::Exclusive)    // one cell per entity
        .add(flecs::DontFragment) // no table per cell
        .add(flecs::OnDeleteTarget, flecs::Delete);
    ecs.component<Cell>("de::Cell")
        // One hold per Cell: a replaced one releases the hold it had (unless it's the same).
        .on_replace([assets](Cell& before, Cell& after) {
            if (assets != nullptr && before.asset.is_valid() && !(before.asset == after.asset)) {
                assets->release(before.asset);
            }
        })
        // Removed or deleted: the hold goes, and so do the members, whose handles may name assets
        // only this cell held (deleting the cell entity has deleted them already).
        .on_remove([assets](flecs::entity cell, Cell& removed) {
            cell.world().delete_with(cell.world().pair<InCell>(cell));
            if (assets != nullptr && removed.asset.is_valid()) {
                assets->release(removed.asset);
            }
            removed.asset = {};
        });
    ecs.component<Cell>().member<asset::AssetHandle>("asset").member<CellCoord>("coord");
}

flecs::entity create_cell(flecs::world& ecs, CellCoord coord, asset::AssetHandle asset) {
    return ecs.entity().set(Cell(asset, coord));
}

void add_to_cell(flecs::entity entity, flecs::entity cell) {
    entity.add<InCell>(cell);
}

CellCoord cell_at(const DVec3& position) {
    return {.x = static_cast<int32_t>(std::floor(position.x / kCellSize)),
            .z = static_cast<int32_t>(std::floor(position.z / kCellSize))};
}

} // namespace de::world
