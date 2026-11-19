#pragma once

#include <cmath>
#include <cstdint>

namespace de::asset {

// The range of a local light's values: what the cooker accepts and loading checks (docs/design.md, Local lights).
struct LightLimits {
    static constexpr float kMaxIntensity = 100000.0f;
    static constexpr float kMaxRadiusM = 1000.0f;
    static constexpr float kMaxConeDeg = 89.0f; // from its axis: a spot is narrower than a half space
    static constexpr float kMaxFlickerHz = 60.0f;
};

// A local light as a file keeps it (a cell's entity, a prefab's part): a point that shines every way, or with a
// cone a spot along its entity's -Z. Its place and turn are its entity's.
struct LightDesc {
    float color[3] = {1.0f, 1.0f, 1.0f}; // linear
    // What it gives a surface facing it from 1 m, in the sun's units (relative to exposure: a light of 5 lights a
    // wall a metre away as a sun of 5 does). It falls with the square of the distance.
    float intensity = 1.0f;
    float radius_m = 4.0f;   // where its light has come down to nothing: what it's culled by
    float cone_deg = 0.0f;   // 0: a point light; else a spot's angle from its axis to where it ends
    float cone_soft = 0.25f; // how much of that angle is the fade at its edge: 0 a hard edge, 1 all of it
    float flicker = 0.0f;    // 0: steady, to 1: the share of its light a flicker takes away at most
    float flicker_hz = 8.0f; // how fast it flickers
    // How much of a shadow it casts, 0 to 1: 1 all of it; 0 none, and it takes no place among the lights that have
    // shadows (docs/design.md, Local lights: shadows cost by the lights that cast them, and of a cluster of candles
    // one casting is nearly the shadow of all).
    float shadow = 1.0f;

    // Finite, and within the limits.
    bool valid() const {
        for (const float channel : color) {
            if (!std::isfinite(channel) || channel < 0.0f) {
                return false;
            }
        }
        return std::isfinite(intensity) && intensity >= 0.0f && intensity <= LightLimits::kMaxIntensity &&
               std::isfinite(radius_m) && radius_m > 0.0f && radius_m <= LightLimits::kMaxRadiusM &&
               std::isfinite(cone_deg) && cone_deg >= 0.0f && cone_deg <= LightLimits::kMaxConeDeg &&
               std::isfinite(cone_soft) && cone_soft >= 0.0f && cone_soft <= 1.0f && std::isfinite(flicker) &&
               flicker >= 0.0f && flicker <= 1.0f && std::isfinite(flicker_hz) && flicker_hz >= 0.0f &&
               flicker_hz <= LightLimits::kMaxFlickerHz && std::isfinite(shadow) && shadow >= 0.0f && shadow <= 1.0f;
    }
};
static_assert(sizeof(LightDesc) == 40);

} // namespace de::asset
