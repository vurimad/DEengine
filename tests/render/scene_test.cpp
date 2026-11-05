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
#include "render/scene/fog_noise.h"
#include "render/scene/gpu_assets.h"
#include "render/scene/scene_renderer.h"
#include "render/shader/shader_compiler.h"
#include "render/shader/shader_library.h"
#include "support/gpu.h"
#include "support/readback.h"
#include "support/sandbox.h"
#include "world/render_extract.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

using de::asset::AssetId;
using de::asset::AssetType;

namespace {

constexpr AssetId kQuad = AssetId::from_value(0x51);
constexpr AssetId kRed = AssetId::from_value(0x52);
constexpr AssetId kWall = AssetId::from_value(0x53);
constexpr AssetId kDarkGreen = AssetId::from_value(0x54);
constexpr AssetId kPureGreen = AssetId::from_value(0x55); // no red or blue: no chroma on one axis
constexpr AssetId kPureGreenMaterial = AssetId::from_value(0x56);
constexpr AssetId kBad = AssetId::from_value(0x57); // emits NaN
constexpr AssetId kBadMaterial = AssetId::from_value(0x58);
constexpr uint32_t kSize = 64;

// A 2 x 2 m square in the XY plane, facing +Z, drawn with one material.
void put_quad(const de::platform::Vfs& vfs, AssetId mesh_id, AssetId material) {
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
    const AssetId materials[] = {material};
    const auto mesh = de::cooker::cook_mesh(model, materials, {.lod_count = 1}, mesh_id, "test", diagnostics);
    REQUIRE(mesh.ok());
    REQUIRE(vfs.write_file(de::asset::cooked_path(mesh_id, AssetType::Mesh).c_str(), mesh->span()).ok());
}

// The square with a red material, and again with a dark green one.
void put_assets(const de::platform::Vfs& vfs) {
    put_quad(vfs, kQuad, kRed);
    put_quad(vfs, kWall, kDarkGreen);
    de::cooker::MaterialBuild red;
    red.set_param(de::asset::material::kBaseColor, 1.0f, 0.0f, 0.0f, 1.0f);
    const auto cooked = de::cooker::cook_material(red, kRed);
    REQUIRE(vfs.write_file(de::asset::cooked_path(kRed, AssetType::Material).c_str(), cooked.span()).ok());
    de::cooker::MaterialBuild green;
    green.set_param(de::asset::material::kBaseColor, 0.0f, 0.2f, 0.0f, 1.0f);
    const auto cooked_green = de::cooker::cook_material(green, kDarkGreen);
    REQUIRE(vfs.write_file(de::asset::cooked_path(kDarkGreen, AssetType::Material).c_str(), cooked_green.span()).ok());
    put_quad(vfs, kPureGreen, kPureGreenMaterial);
    de::cooker::MaterialBuild pure_green;
    pure_green.set_param(de::asset::material::kBaseColor, 0.0f, 1.0f, 0.0f, 1.0f);
    const auto cooked_pure = de::cooker::cook_material(pure_green, kPureGreenMaterial);
    REQUIRE(vfs.write_file(de::asset::cooked_path(kPureGreenMaterial, AssetType::Material).c_str(), cooked_pure.span())
                .ok());
    put_quad(vfs, kBad, kBadMaterial);
    de::cooker::MaterialBuild bad;
    bad.set_param(de::asset::material::kBaseColor, 0.0f, 0.0f, 0.0f, 1.0f);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    bad.set_param(de::asset::material::kEmissive, nan, nan, nan, 1.0f);
    const auto cooked_bad = de::cooker::cook_material(bad, kBadMaterial);
    REQUIRE(vfs.write_file(de::asset::cooked_path(kBadMaterial, AssetType::Material).c_str(), cooked_bad.span()).ok());
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
        // The sun straight at the square (from behind the camera), and fog if asked for.
        const auto render = [&](de::render::SceneView view, const de::render::SceneFog& fog = {}) {
            device->begin_frame();
            uploader->record(); // as the engine does each frame: the material's table entry lands first
            graph->begin(kSize, kSize);
            const de::render::GraphTexture color = graph->import_texture(
                {.texture = *target, .name = "target", .final_state = de::rhi::ResourceState::CopySrc});
            de::render::SceneLighting light{.sun_direction = {0, 0, 1}};
            light.fog = fog;
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

        // Fog: the same view through a thick green fog (constant, 0.5 per meter: about a fifth of
        // the square's light gets through its 3 m), and the sky (as far as the fog reaches) behind it.
        de::render::SceneFog green{.density_per_m = 0.5f, .ambient = {0.0f, 2.0f, 0.0f}, .end_m = 1000.0f};
        const de::core::Array<std::byte> fogged = render(de::render::SceneView::Lit, green);
        const uint8_t* fogged_center = texel(fogged, kSize / 2, kSize / 2);
        CHECK(fogged_center[0] < lit_center[0] - 40); // dimmed
        CHECK(fogged_center[1] > fogged_center[0]);   // and green
        CHECK(texel(fogged, 1, 1)[1] > 200);          // the sky behind 1 km of it: all fog
        // Fog that starts past the square leaves it as it was (the sky behind it still fogged).
        green.start_m = 10.0f;
        const de::core::Array<std::byte> started = render(de::render::SceneView::Lit, green);
        CHECK(texel(started, kSize / 2, kSize / 2)[0] == doctest::Approx(lit_center[0]).epsilon(0.02));
        CHECK(texel(started, kSize / 2, kSize / 2)[1] == doctest::Approx(lit_center[1]).epsilon(0.05));
        CHECK(texel(started, 1, 1)[1] > 200);
        // Height fog far below the camera (its base 1 km down, thinning by e every 10 m up) is
        // gone where the camera looks; the same fog based at the camera's height is thick.
        const de::render::SceneFog low{.height_density_per_m = 0.5f,
                                       .height_falloff_per_m = 0.1f,
                                       .base_height_m = -1000.0,
                                       .ambient = {0.0f, 2.0f, 0.0f},
                                       .end_m = 1000.0f};
        const de::core::Array<std::byte> below = render(de::render::SceneView::Lit, low);
        CHECK(texel(below, kSize / 2, kSize / 2)[0] == doctest::Approx(lit_center[0]).epsilon(0.02));
        de::render::SceneFog level = low;
        level.base_height_m = 0.0;
        level.ambient_height_attenuation = 0.0f; // the fog column above (optical depth 5) would darken it
        const de::core::Array<std::byte> around = render(de::render::SceneView::Lit, level);
        CHECK(texel(around, kSize / 2, kSize / 2)[1] > texel(around, kSize / 2, kSize / 2)[0]);
        const uint32_t foggy_passes = graph->stats().passes;
        // Sun haze starts 5 m out: in fog this thick (0.5 per meter) the first 5 m scatter almost all
        // of the light the camera gets, so the sky behind it gets almost none of the sun's colour.
        de::render::SceneFog sunny{.density_per_m = 0.5f, .sun_back = {0.0f, 0.3f, 0.0f}, .end_m = 1000.0f};
        const de::core::Array<std::byte> haze = render(de::render::SceneView::Lit, sunny);
        CHECK(texel(haze, 1, 1)[1] > 180); // the sun lights the fog (looking away from it: sun_back)
        sunny.sun_start_m = 5.0f;
        const de::core::Array<std::byte> late = render(de::render::SceneView::Lit, sunny);
        CHECK(texel(late, 1, 1)[1] < 110); // about a twelfth of the light: e^-2.5 gets past the first 5 m
        // Far below the base of a thin fog (2 km down, falloff 1 per meter): twice its density, and
        // no precision lost to it: the square is barely touched, not brightened or broken.
        const de::render::SceneFog far_below{.height_density_per_m = 0.001f,
                                             .height_falloff_per_m = 1.0f,
                                             .base_height_m = 2000.0,
                                             .ambient = {0.0f, 2.0f, 0.0f},
                                             .end_m = 1000.0f};
        const de::core::Array<std::byte> deep = render(de::render::SceneView::Lit, far_below);
        CHECK(texel(deep, kSize / 2, kSize / 2)[0] == doctest::Approx(lit_center[0]).epsilon(0.03));
        CHECK(texel(deep, kSize / 2, kSize / 2)[1] == doctest::Approx(lit_center[1]).epsilon(0.05));
        // No fog, or a debug view: no fog pass.
        static_cast<void>(render(de::render::SceneView::Lit));
        CHECK(graph->stats().passes == foggy_passes - 1);
        static_cast<void>(render(de::render::SceneView::BaseColor, green));
        CHECK(graph->stats().passes == foggy_passes - 1);
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

TEST_CASE("shadow cascades split the view, grow outward, and don't change size as the camera turns") {
    const de::render::SceneShadows shadows;
    const float sun[3] = {0.0f, 0.70710678f, 0.70710678f};
    const float looking_ahead[12] = {1, 0, 0, 10, 0, 1, 0, 2, 0, 0, 1, 30};
    // The same camera turned 90 degrees about Y: x' = -z.
    const float turned[12] = {0, 0, 1, 10, 0, 1, 0, 2, -1, 0, 0, 30};
    de::render::ShadowCascade a[4];
    de::render::ShadowCascade b[4];
    de::render::fit_shadow_cascades(looking_ahead, 1.0f, 0.1f, 16.0f / 9.0f, sun, shadows, a);
    de::render::fit_shadow_cascades(turned, 1.0f, 0.1f, 16.0f / 9.0f, sun, shadows, b);
    for (int c = 0; c < 4; ++c) {
        CAPTURE(c);
        if (c > 0) {
            CHECK(a[c].far_m > a[c - 1].far_m);
            CHECK(a[c].texel_m > a[c - 1].texel_m); // farther cascades cover more, coarser
        }
        CHECK(a[c].texel_m == doctest::Approx(b[c].texel_m)); // the sphere doesn't depend on the turn
        CHECK(a[c].depth_per_m == doctest::Approx(b[c].depth_per_m));
    }
    CHECK(a[3].far_m == doctest::Approx(shadows.distance_m));
    // No sun (a zero direction): finite cascades all the same (straight down).
    const float none[3] = {0.0f, 0.0f, 0.0f};
    de::render::ShadowCascade night[de::render::kShadowCascadeCount];
    de::render::fit_shadow_cascades(looking_ahead, 1.0f, 0.1f, 16.0f / 9.0f, none, shadows, night);
    for (const float value : night[0].view_proj) {
        CHECK(std::isfinite(value));
    }
    // A point at the camera, a few meters ahead: inside cascade 0, depth in (0, 1), nearer the sun
    // (higher) is deeper (reverse Z).
    const auto project = [&](const de::render::ShadowCascade& cascade, float x, float y, float z, int row) {
        return cascade.view_proj[row * 4 + 0] * x + cascade.view_proj[row * 4 + 1] * y +
               cascade.view_proj[row * 4 + 2] * z + cascade.view_proj[row * 4 + 3];
    };
    const float ahead[3] = {10.0f, 2.0f, 27.0f};
    CHECK(std::abs(project(a[0], ahead[0], ahead[1], ahead[2], 0)) < 1.0f);
    CHECK(std::abs(project(a[0], ahead[0], ahead[1], ahead[2], 1)) < 1.0f);
    const float depth = project(a[0], ahead[0], ahead[1], ahead[2], 2);
    CHECK(depth > 0.0f);
    CHECK(depth < 1.0f);
    CHECK(project(a[0], ahead[0], ahead[1] + 1.0f, ahead[2] + 1.0f, 2) > depth);
    // Each cascade's center sits on its texel grid (no swimming as the camera moves): in the light's
    // right and up axes, a whole number of texels. A row's xyz is the axis over the radius.
    for (const de::render::ShadowCascade& cascade : a) {
        for (int row = 0; row < 2; ++row) {
            const float* r = &cascade.view_proj[static_cast<size_t>(row) * 4];
            const double radius = 1.0 / std::sqrt(double{r[0]} * r[0] + double{r[1]} * r[1] + double{r[2]} * r[2]);
            const double texels = -r[3] * radius / cascade.texel_m;
            CHECK(std::abs(texels - std::round(texels)) < 0.02);
        }
    }
}

TEST_CASE("the sun casts a shadow where an occluder blocks it, and lights the rest") {
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
        gpu_assets->update(); // queues the material table's upload
        uploader->flush_and_wait();
        gpu_assets->update();
        REQUIRE(gpu_assets->mesh(mesh) != nullptr);

        // A 2 m square at z = 0 facing the camera (at z = 3), and a 0.5 m one 1 m in front of it at
        // x = 0.6. The sun is up and to the right of the camera: (1, 0, 2) toward it, so the small
        // square's shadow lands 0.5 m to the left of it on the back square, at x in (-0.15, 0.35),
        // where the camera sees the back square itself (the occluder covers x > 0.5 from there).
        de::world::RenderFrame frame;
        frame.camera = {.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 3}, .vertical_fov_rad = 1.0f, .valid = true};
        frame.instances.push({.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}, .mesh = mesh});
        frame.instances.push({.transform = {0.25f, 0, 0, 0.6f, 0, 0.25f, 0, 0, 0, 0, 0.25f, 1}, .mesh = mesh});
        const auto target =
            device->create_texture({.width = kSize,
                                    .height = kSize,
                                    .format = de::rhi::Format::RGBA8Unorm,
                                    .usage = de::rhi::TextureUsage::RenderTarget | de::rhi::TextureUsage::CopySrc,
                                    .debug_name = "shadow target"});
        REQUIRE(target.ok());
        auto graph = de::render::RenderGraph::create(*device);
        REQUIRE(graph.ok());
        device->begin_frame();
        uploader->record();
        graph->begin(kSize, kSize);
        const de::render::GraphTexture color = graph->import_texture(
            {.texture = *target, .name = "target", .final_state = de::rhi::ResourceState::CopySrc});
        // Even 2 m cascades: the back square (3 m away) is in cascade 1, clear of its dithered band, so
        // the atlas tiles count.
        de::render::SceneLighting light{.sun_direction = {1, 0, 2}};
        light.shadows = {.resolution = 512, .distance_m = 8.0f, .split_lambda = 0.0f};
        renderer->add_passes(*graph, color, de::rhi::Format::RGBA8Unorm, frame, light,
                             de::render::SceneView::ShadowMask);
        graph->execute();
        device->end_frame();
        CHECK(renderer->stats().shadow_draws == 8); // 2 instances x 4 cascades
        const de::core::Array<std::byte> pixels = de::test::read_texture(*device, *target);
        // x on the back square to pixels: half the view is 3 m x tan(0.5) wide.
        const auto column = [](float x) {
            const float half = static_cast<float>(kSize) * 0.5f;
            return static_cast<uint32_t>(half + x / (3.0f * std::tan(0.5f)) * half);
        };
        CHECK(texel(pixels, column(0.1f), kSize / 2)[0] < 40);       // in the shadow
        CHECK(texel(pixels, column(-0.6f), kSize / 2)[0] > 215);     // lit
        CHECK(texel(pixels, column(0.1f), kSize / 2 + 14)[0] > 215); // below the shadow: lit
        assets->release(mesh);
        assets->update();
        device->wait_idle();
        device->destroy(*target);
    }
    device->wait_idle();
    de::test::check_clean(*device);
}

TEST_CASE("a material whose texture was missing picks it up when it appears") {
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
    // The square with a material sampling a texture that isn't cooked yet.
    put_assets(*vfs);
    constexpr AssetId kGreen = AssetId::from_value(0x53);
    de::cooker::MaterialBuild build;
    build.set_texture(de::asset::material::kBaseColorTexture, kGreen);
    const auto material = de::cooker::cook_material(build, kRed);
    REQUIRE(vfs->write_file(de::asset::cooked_path(kRed, AssetType::Material).c_str(), material.span()).ok());
    auto compiler = de::shader::ShaderCompiler::create(*vfs);
    REQUIRE(compiler.ok());
    auto shaders = de::shader::ShaderLibrary::create(*device, *vfs, jobs, *compiler, {.hot_reload = false});
    REQUIRE(shaders.ok());
    auto uploader = de::rhi::Uploader::create(*device);
    REQUIRE(uploader.ok());
    auto assets = de::asset::AssetRegistry::create(
        {.vfs = &*vfs, .jobs = &jobs, .hot_reload = true, .reload_checks_per_update = 64});
    REQUIRE(assets.ok());
    {
        auto gpu_assets =
            de::render::GpuAssets::create({.device = &*device, .uploader = &*uploader, .assets = &*assets});
        REQUIRE(gpu_assets.ok());
        auto renderer =
            de::render::SceneRenderer::create({.device = &*device, .shaders = &*shaders, .gpu_assets = &*gpu_assets});
        REQUIRE(renderer.ok());
        auto graph = de::render::RenderGraph::create(*device);
        REQUIRE(graph.ok());
        const auto target =
            device->create_texture({.width = kSize,
                                    .height = kSize,
                                    .format = de::rhi::Format::RGBA8Unorm,
                                    .usage = de::rhi::TextureUsage::RenderTarget | de::rhi::TextureUsage::CopySrc,
                                    .debug_name = "target"});
        REQUIRE(target.ok());
        de::world::RenderFrame frame;
        frame.camera = {.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 3}, .vertical_fov_rad = 1.0f, .valid = true};
        const de::asset::AssetHandle mesh = assets->load<de::asset::MeshAsset>(kQuad);
        frame.instances.push({.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}, .mesh = mesh});
        // The engine's frame, a few times over: the registry, the GPU copies, the passes.
        const auto run_frames = [&](int count) {
            for (int i = 0; i < count; ++i) {
                assets->update();
                assets->wait_all();
                gpu_assets->update();
                device->begin_frame();
                uploader->record();
                graph->begin(kSize, kSize);
                const de::render::GraphTexture color = graph->import_texture(
                    {.texture = *target, .name = "target", .final_state = de::rhi::ResourceState::CopySrc});
                renderer->add_passes(*graph, color, de::rhi::Format::RGBA8Unorm, frame, {},
                                     de::render::SceneView::BaseColor);
                graph->execute();
                device->end_frame();
            }
            return de::test::read_texture(*device, *target);
        };
        // The texture is missing: white, the default.
        const de::core::Array<std::byte> before = run_frames(4);
        REQUIRE(gpu_assets->mesh(mesh) != nullptr);
        CHECK(texel(before, kSize / 2, kSize / 2)[1] == 255);
        CHECK(texel(before, kSize / 2, kSize / 2)[0] == 255);
        // The texture is cooked: the registry picks it up, and the material with it.
        de::cooker::Image image;
        image.width = 8;
        image.height = 8;
        for (int i = 0; i < 64; ++i) {
            for (const uint8_t channel : {uint8_t{0}, uint8_t{255}, uint8_t{0}, uint8_t{255}}) {
                image.rgba.push(channel);
            }
        }
        de::cooker::Diagnostics diagnostics;
        const auto green = de::cooker::cook_texture(image, {}, kGreen, nullptr, "test", diagnostics);
        REQUIRE(green.ok());
        REQUIRE(vfs->write_file(de::asset::cooked_path(kGreen, AssetType::Texture).c_str(), green->span()).ok());
        const de::core::Array<std::byte> after = run_frames(6);
        CHECK(texel(after, kSize / 2, kSize / 2)[0] < 30);
        CHECK(texel(after, kSize / 2, kSize / 2)[1] > 220);
        assets->release(mesh);
        assets->update();
        device->wait_idle();
        device->destroy(*target);
    }
    device->wait_idle();
    de::test::check_clean(*device);
}

TEST_CASE("an environment becomes the renderer's lighting") {
    de::asset::EnvironmentAsset environment;
    environment.sun_direction[0] = 0.6f;
    environment.sun_direction[1] = 0.8f;
    environment.sun_direction[2] = 0.0f;
    environment.sun_color[2] = 3.0f;
    environment.shadow_color[1] = 0.25f;
    environment.ground_color[0] = 0.5f;
    environment.exposure_ev = -2.0f;
    environment.shadow_resolution = 1024;
    environment.shadow_distance_m = 90.0f;
    environment.shadow_max_penumbra_texels = 3.0f;
    environment.fog_height_density_per_m = 0.002f;
    environment.fog_base_height_m = -15.0f;
    environment.fog_extinction_tint[2] = 1.3f;
    environment.fog_sun_back[1] = 0.4f;
    environment.fog_ambient_height_attenuation = 0.5f;
    environment.fog_sun_start_m = 7.0f;
    environment.fog_end_m = 9000.0f;
    environment.fog_volume_distance_m = 100.0f;
    const de::render::SceneLighting lighting = de::render::scene_lighting(environment);
    CHECK(lighting.sun_direction[0] == 0.6f);
    CHECK(lighting.sun_direction[1] == 0.8f);
    CHECK(lighting.sun_color[2] == 3.0f);
    CHECK(lighting.shadow_color[1] == 0.25f);
    CHECK(lighting.ground_color[0] == 0.5f);
    CHECK(lighting.exposure_ev == -2.0f);
    CHECK(lighting.shadows.resolution == 1024);
    CHECK(lighting.shadows.distance_m == 90.0f);
    CHECK(lighting.shadows.max_penumbra_texels == 3.0f);
    CHECK(lighting.fog.height_density_per_m == 0.002f);
    CHECK(lighting.fog.base_height_m == -15.0);
    CHECK(lighting.fog.extinction_tint[2] == 1.3f);
    CHECK(lighting.fog.sun_back[1] == 0.4f);
    CHECK(lighting.fog.ambient_height_attenuation == 0.5f);
    CHECK(lighting.fog.sun_start_m == 7.0f);
    CHECK(lighting.fog.end_m == 9000.0f);
    CHECK(lighting.fog.volume_distance_m == 100.0f);
}

TEST_CASE("masked materials are cut out, double-sided ones seen from behind, each by its own pipeline") {
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    const de::test::Sandbox sandbox;
    de::core::JobSystem jobs({.worker_count = 2});
    const de::platform::PathString game_root = sandbox.game();
    auto vfs = de::platform::Vfs::create(
        {.game_root = game_root.c_str(), .src_root = DE_SOURCE_DIR, .jobs = &jobs, .game_writable = true});
    REQUIRE(vfs.ok());
    // A red square per material variant: its alpha mode, flags and base color alpha.
    using de::asset::AlphaMode;
    using de::asset::MaterialFlags;
    struct Variant {
        AlphaMode mode;
        MaterialFlags flags;
        float alpha;
    };
    const Variant variants[] = {
        {.mode = AlphaMode::Opaque, .flags = MaterialFlags::None, .alpha = 0.2f},
        {.mode = AlphaMode::Masked, .flags = MaterialFlags::None, .alpha = 0.2f},
        {.mode = AlphaMode::Masked, .flags = MaterialFlags::None, .alpha = 0.8f},
        {.mode = AlphaMode::Opaque, .flags = MaterialFlags::DoubleSided, .alpha = 1.0f},
        {.mode = AlphaMode::Masked, .flags = MaterialFlags::DoubleSided, .alpha = 0.2f},
        {.mode = AlphaMode::Masked, .flags = MaterialFlags::DoubleSided, .alpha = 0.8f},
    };
    constexpr uint32_t kVariants = 6;
    for (uint32_t v = 0; v < kVariants; ++v) {
        const AssetId material_id = AssetId::from_value(0x300 + v);
        put_quad(*vfs, AssetId::from_value(0x400 + v), material_id);
        de::cooker::MaterialBuild build;
        build.alpha_mode = variants[v].mode;
        build.flags = variants[v].flags;
        build.set_param(de::asset::material::kBaseColor, 1.0f, 0.0f, 0.0f, variants[v].alpha);
        const auto material = de::cooker::cook_material(build, material_id);
        REQUIRE(
            vfs->write_file(de::asset::cooked_path(material_id, AssetType::Material).c_str(), material.span()).ok());
    }
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
        de::asset::AssetHandle meshes[kVariants];
        for (uint32_t v = 0; v < kVariants; ++v) {
            meshes[v] = assets->load<de::asset::MeshAsset>(AssetId::from_value(0x400 + v));
        }
        assets->wait_all();
        gpu_assets->update();
        uploader->flush_and_wait();
        gpu_assets->update();
        const auto target =
            device->create_texture({.width = kSize,
                                    .height = kSize,
                                    .format = de::rhi::Format::RGBA8Unorm,
                                    .usage = de::rhi::TextureUsage::RenderTarget | de::rhi::TextureUsage::CopySrc,
                                    .debug_name = "target"});
        REQUIRE(target.ok());
        auto graph = de::render::RenderGraph::create(*device);
        REQUIRE(graph.ok());
        // Whether variant v's square shows in the middle of the view: from 3 m in front of it, or
        // behind it turned around (180 degrees about Y).
        const auto draws = [&](uint32_t v, bool behind) {
            REQUIRE(gpu_assets->mesh(meshes[v]) != nullptr);
            de::world::RenderFrame frame;
            frame.camera = {.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 3}, .vertical_fov_rad = 1.0f, .valid = true};
            if (behind) {
                frame.camera.transform[0] = -1.0f;
                frame.camera.transform[10] = -1.0f;
                frame.camera.transform[11] = -3.0f;
            }
            frame.instances.push({.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}, .mesh = meshes[v]});
            device->begin_frame();
            uploader->record();
            graph->begin(kSize, kSize);
            const de::render::GraphTexture color = graph->import_texture(
                {.texture = *target, .name = "target", .final_state = de::rhi::ResourceState::CopySrc});
            renderer->add_passes(*graph, color, de::rhi::Format::RGBA8Unorm, frame, {},
                                 de::render::SceneView::BaseColor);
            graph->execute();
            device->end_frame();
            return texel(de::test::read_texture(*device, *target), kSize / 2, kSize / 2)[0] > 200;
        };
        CHECK(draws(0, false));       // opaque: alpha doesn't matter (and no alpha test runs)
        CHECK_FALSE(draws(1, false)); // masked, below the cutoff
        CHECK(draws(2, false));       // masked, above it
        CHECK_FALSE(draws(0, true));  // back face culled
        CHECK(draws(3, true));        // double-sided
        CHECK_FALSE(draws(4, true));  // double-sided and masked, below the cutoff
        CHECK(draws(5, true));
        for (const de::asset::AssetHandle mesh : meshes) {
            assets->release(mesh);
        }
        assets->update();
        device->wait_idle();
        device->destroy(*target);
    }
    device->wait_idle();
    de::test::check_clean(*device);
}

TEST_CASE("fog noise tiles, covers the whole range, and each mip is the mean of the one above") {
    using de::render::kFogNoiseBytes;
    using de::render::kFogNoiseSize;
    de::core::Array<uint8_t> noise(de::core::heap_allocator(de::core::MemoryTag::Render));
    noise.resize(kFogNoiseBytes);
    de::render::make_fog_noise(noise.span());
    const auto at = [&](uint32_t x, uint32_t y, uint32_t z) {
        return static_cast<int>(noise[size_t{z % kFogNoiseSize} * kFogNoiseSize * kFogNoiseSize +
                                      size_t{y % kFogNoiseSize} * kFogNoiseSize + x % kFogNoiseSize]);
    };
    // Across the seam it changes no more than anywhere else, on each axis.
    const size_t top_bytes = size_t{kFogNoiseSize} * kFogNoiseSize * kFogNoiseSize;
    int inside_step = 0;
    int seam_step = 0;
    int lowest = 255;
    int highest = 0;
    for (uint32_t b = 0; b < kFogNoiseSize; ++b) {
        for (uint32_t a = 0; a < kFogNoiseSize; ++a) {
            for (uint32_t c = 0; c < kFogNoiseSize; ++c) {
                const int here = at(c, a, b);
                lowest = std::min(lowest, here);
                highest = std::max(highest, here);
                const int across = std::max({std::abs(here - at(c + 1, a, b)), std::abs(here - at(c, a + 1, b)),
                                             std::abs(here - at(c, a, b + 1))});
                if (c == kFogNoiseSize - 1 || a == kFogNoiseSize - 1 || b == kFogNoiseSize - 1) {
                    seam_step = std::max(seam_step, across);
                } else {
                    inside_step = std::max(inside_step, across);
                }
            }
        }
    }
    CHECK(lowest == 0);
    CHECK(highest == 255);
    CHECK(seam_step <= inside_step + 8);
    // The last mip is one texel, the mean of it all.
    int64_t total = 0;
    for (size_t i = 0; i < top_bytes; ++i) {
        total += noise[i];
    }
    const int mean = static_cast<int>(total / static_cast<int64_t>(top_bytes));
    CHECK(std::abs(static_cast<int>(noise[kFogNoiseBytes - 1]) - mean) <= 4);
    CHECK(mean > 60);
    CHECK(mean < 190);
    // The same every time.
    de::core::Array<uint8_t> again(de::core::heap_allocator(de::core::MemoryTag::Render));
    again.resize(kFogNoiseBytes);
    de::render::make_fog_noise(again.span());
    CHECK(std::memcmp(noise.data(), again.data(), kFogNoiseBytes) == 0);
}

TEST_CASE("the froxel grid agrees with the closed form, and adds the sun's shadows to the fog") {
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    const de::test::Sandbox sandbox;
    de::core::JobSystem jobs({.worker_count = 2});
    const de::platform::PathString game_root = sandbox.game();
    auto vfs = de::platform::Vfs::create(
        {.game_root = game_root.c_str(), .src_root = DE_SOURCE_DIR, .jobs = &jobs, .game_writable = true});
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
        CHECK(gpu_assets->fog_noise_index() == 0); // not on the GPU yet: volumes go without
        auto renderer =
            de::render::SceneRenderer::create({.device = &*device, .shaders = &*shaders, .gpu_assets = &*gpu_assets});
        REQUIRE(renderer.ok());
        const de::asset::AssetHandle mesh = assets->load<de::asset::MeshAsset>(kQuad);
        assets->wait_all();
        gpu_assets->update();
        uploader->flush_and_wait();
        gpu_assets->update();
        REQUIRE(gpu_assets->mesh(mesh) != nullptr);
        CHECK(gpu_assets->fog_noise_index() != 0);
        const auto target =
            device->create_texture({.width = kSize,
                                    .height = kSize,
                                    .format = de::rhi::Format::RGBA8Unorm,
                                    .usage = de::rhi::TextureUsage::RenderTarget | de::rhi::TextureUsage::CopySrc,
                                    .debug_name = "target"});
        REQUIRE(target.ok());
        auto graph = de::render::RenderGraph::create(*device);
        REQUIRE(graph.ok());

        // The camera at the origin looking down -Z, the sun straight overhead.
        de::world::RenderFrame frame;
        frame.camera = {.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}, .vertical_fov_rad = 1.0f, .valid = true};
        const auto record_on = [&](de::rhi::TextureHandle on, uint32_t size, const de::render::SceneLighting& light,
                                   bool taa = false) {
            device->begin_frame();
            uploader->record();
            graph->begin(size, size);
            const de::render::GraphTexture color = graph->import_texture(
                {.texture = on, .name = "target", .final_state = de::rhi::ResourceState::CopySrc});
            renderer->add_passes(*graph, color, de::rhi::Format::RGBA8Unorm, frame, light, de::render::SceneView::Lit,
                                 taa);
            graph->execute();
            device->end_frame();
        };
        const auto draw_on = [&](de::rhi::TextureHandle on, uint32_t size, const de::render::SceneLighting& light,
                                 bool taa = false) {
            record_on(on, size, light, taa);
            return de::test::read_texture(*device, on);
        };
        const auto lit_by = [](const de::render::SceneFog& fog) {
            de::render::SceneLighting light{.sun_direction = {0, 1, 0}};
            light.fog = fog;
            return light;
        };
        const auto render = [&](const de::render::SceneFog& fog) { return draw_on(*target, kSize, lit_by(fog)); };
        // As extraction does: what was drawn is the next frame's previous view.
        const auto remember_camera = [&] {
            std::memcpy(frame.camera.previous_transform, frame.camera.transform, sizeof(frame.camera.transform));
            frame.camera.previous_vertical_fov_rad = frame.camera.vertical_fov_rad;
            frame.camera.previous_near_m = frame.camera.near_m;
            frame.camera.previous_valid = true;
        };
        // Looking a little down, past anything: a fog of the sun's light alone, and of the sky's.
        const uint32_t x = kSize / 2;
        const uint32_t y = kSize - 2;
        const de::render::SceneFog closed{.density_per_m = 0.1f,
                                          .extinction_tint = {0.8f, 1.0f, 1.3f},
                                          .sun_forward = {0.05f, 0.1f, 0.05f},
                                          .sun_back = {0.08f, 0.12f, 0.02f},
                                          .sun_height_attenuation = 0.0f,
                                          .ambient = {0.05f, 0.05f, 0.1f},
                                          .end_m = 1000.0f};
        // Two images agree: every channel within 5% (or a few levels, near black).
        const auto same = [](const de::core::Array<std::byte>& a, const de::core::Array<std::byte>& b, uint32_t at_x,
                             uint32_t at_y) {
            for (const uint32_t channel : {0U, 1U, 2U}) {
                const int from_a = texel(a, at_x, at_y)[channel];
                const int from_b = texel(b, at_x, at_y)[channel];
                CHECK(std::abs(from_a - from_b) <= std::max(4, from_b / 20));
            }
        };
        de::render::SceneFog gridded = closed;
        gridded.volume_distance_m = 60.0f;

        // Nothing casts a shadow: the grid and the closed form give the same fog.
        const de::core::Array<std::byte> closed_clear = render(closed);
        const de::core::Array<std::byte> gridded_clear = render(gridded);
        same(gridded_clear, closed_clear, x, y);
        CHECK(texel(gridded_clear, x, y)[1] > 40); // and there is fog to see
        // A height fog too, whose density changes along the ray: still the same.
        de::render::SceneFog high = closed;
        high.density_per_m = 0.0f;
        high.height_density_per_m = 0.1f;
        high.height_falloff_per_m = 0.05f;
        high.ambient_height_attenuation = 0.0f;
        de::render::SceneFog high_gridded = high;
        high_gridded.volume_distance_m = 60.0f;
        const de::core::Array<std::byte> high_closed = render(high);
        const de::core::Array<std::byte> high_grid = render(high_gridded);
        same(high_grid, high_closed, x, y);

        // Fog that starts 15 m out, and sun haze that starts 30 m out: the same again (thin fog, so
        // that what's past those distances counts).
        de::render::SceneFog started = closed;
        started.density_per_m = 0.02f;
        started.start_m = 15.0f;
        started.sun_forward[1] = started.sun_back[1] = 0.4f;
        de::render::SceneFog started_gridded = started;
        started_gridded.volume_distance_m = 60.0f;
        const de::core::Array<std::byte> started_closed_image = render(started);
        const de::core::Array<std::byte> started_grid_image = render(started_gridded);
        same(started_grid_image, started_closed_image, x, y);
        started.sun_start_m = 30.0f;
        started_gridded.sun_start_m = 30.0f;
        const de::core::Array<std::byte> late_closed_image = render(started);
        const de::core::Array<std::byte> late_grid_image = render(started_gridded);
        CHECK(texel(late_closed_image, x, y)[1] < texel(started_closed_image, x, y)[1]); // it matters here
        same(late_grid_image, late_closed_image, x, y);

        // Thin fog ending past the grid, so the grid and what's beyond it both count; and at the corner
        // of the view, where a ray is longer than its view depth. Then ending inside the grid.
        const auto agree = [&](de::render::SceneFog fog, uint32_t at_x, uint32_t at_y) {
            const de::core::Array<std::byte> closed_image = render(fog);
            fog.volume_distance_m = 60.0f;
            const de::core::Array<std::byte> grid_image = render(fog);
            CHECK(texel(closed_image, at_x, at_y)[1] > 40);
            same(grid_image, closed_image, at_x, at_y);
        };
        de::render::SceneFog thin = closed;
        thin.density_per_m = 0.004f;
        thin.sun_forward[1] = thin.sun_back[1] = 0.4f;
        thin.end_m = 150.0f;
        agree(thin, x, y);
        agree(thin, 1, kSize - 2);
        thin.density_per_m = 0.01f;
        thin.end_m = 40.0f;
        agree(thin, x, y);
        agree(thin, 1, kSize - 2);
        agree(thin, x, 1); // looking up at the sky: no fog dims it past where the fog ends
        // A surface inside the grid, close by: the fog up to it, read from between two slices.
        frame.instances.push({.transform = {5, 0, 0, 0, 0, 5, 0, 0, 0, 0, 5, -1}, .mesh = mesh});
        thin.density_per_m = 0.2f;
        thin.end_m = 1000.0f;
        agree(thin, kSize / 2, kSize / 2);
        frame.instances.clear();
        // A red surface in the grid behind dense fog, with red barely dimmed by it: a channel with a
        // small extinction scale goes on falling with depth (optical depth 27.5, red's e^-2.75 and
        // white's e^-27.5, far below what a stored transmittance could hold).
        frame.instances.push({.transform = {300, 0, 0, 0, 0, 300, 0, 0, 0, 0, 300, -55}, .mesh = mesh});
        de::render::SceneFog deep = closed;
        deep.density_per_m = 0.5f;
        deep.extinction_tint[0] = 0.1f;
        deep.sun_forward[0] = deep.sun_back[0] = deep.ambient[0] = 0.0f;
        agree(deep, kSize / 2, kSize / 2);
        // A grid asked to reach past the shadows (the limit, 2 km) stops where they do: past them it
        // would add nothing to the closed form but coarser slices. Nearer, it reaches as asked.
        deep.volume_distance_m = 2000.0f;
        static_cast<void>(render(deep));
        CHECK(renderer->stats().froxel_reach_m == de::render::SceneShadows{}.distance_m);
        deep.volume_distance_m = 60.0f;
        static_cast<void>(render(deep));
        CHECK(renderer->stats().froxel_reach_m == 60.0f);
        static_cast<void>(render(closed));
        CHECK(renderer->stats().froxel_reach_m == 0.0f);
        frame.instances.clear();

        // A roof 5 m up, 200 m wide (the square turned to lie flat), shadows the fog under it. The
        // closed form can't know; the grid does: what's left is the sky's light, and the sun's past
        // the roof's edge, behind nearly all of this fog.
        frame.instances.push({.transform = {100, 0, 0, 0, 0, 0, 100, 5, 0, 100, 0, 0}, .mesh = mesh});
        const de::core::Array<std::byte> closed_roofed = render(closed);
        const de::core::Array<std::byte> gridded_roofed = render(gridded);
        CHECK(static_cast<int>(texel(closed_roofed, x, y)[1]) ==
              doctest::Approx(static_cast<int>(texel(closed_clear, x, y)[1])).epsilon(0.03));
        CHECK(texel(gridded_roofed, x, y)[1] < texel(gridded_clear, x, y)[1] - 40);
        CHECK(texel(gridded_roofed, x, y)[2] > texel(gridded_roofed, x, y)[1]); // the sky's blue is what's left
        // A grid turned off holds no memory.
        const auto storage_textures = [&] {
            return device->memory_stats().counts[static_cast<size_t>(de::rhi::GpuMemoryCategory::StorageTextures)];
        };
        const uint32_t textures_with_grid = storage_textures();
        CHECK(textures_with_grid >= 2);
        static_cast<void>(render(closed));
        CHECK(storage_textures() <= textures_with_grid - 2);
        static_cast<void>(render(gridded));
        // The grid's two passes run only with one.
        CHECK(graph->stats().culled_passes == 0);
        const uint32_t with_grid = graph->stats().passes;
        static_cast<void>(render(closed));
        CHECK(graph->stats().passes == with_grid - 2);
        frame.instances.clear();

        // Over frames: a shadow edge straight ahead, the roof over the left half of the view. Each frame's
        // samples move around in the froxels; the light keeps most of last frame's at the same place.
        const de::world::RenderInstance half_roof{.transform = {100, 0, 0, -100, 0, 0, 100, 5, 0, 100, 0, 0},
                                                  .mesh = mesh};
        const uint32_t left = kSize / 4;
        const uint32_t right = kSize - kSize / 4;
        const uint32_t middle = kSize / 2;
        const auto next_frame = [&](const de::render::SceneFog& fog) {
            ++frame.number;
            de::core::Array<std::byte> image = render(fog);
            remember_camera();
            return image;
        };
        const auto settle = [&](const de::render::SceneFog& fog) {
            for (int i = 0; i < 48; ++i) {
                ++frame.number;
                record_on(*target, kSize, lit_by(fog));
                remember_camera();
            }
        };
        // Back to a frame with no history to read, and none counted.
        const auto restart = [&] {
            frame.number = 0;
            frame.camera.previous_valid = false;
        };
        const auto green = [&](const de::core::Array<std::byte>& image, uint32_t at_x) {
            return static_cast<int>(texel(image, at_x, y)[1]);
        };
        const auto look_from = [&](float camera_x) { frame.camera.transform[3] = camera_x; };
        // What's what, from one frame: no roof.
        frame.number = 0;
        const de::core::Array<std::byte> open_sky = render(gridded);
        frame.instances.push(half_roof);

        // Settled, it holds still, frame to frame, even along the edge; and each side is what it should be.
        settle(gridded);
        int lowest = 255;
        int highest = 0;
        for (int i = 0; i < 16; ++i) {
            const int level = green(next_frame(gridded), middle);
            lowest = std::min(lowest, level);
            highest = std::max(highest, level);
        }
        const de::core::Array<std::byte> settled = next_frame(gridded);
        CHECK(highest - lowest <= 6);
        CHECK(green(settled, left) < green(open_sky, left) - 20);
        same(settled, open_sky, right, y);

        // The roof goes: the shadow doesn't stay. It takes a few frames, not a moment, and no trace is left.
        frame.instances.clear();
        const de::core::Array<std::byte> just_gone = next_frame(gridded);
        CHECK(green(just_gone, left) < green(open_sky, left) - 10);
        settle(gridded);
        same(next_frame(gridded), open_sky, left, y);

        // A cut: nothing from before, from the first frame, and it's on a part of the world without the roof.
        frame.instances.push(half_roof);
        settle(gridded);
        look_from(400.0f);
        frame.camera.previous_valid = false;
        const de::core::Array<std::byte> after_cut = next_frame(gridded);
        same(after_cut, open_sky, left, y);
        look_from(0.0f);
        frame.camera.previous_valid = false;
        settle(gridded);

        // A frame not drawn (or drawn without the grid): no history across the gap.
        frame.instances.clear();
        ++frame.number;
        const de::core::Array<std::byte> after_gap = next_frame(gridded);
        same(after_gap, open_sky, left, y);

        // Another reach: other slices, so nothing from before.
        frame.instances.push(half_roof);
        settle(gridded);
        frame.instances.clear();
        de::render::SceneFog nearer = gridded;
        nearer.volume_distance_m = 40.0f;
        const de::core::Array<std::byte> after_reach = next_frame(nearer);
        same(after_reach, open_sky, left, y);

        // The camera moves, a meter a frame: the shadow stays with the world. Its edge, now on the side,
        // is where a settled view from there has it.
        frame.instances.push(half_roof);
        frame.camera.previous_valid = false;
        look_from(-20.0f);
        settle(gridded);
        for (int i = 1; i <= 20; ++i) {
            look_from(-20.0f + static_cast<float>(i));
            static_cast<void>(next_frame(gridded));
        }
        const de::core::Array<std::byte> moved = next_frame(gridded);
        settle(gridded);
        const de::core::Array<std::byte> still = next_frame(gridded);
        for (const uint32_t at : {left, middle, right}) {
            CHECK(std::abs(green(moved, at) - green(still, at)) <= 12);
        }
        frame.camera.transform[3] = 0.0f;
        restart();
        frame.instances.clear();

        // History at the wrong slice would drift a shadow's edge along the view: with the slices thick, far
        // out, settled is where a single frame puts it.
        de::render::SceneFog far_fog = closed;
        far_fog.density_per_m = 0.02f;
        far_fog.sun_forward[1] = far_fog.sun_back[1] = 0.4f;
        far_fog.volume_distance_m = 400.0f;
        for (const float edge_z : {-40.0f, -50.0f, -56.0f}) {
            frame.instances.clear();
            frame.instances.push({.transform = {100, 0, 0, 0, 0, 0, 100, 5, 0, 100, 0, edge_z - 100.0f}, .mesh = mesh});
            restart();
            const int single_frame = green(render(far_fog), middle);
            settle(far_fog);
            CAPTURE(edge_z);
            CHECK(std::abs(green(next_frame(far_fog), middle) - single_frame) <= 3);
        }
        frame.instances.clear();

        // Fog that starts nearer: froxels newly in fog held no light last frame, and don't take that as
        // history, so the new fog isn't dim.
        restart();
        de::render::SceneFog start_far = far_fog;
        start_far.start_m = 60.0f;
        settle(start_far);
        de::render::SceneFog start_near = far_fog;
        start_near.start_m = 10.0f;
        const de::core::Array<std::byte> moved_start = next_frame(start_near);
        restart();
        const int fresh_start = green(render(start_near), middle);
        CHECK(std::abs(green(moved_start, middle) - fresh_start) <= 3);

        // Fog starting inside the grid, the camera moving along the view: the start moves with it, past
        // froxels with no fog. They don't darken the fog next to them.
        de::render::SceneFog edge_fog = closed;
        edge_fog.density_per_m = 0.15f;
        edge_fog.sun_forward[1] = edge_fog.sun_back[1] = 0.3f;
        edge_fog.volume_distance_m = 400.0f;
        edge_fog.start_m = 12.0f;
        restart();
        const int still_start = green(render(edge_fog), middle);
        for (int i = 0; i < 48; ++i) {
            frame.camera.transform[11] = -static_cast<float>(i) * 0.7f;
            static_cast<void>(next_frame(edge_fog));
        }
        const int moving_start = green(next_frame(edge_fog), middle);
        CHECK(std::abs(moving_start - still_start) <= 6);
        frame.camera.transform[11] = 0.0f;
        frame.camera.previous_valid = false;

        // Wherever the fog starts inside a froxel, its samples stay in the fog: no frame with none of them
        // in it, gone dark and fading back.
        int start_swing = 0;
        for (const float start_m : {6.0f, 7.0f, 8.0f, 9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f}) {
            edge_fog.start_m = start_m;
            restart();
            static_cast<void>(render(edge_fog));
            settle(edge_fog);
            int start_lowest = 255;
            int start_highest = 0;
            for (int i = 0; i < 16; ++i) {
                const int level = green(next_frame(edge_fog), middle);
                start_lowest = std::min(start_lowest, level);
                start_highest = std::max(start_highest, level);
            }
            start_swing = std::max(start_swing, start_highest - start_lowest);
        }
        CHECK(start_swing <= 2);

        // A bad value in one frame's light lasts that frame: it doesn't live on in the history.
        restart();
        settle(gridded);
        const de::core::Array<std::byte> before_bad = next_frame(gridded);
        de::render::SceneLighting bad_light{.sun_direction = {0, 1, 0}};
        bad_light.fog = gridded;
        bad_light.sun_color[0] = bad_light.sun_color[1] = bad_light.sun_color[2] =
            std::numeric_limits<float>::quiet_NaN();
        ++frame.number;
        static_cast<void>(draw_on(*target, kSize, bad_light));
        remember_camera();
        // The frame right after: the light it had is still there. And nothing lives on.
        same(next_frame(gridded), before_bad, right, y);
        settle(gridded);
        same(next_frame(gridded), before_bad, right, y);

        // Very dense fog in a very bright sun: what's stored still fits its format. Over it, it would be
        // infinite, and stay so through the history once the sun is back to normal.
        de::render::SceneFog dense = gridded;
        dense.density_per_m = 1.0f;
        restart();
        const int dense_normal = green(render(dense), right);
        de::render::SceneLighting blinding = lit_by(dense);
        blinding.sun_color[0] = blinding.sun_color[1] = blinding.sun_color[2] = 5000.0f;
        for (int i = 0; i < 8; ++i) {
            ++frame.number;
            record_on(*target, kSize, blinding);
            remember_camera();
        }
        for (int i = 0; i < 4; ++i) {
            settle(dense); // from as bright as it stores, it fades like anything else
        }
        CHECK(std::abs(green(next_frame(dense), right) - dense_normal) <= 3);
        restart();

        // Sharp shafts, at a grid of realistic size: curtains of shadow along the view. Over frames the
        // samples move, so an edge changes a little from frame to frame; with the history, only a little.
        {
            constexpr uint32_t kShaftSize = 512;
            const auto shaft_target =
                device->create_texture({.width = kShaftSize,
                                        .height = kShaftSize,
                                        .format = de::rhi::Format::RGBA8Unorm,
                                        .usage = de::rhi::TextureUsage::RenderTarget | de::rhi::TextureUsage::CopySrc,
                                        .debug_name = "shaft target"});
            REQUIRE(shaft_target.ok());
            frame.instances.clear();
            for (int i = -15; i <= 15; ++i) {
                frame.instances.push(
                    {.transform = {1.5f, 0, 0, 4.0f * static_cast<float>(i), 0, 0, 100, 8, 0, 100, 0, -50.0f},
                     .mesh = mesh});
            }
            de::render::SceneFog shaft_fog = closed;
            shaft_fog.density_per_m = 0.05f;
            shaft_fog.ambient[0] = shaft_fog.ambient[1] = shaft_fog.ambient[2] = 0.01f;
            shaft_fog.volume_distance_m = 80.0f;
            restart();
            de::render::SceneLighting shaft_light{.sun_direction = {0.35f, 1.0f, 0.0f}};
            shaft_light.fog = shaft_fog;
            const auto shaft_settle = [&](int frames, bool taa = false) {
                for (int i = 0; i < frames; ++i) {
                    ++frame.number;
                    record_on(*shaft_target, kShaftSize, shaft_light, taa);
                    remember_camera();
                }
            };
            const auto max_green_change = [](const de::core::Array<std::byte>& from,
                                             const de::core::Array<std::byte>& to) {
                int most = 0;
                for (size_t at = 1; at < to.size(); at += 4) {
                    most = std::max(most, std::abs(static_cast<int>(static_cast<uint8_t>(to[at])) -
                                                   static_cast<int>(static_cast<uint8_t>(from[at]))));
                }
                return most;
            };
            const auto shaft_frame = [&](bool taa = false) {
                ++frame.number;
                de::core::Array<std::byte> image = draw_on(*shaft_target, kShaftSize, shaft_light, taa);
                remember_camera();
                return image;
            };
            shaft_settle(48);
            de::core::Array<std::byte> before = shaft_frame();
            int most_changed = 0;
            int pixels_changed = 0;
            for (int i = 0; i < 8; ++i) {
                de::core::Array<std::byte> after = shaft_frame();
                for (size_t at = 1; at < after.size(); at += 4) { // green
                    const int change = std::abs(static_cast<int>(static_cast<uint8_t>(after[at])) -
                                                static_cast<int>(static_cast<uint8_t>(before[at])));
                    most_changed = std::max(most_changed, change);
                    pixels_changed += change > 0 ? 1 : 0;
                }
                before = std::move(after);
            }
            CHECK(pixels_changed > 10000); // each froxel settles at its own moving place
            CHECK(most_changed <= 3);      // and only a little: the history, and a different place per froxel

            // The frame after a cut, with no history to average with, isn't jittered: the same as a single
            // frame.
            restart();
            const de::core::Array<std::byte> single_shaft = draw_on(*shaft_target, kShaftSize, shaft_light);
            shaft_settle(8);
            frame.camera.previous_valid = false;
            const de::core::Array<std::byte> after_cut_shaft = shaft_frame();
            CHECK(max_green_change(single_shaft, after_cut_shaft) <= 2);

            // The same where only this froxel has no history: turned right around, every point was behind
            // last frame's camera, though the frame before was drawn.
            shaft_settle(8);
            frame.camera.transform[0] = -1.0f;
            frame.camera.transform[10] = -1.0f;
            const de::core::Array<std::byte> turned_around = shaft_frame();
            restart();
            const de::core::Array<std::byte> single_turned = draw_on(*shaft_target, kShaftSize, shaft_light);
            CHECK(max_green_change(single_turned, turned_around) <= 2);
            frame.camera.transform[0] = 1.0f;
            frame.camera.transform[10] = 1.0f;
            restart();

            // TAA's offset moves the image, not the grid: over frames the shafts don't blur. What's left
            // from the settled image is what TAA does to it.
            restart();
            shaft_settle(64);
            const de::core::Array<std::byte> without_taa = shaft_frame();
            frame.camera.previous_valid = false;
            shaft_settle(96, true);
            const de::core::Array<std::byte> with_taa = shaft_frame(true);
            int taa_total = 0;
            for (size_t at = 1; at < with_taa.size(); at += 4) {
                taa_total += std::abs(static_cast<int>(static_cast<uint8_t>(with_taa[at])) -
                                      static_cast<int>(static_cast<uint8_t>(without_taa[at])));
            }
            const double taa_mean = static_cast<double>(taa_total) / (static_cast<double>(with_taa.size()) / 4.0);
            CHECK(taa_mean < 0.42);
            frame.instances.clear();
            restart();
            device->wait_idle();
            device->destroy(*shaft_target);
        }

        // Fog volumes: a box of mist in clear air, 10 m ahead, 3 m to a side. It takes the world's fog colours.
        de::render::SceneFog clear_air = gridded;
        clear_air.density_per_m = 0.0f;
        restart();
        const auto volume_at = [](float center_x, float center_y, float center_z, float half_size, float density,
                                  float edge) {
            de::world::RenderFogVolume volume{.to_local = {1, 0, 0, -center_x, 0, 1, 0, -center_y, 0, 0, 1, -center_z},
                                              .center = {center_x, center_y, center_z},
                                              .half_extents = {half_size, half_size, half_size},
                                              .density_per_m = density,
                                              .edge_m = edge};
            return volume;
        };
        // Where a point at this x and y, 10 m out, is on the screen.
        const auto screen = [](float at) {
            return static_cast<uint32_t>(static_cast<float>(kSize) * 0.5f * (1.0f + at / (10.0f * std::tan(0.5f))));
        };
        const auto at_pixel = [&](const de::core::Array<std::byte>& image, uint32_t at_x, uint32_t at_y,
                                  uint32_t channel) { return static_cast<int>(texel(image, at_x, at_y)[channel]); };
        const uint32_t heart = kSize / 2 - 1;
        const uint32_t outside = screen(4.5f);
        const de::core::Array<std::byte> no_volume = render(clear_air);
        frame.fog_volumes.push(volume_at(0.0f, 0.0f, -10.0f, 3.0f, 0.5f, 0.5f));
        const de::core::Array<std::byte> box = render(clear_air);
        CHECK(renderer->stats().fog_volumes == 1);
        CHECK(std::abs(at_pixel(box, heart, heart, 1) - at_pixel(no_volume, heart, heart, 1)) > 15);
        CHECK(std::abs(at_pixel(box, outside, heart, 1) - at_pixel(no_volume, outside, heart, 1)) <= 3);

        // Without a grid to hold them, none: and no pass for them.
        de::render::SceneFog no_grid = clear_air;
        no_grid.volume_distance_m = 0.0f;
        const de::core::Array<std::byte> gridless = render(no_grid);
        CHECK(renderer->stats().fog_volumes == 0);
        CHECK(std::abs(at_pixel(gridless, heart, heart, 1) - at_pixel(no_volume, heart, heart, 1)) <= 3);

        // A soft edge thins it toward its surface; its heart is as dense.
        frame.fog_volumes.clear();
        frame.fog_volumes.push(volume_at(0.0f, 0.0f, -10.0f, 3.0f, 0.5f, 2.5f));
        const de::core::Array<std::byte> soft = render(clear_air);
        const uint32_t near_surface = screen(2.3f);
        CHECK(std::abs(at_pixel(soft, near_surface, heart, 1) - at_pixel(no_volume, near_surface, heart, 1)) <
              std::abs(at_pixel(box, near_surface, heart, 1) - at_pixel(no_volume, near_surface, heart, 1)) - 2);

        // An edge wider than the volume ends at its middle, so its heart keeps its density.
        frame.fog_volumes.clear();
        frame.fog_volumes.push(volume_at(0.0f, 0.0f, -10.0f, 2.0f, 0.5f, 2.0f));
        const de::core::Array<std::byte> edge_to_middle = render(clear_air);
        frame.fog_volumes[0].edge_m = 8.0f;
        const de::core::Array<std::byte> edge_past_middle = render(clear_air);
        CHECK(std::abs(at_pixel(edge_past_middle, heart, heart, 1) - at_pixel(edge_to_middle, heart, heart, 1)) <= 1);
        CHECK(std::abs(at_pixel(edge_past_middle, heart, heart, 1) - at_pixel(no_volume, heart, heart, 1)) > 5);

        // An ellipsoid has no corners. A box, looked at toward one, has fog there.
        frame.fog_volumes.clear();
        frame.fog_volumes.push(volume_at(0.0f, 0.0f, -10.0f, 3.0f, 0.5f, 0.5f));
        frame.fog_volumes[0].ellipsoid = true;
        const de::core::Array<std::byte> ball = render(clear_air);
        const uint32_t corner_x = screen(2.4f);
        const uint32_t corner_y = screen(-2.4f);
        CHECK(std::abs(at_pixel(ball, corner_x, corner_y, 1) - at_pixel(no_volume, corner_x, corner_y, 1)) <
              std::abs(at_pixel(box, corner_x, corner_y, 1) - at_pixel(no_volume, corner_x, corner_y, 1)) - 2);
        CHECK(std::abs(at_pixel(ball, heart, heart, 1) - at_pixel(no_volume, heart, heart, 1)) > 15);

        // Turned: a slab 10 m wide and thin, across the view, then along it, seen through its width.
        frame.fog_volumes.clear();
        de::world::RenderFogVolume slab = volume_at(0.0f, 0.0f, -10.0f, 1.0f, 0.5f, 0.2f);
        slab.half_extents[0] = 6.0f;
        slab.half_extents[1] = 6.0f;
        slab.half_extents[2] = 1.0f;
        frame.fog_volumes.push(slab);
        const de::core::Array<std::byte> across = render(clear_air);
        const float turned[12] = {0, 0, 1, 10, 0, 1, 0, 0, -1, 0, 0, 0}; // its x along the view
        std::memcpy(frame.fog_volumes[0].to_local, turned, sizeof(turned));
        const de::core::Array<std::byte> along = render(clear_air);
        const uint32_t aside = screen(4.0f);
        CHECK(std::abs(at_pixel(across, aside, heart, 1) - at_pixel(no_volume, aside, heart, 1)) > 8);
        CHECK(std::abs(at_pixel(along, aside, heart, 1) - at_pixel(no_volume, aside, heart, 1)) <= 3);
        CHECK(std::abs(at_pixel(along, heart, heart, 1) - at_pixel(no_volume, heart, heart, 1)) > 8);

        // Its tint is of the light it scatters.
        frame.fog_volumes.clear();
        frame.fog_volumes.push(volume_at(0.0f, 0.0f, -10.0f, 3.0f, 0.5f, 0.5f));
        frame.fog_volumes[0].tint[1] = 0.1f;
        frame.fog_volumes[0].tint[2] = 0.1f;
        const de::core::Array<std::byte> red_box = render(clear_air);
        CHECK(at_pixel(red_box, heart, heart, 1) < at_pixel(box, heart, heart, 1) - 15);

        // It's in the sun's shadow like any fog.
        frame.fog_volumes[0] = volume_at(0.0f, 0.0f, -10.0f, 3.0f, 0.5f, 0.5f);
        frame.instances.push({.transform = {100, 0, 0, 0, 0, 0, 100, 5, 0, 100, 0, 0}, .mesh = mesh});
        const de::core::Array<std::byte> roofed_box = render(clear_air);
        frame.instances.clear();
        CHECK(at_pixel(roofed_box, heart, heart, 1) < at_pixel(box, heart, heart, 1) - 15);

        // Where the sun's haze starts is about the world's fog: a volume nearer than that is still lit.
        de::render::SceneFog late_sun = clear_air;
        late_sun.sun_start_m = 30.0f;
        const de::core::Array<std::byte> late_sun_box = render(late_sun);
        CHECK(std::abs(at_pixel(late_sun_box, heart, heart, 1) - at_pixel(box, heart, heart, 1)) <= 3);
        // Nor does the world's fog take its light where there is none of it: it starts past the grid here.
        de::render::SceneFog late_start_fog = clear_air;
        late_start_fog.density_per_m = 0.3f;
        late_start_fog.start_m = 500.0f;
        late_start_fog.sun_start_m = 500.0f;
        frame.instances.push(
            {.transform = {100, 0, 0, 0, 0, 100, 0, 0, 0, 0, 100, -40}, .mesh = mesh}); // nothing past it
        const de::core::Array<std::byte> walled_clear = render(clear_air);
        const de::core::Array<std::byte> walled_late_start_fog = render(late_start_fog);
        frame.instances.clear();
        CHECK(std::abs(at_pixel(walled_late_start_fog, heart, heart, 1) - at_pixel(walled_clear, heart, heart, 1)) <=
              3);
        CHECK(at_pixel(walled_clear, heart, heart, 1) > 15);

        // A camera carrying a scale (a child of something scaled) sees it at its true depth.
        frame.fog_volumes[0] = volume_at(0.0f, 0.0f, -40.0f, 3.0f, 0.5f, 0.5f);
        const de::core::Array<std::byte> unscaled_view = render(clear_air);
        for (const int column : {0, 1, 2}) {
            frame.camera.transform[column] *= 2.0f;
            frame.camera.transform[4 + column] *= 2.0f;
            frame.camera.transform[8 + column] *= 2.0f;
        }
        const de::core::Array<std::byte> scaled_view = render(clear_air);
        CHECK(renderer->stats().fog_volumes == 1);
        for (const int column : {0, 1, 2}) {
            frame.camera.transform[column] *= 0.5f;
            frame.camera.transform[4 + column] *= 0.5f;
            frame.camera.transform[8 + column] *= 0.5f;
        }
        CHECK(std::abs(at_pixel(scaled_view, heart, heart, 1) - at_pixel(unscaled_view, heart, heart, 1)) <= 3);
        CHECK(std::abs(at_pixel(scaled_view, heart, heart, 1) - at_pixel(no_volume, heart, heart, 1)) > 8);
        frame.fog_volumes[0] = volume_at(0.0f, 0.0f, -10.0f, 3.0f, 0.5f, 0.5f);

        // Over frames it holds, like the rest of the grid, and goes when it goes.
        settle(clear_air);
        const de::core::Array<std::byte> settled_box = next_frame(clear_air);
        CHECK(std::abs(at_pixel(settled_box, heart, heart, 1) - at_pixel(box, heart, heart, 1)) <= 3);
        frame.fog_volumes.clear();
        const de::core::Array<std::byte> gone_box = next_frame(clear_air);
        CHECK(std::abs(at_pixel(gone_box, heart, heart, 1) - at_pixel(no_volume, heart, heart, 1)) <= 3);
        restart();

        // Around the camera, too, and it's in every slice it reaches. To the side of the view: not sent,
        // unless it reaches into it.
        frame.fog_volumes.push(volume_at(0.0f, 0.0f, 0.0f, 6.0f, 0.5f, 0.5f));
        const de::core::Array<std::byte> inside_it = render(clear_air);
        CHECK(renderer->stats().fog_volumes == 1);
        CHECK(std::abs(at_pixel(inside_it, heart, heart, 1) - at_pixel(no_volume, heart, heart, 1)) > 15);
        frame.fog_volumes.clear();
        frame.fog_volumes.push(volume_at(40.0f, 0.0f, -10.0f, 3.0f, 0.5f, 0.5f));
        frame.fog_volumes.push(volume_at(0.0f, 40.0f, -10.0f, 3.0f, 0.5f, 0.5f));
        static_cast<void>(render(clear_air));
        CHECK(renderer->stats().fog_volumes == 0);
        // With the air clear, ones out of view don't keep the fog passes on.
        CHECK(renderer->stats().froxel_reach_m == 0.0f);
        const uint32_t out_of_view_passes = graph->stats().passes;
        frame.fog_volumes.clear();
        static_cast<void>(render(clear_air));
        CHECK(graph->stats().passes == out_of_view_passes);
        // Panning one in and out of view doesn't make and free the kept 3D textures each time; with none left,
        // they go.
        frame.fog_volumes.push(volume_at(0.0f, 0.0f, -10.0f, 3.0f, 0.5f, 0.5f));
        static_cast<void>(render(clear_air));
        static_cast<void>(render(clear_air));
        const uint32_t textures_in_view = device->stats().live_textures;
        const uint32_t destroys_in_view = device->stats().pending_destroys;
        frame.fog_volumes[0] = volume_at(40.0f, 0.0f, -10.0f, 3.0f, 0.5f, 0.5f);
        static_cast<void>(render(clear_air));
        CHECK(device->stats().live_textures == textures_in_view);
        CHECK(device->stats().pending_destroys <= destroys_in_view);
        frame.fog_volumes.clear();
        static_cast<void>(render(clear_air));
        CHECK(device->stats().live_textures <= textures_in_view - 2);
        // They wait a while, counted from when one was last in view, then one out of view doesn't hold them. Each
        // check looks at the step when no volumes are left, as the render graph's pool lets go of its own over time.
        const auto volume_in_view_then_out = [&](int frames_out) {
            frame.fog_volumes.clear();
            frame.fog_volumes.push(volume_at(0.0f, 0.0f, -10.0f, 3.0f, 0.5f, 0.5f));
            static_cast<void>(render(clear_air));
            frame.fog_volumes[0] = volume_at(40.0f, 0.0f, -10.0f, 3.0f, 0.5f, 0.5f);
            for (int i = 0; i < frames_out; ++i) {
                record_on(*target, kSize, lit_by(clear_air));
            }
        };
        volume_in_view_then_out(200);
        volume_in_view_then_out(200); // over the wait in all, but not since it was last seen
        const uint32_t textures_waiting = device->stats().live_textures;
        frame.fog_volumes.clear();
        static_cast<void>(render(clear_air));
        CHECK(device->stats().live_textures <= textures_waiting - 2);
        volume_in_view_then_out(245);
        const uint32_t textures_past_wait = device->stats().live_textures;
        frame.fog_volumes.clear();
        static_cast<void>(render(clear_air));
        CHECK(device->stats().live_textures >= textures_past_wait);
        frame.fog_volumes.push(volume_at(7.0f, 0.0f, -10.0f, 3.0f, 0.5f, 0.5f)); // 4 m aside, and the view reaches 5.5
        const de::core::Array<std::byte> reaching_in = render(clear_air);
        CHECK(renderer->stats().fog_volumes == 1);
        CHECK(std::abs(at_pixel(reaching_in, screen(4.7f), heart, 1) - at_pixel(no_volume, screen(4.7f), heart, 1)) >
              8);
        CHECK(graph->stats().passes > out_of_view_passes);
        frame.fog_volumes.clear();

        // The grid's reach is in view depth: far to the side of the view, past it by distance but not by
        // depth, it's still in.
        frame.fog_volumes.push(volume_at(30.0f, 0.0f, -59.0f, 1.5f, 0.5f, 0.5f));
        static_cast<void>(render(clear_air));
        CHECK(renderer->stats().fog_volumes == 1);
        frame.fog_volumes.clear();

        // In the last tenth of the grid's reach it fades out, where it couldn't be held much farther.
        de::world::RenderFogVolume far_slab = volume_at(0.0f, 0.0f, -45.0f, 6.0f, 0.3f, 0.2f);
        far_slab.half_extents[2] = 1.0f;
        frame.fog_volumes.push(far_slab);
        const de::core::Array<std::byte> before_fade = render(clear_air);
        far_slab = volume_at(0.0f, 0.0f, -58.5f, 6.0f, 0.3f, 0.2f);
        far_slab.half_extents[2] = 1.0f;
        frame.fog_volumes[0] = far_slab;
        const de::core::Array<std::byte> in_fade = render(clear_air);
        const int seen_before_fade =
            std::abs(at_pixel(before_fade, heart, heart, 1) - at_pixel(no_volume, heart, heart, 1));
        CHECK(seen_before_fade > 8);
        CHECK(std::abs(at_pixel(in_fade, heart, heart, 1) - at_pixel(no_volume, heart, heart, 1)) <
              seen_before_fade / 2);
        frame.fog_volumes.clear();

        // Long along the view: it reaches across many slices, and it's in them all.
        de::world::RenderFogVolume long_one = volume_at(0.0f, 0.0f, -30.0f, 3.0f, 0.1f, 0.5f);
        long_one.half_extents[2] = 25.0f;
        frame.fog_volumes.push(long_one);
        const de::core::Array<std::byte> long_view = render(clear_air);
        frame.fog_volumes[0].half_extents[2] = 5.0f;
        const de::core::Array<std::byte> short_view = render(clear_air);
        CHECK(std::abs(at_pixel(long_view, heart, heart, 1) - at_pixel(short_view, heart, heart, 1)) > 5);
        // The same for an ellipsoid: its sphere reaches as far as its longest axis. Thin, to see all the way along.
        frame.fog_volumes[0].ellipsoid = true;
        frame.fog_volumes[0].density_per_m = 0.03f;
        frame.fog_volumes[0].half_extents[2] = 3.0f;
        const de::core::Array<std::byte> short_ellipsoid = render(clear_air);
        frame.fog_volumes[0].half_extents[2] = 25.0f;
        const de::core::Array<std::byte> long_ellipsoid = render(clear_air);
        CHECK(std::abs(at_pixel(long_ellipsoid, heart, heart, 1) - at_pixel(short_ellipsoid, heart, heart, 1)) > 5);
        frame.fog_volumes.clear();

        // Past the first 32 of them, read from the mask's second half.
        for (int i = 0; i < 40; ++i) {
            frame.fog_volumes.push(volume_at(static_cast<float>(i) * 0.1f - 2.0f, 10.0f, -30.0f, 3.0f, 0.5f, 0.5f));
        }
        frame.fog_volumes.push(volume_at(0.0f, 0.0f, -10.0f, 3.0f, 0.5f, 0.5f));
        const de::core::Array<std::byte> late_in_mask = render(clear_air);
        CHECK(renderer->stats().fog_volumes == 41);
        CHECK(std::abs(at_pixel(late_in_mask, heart, heart, 1) - at_pixel(box, heart, heart, 1)) <= 3);
        frame.fog_volumes.clear();

        // However many overlap, it fits what the froxels store: all that fog looks like the most they hold.
        frame.fog_volumes.push(volume_at(0.0f, 0.0f, -10.0f, 3.0f, 12.0f, 0.5f));
        const de::core::Array<std::byte> densest = render(clear_air);
        frame.fog_volumes.clear();
        for (int i = 0; i < 64; ++i) {
            frame.fog_volumes.push(volume_at(0.0f, 0.0f, -10.0f, 3.0f, 4.0f, 0.5f));
        }
        const de::core::Array<std::byte> stacked = render(clear_air);
        CHECK(std::abs(at_pixel(stacked, heart, heart, 1) - at_pixel(densest, heart, heart, 1)) <= 3);
        CHECK(std::abs(at_pixel(stacked, heart, heart, 1) - at_pixel(no_volume, heart, heart, 1)) > 15);
        frame.fog_volumes.clear();

        // A wall ending the view inside a slice with mist in it, far off, where a slice (here 48 to 50.8 m) is
        // as thick as the mist: it shows the mist in front of it, however much of the slice that is.
        const auto wall_at = [&](float depth) {
            frame.instances.clear();
            frame.instances.push({.transform = {100, 0, 0, 0, 0, 100, 0, 0, 0, 0, 100, -depth}, .mesh = mesh});
        };
        const auto mist_slab = [&](float center_depth, float half_depth) {
            de::world::RenderFogVolume slab_ahead = volume_at(0.0f, 0.0f, -center_depth, 6.0f, 1.0f, 0.1f);
            slab_ahead.half_extents[2] = half_depth;
            return slab_ahead;
        };
        const auto mist_in_slice = [&](const de::render::SceneFog& slice_fog) {
            wall_at(48.3f); // in front of the mist, in the same slice
            const de::core::Array<std::byte> before_bare = render(slice_fog);
            frame.fog_volumes.push(mist_slab(49.8f, 0.9f));
            const de::core::Array<std::byte> before_mist = render(slice_fog);
            frame.fog_volumes.pop();
            CHECK(std::abs(at_pixel(before_mist, heart, heart, 1) - at_pixel(before_bare, heart, heart, 1)) <= 3);

            wall_at(49.6f); // behind the mist, in the same slice
            frame.fog_volumes.push(mist_slab(48.7f, 0.6f));
            const de::core::Array<std::byte> behind_mist = render(slice_fog);
            wall_at(53.0f); // and well past it
            const de::core::Array<std::byte> far_past_mist = render(slice_fog);
            frame.fog_volumes.pop();
            const de::core::Array<std::byte> far_past_bare = render(slice_fog);
            const int all_of_it = at_pixel(far_past_mist, heart, heart, 1) - at_pixel(far_past_bare, heart, heart, 1);
            CHECK(all_of_it > 20);
            CHECK(std::abs(at_pixel(behind_mist, heart, heart, 1) - at_pixel(far_past_mist, heart, heart, 1)) <
                  all_of_it / 4);
            frame.instances.clear();
        };
        mist_in_slice(clear_air);
        // Fog that starts past the grid isn't in the slice: it doesn't weigh on the mist's share.
        de::render::SceneFog fog_past_grid = clear_air;
        fog_past_grid.density_per_m = 3.0f;
        fog_past_grid.start_m = 500.0f;
        fog_past_grid.sun_start_m = 500.0f;
        mist_in_slice(fog_past_grid);
        // Nor does fog that starts partway through the slice, behind the mist, take the mist's light.
        wall_at(49.6f);
        frame.fog_volumes.push(mist_slab(48.7f, 0.6f));
        const de::core::Array<std::byte> mist_alone = render(clear_air);
        de::render::SceneFog fog_behind_mist = clear_air;
        fog_behind_mist.density_per_m = 0.3f;
        fog_behind_mist.start_m = 49.9f;
        fog_behind_mist.sun_start_m = 49.9f;
        const de::core::Array<std::byte> mist_before_fog = render(fog_behind_mist);
        frame.fog_volumes.pop();
        frame.instances.clear();
        CHECK(std::abs(at_pixel(mist_before_fog, heart, heart, 1) - at_pixel(mist_alone, heart, heart, 1)) <= 4);
        // The same for a volume in the mask's second half (the others are above the view's middle).
        for (int i = 0; i < 40; ++i) {
            frame.fog_volumes.push(volume_at(static_cast<float>(i) * 0.1f - 2.0f, 20.0f, -49.0f, 3.0f, 0.5f, 0.5f));
        }
        mist_in_slice(clear_air);
        frame.fog_volumes.clear();

        // A thin wall's light is where it is: wholly under a roof no wider than it, it's as dark as under one that
        // covers everything, though the rest of its slice is sunlit.
        wall_at(55.0f);
        frame.fog_volumes.push(mist_slab(49.2f, 0.4f));
        const de::core::Array<std::byte> thin_lit = render(clear_air);
        frame.instances.push({.transform = {100, 0, 0, 0, 0, 0, 100, 5, 0, 100, 0, -49.2f}, .mesh = mesh});
        const de::core::Array<std::byte> thin_all_roofed = render(clear_air);
        frame.instances.back() = {.transform = {100, 0, 0, 0, 0, 0, 100, 5, 0, 0.8f, 0, -49.2f}, .mesh = mesh};
        const de::core::Array<std::byte> thin_narrow_roof = render(clear_air);
        frame.fog_volumes.clear();
        CHECK(at_pixel(thin_lit, heart, heart, 1) > at_pixel(thin_all_roofed, heart, heart, 1) + 15);
        CHECK(std::abs(at_pixel(thin_narrow_roof, heart, heart, 1) - at_pixel(thin_all_roofed, heart, heart, 1)) <= 6);

        // Slid along the view, through thick far slices, mist doesn't come and go: its points are its own. That
        // goes for a thin wall, thinner than the space between points in a slice, too.
        wall_at(55.0f);
        for (const float half_depth : {1.5f, 0.3f, -0.3f}) { // the last: an ellipsoid
            int least_seen = 255;
            int most_seen = 0;
            for (int i = 0; i < 16; ++i) {
                frame.fog_volumes.clear();
                frame.fog_volumes.push(mist_slab(40.0f + static_cast<float>(i) * 0.35f, std::abs(half_depth)));
                frame.fog_volumes[0].density_per_m = 0.6f / std::abs(half_depth);
                frame.fog_volumes[0].ellipsoid = half_depth < 0.0f;
                const de::core::Array<std::byte> slid = render(clear_air);
                least_seen = std::min(least_seen, at_pixel(slid, heart, heart, 1));
                most_seen = std::max(most_seen, at_pixel(slid, heart, heart, 1));
            }
            // A thin ellipsoid does change along one ray as it slides: it's thinner away from its middle.
            CHECK(most_seen - least_seen <= (half_depth < 0.0f ? 25 : 8));
            CHECK(least_seen > 40);
        }
        frame.instances.clear();
        frame.fog_volumes.clear();

        // Out of reach, or behind the camera: not sent. Too many: the nearest.
        frame.fog_volumes.push(volume_at(0.0f, 0.0f, -500.0f, 3.0f, 0.5f, 0.5f));
        frame.fog_volumes.push(volume_at(0.0f, 0.0f, 50.0f, 3.0f, 0.5f, 0.5f));
        static_cast<void>(render(clear_air));
        CHECK(renderer->stats().fog_volumes == 0);
        frame.fog_volumes.clear();
        for (int i = 0; i < 70; ++i) {
            frame.fog_volumes.push(volume_at(static_cast<float>(i) * 0.1f - 3.5f, 10.0f, -30.0f, 3.0f, 0.5f,
                                             0.5f)); // in view, above the heart
        }
        frame.fog_volumes.push(volume_at(0.0f, 0.0f, -10.0f, 3.0f, 0.5f, 0.5f));
        const de::core::Array<std::byte> crowded = render(clear_air);
        CHECK(renderer->stats().fog_volumes == 64);
        CHECK(renderer->stats().fog_volumes_dropped == 7);
        CHECK(std::abs(at_pixel(crowded, heart, heart, 1) - at_pixel(box, heart, heart, 1)) <= 3);
        // What's replaced is the farthest kept, wherever it is among them, not the first found.
        frame.fog_volumes.clear();
        frame.fog_volumes.push(volume_at(0.0f, 10.0f, -12.0f, 3.0f, 0.5f, 0.5f)); // above the view's middle, nearest
        for (int i = 0; i < 63; ++i) {
            frame.fog_volumes.push(volume_at(static_cast<float>(i) * 0.1f - 3.0f, 10.0f, -30.0f, 3.0f, 0.5f, 0.5f));
        }
        frame.fog_volumes.push(
            volume_at(0.0f, 0.0f, -20.0f, 3.0f, 0.5f, 0.5f)); // nearer than those, not than the first
        const de::core::Array<std::byte> replaced_farthest = render(clear_air);
        CHECK(renderer->stats().fog_volumes == 64);
        CHECK(renderer->stats().fog_volumes_dropped == 1);
        CHECK(std::abs(at_pixel(replaced_farthest, heart, heart, 1) - at_pixel(no_volume, heart, heart, 1)) > 8);
        frame.fog_volumes.clear();
        for (int i = 0; i < 70; ++i) {
            frame.fog_volumes.push(volume_at(static_cast<float>(i) * 0.1f - 3.5f, 10.0f, -30.0f, 3.0f, 0.5f, 0.5f));
        }
        // Nearest by its surface, not its middle: one the camera is inside stays, though its middle is farther
        // than all of those.
        frame.fog_volumes.push(volume_at(0.0f, 0.0f, -45.0f, 50.0f, 0.1f, 0.5f));
        const de::core::Array<std::byte> crowded_around = render(clear_air);
        CHECK(renderer->stats().fog_volumes == 64);
        CHECK(std::abs(at_pixel(crowded_around, heart, heart, 1) - at_pixel(no_volume, heart, heart, 1)) > 15);
        frame.fog_volumes.clear();
        restart();

        // Noise: none asked for, the shape alone, whatever else is set.
        const auto mist_ahead = [&](float depth, float half_depth) {
            de::world::RenderFogVolume volume = volume_at(0.0f, 0.0f, -depth, 15.0f, 0.2f, 1.0f);
            volume.half_extents[2] = half_depth;
            volume.noise_tiles_per_m = 0.05f;
            return volume;
        };
        const auto green_change = [](const de::core::Array<std::byte>& from, const de::core::Array<std::byte>& to) {
            int most = 0;
            for (size_t at = 1; at < from.size(); at += 4) {
                most = std::max(most, std::abs(static_cast<int>(static_cast<uint8_t>(from[at])) -
                                               static_cast<int>(static_cast<uint8_t>(to[at]))));
            }
            return most;
        };
        // How much a row and a column change from pixel to pixel: how much shape there is.
        const auto shape_in = [&](const de::core::Array<std::byte>& image) {
            int total = 0;
            for (uint32_t at = 1; at < kSize; ++at) {
                total += std::abs(at_pixel(image, at, heart, 1) - at_pixel(image, at - 1, heart, 1));
                total += std::abs(at_pixel(image, heart, at, 1) - at_pixel(image, heart, at - 1, 1));
            }
            return total;
        };
        frame.fog_volumes.push(mist_ahead(20.0f, 8.0f));
        frame.fog_volumes[0].noise_tiles_per_m = 0.0f;
        const de::core::Array<std::byte> shape_only = render(clear_air);
        frame.fog_volumes[0] = mist_ahead(20.0f, 8.0f);
        const de::core::Array<std::byte> amount_zero = render(clear_air);
        CHECK(green_change(shape_only, amount_zero) <= 1);
        // Asked for, it eats into the fog, unevenly.
        frame.fog_volumes[0].noise_amount = 1.0f;
        const de::core::Array<std::byte> noisy = render(clear_air);
        CHECK(shape_in(noisy) > shape_in(shape_only) + 20);
        // Moved, the pattern moves; by whole tiles in all its sizes, it's the same.
        frame.fog_volumes[0].noise_offset_tiles[0] = 0.5f;
        const de::core::Array<std::byte> drifted = render(clear_air);
        CHECK(green_change(noisy, drifted) > 6);
        frame.fog_volumes[0].noise_offset_tiles[0] = 4.0f;
        const de::core::Array<std::byte> wrapped_offset = render(clear_air);
        CHECK(green_change(noisy, wrapped_offset) <= 1);
        // The noise is the world's: moving a volume, or turning it, moves its shape and leaves the noise where it is.
        frame.fog_volumes[0] = mist_ahead(20.0f, 8.0f);
        frame.fog_volumes[0].noise_amount = 1.0f;
        frame.fog_volumes[0].noise_offset_tiles[0] = 0.0f;
        const de::core::Array<std::byte> before_moving = render(clear_air);
        const de::world::RenderFogVolume unmoved = frame.fog_volumes[0];
        frame.fog_volumes[0] = mist_ahead(20.0f, 8.0f);
        frame.fog_volumes[0].noise_amount = 1.0f;
        frame.fog_volumes[0].to_local[3] = -1.5f; // 1.5 m along x
        frame.fog_volumes[0].center[0] = 1.5f;
        const de::core::Array<std::byte> moved_volume = render(clear_air);
        CHECK(green_change(moved_volume, before_moving) <= 3);
        const float turned_axes[12] = {0, 0, 1, 20, 0, 1, 0, 0, -1, 0, 0, 0};
        frame.fog_volumes[0] = unmoved;
        std::memcpy(frame.fog_volumes[0].to_local, turned_axes, sizeof(turned_axes));
        frame.fog_volumes[0].half_extents[0] = 8.0f;
        frame.fog_volumes[0].half_extents[2] = 15.0f;
        const de::core::Array<std::byte> turned_volume = render(clear_air);
        CHECK(green_change(turned_volume, before_moving) <= 3);
        // Nor does the render origin moving change anything: the same world, with everything relative to another
        // origin, and the offset taking up the difference. 1024 m at this size of tile isn't a whole number of them.
        frame.fog_volumes[0] = mist_ahead(14.0f, 3.0f);
        frame.fog_volumes[0].noise_amount = 1.0f;
        frame.fog_volumes[0].noise_tiles_per_m = 0.05f;
        const de::core::Array<std::byte> origin_here = render(clear_air);
        frame.camera.transform[3] = -1024.0f;
        frame.fog_volumes[0].to_local[3] = 1024.0f;
        frame.fog_volumes[0].center[0] = -1024.0f;
        frame.fog_volumes[0].noise_offset_tiles[0] = 4.0f - 3.2f; // 1024 * 0.05 = 51.2, wrapped: -(-51.2) mod 4 = 3.2
        const de::core::Array<std::byte> origin_there = render(clear_air);
        frame.camera.transform[3] = 0.0f;
        CHECK(green_change(origin_here, origin_there) <= 2);
        // With noise far finer than the grid can hold, it's flat, and eats nothing: the shape alone.
        frame.fog_volumes[0] = mist_ahead(20.0f, 8.0f);
        frame.fog_volumes[0].noise_amount = 1.0f;
        frame.fog_volumes[0].noise_tiles_per_m = 200.0f;
        const de::core::Array<std::byte> too_fine = render(clear_air);
        CHECK(green_change(too_fine, shape_only) <= 2);
        // noise_amount limits how much a heart can lose: a half leaves no gaps through it.
        frame.fog_volumes[0] = volume_at(0.0f, 0.0f, -14.0f, 8.0f, 1.0f, 0.1f);
        const de::core::Array<std::byte> solid_heart = render(clear_air);
        frame.fog_volumes[0].noise_amount = 0.5f;
        frame.fog_volumes[0].noise_tiles_per_m = 0.25f;
        const de::core::Array<std::byte> noisy_heart = render(clear_air);
        int emptiest_heart = 255;
        for (uint32_t at = 8; at < kSize - 8; ++at) {
            emptiest_heart = std::min(
                emptiest_heart, std::abs(at_pixel(noisy_heart, at, heart, 1) - at_pixel(no_volume, at, heart, 1)));
        }
        CHECK(emptiest_heart > 15);
        // With a tile large enough to show every size: 4 tiles is a whole number of tiles for each.
        frame.fog_volumes[0] = mist_ahead(14.0f, 3.0f);
        frame.fog_volumes[0].noise_amount = 1.0f;
        frame.fog_volumes[0].noise_tiles_per_m = 0.05f;
        const de::core::Array<std::byte> large_tile = render(clear_air);
        frame.fog_volumes[0].noise_offset_tiles[0] = 4.0f;
        const de::core::Array<std::byte> large_tile_wrapped = render(clear_air);
        CHECK(green_change(large_tile, large_tile_wrapped) <= 1);
        // A height fade thins it toward its top, not its bottom.
        const de::world::RenderFogVolume fading = volume_at(0.0f, 0.0f, -12.0f, 3.0f, 0.5f, 0.2f);
        frame.fog_volumes[0] = fading;
        const de::core::Array<std::byte> not_fading = render(clear_air);
        frame.fog_volumes[0].height_fade_m = 3.0f;
        const de::core::Array<std::byte> faded = render(clear_air);
        const auto toward_top = static_cast<uint32_t>(kSize * 0.5f * (1.0f - 2.0f / (12.0f * std::tan(0.5f))));
        const auto toward_bottom = static_cast<uint32_t>(kSize * 0.5f * (1.0f + 2.0f / (12.0f * std::tan(0.5f))));
        CHECK(std::abs(at_pixel(faded, heart, toward_top, 1) - at_pixel(no_volume, heart, toward_top, 1)) <
              std::abs(at_pixel(not_fading, heart, toward_top, 1) - at_pixel(no_volume, heart, toward_top, 1)) - 8);
        CHECK(std::abs(at_pixel(faded, heart, toward_bottom, 1) - at_pixel(not_fading, heart, toward_bottom, 1)) <= 3);
        // Whichever way it's turned, the fade is toward the world's top. Flipped over, it's the same.
        const float flipped_axes[12] = {1, 0, 0, 0, 0, -1, 0, 0, 0, 0, -1, -12};
        std::memcpy(frame.fog_volumes[0].to_local, flipped_axes, sizeof(flipped_axes));
        const de::core::Array<std::byte> flipped_fade = render(clear_air);
        CHECK(green_change(flipped_fade, faded) <= 3);
        // Slices sliding through still noise finer than they can hold: moving the camera, little changes from step
        // to step, as each size reads a coarse enough mip. A thin volume too: its short span in a slice doesn't make
        // finer points of it. And sideways, past columns wider than the slices are deep at this size of image.
        wall_at(58.0f);
        const auto noise_crawl = [&](float half_depth, float density, int axis) {
            frame.fog_volumes.clear();
            frame.fog_volumes.push(mist_ahead(44.0f, half_depth));
            frame.fog_volumes[0].density_per_m = density;
            frame.fog_volumes[0].noise_amount = 1.0f;
            frame.fog_volumes[0].noise_tiles_per_m = 1.0f;
            const auto camera_at = [&](int step) {
                frame.camera.transform[axis] = static_cast<float>(step) * 0.3f;
                return render(clear_air);
            };
            int most = 0;
            for (int i = 0; i < 12; i += 2) {
                most = std::max(most, green_change(camera_at(i), camera_at(i + 1)));
            }
            frame.camera.transform[axis] = 0.0f;
            return most;
        };
        CHECK(noise_crawl(6.0f, 0.2f, 11) <= 20); // 11 here; 33 at the finest mip
        CHECK(noise_crawl(0.6f, 2.0f, 11) <= 20); // 8 here; 49
        CHECK(noise_crawl(6.0f, 0.2f, 3) <= 30);  // 24 here; 39
        frame.instances.clear();
        frame.fog_volumes.clear();
        restart();

        assets->release(mesh);
        assets->update();
        device->wait_idle();
        device->destroy(*target);
    }
    device->wait_idle();
    de::test::check_clean(*device);
}

TEST_CASE("motion vectors follow the camera and the instances, across render origin moves") {
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    const de::test::Sandbox sandbox;
    de::core::JobSystem jobs({.worker_count = 2});
    const de::platform::PathString game_root = sandbox.game();
    auto vfs = de::platform::Vfs::create(
        {.game_root = game_root.c_str(), .src_root = DE_SOURCE_DIR, .jobs = &jobs, .game_writable = true});
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
        uploader->flush_and_wait();
        gpu_assets->update();
        REQUIRE(gpu_assets->mesh(mesh) != nullptr);
        const auto target =
            device->create_texture({.width = kSize,
                                    .height = kSize,
                                    .format = de::rhi::Format::RGBA8Unorm,
                                    .usage = de::rhi::TextureUsage::RenderTarget | de::rhi::TextureUsage::CopySrc,
                                    .debug_name = "target"});
        REQUIRE(target.ok());
        auto graph = de::render::RenderGraph::create(*device);
        REQUIRE(graph.ok());

        // The debug view shows uv motion times 5, about grey, in red and green. The square fills the
        // view's middle from 3 m away: a meter there is 1 / (3 tan 0.5) of the view's half height.
        de::world::RenderFrame frame;
        frame.camera = {.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 3}, .vertical_fov_rad = 1.0f, .valid = true};
        const de::world::RenderInstance square{.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0},
                                               .previous_transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0},
                                               .mesh = mesh};
        frame.instances.push(square);
        const float uv_per_m = 0.5f / (3.0f * std::tan(0.5f));
        const auto shown = [&] {
            device->begin_frame();
            uploader->record();
            graph->begin(kSize, kSize);
            const de::render::GraphTexture color = graph->import_texture(
                {.texture = *target, .name = "target", .final_state = de::rhi::ResourceState::CopySrc});
            renderer->add_passes(*graph, color, de::rhi::Format::RGBA8Unorm, frame, {}, de::render::SceneView::Motion);
            graph->execute();
            device->end_frame();
            // As extraction does: what was drawn is the next frame's previous view.
            std::memcpy(frame.camera.previous_transform, frame.camera.transform, sizeof(frame.camera.transform));
            frame.camera.previous_vertical_fov_rad = frame.camera.vertical_fov_rad;
            frame.camera.previous_near_m = frame.camera.near_m;
            frame.camera.previous_valid = true;
            return de::test::read_texture(*device, *target);
        };
        const auto level = [](float uv_motion) { return (uv_motion * 5.0f + 0.5f) * 255.0f; };
        const auto check_center = [&](float motion_u, float motion_v) {
            const de::core::Array<std::byte> pixels = shown();
            const uint8_t* center = texel(pixels, kSize / 2, kSize / 2);
            CHECK(static_cast<float>(center[0]) == doctest::Approx(level(motion_u)).epsilon(0.03));
            CHECK(static_cast<float>(center[1]) == doctest::Approx(level(motion_v)).epsilon(0.03));
        };
        // The first frame has no last frame: no motion.
        check_center(0.0f, 0.0f);
        // The camera steps 0.1 m right: what it sees moves left.
        frame.camera.transform[3] = 0.1f;
        check_center(-0.1f * uv_per_m, 0.0f);
        // Still: no motion, whatever it was before.
        check_center(0.0f, 0.0f);
        // Up 0.1 m: what it sees moves down the screen, and v grows down.
        frame.camera.transform[7] = 0.1f;
        check_center(0.0f, 0.1f * uv_per_m);
        check_center(0.0f, 0.0f);
        // The square moved 0.2 m right since last frame, and 0.1 m up.
        frame.instances[0].transform[3] = 0.2f;
        frame.instances[0].transform[7] = 0.1f;
        check_center(0.2f * uv_per_m, -0.1f * uv_per_m);
        frame.instances[0] = square;
        check_center(0.0f, 0.0f); // the camera didn't move, and the square says it didn't
        // A cut: the camera jumped, and this frame says not to look back.
        frame.camera.transform[3] = 40.0f;
        frame.camera.previous_valid = false;
        check_center(0.0f, 0.0f);
        frame.camera.transform[3] = 0.1f;
        frame.camera.previous_valid = false;
        check_center(0.0f, 0.0f);
        // A surface that was behind last frame's camera (past the square, looking away from it): no
        // previous position. A motion off the screen, down and right, whatever its clip w's sign said.
        frame.camera.transform[11] = -4.0f;
        static_cast<void>(shown());
        frame.camera.transform[11] = 3.0f;
        const de::core::Array<std::byte> behind = shown();
        CHECK(texel(behind, kSize / 2, kSize / 2)[0] == 255);
        CHECK(texel(behind, kSize / 2, kSize / 2)[1] == 255);
        check_center(0.0f, 0.0f);

        frame.instances.clear();
        static_cast<void>(shown());
        assets->release(mesh);
        assets->update();
        device->wait_idle();
        device->destroy(*target);
    }
    device->wait_idle();
    de::test::check_clean(*device);
}

TEST_CASE("TAA smooths edges and stays still, and moving things leave no ghost") {
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    const de::test::Sandbox sandbox;
    de::core::JobSystem jobs({.worker_count = 2});
    const de::platform::PathString game_root = sandbox.game();
    auto vfs = de::platform::Vfs::create(
        {.game_root = game_root.c_str(), .src_root = DE_SOURCE_DIR, .jobs = &jobs, .game_writable = true});
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
        const de::asset::AssetHandle wall_mesh = assets->load<de::asset::MeshAsset>(kWall);
        const de::asset::AssetHandle green_mesh = assets->load<de::asset::MeshAsset>(kPureGreen);
        const de::asset::AssetHandle bad_mesh = assets->load<de::asset::MeshAsset>(kBad);
        assets->wait_all();
        gpu_assets->update();
        uploader->flush_and_wait();
        gpu_assets->update();
        REQUIRE(gpu_assets->mesh(mesh) != nullptr);
        REQUIRE(gpu_assets->mesh(wall_mesh) != nullptr);
        REQUIRE(gpu_assets->mesh(green_mesh) != nullptr);
        REQUIRE(gpu_assets->mesh(bad_mesh) != nullptr);
        const auto target =
            device->create_texture({.width = kSize,
                                    .height = kSize,
                                    .format = de::rhi::Format::RGBA8Unorm,
                                    .usage = de::rhi::TextureUsage::RenderTarget | de::rhi::TextureUsage::CopySrc,
                                    .debug_name = "target"});
        REQUIRE(target.ok());
        auto graph = de::render::RenderGraph::create(*device);
        REQUIRE(graph.ok());

        // A red square 3 m away over the sky, the sun behind the camera.
        de::world::RenderFrame frame;
        frame.camera = {.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 3}, .vertical_fov_rad = 1.0f, .valid = true};
        const de::world::RenderInstance square{.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0},
                                               .previous_transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0},
                                               .mesh = mesh};
        de::render::SceneLighting light{.sun_direction = {0, 0, 1}};
        const auto draw = [&](bool taa) {
            device->begin_frame();
            uploader->record();
            graph->begin(kSize, kSize);
            const de::render::GraphTexture color = graph->import_texture(
                {.texture = *target, .name = "target", .final_state = de::rhi::ResourceState::CopySrc});
            ++frame.number; // as extraction does
            renderer->add_passes(*graph, color, de::rhi::Format::RGBA8Unorm, frame, light, de::render::SceneView::Lit,
                                 taa);
            graph->execute();
            device->end_frame();
            // As extraction does: what was drawn is the next frame's previous view.
            std::memcpy(frame.camera.previous_transform, frame.camera.transform, sizeof(frame.camera.transform));
            frame.camera.previous_vertical_fov_rad = frame.camera.vertical_fov_rad;
            frame.camera.previous_near_m = frame.camera.near_m;
            frame.camera.previous_valid = true;
            return de::test::read_texture(*device, *target);
        };
        const auto settle = [&] {
            for (int i = 0; i < 40; ++i) {
                static_cast<void>(draw(true));
            }
        };
        // The pixel column of a world x at the square's distance, on a row through the square, away
        // from the horizon (which TAA smooths too).
        constexpr uint32_t kRow = kSize / 2 - 8;
        const auto column = [](float world_x, float camera_x = 0.0f) {
            const float half = static_cast<float>(kSize) * 0.5f;
            return static_cast<uint32_t>(half + (world_x - camera_x) / (3.0f * std::tan(0.5f)) * half);
        };
        const auto red = [&](const de::core::Array<std::byte>& pixels, uint32_t x) {
            return static_cast<int>(texel(pixels, x, kRow)[0]);
        };

        // What's what, without TAA: the sky alone, the square alone. An edge is one or the other.
        const de::core::Array<std::byte> sky_only = draw(false);
        const int sky_red = red(sky_only, column(-0.6f));
        frame.instances.push(square);
        const de::core::Array<std::byte> sharp = draw(false);
        const int square_red = red(sharp, column(0.0f));
        REQUIRE(square_red > sky_red + 100);
        const uint32_t edge = column(1.0f); // the square's right edge runs through this column
        const int sharp_edge = red(sharp, edge);
        CHECK((std::abs(sharp_edge - square_red) < 6 || std::abs(sharp_edge - sky_red) < 6));

        // With TAA, settled: the edge is between the two, inside and outside are as they were, and it
        // holds still from frame to frame.
        settle();
        const de::core::Array<std::byte> settled = draw(true);
        CHECK(red(settled, edge) > sky_red + 15);
        CHECK(red(settled, edge) < square_red - 15);
        CHECK(std::abs(red(settled, column(0.0f)) - square_red) < 6);
        CHECK(std::abs(red(settled, column(1.5f)) - sky_red) < 6);
        const de::core::Array<std::byte> next = draw(true);
        CHECK(std::abs(red(next, edge) - red(settled, edge)) <= 12);

        // A big jump: 0.8 m in a frame. Where it was is sky at once, and it's at its new place at once.
        frame.instances[0].transform[3] = 0.8f;
        frame.instances[0].previous_transform[3] = 0.0f;
        const de::core::Array<std::byte> jumped = draw(true);
        CHECK(std::abs(red(jumped, column(-0.6f)) - sky_red) < 8);
        CHECK(std::abs(red(jumped, column(-0.3f)) - sky_red) < 8);
        CHECK(std::abs(red(jumped, column(1.5f)) - square_red) < 30);
        frame.instances[0] = square;
        settle();

        // Steady motion, a third of a pixel to a few pixels a frame: the classic trail is behind the
        // trailing edge. Two pixels behind it must be sky, every frame; well inside it, the square.
        const float pixel_m = 3.0f * std::tan(0.5f) * 2.0f / static_cast<float>(kSize);
        for (const float pixels_per_frame : {0.33f, 1.0f, 3.0f}) {
            frame.instances[0] = square;
            settle();
            float x = 0.0f;
            for (int i = 0; i < 8; ++i) {
                const float step = pixels_per_frame * pixel_m;
                frame.instances[0].previous_transform[3] = x;
                x += step;
                frame.instances[0].transform[3] = x;
                const de::core::Array<std::byte> moved = draw(true);
                CAPTURE(pixels_per_frame);
                CAPTURE(i);
                CHECK(std::abs(red(moved, column(x - 1.0f - pixel_m * 2.0f)) - sky_red) < 12);
                CHECK(std::abs(red(moved, column(x)) - square_red) < 12);
            }
        }

        // It stops, and the image settles where it stopped: no slide, no trail.
        frame.instances[0].previous_transform[3] = frame.instances[0].transform[3];
        settle();
        const float stopped_at = frame.instances[0].transform[3];
        const de::core::Array<std::byte> stopped = draw(true);
        CHECK(std::abs(red(stopped, column(stopped_at - 1.0f - pixel_m * 2.0f)) - sky_red) < 6);

        // The camera pans, 2 pixels a frame, past the still square: same for what it uncovers.
        frame.instances[0] = square;
        settle();
        float panned_x = 0.0f;
        for (int i = 0; i < 6; ++i) {
            panned_x += pixel_m * 2.0f;
            frame.camera.transform[3] = panned_x;
            const de::core::Array<std::byte> panned = draw(true);
            CAPTURE(i);
            CHECK(std::abs(red(panned, column(1.0f + pixel_m * 2.0f, panned_x)) - sky_red) < 12);
            CHECK(std::abs(red(panned, column(0.0f, panned_x)) - square_red) < 12);
        }

        // Its silhouette through the pan: halfway through a pixel, an edge stays between the two
        // colours whatever the jitter, at once when the camera starts (its speed is nothing to history)
        // and on: the ring of sky around it reads history as well as the square does.
        frame.camera.transform[3] = 0.0f;
        frame.camera.previous_valid = false;
        frame.instances[0].transform[3] = 0.0f;
        frame.instances[0].previous_transform[3] = 0.0f;
        const float half_pixel_x =
            (static_cast<float>(edge) + 0.5f - static_cast<float>(kSize) * 0.5f) * pixel_m - 1.0f;
        frame.instances[0].transform[3] = half_pixel_x;
        frame.instances[0].previous_transform[3] = half_pixel_x;
        settle();
        int edge_lowest = 255;
        int edge_highest = 0;
        for (int i = 1; i <= 8; ++i) {
            frame.camera.transform[3] = pixel_m * 2.0f * static_cast<float>(i);
            const de::core::Array<std::byte> panning = draw(true);
            const int value = red(panning, edge - static_cast<uint32_t>(i) * 2);
            edge_lowest = std::min(edge_lowest, value);
            edge_highest = std::max(edge_highest, value);
        }
        CHECK(edge_lowest > sky_red + 15);
        CHECK(edge_highest < square_red - 15);
        CHECK(edge_highest - edge_lowest < 25);

        // The camera stutters, 8 pixels one way and back every frame, over a world that isn't moving:
        // that costs the world no history at all, since none of the motion is the world's.
        frame.camera.transform[3] = 0.0f;
        frame.camera.previous_valid = false;
        settle();
        edge_lowest = 255;
        edge_highest = 0;
        for (uint32_t i = 1; i <= 16; ++i) {
            const uint32_t away = i % 2 == 0 ? 0 : 8;
            frame.camera.transform[3] = pixel_m * static_cast<float>(away);
            const int value = red(draw(true), edge - away);
            edge_lowest = std::min(edge_lowest, value);
            edge_highest = std::max(edge_highest, value);
        }
        CHECK(edge_highest - edge_lowest < 25);

        // Now a dark wall with a red strip on it, about the same depth: next to the strip's edge the old
        // colour is still among the neighbours, and depth says nothing, so neither clip nor the depth
        // rule can help; each rule below has to do its part alone. Edges sit on a pixel boundary, so the
        // jitter (under half a pixel) can't move them over a pixel centre.
        const auto boundary = [&](int pixel, float camera_x = 0.0f, float distance_m = 3.0f) {
            return camera_x +
                   (static_cast<float>(pixel) - static_cast<float>(kSize) * 0.5f) * pixel_m * distance_m / 3.0f;
        };
        const auto thing = [](de::asset::AssetHandle what, float x, float z, float scale = 1.0f) {
            return de::world::RenderInstance{.transform = {scale, 0, 0, x, 0, scale, 0, 0, 0, 0, 1, z},
                                             .previous_transform = {scale, 0, 0, x, 0, scale, 0, 0, 0, 0, 1, z},
                                             .mesh = what};
        };
        const auto look_from = [&](float camera_x) { frame.camera.transform[3] = camera_x; };
        constexpr int kEdge = 52; // the strip's right edge, at first
        const float strip_right = boundary(kEdge);
        const auto strip_on_wall = [&](float right_edge) {
            frame.instances.clear();
            frame.instances.push(thing(wall_mesh, 0.0f, -0.005f, 20.0f));
            frame.instances.push(thing(mesh, right_edge - 1.0f, 0.0f));
        };
        const auto level = [&](const de::core::Array<std::byte>& pixels, int x) {
            return red(pixels, static_cast<uint32_t>(x));
        };
        look_from(0.0f);
        strip_on_wall(strip_right);
        settle();
        const de::core::Array<std::byte> wall_settled = draw(true);
        const int wall_red = level(wall_settled, kEdge + 4);
        const int strip_red = level(wall_settled, kEdge - 4);
        REQUIRE(strip_red > wall_red + 100);
        const int kept = (wall_red + strip_red) / 2; // over this, the old colour has stayed
        CHECK(level(wall_settled, kEdge + 1) < kept);
        CHECK(level(wall_settled, kEdge - 2) > kept);

        // A cut, two pixels aside: nothing of before stays, though its colour is right there.
        look_from(boundary(kEdge) - boundary(kEdge - 2));
        frame.camera.previous_valid = false;
        const de::core::Array<std::byte> cut = draw(true);
        CHECK(level(cut, kEdge - 2) < kept);
        CHECK(level(cut, kEdge - 2) > wall_red + 15); // a soft blur: not raw aliasing

        // TAA off while the camera moves, then on: the same, across the gap.
        look_from(0.0f);
        frame.camera.previous_valid = false;
        settle();
        look_from(boundary(kEdge) - boundary(kEdge - 2));
        static_cast<void>(draw(false));
        const de::core::Array<std::byte> back_on = draw(true);
        CHECK(level(back_on, kEdge - 2) < kept);

        // The camera pans over it: history comes from where the point was, so what the edge left is wall.
        const auto pan_to = [&](float camera_x) {
            look_from(camera_x);
            return draw(true);
        };
        look_from(0.0f);
        frame.camera.previous_valid = false;
        strip_on_wall(boundary(kEdge + 9));
        settle();
        de::core::Array<std::byte> panned_over;
        for (int i = 1; i <= 3; ++i) {
            panned_over = pan_to(pixel_m * 3.0f * static_cast<float>(i));
        }
        CHECK(level(panned_over, kEdge) < kept);
        CHECK(level(panned_over, kEdge - 2) > kept);

        // Panning the strip in from off the screen: the new pixel has no history, whatever the clamped
        // sample at the screen's edge says.
        constexpr int kLast = static_cast<int>(kSize) - 1;
        look_from(0.0f);
        frame.camera.previous_valid = false;
        strip_on_wall(boundary(kLast + 8) + 2.0f); // its left edge: 2 pixels a frame, 4 frames
        settle();
        de::core::Array<std::byte> entered;
        for (int i = 1; i <= 4; ++i) {
            entered = pan_to(pixel_m * 2.0f * static_cast<float>(i));
        }
        CHECK(level(entered, kLast) > kept);

        // Something in front, gone in a frame without moving: the depth there says it wasn't this.
        look_from(0.0f);
        frame.camera.previous_valid = false;
        strip_on_wall(strip_right);
        frame.instances.push(thing(mesh, strip_right, 1.0f));
        settle();
        frame.instances.pop();
        const de::core::Array<std::byte> gone = draw(true);
        CHECK(level(gone, kEdge) < kept);
        // And in a frame it's there, its edge at once.
        settle();
        frame.instances.push(thing(mesh, boundary(kEdge + 4, 0.0f, 2.0f) + 1.0f, 1.0f));
        const de::core::Array<std::byte> came = draw(true);
        CHECK(level(came, kEdge + 4) > kept);
        frame.instances.pop();

        // Another red thing, about the same depth, leaves the strip behind, 3 pixels a frame. The
        // motion there last frame says the history belonged to it.
        const float nearer = 0.005f;
        settle();
        frame.instances.push(thing(mesh, boundary(kEdge - 13, 0.0f, 3.0f - nearer) + 1.0f, nearer));
        settle();
        for (int trailing = kEdge - 10; trailing <= kEdge + 2; trailing += 3) {
            de::world::RenderInstance& other = frame.instances[frame.instances.size() - 1];
            other.previous_transform[3] = other.transform[3];
            other.transform[3] = boundary(trailing, 0.0f, 3.0f - nearer) + 1.0f;
            const de::core::Array<std::byte> left = draw(true);
            if (trailing == kEdge + 2) {
                CHECK(level(left, kEdge) < kept);
                CHECK(level(left, kEdge) > wall_red + 50); // softened with the history gone: not raw aliasing
            }
        }
        frame.instances.pop();

        // A pixel behind a moving edge takes the edge's motion, a pixel a frame: where it reads history
        // it was wall too. By its own motion (none), it'd read the thing that was just there.
        frame.instances.clear();
        frame.instances.push(thing(wall_mesh, 0.0f, -0.005f, 20.0f));
        frame.instances.push(thing(mesh, boundary(10, 0.0f, 3.0f - nearer) + 1.0f, nearer));
        settle();
        de::core::Array<std::byte> behind;
        for (int trailing = 11; trailing <= 24; ++trailing) {
            de::world::RenderInstance& other = frame.instances[1];
            other.previous_transform[3] = other.transform[3];
            other.transform[3] = boundary(trailing, 0.0f, 3.0f - nearer) + 1.0f;
            behind = draw(true);
        }
        CHECK(level(behind, 23) < kept);

        // Two things moving opposite ways at the same speed: the speed alone says nothing. A dark thing
        // moves left, out from under a red one moving right; a still red strip behind it keeps the old
        // colour among the neighbours.
        const float dark_z = 0.002f;
        const float red_z = 0.004f;
        constexpr int kLeading = 30; // the dark thing's left edge, at the end
        frame.instances.clear();
        frame.instances.push(thing(wall_mesh, 0.0f, -0.005f, 20.0f));
        frame.instances.push(thing(mesh, boundary(kLeading + 12) - 1.0f, 0.0f));
        frame.instances.push(thing(wall_mesh, boundary(kLeading + 12, 0.0f, 3.0f - dark_z) + 1.0f, dark_z));
        frame.instances.push(thing(mesh, boundary(kLeading - 7, 0.0f, 3.0f - red_z) + 1.0f, red_z));
        settle();
        de::core::Array<std::byte> crossed;
        for (int step = 3; step >= 0; --step) {
            de::world::RenderInstance& dark = frame.instances[2];
            de::world::RenderInstance& bright = frame.instances[3];
            dark.previous_transform[3] = dark.transform[3];
            dark.transform[3] = boundary(kLeading + step * 3, 0.0f, 3.0f - dark_z) + 1.0f;
            bright.previous_transform[3] = bright.transform[3];
            bright.transform[3] = boundary(kLeading + 5 - step * 3, 0.0f, 3.0f - red_z) + 1.0f;
            crossed = draw(true);
        }
        CHECK(level(crossed, kLeading) < kept);

        // The sky, the camera pitching 2 pixels a frame. Nothing writes the sky's motion: the horizon
        // moves by the camera's alone, with no trail, and history holds through the turn, so a horizon
        // halfway through a pixel doesn't flicker with the jitter.
        frame.instances.clear();
        const auto pitch_to = [&](float row) {
            const float half = static_cast<float>(kSize) * 0.5f;
            const float angle = std::atan((row - half) / half * std::tan(0.5f));
            frame.camera.transform[5] = std::cos(angle);
            frame.camera.transform[6] = -std::sin(angle);
            frame.camera.transform[9] = std::sin(angle);
            frame.camera.transform[10] = std::cos(angle);
            return draw(true);
        };
        look_from(0.0f);
        frame.camera.previous_valid = false;
        static_cast<void>(pitch_to(32.5f));
        settle();
        int lowest = 255;
        int highest = 0;
        for (uint32_t row = 34; row <= 48; row += 2) {
            const de::core::Array<std::byte> pitched = pitch_to(static_cast<float>(row) + 0.5f);
            CAPTURE(row);
            CHECK(static_cast<int>(texel(pitched, kSize / 2, row - 1)[0]) > 90); // sky at once
            CHECK(static_cast<int>(texel(pitched, kSize / 2, row + 1)[0]) < 40); // the ground at once
            lowest = std::min(lowest, static_cast<int>(texel(pitched, kSize / 2, row)[0]));
            highest = std::max(highest, static_cast<int>(texel(pitched, kSize / 2, row)[0]));
        }
        CHECK(highest - lowest < 25);
        // A fast turn, away from the middle of the screen: the sky is the sky, however it turned.
        lowest = 255;
        highest = 0;
        frame.camera.previous_valid = false;
        static_cast<void>(pitch_to(32.5f));
        settle();
        for (uint32_t row = 40; row <= 56; row += 8) {
            const de::core::Array<std::byte> pitched = pitch_to(static_cast<float>(row) + 0.5f);
            lowest = std::min(lowest, static_cast<int>(texel(pitched, 4, row)[0]));
            highest = std::max(highest, static_cast<int>(texel(pitched, 4, row)[0]));
        }
        CHECK(highest - lowest < 25);
        frame.camera.transform[5] = 1.0f;
        frame.camera.transform[6] = 0.0f;
        frame.camera.transform[9] = 0.0f;
        frame.camera.transform[10] = 1.0f;
        frame.camera.previous_valid = false;

        // The camera snaps around with no cut, a quarter turn and more: points that were behind it, or
        // exactly beside it, have no history, and nothing of the square stays or breaks.
        frame.instances.clear();
        frame.instances.push(square);
        settle();
        for (const float turn : {1.5707963f, -0.7853982f}) {
            frame.camera.transform[0] = std::cos(turn);
            frame.camera.transform[2] = std::sin(turn);
            frame.camera.transform[8] = -std::sin(turn);
            frame.camera.transform[10] = std::cos(turn);
            const de::core::Array<std::byte> snapped = draw(true);
            for (const uint32_t x : {4U, kSize / 2, kSize - 4}) {
                CAPTURE(x);
                CHECK(std::abs(static_cast<int>(texel(snapped, x, kRow)[0]) - sky_red) < 12);
            }
        }
        frame.camera.transform[5] = 1.0f;
        frame.camera.transform[6] = 0.0f;
        frame.camera.transform[9] = 0.0f;
        frame.camera.transform[10] = 1.0f;
        frame.camera.previous_valid = false;

        // Past a quarter turn every point on the screen was behind last frame's camera, and dividing by
        // its w would mirror it back on the screen: the sky above the horizon would read the ground.
        frame.instances.clear();
        const auto yaw_to = [&](float turn) {
            frame.camera.transform[0] = std::cos(turn);
            frame.camera.transform[2] = std::sin(turn);
            frame.camera.transform[8] = -std::sin(turn);
            frame.camera.transform[10] = std::cos(turn);
        };
        yaw_to(1.4f);
        frame.camera.previous_valid = false;
        settle();
        yaw_to(-1.4f);
        const de::core::Array<std::byte> turned_back = draw(true);
        CHECK(static_cast<int>(texel(turned_back, kSize / 2, kSize / 2 - 1)[0]) > 80);
        yaw_to(0.0f);
        frame.camera.previous_valid = false;

        // Pure green on dark green, in white light: no red or blue anywhere, so the colour box has no
        // extent on one axis. History still holds: a pixel inside the edge is the green, not the 3 x 3's mean.
        for (float* color : {light.sun_color, light.sky_color, light.ground_color, light.shadow_color}) {
            color[0] = color[1] = color[2] = 1.0f;
        }
        frame.instances.clear();
        frame.instances.push(thing(wall_mesh, 0.0f, -0.005f, 20.0f));
        frame.instances.push(thing(green_mesh, boundary(kEdge) - 1.0f, 0.0f));
        settle();
        const de::core::Array<std::byte> single_hue = draw(true);
        const int inside_edge = texel(single_hue, kEdge - 1, kRow)[1];
        const int well_inside = texel(single_hue, kEdge - 4, kRow)[1];
        CHECK(std::abs(inside_edge - well_inside) < 6);
        light = {.sun_direction = {0, 0, 1}};

        // A wire a fifth of a pixel wide, nothing moving: the jitter hits it now and then, and when it
        // does that isn't something that came in front. It shows faintly, it doesn't flash.
        frame.instances.clear();
        frame.instances.push(thing(wall_mesh, 0.0f, -0.005f, 20.0f));
        // Half a meter nearer than the wall: well outside the depth slack.
        de::world::RenderInstance wire = thing(mesh, boundary(40, 0.0f, 2.5f) + pixel_m * 0.5f * 2.5f / 3.0f, 0.5f);
        wire.transform[0] = pixel_m * 0.1f * 2.5f / 3.0f;
        wire.previous_transform[0] = wire.transform[0];
        frame.instances.push(wire);
        settle();
        int wire_highest = 0;
        for (int i = 0; i < 32; ++i) {
            wire_highest = std::max(wire_highest, level(draw(true), 40));
        }
        CHECK(wire_highest < kept);

        // The camera follows something at about 20 pixels a frame, with the frame rate wobbling. It
        // stays put on the screen: its history is good, its edge stays smooth.
        frame.instances.clear();
        frame.instances.push(thing(mesh, half_pixel_x, 0.0f));
        look_from(0.0f);
        frame.camera.previous_valid = false;
        settle();
        float followed_x = 0.0f;
        const auto follow = [&](float pixels) {
            followed_x += pixels * pixel_m;
            look_from(followed_x);
            de::world::RenderInstance& hero = frame.instances[0];
            hero.previous_transform[3] = hero.transform[3];
            hero.transform[3] = half_pixel_x + followed_x;
            return draw(true);
        };
        for (int i = 0; i < 4; ++i) {
            static_cast<void>(follow(20.0f));
        }
        int hero_lowest = 255;
        int hero_highest = 0;
        for (int i = 0; i < 8; ++i) {
            const int value = red(follow(i % 2 == 0 ? 21.0f : 19.0f), edge);
            hero_lowest = std::min(hero_lowest, value);
            hero_highest = std::max(hero_highest, value);
        }
        CHECK(hero_highest - hero_lowest < 70);
        CHECK(hero_lowest > sky_red + 15);
        CHECK(hero_highest < square_red - 15);

        // Something coming straight at the camera, 7% nearer a frame, its edge on the ray through a
        // pixel: hardly any motion on the screen, but that isn't only the camera. Its edge stays smooth.
        frame.instances.clear();
        const float ray_x = pixel_m * 0.5f / 3.0f;
        frame.instances.push(thing(mesh, ray_x * 3.0f - 1.0f, 0.0f));
        frame.instances.push(thing(wall_mesh, ray_x * 3.0f + 1.0f, 0.0f));
        look_from(0.0f);
        frame.camera.previous_valid = false;
        settle();
        float approach_m = 3.0f;
        int coming_lowest = 255;
        int coming_highest = 0;
        for (int i = 0; i < 8; ++i) {
            approach_m *= 0.93f;
            const float edge_x = ray_x * approach_m;
            frame.instances[0].previous_transform[3] = frame.instances[0].transform[3];
            frame.instances[0].previous_transform[11] = frame.instances[0].transform[11];
            frame.instances[0].transform[3] = edge_x - 1.0f;
            frame.instances[0].transform[11] = 3.0f - approach_m;
            frame.instances[1].previous_transform[3] = frame.instances[1].transform[3];
            frame.instances[1].previous_transform[11] = frame.instances[1].transform[11];
            frame.instances[1].transform[3] = edge_x + 1.0f;
            frame.instances[1].transform[11] = 3.0f - approach_m;
            const de::core::Array<std::byte> coming = draw(true);
            const int value = static_cast<int>(texel(coming, kSize / 2, kSize / 2 - 4)[0]);
            coming_lowest = std::min(coming_lowest, value);
            coming_highest = std::max(coming_highest, value);
        }
        CHECK(coming_highest - coming_lowest < 60);

        // One bad pixel's worth of NaN stays where it is: the pixels around a NaN thing keep their
        // colour, they don't go black with it.
        frame.instances.clear();
        frame.instances.push(thing(wall_mesh, 0.0f, -0.005f, 20.0f));
        frame.instances.push(thing(bad_mesh, boundary(kEdge) - 1.0f, 0.0f));
        settle();
        const de::core::Array<std::byte> poisoned = draw(true);
        CHECK(static_cast<int>(texel(poisoned, kEdge, kRow)[1]) >
              static_cast<int>(texel(poisoned, kEdge + 3, kRow)[1]) / 2);

        // The sun's disk drifting a third of a pixel a frame: its history is filtered tonemapped, so its
        // brightness doesn't ring into the sky a few pixels off.
        frame.instances.clear();
        look_from(0.0f);
        light = {.sun_direction = {0.0f, 0.2f, -1.0f}};
        frame.camera.previous_valid = false;
        settle();
        int off_disk_lowest = 255;
        int off_disk_highest = 0;
        for (int i = 1; i <= 48; ++i) {
            yaw_to(-std::atan(static_cast<float>(i) * 0.37f / (static_cast<float>(kSize) * 0.5f) * std::tan(0.5f)));
            const de::core::Array<std::byte> drifting = draw(true);
            // The disk's first pixel, along the rows it could be on.
            int brightest = 0;
            uint32_t disk_x = 0;
            uint32_t disk_y = 0;
            for (uint32_t y = 10; y < 30; ++y) {
                for (uint32_t x = 2; x + 2 < kSize; ++x) {
                    const int here = texel(drifting, x, y)[1];
                    if (here > brightest) {
                        brightest = here;
                        disk_x = x;
                        disk_y = y;
                    }
                }
            }
            if (i > 8 && disk_x > 4 && disk_x + 4 < kSize) {
                const int value = texel(drifting, disk_x + 3, disk_y)[1];
                off_disk_lowest = std::min(off_disk_lowest, value);
                off_disk_highest = std::max(off_disk_highest, value);
            }
        }
        CHECK(off_disk_highest - off_disk_lowest < 45);
        light = {.sun_direction = {0, 0, 1}};
        yaw_to(0.0f);
        frame.camera.previous_valid = false;

        // Another size: new history, nothing breaks.
        device->begin_frame();
        uploader->record();
        graph->begin(kSize / 2, kSize / 2);
        const de::render::GraphTexture color = graph->import_texture(
            {.texture = *target, .name = "target", .final_state = de::rhi::ResourceState::CopySrc});
        renderer->add_passes(*graph, color, de::rhi::Format::RGBA8Unorm, frame, {}, de::render::SceneView::Lit, true);
        graph->execute();
        device->end_frame();

        frame.instances.clear();
        assets->release(mesh);
        assets->release(wall_mesh);
        assets->release(green_mesh);
        assets->release(bad_mesh);
        assets->update();
        device->wait_idle();
        device->destroy(*target);
    }
    device->wait_idle();
    de::test::check_clean(*device);
}
