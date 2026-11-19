#pragma once

#include "app/preview_scene.h"
#include "asset/registry.h"
#include "core/allocator.h"
#include "core/array.h"
#include "core/fixed_string.h"
#include "core/math.h"
#include "editor/orbit_camera.h"
#include "editor/preview_pictures.h"
#include "render/scene/scene_renderer.h"
#include "world/render_extract.h"

#include <flecs.h>
#include <imgui.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace de::editor {

class IconSet;

// Where an effect is in its own time, as the viewer keeps it (docs/design.md, VFX viewer). The viewer owns the
// effect's clock outright: every frame it says how old the effect is, and the particles are what that age gives
// (world::EffectClock), so playing, holding, stepping and dragging the time are all the same thing: another age.
struct EffectPlayback {
    double age_s = 0.0;
    bool playing = true;
    float speed = 1.0f;    // of real time, while playing
    bool loop = false;     // back to the start when the age reaches the length
    double length_s = 5.0; // what the time bar spans, and where a loop turns

    // A frame of real time goes by.
    void advance(double frame_s);
    // To the simulation's next step (or the one `steps` on; back for less than 0), and held there. From between two
    // steps, forward goes to the next and back to the one just passed.
    void step(int steps);
    // To an age, as it is: playing goes on from there.
    void seek(double age);
    void restart() { seek(0.0); }
    // Which of the simulation's steps the age has reached.
    uint64_t step_number() const;
    // The effect's clock for this frame: at this age, whatever the world's time is.
    world::EffectClock clock() const { return {.since_s = 0.0, .age_s = age_s, .rate = 0.0f}; }
};

// What the viewer shows of one emitter, as last refreshed.
struct EmitterRow {
    core::FixedString<63> name;
    float rate_per_s = 0.0f;
    float lifetime_s[2] = {};
    uint32_t capacity = 0;
    uint32_t alive = 0; // about: what its births and lifetimes come to at this age
    bool held = false;  // it has its place in the pool
};

// The effect viewer (docs/design.md, VFX viewer): one effect alone in a preview scene, by the engine's renderer,
// under a clock the viewer owns. A window of the editor's, beside the Playground by default: the picture, with a
// camera that turns about the effect; a bar under it to play, hold, step either way, drag the time, change the
// speed and loop; and beside it what the effect is made of, each emitter with how many particles it has, shown or
// hidden by itself, and what the effect costs the GPU as it's seen. A cooked change to the effect shows at once,
// from the start or carrying on.
class EffectViewer {
public:
    static constexpr const char* kWindow = "Effect";

    explicit EffectViewer(core::Allocator& allocator);
    ~EffectViewer() = default;
    EffectViewer(const EffectViewer&) = delete;
    EffectViewer& operator=(const EffectViewer&) = delete;
    EffectViewer(EffectViewer&&) = delete;
    EffectViewer& operator=(EffectViewer&&) = delete;

    // Shows an effect's source ("src:/content/fx/flame.vfx") from its start, in place of what it showed, and brings
    // its window forward. False, saying why in `why`, if it isn't a cooked effect.
    bool open(const PictureEngine& engine, std::string_view path, std::span<char> why);
    void close(const PictureEngine& engine);
    bool is_open() const { return scene_.has_value(); }
    std::string_view path() const { return path_.view(); }

    EffectPlayback& playback() { return playback_; }
    // Emitters left out of the picture, a bit each (they go on being simulated).
    void set_hidden(uint32_t emitters) { hidden_ = emitters; }
    uint32_t hidden() const { return hidden_; }
    // Looks at the whole effect again, from where the viewer first did.
    void frame();

    // Each frame it's open, before the frame draws: the effect's age, what changed in its file, its scene.
    // world_light: the open world's, for looking at the effect under it; null: none to offer.
    void update(const PictureEngine& engine, float frame_s, const render::SceneLighting* world_light);
    // Its picture is on screen this frame, this many pixels: what its window says, and a test in its place.
    void show(uint32_t width, uint32_t height);
    // From Application::draw: its scene's passes, while its picture shows.
    void draw();
    // Its scene, to look into; null while nothing is open.
    app::PreviewScene* scene() { return scene_ ? &*scene_ : nullptr; }
    // Its window. dock_beside: the window it joins as a tab the first time it's shown (the Playground).
    void ui(const PictureEngine& engine, const IconSet& icons, const char* dock_beside);
    // Lets go of its scene and what it holds: before the engine goes.
    void shutdown(const PictureEngine& engine) { close(engine); }

    // What it last refreshed, for tests and the console.
    std::span<const EmitterRow> emitters() const { return rows_.span(); }
    float step_ms() const { return shown_step_ms_; }
    float draw_ms() const { return shown_draw_ms_; }

private:
    enum class Light : uint8_t { Studio, World };
    enum class Backdrop : uint8_t { Dark, Grey, Light, Count };

    void place(const PictureEngine& engine);
    void measure(const PictureEngine& engine);
    void aim();
    void refresh(const PictureEngine& engine);
    void picture(const IconSet& icons, ImVec2 size);
    float view_bar(const IconSet& icons, float width); // how high it came to
    void time_bar(const IconSet& icons, float width);
    void side(float width);

    std::optional<app::PreviewScene> scene_;
    core::FixedString<255> path_;
    asset::AssetHandle held_;
    uint32_t revision_ = 0; // of the effect its rows and bounds were read from
    flecs::entity_t entity_ = 0;
    EffectPlayback playback_{};
    bool restart_on_edit_ = true;
    uint32_t hidden_ = 0;

    // The camera: turned about a point, as far as shows what's wanted (preview_pictures.h has the rules).
    PictureSubject bounds_;
    OrbitCamera camera_{};
    PictureFraming framing_{}; // where it stands this frame
    render::SceneLighting lighting_{};
    Light light_ = Light::Studio;
    Backdrop backdrop_ = Backdrop::Dark;
    bool grid_ = true;
    bool show_bounds_ = false;
    float wind_mps_ = 0.0f;
    core::Array<world::DebugLine> lines_;

    // Its window.
    bool window_open_ = false; // false once its cross is pressed: closed by the next update
    bool focus_ = false;       // brought forward when next shown
    bool visible_ = false;     // its picture is on screen this frame
    // What the time bar spans: its length, or as far as the effect has played when that's further and it isn't
    // looping. It only grows while the effect is open, so the bar doesn't rescale under a hand dragging back.
    double bar_span_s_ = 0.0;
    bool dragging_time_ = false;
    bool was_playing_ = false; // before the time was taken hold of

    // What it shows in numbers, refreshed a few times a second so they can be read.
    core::Array<EmitterRow> rows_;
    double refresh_in_s_ = 0.0;
    float step_ms_ = 0.0f; // smoothed, of the frames since
    float draw_ms_ = 0.0f;
    float shown_step_ms_ = 0.0f;
    float shown_draw_ms_ = 0.0f;
    uint32_t shown_particles_ = 0;
    uint32_t shown_blocks_ = 0;
};

} // namespace de::editor
