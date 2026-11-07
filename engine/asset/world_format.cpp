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
    bool finite = std::isfinite(start_yaw_rad) && std::isfinite(start_pitch_rad);
    for (const double value : start_position) {
        finite = finite && std::isfinite(value);
    }
    if (!finite) {
        return core::fail(ErrorCode::CorruptData, "world's start isn't finite");
    }
    return {};
}

} // namespace de::asset
