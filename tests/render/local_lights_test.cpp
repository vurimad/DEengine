#include "app/preview_scene.h"
#include "asset/light_format.h"
#include "asset/mesh_format.h"
#include "asset/registry.h"
#include "core/job_system.h"
#include "editor/source_files.h"
#include "render/rhi/device.h"
#include "render/rhi/upload.h"
#include "render/scene/gpu_assets.h"
#include "render/scene/local_lights.h"
#include "render/scene/scene_renderer.h"
#include "render/shader/shader_compiler.h"
#include "render/shader/shader_library.h"
#include "support/cook_kit.h"
#include "support/gltf_source.h"
#include "support/gpu.h"
#include "support/sandbox.h"
#include "world/render_extract.h"
#include "world/transform.h"

#include <doctest/doctest.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

using de::DVec3;
using de::Vec3;
using de::asset::AssetId;
using de::test::Sandbox;
using de::world::LocalLight;

TEST_CASE("local lights: a light's values have limits, its flicker is the same for the same time") {
    de::asset::LightDesc light;
    CHECK(light.valid());
    light.radius_m = 0.0f; // it ends nowhere
    CHECK_FALSE(light.valid());
    light = {};
    light.cone_deg = 120.0f; // wider than a half space
    CHECK_FALSE(light.valid());
    light = {};
    light.color[1] = -0.5f;
    CHECK_FALSE(light.valid());
    light = {};
    light.flicker = 1.5f;
    CHECK_FALSE(light.valid());
    // The world's component is the same thing.
    light = {.color = {1.0f, 0.5f, 0.25f}, .intensity = 3.0f, .radius_m = 6.0f, .cone_deg = 20.0f, .flicker = 0.4f};
    const de::asset::LightDesc back = LocalLight::from(light).desc();
    CHECK(back.color[1] == 0.5f);
    CHECK(back.intensity == 3.0f);
    CHECK(back.radius_m == 6.0f);
    CHECK(back.cone_deg == 20.0f);
    CHECK(back.flicker == 0.4f);

    // A flicker stays within 0 and 1, moves, differs from light to light, and is the same for the same time.
    float low = 1.0f;
    float high = 0.0f;
    for (int step = 0; step < 2000; ++step) {
        const float at = de::world::light_flicker(static_cast<double>(step) * 0.013, 8.0f, 7);
        CHECK(at >= 0.0f);
        CHECK(at <= 1.0f);
        low = std::min(low, at);
        high = std::max(high, at);
    }
    CHECK(low < 0.2f);
    CHECK(high > 0.8f);
    CHECK(de::world::light_flicker(12.5, 8.0f, 7) == de::world::light_flicker(12.5, 8.0f, 7));
    CHECK(de::world::light_flicker(12.5, 8.0f, 7) != de::world::light_flicker(12.5, 8.0f, 8));
    // Hours in, it still moves between two frames (the time is taken in double).
    CHECK(de::world::light_flicker(36000.0, 8.0f, 7) != de::world::light_flicker(36000.0 + (1.0 / 240.0), 8.0f, 7));
}

TEST_CASE("local lights: the view's slices thicken with distance, the first and last open-ended") {
    using de::render::light_slice;
    CHECK(light_slice(0.01f) == 0);
    CHECK(light_slice(de::render::kLightSliceNearM) == 0);
    CHECK(light_slice(100000.0f) == de::render::kLightGridZ - 1);
    uint32_t last = 0;
    for (int step = 0; step < 160; ++step) {
        // From 5 cm to past 2 km, 7% farther each step.
        const uint32_t slice = light_slice(0.05f * std::pow(1.07f, static_cast<float>(step)));
        CHECK(slice >= last);     // never back
        CHECK(slice <= last + 1); // none skipped at this step
        last = slice;
    }
    CHECK(last == de::render::kLightGridZ - 1);
    // A doubling of depth crosses the same number of slices anywhere.
    CHECK(light_slice(8.0f) - light_slice(4.0f) == light_slice(64.0f) - light_slice(32.0f));
}

TEST_CASE("local lights: a light lights what it reaches, a spot what's in its cone, by the square of the distance") {
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    const Sandbox sandbox;
    // A square two metres a side facing +Z, cooked as any model is.
    std::vector<de::test::Vertex> vertices;
    std::vector<uint32_t> indices;
    de::test::quad(vertices, indices);
    de::test::write_src(sandbox, "content/props/square.gltf",
                        de::test::gltf(vertices, indices, 0.0f, R"([{"name": "paint"}])", "[]", "[]"));
    AssetId mesh_id;
    {
        de::platform::Vfs cook_vfs = de::test::cooker_vfs(sandbox);
        REQUIRE(de::test::run_cook(cook_vfs).ok());
        de::asset::Guid guid;
        REQUIRE(de::editor::source_asset_id(cook_vfs, "src:/content/props/square.gltf", guid, mesh_id));
    }
    de::core::JobSystem jobs({.worker_count = 2});
    const char* other_root = std::getenv("DE_TEST_ENGINE_ROOT"); // NOLINT(concurrency-mt-unsafe): before any thread
    de::platform::Vfs vfs =
        sandbox.make_vfs({.engine_root = other_root != nullptr && other_root[0] != '\0' ? other_root : DE_SOURCE_DIR,
                          .jobs = &jobs,
                          .game_writable = true});
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
        const de::asset::AssetHandle mesh = assets->load<de::asset::MeshAsset>(mesh_id);
        assets->wait_all();
        REQUIRE(assets->state(mesh) == de::asset::AssetState::Ready);
        gpu_assets->update();
        uploader->flush_and_wait();
        gpu_assets->update();
        REQUIRE(gpu_assets->mesh(mesh) != nullptr);

        constexpr uint32_t kSide = 64;
        auto scene = de::app::PreviewScene::create({.device = &*device,
                                                    .shaders = &*shaders,
                                                    .assets = &*assets,
                                                    .gpu_assets = &*gpu_assets,
                                                    .jobs = &jobs,
                                                    .max_instances = 64,
                                                    .particle_pool_blocks = 16});
        REQUIRE(scene.ok());
        scene->set_size(kSide, kSide);
        // Six metres back, looking at the square's middle: it spans the middle half of the picture.
        scene->set_camera({.position = {0.0, 0.0, 6.0}});
        // No sun, no sky: whatever shows is the local lights'.
        de::render::SceneLighting lighting;
        for (int channel = 0; channel < 3; ++channel) {
            lighting.sun_color[channel] = 0.0f;
            lighting.sky_color[channel] = 0.0f;
            lighting.ground_color[channel] = 0.0f;
            lighting.shadow_color[channel] = 0.0f;
        }
        lighting.backdrop = {.enabled = true, .top = {0.0f, 0.0f, 0.0f}, .bottom = {0.0f, 0.0f, 0.0f}};
        de::core::Array<std::byte> texels(de::core::heap_allocator(de::core::MemoryTag::Render));
        std::vector<std::byte> last;
        const auto frames = [&](uint32_t count) {
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t width = 0;
                uint32_t height = 0;
                uint64_t tag = 0;
                while (scene->take_picture(texels, width, height, tag)) {
                    last.assign(texels.begin(), texels.end());
                }
                scene->advance(1.0 / 60.0);
                gpu_assets->update();
                device->begin_frame();
                uploader->record();
                scene->draw(lighting, false, 1);
                device->end_frame();
            }
        };
        // How bright the picture is at a place on the square, in meters from its middle (x right, y up).
        const auto red_at = [&](float x_m, float y_m) {
            const float half_m =
                6.0f * std::tan(0.3f); // what half the picture spans at the square: the camera's 0.6 rad
            const auto px = static_cast<uint32_t>((0.5f + (0.5f * x_m / half_m)) * static_cast<float>(kSide));
            const auto py = static_cast<uint32_t>((0.5f - (0.5f * y_m / half_m)) * static_cast<float>(kSide));
            return static_cast<int>(last[((size_t{py} * kSide) + px) * 4]);
        };
        const flecs::world& ecs = scene->world().ecs();
        const flecs::entity square = ecs.entity();
        square.add<de::world::StaticTransform>();
        square.set(de::world::MeshInstance{.mesh = mesh});
        de::world::place(square, de::world::Transform{});
        frames(8);
        REQUIRE(last.size() == size_t{kSide} * kSide * 4);
        CHECK(scene->stats().visible_draws == 1);
        CHECK(scene->stats().lights.lights == 0);
        CHECK(red_at(0.0f, 0.0f) < 4); // there, and dark

        // A point light a metre in front of its middle: the middle bright, less so toward its edge, nothing past it.
        const flecs::entity lamp = ecs.entity();
        lamp.set(LocalLight{.intensity = 3.0f, .radius_m = 4.0f});
        de::world::place(lamp, de::world::Transform{.position = DVec3(0.0, 0.0, 1.0)});
        frames(8);
        CHECK(scene->stats().lights.lights == 1);
        CHECK(scene->stats().lights.lit == 1);
        const int middle = red_at(0.0f, 0.0f);
        const int toward_edge = red_at(0.8f, 0.0f);
        CHECK(middle > 120);
        CHECK(toward_edge > 20);
        CHECK(toward_edge < middle - 20);                                       // farther, and at a slant
        CHECK(red_at(0.0f, 0.8f) == doctest::Approx(toward_edge).epsilon(0.1)); // the same every way round
        CHECK(red_at(1.5f, 0.0f) < 4);                                          // off the square: nothing to light

        // Twice as far, a quarter of the light at the middle (and less than half as bright a picture).
        de::world::place(lamp, de::world::Transform{.position = DVec3(0.0, 0.0, 2.0)});
        frames(8);
        const int farther = red_at(0.0f, 0.0f);
        CHECK(farther > 20);
        CHECK(farther < middle - 30);

        // Past its radius it lights nothing, and isn't sent at all once its sphere is out of view.
        de::world::place(lamp, de::world::Transform{.position = DVec3(0.0, 0.0, 4.5)});
        frames(8);
        CHECK(scene->stats().lights.lit == 1); // in view, between the camera and the square
        CHECK(red_at(0.0f, 0.0f) < 4);
        de::world::place(lamp, de::world::Transform{.position = DVec3(0.0, 0.0, 30.0)});
        frames(8);
        CHECK(scene->stats().lights.lights == 1);
        CHECK(scene->stats().lights.lit == 0); // behind the camera, its light not reaching the view
        // Its entity made twice the size, it reaches twice as far (and is four times as bright).
        de::world::place(lamp, de::world::Transform{.position = DVec3(0.0, 0.0, 4.5), .scale = Vec3(2.0f)});
        frames(8);
        CHECK(red_at(0.0f, 0.0f) > 20);

        // A spot two metres off, ten degrees from its axis: a disc 35 cm across the middle, dark round it.
        lamp.set(LocalLight{.intensity = 12.0f, .radius_m = 6.0f, .cone_deg = 10.0f, .cone_soft = 0.2f});
        de::world::place(lamp, de::world::Transform{.position = DVec3(0.0, 0.0, 2.0)});
        frames(8);
        CHECK(red_at(0.0f, 0.0f) > 120);
        CHECK(red_at(0.8f, 0.0f) < 4);
        CHECK(red_at(0.0f, -0.8f) < 4);
        // Turned to face away, it lights nothing of the square.
        de::world::place(lamp, de::world::Transform{.position = DVec3(0.0, 0.0, 2.0),
                                                    .rotation = glm::angleAxis(glm::pi<float>(), Vec3(0, 1, 0))});
        frames(8);
        CHECK(red_at(0.0f, 0.0f) < 4);
        // Turned a little aside, its disc has moved along the square: the way it was turned.
        de::world::place(lamp, de::world::Transform{.position = DVec3(0.0, 0.0, 2.0),
                                                    .rotation = glm::angleAxis(glm::radians(20.0f), Vec3(0, 1, 0))});
        frames(8);
        CHECK(red_at(0.0f, 0.0f) < 4);
        CHECK(red_at(-0.73f, 0.0f) > 60); // 2 m times the tangent of 20 degrees, to the left: -Z turned about +Y

        // In fog a light glows in the air round it, where there's no surface to light: beside the square, against
        // the black backdrop. Without fog, nothing there.
        lamp.set(LocalLight{.intensity = 3.0f, .radius_m = 4.0f});
        de::world::place(lamp, de::world::Transform{.position = DVec3(1.4, 0.0, 1.5)});
        CHECK(red_at(1.6f, 0.0f) < 4);
        lighting.fog.density_per_m = 0.3f;
        lighting.fog.end_m = 1000.0f;
        lighting.fog.volume_distance_m = 20.0f; // the froxel grid, which the lights light
        frames(40);                             // the grid blends into last frame's
        const int glow = red_at(1.6f, 0.0f);
        MESSAGE("a light's glow in fog, beside the square: ", glow);
        CHECK(glow > 12);
        lighting.fog = {};
        frames(8);
        CHECK(red_at(1.6f, 0.0f) < 4);

        // More lights than a frame lights with: the nearest for their size are lit, the rest counted.
        lamp.destruct();
        constexpr uint32_t kMany = de::render::kMaxFrameLights + 44;
        for (uint32_t i = 0; i < kMany; ++i) {
            const flecs::entity one = ecs.entity();
            one.set(LocalLight{.intensity = 0.02f, .radius_m = 3.0f});
            // In a line across the view, a little in front of the square.
            de::world::place(
                one, de::world::Transform{.position = DVec3((static_cast<double>(i) * 0.005) - 0.75, 0.0, 1.0)});
        }
        frames(8);
        CHECK(scene->stats().lights.lights == kMany);
        CHECK(scene->stats().lights.lit == de::render::kMaxFrameLights);
        CHECK(scene->stats().lights.dropped == 44);
        CHECK(red_at(0.0f, 0.0f) > 120); // all of them together, through every word of the clusters

        device->wait_idle();
        assets->release(mesh);
        assets->update();
        device->wait_idle();
    }
    device->wait_idle();
    de::test::check_clean(*device);
}

TEST_CASE("benchmark: what local lights cost the GPU at 2560 x 1440, a floor lit by 21 candles and by 256 lights") {
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    const Sandbox sandbox;
    std::vector<de::test::Vertex> vertices;
    std::vector<uint32_t> indices;
    de::test::quad(vertices, indices);
    de::test::write_src(sandbox, "content/props/square.gltf",
                        de::test::gltf(vertices, indices, 0.0f, R"([{"name": "paint"}])", "[]", "[]"));
    AssetId mesh_id;
    {
        de::platform::Vfs cook_vfs = de::test::cooker_vfs(sandbox);
        REQUIRE(de::test::run_cook(cook_vfs).ok());
        de::asset::Guid guid;
        REQUIRE(de::editor::source_asset_id(cook_vfs, "src:/content/props/square.gltf", guid, mesh_id));
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
        const de::asset::AssetHandle mesh = assets->load<de::asset::MeshAsset>(mesh_id);
        assets->wait_all();
        gpu_assets->update();
        uploader->flush_and_wait();
        gpu_assets->update();
        REQUIRE(gpu_assets->mesh(mesh) != nullptr);
        auto scene = de::app::PreviewScene::create({.device = &*device,
                                                    .shaders = &*shaders,
                                                    .assets = &*assets,
                                                    .gpu_assets = &*gpu_assets,
                                                    .jobs = &jobs,
                                                    .max_instances = 64,
                                                    .particle_pool_blocks = 16});
        REQUIRE(scene.ok());
        scene->set_size(2560, 1440);
        // A floor 24 m square filling the picture, seen from a man's height looking a little down.
        scene->set_camera({.position = {0.0, 1.7, 9.0}, .rotation = glm::angleAxis(-0.2f, Vec3(1, 0, 0))});
        const flecs::world& ecs = scene->world().ecs();
        const flecs::entity floor = ecs.entity();
        floor.add<de::world::StaticTransform>();
        floor.set(de::world::MeshInstance{.mesh = mesh});
        de::world::place(floor, de::world::Transform{.rotation = glm::angleAxis(-glm::half_pi<float>(), Vec3(1, 0, 0)),
                                                     .scale = Vec3(12.0f)});
        const de::render::SceneLighting lighting; // a sun and a sky, as a game's frame has
        // The mean GPU time of the cull and of the deferred light over some frames, once settled.
        struct Cost {
            float cull_ms = 0.0f;
            float light_ms = 0.0f;
        };
        const auto measure = [&] {
            const auto frame = [&] {
                scene->advance(1.0 / 60.0);
                gpu_assets->update();
                device->begin_frame();
                uploader->record();
                scene->draw(lighting);
                device->end_frame();
            };
            for (int i = 0; i < 40; ++i) {
                frame();
            }
            Cost sum;
            constexpr int kFrames = 80;
            for (int i = 0; i < kFrames; ++i) {
                frame();
                for (const de::render::PassGpuTime& pass : scene->gpu_times()) {
                    if (pass.name.view() == "light cull") {
                        sum.cull_ms += pass.ms / static_cast<float>(kFrames);
                    } else if (pass.name.view() == "deferred light") {
                        sum.light_ms += pass.ms / static_cast<float>(kFrames);
                    }
                }
            }
            return sum;
        };
        const Cost none = measure();
        CHECK(scene->stats().lights.lit == 0);
        // The shrine's: seven groups of three candles in a ring, each reaching under 3 m.
        std::vector<flecs::entity> lights;
        for (int group = 0; group < 7; ++group) {
            const float around = glm::two_pi<float>() * static_cast<float>(group) / 7.0f;
            for (int candle = 0; candle < 3; ++candle) {
                const flecs::entity one = ecs.entity();
                one.set(
                    LocalLight{.color = {1.0f, 0.52f, 0.2f}, .intensity = 0.4f, .radius_m = 2.8f, .flicker = 0.25f});
                de::world::place(
                    one, de::world::Transform{.position = DVec3((std::cos(around) * 2.2) + (candle * 0.15), 0.2,
                                                                (std::sin(around) * 2.2) + 2.0 - (candle * 0.1))});
                lights.push_back(one);
            }
        }
        const Cost candles = measure();
        CHECK(scene->stats().lights.lit == 21);
        for (const flecs::entity one : lights) {
            one.destruct();
        }
        // As many as a frame lights with, close together: 3 m lights every 1.25 m, so every pixel is in reach of
        // a dozen and more.
        for (int i = 0; i < 256; ++i) {
            const flecs::entity one = ecs.entity();
            one.set(LocalLight{.intensity = 0.05f, .radius_m = 3.0f});
            const int column = i % 16;
            const int row = i / 16;
            de::world::place(one,
                             de::world::Transform{.position = DVec3((static_cast<double>(column) * 1.25) - 9.4, 0.3,
                                                                    (static_cast<double>(row) * 1.25) - 12.0)});
        }
        const Cost many = measure();
        const uint32_t many_lit = scene->stats().lights.lit; // those of the 256 whose light reaches the view
        CHECK(many_lit > 128);
        MESSAGE("local lights at 2560 x 1440: no lights, deferred light ", none.light_ms,
                " ms; 21 candles, light cull ", candles.cull_ms, " ms and deferred light ", candles.light_ms, " ms; ",
                many_lit, " lights overlapping, light cull ", many.cull_ms, " ms and deferred light ", many.light_ms,
                " ms");
        device->wait_idle();
        assets->release(mesh);
        assets->update();
        device->wait_idle();
    }
    device->wait_idle();
    de::test::check_clean(*device);
}
