#include "asset/mesh_format.h"
#include "asset/registry.h"
#include "core/job_system.h"
#include "editor/source_files.h"
#include "render/graph/render_graph.h"
#include "render/rhi/device.h"
#include "render/rhi/upload.h"
#include "render/scene/gpu_assets.h"
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

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

using de::asset::AssetId;
using de::render::SceneView;
using de::test::Sandbox;

namespace {

constexpr uint32_t kSize = 64;

int middle_red(const de::core::Array<std::byte>& pixels) {
    return int{reinterpret_cast<const uint8_t*>(pixels.data())[(((size_t{kSize / 2} * kSize) + (kSize / 2)) * 4)]};
}

} // namespace

TEST_CASE("auto exposure: a wall lit a hundred times less comes out about as bright, and the eye takes its time") {
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    const Sandbox sandbox;
    std::vector<de::test::Vertex> vertices;
    std::vector<uint32_t> indices;
    de::test::quad(vertices, indices);
    de::test::write_src(
        sandbox, "content/props/stone.gltf",
        de::test::gltf(vertices, indices, 0.0f,
                       R"([{"name": "stone", "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.5, 0.5, 1],
                           "metallicFactor": 0, "roughnessFactor": 1}}])",
                       "[]", "[]"));
    AssetId stone_id;
    {
        de::platform::Vfs cook_vfs = de::test::cooker_vfs(sandbox);
        REQUIRE(de::test::run_cook(cook_vfs).ok());
        de::asset::Guid guid;
        REQUIRE(de::editor::source_asset_id(cook_vfs, "src:/content/props/stone.gltf", guid, stone_id));
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
        auto renderer =
            de::render::SceneRenderer::create({.device = &*device, .shaders = &*shaders, .gpu_assets = &*gpu_assets});
        REQUIRE(renderer.ok());
        const de::asset::AssetHandle stone = assets->load<de::asset::MeshAsset>(stone_id);
        assets->wait_all();
        gpu_assets->update();
        uploader->flush_and_wait();
        gpu_assets->update();
        REQUIRE(gpu_assets->mesh(stone) != nullptr);

        const auto target =
            device->create_texture({.width = kSize,
                                    .height = kSize,
                                    .format = de::rhi::Format::RGBA8Unorm,
                                    .usage = de::rhi::TextureUsage::RenderTarget | de::rhi::TextureUsage::CopySrc,
                                    .debug_name = "scene target"});
        REQUIRE(target.ok());
        auto graph = de::render::RenderGraph::create(*device);
        REQUIRE(graph.ok());
        // The wall filling the picture, 80 cm off; lit by an even ambient alone.
        de::world::RenderFrame frame;
        frame.all_instances = true;
        frame.camera = {.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0.8f}, .vertical_fov_rad = 1.0f, .valid = true};
        frame.instances.push(
            {.transform = de::test::render_transform({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}), .mesh = stone});
        de::render::SceneLighting light;
        const auto ambient = [&](float level) {
            for (int channel = 0; channel < 3; ++channel) {
                light.sun_color[channel] = 0.0f;
                light.sky_color[channel] = level;
                light.ground_color[channel] = level;
            }
        };
        const auto render = [&](int frames) {
            for (int i = 0; i < frames; ++i) {
                gpu_assets->update();
                device->begin_frame();
                uploader->record();
                graph->begin(kSize, kSize);
                const de::render::GraphTexture color = graph->import_texture(
                    {.texture = *target, .name = "target", .final_state = de::rhi::ResourceState::CopySrc});
                renderer->add_passes(*graph, color, de::rhi::Format::RGBA8Unorm, frame, light, SceneView::Lit);
                graph->execute();
                device->end_frame();
            }
            return middle_red(de::test::read_texture(*device, *target));
        };
        const auto exposure_now = [&] {
            de::render::GpuExposureState state;
            REQUIRE(renderer->auto_exposure_state(state));
            return state;
        };

        // The eye at once, for this part: what it settles on, not how it gets there.
        de::render::AutoExposureSettings instant;
        instant.brighten_per_s = 1e6f;
        instant.darken_per_s = 1e6f;
        instant.adaptation = 1.0f; // all the way: the two come out the same
        renderer->set_auto_exposure_settings(instant);
        renderer->set_auto_exposure(true);
        ambient(1.0f);
        const int bright = render(8);
        const float bright_log2 = exposure_now().log2_exposure;
        ambient(0.01f);
        const int dim = render(8);
        const float dim_log2 = exposure_now().log2_exposure;
        MESSAGE("a wall with an ambient of 1 shows ", bright, ", of 0.01 ", dim, "; the exposure went from 2^",
                bright_log2, " to 2^", dim_log2);
        // Both middle grey on the screen: 0.18 through the ACES curve is 0.267, 68 of 255 on this linear target. The
        // exposure made up the 6.6 stops between them.
        CHECK(bright == doctest::Approx(68).epsilon(0.1));
        CHECK(dim == doctest::Approx(bright).epsilon(0.1));
        CHECK(dim_log2 - bright_log2 == doctest::Approx(std::log2(100.0f)).epsilon(0.05));
        // The same wall seen from farther, small in a picture that's mostly black (nothing there, no sky): black says
        // nothing of how bright the scene is, so the wall gets the same exposure, and isn't stops too bright.
        frame.camera.transform[11] = 4.0f;
        ambient(1.0f);
        static_cast<void>(render(8));
        const float small_log2 = exposure_now().log2_exposure;
        MESSAGE("the wall small in a black picture: the exposure 2^", small_log2);
        CHECK(std::abs(small_log2 - bright_log2) < 0.3f);
        frame.camera.transform[11] = 0.8f;
        ambient(0.01f);
        // As an eye adapts, only part of the way from the environment's exposure (0 here): the dim wall comes out
        // darker than the bright one, by the share it doesn't make up.
        de::render::AutoExposureSettings partway = instant;
        partway.adaptation = 0.7f;
        renderer->set_auto_exposure_settings(partway);
        const int dim_partway = render(8);
        const float partway_log2 = exposure_now().log2_exposure;
        CHECK(partway_log2 == doctest::Approx(0.7f * dim_log2).epsilon(0.05));
        CHECK(dim_partway < dim - 15);
        CHECK(dim_partway > 5);
        // Without it, the environment's exposure: the dim wall is next to black.
        renderer->set_auto_exposure(false);
        CHECK(render(2) < bright / 10);

        // The eye's own pace: turned on again it starts where it's going; then the light comes back a hundred times
        // over, and a frame later (a few milliseconds) it has hardly begun to close.
        const de::render::AutoExposureSettings eye;
        renderer->set_auto_exposure_settings(eye);
        renderer->set_auto_exposure(true);
        static_cast<void>(render(4));
        ambient(1.0f);
        static_cast<void>(render(1 + static_cast<int>(de::rhi::kFramesInFlight)));
        const de::render::GpuExposureState state = exposure_now();
        CHECK(state.target_log2 == doctest::Approx(eye.adaptation * bright_log2).epsilon(0.05));
        CHECK(state.log2_exposure > state.target_log2 + 3.0f); // most of the way still to go

        device->wait_idle();
        device->destroy(*target);
        assets->release(stone);
        assets->update();
        device->wait_idle();
    }
    device->wait_idle();
    de::test::check_clean(*device);
}
