#include "app/preview_scene.h"
#include "asset/effect_format.h"
#include "asset/registry.h"
#include "core/job_system.h"
#include "editor/effect_viewer.h"
#include "editor/preview_pictures.h"
#include "render/rhi/device.h"
#include "render/rhi/upload.h"
#include "render/scene/gpu_assets.h"
#include "render/shader/shader_compiler.h"
#include "render/shader/shader_library.h"
#include "support/cook_kit.h"
#include "support/gpu.h"
#include "support/sandbox.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <string_view>

using de::editor::EffectPlayback;
using de::test::Sandbox;

namespace {

constexpr double kStepS = de::asset::kEffectStepS;

// Two emitters whose numbers follow by hand: one particle a step living half a second, and one every other step
// living four. Neither starts settled (prewarm_s: by default an emitter has already run a lifetime at its start).
constexpr std::string_view kTwo = R"({
  "emitters": [
    {"name": "quick", "rate_per_s": 60, "lifetime_s": 0.5, "prewarm_s": 0,
     "modules": [{"type": "velocity", "mps": [0, 1, 0]}, {"type": "size", "m": 0.05}]},
    {"name": "slow", "rate_per_s": 30, "lifetime_s": 4, "prewarm_s": 0,
     "modules": [{"type": "velocity", "mps": [0.2, 0, 0]}, {"type": "size", "m": 0.05}]}
  ]
})";

} // namespace

TEST_CASE("effect playback: an age the viewer owns, played, held, stepped either way, sought and looped") {
    EffectPlayback playback;
    CHECK(playback.playing);
    CHECK(playback.age_s == 0.0);
    // Played: real time, at its speed.
    playback.advance(0.5);
    CHECK(playback.age_s == doctest::Approx(0.5));
    playback.speed = 0.25f;
    playback.advance(1.0);
    CHECK(playback.age_s == doctest::Approx(0.75));
    // The clock it gives is at that age whatever the world's time is.
    CHECK(playback.clock().age_at(0.0) == doctest::Approx(0.75));
    CHECK(playback.clock().age_at(1234.5) == doctest::Approx(0.75));
    // A step holds it, on the simulation's next step: from between two, on is the next and back the one just passed.
    playback.seek(10.4 * kStepS);
    CHECK(playback.playing);
    playback.step(1);
    CHECK_FALSE(playback.playing);
    CHECK(playback.step_number() == 11);
    CHECK(playback.age_s == doctest::Approx(11.0 * kStepS));
    playback.step(1);
    CHECK(playback.step_number() == 12);
    playback.step(-1);
    CHECK(playback.step_number() == 11);
    playback.seek(10.4 * kStepS);
    playback.step(-1);
    CHECK(playback.age_s == doctest::Approx(10.0 * kStepS));
    playback.step(-3);
    CHECK(playback.step_number() == 7);
    // Never before its start; held, a frame going by changes nothing.
    playback.step(-100);
    CHECK(playback.age_s == 0.0);
    playback.advance(1.0);
    CHECK(playback.age_s == 0.0);
    playback.seek(-3.0);
    CHECK(playback.age_s == 0.0);
    // Looping turns it at its length; not looping, it goes on past it.
    playback.playing = true;
    playback.speed = 1.0f;
    playback.length_s = 2.0;
    playback.seek(1.5);
    playback.advance(1.0);
    CHECK(playback.age_s == doctest::Approx(2.5));
    playback.loop = true;
    playback.advance(0.25);
    CHECK(playback.age_s == doctest::Approx(0.75));
    playback.restart();
    CHECK(playback.age_s == 0.0);
}

TEST_CASE("the effect viewer shows an effect at the age it's told, emitter by emitter") {
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    const Sandbox sandbox;
    de::test::write_src(sandbox, "content/fx/two.vfx", kTwo);
    de::test::write_src(sandbox, "content/fx/raw.vfx", kTwo); // another, to open in its place
    de::test::write_src(sandbox, "content/fx/notes.txt", "not an effect");
    {
        de::platform::Vfs cook_vfs = de::test::cooker_vfs(sandbox);
        REQUIRE(de::test::run_cook(cook_vfs).ok());
    }
    de::core::JobSystem jobs({.worker_count = 2});
    de::platform::Vfs vfs = sandbox.make_vfs({.engine_root = DE_SOURCE_DIR, .jobs = &jobs, .game_writable = true});
    auto compiler = de::shader::ShaderCompiler::create(vfs);
    REQUIRE(compiler.ok());
    auto shaders = de::shader::ShaderLibrary::create(*device, vfs, jobs, *compiler, {.hot_reload = false});
    REQUIRE(shaders.ok());
    auto uploader = de::rhi::Uploader::create(*device);
    REQUIRE(uploader.ok());
    auto assets = de::asset::AssetRegistry::create({.vfs = &vfs, .jobs = &jobs, .hot_reload = false});
    REQUIRE(assets.ok());
    {
        auto gpu_assets =
            de::render::GpuAssets::create({.device = &*device, .uploader = &*uploader, .assets = &*assets});
        REQUIRE(gpu_assets.ok());
        const de::editor::PictureEngine engine{.device = &*device,
                                               .shaders = &*shaders,
                                               .assets = &*assets,
                                               .gpu_assets = &*gpu_assets,
                                               .jobs = &jobs,
                                               .vfs = &vfs};
        de::editor::EffectViewer viewer(de::core::heap_allocator(de::core::MemoryTag::Editor));
        char why[160] = {};
        // What isn't an effect, or has no meta, is refused, saying why; nothing is opened.
        CHECK_FALSE(viewer.open(engine, "src:/content/fx/notes.txt", why));
        CHECK(std::string_view(why).find("isn't an effect") != std::string_view::npos);
        CHECK_FALSE(viewer.open(engine, "src:/content/fx/never.vfx", why));
        CHECK(std::string_view(why).find("hasn't been cooked") != std::string_view::npos);
        CHECK_FALSE(viewer.is_open());

        REQUIRE(viewer.open(engine, "src:/content/fx/two.vfx", why));
        CHECK(viewer.is_open());
        CHECK((viewer.path() == "src:/content/fx/two.vfx"));
        // The editor's frame: the viewer moves along, what loaded is taken in, its window shows it, the frame draws.
        // A third of a second a frame, so its numbers are fresh each one.
        const auto frame = [&] {
            viewer.update(engine, 0.34f, nullptr);
            assets->update();
            gpu_assets->update();
            viewer.show(96, 96);
            device->begin_frame();
            uploader->record();
            viewer.draw();
            device->end_frame();
        };
        const auto particles = [&] { return viewer.scene()->stats().particles; };
        viewer.playback().playing = false;
        for (uint32_t i = 0; i < 40 && viewer.emitters().empty(); ++i) {
            frame();
        }
        REQUIRE(viewer.emitters().size() == 2);
        CHECK((viewer.emitters()[0].name == "quick"));
        CHECK((viewer.emitters()[1].name == "slow"));
        CHECK(viewer.emitters()[1].rate_per_s == 30.0f);
        CHECK(viewer.emitters()[1].lifetime_s[1] == 4.0f);
        // The time bar: twice its longest life.
        CHECK(viewer.playback().length_s == 8.0);

        // Two seconds old: thirty of the quick ones alive (a life's worth), sixty of the slow (all there have been).
        viewer.playback().seek(2.0);
        frame();
        CHECK(particles().steps == 120); // from nothing to there in the one frame
        CHECK(particles().emitters_drawn == 2);
        frame();
        CHECK(particles().steps == 0); // held: nothing more
        CHECK(viewer.emitters()[0].held);
        CHECK(viewer.emitters()[0].alive == 30);
        CHECK(viewer.emitters()[1].alive == 60);
        CHECK(viewer.emitters()[1].capacity >= 120);

        // A step on is one step of the simulation; a step back is all the way again, to one step short.
        viewer.playback().step(1);
        frame();
        CHECK(particles().steps == 1);
        viewer.playback().step(-1);
        frame();
        CHECK(particles().steps == 120);
        CHECK(particles().emitters_drawn == 2);

        // The time dragged far on, then back: the slow one's four seconds (240 steps and one) in a single frame,
        // so the picture never goes empty under the hand.
        viewer.playback().seek(60.0);
        frame();
        CHECK(particles().steps == 241);
        CHECK(particles().emitters_drawn == 2);
        viewer.playback().seek(10.0);
        frame();
        CHECK(particles().steps == 241);
        CHECK(particles().emitters_drawn == 2);
        frame();
        CHECK(viewer.emitters()[0].alive == 30);
        CHECK(viewer.emitters()[1].alive == 120);

        // An emitter left out of the picture isn't drawn, and goes on being simulated.
        viewer.set_hidden(1U << 1U);
        viewer.playback().step(1);
        frame();
        CHECK(particles().emitters_drawn == 1);
        CHECK(particles().steps == 1);
        frame();
        CHECK(viewer.emitters()[1].alive == 120);
        viewer.set_hidden(0);
        frame();
        CHECK(particles().emitters_drawn == 2);

        // Played, it moves on by the frame's time at its speed.
        viewer.playback().speed = 0.5f;
        viewer.playback().playing = true;
        const double before = viewer.playback().age_s;
        frame();
        CHECK(viewer.playback().age_s == doctest::Approx(before + (0.34 * 0.5)));
        CHECK(particles().steps > 5);
        CHECK(particles().emitters_drawn == 2);

        // Another effect in its place starts from nothing; closed, its scene is gone.
        REQUIRE(viewer.open(engine, "src:/content/fx/raw.vfx", why));
        CHECK(viewer.playback().age_s == 0.0);
        CHECK(viewer.emitters().empty());
        viewer.close(engine);
        CHECK_FALSE(viewer.is_open());
        CHECK(viewer.scene() == nullptr);
        assets->update();
        device->wait_idle();
    }
    device->wait_idle();
    de::test::check_clean(*device);
}
