#include "world/cell.h"

#include "core/log.h"
#include "core/profile.h"
#include "world/render_extract.h"

#include <cmath>

namespace de::world {

namespace {

// A singleton: the registry cells load through, the one their hooks release through.
struct CellAssets {
    asset::AssetRegistry* assets = nullptr;
};

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
    ecs.component<MeshCollider>("de::MeshCollider").member<asset::AssetHandle>("collision");
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
    cell.set(CellContents{.asset = handle, .revision = assets.revision(handle)});
    const auto* asset = assets.get<asset::CellAsset>(handle);
    if (asset == nullptr) {
        return; // failed, or unloaded: empty
    }
    // The meshes and collision by the header's dependencies, which the cell holds (a dependency the
    // registry skipped, closing a cycle in a corrupt file, is an invalid handle: drawn as a placeholder,
    // and no body).
    const std::span<const asset::AssetRef> dependencies = asset->header.dependencies.get(asset);
    scratch.meshes.resize(dependencies.size());
    for (size_t i = 0; i < dependencies.size(); ++i) {
        scratch.meshes[i] = assets.find(dependencies[i]);
    }
    const std::span<const asset::CellEntity> entities = asset->entities.get(asset);
    // set(), not get_mut() (Cell's replace hook): the same hold, so nothing is released.
    cell.set(Cell(handle, {.x = asset->coord_x, .z = asset->coord_z}));
    scratch.members.resize(entities.size());
    scratch.worlds.resize(entities.size());
    // Deferred, so each member's components land in one table move.
    ecs.defer_begin();
    for (size_t i = 0; i < entities.size(); ++i) {
        const asset::CellEntity& source = entities[i];
        const Transform local{
            .position = {source.position[0], source.position[1], source.position[2]},
            .rotation = Quat(source.rotation[3], source.rotation[0], source.rotation[1], source.rotation[2]),
            .scale = {source.scale[0], source.scale[1], source.scale[2]},
        };
        // Parents come first (the format checks it): their world transforms are known.
        const bool has_parent = source.parent != asset::CellEntity::kNone;
        const WorldTransform world =
            has_parent ? compose(scratch.worlds[source.parent], local)
                       : WorldTransform{.position = local.position, .rotation = local.rotation, .scale = local.scale};
        scratch.worlds[i] = world;
        const bool is_static = core::has_any(source.flags, asset::CellEntityFlags::Static);
        const bool attached = has_parent && !is_static; // static ones become roots where they are
        const flecs::entity member = ecs.entity().add<InCell>(cell);
        if (attached) {
            member.set(flecs::Parent{scratch.members[source.parent]});
        }
        if (is_static) {
            member.add<StaticTransform>();
        }
        if (source.mesh != asset::CellEntity::kNone) {
            member.set(MeshInstance{.mesh = scratch.meshes[source.mesh]});
        }
        if (source.collision != asset::CellEntity::kNone) {
            member.set(MeshCollider{.collision = scratch.meshes[source.collision]});
        }
        if (source.fog_volume != asset::CellEntity::kNone) {
            const asset::CellFogVolume& fog = asset->fog_volumes.get(asset)[source.fog_volume];
            member.set(FogVolume{.density_per_m = fog.density_per_m,
                                 .edge_m = fog.edge_m,
                                 .tint = {fog.tint[0], fog.tint[1], fog.tint[2]},
                                 .ellipsoid = fog.shape == asset::CellFogShape::Ellipsoid,
                                 .noise_size_m = fog.noise_size_m,
                                 .noise_amount = fog.noise_amount,
                                 .drift_mps = {fog.drift_mps[0], fog.drift_mps[1], fog.drift_mps[2]},
                                 .height_fade_m = fog.height_fade_m});
        }
        const Transform placed =
            attached ? local : Transform{.position = world.position, .rotation = world.rotation, .scale = world.scale};
        member.set(placed).set(world).set(PreviousWorldTransform{.value = world});
        scratch.members[i] = member.id();
    }
    ecs.defer_end();
    DE_LOG_INFO("world", "cell [%d, %d] spawned: %zu entities, %zu assets", asset->coord_x, asset->coord_z,
                entities.size(), dependencies.size());
}

// Spawns every cell whose members are behind its loaded file. Immediate: members are made at once,
// not deferred, for the systems after it. Cells are gathered first, then spawned outside the
// query's iteration.
void spawn_cells(flecs::iter& it, const asset::AssetRegistry& assets, CellSpawnScratch& scratch) {
    DE_PROFILE_ZONE("spawn cells");
    scratch.pending.clear();
    while (it.next()) {
        const auto cells = it.field<const Cell>(0);
        const auto contents = it.field<const CellContents>(1);
        for (const size_t i : it) {
            if (!(cells[i].asset == contents[i].asset) || assets.revision(cells[i].asset) != contents[i].revision) {
                scratch.pending.push(it.entity(i).id());
            }
        }
    }
    flecs::world world = it.world();
    for (const flecs::entity_t cell : scratch.pending) {
        spawn_members(world, assets, world.entity(cell), scratch);
    }
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

void add_cell_systems(flecs::world& ecs, flecs::entity tick_phase, flecs::entity frame_phase,
                      asset::AssetRegistry& assets, CellSpawnScratch& scratch) {
    // Twice: at the start of every tick, and before every frame's extraction (a frame may run no
    // tick, and must not extract members whose meshes a reload let go).
    ecs.system<const Cell, const CellContents>("de::SpawnCells")
        .kind(tick_phase)
        .immediate()
        .run([&assets, &scratch](flecs::iter& it) { spawn_cells(it, assets, scratch); });
    ecs.system<const Cell, const CellContents>("de::SpawnCellsBeforeExtract")
        .kind(frame_phase)
        .immediate()
        .run([&assets, &scratch](flecs::iter& it) { spawn_cells(it, assets, scratch); });
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
