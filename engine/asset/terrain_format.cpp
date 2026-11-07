#include "asset/terrain_format.h"

#include "core/allocator.h"
#include "core/array.h"

#include <cmath>

namespace de::asset {

namespace {

// How many of a size's nodes it takes to cover it: the grid a level has to have.
double nodes_across(float size_m, float spacing_m) {
    return std::ceil(static_cast<double>(size_m) / (static_cast<double>(spacing_m) * kTerrainNodeQuads));
}

} // namespace

core::Result<void> TerrainAsset::validate(std::span<const std::byte> file) const {
    using core::ErrorCode;
    if (!span_in_file(levels, file.size()) || !span_in_file(nodes, file.size()) ||
        !span_in_file(patches, file.size())) {
        return core::fail(ErrorCode::CorruptData, "terrain's tables lie outside the file");
    }
    if (!std::isfinite(origin_m[0]) || !std::isfinite(origin_m[1])) {
        return core::fail(ErrorCode::CorruptData, "terrain's origin isn't finite");
    }
    if (!std::isfinite(width_m) || !std::isfinite(depth_m) || width_m <= 0.0f || depth_m <= 0.0f) {
        return core::fail(ErrorCode::CorruptData, "terrain has no size");
    }
    if (levels.count == 0 || levels.count > kTerrainMaxLevels) {
        return core::fail(ErrorCode::CorruptData, "terrain has no levels, or more than it can have");
    }

    const std::span<const TerrainLevel> all_levels = levels.get(file.data());
    uint64_t next_node = 0;
    float spacing_m = kTerrainRootSpacingM;
    for (const TerrainLevel& level : all_levels) {
        if (level.spacing_m != spacing_m) {
            return core::fail(ErrorCode::CorruptData, "terrain's levels don't halve their spacing from the root's");
        }
        if (static_cast<double>(level.nodes_x) != nodes_across(width_m, spacing_m) ||
            static_cast<double>(level.nodes_z) != nodes_across(depth_m, spacing_m)) {
            return core::fail(ErrorCode::CorruptData, "a terrain level's grid isn't the nodes its size touches");
        }
        // Checked as it goes: nothing here can add up past what nodes holds, so nothing wraps.
        const uint64_t count = static_cast<uint64_t>(level.nodes_x) * level.nodes_z;
        if (level.first_node != next_node || count > nodes.count - next_node ||
            level.first_patch != next_node * kTerrainPatchesPerNode * kTerrainPatchesPerNode) {
            return core::fail(ErrorCode::CorruptData, "terrain's nodes and patches don't each belong to one level");
        }
        next_node += count;
        spacing_m /= 2.0f;
    }
    if (next_node != nodes.count || patches.count != next_node * kTerrainPatchesPerNode * kTerrainPatchesPerNode) {
        return core::fail(ErrorCode::CorruptData, "terrain's nodes and patches don't each belong to one level");
    }

    // Every block used once means no more blocks than nodes, which the file holds: this can't ask for much.
    if (block_count > nodes.count) {
        return core::fail(ErrorCode::CorruptData, "terrain has more blocks than nodes");
    }
    core::Array<uint64_t> used(core::heap_allocator(core::MemoryTag::Streaming));
    used.resize((static_cast<size_t>(block_count) + 63) / 64);
    uint32_t blocks_used = 0;
    for (const TerrainNode& node : nodes.get(file.data())) {
        if (node.block == kTerrainNoBlock) {
            continue;
        }
        if (node.block >= block_count) {
            return core::fail(ErrorCode::CorruptData, "a terrain node's block lies past the heights file");
        }
        uint64_t& word = used[node.block / 64];
        const uint64_t bit = uint64_t{1} << (node.block % 64);
        if ((word & bit) != 0) {
            return core::fail(ErrorCode::CorruptData, "two terrain nodes share a block");
        }
        word |= bit;
        ++blocks_used;
        if (node.base_steps > INT32_MAX - static_cast<int32_t>(UINT16_MAX)) {
            return core::fail(ErrorCode::CorruptData, "a terrain node's samples would overflow its base");
        }
    }
    if (blocks_used != block_count) {
        return core::fail(ErrorCode::CorruptData, "a terrain block belongs to no node");
    }

    for (const TerrainPatch& patch : patches.get(file.data())) {
        if (!std::isfinite(patch.min_m) || !std::isfinite(patch.max_m) || patch.min_m > patch.max_m) {
            return core::fail(ErrorCode::CorruptData, "a terrain patch's heights aren't finite and in order");
        }
    }
    return {};
}

core::Result<void> TerrainAsset::check_heights_header(const AssetHeader& heights, uint64_t file_bytes) const {
    using core::ErrorCode;
    if (heights.magic != kAssetMagic || heights.type != AssetType::TerrainHeights) {
        return core::fail(ErrorCode::CorruptData, "not a terrain heights file");
    }
    if (heights.format_version != kFormatVersion || heights.platform != Platform::Pc) {
        return core::fail(ErrorCode::Unsupported, "terrain heights file has another format version or platform");
    }
    if (heights.id != header.id || heights.content_hash != heights_hash) {
        return core::fail(ErrorCode::CorruptData, "terrain heights file isn't the one this terrain was cooked with");
    }
    if (heights.file_size != file_bytes || file_bytes != terrain_block_offset(block_count)) {
        return core::fail(ErrorCode::CorruptData, "terrain heights file isn't as long as its blocks (truncated?)");
    }
    return {};
}

} // namespace de::asset
