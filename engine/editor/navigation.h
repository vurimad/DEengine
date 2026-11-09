#pragma once

#include "core/math.h"
#include "editor/gizmo.h"
#include "tools/camera_script.h"

#include <imgui.h>

namespace de::editor {

// How the Playground's camera moves, as Unity's scene view does it (its documented controls): a pivot stays a distance
// ahead of the camera. The wheel zooms toward it, a middle drag pans, Alt and a left drag orbits about it, Alt and a
// right drag zooms. Flying (the right button held, WASD) is the free camera's; framing a selection eases the camera
// back from it and makes it the pivot. Each frame it takes the camera's pose and gives back where it goes.
class Navigation {
public:
    enum class Drag : uint8_t { None, Orbit, Pan, Zoom };

    // One frame of the mouse over the scene. hovered: the mouse is over the scene and nothing else has it (a handle);
    // flying: the free camera has the right button. pose is the camera's now: true when it changes it.
    bool update(const View& view, bool hovered, bool flying, float delta_s, tools::CameraPose& pose);
    // Eases the camera, turned as it is, back from a sphere so it fills about half the view, and makes its centre the
    // pivot.
    void frame(const tools::CameraPose& pose, const DVec3& center, float radius_m, float vertical_fov_rad);
    // A drag of the camera is under way (or just ended this frame): the click isn't a pick.
    bool dragging() const { return drag_ != Drag::None; }
    // How far ahead of the camera the pivot is.
    float pivot_distance_m() const { return pivot_distance_m_; }

private:
    Drag drag_ = Drag::None;
    float pivot_distance_m_ = 10.0f;
    // Framing: from where the camera was to where it goes, over a short ease.
    bool easing_ = false;
    float ease_s_ = 0.0f;
    DVec3 ease_from_{0.0};
    DVec3 ease_to_{0.0};
};

// Where a camera at a pose looks (unit length): yaw about world up, pitch up from level; -Z at yaw and pitch 0.
Vec3 forward_of(const tools::CameraPose& pose);

} // namespace de::editor
