#pragma once

#include "core/math.h"
#include "platform/input.h"

#include <flecs.h>

namespace de::tools {

// The actions a free camera reads (docs/design.md, Input: game code reads actions).
struct FreeCameraActions {
    platform::ActionId move_right;
    platform::ActionId move_forward;
    platform::ActionId move_up;
    platform::ActionId look_x; // the right stick: a rate
    platform::ActionId look_y;
    platform::ActionId mouse_x; // mouse motion: pixels
    platform::ActionId mouse_y;
    platform::ActionId look_hold; // mouse look only while held; the right stick always looks
    platform::ActionId fast;
};

// Adds the free camera's actions ("free_camera.move_right"...). Their bindings are text, in
// config/input.bindings: WASD and the left stick move, Q/E and the triggers go down and up, the
// mouse (right button held) and the right stick look, Shift goes fast.
FreeCameraActions add_free_camera_actions(platform::Input& input);

// A fly camera for looking around a scene (a debug tool, not the game's camera). Each frame,
// update() moves it by the actions over the real frame time and places its entity there at once
// (place(): no interpolation lag). It looks down its -Z; yaw turns about world up, pitch is
// clamped short of straight up and down. Stick drift is the input's deadzone's job
// (Input::set_stick_deadzone, deengine --deadzone).
class FreeCamera {
public:
    FreeCamera(const DVec3& position, float yaw_rad, float pitch_rad)
        : position_(position), yaw_rad_(yaw_rad), pitch_rad_(pitch_rad) {}

    void update(const platform::Input& input, const FreeCameraActions& actions, float frame_s, flecs::entity camera);

    const DVec3& position() const { return position_; }
    Quat rotation() const;

    float speed_mps = 6.0f;          // walking pace; fast is 6 times this
    float mouse_rad_per_px = 0.003f; // look sensitivity
    float stick_rad_per_s = 2.5f;

private:
    DVec3 position_;
    float yaw_rad_ = 0.0f;
    float pitch_rad_ = 0.0f;
};

} // namespace de::tools
