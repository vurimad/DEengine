#include "world/render_extract.h"

#include "core/assert.h"
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
    ecs.component<FogVolume>("de::FogVolume");
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
        desc.frame->fog_volumes.clear();
    });
    // A few, not worth a job. Moving ones are where they're seen, like anything else.
    ecs.system<const WorldTransform, const PreviousWorldTransform, const FogVolume>("de::ExtractRenderFogVolumes")
        .kind(phase)
        .run([desc](flecs::iter& it) {
            DE_PROFILE_ZONE("render extract fog volumes");
            while (it.next()) {
                const auto currents = it.field<const WorldTransform>(0);
                const auto previouses = it.field<const PreviousWorldTransform>(1);
                const auto volumes = it.field<const FogVolume>(2);
                for (const auto entity_row : it) {
                    const WorldTransform& current = currents[entity_row];
                    const PreviousWorldTransform& previous = previouses[entity_row];
                    const FogVolume& volume = volumes[entity_row];
                    DE_ASSERT(volume.density_per_m >= 0.0f && volume.edge_m >= 0.0f); // cooked ones are checked at load
                    const WorldTransform seen = interpolate(previous.value, current, *desc.alpha);
                    const Vec3 center(seen.position - desc.frame->origin);
                    const glm::mat3 to_axes = glm::mat3_cast(glm::conjugate(seen.rotation));
                    const float noise_per_m = 1.0f / std::max(volume.noise_size_m, 1e-3f);
                    RenderFogVolume out{.density_per_m = volume.density_per_m,
                                        .edge_m = volume.edge_m,
                                        .ellipsoid = volume.ellipsoid,
                                        .noise_per_m = noise_per_m,
                                        .noise_amount = volume.noise_amount,
                                        .height_fade_m = volume.height_fade_m};
                    const Vec3 drift(volume.drift_mps[0], volume.drift_mps[1], volume.drift_mps[2]);
                    for (int row = 0; row < 3; ++row) {
                        const Vec3 axis(to_axes[0][row], to_axes[1][row], to_axes[2][row]);
                        out.to_local[static_cast<size_t>(row) * 4 + 0] = axis.x;
                        out.to_local[static_cast<size_t>(row) * 4 + 1] = axis.y;
                        out.to_local[static_cast<size_t>(row) * 4 + 2] = axis.z;
                        out.to_local[static_cast<size_t>(row) * 4 + 3] = -glm::dot(axis, center);
                        out.center[row] = center[row];
                        out.half_extents[row] = std::abs(seen.scale[row]);
                        out.tint[row] = volume.tint[row];
                        // In double, however long the world has run; wrapped, so a float holds it.
                        const double moved = static_cast<double>(glm::dot(axis, drift)) * *desc.time_s *
                                             static_cast<double>(noise_per_m);
                        out.noise_offset[row] = static_cast<float>(moved - 4.0 * std::floor(moved * 0.25));
                    }
                    desc.frame->fog_volumes.push(out);
                }
            }
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
