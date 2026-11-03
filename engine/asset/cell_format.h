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
    uint32_t reserved = 0;
};
static_assert(sizeof(CellEntity) == 72);

// .decell (docs/design.md, Open world & streaming: world model): the binary form of a .cell text
// source, the entities placed in one cell of the world grid. The meshes they use are the header's
// dependencies, so the registry makes the cell Ready only once they're loaded; an entity names its
// mesh by its index there (AssetRegistry::dependencies gives the handles, in the same order).
// Entities are in an order where every parent comes before its children.
struct CellAsset {
    static constexpr AssetType kType = AssetType::Cell;
    static constexpr uint16_t kFormatVersion = 1;

    AssetHeader header;
    int32_t coord_x = 0; // the cell's place in the grid (world::CellCoord)
    int32_t coord_z = 0;
    OffsetSpan<CellEntity> entities;
    OffsetSpan<char> names; // every entity's name, back to back, no terminators

    // Arrays inside the file; parents earlier in the list, meshes among the dependencies (and of
    // type Mesh), names inside the names array, and finite transforms.
    core::Result<void> validate(std::span<const std::byte> file) const;

    std::string_view name(const void* file, const CellEntity& entity) const {
        return {names.get(file).data() + entity.name_offset, entity.name_length};
    }
};
static_assert(sizeof(CellAsset) == 88);

} // namespace de::asset
