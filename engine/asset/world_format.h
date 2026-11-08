#pragma once

#include "asset/format.h"

#include <cstdint>

namespace de::asset {

// A cell of a world: which cell asset, and where it lies in the grid.
struct WorldCell {
    AssetId cell;        // a .decell
    int32_t coord_x = 0; // as the cell's own coord
    int32_t coord_z = 0;
};
static_assert(sizeof(WorldCell) == 16);

// .deworld (docs/design.md, Open world & streaming: world model): the binary form of a .world text
// source. What a world is made of: its cells by grid coordinate, where the player starts, the
// environment it is lit by, the terrain it stands on and how that looks. The environment, the terrain
// and its layers are its dependencies (loaded with it; the terrain's heights stream on their own); the
// cells are not (they stream), only named.
struct WorldAsset {
    static constexpr AssetType kType = AssetType::World;
    static constexpr uint16_t kFormatVersion = 4; // 2: terrain; 3: terrain layers; 4: the sea
    static constexpr uint32_t kNone = UINT32_MAX;
    static constexpr float kMostSeaLevelM = 100000.0f;

    AssetHeader header;
    double start_position[3] = {}; // meters, world space
    float start_yaw_rad = 0.0f;    // about world up; 0 looks down -Z
    float start_pitch_rad = 0.0f;  // up is positive
    OffsetSpan<WorldCell> cells;   // by coord, z then x: one cell per coord
    uint32_t environment = kNone;  // into the header's dependencies (an environment)
    uint32_t terrain = kNone;      // into the header's dependencies (a terrain); kNone: none
    uint32_t terrain_layers = kNone; // into the header's dependencies; kNone: the terrain's plain look
    uint32_t sea = 0;                // 1: a sea to the horizon, its surface at sea_level_m; 0: none
    float sea_level_m = 0.0f;        // world space
    uint32_t reserved = 0;

    // Cells inside the file, sorted and unique by coord; the environment, the terrain and its layers
    // each one of the dependencies (and of its type); a finite start; a sea 0 or 1, its level within
    // kMostSeaLevelM of 0.
    core::Result<void> validate(std::span<const std::byte> file) const;
};
static_assert(sizeof(WorldAsset) == 128);

} // namespace de::asset
