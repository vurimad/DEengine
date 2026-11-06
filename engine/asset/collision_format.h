#pragma once

#include "asset/asset_id.h"
#include "asset/format.h"

#include <cstddef>
#include <cstdint>

namespace de::asset {

// .dephys (docs/design.md, Asset formats): one mesh's static collision, as the binary state of a Jolt MeshShape. What
// those bytes mean is known only to engine/physics, which builds them for the cooker and restores them: nothing else
// sees Jolt's types. Jolt doesn't check what it reads, so restoring goes through verify_content_hash() first, and
// refuses any other shape_format.
struct CollisionAsset {
    static constexpr AssetType kType = AssetType::Collision;
    static constexpr uint16_t kFormatVersion = 1;

    AssetHeader header;
    uint64_t shape_format = 0; // physics::mesh_shape_format() of what wrote it: another Jolt can't read it
    uint32_t triangles = 0;
    uint32_t vertices = 0;   // every position once, however many vertices the mesh split it into
    float error_m = 0.0f;    // of the LOD it's from (MeshLod::error), before an instance's scale
    uint32_t source_lod = 0; // which LOD of the mesh
    OffsetSpan<std::byte> shape;

    // The shape lies inside the file, and there is one.
    core::Result<void> validate(std::span<const std::byte> file) const;
};
static_assert(sizeof(CollisionAsset) == 96);

// A mesh's collision: a sub-asset of its model, so what names a model can find it without another reference.
constexpr AssetId collision_asset_id(AssetId mesh) {
    return sub_asset_id(mesh, "collision");
}

} // namespace de::asset
