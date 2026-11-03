#include "asset/cell_format.h"

#include <cmath>

namespace de::asset {

core::Result<void> CellAsset::validate(std::span<const std::byte> file) const {
    using core::ErrorCode;
    if (!span_in_file(entities, file.size()) || !span_in_file(names, file.size())) {
        return core::fail(ErrorCode::CorruptData, "cell array lies outside the file");
    }
    const std::span<const AssetRef> dependencies = header.dependencies.get(file.data());
    const std::span<const CellEntity> all = entities.get(file.data());
    for (size_t i = 0; i < all.size(); ++i) {
        const CellEntity& entity = all[i];
        if (entity.parent != CellEntity::kNone && entity.parent >= i) {
            return core::fail(ErrorCode::CorruptData, "cell entity's parent doesn't come before it");
        }
        if (entity.mesh != CellEntity::kNone &&
            (entity.mesh >= dependencies.size() || dependencies[entity.mesh].type != AssetType::Mesh)) {
            return core::fail(ErrorCode::CorruptData, "cell entity's mesh isn't one of the cell's dependencies");
        }
        if (static_cast<uint64_t>(entity.name_offset) + entity.name_length > names.count) {
            return core::fail(ErrorCode::CorruptData, "cell entity's name lies outside the names");
        }
        bool finite = true;
        for (const double value : entity.position) {
            finite = finite && std::isfinite(value);
        }
        for (const float value : entity.rotation) {
            finite = finite && std::isfinite(value);
        }
        for (const float value : entity.scale) {
            finite = finite && std::isfinite(value);
        }
        if (!finite) {
            return core::fail(ErrorCode::CorruptData, "cell entity's transform isn't finite");
        }
    }
    return {};
}

} // namespace de::asset
