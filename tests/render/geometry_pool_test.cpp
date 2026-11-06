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
#include "support/render_transform.h"
#include "support/sandbox.h"
#include "world/render_extract.h"
#include "world/transform.h"
#include "world/world.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <numbers>
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

// A grid like put_grid, with exactly these LODs: each draws in one of two materials, so a picture shows which LOD went
// where, and has this error. The cooker can't be asked for that, so its file is changed afterward.
struct ColoredLod {
    uint32_t material = 0; // 0 or 1
    float error_m = 0.0f;
};
void put_colored_lods(const de::platform::Vfs& vfs, AssetId mesh_id, AssetId first, AssetId second,
                      std::initializer_list<ColoredLod> lods) {
    constexpr uint32_t kCells = 32;
    de::cooker::ModelData model;
    for (uint32_t material = 0; material < 2; ++material) {
        de::cooker::ModelPrimitive& primitive = model.primitives.emplace();
        primitive.material = material;
        static_cast<void>(model.materials.emplace());
        for (uint32_t y = 0; y <= kCells; ++y) {
            for (uint32_t x = 0; x <= kCells; ++x) {
                de::cooker::ModelVertex& vertex = primitive.vertices.emplace();
                vertex.position[0] = (static_cast<float>(x) / kCells * 2.0f) - 1.0f;
                vertex.position[1] = (static_cast<float>(y) / kCells * 2.0f) - 1.0f;
                // Apart by a little: where parts touch, the cooker keeps their vertices where they are.
                vertex.position[2] =
                    (0.02f * std::sin(vertex.position[0] * 7.0f) * std::sin(vertex.position[1] * 5.0f)) +
                    (static_cast<float>(material) * 0.001f);
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
    }
    de::cooker::Diagnostics diagnostics;
    const AssetId materials[] = {first, second};
    auto mesh = de::cooker::cook_mesh(model, materials, {.lod_count = 8}, mesh_id, "test", diagnostics);
    REQUIRE(mesh.ok());
    auto* const asset = reinterpret_cast<MeshAsset*>(mesh->data());
    REQUIRE(asset->lods.count >= lods.size());
    auto* const cooked_lods = reinterpret_cast<de::asset::MeshLod*>(mesh->data() + asset->lods.offset);
    const auto* const parts = reinterpret_cast<const de::asset::MeshPart*>(mesh->data() + asset->parts.offset);
    uint32_t lod = 0;
    for (const ColoredLod& wanted : lods) {
        de::asset::MeshLod& cooked = cooked_lods[lod++];
        cooked.error = wanted.error_m;
        uint32_t found = cooked.part_count;
        for (uint32_t part = 0; part < cooked.part_count; ++part) {
            found = parts[cooked.first_part + part].material_slot == wanted.material ? part : found;
        }
        REQUIRE(found < cooked.part_count);
        cooked.first_part += found;
        cooked.part_count = 1;
    }
    asset->lods.count = lod;
    asset->header.content_hash = de::asset::content_hash(mesh->span());
    REQUIRE(vfs.write_file(de::asset::cooked_path(mesh_id, AssetType::Mesh).c_str(), mesh->span()).ok());
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
            frame.instances.push(
                {.transform = de::test::render_transform({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}), .mesh = mesh});
        }
        return frame;
    }

    // The engine's frame, drawing these meshes at the origin, left in flight.
    void submit(std::initializer_list<AssetHandle> meshes) const { submit(frame_of(meshes)); }

    void submit(const de::world::RenderFrame& frame, const de::render::SceneLighting& lighting = {},
                de::render::SceneView view = de::render::SceneView::BaseColor, bool taa = false) const {
        assets.update();
        assets.wait_all();
        gpu_assets.update();
        device.begin_frame();
        uploader.record();
        graph.begin(kSize, kSize);
        const de::render::GraphTexture color =
            graph.import_texture({.texture = target, .name = "target", .final_state = de::rhi::ResourceState::CopySrc});
        renderer.add_passes(graph, color, de::rhi::Format::RGBA8Unorm, frame, lighting, view, taa);
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
        frame.instances[0].transform.set_position_m(0, -1.0f);
        frame.instances[1].transform.set_position_m(0, 0.0f);
        frame.instances[2].transform.set_position_m(0, 1.0f);
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
            frame.instances.push({.transform = de::test::render_transform({1, 0, 0, kind.x_m, 0, 1, 0, 0, 0, 0, 1, 0}),
                                  .mesh = scene.load(mesh_id)});
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
        frame.instances.push(
            {.transform = de::test::render_transform({2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0}), .mesh = wall});
        frame.instances.push(
            {.transform = de::test::render_transform({0.5f, 0, 0, 0.6f, 0, 0.5f, 0, 0.6f, 0, 0, 0.5f, 1}),
             .mesh = solid});
        frame.instances.push(
            {.transform = de::test::render_transform({0.5f, 0, 0, 0.6f, 0, 0.5f, 0, -0.6f, 0, 0, 0.5f, 1}),
             .mesh = thin});
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
        frame.instances[1].transform.set_position_m(0, -50.0f); // far to the left
        frame.instances[2].transform.set_position_m(2, 10.0f);  // behind the camera, which is at z = 3
        frame.instances[3].transform.set_position_m(1, 50.0f);  // far above
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
        frame.instances[0].transform = de::test::render_transform(transform);
        scene.submit_for_stats(frame);
        CHECK(scene.renderer.stats().visible_draws == 1);
        const auto pixels = de::test::read_texture(scene.device, scene.target);
        CHECK(is_red(pixels, kRight));
        CHECK_FALSE(is_red(pixels, kLeft));

        // The same on one axis only: its bounds grow with its longest axis, whichever that is.
        const float wide[12] = {10, 0, 0, 6, 0, 1, 0, 0, 0, 0, 1, 0};
        frame.instances[0].transform = de::test::render_transform(wide);
        scene.submit_for_stats(frame);
        CHECK(scene.renderer.stats().visible_draws == 1);
        const float tall[12] = {1, 0, 0, 0, 0, 10, 0, 6, 0, 0, 1, 0};
        frame.instances[0].transform = de::test::render_transform(tall);
        scene.submit_for_stats(frame);
        CHECK(scene.renderer.stats().visible_draws == 1);
        const float deep[12] = {1, 0, 0, 2.5f, 0, 1, 0, 0, 0, 0, 10, 0};
        frame.instances[0].transform = de::test::render_transform(deep);
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
        frame.instances[0].transform.set_position_m(0, -30.0f);
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
            frame.instances.push(
                {.transform = de::test::render_transform({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}), .mesh = mesh});
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
        frame.instances.push(
            {.transform = de::test::render_transform({2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0}), .mesh = mesh});
        frame.instances.push(
            {.transform = de::test::render_transform({0.5f, 0, 0, 1.1f, 0, 0.5f, 0, 0, 0, 0, 0.5f, 2}), .mesh = mesh});
        de::render::SceneLighting light{.sun_direction = {1, 0, 2}};
        light.shadows = {.resolution = 512, .distance_m = 8.0f, .split_lambda = 0.0f};
        scene.submit_for_stats(frame, light, de::render::SceneView::ShadowMask);
        CHECK(scene.renderer.stats().visible_draws == 1);
        const auto pixels = de::test::read_texture(scene.device, scene.target);
        CHECK(texel(pixels, kMiddle + 2, kMiddle)[0] < 40);   // the shadow
        CHECK(texel(pixels, kMiddle - 12, kMiddle)[0] > 215); // lit
    });
}

namespace {

// The floor at y = -1, seen from above, and square casters scattered over it and around, some far out of view.
de::world::RenderFrame scattered_casters(AssetHandle mesh) {
    de::world::RenderFrame frame = PoolScene::frame_of({});
    frame.instances.push(
        {.transform = de::test::render_transform({100, 0, 0, 0, 0, 0, 100, -1, 0, -100, 0, 0}), .mesh = mesh});
    uint32_t state = 12345;
    const auto next = [&state](float low, float high) {
        state = (state * 1664525U) + 1013904223U;
        return low + ((high - low) * static_cast<float>(state >> 16U) / 65536.0f);
    };
    for (int i = 0; i < 400; ++i) {
        // Half of them close by, where shadows are easy to see. Tilted, so no sun finds them all edge on.
        const float reach_m = i % 2 == 0 ? 25.0f : 70.0f;
        const float size = next(0.2f, 1.5f);
        const float x = next(-reach_m, reach_m);
        const float y = next(0.0f, 12.0f);
        const float z = next(-reach_m, reach_m);
        frame.instances.push(
            {.transform = de::test::render_transform({0.71f * size, 0, 0.71f * size, x, 0.5f * size, 0.71f * size,
                                                      -0.5f * size, y, -0.5f * size, 0.71f * size, 0.5f * size, z}),
             .mesh = mesh});
    }
    return frame;
}

int pixels_apart(const de::core::Array<std::byte>& a, const de::core::Array<std::byte>& b) {
    int apart = 0;
    for (uint32_t y = 0; y < kSize; ++y) {
        for (uint32_t x = 0; x < kSize; ++x) {
            bool differs = false;
            for (int c = 0; c < 3; ++c) {
                differs = differs || texel(a, x, y)[c] != texel(b, x, y)[c];
            }
            apart += differs ? 1 : 0;
        }
    }
    return apart;
}

} // namespace

TEST_CASE("casters whose shadow lands out of sight are left out, and no picture changes") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle mesh = scene.load(kCenter);
        scene.uploader.flush_and_wait();

        de::world::RenderFrame frame = scattered_casters(mesh);
        // A wide spread of evenings and noons, ahead and turned, with the cascades' hand-over bands where the floor is.
        struct Case {
            float sun[3];
            float turn_rad;
        };
        constexpr Case kCases[] = {
            {.sun = {1, 1, 2}, .turn_rad = 0.0f},     {.sun = {-1, 0.4f, 0.1f}, .turn_rad = 0.0f},
            {.sun = {0, 1, 0}, .turn_rad = 0.7f},     {.sun = {0.2f, 0.3f, -1}, .turn_rad = 2.0f},
            {.sun = {1, 0.15f, 0}, .turn_rad = 4.0f}, {.sun = {-0.6f, 0.7f, 0.6f}, .turn_rad = 5.5f}};
        for (const Case& test_case : kCases) {
            CAPTURE(test_case.turn_rad);
            CAPTURE(test_case.sun[0]);
            const float c = std::cos(test_case.turn_rad);
            const float s = std::sin(test_case.turn_rad);
            const float camera[12] = {c, 0, s, 3 * s, 0, 1, 0, 2, -s, 0, c, 3 * c};
            std::memcpy(frame.camera.transform, camera, sizeof(camera));
            de::render::SceneLighting light{.sun_direction = {test_case.sun[0], test_case.sun[1], test_case.sun[2]}};
            light.shadows = {.resolution = 512, .distance_m = 40.0f, .blend_band = 0.3f};
            // Light shafts read the cascades too.
            light.fog.density_per_m = 0.02f;
            light.fog.volume_distance_m = 30.0f;
            const auto shown = [&](de::render::SceneView view) {
                scene.submit_for_stats(frame, light, view);
                return de::test::read_texture(scene.device, scene.target);
            };
            scene.renderer.set_shadow_caster_cull(false);
            const auto all_mask = shown(de::render::SceneView::ShadowMask);
            const uint32_t all_draws = scene.renderer.stats().visible_shadow_draws;
            const auto all_lit = shown(de::render::SceneView::Lit);
            scene.renderer.set_shadow_caster_cull(true);
            const auto some_mask = shown(de::render::SceneView::ShadowMask);
            const uint32_t some_draws = scene.renderer.stats().visible_shadow_draws;
            const auto some_lit = shown(de::render::SceneView::Lit);
            CHECK(some_draws < all_draws);
            CHECK(pixels_apart(all_mask, some_mask) == 0);
            CHECK(pixels_apart(all_lit, some_lit) == 0);
            // There was something to lose.
            int shadowed = 0;
            for (uint32_t y = 0; y < kSize; ++y) {
                for (uint32_t x = 0; x < kSize; ++x) {
                    shadowed += texel(some_mask, x, y)[0] < 128 ? 1 : 0;
                }
            }
            CHECK(shadowed > 20);
        }
    });
}

TEST_CASE("a caster beside what a cascade lights stays out, though the cascade's map could hold it") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle mesh = scene.load(kCenter);
        scene.uploader.flush_and_wait();

        // The sun behind the camera, so its near and side faces are all swept across: only the planes along the sun,
        // from the edges, keep this out. It's to the right of the view, inside the last cascade's sphere.
        de::world::RenderFrame frame = PoolScene::frame_of({});
        frame.instances.push(
            {.transform = de::test::render_transform({0.5f, 0, 0, 25, 0, 0.5f, 0, 0, 0, 0, 0.5f, -20}), .mesh = mesh});
        de::render::SceneLighting light{.sun_direction = {0, 0.3f, 1}};
        light.shadows = {.resolution = 512, .distance_m = 40.0f};
        scene.renderer.set_shadow_caster_cull(false);
        scene.submit_for_stats(frame, light, de::render::SceneView::ShadowMask);
        CHECK(scene.renderer.stats().visible_shadow_draws >= 1);
        scene.renderer.set_shadow_caster_cull(true);
        scene.submit_for_stats(frame, light, de::render::SceneView::ShadowMask);
        CHECK(scene.renderer.stats().visible_shadow_draws == 0);

        // The same thing in front of the view casts as it always did.
        frame.instances[0].transform.set_position_m(0, 0.0);
        scene.submit_for_stats(frame, light, de::render::SceneView::ShadowMask);
        CHECK(scene.renderer.stats().visible_shadow_draws >= 1);
    });
}

TEST_CASE("a caster too small for a cascade's texels stays out of it, and in the finer ones") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle mesh = scene.load(kCenter);
        scene.uploader.flush_and_wait();

        // Where two cascades meet, so it's in both: even splits of 8 m, 2 m apart, and it's 2 m ahead of the eye.
        de::world::RenderFrame frame = PoolScene::frame_of({});
        constexpr float kSizeM = 0.1f;
        frame.instances.push(
            {.transform = de::test::render_transform({kSizeM, 0, 0, 0, 0, kSizeM, 0, 0, 0, 0, kSizeM, 1}),
             .mesh = mesh});
        de::render::SceneLighting light{.sun_direction = {0, 0, 1}};
        light.shadows = {.resolution = 512, .distance_m = 8.0f, .split_lambda = 0.0f};
        de::render::ShadowCascade cascades[de::render::kShadowCascadeCount];
        const float sun[3] = {0, 0, 1};
        de::render::fit_shadow_cascades(frame.camera.transform, de::DVec3(0.0), 1.0f, 0.1f, 1.0f, sun, light.shadows,
                                        cascades);
        REQUIRE(cascades[1].texel_m > cascades[0].texel_m);

        const auto drawn = [&](float min_texels) {
            scene.renderer.set_shadow_min_texels(min_texels);
            scene.submit_for_stats(frame, light, de::render::SceneView::ShadowMask);
            return scene.renderer.stats().visible_shadow_draws;
        };
        const uint32_t all = drawn(0.0f);
        REQUIRE(all >= 2);
        CHECK(drawn(1e6f) == 0);
        // Its bounding sphere: half a diagonal of the square, whose mesh is 1 m wide.
        const float diameter_m = kSizeM * std::numbers::sqrt2_v<float>;
        const float between = diameter_m / std::sqrt(cascades[0].texel_m * cascades[1].texel_m);
        const uint32_t finer_only = drawn(between);
        CHECK(finer_only >= 1);
        CHECK(finer_only < all);
        CHECK(drawn(0.99f * diameter_m / cascades[0].texel_m) >= 1);
        CHECK(drawn(1.01f * diameter_m / cascades[0].texel_m) == 0);
    });
}

TEST_CASE("the same part in the same list is one instanced draw, and each instance reads its own record") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        put_squares(scene.vfs, kSides, kGreen, {0.0f});
        const AssetHandle red = scene.load(kCenter);
        const AssetHandle green = scene.load(kSides);
        scene.uploader.flush_and_wait();

        de::world::RenderFrame frame = PoolScene::frame_of({});
        for (const float x : {-1.0f, 0.0f, 1.0f}) {
            frame.instances.push(
                {.transform = de::test::render_transform({0.4f, 0, 0, x, 0, 0.4f, 0, 0, 0, 0, 0.4f, 0}), .mesh = red});
        }
        frame.instances.push(
            {.transform = de::test::render_transform({0.4f, 0, 0, 0, 0, 0.4f, 0, 1, 0, 0, 0.4f, 0}), .mesh = green});
        const de::render::SceneLighting light{.sun_direction = {0, 0, 1}};
        scene.submit_for_stats(frame, light);
        const auto merged = de::test::read_texture(scene.device, scene.target);
        const de::render::SceneRendererStats stats = scene.renderer.stats();
        CHECK(stats.visible_draws == 4);
        CHECK(stats.merged_draws == 2);
        CHECK(stats.draws_lost == 0);
        CHECK(stats.visible_shadow_draws >= 4);
        CHECK(stats.merged_shadow_draws < stats.visible_shadow_draws);

        // A draw each, to compare with: the same picture.
        scene.renderer.set_merged_draws(false);
        scene.submit_for_stats(frame, light);
        CHECK(scene.renderer.stats().visible_draws == 4);
        CHECK(scene.renderer.stats().merged_draws == 4);
        CHECK(scene.renderer.stats().merged_shadow_draws == scene.renderer.stats().visible_shadow_draws);
        const auto apart = de::test::read_texture(scene.device, scene.target);
        CHECK(pixels_apart(merged, apart) == 0);
        scene.renderer.set_merged_draws(true);
    });
}

TEST_CASE("merged draws fit however few draws a call can take, and what has no room is counted and left out") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        put_squares(scene.vfs, kSides, kGreen, {0.0f});
        const AssetHandle red = scene.load(kCenter);
        const AssetHandle green = scene.load(kSides);
        scene.uploader.flush_and_wait();

        // Three of one mesh and three of another, in the same list.
        de::world::RenderFrame frame = PoolScene::frame_of({});
        for (const AssetHandle mesh : {red, green}) {
            for (const float x : {-1.0f, 0.0f, 1.0f}) {
                frame.instances.push({.transform = de::test::render_transform(
                                          {0.4f, 0, 0, x, 0, 0.4f, 0, mesh == red ? 0.6f : -0.6f, 0, 0, 0.4f, 0}),
                                      .mesh = mesh});
            }
        }
        const de::render::SceneLighting light{.sun_direction = {0, 0, 1}};
        scene.renderer.set_max_draws_per_call(2);
        scene.submit_for_stats(frame, light);
        // More objects than a call can draw one by one, all drawn.
        CHECK(scene.renderer.stats().merged_draws == 2);
        CHECK(scene.renderer.stats().draws_lost == 0);
        const auto both = de::test::read_texture(scene.device, scene.target);

        // One draw: one mesh is drawn whole, and the other is counted as lost. The cull's second run stays off its
        // records, and what's drawn doesn't run into the next list.
        scene.renderer.set_max_draws_per_call(1);
        scene.submit_for_stats(frame, light);
        // Each view's list has its own room, so this is what the camera's kept, and what it reports isn't what was
        // lost.
        CHECK(scene.renderer.stats().merged_draws == 1);
        CHECK(scene.renderer.stats().visible_draws == 3);
        CHECK(scene.renderer.stats().visible_triangles == 6);
        CHECK(scene.renderer.stats().draws_lost >= 3);
        const auto one = de::test::read_texture(scene.device, scene.target);
        CHECK(pixels_apart(both, one) > 0);
        // About 19.5 pixels a meter, so a row is 12 pixels from the middle, and each square's center 20 from the next.
        const auto squares = [&](uint32_t row, bool is_red_row) {
            int shown = 0;
            for (const uint32_t x : {kLeft, kMiddle, kRight}) {
                const uint8_t* const color = texel(one, x, row);
                shown += (is_red_row ? color[0] > 200 && color[1] < 60 : color[1] > 200 && color[0] < 60) ? 1 : 0;
            }
            return shown;
        };
        // Whichever got the draw, it's all three of it in their own places: nothing of the other's landed in its
        // records.
        const int red_shown = squares(kMiddle - 12, true);
        const int green_shown = squares(kMiddle + 12, false);
        CHECK(((red_shown == 3 && green_shown == 0) || (red_shown == 0 && green_shown == 3)));
        scene.renderer.set_max_draws_per_call(UINT32_MAX);
        scene.submit_for_stats(frame, light);
        CHECK(scene.renderer.stats().draws_lost == 0);
        CHECK(pixels_apart(both, de::test::read_texture(scene.device, scene.target)) == 0);
    });
}

TEST_CASE("what has no room leaves no draw reading old arguments, a draw per object or merged") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        put_squares(scene.vfs, kSides, kGreen, {0.0f});
        const AssetHandle red = scene.load(kCenter);
        const AssetHandle green = scene.load(kSides);
        scene.uploader.flush_and_wait();
        de::world::RenderFrame frame = PoolScene::frame_of({});
        for (const AssetHandle mesh : {red, green}) {
            for (const float x : {-1.0f, 0.0f, 1.0f}) {
                frame.instances.push({.transform = de::test::render_transform(
                                          {0.4f, 0, 0, x, 0, 0.4f, 0, mesh == red ? 0.6f : -0.6f, 0, 0, 0.4f, 0}),
                                      .mesh = mesh});
            }
        }
        const de::render::SceneLighting light{.sun_direction = {0, 0, 1}};
        const auto squares = [&](const de::core::Array<std::byte>& pixels, uint32_t row, bool is_red_row) {
            int shown = 0;
            for (const uint32_t x : {kLeft, kMiddle, kRight}) {
                const uint8_t* const color = texel(pixels, x, row);
                shown += (is_red_row ? color[0] > 200 && color[1] < 60 : color[1] > 200 && color[0] < 60) ? 1 : 0;
            }
            return shown;
        };
        // A draw each: the frames before it leave every slot holding a real draw, of all six.
        scene.renderer.set_merged_draws(false);
        scene.submit_for_stats(frame, light);
        CHECK(squares(de::test::read_texture(scene.device, scene.target), kMiddle - 12, true) == 3);
        // Room for 4: the first mesh takes 3 slots, the second is left out, and the slot of its it could have had draws
        // nothing rather than what was there.
        scene.renderer.set_max_draws_per_call(4);
        scene.submit_for_stats(frame, light);
        const auto pixels = de::test::read_texture(scene.device, scene.target);
        const int red_shown = squares(pixels, kMiddle - 12, true);
        const int green_shown = squares(pixels, kMiddle + 12, false);
        CHECK(((red_shown == 3 && green_shown == 0) || (red_shown == 0 && green_shown == 3)));
        CHECK(scene.renderer.stats().visible_draws == 3);
        CHECK(scene.renderer.stats().merged_draws == 3);
        CHECK(scene.renderer.stats().draws_lost >= 3);
        scene.renderer.set_merged_draws(true);
        scene.renderer.set_max_draws_per_call(UINT32_MAX);
    });
}

TEST_CASE("a frame finds nothing left of the parts the last frame used") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        put_squares(scene.vfs, kSides, kGreen, {0.0f});
        const AssetHandle red = scene.load(kCenter);
        const AssetHandle green = scene.load(kSides);
        scene.uploader.flush_and_wait();
        // From both meshes to one, and back, many times: a key left over would count records that aren't there.
        const auto frame_with = [&](bool with_green) {
            de::world::RenderFrame frame = PoolScene::frame_of({});
            frame.instances.push(
                {.transform = de::test::render_transform({0.4f, 0, 0, -1, 0, 0.4f, 0, 0, 0, 0, 0.4f, 0}), .mesh = red});
            if (with_green) {
                frame.instances.push(
                    {.transform = de::test::render_transform({0.4f, 0, 0, 1, 0, 0.4f, 0, 0, 0, 0, 0.4f, 0}),
                     .mesh = green});
            }
            return frame;
        };
        const auto shown = [&](bool with_green) {
            scene.submit_for_stats(frame_with(with_green));
            return de::test::read_texture(scene.device, scene.target);
        };
        const de::core::Array<std::byte> both = shown(true);
        const de::core::Array<std::byte> only_red = shown(false);
        for (int i = 0; i < 4; ++i) {
            CHECK(pixels_apart(only_red, shown(false)) == 0);
            CHECK(scene.renderer.stats().visible_draws == 1);
            CHECK(scene.renderer.stats().merged_draws == 1);
            CHECK(pixels_apart(both, shown(true)) == 0);
            CHECK(scene.renderer.stats().visible_draws == 2);
            CHECK(scene.renderer.stats().merged_draws == 2);
        }
    });
}

TEST_CASE("merged draws have a record for every object, however few draws a call can take") {
    with_pool({.max_instances = 4096}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle mesh = scene.load(kCenter);
        scene.uploader.flush_and_wait();
        // More than the smallest room the records are given, in one list and one draw.
        constexpr int kCount = 3000;
        de::world::RenderFrame frame = PoolScene::frame_of({});
        for (int i = 0; i < kCount; ++i) {
            const float x = -1.0f + (2.0f * static_cast<float>(i % 60) / 60.0f);
            const int row = i / 60;
            const float y = -1.0f + (2.0f * static_cast<float>(row) / 50.0f);
            frame.instances.push(
                {.transform = de::test::render_transform({0.01f, 0, 0, x, 0, 0.01f, 0, y, 0, 0, 0.01f, 0}),
                 .mesh = mesh});
        }
        scene.renderer.set_max_draws_per_call(2);
        scene.submit_for_stats(frame);
        CHECK(scene.renderer.stats().visible_draws == kCount);
        CHECK(scene.renderer.stats().merged_draws == 1);
        CHECK(scene.renderer.stats().draws_lost == 0);
        scene.renderer.set_max_draws_per_call(UINT32_MAX);
    });
}

TEST_CASE("a cascade drops casters past either end of its depth") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle mesh = scene.load(kCenter);
        scene.uploader.flush_and_wait();
        // With the cull, and with all a map could hold, which is what the cull's pictures are compared with.
        for (const bool cull : {true, false}) {
            CAPTURE(cull);
            scene.renderer.set_shadow_caster_cull(cull);
            de::world::RenderFrame frame = PoolScene::frame_of({mesh});
            de::render::SceneLighting light{.sun_direction = {0, 0, 1}}; // straight behind the camera
            light.shadows = {.resolution = 512, .distance_m = 8.0f, .split_lambda = 0.0f};
            scene.submit_for_stats(frame, light, de::render::SceneView::ShadowMask);
            const uint32_t in_reach = scene.renderer.stats().visible_shadow_draws;
            CHECK(in_reach > 0);
            // Well past how far toward the sun a cascade reaches (200 m at most), straight up the sun's direction.
            frame.instances[0].transform.set_position_m(2, 5000.0f);
            scene.submit_for_stats(frame, light, de::render::SceneView::ShadowMask);
            CHECK(scene.renderer.stats().visible_shadow_draws == 0);
            // And past the far end of every cascade.
            frame.instances[0].transform.set_position_m(2, -5000.0f);
            scene.submit_for_stats(frame, light, de::render::SceneView::ShadowMask);
            CHECK(scene.renderer.stats().visible_shadow_draws == 0);
        }
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
            if (mode == de::asset::AlphaMode::Masked) {
                // One more parameter: a different size, so the reload can't be missed however soon this is.
                build.set_param(de::asset::material::kEmissive, 0.0f, 0.0f, 0.0f, 1.0f);
            }
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
    return {.transform = de::test::render_transform({1, 0, 0, x_m, 0, 1, 0, 0, 0, 0, 1, 0}),
            .previous_transform = de::test::render_transform({1, 0, 0, x_m, 0, 1, 0, 0, 0, 0, 1, 0}),
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

TEST_CASE("the render origin moves with nothing sent, and every pass still finds the instances") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle red = scene.load(kCenter);
        scene.uploader.flush_and_wait();

        // 10,000 km out, where a float couldn't hold a place: a 2 m wall, and a square casting a shadow on it. The same
        // layout as the shadow caster test above.
        const de::DVec3 world{10'240'000.0, 0.0, 0.0};
        const auto at_world = [&](uint32_t slot, float scale, double x, double y, double z) {
            de::world::RenderInstance instance;
            const float matrix[12] = {scale, 0, 0, 0, 0, scale, 0, 0, 0, 0, scale, 0};
            instance.transform = de::test::render_transform(matrix);
            instance.transform.set_position_m(0, world.x + x);
            instance.transform.set_position_m(1, world.y + y);
            instance.transform.set_position_m(2, world.z + z);
            instance.previous_transform = instance.transform;
            instance.mesh = red;
            instance.slot = slot;
            return instance;
        };
        de::world::RenderFrame frame = PoolScene::frame_of({});
        frame.origin = world;
        frame.camera.previous_valid = true;
        std::memcpy(frame.camera.previous_transform, frame.camera.transform, sizeof(frame.camera.transform));
        frame.slot_count = 2;
        frame.instances.push(at_world(0, 2.0f, 0.0, 0.0, 0.0));
        frame.instances.push(at_world(1, 0.5f, 0.6, 0.6, 1.0));
        de::render::SceneLighting light{.sun_direction = {1, 0, 2}};
        light.shadows = {.resolution = 512, .distance_m = 8.0f, .split_lambda = 0.0f};

        const auto pictures = [&] {
            std::array<de::core::Array<std::byte>, 3> out;
            const de::render::SceneView views[3] = {de::render::SceneView::BaseColor, de::render::SceneView::ShadowMask,
                                                    de::render::SceneView::Motion};
            for (size_t v = 0; v < 3; ++v) {
                scene.submit_for_stats(frame, light, views[v]);
                frame.instances.clear(); // sent once
                out[v] = de::test::read_texture(scene.device, scene.target);
            }
            return out;
        };
        // The debug view of motion: grey is none. Nothing here moved, so a wrong pose a frame ago is all that could
        // show.
        const auto no_motion = [](const de::core::Array<std::byte>& motion) {
            const uint8_t* const center = texel(motion, kMiddle, kMiddle);
            return std::abs(int{center[0]} - 127) <= 3 && std::abs(int{center[1]} - 127) <= 3;
        };
        const auto here = pictures();
        CHECK(no_motion(here[2]));
        CHECK(scene.renderer.stats().instance_updates == 0);
        CHECK(scene.renderer.stats().visible_draws == 2);
        CHECK(is_red(here[0], kMiddle));
        CHECK(texel(here[1], kMiddle + 2, kMiddle - 12)[0] < 40); // the shadow
        CHECK(texel(here[1], kMiddle - 12, kMiddle)[0] > 215);    // lit

        // The origin moves a step along x: only what's relative to it changes, and nothing's sent.
        constexpr double kMoved = de::world::kRenderOriginStep;
        frame.origin.x -= kMoved;
        frame.camera.transform[3] = static_cast<float>(kMoved);
        frame.camera.previous_transform[3] = static_cast<float>(kMoved);
        const auto there = pictures();
        CHECK(scene.renderer.stats().instance_updates == 0);
        CHECK(scene.renderer.stats().visible_draws == 2);
        CHECK(texel(there[1], kMiddle + 2, kMiddle - 12)[0] < 40);
        CHECK(no_motion(there[2]));
        for (size_t v = 0; v < 3; ++v) {
            int differs = 0;
            for (uint32_t y = 0; y < kSize; ++y) {
                for (uint32_t x = 0; x < kSize; ++x) {
                    for (int c = 0; c < 3; ++c) {
                        differs += std::abs(int{texel(here[v], x, y)[c]} - int{texel(there[v], x, y)[c]}) > 8 ? 1 : 0;
                    }
                }
            }
            CAPTURE(v);
            // A float's rounding can tip a pixel on a shadow's edge: one on CI's software GPU, none on this machine's.
            CHECK(differs <= (v == 1 ? 12 : 0));
        }
    });
}

TEST_CASE("a freed slot draws nothing, until something else takes it") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        put_squares(scene.vfs, kSides, kGreen, {0.0f});
        const AssetHandle red = scene.load(kCenter);
        const AssetHandle green = scene.load(kSides);
        scene.uploader.flush_and_wait();

        de::world::RenderFrame frame = PoolScene::frame_of({});
        frame.slot_count = 2;
        frame.instances.push(instance_at(red, 0, -1.0f));
        frame.instances.push(instance_at(red, 1, 1.0f));
        scene.submit(frame);
        frame.instances.clear();
        frame.freed_slots.push(0);
        scene.submit_for_stats(frame);
        CHECK(scene.renderer.stats().instances == 1);
        CHECK(scene.renderer.stats().draws == 1); // and the draw lists know
        CHECK(scene.renderer.stats().visible_draws == 1);
        de::core::Array<std::byte> pixels = de::test::read_texture(scene.device, scene.target);
        CHECK_FALSE(is_red(pixels, kLeft));
        CHECK(is_red(pixels, kRight));

        frame.freed_slots.clear();
        frame.instances.push(instance_at(green, 0, 0.0f));
        scene.submit_for_stats(frame);
        CHECK(scene.renderer.stats().instances == 2);
        pixels = de::test::read_texture(scene.device, scene.target);
        CHECK(is_green(pixels, kMiddle));
        CHECK(is_red(pixels, kRight));
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

TEST_CASE("a slot with nothing in it comes first, and the rest still draw") {
    // Lanes of a compute group: llvmpipe crashed with the first gone and the others going on.
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle red = scene.load(kCenter);
        scene.uploader.flush_and_wait();
        de::world::RenderFrame frame = PoolScene::frame_of({});
        frame.slot_count = 2;
        frame.instances.push(instance_at(red, 1, 0.0f));
        scene.submit(frame);
        CHECK(is_red(de::test::read_texture(scene.device, scene.target), kMiddle));

        // Out of view, the same way.
        frame.instances.clear();
        frame.instances.push(instance_at(red, 0, 50.0f));
        frame.instances.push(instance_at(red, 1, 0.0f));
        scene.submit(frame);
        CHECK(is_red(de::test::read_texture(scene.device, scene.target), kMiddle));
    });
}

TEST_CASE("nothing culls against a mesh table that isn't there yet") {
    with_pool({.frame_budget_bytes = 1}, [](const PoolScene& scene) { // one upload a frame: it's behind the defaults
        CHECK_FALSE(scene.gpu_assets.mesh_table_ready());
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle mesh = scene.assets.load<MeshAsset>(kCenter);
        de::world::RenderFrame frame = PoolScene::frame_of({});
        frame.slot_count = 1;
        frame.instances.push(instance_at(mesh, 0, 0.0f));
        scene.submit(frame);
        frame.instances.clear();
        bool ready = false;
        for (int i = 0; i < 20 && !ready; ++i) {
            scene.submit(frame);
            ready = scene.gpu_assets.mesh_table_ready();
        }
        CHECK(ready);
    });
}

TEST_CASE("an instance that found the mesh table full gets its turn") {
    with_pool({.max_meshes = 1}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        put_squares(scene.vfs, kSides, kRed, {0.0f});
        const AssetHandle first = scene.load(kCenter);
        const AssetHandle second = scene.load(kSides);
        scene.uploader.flush_and_wait();
        de::world::RenderFrame frame = PoolScene::frame_of({});
        frame.slot_count = 1;
        frame.instances.push(instance_at(second, 0, 0.0f)); // no slot for it
        scene.submit(frame);
        CHECK_FALSE(scene.renderer.instances_stale());
        frame.instances.clear();
        scene.submit(frame);
        CHECK_FALSE(scene.renderer.instances_stale());

        scene.assets.release(first); // now there is
        scene.assets.update();
        scene.submit(frame);
        CHECK(scene.renderer.instances_stale());
        scene.submit(frame);
        CHECK_FALSE(scene.renderer.instances_stale());
    });
}

TEST_CASE("a mesh short of a mesh slot gets in when one comes free, though no place did") {
    with_pool({.max_meshes = 1}, [](const PoolScene& scene) {
        // A handle that never loads holds the only slot, and has no place in the pool to give back.
        const AssetHandle never = scene.assets.load<MeshAsset>(AssetId::from_value(0xc00));
        REQUIRE(scene.gpu_assets.mesh_slot(never) != de::render::kNoMeshSlot);
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle mesh = scene.load(kCenter);
        scene.gpu_assets.update();
        CHECK(scene.gpu_assets.stats().meshes_dropped == 1);

        scene.assets.wait_all();
        scene.assets.release(never);
        scene.assets.update();
        CHECK(scene.gpu_assets.stats().meshes_dropped == 0);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        CHECK(scene.gpu_assets.mesh(mesh) != nullptr);
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
        frame.instances[1].transform.set_position_m(0, 1.0f);
        frame.instances[2].transform.set_position_m(0, 50.0f);
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

        // Any size has no room until its old place is free. A different size, so the reload can't be missed however
        // soon this is after the first write.
        put_squares(scene.vfs, kSides, kRed, {-1.5f});
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
        scene.renderer.set_lod_fade(0.0f); // where a LOD comes in; just past that, the fade would draw both
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
            frame.instances[0].transform = de::test::render_transform(transform);
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
            frame.instances[0].transform = de::test::render_transform(transform);
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
            frame.instances[0].transform = de::test::render_transform(transform);
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

namespace {

// What fades are tested with: a grid at this share of the error limit, seen head on, and a look at the middle of it.
struct FadeView {
    const PoolScene& scene;
    AssetHandle grid;
    float error_m;

    struct Pixels {
        uint32_t first = 0;  // of the first material: red
        uint32_t second = 0; // green
        uint32_t other = 0;
    };
    static constexpr uint32_t kHalf = 12;
    static constexpr uint32_t kBox = ((2 * kHalf) + 1) * ((2 * kHalf) + 1);

    // A LOD of error_m is at that share of the limit, with the default pixel of error.
    de::world::RenderFrame frame_at(float share) const {
        const GpuMesh* const mesh = scene.gpu_assets.mesh(grid);
        REQUIRE(mesh != nullptr);
        const float pixels_per_radian = static_cast<float>(kSize) / (2.0f * std::tan(0.5f));
        const float distance = (error_m * pixels_per_radian / share) + mesh->bounds_radius_m;
        de::world::RenderFrame frame = PoolScene::frame_of({grid});
        const float transform[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 3.0f - distance};
        frame.instances[0].transform = de::test::render_transform(transform);
        frame.instances[0].previous_transform = de::test::render_transform(transform); // it isn't moving
        return frame;
    }

    Pixels look_at(float share) const {
        scene.submit_for_stats(frame_at(share));
        return count(de::test::read_texture(scene.device, scene.target));
    }

    static Pixels count(const de::core::Array<std::byte>& pixels) {
        Pixels counts;
        for (uint32_t y = kMiddle - kHalf; y <= kMiddle + kHalf; ++y) {
            for (uint32_t x = kMiddle - kHalf; x <= kMiddle + kHalf; ++x) {
                const uint8_t* pixel = texel(pixels, x, y);
                if (pixel[0] > 200 && pixel[1] < 50) {
                    ++counts.first;
                } else if (pixel[1] > 200 && pixel[0] < 50) {
                    ++counts.second;
                } else {
                    ++counts.other;
                }
            }
        }
        return counts;
    }
};

constexpr PoolOptions kFadeOptions{.pool_vertices = 16384, .pool_indices = 131072};
constexpr float kFadeError = 0.02f; // about 2.6 m away at the limit, which fills the box

} // namespace

TEST_CASE("a LOD fading in has its share of the pixels, and the one it takes over from has all the rest") {
    with_pool(kFadeOptions, [](const PoolScene& scene) {
        put_colored_lods(scene.vfs, kGrid, kRed, kGreen, {{.material = 0}, {.material = 1, .error_m = kFadeError}});
        const AssetHandle grid = scene.load(kGrid);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        const FadeView view{.scene = scene, .grid = grid, .error_m = kFadeError};
        const auto stats = [&] { return scene.renderer.stats(); };

        // Short of the limit, only the finer. Past the end of the fade, only the coarser.
        auto seen = view.look_at(1.05f);
        CHECK(seen.first == FadeView::kBox);
        CHECK(stats().visible_draws == 1);
        CHECK(stats().visible_fading_draws == 0);
        seen = view.look_at(0.75f);
        CHECK(seen.second == FadeView::kBox);
        CHECK(stats().visible_draws == 1);
        CHECK(stats().visible_fading_draws == 0);

        // On the way, both, with the coarser's share growing, and each pixel given to exactly one.
        const GpuMesh* const mesh = scene.gpu_assets.mesh(grid);
        const uint32_t triangles = (mesh->lod_parts(0)[0].index_count / 3) + (mesh->lod_parts(1)[0].index_count / 3);
        for (const float fade : {0.1f, 0.25f, 0.5f, 0.75f, 0.9f}) {
            CAPTURE(fade);
            seen = view.look_at(1.0f - (0.2f * fade));
            CHECK(seen.other == 0);
            CHECK(seen.first + seen.second == FadeView::kBox);
            CHECK(static_cast<float>(seen.second) / FadeView::kBox == doctest::Approx(fade).epsilon(0.08));
            CHECK(stats().visible_draws == 2);
            CHECK(stats().visible_fading_draws == 2);
            CHECK(stats().visible_triangles == triangles);
        }

        // With no fade it cuts where a fade starts, and draws one.
        scene.renderer.set_lod_fade(0.0f);
        seen = view.look_at(1.02f);
        CHECK(seen.first == FadeView::kBox);
        seen = view.look_at(0.98f);
        CHECK(seen.second == FadeView::kBox);
        CHECK(stats().visible_draws == 1);
        CHECK(stats().visible_fading_draws == 0);
        // With a longer one, it's still on the way where the default has finished.
        scene.renderer.set_lod_fade(0.5f);
        seen = view.look_at(0.75f);
        CHECK(seen.other == 0);
        CHECK(static_cast<float>(seen.second) / FadeView::kBox == doctest::Approx(0.5f).epsilon(0.08));
        scene.renderer.set_lod_fade(0.2f);

        // LOD 0 forced: no fade, though it's in the middle of one.
        scene.renderer.set_lod_error_pixels(0.0f);
        seen = view.look_at(0.9f);
        CHECK(seen.first == FadeView::kBox);
        CHECK(stats().visible_draws == 1);
        CHECK(stats().visible_fading_draws == 0);
    });
}

TEST_CASE("LOD 0 has nothing to fade from, whatever its error says") {
    with_pool(kFadeOptions, [](const PoolScene& scene) {
        // One LOD, so no room in the fading lists: a fade would lose it.
        put_colored_lods(scene.vfs, kGrid, kRed, kGreen, {{.material = 0, .error_m = kFadeError}});
        const AssetHandle grid = scene.load(kGrid);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        const FadeView view{.scene = scene, .grid = grid, .error_m = kFadeError};
        const auto seen = view.look_at(0.9f);
        CHECK(seen.first == FadeView::kBox);
        CHECK(scene.renderer.stats().visible_draws == 1);
        CHECK(scene.renderer.stats().visible_fading_draws == 0);
        CHECK(scene.renderer.stats().draws_lost == 0);
    });
}

TEST_CASE("a fade between LODs moves with TAA's samples, so the history blends it") {
    with_pool(kFadeOptions, [](const PoolScene& scene) {
        put_colored_lods(scene.vfs, kGrid, kRed, kGreen, {{.material = 0}, {.material = 1, .error_m = kFadeError}});
        const AssetHandle grid = scene.load(kGrid);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        const FadeView view{.scene = scene, .grid = grid, .error_m = kFadeError};
        de::world::RenderFrame frame = view.frame_at(0.9f); // half way
        // A camera that was here last frame too: without that, this is a cut, and TAA has no history.
        std::memcpy(frame.camera.previous_transform, frame.camera.transform, sizeof(frame.camera.transform));
        frame.camera.previous_vertical_fov_rad = frame.camera.vertical_fov_rad;
        frame.camera.previous_near_m = frame.camera.near_m;
        frame.camera.previous_valid = true;
        const de::render::SceneLighting light{.sun_direction = {0, 0, 1}};
        // How far the middle's pixels are from each other in red, as a standard deviation.
        const auto spread = [&](bool taa) {
            // Long enough for the history to settle.
            for (uint32_t i = 0; i < (taa ? 48U : 1U); ++i) {
                ++frame.number;
                scene.submit(frame, light, de::render::SceneView::Lit, taa);
            }
            const auto pixels = de::test::read_texture(scene.device, scene.target);
            float total = 0.0f;
            float squares = 0.0f;
            for (uint32_t y = kMiddle - FadeView::kHalf; y <= kMiddle + FadeView::kHalf; ++y) {
                for (uint32_t x = kMiddle - FadeView::kHalf; x <= kMiddle + FadeView::kHalf; ++x) {
                    const auto red = static_cast<float>(texel(pixels, x, y)[0]);
                    total += red;
                    squares += red * red;
                }
            }
            const float mean = total / FadeView::kBox;
            return std::sqrt(std::max((squares / FadeView::kBox) - (mean * mean), 0.0f));
        };
        const float without = spread(false);
        const float with = spread(true);
        CHECK(without > 50.0f); // red pixels and green ones
        // About a tenth, from two colors as far apart as they go, and a frame's own share is 8%.
        CHECK(with < without / 4.0f);
    });
}

TEST_CASE("a fade is over before the next LOD starts to come in, however near it is") {
    with_pool(kFadeOptions, [](const PoolScene& scene) {
        constexpr float kNext = 1.05f;
        put_colored_lods(
            scene.vfs, kGrid, kRed, kGreen,
            {{.material = 0}, {.material = 1, .error_m = kFadeError}, {.material = 0, .error_m = kFadeError * kNext}});
        const AssetHandle grid = scene.load(kGrid);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        const FadeView view{.scene = scene, .grid = grid, .error_m = kFadeError};
        // The next comes in at 1 / 1.05 = 0.952 of this one's limit. Just before, this one has nearly all of it.
        auto seen = view.look_at(0.96f);
        CHECK(static_cast<float>(seen.second) / FadeView::kBox > 0.7f);
        // And just after it does, still nearly all, not less than it had.
        seen = view.look_at(0.945f);
        CHECK(static_cast<float>(seen.second) / FadeView::kBox > 0.85f);
    });
}

TEST_CASE("a fade passes over a LOD with no more error than the one before it") {
    with_pool(kFadeOptions, [](const PoolScene& scene) {
        put_colored_lods(
            scene.vfs, kGrid, kRed, kGreen,
            {{.material = 0}, {.material = 1, .error_m = kFadeError}, {.material = 1, .error_m = kFadeError}});
        const AssetHandle grid = scene.load(kGrid);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        const FadeView view{.scene = scene, .grid = grid, .error_m = kFadeError};
        // The middle one would never be seen alone, so what it takes over from is LOD 0.
        const auto seen = view.look_at(0.9f);
        CHECK(seen.other == 0);
        CHECK(static_cast<float>(seen.first) / FadeView::kBox == doctest::Approx(0.5f).epsilon(0.08));
    });
}

TEST_CASE("what's fading keeps its alpha test and which sides it shows") {
    with_pool(kFadeOptions, [](const PoolScene& scene) {
        using de::asset::AlphaMode;
        using de::asset::MaterialFlags;
        constexpr AssetId kCutOut = AssetId::from_value(0x76);
        constexpr AssetId kTwoSided = AssetId::from_value(0x77);
        const auto put = [&](AssetId id, AlphaMode mode, MaterialFlags flags, float red, float green, float alpha) {
            de::cooker::MaterialBuild build;
            build.alpha_mode = mode;
            build.flags = flags;
            build.set_param(de::asset::material::kBaseColor, red, green, 0.0f, alpha);
            const auto material = de::cooker::cook_material(build, id);
            REQUIRE(
                scene.vfs.write_file(de::asset::cooked_path(id, AssetType::Material).c_str(), material.span()).ok());
        };
        put(kCutOut, AlphaMode::Masked, MaterialFlags::None, 1.0f, 0.0f, 0.0f);
        put(kTwoSided, AlphaMode::Opaque, MaterialFlags::DoubleSided, 0.0f, 1.0f, 1.0f);

        // LOD 0 entirely cut out, LOD 1 not: only LOD 1's share shows, and nothing red.
        put_colored_lods(scene.vfs, kGrid, kCutOut, kGreen, {{.material = 0}, {.material = 1, .error_m = kFadeError}});
        const AssetHandle masked = scene.load(kGrid);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        const FadeView masked_view{.scene = scene, .grid = masked, .error_m = kFadeError};
        auto seen = masked_view.look_at(0.9f);
        CHECK(seen.first == 0);
        CHECK(static_cast<float>(seen.second) / FadeView::kBox == doctest::Approx(0.5f).epsilon(0.08));
        CHECK(scene.renderer.stats().visible_fading_draws == 2);

        // A masked material that isn't cut out gets its share, as any other does.
        constexpr AssetId kShown = AssetId::from_value(0x79);
        constexpr AssetId kShownMesh = AssetId::from_value(0x7a);
        put(kShown, AlphaMode::Masked, MaterialFlags::None, 1.0f, 0.0f, 1.0f);
        // It's the coarser, and in front: drawn without its share, it would cover everything.
        put_colored_lods(scene.vfs, kShownMesh, kGreen, kShown,
                         {{.material = 0}, {.material = 1, .error_m = kFadeError}});
        const AssetHandle shown = scene.load(kShownMesh);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        const FadeView shown_view{.scene = scene, .grid = shown, .error_m = kFadeError};
        seen = shown_view.look_at(0.9f);
        CHECK(seen.other == 0);
        CHECK(static_cast<float>(seen.first) / FadeView::kBox == doctest::Approx(0.5f).epsilon(0.08));

        // From behind, a double-sided LOD shows and a single-sided one doesn't.
        constexpr AssetId kBoth = AssetId::from_value(0x78);
        put_colored_lods(scene.vfs, kBoth, kRed, kTwoSided, {{.material = 0}, {.material = 1, .error_m = kFadeError}});
        const AssetHandle both = scene.load(kBoth);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        const FadeView both_view{.scene = scene, .grid = both, .error_m = kFadeError};
        de::world::RenderFrame frame = both_view.frame_at(0.9f);
        // Half a turn about Y.
        frame.instances[0].transform.rows[0].linear[0] = -1.0f;
        frame.instances[0].transform.rows[2].linear[2] = -1.0f;
        scene.submit_for_stats(frame);
        seen = FadeView::count(de::test::read_texture(scene.device, scene.target));
        CHECK(seen.first == 0);
        CHECK(static_cast<float>(seen.second) / FadeView::kBox == doctest::Approx(0.5f).epsilon(0.08));
    });
}

TEST_CASE("every instance can be fading at once, and a mesh with one LOD needs no room for that") {
    with_pool(kFadeOptions, [](const PoolScene& scene) {
        put_colored_lods(scene.vfs, kGrid, kRed, kGreen, {{.material = 0}, {.material = 1, .error_m = kFadeError}});
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle grid = scene.load(kGrid);
        const AssetHandle square = scene.load(kCenter);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        const FadeView view{.scene = scene, .grid = grid, .error_m = kFadeError};

        scene.submit_for_stats(PoolScene::frame_of({square}));
        const uint32_t calls_one_lod = scene.renderer.stats().indirect_calls;
        CHECK(scene.renderer.stats().visible_fading_draws == 0);

        de::world::RenderFrame frame = view.frame_at(0.9f);
        const de::world::RenderInstance first = frame.instances[0];
        constexpr uint32_t kCount = 16;
        for (uint32_t i = 1; i < kCount; ++i) {
            frame.instances.push(first);
        }
        scene.submit_for_stats(frame);
        CHECK(scene.renderer.stats().visible_draws == 2 * kCount);
        CHECK(scene.renderer.stats().visible_fading_draws == 2 * kCount);
        CHECK(scene.renderer.stats().draws_lost == 0);
        CHECK(scene.renderer.stats().indirect_calls > calls_one_lod);
        // With no fades, no room for them.
        scene.renderer.set_lod_fade(0.0f);
        scene.submit_for_stats(frame);
        CHECK(scene.renderer.stats().indirect_calls == calls_one_lod);
    });
}

TEST_CASE("shadow cascades cut between LODs while the camera fades") {
    with_pool(kFadeOptions, [](const PoolScene& scene) {
        put_colored_lods(scene.vfs, kGrid, kRed, kGreen, {{.material = 0}, {.material = 1, .error_m = kFadeError}});
        const AssetHandle grid = scene.load(kGrid);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        const FadeView view{.scene = scene, .grid = grid, .error_m = kFadeError};
        const GpuMesh* const mesh = scene.gpu_assets.mesh(grid);
        REQUIRE(mesh != nullptr);
        const uint32_t lod0 = mesh->lod_parts(0)[0].index_count / 3;
        const uint32_t lod1 = mesh->lod_parts(1)[0].index_count / 3;
        const de::render::SceneLighting light{.sun_direction = {0, 0, 1}};
        // Cascades of many sizes, so each passes through the sizes where this LOD would be fading in it. A cascade has
        // room for one LOD of it, and takes no more.
        de::render::SceneLighting shadowed = light;
        for (int step = 0; step < 29; ++step) {
            const float distance_m = 2.0f * std::pow(1.2f, static_cast<float>(step)); // up to about 400 m
            CAPTURE(distance_m);
            shadowed.shadows = {.distance_m = distance_m, .split_lambda = 0.0f};
            scene.submit_for_stats(view.frame_at(0.9f), shadowed, de::render::SceneView::ShadowMask);
            const de::render::SceneRendererStats stats = scene.renderer.stats();
            CHECK(stats.visible_fading_draws == 2);
            CHECK(stats.visible_shadow_draws > 0);
            CHECK(stats.draws_lost == 0);
            CHECK(stats.visible_shadow_triangles >= stats.visible_shadow_draws * std::min(lod0, lod1));
            CHECK(stats.visible_shadow_triangles <= stats.visible_shadow_draws * std::max(lod0, lod1));
        }
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

namespace {

// A red square where the frame starts, and a green pair of squares somewhere out of its sight. They have 2 and 4
// triangles, so what the cull kept says which of them it found, in any view.
struct FreezeScene {
    AssetHandle here;
    AssetHandle there;

    explicit FreezeScene(const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        put_squares(scene.vfs, kSides, kGreen, {-1.0f, 1.0f});
        here = scene.load(kCenter);
        there = scene.load(kSides);
        scene.uploader.flush_and_wait();
    }

    // Each at its place in the world, with the frame's origin there too.
    de::world::RenderFrame frame_at(const de::DVec3& origin, const de::DVec3& there_m) const {
        de::world::RenderFrame frame = PoolScene::frame_of({here, there});
        frame.origin = origin;
        for (int axis = 0; axis < 3; ++axis) {
            frame.instances[0].transform.set_position_m(axis, origin[axis]);
            frame.instances[1].transform.set_position_m(axis, there_m[axis]);
        }
        return frame;
    }
};

void put_camera(de::world::RenderFrame& frame, float x_m, float y_m, float z_m) {
    frame.camera.transform[3] = x_m;
    frame.camera.transform[7] = y_m;
    frame.camera.transform[11] = z_m;
}

de::render::SceneLighting freeze_light() {
    de::render::SceneLighting light{.sun_direction = {0, 0, 1}};
    light.shadows = {.resolution = 512, .distance_m = 8.0f, .split_lambda = 0.0f};
    return light;
}

} // namespace

TEST_CASE("a frozen cull keeps every view where it was, while the picture follows the camera") {
    with_pool({}, [](const PoolScene& scene) {
        const FreezeScene assets(scene);
        const de::render::SceneLighting light = freeze_light();
        de::world::RenderFrame frame = assets.frame_at(de::DVec3(0.0), {500.0, 0.0, 0.0});
        const auto kept = [&](uint32_t triangles, uint32_t shadow_triangles) {
            scene.submit_for_stats(frame, light);
            CHECK(scene.renderer.stats().visible_triangles == triangles);
            CHECK(scene.renderer.stats().visible_shadow_triangles == shadow_triangles);
            return de::test::read_texture(scene.device, scene.target);
        };

        // What each place finds, live: there, more triangles in every view.
        scene.submit_for_stats(frame, light);
        const uint32_t shadows_here = scene.renderer.stats().visible_shadow_triangles;
        put_camera(frame, 500.0f, 0.0f, 3.0f);
        scene.submit_for_stats(frame, light);
        const uint32_t shadows_there = scene.renderer.stats().visible_shadow_triangles;
        REQUIRE(shadows_here > 0);
        REQUIRE(shadows_there > shadows_here);
        put_camera(frame, 0.0f, 0.0f, 3.0f);

        // Frozen here: going there shows nothing, though the green squares are right in front of it.
        scene.renderer.set_cull_frozen(true);
        CHECK(is_red(kept(2, shadows_here), kMiddle));
        put_camera(frame, 500.0f, 0.0f, 3.0f);
        const auto frozen_there = kept(2, shadows_here);
        CHECK_FALSE(is_green(frozen_there, kLeft));
        CHECK_FALSE(
            is_red(frozen_there, kMiddle)); // it's drawn, but that's 500 m away: the picture is the live camera's

        // Unfrozen, the very next frame is live. What the cull kept takes a few frames to read.
        scene.renderer.set_cull_frozen(false);
        scene.submit(frame, light);
        CHECK(is_green(de::test::read_texture(scene.device, scene.target), kLeft));
        CHECK(is_green(kept(4, shadows_there), kRight));

        // Frozen again, it's where the camera is now, with nothing left of the first time.
        scene.renderer.set_cull_frozen(true);
        CHECK(is_green(kept(4, shadows_there), kLeft));
        put_camera(frame, 0.0f, 0.0f, 3.0f);
        CHECK_FALSE(is_red(kept(4, shadows_there), kMiddle));
        scene.renderer.set_cull_frozen(false);
        scene.submit(frame, light);
        CHECK(is_red(de::test::read_texture(scene.device, scene.target), kMiddle));
    });
}

TEST_CASE("a cull frozen with nothing to look through freezes at the first camera") {
    with_pool({}, [](const PoolScene& scene) {
        const FreezeScene assets(scene);
        const de::render::SceneLighting light = freeze_light();
        de::world::RenderFrame frame = assets.frame_at(de::DVec3(0.0), {500.0, 0.0, 0.0});
        scene.renderer.set_cull_frozen(true);
        frame.camera.valid = false;
        scene.submit(frame, light);
        frame.camera.valid = true;
        scene.submit_for_stats(frame, light);
        CHECK(scene.renderer.stats().visible_triangles == 2);
        put_camera(frame, 500.0f, 0.0f, 3.0f);
        scene.submit_for_stats(frame, light);
        CHECK(scene.renderer.stats().visible_triangles == 2);
    });
}

TEST_CASE("a frozen view stays in the same place in the world as the render origin moves") {
    with_pool({}, [](const PoolScene& scene) {
        const FreezeScene assets(scene);
        const de::render::SceneLighting light = freeze_light();
        // The green squares are exactly as far from where the origin goes as the red one is from where it was, so a
        // view left as it was would find them instead.
        constexpr double kStep = de::world::kRenderOriginStep;
        const de::DVec3 start{2.0 * kStep, kStep, -kStep};
        const de::DVec3 moved{kStep, -2.0 * kStep, 3.0 * kStep};
        const de::DVec3 next = start + moved;
        de::world::RenderFrame frame = assets.frame_at(start, next);

        scene.renderer.set_cull_frozen(true);
        scene.submit_for_stats(frame, light);
        CHECK(scene.renderer.stats().visible_triangles == 2);
        const uint32_t shadows = scene.renderer.stats().visible_shadow_triangles;
        REQUIRE(shadows > 0);
        const auto kept_is_the_same = [&] {
            scene.submit_for_stats(frame, light);
            CHECK(scene.renderer.stats().visible_triangles == 2);
            CHECK(scene.renderer.stats().visible_shadow_triangles == shadows);
            return de::test::read_texture(scene.device, scene.target);
        };

        // The origin goes, and the camera stays in the world: the same picture.
        frame.origin = next;
        put_camera(frame, static_cast<float>(-moved.x), static_cast<float>(-moved.y),
                   static_cast<float>(3.0 - moved.z));
        CHECK(is_red(kept_is_the_same(), kMiddle));
        // The camera goes with it, to the green squares, which stay out.
        put_camera(frame, 0.0f, 0.0f, 3.0f);
        CHECK_FALSE(is_green(kept_is_the_same(), kLeft));
        // And further on the origin's way, and back: it's how far it is that counts, not how it got there.
        frame.origin = next + moved;
        put_camera(frame, static_cast<float>(-moved.x), static_cast<float>(-moved.y),
                   static_cast<float>(3.0 - moved.z));
        CHECK_FALSE(is_green(kept_is_the_same(), kLeft));
        frame.origin = start;
        CHECK_FALSE(is_red(kept_is_the_same(), kMiddle)); // that camera is relative to start, nowhere near the square
        put_camera(frame, 0.0f, 0.0f, 3.0f);
        CHECK(is_red(kept_is_the_same(), kMiddle));

        // Frozen after the origin has moved is relative to that origin, with nothing left of this one.
        scene.renderer.set_cull_frozen(false);
        frame.origin = next;
        scene.submit(frame, light);
        CHECK(is_green(de::test::read_texture(scene.device, scene.target), kLeft));
        scene.renderer.set_cull_frozen(true);
        scene.submit(frame, light);
        frame.origin = start;
        put_camera(frame, static_cast<float>(moved.x), static_cast<float>(moved.y), static_cast<float>(3.0 + moved.z));
        scene.submit_for_stats(frame, light);
        CHECK(scene.renderer.stats().visible_triangles == 4);
        CHECK(is_green(de::test::read_texture(scene.device, scene.target), kLeft));
    });
}

TEST_CASE("a frozen view's LODs stay as they were, wherever the camera and the render origin go") {
    with_pool({.pool_vertices = 8192, .pool_indices = 65536}, [](const PoolScene& scene) {
        put_grid(scene.vfs, kGrid, kGreen);
        const AssetHandle grid = scene.load(kGrid);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        const GpuMesh* const mesh = scene.gpu_assets.mesh(grid);
        REQUIRE(mesh != nullptr);
        REQUIRE(mesh->lods.size() >= 3);
        scene.renderer.set_lod_fade(0.0f); // so one LOD's triangles are all there are
        const auto triangles = [&](size_t lod) { return mesh->lod_parts(lod)[0].index_count / 3; };

        // Where LOD 1 is the coarsest good enough. The grid's nearest depth is the camera's less its radius.
        const float pixels_per_radian = static_cast<float>(kSize) / (2.0f * std::tan(0.5f));
        const float depth_m = (mesh->lods[1].error_m * pixels_per_radian * 1.1f) + mesh->bounds_radius_m;
        de::world::RenderFrame frame = PoolScene::frame_of({grid});
        const float transform[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 3.0f - depth_m};
        frame.instances[0].transform = de::test::render_transform(transform);
        const auto picked = [&](uint32_t want) {
            scene.submit_for_stats(frame);
            CHECK(scene.renderer.stats().visible_triangles == want);
        };
        picked(triangles(1));

        // Frozen, it stays at that LOD with the camera right up close, where LOD 0 would be picked, and far away.
        scene.renderer.set_cull_frozen(true);
        picked(triangles(1));
        const float close_z = 3.0f - depth_m + (mesh->bounds_radius_m * 1.01f);
        put_camera(frame, 0.0f, 0.0f, close_z);
        picked(triangles(1));
        put_camera(frame, 0.0f, 0.0f, 3.0f + (10.0f * depth_m));
        picked(triangles(1));
        put_camera(frame, 0.0f, 0.0f, 3.0f);

        // The origin moves along the view, either way: the depth a LOD is picked by is the world's. Left relative to
        // the old origin, it would be behind the grid one way, and a kilometer farther the other.
        constexpr auto kStep = static_cast<float>(de::world::kRenderOriginStep);
        for (const float sign : {1.0f, -1.0f}) {
            CAPTURE(sign);
            frame.origin = de::DVec3(0.0, 0.0, static_cast<double>(sign * kStep));
            put_camera(frame, 0.0f, 0.0f, 3.0f - (sign * kStep));
            picked(triangles(1));
        }
        frame.origin = de::DVec3(0.0);
        put_camera(frame, 0.0f, 0.0f, 3.0f);

        // A limit changed meanwhile waits too. Unfrozen, it applies at once.
        scene.renderer.set_lod_error_pixels(0.0f);
        picked(triangles(1));
        scene.renderer.set_cull_frozen(false);
        picked(triangles(0));
        scene.renderer.set_lod_error_pixels(1.0f);
        picked(triangles(1));

        // A cascade's too, whose LODs go by its texels: these change with the shadows, which stay live.
        de::render::SceneLighting light{.sun_direction = {0, 0, 1}};
        const auto shadow_triangles = [&](uint32_t resolution) {
            light.shadows = {.resolution = resolution, .distance_m = 8.0f, .split_lambda = 0.0f};
            scene.submit_for_stats(frame, light, de::render::SceneView::ShadowMask);
            return scene.renderer.stats().visible_shadow_triangles;
        };
        const uint32_t coarse = shadow_triangles(64);
        const uint32_t fine = shadow_triangles(4096);
        REQUIRE(coarse < fine);
        shadow_triangles(64);
        scene.renderer.set_cull_frozen(true);
        shadow_triangles(64);
        CHECK(shadow_triangles(4096) == coarse);
        scene.renderer.set_cull_frozen(false);
        CHECK(shadow_triangles(4096) == fine);
    });
}

TEST_CASE("what was fading when the cull froze keeps its room in the draw lists, whatever the fade is now") {
    with_pool(kFadeOptions, [](const PoolScene& scene) {
        put_colored_lods(scene.vfs, kGrid, kRed, kGreen, {{.material = 0}, {.material = 1, .error_m = kFadeError}});
        const AssetHandle grid = scene.load(kGrid);
        scene.uploader.flush_and_wait();
        scene.gpu_assets.update();
        const FadeView view{.scene = scene, .grid = grid, .error_m = kFadeError};
        const de::world::RenderFrame frame = view.frame_at(0.9f);

        for (const bool merged : {true, false}) {
            CAPTURE(merged);
            scene.renderer.set_merged_draws(merged);
            scene.renderer.set_lod_fade(0.2f);
            scene.renderer.set_cull_frozen(true);
            scene.submit_for_stats(frame);
            CHECK(scene.renderer.stats().visible_fading_draws == 2);
            scene.renderer.set_lod_fade(0.0f);
            scene.submit_for_stats(frame);
            CHECK(scene.renderer.stats().visible_fading_draws == 2);
            CHECK(scene.renderer.stats().draws_lost == 0);
            const FadeView::Pixels seen = FadeView::count(de::test::read_texture(scene.device, scene.target));
            CHECK(seen.first > 0);
            CHECK(seen.second > 0);
            CHECK(seen.other == 0);

            scene.renderer.set_cull_frozen(false);
            scene.submit_for_stats(frame);
            CHECK(scene.renderer.stats().visible_fading_draws == 0);
        }
    });
}

TEST_CASE("a frozen cull holds with draws merged or not, and with the caster cull on or off") {
    with_pool({}, [](const PoolScene& scene) {
        const FreezeScene assets(scene);
        const de::render::SceneLighting light = freeze_light();
        constexpr double kStep = de::world::kRenderOriginStep;
        const de::DVec3 start{kStep, 0.0, 0.0};
        const de::DVec3 next{2.0 * kStep, 0.0, 0.0};
        for (const bool merged : {true, false}) {
            for (const bool caster_cull : {true, false}) {
                CAPTURE(merged);
                CAPTURE(caster_cull);
                scene.renderer.set_merged_draws(merged);
                scene.renderer.set_shadow_caster_cull(caster_cull);
                // Two red squares at one place, which is one draw if they merge, and the green ones where the origin
                // goes.
                de::world::RenderFrame frame = assets.frame_at(start, next);
                const de::world::RenderInstance red = frame.instances[0];
                frame.instances.push(red);
                const uint32_t camera_draws = merged ? 1 : 2;

                scene.renderer.set_cull_frozen(true);
                scene.submit_for_stats(frame, light);
                const uint32_t shadow_draws = scene.renderer.stats().visible_shadow_draws;
                const uint32_t shadow_triangles = scene.renderer.stats().visible_shadow_triangles;
                REQUIRE(shadow_draws > 0);
                const auto holds = [&](uint32_t merged_camera_draws) {
                    scene.submit_for_stats(frame, light);
                    const de::render::SceneRendererStats stats = scene.renderer.stats();
                    CHECK(stats.visible_draws == 2);
                    CHECK(stats.merged_draws == merged_camera_draws);
                    CHECK(stats.visible_triangles == 4);
                    CHECK(stats.visible_shadow_draws == shadow_draws);
                    CHECK(stats.visible_shadow_triangles == shadow_triangles);
                    CHECK(stats.draws_lost == 0);
                };
                holds(camera_draws);

                // The origin and the camera go to the green squares, and they stay out of every view.
                frame.origin = next;
                holds(camera_draws);
                CHECK_FALSE(is_green(de::test::read_texture(scene.device, scene.target), kLeft));

                // Neither setting reaches what's frozen: the caster cull made its planes, and merging only draws them.
                scene.renderer.set_shadow_caster_cull(!caster_cull);
                holds(camera_draws);
                scene.renderer.set_shadow_caster_cull(caster_cull);
                scene.renderer.set_merged_draws(!merged);
                holds(merged ? 2 : 1);
                scene.renderer.set_merged_draws(merged);

                scene.renderer.set_cull_frozen(false);
                scene.submit(frame, light);
                CHECK(is_green(de::test::read_texture(scene.device, scene.target), kLeft));
            }
        }
        scene.renderer.set_merged_draws(true);
        scene.renderer.set_shadow_caster_cull(true);
    });
}

TEST_CASE("a frozen cull picks the casters, and shadows still go through the live camera's cascades") {
    with_pool({}, [](const PoolScene& scene) {
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        const AssetHandle mesh = scene.load(kCenter);
        scene.uploader.flush_and_wait();

        // A floor, and high above it toward the sun, out of the picture, a caster. The camera at z = 3 sees its shadow
        // on the floor, about 5 m ahead of it. It's 3 m wide, but stretched along its flat side: its bounding sphere is
        // 21 m across each way from it, with nothing more to draw.
        de::world::RenderFrame frame = PoolScene::frame_of({});
        frame.instances.push(
            {.transform = de::test::render_transform({100, 0, 0, 0, 0, 0, 100, -1, 0, -100, 0, 0}), .mesh = mesh});
        frame.instances.push(
            {.transform = de::test::render_transform({3, 0, 0, 0, 0, 2, 0, 11, 0, 0, 30, -14}), .mesh = mesh});
        de::render::SceneLighting light{.sun_direction = {0, 1, -1}};
        light.shadows = {.resolution = 512, .distance_m = 8.0f, .split_lambda = 0.0f};
        const auto mask = [&] {
            scene.submit_for_stats(frame, light, de::render::SceneView::ShadowMask);
            return de::test::read_texture(scene.device, scene.target);
        };
        // The floor about 5 m ahead lands on this row.
        const auto in_shadow = [](const de::core::Array<std::byte>& pixels) {
            return texel(pixels, kMiddle, kMiddle + 12)[0] < 40;
        };
        const auto lit_beside_it = [](const de::core::Array<std::byte>& pixels) {
            return texel(pixels, kMiddle + 28, kMiddle + 12)[0] > 215;
        };
        const auto live = mask();
        REQUIRE(in_shadow(live));
        REQUIRE(lit_beside_it(live));

        // Frozen 20 m to the side, the sphere still reaches those cascades, so the caster is in their lists, but their
        // maps stop well short of where it is and where its shadow lands. So the picture here comes out the same only
        // through this camera's cascades.
        put_camera(frame, 20.0f, 0.0f, 3.0f);
        scene.renderer.set_cull_frozen(true);
        scene.submit(frame, light, de::render::SceneView::ShadowMask);
        put_camera(frame, 0.0f, 0.0f, 3.0f);
        const auto frozen_near = mask();
        CHECK(in_shadow(frozen_near));
        CHECK(pixels_apart(live, frozen_near) == 0);

        // Frozen far to the side, the caster is in none of those cascades: no shadow, though the floor is still drawn.
        scene.renderer.set_cull_frozen(false);
        put_camera(frame, 60.0f, 0.0f, 3.0f);
        scene.renderer.set_cull_frozen(true);
        scene.submit(frame, light, de::render::SceneView::ShadowMask);
        put_camera(frame, 0.0f, 0.0f, 3.0f);
        const auto frozen_aside = mask();
        CHECK_FALSE(in_shadow(frozen_aside));
        CHECK(lit_beside_it(frozen_aside));
        CHECK(scene.renderer.stats().visible_draws == 1);

        // And all of it comes back at once.
        scene.renderer.set_cull_frozen(false);
        scene.submit(frame, light, de::render::SceneView::ShadowMask);
        CHECK(pixels_apart(live, de::test::read_texture(scene.device, scene.target)) == 0);
    });
}

TEST_CASE(
    "a cull held frozen stays, and one frozen anew over frames that drew nothing takes the next drawn frame's views") {
    with_pool({}, [](const PoolScene& scene) {
        const FreezeScene assets(scene);
        const de::render::SceneLighting light = freeze_light();
        de::world::RenderFrame frame = assets.frame_at(de::DVec3(0.0), {500.0, 0.0, 0.0});
        const auto kept = [&] {
            scene.submit_for_stats(frame, light);
            return scene.renderer.stats().visible_triangles;
        };

        // The framework says so every frame, drawn or not: that isn't freezing again.
        scene.renderer.set_cull_frozen(true);
        CHECK(kept() == 2);
        put_camera(frame, 500.0f, 0.0f, 3.0f);
        for (int i = 0; i < 3; ++i) {
            scene.renderer.set_cull_frozen(true);
            scene.empty_frame();
        }
        scene.renderer.set_cull_frozen(true);
        CHECK(kept() == 2);

        // Minimized, it's off for a frame and on again. Nothing was drawn meanwhile, and it's a new freeze all the
        // same.
        scene.renderer.set_cull_frozen(false);
        scene.empty_frame();
        scene.renderer.set_cull_frozen(true);
        scene.empty_frame();
        CHECK(kept() == 4);
        CHECK(is_green(de::test::read_texture(scene.device, scene.target), kLeft));
        put_camera(frame, 0.0f, 0.0f, 3.0f);
        CHECK(kept() == 4);
        CHECK_FALSE(is_red(de::test::read_texture(scene.device, scene.target), kMiddle));
    });
}
