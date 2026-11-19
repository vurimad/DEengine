#pragma once

#include "cooker/json.h"
#include "core/math.h"

#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstddef>
#include <limits>

namespace de::cooker {

// A float from a JSON number, if it fits.
inline bool to_float(double value, float& out) {
    if (!std::isfinite(value) || std::abs(value) > static_cast<double>(std::numeric_limits<float>::max())) {
        return false;
    }
    out = static_cast<float>(value);
    return true;
}

// Where something is, as a cell's entity and a prefab's part say it: "position": [x, y, z] (meters); "rotation": a
// quaternion [x, y, z, w], or "rotation_deg": [x, y, z] in degrees (roll about z, then pitch about x, then yaw about
// y); "scale": a number or [x, y, z], with no zero. Each is left as it was (the origin, unturned, 1) where its key
// isn't there. What's wrong is said to bad(const char*), once for each.
template <typename Position, typename Bad>
void read_entity_transform(yyjson_val* object, const JsonReader& reader, Position (&position)[3], float (&rotation)[4],
                           float (&scale)[3], Bad&& bad) {
    if (reader.has("position") && !read_numbers(yyjson_obj_get(object, "position"), position, 3)) {
        bad("position should be [x, y, z]");
    }
    if (reader.has("rotation") && reader.has("rotation_deg")) {
        bad("has both rotation and rotation_deg");
    } else if (reader.has("rotation")) {
        double q[4] = {};
        const bool read = read_numbers(yyjson_obj_get(object, "rotation"), q, 4);
        // In doubles: large components would overflow a float's square.
        const double norm = std::sqrt((q[0] * q[0]) + (q[1] * q[1]) + (q[2] * q[2]) + (q[3] * q[3]));
        if (!read || !std::isfinite(norm) || !(norm > 1e-6)) {
            bad("rotation should be a quaternion [x, y, z, w], not all zero");
        } else {
            for (size_t c = 0; c < 4; ++c) {
                rotation[c] = static_cast<float>(q[c] / norm);
            }
        }
    } else if (reader.has("rotation_deg")) {
        double read_degrees[3] = {};
        float degrees[3] = {};
        if (!read_numbers(yyjson_obj_get(object, "rotation_deg"), read_degrees, 3) ||
            !(std::abs(read_degrees[0]) <= 1e6 && std::abs(read_degrees[1]) <= 1e6 &&
              std::abs(read_degrees[2]) <= 1e6)) {
            bad("rotation_deg should be [x, y, z] in degrees");
        } else {
            for (size_t c = 0; c < 3; ++c) {
                degrees[c] = static_cast<float>(read_degrees[c]);
            }
            const Quat q = glm::angleAxis(glm::radians(degrees[1]), Vec3(0.0f, 1.0f, 0.0f)) *
                           glm::angleAxis(glm::radians(degrees[0]), Vec3(1.0f, 0.0f, 0.0f)) *
                           glm::angleAxis(glm::radians(degrees[2]), Vec3(0.0f, 0.0f, 1.0f));
            const Quat unit = glm::normalize(q);
            rotation[0] = unit.x;
            rotation[1] = unit.y;
            rotation[2] = unit.z;
            rotation[3] = unit.w;
        }
    }
    if (yyjson_val* given = yyjson_obj_get(object, "scale"); given != nullptr) {
        double read_scale[3] = {};
        if (yyjson_is_num(given)) {
            read_scale[0] = read_scale[1] = read_scale[2] = yyjson_get_num(given);
        } else if (!read_numbers(given, read_scale, 3)) {
            bad("scale should be a number or [x, y, z]");
        }
        if (!to_float(read_scale[0], scale[0]) || !to_float(read_scale[1], scale[1]) ||
            !to_float(read_scale[2], scale[2])) {
            bad("scale is too large for a float");
        } else if (scale[0] == 0.0f || scale[1] == 0.0f || scale[2] == 0.0f) {
            bad("scale has a zero (it couldn't be undone, for attaching)");
        }
    }
}

} // namespace de::cooker
