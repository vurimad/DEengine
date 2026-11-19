#include "world/render_extract.h"

#include "core/assert.h"
#include "core/job_system.h"
#include "core/log.h"
#include "core/profile.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <atomic>
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

float light_flicker(double time_s, float hz, uint32_t seed) {
    // Each light a place of its own in the waves, a few hundred turns in at most so a float holds what's left.
    const double turns = (time_s * static_cast<double>(hz)) + (static_cast<double>(seed & 0xffffU) / 64.0);
    const auto wave = [turns](double rate, double start) {
        const double at = (turns * rate) + start;
        return static_cast<float>(std::sin((at - std::floor(at)) * glm::two_pi<double>()));
    };
    // Rates with no common period within hours; the shares add to a half each side of a half.
    return std::clamp(0.5f + (0.25f * wave(1.0, 0.0)) + (0.15f * wave(2.31, 0.37)) + (0.10f * wave(4.13, 0.71)), 0.0f,
                      1.0f);
}

void register_render_components(flecs::world& ecs) {
    ecs.component<RenderSlot>("de::RenderSlot");
    ecs.component<MeshInstance>("de::MeshInstance")
        .member<asset::AssetHandle>("mesh")
        .add(flecs::With, ecs.component<RenderSlot>());
    ecs.component<Camera>("de::Camera").member<float>("vertical_fov_rad").member<float>("near_m");
    ecs.component<ActiveCamera>("de::ActiveCamera").member<flecs::entity_t>("entity");
    ecs.component<FogVolume>("de::FogVolume")
        .member<float>("density_per_m")
        .member<float>("edge_m")
        .member<float>("tint", 3)
        .member<bool>("ellipsoid")
        .member<float>("noise_size_m")
        .member<float>("noise_amount")
        .member<float>("drift_mps", 3)
        .member<float>("height_fade_m")
        .member<float>("under_roof");
    ecs.component<WaterBody>("de::WaterBody").member<bool>("ellipse").member<float>("waves");
    ecs.component<LocalLight>("de::LocalLight")
        .member<float>("color", 3)
        .member<float>("intensity")
        .member<float>("radius_m")
        .member<float>("cone_deg")
        .member<float>("cone_soft")
        .member<float>("flicker")
        .member<float>("flicker_hz")
        .member<float>("shadow");
    ecs.component<EffectSlot>("de::EffectSlot");
    ecs.component<EffectInstance>("de::EffectInstance")
        .member<asset::AssetHandle>("effect")
        .add(flecs::With, ecs.component<EffectSlot>());
}

namespace {

constexpr uint32_t kChunkRows = 512;

void to_render_transform(const WorldTransform& transform, RenderTransform& out) {
    const Mat3 rotation = glm::mat3_cast(transform.rotation);
    for (int row = 0; row < 3; ++row) {
        out.rows[row].linear[0] = rotation[0][row] * transform.scale.x;
        out.rows[row].linear[1] = rotation[1][row] * transform.scale.y;
        out.rows[row].linear[2] = rotation[2][row] * transform.scale.z;
        out.set_position_m(row, transform.position[row]);
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
    uint64_t frame = 0;
    DVec3 origin{0.0};
    bool has_camera = false; // no camera, no origin to be far from
    bool all = false;        // every row, whatever it last sent
};

// Sends a row. True if it's past where the renderer can reach.
bool send_row(const ExtractJob& job, const WorldTransform& current, const PreviousWorldTransform& previous,
              const MeshInstance& mesh, RenderSlot& slot, bool still, RenderInstance& instance) {
    slot.sent = sent_hash(current, previous.value, mesh.mesh);
    const DVec3 away = glm::abs(current.position - job.origin);
    to_render_transform(interpolate(previous.value, current, job.alpha), instance.transform);
    if (still) {
        instance.previous_transform = instance.transform; // most of a world
    } else {
        to_render_transform(pose_at(previous.value, current, job.previous_alpha), instance.previous_transform);
    }
    instance.mesh = mesh.mesh;
    instance.slot = slot.slot;
    return job.has_camera && std::max({away.x, away.y, away.z}) >= kRenderReachM;
}

struct MovedRowsJob {
    const ExtractJob* job = nullptr;
    const flecs::entity_t* entities = nullptr;
    RenderInstance* instances = nullptr; // one each
    const ecs_world_t* world = nullptr;  // the world itself: a stage won't do for a table's columns
    ecs_id_t current_id = 0;
    ecs_id_t previous_id = 0;
    ecs_id_t mesh_id = 0;
    ecs_id_t slot_id = 0;
    std::atomic<bool> out_of_reach{false};
};

void extract_moved_rows(size_t begin, size_t end, void* context) {
    MovedRowsJob& moved = *static_cast<MovedRowsJob*>(context);
    const auto column = [&moved](const ecs_table_t* table, ecs_id_t id) -> void* {
        const int32_t index = ecs_table_get_column_index(moved.world, table, id);
        return index >= 0 ? ecs_table_get_column(table, index, 0) : nullptr;
    };
    // Found once for a run of rows in one table, which is how they mostly come.
    const ecs_table_t* table = nullptr;
    const WorldTransform* current = nullptr;
    const PreviousWorldTransform* previous = nullptr;
    const MeshInstance* meshes = nullptr;
    RenderSlot* slots = nullptr;
    for (size_t i = begin; i < end; ++i) {
        RenderInstance& instance = moved.instances[i];
        instance.slot = kSkipRenderInstance;
        const flecs::entity_t entity = moved.entities[i];
        if (!ecs_is_alive(moved.world, entity)) {
            continue; // gone since it moved
        }
        const ecs_record_t* const record = ecs_record_find(moved.world, entity);
        if (record->table != table) {
            table = record->table;
            current = static_cast<const WorldTransform*>(column(table, moved.current_id));
            previous = static_cast<const PreviousWorldTransform*>(column(table, moved.previous_id));
            meshes = static_cast<const MeshInstance*>(column(table, moved.mesh_id));
            slots = static_cast<RenderSlot*>(column(table, moved.slot_id));
        }
        if (current == nullptr || previous == nullptr || meshes == nullptr || slots == nullptr) {
            continue; // nothing to draw
        }
        const auto row = static_cast<int32_t>(ECS_RECORD_TO_ROW(record->row));
        slots[row].moved_frame = moved.job->frame;
        const bool still = same_transform(previous[row].value, current[row]);
        if (send_row(*moved.job, current[row], previous[row], meshes[row], slots[row], still, instance)) {
            moved.out_of_reach.store(true, std::memory_order_relaxed);
        }
    }
}

void extract_chunks(size_t begin, size_t end, void* context) {
    const ExtractJob& job = *static_cast<const ExtractJob*>(context);
    for (size_t c = begin; c < end; ++c) {
        RenderExtractScratch::Chunk& chunk = job.scratch->chunks[c];
        for (uint32_t i = 0; i < chunk.count; ++i) {
            RenderInstance& instance = job.instances[chunk.first + i];
            // Sent already, and its moving is no reason to come back to its table next frame.
            if (chunk.slots[i].moved_frame == job.frame) {
                instance.slot = kSkipRenderInstance;
                continue;
            }
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
            chunk.out_of_reach =
                send_row(job, chunk.current[i], chunk.previous[i], chunk.meshes[i], slot, still, instance) ||
                chunk.out_of_reach;
        }
    }
}

} // namespace

void MovedRows::report(std::span<const flecs::entity_t> moved) {
    if (seen) {
        entities.clear();
        reports = 0;
        seen = false;
    }
    if (!moved.empty()) {
        const size_t at = entities.size();
        entities.resize(at + moved.size());
        std::memcpy(entities.data() + at, moved.data(), moved.size_bytes());
        ++reports;
    }
}

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
    // And one among effects from when it gets an EffectInstance.
    ecs.observer<EffectSlot>("de::GiveEffectSlot").event(flecs::OnAdd).each([scratch = desc.scratch](EffectSlot& slot) {
        if (!scratch->free_effect_slots.empty()) {
            slot.slot = scratch->free_effect_slots.back();
            scratch->free_effect_slots.pop();
        } else {
            slot.slot = scratch->next_effect_slot++;
        }
    });
    ecs.observer<EffectSlot>("de::FreeEffectSlot")
        .event(flecs::OnRemove)
        .each([scratch = desc.scratch](EffectSlot& slot) { scratch->free_effect_slots.push(slot.slot); });
    ecs.observer<EffectInstance>("de::DropEffectSlot")
        .event(flecs::OnRemove)
        .each([](flecs::entity entity, EffectInstance&) { entity.remove<EffectSlot>(); });
    // No terms, so it runs every frame, meshes or not: the camera (which may move the origin), and a fresh list of what
    // changed.
    ecs.system("de::ExtractRenderView").kind(phase).run([desc](flecs::iter& it) {
        DE_PROFILE_ZONE("render extract view");
        extract_camera(it.world(), *desc.alpha, *desc.frame);
        // What moves with the world's time has no motion across a cut, as the camera has none.
        desc.frame->previous_time_s = desc.frame->camera.previous_valid ? desc.frame->time_s : *desc.time_s;
        desc.frame->time_s = *desc.time_s;
        desc.frame->instances.clear(); // keeps its capacity: no allocation once the world has settled
        desc.frame->fog_volumes.clear();
        desc.frame->water_bodies.clear();
        desc.frame->effects.clear();
        desc.frame->lights.clear();
        desc.frame->instance_out_of_reach = false;
    });
    // Every effect there is, each frame: the renderer steps and draws what's near, and what's missing from the list
    // has gone. Where it's seen, like anything else that moves.
    ecs.system<const WorldTransform, const PreviousWorldTransform, const EffectInstance, const EffectSlot>(
           "de::ExtractRenderEffects")
        .kind(phase)
        .run([desc](flecs::iter& it) {
            DE_PROFILE_ZONE("render extract effects");
            while (it.next()) {
                const auto currents = it.field<const WorldTransform>(0);
                const auto previouses = it.field<const PreviousWorldTransform>(1);
                const auto effects = it.field<const EffectInstance>(2);
                const auto slots = it.field<const EffectSlot>(3);
                for (const auto entity_row : it) {
                    const WorldTransform seen =
                        interpolate(previouses[entity_row].value, currents[entity_row], *desc.alpha);
                    RenderEffect out{.effect = effects[entity_row].effect,
                                     .slot = slots[entity_row].slot,
                                     // Its own, so two of one effect don't flicker as one; the same while it lives.
                                     .seed = static_cast<uint32_t>(core::mix64(it.entity(entity_row).id())),
                                     .clock = effects[entity_row].clock,
                                     .hidden_emitters = effects[entity_row].hidden_emitters};
                    to_render_transform(seen, out.transform);
                    desc.frame->effects.push(out);
                }
            }
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
                              std::isfinite(volume.drift_mps[1]) && std::isfinite(volume.drift_mps[2]) &&
                              std::isfinite(volume.under_roof));
                    const WorldTransform seen = interpolate(previous.value, current, *desc.alpha);
                    const Vec3 center(seen.position - desc.frame->origin);
                    const glm::mat3 to_axes = glm::mat3_cast(glm::conjugate(seen.rotation));
                    const float noise_tiles_per_m = 1.0f / std::max(volume.noise_size_m, 1e-3f);
                    RenderFogVolume out{.density_per_m = volume.density_per_m,
                                        .edge_m = volume.edge_m,
                                        .ellipsoid = volume.ellipsoid,
                                        .noise_tiles_per_m = noise_tiles_per_m,
                                        .noise_amount = volume.noise_amount,
                                        .height_fade_m = volume.height_fade_m,
                                        .under_roof = volume.under_roof};
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
    // Every local light there is, each frame, as it shines now: the renderer keeps those that reach what's in view.
    ecs.system<const WorldTransform, const PreviousWorldTransform, const LocalLight>("de::ExtractRenderLights")
        .kind(phase)
        .run([desc](flecs::iter& it) {
            DE_PROFILE_ZONE("render extract lights");
            while (it.next()) {
                const auto currents = it.field<const WorldTransform>(0);
                const auto previouses = it.field<const PreviousWorldTransform>(1);
                const auto lights = it.field<const LocalLight>(2);
                for (const auto entity_row : it) {
                    const LocalLight& light = lights[entity_row];
                    // Cooked ones are checked at load; one set from code, here.
                    DE_ASSERT(light.desc().valid());
                    const WorldTransform seen =
                        interpolate(previouses[entity_row].value, currents[entity_row], *desc.alpha);
                    // As if the whole thing were made bigger: farther by the scale, brighter by its square.
                    const float scale =
                        std::max({std::abs(seen.scale.x), std::abs(seen.scale.y), std::abs(seen.scale.z)});
                    float shine = light.intensity * scale * scale;
                    if (light.flicker > 0.0f) {
                        const auto seed = static_cast<uint32_t>(core::mix64(it.entity(entity_row).id()));
                        shine *= 1.0f - (light.flicker * light_flicker(*desc.time_s, light.flicker_hz, seed));
                    }
                    const Vec3 center(seen.position - desc.frame->origin);
                    const Vec3 axis = seen.rotation * Vec3(0.0f, 0.0f, -1.0f);
                    RenderLight out{.position = {center.x, center.y, center.z},
                                    .radius_m = light.radius_m * scale,
                                    .color = {light.color[0] * shine, light.color[1] * shine, light.color[2] * shine},
                                    .direction = {axis.x, axis.y, axis.z},
                                    .shadow = light.shadow};
                    if (light.cone_deg > 0.0f) {
                        // All of its light inside where the edge's fade starts, none past the cone.
                        const float outer = glm::radians(light.cone_deg);
                        const float inner = outer * (1.0f - light.cone_soft);
                        out.cos_outer = std::cos(outer);
                        out.cone_scale = 1.0f / std::max(std::cos(inner) - out.cos_outer, 1e-4f);
                    }
                    desc.frame->lights.push(out);
                }
            }
        });
    // A few, like fog volumes. A lake doesn't move, but one placed in the editor may.
    ecs.system<const WorldTransform, const PreviousWorldTransform, const WaterBody>("de::ExtractRenderWaterBodies")
        .kind(phase)
        .run([desc](flecs::iter& it) {
            DE_PROFILE_ZONE("render extract water bodies");
            while (it.next()) {
                const auto currents = it.field<const WorldTransform>(0);
                const auto previouses = it.field<const PreviousWorldTransform>(1);
                const auto bodies = it.field<const WaterBody>(2);
                for (const auto entity_row : it) {
                    const WaterBody& body = bodies[entity_row];
                    DE_ASSERT(body.waves >= 0.0f && body.waves <= 1.0f);
                    const WorldTransform seen =
                        interpolate(previouses[entity_row].value, currents[entity_row], *desc.alpha);
                    const Vec3 center(seen.position - desc.frame->origin);
                    // Only its yaw: water lies flat whatever its entity's tilt.
                    const Vec3 x_axis = seen.rotation * Vec3(1.0f, 0.0f, 0.0f);
                    const float flat = std::sqrt((x_axis.x * x_axis.x) + (x_axis.z * x_axis.z));
                    RenderWaterBody out{.center = {center.x, center.y, center.z},
                                        .half_extents = {std::abs(seen.scale.x), std::abs(seen.scale.z)},
                                        .ellipse = body.ellipse,
                                        .waves = body.waves};
                    if (flat > 1e-4f) {
                        out.axis[0] = x_axis.x / flat;
                        out.axis[1] = x_axis.z / flat;
                    }
                    desc.frame->water_bodies.push(out);
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
            // Named once for each tick they moved in, if there were several since the last frame. All of them being
            // sent has them in it already.
            MovedRows& moved = scratch.moved_rows;
            if (moved.reports > 1) {
                std::ranges::sort(moved.entities.span());
                const auto repeats = std::ranges::unique(moved.entities.span());
                moved.entities.resize(moved.entities.size() - repeats.size());
                moved.reports = 1;
            }
            const size_t moved_count = all ? 0 : moved.entities.size();
            moved.seen = true;
            out.instances.resize_uninit(rows + moved_count);
            ExtractJob job{.scratch = &scratch,
                           .instances = out.instances.data(),
                           .alpha = *desc.alpha,
                           .previous_alpha = *desc.alpha - *desc.alpha_step,
                           .frame = out.number,
                           .origin = out.origin,
                           .has_camera = out.camera.valid,
                           .all = all};
            if (moved_count > 0) {
                const flecs::world world = it.world();
                MovedRowsJob moved_job{.job = &job,
                                       .entities = moved.entities.data(),
                                       .instances = out.instances.data() + rows,
                                       .world = ecs_get_world(world.c_ptr()),
                                       .current_id = world.id<WorldTransform>(),
                                       .previous_id = world.id<PreviousWorldTransform>(),
                                       .mesh_id = world.id<MeshInstance>(),
                                       .slot_id = world.id<RenderSlot>()};
                constexpr size_t kMovedBatch = 256;
                if (desc.jobs != nullptr && moved_count > kMovedBatch) {
                    desc.jobs->parallel_for(moved_count, kMovedBatch, &extract_moved_rows, &moved_job,
                                            "render extract moved rows");
                } else {
                    extract_moved_rows(0, moved_count, &moved_job);
                }
                out.instance_out_of_reach = moved_job.out_of_reach.load(std::memory_order_relaxed);
            }
            if (desc.jobs != nullptr && scratch.chunks.size() > 1) {
                desc.jobs->parallel_for(scratch.chunks.size(), 1, &extract_chunks, &job, "render extract");
            } else {
                extract_chunks(0, scratch.chunks.size(), &job);
            }
            for (const RenderExtractScratch::Chunk& chunk : scratch.chunks) {
                if (chunk.any_moving) {
                    scratch.moving_tables.find_or_insert(chunk.table) = out.number;
                }
                out.instance_out_of_reach = out.instance_out_of_reach || chunk.out_of_reach;
            }
            if (out.instance_out_of_reach && !scratch.warned_out_of_reach) {
                DE_LOG_ERROR("world",
                             "an instance is over %.0f km from the render origin, so it draws in the wrong place",
                             kRenderReachM / 1000.0);
                scratch.warned_out_of_reach = true;
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
