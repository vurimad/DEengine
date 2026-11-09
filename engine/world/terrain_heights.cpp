#include "world/terrain_heights.h"

#include "asset/format.h"
#include "core/log.h"
#include "core/profile.h"
#include "platform/time.h"
#include "platform/vfs.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace de::world {

namespace {

// A read of part of a file, waited for: for tools, never the frame.
bool read_now(platform::Vfs& vfs, const char* path, uint64_t offset, std::span<std::byte> dest) {
    const core::Result<platform::ReadHandle> handle =
        vfs.read({.path = path, .dest = dest, .offset = offset, .size = dest.size()});
    if (!handle) {
        return false;
    }
    for (platform::ReadStatus status = vfs.status(*handle);
         status == platform::ReadStatus::Pending || status == platform::ReadStatus::InFlight;
         status = vfs.status(*handle)) {
        platform::sleep_ns(50'000);
    }
    const core::Result<size_t> read = vfs.finish(*handle);
    return read && *read == dest.size();
}

} // namespace

void TerrainHeights::set_terrain(const asset::TerrainAsset* terrain) {
    if (terrain == terrain_ && (terrain == nullptr || terrain->heights_hash == heights_hash_)) {
        return;
    }
    terrain_ = terrain;
    heights_hash_ = terrain != nullptr ? terrain->heights_hash : 0;
    header_checked_ = false;
    header_ok_ = false;
    cache_.clear();
}

bool TerrainHeights::heights_ready() {
    if (header_checked_) {
        return header_ok_;
    }
    header_checked_ = true;
    const asset::CookedPath path = asset::cooked_path(terrain_->header.id, asset::AssetType::TerrainHeights);
    asset::AssetHeader header;
    const core::Result<platform::FileInfo> info = vfs_->file_info(path.c_str());
    if (!info || !read_now(*vfs_, path.c_str(), 0, std::as_writable_bytes(std::span(&header, 1)))) {
        DE_LOG_WARN("world", "the ground's heights can't be read: %s isn't there (run the cooker)", path.c_str());
        return false;
    }
    if (const core::Result<void> fits = terrain_->check_heights_header(header, info->size); !fits) {
        DE_LOG_WARN("world", "the ground's heights can't be used: %s: %s (run the cooker)", path.c_str(),
                    fits.error().message);
        return false;
    }
    header_ok_ = true;
    return true;
}

const uint16_t* TerrainHeights::block(uint32_t index) {
    ++clock_;
    for (Cached& cached : cache_) {
        if (cached.block == index) {
            cached.used = clock_;
            return cached.samples;
        }
    }
    if (!heights_ready()) {
        return nullptr;
    }
    // A new one, or the one asked about least recently.
    Cached* slot = nullptr;
    if (cache_.size() < kCachedBlocks) {
        slot = &cache_.push(Cached{});
    } else {
        slot = &*std::ranges::min_element(cache_, {}, &Cached::used);
    }
    const asset::CookedPath path = asset::cooked_path(terrain_->header.id, asset::AssetType::TerrainHeights);
    if (!read_now(*vfs_, path.c_str(), asset::terrain_block_offset(index),
                  std::as_writable_bytes(std::span(slot->samples)))) {
        slot->block = asset::kTerrainNoBlock;
        return nullptr;
    }
    slot->block = index;
    slot->used = clock_;
    return slot->samples;
}

bool TerrainHeights::height_at(double x, double z, double& height_m) {
    if (terrain_ == nullptr) {
        return false;
    }
    const double east = x - terrain_->origin_m[0];
    const double south = z - terrain_->origin_m[1];
    if (!(east >= 0.0 && south >= 0.0 && east <= terrain_->width_m && south <= terrain_->depth_m)) {
        return false;
    }
    const std::span<const asset::TerrainLevel> levels = terrain_->levels.get(terrain_);
    const std::span<const asset::TerrainNode> nodes = terrain_->nodes.get(terrain_);
    // The finest level with a block there.
    for (size_t l = levels.size(); l-- > 0;) {
        const asset::TerrainLevel& level = levels[l];
        const double spacing = level.spacing_m;
        const double node_m = spacing * asset::kTerrainNodeQuads;
        const auto node_x = static_cast<uint32_t>(std::min<double>(std::floor(east / node_m), level.nodes_x - 1.0));
        const auto node_z = static_cast<uint32_t>(std::min<double>(std::floor(south / node_m), level.nodes_z - 1.0));
        const asset::TerrainNode& node = nodes[level.first_node + (node_z * level.nodes_x) + node_x];
        if (node.block == asset::kTerrainNoBlock) {
            continue;
        }
        const uint16_t* samples = block(node.block);
        if (samples == nullptr) {
            return false;
        }
        // Between the four samples around it, in the node's own (its border kept out).
        const double u = (east - (node_x * node_m)) / spacing;
        const double v = (south - (node_z * node_m)) / spacing;
        const auto column = static_cast<uint32_t>(std::clamp(std::floor(u), 0.0, asset::kTerrainNodeQuads - 1.0));
        const auto row = static_cast<uint32_t>(std::clamp(std::floor(v), 0.0, asset::kTerrainNodeQuads - 1.0));
        const double fu = std::clamp(u - column, 0.0, 1.0);
        const double fv = std::clamp(v - row, 0.0, 1.0);
        const auto sample = [&](uint32_t r, uint32_t c) {
            return static_cast<double>(samples[((r + asset::kTerrainNodeBorder) * asset::kTerrainNodeSamples) + c +
                                               asset::kTerrainNodeBorder]);
        };
        const double top = sample(row, column) + ((sample(row, column + 1) - sample(row, column)) * fu);
        const double bottom = sample(row + 1, column) + ((sample(row + 1, column + 1) - sample(row + 1, column)) * fu);
        const double steps = node.base_steps + top + ((bottom - top) * fv);
        height_m = steps * (spacing / asset::kTerrainStepsPerSpacing);
        return true;
    }
    return false;
}

bool TerrainHeights::ray_cast(const DVec3& origin, const Vec3& direction, double max_m, DVec3& hit) {
    DE_PROFILE_ZONE("TerrainHeights::ray_cast");
    if (terrain_ == nullptr) {
        return false;
    }
    const DVec3 along(direction);
    // Above the ground minus how high the ray is: where it turns negative, the ray's gone under.
    const auto above = [&](double t, double& gap) {
        const DVec3 at = origin + (along * t);
        double ground = 0.0;
        if (!height_at(at.x, at.z, ground)) {
            return false;
        }
        gap = at.y - ground;
        return true;
    };
    // Short steps near, longer far, where a miss by a little matters less; then halving the step that crossed.
    double t = 0.0;
    double previous_t = -1.0;
    double previous_gap = 0.0;
    while (t <= max_m) {
        double gap = 0.0;
        if (above(t, gap)) {
            if (gap <= 0.0) {
                if (previous_t < 0.0 || previous_gap <= 0.0) {
                    previous_t = -1.0; // under the ground from where it starts: not a coming down
                } else {
                    double low = previous_t;
                    double high = t;
                    for (int i = 0; i < 32; ++i) {
                        const double middle = (low + high) * 0.5;
                        double middle_gap = 0.0;
                        if (above(middle, middle_gap) && middle_gap > 0.0) {
                            low = middle;
                        } else {
                            high = middle;
                        }
                    }
                    hit = origin + (along * high);
                    return true;
                }
            } else {
                previous_t = t;
                previous_gap = gap;
            }
        } else {
            previous_t = -1.0; // off the map
        }
        t += std::clamp(t * 0.01, 0.25, 8.0);
    }
    return false;
}

} // namespace de::world
