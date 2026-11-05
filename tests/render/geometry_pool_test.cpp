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

#include <doctest/doctest.h>

#include <cstdint>
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

// A flat grid 2 m wide, which simplifies into coarser LODs.
void put_grid(const de::platform::Vfs& vfs, AssetId mesh_id, AssetId material) {
    constexpr uint32_t kCells = 32;
    de::cooker::ModelData model;
    de::cooker::ModelPrimitive& primitive = model.primitives.emplace();
    primitive.material = 0;
    for (uint32_t y = 0; y <= kCells; ++y) {
        for (uint32_t x = 0; x <= kCells; ++x) {
            de::cooker::ModelVertex& vertex = primitive.vertices.emplace();
            vertex.position[0] = (static_cast<float>(x) / kCells * 2.0f) - 1.0f;
            vertex.position[1] = (static_cast<float>(y) / kCells * 2.0f) - 1.0f;
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
    put_mesh(vfs, model, mesh_id, material, 3);
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

    void submit(const de::world::RenderFrame& frame) const {
        assets.update();
        assets.wait_all();
        gpu_assets.update();
        device.begin_frame();
        uploader.record();
        graph.begin(kSize, kSize);
        const de::render::GraphTexture color =
            graph.import_texture({.texture = target, .name = "target", .final_state = de::rhi::ResourceState::CopySrc});
        renderer.add_passes(graph, color, de::rhi::Format::RGBA8Unorm, frame, {}, de::render::SceneView::BaseColor);
        graph.execute();
        device.end_frame();
    }

    // The same, and what it drew.
    de::core::Array<std::byte> draw(std::initializer_list<AssetHandle> meshes) const {
        submit(meshes);
        return de::test::read_texture(device, target);
    }

    // Frames until nothing in flight can hold a place back, and the update that frees them.
    void settle() const {
        device.wait_idle();
        empty_frame();
        device.wait_idle();
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
                                                         .pool_indices = options.pool_indices});
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
            put_squares(scene.vfs, mesh_id, material_id, {kind.x_m});
            frame.instances.push({.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}, .mesh = scene.load(mesh_id)});
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
        CHECK(scene.uploader.idle());
    });
}

TEST_CASE("a mesh in the pool isn't drawn before all of it is recorded") {
    with_pool({.frame_budget_bytes = 1}, [](const PoolScene& scene) { // one upload a frame
        put_squares(scene.vfs, kCenter, kRed, {0.0f});
        put_squares(scene.vfs, kSides, kRed, {-1.0f, 1.0f});
        static_cast<void>(scene.load(kCenter));
        scene.uploader.flush_and_wait();
        // Its material is on the GPU already, so it's waiting for only its own two uploads.
        const AssetHandle sides = scene.load(kSides);
        CHECK(scene.gpu_assets.mesh(sides) == nullptr);
        CHECK(scene.gpu_assets.stats().pending > 0);
        scene.empty_frame(); // the vertices
        scene.gpu_assets.update();
        CHECK(scene.gpu_assets.mesh(sides) == nullptr);
        scene.empty_frame(); // the indices
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
        CHECK(scene.uploader.stats().overwrites == overwrites); // the frame never waited for them
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
        // What's in the pool draws; what isn't is skipped.
        const auto pixels = scene.draw({center, sides});
        CHECK(is_red(pixels, kMiddle));
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

TEST_CASE("a mesh's bounds and every LOD are kept, and only LOD 0 is drawn") {
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
