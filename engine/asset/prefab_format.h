#pragma once

#include "asset/format.h"
#include "asset/light_format.h"

#include <cstdint>
#include <string_view>

namespace de::asset {

// The range of a prefab's sizes: what the cooker accepts and loading checks (docs/design.md, Prefabs).
struct PrefabLimits {
    static constexpr uint32_t kMaxParts = 4096;    // of one prefab, everything it nests flattened in
    static constexpr uint32_t kMaxVariants = 32;   // a bit each in PrefabPart::absent_in
    static constexpr uint32_t kMaxDepth = 8;       // prefabs inside prefabs, in the sources
    static constexpr uint32_t kMaxIdLength = 31;   // of one part's ID, and of a variant's name
    static constexpr float kMaxReachM = 100000.0f; // how far from its origin a part may lie
};

// One part of a prefab: what a cell's entity can be (a mesh with or without collision, an effect, a light), placed
// relative to the prefab's origin. Parts of the prefabs a prefab nests are in with its own: the cooker flattens them,
// so nothing at run time walks a tree.
struct PrefabPart {
    static constexpr uint32_t kNone = UINT32_MAX;

    float position[3] = {};           // meters, from the prefab's origin
    float rotation[4] = {0, 0, 0, 1}; // unit quaternion x, y, z, w
    float scale[3] = {1, 1, 1};       //
    uint32_t mesh = kNone;            // into the header's dependencies (a mesh)
    uint32_t collision = kNone;       // likewise (a mesh's collision): a static body in that shape
    uint32_t effect = kNone;          // likewise (an effect): its emitters play here
    uint32_t absent_in = 0;           // a bit for each variant (PrefabAsset::variants) that leaves this part out
    uint32_t name_offset = 0;         // into PrefabAsset::names: its path of part IDs, "candle_table/flame"
    uint16_t name_length = 0;         // unique in the prefab, never empty
    uint16_t reserved = 0;
    uint32_t light = kNone; // into PrefabAsset::lights: a local light there
    uint32_t reserved2 = 0;
    // A hash of that path: the same every cook, whatever order the parts come in. With the ID of what places the
    // prefab, the part's identity (docs/design.md, Prefabs: identity).
    uint64_t id = 0;
};
static_assert(sizeof(PrefabPart) == 80);

// A variant of a prefab: the same prefab with some parts left out (PrefabPart::absent_in).
struct PrefabVariant {
    uint32_t name_offset = 0; // into PrefabAsset::names
    uint16_t name_length = 0;
    uint16_t reserved = 0;
    uint32_t name_hash = 0; // what a placement names it by (prefab_variant_hash)
};
static_assert(sizeof(PrefabVariant) == 12);

// What a placement keeps of a variant's name: 0 is no variant, the prefab as it is.
uint32_t prefab_variant_hash(std::string_view name);
// A part's ID from its path of part IDs.
uint64_t prefab_part_id(std::string_view path);

// .deprefab (docs/design.md, Prefabs): the binary form of a .prefab text source. A composed thing, defined once: its
// parts in one flat list, each relative to the prefab's origin, with the meshes, collision and effects they use as
// the header's dependencies. A cell's entity places it (CellEntity::prefab); whatever spawns that entity makes an
// entity for each part the placement's variant keeps.
struct PrefabAsset {
    static constexpr AssetType kType = AssetType::Prefab;
    static constexpr uint16_t kFormatVersion = 3; // 2: lights; 3: a light's shadow

    AssetHeader header;
    OffsetSpan<PrefabPart> parts;       // at least one, at most PrefabLimits::kMaxParts
    OffsetSpan<PrefabVariant> variants; // at most PrefabLimits::kMaxVariants
    OffsetSpan<char> names;             // every part's path and every variant's name, back to back, no terminators
    OffsetSpan<LightDesc> lights;       // the parts' lights
    float reach_m = 0.0f;               // no part's origin lies farther from the prefab's
    uint32_t reserved = 0;

    // Arrays inside the file; each part's mesh, collision and effect among the dependencies (and of those types), its
    // light in the lights (each within LightLimits), its name inside the names and not empty, its transform finite
    // with no zero in its scale, its variants' bits within the variants there are; each variant's name inside the
    // names.
    core::Result<void> validate(std::span<const std::byte> file) const;

    std::string_view name(const void* file, const PrefabPart& part) const {
        return {names.get(file).data() + part.name_offset, part.name_length};
    }
    std::string_view name(const void* file, const PrefabVariant& variant) const {
        return {names.get(file).data() + variant.name_offset, variant.name_length};
    }
    // The bit of the variant a placement names (CellEntity::variant), in PrefabPart::absent_in: 0 for no variant, and
    // for one the prefab doesn't have (its parts are all there).
    uint32_t variant_bit(const void* file, uint32_t name_hash) const;
};
static_assert(sizeof(PrefabAsset) == 64 + 4 * sizeof(OffsetSpan<char>) + 8);

} // namespace de::asset
