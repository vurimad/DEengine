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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

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

        // The camera at the origin looking down -Z, the sun straight overhead.
        de::world::RenderFrame frame;
        frame.camera = {.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}, .vertical_fov_rad = 1.0f, .valid = true};
        const auto render = [&](const de::render::SceneFog& fog) {
            device->begin_frame();
            uploader->record();
            graph->begin(kSize, kSize);
            const de::render::GraphTexture color = graph->import_texture(
                {.texture = *target, .name = "target", .final_state = de::rhi::ResourceState::CopySrc});
            de::render::SceneLighting light{.sun_direction = {0, 1, 0}};
            light.fog = fog;
            renderer->add_passes(*graph, color, de::rhi::Format::RGBA8Unorm, frame, light, de::render::SceneView::Lit);
            graph->execute();
            device->end_frame();
            return de::test::read_texture(*device, *target);
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
        // The grid's two passes run only with one.
        CHECK(graph->stats().culled_passes == 0);
        const uint32_t with_grid = graph->stats().passes;
        static_cast<void>(render(closed));
        CHECK(graph->stats().passes == with_grid - 2);

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
