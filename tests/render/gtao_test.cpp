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

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

using de::asset::AssetId;
using de::render::SceneView;
using de::test::Sandbox;

namespace {

constexpr uint32_t kSize = 128;
constexpr float kFovRad = 1.0f;

// A camera at eye looking at a point, its rows as RenderView has them (columns: right, up, back; then where).
struct Camera {
    float rows[12] = {};
    float eye[3] = {};
    float right[3] = {};
    float up[3] = {};
    float back[3] = {};
};

Camera look_at(const float (&eye)[3], const float (&at)[3]) {
    Camera camera;
    float back[3] = {eye[0] - at[0], eye[1] - at[1], eye[2] - at[2]};
    const float length = std::sqrt((back[0] * back[0]) + (back[1] * back[1]) + (back[2] * back[2]));
    for (float& axis : back) {
        axis /= length;
    }
    // Right: the world's up across back; up: back across right.
    float right[3] = {back[2], 0.0f, -back[0]};
    const float right_length = std::sqrt((right[0] * right[0]) + (right[2] * right[2]));
    right[0] /= right_length;
    right[2] /= right_length;
    const float up[3] = {(back[1] * right[2]) - (back[2] * right[1]), (back[2] * right[0]) - (back[0] * right[2]),
                         (back[0] * right[1]) - (back[1] * right[0])};
    for (int i = 0; i < 3; ++i) {
        camera.rows[(i * 4) + 0] = right[i];
        camera.rows[(i * 4) + 1] = up[i];
        camera.rows[(i * 4) + 2] = back[i];
        camera.rows[(i * 4) + 3] = eye[i];
        camera.eye[i] = eye[i];
        camera.right[i] = right[i];
        camera.up[i] = up[i];
        camera.back[i] = back[i];
    }
    return camera;
}

// Where a point lands in the picture.
void pixel_of(const Camera& camera, const float (&point)[3], uint32_t& px, uint32_t& py) {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float d = point[i] - camera.eye[i];
        x += d * camera.right[i];
        y += d * camera.up[i];
        z -= d * camera.back[i];
    }
    const float half = std::tan(kFovRad * 0.5f);
    px = static_cast<uint32_t>(((x / (z * half)) * 0.5f + 0.5f) * static_cast<float>(kSize));
    py = static_cast<uint32_t>((0.5f - ((y / (z * half)) * 0.5f)) * static_cast<float>(kSize));
}

int red(const de::core::Array<std::byte>& pixels, uint32_t x, uint32_t y) {
    return int{reinterpret_cast<const uint8_t*>(pixels.data())[(((size_t{y} * kSize) + x) * 4)]};
}

} // namespace

TEST_CASE("ambient occlusion: open ground is open, a crease is shut by the cosine, a plane seen at a slant doesn't "
          "shut itself, and only the ambient light is darkened") {
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
                           "metallicFactor": 0}}])",
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
        de::world::RenderFrame frame;
        frame.all_instances = true;
        de::render::SceneLighting light;
        // A few frames: the passes' shaders build in the background, and are there from a frame or two on.
        const auto render = [&](SceneView view) {
            for (int i = 0; i < 4; ++i) {
                gpu_assets->update();
                device->begin_frame();
                uploader->record();
                graph->begin(kSize, kSize);
                const de::render::GraphTexture color = graph->import_texture(
                    {.texture = *target, .name = "target", .final_state = de::rhi::ResourceState::CopySrc});
                renderer->add_passes(*graph, color, de::rhi::Format::RGBA8Unorm, frame, light, view);
                graph->execute();
                device->end_frame();
                shaders->update();
            }
            return de::test::read_texture(*device, *target);
        };
        const auto place = [&](const Camera& camera) {
            frame.camera = {.vertical_fov_rad = kFovRad, .valid = true};
            std::copy_n(camera.rows, 12, frame.camera.transform);
        };
        // A floor 8 m square facing up, at the world's middle.
        frame.instances.push(
            {.transform = de::test::render_transform({4, 0, 0, 0, 0, 0, 4, 0, 0, -4, 0, 0}), .mesh = stone});

        // The open floor, from 3 m up and 4 m back: nothing near it, nothing shut.
        const Camera camera = look_at({0.0f, 3.0f, 4.0f}, {0.0f, 0.0f, 0.0f});
        place(camera);
        const de::core::Array<std::byte> open = render(SceneView::AmbientOcclusion);
        CHECK(red(open, kSize / 2, kSize / 2) > 245);
        // At a slant of 6 degrees, the floor's own depth steps away fast across the picture: still nothing shut (an
        // occlusion that samples a sphere round the point, as the first ones did, finds the floor in half of it).
        place(look_at({0.0f, 0.6f, 6.0f}, {0.0f, 0.0f, 0.0f}));
        const de::core::Array<std::byte> slant = render(SceneView::AmbientOcclusion);
        CHECK(red(slant, kSize / 2, kSize / 2) > 240);

        // A wall 4 m square standing on the floor along z = -0.5, facing the camera: a crease.
        frame.instances.push(
            {.transform = de::test::render_transform({2, 0, 0, 0, 0, 2, 0, 2, 0, 0, 2, -0.5f}), .mesh = stone});
        place(camera);
        const de::core::Array<std::byte> crease = render(SceneView::AmbientOcclusion);
        uint32_t px = 0;
        uint32_t py = 0;
        pixel_of(camera, {0.0f, 0.0f, -0.45f}, px, py); // 5 cm from the crease
        const int at_crease = red(crease, px, py);
        pixel_of(camera, {0.0f, 0.0f, 0.0f}, px, py); // 50 cm from it
        const int near_crease = red(crease, px, py);
        pixel_of(camera, {0.0f, 0.0f, 2.0f}, px, py); // 2.5 m from it: past the radius
        const int far_from = red(crease, px, py);
        pixel_of(camera, {0.0f, 0.3f, -0.5f}, px, py); // on the wall, 30 cm up
        const int wall_low = red(crease, px, py);
        pixel_of(camera, {0.0f, 0.0f, -0.2f}, px, py); // on the floor, 30 cm out
        const int floor_low = red(crease, px, py);
        MESSAGE("occlusion by a crease, of 255: 5 cm from it ", at_crease, ", 30 cm ", floor_low, ", 50 cm ",
                near_crease, ", 2.5 m ", far_from, "; on the wall 30 cm up ", wall_low);
        // The floor beside a wall sees half its sky gone at the crease: by the cosine, half the light (the wall
        // takes the horizon's half, which counts less: the integral gives 0.5 for a right angle, at the line).
        CHECK(at_crease < 175);
        CHECK(at_crease > 90);
        CHECK(near_crease > at_crease);
        CHECK(far_from > 245);
        // The crease is a right angle: 30 cm up the wall is as shut as 30 cm out on the floor. Near enough: on the
        // screen the one is seen face on and the other at a slant, so the radius covers other pixels in each.
        CHECK(wall_low < 250);
        CHECK(wall_low == doctest::Approx(floor_low).epsilon(0.05));

        // In the lit picture: with only an ambient, the crease is darker with it than without; with only the sun,
        // straight down, it's the same with or without (the sun's shadows are its own).
        pixel_of(camera, {0.0f, 0.0f, -0.4f}, px, py);
        for (int channel = 0; channel < 3; ++channel) {
            light.sun_color[channel] = 0.0f;
            light.sky_color[channel] = 1.0f;
            light.ground_color[channel] = 1.0f;
        }
        const int ambient_with = red(render(SceneView::Lit), px, py);
        renderer->set_ambient_occlusion(false);
        const int ambient_without = red(render(SceneView::Lit), px, py);
        CHECK(red(render(SceneView::AmbientOcclusion), px, py) == 255); // and the view shows none
        CHECK(ambient_with < ambient_without - 15);
        for (int channel = 0; channel < 3; ++channel) {
            light.sun_color[channel] = 3.0f;
            light.sky_color[channel] = 0.0f;
            light.ground_color[channel] = 0.0f;
        }
        light.sun_direction[0] = 0.0f;
        light.sun_direction[1] = 1.0f;
        light.sun_direction[2] = 0.0f;
        const int sun_without = red(render(SceneView::Lit), px, py);
        renderer->set_ambient_occlusion(true);
        const int sun_with = red(render(SceneView::Lit), px, py);
        CHECK(sun_without > 60);
        CHECK(sun_with == doctest::Approx(sun_without).epsilon(0.02));

        device->wait_idle();
        device->destroy(*target);
        assets->release(stone);
        assets->update();
        device->wait_idle();
    }
    device->wait_idle();
    de::test::check_clean(*device);
}
