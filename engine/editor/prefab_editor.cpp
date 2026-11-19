#include "editor/prefab_editor.h"

#include "asset/effect_format.h"
#include "asset/format.h"
#include "asset/mesh_format.h"
#include "asset/prefab_format.h"
#include "core/hash.h"
#include "core/log.h"
#include "core/profile.h"
#include "editor/asset_browser.h"
#include "editor/cell_source.h"
#include "editor/file_ops.h"
#include "editor/source_files.h"
#include "editor/theme.h"
#include "editor/widgets.h"
#include "platform/file.h"
#include "platform/vfs.h"
#include "world/prefab.h"
#include "world/transform.h"

#include <glm/gtc/constants.hpp>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace de::editor {
namespace {

// What the prefab fills of the picture when it's framed: the rest is air round it, and room for the handles.
constexpr float kFill = 0.72f;
constexpr double kMeasureS = 0.25; // how often what it takes up is measured again (things load, effects reach)
constexpr size_t kMostSteps = 200; // of undo
constexpr float kMessageS = 6.0f;
constexpr uint32_t kGridLines = 21;
constexpr uint32_t kGridMiddle = kGridLines / 2; // the lines through the prefab's origin
constexpr uint32_t kGridColor = 0xff4a4a4aU;
constexpr uint32_t kAxisColor = 0xff6e6e6eU;
constexpr size_t kMostInnerListed = 64;
constexpr float kFarthestDropM = 1000.0f;

std::string_view folder_part(std::string_view path) {
    const size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string_view() : path.substr(0, slash);
}

std::string_view file_part(std::string_view path) {
    const size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// Whether what a variant leaves out covers a part's path: the part itself, or something it's nested in.
bool covers(std::string_view gone, std::string_view path) {
    return path == gone || (path.size() > gone.size() && path.starts_with(gone) && path[gone.size()] == '/');
}

bool addable_kind(std::string_view kind) {
    return kind == "Model" || kind == "Effect" || kind == "Prefab";
}

void push_line(core::Array<world::DebugLine>& lines, Vec3 from, Vec3 to, uint32_t color) {
    lines.push({.from = DVec3(from), .to = DVec3(to), .color = color, .through = false});
}

// Three numbers on a row, each with its axis' tint on its left edge. True if one changed; ended: one was let go of
// after a change.
bool vec3_fields(Vec3& value, float speed, float width, bool& ended) {
    constexpr ThemeColor kAxes[] = {ThemeColor::AxisX, ThemeColor::AxisY, ThemeColor::AxisZ};
    const float gap = px(3.0f);
    const float field = std::floor((width - (gap * 2.0f)) / 3.0f);
    bool changed = false;
    for (int i = 0; i < 3; ++i) {
        ImGui::PushID(i);
        if (i > 0) {
            ImGui::SameLine(0.0f, gap);
        }
        const ImVec2 min = ImGui::GetCursorScreenPos();
        ImGui::SetNextItemWidth(std::max(1.0f, field));
        float number = value[i];
        if (ImGui::DragFloat("##number", &number, speed, 0.0f, 0.0f, "%.3f") && std::isfinite(number)) {
            value[i] = number;
            changed = true;
        }
        ended = ended || ImGui::IsItemDeactivatedAfterEdit();
        ImGui::GetWindowDrawList()->AddRectFilled(min, ImVec2(min.x + px(2.0f), min.y + ImGui::GetFrameHeight()),
                                                  color_u32(kAxes[i]));
        ImGui::PopID();
    }
    return changed;
}

// Text in a property row's value column, centred on the row's frame line.
void value_text(const PropertyRow& row, const char* text, ThemeColor ink) {
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    draw_clipped_text(ImGui::GetWindowDrawList(),
                      ImVec2(cursor.x + ImGui::GetStyle().FramePadding.x,
                             std::floor(cursor.y + ((ImGui::GetFrameHeight() - ImGui::GetFontSize()) * 0.5f))),
                      row.value_end, text, color_u32(ink));
}

// A cross in a property row's last slot: takes what the row holds away. True when pressed.
bool remove_button(const PropertyRow& row, const char* tip) {
    const float slot = property_reset_slot();
    const float extent = std::min(slot, row.height);
    const ImVec2 min(row.min.x + row.width - slot + ((slot - extent) * 0.5f),
                     row.min.y + ((row.height - extent) * 0.5f));
    ImGui::SetCursorScreenPos(min);
    const bool pressed = ImGui::InvisibleButton("##remove", ImVec2(extent, extent));
    const bool hovered = ImGui::IsItemHovered();
    const ImU32 ink = color_u32(hovered ? ThemeColor::TextEmphasis : ThemeColor::TextSecondary);
    const float arm = std::max(2.0f, std::floor(extent * 0.17f));
    const ImVec2 middle(std::floor(min.x + (extent * 0.5f)), std::floor(min.y + (extent * 0.5f)));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddLine(ImVec2(middle.x - arm, middle.y - arm), ImVec2(middle.x + arm, middle.y + arm), ink, px(1.4f));
    draw->AddLine(ImVec2(middle.x - arm, middle.y + arm), ImVec2(middle.x + arm, middle.y - arm), ink, px(1.4f));
    if (hovered) {
        tooltip("%s", tip);
    }
    return pressed;
}

// What a part is made of, in a word or two.
const char* made_of(const PrefabDocPart& part) {
    if (!part.prefab.empty()) {
        return "Prefab";
    }
    static constexpr const char* kKinds[8] = {"Nothing", "Model",        "Effect",        "Model, effect",
                                              "Light",   "Model, light", "Effect, light", "Model, effect, light"};
    return kKinds[(part.mesh.empty() ? 0U : 1U) | (part.effect.empty() ? 0U : 2U) | (part.has_light ? 4U : 0U)];
}

// A dragged source file's path, if that's what the payload is.
std::string_view dropped_file(const ImGuiPayload* payload) {
    return payload == nullptr ? std::string_view()
                              : std::string_view(static_cast<const char*>(payload->Data),
                                                 static_cast<size_t>(std::max(payload->DataSize - 1, 0)));
}

} // namespace

PrefabEditor::PrefabEditor(core::Allocator& allocator)
    : allocator_(&allocator), doc_(allocator), text_(allocator), steps_(allocator), problems_(allocator),
      views_(allocator), held_(allocator), bounds_(allocator), chosen_bounds_(allocator), lines_(allocator),
      commands_(allocator) {}

void PrefabEditor::say(const char* format, std::string_view about, std::string_view more) {
    static_cast<void>(std::snprintf(message_, sizeof(message_), format, static_cast<int>(about.size()), about.data(),
                                    static_cast<int>(more.size()), more.data()));
    message_left_s_ = kMessageS;
}

bool PrefabEditor::ensure_scene(const PictureEngine& engine) {
    if (scene_) {
        return true;
    }
    core::Result<app::PreviewScene> scene = app::PreviewScene::create({.device = engine.device,
                                                                       .shaders = engine.shaders,
                                                                       .assets = engine.assets,
                                                                       .gpu_assets = engine.gpu_assets,
                                                                       .jobs = engine.jobs});
    if (!scene) {
        DE_LOG_ERROR("editor", "no scene to edit a prefab in: %s", scene.error().message);
        return false;
    }
    scene_.emplace(std::move(*scene));
#if DE_BUILD_DEV
    pick_answers_ = scene_->renderer().pick_answer().count;
    picks_waiting_ = 0;
#endif
    return true;
}

bool PrefabEditor::open(const PictureEngine& engine, std::string_view path, std::span<char> why) {
    if (is_open() && path == path_.view() && dirty()) {
        window_open_ = true; // it's here already, with changes: only brought forward
        focus_ = true;
        return true;
    }
    if (dirty()) {
        pending_ = Pending::Open;
        static_cast<void>(pending_path_.assign(path));
        focus_ = true;
        return true;
    }
    return load(engine, path, false, why);
}

bool PrefabEditor::open_new(const PictureEngine& engine, std::string_view path, std::span<char> why) {
    if (dirty()) {
        pending_ = Pending::OpenNew;
        static_cast<void>(pending_path_.assign(path));
        focus_ = true;
        return true;
    }
    return load(engine, path, true, why);
}

bool PrefabEditor::open_new_with(const PictureEngine& engine, std::string_view path,
                                 std::span<const PrefabDocPart> parts, std::span<char> why) {
    if (dirty()) {
        if (!why.empty()) {
            const std::string_view open = path_.view().substr(path_.view().rfind('/') + 1);
            static_cast<void>(std::snprintf(why.data(), why.size(),
                                            "%.*s has changes that aren't saved: save or close it in the prefab "
                                            "editor first",
                                            static_cast<int>(open.size()), open.data()));
        }
        focus_ = true;
        return false;
    }
    if (!load(engine, path, true, why)) {
        return false;
    }
    for (PrefabDocPart part : parts) {
        part.id = doc_.free_id(part.id.view());
        doc_.parts.push(part);
    }
    commit();
    return true;
}

bool PrefabEditor::load(const PictureEngine& engine, std::string_view path, bool is_new, std::span<char> why) {
    const auto refuse = [&](const char* reason) {
        if (!why.empty()) {
            static_cast<void>(
                std::snprintf(why.data(), why.size(), "%.*s %s", static_cast<int>(path.size()), path.data(), reason));
        }
        return false;
    };
    if (file_kind(path) != "Prefab" || !path.starts_with("src:/content/") || !SourcePath::fits(path.size())) {
        return refuse("isn't a prefab (.prefab) in the project's content");
    }
    const SourcePath file(path);
    core::Array<char> text(*allocator_);
    if (is_new) {
        if (engine.vfs->exists(file.c_str())) {
            return refuse("is there already");
        }
    } else {
        const core::Result<core::Array<std::byte>> bytes = engine.vfs->read_whole_file(file.c_str(), *allocator_);
        if (!bytes) {
            return refuse("can't be read");
        }
        text.resize(bytes->size());
        if (!text.empty()) {
            std::memcpy(text.data(), bytes->data(), text.size());
        }
        // Read apart first: one that can't be read leaves what's open as it is.
        PrefabDocument read(*allocator_);
        if (const core::Result<void> parsed = read.parse({text.data(), text.size()}); !parsed) {
            return refuse(parsed.error().message);
        }
    }
    if (!ensure_scene(engine)) {
        return refuse("can't be shown: no scene could be made for it");
    }
    clear_scene(engine);
    doc_.clear();
    if (!is_new) {
        static_cast<void>(doc_.parse({text.data(), text.size()}));
    }
    path_ = file;
    static_cast<void>(folder_.assign(folder_part(path_.view())));
    is_new_ = is_new;
    steps_.clear();
    doc_.write(text_);
    core::Array<char> first(*allocator_);
    first.assign(std::span<const char>(text_.data(), text_.size()));
    steps_.push(std::move(first));
    step_ = 0;
    saved_step_ = is_new ? SIZE_MAX : 0;
    selected_.clear();
    shown_variant_.clear();
    id_of_.clear();
    id_text_[0] = '\0';
    variant_text_[0] = '\0';
    field_edited_ = false;
    was_dragging_ = false;
    left_press_ = false;
    commands_.clear();
    camera_ = OrbitCamera{};
    auto_frame_ = true;
    framed_of_ = 0;
    rebuild_ = true;
    measure_in_s_ = 0.0;
    bounds_.clear();
    chosen_bounds_.clear();
    pending_ = Pending::None;
    window_open_ = true;
    focus_ = true;
    message_[0] = '\0';
    message_left_s_ = 0.0f;
    refresh_problems();
    return true;
}

void PrefabEditor::clear_scene(const PictureEngine& engine) {
    if (scene_) {
        const flecs::world& ecs = scene_->world().ecs();
        for (const PartView& view : views_) {
            if (view.entity != 0 && ecs.is_alive(view.entity)) {
                ecs.entity(view.entity).destruct(); // a placement's parts go with it
            }
        }
    }
    views_.clear();
    for (const asset::AssetHandle handle : held_) {
        engine.assets->release(handle);
    }
    held_.clear();
    built_stamp_ = 0;
#if DE_BUILD_DEV
    outline_.clear();
#endif
}

void PrefabEditor::close(const PictureEngine& engine) {
    clear_scene(engine);
    scene_.reset();
    path_.clear();
    folder_.clear();
    doc_.clear();
    steps_.clear();
    step_ = 0;
    saved_step_ = SIZE_MAX;
    selected_.clear();
    shown_variant_.clear();
    problems_.clear();
    commands_.clear();
    pending_ = Pending::None;
    window_open_ = false;
    visible_ = false;
}

void PrefabEditor::commit() {
    if (!is_open()) {
        return;
    }
    doc_.write(text_);
    const core::Array<char>& now = steps_[step_];
    if (std::string_view(now.data(), now.size()) == std::string_view(text_.data(), text_.size())) {
        return;
    }
    // What was undone is forgotten; if the file's own step was among it, nothing here is what the file holds.
    while (steps_.size() > step_ + 1) {
        steps_.pop();
    }
    if (saved_step_ != SIZE_MAX && saved_step_ > step_) {
        saved_step_ = SIZE_MAX;
    }
    core::Array<char> next(*allocator_);
    next.assign(std::span<const char>(text_.data(), text_.size()));
    steps_.push(std::move(next));
    ++step_;
    if (steps_.size() > kMostSteps) {
        steps_.remove_ordered(0);
        --step_;
        saved_step_ = saved_step_ == SIZE_MAX || saved_step_ == 0 ? SIZE_MAX : saved_step_ - 1;
    }
    refresh_problems();
}

void PrefabEditor::restore(size_t step) {
    static_cast<void>(doc_.parse({steps_[step].data(), steps_[step].size()}));
    step_ = step;
    if (doc_.find_part(selected_.view()) == UINT32_MAX) {
        selected_.clear();
    }
    if (shown_variant_index() == UINT32_MAX) {
        shown_variant_.clear();
    }
    id_of_.clear();
    field_edited_ = false;
    commands_.clear();
    refresh_problems();
}

bool PrefabEditor::undo() {
    if (!is_open() || !can_undo()) {
        return false;
    }
    restore(step_ - 1);
    say("Undone");
    return true;
}

bool PrefabEditor::redo() {
    if (!is_open() || !can_redo()) {
        return false;
    }
    restore(step_ + 1);
    say("Done again");
    return true;
}

uint32_t PrefabEditor::selected_index() const {
    return selected_.empty() ? UINT32_MAX : doc_.find_part(selected_.view());
}

std::string_view PrefabEditor::selected() const {
    return selected_.view();
}

uint32_t PrefabEditor::shown_variant_index() const {
    for (uint32_t v = 0; v < doc_.variants.size() && !shown_variant_.empty(); ++v) {
        if (doc_.variants[v].name.view() == shown_variant_.view()) {
            return v;
        }
    }
    return UINT32_MAX;
}

bool PrefabEditor::part_shown(uint32_t part) const {
    const uint32_t variant = shown_variant_index();
    return variant == UINT32_MAX || !doc_.leaves_out(variant, doc_.parts[part].id.view());
}

bool PrefabEditor::add_part(std::string_view source, const Vec3& position) {
    if (!is_open()) {
        return false;
    }
    const std::string_view kind = file_kind(source);
    const std::string_view name = file_part(source);
    if (!addable_kind(kind)) {
        say("%.*s isn't a model, an effect or a prefab: nothing added", name);
        return false;
    }
    if (source == path_.view()) {
        say("A prefab can't be inside itself");
        return false;
    }
    char relative[256];
    size_t length = 0;
    PrefabDocPart part;
    core::FixedString<255>& made_from = kind == "Model" ? part.mesh : kind == "Effect" ? part.effect : part.prefab;
    if (!source.starts_with("src:/content/") || !relative_path(folder_.view(), source, relative, length) ||
        !made_from.assign(std::string_view(relative, length))) {
        say("%.*s isn't in this project's content: nothing added", name);
        return false;
    }
    part.id = doc_.free_id(source);
    part.position = position;
    doc_.parts.push(part);
    selected_ = part.id;
    commit();
    say("Added '%.*s' from %.*s", part.id.view(), name);
    return true;
}

bool PrefabEditor::remove_part(std::string_view id) {
    const uint32_t index = is_open() ? doc_.find_part(id) : UINT32_MAX;
    if (index == UINT32_MAX) {
        say("There's no part '%.*s'", id);
        return false;
    }
    const core::FixedString<31> gone = doc_.parts[index].id;
    doc_.remove_part(index);
    if (selected_.view() == gone.view()) {
        selected_.clear();
    }
    commit();
    say("Deleted '%.*s'", gone.view());
    return true;
}

bool PrefabEditor::duplicate_part(std::string_view id) {
    const uint32_t index = is_open() ? doc_.find_part(id) : UINT32_MAX;
    if (index == UINT32_MAX) {
        say("There's no part '%.*s'", id);
        return false;
    }
    PrefabDocPart copy = doc_.parts[index];
    copy.id = doc_.free_id(copy.id.view());
    doc_.parts.push(copy);
    selected_ = copy.id;
    commit();
    say("'%.*s' is a copy of '%.*s', where it stands", copy.id.view(), id);
    return true;
}

bool PrefabEditor::rename_part(std::string_view id, std::string_view to) {
    const uint32_t index = is_open() ? doc_.find_part(id) : UINT32_MAX;
    if (index == UINT32_MAX) {
        say("There's no part '%.*s'", id);
        return false;
    }
    if (!PrefabDocument::valid_id(to)) {
        say("'%.*s' won't do as an ID: a-z, 0-9 and _, at most 31 characters", to);
        return false;
    }
    const bool was_selected = selected_.view() == doc_.parts[index].id.view();
    if (!doc_.rename_part(index, to)) {
        say("Another part is '%.*s' already", to);
        return false;
    }
    if (was_selected) {
        static_cast<void>(selected_.assign(to));
    }
    commit();
    return true;
}

bool PrefabEditor::place_part(std::string_view id, const Vec3& position, const Vec3& rotation_deg, const Vec3& scale) {
    const uint32_t index = is_open() ? doc_.find_part(id) : UINT32_MAX;
    if (index == UINT32_MAX) {
        say("There's no part '%.*s'", id);
        return false;
    }
    if (scale.x == 0.0f || scale.y == 0.0f || scale.z == 0.0f) {
        say("A part's scale can't be zero");
        return false;
    }
    PrefabDocPart& part = doc_.parts[index];
    part.position = position;
    part.set_rotation_deg(rotation_deg);
    part.scale = scale;
    commit();
    return true;
}

bool PrefabEditor::set_light(std::string_view id, const asset::LightDesc* light) {
    const uint32_t index = is_open() ? doc_.find_part(id) : UINT32_MAX;
    if (index == UINT32_MAX) {
        say("There's no part '%.*s'", id);
        return false;
    }
    PrefabDocPart& part = doc_.parts[index];
    if (light != nullptr && !part.prefab.empty()) {
        say("'%.*s' is a prefab: a light goes in a part of its own", id);
        return false;
    }
    if (light != nullptr && !light->valid()) {
        say("That light has a value out of range");
        return false;
    }
    part.has_light = light != nullptr;
    part.light = light != nullptr ? *light : asset::LightDesc{};
    commit();
    return true;
}

bool PrefabEditor::select(std::string_view id) {
    if (id.empty()) {
        selected_.clear();
        return true;
    }
    if (!is_open() || doc_.find_part(id) == UINT32_MAX) {
        say("There's no part '%.*s'", id);
        return false;
    }
    static_cast<void>(selected_.assign(id));
    return true;
}

bool PrefabEditor::add_variant(std::string_view name) {
    if (!is_open()) {
        return false;
    }
    if (!PrefabDocument::valid_id(name)) {
        say("'%.*s' won't do as a variant's name: a-z, 0-9 and _, at most 31 characters", name);
        return false;
    }
    for (const PrefabDocVariant& variant : doc_.variants) {
        if (variant.name.view() == name) {
            say("There's a variant '%.*s' already", name);
            return false;
        }
    }
    if (doc_.variants.size() >= asset::PrefabLimits::kMaxVariants) {
        say("A prefab has at most 32 variants");
        return false;
    }
    doc_.variants.push({.name = core::FixedString<31>(name)});
    static_cast<void>(shown_variant_.assign(name));
    commit();
    say("Showing '%.*s': untick the parts it leaves out", name);
    return true;
}

bool PrefabEditor::remove_variant(std::string_view name) {
    for (uint32_t v = 0; is_open() && v < doc_.variants.size(); ++v) {
        if (doc_.variants[v].name.view() == name) {
            if (shown_variant_.view() == name) {
                shown_variant_.clear();
            }
            doc_.remove_variant(v);
            commit();
            say("Deleted the variant '%.*s'", name);
            return true;
        }
    }
    say("There's no variant '%.*s'", name);
    return false;
}

bool PrefabEditor::leave_out(std::string_view variant, std::string_view part, bool out) {
    for (uint32_t v = 0; is_open() && v < doc_.variants.size(); ++v) {
        if (doc_.variants[v].name.view() != variant) {
            continue;
        }
        if (doc_.find_part(part.substr(0, part.find('/'))) == UINT32_MAX) {
            say("There's no part '%.*s'", part);
            return false;
        }
        doc_.leave_out(v, part, out);
        commit();
        return true;
    }
    say("There's no variant '%.*s'", variant);
    return false;
}

bool PrefabEditor::show_variant(std::string_view name) {
    if (name.empty()) {
        shown_variant_.clear();
        return true;
    }
    for (const PrefabDocVariant& variant : doc_.variants) {
        if (is_open() && variant.name.view() == name) {
            shown_variant_ = variant.name;
            return true;
        }
    }
    say("There's no variant '%.*s'", name);
    return false;
}

bool PrefabEditor::save(const PictureEngine& engine) {
    if (!is_open()) {
        return false;
    }
    const std::string_view name = file_part(path_.view());
    if (doc_.parts.empty()) {
        say("Nothing to save yet: a prefab needs a part. Drag one in from Resources");
        return false;
    }
    doc_.write(text_);
    if (const core::Result<void> written =
            engine.vfs->write_file(path_.c_str(), std::as_bytes(std::span<const char>(text_.data(), text_.size())));
        !written) {
        say("%.*s couldn't be written: %.*s", name, written.error().message);
        return false;
    }
    is_new_ = false;
    saved_step_ = step_;
    saved_ = true;
    if (problems_.empty()) {
        say("Saved %.*s", name);
    } else {
        say("Saved %.*s, with problems: it won't cook until they're mended", name);
    }
    return true;
}

bool PrefabEditor::take_saved() {
    const bool saved = saved_;
    saved_ = false;
    return saved;
}

bool PrefabEditor::take_show_file(core::FixedString<255>& path) {
    if (show_file_.empty()) {
        return false;
    }
    path = show_file_;
    show_file_.clear();
    return true;
}

bool PrefabEditor::may_quit() {
    if (!dirty()) {
        return true;
    }
    pending_ = Pending::Quit;
    focus_ = true;
    return false;
}

bool PrefabEditor::take_quit() {
    const bool quit = quit_;
    quit_ = false;
    return quit;
}

void PrefabEditor::finish_pending(const PictureEngine& engine) {
    const Pending pending = pending_;
    pending_ = Pending::None;
    const core::FixedString<255> path = pending_path_;
    switch (pending) {
    case Pending::Close:
        close(engine);
        break;
    case Pending::Open:
    case Pending::OpenNew:
        if (!load(engine, path.view(), pending == Pending::OpenNew, message_)) {
            message_left_s_ = kMessageS;
        }
        break;
    case Pending::Quit:
        quit_ = true;
        break;
    case Pending::None:
        break;
    }
}

bool PrefabEditor::resolve(const PictureEngine& engine, std::string_view relative, std::string_view kind,
                           asset::AssetType type, asset::AssetHandle& out) {
    char full[platform::kMaxPathLength];
    size_t length = 0;
    asset::Guid guid;
    asset::AssetId id;
    if (relative.empty() || !joined_path(folder_.view(), relative, full, length)) {
        return false;
    }
    const std::string_view path(full, length);
    if (file_kind(path) != kind || path == path_.view() || !source_asset_id(*engine.vfs, path, guid, id) ||
        !engine.vfs->exists(asset::cooked_path(id, type).c_str())) {
        return false;
    }
    out = engine.assets->load({.id = id, .type = type});
    if (!out.is_valid()) {
        return false;
    }
    held_.push(out);
    return true;
}

uint64_t PrefabEditor::structure_stamp() const {
    uint64_t stamp = 1;
    const auto add = [&stamp](std::string_view text) {
        stamp = core::mix64(stamp ^ core::fnv1a_64(text.data(), text.size()));
    };
    const uint32_t variant = shown_variant_index();
    for (uint32_t p = 0; p < doc_.parts.size(); ++p) {
        const PrefabDocPart& part = doc_.parts[p];
        add(part.mesh.view());
        add(part.effect.view());
        add(part.prefab.view());
        add(part.variant.view());
        add(variant == UINT32_MAX || !doc_.leaves_out(variant, part.id.view()) ? "shown" : "left out");
    }
    // What the variant shown leaves out inside the prefabs it nests.
    for (const PrefabDocLeftOut& gone : doc_.left_out) {
        if (gone.variant == variant && gone.path.view().find('/') != std::string_view::npos) {
            add(gone.path.view());
        }
    }
    return stamp | 1U;
}

void PrefabEditor::rebuild(const PictureEngine& engine) {
    if (!scene_) {
        return;
    }
    // What it held is let go only once the new is loaded: what both use never leaves memory in between.
    core::Array<asset::AssetHandle> was(*allocator_);
    was.assign(std::span<const asset::AssetHandle>(held_.data(), held_.size()));
    held_.clear();
    const flecs::world& ecs = scene_->world().ecs();
    for (const PartView& view : views_) {
        if (view.entity != 0 && ecs.is_alive(view.entity)) {
            ecs.entity(view.entity).destruct();
        }
    }
    views_.clear();
    for (uint32_t p = 0; p < doc_.parts.size(); ++p) {
        const PrefabDocPart& part = doc_.parts[p];
        PartView view;
        asset::AssetHandle mesh;
        asset::AssetHandle effect;
        asset::AssetHandle prefab;
        if (!part.prefab.empty()) {
            view.prefab_missing = !resolve(engine, part.prefab.view(), "Prefab", asset::AssetType::Prefab, prefab);
        } else {
            view.mesh_missing =
                !part.mesh.empty() && !resolve(engine, part.mesh.view(), "Model", asset::AssetType::Mesh, mesh);
            view.effect_missing = !part.effect.empty() &&
                                  !resolve(engine, part.effect.view(), "Effect", asset::AssetType::Effect, effect);
        }
        if (part_shown(p)) {
            const flecs::entity entity = ecs.entity();
            world::place(entity, {.position = DVec3(part.position), .rotation = part.rotation, .scale = part.scale});
            if (mesh.is_valid()) {
                entity.set(world::MeshInstance{.mesh = mesh});
            }
            if (effect.is_valid()) {
                entity.set(world::EffectInstance{.effect = effect});
            }
            if (prefab.is_valid()) {
                // A placement of its own: the world makes its parts once the prefab is loaded, attached to it, so
                // they go where the handles take it.
                entity.set(world::PrefabInstance{
                    .prefab = prefab,
                    .variant = part.variant.empty() ? 0U : asset::prefab_variant_hash(part.variant.view())});
                entity.add<world::PrefabPending>();
            }
            view.entity = entity.id();
        }
        views_.push(view);
    }
    for (const asset::AssetHandle handle : was) {
        engine.assets->release(handle);
    }
    built_stamp_ = structure_stamp();
    measure_in_s_ = 0.0;
    refresh_problems();
}

void PrefabEditor::place_parts() {
    if (!scene_) {
        return;
    }
    const flecs::world& ecs = scene_->world().ecs();
    for (uint32_t p = 0; p < views_.size() && p < doc_.parts.size(); ++p) {
        const flecs::entity_t id = views_[p].entity;
        if (id == 0 || !ecs.is_alive(id)) {
            continue;
        }
        const PrefabDocPart& part = doc_.parts[p];
        const flecs::entity entity = ecs.entity(id);
        const auto* at = entity.try_get<world::Transform>();
        if (at == nullptr || Vec3(at->position) != part.position || at->rotation != part.rotation ||
            at->scale != part.scale) {
            world::place(entity, {.position = DVec3(part.position), .rotation = part.rotation, .scale = part.scale});
        }
        // Its light, as the document has it now (one out of range isn't shown: Problems says so).
        const auto* lit = entity.try_get<world::LocalLight>();
        if (part.has_light && part.prefab.empty() && part.light.valid()) {
            const world::LocalLight wanted = world::LocalLight::from(part.light);
            if (lit == nullptr || !(*lit == wanted)) {
                entity.set(wanted);
            }
        } else if (lit != nullptr) {
            entity.remove<world::LocalLight>();
        }
    }
}

void PrefabEditor::hide_left_out(const PictureEngine& engine) {
    if (!scene_) {
        return;
    }
    const uint32_t variant = shown_variant_index();
    if (variant == UINT32_MAX) {
        return;
    }
    const bool any_inside = std::ranges::any_of(doc_.left_out, [variant](const PrefabDocLeftOut& gone) {
        return gone.variant == variant && gone.path.view().find('/') != std::string_view::npos;
    });
    if (!any_inside) {
        return;
    }
    const flecs::world& ecs = scene_->world().ecs();
    for (uint32_t p = 0; p < views_.size() && p < doc_.parts.size(); ++p) {
        const flecs::entity_t id = views_[p].entity;
        const auto* placed = id != 0 && ecs.is_alive(id) ? ecs.entity(id).try_get<world::PrefabInstance>() : nullptr;
        const auto* inner = placed != nullptr ? engine.assets->get<asset::PrefabAsset>(placed->prefab) : nullptr;
        if (inner == nullptr) {
            continue;
        }
        const std::span<const asset::PrefabPart> parts = inner->parts.get(inner);
        const std::string_view outer = doc_.parts[p].id.view();
        world::each_prefab_part(ecs, placed->first_part, [&](flecs::entity part) {
            const uint32_t index = part.get<world::PrefabPart>().index;
            core::FixedString<127> path(outer);
            if (index >= parts.size() || !path.append('/') || !path.append(inner->name(inner, parts[index]))) {
                return;
            }
            for (const PrefabDocLeftOut& gone : doc_.left_out) {
                if (gone.variant == variant && covers(gone.path.view(), path.view())) {
                    part.remove<world::MeshInstance>();
                    part.remove<world::EffectInstance>();
                    return;
                }
            }
        });
    }
}

void PrefabEditor::add_bounds(const PictureEngine& engine, flecs::entity entity, PictureSubject& subject) const {
    const auto* where = entity.try_get<world::WorldTransform>();
    if (where == nullptr) {
        return;
    }
    const world::WorldTransform at = *where;
    const auto* placed = entity.try_get<world::MeshInstance>();
    if (const auto* mesh = placed != nullptr ? engine.assets->get<asset::MeshAsset>(placed->mesh) : nullptr;
        mesh != nullptr) {
        for (uint32_t corner = 0; corner < 8; ++corner) {
            const Vec3 local((corner & 1U) != 0 ? mesh->bounds.max[0] : mesh->bounds.min[0],
                             (corner & 2U) != 0 ? mesh->bounds.max[1] : mesh->bounds.min[1],
                             (corner & 4U) != 0 ? mesh->bounds.max[2] : mesh->bounds.min[2]);
            subject.points.push(Vec3(at.position) + (at.rotation * (at.scale * local)));
        }
    }
    const auto* played = entity.try_get<world::EffectInstance>();
    if (const auto* effect = played != nullptr ? engine.assets->get<asset::EffectAsset>(played->effect) : nullptr;
        effect != nullptr) {
        float reach_m = 0.0f;
        for (const asset::EffectEmitter& emitter : effect->emitters.get(effect)) {
            reach_m = std::max(reach_m, emitter.reach_m);
        }
        const float scale = std::max({at.scale.x, at.scale.y, at.scale.z});
        subject.spheres.push(Vec4(Vec3(at.position), std::max(reach_m * scale, 0.01f)));
        subject.nearest_draw_distance_m = subject.nearest_draw_distance_m > 0.0f
                                              ? std::min(subject.nearest_draw_distance_m, effect->draw_distance_m)
                                              : effect->draw_distance_m;
    }
}

void PrefabEditor::part_bounds(const PictureEngine& engine, uint32_t part, PictureSubject& subject) {
    if (!scene_) {
        return;
    }
    const flecs::world& ecs = scene_->world().ecs();
    const flecs::entity_t id = part < views_.size() ? views_[part].entity : 0;
    if (id == 0 || !ecs.is_alive(id)) {
        return;
    }
    const flecs::entity entity = ecs.entity(id);
    add_bounds(engine, entity, subject);
    if (const auto* placed = entity.try_get<world::PrefabInstance>(); placed != nullptr) {
        world::each_prefab_part(ecs, placed->first_part,
                                [&](flecs::entity inner) { add_bounds(engine, inner, subject); });
    }
}

void PrefabEditor::measure(const PictureEngine& engine) {
    bounds_.clear();
    chosen_bounds_.clear();
    const uint32_t chosen = selected_index();
    for (uint32_t p = 0; p < views_.size(); ++p) {
        part_bounds(engine, p, bounds_);
        if (p == chosen) {
            part_bounds(engine, p, chosen_bounds_);
        }
    }
}

void PrefabEditor::frame(bool chosen_only) {
    const bool chosen = chosen_only && (!chosen_bounds_.points.empty() || !chosen_bounds_.spheres.empty());
    camera_.frame(chosen ? chosen_bounds_ : bounds_, kFill);
    auto_frame_ = false; // asked for: it stays as asked
}

void PrefabEditor::refresh_problems() {
    doc_.check(problems_);
    // And what only looking can say: the files its parts name that aren't there, or aren't cooked. Only of the scene
    // as it's made from the document as it stands.
    if (views_.size() != doc_.parts.size() || built_stamp_ != structure_stamp()) {
        return;
    }
    const auto missing = [this](const PrefabDocPart& part, std::string_view file, const char* what) {
        char text[160];
        const std::string_view id = part.id.view();
        static_cast<void>(std::snprintf(text, sizeof(text), "Part '%.*s': %.*s isn't %s that's there and cooked.",
                                        static_cast<int>(id.size()), id.data(), static_cast<int>(file.size()),
                                        file.data(), what));
        problems_.push(core::FixedString<159>(std::string_view(text).substr(0, 159)));
    };
    for (uint32_t p = 0; p < views_.size(); ++p) {
        const PrefabDocPart& part = doc_.parts[p];
        if (views_[p].mesh_missing) {
            missing(part, part.mesh.view(), "a model");
        }
        if (views_[p].effect_missing) {
            missing(part, part.effect.view(), "an effect");
        }
        if (views_[p].prefab_missing) {
            missing(part, part.prefab.view(), "another prefab");
        }
    }
}

uint32_t PrefabEditor::part_of_slot(uint32_t slot) {
    if (!scene_) {
        return UINT32_MAX;
    }
    const flecs::world& ecs = scene_->world().ecs();
    for (uint32_t p = 0; p < views_.size(); ++p) {
        const flecs::entity_t id = views_[p].entity;
        if (id == 0 || !ecs.is_alive(id)) {
            continue;
        }
        const flecs::entity entity = ecs.entity(id);
        bool found = false;
        if (const auto* own = entity.try_get<world::RenderSlot>(); own != nullptr && own->slot == slot) {
            found = true;
        }
        if (const auto* placed = entity.try_get<world::PrefabInstance>(); placed != nullptr && !found) {
            world::each_prefab_part(ecs, placed->first_part, [&](flecs::entity inner) {
                const auto* inner_slot = inner.try_get<world::RenderSlot>();
                found = found || (inner_slot != nullptr && inner_slot->slot == slot);
            });
        }
        if (found) {
            return p;
        }
    }
    return UINT32_MAX;
}

#if DE_BUILD_DEV
void PrefabEditor::gather_outline() {
    if (!scene_) {
        return;
    }
    outline_.clear();
    const flecs::world& ecs = scene_->world().ecs();
    const uint32_t chosen = selected_index();
    const flecs::entity_t id = chosen < views_.size() ? views_[chosen].entity : 0;
    if (id == 0 || !ecs.is_alive(id)) {
        return;
    }
    const auto add = [this](flecs::entity entity) {
        const auto* mesh = entity.try_get<world::MeshInstance>();
        const auto* slot = entity.try_get<world::RenderSlot>();
        if (mesh != nullptr && slot != nullptr) {
            outline_.push({.slot = slot->slot, .mesh = mesh->mesh});
        }
    };
    const flecs::entity entity = ecs.entity(id);
    add(entity);
    if (const auto* placed = entity.try_get<world::PrefabInstance>(); placed != nullptr) {
        world::each_prefab_part(ecs, placed->first_part, add);
    }
}

void PrefabEditor::take_pick() {
    if (!scene_) {
        return;
    }
    const render::PickAnswer answer = scene_->renderer().pick_answer();
    if (answer.count == pick_answers_) {
        // A frame that wasn't drawn never answers: the click did nothing.
        if (picks_waiting_ > 0 && ++pick_wait_frames_ > rhi::kFramesInFlight + 8) {
            picks_waiting_ = 0;
        }
        return;
    }
    pick_answers_ = answer.count;
    pick_wait_frames_ = 0;
    if (picks_waiting_ == 0) {
        return;
    }
    --picks_waiting_;
    const uint32_t part = answer.slot != render::kNothingPicked ? part_of_slot(answer.slot) : UINT32_MAX;
    if (part < doc_.parts.size()) {
        selected_ = doc_.parts[part].id;
    } else {
        selected_.clear();
    }
}
#endif

void PrefabEditor::update(const PictureEngine& engine, float frame_s) {
    DE_PROFILE_ZONE("PrefabEditor::update");
    if (!scene_) {
        return;
    }
    message_left_s_ = std::max(0.0f, message_left_s_ - frame_s);
    if (!window_open_) {
        // Its cross: asked first, if that would lose something.
        if (dirty()) {
            pending_ = Pending::Close;
            window_open_ = true;
            focus_ = true;
        } else {
            close(engine);
            return;
        }
    }
    app::PreviewScene& scene = *scene_;
#if DE_BUILD_DEV
    take_pick();
#endif
    if (rebuild_ || structure_stamp() != built_stamp_) {
        rebuild(engine);
        rebuild_ = false;
    }
    if (!gizmo_.dragging()) {
        place_parts();
    }
    hide_left_out(engine);
    measure_in_s_ -= static_cast<double>(frame_s);
    if (measure_in_s_ <= 0.0) {
        measure(engine);
        measure_in_s_ = kMeasureS;
        // Until the camera is taken hold of, it keeps the whole prefab in view as its parts come in (models load a
        // little after they're asked for, and a nested prefab's parts after that).
        const size_t there = bounds_.points.size() + bounds_.spheres.size();
        if (auto_frame_ && there != 0 && there != framed_of_) {
            camera_.frame(bounds_, kFill);
            framed_of_ = there;
        }
    }
#if DE_BUILD_DEV
    gather_outline();
#endif
    framing_ = camera_.framing(bounds_);
    scene.set_camera(camera_.camera(framing_));
    // A studio's light, the same from wherever it's looked at.
    lighting_ = studio_lighting(picture_axes(), framing_.distance_m);
    static constexpr float kTop[3][3] = {{0.012f, 0.013f, 0.016f}, {0.16f, 0.16f, 0.17f}, {0.62f, 0.62f, 0.64f}};
    static constexpr float kBottom[3][3] = {{0.040f, 0.043f, 0.050f}, {0.24f, 0.24f, 0.25f}, {0.80f, 0.80f, 0.82f}};
    lighting_.backdrop.enabled = true;
    const auto backdrop = static_cast<size_t>(backdrop_);
    for (int channel = 0; channel < 3; ++channel) {
        lighting_.backdrop.top[channel] = kTop[backdrop][channel];
        lighting_.backdrop.bottom[channel] = kBottom[backdrop][channel];
        if (!studio_) {
            // Only a trace of it, so the prefab's shape still reads: what shows is what its own lights do.
            lighting_.sun_color[channel] *= 0.02f;
            lighting_.sky_color[channel] *= 0.02f;
            lighting_.ground_color[channel] *= 0.02f;
        }
    }
    // The ground's grid through the prefab's origin, a step that suits how much is in view.
    lines_.clear();
    // How far the chosen part's light reaches: three rings about it.
    if (const uint32_t chosen = selected_index();
        chosen < doc_.parts.size() && doc_.parts[chosen].has_light && part_shown(chosen)) {
        const PrefabDocPart& part = doc_.parts[chosen];
        const float scale = std::max({std::abs(part.scale.x), std::abs(part.scale.y), std::abs(part.scale.z)});
        const float reach = part.light.radius_m * scale;
        constexpr uint32_t kSegments = 64;
        constexpr uint32_t kReachColor = 0xff39c5f5U; // a warm yellow, as seen
        for (uint32_t i = 0; i < kSegments; ++i) {
            const float a = glm::two_pi<float>() * static_cast<float>(i) / static_cast<float>(kSegments);
            const float b = glm::two_pi<float>() * static_cast<float>(i + 1) / static_cast<float>(kSegments);
            const Vec2 from(std::cos(a) * reach, std::sin(a) * reach);
            const Vec2 to(std::cos(b) * reach, std::sin(b) * reach);
            const Vec3 at = part.position;
            lines_.push({.from = DVec3(at + Vec3(from.x, from.y, 0.0f)),
                         .to = DVec3(at + Vec3(to.x, to.y, 0.0f)),
                         .color = kReachColor});
            lines_.push({.from = DVec3(at + Vec3(from.x, 0.0f, from.y)),
                         .to = DVec3(at + Vec3(to.x, 0.0f, to.y)),
                         .color = kReachColor});
            lines_.push({.from = DVec3(at + Vec3(0.0f, from.x, from.y)),
                         .to = DVec3(at + Vec3(0.0f, to.x, to.y)),
                         .color = kReachColor});
        }
    }
    if (grid_) {
        const float across = 2.0f * framing_.distance_m * std::tan(framing_.vertical_fov_rad * 0.5f);
        const float cell = std::pow(10.0f, std::floor(std::log10(std::max(across, 1e-4f) / 4.0f)));
        const float half = cell * static_cast<float>(kGridMiddle);
        for (uint32_t i = 0; i < kGridLines; ++i) {
            const float at = (static_cast<float>(i) * cell) - half;
            const uint32_t color = i == kGridMiddle ? kAxisColor : kGridColor;
            push_line(lines_, Vec3(at, 0.0f, -half), Vec3(at, 0.0f, half), color);
            push_line(lines_, Vec3(-half, 0.0f, at), Vec3(half, 0.0f, at), color);
        }
    }
#if DE_BUILD_DEV
    scene.renderer().set_debug_lines(lines_.span());
#endif
    scene.advance(static_cast<double>(frame_s));
}

void PrefabEditor::show(uint32_t width, uint32_t height) {
    if (!scene_) {
        return;
    }
    scene_->set_size(width, height);
    visible_ = scene_->texture_id() != 0;
}

void PrefabEditor::draw() {
    if (!scene_ || !visible_) {
        return;
    }
    visible_ = false;
#if DE_BUILD_DEV
    overlay_.enabled = true;
    overlay_.selected = outline_.span();
    scene_->renderer().set_editor_overlay(overlay_);
    overlay_.pick = false; // asked once
#endif
    scene_->draw(lighting_);
}

float PrefabEditor::view_bar(const PictureEngine& engine, float width) {
    const float near = px(4.0f);
    const float apart = px(12.0f);
    FlowRow row(px(6.0f), width - px(6.0f));
    // A chip in the next place on the bar.
    const auto chip = [&row](const char* label, bool on, const char* tip, float gap) {
        row.place(chip_width(label), gap);
        return toggle_chip(label, on, tip);
    };
    if (chip("Save", dirty(),
             is_new_ ? "Write the prefab's file for the first time (Ctrl+S); the editor then cooks it"
                     : "Write the prefab's file (Ctrl+S); the editor then cooks it, and what's placed in the world "
                       "follows",
             0.0f)) {
        static_cast<void>(save(engine));
    }
    ImGui::BeginDisabled(!can_undo());
    if (chip("Undo", false, "Take back the last change to the prefab (Ctrl+Z)", near)) {
        static_cast<void>(undo());
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!can_redo());
    if (chip("Redo", false, "Make the change taken back again (Ctrl+Y)", near)) {
        static_cast<void>(redo());
    }
    ImGui::EndDisabled();
    struct ToolChip {
        const char* label;
        Tool tool;
        const char* tip;
    };
    static constexpr ToolChip kTools[] = {
        {.label = "Select", .tool = Tool::Select, .tip = "Choose parts by clicking them, with no handles (Q)"},
        {.label = "Move", .tool = Tool::Move, .tip = "Handles that move the chosen part (W)"},
        {.label = "Rotate", .tool = Tool::Rotate, .tip = "Handles that turn the chosen part (E)"},
        {.label = "Scale", .tool = Tool::Scale, .tip = "Handles that scale the chosen part (R)"},
    };
    for (const ToolChip& tool : kTools) {
        if (chip(tool.label, tool_ == tool.tool, tool.tip, tool.tool == Tool::Select ? apart : near)) {
            tool_ = tool.tool;
        }
    }
    if (chip(space_ == Space::World ? "Prefab axes" : "Part axes", false,
             "Which axes the handles follow: the prefab's, or the chosen part's own", near)) {
        space_ = space_ == Space::World ? Space::Local : Space::World;
    }
    if (chip("Grid", grid_, "The ground's grid, through the prefab's origin", apart)) {
        grid_ = !grid_;
    }
    if (chip("Studio light", studio_,
             "The studio's light on the prefab. Off: only the prefab's own lights show, as in a dark room", near)) {
        studio_ = !studio_;
    }
    static constexpr const char* kBackdrops[] = {"Dark", "Grey", "Light"};
    row.place(px(76.0f), near);
    if (begin_combo("##backdrop", kBackdrops[static_cast<size_t>(backdrop_)], px(76.0f))) {
        for (size_t i = 0; i < 3; ++i) {
            if (ImGui::Selectable(kBackdrops[i], static_cast<size_t>(backdrop_) == i)) {
                backdrop_ = static_cast<Backdrop>(i);
            }
        }
        end_combo();
    }
    if (chip("Frame", false, "Look at the chosen part, or at the whole prefab with none chosen (F)", near)) {
        frame(true);
    }
    return row.bottom();
}

void PrefabEditor::picture(ImVec2 size) {
    if (!scene_ || size.x < 1.0f || size.y < 1.0f) {
        return;
    }
    app::PreviewScene& scene = *scene_;
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 scale = io.DisplayFramebufferScale;
    show(static_cast<uint32_t>(std::lround(size.x * scale.x)), static_cast<uint32_t>(std::lround(size.y * scale.y)));
    const ImVec2 corner = ImGui::GetCursorScreenPos();
    const ImVec2 far_corner(corner.x + size.x, corner.y + size.y);
    ImGui::InvisibleButton("##picture", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const View view = camera_.view(framing_, corner, size);
    // A file dragged in from Resources: a part, where the mouse is on the ground through the prefab's origin.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(AssetBrowser::kSourceFilePayload)) {
            Vec3 point(0.0f);
            const Vec3 eye(view.position);
            const Vec3 ray = view.ray(io.MousePos);
            if (eye.y > 0.0f && ray.y < -1e-4f) {
                const Vec3 hit = eye + (ray * (-eye.y / ray.y));
                if (glm::length(hit) < kFarthestDropM) {
                    point = Vec3(hit.x, 0.0f, hit.z);
                }
            }
            static_cast<void>(add_part(dropped_file(payload), point));
        }
        ImGui::EndDragDropTarget();
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (const uint64_t texture = scene.texture_id(); texture != 0) {
        draw->AddImage(ImTextureRef(static_cast<ImTextureID>(texture)), corner, far_corner);
    }

    // The camera: the right button turns it about what it looks at (as does Alt and the left), the middle one or
    // Shift slides it, the wheel comes closer.
    const bool turning = active && !gizmo_.dragging() &&
                         (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
                          (io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left)));
    if (turning) {
        camera_.drag(framing_, size, io.MouseDelta, ImGui::IsMouseDown(ImGuiMouseButton_Middle) || io.KeyShift);
        auto_frame_ = false;
    }
    if (hovered && io.MouseWheel != 0.0f) {
        camera_.zoom(io.MouseWheel);
        auto_frame_ = false;
    }

    // The chosen part's handles. What they do goes to its entity; the document takes it from there, and keeps it as
    // one step when the drag ends.
    const flecs::world& ecs = scene.world().ecs();
    const uint32_t chosen = selected_index();
    const flecs::entity_t chosen_id = chosen < views_.size() ? views_[chosen].entity : 0;
    const flecs::entity entity = chosen_id != 0 && ecs.is_alive(chosen_id) ? ecs.entity(chosen_id) : flecs::entity();
    const auto* where = entity.is_valid() ? entity.try_get<world::WorldTransform>() : nullptr;
    draw->PushClipRect(corner, far_corner, true);
    const bool mouse_free = hovered && !turning && !io.KeyAlt;
    const bool on_handle = gizmo_.update(view, draw, tool_, space_, snap_, entity, std::span<const flecs::entity_t>(),
                                         where != nullptr ? where->position : DVec3(0.0), mouse_free, commands_);
    draw->PopClipRect();
    const bool dragging = gizmo_.dragging();
    if ((dragging || was_dragging_) && entity.is_valid() && chosen < doc_.parts.size()) {
        if (const auto* at = entity.try_get<world::Transform>(); at != nullptr) {
            PrefabDocPart& part = doc_.parts[chosen];
            part.position = Vec3(at->position);
            if (at->rotation != part.rotation) {
                part.set_rotation(at->rotation);
            }
            part.scale = at->scale;
        }
    }
    if (was_dragging_ && !dragging) {
        commit();
        commands_.clear();
    }
    was_dragging_ = dragging;

    // A left click, let go without moving, chooses what's under it: not one on a handle, not one that turns.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        left_press_ = !on_handle && !io.KeyAlt;
    }
#if DE_BUILD_DEV
    overlay_.outline_px = std::max(2.0f, 2.0f * scale.x);
    if (left_press_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        left_press_ = false;
        const float still = px(4.0f);
        if (hovered && !dragging && io.MouseDragMaxDistanceSqr[0] < still * still && scene.width() > 0 &&
            scene.height() > 0) {
            const float u = std::clamp((io.MousePos.x - corner.x) / size.x, 0.0f, 1.0f);
            const float v = std::clamp((io.MousePos.y - corner.y) / size.y, 0.0f, 1.0f);
            overlay_.pick = true;
            overlay_.pick_x = std::min(static_cast<uint32_t>(u * static_cast<float>(scene.width())), scene.width() - 1);
            overlay_.pick_y =
                std::min(static_cast<uint32_t>(v * static_cast<float>(scene.height())), scene.height() - 1);
            ++picks_waiting_;
            pick_wait_frames_ = 0;
        }
    }
#endif

    // What it says: why it's empty, and what it last did.
    if (doc_.parts.empty()) {
        draw_clipped_label(corner, far_corner,
                           "Nothing in it yet. Drag a model, an effect or a prefab in from Resources (Ctrl+Space).",
                           color_u32(ThemeColor::TextSecondary), true);
    }
    if (message_left_s_ > 0.0f && message_[0] != '\0') {
        const float pad = px(8.0f);
        const ImVec2 text = ImGui::CalcTextSize(message_);
        const ImVec2 min(corner.x + pad, far_corner.y - pad - text.y - (2.0f * px(4.0f)));
        const ImVec2 max(std::min(min.x + text.x + (2.0f * px(8.0f)), far_corner.x - pad), far_corner.y - pad);
        draw->AddRectFilled(min, max, color_u32(ThemeColor::Surface), px(theme().metrics.control_rounding));
        draw_clipped_label(ImVec2(min.x + px(8.0f), min.y), max, message_, color_u32(ThemeColor::Text), false);
    }
}

void PrefabEditor::parts_list(const PictureEngine& engine, std::string_view resources_file) {
    const float pad = px(8.0f);
    const float font = ImGui::GetFontSize();
    section_header("Parts");
    enum class Ask : uint8_t { None, Rename, Duplicate, Delete };
    Ask ask = Ask::None;
    uint32_t asked_of = UINT32_MAX;
    const uint32_t variant = shown_variant_index();
    if (doc_.parts.empty()) {
        ImGui::Indent(pad);
        ImGui::PushStyleColor(ImGuiCol_Text, color(ThemeColor::TextSecondary));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted("None yet. Drag a model, an effect or a prefab here from Resources (Ctrl+Space), or "
                               "onto the picture.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Unindent(pad);
    } else if (ImGui::BeginTable("##parts", variant != UINT32_MAX ? 3 : 2,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("part", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("made of", ImGuiTableColumnFlags_WidthFixed, font * 7.0f);
        if (variant != UINT32_MAX) {
            ImGui::TableSetupColumn("kept", ImGuiTableColumnFlags_WidthFixed, font * 2.6f);
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
        secondary_text("Part");
        ImGui::TableSetColumnIndex(1);
        secondary_text("Made of");
        if (variant != UINT32_MAX) {
            ImGui::TableSetColumnIndex(2);
            secondary_text("Kept");
        }
        for (uint32_t p = 0; p < doc_.parts.size(); ++p) {
            const PrefabDocPart& part = doc_.parts[p];
            const bool chosen = selected_.view() == part.id.view();
            const bool kept = variant == UINT32_MAX || !doc_.leaves_out(variant, part.id.view());
            const bool missing =
                p < views_.size() && (views_[p].mesh_missing || views_[p].effect_missing || views_[p].prefab_missing);
            ImGui::PushID(static_cast<int>(p));
            ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight());
            if (chosen) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, color_u32(ThemeColor::Selection));
            }
            ImGui::TableSetColumnIndex(0);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
            ImGui::PushStyleColor(ImGuiCol_Text, color(kept ? ThemeColor::Text : ThemeColor::TextDisabled));
            // The row's own colours are the table's: the selectable only takes the mouse.
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                  color(chosen ? ThemeColor::SelectionHovered : ThemeColor::Header));
            if (ImGui::Selectable(part.id.c_str(), chosen,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                  ImVec2(0.0f, ImGui::GetFrameHeight()))) {
                selected_ = part.id;
            }
            ImGui::PopStyleColor(4);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                selected_ = part.id;
                measure(engine);
                frame(true);
            }
            push_menu_style();
            prepare_menu_popup();
            if (ImGui::BeginPopupContextItem("##part_menu")) {
                selected_ = part.id;
                if (ImGui::MenuItem("Rename")) {
                    ask = Ask::Rename;
                    asked_of = p;
                }
                if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
                    ask = Ask::Duplicate;
                    asked_of = p;
                }
                if (ImGui::MenuItem("Delete", "Del")) {
                    ask = Ask::Delete;
                    asked_of = p;
                }
                ImGui::EndPopup();
            }
            pop_menu_style();
            ImGui::TableSetColumnIndex(1);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(color(missing ? ThemeColor::Warning
                                     : kept  ? ThemeColor::TextSecondary
                                             : ThemeColor::TextDisabled),
                               "%s", missing ? "Missing" : made_of(part));
            if (missing && ImGui::IsItemHovered()) {
                tooltip("A file it names isn't there, or isn't cooked yet: Problems, below, says which.");
            }
            if (variant != UINT32_MAX) {
                ImGui::TableSetColumnIndex(2);
                bool keep = kept;
                if (compact_checkbox(keep)) {
                    doc_.leave_out(variant, part.id.view(), !keep);
                    commit();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                    tooltip("Whether the variant shown, '%s', keeps this part", shown_variant_.c_str());
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (asked_of < doc_.parts.size()) {
        const core::FixedString<31> id = doc_.parts[asked_of].id;
        if (ask == Ask::Rename) {
            selected_ = id;
            rename_focus_ = true;
        } else if (ask == Ask::Duplicate) {
            static_cast<void>(duplicate_part(id.view()));
        } else if (ask == Ask::Delete) {
            static_cast<void>(remove_part(id.view()));
        }
    }
    // Adding without dragging: what's chosen in Resources.
    const bool addable = addable_kind(file_kind(resources_file)) && resources_file != path_.view();
    const std::string_view name = file_part(resources_file);
    ImGui::Dummy(ImVec2(0.0f, px(2.0f)));
    ImGui::Indent(pad);
    ImGui::BeginDisabled(!addable);
    if (ImGui::Button("Add Selected File")) {
        static_cast<void>(add_part(resources_file, Vec3(0.0f)));
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (addable) {
            tooltip("Add %.*s, the file chosen in Resources, at the prefab's origin", static_cast<int>(name.size()),
                    name.data());
        } else {
            tooltip("Choose a model, an effect or another prefab in Resources (Ctrl+Space) and this adds it; or drag "
                    "one from there onto the picture or this side.");
        }
    }
    ImGui::Unindent(pad);
    ImGui::Dummy(ImVec2(0.0f, px(6.0f)));
}

void PrefabEditor::inner_parts(const PictureEngine& engine, uint32_t part, uint32_t variant) {
    if (!scene_) {
        return;
    }
    const flecs::world& ecs = scene_->world().ecs();
    const flecs::entity_t id = part < views_.size() ? views_[part].entity : 0;
    const auto* placed = id != 0 && ecs.is_alive(id) ? ecs.entity(id).try_get<world::PrefabInstance>() : nullptr;
    const auto* inner = placed != nullptr ? engine.assets->get<asset::PrefabAsset>(placed->prefab) : nullptr;
    if (inner == nullptr) {
        return;
    }
    const std::span<const asset::PrefabPart> parts = inner->parts.get(inner);
    const uint32_t absent = inner->variant_bit(inner, placed->variant);
    const std::string_view outer = doc_.parts[part].id.view();
    size_t listed = 0;
    for (size_t p = 0; p < parts.size() && listed < kMostInnerListed; ++p) {
        if ((parts[p].absent_in & absent) != 0) {
            continue; // not in the variant of it this part asks for
        }
        const std::string_view name = inner->name(inner, parts[p]);
        core::FixedString<127> path(outer);
        if (!path.append('/') || !path.append(name)) {
            continue;
        }
        const bool exact = doc_.leaves_out(variant, path.view());
        const bool above = !exact && std::ranges::any_of(doc_.left_out, [&](const PrefabDocLeftOut& gone) {
            return gone.variant == variant && covers(gone.path.view(), path.view());
        });
        char label[96];
        static_cast<void>(std::snprintf(label, sizeof(label), "%.*s", static_cast<int>(name.size()), name.data()));
        ImGui::PushID(static_cast<int>(p));
        const PropertyRow row =
            begin_property_row(label, 1, nullptr, exact || above ? ThemeColor::TextDisabled : ThemeColor::Text);
        bool keep = !exact && !above;
        ImGui::BeginDisabled(above);
        if (compact_checkbox(keep)) {
            doc_.leave_out(variant, path.view(), !keep);
            commit();
        }
        ImGui::EndDisabled();
        if (above && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            tooltip("Left out with what it's inside of");
        }
        end_property_row(row);
        ImGui::PopID();
        ++listed;
    }
}

void PrefabEditor::part_properties(const PictureEngine& engine) {
    if (!scene_) {
        return;
    }
    const flecs::world& ecs = scene_->world().ecs(); // by one name from here on, whatever is called in between
    const uint32_t index = selected_index();
    if (index == UINT32_MAX) {
        if (!doc_.parts.empty()) {
            section_header("Part");
            ImGui::Indent(px(8.0f));
            secondary_text("Choose a part, in the list or the picture, to change it.");
            ImGui::Unindent(px(8.0f));
            ImGui::Dummy(ImVec2(0.0f, px(6.0f)));
        }
        return;
    }
    section_header("Part");
    PrefabDocPart& part = doc_.parts[index];
    const PartView view = index < views_.size() ? views_[index] : PartView{};
    bool changed = false; // at once: a step of its own
    bool light_edited = false;
    bool light_ended = false;

    // Its ID: typed, taken when the field is left or Enter pressed.
    if (id_of_.view() != part.id.view()) {
        id_of_ = part.id;
        static_cast<void>(std::snprintf(id_text_, sizeof(id_text_), "%s", part.id.c_str()));
    }
    {
        const PropertyRow row = begin_property_row("ID");
        ImGui::SetNextItemWidth(property_value_width(row));
        if (rename_focus_) {
            ImGui::SetKeyboardFocusHere();
            rename_focus_ = false;
        }
        const bool entered = ImGui::InputText("##id", id_text_, sizeof(id_text_),
                                              ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsNoBlank |
                                                  ImGuiInputTextFlags_AutoSelectAll);
        if ((entered || ImGui::IsItemDeactivatedAfterEdit()) && std::string_view(id_text_) != part.id.view()) {
            const core::FixedString<31> was = part.id;
            if (!rename_part(was.view(), id_text_)) {
                id_of_.clear(); // the field shows what it is again
            }
        }
        if (property_label_hovered(row)) {
            tooltip("What the part is known by: in variants, and by what's saved of a placed prefab. a-z, 0-9 and _.");
        }
        end_property_row(row);
    }

    // What it's made from: each a file, shown in Resources by a click, replaced by dropping another on its row.
    const auto source_row = [&](const char* label, core::FixedString<255>& value, std::string_view kind, bool missing,
                                bool removable) {
        ImGui::PushID(label);
        const PropertyRow row = begin_property_row(label);
        const std::string_view relative = value.view();
        if (relative.empty()) {
            value_text(row, "None: drop one here", ThemeColor::TextDisabled);
        } else {
            const std::string_view file = file_part(relative);
            char name[160];
            static_cast<void>(std::snprintf(name, sizeof(name), "%.*s", static_cast<int>(file.size()), file.data()));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetStyle().FramePadding.x);
            ImGui::AlignTextToFramePadding();
            if (missing) {
                ImGui::PushStyleColor(ImGuiCol_TextLink, color(ThemeColor::Warning));
            }
            if (ImGui::TextLink(name)) {
                char full[platform::kMaxPathLength];
                size_t length = 0;
                if (joined_path(folder_.view(), relative, full, length)) {
                    static_cast<void>(show_file_.assign(std::string_view(full, length)));
                }
            }
            if (missing) {
                ImGui::PopStyleColor();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                tooltip(missing ? "%.*s\nIt isn't there, or isn't cooked yet." : "%.*s\nShow it in Resources.",
                        static_cast<int>(relative.size()), relative.data());
            }
            if (removable && remove_button(row, "Take it off the part")) {
                value.clear();
                changed = true;
            }
        }
        const ImRect area(row.min, ImVec2(row.min.x + row.width, row.min.y + row.height));
        if (ImGui::BeginDragDropTargetCustom(area, ImGui::GetID("##drop"))) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(AssetBrowser::kSourceFilePayload)) {
                const std::string_view dropped = dropped_file(payload);
                char made[256];
                size_t length = 0;
                if (file_kind(dropped) != kind || dropped == path_.view()) {
                    say("That row takes %.*s", kind == "Model"    ? std::string_view("a model")
                                               : kind == "Effect" ? std::string_view("an effect")
                                                                  : std::string_view("another prefab"));
                } else if (relative_path(folder_.view(), dropped, made, length) &&
                           value.assign(std::string_view(made, length))) {
                    changed = true;
                }
            }
            ImGui::EndDragDropTarget();
        }
        end_property_row(row);
        ImGui::PopID();
    };
    if (!part.prefab.empty()) {
        source_row("Prefab", part.prefab, "Prefab", view.prefab_missing, false);
        // Which of that prefab's variants it places.
        const auto* placed = view.entity != 0 && ecs.is_alive(view.entity)
                                 ? ecs.entity(view.entity).try_get<world::PrefabInstance>()
                                 : nullptr;
        const auto* inner = placed != nullptr ? engine.assets->get<asset::PrefabAsset>(placed->prefab) : nullptr;
        const PropertyRow row = begin_property_row("Variant");
        const bool none = inner == nullptr || inner->variants.get(inner).empty();
        ImGui::BeginDisabled(none && part.variant.empty());
        if (begin_combo("##variant",
                        !part.variant.empty() ? part.variant.c_str()
                        : none                ? "None (it has no variants)"
                                              : "None",
                        property_value_width(row))) {
            if (ImGui::Selectable("None", part.variant.empty())) {
                part.variant.clear();
                changed = true;
            }
            for (const asset::PrefabVariant& variant :
                 inner != nullptr ? inner->variants.get(inner) : std::span<const asset::PrefabVariant>()) {
                const std::string_view name = inner->name(inner, variant);
                char label[48];
                static_cast<void>(
                    std::snprintf(label, sizeof(label), "%.*s", static_cast<int>(name.size()), name.data()));
                if (ImGui::Selectable(label, part.variant.view() == name) && part.variant.assign(name)) {
                    changed = true;
                }
            }
            end_combo();
        }
        ImGui::EndDisabled();
        if (property_label_hovered(row)) {
            tooltip("Which of that prefab's variants this part places: the same prefab with some of its parts left "
                    "out.");
        }
        end_property_row(row);
    } else {
        source_row("Model", part.mesh, "Model", view.mesh_missing, !part.effect.empty() || part.has_light);
        {
            const PropertyRow row = begin_property_row("Solid");
            bool solid = part.collision;
            ImGui::BeginDisabled(part.mesh.empty());
            if (compact_checkbox(solid)) {
                part.collision = solid;
                changed = true;
            }
            ImGui::EndDisabled();
            if (property_label_hovered(row)) {
                tooltip("A static body in the model's shape: things collide with it. Needs a model.");
            }
            end_property_row(row);
        }
        source_row("Effect", part.effect, "Effect", view.effect_missing, !part.mesh.empty() || part.has_light);
        if (part.mesh.empty()) {
            part.collision = false;
        }
        // Its light: there or not, and what it is.
        {
            ImGui::PushID("light");
            const PropertyRow row = begin_property_row("Light");
            bool lit = part.has_light;
            if (compact_checkbox(lit)) {
                part.has_light = lit;
                changed = true;
            }
            if (property_label_hovered(row)) {
                tooltip("A local light where the part is: a point that shines every way, or with a cone a spot along "
                        "the part's -Z. It has no shadows yet.");
            }
            end_property_row(row);
            ImGui::PopID();
        }
        if (part.has_light) {
            asset::LightDesc& light = part.light;
            {
                ImGui::PushID("light_color");
                const PropertyRow row = begin_property_row("Color", 1);
                ImGui::SetNextItemWidth(property_value_width(row));
                if (ImGui::ColorEdit3("##color", light.color, ImGuiColorEditFlags_Float)) {
                    light_edited = true;
                }
                light_ended = light_ended || ImGui::IsItemDeactivatedAfterEdit();
                end_property_row(row);
                ImGui::PopID();
            }
            // A number of the light's, in its range.
            const auto number = [&](const char* label, float& value, float speed, float low, float high,
                                    const char* format, const char* tip) {
                ImGui::PushID(label);
                const PropertyRow row = begin_property_row(label, 1);
                ImGui::SetNextItemWidth(property_value_width(row));
                float edited = value;
                if (ImGui::DragFloat("##number", &edited, speed, low, high, format, ImGuiSliderFlags_AlwaysClamp) &&
                    std::isfinite(edited)) {
                    value = std::clamp(edited, low, high);
                    light_edited = true;
                }
                light_ended = light_ended || ImGui::IsItemDeactivatedAfterEdit();
                if (property_label_hovered(row)) {
                    tooltip("%s", tip);
                }
                end_property_row(row);
                ImGui::PopID();
            };
            number("Intensity", light.intensity, 0.02f, 0.0f, asset::LightLimits::kMaxIntensity, "%.3f",
                   "What it gives a surface facing it from 1 m, in the sun's units: a light of 5 lights a wall a "
                   "metre away as a sun of 5 does. It falls with the square of the distance.");
            number("Radius", light.radius_m, 0.05f, 0.05f, asset::LightLimits::kMaxRadiusM, "%.2f m",
                   "Where its light has come down to nothing. Keep it as small as looks right: it's what the light "
                   "costs by.");
            number("Cone", light.cone_deg, 0.5f, 0.0f, asset::LightLimits::kMaxConeDeg, "%.1f deg",
                   "0: a point light. Else a spot along the part's -Z: the angle from its axis to where it ends.");
            if (light.cone_deg > 0.0f) {
                number("Cone edge", light.cone_soft, 0.01f, 0.0f, 1.0f, "%.2f",
                       "How much of the cone's angle is the fade at its edge: 0 a hard edge, 1 all of it.");
            }
            number("Flicker", light.flicker, 0.01f, 0.0f, 1.0f, "%.2f",
                   "0: steady. Else the share of its light a flicker takes away at most, as a flame's.");
            if (light.flicker > 0.0f) {
                number("Flicker rate", light.flicker_hz, 0.1f, 0.0f, asset::LightLimits::kMaxFlickerHz, "%.1f Hz",
                       "How fast it flickers.");
            }
            number("Shadow", light.shadow, 0.01f, 0.0f, 1.0f, "%.2f",
                   "How much of a shadow it casts: 1 all of it, 0 none. A light's shadow is most of what it costs: of "
                   "lights that stand together, one casting is nearly the shadow of all.");
        }
    }

    // Where it is, from the prefab's origin.
    bool ended = false;
    bool moved = false;
    {
        ImGui::PushID("position");
        const PropertyRow row = begin_property_row("Position");
        Vec3 position = part.position;
        if (vec3_fields(position, 0.01f, property_value_width(row), ended)) {
            part.position = position;
            moved = true;
        }
        if (property_label_hovered(row)) {
            tooltip("Meters from the prefab's origin");
        }
        end_property_row(row);
        ImGui::PopID();
    }
    {
        ImGui::PushID("rotation");
        const PropertyRow row = begin_property_row("Rotation");
        Vec3 degrees = part.rotation_deg;
        if (vec3_fields(degrees, 0.5f, property_value_width(row), ended)) {
            part.set_rotation_deg(degrees);
            moved = true;
        }
        if (property_label_hovered(row)) {
            tooltip("Degrees about X, Y and Z: turned about Y first, then X, then Z");
        }
        end_property_row(row);
        ImGui::PopID();
    }
    {
        ImGui::PushID("scale");
        const PropertyRow row = begin_property_row("Scale");
        Vec3 scale = part.scale;
        if (vec3_fields(scale, 0.01f, property_value_width(row), ended)) {
            for (int axis = 0; axis < 3; ++axis) {
                scale[axis] = scale[axis] == 0.0f ? 0.001f : scale[axis]; // never nothing
            }
            part.scale = scale;
            moved = true;
        }
        if (property_label_hovered(row)) {
            tooltip("Times its size, along its own X, Y and Z");
        }
        end_property_row(row);
        ImGui::PopID();
    }
    field_edited_ = field_edited_ || moved || light_edited;
    ended = ended || light_ended;
    if (changed || (ended && field_edited_)) {
        field_edited_ = false;
        commit();
    }

    // With a variant shown, what it keeps of the prefab this part places.
    if (const uint32_t variant = shown_variant_index(); variant != UINT32_MAX && !part.prefab.empty()) {
        char heading[96];
        static_cast<void>(std::snprintf(heading, sizeof(heading), "Kept in '%s'", shown_variant_.c_str()));
        const PropertyRow row = begin_property_row(heading);
        value_text(row, "of what this part places", ThemeColor::TextSecondary);
        end_property_row(row);
        inner_parts(engine, index, variant);
    }
    ImGui::Dummy(ImVec2(0.0f, px(6.0f)));
}

void PrefabEditor::variants_list() {
    const float pad = px(8.0f);
    const float font = ImGui::GetFontSize();
    section_header("Variants");
    ImGui::Indent(pad);
    ImGui::PushStyleColor(ImGuiCol_Text, color(ThemeColor::TextDisabled));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted("The same prefab with some parts left out. Show one, and the list above ticks what it "
                           "keeps.");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Unindent(pad);
    uint32_t remove = UINT32_MAX;
    if (ImGui::BeginTable("##variants", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("delete", ImGuiTableColumnFlags_WidthFixed, font * 4.2f);
        const auto row_of = [&](const char* label, bool shown, const char* tip) {
            ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight());
            if (shown) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, color_u32(ThemeColor::Selection));
            }
            ImGui::TableSetColumnIndex(0);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                  color(shown ? ThemeColor::SelectionHovered : ThemeColor::Header));
            const bool pressed = ImGui::Selectable(label, shown, ImGuiSelectableFlags_AllowOverlap,
                                                   ImVec2(0.0f, ImGui::GetFrameHeight()));
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                tooltip("%s", tip);
            }
            return pressed;
        };
        ImGui::PushID("all");
        if (row_of("All parts", shown_variant_.empty(), "Show the prefab as it is, with every part")) {
            shown_variant_.clear();
        }
        ImGui::PopID();
        for (uint32_t v = 0; v < doc_.variants.size(); ++v) {
            const PrefabDocVariant& variant = doc_.variants[v];
            ImGui::PushID(static_cast<int>(v));
            if (row_of(variant.name.c_str(), shown_variant_.view() == variant.name.view(),
                       "Show this variant in the picture, and tick what it keeps in the list of parts")) {
                shown_variant_ = variant.name;
            }
            ImGui::TableSetColumnIndex(1);
            if (toggle_chip("Delete", false, "Delete this variant: what places it then gets the whole prefab")) {
                remove = v;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (remove < doc_.variants.size()) {
        const core::FixedString<31> name = doc_.variants[remove].name;
        static_cast<void>(remove_variant(name.view()));
    }
    ImGui::Dummy(ImVec2(0.0f, px(2.0f)));
    ImGui::Indent(pad);
    ImGui::SetNextItemWidth(font * 10.0f);
    const bool entered =
        ImGui::InputTextWithHint("##new_variant", "a new variant's name", variant_text_, sizeof(variant_text_),
                                 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsNoBlank);
    ImGui::SameLine(0.0f, px(4.0f));
    ImGui::BeginDisabled(variant_text_[0] == '\0');
    const bool pressed = ImGui::Button("Add Variant");
    ImGui::EndDisabled();
    if ((entered || pressed) && variant_text_[0] != '\0' && add_variant(variant_text_)) {
        variant_text_[0] = '\0';
    }
    ImGui::Unindent(pad);
    ImGui::Dummy(ImVec2(0.0f, px(6.0f)));
}

void PrefabEditor::side(const PictureEngine& engine, float width, std::string_view resources_file) {
    const float pad = px(8.0f);
    const std::string_view file = file_part(path_.view());
    std::string_view folder = folder_.view();
    if (folder.starts_with("src:/")) {
        folder.remove_prefix(5);
    }
    section_header("Prefab");
    ImGui::Indent(pad);
    char text[200];
    static_cast<void>(std::snprintf(text, sizeof(text), "%.*s", static_cast<int>(file.size()), file.data()));
    ImGui::TextUnformatted(text);
    ImGui::SameLine(0.0f, px(8.0f));
    if (is_new_) {
        ImGui::TextColored(color(ThemeColor::Warning), "new: not a file until it's saved");
    } else if (dirty()) {
        ImGui::TextColored(color(ThemeColor::Warning), "changed: not saved");
    } else {
        ImGui::TextColored(color(ThemeColor::TextDisabled), "saved");
    }
    static_cast<void>(std::snprintf(text, sizeof(text), "%.*s", static_cast<int>(folder.size()), folder.data()));
    secondary_text(text);
    if (!is_new_) {
        if (ImGui::Button("Show in Resources")) {
            show_file_ = path_;
        }
    }
    ImGui::Unindent(pad);
    ImGui::Dummy(ImVec2(0.0f, px(6.0f)));

    parts_list(engine, resources_file);
    part_properties(engine);
    variants_list();

    if (!problems_.empty()) {
        section_header("Problems");
        ImGui::Indent(pad);
        ImGui::PushStyleColor(ImGuiCol_Text, color(ThemeColor::Warning));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - (3.0f * pad));
        for (const core::FixedString<159>& problem : problems_) {
            ImGui::TextUnformatted(problem.c_str());
        }
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Unindent(pad);
        ImGui::Dummy(ImVec2(0.0f, px(6.0f)));
    }
}

void PrefabEditor::unsaved_prompt(const PictureEngine& engine) {
    constexpr const char* kPrompt = "Unsaved prefab";
    if (pending_ == Pending::None) {
        return;
    }
    if (!dirty()) {
        finish_pending(engine); // nothing to ask after all
        return;
    }
    if (!ImGui::IsPopupOpen(kPrompt)) {
        ImGui::OpenPopup(kPrompt);
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(16.0f), px(14.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(8.0f), px(10.0f)));
    if (ImGui::BeginPopupModal(kPrompt, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        const std::string_view file = file_part(path_.view());
        ImGui::Text("Save the changes to %.*s?", static_cast<int>(file.size()), file.data());
        ImGui::TextColored(color(ThemeColor::TextSecondary), "What isn't saved is lost.");
        enum class Choice : uint8_t { None, Save, Discard, Cancel };
        Choice choice = Choice::None;
        const ImVec2 button(px(96.0f), 0.0f);
        if (ImGui::Button("Save", button) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            choice = Choice::Save;
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save", button)) {
            choice = Choice::Discard;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", button) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            choice = Choice::Cancel;
        }
        if (choice != Choice::None) {
            ImGui::CloseCurrentPopup();
            if (choice == Choice::Save && save(engine)) {
                finish_pending(engine);
            } else if (choice == Choice::Discard) {
                saved_step_ = step_; // what it holds is let go of
                finish_pending(engine);
            } else {
                pending_ = Pending::None; // cancelled, or the save failed (the picture says why)
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

void PrefabEditor::ui(const PictureEngine& engine, const char* dock_beside, std::string_view resources_file) {
    unsaved_prompt(engine);
    if (!scene_) {
        return;
    }
    // A tab beside the Playground the first time; wherever it was left after that.
    if (const ImGuiWindow* beside = dock_beside != nullptr ? ImGui::FindWindowByName(dock_beside) : nullptr;
        beside != nullptr && beside->DockId != 0) {
        ImGui::SetNextWindowDockID(beside->DockId, ImGuiCond_FirstUseEver);
    }
    ImGui::SetNextWindowSize(ImVec2(px(1040.0f), px(640.0f)), ImGuiCond_FirstUseEver);
    // Brought forward once the panels exist: asked on the first frames (a flag at startup), the layout that's read
    // back then would put the tab it left in front again.
    if (focus_ && ImGui::GetFrameCount() > 2) {
        ImGui::SetNextWindowFocus();
        focus_ = false;
    }
    bool open = true;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool shown = ImGui::Begin(kWindow, &open,
                                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                        (dirty() ? ImGuiWindowFlags_UnsavedDocument : ImGuiWindowFlags_None));
    ImGui::PopStyleVar();
    if (shown) {
        // Its own keys while it has the keyboard: they'd otherwise be the world's (undo, save, delete).
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
            if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S)) {
                static_cast<void>(save(engine));
            }
            if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_Repeat)) {
                static_cast<void>(undo());
            }
            if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_Repeat) ||
                ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_Repeat)) {
                static_cast<void>(redo());
            }
            if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D) && !selected_.empty()) {
                const core::FixedString<31> id = selected_;
                static_cast<void>(duplicate_part(id.view()));
            }
            if (!ImGui::GetIO().WantTextInput) {
                if (ImGui::Shortcut(ImGuiKey_Delete) && !selected_.empty()) {
                    const core::FixedString<31> id = selected_;
                    static_cast<void>(remove_part(id.view()));
                }
                if (ImGui::Shortcut(ImGuiKey_F)) {
                    measure(engine);
                    frame(true);
                }
                if (ImGui::Shortcut(ImGuiKey_Q)) {
                    tool_ = Tool::Select;
                } else if (ImGui::Shortcut(ImGuiKey_W)) {
                    tool_ = Tool::Move;
                } else if (ImGui::Shortcut(ImGuiKey_E)) {
                    tool_ = Tool::Rotate;
                } else if (ImGui::Shortcut(ImGuiKey_R)) {
                    tool_ = Tool::Scale;
                }
                if (ImGui::Shortcut(ImGuiKey_Escape)) {
                    selected_.clear();
                }
            }
        }
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float side_width = std::min(px(340.0f), avail.x * 0.45f);
        const float view_width = avail.x - side_width;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        if (ImGui::BeginChild("##view", ImVec2(view_width, avail.y), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            // The bar takes as many lines as its chips need at this width; the picture has the rest.
            const float bar_height = view_bar(engine, view_width);
            ImGui::SetCursorPos(ImVec2(0.0f, bar_height));
            picture(ImVec2(view_width, std::max(1.0f, avail.y - bar_height)));
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, color(ThemeColor::Surface));
        if (ImGui::BeginChild("##side", ImVec2(side_width, avail.y))) {
            side(engine, side_width, resources_file);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        // A file let go anywhere on that side is a part too, at the prefab's origin.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(AssetBrowser::kSourceFilePayload)) {
                static_cast<void>(add_part(dropped_file(payload), Vec3(0.0f)));
            }
            ImGui::EndDragDropTarget();
        }
    }
    ImGui::End();
    if (!open) {
        window_open_ = false; // closed by the next update, which asks first if it has changes
    }
}

} // namespace de::editor
