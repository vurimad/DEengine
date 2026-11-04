#include "world/render_extract.h"

#include "core/job_system.h"
#include "core/profile.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace de::world {

namespace {

// The active camera, interpolated, moving the render origin first when the camera has gone a
// step away from it. What was drawn last frame becomes the previous view, moved to the new origin.
void extract_camera(const flecs::world& ecs, float alpha, RenderFrame& frame) {
    const RenderView last = frame.camera;
    const DVec3 last_origin = frame.origin;
    ++frame.number;
    frame.camera.valid = false;
    frame.camera.previous_valid = false;
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
    const WorldTransform seen = interpolate(previous->value, *current, alpha);
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
    const bool cut = active->cut || active->entity != frame.camera_entity || !last.valid;
    if (active->cut) {
        ecs.get_mut<ActiveCamera>().cut = false;
    }
    frame.camera_entity = active->entity;
    if (!cut) {
        std::memcpy(frame.camera.previous_transform, last.transform, sizeof(last.transform));
        for (int row = 0; row < 3; ++row) {
            frame.camera.previous_transform[row * 4 + 3] = static_cast<float>(
                static_cast<double>(last.transform[row * 4 + 3]) + last_origin[row] - frame.origin[row]);
        }
        frame.camera.previous_vertical_fov_rad = last.vertical_fov_rad;
        frame.camera.previous_near_m = last.near_m;
        frame.camera.previous_valid = true;
    }
}

} // namespace

void register_render_components(flecs::world& ecs) {
    ecs.component<MeshInstance>("de::MeshInstance").member<asset::AssetHandle>("mesh");
    ecs.component<Camera>("de::Camera").member<float>("vertical_fov_rad").member<float>("near_m");
    ecs.component<ActiveCamera>("de::ActiveCamera").member<flecs::entity_t>("entity");
}

namespace {

constexpr uint32_t kChunkRows = 512;

struct ExtractJob {
    const RenderExtractScratch* scratch = nullptr;
    RenderInstance* instances = nullptr;
    float alpha = 0.0f;
    float previous_alpha = 0.0f; // a frame ago: below 0 on the tick before, by the same motion
    DVec3 origin{0.0};
};

void extract_chunks(size_t begin, size_t end, void* context) {
    const ExtractJob& job = *static_cast<const ExtractJob*>(context);
    for (size_t c = begin; c < end; ++c) {
        const RenderExtractScratch::Chunk& chunk = job.scratch->chunks[c];
        for (uint32_t i = 0; i < chunk.count; ++i) {
            RenderInstance& instance = job.instances[chunk.first + i];
            to_matrix_3x4(interpolate(chunk.previous[i].value, chunk.current[i], job.alpha), job.origin,
                          instance.transform);
            if (same_transform(chunk.previous[i].value, chunk.current[i])) {
                std::memcpy(instance.previous_transform, instance.transform,
                            sizeof(instance.transform)); // most of a world
            } else {
                to_matrix_3x4(pose_at(chunk.previous[i].value, chunk.current[i], job.previous_alpha), job.origin,
                              instance.previous_transform);
            }
            instance.mesh = chunk.meshes[i].mesh;
        }
    }
}

} // namespace

void add_render_extract(flecs::world& ecs, flecs::entity phase, const RenderExtractDesc& desc) {
    // No terms, so it runs every frame, meshes or not: the camera (which may move the origin the
    // instances are made relative to), and a fresh instance list.
    ecs.system("de::ExtractRenderView").kind(phase).run([desc](flecs::iter& it) {
        DE_PROFILE_ZONE("render extract view");
        extract_camera(it.world(), *desc.alpha, *desc.frame);
        desc.frame->instances.clear(); // keeps its capacity: no allocation once the world has settled
    });
    // Every table cut into chunks first (the column pointers stay valid: nothing structural happens
    // during the phase), then one parallel job over all chunks, whatever the tables' sizes.
    ecs.system<const WorldTransform, const PreviousWorldTransform, const MeshInstance>("de::ExtractRenderInstances")
        .kind(phase)
        .run([desc](flecs::iter& it) {
            DE_PROFILE_ZONE("render extract");
            RenderFrame& out = *desc.frame;
            RenderExtractScratch& scratch = *desc.scratch;
            scratch.chunks.clear();
            uint32_t rows = 0;
            while (it.next()) {
                const auto current = it.field<const WorldTransform>(0);
                const auto previous = it.field<const PreviousWorldTransform>(1);
                const auto meshes = it.field<const MeshInstance>(2);
                const auto count = static_cast<uint32_t>(it.count());
                for (uint32_t first = 0; first < count; first += kChunkRows) {
                    scratch.chunks.push({.current = &current[first],
                                         .previous = &previous[first],
                                         .meshes = &meshes[first],
                                         .count = std::min(kChunkRows, count - first),
                                         .first = rows + first});
                }
                rows += count;
            }
            out.instances.resize_uninit(rows);
            ExtractJob job{.scratch = &scratch,
                           .instances = out.instances.data(),
                           .alpha = *desc.alpha,
                           .previous_alpha = *desc.alpha - *desc.alpha_step,
                           .origin = out.origin};
            if (desc.jobs != nullptr && scratch.chunks.size() > 1) {
                desc.jobs->parallel_for(scratch.chunks.size(), 1, &extract_chunks, &job, "render extract");
            } else {
                extract_chunks(0, scratch.chunks.size(), &job);
            }
            DE_PROFILE_PLOT("render instances", static_cast<int64_t>(rows));
        });
}

} // namespace de::world
