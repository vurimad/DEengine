#include "app/preview_scene.h"
#include "asset/effect_format.h"
#include "asset/mesh_format.h"
#include "asset/registry.h"
#include "asset/texture_format.h"
#include "cooker/effect_cooker.h"
#include "core/fixed_string.h"
#include "core/job_system.h"
#include "editor/preview_pictures.h"
#include "editor/source_files.h"
#include "editor/thumbnails.h"
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
#include "support/render_transform.h"
#include "support/sandbox.h"
#include "tools/debug_ui.h"
#include "world/render_extract.h"
#include "world/transform.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

using de::Vec3;
using de::Vec4;
using de::asset::AssetId;
using de::asset::AssetType;
using de::editor::PictureBox;
using de::editor::PictureFraming;
using de::editor::PictureSubject;
using de::test::Sandbox;

namespace {

de::core::Allocator& heap() {
    return de::core::heap_allocator(de::core::MemoryTag::Editor);
}

std::vector<std::byte> picture(uint32_t width, uint32_t height) {
    std::vector<std::byte> texels(size_t{width} * height * 4, std::byte{0});
    for (size_t i = 3; i < texels.size(); i += 4) {
        texels[i] = std::byte{255};
    }
    return texels;
}

void paint(std::vector<std::byte>& texels, uint32_t width, uint32_t x, uint32_t y, uint8_t value) {
    for (int channel = 0; channel < 3; ++channel) {
        texels[(((size_t{y} * width) + x) * 4) + channel] = std::byte{value};
    }
}

// Where a point lands in a picture taken with a framing: -1 to 1 each way.
de::Vec2 seen(const PictureFraming& framing, Vec3 point) {
    const de::editor::PictureAxes axes = de::editor::picture_axes();
    const Vec3 from = point - framing.target;
    const float depth = framing.distance_m - glm::dot(from, axes.toward);
    const float tan_half = std::tan(framing.vertical_fov_rad * 0.5f);
    return {glm::dot(from, axes.right) / (depth * tan_half), glm::dot(from, axes.up) / (depth * tan_half)};
}

constexpr std::string_view kGlow = R"({
  "emitters": [
    {"name": "glow", "rate_per_s": 6, "lifetime_s": 1, "prewarm_s": 0,
     "modules": [{"type": "size", "m": 0.5}, {"type": "color", "rgb": [1, 0.1, 0.05]}],
     "look": {"shape": "glow", "intensity": 0.25, "soft_m": 0}}
  ]
})";

} // namespace

TEST_CASE("pictures: brought down as light, and measured by what's brighter than black") {
    // Left half white, right half black, and a box that's half of each.
    std::vector<std::byte> wide = picture(8, 8);
    for (uint32_t y = 0; y < 8; ++y) {
        for (uint32_t x = 0; x < 4; ++x) {
            paint(wide, 8, x, y, 255);
        }
    }
    std::vector<std::byte> small(size_t{2} * 2 * 4);
    de::editor::shrink_picture(wide, 2, 4, small);
    CHECK(small[0] == std::byte{255});
    CHECK(small[4] == std::byte{0});
    CHECK(small[3] == std::byte{255}); // opaque
    std::vector<std::byte> one(4);
    de::editor::shrink_picture(wide, 1, 8, one);
    // Half the light is 188 of 255 to a screen, not 128.
    CHECK(static_cast<int>(one[0]) == 188);

    std::vector<std::byte> dark = picture(16, 16);
    PictureBox box;
    CHECK_FALSE(de::editor::measure_picture(dark, 16, 16, box));
    paint(dark, 16, 9, 9, 4); // too dim to be anything
    CHECK_FALSE(de::editor::measure_picture(dark, 16, 16, box));
    for (uint32_t x = 4; x < 8; ++x) {
        paint(dark, 16, x, 2, 200);
        paint(dark, 16, x, 3, 40);
    }
    REQUIRE(de::editor::measure_picture(dark, 16, 16, box));
    CHECK(box.min_x == doctest::Approx(-0.5f));
    CHECK(box.max_x == doctest::Approx(0.0f));
    CHECK(box.max_y == doctest::Approx(0.75f)); // rows from the top: the third from it
    CHECK(box.min_y == doctest::Approx(0.5f));
}

TEST_CASE("pictures: a framing holds its subject, and what a picture showed brings it in") {
    constexpr float kFill = 0.8f;
    {
        // A box a metre a side standing on the ground: every corner inside the fill, one of them on its edge.
        PictureSubject subject(heap());
        for (uint32_t corner = 0; corner < 8; ++corner) {
            subject.points.push(Vec3((corner & 1U) != 0 ? 0.5f : -0.5f, (corner & 2U) != 0 ? 1.0f : 0.0f,
                                     (corner & 4U) != 0 ? 0.5f : -0.5f));
        }
        const PictureFraming framing = de::editor::frame_subject(subject, kFill);
        float widest = 0.0f;
        for (const Vec3& point : subject.points) {
            const de::Vec2 at = seen(framing, point);
            widest = std::max({widest, std::abs(at.x), std::abs(at.y)});
        }
        CHECK(widest <= kFill + 1e-3f);
        CHECK(widest > kFill - 0.02f);
        // Seen through a lens, the box of its corners isn't quite in the middle: brought there by what the picture
        // shows, in a step or two, and then left alone. Its box fills the picture's fill exactly.
        PictureFraming fitted = framing;
        bool settled = false;
        for (int step = 0; step < 3 && !settled; ++step) {
            fitted = de::editor::refit_framing(fitted, subject, nullptr, kFill, settled);
        }
        CHECK(settled);
        CHECK(fitted.distance_m == doctest::Approx(framing.distance_m).epsilon(0.15));
        de::Vec2 low(1.0f);
        de::Vec2 high(-1.0f);
        for (const Vec3& point : subject.points) {
            low = glm::min(low, seen(fitted, point));
            high = glm::max(high, seen(fitted, point));
        }
        CHECK(std::max(high.x - low.x, high.y - low.y) * 0.5f == doctest::Approx(kFill).epsilon(0.07));
        CHECK(std::abs(low.x + high.x) < 0.1f);
        CHECK(std::abs(low.y + high.y) < 0.1f);
    }
    {
        // An effect's sphere, as far as its particles may get: all of it, the near side too.
        PictureSubject subject(heap());
        subject.spheres.push(Vec4(0.0f, 0.0f, 0.0f, 1.0f));
        const PictureFraming framing = de::editor::frame_subject(subject, kFill);
        CHECK(framing.distance_m ==
              doctest::Approx((1.0f / (std::tan(framing.vertical_fov_rad * 0.5f) * kFill)) + 1.0f));
        // Its particles turned out to fill a tenth of that, off to the right: looked at there, from a tenth as far.
        const PictureBox measured{.min_x = 0.1f, .max_x = 0.26f, .min_y = -0.08f, .max_y = 0.08f};
        bool settled = true;
        const PictureFraming nearer = de::editor::refit_framing(framing, subject, &measured, kFill, settled);
        CHECK_FALSE(settled);
        // A tenth the size across the picture's middle. No nearer than particles show whole from, though (0.45 m
        // from the sphere they may be anywhere in): from there, through a longer lens.
        const auto across = [](const PictureFraming& of) {
            return of.distance_m * std::tan(of.vertical_fov_rad * 0.5f);
        };
        CHECK(across(nearer) == doctest::Approx(across(framing) * 0.1f));
        CHECK(nearer.distance_m == doctest::Approx(1.45f));
        CHECK(nearer.vertical_fov_rad < framing.vertical_fov_rad);
        const de::editor::PictureAxes axes = de::editor::picture_axes();
        CHECK(glm::dot(nearer.target - framing.target, axes.right) ==
              doctest::Approx(0.18f * framing.distance_m * std::tan(framing.vertical_fov_rad * 0.5f)));
        // And a picture that shows it filling what it should is where it stops.
        const PictureBox filled{.min_x = -0.8f, .max_x = 0.8f, .min_y = -0.5f, .max_y = 0.5f};
        const PictureFraming kept = de::editor::refit_framing(nearer, subject, &filled, kFill, settled);
        CHECK(settled);
        CHECK(kept.distance_m == doctest::Approx(nearer.distance_m));
        CHECK(kept.vertical_fov_rad == doctest::Approx(nearer.vertical_fov_rad));
    }
    {
        // A big effect that isn't drawn from far: a wider lens, not a place past its draw distance.
        PictureSubject subject(heap());
        subject.spheres.push(Vec4(0.0f, 0.0f, 0.0f, 10.0f));
        subject.nearest_draw_distance_m = 50.0f;
        const PictureFraming framing = de::editor::frame_subject(subject, kFill);
        CHECK(framing.vertical_fov_rad > 0.5f);
        CHECK(framing.distance_m < 40.0f);
    }
    {
        // Nothing at all: a framing all the same.
        const PictureSubject subject(heap());
        const PictureFraming framing = de::editor::frame_subject(subject, kFill);
        CHECK(framing.distance_m > 0.0f);
    }
}

TEST_CASE("thumbnails: a drawn picture is asked for, kept in a file, and asked for again when what it shows changed") {
    using de::editor::Thumbnails;
    const Sandbox sandbox;
    de::test::write_src(sandbox, "content/fx/glow.vfx", kGlow);
    de::platform::Vfs vfs = de::test::cooker_vfs(sandbox);
    REQUIRE(de::test::run_cook(vfs).ok());
    de::asset::Guid guid;
    AssetId id;
    REQUIRE(de::editor::source_asset_id(vfs, "src:/content/fx/glow.vfx", guid, id));
    const de::asset::CookedPath cooked = de::asset::cooked_path(id, AssetType::Effect);
    const auto source_now = [&] {
        const auto info = vfs.file_info(cooked.c_str());
        REQUIRE(info.ok());
        return de::editor::PictureSource{
            .asset = {.id = id, .type = AssetType::Effect}, .size = info->size, .modified_time = info->modified_time};
    };
    std::vector<std::byte> texels = picture(de::asset::kThumbnailSize, de::asset::kThumbnailSize);
    de::tools::create_imgui();
    {
        Thumbnails thumbnails(heap(), 1, 2);
        Thumbnails::Picture shown;
        const auto show = [&] { return thumbnails.find("src:/content/fx/glow.vfx", shown); };
        // No file of it yet: the renderer is asked, once.
        CHECK_FALSE(show());
        thumbnails.update(vfs);
        CHECK_FALSE(show());
        thumbnails.update(vfs);
        Thumbnails::RenderRequest request;
        REQUIRE(thumbnails.take_render_request(request));
        CHECK(request.asset.id == id);
        CHECK(request.asset.type == AssetType::Effect);
        CHECK_FALSE(thumbnails.take_render_request(request));
        // Drawn and written: shown from its file.
        const de::editor::PictureSource source = source_now();
        REQUIRE(Thumbnails::write_picture(vfs, id, texels, std::span(&source, 1), true, heap()).ok());
        thumbnails.rendered(request, true);
        thumbnails.update(vfs);
        CHECK(show());
        // Forgotten (a cook, a refresh): its file still holds, so nothing is drawn again.
        thumbnails.forget_all();
        CHECK_FALSE(thumbnails.wants(request));
        CHECK_FALSE(show());
        thumbnails.update(vfs);
        CHECK(show());
        CHECK(thumbnails.renders_waiting() == 0);
        // The effect cooked again from a changed source: its picture is of what it was.
        de::test::write_src(sandbox, "content/fx/glow.vfx",
                            R"({"emitters": [{"name": "glow", "rate_per_s": 12, "lifetime_s": 2}]})");
        REQUIRE(de::test::run_cook(vfs).ok());
        thumbnails.forget_all();
        CHECK_FALSE(show());
        thumbnails.update(vfs);
        CHECK_FALSE(show());
        REQUIRE(thumbnails.take_render_request(request));
        // One that couldn't be drawn shows nothing, and isn't asked for again until it's forgotten.
        thumbnails.rendered(request, false);
        thumbnails.update(vfs);
        CHECK_FALSE(show());
        thumbnails.update(vfs);
        CHECK(thumbnails.renders_waiting() == 0);
        // A picture shot before everything was there is shown for now, and drawn again next time.
        thumbnails.forget_all();
        CHECK_FALSE(show());
        thumbnails.update(vfs);
        REQUIRE(thumbnails.take_render_request(request));
        const de::editor::PictureSource changed = source_now();
        REQUIRE(Thumbnails::write_picture(vfs, id, texels, std::span(&changed, 1), false, heap()).ok());
        thumbnails.rendered(request, true);
        thumbnails.update(vfs);
        CHECK(show());
        thumbnails.forget_all();
        CHECK_FALSE(show());
        thumbnails.update(vfs);
        CHECK(thumbnails.renders_waiting() == 1);
        // A request from before it was forgotten answers nothing.
        const Thumbnails::RenderRequest stale = request;
        thumbnails.rendered(stale, true);
        thumbnails.update(vfs);
        CHECK_FALSE(show());
    }
    de::tools::destroy_imgui();
}

TEST_CASE("a preview scene draws what's put in it by itself, the same picture for the same age") {
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    const Sandbox sandbox;
    de::core::JobSystem jobs({.worker_count = 2});
    const de::platform::PathString game_root = sandbox.game();
    auto vfs = de::platform::Vfs::create(
        {.game_root = game_root.c_str(), .engine_root = DE_SOURCE_DIR, .jobs = &jobs, .game_writable = true});
    REQUIRE(vfs.ok());
    const AssetId glow_id = AssetId::from_value(0x560);
    {
        de::cooker::Source source;
        REQUIRE(source.path.assign("content/glow.vfx"));
        de::cooker::Diagnostics diagnostics;
        const auto effect = de::cooker::import_effect(source, std::as_bytes(std::span(kGlow)), diagnostics);
        REQUIRE(effect.ok());
        const auto cooked = de::cooker::cook_effect(*effect, glow_id);
        REQUIRE(vfs->write_file(de::asset::cooked_path(glow_id, AssetType::Effect).c_str(), cooked.span()).ok());
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
        const de::asset::AssetHandle glow = assets->load<de::asset::EffectAsset>(glow_id);
        assets->wait_all();
        REQUIRE(assets->state(glow) == de::asset::AssetState::Ready);
        // The file its contents came from: what a picture of it is kept by.
        const auto info = vfs->file_info(de::asset::cooked_path(glow_id, AssetType::Effect).c_str());
        REQUIRE(info.ok());
        CHECK(assets->file_info(glow).size == info->size);
        CHECK(assets->file_info(glow).modified_time == info->modified_time);

        constexpr uint32_t kSide = 64;
        auto scene = de::app::PreviewScene::create({.device = &*device,
                                                    .shaders = &*shaders,
                                                    .assets = &*assets,
                                                    .gpu_assets = &*gpu_assets,
                                                    .jobs = &jobs,
                                                    .max_instances = 64,
                                                    .particle_pool_blocks = 16});
        REQUIRE(scene.ok());
        CHECK(scene->texture_id() == 0);
        scene->set_size(kSide, kSide);
        CHECK(scene->texture_id() != 0);
        scene->set_camera({.position = {0.0, 0.0, 3.0}});
        // The glow at the origin, held at a second old: six particles, where they were born.
        const flecs::entity entity = scene->world().ecs().entity();
        de::world::EffectClock clock;
        clock.seek(scene->time_s(), 1.0);
        clock.rate = 0.0f;
        entity.set(de::world::EffectInstance{.effect = glow, .clock = clock});
        de::world::place(entity, de::world::Transform{});
        // Nothing but it: no sun, a little light, and a backdrop red at the top and blue at the bottom.
        de::render::SceneLighting lighting;
        for (int c = 0; c < 3; ++c) {
            lighting.sun_color[c] = 0.0f;
            lighting.sky_color[c] = 0.05f;
            lighting.ground_color[c] = 0.05f;
        }
        lighting.backdrop = {.enabled = true, .top = {0.6f, 0.0f, 0.0f}, .bottom = {0.0f, 0.0f, 0.6f}};

        struct Shot {
            std::vector<std::byte> texels;
            uint64_t tag = 0;
        };
        std::vector<Shot> shots;
        de::core::Array<std::byte> texels(heap());
        const auto frames = [&](uint32_t count, bool keep) {
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t width = 0;
                uint32_t height = 0;
                uint64_t tag = 0;
                while (scene->take_picture(texels, width, height, tag)) {
                    CHECK(width == kSide);
                    CHECK(height == kSide);
                    shots.push_back({.texels = std::vector<std::byte>(texels.begin(), texels.end()), .tag = tag});
                }
                scene->advance(1.0 / 60.0);
                device->begin_frame();
                scene->draw(lighting, false, keep ? uint64_t{shots.size()} + scene->pictures_pending() + 1 : 0);
                device->end_frame();
            }
        };
        const auto texel = [&](const Shot& shot, uint32_t x, uint32_t y, int channel) {
            return static_cast<int>(shot.texels[(((size_t{y} * kSide) + x) * 4) + static_cast<size_t>(channel)]);
        };
        frames(4, false); // caught up to its age
        CHECK(scene->stats().particles.emitters_drawn == 1);
        frames(6, true);
        frames(3, false); // the last of them come back
        REQUIRE(shots.size() == 6);
        for (size_t i = 0; i < shots.size(); ++i) {
            CHECK(shots[i].tag == i + 1); // in the order they were asked for
        }
        // The glow in the middle, redder than the backdrop beside it; the backdrop red at the top and blue at the
        // bottom.
        const Shot first = shots[0];
        constexpr uint32_t kMiddle = kSide / 2;
        CHECK(texel(first, kMiddle, kMiddle, 0) > texel(first, 1, kMiddle, 0) + 20);
        CHECK(texel(first, 1, 0, 0) > texel(first, 1, 0, 2) + 80);
        CHECK(texel(first, 1, kSide - 1, 2) > texel(first, 1, kSide - 1, 0) + 80);
        // Held at its age, every frame's picture is the same, to the bit.
        CHECK(shots[5].texels == first.texels);
        // Half as old it has half the particles: another picture, dimmer in the middle.
        entity.get_mut<de::world::EffectInstance>().clock.seek(scene->time_s(), 0.5);
        shots.clear();
        frames(3, false);
        frames(1, true);
        frames(3, false);
        REQUIRE(shots.size() == 1);
        CHECK(shots[0].texels != first.texels);
        CHECK(texel(shots[0], kMiddle, kMiddle, 0) < texel(first, kMiddle, kMiddle, 0));
        // And back at a second old: the picture it was.
        entity.get_mut<de::world::EffectInstance>().clock.seek(scene->time_s(), 1.0);
        shots.clear();
        frames(3, false);
        frames(1, true);
        frames(3, false);
        REQUIRE(shots.size() == 1);
        CHECK(shots[0].texels == first.texels);
        // What left the scene is gone from its picture.
        entity.destruct();
        shots.clear();
        frames(2, false);
        frames(1, true);
        frames(3, false);
        REQUIRE(shots.size() == 1);
        CHECK(std::abs(texel(shots[0], kMiddle, kMiddle, 0) - texel(shots[0], 1, kMiddle, 0)) <= 2);
        CHECK(scene->stats().particles.effects == 0);
        assets->release(glow);
        assets->update();
        device->wait_idle();
    }
    device->wait_idle();
    de::test::check_clean(*device);
}

TEST_CASE("a preview scene draws a mesh put in it, lit by the light it's given") {
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    const Sandbox sandbox;
    // A square facing +Z, cooked as any model is.
    std::vector<de::test::Vertex> vertices;
    std::vector<uint32_t> indices;
    de::test::quad(vertices, indices);
    de::test::write_src(sandbox, "content/props/square.gltf",
                        de::test::gltf(vertices, indices, 0.0f, R"([{"name": "paint"}])", "[]", "[]"));
    // And a model the game draws, there before it: the previewed mesh isn't the first in the GPU's tables.
    std::vector<de::test::Vertex> wavy;
    std::vector<uint32_t> wavy_indices;
    de::test::grid(8, wavy, wavy_indices);
    de::test::write_src(sandbox, "content/props/ground.gltf",
                        de::test::gltf(wavy, wavy_indices, 0.0f, R"([{"name": "paint"}])", "[]", "[]"));
    AssetId mesh_id;
    AssetId ground_id;
    {
        de::platform::Vfs cook_vfs = de::test::cooker_vfs(sandbox);
        REQUIRE(de::test::run_cook(cook_vfs).ok());
        de::asset::Guid guid;
        REQUIRE(de::editor::source_asset_id(cook_vfs, "src:/content/props/square.gltf", guid, mesh_id));
        REQUIRE(de::editor::source_asset_id(cook_vfs, "src:/content/props/ground.gltf", guid, ground_id));
    }
    de::core::JobSystem jobs({.worker_count = 2});
    const de::platform::PathString game_root = sandbox.game();
    auto vfs = de::platform::Vfs::create(
        {.game_root = game_root.c_str(), .engine_root = DE_SOURCE_DIR, .jobs = &jobs, .game_writable = true});
    REQUIRE(vfs.ok());
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
        const de::asset::AssetHandle ground = assets->load<de::asset::MeshAsset>(ground_id);
        const de::asset::AssetHandle mesh = assets->load<de::asset::MeshAsset>(mesh_id);
        assets->wait_all();
        REQUIRE(assets->state(mesh) == de::asset::AssetState::Ready);
        REQUIRE(assets->state(ground) == de::asset::AssetState::Ready);
        gpu_assets->update();
        uploader->flush_and_wait();
        gpu_assets->update();
        REQUIRE(gpu_assets->mesh(mesh) != nullptr);
        REQUIRE(gpu_assets->mesh(ground) != nullptr);

        constexpr uint32_t kSide = 64;
        auto scene = de::app::PreviewScene::create({.device = &*device,
                                                    .shaders = &*shaders,
                                                    .assets = &*assets,
                                                    .gpu_assets = &*gpu_assets,
                                                    .jobs = &jobs,
                                                    .max_instances = 64,
                                                    .particle_pool_blocks = 16});
        REQUIRE(scene.ok());
        scene->set_size(kSide, kSide);
        scene->set_camera({.position = {0.0, 0.0, 6.0}});
        // The game's own renderer and picture, drawn after the preview's in every frame, as in the editor: what the
        // one draws mustn't change what the other does.
        auto game = de::render::SceneRenderer::create(
            {.device = &*device, .shaders = &*shaders, .gpu_assets = &*gpu_assets, .assets = &*assets});
        REQUIRE(game.ok());
        auto game_graph = de::render::RenderGraph::create(*device);
        REQUIRE(game_graph.ok());
        const auto game_target =
            device->create_texture({.width = 96,
                                    .height = 96,
                                    .format = de::rhi::Format::RGBA8Unorm,
                                    .usage = de::rhi::TextureUsage::RenderTarget | de::rhi::TextureUsage::CopySrc,
                                    .debug_name = "game"});
        REQUIRE(game_target.ok());
        bool game_has_mesh = false;
        uint32_t game_frames = 0;
        // The scene has been drawn empty for a while before anything is put in it, as the editor's is.
        de::render::SceneLighting lighting;
        lighting.sun_direction[0] = 0.0f;
        lighting.sun_direction[1] = 0.0f;
        lighting.sun_direction[2] = 1.0f;
        lighting.backdrop = {.enabled = true, .top = {0.0f, 0.0f, 0.0f}, .bottom = {0.0f, 0.0f, 0.0f}};
        de::core::Array<std::byte> texels(heap());
        std::vector<std::byte> last;
        const auto frames = [&](uint32_t count) {
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t width = 0;
                uint32_t height = 0;
                uint64_t tag = 0;
                while (scene->take_picture(texels, width, height, tag)) {
                    last.assign(texels.begin(), texels.end());
                }
                scene->advance(1.0 / 60.0);
                gpu_assets->update();
                device->begin_frame();
                uploader->record();
                scene->draw(lighting, false, 1);
                // The game's frame: looking the other way at many of the same mesh, or at nothing.
                de::world::RenderFrame frame;
                frame.number = 0;
                frame.time_s = static_cast<double>(game_frames++) / 60.0;
                frame.camera = {
                    .transform = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, -40}, .vertical_fov_rad = 1.0f, .valid = true};
                frame.all_instances = true;
                frame.instances.push(de::world::RenderInstance{
                    .transform = de::test::render_transform({1, 0, 0, 0, 0, 1, 0, -3, 0, 0, 1, 0}), .mesh = ground});
                if (game_has_mesh) {
                    for (int copy = 0; copy < 12; ++copy) {
                        frame.instances.push(de::world::RenderInstance{
                            .transform = de::test::render_transform(
                                {1, 0, 0, static_cast<float>(copy) * 2.0f, 0, 1, 0, 0, 0, 0, 1, 0}),
                            .mesh = mesh});
                    }
                }
                game_graph->begin(96, 96);
                const de::render::GraphTexture color = game_graph->import_texture(
                    {.texture = *game_target, .name = "game", .final_state = de::rhi::ResourceState::CopySrc});
                game->add_passes(*game_graph, color, de::rhi::Format::RGBA8Unorm, frame, de::render::SceneLighting{},
                                 de::render::SceneView::Lit);
                game_graph->execute();
                device->end_frame();
            }
        };
        const auto red_at = [&](uint32_t x, uint32_t y) {
            return static_cast<int>(last[((size_t{y} * kSide) + x) * 4]);
        };
        frames(5);
        REQUIRE(last.size() == size_t{kSide} * kSide * 4);
        CHECK(red_at(kSide / 2, kSide / 2) < 4); // nothing there yet
        const flecs::entity entity = scene->world().ecs().entity();
        entity.add<de::world::StaticTransform>();
        entity.set(de::world::MeshInstance{.mesh = mesh});
        de::world::place(entity, de::world::Transform{});
        frames(6);
        CHECK(scene->stats().instances == 1);
        CHECK(scene->stats().drawn == 1);
        CHECK(scene->stats().draws == 1);
        CHECK(scene->stats().visible_draws == 1);
        CHECK(scene->stats().visible_triangles == 2);
        CHECK(scene->stats().indirect_calls >= 1);
        // The square in the middle, lit; black round it.
        CHECK(red_at(kSide / 2, kSide / 2) > 100);
        CHECK(red_at(1, 1) < 4);
        // The same with the game drawing a dozen of that mesh in its own scene.
        game_has_mesh = true;
        frames(6);
        CHECK(game->stats().instances == 13);
        CHECK(scene->stats().instances == 1);
        CHECK(scene->stats().visible_draws == 1);
        CHECK(red_at(kSide / 2, kSide / 2) > 100);
        CHECK(red_at(1, 1) < 4);
        // Gone when it's taken out.
        entity.destruct();
        frames(6);
        CHECK(red_at(kSide / 2, kSide / 2) < 4);
        CHECK(scene->stats().instances == 0);
        {
            // Another scene, with the mesh in it before its first frame: there from the first picture it takes.
            auto early = de::app::PreviewScene::create({.device = &*device,
                                                        .shaders = &*shaders,
                                                        .assets = &*assets,
                                                        .gpu_assets = &*gpu_assets,
                                                        .jobs = &jobs,
                                                        .max_instances = 64,
                                                        .particle_pool_blocks = 16});
            REQUIRE(early.ok());
            early->set_size(kSide, kSide);
            early->set_camera({.position = {0.0, 0.0, 6.0}});
            const flecs::entity there = early->world().ecs().entity();
            there.add<de::world::StaticTransform>();
            there.set(de::world::MeshInstance{.mesh = mesh});
            de::world::place(there, de::world::Transform{});
            std::vector<std::byte> first_picture;
            for (uint32_t i = 0; i < 6; ++i) {
                uint32_t width = 0;
                uint32_t height = 0;
                uint64_t tag = 0;
                while (early->take_picture(texels, width, height, tag)) {
                    if (tag == 1) {
                        first_picture.assign(texels.begin(), texels.end());
                    }
                }
                early->advance(1.0 / 60.0);
                gpu_assets->update();
                device->begin_frame();
                uploader->record();
                early->draw(lighting, false, i + 1);
                device->end_frame();
            }
            CHECK(early->stats().instances == 1);
            CHECK(early->stats().visible_draws == 1);
            REQUIRE(first_picture.size() == size_t{kSide} * kSide * 4);
            CHECK(static_cast<int>(first_picture[((size_t{kSide / 2} * kSide) + (kSide / 2)) * 4]) > 100);
            device->wait_idle();
        }
        device->wait_idle();
        device->destroy(*game_target);
        assets->release(ground);
        assets->release(mesh);
        assets->update();
        device->wait_idle();
    }
    device->wait_idle();
    de::test::check_clean(*device);
}

TEST_CASE("pictures: a prefab is drawn with its mesh and its effect, filling the picture, and shown from its file") {
    using de::editor::Thumbnails;
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    const Sandbox sandbox;
    // A lamp: a square two metres a side, and a glow above it.
    std::vector<de::test::Vertex> vertices;
    std::vector<uint32_t> indices;
    de::test::quad(vertices, indices);
    de::test::write_src(sandbox, "content/props/square.gltf",
                        de::test::gltf(vertices, indices, 0.0f, R"([{"name": "paint"}])", "[]", "[]"));
    de::test::write_src(sandbox, "content/props/glow.vfx", kGlow);
    de::test::write_src(sandbox, "content/props/lamp.prefab", R"({
  "parts": {
    "body": {"mesh": "square.gltf"},
    "light": {"effect": "glow.vfx", "position": [0, 2, 0]}
  }
})");
    // And something small, of a model with coarser LODs: a wavy tile a hand across, with a spark over its middle.
    std::vector<de::test::Vertex> wavy;
    std::vector<uint32_t> wavy_indices;
    de::test::grid(48, wavy, wavy_indices);
    de::test::write_src(sandbox, "content/props/wavy.gltf",
                        de::test::gltf(wavy, wavy_indices, 0.0f, R"([{"name": "paint"}])", "[]", "[]"));
    de::test::write_src(sandbox, "content/props/spark.vfx", R"({
  "emitters": [
    {"name": "spark", "rate_per_s": 30, "lifetime_s": 0.5,
     "modules": [{"type": "size", "m": 0.02}, {"type": "color", "rgb": [1, 0.1, 0.05]}],
     "look": {"shape": "glow", "intensity": 2}}
  ]
})");
    de::test::write_src(sandbox, "content/props/tile.prefab", R"({
  "parts": {
    "tile": {"mesh": "wavy.gltf", "scale": 0.05},
    "spark": {"effect": "spark.vfx", "position": [0.1, 0.03, 0.1]}
  }
})");
    AssetId lamp_id;
    AssetId tile_id;
    {
        de::platform::Vfs cook_vfs = de::test::cooker_vfs(sandbox);
        REQUIRE(de::test::run_cook(cook_vfs).ok());
        de::asset::Guid guid;
        REQUIRE(de::editor::source_asset_id(cook_vfs, "src:/content/props/lamp.prefab", guid, lamp_id));
        REQUIRE(de::editor::source_asset_id(cook_vfs, "src:/content/props/tile.prefab", guid, tile_id));
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
    de::tools::create_imgui();
    {
        auto gpu_assets =
            de::render::GpuAssets::create({.device = &*device, .uploader = &*uploader, .assets = &*assets});
        REQUIRE(gpu_assets.ok());
        const de::editor::PictureEngine engine{.device = &*device,
                                               .shaders = &*shaders,
                                               .assets = &*assets,
                                               .gpu_assets = &*gpu_assets,
                                               .jobs = &jobs,
                                               .vfs = &vfs};
        Thumbnails thumbnails(heap(), 1, 2);
        de::editor::PreviewPictures pictures(heap());
        Thumbnails::Picture shown;
        // The editor's frame: its panels ask, the pictures move along, what loaded is taken in, the frame draws.
        const char* wanted = "src:/content/props/lamp.prefab";
        const auto frame = [&] {
            const bool there = thumbnails.find(wanted, shown);
            thumbnails.update(vfs);
            pictures.update(engine, thumbnails);
            assets->update();
            gpu_assets->update();
            device->begin_frame();
            uploader->record();
            pictures.draw();
            device->end_frame();
            return there;
        };
        uint32_t frames = 0;
        while (!frame() && frames < 600) {
            ++frames;
        }
        CHECK(frames < 600);
        CHECK(frames > 8); // several pictures were taken on the way
        CHECK(pictures.made() == 1);
        CHECK_FALSE(pictures.busy());

        // Its file: this look, settled, drawn from the prefab first and from what the prefab holds.
        de::core::FixedString<96> path;
        REQUIRE(path.assign("user:/editor/pictures/"));
        REQUIRE(path.append(de::asset::format_asset_id(lamp_id).view()));
        REQUIRE(path.append(".pic"));
        const auto file = vfs.read_whole_file(path.c_str(), heap());
        REQUIRE(file.ok());
        constexpr uint32_t kSide = de::asset::kThumbnailSize;
        uint32_t source_count = 0;
        uint32_t settled = 0;
        std::memcpy(&source_count, file->data() + 8, sizeof(source_count));
        std::memcpy(&settled, file->data() + 12, sizeof(settled));
        CHECK(settled == 1);
        REQUIRE(source_count >= 3); // the prefab, its mesh, its effect
        REQUIRE(file->size() == 16 + (size_t{source_count} * 32) + (size_t{kSide} * kSide * 4));
        de::editor::PictureSource first;
        std::memcpy(&first, file->data() + 16, sizeof(first));
        CHECK(first.asset.id == lamp_id);
        CHECK(first.asset.type == AssetType::Prefab);
        const std::byte* texels = file->data() + 16 + (size_t{source_count} * 32);
        const auto channel = [&](uint32_t x, uint32_t y, int c) {
            return static_cast<int>(texels[(((size_t{y} * kSide) + x) * 4) + static_cast<size_t>(c)]);
        };
        // The backdrop in the corners, dark; the square in the lower part, lit; the glow near the top, red.
        CHECK(channel(2, 2, 1) < 50);
        CHECK(channel(kSide - 3, kSide - 3, 1) < 80);
        uint32_t first_row = kSide;
        uint32_t last_row = 0;
        uint32_t lit = 0;
        uint32_t red = 0;
        for (uint32_t y = 0; y < kSide; ++y) {
            for (uint32_t x = 0; x < kSide; ++x) {
                const int r = channel(x, y, 0);
                const int g = channel(x, y, 1);
                if (r > 110) {
                    first_row = std::min(first_row, y);
                    last_row = std::max(last_row, y);
                }
                lit += r > 110 && g > 110 ? 1U : 0U;
                red += r > 110 && g < r - 60 ? 1U : 0U;
            }
        }
        CHECK(lit > (kSide * kSide) / 8); // the square: a good part of the picture
        CHECK(red > 20);                  // the glow
        CHECK(first_row < kSide / 5);     // and together they fill it, top to bottom, with air round them
        CHECK(first_row > 2);
        CHECK(last_row > (kSide * 4) / 5);
        CHECK(last_row < kSide - 3);
        // Asked for again after a refresh, it's shown from the file: nothing is drawn.
        thumbnails.forget_all();
        for (uint32_t i = 0; i < 4; ++i) {
            static_cast<void>(frame());
        }
        CHECK(frame());
        CHECK(pictures.made() == 1);

        // The small one, in the scene the first left: its tile lit across the picture, its spark on it.
        wanted = "src:/content/props/tile.prefab";
        frames = 0;
        while (!frame() && frames < 600) {
            ++frames;
        }
        CHECK(frames < 600);
        CHECK(pictures.made() == 2);
        de::core::FixedString<96> tile_path;
        REQUIRE(tile_path.assign("user:/editor/pictures/"));
        REQUIRE(tile_path.append(de::asset::format_asset_id(tile_id).view()));
        REQUIRE(tile_path.append(".pic"));
        const auto tile_file = vfs.read_whole_file(tile_path.c_str(), heap());
        REQUIRE(tile_file.ok());
        uint32_t tile_sources = 0;
        std::memcpy(&tile_sources, tile_file->data() + 8, sizeof(tile_sources));
        REQUIRE(tile_file->size() == 16 + (size_t{tile_sources} * 32) + (size_t{kSide} * kSide * 4));
        const std::byte* tile_texels = tile_file->data() + 16 + (size_t{tile_sources} * 32);
        uint32_t tile_lit = 0;
        uint32_t tile_red = 0;
        uint32_t tile_left = kSide;
        uint32_t tile_right = 0;
        for (uint32_t y = 0; y < kSide; ++y) {
            for (uint32_t x = 0; x < kSide; ++x) {
                const std::byte* texel = tile_texels + (((size_t{y} * kSide) + x) * 4);
                const int r = static_cast<int>(texel[0]);
                const int g = static_cast<int>(texel[1]);
                if (r > 110 && g > 110) {
                    ++tile_lit;
                    tile_left = std::min(tile_left, x);
                    tile_right = std::max(tile_right, x);
                }
                tile_red += r > 110 && g < r - 60 ? 1U : 0U;
            }
        }
        CHECK(tile_lit > 1200);
        CHECK(tile_red > 4);
        CHECK(tile_left < kSide / 5); // across the picture, with air either side
        CHECK(tile_left > 2);
        CHECK(tile_right > (kSide * 4) / 5);
        CHECK(tile_right < kSide - 3);

        // The spark by itself, two centimetres across. Particles fade out within a hand of a camera, so it's seen
        // from farther through a long lens: there, and filling its picture.
        wanted = "src:/content/props/spark.vfx";
        frames = 0;
        while (!frame() && frames < 600) {
            ++frames;
        }
        CHECK(frames < 600);
        CHECK(pictures.made() == 3);
        de::asset::Guid spark_guid;
        AssetId spark_id;
        REQUIRE(de::editor::source_asset_id(vfs, "src:/content/props/spark.vfx", spark_guid, spark_id));
        de::core::FixedString<96> spark_path;
        REQUIRE(spark_path.assign("user:/editor/pictures/"));
        REQUIRE(spark_path.append(de::asset::format_asset_id(spark_id).view()));
        REQUIRE(spark_path.append(".pic"));
        const auto spark_file = vfs.read_whole_file(spark_path.c_str(), heap());
        REQUIRE(spark_file.ok());
        uint32_t spark_sources = 0;
        std::memcpy(&spark_sources, spark_file->data() + 8, sizeof(spark_sources));
        REQUIRE(spark_file->size() == 16 + (size_t{spark_sources} * 32) + (size_t{kSide} * kSide * 4));
        const std::byte* spark_texels = spark_file->data() + 16 + (size_t{spark_sources} * 32);
        uint32_t spark_red = 0;
        uint32_t spark_top = kSide;
        uint32_t spark_bottom = 0;
        for (uint32_t y = 0; y < kSide; ++y) {
            for (uint32_t x = 0; x < kSide; ++x) {
                const std::byte* texel = spark_texels + (((size_t{y} * kSide) + x) * 4);
                if (static_cast<int>(texel[0]) > 110 && static_cast<int>(texel[1]) < static_cast<int>(texel[0]) - 60) {
                    ++spark_red;
                    spark_top = std::min(spark_top, y);
                    spark_bottom = std::max(spark_bottom, y);
                }
            }
        }
        CHECK(spark_red > 1500);
        CHECK(spark_bottom - spark_top > kSide / 2);
        pictures.shutdown(engine);
        assets->update();
        device->wait_idle();
    }
    de::tools::destroy_imgui();
    device->wait_idle();
    de::test::check_clean(*device);
}

TEST_CASE("pictures: one waits until what it shows has reached the GPU, however far behind the uploads are") {
    using de::editor::Thumbnails;
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    const Sandbox sandbox;
    std::vector<de::test::Vertex> vertices;
    std::vector<uint32_t> indices;
    de::test::quad(vertices, indices);
    de::test::write_src(sandbox, "content/props/square.gltf",
                        de::test::gltf(vertices, indices, 0.0f, R"([{"name": "paint"}])", "[]", "[]"));
    de::test::write_src(sandbox, "content/props/board.prefab", R"({"parts": {"body": {"mesh": "square.gltf"}}})");
    AssetId board_id;
    {
        de::platform::Vfs cook_vfs = de::test::cooker_vfs(sandbox);
        REQUIRE(de::test::run_cook(cook_vfs).ok());
        de::asset::Guid guid;
        REQUIRE(de::editor::source_asset_id(cook_vfs, "src:/content/props/board.prefab", guid, board_id));
    }
    de::core::JobSystem jobs({.worker_count = 2});
    de::platform::Vfs vfs = sandbox.make_vfs({.engine_root = DE_SOURCE_DIR, .jobs = &jobs, .game_writable = true});
    auto compiler = de::shader::ShaderCompiler::create(vfs);
    REQUIRE(compiler.ok());
    auto shaders = de::shader::ShaderLibrary::create(*device, vfs, jobs, *compiler, {.hot_reload = false});
    REQUIRE(shaders.ok());
    // A frame takes one of these uploads at most, and they're taken oldest first: what's queued behind many waits.
    constexpr uint64_t kLump = uint64_t{48} * 1024;
    auto uploader = de::rhi::Uploader::create(*device, {.frame_budget_bytes = uint64_t{64} * 1024});
    REQUIRE(uploader.ok());
    auto assets = de::asset::AssetRegistry::create({.vfs = &vfs, .jobs = &jobs, .hot_reload = false});
    REQUIRE(assets.ok());
    constexpr uint32_t kLumps = 256;
    const auto lumps = device->create_buffer(
        {.size = kLump * kLumps, .usage = de::rhi::BufferUsage::Storage | de::rhi::BufferUsage::CopyDst});
    REQUIRE(lumps.ok());
    const std::vector<std::byte> lump(kLump, std::byte{7});
    de::tools::create_imgui();
    {
        auto gpu_assets =
            de::render::GpuAssets::create({.device = &*device, .uploader = &*uploader, .assets = &*assets});
        REQUIRE(gpu_assets.ok());
        const de::editor::PictureEngine engine{.device = &*device,
                                               .shaders = &*shaders,
                                               .assets = &*assets,
                                               .gpu_assets = &*gpu_assets,
                                               .jobs = &jobs,
                                               .vfs = &vfs};
        Thumbnails thumbnails(heap(), 1, 2);
        de::editor::PreviewPictures pictures(heap());
        Thumbnails::Picture shown;
        uint32_t frames = 0;
        uint32_t queued = 0;
        bool there = false;
        while (!there && frames < 900) {
            there = thumbnails.find("src:/content/props/board.prefab", shown);
            thumbnails.update(vfs);
            pictures.update(engine, thumbnails);
            assets->update();
            gpu_assets->update();
            // A world coming in beside it: three uploads queued for each one a frame takes, for a while.
            for (uint32_t i = 0; i < 3 && queued < 90; ++i, ++queued) {
                REQUIRE(uploader->upload_buffer(*lumps, kLump * queued, lump, de::rhi::ResourceState::ShaderRead).ok());
            }
            device->begin_frame();
            uploader->record();
            pictures.draw();
            device->end_frame();
            ++frames;
        }
        CHECK(there);
        de::core::FixedString<96> path;
        REQUIRE(path.assign("user:/editor/pictures/"));
        REQUIRE(path.append(de::asset::format_asset_id(board_id).view()));
        REQUIRE(path.append(".pic"));
        const auto file = vfs.read_whole_file(path.c_str(), heap());
        REQUIRE(file.ok());
        constexpr uint32_t kSide = de::asset::kThumbnailSize;
        uint32_t source_count = 0;
        uint32_t settled = 0;
        std::memcpy(&source_count, file->data() + 8, sizeof(source_count));
        std::memcpy(&settled, file->data() + 12, sizeof(settled));
        CHECK(settled == 1);
        REQUIRE(file->size() == 16 + (size_t{source_count} * 32) + (size_t{kSide} * kSide * 4));
        const std::byte* texels = file->data() + 16 + (size_t{source_count} * 32);
        uint32_t lit = 0;
        for (uint32_t i = 0; i < kSide * kSide; ++i) {
            lit += static_cast<int>(texels[size_t{i} * 4]) > 110 && static_cast<int>(texels[(size_t{i} * 4) + 1]) > 110
                       ? 1U
                       : 0U;
        }
        CHECK(lit > (kSide * kSide) / 6); // the square is in its picture
        pictures.shutdown(engine);
        assets->update();
        device->wait_idle();
    }
    de::tools::destroy_imgui();
    device->wait_idle();
    device->destroy(*lumps);
    device->wait_idle();
    de::test::check_clean(*device);
}
