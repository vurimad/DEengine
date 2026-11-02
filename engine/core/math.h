#pragma once

// Math types: GLM (docs/design.md, Tech stack), configured for the engine's conventions by the
// definitions on de::core (CMake): depth 0..1 for clip space, x/y/z/w members only (plain
// layout: reflection and GPU copies see the fields in order), no swizzles.
//
// World space is right-handed and Y-up, in meters (glTF's convention); clip space is +Y up with
// reverse Z (docs/design.md, decisions log).

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace de {

using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;
using Quat = glm::quat; // x, y, z, w in memory
using Mat3 = glm::mat3;
using Mat4 = glm::mat4; // column-major

inline constexpr Vec3 kWorldUp{0.0f, 1.0f, 0.0f};

} // namespace de
