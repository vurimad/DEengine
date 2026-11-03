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

// .deenv: the binary form of a .environment text source. How a place is lit (docs/design.md,
// Rendering; Sky, atmosphere & weather): the sun, the ambient from sky and ground, the tint of
// shadowed sunlight, exposure, and the sun's shadows. Colors are linear, multiplied by their
// intensities already; relative to exposure (a sun of 4 at 0 EV is a bright day). The renderer
// reads it as render::SceneLighting (render::scene_lighting()).
struct EnvironmentAsset {
    static constexpr AssetType kType = AssetType::Environment;
    static constexpr uint16_t kFormatVersion = 1;

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
    uint32_t reserved = 0;

    // Finite values in range: a unit sun direction, colors not negative, the shadow settings
    // within ShadowLimits.
    core::Result<void> validate(std::span<const std::byte> file) const;
};
static_assert(sizeof(EnvironmentAsset) == 160);

} // namespace de::asset
