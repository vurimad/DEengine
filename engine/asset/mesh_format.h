#pragma once

#include "asset/format.h"
#include "core/string_id.h"

#include <cstdint>

namespace de::asset {

// One vertex of a cooked mesh, 32 bytes, read by shaders through a buffer pointer (vertex pulling).
struct MeshVertex {
    float position[3] = {}; // meters, object space (right-handed, Y up)
    uint32_t normal = 0;    // unit vector, octahedral encoding: two snorm16 (x low, y high)
    uint32_t tangent = 0;   // snorm8 x, y, z, w (low to high); w is the bitangent sign, +1 or -1
    float uv[2] = {};       // texture coordinates, origin top left
    uint32_t color = 0;     // rgba8 unorm (r low): vertex color, white if the source had none
};
static_assert(sizeof(MeshVertex) == 32);

struct MeshBounds {
    float min[3] = {};
    float max[3] = {};
    float center[3] = {}; // of the bounding sphere
    float radius = 0.0f;
};
static_assert(sizeof(MeshBounds) == 40);

// A range of triangles drawn with one material.
struct MeshPart {
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    uint32_t material_slot = 0; // into MeshAsset::materials
    uint32_t reserved = 0;
};

// One level of detail: its parts, all indexing the one shared vertex array.
struct MeshLod {
    float error = 0.0f; // meters, object space: how far this LOD may lie from LOD 0
    uint32_t first_part = 0;
    uint32_t part_count = 0;
    // 1: its seams moved as it was simplified: a triangle of it joins vertices from two pieces of the surface that
    // LOD 0 keeps apart (uv islands, faces with normals of their own). Drawn small that doesn't show; what's read off
    // its surface up close (a texture's colour, by a ray) is wrong there.
    uint32_t seams_moved = 0;
};

// The material a slot uses unless an instance overrides it.
struct MeshMaterialSlot {
    core::StringId name; // the source's material name
    AssetId material;    // invalid: the source had none; draw with the default material
};

enum class MeshFlags : uint32_t {
    None = 0,
};

// .demesh (docs/design.md, Asset formats): optimized vertex and index buffers (vertex-cache,
// overdraw and fetch order), a LOD chain, bounds, parts and material slots. Indices are 32-bit.
struct MeshAsset {
    static constexpr AssetType kType = AssetType::Mesh;
    static constexpr uint16_t kFormatVersion = 3; // 3: a LOD says whether its seams moved

    AssetHeader header;
    MeshBounds bounds;
    MeshFlags flags = MeshFlags::None;
    uint32_t vertex_stride = sizeof(MeshVertex);
    OffsetSpan<MeshVertex> vertices;
    OffsetSpan<uint32_t> indices;
    OffsetSpan<MeshLod> lods; // LOD 0 first, coarser after
    OffsetSpan<MeshPart> parts;
    OffsetSpan<MeshMaterialSlot> materials;

    // Arrays inside the file; LODs, parts, slots and every index refer to each other in range.
    core::Result<void> validate(std::span<const std::byte> file) const;
};
static_assert(sizeof(MeshAsset) == 152);

} // namespace de::asset
