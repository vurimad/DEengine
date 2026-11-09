#include "editor/navigation.h"

#include "tools/free_camera.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace de::editor {

namespace {

constexpr float kEaseS = 0.22f;             // how long framing takes
constexpr float kOrbitRadPerPixel = 0.005f; // as the free camera's look
constexpr float kMinPivotM = 0.05f;
constexpr float kMaxPivotM = 20000.0f;

Quat rotation_of(const tools::CameraPose& pose) {
    return glm::angleAxis(pose.yaw_rad, Vec3(0.0f, 1.0f, 0.0f)) *
           glm::angleAxis(pose.pitch_rad, Vec3(1.0f, 0.0f, 0.0f));
}

} // namespace

Vec3 forward_of(const tools::CameraPose& pose) {
    return rotation_of(pose) * Vec3(0.0f, 0.0f, -1.0f);
}

void Navigation::frame(const tools::CameraPose& pose, const DVec3& center, float radius_m, float vertical_fov_rad) {
    // Far enough that the sphere fills about half the view's height.
    const float half_view = std::max(std::tan(vertical_fov_rad * 0.5f), 0.05f);
    const float distance_m = std::clamp(radius_m * 2.0f / half_view, 1.0f, kMaxPivotM);
    pivot_distance_m_ = distance_m;
    ease_from_ = pose.position;
    ease_to_ = center - DVec3(forward_of(pose) * distance_m);
    ease_s_ = 0.0f;
    easing_ = true;
}

bool Navigation::update(const View& view, bool hovered, bool flying, float delta_s, tools::CameraPose& pose) {
    const ImGuiIO& io = ImGui::GetIO();
    bool moved = false;

    // Framing eases out: fast at first, settling at the end.
    if (easing_) {
        ease_s_ += delta_s;
        const float t = std::min(ease_s_ / kEaseS, 1.0f);
        const float eased = 1.0f - ((1.0f - t) * (1.0f - t) * (1.0f - t));
        pose.position = ease_from_ + ((ease_to_ - ease_from_) * static_cast<double>(eased));
        easing_ = t < 1.0f;
        moved = true;
    }
    if (flying) {
        easing_ = false; // the right button takes the camera
        drag_ = Drag::None;
        return moved;
    }

    // A drag starts over the scene and goes on wherever the mouse goes, until its button is let go.
    if (drag_ == Drag::None && hovered) {
        if (io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            drag_ = Drag::Orbit;
        } else if (io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            drag_ = Drag::Zoom;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
            drag_ = Drag::Pan;
        }
        if (drag_ != Drag::None) {
            easing_ = false;
        }
    }
    const ImGuiMouseButton held = drag_ == Drag::Orbit  ? ImGuiMouseButton_Left
                                  : drag_ == Drag::Zoom ? ImGuiMouseButton_Right
                                                        : ImGuiMouseButton_Middle;
    if (drag_ != Drag::None && !ImGui::IsMouseDown(held)) {
        drag_ = Drag::None;
    }

    const Vec3 forward = forward_of(pose);
    const DVec3 pivot = pose.position + DVec3(forward * pivot_distance_m_);
    const ImVec2 delta = io.MouseDelta;
    if (drag_ == Drag::Orbit && (delta.x != 0.0f || delta.y != 0.0f)) {
        // About the pivot: the camera turns and keeps its distance.
        pose.yaw_rad -= delta.x * kOrbitRadPerPixel;
        pose.pitch_rad = std::clamp(pose.pitch_rad - (delta.y * kOrbitRadPerPixel), -tools::FreeCamera::kMaxPitchRad,
                                    tools::FreeCamera::kMaxPitchRad);
        pose.position = pivot - DVec3(forward_of(pose) * pivot_distance_m_);
        moved = true;
    } else if (drag_ == Drag::Pan && (delta.x != 0.0f || delta.y != 0.0f)) {
        // The point at the pivot's distance under the mouse stays under it.
        const Quat rotation = rotation_of(pose);
        const float metres = view.meters_per_pixel(pivot);
        const Vec3 shift =
            ((rotation * Vec3(1.0f, 0.0f, 0.0f)) * -delta.x + (rotation * Vec3(0.0f, 1.0f, 0.0f)) * delta.y) * metres;
        pose.position += DVec3(shift);
        moved = true;
    } else if (drag_ == Drag::Zoom && (delta.x != 0.0f || delta.y != 0.0f)) {
        // Right or down zooms in, as Unity's.
        const float factor = std::exp(-(delta.x + delta.y) * 0.005f);
        const float distance = std::clamp(pivot_distance_m_ * factor, kMinPivotM, kMaxPivotM);
        pose.position = pivot - DVec3(forward * distance);
        pivot_distance_m_ = distance;
        moved = true;
    }

    // The wheel zooms toward the pivot: each notch takes away a share of the way there.
    if (hovered && drag_ == Drag::None && io.MouseWheel != 0.0f) {
        const float factor = std::pow(0.8f, io.MouseWheel);
        const float distance = std::clamp(pivot_distance_m_ * factor, kMinPivotM, kMaxPivotM);
        pose.position = pivot - DVec3(forward * distance);
        pivot_distance_m_ = distance;
        easing_ = false;
        moved = true;
    }
    return moved;
}

} // namespace de::editor
