#include "asset/material_format.h"
#include "asset/mesh_format.h"
#include "asset/registry.h"
#include "cooker/material_cooker.h"
#include "cooker/mesh_cooker.h"
#include "cooker/texture_cooker.h"
#include "core/job_system.h"
#include "render/graph/render_graph.h"
#include "render/rhi/device.h"
#include "render/rhi/upload.h"
#include "render/scene/gpu_assets.h"
#include "render/scene/scene_renderer.h"
#include "render/shader/shader_compiler.h"
#include "render/shader/shader_library.h"
#include "support/gpu.h"
#include "support/readback.h"
#include "support/sandbox.h"
#include "world/render_extract.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

using de::asset::AssetId;
using de::asset::AssetType;

namespace {

constexpr AssetId kQuad = AssetId::from_value(0x51);
constexpr AssetId kRed = AssetId::from_value(0x52);
constexpr uint32_t kSize = 64;

// A 2 x 2 m square in the XY plane, facing +Z, with one red material.
void put_assets(const de::platform::Vfs& vfs) {
    de::cooker::ModelData model;
    de::cooker::ModelPrimitive& primitive = model.primitives.emplace();
    primitive.material = 0;
    const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (const auto& corner : corners) {
        de::cooker::ModelVertex& vertex = primitive.vertices.emplace();
        vertex.position[0] = corner[0];
        vertex.position[1] = corner[1];
        vertex.normal[1] = 0.0f;
        vertex.normal[2] = 1.0f;
        vertex.uv[0] = corner[0] * 0.5f + 0.5f;
        vertex.uv[1] = 0.5f - corner[1] * 0.5f;
    }
    for (const uint32_t index : {0U, 1U, 2U, 0U, 2U, 3U}) {
        primitive.indices.push(index);
    }
    static_cast<void>(model.materials.emplace());
    de::cooker::Diagnostics diagnostics;
    const AssetId materials[] = {kRed};
    const auto mesh = de::cooker::cook_mesh(model, materials, {.lod_count = 1}, kQuad, "test", diagnostics);
    REQUIRE(mesh.ok());
    REQUIRE(vfs.write_file(de::asset::cooked_path(kQuad, AssetType::Mesh).c_str(), mesh->span()).ok());
    de::cooker::MaterialBuild red;
    red.set_param(de::asset::material::kBaseColor, 1.0f, 0.0f, 0.0f, 1.0f);
    const auto cooked = de::cooker::cook_material(red, kRed);
    REQUIRE(vfs.write_file(de::asset::cooked_path(kRed, AssetType::Material).c_str(), cooked.span()).ok());
}

// A mesh of `parts` squares side by side, each with a material of its own sampling a texture of its
// own (with a full mip chain): everything loaded at once.
constexpr AssetId kMany = AssetId::from_value(0x60);

void put_many(const de::platform::Vfs& vfs, uint32_t parts) {
    de::cooker::ModelData model;
    de::core::Array<AssetId> materials;
    for (uint32_t p = 0; p < parts; ++p) {
        const AssetId texture_id = AssetId::from_value(0x100 + p);
        const AssetId material_id = AssetId::from_value(0x200 + p);
        de::cooker::Image image;
        image.width = 64;
        image.height = 64;
        for (uint32_t i = 0; i < 64 * 64; ++i) {
            for (const uint8_t channel :
                 {static_cast<uint8_t>(i), static_cast<uint8_t>(p * 40), uint8_t{90}, uint8_t{255}}) {
                image.rgba.push(channel);
            }
        }
        de::cooker::Diagnostics diagnostics;
        const auto texture = de::cooker::cook_texture(image, {}, texture_id, nullptr, "test", diagnostics);
        REQUIRE(texture.ok());
        REQUIRE(vfs.write_file(de::asset::cooked_path(texture_id, AssetType::Texture).c_str(), texture->span()).ok());
        de::cooker::MaterialBuild build;
        build.set_texture(de::asset::material::kBaseColorTexture, texture_id);
        const auto material = de::cooker::cook_material(build, material_id);
        REQUIRE(vfs.write_file(de::asset::cooked_path(material_id, AssetType::Material).c_str(), material.span()).ok());
        materials.push(material_id);

        de::cooker::ModelPrimitive& primitive = model.primitives.emplace();
        primitive.material = p;
        static_cast<void>(model.materials.emplace());
        for (uint32_t v = 0; v < 3; ++v) {
            de::cooker::ModelVertex& vertex = primitive.vertices.emplace();
            vertex.position[0] = static_cast<float>(p * 3 + v);
            vertex.position[1] = static_cast<float>(v % 2);
            primitive.indices.push(v);
        }
    }
    de::cooker::Diagnostics diagnostics;
    const auto mesh = de::cooker::cook_mesh(model, materials.span(), {.lod_count = 1}, kMany, "test", diagnostics);
    REQUIRE(mesh.ok());
    REQUIRE(vfs.write_file(de::asset::cooked_path(kMany, AssetType::Mesh).c_str(), mesh->span()).ok());
}

const uint8_t* texel(const de::core::Array<std::byte>& pixels, uint32_t x, uint32_t y) {
    return reinterpret_cast<const uint8_t*>(pixels.data()) + (size_t{y} * kSize + x) * 4;
}

} // namespace

TEST_CASE("the view-projection looks down the camera's -Z with reverse-Z depth") {
    // A camera at (0, 0, 5) with no rotation: a point 1 m ahead of the near plane...
    const float camera[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 5};
    float rows[16] = {};
    de::render::camera_view_proj(camera, 1.0f, 0.5f, 2.0f, rows);
    const auto clip = [&](float x, float y, float z, int row) {
        return rows[row * 4 + 0] * x + rows[row * 4 + 1] * y + rows[row * 4 + 2] * z + rows[row * 4 + 3];
    };
    // ...the origin, 5 m ahead: centered, depth near / distance.
    CHECK(clip(0, 0, 0, 0) == doctest::Approx(0.0f));
    CHECK(clip(0, 0, 0, 2) / clip(0, 0, 0, 3) == doctest::Approx(0.5f / 5.0f));
    // On the near plane: depth 1. Far away: toward 0.
    CHECK(clip(0, 0, 4.5f, 2) / clip(0, 0, 4.5f, 3) == doctest::Approx(1.0f));
    CHECK(clip(0, 0, -1e6f, 2) / clip(0, 0, -1e6f, 3) < 1e-5f);
    // Behind the camera: w <= 0 (clipped).
    CHECK(clip(0, 0, 6, 3) < 0.0f);
    // The vertical field of view: a point at tan(fov/2) * distance up lands on the top edge.
    const float top = std::tan(0.5f) * 5.0f;
    CHECK(clip(0, top, 0, 1) / clip(0, top, 0, 3) == doctest::Approx(1.0f));
    // The aspect ratio squeezes x.
    CHECK(clip(top * 2.0f, 0, 0, 0) / clip(top * 2.0f, 0, 0, 3) == doctest::Approx(1.0f));

    // Its inverse takes clip space back: rows x inverse is the identity.
    float inverse[16] = {};
    REQUIRE(de::render::invert_4x4(rows, inverse));
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += rows[r * 4 + k] * inverse[k * 4 + c];
            }
            CHECK(sum == doctest::Approx(r == c ? 1.0f : 0.0f).epsilon(1e-4));
        }
    }
    const float singular[16] = {1, 2, 3, 4, 2, 4, 6, 8, 0, 0, 1, 0, 0, 0, 0, 1};
    CHECK_FALSE(de::render::invert_4x4(singular, inverse));
}

TEST_CASE("loaded meshes and materials reach the GPU, draw where the camera looks, and leave when unloaded") {
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    const de::test::Sandbox sandbox;
    de::core::JobSystem jobs({.worker_count = 2});
    const de::platform::PathString game_root = sandbox.game();
    const de::platform::PathString user_root = sandbox.folder("user");
    const de::platform::PathString temp_root = sandbox.folder("temp");
    auto vfs = de::platform::Vfs::create({.game_root = game_root.c_str(),
                                          .user_root = user_root.c_str(),
                                          .temp_root = temp_root.c_str(),
                                          .src_root = DE_SOURCE_DIR,
                                          .jobs = &jobs,
                                          .game_writable = true});
    REQUIRE(vfs.ok());
    put_assets(*vfs);
    auto compiler = de::shader::ShaderCompiler::create(*vfs);
    REQUIRE(compiler.ok());
    auto shaders = de::shader::ShaderLibrary::create(*device, *vfs, jobs, *compiler, {.hot_reload = false});
    REQUIRE(shaders.ok());
    auto uploader = de::rhi::Uploader::create(*device);
    REQUIRE(uploader.ok());
    auto assets = de::asset::AssetRegistry::create({.vfs = &*vfs, .jobs = &jobs, .hot_reload = false});
    REQUIRE(assets.ok());
    {
        auto gpu_assets =
            de::render::GpuAssets::create({.device = &*device, .uploader = &*uploader, .assets = &*assets});
        REQUIRE(gpu_assets.ok());
        auto renderer =
            de::render::SceneRenderer::create({.device = &*device, .shaders = &*shaders, .gpu_assets = &*gpu_assets});
        REQUIRE(renderer.ok());

        const de::asset::AssetHandle mesh = assets->load<de::asset::MeshAsset>(kQuad);
        assets->wait_all();
        gpu_assets->update();
        CHECK(gpu_assets->mesh(mesh) == nullptr); // its upload isn't recorded yet
        uploader->flush_and_wait();
        gpu_assets->update();
        const de::render::GpuMesh* gpu_mesh = gpu_assets->mesh(mesh);
        REQUIRE(gpu_mesh != nullptr);
        REQUIRE(gpu_mesh->parts.size() == 1);
        CHECK(gpu_mesh->parts[0].material != 0); // its own material, not the default
        CHECK(gpu_assets->stats().materials == 1);

        // The square seen from 3 m in front, filling the middle of a 64 x 64 target.
        de::world::RenderFrame frame;
        frame.camera = {.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 3}, .vertical_fov_rad = 1.0f, .valid = true};
        frame.instances.push({.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}, .mesh = mesh});
        const auto target =
            device->create_texture({.width = kSize,
                                    .height = kSize,
                                    .format = de::rhi::Format::RGBA8Unorm,
                                    .usage = de::rhi::TextureUsage::RenderTarget | de::rhi::TextureUsage::CopySrc,
                                    .debug_name = "scene target"});
        REQUIRE(target.ok());
        auto graph = de::render::RenderGraph::create(*device);
        REQUIRE(graph.ok());
        const auto render = [&](de::render::SceneView view) {
            device->begin_frame();
            uploader->record(); // as the engine does each frame: the material's table entry lands first
            graph->begin(kSize, kSize);
            const de::render::GraphTexture color = graph->import_texture(
                {.texture = *target, .name = "target", .final_state = de::rhi::ResourceState::CopySrc});
            const de::render::SceneLighting light{.sun_direction = {0, 0, 1}};
            renderer->add_passes(*graph, color, de::rhi::Format::RGBA8Unorm, frame, light, view);
            graph->execute();
            device->end_frame();
            return de::test::read_texture(*device, *target);
        };
        // The G-buffer, as its debug views show it: the square's base color, and its normal (+Z).
        const de::core::Array<std::byte> pixels = render(de::render::SceneView::BaseColor);
        CHECK(renderer->stats().drawn == 1);
        CHECK(renderer->stats().draws == 1);
        const de::core::Array<std::byte> normals = render(de::render::SceneView::Normal);
        const uint8_t* normal = texel(normals, kSize / 2, kSize / 2);
        CHECK(normal[0] == doctest::Approx(128).epsilon(0.01));
        CHECK(normal[1] == doctest::Approx(128).epsilon(0.01));
        CHECK(normal[2] == 255);
        // Lit: the sun straight at it (and behind the camera: a white specular highlight on top),
        // so it's bright and clearly red; the sky around it isn't.
        const de::core::Array<std::byte> lit = render(de::render::SceneView::Lit);
        const uint8_t* lit_center = texel(lit, kSize / 2, kSize / 2);
        CHECK(lit_center[0] > 200);
        CHECK(lit_center[0] > 2 * lit_center[1]);
        CHECK(lit_center[0] > 2 * lit_center[2]);
        const uint8_t* lit_sky = texel(lit, 1, 1);
        CHECK(lit_sky[2] > lit_sky[0]); // bluish
        // Nothing to look through: cleared to the empty color (0.10, 0.11, 0.15 by default).
        frame.camera.valid = false;
        const de::core::Array<std::byte> cleared = render(de::render::SceneView::Lit);
        const uint8_t* empty = texel(cleared, kSize / 2, kSize / 2);
        CHECK(empty[0] == doctest::Approx(26).epsilon(0.05));
        CHECK(empty[1] == doctest::Approx(28).epsilon(0.05));
        CHECK(empty[2] == doctest::Approx(38).epsilon(0.05));
        frame.camera.valid = true;
        const uint8_t* center = texel(pixels, kSize / 2, kSize / 2);
        CHECK(center[0] == 255);
        CHECK(center[1] == 0);
        CHECK(center[2] == 0);
        const uint8_t* corner = texel(pixels, 1, 1);
        CHECK(corner[0] == 0); // nothing drawn: a cleared G-buffer
        // The square's edge: 1 m at 3 m with a 1 rad field of view is about 0.61 of the half height.
        const uint8_t* outside = texel(pixels, kSize / 2, kSize / 2 - 24);
        CHECK(outside[0] == 0);
        const uint8_t* inside = texel(pixels, kSize / 2, kSize / 2 - 16);
        CHECK(inside[0] == 255);

        // Unloaded: its GPU copy goes with it.
        assets->release(mesh);
        assets->update();
        CHECK(gpu_assets->mesh(mesh) == nullptr);
        CHECK(gpu_assets->stats().meshes == 0);
        CHECK(gpu_assets->stats().materials == 0);
        device->wait_idle();
        device->destroy(*target);
    }
    device->wait_idle();
    de::test::check_clean(*device);
}

TEST_CASE("a mesh with many materials and mipped textures is ready within a few frames") {
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    const de::test::Sandbox sandbox;
    de::core::JobSystem jobs({.worker_count = 2});
    de::platform::Vfs vfs = sandbox.make_vfs({.jobs = &jobs, .game_writable = true});
    constexpr uint32_t kParts = 8;
    put_many(vfs, kParts);
    auto uploader = de::rhi::Uploader::create(*device);
    REQUIRE(uploader.ok());
    auto assets = de::asset::AssetRegistry::create({.vfs = &vfs, .jobs = &jobs, .hot_reload = false});
    REQUIRE(assets.ok());
    {
        auto gpu_assets =
            de::render::GpuAssets::create({.device = &*device, .uploader = &*uploader, .assets = &*assets});
        REQUIRE(gpu_assets.ok());
        const de::asset::AssetHandle mesh = assets->load<de::asset::MeshAsset>(kMany);
        assets->wait_all();
        // The engine's frame: registry and GPU assets update, then the uploads are recorded. One
        // upload per texture and per frame for the table: 8 textures of 7 mips and 8 materials take
        // a few frames, not one per mip and material.
        int frames = 0;
        while (frames < 4 && (gpu_assets->mesh(mesh) == nullptr || gpu_assets->stats().pending != 0)) {
            gpu_assets->update();
            device->begin_frame();
            uploader->record();
            device->end_frame();
            ++frames;
        }
        gpu_assets->update();
        CHECK(gpu_assets->mesh(mesh) != nullptr);
        CHECK(gpu_assets->stats().pending == 0);
        CHECK(gpu_assets->stats().textures == kParts);
        CHECK(uploader->idle());
        assets->release(mesh);
        assets->update();
        device->wait_idle();
    }
    device->wait_idle();
    de::test::check_clean(*device);
}
