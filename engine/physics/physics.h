#pragma once

#include "core/math.h"

#include <flecs.h>

#include <cstdint>

namespace de::core {
class JobSystem;
}

namespace de::world {
class World;
}

namespace de::physics {

enum class ShapeKind : uint8_t {
    Box,
    Sphere,
    Capsule, // along its Y axis
};

// What a body is shaped like, in meters, before the entity's scale.
struct Collider {
    ShapeKind shape = ShapeKind::Box;
    Vec3 half_extents_m{0.5f};  // Box
    float radius_m = 0.5f;      // Sphere, Capsule
    float half_height_m = 0.5f; // Capsule: of the cylinder between its two half spheres
};

enum class BodyKind : uint8_t {
    Static,    // never moves: where it was when it got its body. To move one, take its body away and give it another
    Kinematic, // goes where its Transform says, pushing what's in the way; nothing pushes it. A root entity
    Dynamic,   // gravity and contacts move it, and it writes its Transform. A root entity
};

// An entity with this and a Collider gets a body at the start of the next tick, from where its Transform says, and
// loses it when either goes. Changing either later doesn't change the body. Bodies of the same shape and size share
// one, so many of a kind cost little.
struct RigidBody {
    BodyKind kind = BodyKind::Static;
    float mass_kg = 0.0f; // Dynamic. 0: from the shape's volume, at 1000 kg per cubic meter
    float friction = 0.5f;
    float restitution = 0.0f;
    float linear_damping = 0.05f;
    float angular_damping = 0.05f;
    // A dynamic body that hits only the world, for corpses and rubble: far cheaper in numbers, as they skip each other.
    // Characters and other moving bodies pass through it, and queries don't see it unless they ask.
    bool debris = false;
    // A dynamic body that waits to be touched, for what a streamed cell brings in already settled.
    bool start_asleep = false;
};

// A capsule that walks, for the player and enemies (docs/design.md, Physics): not a body, and rays and shape casts
// don't hit it. Its entity's position is at its feet. A root entity, and place() it to teleport it. Combat doesn't use
// this shape: it has its own swept capsules.
struct CharacterController {
    // Set when it's made.
    float radius_m = 0.35f;
    float height_m = 1.8f;       // from feet to the top of the capsule, at least twice the radius
    float max_slope_rad = 0.87f; // about 50 degrees: steeper is a wall to walk up, and it slides down it
    float step_height_m = 0.4f;  // walks up what's no taller than this
    float mass_kg = 70.0f;       // how hard it pushes what it walks into
    // Set any tick. Across the ground: gravity and the ground look after the rest. Root motion goes here.
    Vec3 desired_velocity_mps{0.0f};
    // What happened, last tick.
    Vec3 velocity_mps{0.0f};
    Vec3 ground_normal{0.0f, 1.0f, 0.0f};
    bool grounded = false; // on something it can stand on
};

struct PhysicsDesc {
    // Null, or a system with no workers: one thread. The results are the same bit for bit either way.
    core::JobSystem* jobs = nullptr;
    uint32_t max_bodies = 256U * 1024U;
    uint32_t max_body_pairs = 256U * 1024U;
    uint32_t max_contact_constraints = 64U * 1024U;
    // What a step and a character's update work in, made once. Past it they take from the heap rather than fail.
    uint32_t temp_memory_bytes = 32U * 1024U * 1024U;
    Vec3 gravity_mps2{0.0f, -9.81f, 0.0f};
};

struct PhysicsStats {
    uint32_t bodies = 0;
    uint32_t active_bodies = 0; // not asleep, and not static
    uint32_t characters = 0;
    uint32_t shapes = 0; // kept, for all the bodies that share them
    // Last tick: dynamic bodies whose pose reached their Transform, and rows looked at for a place().
    uint32_t transforms_written = 0;
    uint32_t transforms_compared = 0;
};

// What's between a query and what it hit.
struct QueryFilter {
    flecs::entity_t ignore = 0; // don't hit this entity's body
    bool debris = false;        // hit what RigidBody::debris is set on, too
};

struct QueryHit {
    flecs::entity_t entity = 0;
    DVec3 position{0.0}; // where it hit
    Vec3 normal{0.0f, 1.0f, 0.0f};
    float distance_m = 0.0f;
};

// Jolt, on the engine's memory and jobs, in world space in doubles. It steps in the world's Physics phase, at the
// world's fixed tick: bodies and characters come and go with their components, what's made in a tick is there for that
// tick's step, and queries see the last step. Jolt's own types stay inside this library.
//
// Static bodies cost nothing per tick, and neither does anything asleep: a step's cost follows what's awake. Tables are
// the unit of change, though: one body awake makes its table's other rows look changed to what reads transforms, and a
// place() has every row of its table looked at once, so what moves shouldn't share a table with thousands that don't.
//
// Create this before the world and add it once the world's there: the world's last entities let go of their bodies as
// it goes down, so this has to outlive it. Not thread-safe, and queries aren't for use during a step.
class Physics {
public:
    explicit Physics(const PhysicsDesc& desc = {});
    ~Physics();
    Physics(const Physics&) = delete;
    Physics& operator=(const Physics&) = delete;
    Physics(Physics&&) = delete;
    Physics& operator=(Physics&&) = delete;

    void add_to_world(world::World& world);

    // The nearest thing along direction (normalized), out to distance_m.
    bool ray_cast(const DVec3& origin, const Vec3& direction, float distance_m, QueryHit& hit,
                  const QueryFilter& filter = {}) const;

    // The same for a sphere moved along it: the first thing it touches. distance_m is how far its center gets.
    bool sphere_cast(const DVec3& origin, float radius_m, const Vec3& direction, float distance_m, QueryHit& hit,
                     const QueryFilter& filter = {}) const;

    PhysicsStats stats() const;

private:
    struct State;
    State* state_ = nullptr;
};

} // namespace de::physics
