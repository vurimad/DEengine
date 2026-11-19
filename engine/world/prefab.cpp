#include "world/prefab.h"

#include "core/log.h"
#include "core/profile.h"
#include "world/cell.h"
#include "world/render_extract.h"

namespace de::world {

namespace {

// A part's place from its prefab's origin.
Transform local_of_part(const asset::PrefabPart& part) {
    return {.position = DVec3(part.position[0], part.position[1], part.position[2]),
            .rotation = Quat(part.rotation[3], part.rotation[0], part.rotation[1], part.rotation[2]),
            .scale = Vec3(part.scale[0], part.scale[1], part.scale[2])};
}

// Makes (or makes again) the parts of each placement that waits for them and whose prefab is loaded, and puts every
// placement of a prefab that was cooked again back among those waiting.
void spawn_pending(flecs::iter& it, const asset::AssetRegistry& assets, PrefabScratch& scratch) {
    DE_PROFILE_ZONE("spawn prefab parts");
    scratch.pending.clear();
    while (it.next()) {
        const auto instances = it.field<const PrefabInstance>(0);
        for (const size_t i : it) {
            if (assets.revision(instances[i].prefab) != 0) {
                scratch.pending.push(it.entity(i).id());
            }
        }
    }
    flecs::world world = it.world();
    for (const flecs::entity_t id : scratch.pending) {
        const flecs::entity placement = world.entity(id);
        PrefabInstance instance = placement.get<PrefabInstance>();
        const auto* where = placement.try_get<WorldTransform>();
        if (where == nullptr) {
            continue; // nowhere yet: it waits
        }
        delete_prefab_parts(world, instance.first_part);
        instance.revision = assets.revision(instance.prefab);
        instance.placed = *where;
        instance.first_part = make_prefab_parts(world, assets, instance.prefab, instance.variant, id, *where,
                                                placement.has<StaticTransform>());
        placement.set(instance).remove<PrefabPending>();
    }
    // A prefab cooked again: every placement made from what it was. Looked for only when the registry has swapped
    // something in.
    const uint32_t reloads = assets.stats().reloads;
    if (reloads != scratch.reloads_seen) {
        scratch.reloads_seen = reloads;
        scratch.pending.clear();
        world.each([&](flecs::entity placement, const PrefabInstance& instance) {
            const uint32_t now = assets.revision(instance.prefab);
            if (now != 0 && now != instance.revision) {
                scratch.pending.push(placement.id());
            }
        });
        for (const flecs::entity_t id : scratch.pending) {
            world.entity(id).add<PrefabPending>();
        }
    }
}

} // namespace

void register_prefabs(flecs::world& ecs) {
    ecs.component<PrefabInstance>("de::PrefabInstance")
        .member<asset::AssetHandle>("prefab")
        .member<uint32_t>("variant")
        .member<uint32_t>("revision");
    ecs.component<PrefabPart>("de::PrefabPart").member<flecs::entity_t>("placement").member<uint32_t>("index");
    ecs.component<PrefabPending>("de::PrefabPending");
    // A placement's parts go with it. Not while the world itself is going: everything does then.
    ecs.observer<PrefabInstance>("de::DropPrefabParts")
        .event(flecs::OnRemove)
        .each([](flecs::entity placement, PrefabInstance& instance) {
            flecs::world world = placement.world();
            if (!ecs_is_fini(world.c_ptr())) {
                delete_prefab_parts(world, instance.first_part);
            }
            instance.first_part = 0;
        });
}

flecs::entity_t make_prefab_parts(flecs::world& ecs, const asset::AssetRegistry& assets, asset::AssetHandle prefab,
                                  uint32_t variant, flecs::entity_t placement, const WorldTransform& world,
                                  bool is_static) {
    const auto* asset = assets.get<asset::PrefabAsset>(prefab);
    if (asset == nullptr) {
        return 0;
    }
    const std::span<const asset::AssetRef> dependencies = asset->header.dependencies.get(asset);
    const std::span<const asset::PrefabPart> parts = asset->parts.get(asset);
    const std::span<const asset::LightDesc> lights = asset->lights.get(asset);
    const uint32_t left_out = asset->variant_bit(asset, variant);
    if (variant != 0 && left_out == 0) {
        DE_LOG_WARN("world", "prefab %s has no variant the placement asks for: all of it is placed",
                    asset::format_asset_id(asset->header.id).c_str());
    }
    const auto handle_of = [&](uint32_t dependency) { return assets.find(dependencies[dependency]); };
    flecs::entity_t first = 0;
    // Last to first, so the list each part leads on through comes out in the prefab's order.
    for (size_t p = parts.size(); p-- > 0;) {
        const asset::PrefabPart& source = parts[p];
        if ((source.absent_in & left_out) != 0) {
            continue;
        }
        const Transform local = local_of_part(source);
        const WorldTransform part_world = compose(world, local);
        const flecs::entity part =
            ecs.entity().set(PrefabPart{.placement = placement, .next = first, .index = static_cast<uint32_t>(p)});
        if (is_static) {
            part.add<StaticTransform>();
        } else {
            part.set(flecs::Parent{placement});
        }
        if (source.mesh != asset::PrefabPart::kNone) {
            part.set(MeshInstance{.mesh = handle_of(source.mesh)});
        }
        if (source.collision != asset::PrefabPart::kNone) {
            part.set(MeshCollider{.collision = handle_of(source.collision)});
        }
        if (source.effect != asset::PrefabPart::kNone) {
            part.set(EffectInstance{.effect = handle_of(source.effect)});
        }
        if (source.light != asset::PrefabPart::kNone) {
            part.set(LocalLight::from(lights[source.light]));
        }
        // Where it stands, with its previous transform the same: nothing slides in.
        const Transform placed =
            is_static
                ? Transform{.position = part_world.position, .rotation = part_world.rotation, .scale = part_world.scale}
                : local;
        part.set(placed).set(part_world).set(PreviousWorldTransform{.value = part_world});
        first = part.id();
    }
    return first;
}

void delete_prefab_parts(flecs::world& ecs, flecs::entity_t first_part) {
    flecs::entity_t at = first_part;
    while (at != 0 && ecs.is_alive(at)) {
        const flecs::entity part = ecs.entity(at);
        const auto* link = part.try_get<PrefabPart>();
        at = link != nullptr ? link->next : 0;
        part.destruct();
    }
}

flecs::entity placement_of(flecs::entity entity) {
    if (const auto* part = entity.try_get<PrefabPart>(); part != nullptr && entity.world().is_alive(part->placement)) {
        return entity.world().entity(part->placement);
    }
    return entity;
}

void add_prefab_systems(flecs::world& ecs, flecs::entity tick_phase, flecs::entity frame_phase,
                        asset::AssetRegistry& assets, PrefabScratch& scratch) {
    // Twice, as cells spawn: at the start of every tick, and before every frame's extraction.
    ecs.system<const PrefabInstance>("de::SpawnPrefabParts")
        .with<PrefabPending>()
        .kind(tick_phase)
        .immediate()
        .run([&assets, &scratch](flecs::iter& it) { spawn_pending(it, assets, scratch); });
    ecs.system<const PrefabInstance>("de::SpawnPrefabPartsBeforeExtract")
        .with<PrefabPending>()
        .kind(frame_phase)
        .immediate()
        .run([&assets, &scratch](flecs::iter& it) { spawn_pending(it, assets, scratch); });
    // A placement that was put somewhere else takes its static parts along (moving ones are attached). Only tables
    // something wrote to are looked at, and in those only the rows that aren't where their parts were placed from:
    // placements that stand still, which is nearly all of them, cost nothing.
    ecs.system<const WorldTransform, const PrefabInstance>("de::FollowPrefabPlacements")
        .with<StaticTransform>()
        .kind(frame_phase)
        .detect_changes()
        .immediate()
        .run([&assets, &scratch](flecs::iter& it) {
            DE_PROFILE_ZONE("follow prefab placements");
            scratch.pending.clear();
            while (it.next()) {
                if (!it.changed()) {
                    continue;
                }
                const auto worlds = it.field<const WorldTransform>(0);
                const auto instances = it.field<const PrefabInstance>(1);
                for (const size_t i : it) {
                    if (instances[i].first_part != 0 && !same_transform(worlds[i], instances[i].placed)) {
                        scratch.pending.push(it.entity(i).id());
                    }
                }
            }
            const flecs::world world = it.world();
            for (const flecs::entity_t id : scratch.pending) {
                const flecs::entity placement = world.entity(id);
                PrefabInstance instance = placement.get<PrefabInstance>();
                const WorldTransform where = placement.get<WorldTransform>();
                const auto* asset = assets.get<asset::PrefabAsset>(instance.prefab);
                if (asset == nullptr) {
                    continue;
                }
                const std::span<const asset::PrefabPart> parts = asset->parts.get(asset);
                each_prefab_part(world, instance.first_part, [&](flecs::entity part) {
                    const uint32_t index = part.get<PrefabPart>().index;
                    if (index < parts.size()) {
                        const WorldTransform part_world = compose(where, local_of_part(parts[index]));
                        place(part, {.position = part_world.position,
                                     .rotation = part_world.rotation,
                                     .scale = part_world.scale});
                    }
                });
                instance.placed = where;
                placement.set(instance);
            }
        });
}

} // namespace de::world
