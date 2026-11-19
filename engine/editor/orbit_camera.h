#pragma once

#include "app/preview_scene.h"
#include "core/math.h"
#include "editor/gizmo.h"
#include "editor/preview_pictures.h"

#include <imgui.h>

namespace de::editor {

// A camera that turns about what it looks at, for a thing seen by itself (the effect viewer, the prefab editor): a
// point, the side it's seen from, and how far away it would stand through the usual lens. Where it really stands is
// the pictures' rule (preview_pictures.h, look_from): no nearer than an effect's particles show whole from, no
// farther than they're drawn from, the lens taking up the difference.
struct OrbitCamera {
    Vec3 target{0.0f};
    float yaw_rad = kPictureYawRad;
    float pitch_rad = kPicturePitchRad;
    float wanted_m = 1.0f;

    PictureAxes axes() const { return picture_axes(yaw_rad, pitch_rad); }
    // Where it stands now, for this subject.
    PictureFraming framing(const PictureSubject& subject) const;
    // Looks at the whole subject again, from the side a picture is taken from. Left as it is if there's nothing to
    // the subject.
    void frame(const PictureSubject& subject, float fill);
    // The mouse dragged on its picture (size, in points) by `moved`: turned about the target, or with sliding the
    // target moved with the hand.
    void drag(const PictureFraming& now, ImVec2 size, ImVec2 moved, bool sliding);
    // The wheel turned over it: in, or out for less than 0.
    void zoom(float notches);

    // For a preview scene.
    app::PreviewCamera camera(const PictureFraming& now) const;
    // For what's drawn over its picture, which fills size at min on screen: the transform handles, a ray under the
    // mouse.
    View view(const PictureFraming& now, ImVec2 min, ImVec2 size) const;
};

} // namespace de::editor
