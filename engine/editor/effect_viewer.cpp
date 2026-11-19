#include "editor/effect_viewer.h"

#include "asset/effect_format.h"
#include "asset/format.h"
#include "core/log.h"
#include "core/profile.h"
#include "editor/icons.h"
#include "editor/source_files.h"
#include "editor/theme.h"
#include "editor/widgets.h"
#include "platform/vfs.h"
#include "render/scene/particles.h"
#include "world/transform.h"

#include <glm/gtc/constants.hpp>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace de::editor {
namespace {

// What the effect fills of the picture when it's framed: its emitters' reach, with air round it.
constexpr float kFill = 0.9f;
constexpr double kRefreshS = 0.25; // the numbers are read, not watched
constexpr float kSpeeds[] = {0.1f, 0.25f, 0.5f, 1.0f, 2.0f};
// An emitter far behind its clock (the time dragged back) catches up this many steps a frame: 17 seconds' worth, so
// what the time bar shows follows the hand.
constexpr uint32_t kCatchUpSteps = 1024;
constexpr uint32_t kGridLines = 21;
constexpr uint32_t kGridMiddle = kGridLines / 2; // the line through the effect's own place
constexpr uint32_t kCircleSegments = 48;
constexpr uint32_t kGridColor = 0xff4a4a4aU;
constexpr uint32_t kAxisColor = 0xff6e6e6eU;
constexpr uint32_t kBoundsColor = 0xff2f9fe0U; // amber, as seen: red in the low byte

enum class Glyph : uint8_t { ToStart, StepBack, Play, Pause, StepForward };

// A square button of the time bar, its sign drawn here so the five are one set: the accent while on, a step lighter
// under the mouse.
bool transport_button(const char* id, Glyph glyph, const char* tip, bool on = false) {
    const float extent = ImGui::GetFrameHeight();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(extent, extent));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (on || hovered || held) {
        const ThemeColor background = on ? (hovered ? ThemeColor::AccentHovered : ThemeColor::Accent)
                                         : (held ? ThemeColor::ControlActive : ThemeColor::ControlHovered);
        draw->AddRectFilled(min, ImVec2(min.x + extent, min.y + extent), color_u32(background),
                            px(theme().metrics.control_rounding));
    }
    const ImU32 ink = color_u32(on || hovered ? ThemeColor::TextEmphasis : ThemeColor::Text);
    const float inset = std::floor(extent * 0.30f);
    const float left = min.x + inset;
    const float right = min.x + extent - inset;
    const float top = min.y + inset;
    const float bottom = min.y + extent - inset;
    const float middle = (top + bottom) * 0.5f;
    const float bar = std::max(2.0f, std::floor((right - left) * 0.22f));
    const auto forward = [&](float from, float to) {
        draw->AddTriangleFilled(ImVec2(from, top), ImVec2(to, middle), ImVec2(from, bottom), ink);
    };
    const auto backward = [&](float from, float to) {
        draw->AddTriangleFilled(ImVec2(to, top), ImVec2(to, bottom), ImVec2(from, middle), ink);
    };
    switch (glyph) {
    case Glyph::ToStart:
        draw->AddRectFilled(ImVec2(left, top), ImVec2(left + bar, bottom), ink);
        backward(left + bar, right);
        break;
    case Glyph::StepBack:
        backward(left, right - bar - 1.0f);
        draw->AddRectFilled(ImVec2(right - bar, top), ImVec2(right, bottom), ink);
        break;
    case Glyph::Play:
        forward(left, right);
        break;
    case Glyph::Pause:
        draw->AddRectFilled(ImVec2(left, top), ImVec2(left + (bar * 1.4f), bottom), ink);
        draw->AddRectFilled(ImVec2(right - (bar * 1.4f), top), ImVec2(right, bottom), ink);
        break;
    case Glyph::StepForward:
        draw->AddRectFilled(ImVec2(left, top), ImVec2(left + bar, bottom), ink);
        forward(left + bar + 1.0f, right);
        break;
    }
    if (hovered) {
        tooltip("%s", tip);
    }
    return pressed;
}

// A number in its own fixed place: right-aligned in a column wide enough for the widest it will be.
void number_cell(const char* text, float width) {
    const float text_width = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, width - text_width));
    ImGui::TextUnformatted(text);
}

void push_line(core::Array<world::DebugLine>& lines, Vec3 from, Vec3 to, uint32_t color, bool through = true) {
    lines.push({.from = DVec3(from), .to = DVec3(to), .color = color, .through = through});
}

} // namespace

void EffectPlayback::advance(double frame_s) {
    if (!playing || frame_s <= 0.0) {
        return;
    }
    age_s += frame_s * static_cast<double>(speed);
    if (loop && length_s > 0.0 && age_s >= length_s) {
        age_s = std::fmod(age_s, length_s);
    }
}

void EffectPlayback::step(int steps) {
    playing = false;
    const double at = age_s * asset::kEffectStepsPerS;
    const double whole = std::floor(at + 1e-6);
    const bool between = at - whole > 1e-6;
    double target = whole + static_cast<double>(steps);
    if (steps < 0 && between) {
        target += 1.0; // from between two steps, back is to the one just passed
    }
    age_s = std::max(target, 0.0) / asset::kEffectStepsPerS;
}

void EffectPlayback::seek(double age) {
    age_s = std::max(age, 0.0);
}

uint64_t EffectPlayback::step_number() const {
    return static_cast<uint64_t>(std::floor((age_s * asset::kEffectStepsPerS) + 1e-6));
}

EffectViewer::EffectViewer(core::Allocator& allocator) : bounds_(allocator), lines_(allocator), rows_(allocator) {}

bool EffectViewer::open(const PictureEngine& engine, std::string_view path, std::span<char> why) {
    const auto refuse = [&](const char* reason) {
        if (!why.empty()) {
            static_cast<void>(
                std::snprintf(why.data(), why.size(), "%.*s %s", static_cast<int>(path.size()), path.data(), reason));
        }
        return false;
    };
    asset::Guid guid;
    asset::AssetId id;
    if (file_kind(path) != "Effect") {
        return refuse("isn't an effect (.vfx)");
    }
    if (!source_asset_id(*engine.vfs, path, guid, id) ||
        !engine.vfs->exists(asset::cooked_path(id, asset::AssetType::Effect).c_str())) {
        return refuse("hasn't been cooked: cook the project first");
    }
    if (!scene_) {
        core::Result<app::PreviewScene> scene = app::PreviewScene::create({.device = engine.device,
                                                                           .shaders = engine.shaders,
                                                                           .assets = engine.assets,
                                                                           .gpu_assets = engine.gpu_assets,
                                                                           .jobs = engine.jobs,
                                                                           .particle_catch_up_steps = kCatchUpSteps});
        if (!scene) {
            DE_LOG_ERROR("editor", "no scene to view an effect in: %s", scene.error().message);
            return refuse("can't be shown: no scene could be made for it");
        }
        scene_.emplace(std::move(*scene));
    } else {
        const flecs::world& ecs = scene_->world().ecs();
        if (entity_ != 0 && ecs.is_alive(entity_)) {
            ecs.entity(entity_).destruct();
        }
        if (held_.is_valid()) {
            engine.assets->release(held_);
        }
    }
    held_ = engine.assets->load({.id = id, .type = asset::AssetType::Effect});
    static_cast<void>(path_.assign(path));
    entity_ = 0;
    revision_ = 0;
    playback_ = EffectPlayback{};
    bar_span_s_ = 0.0;
    hidden_ = 0;
    rows_.clear();
    bounds_.clear();
    window_open_ = true;
    focus_ = true;
    refresh_in_s_ = 0.0;
    return true;
}

void EffectViewer::close(const PictureEngine& engine) {
    if (scene_) {
        const flecs::world& ecs = scene_->world().ecs();
        if (entity_ != 0 && ecs.is_alive(entity_)) {
            ecs.entity(entity_).destruct();
        }
    }
    entity_ = 0;
    if (held_.is_valid()) {
        engine.assets->release(held_);
    }
    held_ = {};
    scene_.reset();
    path_ = {};
    rows_.clear();
    window_open_ = false;
    visible_ = false;
}

void EffectViewer::place(const PictureEngine& /*engine*/) {
    if (!scene_) {
        return;
    }
    const flecs::entity entity = scene_->world().ecs().entity();
    entity.set(world::EffectInstance{.effect = held_, .clock = playback_.clock()});
    world::place(entity, world::Transform{});
    entity_ = entity.id();
}

// What the effect is made of, and how far it reaches, from its file as it is now.
void EffectViewer::measure(const PictureEngine& engine) {
    rows_.clear();
    bounds_.clear();
    const auto* effect = engine.assets->get<asset::EffectAsset>(held_);
    if (effect == nullptr) {
        return;
    }
    float reach_m = 0.0f;
    for (const asset::EffectEmitter& emitter : effect->emitters.get(effect)) {
        EmitterRow row;
        static_cast<void>(row.name.assign(effect->name(effect, emitter).substr(0, row.name.capacity())));
        row.rate_per_s = emitter.rate_per_s;
        row.lifetime_s[0] = emitter.lifetime_s[0];
        row.lifetime_s[1] = emitter.lifetime_s[1];
        row.capacity = emitter.capacity;
        rows_.push(row);
        reach_m = std::max(reach_m, emitter.reach_m);
    }
    bounds_.spheres.push(Vec4(0.0f, 0.0f, 0.0f, std::max(reach_m, 0.01f)));
    bounds_.nearest_draw_distance_m = effect->draw_distance_m;
}

void EffectViewer::frame() {
    camera_.frame(bounds_, kFill);
}

void EffectViewer::aim() {
    if (!scene_) {
        return;
    }
    framing_ = camera_.framing(bounds_);
    scene_->set_camera(camera_.camera(framing_));
}

void EffectViewer::refresh(const PictureEngine& /*engine*/) {
    shown_step_ms_ = step_ms_;
    shown_draw_ms_ = draw_ms_;
    if (!scene_) {
        return;
    }
    const render::ParticleStats particles = scene_->stats().particles;
    shown_particles_ = particles.particles;
    shown_blocks_ = particles.blocks;
    const render::ParticleSystem* system = scene_->renderer().particles();
    const flecs::world& ecs = scene_->world().ecs();
    const auto* slot =
        entity_ != 0 && ecs.is_alive(entity_) ? ecs.entity(entity_).try_get<world::EffectSlot>() : nullptr;
    for (uint32_t e = 0; e < rows_.size(); ++e) {
        render::ParticleEmitterStats stats;
        const bool held =
            system != nullptr && slot != nullptr && system->emitter_stats(slot->slot, e, stats) && stats.held;
        rows_[e].held = held;
        rows_[e].alive = held ? stats.alive : 0;
    }
}

void EffectViewer::update(const PictureEngine& engine, float frame_s, const render::SceneLighting* world_light) {
    DE_PROFILE_ZONE("EffectViewer::update");
    if (!scene_) {
        return;
    }
    if (!window_open_) {
        close(engine);
        return;
    }
    app::PreviewScene& scene = *scene_;
    // Loaded, or cooked again: what it's made of is read anew, and it starts over if asked to.
    const uint32_t revision = engine.assets->revision(held_);
    if (revision != revision_ && revision != 0) {
        const bool first = revision_ == 0;
        revision_ = revision;
        if (entity_ == 0) {
            place(engine);
        }
        measure(engine);
        if (first) {
            frame();
            // A time bar long enough to see its longest-lived particles come and go twice.
            float longest_s = 0.0f;
            for (const EmitterRow& row : rows_) {
                longest_s = std::max(longest_s, row.lifetime_s[1]);
            }
            playback_.length_s = std::clamp(std::ceil(static_cast<double>(longest_s) * 2.0), 2.0, 30.0);
        } else if (restart_on_edit_) {
            playback_.restart();
        }
        refresh_in_s_ = 0.0;
    }
    playback_.advance(static_cast<double>(frame_s));
    const flecs::world& ecs = scene.world().ecs();
    if (entity_ != 0 && ecs.is_alive(entity_)) {
        auto& instance = ecs.entity(entity_).get_mut<world::EffectInstance>();
        instance.clock = playback_.clock();
        instance.hidden_emitters = hidden_;
    }
    aim();
    // The light: a studio's, the same from wherever the effect is looked at; or the open world's, without its fog.
    if (light_ == Light::World && world_light != nullptr) {
        lighting_ = *world_light;
        lighting_.fog = render::SceneFog{};
    } else {
        lighting_ = studio_lighting(picture_axes(), framing_.distance_m);
        static constexpr float kTop[3][3] = {{0.012f, 0.013f, 0.016f}, {0.16f, 0.16f, 0.17f}, {0.62f, 0.62f, 0.64f}};
        static constexpr float kBottom[3][3] = {{0.040f, 0.043f, 0.050f}, {0.24f, 0.24f, 0.25f}, {0.80f, 0.80f, 0.82f}};
        lighting_.backdrop.enabled = true;
        const auto backdrop = static_cast<size_t>(backdrop_);
        for (int channel = 0; channel < 3; ++channel) {
            lighting_.backdrop.top[channel] = kTop[backdrop][channel];
            lighting_.backdrop.bottom[channel] = kBottom[backdrop][channel];
        }
    }
    lighting_.wind.speed_mps = wind_mps_;
    // The ground's grid, a step that suits how much is in view, and how far each emitter may reach.
    lines_.clear();
    if (grid_) {
        const float across = 2.0f * framing_.distance_m * std::tan(framing_.vertical_fov_rad * 0.5f);
        const float cell = std::pow(10.0f, std::floor(std::log10(std::max(across, 1e-4f) / 4.0f)));
        const float half = cell * static_cast<float>(kGridMiddle);
        for (uint32_t i = 0; i < kGridLines; ++i) {
            const float at = (static_cast<float>(i) * cell) - half;
            const uint32_t color = i == kGridMiddle ? kAxisColor : kGridColor;
            push_line(lines_, Vec3(at, 0.0f, -half), Vec3(at, 0.0f, half), color, false);
            push_line(lines_, Vec3(-half, 0.0f, at), Vec3(half, 0.0f, at), color, false);
        }
    }
    if (show_bounds_) {
        for (const Vec4& sphere : bounds_.spheres) {
            const Vec3 center(sphere);
            for (uint32_t s = 0; s < kCircleSegments; ++s) {
                const float a = glm::two_pi<float>() * static_cast<float>(s) / static_cast<float>(kCircleSegments);
                const float b = glm::two_pi<float>() * static_cast<float>(s + 1) / static_cast<float>(kCircleSegments);
                const Vec2 from(std::cos(a) * sphere.w, std::sin(a) * sphere.w);
                const Vec2 to(std::cos(b) * sphere.w, std::sin(b) * sphere.w);
                push_line(lines_, center + Vec3(from.x, from.y, 0.0f), center + Vec3(to.x, to.y, 0.0f), kBoundsColor);
                push_line(lines_, center + Vec3(from.x, 0.0f, from.y), center + Vec3(to.x, 0.0f, to.y), kBoundsColor);
                push_line(lines_, center + Vec3(0.0f, from.x, from.y), center + Vec3(0.0f, to.x, to.y), kBoundsColor);
            }
        }
    }
#if DE_BUILD_DEV
    scene.renderer().set_debug_lines(lines_.span());
#endif
    scene.advance(static_cast<double>(frame_s));

    // What its particles cost the GPU in the frame the GPU last finished, eased so it can be read.
    float step_ms = 0.0f;
    float draw_ms = 0.0f;
    for (const render::PassGpuTime& pass : scene.gpu_times()) {
        if (pass.name.view() == "particles step" || pass.name.view() == "particles clear") {
            step_ms += pass.ms;
        } else if (pass.name.view() == "particles") {
            draw_ms += pass.ms;
        }
    }
    step_ms_ += (step_ms - step_ms_) * 0.1f;
    draw_ms_ += (draw_ms - draw_ms_) * 0.1f;
    refresh_in_s_ -= static_cast<double>(frame_s);
    if (refresh_in_s_ <= 0.0) {
        refresh(engine);
        refresh_in_s_ = kRefreshS;
    }
}

void EffectViewer::show(uint32_t width, uint32_t height) {
    if (!scene_) {
        return;
    }
    scene_->set_size(width, height);
    visible_ = scene_->texture_id() != 0;
}

void EffectViewer::draw() {
    if (!scene_ || !visible_) {
        return;
    }
    visible_ = false;
    scene_->draw(lighting_);
}

float EffectViewer::view_bar(const IconSet& /*icons*/, float width) {
    const float near = px(4.0f);
    const float apart = px(12.0f);
    FlowRow row(px(6.0f), width - px(6.0f));
    const auto chip = [&row](const char* label, bool on, const char* tip, float gap) {
        row.place(chip_width(label), gap);
        return toggle_chip(label, on, tip);
    };
    if (chip("Grid", grid_, "The ground's grid", 0.0f)) {
        grid_ = !grid_;
    }
    if (chip("Reach", show_bounds_, "How far each emitter's particles may get: what it's culled by", near)) {
        show_bounds_ = !show_bounds_;
    }
    static constexpr const char* kLights[] = {"Studio light", "World light"};
    row.place(px(120.0f), apart);
    if (begin_combo("##light", kLights[static_cast<size_t>(light_)], px(120.0f))) {
        for (size_t i = 0; i < 2; ++i) {
            if (ImGui::Selectable(kLights[i], static_cast<size_t>(light_) == i)) {
                light_ = static_cast<Light>(i);
            }
        }
        end_combo();
    }
    static constexpr const char* kBackdrops[] = {"Dark", "Grey", "Light"};
    row.place(px(76.0f), near);
    ImGui::BeginDisabled(light_ == Light::World);
    if (begin_combo("##backdrop", kBackdrops[static_cast<size_t>(backdrop_)], px(76.0f))) {
        for (size_t i = 0; i < 3; ++i) {
            if (ImGui::Selectable(kBackdrops[i], static_cast<size_t>(backdrop_) == i)) {
                backdrop_ = static_cast<Backdrop>(i);
            }
        }
        end_combo();
    }
    ImGui::EndDisabled();
    // The wind: its word and its slider together, never a line apart.
    const float word = ImGui::CalcTextSize("Wind").x;
    row.place(word + px(6.0f) + px(90.0f), apart);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Wind");
    ImGui::SameLine(0.0f, px(6.0f));
    ImGui::SetNextItemWidth(px(90.0f));
    ImGui::SliderFloat("##wind", &wind_mps_, 0.0f, 20.0f, "%.1f m/s");
    if (chip("Restart on edit", restart_on_edit_,
             "When the effect is cooked again: from its start, or carrying on at the age it has", apart)) {
        restart_on_edit_ = !restart_on_edit_;
    }
    if (chip("Frame", false, "Look at the whole effect again (F)", apart)) {
        frame();
    }
    return row.bottom();
}

void EffectViewer::picture(const IconSet& /*icons*/, ImVec2 size) {
    if (!scene_ || size.x < 1.0f || size.y < 1.0f) {
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 scale = io.DisplayFramebufferScale;
    show(static_cast<uint32_t>(std::lround(size.x * scale.x)), static_cast<uint32_t>(std::lround(size.y * scale.y)));
    const ImVec2 corner = ImGui::GetCursorScreenPos();
    const ImVec2 far_corner(corner.x + size.x, corner.y + size.y);
    ImGui::InvisibleButton("##picture", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemActive()) {
        camera_.drag(framing_, size, io.MouseDelta, ImGui::IsMouseDown(ImGuiMouseButton_Middle) || io.KeyShift);
    }
    if (hovered && io.MouseWheel != 0.0f) {
        camera_.zoom(io.MouseWheel);
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        frame();
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (const uint64_t texture = scene_ ? scene_->texture_id() : 0; texture != 0) {
        draw->AddImage(ImTextureRef(static_cast<ImTextureID>(texture)), corner, far_corner);
    }
    if (revision_ == 0) {
        draw_clipped_label(corner, far_corner, "Loading the effect...", color_u32(ThemeColor::TextSecondary), true);
    }
}

void EffectViewer::time_bar(const IconSet& /*icons*/, float width) {
    const float pad = px(6.0f);
    const float gap = px(2.0f);
    ImGui::SetCursorPosX(pad);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + pad);
    if (transport_button("##start", Glyph::ToStart, "To the start (Home)")) {
        playback_.restart();
    }
    ImGui::SameLine(0.0f, gap);
    if (transport_button("##back", Glyph::StepBack, "Back a step of the simulation, 1/60 s (Left)")) {
        playback_.step(-1);
    }
    ImGui::SameLine(0.0f, gap);
    if (transport_button("##play", playback_.playing ? Glyph::Pause : Glyph::Play,
                         playback_.playing ? "Hold (Space)" : "Play (Space)")) {
        playback_.playing = !playback_.playing;
    }
    ImGui::SameLine(0.0f, gap);
    if (transport_button("##forward", Glyph::StepForward, "On a step of the simulation, 1/60 s (Right)")) {
        playback_.step(1);
    }

    // To the right of the time: the readout in a place of its own, the speed, the loop and its length.
    char time[48];
    static_cast<void>(std::snprintf(time, sizeof(time), "%.3f s", playback_.age_s));
    char step[32];
    static_cast<void>(
        std::snprintf(step, sizeof(step), "step %llu", static_cast<unsigned long long>(playback_.step_number())));
    const float time_width = ImGui::CalcTextSize("0000.000 s").x;
    const float step_width = ImGui::CalcTextSize("step 0000000").x;
    const float speed_width = px(64.0f);
    const float loop_width = (2.0f * px(8.0f)) + ImGui::CalcTextSize("Loop").x;
    const float length_width = px(64.0f);
    const float right_width = time_width + step_width + speed_width + loop_width + length_width + (px(10.0f) * 5.0f);

    // The time: a bar the length the effect is looked at over (or as long as it has played, when that's longer and
    // it isn't looping), filled as far as its age, dragged to set it.
    bar_span_s_ = playback_.loop ? playback_.length_s : std::max({bar_span_s_, playback_.length_s, playback_.age_s});
    ImGui::SameLine(0.0f, px(10.0f));
    const float bar_width = std::max(px(60.0f), width - ImGui::GetCursorPosX() - right_width - pad);
    const float height = ImGui::GetFrameHeight();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##time", ImVec2(bar_width, height));
    const bool active = ImGui::IsItemActive();
    if (active && !dragging_time_) {
        dragging_time_ = true;
        was_playing_ = playback_.playing;
        playback_.playing = false;
    }
    if (active) {
        const float share = std::clamp((ImGui::GetIO().MousePos.x - min.x) / bar_width, 0.0f, 1.0f);
        playback_.seek(static_cast<double>(share) * bar_span_s_);
    } else if (dragging_time_) {
        dragging_time_ = false;
        playback_.playing = was_playing_;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float track_top = min.y + std::floor(height * 0.36f);
    const float track_bottom = min.y + height - std::floor(height * 0.36f);
    const float rounding = px(2.0f);
    draw->AddRectFilled(ImVec2(min.x, track_top), ImVec2(min.x + bar_width, track_bottom),
                        color_u32(ThemeColor::Recessed), rounding);
    const float share =
        bar_span_s_ > 0.0 ? static_cast<float>(std::clamp(playback_.age_s / bar_span_s_, 0.0, 1.0)) : 0.0f;
    const float at = min.x + (bar_width * share);
    draw->AddRectFilled(ImVec2(min.x, track_top), ImVec2(at, track_bottom), color_u32(ThemeColor::Accent), rounding);
    // A tick a second, while there's room for them.
    const double seconds = bar_span_s_;
    if (seconds <= 60.0 && bar_width / static_cast<float>(std::max(seconds, 1.0)) >= px(8.0f)) {
        for (int second = 1; static_cast<double>(second) < seconds; ++second) {
            const float x = min.x + (bar_width * static_cast<float>(static_cast<double>(second) / seconds));
            draw->AddLine(ImVec2(x, track_bottom + px(1.0f)), ImVec2(x, track_bottom + px(4.0f)),
                          color_u32(ThemeColor::TextDisabled));
        }
    }
    const bool hot = active || ImGui::IsItemHovered();
    draw->AddRectFilled(ImVec2(at - px(2.0f), min.y + px(2.0f)), ImVec2(at + px(2.0f), min.y + height - px(2.0f)),
                        color_u32(hot ? ThemeColor::TextEmphasis : ThemeColor::Text), px(1.0f));

    ImGui::SameLine(0.0f, px(10.0f));
    ImGui::AlignTextToFramePadding();
    number_cell(time, time_width);
    ImGui::SameLine(0.0f, px(10.0f));
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, color(ThemeColor::TextSecondary));
    number_cell(step, step_width);
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, px(10.0f));
    char speed[16];
    static_cast<void>(std::snprintf(speed, sizeof(speed), "%.2gx", static_cast<double>(playback_.speed)));
    if (begin_combo("##speed", speed, speed_width)) {
        for (const float value : kSpeeds) {
            char label[16];
            static_cast<void>(std::snprintf(label, sizeof(label), "%.2gx", static_cast<double>(value)));
            if (ImGui::Selectable(label, playback_.speed == value)) {
                playback_.speed = value;
            }
        }
        end_combo();
    }
    ImGui::SameLine(0.0f, px(10.0f));
    if (toggle_chip("Loop", playback_.loop, "Start over when the time reaches the bar's end")) {
        playback_.loop = !playback_.loop;
    }
    ImGui::SameLine(0.0f, px(4.0f));
    ImGui::SetNextItemWidth(length_width);
    auto length = static_cast<float>(playback_.length_s);
    if (ImGui::DragFloat("##length", &length, 0.1f, 0.5f, 600.0f, "%.1f s")) {
        playback_.length_s = static_cast<double>(std::clamp(length, 0.5f, 600.0f));
        bar_span_s_ = std::max(playback_.length_s, playback_.age_s); // the bar as short as it may be again
    }
    if (ImGui::IsItemHovered()) {
        tooltip("How long the time bar is at least, and where a loop turns");
    }
}

void EffectViewer::side(float width) {
    const float pad = px(8.0f);
    const float font = ImGui::GetFontSize();
    const std::string_view path = path_.view();
    const std::string_view file = path.substr(path.rfind('/') + 1);
    char text[128];

    section_header("Effect");
    ImGui::Indent(pad);
    static_cast<void>(std::snprintf(text, sizeof(text), "%.*s", static_cast<int>(file.size()), file.data()));
    ImGui::TextUnformatted(text);
    if (ImGui::BeginTable("##totals", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthFixed, font * 7.0f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, font * 6.0f);
        const auto row = [&](const char* what, const char* value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            secondary_text(what);
            ImGui::TableNextColumn();
            number_cell(value, font * 6.0f);
        };
        static_cast<void>(std::snprintf(text, sizeof(text), "%u", shown_particles_));
        row("Particles", text);
        static_cast<void>(std::snprintf(text, sizeof(text), "%u", shown_blocks_));
        row("Pool blocks", text);
        static_cast<void>(std::snprintf(text, sizeof(text), "%.3f ms", static_cast<double>(shown_step_ms_)));
        row("GPU: step", text);
        static_cast<void>(std::snprintf(text, sizeof(text), "%.3f ms", static_cast<double>(shown_draw_ms_)));
        row("GPU: draw", text);
        ImGui::EndTable();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, color(ThemeColor::TextDisabled));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - (2.0f * pad));
    ImGui::TextUnformatted("The cost of what's shown, at this size. Hide the others to read one emitter's draw.");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Unindent(pad);
    ImGui::Dummy(ImVec2(0.0f, px(6.0f)));

    section_header("Emitters");
    if (rows_.empty()) {
        ImGui::Indent(pad);
        secondary_text(revision_ == 0 ? "Loading..." : "None");
        ImGui::Unindent(pad);
        return;
    }
    if (ImGui::BeginTable("##emitters", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("show", ImGuiTableColumnFlags_WidthFixed, font * 2.6f);
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("rate", ImGuiTableColumnFlags_WidthFixed, font * 3.6f);
        ImGui::TableSetupColumn("alive", ImGuiTableColumnFlags_WidthFixed, font * 5.2f);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        secondary_text("Emitter");
        ImGui::TableSetColumnIndex(2);
        secondary_text("per s");
        ImGui::TableSetColumnIndex(3);
        secondary_text("alive / most");
        for (uint32_t e = 0; e < rows_.size(); ++e) {
            const EmitterRow& row = rows_[e];
            const uint32_t bit = 1U << e;
            const bool shown = (hidden_ & bit) == 0;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(e));
            if (toggle_chip(shown ? "On" : "Off", shown,
                            "Shown or left out of the picture (it goes on being simulated). With Shift: only this "
                            "one, or all of them again")) {
                if (ImGui::GetIO().KeyShift) {
                    const uint32_t others = ((1U << rows_.size()) - 1U) & ~bit;
                    hidden_ = hidden_ == others ? 0U : others;
                } else {
                    hidden_ ^= bit;
                }
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(row.name.c_str());
            if (ImGui::IsItemHovered()) {
                tooltip("Each lives %.2f to %.2f s", static_cast<double>(row.lifetime_s[0]),
                        static_cast<double>(row.lifetime_s[1]));
            }
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            static_cast<void>(std::snprintf(text, sizeof(text), "%.4g", static_cast<double>(row.rate_per_s)));
            number_cell(text, font * 3.6f);
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (row.held) {
                static_cast<void>(std::snprintf(text, sizeof(text), "%u / %u", row.alive, row.capacity));
            } else {
                static_cast<void>(std::snprintf(text, sizeof(text), "- / %u", row.capacity));
            }
            number_cell(text, font * 5.2f);
        }
        ImGui::EndTable();
    }
}

void EffectViewer::ui(const PictureEngine& /*engine*/, const IconSet& icons, const char* dock_beside) {
    if (!scene_) {
        return;
    }
    // A tab beside the Playground the first time; wherever it was left after that.
    if (const ImGuiWindow* beside = dock_beside != nullptr ? ImGui::FindWindowByName(dock_beside) : nullptr;
        beside != nullptr && beside->DockId != 0) {
        ImGui::SetNextWindowDockID(beside->DockId, ImGuiCond_FirstUseEver);
    }
    ImGui::SetNextWindowSize(ImVec2(px(960.0f), px(600.0f)), ImGuiCond_FirstUseEver);
    // Brought forward once the panels exist: asked on the first frames (a flag at startup), the layout that's read
    // back then would put the tab it left in front again.
    if (focus_ && ImGui::GetFrameCount() > 2) {
        ImGui::SetNextWindowFocus();
        focus_ = false;
    }
    bool open = true;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool shown = ImGui::Begin(kWindow, &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (shown) {
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput) {
            if (ImGui::Shortcut(ImGuiKey_Space)) {
                playback_.playing = !playback_.playing;
            }
            if (ImGui::Shortcut(ImGuiKey_RightArrow, ImGuiInputFlags_Repeat)) {
                playback_.step(1);
            }
            if (ImGui::Shortcut(ImGuiKey_LeftArrow, ImGuiInputFlags_Repeat)) {
                playback_.step(-1);
            }
            if (ImGui::Shortcut(ImGuiKey_Home)) {
                playback_.restart();
            }
            if (ImGui::Shortcut(ImGuiKey_F)) {
                frame();
            }
        }
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float side_width = std::min(px(300.0f), avail.x * 0.42f);
        const float view_width = avail.x - side_width;
        const float bar_height = ImGui::GetFrameHeight() + (2.0f * px(6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        if (ImGui::BeginChild("##view", ImVec2(view_width, avail.y), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            // The bar over the picture takes as many lines as it needs at this width.
            const float top_height = view_bar(icons, view_width);
            ImGui::SetCursorPos(ImVec2(0.0f, top_height));
            picture(icons, ImVec2(view_width, std::max(1.0f, avail.y - top_height - bar_height)));
            ImGui::SetCursorPos(ImVec2(0.0f, avail.y - bar_height));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(4.0f), px(4.0f)));
            time_bar(icons, view_width);
            ImGui::PopStyleVar();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, color(ThemeColor::Surface));
        if (ImGui::BeginChild("##side", ImVec2(side_width, avail.y))) {
            side(side_width);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    ImGui::End();
    if (!open) {
        window_open_ = false; // closed by the next update, with what it holds
    }
}

} // namespace de::editor
