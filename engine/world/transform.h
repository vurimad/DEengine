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

// Where an entity is in the world: computed from Transform once per tick, after every PostSim
// system (the same as Transform for a root; composed with the parent's for a child of an entity
// with a transform). Systems read it; the world writes it, and physics for the roots it moves
// (move_root).
struct WorldTransform {
    DVec3 position{0.0};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f};
};

// On entities that never move (most of what a cell places): they get tables of their own, so a
// moving neighbour with the same components doesn't make them look changed (change detection is per
// table), and they cost nothing per tick.
struct StaticTransform {};

// WorldTransform as it was at the start of the current tick, for rendering between ticks. Its own
// component, so copying marks only it, and only tables whose WorldTransform changed are copied.
struct PreviousWorldTransform {
    WorldTransform value;
};

// A child's world transform from its parent's. Scale composes per axis: a non-uniformly scaled
// parent with a rotated child skews in truth, which this doesn't represent (no shear).
WorldTransform compose(const WorldTransform& parent, const Transform& local);

// Where an entity is in the world right now, from the Transforms up its parents, so it holds for what
// was made or moved this tick, before WorldTransform catches up. A parent with no transform makes it a
// root, whichever hierarchy it's in.
WorldTransform world_of(flecs::entity entity, const Transform& local);

// A root's new pose, one row at a time, for what moves a few among many (physics). Writing Transform
// instead has the world copy the whole table, at the end of this tick and again at the start of the
// next, and has the renderer look through it every frame. This leaves the row as those copies would
// have and marks no table, so report the entity to World::moved_rows() for the renderer. In the tick
// after it stops, set its previous to its world and report it once more, or it's drawn going over its
// last move for good.
inline void move_root(Transform& transform, WorldTransform& world, PreviousWorldTransform& previous,
                      const DVec3& position, const Quat& rotation) {
    previous.value = world;
    world.position = position;
    world.rotation = rotation;
    transform.position = position;
    transform.rotation = rotation;
}

// The transform rendering shows between two ticks: alpha 0 is the previous tick, 1 the current.
// Equal transforms (most of a world: static) come back as they are, without a slerp.
WorldTransform interpolate(const WorldTransform& previous, const WorldTransform& current, float alpha);

// Where the same motion had it at alpha, which may lie before the previous tick (below 0): the
// position and rotation carry on backward; the scale stays the previous tick's (extrapolated, a
// growing thing would pass through zero and mirror). For motion vectors.
WorldTransform pose_at(const WorldTransform& previous, const WorldTransform& current, float alpha);

// Whether two transforms are equal: what doesn't move needs no second look.
bool same_transform(const WorldTransform& a, const WorldTransform& b);

// A float 3x4 matrix (rows: x, y, z; translation in the last column) of a world transform,
// relative to origin: where doubles become floats, for the GPU.
void to_matrix_3x4(const WorldTransform& transform, const DVec3& origin, float (&out)[12]);

// Puts an entity where it should be at once: sets its Transform, and its world and previous world
// transforms and those of everything attached to it, so nothing slides in from where it was (or
// from the origin). For spawning and teleports.
void place(flecs::entity entity, const Transform& transform);

// Takes an entity off its parent, where it is: its Transform becomes its world transform (a dropped
// sword stays where it fell, instead of reading its offset as a world position).
void detach(flecs::entity entity);

// Puts an entity on a parent (flecs::Parent), where it is: its Transform becomes the local
// transform that composes back to its world transform.
void attach(flecs::entity entity, flecs::entity parent);

// Registers the components with reflection (inspectable, serializable): Transform brings
// WorldTransform and PreviousWorldTransform with it. World::create calls it, and adds the
// systems that keep PreviousWorldTransform (first in Input) and compute WorldTransform (in a phase
// of its own after PostSim).
void register_transform(flecs::world& ecs);

// The systems above, in the phases World::create gives them.
void add_transform_systems(flecs::world& ecs, flecs::entity first_phase, flecs::entity transform_phase);

} // namespace de::world
