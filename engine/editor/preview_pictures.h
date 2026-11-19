#pragma once

#include "app/preview_scene.h"
#include "asset/registry.h"
#include "core/allocator.h"
#include "core/array.h"
#include "core/math.h"
#include "editor/thumbnails.h"
#include "render/scene/scene_renderer.h"

#include <flecs.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace de::core {
class JobSystem;
}
namespace de::platform {
class Vfs;
}
namespace de::rhi {
class Device;
}
namespace de::shader {
class ShaderLibrary;
}
namespace de::render {
class GpuAssets;
}

namespace de::editor {

// What drawing pictures uses of the running engine.
struct PictureEngine {
    rhi::Device* device = nullptr;
    shader::ShaderLibrary* shaders = nullptr;
    asset::AssetRegistry* assets = nullptr;
    render::GpuAssets* gpu_assets = nullptr;
    core::JobSystem* jobs = nullptr;
    platform::Vfs* vfs = nullptr; // where the pictures' files go (user:/)
};

// A box of texels to a picture's side, each way: what a drawn picture is shot at, and brought down from.
inline constexpr uint32_t kPictureSupersample = 4;

// Brings a square picture down by whole boxes of texels: size x size from (size * box) x (size * box), RGBA sRGB,
// averaged as light (not as the numbers a screen is sent), so a bright edge against a dark ground stays as bright as
// it was. out is size * size * 4 bytes.
void shrink_picture(std::span<const std::byte> rgba, uint32_t size, uint32_t box, std::span<std::byte> out);

// What a picture shows of its subject, measured on a black ground: the box of texels brighter than nothing, as the
// picture's own coordinates from -1 to 1 (x right, y up). False if there are none.
struct PictureBox {
    float min_x = 0.0f;
    float max_x = 0.0f;
    float min_y = 0.0f;
    float max_y = 0.0f;
};
bool measure_picture(std::span<const std::byte> rgba, uint32_t width, uint32_t height, PictureBox& out);

// Where a picture is taken from: looking at a point from a distance, always from the same side (the front, a little
// to the left and above, as the cooker draws models).
inline constexpr float kPictureFovRad = 0.35f;
struct PictureFraming {
    Vec3 target{0.0f};
    float distance_m = 1.0f;
    float vertical_fov_rad = kPictureFovRad;
};

// What a subject takes up: the corners of its meshes' boxes, and a sphere round each effect (as far as its particles
// may get: loose, and only where to start looking).
struct PictureSubject {
    core::Array<Vec3> points;
    core::Array<Vec4> spheres;         // xyz: the centre, w: the radius
    float nearest_draw_distance_m = 0; // of its effects; 0: none

    explicit PictureSubject(core::Allocator& allocator) : points(allocator), spheres(allocator) {}
    void clear() {
        points.clear();
        spheres.clear();
        nearest_draw_distance_m = 0.0f;
    }
};

// The camera's axes for a picture: right, up, and from the subject toward the camera.
struct PictureAxes {
    Vec3 right;
    Vec3 up;
    Vec3 toward;
};
// Looking from an angle round the subject (0: from +Z; positive: from its right) and above it.
PictureAxes picture_axes(float yaw_rad, float pitch_rad);
// A picture's own: the front, a little to the left and above.
inline constexpr float kPictureYawRad = -0.39762799f;
inline constexpr float kPicturePitchRad = 0.32047221f;
PictureAxes picture_axes();

// Where a subject's middle is as seen along those axes, and how far the usual lens would stand to hold all of it
// within `fill` of the picture. False if there's nothing to it.
bool subject_extent(const PictureSubject& subject, const PictureAxes& axes, float fill, Vec3& middle, float& wanted_m);
// Looks from a distance that would show the subject at the size wanted, if the subject lets it: no nearer than its
// effects show whole from, no farther than they're drawn from. Past either, it stays there and the lens takes up the
// difference, so the picture is the same size. framing.vertical_fov_rad is the lens the distance was wanted for.
void look_from(PictureFraming& framing, const PictureSubject& subject, float wanted_m);
// A studio's light for a thing seen from those axes at that distance: a warm key from the upper left, in front; a
// cool sky to fill; a ground that gives a little back; no fog, still air, a backdrop (dark: its colours are the
// caller's to change).
render::SceneLighting studio_lighting(const PictureAxes& axes, float distance_m);

// A framing that holds all of a subject, filling this much of the picture's width or height at most. An effect
// isn't drawn past its draw distance, and its particles fade out near a camera: the camera keeps between the two,
// and where that isn't the distance the size wants, the lens widens or narrows instead.
PictureFraming frame_subject(const PictureSubject& subject, float fill);

// A framing moved and scaled so that what a picture taken with it showed (a box measured from it, with the
// subject's own points added) fills this much. settled: it was already about there.
PictureFraming refit_framing(const PictureFraming& framing, const PictureSubject& subject, const PictureBox* measured,
                             float fill, bool& settled);

// Draws the pictures of effects and prefabs (docs/design.md, Preview scenes): each alone in a scene of its own, by
// the engine's renderer, under one studio light, from one side. An effect is held at an age where it has settled
// (as long as its longest-lived particles last), so its picture is the same every time. One at a time: loaded, put
// in the scene, waited on until everything it shows is on the GPU and its particles have caught up, measured (a
// picture on black says what it covers, which nothing else knows of particles), framed on that, shot at four times
// the size, brought down, and written with what it was drawn from (Thumbnails::write_picture). Its scene goes when
// it has had nothing to draw for a while.
class PreviewPictures {
public:
    explicit PreviewPictures(core::Allocator& allocator);
    ~PreviewPictures() = default;
    PreviewPictures(const PreviewPictures&) = delete;
    PreviewPictures& operator=(const PreviewPictures&) = delete;
    PreviewPictures(PreviewPictures&&) = delete;
    PreviewPictures& operator=(PreviewPictures&&) = delete;

    // Each frame, before the frame draws: takes the next picture Thumbnails asks for when it's free, and moves the
    // one it's on along.
    void update(const PictureEngine& engine, Thumbnails& thumbnails);
    // From Application::draw: its scene's passes, while it's on a picture.
    void draw();
    // Lets go of its scene and what it holds: before the engine goes.
    void shutdown(const PictureEngine& engine);

    bool busy() const { return stage_ != Stage::Idle; }
    uint32_t made() const { return made_; } // pictures written so far

private:
    enum class Stage : uint8_t { Idle, Loading, Settling, Measuring, Shooting };

    bool begin(const PictureEngine& engine);
    void place_subject(const PictureEngine& engine, app::PreviewScene& scene);
    bool ready(const PictureEngine& engine, const app::PreviewScene& scene) const;
    void aim(app::PreviewScene& scene);
    void shoot(bool measuring);
    void finish(const PictureEngine& engine, Thumbnails& thumbnails, bool made);
    void clear_subject(const PictureEngine& engine);
    bool collect_sources(const PictureEngine& engine);

    core::Allocator* allocator_;
    std::optional<app::PreviewScene> scene_;
    Stage stage_ = Stage::Idle;
    Thumbnails::RenderRequest request_{};
    asset::AssetHandle held_;        // the effect or the prefab, held while it's drawn
    flecs::entity_t subject_ = 0;    // the effect's entity, or what a prefab's parts stand round
    flecs::entity_t first_part_ = 0; // a prefab's parts
    PictureSubject bounds_;
    PictureFraming framing_{};
    render::SceneLighting lighting_{};
    core::Array<asset::AssetHandle> meshes_; // what the subject draws: all on the GPU before it's shot
    uint32_t effects_ = 0;                   // and plays
    uint32_t emitters_ = 0;
    uint32_t frames_ = 0;       // in this stage
    uint32_t measures_ = 0;     // pictures measured for this one
    uint32_t idle_frames_ = 0;  // with nothing asked for
    uint32_t flush_frames_ = 0; // still to draw, so the renderer hears what left the scene
    uint64_t next_tag_ = 1;
    uint64_t waiting_tag_ = 0; // the picture it waits for
    uint64_t shot_tag_ = 0;    // asked of this frame's draw
    bool draw_now_ = false;
    bool settled_ = false; // everything it shows was there when it was shot
    core::Array<std::byte> picture_;
    core::Array<std::byte> small_;
    core::Array<PictureSource> drawn_from_;
    uint32_t made_ = 0;
};

} // namespace de::editor
