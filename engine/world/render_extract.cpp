#include "world/render_extract.h"

#include "core/profile.h"
#include "world/world.h"

#include <cmath>

namespace de::world {

namespace {

// The active camera, interpolated, moving the render origin first when the camera has gone a
// step away from it.
void extract_camera(const flecs::world& ecs, RenderFrame& frame) {
    frame.camera.valid = false;
    const auto* active = ecs.try_get<ActiveCamera>();
    if (active == nullptr || active->entity == 0 || !ecs.is_alive(active->entity)) {
        return;
    }
    const flecs::entity entity = ecs.entity(active->entity);
    const auto* current = entity.try_get<WorldTransform>();
    const auto* previous = entity.try_get<PreviousWorldTransform>();
    const auto* camera = entity.try_get<Camera>();
    if (current == nullptr || previous == nullptr || camera == nullptr) {
        return;
    }
    const WorldTransform seen = interpolate(previous->value, *current, frame.alpha);
    const DVec3 offset = seen.position - frame.origin;
    if (std::abs(offset.x) > kRenderOriginStep || std::abs(offset.y) > kRenderOriginStep ||
        std::abs(offset.z) > kRenderOriginStep) {
        frame.origin = glm::round(seen.position / kRenderOriginStep) * kRenderOriginStep;
        ++frame.origin_moves;
    }
    to_matrix_3x4(seen, frame.origin, frame.camera.transform);
    frame.camera.vertical_fov_rad = camera->vertical_fov_rad;
    frame.camera.near_m = camera->near_m;
    frame.camera.valid = true;
}

} // namespace

void register_render_components(flecs::world& ecs) {
    ecs.component<MeshInstance>("de::MeshInstance").member<asset::AssetHandle>("mesh");
    ecs.component<Camera>("de::Camera").member<float>("vertical_fov_rad").member<float>("near_m");
    ecs.component<ActiveCamera>("de::ActiveCamera").member<flecs::entity_t>("entity");
}

void add_render_extract(flecs::world& ecs, flecs::entity phase, RenderFrame& frame, core::JobSystem* jobs) {
    RenderFrame* target = &frame;
    // No terms, so it runs every frame, meshes or not: the camera (which may move the origin the
    // instances are made relative to), and a fresh instance list.
    ecs.system("de::ExtractRenderView").kind(phase).run([target](flecs::iter& it) {
        DE_PROFILE_ZONE("render extract view");
        extract_camera(it.world(), *target);
        target->instances.clear(); // keeps its capacity: no allocation once the world has settled
    });
    ecs.system<const WorldTransform, const PreviousWorldTransform, const MeshInstance>("de::ExtractRenderInstances")
        .kind(phase)
        .run([target, jobs](flecs::iter& it) {
            DE_PROFILE_ZONE("render extract");
            RenderFrame& out = *target;
            while (it.next()) {
                const size_t base = out.instances.size();
                out.instances.resize_uninit(base + it.count());
                const auto current = it.field<const WorldTransform>(0);
                const auto previous = it.field<const PreviousWorldTransform>(1);
                const auto meshes = it.field<const MeshInstance>(2);
                RenderInstance* instances = out.instances.data() + base;
                const float alpha = out.alpha;
                const DVec3 origin = out.origin;
                for_each_row_parallel(jobs, it.count(), 1024, [&](size_t i) {
                    RenderInstance& instance = instances[i];
                    to_matrix_3x4(interpolate(previous[i].value, current[i], alpha), origin, instance.transform);
                    instance.mesh = meshes[i].mesh;
                    instance.reserved[0] = 0;
                    instance.reserved[1] = 0;
                });
            }
            DE_PROFILE_PLOT("render instances", static_cast<int64_t>(out.instances.size()));
        });
}

} // namespace de::world
