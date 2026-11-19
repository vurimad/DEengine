#include "editor/orbit_camera.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace de::editor {
namespace {

constexpr float kTurnPerPixel = 0.008f; // radians for a pixel dragged
constexpr float kLowestPitchRad = -1.45f;
constexpr float kHighestPitchRad = 1.45f;
constexpr float kZoomPerNotch = 0.88f;
constexpr float kNearestM = 0.005f;
constexpr float kFarthestM = 5000.0f;

} // namespace

PictureFraming OrbitCamera::framing(const PictureSubject& subject) const {
    PictureFraming now{.target = target, .distance_m = wanted_m, .vertical_fov_rad = kPictureFovRad};
    look_from(now, subject, wanted_m);
    return now;
}

void OrbitCamera::frame(const PictureSubject& subject, float fill) {
    Vec3 middle(0.0f);
    float wanted = 1.0f;
    if (subject_extent(subject, picture_axes(), fill, middle, wanted)) {
        yaw_rad = kPictureYawRad;
        pitch_rad = kPicturePitchRad;
        target = middle;
        wanted_m = std::clamp(wanted, kNearestM, kFarthestM);
    }
}

void OrbitCamera::drag(const PictureFraming& now, ImVec2 size, ImVec2 moved, bool sliding) {
    if (sliding) {
        // The point looked at moves with the hand, by what a pixel spans where it is.
        const PictureAxes seen = axes();
        const float per_pixel = 2.0f * now.distance_m * std::tan(now.vertical_fov_rad * 0.5f) / std::max(size.y, 1.0f);
        target -= seen.right * (moved.x * per_pixel);
        target += seen.up * (moved.y * per_pixel);
        return;
    }
    yaw_rad -= moved.x * kTurnPerPixel;
    pitch_rad = std::clamp(pitch_rad + (moved.y * kTurnPerPixel), kLowestPitchRad, kHighestPitchRad);
}

void OrbitCamera::zoom(float notches) {
    wanted_m = std::clamp(wanted_m * std::pow(kZoomPerNotch, notches), kNearestM, kFarthestM);
}

app::PreviewCamera OrbitCamera::camera(const PictureFraming& now) const {
    const PictureAxes seen = axes();
    const Vec3 eye = now.target + (seen.toward * now.distance_m);
    return {.position = DVec3(eye),
            .rotation = glm::quat_cast(Mat3(seen.right, seen.up, seen.toward)),
            .vertical_fov_rad = now.vertical_fov_rad,
            .near_m = std::clamp(now.distance_m * 0.05f, 0.001f, 1.0f)};
}

View OrbitCamera::view(const PictureFraming& now, ImVec2 min, ImVec2 size) const {
    const app::PreviewCamera seen = camera(now);
    return {.position = seen.position,
            .rotation = seen.rotation,
            .vertical_fov_rad = seen.vertical_fov_rad,
            .min = min,
            .size = size};
}

} // namespace de::editor
