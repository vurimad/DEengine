#include "asset/mesh_format.h"
#include "asset/registry.h"
#include "core/job_system.h"
#include "core/math.h"
#include "editor/source_files.h"
#include "render/graph/render_graph.h"
#include "render/rhi/command_list.h"
#include "render/rhi/device.h"
#include "render/rhi/upload.h"
#include "render/scene/gpu_assets.h"
#include "render/scene/path_tracer.h"
#include "render/scene/scene_renderer.h"
#include "render/shader/shader_compiler.h"
#include "render/shader/shader_library.h"
#include "support/cook_kit.h"
#include "support/gltf_source.h"
#include "support/gpu.h"
#include "support/readback.h"
#include "support/render_transform.h"
#include "support/sandbox.h"
#include "world/render_extract.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

using de::asset::AssetId;
using de::render::SceneView;
using de::test::Sandbox;

namespace {

constexpr uint32_t kSize = 64;

} // namespace

// The furnace: inside a closed box whose walls all glow by Le and scatter a share rho of what falls on them, the light
// is the same everywhere and known on paper: Le + rho Le + rho^2 Le + ... = Le / (1 - rho). With bounces held to n it's
// that sum's first n + 1 terms, the same for every path, so exact. The path tracer must give both.
TEST_CASE("path tracer: in a glowing furnace it gives what's known on paper, exactly with bounces held and on "
          "average with none; the compare finds the rasterized picture as far off as it is") {
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    if (!device->capabilities().ray_query) {
        MESSAGE("this GPU has no ray queries: nothing to path trace");
        return;
    }
    const Sandbox sandbox;
    std::vector<de::test::Vertex> vertices;
    std::vector<uint32_t> indices;
    de::test::quad(vertices, indices);
    // Grey, glowing, seen from both sides, not a metal: Le 0.5, rho 0.5.
    de::test::write_src(sandbox, "content/props/furnace.gltf",
                        de::test::gltf(vertices, indices, 0.0f,
                                       R"([{"name": "furnace", "doubleSided": true, "emissiveFactor": [0.5, 0.5, 0.5],
                                            "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.5, 0.5, 1],
                                                                     "metallicFactor": 0, "roughnessFactor": 1}}])",
                                       "[]", "[]"));
    AssetId wall_id;
    {
        de::platform::Vfs cook_vfs = de::test::cooker_vfs(sandbox);
        REQUIRE(de::test::run_cook(cook_vfs).ok());
        de::asset::Guid guid;
        REQUIRE(de::editor::source_asset_id(cook_vfs, "src:/content/props/furnace.gltf", guid, wall_id));
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
        auto renderer = de::render::SceneRenderer::create({.device = &*device,
                                                           .shaders = &*shaders,
                                                           .gpu_assets = &*gpu_assets,
                                                           .assets = &*assets,
                                                           .ray_tracing = true});
        REQUIRE(renderer.ok());
        REQUIRE(renderer->path_tracer() != nullptr);
        const de::asset::AssetHandle wall = assets->load<de::asset::MeshAsset>(wall_id);
        assets->wait_all();
        gpu_assets->update();
        uploader->flush_and_wait();
        gpu_assets->update();
        REQUIRE(gpu_assets->mesh(wall) != nullptr);

        const auto target =
            device->create_texture({.width = kSize,
                                    .height = kSize,
                                    .format = de::rhi::Format::RGBA8Unorm,
                                    .usage = de::rhi::TextureUsage::RenderTarget | de::rhi::TextureUsage::CopySrc,
                                    .debug_name = "scene target"});
        REQUIRE(target.ok());
        auto graph = de::render::RenderGraph::create(*device);
        REQUIRE(graph.ok());
        de::world::RenderFrame frame;
        frame.all_instances = true;
        // No sun, no sky: all the light is the walls'.
        de::render::SceneLighting light;
        for (int channel = 0; channel < 3; ++channel) {
            light.sun_color[channel] = 0.0f;
            light.shadow_color[channel] = 0.0f;
            light.sky_color[channel] = 0.0f;
            light.ground_color[channel] = 0.0f;
        }
        // A box 4 m a side round (0.3, 6.3, 0.3): six squares of 4.04 m, a little past each edge so no ray finds a
        // crack where two meet. The camera in its middle, looking along -Z.
        constexpr float kHalf = 2.0f;
        constexpr float kScale = 2.02f;
        const float c[3] = {0.3f, 6.3f, 0.3f};
        for (const float side : {-1.0f, 1.0f}) {
            frame.instances.push({.transform = de::test::render_transform(
                                      {kScale, 0, 0, c[0], 0, kScale, 0, c[1], 0, 0, kScale, c[2] + (side * kHalf)}),
                                  .mesh = wall}); // across Z
            frame.instances.push({.transform = de::test::render_transform(
                                      {0, 0, kScale, c[0] + (side * kHalf), 0, kScale, 0, c[1], -kScale, 0, 0, c[2]}),
                                  .mesh = wall}); // across X
            frame.instances.push({.transform = de::test::render_transform(
                                      {kScale, 0, 0, c[0], 0, 0, -kScale, c[1] + (side * kHalf), 0, kScale, 0, c[2]}),
                                  .mesh = wall}); // across Y
        }
        frame.camera = {
            .transform = {1, 0, 0, c[0], 0, 1, 0, c[1], 0, 0, 1, c[2]}, .vertical_fov_rad = 1.0f, .valid = true};
        const auto draw = [&] {
            ++frame.number;
            gpu_assets->update();
            device->begin_frame();
            uploader->record();
            graph->begin(kSize, kSize);
            const de::render::GraphTexture color = graph->import_texture(
                {.texture = *target, .name = "target", .final_state = de::rhi::ResourceState::CopySrc});
            renderer->add_passes(*graph, color, de::rhi::Format::RGBA8Unorm, frame, light, SceneView::Lit);
            graph->execute();
            device->end_frame();
            shaders->update();
        };
        // The mean's red, per pixel, once the frames that added it have run.
        de::render::PathTracer& tracer = *renderer->path_tracer();
        const auto mean = [&] {
            device->wait_idle();
            const de::rhi::TextureHandle accumulation = tracer.accumulation();
            device->begin_frame();
            de::rhi::CommandList list = device->begin_command_list();
            list.barrier(de::rhi::TextureBarrier{.texture = accumulation,
                                                 .before = de::rhi::ResourceState::ShaderRead,
                                                 .after = de::rhi::ResourceState::CopySrc});
            device->end_frame();
            const de::core::Array<std::byte> bytes = de::test::read_texture(*device, accumulation);
            device->begin_frame();
            de::rhi::CommandList back = device->begin_command_list();
            back.barrier(de::rhi::TextureBarrier{.texture = accumulation,
                                                 .before = de::rhi::ResourceState::CopySrc,
                                                 .after = de::rhi::ResourceState::ShaderRead});
            device->end_frame();
            std::vector<float> red(size_t{kSize} * kSize);
            for (size_t i = 0; i < red.size(); ++i) {
                std::memcpy(&red[i], bytes.data() + (i * 4 * sizeof(float)), sizeof(float));
            }
            return red;
        };
        renderer->set_path_traced(true);
        // The shaders build in the background: a few frames first.
        for (int i = 0; i < 4; ++i) {
            draw();
        }
        // Bounces held: every path the same, every pixel exact.
        for (const uint32_t bounces : {0U, 1U, 2U}) {
            tracer.set_max_bounces(bounces);
            for (int i = 0; i < 4; ++i) {
                draw();
            }
            const std::vector<float> red = mean();
            const float expected =
                0.5f * (2.0f - std::pow(0.5f, static_cast<float>(bounces))); // Le (1 - rho^(n+1)) / (1 - rho)
            const auto [low, high] = std::ranges::minmax_element(red);
            MESSAGE("bounces ", bounces, ": every pixel ", *low, " to ", *high, ", expected ", expected);
            CHECK(*low == doctest::Approx(expected).epsilon(0.002));
            CHECK(*high == doctest::Approx(expected).epsilon(0.002));
        }
        // No hold: Russian roulette ends paths at random, and the mean comes to the sum, Le / (1 - rho) = 1.
        tracer.set_max_bounces(256);
        for (int i = 0; i < 256; ++i) {
            draw();
        }
        CHECK(renderer->stats().path_tracer_samples >= 250);
        const std::vector<float> red = mean();
        double sum = 0.0;
        for (const float value : red) {
            sum += value;
        }
        const double average = sum / static_cast<double>(red.size());
        const auto [low, high] = std::ranges::minmax_element(red);
        MESSAGE("unheld, ", renderer->stats().path_tracer_samples, " samples: the picture's mean ", average,
                ", pixels ", *low, " to ", *high, ", expected 1");
        CHECK(average == doctest::Approx(1.0).epsilon(0.01));
        CHECK(*low > 0.85f);
        CHECK(*high < 1.15f);

        // The compare: the rasterized picture, with no probes and no ambient, shows only the walls' own glow, 0.5. The
        // path tracer held to no bounces gives the same, so they agree; with bounces it gives 1, so the rasterized
        // picture is a stop too dark everywhere.
        renderer->set_path_traced(false);
        renderer->set_lighting_compare(true);
        tracer.set_max_bounces(0);
        for (int i = 0; i < 8; ++i) {
            draw();
        }
        const de::render::LightingCompareStats same = renderer->stats().compare;
        MESSAGE("compared with no bounces: ", same.pixels, " pixels, ", same.mean_stops, " stops off on average");
        CHECK(same.pixels == kSize * kSize);
        CHECK(same.mean_stops < 0.01f);
        CHECK(same.over_10 == 0.0f);
        tracer.set_max_bounces(256);
        for (int i = 0; i < 64; ++i) {
            draw();
        }
        const de::render::LightingCompareStats darker = renderer->stats().compare;
        MESSAGE("compared with bounces: ", darker.mean_stops, " stops off on average, ", darker.darker_10 * 100.0f,
                "% too dark by 10%");
        CHECK(darker.mean_stops == doctest::Approx(1.0f).epsilon(0.1));
        CHECK(darker.darker_10 > 0.95f);
        CHECK(darker.brighter_10 == 0.0f);

        // A light box over the furnace (GI plan step 3): baked, the light arriving from every way is the walls' 1, so
        // the rasterized picture is 0.5 + 0.5 x 1 = 1, as the path tracer's. A little past the walls each way, so a
        // wall's surface is well inside it, and hardly faded at its edge.
        const de::render::LightBoxDesc box{.min_m = de::DVec3(c[0] - 2.2, c[1] - 2.2, c[2] - 2.2),
                                           .max_m = de::DVec3(c[0] + 2.2, c[1] + 2.2, c[2] + 2.2),
                                           .voxel_m = 0.5f,
                                           .fade_m = 0.05f};
        renderer->set_light_boxes(std::span(&box, 1));
        for (int i = 0; i < 40 && renderer->stats().light_boxes.baked < 1.0f; ++i) {
            draw();
        }
        CHECK(renderer->stats().light_boxes.baked == 1.0f);
        for (int i = 0; i < 8; ++i) {
            draw();
        }
        const de::render::LightingCompareStats boxed = renderer->stats().compare;
        MESSAGE("compared with a light box baked: ", boxed.mean_stops, " stops off on average, ",
                boxed.over_10 * 100.0f, "% off by over 10%; ", renderer->stats().light_boxes.voxels, " voxels");
        CHECK(boxed.mean_stops < 0.05f);
        CHECK(boxed.over_10 < 0.05f);

        device->wait_idle();
        device->destroy(*target);
        assets->release(wall);
        assets->update();
        device->wait_idle();
    }
    device->wait_idle();
    de::test::check_clean(*device);
}

// Direct light (GI plan step 2): a floor, a plate above it and a point light above that, no sun and no sky, the probes
// and the ambient occlusion off and the path tracer held to no bounces: the rasterized picture's light is what the
// path tracer's is, its shadow maps casting the plate's shadow where the path tracer's rays find it. Without the
// local light's shadow, the shadow (about a seventh of the picture) would be too bright.
TEST_CASE("path tracer: a local light's direct light and shadow come out in the rasterized picture as path traced") {
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    if (!device->capabilities().ray_query) {
        MESSAGE("this GPU has no ray queries: nothing to path trace");
        return;
    }
    const Sandbox sandbox;
    std::vector<de::test::Vertex> vertices;
    std::vector<uint32_t> indices;
    de::test::quad(vertices, indices);
    de::test::write_src(sandbox, "content/props/white.gltf",
                        de::test::gltf(vertices, indices, 0.0f,
                                       R"([{"name": "white", "doubleSided": true,
                                            "pbrMetallicRoughness": {"baseColorFactor": [0.7, 0.7, 0.7, 1],
                                                                     "metallicFactor": 0, "roughnessFactor": 1}}])",
                                       "[]", "[]"));
    // And a one-sided wall, seen from behind by one camera: drawn, its back isn't there; path traced, a ray goes on
    // past it, and what it meets next is placed from there.
    de::test::write_src(sandbox, "content/props/one_sided.gltf",
                        de::test::gltf(vertices, indices, 0.0f,
                                       R"([{"name": "one sided", "pbrMetallicRoughness": {"baseColorFactor": [0.7, 0.7,
                                            0.7, 1], "metallicFactor": 0, "roughnessFactor": 1}}])",
                                       "[]", "[]"));
    AssetId white_id;
    AssetId one_sided_id;
    {
        de::platform::Vfs cook_vfs = de::test::cooker_vfs(sandbox);
        REQUIRE(de::test::run_cook(cook_vfs).ok());
        de::asset::Guid guid;
        REQUIRE(de::editor::source_asset_id(cook_vfs, "src:/content/props/white.gltf", guid, white_id));
        REQUIRE(de::editor::source_asset_id(cook_vfs, "src:/content/props/one_sided.gltf", guid, one_sided_id));
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
        auto renderer = de::render::SceneRenderer::create({.device = &*device,
                                                           .shaders = &*shaders,
                                                           .gpu_assets = &*gpu_assets,
                                                           .assets = &*assets,
                                                           .ray_tracing = true});
        REQUIRE(renderer.ok());
        REQUIRE(renderer->path_tracer() != nullptr);
        const de::asset::AssetHandle white = assets->load<de::asset::MeshAsset>(white_id);
        const de::asset::AssetHandle one_sided = assets->load<de::asset::MeshAsset>(one_sided_id);
        assets->wait_all();
        gpu_assets->update();
        uploader->flush_and_wait();
        gpu_assets->update();
        REQUIRE(gpu_assets->mesh(white) != nullptr);
        REQUIRE(gpu_assets->mesh(one_sided) != nullptr);

        const auto target =
            device->create_texture({.width = kSize,
                                    .height = kSize,
                                    .format = de::rhi::Format::RGBA8Unorm,
                                    .usage = de::rhi::TextureUsage::RenderTarget | de::rhi::TextureUsage::CopySrc,
                                    .debug_name = "scene target"});
        REQUIRE(target.ok());
        auto graph = de::render::RenderGraph::create(*device);
        REQUIRE(graph.ok());
        de::world::RenderFrame frame;
        frame.all_instances = true;
        de::render::SceneLighting light;
        for (int channel = 0; channel < 3; ++channel) {
            light.sun_color[channel] = 0.0f;
            light.shadow_color[channel] = 0.0f;
            light.sky_color[channel] = 0.0f;
            light.ground_color[channel] = 0.0f;
        }
        // A one-sided wall 1.5 m off on the -X side, facing the plate (+X): the camera on that side sees its back.
        frame.instances.push(
            {.transform = de::test::render_transform({0, 0, 1.5f, -1.2f, 0, 1.5f, 0, 1.5f, -1.5f, 0, 0, 0.3f}),
             .mesh = one_sided});
        // A floor 4 m square facing up, a plate of 0.6 m 0.75 m above it, a light 1.5 m up; the camera 3 m up, looking
        // down. The square faces +Z: turned so it faces +Y.
        frame.instances.push(
            {.transform = de::test::render_transform({2, 0, 0, 0.3f, 0, 0, 2, 0, 0, -2, 0, 0.3f}), .mesh = white});
        frame.instances.push(
            {.transform = de::test::render_transform({0.3f, 0, 0, 0.3f, 0, 0, 0.3f, 0.75f, 0, -0.3f, 0, 0.3f}),
             .mesh = white});
        frame.lights.push({.position = {0.3f, 1.5f, 0.3f}, .radius_m = 10.0f, .color = {2.0f, 2.0f, 2.0f}});
        frame.camera = {
            .transform = {1, 0, 0, 0.3f, 0, 0, 1, 3.0f, 0, -1, 0, 0.3f}, .vertical_fov_rad = 1.0f, .valid = true};
        renderer->set_ambient_occlusion(false);
        renderer->set_lighting_compare(true);
        renderer->path_tracer()->set_max_bounces(0);
        const auto draw_frame = [&] {
            ++frame.number;
            gpu_assets->update();
            device->begin_frame();
            uploader->record();
            graph->begin(kSize, kSize);
            const de::render::GraphTexture color = graph->import_texture(
                {.texture = *target, .name = "target", .final_state = de::rhi::ResourceState::CopySrc});
            renderer->add_passes(*graph, color, de::rhi::Format::RGBA8Unorm, frame, light, SceneView::Lit);
            graph->execute();
            device->end_frame();
            shaders->update();
        };
        for (int i = 0; i < 48; ++i) {
            draw_frame();
        }
        const de::render::LightingCompareStats compared = renderer->stats().compare;
        MESSAGE("direct light against the path tracer: ", compared.mean_stops, " stops off on average; ",
                compared.over_10 * 100.0f, "% off by over 10%, ", compared.over_25 * 100.0f, "% by over 25%; ",
                renderer->stats().lights.shadowed, " light shadowed");
        CHECK(renderer->stats().lights.shadowed == 1);
        CHECK(compared.pixels > kSize * kSize / 2);
        // At 64 x 64 a shadow's edge is a few percent of the picture, each pixel of it a step off.
        CHECK(compared.mean_stops < 0.1f);
        CHECK(compared.over_25 < 0.05f);

        // And the same from every side: a shadow doesn't depend on where it's seen from. Eight cameras round the plate,
        // 2.5 m off and 2 m up, looking at its middle; one low and one straight down.
        const auto look = [&](float x, float y, float z) {
            const de::Vec3 eye(x, y, z);
            const de::Vec3 target(0.3f, 0.4f, 0.3f);
            const de::Vec3 back = glm::normalize(eye - target);
            const de::Vec3 right = glm::normalize(glm::cross(de::Vec3(0, 1, 0), back));
            const de::Vec3 up = glm::cross(back, right);
            frame.camera = {
                .transform = {right.x, up.x, back.x, eye.x, right.y, up.y, back.y, eye.y, right.z, up.z, back.z, eye.z},
                .vertical_fov_rad = 1.0f,
                .valid = true};
        };
        float worst = 0.0f;
        for (int side = 0; side < 10; ++side) {
            const float around = glm::two_pi<float>() * static_cast<float>(side) / 8.0f;
            if (side < 8) {
                look(0.3f + (2.5f * std::cos(around)), 2.0f, 0.3f + (2.5f * std::sin(around)));
            } else if (side == 8) {
                look(0.3f + 3.0f, 0.5f, 0.3f + 0.4f); // low, across the floor
            } else {
                look(0.31f, 3.5f, 0.32f); // nearly straight down
            }
            for (int i = 0; i < 48; ++i) {
                draw_frame();
            }
            const de::render::LightingCompareStats seen = renderer->stats().compare;
            MESSAGE("seen from side ", side, ": ", seen.over_25 * 100.0f, "% off by over 25%, ", seen.mean_stops,
                    " stops on average");
            worst = std::max(worst, seen.over_25);
        }
        // A shadow's edge is soft now (a light has a size, in both renderers), and the rasterized edge is eight looks a
        // pixel with no TAA under the compare: grain, each grain a step off the path tracer's mean. At 64 x 64 the
        // edge's pixels are up to 6% of the picture from the sides that see it longest (5% when it was hard).
        CHECK(worst < 0.07f);

        // The light's tiles are kept from frame to frame while nothing that casts changes, and drawn again when
        // something does (docs/design.md, Local lights: tiles kept). Sent only what changed, as the world sends it:
        // nothing, and no face is drawn; then the plate moved 0.6 m aside, and the light's six faces are drawn that
        // frame and its shadow is where the path tracer has it, not where it was.
        frame.all_instances = false;
        frame.slot_count = 3;
        frame.instances.clear();
        for (int i = 0; i < 48; ++i) {
            draw_frame();
        }
        CHECK(renderer->stats().lights.shadow_faces == 6);
        CHECK(renderer->stats().lights.shadow_faces_drawn == 0);
        CHECK(renderer->stats().compare.over_25 < 0.07f);
        frame.instances.push(
            {.transform = de::test::render_transform({0.3f, 0, 0, 0.9f, 0, 0, 0.3f, 0.75f, 0, -0.3f, 0, 0.3f}),
             .mesh = white,
             .slot = 2});
        draw_frame();
        CHECK(renderer->stats().lights.shadow_faces_drawn == 6);
        frame.instances.clear();
        for (int i = 0; i < 48; ++i) {
            draw_frame();
        }
        CHECK(renderer->stats().lights.shadow_faces_drawn == 0);
        const de::render::LightingCompareStats moved = renderer->stats().compare;
        MESSAGE("the plate moved, its light's tiles kept but for that frame: ", moved.over_25 * 100.0f,
                "% off by over 25%, ", moved.mean_stops, " stops on average");
        CHECK(moved.over_25 < 0.07f);
        CHECK(moved.mean_stops < 0.1f);

        device->wait_idle();
        device->destroy(*target);
        assets->release(white);
        assets->release(one_sided);
        assets->update();
        device->wait_idle();
    }
    device->wait_idle();
    de::test::check_clean(*device);
}
