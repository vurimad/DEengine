#pragma once

#include "asset/format.h"
#include "asset/light_format.h"
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
    // How far the place it fills is closed to the sky, 0 to 1, in place of the environment's under_roof there (a
    // hall with a roof in an open world: 1; a courtyard in a roofed one: 0); negative: it doesn't say. A volume with
    // no density is only this.
    float under_roof = -1.0f;
};
static_assert(sizeof(CellFogVolume) == 52);

enum class CellWaterShape : uint32_t { Rectangle, Ellipse };

// A body of still water a cell places, a lake or a pond: a flat surface at its entity's height, its size its entity's
// scale (half extents along x and z, in meters). It shows only where the ground is lower than it.
struct CellWaterBody {
    CellWaterShape shape = CellWaterShape::Ellipse;
    float waves = 0.15f; // 0: glassy, 1: the sea's own waves (the environment's wind on open water)
};
static_assert(sizeof(CellWaterBody) == 8);

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
    uint32_t collision = kNone;  // into the header's dependencies (a mesh's collision): a static body in that shape
    uint32_t source_index = 0;   // its place in the .cell source's entities, for the editor's saving: unique
    uint32_t water_body = kNone; // into CellAsset::water_bodies
    uint32_t effect = kNone;     // into the header's dependencies (an effect): its emitters play here
    // Into the header's dependencies (a prefab): every part of it is here, placed from this entity (docs/design.md,
    // Prefabs). And which of its variants, by the hash of its name (asset::prefab_variant_hash; 0: the prefab as it
    // is).
    uint32_t prefab = kNone;
    uint32_t variant = 0;
    uint32_t light = kNone; // into CellAsset::lights: a local light where it is
};
static_assert(sizeof(CellEntity) == 104);

// .decell (docs/design.md, Open world & streaming: world model): the binary form of a .cell text
// source, the entities placed in one cell of the world grid. The meshes they use, the collision of
// those that ask for it, and the effects they play are the header's dependencies, so the registry makes the cell Ready
// only once they're loaded; an entity names each by its index there (look its handle up with AssetRegistry::find on
// that header entry). Entities are in an order where every parent comes before its children.
struct CellAsset {
    static constexpr AssetType kType = AssetType::Cell;
    // 4: collision; 5: each entity's place in the source; 6: water; 7: effects; 8: prefabs; 9: lights; 10: a fog
    // volume's under_roof; 11: a light's shadow
    static constexpr uint16_t kFormatVersion = 11;

    AssetHeader header;
    int32_t coord_x = 0; // the cell's place in the grid (world::CellCoord)
    int32_t coord_z = 0;
    OffsetSpan<CellEntity> entities;
    OffsetSpan<char> names; // every entity's name, back to back, no terminators
    OffsetSpan<CellFogVolume> fog_volumes;
    OffsetSpan<CellWaterBody> water_bodies;
    OffsetSpan<LightDesc> lights;

    // Arrays inside the file; parents earlier in the list, meshes, collision and effects among the
    // dependencies (and of those types), names inside the names array, finite transforms, and fog
    // volumes in the list, with finite values, a density up to the limit, an edge of 0 or more, and
    // a known shape; water bodies in the list, of a known shape, their waves 0 to 1; lights in the
    // list, within LightLimits.
    core::Result<void> validate(std::span<const std::byte> file) const;

    std::string_view name(const void* file, const CellEntity& entity) const {
        return {names.get(file).data() + entity.name_offset, entity.name_length};
    }
};
static_assert(sizeof(CellAsset) == 88 + sizeof(OffsetSpan<CellFogVolume>) + sizeof(OffsetSpan<CellWaterBody>) +
                                       sizeof(OffsetSpan<LightDesc>));

} // namespace de::asset
