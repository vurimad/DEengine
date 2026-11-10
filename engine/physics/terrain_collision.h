#pragma once

// Private to de_physics: Jolt's types are in it.

#include "asset/terrain_format.h"
#include "core/allocator.h"
#include "core/array.h"
#include "core/hash_map.h"
#include "core/math.h"
#include "physics/physics.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>

#include <cstdint>

namespace de::core {
class JobSystem;
}

namespace de::platform {
class Vfs;
}

namespace JPH {
class PhysicsSystem;
}

namespace de::physics {

struct TerrainTile;

// The terrain's collision (docs/design.md, Terrain & vegetation: what needs the ground every frame reads it the
// streamed way): a Jolt height field per node of the finest level (128 squares a side, 256 m at 2 m spacing), the same
// heights the renderer draws at its finest, read a block at a time from the heights file and built on the job system,
// only where something needs the ground: around anchors each tick (the camera, characters, what moves).
//
// A tile within kKeepM of an anchor is read and built in the background, and added as a static body once it's built.
// One within kNeedM of an anchor that asks for it (what stands or falls there) is waited for, so nothing falls through
// ground that's still loading. A tile no anchor has wanted for kForgetTicks goes.
class TerrainColliders {
public:
    static constexpr double kKeepM = 320.0;
    static constexpr double kNeedM = 24.0;
    static constexpr uint64_t kForgetTicks = kTerrainForgetTicks;

    TerrainColliders(core::JobSystem* jobs, core::Allocator& allocator);
    ~TerrainColliders(); // waits for reads in flight; take the bodies away first (remove_all)
    TerrainColliders(const TerrainColliders&) = delete;
    TerrainColliders& operator=(const TerrainColliders&) = delete;
    TerrainColliders(TerrainColliders&&) = delete;
    TerrainColliders& operator=(TerrainColliders&&) = delete;

    // The terrain (the registry's bytes, valid while it's set) and where its heights are read from; null for none.
    // Another one, or the same cooked again, takes every tile away.
    void set_terrain(const asset::TerrainAsset* terrain, platform::Vfs* vfs, JPH::PhysicsSystem& system);
    bool has_terrain() const { return terrain_ != nullptr; }

    // This tick: the ground is wanted around anchor; need: waited for close to it.
    void want(const DVec3& anchor, bool need);
    // After the tick's anchors: what's built is added (layer), what's needed and not built is waited for, what's been
    // unwanted too long goes.
    void update(JPH::PhysicsSystem& system, JPH::ObjectLayer layer);
    void remove_all(JPH::PhysicsSystem& system);

    uint32_t tiles() const { return live_; } // with a body

private:
    TerrainTile* start(uint32_t node);
    void finish_now(TerrainTile& tile);
    void destroy(TerrainTile* tile);

    core::JobSystem* jobs_;
    core::Allocator* allocator_;
    const asset::TerrainAsset* terrain_ = nullptr;
    uint64_t heights_hash_ = 0;
    platform::Vfs* vfs_ = nullptr;
    bool heights_ok_ = false;
    core::HashMap<uint32_t, TerrainTile*> tiles_; // by node, in the finest level
    core::Array<TerrainTile*> needed_;            // this tick's, waited for in update()
    core::Array<JPH::BodyID> adding_;
    uint64_t tick_ = 0;
    uint32_t live_ = 0;
};

} // namespace de::physics
