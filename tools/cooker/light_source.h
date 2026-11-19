#pragma once

#include "asset/light_format.h"
#include "cooker/entity_transform.h"
#include "cooker/json.h"

#include <string_view>

namespace de::cooker {

// A local light, as a cell's entity and a prefab's part say it (docs/design.md, Local lights):
//   "light": {"color": [1, 0.62, 0.3], "intensity": 2, "radius_m": 4,
//             "cone_deg": 25, "cone_soft": 0.3, "flicker": 0.25, "flicker_hz": 9, "shadow": 1}
// color and intensity as an environment's sun (intensity: what it gives a surface facing it from 1 m, in the sun's
// units); radius_m where its light ends; cone_deg makes it a spot along its entity's -Z (the angle from its axis),
// cone_soft how much of that is the fade at its edge; flicker the share of its light a flicker takes away, at
// flicker_hz. Only what's given is read: the rest is LightDesc's own. What's wrong is said to bad(const char*).
template <typename Bad>
void read_light(yyjson_val* object, std::string_view path, Diagnostics& diagnostics, asset::LightDesc& light,
                Bad&& bad) {
    if (!yyjson_is_obj(object)) {
        bad("light should be an object: its color, intensity and radius_m");
        return;
    }
    static constexpr const char* kKeys[] = {"color",     "intensity", "radius_m",   "cone_deg",
                                            "cone_soft", "flicker",   "flicker_hz", "shadow"};
    const JsonReader reader{.object = object, .path = path, .diagnostics = &diagnostics};
    reader.check_keys(kKeys, "light");
    if (reader.has("color")) {
        double color[3] = {};
        if (!read_numbers(yyjson_obj_get(object, "color"), color, 3) || !to_float(color[0], light.color[0]) ||
            !to_float(color[1], light.color[1]) || !to_float(color[2], light.color[2]) || color[0] < 0.0 ||
            color[1] < 0.0 || color[2] < 0.0) {
            bad("light color should be 3 numbers of 0 or more");
        }
    }
    // A number in a range, or the light keeps what it had and the range is said.
    const auto number = [&](const char* key, float& value, double low, double high, bool above_low, const char* wrong) {
        const double read = reader.number(key, static_cast<double>(value));
        float fits = 0.0f;
        if (!to_float(read, fits) || read > high || (above_low ? read <= low : read < low)) {
            bad(wrong);
            return;
        }
        value = fits;
    };
    number("intensity", light.intensity, 0.0, static_cast<double>(asset::LightLimits::kMaxIntensity), false,
           "light intensity should be from 0 to 100000");
    number("radius_m", light.radius_m, 0.0, static_cast<double>(asset::LightLimits::kMaxRadiusM), true,
           "light radius_m should be above 0, up to 1000");
    number("cone_deg", light.cone_deg, 0.0, static_cast<double>(asset::LightLimits::kMaxConeDeg), false,
           "light cone_deg should be from 0 (a point light) to 89");
    number("cone_soft", light.cone_soft, 0.0, 1.0, false, "light cone_soft should be from 0 to 1");
    number("flicker", light.flicker, 0.0, 1.0, false, "light flicker should be from 0 to 1");
    number("flicker_hz", light.flicker_hz, 0.0, static_cast<double>(asset::LightLimits::kMaxFlickerHz), false,
           "light flicker_hz should be from 0 to 60");
    number("shadow", light.shadow, 0.0, 1.0, false, "light shadow should be from 0 (it casts none) to 1");
}

} // namespace de::cooker
