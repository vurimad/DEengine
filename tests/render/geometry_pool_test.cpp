#include "asset/material_format.h"
#include "asset/mesh_format.h"
#include "asset/registry.h"
#include "cooker/material_cooker.h"
#include "cooker/mesh_cooker.h"
#include "core/job_system.h"
#include "render/graph/render_graph.h"
#include "render/rhi/device.h"
#include "render/rhi/rhi.h"
#include "render/rhi/upload.h"
#include "render/scene/gpu_assets.h"
#include "render/scene/scene_renderer.h"
#include "render/shader/shader_compiler.h"
#include "render/shader/shader_library.h"
#include "support/gpu.h"
#include "support/readback.h"
#include "support/sandbox.h"
#include "world/render_extract.h"
#include "world/transform.h"
#include "world/world.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <span>

using de::asset::AssetHandle;
using de::asset::AssetId;
using de::asset::AssetType;
using de::asset::MeshAsset;
using de::render::GpuMesh;

namespace {

constexpr AssetId kRed = AssetId::from_value(0x71);
constexpr AssetId kGreen = AssetId::from_value(0x72);
constexpr AssetId kCenter = AssetId::from_value(0x73); // a square in the middle of the view
constexpr AssetId kSides = AssetId::from_value(0x74);  // a square either side of the middle
constexpr AssetId kGrid = AssetId::from_value(0x75);
constexpr uint32_t kSize = 64;
// The view from 3 m in front of the origin: about 19.5 pixels per meter, so X = +-1 m lands here.
constexpr uint32_t kMiddle = kSize / 2;
constexpr uint32_t kLeft = kMiddle - 20;
constexpr uint32_t kRight = kMiddle + 20;
constexpr uint64_t kSquareVertices = 4;
constexpr uint64_t kSquareIndices = 6;

void put_mesh(const de::platform::Vfs& vfs, de::cooker::ModelData& model, AssetId mesh_id, AssetId material,
              uint32_t lod_count = 1) {
    static_cast<void>(model.materials.emplace());
    de::cooker::Diagnostics diagnostics;
    const AssetId materials[] = {material};
    const auto mesh = de::cooker::cook_mesh(model, materials, {.lod_count = lod_count}, mesh_id, "test", diagnostics);
    REQUIRE(mesh.ok());
    REQUIRE(vfs.write_file(de::asset::cooked_path(mesh_id, AssetType::Mesh).c_str(), mesh->span()).ok());
}

// Squares 1 m wide in the XY plane, one per center on X, facing +Z.
void put_squares(const de::platform::Vfs& vfs, AssetId mesh_id, AssetId material,
                 std::initializer_list<float> centers) {
    de::cooker::ModelData model;
    de::cooker::ModelPrimitive& primitive = model.primitives.emplace();
    primitive.material = 0;
    for (const float center : centers) {
        const auto first = static_cast<uint32_t>(primitive.vertices.size());
        const float corners[4][2] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
        for (const auto& corner : corners) {
            de::cooker::ModelVertex& vertex = primitive.vertices.emplace();
            vertex.position[0] = center + corner[0];
            vertex.position[1] = corner[1];
            vertex.normal[1] = 0.0f;
            vertex.normal[2] = 1.0f;
        }
        for (const uint32_t index : {0U, 1U, 2U, 0U, 2U, 3U}) {
            primitive.indices.push(first + index);
        }
    }
    put_mesh(vfs, model, mesh_id, material);
}

// A grid 2 m wide with bumps in it, which simplifies into coarser LODs that lie further from it each time.
void put_grid(const de::platform::Vfs& vfs, AssetId mesh_id, AssetId material, float bump_m = 0.05f,
              uint32_t lod_count = 4) {
    constexpr uint32_t kCells = 32;
    de::cooker::ModelData model;
    de::cooker::ModelPrimitive& primitive = model.primitives.emplace();
    primitive.material = 0;
    for (uint32_t y = 0; y <= kCells; ++y) {
        for (uint32_t x = 0; x <= kCells; ++x) {
            de::cooker::ModelVertex& vertex = primitive.vertices.emplace();
            vertex.position[0] = (static_cast<float>(x) / kCells * 2.0f) - 1.0f;
            vertex.position[1] = (static_cast<float>(y) / kCells * 2.0f) - 1.0f;
            vertex.position[2] = bump_m * std::sin(vertex.position[0] * 7.0f) * std::sin(vertex.position[1] * 5.0f);
            vertex.normal[1] = 0.0f;
            vertex.normal[2] = 1.0f;
        }
    }
    for (uint32_t y = 0; y < kCells; ++y) {
        for (uint32_t x = 0; x < kCells; ++x) {
            const uint32_t a = (y * (kCells + 1)) + x;
            const uint32_t b = a + kCells + 1;
            for (const uint32_t index : {a, a + 1, b + 1, a, b + 1, b}) {
                primitive.indices.push(index);
            }
        }
    }
    put_mesh(vfs, model, mesh_id, material, lod_count);
}

void put_material(const de::platform::Vfs& vfs, AssetId id, float red, float green) {
    de::cooker::MaterialBuild material;
    material.set_param(de::asset::material::kBaseColor, red, green, 0.0f, 1.0f);
    const auto cooked = de::cooker::cook_material(material, id);
    REQUIRE(vfs.write_file(de::asset::cooked_path(id, AssetType::Material).c_str(), cooked.span()).ok());
}

struct PoolOptions {
    uint32_t pool_vertices = 1024;
    uint32_t pool_indices = 1024;
    uint32_t max_meshes = 64;
    uint32_t max_parts = 1024;
    uint32_t max_lods = 1024;
    uint32_t max_instances = 1024;
    uint64_t frame_budget_bytes = uint64_t{16} << 20U;
    bool hot_reload = false;
};

// What a test sees of a scene with a geometry pool.
struct PoolScene {
    de::rhi::Device& device;
    de::platform::Vfs& vfs;
    de::rhi::Uploader& uploader;
    de::asset::AssetRegistry& assets;
    de::render::GpuAssets& gpu_assets;
    de::render::SceneRenderer& renderer;
    de::render::RenderGraph& graph;
    de::rhi::TextureHandle target;

    // A mesh, loaded and on its way: it's in the pool after this returns, whatever else is loading.
    AssetHandle load(AssetId id) const {
        const AssetHandle handle = assets.load<MeshAsset>(id);
        assets.wait_all();
        assets.update();
        gpu_assets.update();
        return handle;
    }

    // The engine's frame, with only what the uploader records in it.
    void empty_frame() const {
        assets.update();
        assets.wait_all();
        gpu_assets.update();
        device.begin_frame();
        uploader.record();
        device.end_frame();
    }

    // A frame of these meshes at the origin.
    static de::world::RenderFrame frame_of(std::initializer_list<AssetHandle> meshes) {
        de::world::RenderFrame frame;
        frame.camera = {.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 3}, .vertical_fov_rad = 1.0f, .valid = true};
        for (const AssetHandle mesh : meshes) {
            frame.instances.push({.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}, .mesh = mesh});
        }
        return frame;
    }

    // The engine's frame, drawing these meshes at the origin, left in flight.
    void submit(std::initializer_list<AssetHandle> meshes) const { submit(frame_of(meshes)); }

    void submit(const de::world::RenderFrame& frame, const de::render::SceneLighting& lighting = {},
                de::render::SceneView view = de::render::SceneView::BaseColor) const {
        assets.update();
        assets.wait_all();
        gpu_assets.update();
        device.begin_frame();
        uploader.record();
        graph.begin(kSize, kSize);
        const de::render::GraphTexture color =
            graph.import_texture({.texture = target, .name = "target", .final_state = de::rhi::ResourceState::CopySrc});
        renderer.add_passes(graph, color, de::rhi::Format::RGBA8Unorm, frame, lighting, view);
        graph.execute();
        device.end_frame();
    }

    // The same, and what it drew.
    de::core::Array<std::byte> draw(std::initializer_list<AssetHandle> meshes) const {
        submit(meshes);
        return de::test::read_texture(device, target);
    }

    // Draws this frame until what the cull kept can be read: read_cull_stats reads a slot's last frame.
    void submit_for_stats(const de::world::RenderFrame& frame, const de::render::SceneLighting& lighting = {},
                          de::render::SceneView view = de::render::SceneView::BaseColor) const {
        for (uint32_t i = 0; i < de::rhi::kFramesInFlight + 1; ++i) {
            submit(frame, lighting, view);
        }
    }

    // Frames until nothing in flight can hold a place back, and the update that frees them.
    void settle() const {
        device.wait_idle();
        empty_frame();
        device.wait_idle();
        gpu_assets.update();
        // What got back in now it fits has uploads of its own.
        uploader.flush_and_wait();
        gpu_assets.update();
    }
};

template <typename Body>
void with_pool(const PoolOptions& options, Body body) {
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
    put_material(*vfs, kRed, 1.0f, 0.0f);
    put_material(*vfs, kGreen, 0.0f, 1.0f);
    auto compiler = de::shader::ShaderCompiler::create(*vfs);
    REQUIRE(compiler.ok());
    auto shaders = de::shader::ShaderLibrary::create(*device, *vfs, jobs, *compiler, {.hot_reload = false});
    REQUIRE(shaders.ok());
    auto uploader = de::rhi::Uploader::create(*device, {.frame_budget_bytes = options.frame_budget_bytes});
    REQUIRE(uploader.ok());
    auto assets = de::asset::AssetRegistry::create(
        {.vfs = &*vfs, .jobs = &jobs, .hot_reload = options.hot_reload, .reload_checks_per_update = 64});
    REQUIRE(assets.ok());
    {
        auto gpu_assets = de::render::GpuAssets::create({.device = &*device,
                                                         .uploader = &*uploader,
                                                         .assets = &*assets,
                                                         .pool_vertices = options.pool_vertices,
                                                         .pool_indices = options.pool_indices,
                                                         .max_meshes = options.max_meshes,
                                                         .max_parts = options.max_parts,
                                                         .max_lods = options.max_lods});
        REQUIRE(gpu_assets.ok());
        auto renderer = de::render::SceneRenderer::create({.device = &*device,
                                                           .shaders = &*shaders,
                                                           .gpu_assets = &*gpu_assets,
                                                           .max_instances = options.max_instances});
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
        body(PoolScene{.device = *device,
                       .vfs = *vfs,
                       .uploader = *uploader,
                       .assets = *assets,
                       .gpu_assets = *gpu_assets,
                       .renderer = *renderer,
                       .graph = *graph,
                       .target = *target});
        device->wait_idle();
        device->destroy(*target);
    }
    device->wait_idle();
    de::test::check_clean(*device);
}

const uint8_t* texel(const de::core::Array<std::byte>& pixels, uint32_t x, uint32_t y) {
    return reinterpret_cast<const uint8_t*>(pixels.data()) + (size_t{y} * kSize + x) * 4;
}

bool is_red(const de::core::Array<std::byte>& pixels, uint32_t x) {
    const uint8_t* pixel = texel(pixels, x, kMiddle);
    return pixel[0] > 200 && pixel[1] < 50;
}

bool is_green(const de::core::Array<std::byte>& pixels, uint32_t x) {
    const uint8_t* pixel = texel(pixels, x, kMiddle);
    return pixel[1] > 200 && pixel[0] < 50;
}

} // namespace

TEST_CASE("meshes in the pool draw from their own vertices and indices") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        put_squares(scene.vfs, kSides, kGreen, {-1.0f, 1.0f});
        const AssetHandle center = scene.load(kCenter);
        const AssetHandle sides = scene.load(kSides);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        const GpuMesh* center_mesh = scene.gpu_assets.mesh(center);
        const GpuMesh* sides_mesh = scene.gpu_assets.mesh(sides);
        REQUIRE(center_mesh != nullptr);
        REQUIRE(sides_mesh != nullptr);
        // Side by side in the pool, the second one's parts in pool terms.
        CHECK(center_mesh->vertex_offset == 0);
        CHECK(sides_mesh->vertex_offset == kSquareVertices);
        CHECK(center_mesh->parts[0].first_index == 0);
        CHECK(sides_mesh->parts[0].first_index == kSquareIndices);
        CHECK(sides_mesh->parts[0].index_count == 2 * kSquareIndices);
        CHECK(scene.gpu_assets.stats().pool_vertices_free == 1024 - (3 * kSquareVertices));
        CHECK(scene.gpu_assets.stats().pool_indices_free == 1024 - (3 * kSquareIndices));

        const auto only_center = scene.draw({center});
        CHECK(is_red(only_center, kMiddle));
        CHECK_FALSE(is_red(only_center, kLeft));
        CHECK_FALSE(is_red(only_center, kRight));
        const auto only_sides = scene.draw({sides});
        CHECK(is_green(only_sides, kLeft));
        CHECK(is_green(only_sides, kRight));
        CHECK_FALSE(is_green(only_sides, kMiddle));
        const auto both = scene.draw({sides, center, sides});
        CHECK(is_red(both, kMiddle));
        CHECK(is_green(both, kLeft));
        CHECK(is_green(both, kRight));
    });
}

TEST_CASE("every draw in one indirect call reads its own instance and material") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        put_squares(scene.vfs, kSides, kGreen, {0.0f});
        const AssetHandle red = scene.load(kCenter);
        const AssetHandle green = scene.load(kSides);
        scene.uploader.flush_and_wait();
        // Each somewhere else, in one pipeline state: three draws in one call.
        de::world::RenderFrame frame = PoolScene::frame_of({red, green, red});
        frame.instances[0].transform[3] = -1.0f;
        frame.instances[1].transform[3] = 0.0f;
        frame.instances[2].transform[3] = 1.0f;
        scene.submit(frame);
        const auto pixels = de::test::read_texture(scene.device, scene.target);
        CHECK(is_red(pixels, kLeft));
        CHECK(is_green(pixels, kMiddle));
        CHECK(is_red(pixels, kRight));
        CHECK(scene.renderer.stats().draws == 3);
        CHECK(scene.renderer.stats().indirect_calls == 1 + de::render::kShadowCascadeCount);
    });
}

TEST_CASE("draws of every pipeline state in one frame read their own records") {
    with_pool({}, [](const PoolScene& scene) {
        using de::asset::AlphaMode;
        using de::asset::MaterialFlags;
        struct Kind {
            AlphaMode mode;
            MaterialFlags flags;
            float color[3];
            float x_m; // 1 m wide, side by side
            uint32_t pixel;
        };
        // Listed backward: the renderer sorts them by kind, and each kind's draws start somewhere else.
        const Kind kinds[] = {
            {.mode = AlphaMode::Masked,
             .flags = MaterialFlags::DoubleSided,
             .color = {1, 1, 0},
             .x_m = 1.5f,
             .pixel = 61},
            {.mode = AlphaMode::Masked, .flags = MaterialFlags::None, .color = {0, 0, 1}, .x_m = 0.5f, .pixel = 42},
            {.mode = AlphaMode::Opaque,
             .flags = MaterialFlags::DoubleSided,
             .color = {0, 1, 0},
             .x_m = -0.5f,
             .pixel = 22},
            {.mode = AlphaMode::Opaque, .flags = MaterialFlags::None, .color = {1, 0, 0}, .x_m = -1.5f, .pixel = 3},
        };
        de::world::RenderFrame frame = PoolScene::frame_of({});
        uint32_t id = 0;
        for (const Kind& kind : kinds) {
            const AssetId material_id = AssetId::from_value(0x800 + id);
            const AssetId mesh_id = AssetId::from_value(0x900 + id);
            de::cooker::MaterialBuild build;
            build.alpha_mode = kind.mode;
            build.flags = kind.flags;
            build.set_param(de::asset::material::kBaseColor, kind.color[0], kind.color[1], kind.color[2], 1.0f);
            const auto material = de::cooker::cook_material(build, material_id);
            REQUIRE(
                scene.vfs.write_file(de::asset::cooked_path(material_id, AssetType::Material).c_str(), material.span())
                    .ok());
            put_squares(scene.vfs, mesh_id, material_id, {0.0f});
            frame.instances.push(
                {.transform = {1, 0, 0, kind.x_m, 0, 1, 0, 0, 0, 0, 1, 0}, .mesh = scene.load(mesh_id)});
            ++id;
        }
        scene.uploader.flush_and_wait();
        scene.submit(frame);
        const auto pixels = de::test::read_texture(scene.device, scene.target);
        for (const Kind& kind : kinds) {
            const uint8_t* pixel = texel(pixels, kind.pixel, kMiddle);
            for (int channel = 0; channel < 3; ++channel) {
                CHECK((pixel[channel] > 200) == (kind.color[channel] > 0.5f));
            }
        }
        CHECK(scene.renderer.stats().drawn == 4);
        CHECK(scene.renderer.stats().draws == 4);
        CHECK(scene.renderer.stats().shadow_draws == 16);
        // A call for each of the G-buffer's pipeline states, and casters have two.
        CHECK(scene.renderer.stats().indirect_calls == 4 + (2 * de::render::kShadowCascadeCount));
    });
}

TEST_CASE("a shadow caster reads its own instance and material") {
    with_pool({}, [](const PoolScene& scene) {
        constexpr AssetId kSolidMaterial = AssetId::from_value(0xa00);
        constexpr AssetId kThinMaterial = AssetId::from_value(0xa01);
        constexpr AssetId kSolid = AssetId::from_value(0xa02);
        constexpr AssetId kThin = AssetId::from_value(0xa03);
        const auto put_masked = [&](AssetId id, float alpha) {
            de::cooker::MaterialBuild build;
            build.alpha_mode = de::asset::AlphaMode::Masked;
            build.set_param(de::asset::material::kBaseColor, 1.0f, 1.0f, 1.0f, alpha);
            const auto material = de::cooker::cook_material(build, id);
            REQUIRE(
                scene.vfs.write_file(de::asset::cooked_path(id, AssetType::Material).c_str(), material.span()).ok());
        };
        put_masked(kSolidMaterial, 0.8f); // above the cutoff: it casts
        put_masked(kThinMaterial, 0.2f);  // below: it doesn't
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        put_squares(scene.vfs, kSolid, kSolidMaterial, {0.0f});
        put_squares(scene.vfs, kThin, kThinMaterial, {0.0f});
        const AssetHandle wall = scene.load(kCenter);
        const AssetHandle solid = scene.load(kSolid);
        const AssetHandle thin = scene.load(kThin);
        scene.uploader.flush_and_wait();

        // A 2 m wall at z = 0, and two 0.5 m squares 1 m in front of it, one above the other. The sun is at (1, 0, 2),
        // so each would shadow the wall 0.5 m to its left. Both are masked, so they share a call, and their records
        // come after the wall's.
        de::world::RenderFrame frame = PoolScene::frame_of({});
        frame.instances.push({.transform = {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0}, .mesh = wall});
        frame.instances.push({.transform = {0.5f, 0, 0, 0.6f, 0, 0.5f, 0, 0.6f, 0, 0, 0.5f, 1}, .mesh = solid});
        frame.instances.push({.transform = {0.5f, 0, 0, 0.6f, 0, 0.5f, 0, -0.6f, 0, 0, 0.5f, 1}, .mesh = thin});
        de::render::SceneLighting light{.sun_direction = {1, 0, 2}};
        light.shadows = {.resolution = 512, .distance_m = 8.0f, .split_lambda = 0.0f};
        scene.submit(frame, light, de::render::SceneView::ShadowMask);
        const auto pixels = de::test::read_texture(scene.device, scene.target);
        // About 19.5 pixels a meter on the wall: x = 0.1 m is 2 pixels right of the middle, y = +-0.6 m 12 up or down.
        constexpr uint32_t kShadowColumn = kMiddle + 2;
        CHECK(texel(pixels, kShadowColumn, kMiddle - 12)[0] < 40);  // the solid one's shadow
        CHECK(texel(pixels, kMiddle - 8, kMiddle - 12)[0] > 215);   // and only there: x = -0.4 m is past its edge
        CHECK(texel(pixels, kShadowColumn, kMiddle + 12)[0] > 215); // the thin one casts none
        CHECK(texel(pixels, kMiddle - 12, kMiddle)[0] > 215);       // lit
    });
}

TEST_CASE("the cull keeps what's in view and drops the rest") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle mesh = scene.load(kCenter);
        scene.uploader.flush_and_wait();
        de::world::RenderFrame frame = PoolScene::frame_of({mesh, mesh, mesh, mesh});
        frame.instances[1].transform[3] = -50.0f; // far to the left
        frame.instances[2].transform[11] = 10.0f; // behind the camera, which is at z = 3
        frame.instances[3].transform[7] = 50.0f;  // far above
        scene.submit_for_stats(frame);
        const de::render::SceneRendererStats stats = scene.renderer.stats();
        CHECK(stats.drawn == 4);
        CHECK(stats.draws == 4); // what it was given
        CHECK(stats.visible_draws == 1);
        CHECK(stats.shadow_draws == 16);
        CHECK(stats.visible_shadow_draws < 16);
        CHECK(is_red(de::test::read_texture(scene.device, scene.target), kMiddle));
    });
}

TEST_CASE("the cull keeps what only reaches into the view, by the size it's drawn at") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle mesh = scene.load(kCenter);
        scene.uploader.flush_and_wait();
        // 10 m wide, its center 6 m to the right: well out of the view (1.64 m to either side), its left edge in it.
        de::world::RenderFrame frame = PoolScene::frame_of({mesh});
        const float transform[12] = {10, 0, 0, 6, 0, 10, 0, 0, 0, 0, 10, 0};
        std::memcpy(frame.instances[0].transform, transform, sizeof(transform));
        scene.submit_for_stats(frame);
        CHECK(scene.renderer.stats().visible_draws == 1);
        const auto pixels = de::test::read_texture(scene.device, scene.target);
        CHECK(is_red(pixels, kRight));
        CHECK_FALSE(is_red(pixels, kLeft));

        // The same on one axis only: its bounds grow with its longest axis, whichever that is.
        const float wide[12] = {10, 0, 0, 6, 0, 1, 0, 0, 0, 0, 1, 0};
        std::memcpy(frame.instances[0].transform, wide, sizeof(wide));
        scene.submit_for_stats(frame);
        CHECK(scene.renderer.stats().visible_draws == 1);
        const float tall[12] = {1, 0, 0, 0, 0, 10, 0, 6, 0, 0, 1, 0};
        std::memcpy(frame.instances[0].transform, tall, sizeof(tall));
        scene.submit_for_stats(frame);
        CHECK(scene.renderer.stats().visible_draws == 1);
        const float deep[12] = {1, 0, 0, 2.5f, 0, 1, 0, 0, 0, 0, 10, 0};
        std::memcpy(frame.instances[0].transform, deep, sizeof(deep));
        scene.submit_for_stats(frame);
        CHECK(scene.renderer.stats().visible_draws ==
              1); // a flat square, so it isn't really: a sphere is all the cull knows
    });
}

TEST_CASE("the cull finds a mesh's bounds wherever its vertices are") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kSides, kRed, {30.0f}); // its vertices are 30 m from its origin
        const AssetHandle mesh = scene.load(kSides);
        scene.uploader.flush_and_wait();
        de::world::RenderFrame frame = PoolScene::frame_of({mesh});
        scene.submit_for_stats(frame);
        CHECK(scene.renderer.stats().visible_draws == 0); // the origin is in view, the square isn't
        frame.instances[0].transform[3] = -30.0f;
        scene.submit_for_stats(frame);
        CHECK(scene.renderer.stats().visible_draws == 1); // and now it's at the origin
        CHECK(is_red(de::test::read_texture(scene.device, scene.target), kMiddle));
    });
}

TEST_CASE("the cull adds every part of a mesh, each with its own material") {
    with_pool({}, [](const PoolScene& scene) {
        constexpr AssetId kTwoParts = AssetId::from_value(0xb10);
        de::cooker::ModelData model;
        for (const float center : {-1.0f, 1.0f}) {
            de::cooker::ModelPrimitive& primitive = model.primitives.emplace();
            primitive.material = center < 0.0f ? 0 : 1;
            const float corners[4][2] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
            for (const auto& corner : corners) {
                de::cooker::ModelVertex& vertex = primitive.vertices.emplace();
                vertex.position[0] = center + corner[0];
                vertex.position[1] = corner[1];
                vertex.normal[1] = 0.0f;
                vertex.normal[2] = 1.0f;
            }
            for (const uint32_t index : {0U, 1U, 2U, 0U, 2U, 3U}) {
                primitive.indices.push(index);
            }
            static_cast<void>(model.materials.emplace());
        }
        de::cooker::Diagnostics diagnostics;
        const AssetId materials[] = {kRed, kGreen};
        const auto cooked = de::cooker::cook_mesh(model, materials, {.lod_count = 1}, kTwoParts, "test", diagnostics);
        REQUIRE(cooked.ok());
        REQUIRE(scene.vfs.write_file(de::asset::cooked_path(kTwoParts, AssetType::Mesh).c_str(), cooked->span()).ok());
        const AssetHandle mesh = scene.load(kTwoParts);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        REQUIRE(scene.gpu_assets.mesh(mesh) != nullptr);
        CHECK(scene.gpu_assets.mesh(mesh)->lod_parts(0).size() == 2);
        const de::world::RenderFrame frame = PoolScene::frame_of({mesh});
        scene.submit_for_stats(frame);
        CHECK(scene.renderer.stats().draws == 2);
        CHECK(scene.renderer.stats().visible_draws == 2);
        CHECK(scene.renderer.stats().visible_triangles == 4); // two squares, not the last one twice
        const auto pixels = de::test::read_texture(scene.device, scene.target);
        CHECK(is_red(pixels, kLeft));
        CHECK(is_green(pixels, kRight));
    });
}

TEST_CASE("a material that arrives after its mesh reaches the part table") {
    with_pool({.hot_reload = true}, [](const PoolScene& scene) {
        constexpr AssetId kLate = AssetId::from_value(0xb20);
        put_squares(scene.vfs, kCenter, kLate, {0.0f});
        const AssetHandle mesh = scene.load(kCenter);
        scene.uploader.flush_and_wait();
        const auto before = scene.draw({mesh});
        CHECK(texel(before, kMiddle, kMiddle)[1] > 200); // the default: white
        CHECK(texel(before, kMiddle, kMiddle)[2] > 200);

        put_material(scene.vfs, kLate, 0.0f, 1.0f);
        for (int i = 0; i < 100 && scene.gpu_assets.stats().materials == 0; ++i) {
            scene.empty_frame();
        }
        REQUIRE(scene.gpu_assets.stats().materials == 1);
        scene.uploader.flush_and_wait();
        const auto after = scene.draw({mesh});
        CHECK(texel(after, kMiddle, kMiddle)[1] > 200);
        CHECK(texel(after, kMiddle, kMiddle)[2] < 50); // its own: green
    });
}

TEST_CASE("the cull reaches every instance") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle mesh = scene.load(kCenter);
        scene.uploader.flush_and_wait();
        de::world::RenderFrame frame = PoolScene::frame_of({});
        constexpr uint32_t kCount = 130; // two groups of 64 and a bit
        for (uint32_t i = 0; i < kCount; ++i) {
            frame.instances.push({.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}, .mesh = mesh});
        }
        scene.submit_for_stats(frame);
        CHECK(scene.renderer.stats().visible_draws == kCount);
    });
}

TEST_CASE("a caster out of the camera's view still casts its shadow") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle mesh = scene.load(kCenter);
        scene.uploader.flush_and_wait();
        // A 2 m wall at z = 0, and a 0.5 m square at z = 2, x = 1.1. The camera at z = 3 sees 0.55 m to either side at
        // that depth, so not the square, but with the sun at (1, 0, 2) its shadow lands 1 m to its left: x = 0.1 on the
        // wall.
        de::world::RenderFrame frame = PoolScene::frame_of({});
        frame.instances.push({.transform = {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0}, .mesh = mesh});
        frame.instances.push({.transform = {0.5f, 0, 0, 1.1f, 0, 0.5f, 0, 0, 0, 0, 0.5f, 2}, .mesh = mesh});
        de::render::SceneLighting light{.sun_direction = {1, 0, 2}};
        light.shadows = {.resolution = 512, .distance_m = 8.0f, .split_lambda = 0.0f};
        scene.submit_for_stats(frame, light, de::render::SceneView::ShadowMask);
        CHECK(scene.renderer.stats().visible_draws == 1);
        const auto pixels = de::test::read_texture(scene.device, scene.target);
        CHECK(texel(pixels, kMiddle + 2, kMiddle)[0] < 40);   // the shadow
        CHECK(texel(pixels, kMiddle - 12, kMiddle)[0] > 215); // lit
    });
}

TEST_CASE("a cascade drops casters past either end of its depth") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle mesh = scene.load(kCenter);
        scene.uploader.flush_and_wait();
        de::world::RenderFrame frame = PoolScene::frame_of({mesh});
        de::render::SceneLighting light{.sun_direction = {0, 0, 1}}; // straight behind the camera
        light.shadows = {.resolution = 512, .distance_m = 8.0f, .split_lambda = 0.0f};
        scene.submit_for_stats(frame, light, de::render::SceneView::ShadowMask);
        const uint32_t in_reach = scene.renderer.stats().visible_shadow_draws;
        CHECK(in_reach > 0);
        // Well past how far toward the sun a cascade reaches (200 m at most), straight up the sun's direction.
        frame.instances[0].transform[11] = 5000.0f;
        scene.submit_for_stats(frame, light, de::render::SceneView::ShadowMask);
        CHECK(scene.renderer.stats().visible_shadow_draws == 0);
        // And past the far end of every cascade.
        frame.instances[0].transform[11] = -5000.0f;
        scene.submit_for_stats(frame, light, de::render::SceneView::ShadowMask);
        CHECK(scene.renderer.stats().visible_shadow_draws == 0);
    });
}

TEST_CASE("what the cull kept isn't reported again for frames that drew nothing") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle mesh = scene.load(kCenter);
        scene.uploader.flush_and_wait();
        de::world::RenderFrame frame = PoolScene::frame_of({mesh});
        scene.submit_for_stats(frame);
        CHECK(scene.renderer.stats().visible_draws == 1);
        frame.camera.valid = false;
        for (uint32_t i = 0; i < 2 * de::rhi::kFramesInFlight + 1; ++i) {
            scene.submit(frame);
        }
        CHECK(scene.renderer.stats().visible_draws == 0);
        CHECK(scene.renderer.stats().visible_shadow_draws == 0);
    });
}

TEST_CASE("a material that changes how it draws keeps its meshes drawn") {
    with_pool({.hot_reload = true}, [](const PoolScene& scene) {
        constexpr AssetId kChanging = AssetId::from_value(0xb00);
        const auto put_changing = [&](de::asset::AlphaMode mode) {
            de::cooker::MaterialBuild build;
            build.alpha_mode = mode;
            build.set_param(de::asset::material::kBaseColor, 1.0f, 0.0f, 0.0f, 1.0f);
            const auto material = de::cooker::cook_material(build, kChanging);
            REQUIRE(
                scene.vfs.write_file(de::asset::cooked_path(kChanging, AssetType::Material).c_str(), material.span())
                    .ok());
        };
        put_changing(de::asset::AlphaMode::Opaque);
        put_squares(scene.vfs, kCenter, kChanging, {0.0f});
        const AssetHandle mesh = scene.load(kCenter);
        scene.uploader.flush_and_wait();
        CHECK(is_red(scene.draw({mesh}), kMiddle));

        // Opaque's draw list holds it now; once masked, it goes in another, which has to have room.
        put_changing(de::asset::AlphaMode::Masked);
        const uint32_t reloads = scene.assets.stats().reloads;
        for (int i = 0; i < 100 && scene.assets.stats().reloads == reloads; ++i) {
            scene.empty_frame();
        }
        REQUIRE(scene.assets.stats().reloads > reloads);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        const auto pixels = scene.draw({mesh});
        CHECK(is_red(pixels, kMiddle));
    });
}

TEST_CASE("a mesh takes up no room in the tables once it's gone, and doesn't fit when they're full") {
    // Out of mesh slots, or out of room for parts, though the pool has room.
    for (const PoolOptions options :
         {PoolOptions{.max_meshes = 1, .max_parts = 16}, PoolOptions{.max_meshes = 16, .max_parts = 1}}) {
        with_pool(options, [](const PoolScene& scene) {
            put_squares(scene.vfs, kCenter, kRed, {0.0f});
            put_squares(scene.vfs, kSides, kGreen, {-1.0f, 1.0f});
            const AssetHandle center = scene.load(kCenter);
            const AssetHandle sides = scene.load(kSides);
            scene.uploader.flush_and_wait();
            scene.gpu_assets.update();
            CHECK(scene.gpu_assets.mesh(center) != nullptr);
            CHECK(scene.gpu_assets.mesh(sides) == nullptr);
            CHECK(scene.gpu_assets.stats().meshes_dropped == 1);

            scene.assets.release(center);
            scene.assets.update();
            scene.settle();
            scene.uploader.flush_and_wait();
            scene.gpu_assets.update();
            REQUIRE(scene.gpu_assets.mesh(sides) != nullptr);
            CHECK(scene.gpu_assets.stats().meshes_dropped == 0);
            const auto pixels = scene.draw({sides});
            CHECK(is_green(pixels, kLeft));
        });
    }
}

namespace {

// An instance for a slot, x meters along.
de::world::RenderInstance instance_at(AssetHandle mesh, uint32_t slot, float x_m) {
    return {.transform = {1, 0, 0, x_m, 0, 1, 0, 0, 0, 0, 1, 0},
            .previous_transform = {1, 0, 0, x_m, 0, 1, 0, 0, 0, 0, 1, 0},
            .mesh = mesh,
            .slot = slot};
}

} // namespace

TEST_CASE("instances stay on the GPU until something about them changes") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        put_squares(scene.vfs, kSides, kGreen, {0.0f});
        const AssetHandle red = scene.load(kCenter);
        const AssetHandle green = scene.load(kSides);
        scene.uploader.flush_and_wait();

        de::world::RenderFrame frame = PoolScene::frame_of({});
        frame.slot_count = 3;
        frame.instances.push(instance_at(red, 0, -1.0f));
        frame.instances.push(instance_at(green, 2, 1.0f));
        scene.submit(frame);
        CHECK(scene.renderer.stats().instance_updates == 2);
        CHECK(scene.renderer.stats().instances == 2);
        CHECK(scene.renderer.stats().drawn == 2);
        de::core::Array<std::byte> pixels = de::test::read_texture(scene.device, scene.target);
        CHECK(is_red(pixels, kLeft));
        CHECK(is_green(pixels, kRight));

        // Nothing changes: nothing's sent, and they're all still there.
        frame.instances.clear();
        scene.submit_for_stats(frame);
        CHECK(scene.renderer.stats().instance_updates == 0);
        CHECK(scene.renderer.stats().instances == 2);
        CHECK(scene.renderer.stats().draws == 2);
        CHECK(scene.renderer.stats().visible_draws == 2);
        pixels = de::test::read_texture(scene.device, scene.target);
        CHECK(is_red(pixels, kLeft));
        CHECK(is_green(pixels, kRight));
        CHECK_FALSE(is_red(pixels, kMiddle));

        // One moves, and only it's sent. The other stays where it was, whatever a skipped row holds.
        frame.instances.push(instance_at(red, de::world::kSkipRenderInstance, 0.0f));
        frame.instances.push(instance_at(green, 2, 0.0f));
        scene.submit(frame);
        CHECK(scene.renderer.stats().instance_updates == 1);
        pixels = de::test::read_texture(scene.device, scene.target);
        CHECK(is_red(pixels, kLeft));
        CHECK(is_green(pixels, kMiddle));
        CHECK_FALSE(is_green(pixels, kRight));
    });
}

TEST_CASE("DIAG A: slot 1 freed, slot 0 alive") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle red = scene.load(kCenter);
        scene.uploader.flush_and_wait();
        de::world::RenderFrame frame = PoolScene::frame_of({});
        frame.slot_count = 2;
        frame.instances.push(instance_at(red, 0, -1.0f));
        frame.instances.push(instance_at(red, 1, 1.0f));
        scene.submit(frame);
        frame.instances.clear();
        frame.freed_slots.push(1);
        fprintf(stderr, "DIAG A before\n"); fflush(stderr);
        scene.submit(frame);
        fprintf(stderr, "DIAG A after\n"); fflush(stderr);
    });
}

TEST_CASE("DIAG B: slot 0 never had a mesh, slot 1 alive, no free record") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle red = scene.load(kCenter);
        scene.uploader.flush_and_wait();
        de::world::RenderFrame frame = PoolScene::frame_of({});
        frame.slot_count = 2;
        frame.instances.push(instance_at(red, 1, 1.0f));
        fprintf(stderr, "DIAG B before\n"); fflush(stderr);
        scene.submit(frame);
        fprintf(stderr, "DIAG B after\n"); fflush(stderr);
    });
}

TEST_CASE("DIAG D: slot 0 out of view, slot 1 in view") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle red = scene.load(kCenter);
        scene.uploader.flush_and_wait();
        de::world::RenderFrame frame = PoolScene::frame_of({});
        frame.slot_count = 2;
        frame.instances.push(instance_at(red, 0, 50.0f));
        frame.instances.push(instance_at(red, 1, 0.0f));
        scene.submit(frame);
        CHECK(is_red(de::test::read_texture(scene.device, scene.target), kMiddle));
    });
}

TEST_CASE("DIAG C: slot 0 freed, nothing else alive") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle red = scene.load(kCenter);
        scene.uploader.flush_and_wait();
        de::world::RenderFrame frame = PoolScene::frame_of({});
        frame.slot_count = 1;
        frame.instances.push(instance_at(red, 0, -1.0f));
        scene.submit(frame);
        frame.instances.clear();
        frame.freed_slots.push(0);
        fprintf(stderr, "DIAG C before\n"); fflush(stderr);
        scene.submit(frame);
        fprintf(stderr, "DIAG C after\n"); fflush(stderr);
    });
}

TEST_CASE("a freed slot draws nothing, until something else takes it") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        fprintf(stderr, "DIAG step 1\n"); fflush(stderr);
        put_squares(scene.vfs, kSides, kGreen, {0.0f});
        fprintf(stderr, "DIAG step 2\n"); fflush(stderr);
        const AssetHandle red = scene.load(kCenter);
        fprintf(stderr, "DIAG step 3\n"); fflush(stderr);
        const AssetHandle green = scene.load(kSides);
        fprintf(stderr, "DIAG step 4\n"); fflush(stderr);
        scene.uploader.flush_and_wait();
        fprintf(stderr, "DIAG step 5\n"); fflush(stderr);

        de::world::RenderFrame frame = PoolScene::frame_of({});
        fprintf(stderr, "DIAG step 6\n"); fflush(stderr);
        frame.slot_count = 2;
        fprintf(stderr, "DIAG step 7\n"); fflush(stderr);
        frame.instances.push(instance_at(red, 0, -1.0f));
        fprintf(stderr, "DIAG step 8\n"); fflush(stderr);
        frame.instances.push(instance_at(red, 1, 1.0f));
        fprintf(stderr, "DIAG step 9\n"); fflush(stderr);
        scene.submit(frame);
        fprintf(stderr, "DIAG step 10\n"); fflush(stderr);
        frame.instances.clear();
        fprintf(stderr, "DIAG step 11\n"); fflush(stderr);
        frame.freed_slots.push(0);
        fprintf(stderr, "DIAG step 12\n"); fflush(stderr);
        scene.submit_for_stats(frame);
        fprintf(stderr, "DIAG step 13\n"); fflush(stderr);
        CHECK(scene.renderer.stats().instances == 1);
        fprintf(stderr, "DIAG step 14\n"); fflush(stderr);
        CHECK(scene.renderer.stats().draws == 1); // and the draw lists know
        CHECK(scene.renderer.stats().visible_draws == 1);
        fprintf(stderr, "DIAG step 15\n"); fflush(stderr);
        de::core::Array<std::byte> pixels = de::test::read_texture(scene.device, scene.target);
        fprintf(stderr, "DIAG step 16\n"); fflush(stderr);
        CHECK_FALSE(is_red(pixels, kLeft));
        fprintf(stderr, "DIAG step 17\n"); fflush(stderr);
        CHECK(is_red(pixels, kRight));
        fprintf(stderr, "DIAG step 18\n"); fflush(stderr);

        frame.freed_slots.clear();
        fprintf(stderr, "DIAG step 19\n"); fflush(stderr);
        frame.instances.push(instance_at(green, 0, 0.0f));
        fprintf(stderr, "DIAG step 20\n"); fflush(stderr);
        scene.submit_for_stats(frame);
        fprintf(stderr, "DIAG step 21\n"); fflush(stderr);
        CHECK(scene.renderer.stats().instances == 2);
        fprintf(stderr, "DIAG step 22\n"); fflush(stderr);
        pixels = de::test::read_texture(scene.device, scene.target);
        fprintf(stderr, "DIAG step 23\n"); fflush(stderr);
        CHECK(is_green(pixels, kMiddle));
        fprintf(stderr, "DIAG step 24\n"); fflush(stderr);
        CHECK(is_red(pixels, kRight));
        fprintf(stderr, "DIAG step 25\n"); fflush(stderr);
    });
}

TEST_CASE("a frame with all the instances forgets any others") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle red = scene.load(kCenter);
        scene.uploader.flush_and_wait();
        de::world::RenderFrame frame = PoolScene::frame_of({});
        frame.slot_count = 2;
        frame.instances.push(instance_at(red, 0, -1.0f));
        frame.instances.push(instance_at(red, 1, 1.0f));
        scene.submit(frame);

        frame.instances.clear();
        frame.instances.push(instance_at(red, 1, 0.0f));
        frame.all_instances = true;
        scene.submit_for_stats(frame);
        CHECK(scene.renderer.stats().instances == 1);
        CHECK(scene.renderer.stats().visible_draws == 1);
        const auto pixels = de::test::read_texture(scene.device, scene.target);
        CHECK_FALSE(is_red(pixels, kLeft));
        CHECK(is_red(pixels, kMiddle));
        CHECK_FALSE(is_red(pixels, kRight));
    });
}

TEST_CASE("an instance whose mesh isn't ready starts drawing when it is, with nothing more sent") {
    with_pool({.frame_budget_bytes = 1}, [](const PoolScene& scene) { // one upload a frame
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle mesh = scene.assets.load<MeshAsset>(kCenter);
        de::world::RenderFrame frame = PoolScene::frame_of({});
        frame.slot_count = 1;
        frame.instances.push(instance_at(mesh, 0, 0.0f));
        scene.submit(frame); // it hasn't even been seen to load
        CHECK(scene.renderer.stats().instances == 1);
        CHECK(scene.renderer.stats().drawn == 0);

        frame.instances.clear();
        bool drawn = false;
        for (int i = 0; i < 40 && !drawn; ++i) {
            scene.submit(frame);
            drawn = is_red(de::test::read_texture(scene.device, scene.target), kMiddle);
        }
        CHECK(drawn);
        CHECK(scene.renderer.stats().drawn == 1);
        CHECK(scene.renderer.stats().instance_updates == 0);
    });
}

TEST_CASE("an instance keeps drawing its mesh through a reload, with nothing more sent") {
    with_pool({.hot_reload = true}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle mesh = scene.load(kCenter);
        scene.uploader.flush_and_wait();
        de::world::RenderFrame frame = PoolScene::frame_of({});
        frame.slot_count = 1;
        frame.instances.push(instance_at(mesh, 0, 0.0f));
        scene.submit(frame);
        frame.instances.clear();
        scene.submit(frame);
        CHECK(is_red(de::test::read_texture(scene.device, scene.target), kMiddle));

        put_squares(scene.vfs, kCenter, kRed, {-1.0f, 1.0f});
        const uint32_t reloads = scene.assets.stats().reloads;
        for (int i = 0; i < 100 && scene.assets.stats().reloads == reloads; ++i) {
            scene.submit(frame);
        }
        REQUIRE(scene.assets.stats().reloads > reloads);
        scene.uploader.flush_and_wait();
        scene.submit(frame);
        scene.uploader.flush_and_wait();
        scene.submit_for_stats(frame);
        CHECK(scene.renderer.stats().instance_updates == 0);
        CHECK(scene.renderer.stats().draws == 1);
        const auto pixels = de::test::read_texture(scene.device, scene.target);
        CHECK(is_red(pixels, kLeft));
        CHECK(is_red(pixels, kRight));
        CHECK_FALSE(is_red(pixels, kMiddle));
    });
}

TEST_CASE("an instance stops drawing when its mesh unloads") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        put_squares(scene.vfs, kSides, kRed, {0.0f});
        const AssetHandle going = scene.load(kCenter);
        const AssetHandle staying = scene.load(kSides);
        scene.uploader.flush_and_wait();
        // The one out of view leaves room in the draw list, for what shouldn't get drawn.
        de::world::RenderFrame frame = PoolScene::frame_of({});
        frame.slot_count = 3;
        frame.instances.push(instance_at(going, 0, 0.0f));
        frame.instances.push(instance_at(staying, 1, -1.0f));
        frame.instances.push(instance_at(staying, 2, 50.0f));
        scene.submit(frame);
        frame.instances.clear();
        CHECK(is_red(de::test::read_texture(scene.device, scene.target), kMiddle));

        scene.assets.release(going);
        scene.assets.update();
        scene.uploader.flush_and_wait();
        scene.submit_for_stats(frame);
        CHECK(scene.renderer.stats().drawn == 2);
        CHECK(scene.renderer.stats().draws == 2);
        CHECK(scene.renderer.stats().visible_draws == 1);
        const auto pixels = de::test::read_texture(scene.device, scene.target);
        CHECK_FALSE(is_red(pixels, kMiddle));
        CHECK(is_red(pixels, kLeft));
    });
}

TEST_CASE("a world's instances draw through the frames that send nothing") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle mesh = scene.load(kCenter);
        scene.uploader.flush_and_wait();
        auto world = de::world::World::create();
        REQUIRE(world.ok());
        const flecs::world& ecs = world->ecs();
        const auto rock_at = [&](double x_m) {
            const flecs::entity rock =
                ecs.entity().add<de::world::StaticTransform>().set(de::world::MeshInstance{.mesh = mesh});
            de::world::place(rock, de::world::Transform{.position = {x_m, 0.0, 0.0}});
            return rock;
        };
        const flecs::entity left = rock_at(-1.0);
        const flecs::entity right = rock_at(1.0);
        const flecs::entity eye = ecs.entity().set(de::world::Camera{.vertical_fov_rad = 1.0f});
        de::world::place(eye, de::world::Transform{.position = {0.0, 0.0, 3.0}});
        ecs.set(de::world::ActiveCamera{.entity = eye.id()});
        const auto frame_of_world = [&] {
            world->advance(1.0 / 60.0);
            scene.submit(world->render_frame());
            return de::test::read_texture(scene.device, scene.target);
        };
        de::core::Array<std::byte> pixels = frame_of_world();
        for (int i = 0; i < 4; ++i) {
            pixels = frame_of_world();
        }
        CHECK(scene.renderer.stats().instance_updates == 0);
        CHECK(scene.renderer.stats().instances == 2);
        CHECK(is_red(pixels, kLeft));
        CHECK(is_red(pixels, kRight));

        // One goes, and the other stays with nothing sent for it.
        left.destruct();
        for (int i = 0; i < 3; ++i) {
            pixels = frame_of_world();
        }
        CHECK(scene.renderer.stats().instances == 1);
        CHECK_FALSE(is_red(pixels, kLeft));
        CHECK(is_red(pixels, kRight));
        CHECK_FALSE(scene.renderer.instances_stale());

        // A frame not drawn, then one with what changed in the meantime.
        de::world::place(right, de::world::Transform{.position = {0.0, 0.0, 0.0}});
        world->advance(1.0 / 60.0); // not drawn: this frame's change never reaches the renderer
        world->advance(1.0 / 60.0);
        scene.submit(world->render_frame());
        CHECK(scene.renderer.instances_stale());
        world->resync_render_instances();
        pixels = frame_of_world();
        CHECK_FALSE(scene.renderer.instances_stale());
        CHECK(is_red(pixels, kMiddle));
        CHECK_FALSE(is_red(pixels, kRight));
    });
}

TEST_CASE("a slot freed and taken again in one frame draws what took it") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        put_squares(scene.vfs, kSides, kGreen, {0.0f});
        const AssetHandle red = scene.load(kCenter);
        const AssetHandle green = scene.load(kSides);
        scene.uploader.flush_and_wait();
        // Many of them, so a wrong order would show: the two records would be far apart.
        constexpr uint32_t kCount = 256;
        de::world::RenderFrame frame = PoolScene::frame_of({});
        frame.slot_count = kCount;
        for (uint32_t slot = 0; slot < kCount; ++slot) {
            frame.instances.push(instance_at(red, slot, 0.0f));
        }
        scene.submit(frame);
        frame.instances.clear();
        for (uint32_t slot = 0; slot < kCount; ++slot) {
            frame.freed_slots.push(slot);
            frame.instances.push(instance_at(green, slot, 50.0f + static_cast<float>(slot % 2)));
        }
        scene.submit(frame);
        CHECK(scene.renderer.stats().instance_updates == kCount); // no second record for any of them
        CHECK(scene.renderer.stats().instances == kCount);
        frame.instances.clear();
        frame.freed_slots.clear();
        scene.submit_for_stats(frame);
        CHECK(scene.renderer.stats().draws == kCount);
    });
}

TEST_CASE("slots a shrunken list leaves don't come back as they were") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle mesh = scene.load(kCenter);
        scene.uploader.flush_and_wait();
        de::world::RenderFrame frame = PoolScene::frame_of({});
        frame.slot_count = 3;
        frame.instances.push(instance_at(mesh, 0, -1.0f));
        frame.instances.push(instance_at(mesh, 1, 0.0f));
        frame.instances.push(instance_at(mesh, 2, 1.0f));
        scene.submit(frame);
        frame.instances.clear();
        frame.slot_count = 1;
        scene.submit(frame);
        CHECK(scene.renderer.stats().instances == 1);
        frame.slot_count = 3;
        scene.submit_for_stats(frame);
        CHECK(scene.renderer.stats().instances == 1);
        CHECK(scene.renderer.stats().visible_draws == 1);
        const auto pixels = de::test::read_texture(scene.device, scene.target);
        CHECK(is_red(pixels, kLeft));
        CHECK_FALSE(is_red(pixels, kMiddle));
        CHECK_FALSE(is_red(pixels, kRight));
    });
}

TEST_CASE("a mesh on its way out keeps room in the draw lists until the cull stops finding it") {
    with_pool({.frame_budget_bytes = 1}, [](const PoolScene& scene) { // one upload a frame: the queue backs up
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        put_squares(scene.vfs, kSides, kRed, {0.0f});
        const AssetHandle going = scene.load(kCenter);
        const AssetHandle staying = scene.load(kSides);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        scene.uploader.flush_and_wait();
        de::world::RenderFrame frame = PoolScene::frame_of({});
        frame.slot_count = 2;
        frame.instances.push(instance_at(going, 0, -1.0f));
        frame.instances.push(instance_at(staying, 1, 1.0f));
        scene.submit(frame);
        scene.uploader.flush_and_wait();
        frame.instances.clear();
        scene.assets.release(going);
        scene.assets.update();
        // Its table entry hasn't gone up yet, so it's still in the draw lists: they have to fit both.
        scene.submit(frame);
        CHECK(scene.renderer.stats().draws == 2);
        scene.uploader.flush_and_wait();
        scene.submit(frame);
        CHECK(scene.renderer.stats().draws == 1);
    });
}

TEST_CASE("a handle that's gone takes no mesh slot") {
    with_pool({.max_meshes = 2}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle mesh = scene.load(kCenter);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        CHECK(scene.gpu_assets.mesh_slot(mesh) != de::render::kNoMeshSlot);
        scene.assets.release(mesh);
        scene.assets.update();
        CHECK(scene.gpu_assets.mesh_slot(mesh) == de::render::kNoMeshSlot);
        CHECK(scene.gpu_assets.mesh_slot(AssetHandle{}) == de::render::kNoMeshSlot);
    });
}

TEST_CASE("instances past the limit aren't drawn, and the rest are") {
    with_pool({.max_instances = 1}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle mesh = scene.load(kCenter);
        scene.uploader.flush_and_wait();
        de::world::RenderFrame frame = PoolScene::frame_of({});
        frame.slot_count = 2;
        frame.instances.push(instance_at(mesh, 0, -1.0f));
        frame.instances.push(instance_at(mesh, 1, 1.0f));
        scene.submit_for_stats(frame);
        CHECK(scene.renderer.stats().instances == 1);
        const auto pixels = de::test::read_texture(scene.device, scene.target);
        CHECK(is_red(pixels, kLeft));
        CHECK_FALSE(is_red(pixels, kRight));
    });
}

TEST_CASE("the renderer says when instances went stale") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle mesh = scene.load(kCenter);
        scene.uploader.flush_and_wait();
        de::world::RenderFrame frame = PoolScene::frame_of({mesh});
        frame.number = 4; // the frames before it weren't drawn
        scene.submit(frame);
        CHECK(scene.renderer.instances_stale());
        frame.number = 5;
        scene.submit(frame);
        CHECK_FALSE(scene.renderer.instances_stale());
        frame.number = 7; // a frame wasn't drawn
        scene.submit(frame);
        CHECK(scene.renderer.instances_stale());
        frame.number = 8;
        scene.submit(frame);
        CHECK_FALSE(scene.renderer.instances_stale());
        frame.number = 9;
        frame.camera.valid = false; // this one has nothing to look through
        scene.submit(frame);
        CHECK(scene.renderer.instances_stale());
    });
}

TEST_CASE("many meshes reach the pool in one frame") {
    with_pool({}, [](const PoolScene& scene) {
        constexpr int kCount = 16;
        AssetHandle meshes[kCount];
        for (int i = 0; i < kCount; ++i) {
            const AssetId id = AssetId::from_value(0x400 + i);
            put_squares(scene.vfs, id, kRed, {0.0f});
            meshes[i] = scene.load(id);
        }
        scene.empty_frame();
        scene.gpu_assets.update();
        for (const AssetHandle mesh : meshes) {
            CHECK(scene.gpu_assets.mesh(mesh) != nullptr);
        }
        // Their entries in the mesh table, written as they became ready, go in one frame too.
        scene.empty_frame();
        CHECK(scene.uploader.idle());
    });
}

TEST_CASE("a mesh in the pool isn't drawn before all of it is recorded") {
    with_pool({.frame_budget_bytes = 1}, [](const PoolScene& scene) { // one upload a frame
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        put_squares(scene.vfs, kSides, kRed, {-1.0f, 1.0f});
        static_cast<void>(scene.load(kCenter));
        scene.uploader.flush_and_wait();
        // Its material is on the GPU already, so it's waiting for only its own five uploads.
        const AssetHandle sides = scene.load(kSides);
        CHECK(scene.gpu_assets.mesh(sides) == nullptr);
        CHECK(scene.gpu_assets.stats().pending > 0);
        for (const char* upload : {"the vertices", "the indices", "its parts"}) {
            scene.empty_frame();
            scene.gpu_assets.update();
            CAPTURE(upload);
            CHECK(scene.gpu_assets.mesh(sides) == nullptr);
        }
        scene.empty_frame(); // its LODs
        scene.gpu_assets.update();
        CHECK(scene.gpu_assets.mesh(sides) != nullptr);
    });
}

TEST_CASE("a mesh's place in the pool waits for the frames that may still draw from it") {
    with_pool({.pool_vertices = 8, .pool_indices = 12}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        put_squares(scene.vfs, kSides, kGreen, {-1.0f, 1.0f});
        const AssetHandle center = scene.load(kCenter);
        scene.uploader.flush_and_wait();
        CHECK(is_red(scene.draw({center}), kMiddle));
        CHECK(scene.gpu_assets.stats().pool_vertices_free == 8 - kSquareVertices);

        // Gone, with a frame that draws it possibly still running: it's free only when that's done.
        scene.submit({center});
        const uint64_t last_draw = scene.device.frame_index();
        scene.assets.release(center);
        scene.assets.update();
        CHECK(scene.gpu_assets.mesh(center) == nullptr);
        for (int update = 0; update < 3; ++update) {
            scene.gpu_assets.update();
            if (scene.gpu_assets.stats().pool_vertices_free == 8) {
                CHECK(scene.device.completed_frame() >= last_draw);
            }
        }
        scene.settle();
        CHECK(scene.gpu_assets.stats().pool_vertices_free == 8);
        CHECK(scene.gpu_assets.stats().pool_indices_free == 12);
        // All of it, for a mesh that only fits in the same place.
        const AssetHandle sides = scene.load(kSides);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        const GpuMesh* sides_mesh = scene.gpu_assets.mesh(sides);
        REQUIRE(sides_mesh != nullptr);
        CHECK(sides_mesh->vertex_offset == 0);
        CHECK(sides_mesh->parts[0].first_index == 0);
        const auto pixels = scene.draw({sides});
        CHECK(is_green(pixels, kLeft));
        CHECK(is_green(pixels, kRight));
        CHECK_FALSE(is_green(pixels, kMiddle));
    });
}

TEST_CASE("meshes stream into the pool while others are drawn") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle center = scene.load(kCenter);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        const uint32_t overwrites = scene.uploader.stats().overwrites;
        // A new mesh every frame, with frames in flight: sync validation watches the pool's ranges.
        constexpr int kCount = 16;
        AssetHandle streamed[kCount];
        for (int i = 0; i < kCount; ++i) {
            const AssetId id = AssetId::from_value(0x500 + i);
            put_squares(scene.vfs, id, kRed, {-1.0f, 1.0f});
            streamed[i] = scene.load(id);
            scene.submit({center});
        }
        // The pool's uploads never made the frame wait for them; only each mesh's entry in the mesh table did.
        CHECK(scene.uploader.stats().overwrites <= overwrites + kCount);
        scene.uploader.flush_and_wait();
        const auto pixels = scene.draw({center, streamed[kCount - 1]});
        CHECK(is_red(pixels, kMiddle));
        CHECK(is_red(pixels, kLeft));
        CHECK(is_red(pixels, kRight));
    });
}

TEST_CASE("a reloaded mesh moves in the pool, and its old place comes back") {
    with_pool({.hot_reload = true}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle mesh = scene.load(kCenter);
        scene.uploader.flush_and_wait();
        CHECK(is_red(scene.draw({mesh}), kMiddle));

        // Rewritten by the cooker, bigger. It gets a new place first: the old one is still being drawn.
        put_squares(scene.vfs, kCenter, kRed, {-1.0f, 1.0f});
        const uint32_t reloads = scene.assets.stats().reloads;
        for (int i = 0; i < 100 && scene.assets.stats().reloads == reloads; ++i) {
            scene.empty_frame();
        }
        REQUIRE(scene.assets.stats().reloads > reloads);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        const GpuMesh* gpu_mesh = scene.gpu_assets.mesh(mesh);
        REQUIRE(gpu_mesh != nullptr);
        CHECK(gpu_mesh->vertex_offset != 0);
        CHECK(scene.gpu_assets.stats().bytes ==
              (2 * kSquareVertices * sizeof(de::asset::MeshVertex)) + (2 * kSquareIndices * sizeof(uint32_t)));

        const auto pixels = scene.draw({mesh});
        CHECK(is_red(pixels, kLeft));
        CHECK(is_red(pixels, kRight));
        CHECK_FALSE(is_red(pixels, kMiddle));
        scene.settle();
        CHECK(scene.gpu_assets.stats().pool_vertices_free == 1024 - (2 * kSquareVertices));
        CHECK(scene.gpu_assets.stats().pool_indices_free == 1024 - (2 * kSquareIndices));
    });
}

TEST_CASE("a mesh that doesn't fit in the pool is left out, whichever buffer is full") {
    const auto left_out = [](const PoolScene& scene, uint64_t vertices_free, uint64_t indices_free) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        put_squares(scene.vfs, kSides, kGreen, {-1.0f, 1.0f});
        const AssetHandle center = scene.load(kCenter);
        const AssetHandle sides = scene.load(kSides);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        CHECK(scene.gpu_assets.mesh(center) != nullptr);
        CHECK(scene.gpu_assets.mesh(sides) == nullptr);
        const de::render::GpuAssetsStats stats = scene.gpu_assets.stats();
        CHECK(stats.meshes == 1);
        CHECK(stats.meshes_dropped == 1);
        CHECK(stats.pending == 0);
        CHECK(stats.bytes == (kSquareVertices * sizeof(de::asset::MeshVertex)) + (kSquareIndices * sizeof(uint32_t)));
        // Nothing of it stays taken.
        CHECK(stats.pool_vertices_free == vertices_free);
        CHECK(stats.pool_indices_free == indices_free);
        // What's in the pool draws; what isn't is skipped, wherever it stands. The third has room for what the second
        // would add if it took some other mesh's place, as it's out of view.
        de::world::RenderFrame frame = PoolScene::frame_of({center, sides, center});
        frame.instances[1].transform[3] = 1.0f;
        frame.instances[2].transform[3] = 50.0f;
        scene.submit_for_stats(frame);
        CHECK(scene.renderer.stats().draws == 2);
        CHECK(scene.renderer.stats().visible_draws == 1);
        const auto pixels = de::test::read_texture(scene.device, scene.target);
        CHECK(is_red(pixels, kMiddle));
        CHECK_FALSE(is_red(pixels, kRight));
        CHECK_FALSE(is_green(pixels, kLeft));
    };
    with_pool({.pool_vertices = 6, .pool_indices = 1024},
              [&](const PoolScene& scene) { left_out(scene, 6 - kSquareVertices, 1024 - kSquareIndices); });
    with_pool({.pool_vertices = 1024, .pool_indices = 8},
              [&](const PoolScene& scene) { left_out(scene, 1024 - kSquareVertices, 8 - kSquareIndices); });
}

TEST_CASE("a mesh that didn't fit gets in when space comes back") {
    with_pool({.pool_vertices = 8, .pool_indices = 12}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        put_squares(scene.vfs, kSides, kGreen, {-1.0f, 1.0f});
        const AssetHandle center = scene.load(kCenter);
        const AssetHandle sides = scene.load(kSides);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        CHECK(scene.gpu_assets.stats().meshes_dropped == 1);
        CHECK(scene.gpu_assets.mesh(sides) == nullptr);

        scene.assets.release(center);
        scene.assets.update();
        scene.settle();
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        CHECK(scene.gpu_assets.stats().meshes_dropped == 0);
        REQUIRE(scene.gpu_assets.mesh(sides) != nullptr);
        CHECK(is_green(scene.draw({sides}), kLeft));
    });
}

TEST_CASE("a mesh that didn't fit stops waiting when it's released") {
    with_pool({.pool_vertices = 4, .pool_indices = 6}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        put_squares(scene.vfs, kSides, kGreen, {-1.0f, 1.0f});
        const AssetHandle center = scene.load(kCenter);
        const AssetHandle sides = scene.load(kSides);
        CHECK(scene.gpu_assets.stats().meshes_dropped == 1);
        scene.assets.release(sides);
        scene.assets.update();
        CHECK(scene.gpu_assets.stats().meshes_dropped == 0);
        // Space coming back doesn't bring it back.
        scene.assets.release(center);
        scene.assets.update();
        scene.settle();
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        CHECK(scene.gpu_assets.stats().meshes == 0);
        CHECK(scene.gpu_assets.stats().pool_vertices_free == 4);
    });
}

TEST_CASE("a mesh reloaded in a full pool comes back once its old place is free") {
    with_pool({.pool_vertices = 8, .pool_indices = 12, .hot_reload = true}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        put_squares(scene.vfs, kSides, kGreen, {-1.0f, 1.0f});
        const AssetHandle mesh = scene.load(kSides); // all of the pool
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        REQUIRE(scene.gpu_assets.mesh(mesh) != nullptr);

        // As big again, so there's no room until its old place is free.
        put_squares(scene.vfs, kSides, kRed, {-1.5f, 1.5f});
        const uint32_t reloads = scene.assets.stats().reloads;
        for (int i = 0; i < 100 && scene.assets.stats().reloads == reloads; ++i) {
            scene.empty_frame();
        }
        REQUIRE(scene.assets.stats().reloads > reloads);
        scene.settle();
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        CHECK(scene.gpu_assets.stats().meshes_dropped == 0);
        REQUIRE(scene.gpu_assets.mesh(mesh) != nullptr);
        CHECK(is_red(scene.draw({mesh}), kLeft - 10));
    });
}

TEST_CASE("a mesh with a missing material stays drawn as other materials load") {
    with_pool({}, [](const PoolScene& scene) {
        constexpr AssetId kNoSuchMaterial = AssetId::from_value(0x77);
        put_squares(scene.vfs, kCenter, kNoSuchMaterial, {0.0f});
        put_squares(scene.vfs, kSides, kGreen, {-1.0f, 1.0f});
        const AssetHandle center = scene.load(kCenter);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        REQUIRE(scene.gpu_assets.mesh(center) != nullptr);
        static_cast<void>(scene.load(kSides)); // its material loads: nothing about this mesh changes
        CHECK(scene.gpu_assets.mesh(center) != nullptr);
    });
}

TEST_CASE("a mesh waits for its own material's table upload, not for the ones queued after it") {
    with_pool({.frame_budget_bytes = 1}, [](const PoolScene& scene) { // one upload a frame: the queue backs up
        // A new mesh and a new material every frame, so a table upload is always queued.
        const auto add_mesh = [&](int i) {
            const AssetId mesh_id = AssetId::from_value(0x600 + i);
            const AssetId material_id = AssetId::from_value(0x700 + i);
            put_material(scene.vfs, material_id, 1.0f, 0.0f);
            put_squares(scene.vfs, mesh_id, material_id, {0.0f});
            return scene.load(mesh_id);
        };
        const AssetHandle first = add_mesh(0);
        bool ready = false;
        for (int i = 1; i < 12 && !ready; ++i) {
            scene.empty_frame();
            static_cast<void>(add_mesh(i));
            ready = scene.gpu_assets.mesh(first) != nullptr;
        }
        CHECK(ready);
        CHECK_FALSE(scene.uploader.idle());
        scene.uploader.flush_and_wait();
    });
}

TEST_CASE("a mesh's bounds and every LOD are kept") {
    with_pool({.pool_vertices = 8192, .pool_indices = 65536}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f}); // so the grid isn't at the start of the pool
        put_grid(scene.vfs, kGrid, kGreen);
        static_cast<void>(scene.load(kCenter));
        const AssetHandle grid = scene.load(kGrid);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        const GpuMesh* mesh = scene.gpu_assets.mesh(grid);
        REQUIRE(mesh != nullptr);
        const auto* asset = scene.assets.get<MeshAsset>(grid);
        REQUIRE(asset != nullptr);

        CHECK(mesh->bounds_radius_m == asset->bounds.radius);
        for (int axis = 0; axis < 3; ++axis) {
            CHECK(mesh->bounds_min[axis] == asset->bounds.min[axis]);
            CHECK(mesh->bounds_max[axis] == asset->bounds.max[axis]);
            CHECK(mesh->bounds_center[axis] == asset->bounds.center[axis]);
        }
        CHECK(mesh->bounds_min[0] == -1.0f);
        CHECK(mesh->bounds_max[0] == 1.0f);

        // A mesh whose center isn't the origin.
        constexpr AssetId kOffCenter = AssetId::from_value(0x76);
        put_squares(scene.vfs, kOffCenter, kRed, {2.0f});
        const AssetHandle off_center = scene.load(kOffCenter);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        const GpuMesh* off_center_mesh = scene.gpu_assets.mesh(off_center);
        REQUIRE(off_center_mesh != nullptr);
        CHECK(off_center_mesh->bounds_center[0] == doctest::Approx(2.0f));

        mesh = scene.gpu_assets.mesh(grid);
        REQUIRE(mesh != nullptr);
        const auto lods = asset->lods.get(asset);
        const auto parts = asset->parts.get(asset);
        REQUIRE(lods.size() >= 2);
        REQUIRE(mesh->lods.size() == lods.size());
        CHECK(mesh->lods[0].error_m == 0.0f);
        for (size_t lod = 0; lod < lods.size(); ++lod) {
            CHECK(mesh->lods[lod].error_m == lods[lod].error);
            const auto kept = mesh->lod_parts(lod);
            REQUIRE(kept.size() == lods[lod].part_count);
            for (size_t p = 0; p < kept.size(); ++p) {
                const de::asset::MeshPart& part = parts[lods[lod].first_part + p];
                CHECK(kept[p].first_index == kSquareIndices + part.first_index);
                CHECK(kept[p].index_count == part.index_count);
                CHECK(kept[p].material != 0);
            }
        }
        CHECK(mesh->lod_parts(1)[0].index_count < mesh->lod_parts(0)[0].index_count);
        CHECK(is_green(scene.draw({grid}), kMiddle));
        CHECK(scene.renderer.stats().draws == mesh->lods[0].part_count);
    });
}

TEST_CASE("a view picks the coarsest LOD whose error it wouldn't see") {
    with_pool({.pool_vertices = 8192, .pool_indices = 65536}, [](const PoolScene& scene) {
        put_grid(scene.vfs, kGrid, kGreen);
        const AssetHandle grid = scene.load(kGrid);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        const GpuMesh* mesh = scene.gpu_assets.mesh(grid);
        REQUIRE(mesh != nullptr);
        REQUIRE(mesh->lods.size() >= 3);
        const auto triangles = [&](size_t lod) { return mesh->lod_parts(lod)[0].index_count / 3; };
        for (size_t lod = 1; lod < mesh->lods.size(); ++lod) {
            CHECK(mesh->lods[lod].error_m > mesh->lods[lod - 1].error_m);
            CHECK(triangles(lod) < triangles(lod - 1));
        }
        // A pixel of error at this distance in a 64 pixel target, seen from the nearest point of its sphere.
        const float pixels_per_radian = static_cast<float>(kSize) / (2.0f * std::tan(0.5f));
        const auto distance_for = [&](float error_m, float scale) {
            return (error_m * scale * pixels_per_radian) + (mesh->bounds_radius_m * scale);
        };
        const auto kept_at = [&](float distance, float scale) {
            de::world::RenderFrame frame = PoolScene::frame_of({grid});
            const float transform[12] = {scale, 0, 0, 0, 0, scale, 0, 0, 0, 0, scale, 3.0f - distance};
            std::memcpy(frame.instances[0].transform, transform, sizeof(transform));
            scene.submit_for_stats(frame);
            return scene.renderer.stats().visible_triangles;
        };
        CAPTURE(mesh->lods[1].error_m);
        // Right up close, all of it.
        CHECK(kept_at(mesh->bounds_radius_m * 1.01f, 1.0f) == triangles(0));
        for (size_t lod = 1; lod < mesh->lods.size(); ++lod) {
            CAPTURE(lod);
            const float boundary = distance_for(mesh->lods[lod].error_m, 1.0f);
            // Just inside the distance where its error would be a pixel, the LOD before it; just outside, it.
            CHECK(kept_at(boundary * 0.98f, 1.0f) == triangles(lod - 1));
            CHECK(kept_at(boundary * 1.02f, 1.0f) == triangles(lod));
            // Its error scales with the instance: twice as big, twice as far.
            const float scaled = distance_for(mesh->lods[lod].error_m, 2.0f);
            CHECK(kept_at(scaled * 0.98f, 2.0f) == triangles(lod - 1));
            CHECK(kept_at(scaled * 1.02f, 2.0f) == triangles(lod));
        }
        // Off to the side at the same depth, the same: what's on screen goes by depth.
        const auto kept_off_axis = [&](float depth) {
            de::world::RenderFrame frame = PoolScene::frame_of({grid});
            const float offset = depth * 0.6f; // about 31 degrees: well in view at this aspect
            const float transform[12] = {1, 0, 0, 0, 0, 1, 0, offset, 0, 0, 1, 3.0f - depth};
            std::memcpy(frame.instances[0].transform, transform, sizeof(transform));
            scene.submit_for_stats(frame);
            return scene.renderer.stats().visible_triangles;
        };
        for (size_t lod = 1; lod < mesh->lods.size(); ++lod) {
            CAPTURE(lod);
            const float boundary = distance_for(mesh->lods[lod].error_m, 1.0f);
            CHECK(kept_off_axis(boundary * 0.98f) == triangles(lod - 1));
            CHECK(kept_off_axis(boundary * 1.02f) == triangles(lod));
        }
        // Past the last, the last.
        const size_t last = mesh->lods.size() - 1;
        CHECK(kept_at(distance_for(mesh->lods[last].error_m, 1.0f) * 3.0f, 1.0f) == triangles(last));
    });
}

TEST_CASE("a picked LOD stays within a pixel of LOD 0 on screen") {
    with_pool({.pool_vertices = 8192, .pool_indices = 65536}, [](const PoolScene& scene) {
        put_grid(scene.vfs, kGrid, kGreen, 0.4f, 8);
        const AssetHandle grid = scene.load(kGrid);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        const GpuMesh* mesh = scene.gpu_assets.mesh(grid);
        REQUIRE(mesh != nullptr);
        // Seen side on, so its bumps are its outline. The topmost covered row of each column.
        const auto outline = [&](float distance, float lod_error_pixels, uint32_t& triangles) {
            scene.renderer.set_lod_error_pixels(lod_error_pixels);
            de::world::RenderFrame frame = PoolScene::frame_of({grid});
            const float transform[12] = {1, 0, 0, 0, 0, 0, 1, 0, 0, -1, 0, 3.0f - distance};
            std::memcpy(frame.instances[0].transform, transform, sizeof(transform));
            scene.submit_for_stats(frame);
            triangles = scene.renderer.stats().visible_triangles;
            const auto pixels = de::test::read_texture(scene.device, scene.target);
            de::core::Array<int> rows;
            for (uint32_t x = 0; x < kSize; ++x) {
                int top = -1;
                for (uint32_t y = 0; y < kSize && top < 0; ++y) {
                    top = texel(pixels, x, y)[1] > 200 ? static_cast<int>(y) : -1;
                }
                rows.push(top);
            }
            return rows;
        };
        // How far apart two outlines get: from each point on one, to the nearest point on the other, in pixels. Columns
        // alone would overstate it: on a steep bump, a pixel sideways is several rows.
        const auto one_way = [](const de::core::Array<int>& from, const de::core::Array<int>& to) {
            constexpr auto kColumns = static_cast<int>(kSize);
            const auto at = [](const de::core::Array<int>& rows, int column) {
                return rows[static_cast<size_t>(column)];
            };
            int worst = 0;
            for (int x = 0; x < kColumns; ++x) {
                if (at(from, x) < 0) {
                    continue;
                }
                int nearest = kColumns;
                for (int other = std::max(x - 8, 0); other < std::min(x + 9, kColumns); ++other) {
                    if (at(to, other) >= 0) {
                        nearest =
                            std::min(nearest, std::max(std::abs(other - x), std::abs(at(to, other) - at(from, x))));
                    }
                }
                worst = std::max(worst, nearest);
            }
            return worst;
        };
        const auto worst_difference = [&](const de::core::Array<int>& a, const de::core::Array<int>& b) {
            return std::max(one_way(a, b), one_way(b, a));
        };
        const uint32_t lod0_triangles = mesh->lod_parts(0)[0].index_count / 3;
        bool any_coarser = false;
        for (const float distance : {4.0f, 6.0f, 9.0f, 14.0f}) {
            CAPTURE(distance);
            uint32_t exact_triangles = 0;
            uint32_t picked_triangles = 0;
            const auto exact = outline(distance, 0.0f, exact_triangles);
            const auto picked = outline(distance, 1.0f, picked_triangles);
            CHECK(exact_triangles == lod0_triangles);
            any_coarser = any_coarser || picked_triangles < lod0_triangles;
            // A pixel of error, and a pixel from where a row's center falls.
            CHECK(worst_difference(exact, picked) <= 2);
        }
        CHECK(any_coarser);

        // What this test would see if the limit weren't kept.
        uint32_t coarse_triangles = 0;
        uint32_t exact_triangles = 0;
        const auto exact = outline(4.0f, 0.0f, exact_triangles);
        const auto coarse = outline(4.0f, 100.0f, coarse_triangles);
        CHECK(coarse_triangles < exact_triangles);
        CHECK(worst_difference(exact, coarse) > 2);
    });
}

TEST_CASE("a limit of 0 draws LOD 0 however small a LOD's error, in every view") {
    with_pool({.pool_vertices = 8192, .pool_indices = 65536}, [](const PoolScene& scene) {
        put_grid(scene.vfs, kGrid, kGreen, 0.0f, 8); // flat: its LODs lie nowhere from it
        const AssetHandle grid = scene.load(kGrid);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        const GpuMesh* mesh = scene.gpu_assets.mesh(grid);
        REQUIRE(mesh != nullptr);
        const uint32_t lod0 = mesh->lod_parts(0)[0].index_count / 3;
        REQUIRE(mesh->lods.size() >= 2);
        const de::world::RenderFrame frame = PoolScene::frame_of({grid});
        const de::render::SceneLighting light{.sun_direction = {0, 0, 1}};
        scene.renderer.set_lod_error_pixels(0.0f);
        scene.submit_for_stats(frame, light, de::render::SceneView::ShadowMask);
        CHECK(scene.renderer.stats().visible_triangles == lod0);
        CHECK(scene.renderer.stats().visible_shadow_triangles >= lod0);
        CHECK(scene.renderer.stats().visible_shadow_triangles % lod0 == 0);
    });
}

TEST_CASE("each shadow cascade picks its LOD by its own texels, wherever the camera is") {
    with_pool({.pool_vertices = 8192, .pool_indices = 65536}, [](const PoolScene& scene) {
        put_grid(scene.vfs, kGrid, kGreen);
        const AssetHandle grid = scene.load(kGrid);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        const GpuMesh* mesh = scene.gpu_assets.mesh(grid);
        REQUIRE(mesh != nullptr);
        REQUIRE(mesh->lods.size() >= 3);
        const uint32_t lod0 = mesh->lod_parts(0)[0].index_count / 3;
        const uint32_t last = mesh->lod_parts(mesh->lods.size() - 1)[0].index_count / 3;
        de::render::SceneLighting light{.sun_direction = {0, 0, 1}};
        const auto shadow_triangles = [&](uint32_t resolution) {
            light.shadows = {.resolution = resolution, .distance_m = 8.0f, .split_lambda = 0.0f};
            scene.submit_for_stats(PoolScene::frame_of({grid}), light, de::render::SceneView::ShadowMask);
            return scene.renderer.stats().visible_shadow_triangles;
        };
        // Texels so coarse that no LOD's error shows, and fine enough that they show.
        const uint32_t coarse = shadow_triangles(64);
        const uint32_t fine = shadow_triangles(4096);
        CHECK(coarse > 0);
        CHECK(coarse < fine);
        CHECK(coarse <= last * 4 + lod0 * 4);
        CHECK(fine >= lod0);
    });
}

TEST_CASE("a mesh's LODs need room in the table too") {
    with_pool({.pool_vertices = 8192, .pool_indices = 65536, .max_lods = 1}, [](const PoolScene& scene) {
        put_grid(scene.vfs, kGrid, kGreen);
        const AssetHandle grid = scene.load(kGrid);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        CHECK(scene.gpu_assets.mesh(grid) == nullptr);
        CHECK(scene.gpu_assets.stats().meshes_dropped == 1);

        // One LOD fits, and its place comes back for the next.
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        put_squares(scene.vfs, kSides, kGreen, {-1.0f, 1.0f});
        const AssetHandle first = scene.load(kCenter);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        CHECK(scene.gpu_assets.mesh(first) != nullptr);
        const AssetHandle waiting = scene.load(kSides);
        CHECK(scene.gpu_assets.stats().meshes_dropped == 2);
        scene.assets.release(first);
        scene.assets.update();
        scene.settle();
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        CHECK(scene.gpu_assets.mesh(waiting) != nullptr);
        CHECK(scene.gpu_assets.stats().meshes_dropped == 1);
    });
}
