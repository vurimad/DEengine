#include "world/transform.h"

#include "core/profile.h"

#include <glm/gtc/quaternion.hpp>

namespace de::world {

namespace {

bool same(const WorldTransform& a, const WorldTransform& b) {
    return a.position == b.position && a.rotation == b.rotation && a.scale == b.scale;
}

WorldTransform as_world(const Transform& transform) {
    return {.position = transform.position, .rotation = transform.rotation, .scale = transform.scale};
}

} // namespace

WorldTransform compose(const WorldTransform& parent, const Transform& local) {
    // A child's offset is small (an attachment), so float rotation and scale lose nothing; the
    // parent's double position carries the precision.
    const DVec3 offset = DVec3(parent.rotation * (parent.scale * Vec3(local.position)));
    return {
        .position = parent.position + offset,
        .rotation = glm::normalize(parent.rotation * local.rotation),
        .scale = parent.scale * local.scale,
    };
}

WorldTransform interpolate(const WorldTransform& previous, const WorldTransform& current, float alpha) {
    if (same(previous, current)) {
        return current;
    }
    return {
        .position = glm::mix(previous.position, current.position, static_cast<double>(alpha)),
        .rotation = glm::slerp(previous.rotation, current.rotation, alpha),
        .scale = glm::mix(previous.scale, current.scale, alpha),
    };
}

void to_matrix_3x4(const WorldTransform& transform, const DVec3& origin, float (&out)[12]) {
    const Mat3 rotation = glm::mat3_cast(transform.rotation);
    const Vec3 position = Vec3(transform.position - origin);
    for (int row = 0; row < 3; ++row) {
        out[row * 4 + 0] = rotation[0][row] * transform.scale.x;
        out[row * 4 + 1] = rotation[1][row] * transform.scale.y;
        out[row * 4 + 2] = rotation[2][row] * transform.scale.z;
        out[row * 4 + 3] = position[row];
    }
}

namespace {

// The world transform of an entity whose local one is `local`: composed with its parent's, if its
// parent has one (otherwise it's a root: a scope or a grouping entity isn't a transform parent).
WorldTransform world_of(flecs::entity entity, const Transform& local) {
    const flecs::entity parent = entity.parent();
    const auto* parent_world = parent.is_valid() ? parent.try_get<WorldTransform>() : nullptr;
    return parent_world != nullptr ? compose(*parent_world, local) : as_world(local);
}

// Sets an entity's world and previous world transforms, then those of everything attached to it.
void snap(flecs::entity entity, const WorldTransform& world) {
    entity.set(world);
    entity.set(PreviousWorldTransform{.value = world});
    entity.children([&world](flecs::entity child) {
        const auto* local = child.try_get<Transform>();
        if (local != nullptr) {
            snap(child, compose(world, *local));
        }
    });
}

} // namespace

void place(flecs::entity entity, const Transform& transform) {
    entity.set(transform);
    snap(entity, world_of(entity, transform));
}

void detach(flecs::entity entity) {
    const auto* world = entity.try_get<WorldTransform>();
    const Transform in_world =
        world != nullptr ? Transform{.position = world->position, .rotation = world->rotation, .scale = world->scale}
                         : Transform{};
    entity.remove<flecs::Parent>();
    entity.remove(flecs::ChildOf, flecs::Wildcard);
    entity.set(in_world);
}

void attach(flecs::entity entity, flecs::entity parent) {
    // Copied first: the removes and sets below move the entity to another table, and a pointer into
    // its old row would then read whichever entity took that row's place.
    const auto* world_ptr = entity.try_get<WorldTransform>();
    const auto* parent_ptr = parent.try_get<WorldTransform>();
    const bool placed = world_ptr != nullptr && parent_ptr != nullptr;
    const WorldTransform world_value = world_ptr != nullptr ? *world_ptr : WorldTransform{};
    const WorldTransform parent_value = parent_ptr != nullptr ? *parent_ptr : WorldTransform{};
    entity.remove(flecs::ChildOf, flecs::Wildcard);
    entity.set(flecs::Parent{parent});
    if (!placed) {
        return;
    }
    const WorldTransform* world = &world_value;
    const WorldTransform* parent_world = &parent_value;
    // The local transform that composes back to where the entity is now.
    const Quat inverse = glm::inverse(parent_world->rotation);
    entity.set(Transform{
        .position = DVec3(inverse * Vec3(world->position - parent_world->position) / parent_world->scale),
        .rotation = glm::normalize(inverse * world->rotation),
        .scale = world->scale / parent_world->scale,
    });
}

void register_transform(flecs::world& ecs) {
    // Reflection: the math types by member, then the components built from them.
    ecs.component<Vec3>("de::Vec3").member<float>("x").member<float>("y").member<float>("z");
    ecs.component<DVec3>("de::DVec3").member<double>("x").member<double>("y").member<double>("z");
    ecs.component<Quat>("de::Quat").member<float>("x").member<float>("y").member<float>("z").member<float>("w");
    ecs.component<WorldTransform>("de::WorldTransform")
        .member<DVec3>("position")
        .member<Quat>("rotation")
        .member<Vec3>("scale");
    ecs.component<PreviousWorldTransform>("de::PreviousWorldTransform").member<WorldTransform>("value");
    ecs.component<StaticTransform>("de::StaticTransform");
    // Every entity with a Transform has the two computed ones.
    ecs.component<Transform>("de::Transform")
        .member<DVec3>("position")
        .member<Quat>("rotation")
        .member<Vec3>("scale")
        .add(flecs::With, ecs.component<WorldTransform>())
        .add(flecs::With, ecs.component<PreviousWorldTransform>());
}

void add_transform_systems(flecs::world& ecs, flecs::entity first_phase, flecs::entity transform_phase) {
    // First of every tick: PreviousWorldTransform catches up with WorldTransform, for the tables
    // whose WorldTransform changed since (static ones cost nothing).
    ecs.system<const WorldTransform, PreviousWorldTransform>("de::KeepPreviousWorldTransform")
        .kind(first_phase)
        .term_at(1)
        .out() // written only: changes to it alone don't make a table "changed"
        .detect_changes()
        .run([](flecs::iter& it) {
            DE_PROFILE_ZONE("keep previous world transforms");
            while (it.next()) {
                if (!it.changed()) {
                    it.skip();
                    continue;
                }
                const auto current = it.field<const WorldTransform>(0);
                const auto kept = it.field<PreviousWorldTransform>(1);
                for (const size_t i : it) {
                    kept[i].value = current[i];
                }
            }
        });

    // Last of every tick (a phase of its own, after every PostSim system): world transforms, down
    // every hierarchy, parents first (cascade). An entity whose parent has a WorldTransform composes
    // with it, every tick (children are few: attachments, prefab parts); any other is a root, and
    // copies its Transform only in tables where it changed. A scope or grouping parent without a
    // transform makes a root, whichever hierarchy (Parent or ChildOf) it's in.
    ecs.system<const Transform, WorldTransform, const WorldTransform>("de::WorldTransforms")
        .kind(transform_phase)
        .term_at(1)
        .out()
        .term_at(2)
        .parent()
        .cascade()
        .optional()
        .detect_changes()
        .run([](flecs::iter& it) {
            DE_PROFILE_ZONE("world transforms");
            while (it.next()) {
                const bool has_parent = it.is_set(2);
                // Entities with a flecs::Parent come one row at a time, and change detection is per
                // table: after the table's first row, it.changed() says nothing changed. They're
                // attachments (few): always computed.
                const bool per_row = it.table().has<flecs::Parent>();
                if (!has_parent && !per_row && !it.changed()) {
                    it.skip();
                    continue;
                }
                const auto local = it.field<const Transform>(0);
                const auto world = it.field<WorldTransform>(1);
                if (has_parent) {
                    const WorldTransform parent = it.field<const WorldTransform>(2)[0]; // shared by the rows
                    for (const size_t i : it) {
                        world[i] = compose(parent, local[i]);
                    }
                } else {
                    for (const size_t i : it) {
                        world[i] = as_world(local[i]);
                    }
                }
            }
        });
}

} // namespace de::world
