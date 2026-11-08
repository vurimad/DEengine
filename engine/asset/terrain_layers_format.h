#pragma once

#include "asset/format.h"
#include "core/enum_flags.h"

#include <cstdint>

namespace de::asset {

// How the ground looks (docs/design.md, Terrain & vegetation: materials and texturing), apart from its shape: heights
// take long to cook and change little, and rules change often, and one map can have several looks.

// The most layers a look can have. Every ground pixel tests them all, and reads the textures of at most three.
inline constexpr uint32_t kTerrainMaxLayers = 16;
// A layer's mask or detail when no channel of the masks or the details limits it.
inline constexpr uint32_t kTerrainNoMask = 4;

// How a layer's textures lie on the ground.
enum class TerrainProjection : uint32_t {
    Top = 0,   // from straight above, which stretches on steep ground
    Sides = 1, // from whichever two of above and the sides face the ground most, with the texture's up kept up
};

enum class TerrainLayerFlags : uint32_t {
    None = 0,
    // Its detail channel is a coverage: how much of each texel the layer covers, 0 or 1 but at edges. Up close the rule
    // draws a sharp edge through it; far off, where a pixel spans texels, the layer takes the share the channel averages
    // to there, so features thinner than a pixel still show, as a camera sees them, rather than vanishing.
    DetailCoverage = 1U << 0U,
};
DE_ENUM_FLAGS(TerrainLayerFlags)

// One layer of the stack. Layers cover the ones before them, as far as their own rules say. A rule is a shape over a
// value: nothing up to [0], rising to all of it at [1], all of it up to [2], falling to nothing at [3].
struct TerrainLayer {
    uint32_t material = 0; // into the header's dependencies. Its metallic-roughness texture holds occlusion, roughness
                           // and, in place of metalness, the surface's height, which the layers blend by
    float tile_m = 1.0f;   // how far apart its textures repeat
    TerrainProjection projection = TerrainProjection::Top;
    uint32_t mask = kTerrainNoMask;     // which channel of the masks limits it
    float height_m[4] = {};             // the rule over height above the sea
    float slope_cos[4] = {};            // over the ground's normal.y, so these fall. Past 1 or -1: no limit that way
    float mask_from = 0.0f;             // what its mask channel has to be: nothing here
    float mask_to = 1.0f;               // and all of it here; below mask_from, it's where the channel is low
    float noise = 0.0f;                 // how far noise breaks up where its rules fade, from 0
    float noise_large = 0.0f;           // 0: at about 30 m; 1: at about 300 m
    float variation = 0.0f;             // how far its brightness wanders across the land, as a share
    float tint[3] = {1.0f, 1.0f, 1.0f}; // multiplies its color; linear
    uint32_t detail = kTerrainNoMask;   // which channel of the details limits it too
    float detail_from = 0.0f;           // what that channel has to be: nothing here
    float detail_to = 1.0f;             // and all of it here
    TerrainLayerFlags flags = TerrainLayerFlags::None;
};
static_assert(sizeof(TerrainLayer) == 96);

// Grass over the ground (docs/design.md, Terrain & vegetation: grass fields): where it grows and what it's like. Blades
// are made around the camera, from these.
struct TerrainGrass {
    float density_per_m2 = 0.0f;   // blades up close where it all grows. 0: no grass
    float height_m[2] = {};        // its lowest and tallest blades
    float width_m = 0.0f;          // a blade's width at the root, up close
    float where_height_m[4] = {};  // a rule over height above the sea, as a layer's
    float where_slope_cos[4] = {}; // and over the ground's normal.y
    float color_base[3] = {};      // at the root; linear
    uint32_t excluded_masks = 0;   // a bit for each channel of the masks where it doesn't grow
    float color_tip[3] = {};       // at the tip; linear
    float patches = 0.0f;          // how far noise thins it into tufts and bare ground, 0 to 1
    // Its color where one channel of the masks says otherwise, as autumn grass is in an autumn region.
    float other_base[3] = {};
    uint32_t other_mask = kTerrainNoMask;
    float other_tip[3] = {};
    uint32_t reserved = 0;
};
static_assert(sizeof(TerrainGrass) == 112);

// .delayers: the binary form of a .terrainlayers text source. Its materials are its dependencies, loaded with it.
struct TerrainLayersAsset {
    static constexpr AssetType kType = AssetType::TerrainLayers;
    static constexpr uint16_t kFormatVersion = 4;
    static constexpr uint32_t kNone = UINT32_MAX;

    AssetHeader header;
    OffsetSpan<TerrainLayer> layers; // from the bottom of the stack up
    uint32_t masks = kNone; // into the dependencies: a material whose metallic-roughness texture holds the map's masks,
                            // four channels stretched over the whole map. kNone: every channel reads 0
    uint32_t details = kNone; // the same, for what the generator found in the land's own shape: where snow lies, where
                              // water runs, where scree gathers, where ridges stand out. Finer than the masks
    uint32_t lighting = kNone; // the same, for light baked for the environment's sun (shaders/scene/terrain_light.slang):
                               // how deep in shadow each place lies (r) and how much sky it sees (g). White: all lit
    uint32_t normals = kNone;  // a material whose normal texture holds the ground's own normal over the whole map, finer
                               // than its heights: red is the normal's x (east), green its z (south). Far off, where the
                               // mesh is coarse, it still shades every rib. kNone: the mesh's normals
    float blend = 0.25f;       // how soft height blending is: the share of a surface's height range that shows through
    TerrainGrass grass;
    uint32_t reserved = 0;

    // The layers inside the file, and no more than kTerrainMaxLayers; each one's material, and the masks, details,
    // lighting and normals, a Material among the dependencies; every number finite, in range and each rule in order.
    core::Result<void> validate(std::span<const std::byte> file) const;
};
static_assert(sizeof(TerrainLayersAsset) == 208);

} // namespace de::asset
