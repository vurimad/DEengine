#pragma once

#include "asset/mesh_format.h"
#include "cooker/model.h"

#include <cstdint>
#include <span>

namespace de::cooker {

// The .demesh of a model: one part per material (LOD 0), vertices deduplicated and optimized
// (vertex cache, overdraw, fetch order), then a LOD chain simplified from LOD 0 over the same
// vertices. material_ids[i] is the cooked material of model material i; primitives without a
// material use a slot with no material (the default one).
core::Result<core::Array<std::byte>> cook_mesh(const ModelData& model, std::span<const asset::AssetId> material_ids,
                                               const MeshSettings& settings, asset::AssetId id, std::string_view path,
                                               Diagnostics& diagnostics);

// MeshVertex packing (also what shaders unpack).
uint32_t encode_octahedral(const float (&normal)[3]); // unit vector -> two snorm16
void decode_octahedral(uint32_t packed, float (&normal)[3]);
uint32_t encode_snorm8x4(const float (&value)[4]);
uint32_t encode_unorm8x4(const float (&value)[4]);

} // namespace de::cooker
