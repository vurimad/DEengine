#pragma once

#include "asset/asset_id.h"
#include "core/array.h"
#include "core/math.h"

#include <cstdint>

namespace de::render {

// The sea's state (docs/design.md, Water; engine/render/scene/ocean.h): a swell come in from far off, and the waves the
// environment's wind raises over its fetch. The default is a calm sea: no swell, and what wind there is.
struct SceneSea {
    float swell_height_m = 0.0f;                // significant height (the mean of the highest third)
    float swell_period_s = 10.0f;               // its peak period
    float swell_direction[2] = {0.0f, -1.0f};   // toward where it travels (x, z)
    float fetch_m = 100000.0f;                  // how far the wind has blown over open water
    float choppiness = 0.8f;                    // how far crests sharpen: 0 round, about 1 sharp
};

// Water (docs/design.md, Water): what the world has of it. For now, a sea: a surface at one height, to the horizon.
struct SceneWater {
    bool sea = false;
    double sea_level_m = 0.0; // world space
    asset::AssetId shore;     // the sea's shore map, a material (tools/worldgen/shore.py); none: open water everywhere
    asset::AssetId foam;      // its foam, a material (base color; alpha how much is foam); none: foam drawn from noise
};

// The water's mesh: kWaterLevels square grids around the camera, each kWaterLevelQuads squares across, the first
// kWaterNearSpacingM apart and each level's twice the one inside it, with a hole where that one lies (the first has
// none): 0.5 m squares under the camera, about 49 km to the edge of the last. Each level's middle is on a grid twice
// its spacing, so its vertices stay put in the world as the camera moves, and each lands on the next level's every
// other vertex. The level inside it is then off its middle by a square at most, either way: one of 9 holes.
inline constexpr uint32_t kWaterLevels = 12;
inline constexpr uint32_t kWaterLevelQuads = 96;
inline constexpr float kWaterNearSpacingM = 0.5f;
inline constexpr uint32_t kWaterHoles = 9; // the hole off the middle by -1, 0 or 1 squares, along x then z
inline constexpr uint32_t kWaterNoHole = kWaterHoles;

// Matches WaterLevel in shaders/scene/water.slang.
struct GpuWaterLevel {
    float center[2] = {}; // x, z: relative to the render origin
    float spacing_m = 0.0f;
    uint32_t hole = kWaterNoHole; // which of the holes: (dx + 1) + 3 (dz + 1); kWaterNoHole for the first level
};
static_assert(sizeof(GpuWaterLevel) == 16);

// Each level around a camera, relative to the render origin; world positions in meters.
void water_levels(const DVec3& camera, const DVec3& origin, GpuWaterLevel (&out)[kWaterLevels]);

// The mesh's indices: a level's grid of (kWaterLevelQuads + 1)^2 vertices, row by row along x, two triangles a square,
// facing up. First every square (the first level), then for each hole in order the squares around it.
core::Array<uint16_t> water_mesh_indices(core::Allocator& allocator);
// Where a level draws from in those, and how many.
uint32_t water_first_index(uint32_t hole);
uint32_t water_index_count(uint32_t hole);

} // namespace de::render
