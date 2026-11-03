#include "world/cell.h"

#include <cmath>

namespace de::world {

void register_cells(flecs::world& ecs, asset::AssetRegistry* assets) {
    ecs.component<asset::AssetHandle>("de::AssetHandle").member<uint32_t>("index").member<uint32_t>("generation");
    ecs.component<CellCoord>("de::CellCoord").member<int32_t>("x").member<int32_t>("z");
    ecs.component<Cell>("de::Cell").member<asset::AssetHandle>("asset").member<CellCoord>("coord");
    ecs.component<InCell>("de::InCell")
        .add(flecs::Relationship)
        .add(flecs::Exclusive)    // one cell per entity
        .add(flecs::DontFragment) // no table per cell
        .add(flecs::OnDeleteTarget, flecs::Delete);
    // The cell's hold on its asset goes with the cell: removed, or deleted (at shutdown too).
    ecs.observer<const Cell>("de::ReleaseCellAsset").event(flecs::OnRemove).each([assets](const Cell& cell) {
        if (assets != nullptr && cell.asset.is_valid()) {
            assets->release(cell.asset);
        }
    });
}

flecs::entity create_cell(flecs::world& ecs, CellCoord coord, asset::AssetHandle asset) {
    return ecs.entity().set(Cell{.asset = asset, .coord = coord});
}

void add_to_cell(flecs::entity entity, flecs::entity cell) {
    entity.add<InCell>(cell);
}

CellCoord cell_at(const DVec3& position) {
    return {.x = static_cast<int32_t>(std::floor(position.x / kCellSize)),
            .z = static_cast<int32_t>(std::floor(position.z / kCellSize))};
}

} // namespace de::world
