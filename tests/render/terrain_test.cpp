#include "asset/registry.h"
#include "asset/terrain_format.h"
#include "cooker/terrain_cooker.h"
#include "core/job_system.h"
#include "render/graph/render_graph.h"
#include "render/rhi/device.h"
#include "render/rhi/upload.h"
#include "render/scene/gpu_assets.h"
#include "render/scene/scene_renderer.h"
#include "render/scene/terrain.h"
#include "render/shader/shader_compiler.h"
#include "render/shader/shader_library.h"
#include "support/gpu.h"
#include "support/readback.h"
#include "support/sandbox.h"
#include "world/render_extract.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <utility>

using de::DVec3;
using de::asset::AssetId;
using de::asset::AssetType;

namespace {

constexpr AssetId kTerrain = AssetId::from_value(0x71);
constexpr uint32_t kSize = 64;

// A cooked map of rolling hills, side_m across at spacing_m, its corner at the world's origin.
de::cooker::CookedTerrain cook_hills(float side_m, double spacing_m) {
    de::cooker::HeightImage image;
    image.width = static_cast<uint32_t>(side_m / spacing_m) + 1;
    image.depth = image.width;
    image.samples.resize(static_cast<size_t>(image.width) * image.depth);
    for (uint32_t z = 0; z < image.depth; ++z) {
        for (uint32_t x = 0; x < image.width; ++x) {
            const double height = (std::sin(static_cast<double>(x) * spacing_m * 0.01) *
                                   std::cos(static_cast<double>(z) * spacing_m * 0.013) * 0.5) +
                                  0.5;
            image.samples[(static_cast<size_t>(z) * image.width) + x] = static_cast<uint16_t>(height * 65535.0);
        }
    }
    const de::cooker::TerrainSource source{
        .height = {}, .meters_per_pixel = spacing_m, .lowest_m = 0.0, .highest_m = 60.0, .origin_m = {0.0, 0.0}};
    de::cooker::Diagnostics diagnostics(false);
    auto cooked = de::cooker::cook_terrain(source, image, kTerrain, nullptr, "hills.terrain", diagnostics);
    REQUIRE(cooked.ok());
    return std::move(*cooked);
}

// Everything a frame needs, with a map in game:/.
struct Rig {
    de::test::Sandbox sandbox;
    de::core::JobSystem jobs{{.worker_count = 2}};
    de::platform::PathString game_root = sandbox.game();
    de::platform::PathString user_root = sandbox.folder("user");
    de::platform::PathString temp_root = sandbox.folder("temp");
    de::core::Result<de::platform::Vfs> vfs = de::platform::Vfs::create({.game_root = game_root.c_str(),
                                                                         .user_root = user_root.c_str(),
                                                                         .temp_root = temp_root.c_str(),
                                                                         .src_root = DE_SOURCE_DIR,
                                                                         .jobs = &jobs,
                                                                         .game_writable = true});

    void put(const de::cooker::CookedTerrain& cooked) {
        REQUIRE(vfs.ok());
        REQUIRE(
            vfs->write_file(de::asset::cooked_path(kTerrain, AssetType::Terrain).c_str(), cooked.terrain.span()).ok());
        REQUIRE(
            vfs->write_file(de::asset::cooked_path(kTerrain, AssetType::TerrainHeights).c_str(), cooked.heights.span())
                .ok());
    }
};

// Nothing about height stands in the way.
de::core::Array<float> open_reach(const de::asset::TerrainAsset& terrain) {
    de::core::Array<float> reach(de::core::heap_allocator(de::core::MemoryTag::Render));
    reach.resize(static_cast<size_t>(terrain.nodes.count) * 2);
    for (size_t i = 0; i < reach.size(); i += 2) {
        reach[i] = -1e6f;
        reach[i + 1] = 1e6f;
    }
    return reach;
}

const uint8_t* texel(const de::core::Array<std::byte>& pixels, uint32_t x, uint32_t y) {
    return reinterpret_cast<const uint8_t*>(pixels.data()) + ((static_cast<size_t>(y) * kSize + x) * 4);
}

} // namespace

TEST_CASE("terrain wants every root, and what's near the camera, coarsest and nearest first") {
    const de::cooker::CookedTerrain cooked = cook_hills(4096.0f, 2.0);
    const std::span<const std::byte> file = cooked.terrain.span();
    const auto loaded = de::asset::load_in_place<de::asset::TerrainAsset>(file);
    REQUIRE(loaded.ok());
    const de::asset::TerrainAsset& terrain = **loaded;
    const auto levels = terrain.levels.get(file.data());
    REQUIRE(levels.size() == 4); // 16 m to 2 m
    const de::core::Array<float> reach = open_reach(terrain);
    de::core::Array<de::render::TerrainWant> wanted(de::core::heap_allocator(de::core::MemoryTag::Render));

    const uint32_t roots = levels[0].nodes_x * levels[0].nodes_z;
    de::render::terrain_wanted_nodes(terrain, file, reach, DVec3(1000.0, 10.0, 1000.0), wanted);
    uint32_t root_count = 0;
    uint32_t last_level = 0;
    bool finest = false;
    float last_distance = 0.0f;
    for (const de::render::TerrainWant& want : wanted) {
        root_count += want.level == 0 ? 1 : 0;
        finest = finest || want.level == 3;
        CHECK(want.level >= last_level); // the coarsest first
        if (want.level == last_level) {
            CHECK(want.distance_m >= last_distance); // and within a level, the nearest first
        }
        last_level = want.level;
        last_distance = want.distance_m;
        // Nothing past its level's range and margin, roots aside.
        if (want.level > 0) {
            CHECK(want.distance_m <
                  de::render::terrain_level_range_m(levels[want.level].spacing_m) * de::render::kTerrainStreamMargin);
        }
    }
    CHECK(root_count == roots);
    CHECK(finest);
    const size_t near_ground = wanted.size();

    // Far off the map: only the roots, whatever else there is.
    de::render::terrain_wanted_nodes(terrain, file, reach, DVec3(50000.0, 10.0, 50000.0), wanted);
    CHECK(wanted.size() == roots);
    CHECK(near_ground > roots);
}

TEST_CASE("terrain streams in around the camera, draws the ground with no holes, and leaves the sky") {
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    Rig rig;
    rig.put(cook_hills(2048.0f, 4.0));
    auto compiler = de::shader::ShaderCompiler::create(*rig.vfs);
    REQUIRE(compiler.ok());
    auto shaders = de::shader::ShaderLibrary::create(*device, *rig.vfs, rig.jobs, *compiler, {.hot_reload = false});
    REQUIRE(shaders.ok());
    auto uploader = de::rhi::Uploader::create(*device);
    REQUIRE(uploader.ok());
    auto assets = de::asset::AssetRegistry::create({.vfs = &*rig.vfs, .jobs = &rig.jobs, .hot_reload = false});
    REQUIRE(assets.ok());
    {
        auto gpu_assets =
            de::render::GpuAssets::create({.device = &*device, .uploader = &*uploader, .assets = &*assets});
        REQUIRE(gpu_assets.ok());
        auto renderer = de::render::SceneRenderer::create({.device = &*device,
                                                           .shaders = &*shaders,
                                                           .gpu_assets = &*gpu_assets,
                                                           .uploader = &*uploader,
                                                           .vfs = &*rig.vfs});
        REQUIRE(renderer.ok());
        const de::asset::AssetHandle handle = assets->load<de::asset::TerrainAsset>(kTerrain);
        assets->wait_all();
        const auto* terrain = assets->get<de::asset::TerrainAsset>(handle);
        REQUIRE(terrain != nullptr);
        renderer->set_terrain(terrain, assets->file(handle));

        const auto target =
            device->create_texture({.width = kSize,
                                    .height = kSize,
                                    .format = de::rhi::Format::RGBA8Unorm,
                                    .usage = de::rhi::TextureUsage::RenderTarget | de::rhi::TextureUsage::CopySrc,
                                    .debug_name = "terrain target"});
        REQUIRE(target.ok());
        auto graph = de::render::RenderGraph::create(*device);
        REQUIRE(graph.ok());

        // Above the map's corner, looking along its diagonal, level: the horizon across the middle of the picture, and
        // every level of the map in view. The render origin is off the corner, as it would be far from the world's
        // middle.
        de::world::RenderFrame frame;
        frame.origin = DVec3(1024.0, 0.0, 1024.0);
        const float root = std::sqrt(0.5f);
        // Looking down -Z, turned to face +X +Z (south-east, across the map).
        frame.camera = {
            .transform = {-root, 0, -root, 16.0f - 1024.0f, 0, 1, 0, 90.0f, root, 0, -root, 16.0f - 1024.0f},
            .vertical_fov_rad = 1.0f,
            .valid = true};
        const de::render::SceneLighting light;
        const auto draw = [&] {
            device->begin_frame();
            uploader->record();
            graph->begin(kSize, kSize);
            const de::render::GraphTexture color = graph->import_texture(
                {.texture = *target, .name = "target", .final_state = de::rhi::ResourceState::CopySrc});
            renderer->add_passes(*graph, color, de::rhi::Format::RGBA8Unorm, frame, light,
                                 de::render::SceneView::BaseColor);
            graph->execute();
            device->end_frame();
        };

        // There's nothing to draw until the heights are in.
        de::render::TerrainStats stats{};
        for (int i = 0; i < 2000 && (stats.wanted == 0 || stats.resident < stats.wanted); ++i) {
            draw();
            stats = renderer->stats().terrain;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1)); // reads finish on other threads, in their own time
        }
        CHECK(stats.wanted > 1);
        CHECK(stats.resident == stats.wanted);
        CHECK(stats.failed == 0);
        CHECK(stats.blocks == terrain->block_count);
        // What was drawn comes back a frame or two late.
        for (int i = 0; i < 4; ++i) {
            draw();
        }
        stats = renderer->stats().terrain;
        CHECK(stats.patches > 0);
        CHECK(stats.triangles > 0);

        const de::core::Array<std::byte> pixels = de::test::read_texture(*device, *target);
        // Below the horizon it's all ground: a gap between levels would show the sky's nothing.
        for (uint32_t y = kSize / 2 + 8; y < kSize; ++y) {
            for (uint32_t x = 0; x < kSize; ++x) {
                const uint8_t* pixel = texel(pixels, x, y);
                CHECK(static_cast<int>(pixel[0]) + pixel[1] + pixel[2] > 0);
            }
        }
        // Above it there's nothing.
        const uint8_t* sky = texel(pixels, kSize / 2, 2);
        CHECK(static_cast<int>(sky[0]) + sky[1] + sky[2] == 0);

        // Nothing is left drawing from a terrain that's gone.
        renderer->set_terrain(nullptr, {});
        for (int i = 0; i < 4; ++i) {
            draw();
        }
        const de::core::Array<std::byte> empty = de::test::read_texture(*device, *target);
        const uint8_t* gone = texel(empty, kSize / 2, kSize - 4);
        CHECK(static_cast<int>(gone[0]) + gone[1] + gone[2] == 0);

        assets->release(handle);
        assets->update();
        device->wait_idle();
        device->destroy(*target);
    }
}

TEST_CASE("terrain keeps to its blocks as the camera moves, and refuses heights it wasn't cooked with") {
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    Rig rig;
    de::cooker::CookedTerrain cooked = cook_hills(4096.0f, 4.0);
    rig.put(cooked);
    auto uploader = de::rhi::Uploader::create(*device);
    REQUIRE(uploader.ok());
    const std::span<const std::byte> file = cooked.terrain.span();
    const auto loaded = de::asset::load_in_place<de::asset::TerrainAsset>(file);
    REQUIRE(loaded.ok());
    const de::asset::TerrainAsset& asset = **loaded;

    // Room for the roots and a few more: fewer than a camera wants.
    constexpr uint32_t kBlocks = 8;
    auto terrain = de::render::Terrain::create(
        {.device = &*device, .uploader = &*uploader, .vfs = &*rig.vfs, .max_blocks = kBlocks});
    REQUIRE(terrain.ok());
    terrain->set(&asset, file);
    const auto settle = [&](const DVec3& camera) {
        // Past what it takes to let go and take up slots, until nothing's on its way.
        for (int i = 0; i < 2000; ++i) {
            device->begin_frame();
            uploader->record();
            terrain->update(camera);
            device->end_frame();
            const de::render::TerrainStats now = terrain->stats();
            REQUIRE(now.resident <= kBlocks);
            if (i >= 24 && now.reading == 0 && now.uploading == 0) {
                break;
            }
            // Reads finish on other threads, in their own time.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    settle(DVec3(200.0, 30.0, 200.0));
    const de::render::TerrainStats first = terrain->stats();
    CHECK(first.resident == kBlocks); // a slot lost along the way would leave this short
    CHECK(first.over_capacity > 0);
    CHECK(first.failed == 0);
    // Across the map: what isn't wanted any more makes room for what is.
    settle(DVec3(3900.0, 30.0, 3900.0));
    const de::render::TerrainStats second = terrain->stats();
    CHECK(second.blocks_loaded > first.blocks_loaded);
    CHECK(second.resident == kBlocks);
    CHECK(second.failed == 0);

    // Heights from another cook are refused whole: nothing is read from them.
    de::asset::AssetHeader other{};
    std::memcpy(&other, cooked.heights.data(), sizeof(other));
    other.content_hash ^= 1U;
    std::memcpy(cooked.heights.data(), &other, sizeof(other));
    rig.put(cooked);
    terrain->set(&asset, file);
    for (int i = 0; i < 12; ++i) {
        device->begin_frame();
        uploader->record();
        terrain->update(DVec3(200.0, 30.0, 200.0));
        device->end_frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(terrain->stats().resident == 0);
    device->wait_idle();
}

TEST_CASE("height fog thins with a low camera, and comes back as it rises") {
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    Rig rig;
    REQUIRE(rig.vfs.ok());
    auto compiler = de::shader::ShaderCompiler::create(*rig.vfs);
    REQUIRE(compiler.ok());
    auto shaders = de::shader::ShaderLibrary::create(*device, *rig.vfs, rig.jobs, *compiler, {.hot_reload = false});
    REQUIRE(shaders.ok());
    auto uploader = de::rhi::Uploader::create(*device);
    REQUIRE(uploader.ok());
    auto assets = de::asset::AssetRegistry::create({.vfs = &*rig.vfs, .jobs = &rig.jobs, .hot_reload = false});
    REQUIRE(assets.ok());
    {
        auto gpu_assets =
            de::render::GpuAssets::create({.device = &*device, .uploader = &*uploader, .assets = &*assets});
        REQUIRE(gpu_assets.ok());
        auto renderer =
            de::render::SceneRenderer::create({.device = &*device, .shaders = &*shaders, .gpu_assets = &*gpu_assets});
        REQUIRE(renderer.ok());
        const auto target =
            device->create_texture({.width = kSize,
                                    .height = kSize,
                                    .format = de::rhi::Format::RGBA8Unorm,
                                    .usage = de::rhi::TextureUsage::RenderTarget | de::rhi::TextureUsage::CopySrc,
                                    .debug_name = "fog target"});
        REQUIRE(target.ok());
        auto graph = de::render::RenderGraph::create(*device);
        REQUIRE(graph.ok());

        // Only the sky, seen through fog that's nearly the same at every height, so what differs is how much fog there
        // is.
        de::render::SceneLighting light;
        light.fog.height_density_per_m = 0.00005f; // about 60% of the sky at 20 km, so more or less of it shows
        light.fog.height_falloff_per_m = 1e-4f;
        // Dim enough that the tonemap doesn't flatten the difference between more and less of it.
        light.fog.ambient[0] = light.fog.ambient[1] = light.fog.ambient[2] = 0.12f;
        light.fog.ambient_height_attenuation = 0.0f; // not dimmed by what's above it: all this fog is bright
        light.fog.low_camera_keep = 0.0f;
        light.fog.low_camera_below_m = 100.0f;
        light.fog.low_camera_above_m = 300.0f;
        const auto red_at = [&](double camera_y) {
            de::world::RenderFrame frame;
            frame.origin = DVec3(0.0, camera_y, 0.0);
            frame.camera = {.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}, .vertical_fov_rad = 1.0f, .valid = true};
            device->begin_frame();
            uploader->record();
            graph->begin(kSize, kSize);
            const de::render::GraphTexture color = graph->import_texture(
                {.texture = *target, .name = "target", .final_state = de::rhi::ResourceState::CopySrc});
            renderer->add_passes(*graph, color, de::rhi::Format::RGBA8Unorm, frame, light);
            graph->execute();
            device->end_frame();
            const de::core::Array<std::byte> pixels = de::test::read_texture(*device, *target);
            return static_cast<int>(texel(pixels, kSize / 2, kSize / 2)[0]);
        };
        const int low_thinned = red_at(0.0);
        const int middle = red_at(200.0);
        const int high = red_at(1000.0);
        CHECK(low_thinned < middle);
        CHECK(middle < high);
        // Off, to compare: the same fog wherever the camera is.
        renderer->set_fog_low_camera(false);
        const int low_full = red_at(0.0);
        CHECK(low_full > low_thinned + 10);
        // High up, it makes no difference.
        CHECK(red_at(1000.0) == high);
        device->wait_idle();
        device->destroy(*target);
    }
}
