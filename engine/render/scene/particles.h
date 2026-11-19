#pragma once

#include "asset/effect_format.h"
#include "asset/registry.h"
#include "core/array.h"
#include "core/math.h"
#include "core/result.h"
#include "render/graph/render_graph.h"
#include "render/rhi/rhi.h"

#include <cstdint>
#include <span>

namespace de::rhi {
class Device;
}
namespace de::shader {
class ShaderLibrary;
}
namespace de::world {
struct RenderFrame;
}

namespace de::render {

// Particles (docs/design.md, VFX foundation): every effect's, in one pool on the GPU. The CPU works per emitter (how
// many are born in a step, what's in view, what's drawn first); the GPU per particle, in two programs: a compute pass
// that steps them (shaders/scene/particles_step.slang) and a draw (shaders/scene/particles.slang).

// The pool: blocks of this many particles. An emitter instance holds a run of whole blocks.
inline constexpr uint32_t kParticleBlockSize = 64;
inline constexpr uint32_t kParticlePoolBlocks = 16384; // 1,048,576 particles, 32 MiB
// An emitter that's behind its effect's clock (new, back in view, or its clock set elsewhere) catches up at most so
// many steps a frame, undrawn until it has: this many, unless whoever makes the system says otherwise
// (ParticleSystemDesc::catch_up_steps; the effect viewer, whose time is dragged about, takes far more). The world's
// own time never runs ahead by more than a few ticks a frame (WorldDesc::max_ticks_per_frame), so what's in view and
// playing owes a few at most.
inline constexpr uint32_t kParticleCatchUpSteps = 64;
inline constexpr uint32_t kMostParticleCatchUpSteps = 4096;

// A particle as the GPU keeps it (Particle in shaders/scene/particles_common.slang). Its lifetime, size and colour
// aren't kept: they follow from its seed and its age.
struct GpuParticle {
    float position[3] = {}; // relative to its emitter's anchor (world space), or in its emitter's axes (local)
    float age = -1.0f;      // seconds; below 0: no particle here
    float velocity[3] = {}; // m/s, in the same axes
    uint32_t seed = 0;
};
static_assert(sizeof(GpuParticle) == 32);

// Runs of the pool's blocks, given and taken back: first fit, lowest first, with runs that touch joined again. Kept
// on the CPU; nothing here touches the GPU.
class ParticleBlocks {
public:
    static constexpr uint32_t kNone = UINT32_MAX;

    explicit ParticleBlocks(uint32_t blocks);

    // The first block of a run of count, or kNone if no run that long is free. count is at least 1.
    uint32_t take(uint32_t count);
    // Gives back a run take() returned, whole.
    void give(uint32_t first, uint32_t count);

    uint32_t free_blocks() const { return free_; }
    uint32_t blocks() const { return blocks_; }
    // How many runs the free blocks are in: 1 when nothing's taken, more as the pool fragments.
    uint32_t free_runs() const { return static_cast<uint32_t>(runs_.size()); }

private:
    struct Run {
        uint32_t first = 0;
        uint32_t count = 0;
    };
    core::Array<Run> runs_{core::heap_allocator(core::MemoryTag::Render)}; // free, by first, never touching
    uint32_t blocks_ = 0;
    uint32_t free_ = 0;
};

// How many particles an emitter has had born by the end of its step `steps` (steps of asset::kEffectStepS since it
// started): whole numbers of its own clock, so the same steps always give the same births.
uint64_t particles_born(float rate_per_s, uint64_t steps);

// How many steps a time is: the fewest that reach it.
uint64_t particle_steps(double seconds);

// Whether a sphere touches the view: inside the camera's near plane and four sides (no far plane).
struct ParticleView {
    Vec3 position{0.0f}; // the camera, relative to the render origin
    Vec3 planes[5];      // inward unit normals: near, left, right, bottom, top
    float near_m = 0.1f;

    static ParticleView from_camera(const float (&camera)[12], float vertical_fov_rad, float near_m, float aspect);
    bool sees(Vec3 center, float radius) const;
};

struct ParticleSystemDesc {
    rhi::Device* device = nullptr;
    shader::ShaderLibrary* shaders = nullptr;
    asset::AssetRegistry* assets = nullptr; // where an effect's handle leads to its asset::EffectAsset
    uint32_t pool_blocks = kParticlePoolBlocks;
    uint32_t catch_up_steps = kParticleCatchUpSteps; // a frame, of an emitter behind its clock; each is a pass
    // What the draw renders into: the lit image, the coverage TAA reads, the scene's depth.
    rhi::Format color_format = rhi::Format::RGBA16Float;
    rhi::Format coverage_format = rhi::Format::R8Unorm;
    rhi::Format depth_format = rhi::Format::D32Float;
};

// What a frame's passes need of the scene they're added to.
struct ParticlePassInput {
    const world::RenderFrame* frame = nullptr;
    uint64_t frame_address = 0; // the scene's SceneFrame on the GPU, written already
    float aspect = 1.0f;
    float wind_mps = 0.0f;   // the most the wind blows: how far it can carry what follows it
    GraphTexture color{};    // the lit, fogged image; invalid: nothing is drawn, only stepped
    GraphTexture coverage{}; // how much of each pixel see-through things cover; invalid: nothing has drawn into it yet
    GraphTexture depth{};
    GraphTexture atlas{};      // the sun's shadow atlas
    GraphTexture integrated{}; // the froxel grid, if froxels
    bool fog = false;          // the image was fogged, so particles are too
    bool froxels = false;
};

struct ParticleStats {
    uint32_t effects = 0;          // in the frame
    uint32_t emitters = 0;         // of those, holding blocks: in reach of the camera
    uint32_t emitters_drawn = 0;   // of those, in view and caught up
    uint32_t emitters_starved = 0; // in reach, but the pool had no run for them
    uint32_t blocks = 0;           // held, of the pool's
    uint32_t pool_blocks = 0;
    uint32_t particles = 0;      // alive, about: what each emitter's rate and lifetimes come to
    uint32_t steps = 0;          // compute passes this frame: 0 on most frames above 60 fps
    uint32_t blocks_stepped = 0; // in all of them
    uint32_t blocks_drawn = 0;
};

// What one emitter of an effect in the frame holds, for a viewer to show.
struct ParticleEmitterStats {
    bool held = false;       // it has its run of the pool: in reach, and there was room
    uint32_t capacity = 0;   // the most alive at once
    uint64_t steps_done = 0; // steps its particles have taken, of those its effect's age comes to
    uint64_t born = 0;       // particles born by then
    uint32_t alive = 0;      // about: those born within a mean lifetime of then, its capacity at most
};

struct ParticleSystemState;

// On the thread that renders. The device, shader library and registry must outlive it. Move-only.
class ParticleSystem {
public:
    // Its pool and programs. Fails if the pool can't be made.
    static core::Result<ParticleSystem> create(const ParticleSystemDesc& desc);

    ~ParticleSystem();
    ParticleSystem(ParticleSystem&& other) noexcept;
    ParticleSystem& operator=(ParticleSystem&& other) noexcept;
    ParticleSystem(const ParticleSystem&) = delete;
    ParticleSystem& operator=(const ParticleSystem&) = delete;

    // Takes in the frame's effects, and adds the passes that step what's due and draw what's in view: "particles
    // clear" (runs just taken), "particles step" (one for each step some emitter takes this frame) and "particles".
    // Returns the coverage it drew into: input.coverage, one made here if that was invalid, or invalid if it drew
    // nothing. Adds nothing while its shaders are missing; an effect whose asset hasn't loaded waits.
    GraphTexture add_passes(RenderGraph& graph, const ParticlePassInput& input);

    ParticleStats stats() const;

    // The pool, for tests to read back: GpuParticle by index, and where an effect's emitter keeps its particles
    // (false if it holds none).
    rhi::BufferHandle pool() const;
    bool emitter_run(uint32_t effect_slot, uint32_t emitter, uint32_t& first_particle, uint32_t& capacity) const;
    // An emitter's numbers as of the last add_passes(); false if the effect isn't in the frame, isn't loaded or
    // has no such emitter.
    bool emitter_stats(uint32_t effect_slot, uint32_t emitter, ParticleEmitterStats& out) const;

private:
    explicit ParticleSystem(ParticleSystemState* state) : state_(state) {}

    ParticleSystemState* state_ = nullptr;
};

} // namespace de::render
