#pragma once

#include "asset/format.h"
#include "core/enum_flags.h"

#include <cstdint>
#include <string_view>

namespace de::asset {

// The world is a grid of cells this wide, in meters (docs/design.md, Open world & streaming).
inline constexpr double kCellSize = 128.0;

enum class CellEntityFlags : uint16_t {
    None = 0,
    Static = 1 << 0, // never moves: spawned with StaticTransform
};
DE_ENUM_FLAGS(CellEntityFlags)

enum class CellFogShape : uint32_t { Box, Ellipsoid };

inline constexpr float kMaxFogVolumeDensityPerM = 4.0f;

// A fog volume a cell places: its size is its entity's scale (half extents, in meters).
struct CellFogVolume {
    float density_per_m = 0.1f; // extinction at its heart, on top of the world's fog
    float edge_m = 1.0f;        // how far in from its surface it takes to reach that
    float tint[3] = {1, 1, 1};  // of the light it scatters
    CellFogShape shape = CellFogShape::Box;
    float noise_size_m = 8.0f;      // how big its noise's largest shapes are
    float noise_amount = 0.0f;      // 0: smooth, to 1: as much as noise can eat away
    float drift_mps[3] = {0, 0, 0}; // how its noise moves through it: wind, in the world's axes
    float height_fade_m = 0.0f;     // how far down from its top it thins from nothing; 0: not at all
};
static_assert(sizeof(CellFogVolume) == 48);

// One entity placed in a cell. Its transform is relative to its parent, or the world without one.
struct CellEntity {
    static constexpr uint32_t kNone = UINT32_MAX;

    double position[3] = {};          // meters
    float rotation[4] = {0, 0, 0, 1}; // unit quaternion x, y, z, w
    float scale[3] = {1, 1, 1};       //
    uint32_t parent = kNone;          // an earlier entity of the cell (parents come first)
    uint32_t mesh = kNone;            // into the header's dependencies (a mesh)
    uint32_t name_offset = 0;         // into CellAsset::names
    uint16_t name_length = 0;         // unique in the cell; may be empty
    CellEntityFlags flags = CellEntityFlags::None;
    uint32_t fog_volume = kNone; // into CellAsset::fog_volumes
};
static_assert(sizeof(CellEntity) == 72);

// .decell (docs/design.md, Open world & streaming: world model): the binary form of a .cell text
// source, the entities placed in one cell of the world grid. The meshes they use are the header's
// dependencies, so the registry makes the cell Ready only once they're loaded; an entity names its
// mesh by its index there (look its handle up with AssetRegistry::find on that header entry).
// Entities are in an order where every parent comes before its children.
struct CellAsset {
    static constexpr AssetType kType = AssetType::Cell;
    static constexpr uint16_t kFormatVersion = 3;

    AssetHeader header;
    int32_t coord_x = 0; // the cell's place in the grid (world::CellCoord)
    int32_t coord_z = 0;
    OffsetSpan<CellEntity> entities;
    OffsetSpan<char> names; // every entity's name, back to back, no terminators
    OffsetSpan<CellFogVolume> fog_volumes;

    // Arrays inside the file; parents earlier in the list, meshes among the dependencies (and of
    // type Mesh), names inside the names array, finite transforms, and fog volumes in the list, with
    // finite values, a density up to the limit, an edge of 0 or more, and a known shape.
    core::Result<void> validate(std::span<const std::byte> file) const;

    std::string_view name(const void* file, const CellEntity& entity) const {
        return {names.get(file).data() + entity.name_offset, entity.name_length};
    }
};
static_assert(sizeof(CellAsset) == 88 + sizeof(OffsetSpan<CellFogVolume>));

} // namespace de::asset
