#pragma once

#include "core/math.h"

#include <flecs.h>

namespace de::world {

// Where an entity is, relative to its parent (or the world): hot, written by simulation.
struct Transform {
    Vec3 position{0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // identity (w first in the constructor)
    Vec3 scale{1.0f};
};

// Transform as it was at the start of the current tick, for rendering between ticks. Its own
// component, so the copy marks only it, and only tables whose Transform changed are copied.
struct PreviousTransform {
    Transform value;
};

// The transform rendering shows between two ticks: alpha 0 is the previous tick, 1 the current.
Transform interpolate(const Transform& previous, const Transform& current, float alpha);

// The transform as a matrix: translation * rotation * scale.
Mat4 to_matrix(const Transform& transform);

// Registers the components with reflection (inspectable, serializable). World::create calls it,
// and adds the system that keeps PreviousTransform, first in the Input phase of every tick.
void register_transform(flecs::world& ecs);

} // namespace de::world
