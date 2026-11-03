#include "asset/environment_format.h"

#include <cmath>

namespace de::asset {

namespace {

bool finite_at_least(float value, float least) {
    return std::isfinite(value) && value >= least;
}

bool within(float value, float least, float most) {
    return finite_at_least(value, least) && value <= most;
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
    using L = ShadowLimits;
    const bool shadows = shadow_resolution >= L::kMinResolution && shadow_resolution <= L::kMaxResolution &&
                         within(shadow_distance_m, L::kMinDistanceM, L::kMaxDistanceM) &&
                         within(shadow_split_lambda, 0.0f, 1.0f) && within(shadow_blend_band, 0.0f, L::kMaxBlendBand) &&
                         within(shadow_caster_reach_m, 0.0f, L::kMaxCasterReachM) &&
                         within(shadow_penumbra_per_m, 0.0f, L::kMaxPenumbraPerM) &&
                         within(shadow_max_penumbra_texels, L::kMinPenumbraTexels, L::kMaxPenumbraTexels);
    if (!shadows) {
        return core::fail(ErrorCode::CorruptData, "environment's shadow settings are out of range");
    }
    using F = FogLimits;
    bool fog = within(fog_density_per_m, 0.0f, F::kMaxDensityPerM) &&
               within(fog_height_density_per_m, 0.0f, F::kMaxDensityPerM) &&
               within(fog_height_falloff_per_m, F::kMinFalloffPerM, F::kMaxFalloffPerM) &&
               within(fog_base_height_m, -F::kMaxBaseHeightM, F::kMaxBaseHeightM) &&
               within(fog_sun_height_attenuation, 0.0f, F::kMaxAttenuation) &&
               within(fog_ambient_height_attenuation, 0.0f, F::kMaxAttenuation) &&
               within(fog_start_m, 0.0f, F::kMaxDistanceM) && within(fog_sun_start_m, 0.0f, F::kMaxDistanceM) &&
               within(fog_end_m, 0.0f, F::kMaxDistanceM) && fog_end_m > fog_start_m;
    for (int i = 0; i < 3; ++i) {
        fog = fog && within(fog_extinction_tint[i], F::kMinExtinctionTint, F::kMaxExtinctionTint) &&
              finite_at_least(fog_sun_forward[i], 0.0f) && finite_at_least(fog_sun_back[i], 0.0f) &&
              finite_at_least(fog_ambient[i], 0.0f);
    }
    if (!fog) {
        return core::fail(ErrorCode::CorruptData, "environment's fog settings are out of range");
    }
    return {};
}

} // namespace de::asset
