#pragma once

#include "asset/prefab_format.h"
#include "asset/registry.h"
#include "core/array.h"
#include "world/transform.h"

#include <flecs.h>

#include <cstdint>

namespace de::world {

// Prefabs in the world (docs/design.md, Prefabs). A placement is one entity that names a prefab; each part the
// prefab's variant keeps is an entity of its own beside it, made from the cooked prefab's flat list. Parts aren't
// children: a placement that doesn't move has parts that are roots where they stand, which cost nothing per tick. A
// placement that moves carries its parts with flecs::Parent, as anything attached is.

// On an entity that places a prefab. Whatever sets it holds the prefab (a cell holds what its entities name).
struct PrefabInstance {
    asset::AssetHandle prefab;      // an asset::PrefabAsset
    uint32_t variant = 0;           // asset::prefab_variant_hash of the variant it asks for; 0: the prefab as it is
    uint32_t revision = 0;          // of the prefab its parts were made from (AssetRegistry::revision); 0: none yet
    flecs::entity_t first_part = 0; // its parts, each leading to the next (PrefabPart::next); 0: none
    WorldTransform placed{};        // where its parts were last placed from: they're placed again when it's elsewhere
};

// On a part of a placed prefab: whose it is, the next part of the same placement, and which of the prefab's parts
// (PrefabAsset::parts) it was made from. A part goes with its placement.
struct PrefabPart {
    flecs::entity_t placement = 0;
    flecs::entity_t next = 0;
    uint32_t index = 0;
};

// On a placement whose parts are still to be made, or to be made again: set with a PrefabInstance by whoever places
// a prefab outside a cell (the editor, gameplay), and by the world when the prefab is cooked again.
struct PrefabPending {};

// Registers the components, and the observer that deletes a placement's parts when it loses its PrefabInstance or
// goes. World::create calls it.
void register_prefabs(flecs::world& ecs);

// Makes the entities for a placement's parts now, from a loaded prefab: each with its mesh, collision and effect
// (handles the prefab holds), placed from `world`; static ones as roots, moving ones attached to the placement. The
// placement's variant leaves out what it leaves out. Returns the first part, each leading to the next; 0 if the
// prefab isn't loaded or keeps no part. Safe in deferred mode (a cell's spawn).
flecs::entity_t make_prefab_parts(flecs::world& ecs, const asset::AssetRegistry& assets, asset::AssetHandle prefab,
                                  uint32_t variant, flecs::entity_t placement, const WorldTransform& world,
                                  bool is_static);

// Deletes a placement's parts (first_part and on).
void delete_prefab_parts(flecs::world& ecs, flecs::entity_t first_part);

// The placement an entity belongs to: a part's placement, or the entity itself.
flecs::entity placement_of(flecs::entity entity);

// Calls each(part entity) for every part of a placement.
template <typename Each>
void each_prefab_part(const flecs::world& ecs, flecs::entity_t first_part, Each&& each) {
    flecs::entity_t at = first_part;
    while (at != 0 && ecs.is_alive(at)) {
        const flecs::entity part = ecs.entity(at);
        const auto* link = part.try_get<PrefabPart>();
        const flecs::entity_t next = link != nullptr ? link->next : 0;
        each(part);
        at = next;
    }
}

// The systems' working memory, kept between uses.
struct PrefabScratch {
    core::Array<flecs::entity_t> pending{core::heap_allocator(core::MemoryTag::World)};
    uint32_t reloads_seen = 0; // AssetRegistryStats::reloads when placements were last checked against their prefabs
};

// Adds the systems: in tick_phase and in frame_phase (before render extraction, as cells spawn), parts are made for
// placements that wait for them (PrefabPending) once their prefab is loaded, and made again for every placement of a
// prefab that was cooked again; in frame_phase, the parts of a placement that was put somewhere else (the editor
// moving it) are put where it is. A placement nothing touches costs nothing. scratch must outlive the world.
void add_prefab_systems(flecs::world& ecs, flecs::entity tick_phase, flecs::entity frame_phase,
                        asset::AssetRegistry& assets, PrefabScratch& scratch);

} // namespace de::world
