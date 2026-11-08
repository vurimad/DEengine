#include "asset/terrain_layers_format.h"

#include <algorithm>
#include <cmath>

namespace de::asset {

namespace {

bool within(float value, float least, float most) {
    return std::isfinite(value) && value >= least && value <= most;
}

bool layer_numbers_fit(const TerrainLayer& layer) {
    // The cook's own limits: a cooked file past them was changed, or cooked by something else.
    constexpr float kMostTileM = 4096.0f;
    constexpr float kLeastTileM = 1.0f / 16.0f;
    constexpr float kMostNoise = 16.0f;
    constexpr float kMostHeightM = 1e6f;
    constexpr float kLeastRange = 1e-3f; // a channel rule's ends: the renderer divides by how far apart they are
    const auto rises = [](const float (&rule)[4]) {
        return rule[0] <= rule[1] && rule[1] <= rule[2] && rule[2] <= rule[3];
    };
    const auto falls = [](const float (&rule)[4]) {
        return rule[0] >= rule[1] && rule[1] >= rule[2] && rule[2] >= rule[3];
    };
    bool fits = within(layer.tile_m, kLeastTileM, kMostTileM) && within(layer.noise, 0.0f, kMostNoise) &&
                within(layer.noise_large, 0.0f, 1.0f) && within(layer.variation, 0.0f, 1.0f) &&
                within(layer.mask_from, 0.0f, 1.0f) && within(layer.mask_to, 0.0f, 1.0f) &&
                std::abs(layer.mask_to - layer.mask_from) >= kLeastRange && layer.mask <= kTerrainNoMask &&
                within(layer.detail_from, 0.0f, 1.0f) && within(layer.detail_to, 0.0f, 1.0f) &&
                std::abs(layer.detail_to - layer.detail_from) >= kLeastRange && layer.detail <= kTerrainNoMask &&
                (layer.flags & ~TerrainLayerFlags::DetailCoverage) == TerrainLayerFlags::None &&
                (!core::has_any(layer.flags, TerrainLayerFlags::DetailCoverage) || layer.detail < kTerrainNoMask) &&
                (layer.projection == TerrainProjection::Top || layer.projection == TerrainProjection::Sides);
    fits = fits &&
           std::ranges::all_of(layer.height_m, [](float h) { return within(h, -kMostHeightM, kMostHeightM); }) &&
           std::ranges::all_of(layer.slope_cos, [](float c) { return within(c, -2.0f, 2.0f); }) &&
           std::ranges::all_of(layer.tint, [](float c) { return within(c, 0.0f, 16.0f); });
    return fits && rises(layer.height_m) && falls(layer.slope_cos);
}

bool grass_numbers_fit(const TerrainGrass& grass) {
    constexpr float kMostDensity = 2048.0f;
    constexpr float kMostHeightM = 16.0f;
    constexpr float kMostWidthM = 2.0f;
    constexpr float kMostRuleM = 1e6f;
    const auto color_fits = [](const float (&color)[3]) {
        return std::ranges::all_of(color, [](float c) { return within(c, 0.0f, 16.0f); });
    };
    return within(grass.density_per_m2, 0.0f, kMostDensity) && within(grass.height_m[0], 0.0f, kMostHeightM) &&
           within(grass.height_m[1], grass.height_m[0], kMostHeightM) && within(grass.width_m, 0.0f, kMostWidthM) &&
           within(grass.patches, 0.0f, 1.0f) && grass.excluded_masks < (1U << kTerrainNoMask) &&
           color_fits(grass.color_base) && color_fits(grass.color_tip) && color_fits(grass.other_base) &&
           color_fits(grass.other_tip) && grass.other_mask <= kTerrainNoMask &&
           std::ranges::all_of(
               grass.where_height_m, [](float h) { return within(h, -kMostRuleM, kMostRuleM); }) &&
           std::ranges::all_of(
               grass.where_slope_cos, [](float c) { return within(c, -2.0f, 2.0f); }) &&
           grass.where_height_m[0] <= grass.where_height_m[1] && grass.where_height_m[1] <= grass.where_height_m[2] &&
           grass.where_height_m[2] <= grass.where_height_m[3] && grass.where_slope_cos[0] >= grass.where_slope_cos[1] &&
           grass.where_slope_cos[1] >= grass.where_slope_cos[2] && grass.where_slope_cos[2] >= grass.where_slope_cos[3];
}

} // namespace

core::Result<void> TerrainLayersAsset::validate(std::span<const std::byte> file) const {
    using core::ErrorCode;
    if (!span_in_file(layers, file.size())) {
        return core::fail(ErrorCode::CorruptData, "terrain layers lie outside the file");
    }
    if (layers.count > kTerrainMaxLayers) {
        return core::fail(ErrorCode::CorruptData, "more terrain layers than a look can have");
    }
    const std::span<const AssetRef> dependencies = header.dependencies.get(file.data());
    const auto is_material = [&](uint32_t index) {
        return index < dependencies.size() && dependencies[index].type == AssetType::Material;
    };
    if (masks != kNone && !is_material(masks)) {
        return core::fail(ErrorCode::CorruptData, "terrain masks aren't a material among the dependencies");
    }
    if (details != kNone && !is_material(details)) {
        return core::fail(ErrorCode::CorruptData, "terrain details aren't a material among the dependencies");
    }
    if (lighting != kNone && !is_material(lighting)) {
        return core::fail(ErrorCode::CorruptData, "terrain lighting isn't a material among the dependencies");
    }
    if (normals != kNone && !is_material(normals)) {
        return core::fail(ErrorCode::CorruptData, "terrain normals aren't a material among the dependencies");
    }
    if (colors != kNone && !is_material(colors)) {
        return core::fail(ErrorCode::CorruptData, "terrain colors aren't a material among the dependencies");
    }
    if (!within(blend, 0.0f, 1.0f)) {
        return core::fail(ErrorCode::CorruptData, "terrain layers' blend is out of range");
    }
    if (!grass_numbers_fit(grass)) {
        return core::fail(ErrorCode::CorruptData, "terrain grass has a number out of range or a rule out of order");
    }
    for (const TerrainLayer& layer : layers.get(file.data())) {
        if (!is_material(layer.material)) {
            return core::fail(ErrorCode::CorruptData, "a terrain layer's material isn't among the dependencies");
        }
        if (!layer_numbers_fit(layer)) {
            return core::fail(ErrorCode::CorruptData,
                              "a terrain layer has a number out of range or a rule out of order");
        }
    }
    return {};
}

} // namespace de::asset
