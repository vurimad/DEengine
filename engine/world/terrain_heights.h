#pragma once

#include "asset/terrain_format.h"
#include "core/allocator.h"
#include "core/array.h"
#include "core/math.h"

#include <cstdint>

namespace de::platform {
class Vfs;
}

namespace de::world {

// The ground's height anywhere on a cooked terrain, on the CPU (docs/design.md, Terrain & vegetation): for tools and
// the editor (placing things on the ground), and for what needs the ground on the CPU later. A point is answered from
// the finest level that has a block there, between its four samples (bilinear: within a step or so of the drawn
// triangles). Blocks are read from the heights file when first asked about, blocking (tools, not the frame), and the
// last kCachedBlocks are kept.
//
//     TerrainHeights heights(vfs, allocator);
//     heights.set_terrain(assets.get<asset::TerrainAsset>(terrain));
//     double y = 0.0;
//     if (heights.height_at(x, z, y)) { ... }
class TerrainHeights {
public:
    static constexpr uint32_t kCachedBlocks = 64; // 64 x 35 KiB

    TerrainHeights(platform::Vfs& vfs, core::Allocator& allocator) : vfs_(&vfs), cache_(allocator) {}

    // The terrain to answer about: a cooked asset whose bytes stay valid while it's set (the registry's), or null for
    // none. Setting the same one again keeps what's read; another (or the same cooked again) starts over.
    void set_terrain(const asset::TerrainAsset* terrain);
    bool has_terrain() const { return terrain_ != nullptr; }

    // The ground's height at x, z (world meters): false off the map, or if its heights can't be read.
    bool height_at(double x, double z, double& height_m);
    // Where a ray from origin along direction (unit length) first comes down onto the ground, within max_m: false if
    // it doesn't (a ray starting under the ground doesn't count).
    bool ray_cast(const DVec3& origin, const Vec3& direction, double max_m, DVec3& hit);

private:
    struct Cached {
        uint32_t block = asset::kTerrainNoBlock;
        uint64_t used = 0; // when it was last asked about: the least recent goes first
        uint16_t samples[asset::kTerrainNodeSamples * asset::kTerrainNodeSamples] = {};
    };
    // The block's samples, read now if they aren't kept: null if it can't be read.
    const uint16_t* block(uint32_t index);
    bool heights_ready();

    platform::Vfs* vfs_;
    const asset::TerrainAsset* terrain_ = nullptr;
    uint64_t heights_hash_ = 0;
    bool header_checked_ = false;
    bool header_ok_ = false;
    core::Array<Cached> cache_;
    uint64_t clock_ = 0;
};

} // namespace de::world
