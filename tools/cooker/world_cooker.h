#pragma once

#include "asset/environment_format.h"
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
    asset::AssetRef environment;                       // the one dependency
    core::Array<SourcePath> read{tools_heap()};        // the sources it read: the environment, then the cells
};

// A .world text source (JSON): what a world is made of. Paths are relative to the world file.
//     {
//       "environment": "evening.environment",          // how it's lit
//       "start": {"position": [64, 2, 74],              // where the player starts, meters
//                 "yaw_deg": 0, "pitch_deg": -7},       // optional: the view (0 yaw looks down -Z)
//       "cells": ["cell_0_0.cell", "cell_1_0.cell"]     // its cells; each one's coord is read from
//     }                                                 // it, and must be the only cell there
// read gives a source's bytes (a cell's, for its coord).
using SourceReader = core::FunctionRef<core::Result<core::Array<std::byte>>(std::string_view path)>;
core::Result<WorldBuild> import_world(const SourceTree& tree, const Source& source, std::span<const std::byte> bytes,
                                      SourceReader read, Diagnostics& diagnostics);

// The .deworld; its environment is its dependency.
core::Array<std::byte> cook_world(const WorldBuild& build, asset::AssetId id);

// A .environment text source (JSON): how a place is lit. Every setting is required, so a file says
// all of its look; colors are linear.
//     {
//       "sun": {"elevation_deg": 19, "azimuth_deg": 324,   // above the horizon; clockwise from
//               "color": [1, 0.78, 0.52], "intensity": 4.6}, // north (-Z) toward east (+X)
//       "sky": {"color": [0.3, 0.4, 0.62], "intensity": 1}, // ambient from above (intensity
//       "ground": {"color": [0.12, 0.1, 0.07]},             // defaults to 1), and from below
//       "shadow_color": [0.05, 0.06, 0.09],                 // what shadowed sunlight keeps
//       "exposure_ev": 0,
//       "shadows": {"resolution": 2048, "distance_m": 140, "split_lambda": 0.75, "blend_band": 0.12,
//                   "caster_reach_m": 200, "penumbra_per_m": 0.06, "max_penumbra_texels": 6}
//     }
// The shadow settings are render::SceneShadows'.
core::Result<asset::EnvironmentAsset> import_environment(const Source& source, std::span<const std::byte> bytes,
                                                         Diagnostics& diagnostics);

core::Array<std::byte> cook_environment(const asset::EnvironmentAsset& environment, asset::AssetId id);

} // namespace de::cooker
