#include "asset/mesh_format.h"
#include "asset/registry.h"
#include "core/job_system.h"
#include "editor/source_files.h"
#include "render/graph/render_graph.h"
#include "render/rhi/device.h"
#include "render/rhi/upload.h"
#include "render/scene/gpu_assets.h"
#include "render/scene/scene_renderer.h"
#include "render/scene/trace_scene.h"
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

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using de::asset::AssetId;
using de::render::SceneView;
using de::test::Sandbox;

namespace {

constexpr uint32_t kSize = 64;

const uint8_t* texel(const de::core::Array<std::byte>& pixels, uint32_t x, uint32_t y) {
    return reinterpret_cast<const uint8_t*>(pixels.data()) + (((size_t{y} * kSize) + x) * 4);
}

// By its colour, however dim: in shadow only the sky lights it.
bool reddish(const uint8_t* at) {
    return at[0] > 8 && at[0] > 2 * at[1] && at[0] > 2 * at[2];
}

bool greenish(const uint8_t* at) {
    return at[1] > 8 && at[1] > 2 * at[0] && at[1] > 2 * at[2];
}

} // namespace

TEST_CASE("ray tracing: rays see the coarsest level of detail that's as good as the finest to them") {
    using de::render::trace_lod;
    // Four levels of two parts each: the finest, two within what a ray can tell, one past it.
    de::render::GpuMeshLod lods[4] = {{.error_m = 0.0f, .first_part = 0, .part_count = 2},
                                      {.error_m = 0.005f, .first_part = 2, .part_count = 2},
                                      {.error_m = 0.015f, .first_part = 4, .part_count = 2},
                                      {.error_m = 0.05f, .first_part = 6, .part_count = 2}};
    de::render::GpuMesh mesh;
    mesh.lods = lods;
    CHECK(trace_lod(mesh) == 2);
    // One whose seams moved has colours a ray would get wrong: the next finer that kept them.
    mesh.seams_moved_lods = 1U << 2U;
    CHECK(trace_lod(mesh) == 1);
    mesh.seams_moved_lods = (1U << 1U) | (1U << 2U) | (1U << 3U);
    CHECK(trace_lod(mesh) == 0); // a mesh with its uvs in pieces: as it's drawn up close
    mesh.seams_moved_lods = 1U << 1U;
    CHECK(trace_lod(mesh) == 2); // a coarser one may have kept them
    // One that has lost a part isn't the same thing to a ray.
    mesh.seams_moved_lods = 0;
    lods[2].part_count = 1;
    CHECK(trace_lod(mesh) == 1);
    // Only the finest: that one.
    mesh.lods = std::span<const de::render::GpuMeshLod>(lods, 1);
    CHECK(trace_lod(mesh) == 0);
}

TEST_CASE("ray tracing: the traced view shows what rays meet, where the picture has it, the sun's light cut off by "
          "what stands in its way") {
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    const Sandbox sandbox;
    // Two squares two metres a side facing +Z, cooked as any model is: a red one and a green one.
    std::vector<de::test::Vertex> vertices;
    std::vector<uint32_t> indices;
    de::test::quad(vertices, indices);
    de::test::write_src(
        sandbox, "content/props/square.gltf",
        de::test::gltf(vertices, indices, 0.0f,
                       R"([{"name": "red", "pbrMetallicRoughness": {"baseColorFactor": [1, 0, 0, 1]}}])", "[]", "[]"));
    de::test::write_src(
        sandbox, "content/props/cover.gltf",
        de::test::gltf(vertices, indices, 0.0f,
                       R"([{"name": "green", "pbrMetallicRoughness": {"baseColorFactor": [0, 1, 0, 1]}}])", "[]",
                       "[]"));
    AssetId square_id;
    AssetId cover_id;
    {
        de::platform::Vfs cook_vfs = de::test::cooker_vfs(sandbox);
        REQUIRE(de::test::run_cook(cook_vfs).ok());
        de::asset::Guid guid;
        REQUIRE(de::editor::source_asset_id(cook_vfs, "src:/content/props/square.gltf", guid, square_id));
        REQUIRE(de::editor::source_asset_id(cook_vfs, "src:/content/props/cover.gltf", guid, cover_id));
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
        const de::asset::AssetHandle square = assets->load<de::asset::MeshAsset>(square_id);
        const de::asset::AssetHandle cover = assets->load<de::asset::MeshAsset>(cover_id);
        assets->wait_all();
        gpu_assets->update();
        uploader->flush_and_wait();
        gpu_assets->update();
        REQUIRE(gpu_assets->mesh(square) != nullptr);
        REQUIRE(gpu_assets->mesh(cover) != nullptr);

        const auto target =
            device->create_texture({.width = kSize,
                                    .height = kSize,
                                    .format = de::rhi::Format::RGBA8Unorm,
                                    .usage = de::rhi::TextureUsage::RenderTarget | de::rhi::TextureUsage::CopySrc,
                                    .debug_name = "scene target"});
        REQUIRE(target.ok());
        auto graph = de::render::RenderGraph::create(*device);
        REQUIRE(graph.ok());
        // The red square at the origin, seen from 3 m in front; the sun from the right and behind the camera.
        de::world::RenderFrame frame;
        frame.camera = {.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 3}, .vertical_fov_rad = 1.0f, .valid = true};
        frame.all_instances = true;
        frame.instances.push(
            {.transform = de::test::render_transform({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}), .mesh = square});
        const float slant = std::sqrt(0.5f);
        const de::render::SceneLighting light{.sun_direction = {slant, 0.0f, slant}};
        const auto draw = [&](SceneView view) {
            gpu_assets->update();
            device->begin_frame();
            uploader->record();
            graph->begin(kSize, kSize);
            const de::render::GraphTexture color = graph->import_texture(
                {.texture = *target, .name = "target", .final_state = de::rhi::ResourceState::CopySrc});
            renderer->add_passes(*graph, color, de::rhi::Format::RGBA8Unorm, frame, light, view);
            graph->execute();
            device->end_frame();
        };
        const auto render = [&](SceneView view) {
            draw(view);
            return de::test::read_texture(*device, *target);
        };
        // Where a point of the world lands in the picture.
        const auto pixel = [](float x, float y, float z, uint32_t& px, uint32_t& py) {
            const float half = (3.0f - z) * std::tan(0.5f);
            px = static_cast<uint32_t>((0.5f + (0.5f * x / half)) * static_cast<float>(kSize));
            py = static_cast<uint32_t>((0.5f - (0.5f * y / half)) * static_cast<float>(kSize));
        };

        if (!device->capabilities().ray_query) {
            MESSAGE("this GPU has no ray queries: the traced view is the lit one, and nothing is built");
            const de::core::Array<std::byte> plain = render(SceneView::Traced);
            CHECK(reddish(texel(plain, kSize / 2, kSize / 2)));
            CHECK(renderer->stats().trace.meshes == 0);
            CHECK(device->stats().live_accelerations == 0);
        } else {
            // Its bottom level is built in the first frame, and rays see it from the next.
            draw(SceneView::Traced);
            CHECK(renderer->stats().trace.builds == 1);
            CHECK(renderer->stats().trace.meshes == 1);
            draw(SceneView::Traced);
            CHECK(renderer->stats().trace.builds == 0);
            CHECK(renderer->stats().trace.waiting == 0);
            CHECK(renderer->stats().trace.instances == 1);
            const de::core::Array<std::byte> alone = render(SceneView::Traced);
            const uint8_t* middle = texel(alone, kSize / 2, kSize / 2);
            CHECK(reddish(middle)); // the square, by its material
            CHECK(middle[0] > 120); // in the sun
            const uint8_t* sky = texel(alone, 1, 1);
            CHECK_FALSE(reddish(sky)); // past it, the sky
            CHECK(sky[2] >= sky[0]);

            // The green one, half the size, a metre nearer, to the right and a little up: between the red one's
            // middle and the sun, and not between it and the camera.
            frame.instances.push(
                {.transform = de::test::render_transform({0.5f, 0, 0, 1.0f, 0, 0.5f, 0, 0.25f, 0, 0, 0.5f, 1.0f}),
                 .mesh = cover});
            draw(SceneView::Traced);
            draw(SceneView::Traced);
            const de::core::Array<std::byte> both = render(SceneView::Traced);
            CHECK(renderer->stats().trace.meshes == 2);
            CHECK(renderer->stats().trace.instances == 2);
            uint32_t px = 0;
            uint32_t py = 0;
            pixel(1.0f, 0.25f, 1.0f, px, py);
            CHECK(greenish(texel(both, px, py))); // each instance by its own mesh and material
            // The red one's middle is in the green one's shadow now; its left isn't.
            const uint8_t* shaded = texel(both, kSize / 2, kSize / 2);
            CHECK(reddish(shaded));
            CHECK(shaded[0] < middle[0] - 30);
            pixel(-0.8f, 0.0f, 0.0f, px, py);
            CHECK(texel(both, px, py)[0] == doctest::Approx(texel(alone, px, py)[0]).epsilon(0.03));

            // Rays meet what the picture shows, where it shows it: each pixel is the red square, the green one or
            // neither in both.
            const de::core::Array<std::byte> drawn = render(SceneView::BaseColor);
            uint32_t differing = 0;
            uint32_t red = 0;
            uint32_t green = 0;
            for (uint32_t y = 0; y < kSize; ++y) {
                for (uint32_t x = 0; x < kSize; ++x) {
                    const uint8_t* ray = texel(both, x, y);
                    const uint8_t* raster = texel(drawn, x, y);
                    differing += (reddish(ray) != reddish(raster) || greenish(ray) != greenish(raster)) ? 1 : 0;
                    red += reddish(raster) ? 1 : 0;
                    green += greenish(raster) ? 1 : 0;
                }
            }
            CHECK(red > 1000); // about 39 pixels a side, less what the green one hides
            CHECK(green > 200);
            CHECK(differing < 24); // edges at most

            // The green one moved away to the left: its shadow goes with it, the same frame.
            frame.instances[1].transform =
                de::test::render_transform({0.5f, 0, 0, -1.0f, 0, 0.5f, 0, 0.25f, 0, 0, 0.5f, 1.0f});
            const de::core::Array<std::byte> moved = render(SceneView::Traced);
            CHECK(texel(moved, kSize / 2, kSize / 2)[0] == doctest::Approx(middle[0]).epsilon(0.03));
            pixel(-1.0f, 0.25f, 1.0f, px, py);
            CHECK(greenish(texel(moved, px, py)));

            // With no instance left, the meshes' bottom levels are let go (looked for now and then), and rays meet
            // nothing.
            frame.instances.clear();
            for (int i = 0; i < 130; ++i) {
                draw(SceneView::Traced);
            }
            CHECK(renderer->stats().trace.meshes == 0);
            CHECK(renderer->stats().trace.instances == 0);
            const de::core::Array<std::byte> empty = render(SceneView::Traced);
            CHECK_FALSE(reddish(texel(empty, kSize / 2, kSize / 2)));
            device->wait_idle();
            CHECK(device->stats().live_accelerations == 1); // the top level
            // Back, it's built again.
            frame.instances.push(
                {.transform = de::test::render_transform({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}), .mesh = square});
            draw(SceneView::Traced);
            draw(SceneView::Traced);
            const de::core::Array<std::byte> back = render(SceneView::Traced);
            CHECK(renderer->stats().trace.meshes == 1);
            CHECK(texel(back, kSize / 2, kSize / 2)[0] == doctest::Approx(middle[0]).epsilon(0.03));
        }

        device->wait_idle();
        assets->release(square);
        assets->release(cover);
        assets->update();
        device->wait_idle();
    }
    device->wait_idle();
    CHECK(device->stats().live_accelerations == 0);
    de::test::check_clean(*device);
}

TEST_CASE("benchmark: what rays cost the GPU at 2560 x 1440, a top level over 64 to 32761 instances and a ray a "
          "pixel with its shadow ray") {
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    if (!device->capabilities().ray_query) {
        MESSAGE("this GPU has no ray queries: nothing to measure");
        return;
    }
    const Sandbox sandbox;
    // A wavy patch of ground 4 m a side, 4,608 triangles.
    std::vector<de::test::Vertex> vertices;
    std::vector<uint32_t> indices;
    de::test::grid(48, vertices, indices);
    de::test::write_src(sandbox, "content/props/patch.gltf",
                        de::test::gltf(vertices, indices, 0.0f, R"([{"name": "paint"}])", "[]", "[]"));
    AssetId patch_id;
    {
        de::platform::Vfs cook_vfs = de::test::cooker_vfs(sandbox);
        REQUIRE(de::test::run_cook(cook_vfs).ok());
        de::asset::Guid guid;
        REQUIRE(de::editor::source_asset_id(cook_vfs, "src:/content/props/patch.gltf", guid, patch_id));
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
                                                           .ray_tracing = true,
                                                           .max_instances = 65536});
        REQUIRE(renderer.ok());
        const de::asset::AssetHandle patch = assets->load<de::asset::MeshAsset>(patch_id);
        assets->wait_all();
        gpu_assets->update();
        uploader->flush_and_wait();
        gpu_assets->update();
        REQUIRE(gpu_assets->mesh(patch) != nullptr);

        constexpr uint32_t kWidth = 2560;
        constexpr uint32_t kHeight = 1440;
        const auto target =
            device->create_texture({.width = kWidth,
                                    .height = kHeight,
                                    .format = de::rhi::Format::RGBA8Unorm,
                                    .usage = de::rhi::TextureUsage::RenderTarget | de::rhi::TextureUsage::CopySrc,
                                    .debug_name = "scene target"});
        REQUIRE(target.ok());
        auto graph = de::render::RenderGraph::create(*device);
        REQUIRE(graph.ok());
        de::world::RenderFrame frame;
        frame.all_instances = true;
        const de::render::SceneLighting light{.sun_direction = {0.5f, 0.7071f, 0.5f}};
        const auto draw = [&] {
            gpu_assets->update();
            device->begin_frame();
            uploader->record();
            graph->begin(kWidth, kHeight);
            const de::render::GraphTexture color = graph->import_texture(
                {.texture = *target, .name = "target", .final_state = de::rhi::ResourceState::CopySrc});
            renderer->add_passes(*graph, color, de::rhi::Format::RGBA8Unorm, frame, light, SceneView::Traced);
            graph->execute();
            device->end_frame();
        };
        // The mean GPU time of the passes over some frames, once settled.
        struct Cost {
            float instances_ms = 0.0f;
            float top_level_ms = 0.0f;
            float view_ms = 0.0f;
        };
        // side x side patches as a floor, seen from straight above, from where it just fills the picture.
        const auto measure = [&](uint32_t side) {
            frame.instances.clear();
            for (uint32_t z = 0; z < side; ++z) {
                for (uint32_t x = 0; x < side; ++x) {
                    frame.instances.push(
                        {.transform = de::test::render_transform({1, 0, 0, static_cast<float>(x) * 4.0f, 0, 1, 0, 0, 0,
                                                                  0, 1, static_cast<float>(z) * 4.0f}),
                         .mesh = patch});
                }
            }
            const float half_m = static_cast<float>(side) * 2.0f;
            const float aspect = static_cast<float>(kWidth) / static_cast<float>(kHeight);
            const float height_m = 0.95f * half_m / (aspect * std::tan(0.5f));
            // Looking down: its right is +X, its up is -Z, and it looks along -Y.
            frame.camera = {.transform = {1, 0, 0, half_m, 0, 0, 1, height_m, 0, -1, 0, half_m},
                            .vertical_fov_rad = 1.0f,
                            .valid = true};
            for (int i = 0; i < 40; ++i) {
                draw();
            }
            Cost sum;
            constexpr int kFrames = 80;
            for (int i = 0; i < kFrames; ++i) {
                draw();
                for (const de::render::PassGpuTime& pass : graph->gpu_times()) {
                    if (pass.name.view() == "trace instances") {
                        sum.instances_ms += pass.ms / static_cast<float>(kFrames);
                    } else if (pass.name.view() == "trace top level") {
                        sum.top_level_ms += pass.ms / static_cast<float>(kFrames);
                    } else if (pass.name.view() == "trace view") {
                        sum.view_ms += pass.ms / static_cast<float>(kFrames);
                    }
                }
            }
            return sum;
        };
        const Cost few = measure(8);
        CHECK(renderer->stats().trace.instances == 64);
        CHECK(renderer->stats().trace.meshes == 1);
        const uint64_t triangles = renderer->stats().trace.triangles;
        CHECK(triangles > 0);
        CHECK(triangles <= 48 * 48 * 2);
        const Cost some = measure(64);
        CHECK(renderer->stats().trace.instances == 4096);
        const Cost many = measure(181);
        CHECK(renderer->stats().trace.instances == 181 * 181);
        MESSAGE("rays at 2560 x 1440, a patch of ", triangles,
                " triangles to a ray; instances, top level, a ray a "
                "pixel and its shadow ray, in ms: 64 instances ",
                few.instances_ms, " ", few.top_level_ms, " ", few.view_ms, "; 4096 ", some.instances_ms, " ",
                some.top_level_ms, " ", some.view_ms, "; 32761 ", many.instances_ms, " ", many.top_level_ms, " ",
                many.view_ms);
        // Loose limits, on a GPU that may be shared: what would say something is badly wrong.
        CHECK(few.top_level_ms < 2.0f);
        CHECK(many.top_level_ms < 20.0f);
        CHECK(many.view_ms < 50.0f);

        device->wait_idle();
        assets->release(patch);
        assets->update();
        device->wait_idle();
    }
    device->wait_idle();
    de::test::check_clean(*device);
}
