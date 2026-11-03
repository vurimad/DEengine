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
// source. What a world is made of: its cells by grid coordinate, where the player starts, and the
// environment it is lit by. The environment is its one dependency (loaded with it); the cells are
// not (they stream), only named.
struct WorldAsset {
    static constexpr AssetType kType = AssetType::World;
    static constexpr uint16_t kFormatVersion = 1;
    static constexpr uint32_t kNone = UINT32_MAX;

    AssetHeader header;
    double start_position[3] = {}; // meters, world space
    float start_yaw_rad = 0.0f;    // about world up; 0 looks down -Z
    float start_pitch_rad = 0.0f;  // up is positive
    OffsetSpan<WorldCell> cells;   // by coord, z then x: one cell per coord
    uint32_t environment = kNone;  // into the header's dependencies (an environment)
    uint32_t reserved = 0;

    // Cells inside the file, sorted and unique by coord; the environment one of the dependencies
    // (and of type Environment); a finite start.
    core::Result<void> validate(std::span<const std::byte> file) const;
};
static_assert(sizeof(WorldAsset) == 112);

} // namespace de::asset
