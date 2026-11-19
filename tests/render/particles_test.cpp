#include "asset/effect_format.h"
#include "asset/registry.h"
#include "cooker/effect_cooker.h"
#include "core/job_system.h"
#include "platform/vfs.h"
#include "render/graph/render_graph.h"
#include "render/rhi/command_list.h"
#include "render/rhi/device.h"
#include "render/scene/particles.h"
#include "render/shader/shader_compiler.h"
#include "render/shader/shader_library.h"
#include "support/gpu.h"
#include "support/readback.h"
#include "support/render_transform.h"
#include "support/sandbox.h"
#include "world/render_extract.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

using de::asset::AssetId;
using de::asset::AssetType;
using de::render::GpuParticle;
using de::render::ParticleBlocks;
using de::render::ParticleSystem;
using de::render::ParticleView;

namespace {

constexpr double kStepS = de::asset::kEffectStepS;

// One particle a step, straight up at 1 m/s, each living half a second: where and how old each is follows by hand.
constexpr std::string_view kColumn = R"({
  "draw_distance_m": 20,
  "emitters": [
    {"name": "column", "rate_per_s": 60, "lifetime_s": 0.5, "prewarm_s": 0,
     "modules": [{"type": "velocity", "mps": [0, 1, 0]}]}
  ]
})";

// Every random choice and every force at once, three particles every two steps.
constexpr std::string_view kSwirl = R"({
  "emitters": [
    {"name": "swirl", "rate_per_s": 90, "lifetime_s": [0.4, 0.9], "prewarm_s": 0,
     "modules": [
       {"type": "sphere", "radius_m": 0.1},
       {"type": "cone_velocity", "direction": [0, 1, 0], "angle_deg": 25, "speed_mps": [0.5, 1.5]},
       {"type": "acceleration", "mps2": [0, -2, 0]},
       {"type": "drag", "per_s": 0.8},
       {"type": "turbulence", "strength_mps2": 3, "size_m": 0.4, "speed_per_s": 1.5}
     ]}
  ]
})";

// A camera at a place on +Z, looking down -Z at the origin, or the other way.
de::world::RenderView camera_at(float z, bool looking_away = false) {
    const float turn = looking_away ? -1.0f : 1.0f;
    return {.transform = {turn, 0, 0, 0, 0, 1, 0, 0, 0, 0, turn, z}, .vertical_fov_rad = 1.0f, .valid = true};
}

// The time of a test's frame: the first at 0, the second an eighth of a step later, the rest dt apart. So the
// particles' clock is never on the edge of a step, where a rounding would decide which side it falls.
double frame_time(uint32_t frame, double dt) {
    return frame == 0 ? 0.0 : (kStepS / 8.0) + ((frame - 1) * dt);
}

// What a particle test needs up: a device, the engine's shaders, a registry over a sandbox's cooked folder.
struct Kit {
    de::rhi::Device* device = nullptr;
    de::platform::Vfs* vfs = nullptr;
    de::shader::ShaderLibrary* shaders = nullptr;
    de::asset::AssetRegistry* assets = nullptr;

    // Cooks an effect from its text into the sandbox, and loads it.
    de::asset::AssetHandle effect(std::string_view text, uint64_t id_value) const {
        de::cooker::Source source;
        REQUIRE(source.path.assign("content/test.vfx"));
        de::cooker::Diagnostics diagnostics;
        const auto build = de::cooker::import_effect(source, std::as_bytes(std::span(text)), diagnostics);
        REQUIRE(build.ok());
        const AssetId id = AssetId::from_value(id_value);
        const de::core::Array<std::byte> cooked = de::cooker::cook_effect(*build, id);
        REQUIRE(vfs->write_file(de::asset::cooked_path(id, AssetType::Effect).c_str(), cooked.span()).ok());
        const de::asset::AssetHandle handle = assets->load<de::asset::EffectAsset>(id);
        assets->wait_all();
        REQUIRE(assets->state(handle) == de::asset::AssetState::Ready);
        return handle;
    }

    // One frame of a particle system: the effect at the origin, seen from the camera, at a time, as old as its clock
    // says then (as old as the time, unless told otherwise). Stepped, not drawn.
    void frame(ParticleSystem& particles, de::render::RenderGraph& graph, de::asset::AssetHandle effect,
               const de::world::RenderView& camera, double time_s, uint32_t slots = 1,
               const de::world::EffectClock& clock = {}) const {
        de::world::RenderFrame frame;
        frame.time_s = time_s;
        frame.camera = camera;
        for (uint32_t slot = 0; slot < slots; ++slot) {
            frame.effects.push({.transform = de::test::render_transform({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}),
                                .effect = effect,
                                .slot = slot,
                                .seed = 7 + slot,
                                .clock = clock});
        }
        device->begin_frame();
        graph.begin(64, 64);
        particles.add_passes(graph, {.frame = &frame});
        graph.execute();
        device->end_frame();
    }

    // An emitter's particles, read back from the pool.
    std::vector<GpuParticle> read(const ParticleSystem& particles, uint32_t slot = 0, uint32_t emitter = 0) const {
        uint32_t first = 0;
        uint32_t capacity = 0;
        REQUIRE(particles.emitter_run(slot, emitter, first, capacity));
        const uint64_t offset = uint64_t{first} * sizeof(GpuParticle);
        const uint64_t size = uint64_t{capacity} * sizeof(GpuParticle);
        const auto readback = device->create_buffer(
            {.size = size, .usage = de::rhi::BufferUsage::CopyDst, .memory = de::rhi::MemoryLocation::Readback});
        REQUIRE(readback.ok());
        device->begin_frame();
        de::rhi::CommandList list = device->begin_command_list();
        // As the particles' passes left it, and back.
        list.barrier(particles.pool(), de::rhi::ResourceState::ShaderRead, de::rhi::ResourceState::CopySrc);
        list.copy_buffer(particles.pool(), offset, *readback, 0, size);
        list.barrier(particles.pool(), de::rhi::ResourceState::CopySrc, de::rhi::ResourceState::ShaderRead);
        list.barrier(de::rhi::BufferBarrier{
            .buffer = *readback, .before = de::rhi::ResourceState::CopyDst, .after = de::rhi::ResourceState::HostRead});
        device->end_frame();
        device->wait_idle();
        device->invalidate_mapped(*readback);
        std::vector<GpuParticle> out(capacity);
        std::memcpy(out.data(), device->mapped(*readback).data(), static_cast<size_t>(size));
        device->destroy(*readback);
        return out;
    }
};

// Runs body with a kit, or not at all on a machine with no GPU.
template <typename Body>
void with_kit(Body&& body) {
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    {
        const de::test::Sandbox sandbox;
        de::core::JobSystem jobs({.worker_count = 2});
        const de::platform::PathString game_root = sandbox.game();
        auto vfs = de::platform::Vfs::create(
            {.game_root = game_root.c_str(), .engine_root = DE_SOURCE_DIR, .jobs = &jobs, .game_writable = true});
        REQUIRE(vfs.ok());
        auto compiler = de::shader::ShaderCompiler::create(*vfs);
        REQUIRE(compiler.ok());
        auto shaders = de::shader::ShaderLibrary::create(*device, *vfs, jobs, *compiler, {.hot_reload = false});
        REQUIRE(shaders.ok());
        auto assets = de::asset::AssetRegistry::create({.vfs = &*vfs, .jobs = &jobs, .hot_reload = false});
        REQUIRE(assets.ok());
        body(Kit{.device = &*device, .vfs = &*vfs, .shaders = &*shaders, .assets = &*assets});
        device->wait_idle();
    }
    device->wait_idle();
    de::test::check_clean(*device);
}

uint32_t alive(const std::vector<GpuParticle>& particles) {
    uint32_t count = 0;
    for (const GpuParticle& particle : particles) {
        count += particle.age >= 0.0f ? 1U : 0U;
    }
    return count;
}

// The same particles alive in the same slots, each the same to the bit. What's in a slot with no particle isn't
// compared: a run that was cleared and one whose particles died there differ only in that.
bool same_particles(const std::vector<GpuParticle>& a, const std::vector<GpuParticle>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const bool alive_a = a[i].age >= 0.0f;
        if (alive_a != (b[i].age >= 0.0f) ||
            (alive_a && !std::ranges::equal(std::as_bytes(std::span(&a[i], 1)), std::as_bytes(std::span(&b[i], 1))))) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("particle blocks are taken lowest first, and runs given back join their neighbours") {
    ParticleBlocks blocks(16);
    CHECK(blocks.free_blocks() == 16);
    CHECK(blocks.free_runs() == 1);
    CHECK(blocks.take(4) == 0);
    CHECK(blocks.take(4) == 4);
    CHECK(blocks.take(8) == 8);
    CHECK(blocks.take(1) == ParticleBlocks::kNone);
    CHECK(blocks.free_blocks() == 0);
    CHECK(blocks.free_runs() == 0);

    // A hole of 4: room for 4, not for 8 though 8 would be free in all once another 4 goes.
    blocks.give(4, 4);
    CHECK(blocks.take(8) == ParticleBlocks::kNone);
    blocks.give(12, 4); // [8, 12) is still held: two runs, not one
    CHECK(blocks.free_blocks() == 8);
    CHECK(blocks.free_runs() == 2);
    CHECK(blocks.take(8) == ParticleBlocks::kNone);
    CHECK(blocks.take(4) == 4); // the lowest that fits
    blocks.give(4, 4);
    // The run between them joins both; the first one, the front.
    blocks.give(8, 4);
    CHECK(blocks.free_runs() == 1);
    CHECK(blocks.take(12) == 4);
    blocks.give(4, 12);
    blocks.give(0, 4);
    CHECK(blocks.free_blocks() == 16);
    CHECK(blocks.free_runs() == 1);
    // A small run comes out of the first hole it fits, a large one from past it.
    CHECK(blocks.take(2) == 0);
    CHECK(blocks.take(2) == 2);
    blocks.give(0, 2);
    CHECK(blocks.take(4) == 4);
    CHECK(blocks.take(1) == 0);
    CHECK(blocks.take(1) == 1);
    CHECK(blocks.free_blocks() == 8);
}

TEST_CASE("an emitter's births are whole numbers of its own clock, the same for the same steps") {
    using de::render::particle_steps;
    using de::render::particles_born;
    CHECK(particles_born(60.0f, 0) == 0);
    for (uint64_t steps = 1; steps <= 600; ++steps) {
        CHECK(particles_born(60.0f, steps) == steps);           // one a step, with none lost to rounding
        CHECK(particles_born(30.0f, steps) == steps / 2);       // one every other step
        CHECK(particles_born(90.0f, steps) == (steps * 3) / 2); // one, then two
        CHECK(particles_born(90.0f, steps) >= particles_born(90.0f, steps - 1));
    }
    CHECK(particles_born(0.5f, 119) == 0); // one every two seconds: none before
    CHECK(particles_born(0.5f, 120) == 1);
    CHECK(particles_born(100000.0f, 60) == 100000);
    CHECK(particles_born(100000.0f, 1) == 1666);

    CHECK(particle_steps(0.0) == 0);
    CHECK(particle_steps(-1.0) == 0);
    CHECK(particle_steps(kStepS) == 1);
    CHECK(particle_steps(0.5) == 30);
    CHECK(particle_steps(0.51) == 31); // the fewest that reach it
    CHECK(particle_steps(1.0) == 60);
}

TEST_CASE("the particles' view holds what's in front of the camera and within its sides, by its sphere") {
    // At the origin, looking down -Z, a right angle wide and as tall.
    const float camera[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const float right_angle = 1.5707964f;
    const ParticleView view = ParticleView::from_camera(camera, right_angle, 0.1f, 1.0f);
    CHECK(view.sees({0.0f, 0.0f, -10.0f}, 0.1f));
    CHECK_FALSE(view.sees({0.0f, 0.0f, 10.0f}, 1.0f)); // behind
    CHECK(view.sees({0.0f, 0.0f, 0.5f}, 1.0f));        // behind, but reaching past the near plane
    CHECK_FALSE(view.sees({0.0f, 0.0f, 0.5f}, 0.3f));
    // 10 m ahead the view is 10 m to each side: past that only by as much as its radius, measured square to the side.
    CHECK(view.sees({10.5f, 0.0f, -10.0f}, 1.0f));
    CHECK_FALSE(view.sees({20.0f, 0.0f, -10.0f}, 1.0f));
    CHECK(view.sees({-10.5f, 0.0f, -10.0f}, 1.0f));
    CHECK_FALSE(view.sees({-20.0f, 0.0f, -10.0f}, 1.0f));
    CHECK(view.sees({0.0f, 10.5f, -10.0f}, 1.0f));
    CHECK_FALSE(view.sees({0.0f, 20.0f, -10.0f}, 1.0f));
    CHECK_FALSE(view.sees({0.0f, -20.0f, -10.0f}, 1.0f));
    // Twice as wide as tall: what was out to the side is in, what's above isn't.
    const ParticleView wide = ParticleView::from_camera(camera, right_angle, 0.1f, 2.0f);
    CHECK(wide.sees({15.0f, 0.0f, -10.0f}, 0.1f));
    CHECK_FALSE(wide.sees({0.0f, 15.0f, -10.0f}, 0.1f));
    // Moved and turned about: it looks where its camera looks.
    const float turned[12] = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 5}; // at z 5, looking down +Z
    const ParticleView back = ParticleView::from_camera(turned, right_angle, 0.1f, 1.0f);
    CHECK(back.sees({0.0f, 0.0f, 15.0f}, 0.1f));
    CHECK_FALSE(back.sees({0.0f, 0.0f, 0.0f}, 0.1f));
}

TEST_CASE("particles are born at their emitter's rate, each in its slot of the ring, and go when their life is over") {
    with_kit([](const Kit& kit) {
        const de::asset::AssetHandle effect = kit.effect(kColumn, 0x700);
        auto particles = ParticleSystem::create(
            {.device = kit.device, .shaders = kit.shaders, .assets = kit.assets, .pool_blocks = 8});
        REQUIRE(particles.ok());
        auto graph = de::render::RenderGraph::create(*kit.device);
        REQUIRE(graph.ok());
        const de::world::RenderView camera = camera_at(5.0f);
        // Frame 0 starts the clock and frame 1 is an eighth of a step on: each frame after that is a step.
        const auto run_to = [&](uint32_t from, uint32_t to) {
            for (uint32_t frame = from; frame <= to; ++frame) {
                kit.frame(*particles, *graph, effect, camera, frame_time(frame, kStepS));
            }
        };
        run_to(0, 11);
        CHECK(particles->stats().emitters == 1);
        CHECK(particles->stats().blocks == 1);
        CHECK(particles->stats().steps == 1);
        std::vector<GpuParticle> pool = kit.read(*particles);
        // What's born while one lives, a step more, and one: 60 * (0.5 + 1/60) + 1.
        REQUIRE(pool.size() == 32);
        CHECK(alive(pool) == 10);
        const auto step_s = static_cast<float>(kStepS);
        for (uint32_t serial = 0; serial < 10; ++serial) {
            // Born half-way through its step (the only one born in it), then a step older for each step since. It
            // rose at 1 m/s all the while, so it's as high as it's old.
            const float age = (0.5f * step_s) + (static_cast<float>(9 - serial) * step_s);
            CHECK(pool[serial].age == doctest::Approx(age).epsilon(1e-4));
            CHECK(pool[serial].position[1] == doctest::Approx(age).epsilon(1e-4));
            CHECK(pool[serial].position[0] == 0.0f);
            CHECK(pool[serial].velocity[1] == 1.0f);
        }
        CHECK(pool[10].age < 0.0f);
        CHECK(pool[3].seed != pool[4].seed); // each its own

        // 40 steps: one lives 30, so the first 10 are gone, and the ring has come round over the first 8 slots.
        run_to(12, 41);
        pool = kit.read(*particles);
        CHECK(alive(pool) == 30);
        for (uint32_t serial = 10; serial < 40; ++serial) {
            const float age = (0.5f * step_s) + (static_cast<float>(39 - serial) * step_s);
            CHECK(pool[serial % 32].age == doctest::Approx(age).epsilon(1e-4));
        }
        CHECK(pool[8].age < 0.0f); // the 9th and 10th died where they were; the ring hasn't reached them yet
        CHECK(pool[9].age < 0.0f);
        // About what its rate and lifetime come to.
        CHECK(particles->stats().particles == 30);
        kit.assets->release(effect);
        kit.assets->update();
    });
}

TEST_CASE("the same steps leave the same particles, however many frames they fell in") {
    with_kit([](const Kit& kit) {
        const de::asset::AssetHandle effect = kit.effect(kSwirl, 0x701);
        auto graph = de::render::RenderGraph::create(*kit.device);
        REQUIRE(graph.ok());
        const de::world::RenderView camera = camera_at(8.0f);
        // Two seconds, in frames a step long, a quarter of one, and two.
        const auto run = [&](double dt, uint32_t frames) {
            auto particles = ParticleSystem::create(
                {.device = kit.device, .shaders = kit.shaders, .assets = kit.assets, .pool_blocks = 8});
            REQUIRE(particles.ok());
            uint32_t steps = 0;
            // The clock starts with the first frame and is an eighth of a step along at the second: `frames` more.
            for (uint32_t frame = 0; frame < frames + 2; ++frame) {
                kit.frame(*particles, *graph, effect, camera, frame_time(frame, dt));
                steps += particles->stats().steps;
            }
            CHECK(steps == 120);
            std::vector<GpuParticle> pool = kit.read(*particles);
            kit.device->wait_idle();
            return pool;
        };
        const std::vector<GpuParticle> at_60 = run(kStepS, 120);
        const std::vector<GpuParticle> at_240 = run(kStepS / 4.0, 480);
        const std::vector<GpuParticle> at_30 = run(kStepS * 2.0, 60);
        // Some of each lifetime: neither none nor all the ring.
        CHECK(alive(at_60) > 30);
        CHECK(alive(at_60) < at_60.size());
        CHECK(same_particles(at_60, at_240));
        CHECK(same_particles(at_60, at_30));
        // And they went somewhere: up from the emitter, apart from each other.
        float highest = 0.0f;
        float widest = 0.0f;
        for (const GpuParticle& particle : at_60) {
            if (particle.age >= 0.0f) {
                highest = std::max(highest, particle.position[1]);
                widest = std::max(widest, std::abs(particle.position[0]));
            }
        }
        CHECK(highest > 0.2f);
        CHECK(widest > 0.05f);
        kit.assets->release(effect);
        kit.assets->update();
    });
}

TEST_CASE("an emitter out of view isn't stepped, and has the particles it would have had when it's back in it") {
    with_kit([](const Kit& kit) {
        const de::asset::AssetHandle effect = kit.effect(kSwirl, 0x702);
        auto graph = de::render::RenderGraph::create(*kit.device);
        REQUIRE(graph.ok());
        // Looked at all along, and looked away from until the last frame: a little while (it steps the ones it
        // missed), and longer than any particle lives (it starts a lifetime back instead).
        for (const uint32_t frames : {20U, 150U}) {
            auto watched = ParticleSystem::create(
                {.device = kit.device, .shaders = kit.shaders, .assets = kit.assets, .pool_blocks = 8});
            auto unwatched = ParticleSystem::create(
                {.device = kit.device, .shaders = kit.shaders, .assets = kit.assets, .pool_blocks = 8});
            REQUIRE(watched.ok());
            REQUIRE(unwatched.ok());
            for (uint32_t frame = 0; frame < frames; ++frame) {
                kit.frame(*watched, *graph, effect, camera_at(8.0f), frame_time(frame, kStepS));
                kit.frame(*unwatched, *graph, effect, camera_at(8.0f, true), frame_time(frame, kStepS));
                CHECK(unwatched->stats().steps == 0);
                CHECK(unwatched->stats().emitters == 1); // it keeps its blocks: it's near, only not in view
                CHECK(unwatched->stats().emitters_drawn == 0);
            }
            kit.frame(*watched, *graph, effect, camera_at(8.0f), frame_time(frames, kStepS));
            kit.frame(*unwatched, *graph, effect, camera_at(8.0f), frame_time(frames, kStepS));
            // All it was behind in the one frame: the 19 steps there have been, or the 55 that a particle's longest
            // life (0.9 s, 54 steps) and one come to.
            CHECK(unwatched->stats().steps == (frames == 20 ? 19 : 55));
            CHECK(unwatched->stats().emitters_drawn == 1);
            const std::vector<GpuParticle> seen = kit.read(*watched);
            const std::vector<GpuParticle> caught_up = kit.read(*unwatched);
            CHECK(alive(seen) > 20);
            CHECK(same_particles(seen, caught_up));
            kit.device->wait_idle();
        }
        kit.assets->release(effect);
        kit.assets->update();
    });
}

TEST_CASE("an effect's particles are what its clock's age gives, however it got there") {
    with_kit([](const Kit& kit) {
        const de::asset::AssetHandle effect = kit.effect(kSwirl, 0x704);
        auto graph = de::render::RenderGraph::create(*kit.device);
        REQUIRE(graph.ok());
        const de::world::RenderView camera = camera_at(8.0f);
        const auto make = [&] {
            auto particles = ParticleSystem::create(
                {.device = kit.device, .shaders = kit.shaders, .assets = kit.assets, .pool_blocks = 8});
            REQUIRE(particles.ok());
            return particles;
        };
        // Played for two seconds, a step a frame: what the others must come to.
        auto played = make();
        for (uint32_t frame = 0; frame <= 121; ++frame) {
            kit.frame(*played, *graph, effect, camera, frame_time(frame, kStepS));
        }
        const std::vector<GpuParticle> expected = kit.read(*played);
        CHECK(alive(expected) > 30);
        const double age_s = frame_time(121, kStepS);

        {
            // Set to that age at once, at some other time of the world: a lifetime's steps and one in the one frame,
            // drawn from then on.
            auto set = make();
            de::world::EffectClock clock;
            clock.seek(40.0, age_s);
            kit.frame(*set, *graph, effect, camera, 40.0, 1, clock);
            CHECK(set->stats().steps == 55);
            CHECK(set->stats().emitters_drawn == 1);
            CHECK(same_particles(expected, kit.read(*set)));
            // Held still there: frames go by and nothing steps.
            clock.set_rate(40.0, 0.0f);
            for (uint32_t frame = 1; frame <= 5; ++frame) {
                kit.frame(*set, *graph, effect, camera, 40.0 + (frame * kStepS), 1, clock);
                CHECK(set->stats().steps == 0);
                CHECK(set->stats().emitters_drawn == 1);
            }
            CHECK(same_particles(expected, kit.read(*set)));
            kit.device->wait_idle();
        }
        {
            // Played a second longer, then set back: from nothing, to the same particles.
            for (uint32_t frame = 122; frame <= 181; ++frame) {
                kit.frame(*played, *graph, effect, camera, frame_time(frame, kStepS));
            }
            CHECK_FALSE(same_particles(expected, kit.read(*played)));
            de::world::EffectClock clock;
            const double now_s = frame_time(182, kStepS);
            clock.seek(now_s, age_s);
            kit.frame(*played, *graph, effect, camera, now_s, 1, clock);
            CHECK(played->stats().steps == 55);
            CHECK(same_particles(expected, kit.read(*played)));
            kit.device->wait_idle();
        }
        {
            // At half the world's rate: twice the frames for the same steps, and the same particles.
            auto slow = make();
            de::world::EffectClock clock;
            clock.rate = 0.5f;
            uint32_t steps = 0;
            for (uint32_t frame = 0; frame <= 241; ++frame) {
                kit.frame(*slow, *graph, effect, camera, frame_time(frame, kStepS), 1, clock);
                steps += slow->stats().steps;
            }
            // The world's time is at 240 steps and an eighth; the effect's at half that.
            CHECK(steps == 120);
            CHECK(same_particles(expected, kit.read(*slow)));
            kit.device->wait_idle();
        }
        kit.assets->release(effect);
        kit.assets->update();
    });
}

TEST_CASE("an emitter far behind its clock catches up as many steps a frame as the system allows, undrawn till then") {
    with_kit([](const Kit& kit) {
        // One particle every other step, each living four seconds: 241 steps to have them all.
        constexpr std::string_view kSlow = R"({
  "emitters": [
    {"name": "slow", "rate_per_s": 30, "lifetime_s": 4, "prewarm_s": 0,
     "modules": [{"type": "velocity", "mps": [0, 0.1, 0]}]}
  ]
})";
        const de::asset::AssetHandle effect = kit.effect(kSlow, 0x705);
        auto graph = de::render::RenderGraph::create(*kit.device);
        REQUIRE(graph.ok());
        const de::world::RenderView camera = camera_at(8.0f);
        de::world::EffectClock clock;
        clock.seek(0.0, 20.0);
        clock.rate = 0.0f;
        {
            // As the game has it: 64 a frame, so four frames, and drawn only once it's there.
            auto particles = ParticleSystem::create(
                {.device = kit.device, .shaders = kit.shaders, .assets = kit.assets, .pool_blocks = 8});
            REQUIRE(particles.ok());
            const uint32_t expected[] = {64, 64, 64, 49};
            for (uint32_t frame = 0; frame < 4; ++frame) {
                kit.frame(*particles, *graph, effect, camera, frame * kStepS, 1, clock);
                CHECK(particles->stats().steps == expected[frame]);
                CHECK(particles->stats().emitters_drawn == (frame == 3 ? 1U : 0U));
            }
            de::render::ParticleEmitterStats stats;
            REQUIRE(particles->emitter_stats(0, 0, stats));
            CHECK(stats.held);
            CHECK(stats.steps_done == 1200);
            CHECK(stats.born == 600);
            CHECK(stats.alive == 120); // four seconds' worth
            CHECK(stats.capacity >= 120);
            CHECK(alive(kit.read(*particles)) == 120);
            CHECK_FALSE(particles->emitter_stats(0, 1, stats)); // no such emitter
            CHECK_FALSE(particles->emitter_stats(5, 0, stats)); // no such effect
            kit.device->wait_idle();
        }
        {
            // As a viewer has it: all of them in the one frame, and the same particles.
            auto generous = ParticleSystem::create({.device = kit.device,
                                                    .shaders = kit.shaders,
                                                    .assets = kit.assets,
                                                    .pool_blocks = 8,
                                                    .catch_up_steps = 512});
            REQUIRE(generous.ok());
            kit.frame(*generous, *graph, effect, camera, 0.0, 1, clock);
            CHECK(generous->stats().steps == 241);
            CHECK(generous->stats().emitters_drawn == 1);
            CHECK(alive(kit.read(*generous)) == 120);
            // Left out of the picture, it isn't drawn; its clock moved on, it's simulated all the same.
            de::world::EffectClock later = clock;
            later.seek(0.0, 20.0 + (3.0 * kStepS));
            de::world::RenderFrame frame;
            frame.time_s = kStepS;
            frame.camera = camera;
            frame.effects.push({.transform = de::test::render_transform({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}),
                                .effect = effect,
                                .slot = 0,
                                .seed = 7,
                                .clock = later,
                                .hidden_emitters = 1});
            kit.device->begin_frame();
            graph->begin(64, 64);
            generous->add_passes(*graph, {.frame = &frame});
            graph->execute();
            kit.device->end_frame();
            CHECK(generous->stats().steps == 3);
            CHECK(generous->stats().emitters == 1);
            CHECK(generous->stats().emitters_drawn == 0);
            kit.device->wait_idle();
        }
        // A system has to be allowed some steps, and not more than it has lists for.
        CHECK_FALSE(ParticleSystem::create({.device = kit.device,
                                            .shaders = kit.shaders,
                                            .assets = kit.assets,
                                            .pool_blocks = 8,
                                            .catch_up_steps = 0})
                        .ok());
        CHECK_FALSE(ParticleSystem::create({.device = kit.device,
                                            .shaders = kit.shaders,
                                            .assets = kit.assets,
                                            .pool_blocks = 8,
                                            .catch_up_steps = de::render::kMostParticleCatchUpSteps + 1})
                        .ok());
        kit.assets->release(effect);
        kit.assets->update();
    });
}

TEST_CASE("an effect past its draw distance gives its blocks back, and a full pool starts nothing more") {
    with_kit([](const Kit& kit) {
        const de::asset::AssetHandle effect = kit.effect(kColumn, 0x703);
        auto graph = de::render::RenderGraph::create(*kit.device);
        REQUIRE(graph.ok());
        {
            auto particles = ParticleSystem::create(
                {.device = kit.device, .shaders = kit.shaders, .assets = kit.assets, .pool_blocks = 8});
            REQUIRE(particles.ok());
            uint32_t frame = 0;
            for (; frame < 5; ++frame) {
                kit.frame(*particles, *graph, effect, camera_at(5.0f), frame_time(frame, kStepS));
            }
            CHECK(particles->stats().blocks == 1);
            uint32_t first = 0;
            uint32_t capacity = 0;
            CHECK(particles->emitter_run(0, 0, first, capacity));
            // 20 m is as far as it shows: from 50 it holds nothing.
            kit.frame(*particles, *graph, effect, camera_at(50.0f), frame_time(frame++, kStepS));
            CHECK(particles->stats().effects == 1);
            CHECK(particles->stats().emitters == 0);
            CHECK(particles->stats().blocks == 0);
            CHECK_FALSE(particles->emitter_run(0, 0, first, capacity));
            // Back near, it has its blocks again, and the particles it would have had: as old as it is, it takes the
            // steps it has missed (all 5 so far) at once.
            kit.frame(*particles, *graph, effect, camera_at(5.0f), frame_time(frame++, kStepS));
            CHECK(particles->stats().blocks == 1);
            CHECK(particles->stats().steps == 5);
            CHECK(alive(kit.read(*particles)) == 5);
            kit.frame(*particles, *graph, effect, camera_at(5.0f), frame_time(frame++, kStepS));
            CHECK(particles->stats().steps == 1);
            CHECK(alive(kit.read(*particles)) == 6);
            // And an effect that's no longer in the frame is gone, with its blocks.
            de::world::RenderFrame empty;
            empty.time_s = frame_time(frame++, kStepS);
            empty.camera = camera_at(5.0f);
            kit.device->begin_frame();
            graph->begin(64, 64);
            particles->add_passes(*graph, {.frame = &empty});
            graph->execute();
            kit.device->end_frame();
            CHECK(particles->stats().effects == 0);
            CHECK(particles->stats().blocks == 0);
            kit.device->wait_idle();
        }
        {
            // One block in all: the first effect gets it, the second waits, and has it once the first is gone.
            auto particles = ParticleSystem::create(
                {.device = kit.device, .shaders = kit.shaders, .assets = kit.assets, .pool_blocks = 1});
            REQUIRE(particles.ok());
            kit.frame(*particles, *graph, effect, camera_at(5.0f), frame_time(0, kStepS), 2);
            CHECK(particles->stats().effects == 2);
            CHECK(particles->stats().emitters == 1);
            CHECK(particles->stats().emitters_starved == 1);
            uint32_t first = 0;
            uint32_t capacity = 0;
            CHECK(particles->emitter_run(0, 0, first, capacity));
            CHECK_FALSE(particles->emitter_run(1, 0, first, capacity));
            // Only the second, at slot 1.
            de::world::RenderFrame frame;
            frame.time_s = frame_time(1, kStepS);
            frame.camera = camera_at(5.0f);
            frame.effects.push({.transform = de::test::render_transform({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}),
                                .effect = effect,
                                .slot = 1,
                                .seed = 8});
            kit.device->begin_frame();
            graph->begin(64, 64);
            particles->add_passes(*graph, {.frame = &frame});
            graph->execute();
            kit.device->end_frame();
            CHECK(particles->stats().emitters == 1);
            CHECK(particles->stats().emitters_starved == 0);
            CHECK(particles->emitter_run(1, 0, first, capacity));
            kit.device->wait_idle();
        }
        kit.assets->release(effect);
        kit.assets->update();
    });
}
