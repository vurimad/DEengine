#include "world/transform.h"

#include "core/profile.h"

#include <glm/gtc/quaternion.hpp>

#include <cstring>

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

void place(flecs::entity entity, const Transform& transform) {
    entity.set(transform);
    const flecs::entity parent = entity.parent();
    WorldTransform world = as_world(transform);
    if (parent.is_valid()) {
        const auto* parent_world = parent.try_get<WorldTransform>();
        if (parent_world == nullptr) {
            return; // computed at the end of the next tick
        }
        world = compose(*parent_world, transform);
    }
    entity.set(world);
    entity.set(PreviousWorldTransform{.value = world});
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
    // Every entity with a Transform has the two computed ones.
    ecs.component<Transform>("de::Transform")
        .member<DVec3>("position")
        .member<Quat>("rotation")
        .member<Vec3>("scale")
        .add(flecs::With, ecs.component<WorldTransform>())
        .add(flecs::With, ecs.component<PreviousWorldTransform>());
}

void add_transform_systems(flecs::world& ecs, flecs::entity first_phase, flecs::entity last_sim_phase) {
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

    // Last of every tick: world transforms. Entities without a parent (almost all: everything
    // placed in cells) copy Transform, only in tables where it changed. Children compose their
    // parent's, parents first (cascade), every tick: they are few (attachments, prefab parts).
    ecs.system<const Transform, WorldTransform>("de::RootWorldTransforms")
        .kind(last_sim_phase)
        .without<flecs::Parent>()
        .term_at(1)
        .out()
        .detect_changes()
        .run([](flecs::iter& it) {
            DE_PROFILE_ZONE("root world transforms");
            while (it.next()) {
                if (!it.changed()) {
                    it.skip();
                    continue;
                }
                const auto local = it.field<const Transform>(0);
                const auto world = it.field<WorldTransform>(1);
                for (const size_t i : it) {
                    world[i] = as_world(local[i]);
                }
            }
        });
    ecs.system<const Transform, WorldTransform, const WorldTransform>("de::ChildWorldTransforms")
        .kind(last_sim_phase)
        .term_at(1)
        .out()
        .term_at(2)
        .parent()
        .cascade()
        .run([](flecs::iter& it) {
            DE_PROFILE_ZONE("child world transforms");
            while (it.next()) {
                const auto local = it.field<const Transform>(0);
                const auto world = it.field<WorldTransform>(1);
                const auto parent = it.field<const WorldTransform>(2); // one value: shared by the rows
                for (const size_t i : it) {
                    world[i] = compose(parent[0], local[i]);
                }
            }
        });
}

} // namespace de::world
