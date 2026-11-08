#pragma once

#include "asset/format.h"

#include <cstdint>

namespace de::asset {

// The range of each sun shadow setting: what the cooker accepts, loading checks, and the renderer
// holds lighting made in code to.
struct ShadowLimits {
    static constexpr uint32_t kMinResolution = 64; // per cascade
    static constexpr uint32_t kMaxResolution = 8192;
    static constexpr float kMinDistanceM = 1.0f;
    static constexpr float kMaxDistanceM = 1e5f;
    static constexpr float kMaxBlendBand = 0.5f; // split_lambda is 0 to 1
    static constexpr float kMaxCasterReachM = 1e5f;
    static constexpr float kMaxPenumbraPerM = 10.0f;
    static constexpr float kMinPenumbraTexels = 1.0f;
    static constexpr float kMaxPenumbraTexels = 64.0f;
};

// The range of each fog setting (render::SceneFog), as for the shadows.
struct FogLimits {
    static constexpr float kMaxDensityPerM = 1.0f;  // constant and height fog extinction
    static constexpr float kMaxFalloffPerM = 10.0f; // the height fog's falloff (at least 1e-4)
    static constexpr float kMinFalloffPerM = 1e-4f;
    static constexpr float kMaxBaseHeightM = 1e5f; // either side of 0
    static constexpr float kMinExtinctionTint = 0.01f;
    static constexpr float kMaxExtinctionTint = 100.0f;
    static constexpr float kMaxAttenuation = 100.0f;      // sun and ambient height attenuation
    static constexpr float kMaxDistanceM = 1e6f;          // start, sun start and end
    static constexpr float kMaxVolumeDistanceM = 2000.0f; // the froxel grid's reach (0: no grid)
};

// The range of each wind setting (render::SceneWind), as for the shadows.
struct WindLimits {
    static constexpr float kMaxSpeedMps = 60.0f; // the steady wind, and what a gust adds to it
    static constexpr float kMinGustSizeM = 1.0f;
    static constexpr float kMaxGustSizeM = 10000.0f;
};

// The range of each sea setting (render::SceneSea), as for the wind.
struct SeaLimits {
    static constexpr float kMaxSwellHeightM = 15.0f;
    static constexpr float kMinSwellPeriodS = 3.0f;
    static constexpr float kMaxSwellPeriodS = 25.0f;
    static constexpr float kMinFetchM = 1000.0f;
    static constexpr float kMaxFetchM = 2000000.0f;
    static constexpr float kMaxChoppiness = 1.5f;
};

// .deenv: the binary form of a .environment text source. How a place is lit (docs/design.md,
// Rendering; Sky, atmosphere & weather): the sun, the ambient from sky and ground, the tint of
// shadowed sunlight, exposure, the sun's shadows and the fog. Colors are linear, multiplied by their
// intensities already; relative to exposure (a sun of 4 at 0 EV is a bright day). The renderer
// reads it as render::SceneLighting (render::scene_lighting()).
struct EnvironmentAsset {
    static constexpr AssetType kType = AssetType::Environment;
    static constexpr uint16_t kFormatVersion = 6; // 5: wind; 6: the sea

    AssetHeader header;
    float sun_direction[3] = {0, 1, 0}; // toward the sun, unit length
    float exposure_ev = 0.0f;
    float sun_color[3] = {};
    float shadow_color[3] = {}; // what shadowed sunlight keeps
    float sky_color[3] = {};    // ambient from above
    float ground_color[3] = {}; // ambient from below
    // The sun's shadows (render::SceneShadows).
    uint32_t shadow_resolution = 2048; // per cascade, 64 to 8192
    float shadow_distance_m = 0.0f;
    float shadow_split_lambda = 0.0f; // 0 to 1
    float shadow_blend_band = 0.0f;   // 0 to 0.5
    float shadow_caster_reach_m = 0.0f;
    float shadow_penumbra_per_m = 0.0f;
    float shadow_max_penumbra_texels = 1.0f; // at least 1
    // Fog (render::SceneFog).
    float fog_density_per_m = 0.0f;
    float fog_height_density_per_m = 0.0f;
    float fog_height_falloff_per_m = 0.05f;
    float fog_base_height_m = 0.0f;
    float fog_extinction_tint[3] = {1.0f, 1.0f, 1.0f};
    float fog_sun_forward[3] = {}; // colours times their intensities
    float fog_sun_back[3] = {};
    float fog_ambient[3] = {};
    float fog_sun_height_attenuation = 1.0f;
    float fog_ambient_height_attenuation = 1.0f;
    float fog_start_m = 0.0f;
    float fog_sun_start_m = 0.0f;
    float fog_end_m = 20000.0f;         // past fog_start_m
    float fog_volume_distance_m = 0.0f; // the froxel grid's reach; 0: no grid
    // How much of the height fog is left with the camera low (render::SceneFog); 1: all of it.
    float fog_low_camera_keep = 1.0f; // 0 to 1
    float fog_low_camera_below_m = 0.0f;
    float fog_low_camera_above_m = 0.0f; // at least fog_low_camera_below_m
    // The wind (render::SceneWind): what grass and plants move in.
    float wind_direction[2] = {1.0f, 0.0f}; // toward where it blows, on the ground (x, z), unit length
    float wind_speed_mps = 0.0f;            // the steady wind. 0 and no gusts: still air
    float wind_gust_mps = 0.0f;             // what a gust adds to it
    float wind_gust_size_m = 40.0f;         // across a patch of gusting air, which travels with the wind
    float wind_turbulence = 0.0f;           // 0 to 1: how far it swirls off its way
    // The sea (render::SceneSea): the swell come in from far off, and the waves the wind raises over its fetch.
    float sea_swell_height_m = 0.0f;              // significant height; 0: no swell
    float sea_swell_period_s = 10.0f;             // its peak period
    float sea_swell_direction[2] = {0.0f, -1.0f}; // toward where it travels (x, z), unit length
    float sea_fetch_m = 100000.0f;                // how far the wind has blown over open water
    float sea_choppiness = 0.8f;                  // how far crests sharpen, 0 (round) to SeaLimits::kMaxChoppiness

    // Finite values in range: a unit sun direction, colors not negative, the shadow settings
    // within ShadowLimits, the fog's within FogLimits, the wind's within WindLimits and its direction unit length, the
    // sea's within SeaLimits and its swell's direction unit length.
    core::Result<void> validate(std::span<const std::byte> file) const;
};
static_assert(sizeof(EnvironmentAsset) == 304);

} // namespace de::asset
