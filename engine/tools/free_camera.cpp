#include "tools/free_camera.h"

#include "core/profile.h"
#include "core/string_id.h"
#include "world/transform.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>

namespace de::tools {

using namespace core::literals;
using platform::ActionKind;

core::Result<FreeCameraActions> add_free_camera_actions(platform::Input& input) {
    const FreeCameraActions actions{
        .move_right = input.add_action("free_camera.move_right"_sid, ActionKind::Axis),
        .move_forward = input.add_action("free_camera.move_forward"_sid, ActionKind::Axis),
        .move_up = input.add_action("free_camera.move_up"_sid, ActionKind::Axis),
        .look_x = input.add_action("free_camera.look_x"_sid, ActionKind::Axis),
        .look_y = input.add_action("free_camera.look_y"_sid, ActionKind::Axis),
        .mouse_x = input.add_action("free_camera.mouse_x"_sid, ActionKind::Axis),
        .mouse_y = input.add_action("free_camera.mouse_y"_sid, ActionKind::Axis),
        .look_hold = input.add_action("free_camera.look_hold"_sid, ActionKind::Button),
        .fast = input.add_action("free_camera.fast"_sid, ActionKind::Button),
    };
    struct Binding {
        platform::ActionId action;
        const char* name;
        float scale;
    };
    const Binding bindings[] = {
        {.action = actions.move_right, .name = "key:D", .scale = 1.0f},
        {.action = actions.move_right, .name = "key:A", .scale = -1.0f},
        {.action = actions.move_right, .name = "gamepad:leftx", .scale = 1.0f},
        {.action = actions.move_forward, .name = "key:W", .scale = 1.0f},
        {.action = actions.move_forward, .name = "key:S", .scale = -1.0f},
        {.action = actions.move_forward, .name = "gamepad:lefty", .scale = -1.0f}, // stick up is negative
        {.action = actions.move_up, .name = "key:E", .scale = 1.0f},
        {.action = actions.move_up, .name = "key:Q", .scale = -1.0f},
        {.action = actions.move_up, .name = "gamepad:righttrigger", .scale = 1.0f},
        {.action = actions.move_up, .name = "gamepad:lefttrigger", .scale = -1.0f},
        {.action = actions.look_x, .name = "gamepad:rightx", .scale = 1.0f},
        {.action = actions.look_y, .name = "gamepad:righty", .scale = 1.0f},
        {.action = actions.mouse_x, .name = "mouse:x", .scale = 1.0f},
        {.action = actions.mouse_y, .name = "mouse:y", .scale = 1.0f},
        {.action = actions.look_hold, .name = "mouse:right", .scale = 1.0f},
        {.action = actions.fast, .name = "key:Left Shift", .scale = 1.0f},
        {.action = actions.fast, .name = "gamepad:leftstick", .scale = 1.0f},
    };
    for (const Binding& binding : bindings) {
        if (const core::Result<void> bound = input.bind(binding.action, binding.name, binding.scale); !bound) {
            return core::fail(bound.error());
        }
    }
    return actions;
}

Quat FreeCamera::rotation() const {
    return glm::angleAxis(yaw_rad_, kWorldUp) * glm::angleAxis(pitch_rad_, Vec3(1.0f, 0.0f, 0.0f));
}

void FreeCamera::update(const platform::Input& input, const FreeCameraActions& actions, float frame_s,
                        flecs::entity camera) {
    DE_PROFILE_ZONE("free camera");
    // Look: the stick as a rate, mouse motion in pixels while its button is held.
    const bool mouse = input.state(actions.look_hold).down;
    const float stick = stick_rad_per_s * frame_s;
    const float yaw = input.state(actions.look_x).value * stick +
                      (mouse ? input.state(actions.mouse_x).value * mouse_rad_per_px : 0.0f);
    const float pitch = input.state(actions.look_y).value * stick +
                        (mouse ? input.state(actions.mouse_y).value * mouse_rad_per_px : 0.0f);
    yaw_rad_ -= yaw;
    pitch_rad_ = std::clamp(pitch_rad_ - pitch, -1.55f, 1.55f);

    const Quat rotation = this->rotation();
    const Vec3 right = rotation * Vec3(1.0f, 0.0f, 0.0f);
    const Vec3 forward = rotation * Vec3(0.0f, 0.0f, -1.0f);
    const Vec3 move = right * input.state(actions.move_right).value +
                      forward * input.state(actions.move_forward).value + kWorldUp * input.state(actions.move_up).value;
    const float speed = speed_mps * (input.state(actions.fast).down ? 6.0f : 1.0f);
    position_ += DVec3(move) * static_cast<double>(speed * frame_s);
    world::place(camera, world::Transform{.position = position_, .rotation = rotation});
}

} // namespace de::tools
