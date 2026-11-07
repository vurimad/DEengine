#pragma once

#include "asset/collision_format.h"
#include "cooker/settings.h"

#include <span>

namespace de::cooker {

// The .dephys of a cooked mesh (.demesh, as cook_mesh made it): static level collision from a simplified mesh
// (docs/design.md, Physics). From the finest LOD with at most settings.collision_triangles triangles: the closest to
// what's drawn that collision can afford. With none that small, the coarsest, and a warning. All its parts in one, with
// every position once: LODs split vertices along uv seams and between materials, and Jolt would take each of those for
// an open edge, for what slides across them to catch on.
//
// A mesh Jolt can't make a shape of has no collision, which isn't an error for something that may only be drawn: it's
// a warning, and failure here. An error is reported only for a mesh file that can't be read.
core::Result<core::Array<std::byte>> cook_collision(std::span<const std::byte> mesh_file, const MeshSettings& settings,
                                                    asset::AssetId id, std::string_view path, Diagnostics& diagnostics);

} // namespace de::cooker
