#include "world/render_extract.h"

#include "core/assert.h"
#include "core/job_system.h"
#include "core/profile.h"

#include <algorithm>
#include <bit>
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
    ecs.component<RenderSlot>("de::RenderSlot");
    ecs.component<MeshInstance>("de::MeshInstance")
        .member<asset::AssetHandle>("mesh")
        .add(flecs::With, ecs.component<RenderSlot>());
    ecs.component<Camera>("de::Camera").member<float>("vertical_fov_rad").member<float>("near_m");
    ecs.component<ActiveCamera>("de::ActiveCamera").member<flecs::entity_t>("entity");
    ecs.component<FogVolume>("de::FogVolume");
}

namespace {

constexpr uint32_t kChunkRows = 512;

void to_render_transform(const WorldTransform& transform, RenderTransform& out) {
    float matrix[12];
    to_matrix_3x4(transform, transform.position, matrix); // its own place as the origin: the position's left out
    for (int row = 0; row < 3; ++row) {
        out.rows[row].linear[0] = matrix[row * 4 + 0];
        out.rows[row].linear[1] = matrix[row * 4 + 1];
        out.rows[row].linear[2] = matrix[row * 4 + 2];
        out.rows[row].position = render_position(transform.position[row]);
    }
}

// What a row depends on. Word by word: there's padding in these structs.
uint64_t sent_hash(const WorldTransform& current, const WorldTransform& previous, asset::AssetHandle mesh) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    const auto mix = [&hash](uint64_t word) {
        hash ^= word;
        hash *= 0x100000001b3ULL;
        hash ^= hash >> 29U;
    };
    const auto mix_transform = [&mix](const WorldTransform& transform) {
        for (int axis = 0; axis < 3; ++axis) {
            mix(std::bit_cast<uint64_t>(transform.position[axis]));
            mix(std::bit_cast<uint32_t>(transform.scale[axis]));
        }
        for (int axis = 0; axis < 4; ++axis) {
            mix(std::bit_cast<uint32_t>(transform.rotation[axis]));
        }
    };
    mix_transform(current);
    mix_transform(previous);
    mix((uint64_t{mesh.index} << 32U) | mesh.generation);
    return hash;
}

struct ExtractJob {
    RenderExtractScratch* scratch = nullptr;
    RenderInstance* instances = nullptr;
    float alpha = 0.0f;
    float previous_alpha = 0.0f; // a frame ago: below 0 on the tick before, by the same motion
    bool all = false;            // every row, whatever it last sent
};

void extract_chunks(size_t begin, size_t end, void* context) {
    const ExtractJob& job = *static_cast<const ExtractJob*>(context);
    for (size_t c = begin; c < end; ++c) {
        RenderExtractScratch::Chunk& chunk = job.scratch->chunks[c];
        for (uint32_t i = 0; i < chunk.count; ++i) {
            RenderInstance& instance = job.instances[chunk.first + i];
            const bool still = same_transform(chunk.previous[i].value, chunk.current[i]);
            chunk.any_moving = chunk.any_moving || !still;
            if (still && !chunk.write_all) {
                instance.slot = kSkipRenderInstance;
                continue;
            }
            // Something wrote to its table, but spawning a neighbour does that too.
            RenderSlot& slot = chunk.slots[i];
            const uint64_t hash = sent_hash(chunk.current[i], chunk.previous[i].value, chunk.meshes[i].mesh);
            if (still && hash == slot.sent && !job.all) {
                instance.slot = kSkipRenderInstance;
                continue;
            }
            slot.sent = hash;
            to_render_transform(interpolate(chunk.previous[i].value, chunk.current[i], job.alpha), instance.transform);
            if (still) {
                instance.previous_transform = instance.transform; // most of a world
            } else {
                to_render_transform(pose_at(chunk.previous[i].value, chunk.current[i], job.previous_alpha),
                                    instance.previous_transform);
            }
            instance.mesh = chunk.meshes[i].mesh;
            instance.slot = slot.slot;
        }
    }
}

} // namespace

void add_render_extract(flecs::world& ecs, flecs::entity phase, const RenderExtractDesc& desc) {
    // A slot from when an entity gets a MeshInstance, until it loses it or goes.
    ecs.observer<RenderSlot>("de::GiveRenderSlot").event(flecs::OnAdd).each([scratch = desc.scratch](RenderSlot& slot) {
        if (!scratch->free_slots.empty()) {
            slot.slot = scratch->free_slots.back();
            scratch->free_slots.pop();
        } else {
            slot.slot = scratch->next_slot++;
        }
    });
    ecs.observer<RenderSlot>("de::FreeRenderSlot")
        .event(flecs::OnRemove)
        .each([scratch = desc.scratch](RenderSlot& slot) {
            scratch->free_slots.push(slot.slot);
            scratch->freed_slots.push(slot.slot);
        });
    ecs.observer<MeshInstance>("de::DropRenderSlot")
        .event(flecs::OnRemove)
        .each([](flecs::entity entity, MeshInstance&) { entity.remove<RenderSlot>(); });
    // No terms, so it runs every frame, meshes or not: the camera (which may move the origin), and a fresh list of what
    // changed.
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
                    // Cooked ones are checked at load; one set from code, here.
                    DE_ASSERT(volume.density_per_m >= 0.0f && volume.edge_m >= 0.0f && volume.noise_size_m > 0.0f &&
                              volume.noise_amount >= 0.0f && volume.noise_amount <= 1.0f &&
                              volume.height_fade_m >= 0.0f && std::isfinite(volume.drift_mps[0]) &&
                              std::isfinite(volume.drift_mps[1]) && std::isfinite(volume.drift_mps[2]));
                    const WorldTransform seen = interpolate(previous.value, current, *desc.alpha);
                    const Vec3 center(seen.position - desc.frame->origin);
                    const glm::mat3 to_axes = glm::mat3_cast(glm::conjugate(seen.rotation));
                    const float noise_tiles_per_m = 1.0f / std::max(volume.noise_size_m, 1e-3f);
                    RenderFogVolume out{.density_per_m = volume.density_per_m,
                                        .edge_m = volume.edge_m,
                                        .ellipsoid = volume.ellipsoid,
                                        .noise_tiles_per_m = noise_tiles_per_m,
                                        .noise_amount = volume.noise_amount,
                                        .height_fade_m = volume.height_fade_m};
                    for (int row = 0; row < 3; ++row) {
                        const Vec3 axis(to_axes[0][row], to_axes[1][row], to_axes[2][row]);
                        out.to_local[static_cast<size_t>(row) * 4 + 0] = axis.x;
                        out.to_local[static_cast<size_t>(row) * 4 + 1] = axis.y;
                        out.to_local[static_cast<size_t>(row) * 4 + 2] = axis.z;
                        out.to_local[static_cast<size_t>(row) * 4 + 3] = -glm::dot(axis, center);
                        out.center[row] = center[row];
                        out.half_extents[row] = std::abs(seen.scale[row]);
                        out.tint[row] = volume.tint[row];
                        // In double, however long the world has run and wherever the origin is; wrapped, so a
                        // float holds it. The render origin's part keeps the noise still in the world as it moves.
                        const double moved =
                            (static_cast<double>(volume.drift_mps[row]) * *desc.time_s - desc.frame->origin[row]) *
                            static_cast<double>(noise_tiles_per_m);
                        const auto wrapped = static_cast<float>(moved - 4.0 * std::floor(moved * 0.25));
                        out.noise_offset_tiles[row] = wrapped < 4.0f ? wrapped : 0.0f; // it can round up to 4
                    }
                    desc.frame->fog_volumes.push(out);
                }
            }
        });
    // Every table cut into chunks first (the column pointers stay valid: nothing structural happens
    // during the phase), then one parallel job over all chunks, whatever the tables' sizes.
    ecs.system<const WorldTransform, const PreviousWorldTransform, const MeshInstance, RenderSlot>(
           "de::ExtractRenderInstances")
        .kind(phase)
        .term_at(3)
        .out() // written only: changes to it alone don't make a table "changed"
        .detect_changes()
        .run([desc](flecs::iter& it) {
            DE_PROFILE_ZONE("render extract");
            RenderFrame& out = *desc.frame;
            RenderExtractScratch& scratch = *desc.scratch;
            scratch.chunks.clear();
            // All of them: the renderer lost track.
            const bool all = scratch.resync;
            if (all && !out.camera.valid) {
                // Nothing would draw it. It's sent when something can, and tables stay marked as changed until then.
                out.all_instances = false;
                out.instances.clear();
                while (it.next()) {
                    it.skip();
                }
                return;
            }
            scratch.resync = false;
            out.all_instances = all;
            uint32_t rows = 0;
            while (it.next()) {
                const bool changed = it.changed();
                const auto table = reinterpret_cast<uint64_t>(it.c_ptr()->table);
                const uint64_t* moved_at = scratch.moving_tables.find(table);
                const bool was_moving = moved_at != nullptr && *moved_at + 1 == out.number;
                if (!all && !changed && !was_moving) {
                    it.skip();
                    continue;
                }
                const auto current = it.field<const WorldTransform>(0);
                const auto previous = it.field<const PreviousWorldTransform>(1);
                const auto meshes = it.field<const MeshInstance>(2);
                const auto slots = it.field<RenderSlot>(3);
                const auto count = static_cast<uint32_t>(it.count());
                for (uint32_t first = 0; first < count; first += kChunkRows) {
                    scratch.chunks.push({.current = &current[first],
                                         .previous = &previous[first],
                                         .meshes = &meshes[first],
                                         .slots = &slots[first],
                                         .table = table,
                                         .count = std::min(kChunkRows, count - first),
                                         .first = rows + first,
                                         .write_all = all || changed});
                }
                rows += count;
            }
            out.instances.resize_uninit(rows);
            ExtractJob job{.scratch = &scratch,
                           .instances = out.instances.data(),
                           .alpha = *desc.alpha,
                           .previous_alpha = *desc.alpha - *desc.alpha_step,
                           .all = all};
            if (desc.jobs != nullptr && scratch.chunks.size() > 1) {
                desc.jobs->parallel_for(scratch.chunks.size(), 1, &extract_chunks, &job, "render extract");
            } else {
                extract_chunks(0, scratch.chunks.size(), &job);
            }
            for (const RenderExtractScratch::Chunk& chunk : scratch.chunks) {
                if (chunk.any_moving) {
                    scratch.moving_tables.find_or_insert(chunk.table) = out.number;
                }
            }
            // What's gone since the last frame. All of them means anything not among them, so these aren't needed.
            out.freed_slots.clear();
            if (!all) {
                out.freed_slots.assign(scratch.freed_slots.span());
            }
            scratch.freed_slots.clear();
            out.slot_count = scratch.next_slot;
            DE_PROFILE_PLOT("render instance rows", static_cast<int64_t>(rows));
        });
}

} // namespace de::world
