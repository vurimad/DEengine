#pragma once

#include "asset/environment_format.h"
#include "asset/terrain_layers_format.h"
#include "asset/world_format.h"
#include "cooker/common.h"
#include "core/function_ref.h"

#include <span>
#include <string_view>

namespace de::cooker {

class SourceTree;
struct Source;

// A world on its way to a .deworld.
struct WorldBuild {
    double start_position[3] = {};
    float start_yaw_rad = 0.0f;
    float start_pitch_rad = 0.0f;
    core::Array<asset::WorldCell> cells{tools_heap()}; // sorted by coord, z then x
    asset::AssetRef environment;                       // the first dependency
    asset::AssetRef terrain;                           // the second, if it has one (its ID valid)
    asset::AssetRef terrain_layers;                    // next, if it has them (their ID valid)
    bool sea = false;
    float sea_level_m = 0.0f;
    asset::AssetRef sea_shore;                  // the shore map's material, if it has one (its ID valid)
    asset::AssetRef sea_foam;                   // the foam's material, likewise
    core::Array<SourcePath> read{tools_heap()}; // the sources it names: environment, terrain, layers, cells
};

// A .world text source (JSON): what a world is made of. Paths are relative to the world file.
//     {
//       "environment": "evening.environment",          // how it's lit
//       "terrain": "land.terrain",                      // optional: what it stands on
//       "terrain_layers": "land.terrainlayers",         // optional, with a terrain: how that looks
//       "sea": {"level_m": 0,                           // optional: a sea to the horizon, its surface's height
//               "shore": "shore.material",              // optional: its shore map (tools/worldgen/shore.py)
//               "foam": "sea_foam.material"},           // optional: its foam (base color; alpha how much is foam)
//       "start": {"position": [64, 2, 74],              // where the player starts, meters
//                 "yaw_deg": 0, "pitch_deg": -7},       // optional: the view (0 yaw looks down -Z)
//       "cells": ["cell_0_0.cell", "cell_1_0.cell"]     // its cells; each one's coord is read from
//     }                                                 // it, and must be the only cell there
// read gives a source's bytes (a cell's, for its coord).
using SourceReader = core::FunctionRef<core::Result<core::Array<std::byte>>(std::string_view path)>;
core::Result<WorldBuild> import_world(const SourceTree& tree, const Source& source, std::span<const std::byte> bytes,
                                      SourceReader read, Diagnostics& diagnostics);

// The .deworld; its environment, its terrain and its layers are its dependencies.
core::Array<std::byte> cook_world(const WorldBuild& build, asset::AssetId id);

// A .terrainlayers text source (JSON): how the ground looks. Layers run from the bottom of the stack up, and each
// covers what's below as far as its rules say. A rule is four numbers: nothing up to the first, rising to all of it at
// the second, all of it to the third, falling to nothing at the fourth. A channel rule is two: nothing and all of it,
// rising, or falling for where the channel is low. Paths are relative to this file. Optional but for those marked.
//     {
//       "masks": "masks.material",       // its metallic_roughness texture: four channels over the whole map
//       "details": "details.material",   // the same, for what the generator found in the land's shape
//       "lighting": "light.material",    // the same, for light baked for the environment's sun
//       "normals": "normals.material",   // its normal texture: the ground's own normal over the whole map (x, z)
//       "colors": "paint.material",      // its metallic_roughness texture: masks the ground is painted from per pixel
//       "color_map": "color.material",   // its base_color texture: the ground's color over the whole map (far off; up
//                                        // close the layers are tinted to it)
//       "color_map_strength": 1,         // 0 to 1: how far up close the layers take its color
//       "blend": 0.25,                   // how soft blending by height is, 0 to 1
//       "layers": [{                     // required
//         "material": "sand.material",   // required: metallic_roughness holds occlusion, roughness, height
//         "tile_m": 3,                   // required: how far apart its textures repeat
//         "height_m": [-5, -5, 4, 12],   // over height above the sea
//         "slope_deg": [0, 0, 20, 30],   // over the ground's slope, 0 to 90
//         "mask": "r",                   // r, g, b or a: only as far as this channel of the masks says
//         "mask_range": [0.4, 0.6],      // that channel's rule
//         "detail": "g",                 // r, g, b or a: only as far as this channel of the details says too
//         "detail_range": [0.2, 0.5],    // that channel's rule
//         "detail_coverage": true,       // that channel is how much of each texel it covers: far off, it takes the
//         share "noise": 0.5,                  // how far noise breaks up where these rules fade "noise_large": 0, // 0
//         breaks it up at about 30 m, 1 at about 300 m "variation": 0.1,              // how far its brightness wanders
//         across the land "tint": [1, 0.9, 0.8],         // multiplies its color, linear "projection": "sides" // "top"
//         (the default) or "sides", for steep ground
//       }],
//       "grass": {                       // blades made around the camera
//         "density_per_m2": 120,         // required: up close, where it all grows
//         "height_m": [0.3, 0.7],        // required: its lowest and tallest blades
//         "width_m": 0.03,               // required: at the root, up close
//         "where_height_m": [2, 8, 500, 700], // over height above the sea
//         "where_slope_deg": [0, 0, 24, 34],  // over the ground's slope
//         "not_where": "rb",             // channels of the masks where it doesn't grow
//         "patches": 0.3,                // how far noise thins it into tufts, 0 to 1
//         "base": [0.04, 0.07, 0.02],    // required: its color at the root, linear
//         "tip": [0.2, 0.28, 0.07],      // required: and at the tip
//         "other": {"mask": "a", "base": [0.09, 0.05, 0.01], "tip": [0.4, 0.25, 0.05]} // where that channel says
//       }
//     }
struct TerrainLayersBuild {
    core::Array<asset::TerrainLayer> layers{tools_heap()};
    core::Array<asset::AssetRef> dependencies{tools_heap()}; // its materials, each once
    uint32_t masks = asset::TerrainLayersAsset::kNone;
    uint32_t details = asset::TerrainLayersAsset::kNone;
    uint32_t lighting = asset::TerrainLayersAsset::kNone;
    uint32_t normals = asset::TerrainLayersAsset::kNone;
    uint32_t colors = asset::TerrainLayersAsset::kNone;
    uint32_t color_map = asset::TerrainLayersAsset::kNone;
    float color_map_strength = 1.0f;
    float blend = 0.25f;
    asset::TerrainGrass grass;
    core::Array<SourcePath> read{tools_heap()}; // the sources it names
};
core::Result<TerrainLayersBuild> import_terrain_layers(const SourceTree& tree, const Source& source,
                                                       std::span<const std::byte> bytes, Diagnostics& diagnostics);
core::Array<std::byte> cook_terrain_layers(const TerrainLayersBuild& build, asset::AssetId id);

// A .environment text source (JSON): how a place is lit. Every setting but the intensities (1 when
// left out) and the fog's low_camera is required, so a file says all of its look; colors are linear. Shadow settings
// are held to asset::ShadowLimits.
//     {
//       "sun": {"elevation_deg": 19, "azimuth_deg": 324,   // above the horizon; clockwise from
//               "color": [1, 0.78, 0.52], "intensity": 4.6}, // north (-Z) toward east (+X)
//       "sky": {"color": [0.3, 0.4, 0.62], "intensity": 1}, // ambient from above (intensity
//       "ground": {"color": [0.12, 0.1, 0.07]},             // defaults to 1), and from below
//       "shadow_color": [0.05, 0.06, 0.09],                 // what shadowed sunlight keeps
//       "exposure_ev": 0,
//       "shadows": {"resolution": 2048, "distance_m": 140, "split_lambda": 0.75, "blend_band": 0.12,
//                   "caster_reach_m": 200, "penumbra_per_m": 0.06, "max_penumbra_texels": 6},
//       "fog": {"density_per_m": 0.00008, "height_density_per_m": 0.004, "height_falloff_per_m": 0.08,  // per meter
//               "base_height_m": 0, "extinction_tint": [0.8, 1, 1.3],
//               "sun_forward": {"color": [1, 0.85, 0.7], "intensity": 0.5},  // times the sun's
//               "sun_back": {"color": [0.5, 0.6, 0.8], "intensity": 0.08},
//               "sun_height_attenuation": 1, "ambient": {"color": [0.3, 0.4, 0.62]},
//               "ambient_height_attenuation": 1, "start_m": 0, "sun_start_m": 5, "end_m": 20000,
//               "volume_distance_m": 128,  // the froxel grid's reach (light shafts); 0: none
//               "low_camera": {"keep": 0.3, "below_m": 60, "above_m": 350}}  // optional: how much height fog is left
//                                  // with the camera below below_m, coming back fully by above_m
//     }
// The shadow settings are render::SceneShadows', the fog's render::SceneFog's (asset::FogLimits).
core::Result<asset::EnvironmentAsset> import_environment(const Source& source, std::span<const std::byte> bytes,
                                                         Diagnostics& diagnostics);

core::Array<std::byte> cook_environment(const asset::EnvironmentAsset& environment, asset::AssetId id);

} // namespace de::cooker
