#include "asset/mesh_format.h"

#include <algorithm>

namespace de::asset {

core::Result<void> MeshAsset::validate(std::span<const std::byte> file) const {
    using core::ErrorCode;
    const uint64_t size = file.size();
    if (!span_in_file(vertices, size) || !span_in_file(indices, size) || !span_in_file(lods, size) ||
        !span_in_file(parts, size) || !span_in_file(materials, size)) {
        return core::fail(ErrorCode::CorruptData, "mesh array lies outside the file");
    }
    if (vertex_stride != sizeof(MeshVertex)) {
        return core::fail(ErrorCode::CorruptData, "mesh vertex layout differs from this build's");
    }
    if (lods.count == 0 || indices.count % 3 != 0) {
        return core::fail(ErrorCode::CorruptData, "mesh needs a LOD and whole triangles");
    }
    for (const MeshLod& lod : lods.get(file.data())) {
        if (lod.part_count == 0 || static_cast<uint64_t>(lod.first_part) + lod.part_count > parts.count) {
            return core::fail(ErrorCode::CorruptData, "mesh LOD refers to parts it doesn't have");
        }
    }
    for (const MeshPart& part : parts.get(file.data())) {
        if (part.first_index % 3 != 0 || part.index_count % 3 != 0 ||
            static_cast<uint64_t>(part.first_index) + part.index_count > indices.count ||
            part.material_slot >= materials.count) {
            return core::fail(ErrorCode::CorruptData, "mesh part refers to indices or a slot it doesn't have");
        }
    }
    // Shaders fetch vertices by pointer, where nothing catches a read past the buffer (a lost
    // device at best). One pass at memory speed, small next to reading the file.
    uint32_t largest = 0;
    for (const uint32_t index : indices.get(file.data())) {
        largest = std::max(largest, index);
    }
    if (indices.count != 0 && largest >= vertices.count) {
        return core::fail(ErrorCode::CorruptData, "mesh index refers past its vertices");
    }
    return {};
}

} // namespace de::asset
