#include "world/transform.h"

#include "core/profile.h"
#include "world/world.h"

#include <glm/gtc/matrix_transform.hpp>

namespace de::world {

Transform interpolate(const Transform& previous, const Transform& current, float alpha) {
    return {
        .position = glm::mix(previous.position, current.position, alpha),
        .rotation = glm::slerp(previous.rotation, current.rotation, alpha),
        .scale = glm::mix(previous.scale, current.scale, alpha),
    };
}

Mat4 to_matrix(const Transform& transform) {
    Mat4 matrix = glm::mat4_cast(transform.rotation);
    matrix[0] *= transform.scale.x;
    matrix[1] *= transform.scale.y;
    matrix[2] *= transform.scale.z;
    matrix[3] = Vec4(transform.position, 1.0f);
    return matrix;
}

void register_transform(flecs::world& ecs) {
    // Reflection: the math types by member, then the components built from them.
    ecs.component<Vec3>("de::Vec3").member<float>("x").member<float>("y").member<float>("z");
    ecs.component<Quat>("de::Quat").member<float>("x").member<float>("y").member<float>("z").member<float>("w");
    ecs.component<Transform>("de::Transform").member<Vec3>("position").member<Quat>("rotation").member<Vec3>("scale");
    ecs.component<PreviousTransform>("de::PreviousTransform").member<Transform>("value");
}

} // namespace de::world
