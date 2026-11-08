#include "asset/world_format.h"

#include <cmath>

namespace de::asset {

core::Result<void> WorldAsset::validate(std::span<const std::byte> file) const {
    using core::ErrorCode;
    if (!span_in_file(cells, file.size())) {
        return core::fail(ErrorCode::CorruptData, "world cells lie outside the file");
    }
    const std::span<const WorldCell> all = cells.get(file.data());
    for (size_t i = 1; i < all.size(); ++i) {
        const WorldCell& a = all[i - 1];
        const WorldCell& b = all[i];
        if (a.coord_z > b.coord_z || (a.coord_z == b.coord_z && a.coord_x >= b.coord_x)) {
            return core::fail(ErrorCode::CorruptData, "world cells aren't sorted by coord, one per coord");
        }
    }
    const std::span<const AssetRef> dependencies = header.dependencies.get(file.data());
    if (environment != kNone &&
        (environment >= dependencies.size() || dependencies[environment].type != AssetType::Environment)) {
        return core::fail(ErrorCode::CorruptData, "world's environment isn't one of its dependencies");
    }
    if (terrain != kNone && (terrain >= dependencies.size() || dependencies[terrain].type != AssetType::Terrain)) {
        return core::fail(ErrorCode::CorruptData, "world's terrain isn't one of its dependencies");
    }
    if (terrain_layers != kNone &&
        (terrain_layers >= dependencies.size() || dependencies[terrain_layers].type != AssetType::TerrainLayers)) {
        return core::fail(ErrorCode::CorruptData, "world's terrain layers aren't one of its dependencies");
    }
    bool finite = std::isfinite(start_yaw_rad) && std::isfinite(start_pitch_rad);
    for (const double value : start_position) {
        finite = finite && std::isfinite(value);
    }
    if (!finite) {
        return core::fail(ErrorCode::CorruptData, "world's start isn't finite");
    }
    if (sea > 1 || !std::isfinite(sea_level_m) || std::abs(sea_level_m) > kMostSeaLevelM) {
        return core::fail(ErrorCode::CorruptData, "world's sea is out of range");
    }
    if (sea_shore != kNone &&
        (sea == 0 || sea_shore >= dependencies.size() || dependencies[sea_shore].type != AssetType::Material)) {
        return core::fail(ErrorCode::CorruptData, "world's shore map isn't a material among its dependencies, or has no sea");
    }
    if (sea_foam != kNone &&
        (sea == 0 || sea_foam >= dependencies.size() || dependencies[sea_foam].type != AssetType::Material)) {
        return core::fail(ErrorCode::CorruptData, "world's sea foam isn't a material among its dependencies, or has no sea");
    }
    return {};
}

} // namespace de::asset
