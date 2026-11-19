#include "editor/preview_pictures.h"

#include "asset/effect_format.h"
#include "asset/mesh_format.h"
#include "asset/prefab_format.h"
#include "asset/texture_format.h"
#include "core/log.h"
#include "core/profile.h"
#include "platform/time.h"
#include "render/scene/gpu_assets.h"
#include "render/shader/shader_library.h"
#include "world/prefab.h"
#include "world/render_extract.h"
#include "world/transform.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace de::editor {
namespace {

constexpr uint32_t kSize = asset::kThumbnailSize;
constexpr uint32_t kShotSize = kSize * kPictureSupersample;
constexpr double kStepS = 1.0 / 60.0; // the scene's time moves on by this a frame, whatever the frame rate
// What a subject fills of the picture, at most: the rest is air round it.
constexpr float kFill = 0.82f;
constexpr float kLeastDistanceM = 0.02f;
constexpr float kWidestFovRad = 1.4f;
constexpr float kNarrowestFovRad = 0.002f;
// Particles fade out as they near a camera, and are gone within a hand of it (shaders/scene/particles.slang, 0.1 m
// and the 0.3 m past it): a picture of an effect is taken from farther than that from anything of it.
constexpr float kParticlesWholeFromM = 0.45f;
// An effect is shown as old as its longest-lived particles get, so every age of them is in the picture; no older
// than this, which is as far as a picture waits on it.
constexpr float kLongestAgeS = 20.0f;
// A texel brighter than this, of 255, is something: below it is the black the measuring picture is taken on.
constexpr uint8_t kSomething = 8;
constexpr uint32_t kMostMeasures = 4;
constexpr uint32_t kLeastSettleFrames = 3;
constexpr uint32_t kSettleFrames = 600; // waited for what a picture shows to be there, before it's shot as it is
constexpr uint32_t kLoadFrames = 3600;
constexpr uint32_t kPictureFrames = 120; // a picture comes back in three
constexpr uint32_t kIdleFrames = 900;    // with nothing to draw, before the scene goes
constexpr uint32_t kFlushFrames = 3;

// What an 8-bit sRGB value is as light.
const float* srgb_to_linear() {
    static float table[256];
    static bool made = false;
    if (!made) {
        for (int i = 0; i < 256; ++i) {
            const float c = static_cast<float>(i) / 255.0f;
            table[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        made = true;
    }
    return table;
}

std::byte linear_to_srgb(float light) {
    const float c = std::clamp(light, 0.0f, 1.0f);
    const float encoded = c <= 0.0031308f ? c * 12.92f : (1.055f * std::pow(c, 1.0f / 2.4f)) - 0.055f;
    return static_cast<std::byte>(static_cast<uint8_t>(std::lround(encoded * 255.0f)));
}

} // namespace

void shrink_picture(std::span<const std::byte> rgba, uint32_t size, uint32_t box, std::span<std::byte> out) {
    const size_t wide = size_t{size} * box;
    if (box == 0 || rgba.size() != wide * wide * 4 || out.size() != size_t{size} * size * 4) {
        return;
    }
    const float* light = srgb_to_linear();
    const float share = 1.0f / static_cast<float>(box * box);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            float sum[4] = {};
            for (uint32_t by = 0; by < box; ++by) {
                const std::byte* row = rgba.data() + ((((size_t{y} * box) + by) * wide) + (size_t{x} * box)) * 4;
                for (uint32_t bx = 0; bx < box; ++bx) {
                    for (int channel = 0; channel < 3; ++channel) {
                        sum[channel] += light[static_cast<uint8_t>(row[(bx * 4) + channel])];
                    }
                    sum[3] += static_cast<float>(static_cast<uint8_t>(row[(bx * 4) + 3]));
                }
            }
            std::byte* to = out.data() + (((size_t{y} * size) + x) * 4);
            for (int channel = 0; channel < 3; ++channel) {
                to[channel] = linear_to_srgb(sum[channel] * share);
            }
            to[3] = static_cast<std::byte>(static_cast<uint8_t>(std::lround(sum[3] * share)));
        }
    }
}

bool measure_picture(std::span<const std::byte> rgba, uint32_t width, uint32_t height, PictureBox& out) {
    if (width == 0 || height == 0 || rgba.size() != size_t{width} * height * 4) {
        return false;
    }
    uint32_t left = width;
    uint32_t right = 0;
    uint32_t top = height;
    uint32_t bottom = 0;
    for (uint32_t y = 0; y < height; ++y) {
        const std::byte* row = rgba.data() + (size_t{y} * width * 4);
        for (uint32_t x = 0; x < width; ++x) {
            const std::byte* texel = row + (size_t{x} * 4);
            const uint8_t brightest = std::max(
                {static_cast<uint8_t>(texel[0]), static_cast<uint8_t>(texel[1]), static_cast<uint8_t>(texel[2])});
            if (brightest > kSomething) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    }
    if (left > right) {
        return false;
    }
    // Texel edges, as the picture's own coordinates: x from -1 at the left to 1, y from -1 at the bottom to 1.
    out.min_x = (static_cast<float>(left) / static_cast<float>(width) * 2.0f) - 1.0f;
    out.max_x = (static_cast<float>(right + 1) / static_cast<float>(width) * 2.0f) - 1.0f;
    out.max_y = 1.0f - (static_cast<float>(top) / static_cast<float>(height) * 2.0f);
    out.min_y = 1.0f - (static_cast<float>(bottom + 1) / static_cast<float>(height) * 2.0f);
    return true;
}

PictureAxes picture_axes(float yaw_rad, float pitch_rad) {
    PictureAxes axes;
    axes.toward =
        Vec3(std::sin(yaw_rad) * std::cos(pitch_rad), std::sin(pitch_rad), std::cos(yaw_rad) * std::cos(pitch_rad));
    axes.right = glm::normalize(glm::cross(kWorldUp, axes.toward));
    axes.up = glm::cross(axes.toward, axes.right);
    return axes;
}

PictureAxes picture_axes() {
    return picture_axes(kPictureYawRad, kPicturePitchRad);
}

void look_from(PictureFraming& framing, const PictureSubject& subject, float wanted_m) {
    float least = kLeastDistanceM;
    for (const Vec4& sphere : subject.spheres) {
        least = std::max(least, kParticlesWholeFromM + sphere.w);
    }
    // An effect isn't drawn from past its draw distance, and fades before it.
    const float farthest = subject.nearest_draw_distance_m * 0.6f;
    float distance = std::max(wanted_m, least);
    if (farthest > 0.0f) {
        distance = std::min(distance, std::max(farthest, kLeastDistanceM));
    }
    const float tan_half = std::tan(framing.vertical_fov_rad * 0.5f) * std::max(wanted_m, 1e-6f) / distance;
    framing.vertical_fov_rad = std::clamp(2.0f * std::atan(tan_half), kNarrowestFovRad, kWidestFovRad);
    framing.distance_m = distance;
}
bool subject_extent(const PictureSubject& subject, const PictureAxes& axes, float fill, Vec3& middle, float& wanted_m) {
    // The middle of what it takes up, along each of the camera's axes.
    Vec3 low(FLT_MAX);
    Vec3 high(-FLT_MAX);
    const auto take = [&](Vec3 point, float radius) {
        const Vec3 along(glm::dot(point, axes.right), glm::dot(point, axes.up), glm::dot(point, axes.toward));
        low = glm::min(low, along - radius);
        high = glm::max(high, along + radius);
    };
    for (const Vec3& point : subject.points) {
        take(point, 0.0f);
    }
    for (const Vec4& sphere : subject.spheres) {
        take(Vec3(sphere), sphere.w);
    }
    if (low.x > high.x) {
        return false;
    }
    const Vec3 along = (low + high) * 0.5f;
    middle = (axes.right * along.x) + (axes.up * along.y) + (axes.toward * along.z);
    // Far enough that everything is inside the fill: what's nearer the camera needs it farther.
    const float tan_half = std::tan(kPictureFovRad * 0.5f) * fill;
    float distance = 0.0f;
    const auto fit = [&](Vec3 point, float radius) {
        const Vec3 from = point - middle;
        const float across = std::max(std::abs(glm::dot(from, axes.right)), std::abs(glm::dot(from, axes.up)));
        distance = std::max(distance, ((across + radius) / tan_half) + glm::dot(from, axes.toward) + radius);
    };
    for (const Vec3& point : subject.points) {
        fit(point, 0.0f);
    }
    for (const Vec4& sphere : subject.spheres) {
        fit(Vec3(sphere), sphere.w);
    }
    wanted_m = distance;
    return true;
}

render::SceneLighting studio_lighting(const PictureAxes& axes, float distance_m) {
    render::SceneLighting lighting;
    const Vec3 key = glm::normalize((axes.up * 0.75f) - (axes.right * 0.5f) + (axes.toward * 0.45f));
    lighting.sun_direction[0] = key.x;
    lighting.sun_direction[1] = key.y;
    lighting.sun_direction[2] = key.z;
    const float sun[3] = {3.1f, 2.95f, 2.75f};
    const float sky[3] = {0.46f, 0.50f, 0.58f};
    const float ground[3] = {0.20f, 0.185f, 0.17f};
    std::copy_n(sun, 3, lighting.sun_color);
    std::copy_n(sky, 3, lighting.sky_color);
    std::copy_n(ground, 3, lighting.ground_color);
    lighting.shadows.resolution = 1024;
    lighting.shadows.distance_m = std::max(distance_m * 4.0f, 2.0f);
    lighting.shadows.caster_reach_m = lighting.shadows.distance_m;
    lighting.backdrop.enabled = true;
    return lighting;
}

PictureFraming frame_subject(const PictureSubject& subject, float fill) {
    PictureFraming framing;
    float wanted_m = 0.0f;
    if (subject_extent(subject, picture_axes(), fill, framing.target, wanted_m)) {
        look_from(framing, subject, wanted_m);
    }
    return framing;
}

PictureFraming refit_framing(const PictureFraming& framing, const PictureSubject& subject, const PictureBox* measured,
                             float fill, bool& settled) {
    const PictureAxes axes = picture_axes();
    const float tan_half = std::tan(framing.vertical_fov_rad * 0.5f);
    // What the picture showed: what was measured in it, and the subject's own points through the same camera (a dark
    // mesh on black measures as nothing).
    bool any = measured != nullptr;
    PictureBox box = measured != nullptr ? *measured : PictureBox{};
    for (const Vec3& point : subject.points) {
        const Vec3 from = point - framing.target;
        const float depth = framing.distance_m - glm::dot(from, axes.toward);
        if (depth <= 1e-4f) {
            continue;
        }
        const float x = glm::dot(from, axes.right) / (depth * tan_half);
        const float y = glm::dot(from, axes.up) / (depth * tan_half);
        if (!any) {
            box = {.min_x = x, .max_x = x, .min_y = y, .max_y = y};
            any = true;
        } else {
            box.min_x = std::min(box.min_x, x);
            box.max_x = std::max(box.max_x, x);
            box.min_y = std::min(box.min_y, y);
            box.max_y = std::max(box.max_y, y);
        }
    }
    settled = true;
    if (!any) {
        return framing; // nothing shows: as good as it gets
    }
    const float middle_x = (box.min_x + box.max_x) * 0.5f;
    const float middle_y = (box.min_y + box.max_y) * 0.5f;
    const float half = std::max(box.max_x - box.min_x, box.max_y - box.min_y) * 0.5f;
    if (half < 1e-4f) {
        return framing;
    }
    // Its middle to the picture's, at the subject's distance; then as near as makes it fill what it should.
    PictureFraming next = framing;
    next.target += (axes.right * (middle_x * framing.distance_m * tan_half)) +
                   (axes.up * (middle_y * framing.distance_m * tan_half));
    const float scale = half / fill;
    look_from(next, subject, framing.distance_m * scale);
    settled = scale > 0.94f && scale < 1.06f && std::abs(middle_x) < 0.04f && std::abs(middle_y) < 0.04f;
    return next;
}

PreviewPictures::PreviewPictures(core::Allocator& allocator)
    : allocator_(&allocator), bounds_(allocator), meshes_(allocator), picture_(allocator), small_(allocator),
      drawn_from_(allocator) {}

bool PreviewPictures::begin(const PictureEngine& engine) {
    if (!scene_) {
        const uint64_t start_ns = platform::time_ns();
        core::Result<app::PreviewScene> scene = app::PreviewScene::create({.device = engine.device,
                                                                           .shaders = engine.shaders,
                                                                           .assets = engine.assets,
                                                                           .gpu_assets = engine.gpu_assets,
                                                                           .jobs = engine.jobs});
        if (!scene) {
            DE_LOG_ERROR("editor", "no scene to draw pictures in: %s", scene.error().message);
            return false;
        }
        scene_.emplace(std::move(*scene));
        scene_->set_size(kShotSize, kShotSize);
        DE_LOG_INFO("editor", "pictures: a scene to draw them in, made in %.1f ms",
                    static_cast<double>(platform::time_ns() - start_ns) / static_cast<double>(platform::kNsPerMs));
    }
    held_ = engine.assets->load(request_.asset);
    stage_ = Stage::Loading;
    frames_ = 0;
    measures_ = 0;
    settled_ = false;
    return true;
}

void PreviewPictures::place_subject(const PictureEngine& engine, app::PreviewScene& scene) {
    flecs::world& ecs = scene.world().ecs();
    bounds_.clear();
    meshes_.clear();
    effects_ = 0;
    emitters_ = 0;
    const double now_s = scene.time_s();
    const auto add_effect = [&](flecs::entity entity, const world::WorldTransform& where) {
        auto* placed = entity.try_get_mut<world::EffectInstance>();
        const auto* effect = placed != nullptr ? engine.assets->get<asset::EffectAsset>(placed->effect) : nullptr;
        if (effect == nullptr) {
            return;
        }
        float longest_s = 0.0f;
        float reach_m = 0.0f;
        for (const asset::EffectEmitter& emitter : effect->emitters.get(effect)) {
            longest_s = std::max(longest_s, emitter.lifetime_s[1]);
            reach_m = std::max(reach_m, emitter.reach_m);
            ++emitters_;
        }
        // Held at that age: the same particles in every picture taken of it.
        placed->clock.seek(now_s, std::min(longest_s, kLongestAgeS));
        placed->clock.rate = 0.0f;
        ++effects_;
        const float scale = std::max({where.scale.x, where.scale.y, where.scale.z, 1.0f});
        bounds_.spheres.push(Vec4(Vec3(where.position), reach_m * scale));
        bounds_.nearest_draw_distance_m = bounds_.nearest_draw_distance_m > 0.0f
                                              ? std::min(bounds_.nearest_draw_distance_m, effect->draw_distance_m)
                                              : effect->draw_distance_m;
    };
    const auto add_mesh = [&](flecs::entity entity, const world::WorldTransform& where) {
        const auto* placed = entity.try_get<world::MeshInstance>();
        const auto* mesh = placed != nullptr ? engine.assets->get<asset::MeshAsset>(placed->mesh) : nullptr;
        if (mesh == nullptr) {
            return;
        }
        meshes_.push(placed->mesh);
        for (uint32_t corner = 0; corner < 8; ++corner) {
            const Vec3 local((corner & 1U) != 0 ? mesh->bounds.max[0] : mesh->bounds.min[0],
                             (corner & 2U) != 0 ? mesh->bounds.max[1] : mesh->bounds.min[1],
                             (corner & 4U) != 0 ? mesh->bounds.max[2] : mesh->bounds.min[2]);
            bounds_.points.push(Vec3(where.position) + (where.rotation * (where.scale * local)));
        }
    };
    const flecs::entity subject = ecs.entity();
    subject_ = subject.id();
    if (request_.asset.type == asset::AssetType::Effect) {
        subject.set(world::EffectInstance{.effect = held_});
        world::place(subject, world::Transform{});
        add_effect(subject, world::WorldTransform{});
        return;
    }
    world::place(subject, world::Transform{});
    first_part_ = world::make_prefab_parts(ecs, *engine.assets, held_, 0, subject_, world::WorldTransform{}, true);
    world::each_prefab_part(ecs, first_part_, [&](flecs::entity part) {
        if (const auto* where = part.try_get<world::WorldTransform>(); where != nullptr) {
            const world::WorldTransform at = *where; // copied: nothing below may move the part's row under it
            add_mesh(part, at);
            add_effect(part, at);
        }
    });
}

bool PreviewPictures::ready(const PictureEngine& engine, const app::PreviewScene& scene) const {
    if (frames_ < kLeastSettleFrames || engine.shaders->stats().first_builds != 0) {
        return false;
    }
    // What it shows has reached the GPU, not only set off for it: while a world is coming in, uploads queue for
    // many frames, and a picture taken meanwhile would lack what's still in the queue.
    for (const asset::AssetHandle handle : meshes_) {
        if (!engine.gpu_assets->mesh_on_gpu(handle)) {
            return false;
        }
        const auto* mesh = engine.assets->get<asset::MeshAsset>(handle);
        if (mesh == nullptr) {
            continue;
        }
        for (const asset::MeshMaterialSlot& slot : mesh->materials.get(mesh)) {
            // One that couldn't load is drawn with the default: nothing more is coming for it.
            const asset::AssetHandle material =
                engine.assets->find({.id = slot.material, .type = asset::AssetType::Material});
            if (slot.material.is_valid() && engine.assets->state(material) == asset::AssetState::Ready &&
                !engine.gpu_assets->material_on_gpu(slot.material)) {
                return false;
            }
        }
    }
    // And the renderer's own word for it: with meshes to show, its cull has found something to draw (what it
    // kept, read back from the GPU a frame or two ago).
    const render::SceneRendererStats stats = scene.stats();
    if (!meshes_.empty() && stats.visible_draws == 0) {
        return false;
    }
    // Its particles where their clocks say, all of them in the picture.
    const render::ParticleStats& particles = stats.particles;
    return particles.steps == 0 && particles.emitters_starved == 0 && particles.emitters_drawn == emitters_;
}

void PreviewPictures::aim(app::PreviewScene& scene) {
    const PictureAxes axes = picture_axes();
    const Vec3 eye = framing_.target + (axes.toward * framing_.distance_m);
    scene.set_camera({.position = DVec3(eye),
                      .rotation = glm::quat_cast(Mat3(axes.right, axes.up, axes.toward)),
                      .vertical_fov_rad = framing_.vertical_fov_rad,
                      .near_m = std::clamp(framing_.distance_m * 0.05f, 0.001f, 1.0f)});
    lighting_ = studio_lighting(axes, framing_.distance_m);
}

void PreviewPictures::shoot(bool measuring) {
    // On black to measure what's there; on the studio's backdrop to keep.
    const float top[3] = {0.012f, 0.013f, 0.016f};
    const float bottom[3] = {0.040f, 0.043f, 0.050f};
    for (int channel = 0; channel < 3; ++channel) {
        lighting_.backdrop.top[channel] = measuring ? 0.0f : top[channel];
        lighting_.backdrop.bottom[channel] = measuring ? 0.0f : bottom[channel];
    }
    shot_tag_ = next_tag_++;
    waiting_tag_ = shot_tag_;
}

bool PreviewPictures::collect_sources(const PictureEngine& engine) {
    drawn_from_.clear();
    core::Array<asset::AssetHandle> to_visit(*allocator_);
    to_visit.push(held_);
    while (!to_visit.empty()) {
        const asset::AssetHandle handle = to_visit.back();
        to_visit.pop();
        const asset::AssetRef asset = engine.assets->asset(handle);
        const bool known =
            std::ranges::any_of(drawn_from_, [&](const PictureSource& source) { return source.asset == asset; });
        if (known || !asset.id.is_valid()) {
            continue;
        }
        // One that isn't there has no file: its picture holds until it has one.
        const platform::FileInfo info = engine.assets->file_info(handle);
        drawn_from_.push({.asset = asset, .size = info.size, .modified_time = info.modified_time});
        for (const asset::AssetHandle dependency : engine.assets->dependencies(handle)) {
            to_visit.push(dependency);
        }
    }
    return !drawn_from_.empty() && drawn_from_[0].asset == request_.asset && drawn_from_[0].size != 0;
}

void PreviewPictures::clear_subject(const PictureEngine& engine) {
    if (scene_) {
        flecs::world& ecs = scene_->world().ecs();
        world::delete_prefab_parts(ecs, first_part_);
        if (subject_ != 0 && ecs.is_alive(subject_)) {
            ecs.entity(subject_).destruct();
        }
    }
    first_part_ = 0;
    subject_ = 0;
    if (held_.is_valid()) {
        engine.assets->release(held_);
    }
    held_ = {};
}

void PreviewPictures::finish(const PictureEngine& engine, Thumbnails& thumbnails, bool made) {
    thumbnails.rendered(request_, made); // nothing, if it's no longer wanted
    clear_subject(engine);
    stage_ = Stage::Idle;
    idle_frames_ = 0;
    flush_frames_ = kFlushFrames;
    shot_tag_ = 0;
    waiting_tag_ = 0;
    made_ += made ? 1U : 0U;
}

void PreviewPictures::update(const PictureEngine& engine, Thumbnails& thumbnails) {
    DE_PROFILE_ZONE("PreviewPictures::update");
    draw_now_ = false;
    if (stage_ == Stage::Idle) {
        if (!thumbnails.take_render_request(request_)) {
            if (!scene_) {
                return;
            }
            if (flush_frames_ > 0) {
                // What left the scene reaches its renderer, before what it drew is let go.
                --flush_frames_;
                scene_->advance(kStepS);
                draw_now_ = true;
            } else if (++idle_frames_ > kIdleFrames) {
                scene_.reset();
                DE_LOG_INFO("editor", "pictures: nothing to draw, the scene is let go (%u made)", made_);
            }
            return;
        }
        idle_frames_ = 0;
        if (!begin(engine)) {
            thumbnails.rendered(request_, false);
            return;
        }
    }
    if (!thumbnails.wants(request_)) {
        finish(engine, thumbnails, false); // forgotten meanwhile (a cook, a refresh): asked again if still shown
        return;
    }
    if (!scene_) {
        return; // a picture is under way only in a scene
    }
    app::PreviewScene& scene = *scene_;
    ++frames_;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t tag = 0;
    switch (stage_) {
    case Stage::Idle:
        break;
    case Stage::Loading: {
        const asset::AssetState state = engine.assets->state(held_);
        if (state == asset::AssetState::Failed || frames_ > kLoadFrames) {
            finish(engine, thumbnails, false);
            return;
        }
        if (state == asset::AssetState::Ready) {
            place_subject(engine, scene);
            framing_ = frame_subject(bounds_, kFill);
            aim(scene);
            stage_ = Stage::Settling;
            frames_ = 0;
        }
        break;
    }
    case Stage::Settling: {
        const bool there = ready(engine, scene);
        if (there || frames_ > kSettleFrames) {
            if (!there) {
                DE_LOG_WARN("editor", "pictures: %s wasn't all there after %u frames: drawn as it is",
                            asset::format_asset_id(request_.asset.id).c_str(), kSettleFrames);
            }
            settled_ = there;
            shoot(true);
            stage_ = Stage::Measuring;
            frames_ = 0;
        }
        break;
    }
    case Stage::Measuring:
        if (scene.take_picture(picture_, width, height, tag) && tag == waiting_tag_) {
            PictureBox box;
            const bool any = measure_picture(picture_.span(), width, height, box);
            bool framed = false;
            framing_ = refit_framing(framing_, bounds_, any ? &box : nullptr, kFill, framed);
            aim(scene);
            if (framed || ++measures_ >= kMostMeasures) {
                // What it's drawn from, as of the picture that's kept.
                settled_ = collect_sources(engine) && settled_;
                shoot(false);
                stage_ = Stage::Shooting;
            } else {
                shoot(true);
            }
            frames_ = 0;
        } else if (frames_ > kPictureFrames) {
            finish(engine, thumbnails, false);
            return;
        }
        break;
    case Stage::Shooting:
        if (scene.take_picture(picture_, width, height, tag) && tag == waiting_tag_) {
            bool made = false;
            DE_LOG_DEBUG("editor", "pictures: %s drawn from %.3f m after %u measures: %zu meshes, %u effects%s",
                         asset::format_asset_id(request_.asset.id).c_str(), static_cast<double>(framing_.distance_m),
                         measures_, meshes_.size(), effects_, settled_ ? "" : " (not all there)");
            if (width == kShotSize && height == kShotSize) {
                small_.resize(size_t{kSize} * kSize * 4);
                shrink_picture(picture_.span(), kSize, kPictureSupersample, small_.span());
                const core::Result<void> written = Thumbnails::write_picture(
                    *engine.vfs, request_.asset.id, small_.span(), drawn_from_.span(), settled_, *allocator_);
                if (!written) {
                    DE_LOG_ERROR("editor", "pictures: %s: %s", asset::format_asset_id(request_.asset.id).c_str(),
                                 written.error().message);
                }
                made = written.ok();
            }
            finish(engine, thumbnails, made);
            return;
        }
        if (frames_ > kPictureFrames) {
            finish(engine, thumbnails, false);
            return;
        }
        break;
    }
    scene.advance(kStepS);
    draw_now_ = true;
}

void PreviewPictures::draw() {
    if (!scene_ || !draw_now_) {
        return;
    }
    draw_now_ = false;
    if (shot_tag_ != 0) {
        frames_ = 0; // the picture is waited for from the frame that takes it
    }
    scene_->draw(lighting_, false, shot_tag_);
    shot_tag_ = 0;
}

void PreviewPictures::shutdown(const PictureEngine& engine) {
    clear_subject(engine);
    scene_.reset();
    stage_ = Stage::Idle;
}

} // namespace de::editor
