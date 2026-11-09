#include "editor/editor.h"

#include "asset/mesh_format.h"
#include "core/log.h"
#include "core/profile.h"
#include "core/text.h"
#include "editor/outline.h"
#include "editor/source_files.h"
#include "editor/theme.h"
#include "editor/widgets.h"
#include "platform/input.h"
#include "platform/vfs.h"
#include "platform/window.h"
#include "render/scene/scene_renderer.h"
#include "world/render_extract.h"
#include "world/transform.h"
#include "world/world.h"

#include <imgui.h>
#include <imgui_internal.h> // the dock builder

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <system_error>

#if DE_BUILD_DEV
namespace de::editor {

namespace {

constexpr const char* kLayoutPath = "user:/editor/layout.ini";
constexpr const char* kSettingsPath = "user:/editor/editor.settings";
constexpr float kUiScales[] = {0.8f, 0.9f, 1.0f, 1.1f, 1.25f, 1.5f};

// "X,Y": two whole numbers (--pick).
bool parse_pixel(std::string_view text, uint32_t& x, uint32_t& y) {
    const size_t comma = text.find(',');
    if (comma == std::string_view::npos) {
        return false;
    }
    const std::string_view first = text.substr(0, comma);
    const std::string_view second = text.substr(comma + 1);
    const auto [x_end, x_error] = std::from_chars(first.data(), first.data() + first.size(), x);
    const auto [y_end, y_error] = std::from_chars(second.data(), second.data() + second.size(), y);
    return x_error == std::errc() && y_error == std::errc() && x_end == first.data() + first.size() &&
           y_end == second.data() + second.size();
}

core::Allocator& editor_heap() {
    return core::heap_allocator(core::MemoryTag::Editor);
}

void load_layout(const platform::Vfs& vfs) {
    if (!vfs.exists(kLayoutPath)) {
        return; // the first run: the default layout
    }
    auto ini = vfs.read_whole_file(kLayoutPath, editor_heap());
    if (!ini) {
        DE_LOG_WARN("editor", "can't read %s: %s", kLayoutPath, ini.error().message);
        return;
    }
    ImGui::LoadIniSettingsFromMemory(reinterpret_cast<const char*>(ini->data()), ini->size());
}

void save_layout(const platform::Vfs& vfs) {
    size_t size = 0;
    const char* ini = ImGui::SaveIniSettingsToMemory(&size);
    if (const auto written = vfs.write_file(kLayoutPath, std::as_bytes(std::span(ini, size))); !written) {
        DE_LOG_WARN("editor", "can't keep the layout in %s: %s", kLayoutPath, written.error().message);
    }
}

// What the editor keeps between runs: its interface scale, its tool, its axes and its snapping.
struct Settings {
    float ui_scale = 1.0f;
    Tool tool = Tool::Move;
    Space space = Space::World;
    HandlesAt handles_at = HandlesAt::Pivot;
    Snap snap;
};

float to_number(std::string_view text, float fallback) {
    char number[32] = {};
    std::memcpy(number, text.data(), std::min(text.size(), sizeof(number) - 1));
    char* end = nullptr;
    const float value = std::strtof(number, &end);
    return end != number ? value : fallback;
}

Settings load_settings(const platform::Vfs& vfs) {
    Settings settings;
    if (!vfs.exists(kSettingsPath)) {
        return settings;
    }
    auto file = vfs.read_whole_file(kSettingsPath, editor_heap());
    if (!file) {
        return settings;
    }
    std::string_view text = core::skip_bom(std::string_view(reinterpret_cast<const char*>(file->data()), file->size()));
    while (!text.empty()) {
        const std::string_view line = core::strip_comment(core::next_line(text));
        const size_t space = line.find(' ');
        const std::string_view key = line.substr(0, space);
        const std::string_view value =
            space == std::string_view::npos ? std::string_view() : core::trim(line.substr(space));
        // "on 0.5": the switch, then the step.
        const bool on = value.starts_with("on");
        const size_t step_at = value.find(' ');
        const std::string_view step =
            step_at == std::string_view::npos ? std::string_view() : value.substr(step_at + 1);
        if (key == "ui_scale") {
            const float scale = to_number(value, 1.0f);
            settings.ui_scale = scale >= 0.5f && scale <= 3.0f ? scale : 1.0f;
        } else if (key == "tool") {
            settings.tool = value == "select"   ? Tool::Select
                            : value == "rotate" ? Tool::Rotate
                            : value == "scale"  ? Tool::Scale
                                                : Tool::Move;
        } else if (key == "space") {
            settings.space = value == "local" ? Space::Local : Space::World;
        } else if (key == "handles") {
            settings.handles_at = value == "center" ? HandlesAt::Center : HandlesAt::Pivot;
        } else if (key == "snap_move") {
            settings.snap.move = on;
            settings.snap.move_m = std::max(0.001f, to_number(step, settings.snap.move_m));
        } else if (key == "snap_rotate") {
            settings.snap.rotate = on;
            settings.snap.rotate_deg = std::max(0.01f, to_number(step, settings.snap.rotate_deg));
        } else if (key == "snap_scale") {
            settings.snap.scale = on;
            settings.snap.scale_step = std::max(0.001f, to_number(step, settings.snap.scale_step));
        }
    }
    return settings;
}

void save_settings(const platform::Vfs& vfs, const Settings& settings) {
    constexpr const char* kTools[] = {"select", "move", "rotate", "scale"};
    char text[512];
    const int length =
        std::snprintf(text, sizeof(text),
                      "# The editor's interface scale (on top of the display's), its tool, its axes, where its handles "
                      "sit and its snapping.\n"
                      "ui_scale %.2f\ntool %s\nspace %s\nhandles %s\nsnap_move %s %g\nsnap_rotate %s %g\n"
                      "snap_scale %s %g\n",
                      static_cast<double>(settings.ui_scale), kTools[static_cast<int>(settings.tool)],
                      settings.space == Space::Local ? "local" : "world",
                      settings.handles_at == HandlesAt::Center ? "center" : "pivot", settings.snap.move ? "on" : "off",
                      static_cast<double>(settings.snap.move_m), settings.snap.rotate ? "on" : "off",
                      static_cast<double>(settings.snap.rotate_deg), settings.snap.scale ? "on" : "off",
                      static_cast<double>(settings.snap.scale_step));
    if (length > 0) {
        static_cast<void>(vfs.write_file(kSettingsPath, std::as_bytes(std::span(text, static_cast<size_t>(length)))));
    }
}

// A square toolbar button with an icon: the accent while on, a step lighter under the mouse.
bool icon_button(const IconSet& icons, const char* id, Icon icon, const char* tip, bool on, bool enabled = true) {
    const float extent = ImGui::GetFrameHeight();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(extent, extent)) && enabled;
    const bool hovered = enabled && ImGui::IsItemHovered();
    const bool held = enabled && ImGui::IsItemActive();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (on || hovered || held) {
        const ThemeColor background = on ? (hovered ? ThemeColor::AccentHovered : ThemeColor::Accent)
                                         : (held ? ThemeColor::ControlActive : ThemeColor::ControlHovered);
        draw->AddRectFilled(min, ImVec2(min.x + extent, min.y + extent), color_u32(background),
                            px(theme().metrics.control_rounding));
    }
    const ThemeColor ink = !enabled        ? ThemeColor::TextDisabled
                           : on || hovered ? ThemeColor::TextEmphasis
                                           : ThemeColor::Text;
    const float size = std::floor(extent * 0.72f);
    const float inset = std::floor((extent - size) * 0.5f);
    icons.draw(draw, icon, ImVec2(min.x + inset, min.y + inset), size, color_u32(ink));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        tooltip("%s", tip);
    }
    return pressed;
}

// A thin rule between toolbar groups.
void toolbar_rule() {
    ImGui::SameLine(0.0f, px(6.0f));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float inset = px(3.0f);
    const float width = std::max(1.0f, px(1.0f));
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x, at.y + inset),
                                              ImVec2(at.x + width, at.y + ImGui::GetFrameHeight() - inset),
                                              color_u32(ThemeColor::Control));
    ImGui::Dummy(ImVec2(width, ImGui::GetFrameHeight()));
    ImGui::SameLine(0.0f, px(6.0f));
}

// A snap's step, as a short list beside its switch.
template <size_t N>
void snap_step(const char* id, float& step, const float (&steps)[N], const char* unit) {
    char preview[24];
    static_cast<void>(std::snprintf(preview, sizeof(preview), "%g%s", static_cast<double>(step), unit));
    if (begin_combo(id, preview, px(62.0f))) {
        for (const float option : steps) {
            char label[24];
            static_cast<void>(std::snprintf(label, sizeof(label), "%g%s", static_cast<double>(option), unit));
            if (ImGui::Selectable(label, std::abs(option - step) < 1e-6f)) {
                step = option;
            }
        }
        end_combo();
    }
}

} // namespace

Editor::Editor()
    : commands_(editor_heap()), hierarchy_(editor_heap()), icons_(editor_heap()), selected_meshes_(editor_heap()),
      selected_entities_(editor_heap()), properties_(editor_heap()), browser_(editor_heap()) {}

bool Editor::startup(app::Engine& engine, const app::FrameworkArgs& args) {
    if (!viewer_.startup(engine, args)) {
        return false;
    }
    viewer_.set_quit_on_escape(false);
    engine.frame_stats = false; // F1, or View > Frame Stats
    if (!load_fonts(engine.vfs)) {
        DE_LOG_WARN("editor", "the editor's fonts didn't load: ImGui's own stands in");
    }
    if (!icons_.load(engine.vfs)) {
        DE_LOG_WARN("editor", "some of the editor's icons didn't load: their buttons show empty");
    }
    const Settings settings = load_settings(engine.vfs);
    ui_scale_ = settings.ui_scale;
    tool_ = settings.tool;
    space_ = settings.space;
    handles_at_ = settings.handles_at;
    snap_ = settings.snap;
    select_name_ = app::flag_value(args, "--select");
    show_panel_ = app::flag_value(args, "--show");
    if (const char* file = app::flag_value(args, "--select-file")) {
        selection_.select_file(file);
    }
    if (const char* tool = app::flag_value(args, "--tool")) {
        const std::string_view name(tool);
        tool_ = name == "select"   ? Tool::Select
                : name == "rotate" ? Tool::Rotate
                : name == "scale"  ? Tool::Scale
                                   : Tool::Move;
    }
    if (const char* handles = app::flag_value(args, "--handles")) {
        handles_at_ = std::string_view(handles) == "center" ? HandlesAt::Center : HandlesAt::Pivot;
    }
    if (const char* drag = app::flag_value(args, "--drag")) {
        const std::string_view text(drag);
        const size_t comma = text.find(',');
        const std::string_view first = text.substr(0, comma);
        int handle = -1;
        const std::from_chars_result parsed = std::from_chars(first.data(), first.data() + first.size(), handle);
        if (comma == std::string_view::npos || parsed.ec != std::errc() || parsed.ptr != first.data() + first.size() ||
            handle < 0 || handle > Gizmo::kMiddle) {
            DE_LOG_ERROR("editor", "--drag: '%s' isn't HANDLE,AMOUNT", drag);
            return false;
        }
        gizmo_.show_drag(handle, to_number(text.substr(comma + 1), 0.0f));
    }
    keep_settings_ = !engine.scripted; // a picture's flags aren't the user's choices
    if (const char* pixel = app::flag_value(args, "--pick")) {
        if (!parse_pixel(pixel, pick_flag_x_, pick_flag_y_)) {
            DE_LOG_ERROR("editor", "--pick: '%s' isn't a pixel (X,Y)", pixel);
            return false;
        }
        pick_flag_ = true;
    }
    browser_.load_settings(engine.vfs);
    load_layout(engine.vfs);
    DE_LOG_INFO("editor", "editing world %s", asset::format_asset_id(viewer_.world_id()).c_str());
    return true;
}

void Editor::shutdown(app::Engine& engine) {
    if (keep_settings_) {
        save_layout(engine.vfs);
        browser_.save_settings(engine.vfs);
        save_settings(
            engine.vfs,
            {.ui_scale = ui_scale_, .tool = tool_, .space = space_, .handles_at = handles_at_, .snap = snap_});
    }
    viewer_.shutdown(engine);
}

bool Editor::update(app::Engine& engine, float frame_s) {
    DE_PROFILE_ZONE("Editor::update");
    // The camera reads input while the right button, pressed over the Playground, is held, wherever the mouse goes:
    // the keys are the tools' the rest of the time.
    const platform::ActionState& look = engine.input.state(viewer_.camera_actions().look_hold);
    if (!look.down) {
        looking_ = false;
    } else if (look.pressed && playground_hovered_ && !ImGui::GetIO().KeyAlt) { // Alt and the right button zoom
        looking_ = true;
    }
    viewer_.set_camera_input(looking_);
    return viewer_.update(engine, frame_s) && !quit_;
}

void Editor::ui(app::Engine& engine) {
    DE_PROFILE_ZONE("Editor::ui");
    // The style at the user's scale times the display's: again whenever either changes (another monitor).
    const float display = engine.window.content_scale();
    if (ui_scale_ != applied_ui_scale_ || display != applied_display_) {
        apply_style(ui_scale_, display);
        applied_ui_scale_ = ui_scale_;
        applied_display_ = display;
    }

    // The editor's window: the whole viewport, the chrome colour showing through the gaps between panels.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float header = header_row_height();
    const float header_padding = std::max(0.0f, (header - ImGui::GetFontSize()) * 0.5f);
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    // The menu bar takes its height from the frame padding at Begin: one header row.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(8.0f), header_padding));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, color(ThemeColor::Chrome));
    constexpr ImGuiWindowFlags kHost =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##editor", nullptr, kHost);
    ImGui::PopStyleColor();
    header_row(engine);
    ImGui::PopStyleVar();
    const float gap = px(theme().metrics.panel_gap);
    const float status = toolbar_height();
    const ImVec2 origin = ImGui::GetCursorPos();
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float body = std::max(1.0f, available.y - status - (gap * 2.0f));
    ImGui::SetCursorPos(ImVec2(origin.x + gap, origin.y + gap));
    dock_space(ImVec2(std::max(1.0f, available.x - (gap * 2.0f)), body));
    ImGui::SetCursorPos(ImVec2(origin.x, origin.y + gap + body + gap));
    status_bar(ImVec2(available.x, status));
    ImGui::End();
    ImGui::PopStyleVar(3);

    take_pick(engine);
    if (select_name_ != nullptr) {
        // --select: the first entity of that name, once its cell has spawned it.
        char label[128];
        engine.world.ecs().each([&](flecs::entity entity, const world::Transform&) {
            if (select_name_ != nullptr) {
                entity_label(engine.assets, entity, label);
                if (std::strcmp(label, select_name_) == 0) {
                    selection_.select_entity(entity.id());
                    select_name_ = nullptr;
                }
            }
        });
    }
    hierarchy_.draw(engine, selection_, viewer_.camera_entity().id());
    DVec3 go_to(0.0);
    if (cell_map_.draw(engine, selection_, go_to)) {
        go_to_place(go_to);
    }
    const bool open_asked = properties_.draw(engine, selection_, commands_);
    const bool opened = browser_.draw(engine.vfs, selection_);
    gather_selection(engine);
    playground(engine);
    shortcuts(engine);
    if (show_panel_ != nullptr && ImGui::GetFrameCount() > 2) { // once the panels exist
        ImGui::SetWindowFocus(show_panel_);
        show_panel_ = nullptr;
    }
    if ((open_asked || opened) && selection_.kind == Selection::Kind::File &&
        file_kind(selection_.file.view()) == "World") {
        open_world(engine);
    }
    if (selection_.frame_entity) {
        selection_.frame_entity = false;
        frame_selection(engine);
    }
    if (pick_flag_ && picks_waiting_ == 0 && engine.viewport.width > 0 && engine.viewport.height > 0) {
        ask_pick(engine, std::min(pick_flag_x_, engine.viewport.width - 1),
                 std::min(pick_flag_y_, engine.viewport.height - 1)); // again if a frame drops it
    }
    send_selection(engine);
}

void Editor::ask_pick(app::Engine& engine, uint32_t x, uint32_t y) {
    engine.editor_overlay.pick = true;
    engine.editor_overlay.pick_x = x;
    engine.editor_overlay.pick_y = y;
    ++picks_waiting_;
    pick_wait_frames_ = 0;
}

void Editor::take_pick(app::Engine& engine) {
    const render::PickAnswer answer = engine.pick_answer;
    if (answer.count == pick_answers_) {
        // A frame that wasn't drawn (minimized, the scene's size changing) never answers: the click did nothing.
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
    pick_flag_ = false;
    // The slot's entity; nothing there (the sky, the ground) selects nothing. A search, but only once per click.
    flecs::entity_t picked = 0;
    if (answer.slot != render::kNothingPicked) {
        engine.world.ecs().each([&](flecs::entity entity, const world::RenderSlot& slot) {
            if (slot.slot == answer.slot) {
                picked = entity.id();
            }
        });
    }
    selection_.select_entity(picked);
}

void Editor::gather_selection(app::Engine& engine) {
    const flecs::world& ecs = engine.world.ecs();
    const flecs::entity_t root =
        selection_.kind == Selection::Kind::Entity && ecs.is_alive(selection_.entity) ? selection_.entity : 0;
    gathered_s_ += ImGui::GetIO().DeltaTime;
    if (root == gathered_for_ && gathered_s_ < 0.25f) {
        return;
    }
    gathered_for_ = root;
    gathered_s_ = 0.0f;
    selected_meshes_.clear();
    selected_entities_.clear();
    if (root == 0) {
        return;
    }
    // It, and whatever hangs from it at any depth (flecs::Parent or ChildOf).
    ecs.each([&](flecs::entity entity, const world::MeshInstance& mesh, const world::RenderSlot& slot) {
        flecs::entity at = entity;
        for (uint32_t depth = 0; depth < 64 && at.is_valid(); ++depth) {
            if (at.id() == root) {
                selected_meshes_.push({.slot = slot.slot, .mesh = mesh.mesh});
                selected_entities_.push(entity.id());
                return;
            }
            const auto* attached = at.try_get<flecs::Parent>();
            at = attached != nullptr ? ecs.entity(attached->value) : at.parent();
        }
    });
}

DVec3 Editor::handles_origin(const app::Engine& engine, flecs::entity selected) const {
    const auto* place = selected.is_valid() ? selected.try_get<world::WorldTransform>() : nullptr;
    if (place == nullptr) {
        return DVec3(0.0);
    }
    if (handles_at_ == HandlesAt::Pivot) {
        return place->position;
    }
    // The centre of the box round its geometry and what's attached to it, as placed now; its pivot if it has none.
    const flecs::world& ecs = engine.world.ecs();
    DVec3 low(std::numeric_limits<double>::max());
    DVec3 high(std::numeric_limits<double>::lowest());
    bool any = false;
    for (const flecs::entity_t id : selected_entities_) {
        if (!ecs.is_alive(id)) {
            continue;
        }
        const flecs::entity entity = ecs.entity(id);
        const auto* instance = entity.try_get<world::MeshInstance>();
        const auto* at = entity.try_get<world::WorldTransform>();
        const auto* mesh = instance != nullptr ? engine.assets.get<asset::MeshAsset>(instance->mesh) : nullptr;
        if (at == nullptr || mesh == nullptr) {
            continue;
        }
        for (uint32_t c = 0; c < 8; ++c) {
            const Vec3 corner((c & 1U) != 0 ? mesh->bounds.max[0] : mesh->bounds.min[0],
                              (c & 2U) != 0 ? mesh->bounds.max[1] : mesh->bounds.min[1],
                              (c & 4U) != 0 ? mesh->bounds.max[2] : mesh->bounds.min[2]);
            const DVec3 point = at->position + DVec3(at->rotation * (at->scale * corner));
            low = glm::min(low, point);
            high = glm::max(high, point);
        }
        any = true;
    }
    return any ? (low + high) * 0.5 : place->position;
}

void Editor::send_selection(app::Engine& engine) {
    engine.editor_overlay.enabled = true;
    engine.editor_overlay.selected = selected_meshes_.span();
    engine.editor_overlay.outline_px = std::max(2.0f, 2.0f * engine.window.content_scale());
}

void Editor::header_row(app::Engine& engine) {
    if (!ImGui::BeginMenuBar()) {
        return;
    }
    const flecs::world& ecs = engine.world.ecs();
    const ImVec2 row_min = ImGui::GetWindowPos();
    const float height = header_row_height();
    // The name, then the menus.
    {
        constexpr const char* kBrand = "DEENGINE";
        const float inset = px(12.0f);
        if (fonts().semibold != nullptr) {
            ImGui::PushFont(fonts().semibold, 0.0f);
        }
        const ImVec2 size = ImGui::CalcTextSize(kBrand);
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(row_min.x + inset, std::floor(row_min.y + ((height - size.y) * 0.5f))),
            color_u32(ThemeColor::TextEmphasis), kBrand);
        if (fonts().semibold != nullptr) {
            ImGui::PopFont();
        }
        ImGui::SetCursorScreenPos(ImVec2(row_min.x + inset + size.x + px(16.0f), ImGui::GetCursorScreenPos().y));
    }
    // The menus' popups mustn't take the header's zero padding.
    push_menu_style();
    prepare_menu_popup();
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Quit")) {
            quit_ = true;
        }
        ImGui::EndMenu();
    }
    prepare_menu_popup();
    if (ImGui::BeginMenu("Edit")) {
        char undo[128];
        char redo[128];
        const char* undo_name = commands_.undo_name(ecs);
        const char* redo_name = commands_.redo_name(ecs);
        static_cast<void>(std::snprintf(undo, sizeof(undo), "Undo %s", undo_name != nullptr ? undo_name : ""));
        static_cast<void>(std::snprintf(redo, sizeof(redo), "Redo %s", redo_name != nullptr ? redo_name : ""));
        if (ImGui::MenuItem(undo, "Ctrl+Z", false, commands_.can_undo())) {
            static_cast<void>(commands_.undo(ecs));
        }
        if (ImGui::MenuItem(redo, "Ctrl+Y", false, commands_.can_redo())) {
            static_cast<void>(commands_.redo(ecs));
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Frame Selected", "F", false, selection_.kind == Selection::Kind::Entity)) {
            selection_.frame_entity = true;
        }
        ImGui::EndMenu();
    }
    prepare_menu_popup();
    if (ImGui::BeginMenu("View")) {
        menu_section("Scene");
        for (uint32_t i = 0; i < static_cast<uint32_t>(render::SceneView::Count); ++i) {
            const auto view = static_cast<render::SceneView>(i);
            if (ImGui::MenuItem(render::scene_view_name(view), nullptr, engine.view == view)) {
                engine.view = view;
            }
        }
        ImGui::Separator();
        ImGui::MenuItem("Temporal Anti-Aliasing", "F3", &engine.taa);
        ImGui::MenuItem("Frame Stats", "F1", &engine.frame_stats);
        menu_section("Interface Scale");
        for (const float scale : kUiScales) {
            char label[16];
            static_cast<void>(std::snprintf(label, sizeof(label), "%.0f%%", static_cast<double>(scale * 100.0f)));
            if (ImGui::MenuItem(label, nullptr, std::abs(ui_scale_ - scale) < 0.001f)) {
                ui_scale_ = scale;
            }
        }
        ImGui::EndMenu();
    }
    prepare_menu_popup();
    if (ImGui::BeginMenu("Window")) {
        if (ImGui::MenuItem("Reset Layout")) {
            reset_layout_ = true;
        }
        ImGui::EndMenu();
    }
    pop_menu_style();
    ImGui::EndMenuBar();
}

void Editor::dock_space(ImVec2 size) {
    // Named with the default layout's version: a new default replaces layouts kept from the old one.
    const ImGuiID dock = ImGui::GetID("##dock 3");
    if (reset_layout_ || ImGui::DockBuilderGetNode(dock) == nullptr) {
        // Left: the hierarchy (the cell map in a tab beside it) over the properties. Right: the Playground over the
        // resources.
        reset_layout_ = false;
        ImGui::DockBuilderRemoveNode(dock);
        ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dock, size);
        ImGuiID left = 0;
        ImGuiID right = 0;
        ImGui::DockBuilderSplitNode(dock, ImGuiDir_Left, 0.26f, &left, &right);
        ImGuiID hierarchy = 0;
        ImGuiID properties = 0;
        ImGui::DockBuilderSplitNode(left, ImGuiDir_Up, 0.45f, &hierarchy, &properties);
        ImGuiID resources = 0;
        ImGuiID playground = 0;
        ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.36f, &resources, &playground);
        ImGui::DockBuilderDockWindow(HierarchyPanel::kWindow, hierarchy);
        ImGui::DockBuilderDockWindow(CellMapPanel::kWindow, hierarchy);
        ImGui::DockBuilderDockWindow(PropertiesPanel::kWindow, properties);
        ImGui::DockBuilderDockWindow(kPlayground, playground);
        ImGui::DockBuilderDockWindow(AssetBrowser::kWindow, resources);
        ImGui::DockBuilderFinish(dock);
    }
    ImGui::DockSpace(dock, size, ImGuiDockNodeFlags_NoWindowMenuButton);
}

void Editor::status_bar(ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, color(ThemeColor::Surface));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(px(10.0f), std::max(0.0f, (size.y - ImGui::GetTextLineHeight()) * 0.5f)));
    if (ImGui::BeginChild("##status", size, ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        // What the focused panel reports, or the editor's last word; the camera's speed at the right.
        const char* text = hierarchy_.focused()  ? hierarchy_.status()
                           : cell_map_.focused() ? cell_map_.status()
                           : browser_.focused()  ? browser_.status()
                                                 : message_;
        ImGui::TextColored(color(ThemeColor::TextSecondary), "%s", text);
        char speed[48];
        static_cast<void>(
            std::snprintf(speed, sizeof(speed), "Camera %.1f m/s", static_cast<double>(viewer_.camera().speed_mps)));
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - ImGui::CalcTextSize(speed).x -
                                                             ImGui::GetStyle().WindowPadding.x));
        ImGui::TextColored(color(ThemeColor::TextSecondary), "%s", speed);
        if (ImGui::IsItemHovered()) {
            tooltip("The mouse wheel over the Playground changes it");
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

View Editor::playground_view(ImVec2 min, ImVec2 size) const {
    float fov_rad = 1.0f;
    if (const auto* camera = viewer_.camera_entity().try_get<world::Camera>()) {
        fov_rad = camera->vertical_fov_rad;
    }
    return {.position = viewer_.camera().position(),
            .rotation = viewer_.camera().rotation(),
            .vertical_fov_rad = fov_rad,
            .min = min,
            .size = size};
}

void Editor::playground_toolbar(app::Engine& engine) {
    // On the panel surface, above the scene: what the mouse does to it on the left, how it's seen on the right.
    const float height = toolbar_height();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, color(ThemeColor::Surface));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(px(6.0f), std::max(0.0f, (height - ImGui::GetFrameHeight()) * 0.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(2.0f), 0.0f));
    if (ImGui::BeginChild("##playground_toolbar", ImVec2(0.0f, height), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        const flecs::world& ecs = engine.world.ecs();
        if (icon_button(icons_, "##undo", Icon::Undo, "Undo (Ctrl+Z)", false, commands_.can_undo())) {
            static_cast<void>(commands_.undo(ecs));
        }
        ImGui::SameLine();
        if (icon_button(icons_, "##redo", Icon::Redo, "Redo (Ctrl+Y)", false, commands_.can_redo())) {
            static_cast<void>(commands_.redo(ecs));
        }
        toolbar_rule();
        struct ToolButton {
            Tool tool;
            Icon icon;
            const char* tip;
        };
        constexpr ToolButton kTools[] = {{.tool = Tool::Select, .icon = Icon::Select, .tip = "Select (Q)"},
                                         {.tool = Tool::Move, .icon = Icon::Move, .tip = "Move (W)"},
                                         {.tool = Tool::Rotate, .icon = Icon::Rotate, .tip = "Rotate (E)"},
                                         {.tool = Tool::Scale, .icon = Icon::Scale, .tip = "Scale (R)"}};
        for (const ToolButton& entry : kTools) {
            ImGui::PushID(static_cast<int>(entry.tool));
            if (icon_button(icons_, "##tool", entry.icon, entry.tip, tool_ == entry.tool)) {
                tool_ = entry.tool;
            }
            ImGui::PopID();
            if (entry.tool != Tool::Scale) {
                ImGui::SameLine();
            }
        }
        toolbar_rule();
        const bool scaling = tool_ == Tool::Scale; // always along the selection's own axes
        if (icon_button(icons_, "##space", space_ == Space::World && !scaling ? Icon::WorldCoords : Icon::LocalCoords,
                        scaling                  ? "Scaling always goes along the selection's own axes"
                        : space_ == Space::World ? "World axes (Ctrl+` for the selection's own)"
                                                 : "The selection's own axes (Ctrl+` for the world's)",
                        false, !scaling)) {
            space_ = space_ == Space::World ? Space::Local : Space::World;
        }
        ImGui::SameLine();
        if (icon_button(icons_, "##handles", handles_at_ == HandlesAt::Pivot ? Icon::Pivot : Icon::Center,
                        handles_at_ == HandlesAt::Pivot
                            ? "Handles at the pivot (click for the centre of the geometry)"
                            : "Handles at the centre of the geometry: turning and scaling go about it (click for the "
                              "pivot)",
                        false)) {
            handles_at_ = handles_at_ == HandlesAt::Pivot ? HandlesAt::Center : HandlesAt::Pivot;
        }
        toolbar_rule();
        constexpr float kMoveSteps[] = {0.01f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f};
        constexpr float kAngleSteps[] = {1.0f, 5.0f, 10.0f, 15.0f, 30.0f, 45.0f, 90.0f};
        constexpr float kScaleSteps[] = {0.01f, 0.05f, 0.1f, 0.25f, 0.5f, 1.0f};
        if (icon_button(icons_, "##snap_move", Icon::SnapGrid, "Snap moves to a grid", snap_.move)) {
            snap_.move = !snap_.move;
        }
        ImGui::SameLine();
        snap_step("##snap_move_step", snap_.move_m, kMoveSteps, " m");
        ImGui::SameLine(0.0f, px(8.0f));
        if (icon_button(icons_, "##snap_rotate", Icon::SnapAngle, "Snap turns to steps of an angle", snap_.rotate)) {
            snap_.rotate = !snap_.rotate;
        }
        ImGui::SameLine();
        snap_step("##snap_rotate_step", snap_.rotate_deg, kAngleSteps, "°");
        ImGui::SameLine(0.0f, px(8.0f));
        if (icon_button(icons_, "##snap_scale", Icon::SnapScale, "Snap scaling to steps", snap_.scale)) {
            snap_.scale = !snap_.scale;
        }
        ImGui::SameLine();
        snap_step("##snap_scale_step", snap_.scale_step, kScaleSteps, "");

        // The right: the camera's speed, the view, framing the selection.
        char speed[32];
        static_cast<void>(
            std::snprintf(speed, sizeof(speed), "%.3g m/s", static_cast<double>(viewer_.camera().speed_mps)));
        const float extent = ImGui::GetFrameHeight();
        const float speed_width = extent + ImGui::CalcTextSize(speed).x + px(8.0f);
        const float view_width = px(120.0f);
        const float right = speed_width + view_width + extent + (px(6.0f) * 2.0f);
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + px(12.0f),
                                 ImGui::GetWindowWidth() - right - ImGui::GetStyle().WindowPadding.x));
        {
            constexpr const char* kPopup = "##camera_speed";
            const ImVec2 min = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##camera", ImVec2(speed_width, extent))) {
                ImGui::OpenPopup(kPopup);
            }
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList* draw = ImGui::GetWindowDrawList();
            if (hovered || ImGui::IsPopupOpen(kPopup)) {
                draw->AddRectFilled(min, ImVec2(min.x + speed_width, min.y + extent),
                                    color_u32(ThemeColor::ControlHovered), px(theme().metrics.control_rounding));
            }
            const float size = std::floor(extent * 0.72f);
            const float inset = std::floor((extent - size) * 0.5f);
            icons_.draw(draw, Icon::Camera, ImVec2(min.x + inset, min.y + inset), size,
                        color_u32(hovered ? ThemeColor::TextEmphasis : ThemeColor::Text));
            draw->AddText(ImVec2(min.x + extent, std::floor(min.y + ((extent - ImGui::GetFontSize()) * 0.5f))),
                          color_u32(ThemeColor::Text), speed);
            if (hovered) {
                tooltip("The camera's speed: the wheel over the scene changes it");
            }
            push_menu_style();
            if (ImGui::BeginPopup(kPopup)) {
                menu_section("Camera speed");
                float value = viewer_.camera().speed_mps;
                ImGui::SetNextItemWidth(px(200.0f));
                if (ImGui::SliderFloat("##speed", &value, tools::FreeCamera::kMinSpeedMps,
                                       tools::FreeCamera::kMaxSpeedMps, "%.3g m/s", ImGuiSliderFlags_Logarithmic)) {
                    viewer_.set_camera_speed(value);
                }
                ImGui::EndPopup();
            }
            pop_menu_style();
        }
        ImGui::SameLine(0.0f, px(6.0f));
        if (begin_combo("##scene_view", render::scene_view_name(engine.view), view_width)) {
            for (uint32_t i = 0; i < static_cast<uint32_t>(render::SceneView::Count); ++i) {
                const auto view = static_cast<render::SceneView>(i);
                if (ImGui::Selectable(render::scene_view_name(view), engine.view == view)) {
                    engine.view = view;
                }
            }
            end_combo();
        }
        ImGui::SameLine(0.0f, px(6.0f));
        if (icon_button(icons_, "##frame", Icon::FocusSelection, "Frame the selection (F)", false,
                        selection_.kind == Selection::Kind::Entity)) {
            selection_.frame_entity = true;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void Editor::playground(app::Engine& engine) {
    // Alone in its area, it shows no tab: the toolbar and the game's view edge to edge.
    ImGuiWindowClass window_class;
    window_class.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_AutoHideTabBar;
    ImGui::SetNextWindowClass(&window_class);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool shown =
        ImGui::Begin(kPlayground, nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    playground_hovered_ = false;
    if (shown) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        playground_toolbar(engine);
        ImGui::PopStyleVar();
    }
    const ImVec2 size = ImGui::GetContentRegionAvail();
    if (shown && size.x >= 1.0f && size.y >= 1.0f) {
        // The scene at the panel's size in pixels, from the next frame on; this frame's is stretched to fit.
        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2 scale = io.DisplayFramebufferScale;
        engine.viewport.want_width = static_cast<uint32_t>(std::lround(size.x * scale.x));
        engine.viewport.want_height = static_cast<uint32_t>(std::lround(size.y * scale.y));
        const ImVec2 corner = ImGui::GetCursorScreenPos();
        const ImVec2 far_corner(corner.x + size.x, corner.y + size.y);
        // A button under the picture: clicking the scene takes the keyboard from a text box, and hovering is exact.
        ImGui::InvisibleButton("scene", size,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                   ImGuiButtonFlags_MouseButtonMiddle);
        playground_hovered_ = ImGui::IsItemHovered();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        if (engine.viewport.texture_id != 0) {
            draw->AddImage(ImTextureRef(static_cast<ImTextureID>(engine.viewport.texture_id)), corner, far_corner);
        }

        // The selection's handles; a click elsewhere in the scene selects what's under it.
        const View view = playground_view(corner, size);
        const flecs::world& ecs = engine.world.ecs();
        const flecs::entity selected = selection_.kind == Selection::Kind::Entity && ecs.is_alive(selection_.entity)
                                           ? ecs.entity(selection_.entity)
                                           : flecs::entity();
        draw->PushClipRect(corner, far_corner, true);
        // Alt keeps the mouse for the camera: no handle takes it then.
        const bool mouse_free = playground_hovered_ && !looking_ && !io.KeyAlt && !navigation_.dragging();
        const bool on_gizmo = gizmo_.update(view, draw, tool_, space_, snap_, selected,
                                            handles_origin(engine, selected), mouse_free, commands_);
        draw->PopClipRect();

        // The camera: orbit, pan and zoom (Unity's scene view); flying is the right button's.
        tools::CameraPose pose = camera_pose();
        if (navigation_.update(view, playground_hovered_ && !gizmo_.dragging(), looking_, io.DeltaTime, pose)) {
            viewer_.set_camera_pose(pose);
        }

        // A left click, let go without moving, selects what's under it: not one on a handle, not one that orbits.
        if (playground_hovered_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            left_press_ = !on_gizmo && !io.KeyAlt && !looking_;
        }
        if (left_press_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            left_press_ = false;
            const float still = px(4.0f);
            if (playground_hovered_ && !gizmo_.dragging() && io.MouseDragMaxDistanceSqr[0] < still * still &&
                engine.viewport.width > 0 && engine.viewport.height > 0) {
                // The pixel of the scene's texture under the mouse (shown stretched to the panel while it's resized).
                const float u = std::clamp((io.MousePos.x - corner.x) / size.x, 0.0f, 1.0f);
                const float v = std::clamp((io.MousePos.y - corner.y) / size.y, 0.0f, 1.0f);
                ask_pick(engine,
                         std::min(static_cast<uint32_t>(u * static_cast<float>(engine.viewport.width)),
                                  engine.viewport.width - 1),
                         std::min(static_cast<uint32_t>(v * static_cast<float>(engine.viewport.height)),
                                  engine.viewport.height - 1));
            }
        }

        if (playground_hovered_ && !looking_) {
            // Q W E R: the tools.
            if (!io.WantTextInput && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift) {
                if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
                    tool_ = Tool::Select;
                } else if (ImGui::IsKeyPressed(ImGuiKey_W, false)) {
                    tool_ = Tool::Move;
                } else if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
                    tool_ = Tool::Rotate;
                } else if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
                    tool_ = Tool::Scale;
                }
            }
        }
    }
    ImGui::End();
}

void Editor::shortcuts(app::Engine& engine) {
    const flecs::world& ecs = engine.world.ecs();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal)) {
        static_cast<void>(commands_.undo(ecs));
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_RouteGlobal) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal)) {
        static_cast<void>(commands_.redo(ecs));
    }
    // Ctrl+`: the world's axes or the selection's own, as Unreal's.
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_GraveAccent, ImGuiInputFlags_RouteGlobal)) {
        space_ = space_ == Space::World ? Space::Local : Space::World;
    }
    // F, unless it's being typed.
    if (!ImGui::GetIO().WantTextInput && ImGui::Shortcut(ImGuiKey_F, ImGuiInputFlags_RouteGlobal) &&
        selection_.kind == Selection::Kind::Entity) {
        selection_.frame_entity = true;
    }
}

tools::CameraPose Editor::camera_pose() const {
    const Vec3 forward = viewer_.camera().rotation() * Vec3(0.0f, 0.0f, -1.0f);
    return {.position = viewer_.camera().position(),
            .yaw_rad = std::atan2(-forward.x, -forward.z),
            .pitch_rad = std::asin(std::clamp(forward.y, -1.0f, 1.0f))};
}

void Editor::go_to_place(const DVec3& place) {
    // There, at the camera's height, looking as it was.
    tools::CameraPose pose = camera_pose();
    pose.position = DVec3(place.x, pose.position.y, place.z);
    viewer_.set_camera_pose(pose);
}

void Editor::frame_selection(app::Engine& engine) {
    const flecs::world& ecs = engine.world.ecs();
    if (selection_.kind != Selection::Kind::Entity || !ecs.is_alive(selection_.entity)) {
        return;
    }
    const flecs::entity entity = ecs.entity(selection_.entity);
    const auto* place = entity.try_get<world::WorldTransform>();
    if (place == nullptr) {
        return; // nothing placed (a cell)
    }
    // How big it is: its mesh's bounds at its scale, or a few meters.
    float radius_m = 1.0f;
    if (const auto* instance = entity.try_get<world::MeshInstance>()) {
        if (const auto* mesh = engine.assets.get<asset::MeshAsset>(instance->mesh)) {
            radius_m = mesh->bounds.radius * std::max({place->scale.x, place->scale.y, place->scale.z});
        }
    }
    // Its bounds' centre, where it is.
    DVec3 center = place->position;
    if (const auto* instance = entity.try_get<world::MeshInstance>()) {
        if (const auto* mesh = engine.assets.get<asset::MeshAsset>(instance->mesh)) {
            const Vec3 offset(mesh->bounds.center[0], mesh->bounds.center[1], mesh->bounds.center[2]);
            center += DVec3(place->rotation * (place->scale * offset));
        }
    }
    // The camera eases back along where it looks until the sphere around it fills about half the view.
    const auto* lens = viewer_.camera_entity().try_get<world::Camera>();
    navigation_.frame(camera_pose(), center, std::max(radius_m, 0.25f),
                      lens != nullptr ? lens->vertical_fov_rad : 1.0f);
}

void Editor::open_world(app::Engine& engine) {
    asset::Guid guid;
    asset::AssetId id;
    if (!source_asset_id(engine.vfs, selection_.file.view(), guid, id)) {
        static_cast<void>(
            std::snprintf(message_, sizeof(message_), "%s has no meta: run the cooker first", selection_.file.c_str()));
        DE_LOG_ERROR("editor", "%s", message_);
        return;
    }
    if (id == viewer_.world_id()) {
        return; // already open
    }
    DE_LOG_INFO("editor", "opening %s (%s)", selection_.file.c_str(), asset::format_asset_id(id).c_str());
    commands_.clear();     // their entities go with the world
    selection_.entity = 0; // the file stays selected
    hierarchy_.reset();
    if (viewer_.open_world(engine, id)) {
        static_cast<void>(std::snprintf(message_, sizeof(message_), "Opened %s", selection_.file.c_str()));
    } else {
        static_cast<void>(std::snprintf(message_, sizeof(message_), "%s didn't load: is it cooked? (the log says why)",
                                        selection_.file.c_str()));
        DE_LOG_ERROR("editor", "%s", message_);
    }
}

} // namespace de::editor
#endif
