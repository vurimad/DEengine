#pragma once

#include "asset/format.h"

#include <cstdint>

namespace de::asset {

// Terrain on disk (docs/design.md, Terrain & vegetation): a pyramid of square nodes, the same number of samples in
// each, where each level has half the spacing of the one above. A node's heights are a fixed-size block in a file of
// their own, read one block at a time, so what's loaded can follow the camera. The rest (what's where, and how high it
// reaches) is small and stays loaded, so the whole map can always be reasoned about.

// Squares along a node's side.
inline constexpr uint32_t kTerrainNodeQuads = 128;
// Samples kept past each side of a node, from its neighbours: enough for a normal at its edge at twice the spacing.
inline constexpr uint32_t kTerrainNodeBorder = 2;
// Samples along a block's side.
inline constexpr uint32_t kTerrainNodeSamples = kTerrainNodeQuads + 1 + (2 * kTerrainNodeBorder);
// A block's bytes: 16-bit samples, row by row from the north-west, padded to 4 bytes so shaders can read words.
inline constexpr uint64_t kTerrainNodeBytes =
    ((static_cast<uint64_t>(kTerrainNodeSamples) * kTerrainNodeSamples * 2) + 3) & ~static_cast<uint64_t>(3);
// What's culled and drawn is a patch: a node is this many along a side.
inline constexpr uint32_t kTerrainPatchesPerNode = 4;
inline constexpr uint32_t kTerrainPatchQuads = kTerrainNodeQuads / kTerrainPatchesPerNode;
// The coarsest level's spacing, so its nodes are 2,048 m across. Finer levels halve it.
inline constexpr float kTerrainRootSpacingM = 16.0f;
inline constexpr uint32_t kTerrainMaxLevels = 8;
// A node with no block: nothing of the map lies in it.
inline constexpr uint32_t kTerrainNoBlock = UINT32_MAX;

// A sample's height is (base_steps + sample) * step, with a step this share of the level's spacing. Neighbours on a
// level round to the same steps, so a shared edge is the same to the bit. A node holds 1024 spacings of height, a rise
// of 8 to 1 across it; the cooker fails on land steeper than that. 4 to 1 wasn't enough: the first real map's cliffs
// rise 1,081 m across a 2 m node.
inline constexpr float kTerrainStepsPerSpacing = 64.0f;

// One level of the pyramid; finer levels come later in TerrainAsset::levels.
struct TerrainLevel {
    float spacing_m = 0.0f; // between samples
    uint32_t nodes_x = 0;   // this level's grid: every node touching the map
    uint32_t nodes_z = 0;
    uint32_t first_node = 0;  // into TerrainAsset::nodes, z then x
    uint32_t first_patch = 0; // into TerrainAsset::patches, z then x, kTerrainPatchesPerNode times as many along a side
    uint32_t reserved[3] = {};
};
static_assert(sizeof(TerrainLevel) == 32);

struct TerrainNode {
    int32_t base_steps = 0;           // what its samples add to, in its level's steps
    uint32_t block = kTerrainNoBlock; // in the heights file
};
static_assert(sizeof(TerrainNode) == 8);

// How high a patch reaches, in meters, whatever its morph does; for choosing and culling it without its heights.
struct TerrainPatch {
    float min_m = 0.0f;
    float max_m = 0.0f;
};
static_assert(sizeof(TerrainPatch) == 8);

// .deterrain: the binary form of a .terrain text source, with no heights in it. The heights are a second file of the
// same ID (cooked_path(id, AssetType::TerrainHeights)): an AssetHeader with this format's version, then blocks of
// kTerrainNodeBytes in order. The registry never loads that file; what streams terrain reads a block at a time through
// platform::Vfs, at terrain_block_offset(), and checks its header against heights_hash once (check_heights_header).
//
// The map's north-west sample is at origin_m, with X east and Z south. A node's corner is at origin_m + its grid
// position times spacing_m times kTerrainNodeQuads. What's outside width_m and depth_m isn't land: nodes along those
// edges are padded with the nearest sample, and whatever draws them stops at the edge.
struct TerrainAsset {
    static constexpr AssetType kType = AssetType::Terrain;
    static constexpr uint16_t kFormatVersion = 1;

    AssetHeader header;
    double origin_m[2] = {}; // X and Z
    float width_m = 0.0f;    // along X, from the origin
    float depth_m = 0.0f;    // along Z
    uint32_t block_count = 0;
    uint32_t reserved0 = 0;
    uint64_t heights_hash = 0; // the heights file's AssetHeader::content_hash
    OffsetSpan<TerrainLevel> levels;
    OffsetSpan<TerrainNode> nodes;
    OffsetSpan<TerrainPatch> patches;
    uint8_t reserved1[8] = {};

    // Everything inside the file; levels 1 to kTerrainMaxLevels, the first at kTerrainRootSpacingM, each one half the
    // last's spacing, with the grid of nodes its size touches (so at most twice the last's); every node and patch
    // belonging to exactly one level; every block below block_count, used once; a base its samples can add to; every
    // patch's heights finite and in order; a finite origin and a positive size.
    core::Result<void> validate(std::span<const std::byte> file) const;

    // Whether a heights file's header, and its length in bytes, are those of the file this terrain was cooked with. For
    // what streams terrain, which reads only the header, and only once.
    core::Result<void> check_heights_header(const AssetHeader& heights, uint64_t file_bytes) const;
};
static_assert(sizeof(TerrainAsset) == 136);

// Where a block starts in the heights file.
constexpr uint64_t terrain_block_offset(uint32_t block) {
    return sizeof(AssetHeader) + (static_cast<uint64_t>(block) * kTerrainNodeBytes);
}

} // namespace de::asset
