#include "asset/cell_format.h"

#include <cmath>

namespace de::asset {

core::Result<void> CellAsset::validate(std::span<const std::byte> file) const {
    using core::ErrorCode;
    if (!span_in_file(entities, file.size()) || !span_in_file(names, file.size()) ||
        !span_in_file(fog_volumes, file.size()) || !span_in_file(water_bodies, file.size()) ||
        !span_in_file(lights, file.size())) {
        return core::fail(ErrorCode::CorruptData, "cell array lies outside the file");
    }
    const std::span<const AssetRef> dependencies = header.dependencies.get(file.data());
    const std::span<const CellEntity> all = entities.get(file.data());
    for (const CellFogVolume& volume : fog_volumes.get(file.data())) {
        const bool good =
            std::isfinite(volume.density_per_m) && volume.density_per_m >= 0.0f &&
            volume.density_per_m <= kMaxFogVolumeDensityPerM && std::isfinite(volume.edge_m) && volume.edge_m >= 0.0f &&
            std::isfinite(volume.tint[0]) && std::isfinite(volume.tint[1]) && std::isfinite(volume.tint[2]) &&
            volume.tint[0] >= 0.0f && volume.tint[1] >= 0.0f && volume.tint[2] >= 0.0f &&
            (volume.shape == CellFogShape::Box || volume.shape == CellFogShape::Ellipsoid) &&
            std::isfinite(volume.noise_size_m) && volume.noise_size_m > 0.0f && std::isfinite(volume.noise_amount) &&
            volume.noise_amount >= 0.0f && volume.noise_amount <= 1.0f && std::isfinite(volume.drift_mps[0]) &&
            std::isfinite(volume.drift_mps[1]) && std::isfinite(volume.drift_mps[2]) &&
            std::isfinite(volume.height_fade_m) && volume.height_fade_m >= 0.0f && std::isfinite(volume.under_roof) &&
            volume.under_roof <= 1.0f;
        if (!good) {
            return core::fail(ErrorCode::CorruptData, "cell fog volume has bad values");
        }
    }
    for (const CellWaterBody& water : water_bodies.get(file.data())) {
        if ((water.shape != CellWaterShape::Rectangle && water.shape != CellWaterShape::Ellipse) ||
            !std::isfinite(water.waves) || water.waves < 0.0f || water.waves > 1.0f) {
            return core::fail(ErrorCode::CorruptData, "cell water body has bad values");
        }
    }
    for (const LightDesc& light : lights.get(file.data())) {
        if (!light.valid()) {
            return core::fail(ErrorCode::CorruptData, "cell light has bad values");
        }
    }
    for (size_t i = 0; i < all.size(); ++i) {
        const CellEntity& entity = all[i];
        if (entity.parent != CellEntity::kNone && entity.parent >= i) {
            return core::fail(ErrorCode::CorruptData, "cell entity's parent doesn't come before it");
        }
        if (entity.source_index >= all.size()) {
            return core::fail(ErrorCode::CorruptData, "cell entity's place in its source is past the end");
        }
        if (entity.mesh != CellEntity::kNone &&
            (entity.mesh >= dependencies.size() || dependencies[entity.mesh].type != AssetType::Mesh)) {
            return core::fail(ErrorCode::CorruptData, "cell entity's mesh isn't one of the cell's dependencies");
        }
        if (entity.collision != CellEntity::kNone &&
            (entity.collision >= dependencies.size() || dependencies[entity.collision].type != AssetType::Collision)) {
            return core::fail(ErrorCode::CorruptData, "cell entity's collision isn't one of the cell's dependencies");
        }
        if (entity.effect != CellEntity::kNone &&
            (entity.effect >= dependencies.size() || dependencies[entity.effect].type != AssetType::Effect)) {
            return core::fail(ErrorCode::CorruptData, "cell entity's effect isn't one of the cell's dependencies");
        }
        if (entity.prefab != CellEntity::kNone &&
            (entity.prefab >= dependencies.size() || dependencies[entity.prefab].type != AssetType::Prefab)) {
            return core::fail(ErrorCode::CorruptData, "cell entity's prefab isn't one of the cell's dependencies");
        }
        if (entity.fog_volume != CellEntity::kNone && entity.fog_volume >= fog_volumes.count) {
            return core::fail(ErrorCode::CorruptData, "cell entity's fog volume isn't in the cell");
        }
        if (entity.water_body != CellEntity::kNone && entity.water_body >= water_bodies.count) {
            return core::fail(ErrorCode::CorruptData, "cell entity's water body isn't in the cell");
        }
        if (entity.light != CellEntity::kNone && entity.light >= lights.count) {
            return core::fail(ErrorCode::CorruptData, "cell entity's light isn't in the cell");
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
