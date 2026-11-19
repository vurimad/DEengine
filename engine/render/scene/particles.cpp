#include "render/scene/particles.h"

#include "core/hash_map.h"
#include "core/log.h"
#include "core/profile.h"
#include "render/rhi/command_list.h"
#include "render/rhi/device.h"
#include "render/shader/shader_library.h"
#include "world/render_extract.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace de::render {

namespace {

constexpr uint32_t kBlockBits = 14; // shaders/scene/particles_common.slang: kParticleBlockBits
constexpr uint32_t kMaxListRecords = 1U << (32U - kBlockBits);
static_assert(kParticlePoolBlocks <= (1U << kBlockBits));
constexpr uint64_t kBlockBytes = uint64_t{kParticleBlockSize} * sizeof(GpuParticle);
// A particle with no particle in it: every float of it -1, so its age is below 0.
constexpr uint32_t kNoParticleWord = 0xbf800000U;
// An effect fades out over the last tenth of its draw distance.
constexpr float kFadeShare = 0.1f;
// An emitter is stepped while its particles could be this much past the view's edge, so one that comes into view has
// caught up before it shows.
constexpr float kNearViewScale = 1.25f;
constexpr float kNearViewMarginM = 2.0f;

// Matches ParticleEmitter in shaders/scene/particles_common.slang.
struct GpuParticleEmitter {
    float rows[3][4] = {};
    float from[4] = {};
    uint32_t anchor_steps[3] = {};
    uint32_t flags = 0;
    uint32_t first_particle = 0;
    uint32_t capacity = 0;
    uint32_t seed = 0;
    float opacity = 0.0f;
    float lifetime[2] = {};
    float intensity = 1.0f;
    float soft_m = 0.0f;
    uint32_t birth_first = 0;
    uint32_t birth_words = 0;
    uint32_t step_first = 0;
    uint32_t step_words = 0;
    uint32_t draw_first = 0;
    uint32_t draw_words = 0;
    float since_step_s = 0.0f;
    uint32_t reserved = 0;
};
static_assert(sizeof(GpuParticleEmitter) == 144);

// Matches ParticleEmitterStep.
struct GpuParticleEmitterStep {
    uint32_t emitter = 0;
    uint32_t ring_first = 0;
    uint32_t born_count = 0;
    uint32_t serial_first = 0;
    float from_t = 0.0f;
    float to_t = 1.0f;
    float time_s = 0.0f;
    uint32_t reserved = 0;
};
static_assert(sizeof(GpuParticleEmitterStep) == 32);

// Matches ParticleStepPush in shaders/scene/particles_step.slang.
struct ParticleStepPush {
    uint64_t frame = 0;
    uint64_t particles = 0;
    uint64_t emitters = 0;
    uint64_t steps = 0;
    uint64_t words = 0;
    uint64_t blocks = 0;
    uint32_t block_count = 0;
    float step_s = 0.0f;
    uint32_t reserved[2] = {};
};
static_assert(sizeof(ParticleStepPush) == 64);

// Matches ParticleDrawPush in shaders/scene/particles.slang.
struct ParticleDrawPush {
    uint64_t frame = 0;
    uint64_t particles = 0;
    uint64_t emitters = 0;
    uint64_t words = 0;
    uint64_t blocks = 0;
    uint32_t depth = 0;
    uint32_t atlas = 0;
    float camera_right[3] = {};
    uint32_t reserved = 0;
    float camera_up[3] = {};
    float near_m = 0.1f;
    uint32_t integrated = 0;
    uint32_t grid_sampler = 0;
    uint32_t fog = 0;
    uint32_t froxels = 0;
};
static_assert(sizeof(ParticleDrawPush) == 96);

// One emitter of one effect in the world, between frames.
struct EmitterState {
    uint32_t first_block = ParticleBlocks::kNone; // its run in the pool; none while out of reach, or starved
    uint32_t block_count = 0;
    uint64_t steps_done = 0;      // steps its particles have taken, of those its effect's age comes to
    uint64_t born = 0;            // particles born by then
    float stepped_offset[3] = {}; // where it was at its last step, relative to its instance's anchor
    bool has_particles = false;   // some step has run since its blocks were last cleared
};

// One effect in the world, between frames: at RenderEffect::slot.
struct InstanceState {
    asset::AssetHandle effect;
    uint32_t effect_index = 0; // into effects
    uint32_t revision = 0;     // of the asset its emitters were made from; 0: none yet
    uint32_t seed = 0;
    uint32_t first_emitter = 0; // a run of emitter_count in emitter_states
    uint32_t emitter_count = 0;
    uint64_t seen = 0;               // the last frame it was in
    uint32_t anchor_steps[3] = {};   // what its world-space particles are relative to: where it first held blocks
    uint32_t position_steps[3] = {}; // where it was last frame
    // Its clock this frame (world::EffectClock): the whole steps its age comes to, and how far past the last it is.
    uint64_t steps = 0;
    float since_step_s = 0.0f;
    bool live = false;
    bool anchored = false;
    bool placed = false; // position_steps holds last frame's
};

// An effect asset some instance uses.
struct EffectState {
    asset::AssetHandle handle;
    const asset::EffectAsset* asset = nullptr; // this frame's; null while it isn't loaded
    uint32_t revision = 0;
    uint32_t users = 0;
    uint32_t words_at = 0;    // where its words are in this frame's
    uint64_t words_frame = 0; // the frame that holds for
};

struct DrawnEmitter {
    float distance = 0.0f;
    uint32_t record = 0;
    uint32_t first_block = 0;
    uint32_t block_count = 0;
};

struct ClearedRun {
    uint32_t first_block = 0;
    uint32_t block_count = 0;
};

core::Allocator& render_heap() {
    return core::heap_allocator(core::MemoryTag::Render);
}

uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

// A well-mixed 32 bits from two.
uint32_t mix_seed(uint32_t a, uint32_t b) {
    return static_cast<uint32_t>(core::mix64((uint64_t{a} << 32U) | b));
}

} // namespace

ParticleBlocks::ParticleBlocks(uint32_t blocks) : blocks_(blocks), free_(blocks) {
    if (blocks > 0) {
        runs_.push({.first = 0, .count = blocks});
    }
}

uint32_t ParticleBlocks::take(uint32_t count) {
    DE_ASSERT(count > 0);
    for (size_t i = 0; i < runs_.size(); ++i) {
        Run& run = runs_[i];
        if (run.count < count) {
            continue;
        }
        const uint32_t first = run.first;
        run.first += count;
        run.count -= count;
        if (run.count == 0) {
            runs_.remove_ordered(i);
        }
        free_ -= count;
        return first;
    }
    return kNone;
}

void ParticleBlocks::give(uint32_t first, uint32_t count) {
    DE_ASSERT(count > 0 && uint64_t{first} + count <= blocks_);
    // Where it goes among the free runs: before the first one past it.
    size_t at = 0;
    while (at < runs_.size() && runs_[at].first < first) {
        ++at;
    }
    DE_ASSERT_MSG((at == 0 || runs_[at - 1].first + runs_[at - 1].count <= first) &&
                      (at == runs_.size() || first + count <= runs_[at].first),
                  "particle blocks given back twice");
    free_ += count;
    const bool joins_before = at > 0 && runs_[at - 1].first + runs_[at - 1].count == first;
    const bool joins_after = at < runs_.size() && first + count == runs_[at].first;
    if (joins_before && joins_after) {
        runs_[at - 1].count += count + runs_[at].count;
        runs_.remove_ordered(at);
    } else if (joins_before) {
        runs_[at - 1].count += count;
    } else if (joins_after) {
        runs_[at].first = first;
        runs_[at].count += count;
    } else {
        runs_.push({.first = first, .count = count});
        std::rotate(runs_.begin() + at, runs_.end() - 1, runs_.end());
    }
}

uint64_t particles_born(float rate_per_s, uint64_t steps) {
    // Divided last: a rate that goes into the steps a whole number of times gives exactly that, with nothing lost
    // to a step that isn't a whole number of anything a float holds.
    return static_cast<uint64_t>(
        std::floor(static_cast<double>(rate_per_s) * static_cast<double>(steps) / asset::kEffectStepsPerS));
}

uint64_t particle_steps(double seconds) {
    return seconds <= 0.0 ? 0 : static_cast<uint64_t>(std::ceil((seconds * asset::kEffectStepsPerS) - 1e-6));
}

ParticleView ParticleView::from_camera(const float (&camera)[12], float vertical_fov_rad, float near_m, float aspect) {
    const Vec3 right = glm::normalize(Vec3(camera[0], camera[4], camera[8]));
    const Vec3 up = glm::normalize(Vec3(camera[1], camera[5], camera[9]));
    const Vec3 forward = glm::normalize(Vec3(-camera[2], -camera[6], -camera[10]));
    const float tan_y = std::tan(vertical_fov_rad * 0.5f);
    const float tan_x = tan_y * aspect;
    ParticleView view;
    view.position = Vec3(camera[3], camera[7], camera[11]);
    view.near_m = near_m;
    view.planes[0] = forward;
    view.planes[1] = glm::normalize(right + (forward * tan_x));
    view.planes[2] = glm::normalize((forward * tan_x) - right);
    view.planes[3] = glm::normalize(up + (forward * tan_y));
    view.planes[4] = glm::normalize((forward * tan_y) - up);
    return view;
}

bool ParticleView::sees(Vec3 center, float radius) const {
    const Vec3 to = center - position;
    if (glm::dot(to, planes[0]) - near_m < -radius) {
        return false;
    }
    for (int side = 1; side < 5; ++side) {
        if (glm::dot(to, planes[side]) < -radius) {
            return false;
        }
    }
    return true;
}

struct ParticleSystemState {
    rhi::Device* device = nullptr;
    shader::ShaderLibrary* shaders = nullptr;
    asset::AssetRegistry* assets = nullptr;
    rhi::BufferHandle pool;
    uint64_t pool_address = 0;
    rhi::Format coverage_format = rhi::Format::R8Unorm;
    bool pool_used = false; // by some frame: the graph is told how that left it
    shader::ProgramHandle step_program;
    shader::ProgramHandle draw_program;
    ParticleBlocks blocks;

    // The world's time at the last frame: how long this one is, for how fast an effect moved.
    double last_time_s = 0.0;
    bool has_time = false;
    uint64_t frame = 0; // add_passes calls so far

    core::Array<InstanceState> instances{render_heap()}; // by RenderEffect::slot
    core::Array<EmitterState> emitter_states{render_heap()};
    // Runs of emitter_states no instance holds, by how long they are.
    core::Array<uint32_t> free_emitter_runs[asset::EffectLimits::kMaxEmitters + 1];
    core::Array<EffectState> effects{render_heap()};
    core::Array<uint32_t> free_effects{render_heap()};
    core::HashMap<asset::AssetHandle, uint32_t> effect_of{render_heap()};

    // This frame's, kept between frames so nothing is allocated once settled.
    core::Array<GpuParticleEmitter> records{render_heap()};
    core::Array<GpuParticleEmitterStep> step_records{render_heap()};
    core::Array<uint32_t> words{render_heap()};
    core::Array<uint32_t> draw_blocks{render_heap()};
    // A list of blocks for each step a frame may take (ParticleSystemDesc::catch_up_steps), and where each list is
    // in the frame's buffer.
    core::Array<core::Array<uint32_t>> step_blocks{render_heap()};
    core::Array<uint64_t> lists_at{render_heap()};
    uint32_t catch_up_steps = kParticleCatchUpSteps;
    core::Array<DrawnEmitter> drawn{render_heap()};
    core::Array<ClearedRun> cleared{render_heap()};

    rhi::BufferHandle frame_buffers[rhi::kFramesInFlight];
    uint64_t frame_capacity[rhi::kFramesInFlight] = {};

    ParticleStats stats;

    explicit ParticleSystemState(uint32_t pool_blocks) : blocks(pool_blocks) {}

    // --- What instances hold ---

    uint32_t take_emitter_states(uint32_t count) {
        core::Array<uint32_t>& free = free_emitter_runs[count];
        if (!free.empty()) {
            const uint32_t first = free.back();
            free.pop();
            for (uint32_t e = 0; e < count; ++e) {
                emitter_states[first + e] = EmitterState{};
            }
            return first;
        }
        const auto first = static_cast<uint32_t>(emitter_states.size());
        emitter_states.resize(emitter_states.size() + count);
        return first;
    }

    void give_blocks(EmitterState& emitter) {
        if (emitter.first_block != ParticleBlocks::kNone) {
            blocks.give(emitter.first_block, emitter.block_count);
        }
        emitter = EmitterState{};
    }

    // Its emitters' blocks back to the pool, and its emitters forgotten: it starts again when it's next wanted.
    void release_emitters(InstanceState& instance) {
        for (uint32_t e = 0; e < instance.emitter_count; ++e) {
            give_blocks(emitter_states[instance.first_emitter + e]);
        }
        if (instance.emitter_count > 0) {
            free_emitter_runs[instance.emitter_count].push(instance.first_emitter);
        }
        instance.emitter_count = 0;
        instance.revision = 0;
        instance.anchored = false;
    }

    uint32_t use_effect(asset::AssetHandle handle) {
        if (const uint32_t* known = effect_of.find(handle); known != nullptr) {
            ++effects[*known].users;
            return *known;
        }
        uint32_t index = 0;
        if (!free_effects.empty()) {
            index = free_effects.back();
            free_effects.pop();
        } else {
            index = static_cast<uint32_t>(effects.size());
            effects.emplace();
        }
        effects[index] = EffectState{.handle = handle, .users = 1};
        effect_of.set(handle, index);
        refresh(effects[index]);
        return index;
    }

    void release(InstanceState& instance) {
        if (!instance.live) {
            return;
        }
        release_emitters(instance);
        EffectState& effect = effects[instance.effect_index];
        if (--effect.users == 0) {
            effect_of.remove(effect.handle);
            effect = EffectState{};
            free_effects.push(instance.effect_index);
        }
        instance = InstanceState{};
    }

    // What the registry has for it now.
    void refresh(EffectState& effect) const {
        effect.asset = assets->get<asset::EffectAsset>(effect.handle);
        effect.revision = effect.asset != nullptr ? assets->revision(effect.handle) : 0;
    }

    // Where the effect's words are in this frame's, copied in the first time a frame asks.
    uint32_t words_of(EffectState& effect) {
        if (effect.words_frame != frame) {
            effect.words_frame = frame;
            effect.words_at = static_cast<uint32_t>(words.size());
            for (const uint32_t word : effect.asset->words.get(effect.asset)) {
                words.push(word);
            }
        }
        return effect.words_at;
    }

    // This frame slot's buffer, at least this big: written now, read by this frame's GPU work.
    rhi::BufferHandle frame_buffer(uint64_t needed) {
        const auto slot = static_cast<uint32_t>(device->frame_index() % rhi::kFramesInFlight);
        if (frame_capacity[slot] < needed) {
            device->destroy(frame_buffers[slot]);
            const uint64_t capacity = std::max<uint64_t>(needed + (needed / 2), uint64_t{16} * 1024);
            const core::Result<rhi::BufferHandle> buffer = device->create_buffer({.size = capacity,
                                                                                  .usage = rhi::BufferUsage::Storage,
                                                                                  .memory = rhi::MemoryLocation::Upload,
                                                                                  .debug_name = "particles frame"});
            frame_buffers[slot] = buffer ? *buffer : rhi::BufferHandle{};
            frame_capacity[slot] = buffer ? capacity : 0;
        }
        return frame_buffers[slot];
    }

    // --- A frame ---

    // What one emitter of one instance does this frame: takes or gives back its blocks, steps as far as it's owed
    // (listing its blocks for each step), and is listed to draw if it's in view and caught up.
    void update_emitter(const world::RenderEffect& placed, InstanceState& instance, EffectState& effect, uint32_t e,
                        const ParticleView& view, Vec3 center, float distance, float scale, float trail_m,
                        float wind_mps) {
        const asset::EffectAsset& asset = *effect.asset;
        const asset::EffectEmitter& def = asset.emitters.get(&asset)[e];
        EmitterState& emitter = emitter_states[instance.first_emitter + e];
        const bool local = core::has_any(def.flags, asset::EffectEmitterFlags::Local);
        const float reach =
            (def.reach_m * std::max(scale, 1.0f)) + (def.wind_reach_s * wind_mps) + (local ? 0.0f : trail_m);
        if (distance - reach > asset.draw_distance_m) {
            give_blocks(emitter); // too far to show: its particles go, and it starts again settled when it's back
            return;
        }
        const uint32_t block_count = (def.capacity + kParticleBlockSize - 1) / kParticleBlockSize;
        if (emitter.first_block == ParticleBlocks::kNone) {
            const uint32_t first = blocks.take(block_count);
            if (first == ParticleBlocks::kNone) {
                ++stats.emitters_starved;
                return;
            }
            emitter = EmitterState{.first_block = first, .block_count = block_count};
            cleared.push({.first_block = first, .block_count = block_count});
            if (!instance.anchored) {
                instance.anchored = true;
                for (int axis = 0; axis < 3; ++axis) {
                    instance.anchor_steps[axis] = placed.transform.rows[axis].position_steps;
                }
            }
        }
        ++stats.emitters;
        stats.blocks += emitter.block_count;
        const float mean_life = 0.5f * (def.lifetime_s[0] + def.lifetime_s[1]);
        stats.particles +=
            static_cast<uint32_t>(std::min({static_cast<double>(def.capacity), static_cast<double>(emitter.born),
                                            static_cast<double>(def.rate_per_s * mean_life)}));

        // Where it is, relative to its anchor (world space), and where it was at its last step.
        float offset[3] = {};
        if (!local) {
            for (int axis = 0; axis < 3; ++axis) {
                const auto steps =
                    static_cast<int32_t>(placed.transform.rows[axis].position_steps - instance.anchor_steps[axis]);
                offset[axis] = static_cast<float>(static_cast<double>(steps) / world::kRenderStepsPerM);
            }
        }
        if (!emitter.has_particles && emitter.steps_done == 0) {
            std::memcpy(emitter.stepped_offset, offset, sizeof(offset)); // nowhere before: it starts where it is
        }

        // How far behind its effect's clock it is. Further than any of its particles lives, and nothing in its blocks
        // would last to now: they're cleared and it's put a lifetime short of now, which leaves exactly the particles
        // stepping all the way would have. Ahead of it (the clock was set back): from nothing, the same way.
        const uint64_t life_steps = particle_steps(def.lifetime_s[1]) + 1;
        const uint64_t target = instance.steps + particle_steps(def.prewarm_s);
        if (emitter.steps_done > target) {
            if (emitter.has_particles) {
                cleared.push({.first_block = emitter.first_block, .block_count = emitter.block_count});
                emitter.has_particles = false;
            }
            emitter.steps_done = 0;
            emitter.born = 0;
        }
        uint64_t owed = target - emitter.steps_done;
        const bool near_view = view.sees(center, (reach * kNearViewScale) + kNearViewMarginM);
        uint32_t record = UINT32_MAX;
        if (near_view && owed > 0) {
            if (owed > life_steps) {
                if (emitter.has_particles) {
                    cleared.push({.first_block = emitter.first_block, .block_count = emitter.block_count});
                    emitter.has_particles = false;
                }
                emitter.steps_done = target - life_steps;
                emitter.born = particles_born(def.rate_per_s, emitter.steps_done);
                owed = life_steps;
            }
            const auto run = static_cast<uint32_t>(std::min<uint64_t>(owed, catch_up_steps));
            record = add_record(placed, instance, effect, def, emitter, e, offset, distance);
            for (uint32_t j = 0; j < run && record != UINT32_MAX; ++j) {
                if (step_records.size() >= kMaxListRecords) {
                    break; // more than a frame's lists can name: the rest waits a frame
                }
                const uint64_t born_after = particles_born(def.rate_per_s, emitter.steps_done + 1);
                // More born in a step than its ring holds: only the last of them fit.
                const uint64_t count = std::min<uint64_t>(born_after - emitter.born, def.capacity);
                const uint64_t first_serial = born_after - count;
                const auto step_record = static_cast<uint32_t>(step_records.size());
                step_records.push({.emitter = record,
                                   .ring_first = static_cast<uint32_t>(first_serial % def.capacity),
                                   .born_count = static_cast<uint32_t>(count),
                                   .serial_first = static_cast<uint32_t>(first_serial),
                                   .from_t = static_cast<float>(j) / static_cast<float>(run),
                                   .to_t = static_cast<float>(j + 1) / static_cast<float>(run),
                                   // Its own clock at the end of the step, wrapped to keep a float's precision:
                                   // what turbulence changes by, the same every time it starts again.
                                   .time_s = static_cast<float>(std::fmod(
                                       static_cast<double>(emitter.steps_done + 1) * asset::kEffectStepS, 4096.0))});
                for (uint32_t b = 0; b < emitter.block_count; ++b) {
                    step_blocks[j].push((emitter.first_block + b) | (step_record << kBlockBits));
                }
                stats.blocks_stepped += emitter.block_count;
                ++emitter.steps_done;
                emitter.born = born_after;
                emitter.has_particles = true;
                --owed;
            }
            std::memcpy(emitter.stepped_offset, offset, sizeof(offset));
        }
        // Drawn once it's where its clock says, and only what the view really holds; not if it's left out.
        const bool hidden = (placed.hidden_emitters & (1U << e)) != 0;
        if (owed == 0 && emitter.has_particles && !hidden && view.sees(center, reach)) {
            if (record == UINT32_MAX) {
                record = add_record(placed, instance, effect, def, emitter, e, offset, distance);
            }
            if (record != UINT32_MAX) {
                drawn.push({.distance = distance,
                            .record = record,
                            .first_block = emitter.first_block,
                            .block_count = emitter.block_count});
            }
        }
    }

    // The emitter as the shaders read it this frame; UINT32_MAX if a frame's lists can't name another.
    uint32_t add_record(const world::RenderEffect& placed, const InstanceState& instance, EffectState& effect,
                        const asset::EffectEmitter& def, const EmitterState& emitter, uint32_t e,
                        const float (&offset)[3], float distance) {
        if (records.size() >= kMaxListRecords) {
            return UINT32_MAX;
        }
        const bool local = core::has_any(def.flags, asset::EffectEmitterFlags::Local);
        const uint32_t words_at = words_of(effect);
        const float draw_distance = effect.asset->draw_distance_m;
        GpuParticleEmitter gpu;
        for (int axis = 0; axis < 3; ++axis) {
            std::memcpy(gpu.rows[axis], placed.transform.rows[axis].linear, sizeof(float) * 3);
            gpu.rows[axis][3] = offset[axis];
            gpu.from[axis] = emitter.stepped_offset[axis];
            // Local particles are in the emitter's own axes, wherever it is now.
            gpu.anchor_steps[axis] = local ? placed.transform.rows[axis].position_steps : instance.anchor_steps[axis];
        }
        gpu.from[3] = std::clamp((draw_distance - distance) / (draw_distance * kFadeShare), 0.0f, 1.0f);
        gpu.flags = static_cast<uint32_t>(def.flags) | (static_cast<uint32_t>(def.shape) << 8U);
        gpu.first_particle = emitter.first_block * kParticleBlockSize;
        gpu.capacity = def.capacity;
        gpu.seed = mix_seed(instance.seed, e);
        gpu.opacity = def.opacity;
        gpu.lifetime[0] = def.lifetime_s[0];
        gpu.lifetime[1] = def.lifetime_s[1];
        gpu.intensity = def.intensity;
        gpu.soft_m = def.soft_m;
        gpu.birth_first = words_at + def.birth.first;
        gpu.birth_words = def.birth.words;
        gpu.step_first = words_at + def.step.first;
        gpu.step_words = def.step.words;
        gpu.draw_first = words_at + def.draw.first;
        gpu.draw_words = def.draw.words;
        gpu.since_step_s = instance.since_step_s;
        records.push(gpu);
        return static_cast<uint32_t>(records.size() - 1);
    }

    // Every effect of the frame taken in: what's stepped and drawn is listed, what's gone is let go.
    void update(const world::RenderFrame& render_frame, float aspect, float wind_mps) {
        ++frame;
        const double frame_s = has_time ? std::max(render_frame.time_s - last_time_s, 0.0) : 0.0;
        last_time_s = render_frame.time_s;
        has_time = true;
        records.clear();
        step_records.clear();
        words.clear();
        draw_blocks.clear();
        drawn.clear();
        cleared.clear();
        for (core::Array<uint32_t>& list : step_blocks) {
            list.clear();
        }
        stats = ParticleStats{.pool_blocks = blocks.blocks()};
        for (EffectState& effect : effects) {
            if (effect.users > 0) {
                refresh(effect);
            }
        }
        const ParticleView view = ParticleView::from_camera(
            render_frame.camera.transform, render_frame.camera.vertical_fov_rad, render_frame.camera.near_m, aspect);
        // Who's in the frame, then who isn't: what a departed effect held is free for one that arrived with it.
        for (const world::RenderEffect& placed : render_frame.effects) {
            ++stats.effects;
            if (placed.slot >= instances.size()) {
                instances.resize(size_t{placed.slot} + 1);
            }
            InstanceState& instance = instances[placed.slot];
            if (instance.live && (instance.effect != placed.effect || instance.seed != placed.seed)) {
                release(instance); // another's slot, taken over
            }
            instance.seen = frame;
        }
        for (InstanceState& instance : instances) {
            if (instance.live && instance.seen != frame) {
                release(instance);
            }
        }
        for (const world::RenderEffect& placed : render_frame.effects) {
            InstanceState& instance = instances[placed.slot];
            if (!instance.live) {
                instance.live = true;
                instance.effect = placed.effect;
                instance.seed = placed.seed;
                instance.effect_index = use_effect(placed.effect);
                instance.seen = frame;
            }
            EffectState& effect = effects[instance.effect_index];
            if (effect.asset == nullptr) {
                release_emitters(instance); // not loaded, or gone: it waits
                continue;
            }
            // Its age in whole steps, and what's left over: what its particles are carried on by when drawn. A
            // millionth of a step is taken as reached, so an age that is a whole number of steps but for a rounding
            // doesn't wait a frame for it.
            const double age_steps = (placed.clock.age_at(render_frame.time_s) * asset::kEffectStepsPerS) + 1e-6;
            instance.steps = static_cast<uint64_t>(age_steps);
            instance.since_step_s = static_cast<float>(
                std::max(age_steps - static_cast<double>(instance.steps) - 1e-6, 0.0) * asset::kEffectStepS);
            if (instance.revision != effect.revision) {
                // The first frame it's loaded, or it was cooked again: from the start.
                release_emitters(instance);
                instance.revision = effect.revision;
                instance.emitter_count = effect.asset->emitters.count;
                instance.first_emitter = take_emitter_states(instance.emitter_count);
            }
            const Vec3 center(placed.transform.position_m(0, render_frame.origin),
                              placed.transform.position_m(1, render_frame.origin),
                              placed.transform.position_m(2, render_frame.origin));
            const float distance = glm::length(center - view.position);
            // How big it's scaled, and how far a particle it left behind may trail it: as fast as it moved this
            // frame, for as long as one lives.
            float scale = 0.0f;
            float moved_m = 0.0f;
            for (int axis = 0; axis < 3; ++axis) {
                const world::RenderTransform::Row* rows = placed.transform.rows;
                scale = std::max(scale,
                                 glm::length(Vec3(rows[0].linear[axis], rows[1].linear[axis], rows[2].linear[axis])));
                const auto steps = static_cast<int32_t>(rows[axis].position_steps - instance.position_steps[axis]);
                const auto along = static_cast<float>(static_cast<double>(steps) / world::kRenderStepsPerM);
                moved_m += instance.placed ? along * along : 0.0f;
                instance.position_steps[axis] = rows[axis].position_steps;
            }
            instance.placed = true;
            const float speed_mps = frame_s > 1e-4 ? std::sqrt(moved_m) / static_cast<float>(frame_s) : 0.0f;
            const std::span<const asset::EffectEmitter> defs = effect.asset->emitters.get(effect.asset);
            for (uint32_t e = 0; e < instance.emitter_count; ++e) {
                update_emitter(placed, instance, effect, e, view, center, distance, scale,
                               speed_mps * defs[e].lifetime_s[1], wind_mps);
            }
        }
        // The farthest emitter first; of two as far, the one listed first, so they don't swap from frame to frame.
        std::ranges::sort(drawn, [](const DrawnEmitter& a, const DrawnEmitter& b) {
            return a.distance != b.distance ? a.distance > b.distance : a.record < b.record;
        });
        for (const DrawnEmitter& emitter : drawn) {
            for (uint32_t b = 0; b < emitter.block_count; ++b) {
                draw_blocks.push((emitter.first_block + b) | (emitter.record << kBlockBits));
            }
        }
        stats.emitters_drawn = static_cast<uint32_t>(drawn.size());
        stats.blocks_drawn = static_cast<uint32_t>(draw_blocks.size());
    }
};

core::Result<ParticleSystem> ParticleSystem::create(const ParticleSystemDesc& desc) {
    if (desc.device == nullptr || desc.shaders == nullptr || desc.assets == nullptr || desc.pool_blocks == 0 ||
        desc.pool_blocks > (1U << kBlockBits) || desc.catch_up_steps == 0 ||
        desc.catch_up_steps > kMostParticleCatchUpSteps) {
        return core::fail(core::ErrorCode::InvalidArgument,
                          "the particle system needs a device, a shader library, a registry, a pool and some steps to "
                          "catch up by");
    }
    const core::Result<rhi::BufferHandle> pool = desc.device->create_buffer(
        {.size = uint64_t{desc.pool_blocks} * kBlockBytes,
         .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst | rhi::BufferUsage::CopySrc,
         .debug_name = "particle pool"});
    if (!pool) {
        return core::fail(pool.error());
    }
    auto* state = ::new (render_heap().allocate(sizeof(ParticleSystemState), alignof(ParticleSystemState)))
        ParticleSystemState(desc.pool_blocks);
    state->coverage_format = desc.coverage_format;
    state->device = desc.device;
    state->shaders = desc.shaders;
    state->assets = desc.assets;
    state->pool = *pool;
    state->pool_address = desc.device->buffer_address(*pool);
    for (core::Array<uint32_t>& list : state->free_emitter_runs) {
        list = core::Array<uint32_t>(render_heap());
    }
    state->catch_up_steps = desc.catch_up_steps;
    state->step_blocks.resize(desc.catch_up_steps);
    state->lists_at.resize(desc.catch_up_steps, 0);
    for (core::Array<uint32_t>& list : state->step_blocks) {
        list = core::Array<uint32_t>(render_heap());
    }
    const core::Result<shader::ProgramHandle> step =
        desc.shaders->create_compute({.module = "scene.particles_step", .debug_name = "particles step"});
    state->step_program = step ? *step : shader::ProgramHandle{};
    // Over the lit image and the coverage by their alpha, premultiplied, as the see-through meshes are: tested against
    // the scene's depth and writing none, and a quad has no back.
    rhi::GraphicsPipelineDesc draw{.color_count = 2,
                                   .depth_format = desc.depth_format,
                                   .depth_test = true,
                                   .depth_write = false,
                                   .depth_compare = rhi::CompareOp::GreaterEqual,
                                   .cull = rhi::CullMode::None,
                                   .debug_name = "particles"};
    draw.color_formats[0] = desc.color_format;
    draw.color_formats[1] = desc.coverage_format;
    draw.blend[0] = rhi::BlendMode::Premultiplied;
    draw.blend[1] = rhi::BlendMode::Premultiplied;
    const core::Result<shader::ProgramHandle> program =
        desc.shaders->create_graphics({.module = "scene.particles", .state = draw});
    state->draw_program = program ? *program : shader::ProgramHandle{};
    if (!step || !program) {
        DE_LOG_WARN("render", "the particles' shaders didn't build: no particles until they do");
    }
    return ParticleSystem(state);
}

ParticleSystem::~ParticleSystem() {
    if (state_ == nullptr) {
        return;
    }
    state_->shaders->destroy(state_->step_program);
    state_->shaders->destroy(state_->draw_program);
    state_->device->destroy(state_->pool);
    for (const rhi::BufferHandle buffer : state_->frame_buffers) {
        state_->device->destroy(buffer);
    }
    state_->~ParticleSystemState();
    render_heap().deallocate(state_, sizeof(ParticleSystemState), alignof(ParticleSystemState));
}

ParticleSystem::ParticleSystem(ParticleSystem&& other) noexcept : state_(other.state_) {
    other.state_ = nullptr;
}

ParticleSystem& ParticleSystem::operator=(ParticleSystem&& other) noexcept {
    if (this != &other) {
        std::swap(state_, other.state_);
    }
    return *this;
}

ParticleStats ParticleSystem::stats() const {
    return state_->stats;
}

rhi::BufferHandle ParticleSystem::pool() const {
    return state_->pool;
}

bool ParticleSystem::emitter_run(uint32_t effect_slot, uint32_t emitter, uint32_t& first_particle,
                                 uint32_t& capacity) const {
    const ParticleSystemState& state = *state_;
    if (effect_slot >= state.instances.size()) {
        return false;
    }
    const InstanceState& instance = state.instances[effect_slot];
    if (!instance.live || emitter >= instance.emitter_count) {
        return false;
    }
    const EmitterState& held = state.emitter_states[instance.first_emitter + emitter];
    const asset::EffectAsset* asset = state.effects[instance.effect_index].asset;
    if (held.first_block == ParticleBlocks::kNone || asset == nullptr) {
        return false;
    }
    first_particle = held.first_block * kParticleBlockSize;
    capacity = asset->emitters.get(asset)[emitter].capacity;
    return true;
}

bool ParticleSystem::emitter_stats(uint32_t effect_slot, uint32_t emitter, ParticleEmitterStats& out) const {
    const ParticleSystemState& state = *state_;
    out = ParticleEmitterStats{};
    if (effect_slot >= state.instances.size()) {
        return false;
    }
    const InstanceState& instance = state.instances[effect_slot];
    const asset::EffectAsset* asset = instance.live ? state.effects[instance.effect_index].asset : nullptr;
    if (asset == nullptr || emitter >= instance.emitter_count) {
        return false;
    }
    const EmitterState& held = state.emitter_states[instance.first_emitter + emitter];
    const asset::EffectEmitter& def = asset->emitters.get(asset)[emitter];
    out.held = held.first_block != ParticleBlocks::kNone;
    out.capacity = def.capacity;
    out.steps_done = held.steps_done;
    out.born = held.born;
    // Those born within a mean lifetime of its last step.
    const uint64_t life_steps = particle_steps(0.5 * (static_cast<double>(def.lifetime_s[0]) + def.lifetime_s[1]));
    const uint64_t before =
        held.steps_done > life_steps ? particles_born(def.rate_per_s, held.steps_done - life_steps) : 0;
    out.alive = static_cast<uint32_t>(std::min<uint64_t>(held.born - std::min(before, held.born), def.capacity));
    return true;
}

GraphTexture ParticleSystem::add_passes(RenderGraph& graph, const ParticlePassInput& input) {
    DE_PROFILE_ZONE("particles");
    ParticleSystemState& state = *state_;
    const rhi::PipelineHandle step_pipeline = state.shaders->pipeline(state.step_program);
    const rhi::PipelineHandle draw_pipeline = state.shaders->pipeline(state.draw_program);
    if (input.frame == nullptr || !input.frame->camera.valid || !step_pipeline.is_valid() ||
        !draw_pipeline.is_valid()) {
        return input.coverage;
    }
    const world::RenderFrame& frame = *input.frame;
    state.update(frame, input.aspect, input.wind_mps);
    uint32_t step_count = 0;
    while (step_count < state.catch_up_steps && !state.step_blocks[step_count].empty()) {
        ++step_count;
    }
    state.stats.steps = step_count;
    if (state.cleared.empty() && step_count == 0 && state.draw_blocks.empty()) {
        return input.coverage;
    }

    // This frame's emitters, steps, words and lists into one buffer.
    const uint64_t records_at = 0;
    const uint64_t steps_at = align_up(records_at + (state.records.size() * sizeof(GpuParticleEmitter)), 16);
    const uint64_t words_at = align_up(steps_at + (state.step_records.size() * sizeof(GpuParticleEmitterStep)), 16);
    const uint64_t draw_at = align_up(words_at + (state.words.size() * sizeof(uint32_t)), 16);
    core::Array<uint64_t>& lists_at = state.lists_at;
    uint64_t end = align_up(draw_at + (state.draw_blocks.size() * sizeof(uint32_t)), 16);
    for (uint32_t j = 0; j < step_count; ++j) {
        lists_at[j] = end;
        end = align_up(end + (state.step_blocks[j].size() * sizeof(uint32_t)), 16);
    }
    const rhi::BufferHandle buffer = state.frame_buffer(std::max<uint64_t>(end, 16));
    if (!buffer.is_valid()) {
        return input.coverage;
    }
    const std::span<std::byte> mapped = state.device->mapped(buffer);
    const auto write = [&mapped](uint64_t at, const void* data, size_t bytes) {
        if (bytes > 0) {
            std::memcpy(mapped.data() + at, data, bytes);
        }
    };
    write(records_at, state.records.data(), state.records.size() * sizeof(GpuParticleEmitter));
    write(steps_at, state.step_records.data(), state.step_records.size() * sizeof(GpuParticleEmitterStep));
    write(words_at, state.words.data(), state.words.size() * sizeof(uint32_t));
    write(draw_at, state.draw_blocks.data(), state.draw_blocks.size() * sizeof(uint32_t));
    for (uint32_t j = 0; j < step_count; ++j) {
        write(lists_at[j], state.step_blocks[j].data(), state.step_blocks[j].size() * sizeof(uint32_t));
    }
    state.device->flush_mapped(buffer, 0, end);
    const uint64_t address = state.device->buffer_address(buffer);

    // The pool stays between frames, so the graph is told how the last one left it.
    const GraphBuffer pool =
        graph.import_buffer({.buffer = state.pool,
                             .name = "particle pool",
                             .state = state.pool_used ? rhi::ResourceState::ShaderRead : rhi::ResourceState::Undefined,
                             .final_state = rhi::ResourceState::ShaderRead});
    state.pool_used = true;
    struct PoolPass {
        ParticleSystemState* state = nullptr;
    };
    // Runs just taken, or fallen too far behind to be worth stepping: no particles in them.
    if (!state.cleared.empty()) {
        graph.add_pass<PoolPass>(
            "particles clear",
            [&](PassBuilder& pass, PoolPass& data) {
                data.state = &state;
                pass.use(pool, rhi::ResourceState::CopyDst);
                pass.never_cull(); // what it leaves is for later frames too
            },
            [pool](const PoolPass& data, PassContext& context) {
                for (const ClearedRun& run : data.state->cleared) {
                    context.list.fill_buffer(context.buffer(pool), run.first_block * kBlockBytes,
                                             run.block_count * kBlockBytes, kNoParticleWord);
                }
            });
    }
    for (uint32_t j = 0; j < step_count; ++j) {
        const ParticleStepPush push{.frame = input.frame_address,
                                    .particles = state.pool_address,
                                    .emitters = address + records_at,
                                    .steps = address + steps_at,
                                    .words = address + words_at,
                                    .blocks = address + lists_at[j],
                                    .block_count = static_cast<uint32_t>(state.step_blocks[j].size()),
                                    .step_s = static_cast<float>(asset::kEffectStepS)};
        graph.add_pass(
            "particles step",
            [&](PassBuilder& pass) {
                pass.use(pool, rhi::ResourceState::ShaderWrite, rhi::ShaderStages::Compute);
                pass.never_cull();
            },
            [push, step_pipeline](PassContext& context) {
                DE_PROFILE_ZONE("particles step pass");
                context.list.bind_pipeline(step_pipeline);
                context.list.push_constants(push);
                context.list.dispatch(push.block_count);
            });
    }
    if (state.draw_blocks.empty() || !input.color.is_valid()) {
        return input.coverage; // nothing in view, or nowhere to draw: stepped all the same
    }

    const float* camera = frame.camera.transform;
    const Vec3 right = glm::normalize(Vec3(camera[0], camera[4], camera[8]));
    const Vec3 up = glm::normalize(Vec3(camera[1], camera[5], camera[9]));
    struct DrawPass {
        GraphTexture coverage;
        bool coverage_made = false;
    };
    const ParticleDrawPush draw_push{.frame = input.frame_address,
                                     .particles = state.pool_address,
                                     .emitters = address + records_at,
                                     .words = address + words_at,
                                     .blocks = address + draw_at,
                                     .camera_right = {right.x, right.y, right.z},
                                     .camera_up = {up.x, up.y, up.z},
                                     .near_m = frame.camera.near_m,
                                     .grid_sampler = rhi::sampler_index(rhi::DefaultSampler::LinearClamp),
                                     .fog = input.fog ? 1U : 0U,
                                     .froxels = input.froxels ? 1U : 0U};
    const auto block_count = static_cast<uint32_t>(state.draw_blocks.size());
    return graph
        .add_pass<DrawPass>(
            "particles",
            [&](PassBuilder& pass, DrawPass& data) {
                data.coverage = input.coverage;
                if (!data.coverage.is_valid()) {
                    data.coverage_made = true;
                    data.coverage = pass.create_texture(
                        {.desc = {.format = state.coverage_format, .debug_name = "transparent coverage"},
                         .reference_scale = 1.0f});
                }
                pass.use(input.color, rhi::ResourceState::RenderTarget);
                pass.use(data.coverage, rhi::ResourceState::RenderTarget);
                pass.use(input.depth, rhi::ResourceState::DepthRead | rhi::ResourceState::ShaderRead,
                         rhi::ShaderStages::Fragment);
                pass.use(input.atlas, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Vertex);
                if (input.froxels) {
                    pass.use(input.integrated, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Vertex);
                }
                pass.use(pool, rhi::ResourceState::ShaderRead, rhi::ShaderStages::Vertex);
            },
            [draw_push, draw_pipeline, block_count, color = input.color, depth = input.depth, atlas = input.atlas,
             integrated = input.integrated, froxels = input.froxels](const DrawPass& data, PassContext& context) {
                DE_PROFILE_ZONE("particles pass");
                const rhi::ColorAttachment colors[2] = {
                    {.texture = context.texture(color), .load = rhi::LoadOp::Load},
                    // Nothing see-through drew before: nothing covered so far.
                    {.texture = context.texture(data.coverage),
                     .load = data.coverage_made ? rhi::LoadOp::Clear : rhi::LoadOp::Load}};
                const rhi::DepthAttachment depth_attachment{
                    .texture = context.texture(depth), .load = rhi::LoadOp::Load, .read_only = true};
                context.list.begin_rendering({.colors = colors, .depth = &depth_attachment});
                context.list.bind_pipeline(draw_pipeline);
                ParticleDrawPush push = draw_push;
                push.depth = context.device.texture_index(context.texture(depth));
                push.atlas = context.device.texture_index(context.texture(atlas));
                push.integrated = froxels ? context.device.texture_index(context.texture(integrated)) : 0;
                context.list.push_constants(push);
                // A quad to a particle, a block's 64 to a listed block.
                context.list.draw(6, block_count * kParticleBlockSize);
                context.list.end_rendering();
            })
        .coverage;
}

} // namespace de::render
