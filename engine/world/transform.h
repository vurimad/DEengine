#pragma once

#include "core/math.h"

#include <flecs.h>

namespace de::world {

// Where an entity is, relative to its parent (flecs::Parent), or in the world for an entity
// without one. Hot, written by simulation. Positions are doubles: one fixed world space, never
// rebased (docs/design.md, Large-world precision).
struct Transform {
    DVec3 position{0.0};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // identity (w first in the constructor)
    Vec3 scale{1.0f};
};

// Where an entity is in the world: computed from Transform once per tick, at the end of PostSim
// (the same as Transform for an entity without a parent; composed down the hierarchy otherwise).
// Systems read it; only the world writes it.
struct WorldTransform {
    DVec3 position{0.0};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f};
};

// WorldTransform as it was at the start of the current tick, for rendering between ticks. Its own
// component, so copying marks only it, and only tables whose WorldTransform changed are copied.
struct PreviousWorldTransform {
    WorldTransform value;
};

// A child's world transform from its parent's. Scale composes per axis: a non-uniformly scaled
// parent with a rotated child skews in truth, which this doesn't represent (no shear).
WorldTransform compose(const WorldTransform& parent, const Transform& local);

// The transform rendering shows between two ticks: alpha 0 is the previous tick, 1 the current.
// Equal transforms (most of a world: static) come back as they are, without a slerp.
WorldTransform interpolate(const WorldTransform& previous, const WorldTransform& current, float alpha);

// A float 3x4 matrix (rows: x, y, z; translation in the last column) of a world transform,
// relative to origin: where doubles become floats, for the GPU.
void to_matrix_3x4(const WorldTransform& transform, const DVec3& origin, float (&out)[12]);

// Puts an entity where it should be at once: sets its Transform and, for an entity without a
// parent (or whose parent has a WorldTransform), its WorldTransform and PreviousWorldTransform, so
// it is right before the next tick and doesn't slide in from the origin. For spawning, teleports.
void place(flecs::entity entity, const Transform& transform);

// Registers the components with reflection (inspectable, serializable): Transform brings
// WorldTransform and PreviousWorldTransform with it. World::create calls it, and adds the
// systems that keep PreviousWorldTransform (first in Input) and compute WorldTransform (last in
// PostSim).
void register_transform(flecs::world& ecs);

// The systems above, in the phases World::create gives them.
void add_transform_systems(flecs::world& ecs, flecs::entity first_phase, flecs::entity last_sim_phase);

} // namespace de::world
