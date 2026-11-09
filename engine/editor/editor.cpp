#include "editor/editor.h"

#include "asset/cell_format.h"
#include "asset/mesh_format.h"
#include "core/log.h"
#include "core/profile.h"
#include "core/text.h"
#include "editor/cell_source.h"
#include "editor/outline.h"
#include "editor/source_files.h"
#include "editor/theme.h"
#include "editor/widgets.h"
#include "physics/physics.h"
#include "platform/input.h"
#include "platform/process.h"
#include "platform/vfs.h"
#include "platform/window.h"
#include "render/scene/scene_renderer.h"
#include "tools/console_ui.h"
#include "tools/debug_ui.h"
#include "tools/stats_panel.h"
#include "world/cell.h"
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
    float drawer_share = 0.4f;
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
        } else if (key == "drawer") {
            settings.drawer_share = std::clamp(to_number(value, settings.drawer_share), 0.15f, 0.85f);
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
                      "snap_scale %s %g\ndrawer %.2f\n",
                      static_cast<double>(settings.ui_scale), kTools[static_cast<int>(settings.tool)],
                      settings.space == Space::Local ? "local" : "world",
                      settings.handles_at == HandlesAt::Center ? "center" : "pivot", settings.snap.move ? "on" : "off",
                      static_cast<double>(settings.snap.move_m), settings.snap.rotate ? "on" : "off",
                      static_cast<double>(settings.snap.rotate_deg), settings.snap.scale ? "on" : "off",
                      static_cast<double>(settings.snap.scale_step), static_cast<double>(settings.drawer_share));
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
// A scene view's name as the editor shows it, each word capitalized: "base color" -> "Base Color".
struct ViewName {
    char text[48] = {};

    explicit ViewName(render::SceneView view) {
        const char* name = render::scene_view_name(view);
        bool word_start = true;
        for (size_t i = 0; name[i] != '\0' && i + 1 < sizeof(text); ++i) {
            const char c = name[i];
            text[i] = word_start && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
            word_start = c == ' ';
        }
    }
};

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
      selected_entities_(editor_heap()), properties_(editor_heap()), browser_(editor_heap()),
      cell_sources_(editor_heap()), cooking_(editor_heap()), holds_(editor_heap()), members_(editor_heap()),
      scratch_entities_(editor_heap()) {}

bool Editor::startup(app::Engine& engine, const app::FrameworkArgs& args) {
    if (!viewer_.startup(engine, args)) {
        return false;
    }
    viewer_.set_quit_on_escape(false);
    engine_ = &engine;
    commands_.set_maker(this);
    terrain_heights_.emplace(engine.vfs, editor_heap());
    engine.console.add_command("editor.save", "",
                               "Save the edits into the cells' text sources, then cook them (as Ctrl+S does).",
                               &Editor::save_command, this);
    engine.console.add_command("editor.move", "<name> <x> <y> <z>",
                               "Move the entity of that name to a place in meters (undoable), and select it.",
                               &Editor::move_command, this);
    engine.console.add_command("editor.place", "<src:/content/...model> <x> <y> <z>",
                               "Place a model there (meters), as dropping it on the Playground does.",
                               &Editor::place_command, this);
    engine.console.add_command(
        "editor.drop", "<src:/content/...model>",
        "Drop a model at the middle of the Playground, on the ground there (as the mouse would).",
        &Editor::drop_command, this);
    engine.console.add_command("editor.duplicate", "<name>", "Duplicate the entity of that name (as Ctrl+D does).",
                               &Editor::duplicate_command, this);
    engine.console.add_command("editor.delete", "<name>", "Delete the entity of that name (as the Delete key does).",
                               &Editor::delete_command, this);
    engine.console.add_command("editor.ground", "<name>",
                               "Drop the entity of that name onto the ground below it (as End does).",
                               &Editor::ground_command, this);
    engine.console.add_command("editor.undo", "", "Undo the last edit (as Ctrl+Z does).", &Editor::undo_command, this);
    engine.console.add_command("editor.redo", "", "Do again what was undone (as Ctrl+Y does).", &Editor::redo_command,
                               this);
    // A click on the editor while another window is in front is a click, not only a way back to it.
    platform::Window::set_focus_clicks(true);
    engine.frame_stats = app::has_flag(args, "--stats"); // hidden until F1 or View > Frame Stats, unless asked for
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
    drawer_share_ = settings.drawer_share;
    if (const char* typed = app::flag_value(args, "--cmd"); typed != nullptr && engine.console_ui != nullptr) {
        engine.console_ui->set_command_line(typed);
    }
    if (const char* drawer = app::flag_value(args, "--drawer")) {
        drawer_ = std::string_view(drawer) == "output" ? Drawer::Output : Drawer::Resources;
    }
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
        save_settings(engine.vfs, {.ui_scale = ui_scale_,
                                   .tool = tool_,
                                   .space = space_,
                                   .handles_at = handles_at_,
                                   .snap = snap_,
                                   .drawer_share = drawer_share_});
    }
    commands_.clear();
    release_holds();
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
    const ImVec2 panels_min = ImGui::GetCursorScreenPos();
    const ImVec2 panels_size(std::max(1.0f, available.x - (gap * 2.0f)), body);
    dock_space(panels_size);
    ImGui::SetCursorPos(ImVec2(origin.x, origin.y + gap + body + gap));
    status_bar(engine, ImVec2(available.x, status));
    ImGui::End();
    ImGui::PopStyleVar(3);

    take_pick(engine);
    follow_saves(engine);
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
    const bool opened = drawer(engine, panels_min, panels_size);
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
    unsaved_prompt(engine);
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
        if (ImGui::MenuItem("Save", "Ctrl+S", false, unsaved())) {
            save(engine);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) {
            if (unsaved()) {
                pending_ = Pending::Quit; // asked first
            } else {
                quit_ = true;
            }
        }
        ImGui::EndMenu();
    }
    prepare_menu_popup();
    if (ImGui::BeginMenu("Edit")) {
        char undo_label[128];
        char redo_label[128];
        const char* undo_name = commands_.undo_name(ecs);
        const char* redo_name = commands_.redo_name(ecs);
        static_cast<void>(
            std::snprintf(undo_label, sizeof(undo_label), "Undo %s", undo_name != nullptr ? undo_name : ""));
        static_cast<void>(
            std::snprintf(redo_label, sizeof(redo_label), "Redo %s", redo_name != nullptr ? redo_name : ""));
        if (ImGui::MenuItem(undo_label, "Ctrl+Z", false, commands_.can_undo())) {
            undo(engine);
        }
        if (ImGui::MenuItem(redo_label, "Ctrl+Y", false, commands_.can_redo())) {
            redo(engine);
        }
        ImGui::Separator();
        const bool an_entity =
            selection_.kind == Selection::Kind::Entity && engine.world.ecs().is_alive(selection_.entity);
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, an_entity)) {
            duplicate_selection(engine);
        }
        if (ImGui::MenuItem("Delete", "Del", false, an_entity)) {
            delete_selection(engine);
        }
        if (ImGui::MenuItem("Drop to Ground", "End", false, an_entity)) {
            static_cast<void>(drop_selection_to_ground(engine));
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Frame Selected", "F", false, selection_.kind == Selection::Kind::Entity)) {
            selection_.frame_entity = true;
        }
        ImGui::EndMenu();
    }
    prepare_menu_popup();
    if (ImGui::BeginMenu("View")) {
        // The scene's views are the Playground toolbar's.
        ImGui::MenuItem("Frame Stats", "F1", &engine.frame_stats);
        ImGui::EndMenu();
    }
    prepare_menu_popup();
    if (ImGui::BeginMenu("Rendering Settings")) {
        ImGui::MenuItem("Temporal Anti-Aliasing", "F3", &engine.taa);
        ImGui::EndMenu();
    }
    prepare_menu_popup();
    if (ImGui::BeginMenu("Preferences")) {
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
        if (ImGui::MenuItem("Resources", "Ctrl+Space", drawer_ == Drawer::Resources)) {
            drawer_ = drawer_ == Drawer::Resources ? Drawer::None : Drawer::Resources;
        }
        if (ImGui::MenuItem("Output", nullptr, drawer_ == Drawer::Output)) {
            drawer_ = drawer_ == Drawer::Output ? Drawer::None : Drawer::Output;
        }
        ImGui::Separator();
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
    const ImGuiID dock = ImGui::GetID("##dock 4");
    if (reset_layout_ || ImGui::DockBuilderGetNode(dock) == nullptr) {
        // Left: the hierarchy (the cell map in a tab beside it) over the properties. Right: the Playground. Resources
        // and the output are drawers, out of the way until they're wanted.
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
        ImGui::DockBuilderDockWindow(HierarchyPanel::kWindow, hierarchy);
        ImGui::DockBuilderDockWindow(CellMapPanel::kWindow, hierarchy);
        ImGui::DockBuilderDockWindow(PropertiesPanel::kWindow, properties);
        ImGui::DockBuilderDockWindow(kPlayground, right);
        ImGui::DockBuilderFinish(dock);
    }
    dock_space_with_splitters(dock, size, ImGuiDockNodeFlags_NoWindowMenuButton);
}

bool Editor::drawer_button(const char* id, const char* label, bool content, bool open) {
    const float height = ImGui::GetFrameHeight();
    const float glyph = std::floor(height * 0.62f);
    const float pad = px(8.0f);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 size(pad + glyph + px(6.0f) + ImGui::CalcTextSize(label).x + pad, height);
    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (open || hovered) {
        draw->AddRectFilled(
            min, ImVec2(min.x + size.x, min.y + size.y),
            color_u32(open ? (hovered ? ThemeColor::AccentHovered : ThemeColor::Accent) : ThemeColor::ControlHovered),
            px(theme().metrics.control_rounding));
    }
    const ImVec2 glyph_min(min.x + pad, std::floor(min.y + ((height - glyph) * 0.5f)));
    const ImU32 ink = color_u32(open || hovered ? ThemeColor::TextEmphasis : ThemeColor::Text);
    if (content) {
        draw_folder_glyph(glyph_min, glyph, glyph * 0.82f);
    } else {
        // Lines of text: the log.
        const float line = std::max(1.0f, px(1.5f));
        for (int i = 0; i < 4; ++i) {
            const float y = glyph_min.y + (glyph * (0.18f + (0.22f * static_cast<float>(i))));
            const float length = glyph * (i == 3 ? 0.55f : 0.95f);
            draw->AddLine(ImVec2(glyph_min.x, y), ImVec2(glyph_min.x + length, y), ink, line);
        }
    }
    draw->AddText(ImVec2(glyph_min.x + glyph + px(6.0f), std::floor(min.y + ((height - ImGui::GetFontSize()) * 0.5f))),
                  ink, label);
    return pressed;
}

void Editor::status_bar(app::Engine& engine, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, color(ThemeColor::Surface));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(px(6.0f), std::max(0.0f, (size.y - ImGui::GetFrameHeight()) * 0.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(4.0f), 0.0f));
    if (ImGui::BeginChild("##status", size, ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        // The drawers, as Unreal's bottom bar has them: Resources and the output.
        if (drawer_button("##resources drawer", "Resources", true, drawer_ == Drawer::Resources)) {
            drawer_ = drawer_ == Drawer::Resources ? Drawer::None : Drawer::Resources;
        }
        if (ImGui::IsItemHovered()) {
            tooltip("The project's files (Ctrl+Space)");
        }
        ImGui::SameLine();
        if (drawer_button("##output drawer", "Output", false, drawer_ == Drawer::Output)) {
            drawer_ = drawer_ == Drawer::Output ? Drawer::None : Drawer::Output;
        }
        if (ImGui::IsItemHovered()) {
            tooltip("What the engine and the console have said");
        }
        // A command line: the console's (` over the Playground opens its window too).
        ImGui::SameLine(0.0f, px(12.0f));
        const float label_y = ImGui::GetCursorPosY() + ((ImGui::GetFrameHeight() - ImGui::GetFontSize()) * 0.5f);
        const float label_x = ImGui::GetCursorPosX();
        ImGui::SetCursorPosY(label_y);
        const bool typing = engine.console_ui != nullptr && engine.console_ui->command_line_active();
        ImGui::TextColored(color(typing ? ThemeColor::Accent : ThemeColor::TextSecondary), "Cmd");
        ImGui::SameLine(label_x + ImGui::CalcTextSize("Cmd").x + px(6.0f));
        ImGui::SetCursorPosY(label_y - ((ImGui::GetFrameHeight() - ImGui::GetFontSize()) * 0.5f));
        if (engine.console_ui != nullptr) {
            engine.console_ui->draw_command_line(px(380.0f), "a console command (help lists them)");
        }
        // What the focused panel reports, or the editor's last word. (The camera's speed is the toolbar's.)
        const char* text = hierarchy_.focused()           ? hierarchy_.status()
                           : cell_map_.focused()          ? cell_map_.status()
                           : drawer_ == Drawer::Resources ? browser_.status()
                                                          : message_;
        ImGui::SameLine(0.0f, px(14.0f));
        ImGui::SetCursorPosY(label_y);
        ImGui::TextColored(color(ThemeColor::TextSecondary), "%s", text);
        if (unsaved() || cooker_.running()) {
            const char* state = cooker_.running() ? "Cooking..." : "Unsaved edits (Ctrl+S)";
            ImGui::SameLine(std::max(ImGui::GetCursorPosX() + px(14.0f),
                                     ImGui::GetWindowWidth() - ImGui::CalcTextSize(state).x - px(12.0f)));
            ImGui::SetCursorPosY(label_y);
            ImGui::TextColored(color(cooker_.running() ? ThemeColor::TextSecondary : ThemeColor::Warning), "%s", state);
        }
        status_hovered_ = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

bool Editor::drawer(app::Engine& engine, ImVec2 min, ImVec2 size) {
    if (drawer_ == Drawer::None) {
        return false;
    }
    // Up from the bottom bar, over the panels, as tall as was last dragged.
    const float height = std::clamp(size.y * drawer_share_, std::min(px(140.0f), size.y), size.y);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, color(ThemeColor::Surface));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    constexpr ImGuiWindowFlags kDrawer = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                                         ImGuiWindowFlags_NoScrollWithMouse;
    const bool content = drawer_ == Drawer::Resources;
    bool hovered = false;
    bool opened = false;
    if (begin_over_panels(content ? "Resources##drawer" : "Output##drawer", ImVec2(min.x, min.y + size.y - height),
                          ImVec2(size.x, height), kDrawer)) {
        hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                                         ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 top = ImGui::GetWindowPos();
        // Its top edge: dragged, it's taller or shorter.
        const float edge = std::max(4.0f, px(5.0f));
        ImGui::SetCursorScreenPos(top);
        ImGui::InvisibleButton("##drawer edge", ImVec2(size.x, edge));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        }
        if (ImGui::IsItemActive()) {
            drawer_share_ = std::clamp((min.y + size.y - ImGui::GetIO().MousePos.y) / size.y, 0.15f, 0.85f);
        }
        draw->AddLine(
            top, ImVec2(top.x + size.x, top.y),
            color_u32(ImGui::IsItemHovered() || ImGui::IsItemActive() ? ThemeColor::Accent : ThemeColor::Border),
            std::max(1.0f, px(1.0f)));

        // Its header: what it is, its own controls, and a close button at the right.
        const float row = header_row_height();
        const float inset = px(10.0f);
        ImGui::SetCursorScreenPos(ImVec2(top.x + inset, top.y + edge));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(6.0f), 0.0f));
        const float text_y = top.y + edge + ((row - ImGui::GetFontSize()) * 0.5f);
        draw->AddText(ImVec2(top.x + inset, text_y), color_u32(ThemeColor::TextEmphasis),
                      content ? "Resources" : "Output");
        if (!content) {
            // A filter over the lines, and clearing them.
            ImGui::SetCursorScreenPos(
                ImVec2(top.x + inset + px(70.0f), top.y + edge + ((row - ImGui::GetFrameHeight()) * 0.5f)));
            search_field("##output filter", output_filter_, sizeof(output_filter_), "Filter the lines", px(260.0f));
            ImGui::SameLine();
            if (ImGui::Button("Clear") && engine.console_ui != nullptr) {
                engine.console_ui->clear_log();
            }
        }
        const float close = ImGui::GetFrameHeight();
        ImGui::SetCursorScreenPos(ImVec2(top.x + size.x - close - inset, top.y + edge + ((row - close) * 0.5f)));
        if (ImGui::InvisibleButton("##close drawer", ImVec2(close, close))) {
            drawer_ = Drawer::None;
        }
        const ImVec2 cross = ImGui::GetItemRectMin();
        const float pad = close * 0.3f;
        const ImU32 cross_ink = color_u32(ImGui::IsItemHovered() ? ThemeColor::TextEmphasis : ThemeColor::Text);
        draw->AddLine(ImVec2(cross.x + pad, cross.y + pad), ImVec2(cross.x + close - pad, cross.y + close - pad),
                      cross_ink, std::max(1.0f, px(1.5f)));
        draw->AddLine(ImVec2(cross.x + close - pad, cross.y + pad), ImVec2(cross.x + pad, cross.y + close - pad),
                      cross_ink, std::max(1.0f, px(1.5f)));
        ImGui::PopStyleVar();

        // What it holds, below.
        ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + edge + row));
        const ImVec2 room(size.x, std::max(1.0f, height - edge - row));
        if (content) {
            if (ImGui::BeginChild("##content", room)) {
                opened = browser_.draw(engine.vfs, selection_);
            }
            ImGui::EndChild();
        } else if (engine.console_ui != nullptr) {
            ImGui::SetCursorScreenPos(ImVec2(top.x + inset, top.y + edge + row));
            engine.console_ui->draw_log_panel(room.x - (2.0f * inset), room.y - px(4.0f), output_filter_);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    // A click anywhere else puts it away, as Unreal's drawers do; not while a menu or popup of its own is open.
    const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    if (clicked && !hovered && !status_hovered_ && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup)) {
        drawer_ = Drawer::None;
    }
    return opened;
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
        if (icon_button(icons_, "##undo", Icon::Undo, "Undo (Ctrl+Z)", false, commands_.can_undo())) {
            undo(engine);
        }
        ImGui::SameLine();
        if (icon_button(icons_, "##redo", Icon::Redo, "Redo (Ctrl+Y)", false, commands_.can_redo())) {
            redo(engine);
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
                tooltip("The camera's speed: the wheel changes it while the right button flies the camera");
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
        if (begin_combo("##scene_view", ViewName(engine.view).text, view_width)) {
            for (uint32_t i = 0; i < static_cast<uint32_t>(render::SceneView::Count); ++i) {
                const auto view = static_cast<render::SceneView>(i);
                if (ImGui::Selectable(ViewName(view).text, engine.view == view)) {
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
        engine.viewport.ui_x = corner.x; // the frame stats go over the picture
        engine.viewport.ui_y = corner.y;
        engine.viewport.ui_width = size.x;
        engine.viewport.ui_height = size.y;
        // A button under the picture: clicking the scene takes the keyboard from a text box, and hovering is exact.
        ImGui::InvisibleButton("scene", size,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                   ImGuiButtonFlags_MouseButtonMiddle);
        playground_hovered_ = ImGui::IsItemHovered();
        // A model dragged from the resources and let go over the scene: placed there.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(AssetBrowser::kSourceFilePayload)) {
                const std::string_view dropped(static_cast<const char*>(payload->Data),
                                               static_cast<size_t>(std::max(payload->DataSize - 1, 0)));
                place_model(engine, dropped, playground_view(corner, size), io.MousePos);
            }
            ImGui::EndDragDropTarget();
        }
        ImDrawList* draw = ImGui::GetWindowDrawList();
        if (engine.viewport.texture_id != 0) {
            draw->AddImage(ImTextureRef(static_cast<ImTextureID>(engine.viewport.texture_id)), corner, far_corner);
        }

        // The selection's handles; a click elsewhere in the scene selects what's under it.
        const View view = playground_view(corner, size);
        last_view_ = view;
        const flecs::world& ecs = engine.world.ecs();
        const flecs::entity selected = selection_.kind == Selection::Kind::Entity && ecs.is_alive(selection_.entity)
                                           ? ecs.entity(selection_.entity)
                                           : flecs::entity();
        // The frame stats over the picture, as a part of this panel: whatever comes over it (a drawer, a menu) covers
        // them.
        if (engine.frame_stats && engine.stats_panel != nullptr) {
            engine.stats_panel->draw_in_window(corner.x, corner.y, size.x, size.y);
        }
        if (engine.freeze_cull) {
            tools::draw_viewport_notice(draw, corner.x, corner.y, size.x, "CULL FROZEN (F4)");
        }
        engine.viewport.overlays_drawn = true;
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
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal)) {
        save(engine);
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D, ImGuiInputFlags_RouteGlobal)) {
        duplicate_selection(engine);
    }
    if (!ImGui::GetIO().WantTextInput && ImGui::Shortcut(ImGuiKey_Delete, ImGuiInputFlags_RouteGlobal)) {
        delete_selection(engine);
    }
    if (!ImGui::GetIO().WantTextInput && ImGui::Shortcut(ImGuiKey_End, ImGuiInputFlags_RouteGlobal)) {
        static_cast<void>(drop_selection_to_ground(engine));
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal)) {
        undo(engine);
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_RouteGlobal) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal)) {
        redo(engine);
    }
    // Ctrl+Space: the Resources drawer (Unreal's content drawer).
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Space, ImGuiInputFlags_RouteGlobal)) {
        drawer_ = drawer_ == Drawer::Resources ? Drawer::None : Drawer::Resources;
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

void Editor::save(app::Engine& engine) {
    if (cooker_.running()) {
        static_cast<void>(std::snprintf(message_, sizeof(message_), "Still cooking the last save: save again after"));
        return;
    }
    if (!cell_sources_.scan(engine.vfs) && cell_sources_.size() == 0) {
        static_cast<void>(std::snprintf(message_, sizeof(message_), "Nothing saved: src:/content couldn't be read"));
        return;
    }
    // Whatever the commands and the selection name, keyed now: the cook will spawn them again.
    keyed_changes_ = UINT64_MAX;
    cooking_.clear();
    core::Array<MemberMove> moves(editor_heap());
    core::Array<MemberRemoval> removed(editor_heap());
    commands_.each_taken_member(engine.world.ecs(), [&removed](const EntityRecord& record) {
        removed.push({.key = record.key, .name = record.cooked_name});
    });
    core::Array<PlacedMember> placed(editor_heap());
    const CellSaveReport report = save_cells(engine.world.ecs(), engine.assets, engine.vfs, cell_sources_,
                                             viewer_.world_id(), &cooking_, &moves, removed.span(), &placed);
    members_.apply(moves.span()); // what moved in the files: the cook will spawn it there
    for (const PlacedMember& made : placed) {
        // In its file now: the member its cell comes back with takes its place.
        if (auto* entry = engine.world.ecs().entity(made.entity).try_get_mut<EditorEntry>()) {
            entry->saved = true;
        }
        members_.note_replaced(engine.world.ecs(), made.entity, made.key);
    }
    if (report.cells_failed > 0) {
        static_cast<void>(std::snprintf(message_, sizeof(message_), "%u %s couldn't be saved (the Output says why)",
                                        report.cells_failed, report.cells_failed == 1 ? "file" : "files"));
        start_cook(engine); // what was written, cooked
        return;             // the edits stay unsaved
    }
    saved_changes_ = commands_.changes();
    commands_.forget_saved_values(); // what's saved now is the world as it is
    if (report.files_written == 0) {
        static_cast<void>(std::snprintf(message_, sizeof(message_), "Nothing to save: the cells are as their files"));
        return;
    }
    if (report.outside_cell > 0) {
        DE_LOG_WARN("editor",
                    "%u moved out of their cell's square, but no world lists their cell's file: they stay in it",
                    report.outside_cell);
    }
    start_cook(engine);
    char moved[160] = "";
    int written = 0;
    if (report.added > 0) {
        written +=
            std::snprintf(moved + written, sizeof(moved) - static_cast<size_t>(written), ", %u added", report.added);
    }
    if (report.removed > 0 && written >= 0 && static_cast<size_t>(written) < sizeof(moved)) {
        written += std::snprintf(moved + written, sizeof(moved) - static_cast<size_t>(written), ", %u deleted",
                                 report.removed);
    }
    if (report.refiled > 0 && written >= 0 && static_cast<size_t>(written) < sizeof(moved)) {
        static_cast<void>(std::snprintf(moved + written, sizeof(moved) - static_cast<size_t>(written),
                                        ", %u filed into another cell", report.refiled));
    }
    static_cast<void>(std::snprintf(message_, sizeof(message_), "Saved %u %s (%u %s%s)%s", report.files_written,
                                    report.files_written == 1 ? "file" : "files", report.entities_changed,
                                    report.entities_changed == 1 ? "entity" : "entities", moved,
                                    cooker_.running() ? ": cooking" : ": run the cooker to see them in the game"));
    DE_LOG_INFO("editor", "%s", message_);
}

void Editor::start_cook(app::Engine& engine) {
    if (cooking_.empty() || cooker_.running()) {
        return;
    }
    // In the background, into the engine's own cooked folder: cells and worlds (the shaders stay as they are).
    const core::Result<platform::PathString> cooker = platform::program_beside_engine("cooker");
    const char* src_root = engine.vfs.mount_root(platform::Mount::Src);
    const char* game_root = engine.vfs.mount_root(platform::Mount::Game);
    if (cooker && src_root != nullptr && game_root != nullptr) {
        const char* args[] = {cooker->c_str(), "--src", src_root, "--out", game_root, "--no-shaders"};
        if (core::Result<platform::Process> started = platform::Process::start(args)) {
            cooker_ = std::move(*started);
        }
    }
}

asset::AssetHandle Editor::hold(const asset::AssetRef& ref) {
    if (const asset::AssetHandle* held = holds_.find(ref.id.value()); held != nullptr) {
        return *held;
    }
    const asset::AssetHandle handle = engine_->assets.load(ref);
    holds_.set(ref.id.value(), handle);
    return handle;
}

void Editor::release_holds() {
    if (engine_ == nullptr) {
        return;
    }
    for (auto [id, handle] : holds_) {
        engine_->assets.release(handle);
    }
    holds_.clear();
}

flecs::entity Editor::make(const EntityRecord& record) {
    const flecs::world& ecs = engine_->world.ecs();
    const flecs::entity entity = ecs.entity();
    // Its cell's member again while the cell is as it was (its file still has it); else one the editor made.
    bool member = false;
    if (record.cell != 0 && ecs.is_alive(record.cell)) {
        const auto* contents = ecs.entity(record.cell).try_get<world::CellContents>();
        if (contents != nullptr && contents->revision == record.revision) {
            entity.add<world::InCell>(ecs.entity(record.cell)).set(world::CellMember{.index = record.member_index});
            member = true;
        }
    }
    if (!member) {
        EditorEntry entry = record.entry;
        entry.saved = false;
        entity.set(entry);
    }
    if (record.is_static) {
        entity.add<world::StaticTransform>();
    }
    if (record.mesh.is_valid()) {
        entity.set(world::MeshInstance{.mesh = hold({.id = record.mesh, .type = asset::AssetType::Mesh})});
    }
    if (record.collision.is_valid()) {
        entity.set(
            world::MeshCollider{.collision = hold({.id = record.collision, .type = asset::AssetType::Collision})});
    }
    if (record.has_fog) {
        entity.set(record.fog);
    }
    world::place(entity, record.transform);
    return entity;
}

void Editor::record(flecs::entity entity, EntityRecord& out) {
    out = EntityRecord{};
    if (const auto* where = entity.try_get<world::WorldTransform>()) {
        out.transform = {.position = where->position, .rotation = where->rotation, .scale = where->scale};
    } else if (const auto* local = entity.try_get<world::Transform>()) {
        out.transform = *local;
    }
    const asset::AssetRegistry& assets = engine_->assets;
    if (const auto* mesh = entity.try_get<world::MeshInstance>(); mesh != nullptr && mesh->mesh.is_valid()) {
        out.mesh = assets.asset(mesh->mesh).id;
    }
    if (const auto* collider = entity.try_get<world::MeshCollider>();
        collider != nullptr && collider->collision.is_valid()) {
        out.collision = assets.asset(collider->collision).id;
    }
    if (const auto* fog = entity.try_get<world::FogVolume>()) {
        out.has_fog = true;
        out.fog = *fog;
    }
    out.is_static = entity.has<world::StaticTransform>();
    if (const auto* made = entity.try_get<EditorEntry>()) {
        out.entry = *made;
        out.has_entry = true;
    }
    const auto* member = entity.try_get<world::CellMember>();
    const flecs::entity cell = member != nullptr ? entity.target<world::InCell>() : flecs::entity();
    const auto* held = cell.is_valid() ? cell.try_get<world::Cell>() : nullptr;
    const auto* cooked = held != nullptr ? assets.get<asset::CellAsset>(held->asset) : nullptr;
    if (cooked == nullptr || member->index >= cooked->entities.count) {
        return;
    }
    const asset::CellEntity& entry = cooked->entities.get(cooked)[member->index];
    out.cell = cell.id();
    out.member_index = member->index;
    if (const auto* contents = cell.try_get<world::CellContents>()) {
        out.revision = contents->revision;
    }
    out.key = {.x = cooked->coord_x, .z = cooked->coord_z, .source_index = entry.source_index};
    const std::string_view name = cooked->name(cooked, entry);
    static_cast<void>(
        std::snprintf(out.cooked_name, sizeof(out.cooked_name), "%.*s", static_cast<int>(name.size()), name.data()));
    // As an entry of its own, from its file, for when it's put back after a save took it out.
    out.has_entry = entry_from_file(out, out.entry);
}

void Editor::unmake(flecs::entity entity) {
    members_.forget(entity.id()); // gone on purpose: whatever spawns where it was isn't it
    if (selection_.kind == Selection::Kind::Entity && selection_.entity == entity.id()) {
        selection_.select_entity(0);
    }
    entity.destruct();
}

bool Editor::entry_from_file(const EntityRecord& record, EditorEntry& out) {
    const uint32_t world_index = cell_sources_.size() > 0 ? cell_sources_.world_index(viewer_.world_id()) : UINT32_MAX;
    const CellSources::Cell* cell =
        world_index != UINT32_MAX ? cell_sources_.cell_at(world_index, record.key.x, record.key.z) : nullptr;
    if (cell == nullptr) {
        // Looked through again: the sources may be new since.
        static_cast<void>(cell_sources_.scan(engine_->vfs));
        const uint32_t again = cell_sources_.world_index(viewer_.world_id());
        cell = again != UINT32_MAX ? cell_sources_.cell_at(again, record.key.x, record.key.z) : nullptr;
    }
    if (cell == nullptr) {
        return false;
    }
    const auto bytes = engine_->vfs.read_whole_file(cell->path.c_str(), editor_heap());
    if (!bytes) {
        return false;
    }
    CellSource file(editor_heap());
    if (!file.parse({reinterpret_cast<const char*>(bytes->data()), bytes->size()}) ||
        record.key.source_index >= file.entity_count() || file.name(record.key.source_index) != record.cooked_name) {
        return false;
    }
    // A root of its own: no parent (where it stands is written at the save).
    const std::string_view object = file.entity_text(record.key.source_index);
    core::Array<char> wrapped(editor_heap());
    constexpr std::string_view kOpen = R"({"entities": [)";
    constexpr std::string_view kClose = "]}";
    wrapped.resize(kOpen.size() + object.size() + kClose.size());
    std::memcpy(wrapped.data(), kOpen.data(), kOpen.size());
    std::memcpy(wrapped.data() + kOpen.size(), object.data(), object.size());
    std::memcpy(wrapped.data() + kOpen.size() + object.size(), kClose.data(), kClose.size());
    CellSource alone(editor_heap());
    if (!alone.parse({wrapped.data(), wrapped.size()})) {
        return false;
    }
    static_cast<void>(alone.remove_entity_member(0, "parent"));
    const std::string_view root = alone.entity_text(0);
    if (root.size() >= sizeof(out.object)) {
        return false;
    }
    out = EditorEntry{};
    std::memcpy(out.object, root.data(), root.size());
    const std::string_view path = cell->path.view();
    const std::string_view folder = path.substr(0, path.rfind('/'));
    static_cast<void>(
        std::snprintf(out.folder, sizeof(out.folder), "%.*s", static_cast<int>(folder.size()), folder.data()));
    static_cast<void>(std::snprintf(out.name, sizeof(out.name), "%s", record.cooked_name));
    return true;
}

void Editor::undo(app::Engine& engine) {
    static_cast<void>(commands_.undo(engine.world.ecs()));
    if (commands_.last_made() != 0) {
        selection_.select_entity(commands_.last_made());
    } else if (selection_.kind == Selection::Kind::Entity && !engine.world.ecs().is_alive(selection_.entity)) {
        selection_.select_entity(0);
    }
}

void Editor::redo(app::Engine& engine) {
    static_cast<void>(commands_.redo(engine.world.ecs()));
    if (commands_.last_made() != 0) {
        selection_.select_entity(commands_.last_made());
    } else if (selection_.kind == Selection::Kind::Entity && !engine.world.ecs().is_alive(selection_.entity)) {
        selection_.select_entity(0);
    }
}

void Editor::delete_selection(app::Engine& engine) {
    const flecs::world& ecs = engine.world.ecs();
    if (selection_.kind != Selection::Kind::Entity || !ecs.is_alive(selection_.entity)) {
        return;
    }
    const flecs::entity entity = ecs.entity(selection_.entity);
    if (!entity.has<world::Transform>() || entity.has<world::Cell>() || entity.id() == viewer_.camera_entity().id()) {
        return; // only what's placed: not a cell, not the editor's camera
    }
    char label[128];
    entity_label(engine.assets, entity, label);
    if (commands_.remove(entity)) {
        static_cast<void>(std::snprintf(message_, sizeof(message_), "Deleted %s (Ctrl+Z puts it back)", label));
    }
}

bool Editor::ground_below(app::Engine& engine, const DVec3& start, flecs::entity ignore, DVec3& out) {
    constexpr double kReach = 10000.0;
    const Vec3 down(0.0f, -1.0f, 0.0f);
    bool found = false;
    // Something solid: not what's being dropped, nor what hangs on it (cast again past those).
    DVec3 from = start;
    for (int tries = 0; tries < 16; ++tries) {
        physics::QueryHit hit;
        if (!engine.physics.ray_cast(from, down, static_cast<float>(kReach), hit, {.ignore = ignore.id()})) {
            break;
        }
        bool its_own = false;
        for (flecs::entity at = engine.world.ecs().entity(hit.entity); at.is_valid() && at.is_alive() && !its_own;) {
            const auto* parent = at.try_get<flecs::Parent>();
            if (parent == nullptr) {
                break;
            }
            its_own = parent->value == ignore.id();
            at = engine.world.ecs().entity(parent->value);
        }
        if (!its_own) {
            out = hit.position;
            found = true;
            break;
        }
        from = DVec3(hit.position.x, hit.position.y - 0.01, hit.position.z);
    }
    // The terrain, if it's higher.
    if (terrain_heights_.has_value()) {
        world::TerrainHeights& heights = *terrain_heights_;
        heights.set_terrain(engine.assets.get<asset::TerrainAsset>(engine.terrain));
        double ground = 0.0;
        if (heights.height_at(start.x, start.z, ground) && ground <= start.y && (!found || ground > out.y)) {
            out = DVec3(start.x, ground, start.z);
            found = true;
        }
    }
    return found;
}

bool Editor::drop_selection_to_ground(app::Engine& engine) {
    const flecs::world& ecs = engine.world.ecs();
    if (selection_.kind != Selection::Kind::Entity || !ecs.is_alive(selection_.entity)) {
        return false;
    }
    const flecs::entity entity = ecs.entity(selection_.entity);
    const auto* where = entity.try_get<world::WorldTransform>();
    if (where == nullptr || entity.has<world::Cell>() || entity.id() == viewer_.camera_entity().id()) {
        return false;
    }
    // Its lowest and highest points, as it's turned and scaled now: the box round its mesh (its pivot without one).
    const world::WorldTransform now = *where;
    double lowest = 0.0;
    double highest = 0.0;
    const auto* instance = entity.try_get<world::MeshInstance>();
    if (const auto* mesh = instance != nullptr ? engine.assets.get<asset::MeshAsset>(instance->mesh) : nullptr) {
        lowest = std::numeric_limits<double>::max();
        highest = std::numeric_limits<double>::lowest();
        for (uint32_t c = 0; c < 8; ++c) {
            const Vec3 corner((c & 1U) != 0 ? mesh->bounds.max[0] : mesh->bounds.min[0],
                              (c & 2U) != 0 ? mesh->bounds.max[1] : mesh->bounds.min[1],
                              (c & 4U) != 0 ? mesh->bounds.max[2] : mesh->bounds.min[2]);
            const double height = (now.rotation * (now.scale * corner)).y;
            lowest = std::min(lowest, height);
            highest = std::max(highest, height);
        }
    }
    // Looked for from just over its top, so one half sunk into the ground comes up onto it; its lowest point lands.
    DVec3 ground;
    bool below = ground_below(engine, now.position + DVec3(0.0, highest + 0.5, 0.0), entity, ground);
    // Under the terrain altogether: up onto it.
    if (double terrain = 0.0; !below && terrain_heights_.has_value() &&
                              terrain_heights_->height_at(now.position.x, now.position.z, terrain)) {
        ground = DVec3(now.position.x, terrain, now.position.z);
        below = true;
    }
    if (!below) {
        static_cast<void>(std::snprintf(message_, sizeof(message_), "Nothing below it to drop onto"));
        return false;
    }
    world::WorldTransform dropped = now;
    dropped.position.y = ground.y - lowest;
    // As its Transform has it: in the world, or relative to what it's attached to.
    world::Transform placed{.position = dropped.position, .rotation = dropped.rotation, .scale = dropped.scale};
    if (const auto* parent = entity.try_get<flecs::Parent>()) {
        const auto* parent_world = ecs.entity(parent->value).try_get<world::WorldTransform>();
        if (parent_world == nullptr) {
            return false;
        }
        placed = world::local_of(*parent_world, dropped);
    }
    const bool moved =
        commands_.set_component(entity, ecs.component<world::Transform>().id(), std::as_bytes(std::span(&placed, 1)));
    if (moved) {
        const double moved_by = now.position.y - dropped.position.y;
        static_cast<void>(std::snprintf(message_, sizeof(message_), "Dropped to the ground, %.2f m %s",
                                        std::abs(moved_by), moved_by >= 0.0 ? "down" : "up"));
    }
    return moved;
}

void Editor::duplicate_selection(app::Engine& engine) {
    const flecs::world& ecs = engine.world.ecs();
    if (selection_.kind != Selection::Kind::Entity || !ecs.is_alive(selection_.entity)) {
        return;
    }
    const flecs::entity source = ecs.entity(selection_.entity);
    if (!source.has<world::Transform>() || source.has<world::Cell>() || source.id() == viewer_.camera_entity().id()) {
        return;
    }
    EntityRecord copy;
    record(source, copy);
    if (!copy.has_entry) {
        static_cast<void>(std::snprintf(message_, sizeof(message_),
                                        "Can't duplicate it: its cell's file wasn't found (cook, then try again)"));
        return;
    }
    copy.cell = 0; // a new one: made in the editor, saved into the cell it stands in
    char name[128];
    unique_name(engine, copy.entry.name, name);
    static_cast<void>(rename_entry(copy.entry, name));
    const flecs::entity made = make(copy);
    commands_.created(made);
    selection_.select_entity(made.id());
    static_cast<void>(
        std::snprintf(message_, sizeof(message_), "Duplicated %s: move it where it goes", copy.entry.name));
}

void Editor::place_model(app::Engine& engine, std::string_view path, const View& view, ImVec2 screen) {
    // Where the ray under the mouse meets the ground: the nearer of something solid and the terrain, else the level of
    // the world's zero.
    const Vec3 direction = view.ray(screen);
    DVec3 point = view.position + (DVec3(direction) * 20.0);
    constexpr double kReach = 5000.0;
    double nearest = kReach;
    bool found = false;
    physics::QueryHit hit;
    if (engine.physics.ray_cast(view.position, direction, static_cast<float>(kReach), hit)) {
        point = hit.position;
        nearest = hit.distance_m;
        found = true;
    }
    if (terrain_heights_.has_value()) {
        world::TerrainHeights& heights = *terrain_heights_;
        heights.set_terrain(engine.assets.get<asset::TerrainAsset>(engine.terrain));
        if (DVec3 ground; heights.ray_cast(view.position, direction, nearest, ground)) {
            point = ground;
            found = true;
        }
    }
    if (found) {
        // On what it met.
    } else if (direction.y < -1e-4f) {
        const double along = -view.position.y / static_cast<double>(direction.y);
        if (along > 0.0 && along < 5000.0) {
            point = view.position + (DVec3(direction) * along);
        }
    }
    static_cast<void>(place_model_at(engine, path, point));
}

bool Editor::place_model_at(app::Engine& engine, std::string_view path, const DVec3& point) {
    if (file_kind(path) != "Model") {
        static_cast<void>(std::snprintf(message_, sizeof(message_), "Only models (.gltf, .glb, .fbx) can be placed"));
        return false;
    }
    asset::Guid guid;
    asset::AssetId mesh;
    if (!source_asset_id(engine.vfs, path, guid, mesh)) {
        static_cast<void>(std::snprintf(message_, sizeof(message_), "%.*s has no meta: run the cooker first",
                                        static_cast<int>(path.size()), path.data()));
        return false;
    }
    // Its entry: named after its file, the model by its name from its own folder, solid as props are.
    const size_t slash = path.rfind('/');
    const std::string_view file = path.substr(slash + 1);
    const std::string_view stem = file.substr(0, file.rfind('.'));
    EntityRecord placed;
    placed.transform = {.position = point};
    placed.mesh = mesh;
    placed.is_static = true;
    placed.has_entry = true;
    static_cast<void>(std::snprintf(
        placed.entry.object, sizeof(placed.entry.object), R"({"name": "%.*s", "mesh": "%.*s", "collision": "mesh"})",
        static_cast<int>(stem.size()), stem.data(), static_cast<int>(file.size()), file.data()));
    const std::string_view folder = path.substr(0, slash);
    const size_t folder_length = std::min(folder.size(), sizeof(placed.entry.folder) - 1);
    std::memcpy(placed.entry.folder, folder.data(), folder_length);
    placed.entry.folder[folder_length] = 0;
    static_cast<void>(std::snprintf(placed.entry.name, sizeof(placed.entry.name), "%.*s", static_cast<int>(stem.size()),
                                    stem.data()));
    char name[128];
    unique_name(engine, placed.entry.name, name);
    static_cast<void>(rename_entry(placed.entry, name));
    const flecs::entity made = make(placed);
    commands_.created(made);
    selection_.select_entity(made.id());
    static_cast<void>(
        std::snprintf(message_, sizeof(message_), "Placed %s (saved into its cell with Ctrl+S)", placed.entry.name));
    return true;
}

void Editor::unique_name(const app::Engine& engine, std::string_view base, std::span<char> out) const {
    // The names there are now.
    core::Array<core::FixedString<128>> names(editor_heap());
    char label[128];
    engine.world.ecs().each([&](flecs::entity entity, const world::Transform&) {
        entity_label(engine.assets, entity, label);
        names.push(core::FixedString<128>(std::string_view(label)));
    });
    const auto taken = [&names](std::string_view name) {
        return std::ranges::any_of(names, [name](const core::FixedString<128>& had) { return had.view() == name; });
    };
    free_name(base, taken, out);
}

bool Editor::rename_entry(EditorEntry& entry, std::string_view name) {
    constexpr std::string_view kOpen = R"({"entities": [)";
    constexpr std::string_view kClose = "]}";
    const std::string_view object(entry.object);
    core::Array<char> wrapped(editor_heap());
    wrapped.resize(kOpen.size() + object.size() + kClose.size());
    std::memcpy(wrapped.data(), kOpen.data(), kOpen.size());
    std::memcpy(wrapped.data() + kOpen.size(), object.data(), object.size());
    std::memcpy(wrapped.data() + kOpen.size() + object.size(), kClose.data(), kClose.size());
    CellSource alone(editor_heap());
    char quoted[160];
    const int length = std::snprintf(quoted, sizeof(quoted), "\"%.*s\"", static_cast<int>(name.size()), name.data());
    if (!alone.parse({wrapped.data(), wrapped.size()}) || length <= 0 ||
        !alone.set_entity_member(0, "name", {quoted, static_cast<size_t>(length)})) {
        return false;
    }
    const std::string_view renamed = alone.entity_text(0);
    if (renamed.size() >= sizeof(entry.object)) {
        return false;
    }
    std::memcpy(entry.object, renamed.data(), renamed.size());
    entry.object[renamed.size()] = '\0';
    const size_t name_length = std::min(name.size(), sizeof(entry.name) - 1);
    std::memcpy(entry.name, name.data(), name_length);
    entry.name[name_length] = '\0';
    return true;
}

flecs::entity Editor::find_named(const app::Engine& engine, std::string_view name) const {
    flecs::entity found;
    char label[128];
    engine.world.ecs().each([&](flecs::entity entity, const world::Transform&) {
        entity_label(engine.assets, entity, label);
        if (!found.is_valid() && name == label) {
            found = entity;
        }
    });
    return found;
}

void Editor::place_command(void* user, const console::Args& args, console::Output& out) {
    auto* editor = static_cast<Editor*>(user);
    double place[3] = {};
    bool numbers = args.size() == 4;
    for (size_t i = 0; numbers && i < 3; ++i) {
        const std::string_view word = args[i + 1];
        const std::from_chars_result parsed = std::from_chars(word.data(), word.data() + word.size(), place[i]);
        numbers = parsed.ec == std::errc() && parsed.ptr == word.data() + word.size();
    }
    if (!numbers) {
        out.error("editor.place takes a model's src:/ path and x y z");
        return;
    }
    if (!editor->place_model_at(*editor->engine_, args[0], DVec3(place[0], place[1], place[2]))) {
        out.error("%s", editor->message_);
        return;
    }
    out.info("%s", editor->message_);
}

void Editor::drop_command(void* user, const console::Args& args, console::Output& out) {
    auto* editor = static_cast<Editor*>(user);
    if (args.size() != 1 || editor->last_view_.size.x <= 0.0f) {
        out.error("editor.drop takes a model's src:/ path, with the Playground shown");
        return;
    }
    const View& view = editor->last_view_;
    const ImVec2 middle(view.min.x + (view.size.x * 0.5f), view.min.y + (view.size.y * 0.5f));
    const flecs::entity_t before = editor->selection_.entity;
    editor->place_model(*editor->engine_, args[0], view, middle);
    const flecs::world& ecs = editor->engine_->world.ecs();
    if (editor->selection_.entity == before || !ecs.is_alive(editor->selection_.entity)) {
        out.error("%s", editor->message_);
        return;
    }
    const DVec3 at = ecs.entity(editor->selection_.entity).get<world::Transform>().position;
    out.info("%s, at %.3f %.3f %.3f", editor->message_, at.x, at.y, at.z);
}

void Editor::duplicate_command(void* user, const console::Args& args, console::Output& out) {
    auto* editor = static_cast<Editor*>(user);
    const flecs::entity found = args.size() == 1 ? editor->find_named(*editor->engine_, args[0]) : flecs::entity();
    if (!found.is_valid()) {
        out.error("editor.duplicate: name an entity that's there");
        return;
    }
    editor->selection_.select_entity(found.id());
    editor->duplicate_selection(*editor->engine_);
    out.info("%s", editor->message_);
}

void Editor::undo_command(void* user, const console::Args& /*args*/, console::Output& out) {
    auto* editor = static_cast<Editor*>(user);
    const char* what = editor->commands_.undo_name(editor->engine_->world.ecs());
    if (what == nullptr) {
        out.error("nothing to undo");
        return;
    }
    out.info("undo %s", what);
    editor->undo(*editor->engine_);
}

void Editor::redo_command(void* user, const console::Args& /*args*/, console::Output& out) {
    auto* editor = static_cast<Editor*>(user);
    const char* what = editor->commands_.redo_name(editor->engine_->world.ecs());
    if (what == nullptr) {
        out.error("nothing to redo");
        return;
    }
    out.info("redo %s", what);
    editor->redo(*editor->engine_);
}

void Editor::ground_command(void* user, const console::Args& args, console::Output& out) {
    auto* editor = static_cast<Editor*>(user);
    const flecs::entity found = args.size() == 1 ? editor->find_named(*editor->engine_, args[0]) : flecs::entity();
    if (!found.is_valid()) {
        out.error("editor.ground: name an entity that's there");
        return;
    }
    editor->selection_.select_entity(found.id());
    if (!editor->drop_selection_to_ground(*editor->engine_)) {
        out.error("%s", editor->message_);
        return;
    }
    const DVec3 at = found.get<world::WorldTransform>().position;
    out.info("%s: at %.3f %.3f %.3f", editor->message_, at.x, at.y, at.z);
}

void Editor::delete_command(void* user, const console::Args& args, console::Output& out) {
    auto* editor = static_cast<Editor*>(user);
    const flecs::entity found = args.size() == 1 ? editor->find_named(*editor->engine_, args[0]) : flecs::entity();
    if (!found.is_valid()) {
        out.error("editor.delete: name an entity that's there");
        return;
    }
    editor->selection_.select_entity(found.id());
    editor->delete_selection(*editor->engine_);
    out.info("%s", editor->message_);
}

bool Editor::may_close(app::Engine& /*engine*/) {
    if (!unsaved() || !keep_settings_) {
        return true; // nothing to lose (or a picture's run)
    }
    pending_ = Pending::Quit;
    return false;
}

void Editor::unsaved_prompt(app::Engine& engine) {
    constexpr const char* kPrompt = "Unsaved edits";
    if (pending_ == Pending::None) {
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
        ImGui::TextUnformatted(pending_ == Pending::Quit ? "Save your edits before closing?"
                                                         : "Save your edits before opening another world?");
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
            const Pending pending = pending_;
            pending_ = Pending::None;
            if (choice == Choice::Save) {
                save(engine);
            }
            // Saved (or let go): on with what was asked. A save that failed stays here, with the reason shown.
            if (choice == Choice::Discard || (choice == Choice::Save && !unsaved())) {
                if (pending == Pending::Quit) {
                    quit_ = true; // the cooker, if it's running, finishes on its own
                } else {
                    open_world(engine, true);
                }
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

void Editor::save_command(void* user, const console::Args& /*args*/, console::Output& out) {
    auto* editor = static_cast<Editor*>(user);
    editor->save(*editor->engine_);
    out.info("%s", editor->message_);
}

void Editor::move_command(void* user, const console::Args& args, console::Output& out) {
    auto* editor = static_cast<Editor*>(user);
    app::Engine& engine = *editor->engine_;
    if (args.size() != 4) {
        out.error("editor.move takes a name and x y z");
        return;
    }
    double place[3] = {};
    for (size_t i = 0; i < 3; ++i) {
        const std::string_view word = args[i + 1];
        const std::from_chars_result parsed = std::from_chars(word.data(), word.data() + word.size(), place[i]);
        if (parsed.ec != std::errc() || parsed.ptr != word.data() + word.size()) {
            out.error("editor.move: '%.*s' isn't a number", static_cast<int>(word.size()), word.data());
            return;
        }
    }
    const std::string_view name = args[0];
    const flecs::entity found = editor->find_named(engine, name);
    if (!found.is_valid()) {
        out.error("editor.move: nothing is called %.*s", static_cast<int>(name.size()), name.data());
        return;
    }
    world::Transform moved = found.get<world::Transform>();
    moved.position = DVec3(place[0], place[1], place[2]);
    if (!editor->commands_.set_component(found, engine.world.ecs().component<world::Transform>().id(),
                                         std::as_bytes(std::span(&moved, 1)))) {
        out.error("editor.move: %.*s couldn't be moved", static_cast<int>(name.size()), name.data());
        return;
    }
    editor->selection_.select_entity(found.id());
    out.info("moved %.*s to %g %g %g", static_cast<int>(name.size()), name.data(), place[0], place[1], place[2]);
}

void Editor::follow_saves(app::Engine& engine) {
    DE_PROFILE_ZONE("Editor::follow_saves");
    int code = 0;
    if (cooker_.running() && cooker_.finished(code)) {
        // Judged by what was saved: something else failing to cook (a terrain being worked on) isn't this save's.
        uint32_t failed = 0;
        const auto report = engine.vfs.read_whole_file("game:/cook_report.json", editor_heap());
        const std::string_view text =
            report ? std::string_view(reinterpret_cast<const char*>(report->data()), report->size()) : "";
        for (const platform::PathString& path : cooking_) {
            if (!report || cook_report_has_error(text, path.view())) {
                ++failed;
                DE_LOG_ERROR("editor", "%s didn't cook: game:/cook_report.json says why", path.c_str());
            }
        }
        if (failed == 0) {
            static_cast<void>(std::snprintf(message_, sizeof(message_), "Saved and cooked"));
            DE_LOG_INFO("editor", "the saved cells cooked%s: hot reload brings them in",
                        code == 0 ? "" : " (something else didn't: the cook report says what)");
        } else {
            static_cast<void>(std::snprintf(message_, sizeof(message_),
                                            "Saved, but %u %s didn't cook: the Output says why", failed,
                                            failed == 1 ? "cell" : "cells"));
        }
        cooker_ = {};
    }
    // What the commands and the selection name, where it came from, while it's alive; and after a cell is spawned
    // again, what took its place.
    const flecs::world& ecs = engine.world.ecs();
    if (selection_.kind == Selection::Kind::Entity) {
        members_.note(ecs, engine.assets, selection_.entity);
    }
    if (commands_.changes() != keyed_changes_) {
        keyed_changes_ = commands_.changes();
        commands_.entities(scratch_entities_);
        for (const flecs::entity_t entity : scratch_entities_) {
            members_.note(ecs, engine.assets, entity);
        }
    }
    members_.follow(ecs, engine.assets, [this, &engine](flecs::entity_t from, flecs::entity_t to) {
        commands_.remap(from, to);
        if (engine.world.ecs().is_alive(from)) {
            engine.world.ecs().entity(from).destruct(); // made in the editor, and now its cell's member
        }
        if (selection_.kind == Selection::Kind::Entity && selection_.entity == from) {
            selection_.entity = to;
        }
        gathered_for_ = 0; // the selection's meshes again
    });
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

void Editor::open_world(app::Engine& engine, bool unsaved_asked) {
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
    if (unsaved() && !unsaved_asked) {
        pending_ = Pending::OpenWorld; // asked first; it comes back here
        return;
    }
    DE_LOG_INFO("editor", "opening %s (%s)", selection_.file.c_str(), asset::format_asset_id(id).c_str());
    commands_.clear(); // their entities go with the world
    release_holds();
    saved_changes_ = commands_.changes();
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
