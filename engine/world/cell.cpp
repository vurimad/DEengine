#include "world/cell.h"

#include "core/log.h"
#include "core/profile.h"
#include "world/render_extract.h"

#include <cmath>
#include <cstring>

namespace de::world {

namespace {

// A singleton: the registry cells load through, the one their hooks release through.
struct CellAssets {
    asset::AssetRegistry* assets = nullptr;
};

// The file a cell's members should match: its loaded file (null while loading, or if it failed).
CellContents loaded_contents(const asset::AssetRegistry& assets, asset::AssetHandle cell) {
    const std::span<const std::byte> file = assets.file(cell);
    if (file.empty()) {
        return {};
    }
    asset::AssetHeader header;
    std::memcpy(&header, file.data(), sizeof(header));
    return {.spawned_from = file.data(), .content_hash = header.content_hash};
}

} // namespace

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
    ecs.component<CellContents>("de::CellContents");
    ecs.component<CellAssets>("de::CellAssets");
    if (assets != nullptr) {
        ecs.set(CellAssets{.assets = assets});
    }
}

namespace {

// Spawns the members of a cell from its loaded file, replacing any it had.
void spawn_members(flecs::world& ecs, const asset::AssetRegistry& assets, flecs::entity cell,
                   CellSpawnScratch& scratch) {
    ecs.delete_with(ecs.pair<InCell>(cell));
    const asset::AssetHandle handle = cell.get<Cell>().asset;
    const auto* asset = assets.get<asset::CellAsset>(handle);
    cell.set(loaded_contents(assets, handle));
    if (asset == nullptr) {
        return; // failed, or unloaded: empty
    }
    const std::span<const asset::CellEntity> entities = asset->entities.get(asset);
    const std::span<const asset::AssetHandle> meshes = assets.dependencies(handle);
    // set(), not get_mut() (Cell's replace hook): the same hold, so nothing is released.
    cell.set(Cell(handle, {.x = asset->coord_x, .z = asset->coord_z}));
    scratch.members.resize(entities.size());
    scratch.worlds.resize(entities.size());
    for (size_t i = 0; i < entities.size(); ++i) {
        const asset::CellEntity& source = entities[i];
        const Transform local{
            .position = {source.position[0], source.position[1], source.position[2]},
            .rotation = Quat(source.rotation[3], source.rotation[0], source.rotation[1], source.rotation[2]),
            .scale = {source.scale[0], source.scale[1], source.scale[2]},
        };
        // Parents come first: theirs is known (validated by the format).
        const WorldTransform world =
            source.parent != asset::CellEntity::kNone
                ? compose(scratch.worlds[source.parent], local)
                : WorldTransform{.position = local.position, .rotation = local.rotation, .scale = local.scale};
        scratch.worlds[i] = world;
        const flecs::entity member = ecs.entity().add<InCell>(cell);
        if (source.parent != asset::CellEntity::kNone) {
            member.set(flecs::Parent{scratch.members[source.parent]});
        }
        if (core::has_any(source.flags, asset::CellEntityFlags::Static)) {
            member.add<StaticTransform>();
        }
        if (source.mesh != asset::CellEntity::kNone && source.mesh < meshes.size()) {
            member.set(MeshInstance{.mesh = meshes[source.mesh]});
        }
        member.set(local).set(world).set(PreviousWorldTransform{.value = world});
        scratch.members[i] = member.id();
    }
    DE_LOG_INFO("world", "cell [%d, %d] spawned: %zu entities, %zu meshes", asset->coord_x, asset->coord_z,
                entities.size(), meshes.size());
}

} // namespace

flecs::entity load_cell(flecs::world& ecs, asset::AssetId id, platform::IoPriority priority) {
    const auto* registry = ecs.try_get<CellAssets>();
    if (registry == nullptr) {
        DE_LOG_ERROR("world", "can't load cell %s: the world has no asset registry (WorldDesc::assets)",
                     asset::format_asset_id(id).c_str());
        return {};
    }
    return ecs.entity().set(Cell(registry->assets->load<asset::CellAsset>(id, priority), {})).add<CellContents>();
}

void add_cell_systems(flecs::world& ecs, flecs::entity phase, asset::AssetRegistry& assets, CellSpawnScratch& scratch) {
    // Immediate: members are made at once, not deferred, so they're there for the tick's systems.
    // Cells whose file changed are gathered first, then spawned outside the query's iteration.
    ecs.system<const Cell, const CellContents>("de::SpawnCells")
        .kind(phase)
        .immediate()
        .run([&assets, &scratch](flecs::iter& it) {
            DE_PROFILE_ZONE("spawn cells");
            scratch.pending.clear();
            while (it.next()) {
                const auto cells = it.field<const Cell>(0);
                const auto contents = it.field<const CellContents>(1);
                for (const size_t i : it) {
                    const CellContents loaded = loaded_contents(assets, cells[i].asset);
                    if (loaded.spawned_from != contents[i].spawned_from ||
                        loaded.content_hash != contents[i].content_hash) {
                        scratch.pending.push(it.entity(i).id());
                    }
                }
            }
            flecs::world world = it.world();
            for (const flecs::entity_t cell : scratch.pending) {
                spawn_members(world, assets, world.entity(cell), scratch);
            }
        });
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
