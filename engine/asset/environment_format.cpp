#include "asset/environment_format.h"

#include <cmath>

namespace de::asset {

namespace {

bool finite_at_least(float value, float least) {
    return std::isfinite(value) && value >= least;
}

} // namespace

core::Result<void> EnvironmentAsset::validate(std::span<const std::byte> /*file*/) const {
    using core::ErrorCode;
    const float length_squared =
        sun_direction[0] * sun_direction[0] + sun_direction[1] * sun_direction[1] + sun_direction[2] * sun_direction[2];
    if (!std::isfinite(length_squared) || std::abs(length_squared - 1.0f) > 1e-3f) {
        return core::fail(ErrorCode::CorruptData, "environment's sun direction isn't unit length");
    }
    bool colors = std::isfinite(exposure_ev);
    for (const float* color : {sun_color, shadow_color, sky_color, ground_color}) {
        for (int i = 0; i < 3; ++i) {
            colors = colors && finite_at_least(color[i], 0.0f);
        }
    }
    if (!colors) {
        return core::fail(ErrorCode::CorruptData, "environment has a negative or non-finite color");
    }
    const bool shadows = shadow_resolution >= 64 && shadow_resolution <= 8192 &&
                         finite_at_least(shadow_distance_m, 0.0f) && finite_at_least(shadow_split_lambda, 0.0f) &&
                         shadow_split_lambda <= 1.0f && finite_at_least(shadow_blend_band, 0.0f) &&
                         shadow_blend_band <= 0.5f && finite_at_least(shadow_caster_reach_m, 0.0f) &&
                         finite_at_least(shadow_penumbra_per_m, 0.0f) &&
                         finite_at_least(shadow_max_penumbra_texels, 1.0f);
    if (!shadows) {
        return core::fail(ErrorCode::CorruptData, "environment's shadow settings are out of range");
    }
    return {};
}

} // namespace de::asset
